"""What the desktop file browser shows for a selected file (issue #159).

The browser used to preview FITS headers only ("Header preview is available
only for FITS files"). Every file now gets a short, typed description built
from its first bytes, so the same preview works through a remote backend:

  - FITS (also ``.fits.gz`` and ``.fz``): a one-line summary per HDU, then the
    header cards the header route gives (:func:`preview_cards`);
  - text, sniffed from the bytes (UTF-8, else Latin-1), not from the name: the
    first lines, with a delimited table named by the separator and column
    count the catalogue readers would use (:mod:`app.text_table`);
  - a VisIVO Binary Table: its ``.head`` (for a ``.bin``, the one beside it);
  - HDF5: the group/dataset tree with shapes and types, no data read;
  - gzip/bzip2/xz/zip that is not FITS: the start of the decompressed stream
    when it is text, otherwise what the archive holds;
  - anything else: a hex/ASCII dump of the first 256 bytes.

Bounds, because the browser asks on every click and a remote backend may sit
on a parallel file system: the file's content is read at most ``max_bytes``
(decompressed bytes for an archive), the answer's text is at most
``max_lines`` lines and ``max_bytes`` UTF-8 bytes, an HDF5 walk stops after
:data:`HDF5_MAX_OBJECTS` objects, and a FITS file is read header by header
(the gzip stream is read up to the image header, never decompressed whole).
"""
from __future__ import annotations

import bz2
import codecs
import gzip
import lzma
import os
import zipfile
import zlib
from pathlib import Path
from typing import Callable, Iterable

from .text_table import describe_table

#: The defaults and the caps of the request's bounds.
DEFAULT_MAX_BYTES = 16384
MAX_BYTES_CAP = 65536
DEFAULT_MAX_LINES = 60
MAX_LINES_CAP = 400

#: An HDF5 walk stops after this many groups and datasets.
HDF5_MAX_OBJECTS = 200
#: HDUs summarised above a FITS header; the walk stops there.
FITS_MAX_HDUS = 10
#: Entries a directory preview counts before it stops ("1000+ entries").
DIRECTORY_MAX_ENTRIES = 1000
#: Bytes a binary file's hex dump shows.
HEX_DUMP_BYTES = 256
#: A ``.head`` larger than this is not parsed as a VBT header (they are a few
#: lines); it is still shown as text.
_VBT_HEAD_MAX_BYTES = 65536

_HDF5_MAGIC = b"\x89HDF\r\n\x1a\n"
_HDF5_SUFFIXES = (".h5", ".hdf5", ".hdf", ".he5", ".nc", ".bfdata")
_SEPARATOR_NAMES = {",": "Comma-separated", "\t": "Tab-separated", ";": "Semicolon-separated"}
# Characters that do not occur in text: C0 controls other than the usual
# whitespace (and ESC, which colourised logs hold).
_TEXT_CONTROLS = set(range(32)) - {9, 10, 11, 12, 13, 27}


# ── Bounds ───────────────────────────────────────────────────────────────────

def clamp_bounds(max_bytes: int, max_lines: int) -> tuple[int, int]:
    """The request's bounds within their caps (both are at least 1)."""
    return (max(1, min(int(max_bytes), MAX_BYTES_CAP)),
            max(1, min(int(max_lines), MAX_LINES_CAP)))


def _bounded(lines: Iterable[str], max_lines: int, max_bytes: int) -> tuple[str, bool]:
    """``(text, truncated)``: the first lines that fit in both bounds.

    ``lines`` may be a generator; at most one line past the bounds is drawn
    from it, to tell whether anything was left out. A first line longer than
    the byte bound is cut (at a character boundary) rather than dropped.
    """
    kept: list[str] = []
    used = 0
    for line in lines:
        cost = len(line.encode("utf-8")) + (1 if kept else 0)
        if len(kept) >= max_lines or used + cost > max_bytes:
            if not kept:
                kept.append(line.encode("utf-8")[:max_bytes].decode("utf-8", errors="ignore"))
            return "\n".join(kept), True
        kept.append(line)
        used += cost
    return "\n".join(kept), False


def _result(kind: str, fmt: str, text: str = "", truncated: bool = False) -> dict:
    return {"kind": kind, "format": fmt, "text": text, "truncated": truncated}


# ── Text ─────────────────────────────────────────────────────────────────────

def _decode(raw: bytes, cut: bool) -> tuple[str, str] | None:
    """``(text, encoding)`` when ``raw`` reads as text, None when it is binary.

    UTF-8 first; bytes that are not UTF-8 are Latin-1, which decodes anything,
    so the content decides: a NUL, or more than 1 % of control characters, is
    binary. ``cut`` says ``raw`` stops inside the file, where a multi-byte
    character may have been split.
    """
    if b"\x00" in raw:
        return None
    if raw.startswith(codecs.BOM_UTF8):
        raw = raw[len(codecs.BOM_UTF8):]
    try:
        text = codecs.getincrementaldecoder("utf-8")().decode(raw, final=not cut)
        encoding = "UTF-8"
    except UnicodeDecodeError:
        text = raw.decode("latin-1")
        encoding = "Latin-1"
    controls = sum(1 for byte in raw if byte in _TEXT_CONTROLS)
    if encoding == "Latin-1":
        # C1 controls (0x80-0x9f) are what UTF-16, images and executables
        # leave in a Latin-1 reading; text in Latin-1 holds almost none.
        controls += sum(1 for byte in raw if 0x80 <= byte <= 0x9F)
    if raw and controls > len(raw) // 100:
        return None
    return text, encoding


def _text_format(text: str, cut: bool, suffix: str, encoding: str) -> str:
    """The short label of a text file: the table it is, or its language."""
    head = text.lstrip()
    if suffix == ".speck":
        fmt = "speck point catalogue"
    elif head.startswith("<"):
        fmt = "VOTable" if "<VOTABLE" in head[:4096].upper() else "XML"
    elif head.startswith(("|", "\\")):
        fmt = "IPAC table"
    elif suffix == ".json" or head.startswith("{"):
        fmt = "JSON"
    else:
        table = describe_table(text, truncated=cut)
        if table is not None:
            separator, columns = table
            fmt = f"{_SEPARATOR_NAMES[separator]} table, {columns} columns"
        else:
            fmt = "Empty file" if not text else "Text"
    return f"{fmt} (Latin-1)" if encoding == "Latin-1" else fmt


def _text_lines(text: str, cut: bool) -> list[str]:
    lines = text.splitlines()
    if cut and len(lines) > 1:
        lines = lines[:-1]   # the byte bound ends in half a line
    return lines


def _text_preview(raw: bytes, cut: bool, suffix: str, max_bytes: int,
                  max_lines: int) -> dict | None:
    """The text result for ``raw`` (the file's first bytes), None if binary."""
    decoded = _decode(raw, cut)
    if decoded is None:
        return None
    text, encoding = decoded
    shown, more = _bounded(_text_lines(text, cut), max_lines, max_bytes)
    return _result("text", _text_format(text, cut, suffix, encoding), shown, more or cut)


def _read_start(path: Path, max_bytes: int) -> tuple[bytes, bool]:
    """``(bytes, cut)``: at most ``max_bytes`` of the file, and whether it goes on."""
    with open(path, "rb") as handle:
        raw = handle.read(max_bytes + 1)
    return raw[:max_bytes], len(raw) > max_bytes


# ── Binary ───────────────────────────────────────────────────────────────────

def hex_dump(raw: bytes) -> list[str]:
    """``hexdump -C``-style lines: offset, 16 bytes in hex, their ASCII."""
    lines = []
    for offset in range(0, len(raw), 16):
        row = raw[offset:offset + 16]
        hexes = " ".join(f"{byte:02x}" for byte in row)
        ascii_ = "".join(chr(byte) if 32 <= byte < 127 else "." for byte in row)
        lines.append(f"{offset:08x}  {hexes:<47}  |{ascii_}|")
    return lines


def _binary_preview(path: Path, max_bytes: int, max_lines: int) -> dict:
    raw, _ = _read_start(path, min(HEX_DUMP_BYTES, max_bytes))
    if not raw:
        return _result("binary", "Empty file")
    text, more = _bounded(hex_dump(raw), max_lines, max_bytes)
    size = path.stat().st_size
    return _result("binary", "Binary data", text, more or size > len(raw))


# ── FITS ─────────────────────────────────────────────────────────────────────

def preview_cards(hdul) -> list[str]:
    """The image's own header (WCS, BUNIT) — the extension's, on a
    multi-extension product — followed by the primary's descriptive cards it
    does not repeat (OBJECT, the ESO hierarchy). Structural cards and
    duplicates stay out: the header editor needs every keyword once, and
    edits go to the image HDU. ``hdul`` is an HDUList, or the headers read off
    a gzip stream (``gzip_image_headers``)."""
    from .fits_dataset import image_hdu_index  # noqa: PLC0415

    index = image_hdu_index(hdul)
    image_header = hdul[index].header
    cards = [str(card) for card in image_header.cards]
    if index != 0:
        skip = {"SIMPLE", "BITPIX", "NAXIS", "EXTEND", "PCOUNT", "GCOUNT",
                "XTENSION", "EXTNAME", "EXTVER", "END", "COMMENT", "HISTORY", ""}
        for card in hdul[0].header.cards:
            key = card.keyword
            if key in skip or key.startswith("NAXIS") or key in image_header:
                continue
            cards.append(str(card))
    return cards


def _axes(header) -> list[int]:
    return [int(header.get(f"NAXIS{k}", 0)) for k in range(1, int(header.get("NAXIS", 0)) + 1)]


def _hdu_label(header, is_image: bool) -> str:
    """``image 700×700×158 (BITPIX -32)``, ``table, 5 columns, 120 rows`` or
    ``no data`` for one HDU's header."""
    axes = _axes(header)
    xtension = str(header.get("XTENSION", "")).strip().upper()
    if xtension in ("BINTABLE", "TABLE") and not is_image:
        rows = axes[1] if len(axes) > 1 else 0
        return f"table, {int(header.get('TFIELDS', 0))} columns, {rows} rows"
    if not axes or not all(axes):
        return "no data"
    return f"image {'×'.join(str(n) for n in axes)} (BITPIX {header.get('BITPIX', '?')})"


def _fits_format(hdul, compression: str) -> str:
    """``FITS image 700×700×158``: the picked image HDU, else the first table."""
    from .fits_dataset import image_hdu_index  # noqa: PLC0415

    index = image_hdu_index(hdul)
    header = hdul[index].header
    axes = _axes(header)
    if axes and all(axes) and sum(n > 1 for n in axes) >= 1 and getattr(hdul[index], "is_image", True):
        fmt = f"FITS image {'×'.join(str(n) for n in axes)}"
    else:
        fmt = "FITS, no image"
        for hdu in hdul[:FITS_MAX_HDUS]:
            if str(hdu.header.get("XTENSION", "")).strip().upper() in ("BINTABLE", "TABLE") \
                    and not getattr(hdu, "is_image", False):
                rows = _axes(hdu.header)
                fmt = (f"FITS table, {int(hdu.header.get('TFIELDS', 0))} columns, "
                       f"{rows[1] if len(rows) > 1 else 0} rows")
                break
    return f"{fmt} ({compression})" if compression else fmt


def _fits_lines(hdul) -> Iterable[str]:
    for index, hdu in enumerate(hdul[:FITS_MAX_HDUS]):
        name = str(hdu.header.get("EXTNAME", "PRIMARY" if index == 0 else "")).strip()
        yield f"HDU {index}  {name or '-'}  {_hdu_label(hdu.header, getattr(hdu, 'is_image', False))}"
    yield ""
    yield from preview_cards(hdul)


# Decompressed HDU data the preview may skip to reach later gzip headers.
GZIP_MAX_SKIP_BYTES = 64 * 1024 * 1024


class _LazyHdus:
    """An astropy HDUList read lazily, sliced only as far as asked, so the
    summary never walks a thousand-extension file to its end."""

    def __init__(self, hdul):
        self._hdul = hdul

    def __iter__(self):
        return iter(self._hdul)

    def __getitem__(self, item):
        if isinstance(item, slice):
            out = []
            for index, hdu in enumerate(self._hdul):
                if index >= item.stop:
                    break
                out.append(hdu)
            return out
        return self._hdul[item]


def _fits_preview(path: Path, gzipped: bool, max_bytes: int, max_lines: int) -> dict:
    from astropy.io import fits  # noqa: PLC0415

    from .compressed_fits import gzip_image_headers  # noqa: PLC0415

    try:
        if gzipped:
            # Read off the gzip stream up to the image header (#57); astropy
            # would decompress the stream whole to find the next HDU.
            # Bounded: a click must not decompress a large table to reach
            # the next header.
            hdus = gzip_image_headers(path, max_skip_bytes=GZIP_MAX_SKIP_BYTES)
            if hdus is None:
                return _result("fits", "FITS (gzip)",
                               "The FITS headers could not be read off the gzip stream.")
            text, more = _bounded(_fits_lines(hdus), max_lines, max_bytes)
            return _result("fits", _fits_format(hdus, "gzip"), text, more)
        with fits.open(path, memmap=True, lazy_load_hdus=True) as hdul:
            hdus = _LazyHdus(hdul)
            tiled = any(isinstance(hdu, fits.CompImageHDU) for hdu in hdus[:FITS_MAX_HDUS])
            text, more = _bounded(_fits_lines(hdus), max_lines, max_bytes)
            return _result("fits", _fits_format(hdus, "tile-compressed" if tiled else ""),
                           text, more)
    except Exception as exc:  # noqa: BLE001 — a damaged header is shown, not raised
        return _result("fits", "FITS (header unreadable)",
                       f"The FITS header could not be read: {exc}")


# ── VisIVO Binary Table ──────────────────────────────────────────────────────

def _vbt_head_of(path: Path, allow: Callable[[Path], bool]) -> Path | None:
    """The ``.head`` that describes ``path``: itself, or the one beside a
    ``.bin`` (``x.bin.head``, else ``x.head``) that the caller may read."""
    if path.suffix.lower() == ".head":
        return path
    if path.suffix.lower() != ".bin":
        return None
    for candidate in (Path(str(path) + ".head"), path.with_suffix(".head")):
        if candidate.is_file() and allow(candidate):
            return candidate
    return None


def _vbt_format(head: Path) -> str | None:
    """``VisIVO Binary Table, volume 64×64×64, 2 fields``; None when ``head``
    is not a VBT header (a SCAMP ``.head`` is a text FITS header)."""
    from .volume_source import parse_vbt_header  # noqa: PLC0415

    try:
        if head.stat().st_size > _VBT_HEAD_MAX_BYTES:
            return None
        meta = parse_vbt_header(head)
    except (OSError, ValueError, UnicodeDecodeError, IndexError):
        return None
    fields = meta["field_count"]
    if meta["kind"] == "volume":
        dims = "×".join(str(n) for n in meta["dimensions"])
        return f"VisIVO Binary Table, volume {dims}, {fields} fields"
    return f"VisIVO Binary Table, {meta['num_rows']} points, {fields} fields"


def _vbt_preview(head: Path, max_bytes: int, max_lines: int) -> dict | None:
    fmt = _vbt_format(head)
    if fmt is None:
        return None
    raw, cut = _read_start(head, max_bytes)
    decoded = _decode(raw, cut)
    text = decoded[0] if decoded else ""
    shown, more = _bounded(_text_lines(text, cut), max_lines, max_bytes)
    return _result("vbt", fmt, shown, more or cut)


# ── HDF5 ─────────────────────────────────────────────────────────────────────

def _hdf5_preview(path: Path, max_bytes: int, max_lines: int) -> dict:
    """The tree, one object per line, shapes and types from the metadata.

    ``visititems`` follows hard links only, so an external link (which could
    name a file outside the data roots) is never opened, and the walk stops
    as soon as the callback returns a value.
    """
    try:
        import h5py  # noqa: PLC0415
    except ImportError:
        return _result("hdf5", "HDF5", "h5py is not installed on the backend.")

    limit = min(HDF5_MAX_OBJECTS, max_lines)
    lines: list[str] = []
    counts = {"group": 0, "dataset": 0}

    def visit(name: str, obj) -> bool | None:
        if len(lines) >= limit:
            return True          # one object past the bound: stop, truncated
        depth = name.count("/")
        leaf = name.rsplit("/", 1)[-1]
        if isinstance(obj, h5py.Dataset):
            counts["dataset"] += 1
            shape = "×".join(str(n) for n in obj.shape) if obj.shape else "scalar"
            lines.append(f"{'  ' * depth}{leaf}  {shape}  {obj.dtype}")
        elif isinstance(obj, h5py.Group):
            counts["group"] += 1
            lines.append(f"{'  ' * depth}{leaf}/")
        else:
            lines.append(f"{'  ' * depth}{leaf}  (named type)")
        return None

    try:
        try:
            # No file lock: a file a pipeline is still writing can be looked
            # at, and parallel file systems without lock support do not fail.
            handle = h5py.File(path, "r", locking=False)
        except TypeError:   # h5py < 3.5
            handle = h5py.File(path, "r")
        with handle:
            stopped = handle.visititems(visit) is not None
    except Exception as exc:  # noqa: BLE001
        return _result("hdf5", "HDF5", f"The HDF5 structure could not be read: {exc}")
    text, more = _bounded(lines, max_lines, max_bytes)
    plus = "+" if stopped else ""
    fmt = f"HDF5, {counts['group']}{plus} groups, {counts['dataset']}{plus} datasets"
    return _result("hdf5", fmt, text or "(empty file)", more or stopped)


# ── Compressed ───────────────────────────────────────────────────────────────

def _compression(raw: bytes) -> str | None:
    if raw[:2] == b"\x1f\x8b":
        return "gzip"
    if raw[:3] == b"BZh" and raw[3:4].isdigit():
        return "bzip2"
    if raw[:6] == b"\xfd7zXZ\x00":
        return "xz"
    if raw[:4] in (b"PK\x03\x04", b"PK\x05\x06"):
        return "zip"
    return None


_OPENERS = {"gzip": gzip.open, "bzip2": bz2.open, "xz": lzma.open}
_STREAM_ERRORS = (OSError, EOFError, zlib.error, lzma.LZMAError, ValueError)


def _gunzip_starts_with_fits(path: Path) -> bool:
    try:
        with gzip.open(path, "rb") as stream:
            return stream.read(9) == b"SIMPLE  ="
    except _STREAM_ERRORS:
        return False


def _stream_preview(read: Callable[[int], bytes], label: str, inner_suffix: str,
                    max_bytes: int, max_lines: int) -> dict:
    """The start of a decompressed stream, as text when it is text."""
    try:
        raw = read(max_bytes + 1)
    except _STREAM_ERRORS as exc:
        return _result("compressed", f"{label}-compressed",
                       f"The compressed stream could not be read: {exc}")
    cut = len(raw) > max_bytes
    raw = raw[:max_bytes]
    text = _text_preview(raw, cut, inner_suffix, max_bytes, max_lines)
    if text is None:
        return _result("compressed", f"{label}-compressed data",
                       "The decompressed content is not text.", cut)
    return _result("compressed", f"{text['format']} ({label}-compressed)", text["text"],
                   text["truncated"])


def _zip_preview(path: Path, max_bytes: int, max_lines: int) -> dict:
    try:
        with zipfile.ZipFile(path) as archive:
            members = [info for info in archive.infolist() if not info.is_dir()]
            if len(members) == 1 and not members[0].flag_bits & 0x1:
                member = members[0]
                with archive.open(member) as stream:
                    result = _stream_preview(stream.read, "zip",
                                             Path(member.filename).suffix.lower(),
                                             max_bytes, max_lines)
                result["format"] = result["format"].replace(
                    "(zip-compressed)", f"(zip: {member.filename})")
                return result
            listing = (f"{info.filename}  {info.file_size} bytes" for info in members)
            text, more = _bounded(listing, max_lines, max_bytes)
            return _result("compressed", f"ZIP archive, {len(members)} files", text, more)
    except (zipfile.BadZipFile, NotImplementedError, RuntimeError) + _STREAM_ERRORS as exc:
        return _result("compressed", "ZIP archive", f"The archive could not be read: {exc}")


# ── Directory ────────────────────────────────────────────────────────────────

def _directory_preview(path: Path, max_bytes: int, max_lines: int, hide_hidden: bool) -> dict:
    names: list[str] = []
    more = False
    with os.scandir(path) as entries:
        for entry in entries:
            if hide_hidden and entry.name.startswith("."):
                continue
            if len(names) >= DIRECTORY_MAX_ENTRIES:
                more = True
                break
            try:
                is_dir = entry.is_dir()
            except OSError:
                is_dir = False
            names.append(entry.name + ("/" if is_dir else ""))
    names.sort(key=lambda name: (not name.endswith("/"), name.lower()))
    text, cut = _bounded(names, max_lines, max_bytes)
    count = f"{len(names)}+" if more else str(len(names))
    return _result("directory", f"Directory, {count} entries", text, cut or more)


# ── Entry point ──────────────────────────────────────────────────────────────

def preview_path(path: str | Path, max_bytes: int = DEFAULT_MAX_BYTES,
                 max_lines: int = DEFAULT_MAX_LINES, *,
                 allow: Callable[[Path], bool] = lambda _p: True,
                 hide_hidden: bool = False) -> dict:
    """``{size_bytes, kind, format, text, truncated}`` for an existing path the
    caller has already confined to the data roots.

    ``allow`` vets a second file the preview would read (the ``.head`` beside
    a ``.bin``), so a sibling that is a symlink out of the roots is not
    followed. Raises ``OSError`` when the file cannot be opened at all.
    """
    path = Path(path)
    max_bytes, max_lines = clamp_bounds(max_bytes, max_lines)
    if path.is_dir():
        return {"size_bytes": 0, **_directory_preview(path, max_bytes, max_lines, hide_hidden)}
    size = path.stat().st_size
    if not path.is_file():
        # A FIFO or a device: opening it could block the thread for ever.
        return {"size_bytes": size, **_result("binary", "Special file (not read)")}
    return {"size_bytes": size, **_file_preview(path, max_bytes, max_lines, allow)}


def _file_preview(path: Path, max_bytes: int, max_lines: int,
                  allow: Callable[[Path], bool]) -> dict:
    suffix = path.suffix.lower()
    head = _vbt_head_of(path, allow)
    if head is not None:
        result = _vbt_preview(head, max_bytes, max_lines)
        if result is not None:
            return result

    with open(path, "rb") as handle:
        magic = handle.read(16)
    if magic.startswith(_HDF5_MAGIC):
        return _hdf5_preview(path, max_bytes, max_lines)
    if suffix in _HDF5_SUFFIXES and magic and not magic.startswith(b"SIMPLE"):
        # A user block puts the signature at 512, 1024, … bytes in.
        try:
            import h5py  # noqa: PLC0415
            if h5py.is_hdf5(path):
                return _hdf5_preview(path, max_bytes, max_lines)
        except Exception:  # noqa: BLE001
            pass
    if magic.startswith(b"SIMPLE"):
        return _fits_preview(path, False, max_bytes, max_lines)

    compression = _compression(magic)
    if compression == "gzip" and _gunzip_starts_with_fits(path):
        return _fits_preview(path, True, max_bytes, max_lines)
    if compression == "zip":
        return _zip_preview(path, max_bytes, max_lines)
    if compression is not None:
        inner = Path(path.stem).suffix.lower()
        with _OPENERS[compression](path, "rb") as stream:
            return _stream_preview(stream.read, compression, inner, max_bytes, max_lines)

    raw, cut = _read_start(path, max_bytes)
    text = _text_preview(raw, cut, suffix, max_bytes, max_lines)
    if text is not None:
        return text
    return _binary_preview(path, max_bytes, max_lines)
