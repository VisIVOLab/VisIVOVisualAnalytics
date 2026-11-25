"""The legacy VisIVO Importer's raw formats, read in place (issue #551).

Three formats of the VisIVO Importer describe raw binary dumps with a small
text *descriptor* (the file's first word names it) that lists the data files,
one per snapshot, each with its time tag. The importer converted each file to
a VisIVO Binary Table; here they are read where they lie, as the other
simulation formats are (VBT: app/volume_source.py, PLUTO: app/pluto_source.py,
Gadget: app/gadget_snapshot.py).

``rawPointsDesc``  one table of points per file: ``x y z`` of every point
                   interleaved (3 N values), then each scalar field as a block
                   of N values. The scalar count is not written down: it is
                   what the file's size holds (``size / (N * itemsize) - 3``).
``rawGridsDesc``   one scalar field per file, one value per mesh point, x
                   fastest: a volume (an image when 2-D).
``flyDesc``        N-body snapshots of the FLY code: ``x y z`` of every
                   particle interleaved, then ``vx vy vz`` interleaved.
                   A bare file (``.fly``, no descriptor) is read too when its
                   size and values leave one reading of its type and byte
                   order; else the user is told to write a descriptor.

A descriptor is whitespace-separated words, in this order (the importer's
own reader, ``Importers/*source.cpp``, is the specification)::

    rawPointsDesc | flyDesc   name  type  time  N  boxx boxy boxz  endian  (time file)...
    rawGridsDesc              name  ndim type  n1 .. n_ndim        time endian (time file)...

``type`` is ``Float``/``float``/``f`` or ``Double``/``double``/``d``, ``endian``
``b`` or ``l``. The importer documents an example with one more word before
the name (an id); it is skipped when present. The importer's own grid reader
never took a double (a pointer comparison that is always false) and converted
every grid as float; here the declared type is read, and a file whose size
does not match it is refused saying so.

Nothing here carries physical units: these formats have none, so values and
coordinates show as the file's own numbers (app/physical_units.py answers its
neutral table for them).

A file of a descriptor is named by a reference, ``<descriptor>::raw-points=<k>``
(points) or ``<descriptor>::raw-grid=<k>`` (a grid), ``k`` its place in the
list: a plain string, so it pickles to pyramid workers and render sessions,
which reopen it, and a series is the descriptor's list (:func:`series`).
Files are resolved beside the descriptor (or by absolute path); a remote
(``http://``, ``ftp://``) file is not fetched.
"""
from __future__ import annotations

import math
import os
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

import numpy as np

POINTS_SEP = "::raw-points="
GRID_SEP = "::raw-grid="

#: The first word of a descriptor -> its kind. ``rawPointDesc`` and
#: ``rawGridDesc`` are how the importer's manual spells the layout lists.
KINDS = {"rawpointsdesc": "rawpoints", "rawpointdesc": "rawpoints",
         "rawgridsdesc": "rawgrids", "rawgriddesc": "rawgrids", "flydesc": "fly"}
#: A bare FLY file has no descriptor; its extension is what names it.
BARE_FLY_SUFFIX = ".fly"
_REMOTE = ("http://", "https://", "ftp://", "sftp://")
_FLOAT, _DOUBLE = ("float", "f", "single", "float32"), ("double", "d", "float64")
_ENDIANS = {"b": "big", "big": "big", "l": "little", "little": "little"}
_SNIFF = 16
#: More scalar fields than this in a points file say a wrong point count.
MAX_SCALARS = 4096


class RawFormatError(ValueError):
    """A descriptor or data file that does not read as described."""


@dataclass(frozen=True)
class RawDescriptor:
    path: str                                  # the descriptor (or bare FLY file)
    kind: str                                  # "rawpoints" | "rawgrids" | "fly"
    name: str                                  # the variable's name
    dtype: np.dtype                            # sample type, byte order included
    entries: tuple[tuple[float, str], ...]     # (time tag, data file)
    rows: int = 0                              # points per file (rawpoints, fly)
    dims: tuple[int, ...] = ()                 # grid cells per axis, x first
    box: tuple[float, ...] = ()
    bare: bool = False                         # a FLY file with no descriptor

    @property
    def endian(self) -> str:
        # str, not byteorder: a native order reads "=" there.
        return "big" if self.dtype.str.startswith(">") else "little"

    @property
    def is_points(self) -> bool:
        return self.kind in ("rawpoints", "fly")


@dataclass(frozen=True)
class RawFile:
    """One data file of a descriptor, checked against its size."""
    desc: RawDescriptor
    index: int
    path: str
    time: float
    scalars: int = 0                           # rawpoints: fields after x, y, z

    @property
    def kind(self) -> str:
        return self.desc.kind

    @property
    def names(self) -> list[str]:
        if self.kind == "fly":
            return ["X", "Y", "Z", "Vx", "Vy", "Vz"]
        return ["X", "Y", "Z"] + [f"scalar{i}" for i in range(self.scalars)]

    @property
    def num_rows(self) -> int:
        return self.desc.rows

    @property
    def shape(self) -> tuple[int, int, int]:
        """A grid as (z, y, x), the cube path's convention; an axis a lower
        dimension lacks has one cell."""
        dims = (list(self.desc.dims) + [1, 1, 1])[:3]
        return (dims[2], dims[1], dims[0])

    def columns(self) -> dict[str, np.ndarray]:
        """Each column as a view of the file's map: positions (and a FLY's
        velocities) are strided over their interleaved block, scalars
        contiguous. Nothing is read until a row is."""
        n, dt = self.desc.rows, self.desc.dtype
        out: dict[str, np.ndarray] = {}
        blocks = [(0, 3)] + ([(3 * n * dt.itemsize, 3)] if self.kind == "fly" else [])
        names = iter(self.names)
        for offset, width in blocks:
            mm = np.memmap(self.path, dtype=dt, mode="r", offset=offset, shape=(n, width))
            for j in range(width):
                out[next(names)] = mm[:, j]
        if self.scalars:
            mm = np.memmap(self.path, dtype=dt, mode="r", offset=3 * n * dt.itemsize,
                           shape=(self.scalars, n))
            for j in range(self.scalars):
                out[next(names)] = mm[j]
        return out

    def describe(self) -> dict:
        """What the file states about itself, for the table's metadata."""
        d = self.desc
        return {"format": "fly" if self.kind == "fly" else "rawpoints",
                "descriptor": Path(d.path).name, "variable": d.name,
                "time": self.time, "step": self.index + 1, "steps": len(d.entries),
                "file": Path(self.path).name, "box_size": list(d.box),
                "byte_order": d.endian, "scalar_type": "float64" if d.dtype.itemsize == 8
                else "float32", "catalogue_kind": "particles"}


# ── The descriptor ───────────────────────────────────────────────────────────

def _type_of(word: str) -> str | None:
    w = word.lower()
    return "f4" if w in _FLOAT else "f8" if w in _DOUBLE else None


def _is_type(word: str) -> bool:
    return _type_of(word) is not None


def _dtype(path, word: str, endian: str) -> np.dtype:
    code = _type_of(word)
    if code is None:
        raise RawFormatError(f"{path}: unknown data type {word!r} (Float or Double).")
    if endian.lower() not in _ENDIANS:
        raise RawFormatError(f"{path}: unknown endianness {endian!r} (b or l).")
    return np.dtype(("<" if _ENDIANS[endian.lower()] == "little" else ">") + code)


def _entries(path, words: list[str]) -> tuple[tuple[float, str], ...]:
    out = []
    for i in range(0, len(words) - 1, 2):
        try:
            time = float(words[i])
        except ValueError:
            raise RawFormatError(f"{path}: expected a time tag before {words[i + 1]!r}, "
                                 f"got {words[i]!r}.") from None
        name = words[i + 1]
        if name.lower().startswith(_REMOTE):
            raise RawFormatError(f"{path} lists a remote file ({name}); this reads files "
                                 "on the backend's disks: copy it beside the descriptor.")
        out.append((time, name if os.path.isabs(name)
                    else os.path.join(os.path.dirname(os.path.abspath(path)), name)))
    if not out:
        raise RawFormatError(f"{path}: the descriptor lists no data file.")
    return tuple(out)


def _number(path, word: str, what: str, kind=int):
    try:
        return kind(word)
    except ValueError:
        raise RawFormatError(f"{path}: {what} should be a number, got {word!r}.") from None


def parse_descriptor(path: str | Path) -> RawDescriptor:
    """The descriptor at ``path``; RawFormatError (a ValueError) when it is
    not one or does not read as the importer's reader would."""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        words = fh.read().split()
    kind = KINDS.get(words[0].lower()) if words else None
    if kind is None:
        raise RawFormatError(f"{path} is not a rawPointsDesc, rawGridsDesc or flyDesc "
                             "descriptor.")
    t = words[1:]
    if kind == "rawgrids":
        # name ndim type n1..n_ndim time endian (time file)...
        if len(t) > 3 and not _is_type(t[2]) and _is_type(t[3]):
            t = t[1:]                                  # the manual's leading id
        if len(t) < 3:
            raise RawFormatError(f"{path}: the grid descriptor stops before its sizes.")
        ndim = _number(path, t[1], "the number of dimensions")
        if not 1 <= ndim <= 3:
            raise RawFormatError(f"{path}: {ndim} dimensions (1 to 3 are read).")
        if len(t) < 3 + ndim + 2:
            raise RawFormatError(f"{path}: the grid descriptor stops before its files.")
        dims = tuple(_number(path, w, "a grid size") for w in t[3:3 + ndim])
        if any(n < 1 for n in dims):
            raise RawFormatError(f"{path}: grid sizes must be positive, got {dims}.")
        endian = t[3 + ndim + 1]
        return RawDescriptor(str(path), kind, t[0], _dtype(path, t[2], endian),
                             _entries(path, t[3 + ndim + 2:]), dims=dims)
    # name type time N boxx boxy boxz endian (time file)...
    if len(t) > 2 and not _is_type(t[1]) and _is_type(t[2]):
        t = t[1:]                                      # the manual's leading id
    if len(t) < 8:
        raise RawFormatError(f"{path}: the descriptor stops before its files.")
    rows = _number(path, t[3], "the number of points")
    if rows < 1:
        raise RawFormatError(f"{path}: {rows} points.")
    box = tuple(_number(path, w, "a box size", float) for w in t[4:7])
    return RawDescriptor(str(path), kind, t[0], _dtype(path, t[1], t[7]),
                         _entries(path, t[8:]), rows=rows, box=box)


@lru_cache(maxsize=32)
def _cached(path: str, _size: int, _mtime_ns: int) -> RawDescriptor:
    return parse_descriptor(path)


def sniff(path: str | Path) -> str | None:
    """``"rawpoints"``, ``"rawgrids"``, ``"fly"`` for a descriptor, ``"bare-fly"``
    for a ``.fly`` file, None for anything else: the first bytes, nothing more."""
    p = Path(path)
    try:
        if not p.is_file():
            return None
        with open(p, "rb") as fh:
            head = fh.read(_SNIFF)
    except OSError:
        return None
    word = head.split(None, 1)[0].decode("ascii", "replace").lower() if head.strip() else ""
    kind = KINDS.get(word)
    if kind is not None:
        return kind
    return "bare-fly" if p.suffix.lower() == BARE_FLY_SUFFIX else None


def descriptor(path: str | Path) -> RawDescriptor:
    """The descriptor of ``path`` (a descriptor file, or a bare FLY file read
    by what it holds), cached by its size and time. RawFormatError when it is
    neither or cannot be read."""
    kind = sniff(path)
    if kind is None:
        raise RawFormatError(f"{path} is not a raw-descriptor file.")
    st = os.stat(path)
    if kind == "bare-fly":
        return _cached_bare(str(path), st.st_size, st.st_mtime_ns)
    return _cached(str(path), st.st_size, st.st_mtime_ns)


# ── A bare FLY file ──────────────────────────────────────────────────────────

#: A reading is believed when this share of its sampled values is implausible
#: at most and every other reading is at least ``_IMPLAUSIBLE`` worse. A float
#: file read as double still looks plausible (the high word of each double is
#: a float), so a size that fits both types is left to a descriptor.
_PLAUSIBLE, _IMPLAUSIBLE = 0.02, 0.1
_CHUNKS, _CHUNK_VALUES = 8, 512


@lru_cache(maxsize=32)
def _cached_bare(path: str, size: int, _mtime_ns: int) -> RawDescriptor:
    return bare_fly(path, size)


def bare_fly(path: str, size: int | None = None) -> RawDescriptor:
    """A descriptor for a FLY file that has none, from its size and values.

    The file holds 6 N samples, so a size divisible by 24 may be float and by
    48 double; the byte order is not written anywhere. Every reading the size
    allows is scored by how many of its sampled values no real field holds
    (the check that finds a VBT in the wrong order, app/vbt_byte_order.py); a
    reading is taken only when it is the one plausible reading. Otherwise
    RawFormatError says to write a ``flyDesc`` (the importer asks for
    ``--npoints``, ``--double`` and ``--bigendian`` for the same reason)."""
    from .vbt_byte_order import implausible_share  # noqa: PLC0415
    size = os.stat(path).st_size if size is None else size
    scores: list[tuple[float, str, int]] = []
    for code, item in (("f4", 4), ("f8", 8)):
        if size % (6 * item):
            continue
        rows = size // (6 * item)
        sample = _sample(path, size, item)
        for endian, mark in (("little", "<"), ("big", ">")):
            values = np.frombuffer(sample, dtype=np.dtype(mark + code))
            if values.size < 64:
                continue
            with np.errstate(invalid="ignore"):    # NaN patterns of a wrong reading
                scores.append((implausible_share(values), mark + code, rows))
    scores.sort()
    best = scores[0] if scores else None
    others = scores[1:]
    if (best is None or best[0] > _PLAUSIBLE
            or any(s[0] < best[0] + _IMPLAUSIBLE for s in others)):
        raise RawFormatError(
            f"{Path(path).name} is a FLY file with no descriptor, and its size and values "
            "do not say whether it is float or double, little or big endian, or how many "
            "particles it holds: write a flyDesc descriptor beside it (type, number of "
            "particles, byte order and the file's name) and open that.")
    _score, code, rows = best
    return RawDescriptor(str(path), "fly", Path(path).stem, np.dtype(code), ((0.0, str(path)),),
                         rows=rows, bare=True)


def _sample(path: str, size: int, item: int) -> bytes:
    """Chunks spread over the file, each a whole number of samples."""
    length = _CHUNK_VALUES * item
    if size <= length * _CHUNKS:
        with open(path, "rb") as fh:
            return fh.read()
    starts = [(i * (size - length) // (_CHUNKS - 1)) // item * item for i in range(_CHUNKS)]
    parts = []
    with open(path, "rb") as fh:
        for s in starts:
            fh.seek(s)
            parts.append(fh.read(length))
    return b"".join(parts)


# ── One file of a descriptor ─────────────────────────────────────────────────

def split_ref(path: str | Path, sep: str) -> tuple[str, int | None]:
    text = str(path)
    base, found, k = text.rpartition(sep)
    if found and k.isascii() and k.isdigit():
        return base, int(k)
    return text, None


def split_points_ref(path: str | Path) -> tuple[str, int | None]:
    """``(descriptor, k)`` of a points reference; ``(path, None)`` else."""
    return split_ref(path, POINTS_SEP)


def split_grid_ref(path: str | Path) -> tuple[str, int | None]:
    return split_ref(path, GRID_SEP)


def is_points_ref(path: str | Path) -> bool:
    """From the string alone."""
    return split_points_ref(path)[1] is not None


def is_grid_ref(path: str | Path) -> bool:
    return split_grid_ref(path)[1] is not None


def points_ref(path: str | Path, k: int = 0) -> str:
    return f"{split_points_ref(path)[0]}{POINTS_SEP}{int(k)}"


def grid_ref(path: str | Path, k: int = 0) -> str:
    return f"{split_grid_ref(path)[0]}{GRID_SEP}{int(k)}"


def open_file(path: str | Path, index: int | None = None, want: str | None = None) -> RawFile:
    """File ``index`` of the descriptor ``path`` names (a reference, the
    descriptor itself with ``index`` or the first file, a bare FLY file),
    its size checked against what the descriptor says. ``want`` is
    ``"points"`` or ``"grid"``: the other kind is refused. RawFormatError
    (a ValueError) for anything that does not read as described."""
    base, k = split_points_ref(path)
    if k is None:
        base, k = split_grid_ref(path)
    if k is None:
        k = 0 if index is None else int(index)
    desc = descriptor(base)
    if want is not None and desc.is_points != (want == "points"):
        raise RawFormatError(f"{Path(base).name} describes "
                             f"{'points' if desc.is_points else 'a grid'}, not {want}.")
    if not 0 <= k < len(desc.entries):
        raise RawFormatError(f"file {k} is outside 0..{len(desc.entries) - 1} of "
                             f"{Path(base).name}.")
    time, data = desc.entries[k]
    try:
        size = os.stat(data).st_size
    except OSError:
        raise RawFormatError(f"{Path(base).name} lists {Path(data).name}, which is not "
                             "there.") from None
    item = desc.dtype.itemsize
    kind_name = "float64" if item == 8 else "float32"
    scalars = 0
    if desc.kind == "rawgrids":
        expected = math.prod(desc.dims) * item
        other = math.prod(desc.dims) * (12 - item)
        if size != expected:
            hint = (f" (that is the size of {'float32' if item == 8 else 'float64'} "
                    "samples: the descriptor's type may be wrong)") if size == other else ""
            raise RawFormatError(f"{Path(data).name} holds {size} bytes; the descriptor "
                                 f"describes {expected} ({kind_name}, "
                                 f"{' x '.join(str(n) for n in desc.dims)}){hint}.")
    elif desc.kind == "fly":
        expected = 6 * desc.rows * item
        if size != expected:
            raise RawFormatError(f"{Path(data).name} holds {size} bytes; the descriptor "
                                 f"describes {expected} (6 x {desc.rows} {kind_name}).")
    else:
        block = desc.rows * item
        if size % block or size // block < 3:
            raise RawFormatError(
                f"{Path(data).name} holds {size} bytes: not 3 + n blocks of {desc.rows} "
                f"{kind_name} values ({block} bytes each) as the descriptor says.")
        scalars = size // block - 3
        if scalars > MAX_SCALARS:
            raise RawFormatError(f"{Path(data).name} would hold {scalars} scalar fields: the "
                                 "descriptor's point count is probably too small.")
    return RawFile(desc, k, data, time, scalars)


def points_file(path: str | Path) -> RawFile:
    return open_file(path, want="points")


def grid_file(path: str | Path) -> RawFile:
    return open_file(path, want="grid")


# ── Opening and listing ──────────────────────────────────────────────────────

def _checked(path: str | Path, want: str, allowed) -> str | None:
    """The reference ``path`` opens by, or None when it is not a file of this
    kind (``want``). ``allowed`` (path -> bool): the data file read must pass
    it too, as the descriptor passed the data roots; one that does not raises
    PermissionError."""
    base, k = split_points_ref(path) if want == "points" else split_grid_ref(path)
    kind = sniff(base)
    if kind is None or (kind in ("rawpoints", "fly", "bare-fly")) != (want == "points"):
        return None
    rf = open_file(base, k, want)
    if allowed is not None and not allowed(rf.path):
        raise PermissionError(f"{Path(rf.path).name}, a file of this descriptor, lies "
                              "outside the folders this backend may read.")
    return (points_ref if want == "points" else grid_ref)(base, rf.index)


def points_dataset_path(path: str | Path, allowed=None) -> str | None:
    """The reference a table of raw points (rawpoints or FLY) opens by, given
    its descriptor, a bare ``.fly`` file or a reference; None for anything
    else; ValueError saying why for one that cannot be read."""
    return _checked(path, "points", allowed)


def grid_dataset_path(path: str | Path, allowed=None) -> str | None:
    """As :func:`points_dataset_path`, for a raw grid (volume or image)."""
    return _checked(path, "grid", allowed)


def listing(path: str | Path) -> list[dict]:
    """Every file of the descriptor ``path`` belongs to, ``{name, path,
    time}``, whose data file is there: one dataset each."""
    base, _k = split_points_ref(path)
    ref = points_ref
    if _k is None:
        base, _k = split_grid_ref(path)
        ref = grid_ref
    desc = descriptor(base)
    out = []
    for k, (time, data) in enumerate(desc.entries):
        if os.path.isfile(data):
            out.append({"name": f"{desc.name} t={time:g} ({Path(data).name})",
                        "path": ref(base, k), "time": time})
    return out


def series(path: str | Path, allowed=None) -> dict | None:
    """The series a descriptor's list makes (app/snapshot_series.py shape:
    ``paths`` of references in the listed order, ``numbers`` the time tags,
    ``index`` of ``path``, ``count``); None when ``path`` is not a file of a
    descriptor. A member ``allowed`` refuses, or whose file is not there, is
    left out."""
    base, k = split_points_ref(path)
    ref = points_ref
    if k is None:
        base, k = split_grid_ref(path)
        ref = grid_ref
    if k is None or sniff(base) is None:
        return None
    desc = descriptor(base)
    found = [(i, t) for i, (t, data) in enumerate(desc.entries)
             if i == k or (os.path.isfile(data) and (allowed is None or allowed(data)))]
    if not any(i == k for i, _ in found):
        return None
    return {"paths": [ref(base, i) for i, _ in found], "numbers": [t for _, t in found],
            "index": next(n for n, (i, _) in enumerate(found) if i == k),
            "count": len(found)}


def identity_files(path: str | Path) -> list[str]:
    """The files whose versions make a file of a descriptor what it is: its
    data and its descriptor ([] when ``path`` is not one or cannot be read)."""
    try:
        rf = open_file(path)
    except (OSError, ValueError):
        return []
    return [rf.path, rf.desc.path]
