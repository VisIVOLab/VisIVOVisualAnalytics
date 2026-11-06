"""Refuse a truncated FITS file before anything reads its data (issue #118).

A FITS file cut short (an interrupted copy, a full disk, a partial download)
opens without complaint: astropy reads the headers and only warns. The
failure comes later, when the data is memory-mapped, as numpy's "buffer is
too small for requested array", which says nothing about the cause. The
WALLABY Eridanus cube on a cluster's parallel filesystem was 697.9 GB on disk
against 961.4 GB declared by its header, and failed exactly that way.

:func:`check_not_truncated` compares the file's size with what its headers
declare, from header blocks and ``os.stat`` only: the data is never read,
so it costs the same on a 1 TB cube as on a 1 MB image.

What is checked, HDU by HDU from the start of the file up to and including
the HDU that will be read (the image HDU, chosen as
:func:`app.fits_dataset.image_hdu_index` chooses it; a random-groups primary
is not an image there, as astropy's ``GroupsHDU.is_image`` is false):

* every header must be complete, END card included;
* every HDU's data must be present: ``|BITPIX|/8 * GCOUNT * (PCOUNT +
  NAXIS1 * ... * NAXISn)`` bytes, NAXIS1 left out of the product for random
  groups. A tile-compressed image is a BINTABLE, so its declared size is the
  table's (rows plus heap), never the image's.

What is deliberately not an error:

* bytes past the declared end (astropy ignores them);
* a file cut inside the final HDU's padding to the 2880-byte block: all the
  data is there and astropy reads it, so refusing it would refuse a readable
  file;
* an HDU after the one being read: it is never read on the open path. When
  no HDU qualifies as the image, HDU 0 is the one read (as in
  ``image_hdu_index``), so a cut further on is not an error as long as HDU 0
  holds data; with an empty HDU 0 there is nothing else to read, and the cut
  is reported.

Not checked: a file that does not start with a plain FITS primary header, which
includes gzip/bzip2/zip-compressed FITS (``.fits.gz``). Their headers declare
the decompressed size, which the compressed file's size cannot be compared
with without decompressing it all; a truncated stream fails in the
decompressor instead, with its own message.
"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path

BLOCK = 2880
CARD = 80
# A header longer than this is not a header worth parsing here: give up and
# let astropy have its say rather than read megabytes of a corrupt file.
_MAX_HEADER_BLOCKS = 10_000
_MAX_HDUS = 10_000


class FitsTruncatedError(ValueError):
    """The file is shorter than its headers declare. The message is written for
    the user: it names the file, both sizes and how much of the image is there."""

    def __init__(self, message: str, *, path: str, size: int, declared: int | None) -> None:
        super().__init__(message)
        self.path = path
        self.size = size
        self.declared = declared


@dataclass(frozen=True)
class _Hdu:
    index: int
    header_offset: int
    data_offset: int
    data_size: int        # without padding
    cards: dict[str, object]

    @property
    def padded_end(self) -> int:
        return self.data_offset + _padded(self.data_size)


def _padded(n: int) -> int:
    return -(-n // BLOCK) * BLOCK


def _value(card: str) -> object:
    """The value of a fixed-format card: a string, a bool, an int or None."""
    text = card[10:].strip()
    if text.startswith("'"):
        end = text.find("'", 1)
        return text[1:end if end > 0 else None].rstrip()
    text = text.split("/", 1)[0].strip()
    if text in ("T", "F"):
        return text == "T"
    try:
        return int(text)
    except ValueError:
        return None


# Only the keywords that size an HDU or pick the image one.
_WANTED = {"SIMPLE", "XTENSION", "BITPIX", "NAXIS", "PCOUNT", "GCOUNT", "GROUPS",
           "ZIMAGE", "ZNAXIS", "EXTNAME"}


_AXIS_KEY = re.compile(r"Z?NAXIS\d+")


def _wanted(key: str) -> bool:
    return key in _WANTED or _AXIS_KEY.fullmatch(key) is not None


def _int(cards: dict[str, object], key: str, default: int = 0) -> int:
    value = cards.get(key, default)
    return value if isinstance(value, int) and not isinstance(value, bool) else default


def _data_size(cards: dict[str, object]) -> int:
    naxis = _int(cards, "NAXIS")
    if naxis <= 0:
        return 0
    axes = [_int(cards, f"NAXIS{k}") for k in range(1, naxis + 1)]
    if cards.get("GROUPS") is True and axes and axes[0] == 0:
        axes = axes[1:]   # random groups: NAXIS1 = 0 is a marker, not a length
    count = 1
    for n in axes:
        count *= n
    return abs(_int(cards, "BITPIX", 8)) // 8 * _int(cards, "GCOUNT", 1) * (_int(cards, "PCOUNT") + count)


def _image_axes(cards: dict[str, object]) -> list[int] | None:
    """The image's axis lengths (NAXIS1 first) when the HDU is an image as
    astropy's ``is_image`` has it, else None; a tile-compressed image's from
    ZNAXISn."""
    xtension = cards.get("XTENSION")
    if xtension is None:   # the primary, unless it holds random groups
        if cards.get("GROUPS") is True:
            return None
        prefix = "NAXIS"
    elif xtension == "IMAGE":
        prefix = "NAXIS"
    elif xtension == "BINTABLE" and cards.get("ZIMAGE") is True:
        prefix = "ZNAXIS"
    else:
        return None
    return [_int(cards, f"{prefix}{k}", 1) for k in range(1, _int(cards, prefix) + 1)]


def _is_target_image(cards: dict[str, object]) -> bool:
    """The rule of :func:`app.fits_dataset.image_hdu_index`, from raw cards:
    an image with at least two axes longer than one."""
    axes = _image_axes(cards)
    return axes is not None and len(axes) >= 2 and sum(n > 1 for n in axes) >= 2


def _fmt(n: int) -> str:
    """Decimal units, one decimal place: 697.9 GB."""
    value = float(n)
    for unit in ("B", "kB", "MB", "GB", "TB"):
        if value < 1000.0 or unit == "TB":
            return f"{int(value)} B" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1000.0
    return f"{n} B"


def _sizes(size: int, declared: int) -> str:
    on_disk, wanted = _fmt(size), _fmt(declared)
    if on_disk == wanted:   # a few bytes short of a large file: say how many
        on_disk, wanted = f"{size:,} bytes", f"{declared:,} bytes"
    return f"{on_disk} on disk, {wanted} declared"


def _hdu_name(hdu: _Hdu) -> str:
    name = hdu.cards.get("EXTNAME")
    return f"HDU {hdu.index} named {name}" if isinstance(name, str) and name else f"HDU {hdu.index}"


def _what_is_present(hdu: _Hdu, size: int, target: bool) -> str:
    """How much of the cut HDU is there: planes of a cube, rows of an image,
    or which HDU is incomplete when that cannot be counted (a table, a
    tile-compressed image, an HDU before the image)."""
    cards = hdu.cards
    axes = _image_axes(cards)
    plain = (target and axes is not None and cards.get("XTENSION") != "BINTABLE"
             and cards.get("GROUPS") is not True and _int(cards, "PCOUNT") == 0
             and _int(cards, "GCOUNT", 1) == 1 and len(axes) >= 2 and all(n > 0 for n in axes))
    if plain:
        item = abs(_int(cards, "BITPIX", 8)) // 8
        if len(axes) >= 3:
            unit, step = "planes", item * axes[0] * axes[1]
            total = 1
            for n in axes[2:]:
                total *= n
        else:
            unit, step, total = "rows", item * axes[0], axes[1]
        present = min(total, max(0, size - hdu.data_offset) // step)
        return f"{present} of {total} {unit} present"
    if cards.get("XTENSION") == "BINTABLE" and cards.get("ZIMAGE") is True:
        return f"the compressed data of {_hdu_name(hdu)} is incomplete"
    return f"the data of {_hdu_name(hdu)} is incomplete"


def _read_header(f, offset: int, size: int) -> tuple[dict[str, object], int] | None:
    """The wanted cards of the header at ``offset`` and the offset after its
    last block; None when the file ends before the END card."""
    cards: dict[str, object] = {}
    f.seek(offset)
    for blocks in range(1, _MAX_HEADER_BLOCKS + 1):
        block = f.read(BLOCK)
        if len(block) < BLOCK:
            return None
        text = block.decode("ascii", errors="replace")
        for i in range(0, BLOCK, CARD):
            card = text[i:i + CARD]
            key = card[:8].rstrip()
            if key == "END":
                return cards, offset + blocks * BLOCK
            if card[8:10] == "= " and _wanted(key) and key not in cards:
                cards[key] = _value(card)
    raise _GiveUp


class _GiveUp(Exception):
    """The file is too odd to judge here: leave it to astropy."""


def _starts_extension(f, offset: int, size: int) -> bool:
    """Whether the bytes at ``offset`` begin an extension header (or the start
    of one, when fewer than eight bytes remain). Anything else after the last
    HDU is trailing bytes, which astropy ignores."""
    f.seek(offset)
    head = f.read(8)
    if len(head) == 8:
        return head == b"XTENSION"
    return bool(head) and b"XTENSION".startswith(head)


def _file_size(path: str) -> int:
    """The size on disk (a seam for tests, which cannot write a terabyte)."""
    return os.stat(path).st_size


def check_not_truncated(path: str | Path, hdu: int | None = None) -> None:
    """Raise :class:`FitsTruncatedError` when ``path`` is shorter than its
    headers declare, up to and including HDU ``hdu`` (None: the image HDU).

    Header blocks and ``os.stat`` only. A path that is not a plain FITS file
    (a VBT field, HDF5, gzip-compressed FITS) passes unchecked; see the module
    docstring for what is and is not an error.
    """
    path = str(path)
    try:
        size = _file_size(path)
        f = open(path, "rb")  # noqa: SIM115 — closed below
    except OSError:
        return   # not a file here: the caller's own open says why
    with f:
        if f.read(9) != b"SIMPLE  =":
            return
        name = os.path.basename(path)
        offset = 0
        last: _Hdu | None = None
        primary_has_data = False
        try:
            for index in range(_MAX_HDUS):
                if index > 0:
                    if offset >= size or not _starts_extension(f, offset, size):
                        return   # the end of the file, or trailing bytes
                parsed = _read_header(f, offset, size)
                if parsed is None:
                    if hdu is None and primary_has_data:
                        return   # no image before the cut: HDU 0 is read, and it is whole
                    raise FitsTruncatedError(
                        f"The file {name} is truncated: {_fmt(size)} on disk, ending inside "
                        f"the header of HDU {index}"
                        + (f" (after {_hdu_name(last)})." if last is not None else "."),
                        path=path, size=size, declared=None)
                cards, data_offset = parsed
                last = _Hdu(index, offset, data_offset, _data_size(cards), cards)
                target = index == hdu if hdu is not None else _is_target_image(cards)
                if last.data_offset + last.data_size > size:
                    if hdu is None and not target and primary_has_data:
                        # Like image_hdu_index: with no image HDU, HDU 0 is
                        # the one read, and it is whole; the cut HDU is not read.
                        return
                    raise FitsTruncatedError(
                        f"The file {name} is truncated: {_sizes(size, last.padded_end)} "
                        f"({_what_is_present(last, size, target)}).",
                        path=path, size=size, declared=last.padded_end)
                if target:
                    return
                if index == 0:
                    primary_has_data = last.data_size > 0
                offset = last.padded_end
        except _GiveUp:
            return
