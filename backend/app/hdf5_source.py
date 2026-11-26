"""Generic HDF5 tables and volumes, read in place (issue #552).

Any HDF5 that is not a Gadget/AREPO snapshot or SubFind catalogue
(app/gadget_snapshot.py) and not a beamformed dynamic spectrum
(app/hdf5_dynspec.py, which keeps its own detection) is read where it lies:

* a **table** of points: the 1-D numeric datasets of one group that share a
  length, or a 1-D compound dataset (one column per numeric field);
* a **volume**: a rank-3 numeric dataset (a cube);
* an **image**: a rank-2 numeric dataset.

A table is a component of the particle seam (app/point_table.py
COMPONENT_FORMATS, as a Gadget particle type is): its columns are views of the
file's memory map when the dataset is contiguous and uncompressed (the offset
HDF5 reports), and read by rows, window by window in whole chunks, otherwise,
so nothing of the file is read when it opens. A volume is a raw field
(app/volume_source.py RawField): contiguous and uncompressed, it is
memory-mapped at the dataset's own offset and takes the brick pyramid and the
render sessions as a PLUTO variable does; chunked or compressed, it is copied
once, slab by slab, into a raw file of the workspace cache and that is mapped.
A lazy chunked reader was not built: every reader of a cube takes a map of
samples (``RawField.memmap``), and a second reading mechanism would have to
be taught to the pyramid, the moments, the statistics and the render sessions.

Two references name what to read, plain strings, so they pickle to the pyramid
workers and render sessions, which reopen them::

    <file>::hdf5=/group/dataset[o0:c0,o1:c1,o2:c2]   a volume or an image
    <file>::hdf5-table=/group[o:c]                   a table (a group, or a compound dataset)

``::hdf5=`` is what a user writes for either kind (:func:`canonical_ref` turns
it into the one the dataset is read by); the optional ``[...]`` is a
hyperslab, ``offset:count`` per axis in the file's own axis order (the
importer's ``--hyperslab``): an empty part is the whole axis, a missing count
runs to the end. A table's has one part, its rows.

Units are read from the dataset's attributes (``units``, ``unit``, ``BUNIT``)
and given to app/physical_units.py as a label with the factor 1: the values
are the file's.
"""
from __future__ import annotations

import hashlib
import os
import re
import threading
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

import numpy as np

TABLE_SEP = "::hdf5-table="
VOLUME_SEP = "::hdf5="
MAGIC = b"\x89HDF\r\n\x1a\n"
#: Attributes that give a dataset's unit, first found wins.
UNIT_ATTRS = ("units", "unit", "BUNIT")
_SLAB = re.compile(r"\[([^\[\]]*)\]$")

#: What marks a file as a beamformed / dynamic spectrum (app/hdf5_dynspec.py
#: reads these): the LOFAR BFData groups, or its sampling attributes. Such a
#: file keeps the dynamic-spectrum opener.
#: (A bare ``DT`` is left out: simulation files carry one.)
_DYNSPEC_ATTRS = frozenset({
    "TSAMP", "TSAMPLING", "CADENCE_S", "FREQ_START_HZ", "FREQ_LOW_HZ",
    "FCH1", "FREQ_LOW", "DELTA_FREQ_HZ", "DF_HZ", "FOFF", "CHANNEL_BW_HZ"})
_DYNSPEC_NAMES = ("dynspec", "dynamic_spec", "waterfall", "spectrogram")


# ── References ───────────────────────────────────────────────────────────────

Slab = tuple[tuple[int, "int | None"], ...]


def parse_slab(text: str) -> Slab:
    """``"o0:c0,o1:c1"`` as ``((o0, c0), (o1, c1))``: an empty part is the whole
    axis ``(0, None)``, a missing count (``"5:"`` or ``"5"``) runs to the end."""
    out = []
    for part in text.split(","):
        part = part.strip()
        if part in ("", ":"):
            out.append((0, None))
            continue
        start, _, count = part.partition(":")
        try:
            offset = int(start) if start.strip() else 0
            number = int(count) if count.strip() else None
        except ValueError:
            raise ValueError(f"hyperslab {text!r}: {part!r} is not offset:count.") from None
        if offset < 0 or (number is not None and number < 1):
            raise ValueError(f"hyperslab {text!r}: {part!r} needs an offset of 0 or more "
                             "and a count of 1 or more.")
        out.append((offset, number))
    return tuple(out)


def _split(path, sep: str) -> tuple[str, str | None, Slab | None]:
    """``(file, in-file name, hyperslab)`` of a reference with ``sep``;
    ``(path, None, None)`` for any other string. A bad hyperslab raises."""
    text = str(path)
    base, found, rest = text.rpartition(sep)
    if not found:
        return text, None, None
    slab = None
    match = _SLAB.search(rest)
    if match:
        slab = parse_slab(match.group(1))
        rest = rest[:match.start()]
    return base, rest or "/", slab


def split_volume_ref(path) -> tuple[str, str | None, Slab | None]:
    return _split(path, VOLUME_SEP)


def split_table_ref(path) -> tuple[str, str | None, Slab | None]:
    return _split(path, TABLE_SEP)


def is_volume_ref(path) -> bool:
    """From the string alone."""
    base, found, _ = str(path).rpartition(VOLUME_SEP)
    return bool(found and base)


def is_table_ref(path) -> bool:
    """From the string alone (a bare suffix counts: a series member's)."""
    return TABLE_SEP in str(path)


def is_ref(path) -> bool:
    return is_volume_ref(path) or is_table_ref(path)


def split_ref(path) -> tuple[str, str | None]:
    """``(file, what is named)`` of a table reference, as every component
    module offers it (app/point_table.py): the file is what the data roots
    check, the rest (name and hyperslab text) goes back on after."""
    text = str(path)
    base, found, rest = text.rpartition(TABLE_SEP)
    return (base, rest) if found else (text, None)


def ref_base(path) -> str:
    """The file a reference of either kind reads; ``path`` for anything else."""
    for sep in (TABLE_SEP, VOLUME_SEP):
        base, found, _ = str(path).rpartition(sep)
        if found and base:
            return base
    return str(path)


def make_ref(base, kind: str, name: str, slab_text: str = "") -> str:
    sep = TABLE_SEP if kind == "table" else VOLUME_SEP
    return f"{base}{sep}{name}" + (f"[{slab_text}]" if slab_text else "")


def _slab_text(path) -> str:
    match = _SLAB.search(str(path))
    return match.group(1) if match else ""


def is_hdf5_file(path) -> bool:
    try:
        with open(path, "rb") as fh:
            return fh.read(len(MAGIC)) == MAGIC
    except OSError:
        return False


def _open(path):
    from .hdf5_dynspec import _open_h5   # noqa: PLC0415
    return _open_h5(Path(path))


def _stat(path) -> tuple[int, int]:
    st = os.stat(path)
    return st.st_size, st.st_mtime_ns


# ── What a file holds ────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Candidate:
    """Something in a file that opens: ``path`` is the dataset (a group, for a
    table made of the datasets of one)."""
    path: str
    kind: str                         # "table" | "image" | "cube"
    shape: tuple[int, ...]            # a table: (rows,)
    dtype: str
    units: str = ""
    columns: tuple[str, ...] = ()

    @property
    def label(self) -> str:
        unit = f", {self.units}" if self.units else ""
        if self.kind == "table":
            return f"{self.path}  (table: {self.shape[0]} rows, {len(self.columns)} columns)"
        return f"{self.path}  ({' x '.join(str(n) for n in self.shape)}, {self.dtype}{unit})"

    @property
    def rows(self) -> int:
        return int(self.shape[0]) if self.kind == "table" else 0


@dataclass(frozen=True)
class Layout:
    candidates: tuple[Candidate, ...]
    dynspec: bool


def _text(value) -> str:
    if isinstance(value, np.ndarray):
        value = value.reshape(-1)[0] if value.size else ""
    if isinstance(value, bytes):
        value = value.decode("utf-8", "replace")
    return str(value).strip()


def _unit(ds) -> str:
    for key in UNIT_ATTRS:
        if key in ds.attrs:
            try:
                return _text(ds.attrs[key])
            except (TypeError, ValueError, IndexError):
                continue
    return ""


def _scalar_ok(dtype: np.dtype) -> bool:
    """An integer, or a float the readers take (float32 or float64: not a
    half or an extended type)."""
    return dtype.kind in "iu" or (dtype.kind == "f" and dtype.itemsize in (4, 8))


def _numeric(dtype: np.dtype) -> bool:
    return dtype.names is None and _scalar_ok(dtype)


def _compound_fields(dtype: np.dtype) -> list[str]:
    """The numeric scalar fields of a compound type, in order."""
    out = []
    for name in dtype.names or ():
        field = dtype.fields[name][0]
        if field.subdtype is None and _scalar_ok(field):
            out.append(name)
    return out


def _table_members(group) -> list[tuple[str, object]]:
    """``(name, dataset)`` of the 1-D numeric datasets of ``group`` that share
    the length most of them have (the longer on a tie)."""
    import h5py   # noqa: PLC0415
    by_length: dict[int, list] = {}
    for name in group:
        if isinstance(group.get(name, getlink=True), h5py.ExternalLink):
            continue                      # a file the link names is not this one
        item = group[name]
        if (isinstance(item, h5py.Dataset) and item.ndim == 1 and _numeric(item.dtype)
                and not item.external and not item.is_virtual):
            by_length.setdefault(int(item.shape[0]), []).append((name, item))
    if not by_length:
        return []
    best = max(by_length, key=lambda n: (len(by_length[n]), n))
    return by_length[best]


def _looks_dynspec(f, datasets: list) -> bool:
    if any(str(name).startswith("SUB_ARRAY_POINTING") for name in f.keys()):
        return True
    if {str(k).upper() for k in f.attrs} & _DYNSPEC_ATTRS:
        return True
    for path, ds in datasets:
        if ds.ndim < 2 or not _numeric(ds.dtype):
            continue
        if {str(k).upper() for k in ds.attrs} & _DYNSPEC_ATTRS:
            return True
        if any(hint in path.lower() for hint in _DYNSPEC_NAMES):
            return True
    return False


@lru_cache(maxsize=16)
def _layout(path: str, _size: int, _mtime_ns: int) -> Layout:
    import h5py   # noqa: PLC0415
    with _open(path) as f:
        datasets: list[tuple[str, object]] = []

        def visit(name, obj):
            if isinstance(obj, h5py.Dataset):
                datasets.append(("/" + name, obj))
        f.visititems(visit)
        found: list[tuple[int, Candidate]] = []
        groups_done: set[str] = set()
        for order, (full, ds) in enumerate(datasets):
            shape, dtype = tuple(int(n) for n in ds.shape), ds.dtype
            if ds.external or ds.is_virtual:
                continue
            if len(shape) == 1 and dtype.names:
                fields = _compound_fields(dtype)
                if fields and shape[0] > 0:
                    found.append((order, Candidate(full, "table", shape, "compound",
                                                   columns=tuple(fields))))
            elif len(shape) == 1 and _numeric(dtype):
                parent = full.rpartition("/")[0] or "/"
                if parent in groups_done:
                    continue
                groups_done.add(parent)
                members = _table_members(f[parent])
                if len(members) >= 2 and members[0][1].shape[0] > 0:
                    found.append((order, Candidate(
                        parent, "table", (int(members[0][1].shape[0]),), "mixed",
                        columns=tuple(name for name, _ in members))))
            elif len(shape) == 2 and _numeric(dtype) and min(shape) > 1:
                found.append((order, Candidate(full, "image", shape, dtype.str, _unit(ds))))
            elif len(shape) == 3 and _numeric(dtype) and min(shape) > 0:
                found.append((order, Candidate(full, "cube", shape, dtype.str, _unit(ds))))
        found.sort(key=lambda entry: entry[0])
        return Layout(tuple(c for _, c in found), _looks_dynspec(f, datasets))


def layout(path) -> Layout:
    """What the HDF5 file ``path`` holds (names, shapes and attributes only: no
    dataset is read); OSError for a file that is not readable as HDF5."""
    size, mtime = _stat(path)
    return _layout(str(path), size, mtime)


def generic(path) -> Layout | None:
    """The layout of ``path`` when it is a generic HDF5 file: readable, not a
    Gadget snapshot (it keeps its own detection) and not a dynamic spectrum;
    None for anything else, including a file that cannot be read as HDF5."""
    from . import gadget_snapshot as gs   # noqa: PLC0415
    if not is_hdf5_file(path) or gs.detect(path) is not None:
        return None
    try:
        lay = layout(path)
    except (OSError, ValueError, RuntimeError, KeyError):
        return None
    return None if lay.dynspec else lay


def _kind_of(f, name: str) -> tuple[str, object]:
    """``("table" | "volume", object)`` for what ``name`` is in ``f``; a
    ValueError says why it is neither."""
    import h5py   # noqa: PLC0415
    if name not in f:
        raise ValueError(f"{name} is not in the file.")
    # What the file itself holds: a link to another file, a dataset whose
    # storage is another file (external or virtual) would read where the data
    # roots were not asked.
    parts = [p for p in name.split("/") if p]
    for i in range(len(parts)):
        if isinstance(f.get("/" + "/".join(parts[:i + 1]), getlink=True), h5py.ExternalLink):
            raise ValueError(f"{name} is a link to another file; it is not followed.")
    obj = f[name]
    if isinstance(obj, h5py.Dataset) and (obj.external or obj.is_virtual):
        raise ValueError(f"{name} is stored in other files (external or virtual); open "
                         "those.")
    if isinstance(obj, h5py.Group):
        if len(_table_members(obj)) < 1:
            raise ValueError(f"{name} holds no 1-D numeric dataset to read as columns.")
        return "table", obj
    shape, dtype = tuple(obj.shape), obj.dtype
    if len(shape) == 1 and dtype.names and _compound_fields(dtype):
        return "table", obj
    if len(shape) in (2, 3) and _numeric(dtype):
        return "volume", obj
    raise ValueError(f"{name} is a {len(shape)}-D dataset of {dtype}: a table is a group of "
                     "1-D datasets or a compound dataset, a volume a rank-2 or rank-3 "
                     "numeric dataset.")


def _listing(path, wanted) -> str:
    return "; ".join(f"{path}{TABLE_SEP if c.kind == 'table' else VOLUME_SEP}{c.path}"
                     for c in wanted)


def canonical_ref(path, want: str | None = None) -> str | None:
    """The reference the file or reference ``path`` is read by, or None when
    it is no generic HDF5.

    ``want`` is ``"table"`` or ``"volume"`` (None: either). A reference
    (either separator) is checked against the file and given the separator of
    what it names. A plain file opens when exactly one thing in it is of the
    kind asked for; with several a ValueError lists them (the user names one
    with ``<file>::hdf5=/path``). OSError for a file that cannot be read."""
    text = str(path)
    if is_ref(text):
        base = ref_base(text)
        sep = TABLE_SEP if is_table_ref(text) else VOLUME_SEP
        _, name, slab = _split(text, sep)
        if not is_hdf5_file(base):
            raise ValueError(f"{Path(base).name} is not an HDF5 file.")
        with _open(base) as f:
            kind, obj = _kind_of(f, name)
            if slab is not None:        # a hyperslab outside the dataset says so here
                import h5py   # noqa: PLC0415
                shape = ((int(_table_members(obj)[0][1].shape[0]),)
                         if isinstance(obj, h5py.Group) else tuple(int(n) for n in obj.shape))
                _window(shape, slab)
        if want is not None and kind != want:
            raise ValueError(f"{name} is a {kind}, not a {want}.")
        return make_ref(base, kind, name, _slab_text(text))
    lay = generic(text)
    if lay is None:
        return None
    wanted = [c for c in lay.candidates
              if want is None or (c.kind == "table") == (want == "table")]
    if not wanted:
        if want == "volume" and lay.candidates:
            raise ValueError(f"{Path(text).name} holds tables only: open it as a 3-D catalogue.")
        return None
    if len(wanted) > 1:
        raise ValueError(f"{Path(text).name} holds {len(wanted)} datasets that can open: name "
                         f"one as <file>{VOLUME_SEP}/path ({_listing(text, wanted)}).")
    only = wanted[0]
    return make_ref(text, "table" if only.kind == "table" else "volume", only.path)


# ── Memory-mapping, or a copy of what cannot be mapped ──────────────────────

def _in_place(ds) -> int | None:
    """The byte offset of a dataset stored as one contiguous, unfiltered block
    of the file itself, else None."""
    try:
        plist = ds.id.get_create_plist()
        if (ds.chunks is not None or ds.compression is not None or plist.get_nfilters()
                or ds.is_virtual or ds.external):
            return None
        return ds.id.get_offset()
    except (AttributeError, TypeError, ValueError):
        return None


def _window(shape, slab: Slab | None) -> list[tuple[int, int]]:
    """``(start, count)`` per axis of ``shape`` for a hyperslab; ValueError for
    one with more parts than axes or outside the dataset."""
    parts = list(slab or ())
    if len(parts) > len(shape):
        raise ValueError(f"hyperslab of {len(parts)} axes for a dataset of {len(shape)}.")
    parts += [(0, None)] * (len(shape) - len(parts))
    out = []
    for axis, ((offset, count), n) in enumerate(zip(parts, shape)):
        count = n - offset if count is None else count
        if offset >= n or offset + count > n:
            raise ValueError(f"hyperslab axis {axis} ({offset}:{count}) is outside the "
                             f"dataset's {n} samples.")
        out.append((offset, count))
    return out


def _contiguous(window: list[tuple[int, int]], shape) -> bool:
    """Whether a hyperslab is one run of the dataset's bytes: once an axis
    takes more than one sample, every later axis is taken whole."""
    seen_wide = False
    for (offset, count), n in zip(window, shape):
        if seen_wide and count != n:
            return False
        seen_wide = seen_wide or count > 1
    return True


@dataclass(frozen=True)
class Placement:
    """Where a volume's samples are: a file, an offset and a (z, y, x) shape."""
    path: str
    offset: int
    dtype: np.dtype
    shape: tuple[int, int, int]
    sidecars: tuple[str, ...] = ()


def copy_dir() -> Path:
    """Where chunked or compressed volumes are kept as raw files (a sibling of
    the decompression cache: its eviction does not know these)."""
    from .compressed_fits import cache_dir   # noqa: PLC0415
    return Path(str(cache_dir()) + "-hdf5")


#: Bytes read from a dataset at once while it is copied.
COPY_BLOCK_BYTES = 64 << 20
_COPY_LOCKS: dict[str, threading.Lock] = {}
_COPY_GUARD = threading.Lock()


def _copy_path(base: str, name: str, slab_text: str) -> Path:
    size, mtime = _stat(base)
    key = hashlib.sha256(f"{os.path.realpath(base)}\n{name}\n{slab_text}\n{size}\n{mtime}"
                         .encode("utf-8", "surrogateescape")).hexdigest()[:24]
    return copy_dir() / key / "data.bin"


def _copy_volume(base: str, name: str, window, native: np.dtype, target: Path) -> None:
    """Write the hyperslab of a dataset to ``target``, a block of its first
    axis at a time (whole HDF5 chunks are decompressed, one block in memory)."""
    import shutil   # noqa: PLC0415
    target.parent.mkdir(parents=True, exist_ok=True)
    total = int(np.prod([c for _, c in window])) * native.itemsize
    free = shutil.disk_usage(target.parent).free
    if free < total * 1.05:
        raise OSError(f"Not enough free space in {target.parent} to unpack the dataset "
                      f"({total >> 20} MB needed, {free >> 20} MB free).")
    tmp = target.with_name(f"{target.name}.{os.getpid()}.{threading.get_ident()}.tmp")
    try:
        with _open(base) as f, open(tmp, "wb") as out:
            ds = f[name]
            (first, count), rest = window[0], window[1:]
            per = max(1, int(np.prod([c for _, c in rest])) * native.itemsize)
            step = max(1, COPY_BLOCK_BYTES // per)
            tail = tuple(slice(o, o + c) for o, c in rest)
            for a in range(first, first + count, step):
                block = ds[(slice(a, min(first + count, a + step)),) + tail]
                np.ascontiguousarray(block, dtype=native).tofile(out)
        os.replace(tmp, target)
    finally:
        tmp.unlink(missing_ok=True)


def volume_placement(ref) -> Placement:
    """Where the samples of the volume or image ``ref`` are, as (z, y, x) (an
    image one plane deep). Contiguous and unfiltered: mapped in the file at the
    dataset's offset, moved on to the first sample of a hyperslab that is one run
    of bytes. Anything else is copied once into :func:`copy_dir`."""
    base, name, slab = split_volume_ref(ref)
    if name is None:
        raise ValueError(f"{ref} names no HDF5 dataset.")
    with _open(base) as f:
        kind, ds = _kind_of(f, name)
        if kind != "volume":
            raise ValueError(f"{name} is a table, not a volume.")
        shape, dtype = tuple(int(n) for n in ds.shape), ds.dtype
        window = _window(shape, slab)
        out = tuple(c for _, c in window)
        out = (1, *out) if len(out) == 2 else out
        offset = _in_place(ds)
        if offset is not None and _contiguous(window, shape):
            skip = 0
            stride = dtype.itemsize
            for (start, _count), n in zip(reversed(window), reversed(shape)):
                skip += start * stride
                stride *= n
            return Placement(base, int(offset) + skip, dtype, out)   # type: ignore[arg-type]
    native = dtype.newbyteorder("=")
    target = _copy_path(base, name, _slab_text(ref))
    with _COPY_GUARD:
        lock = _COPY_LOCKS.setdefault(str(target), threading.Lock())
    with lock:
        if not target.is_file():
            _copy_volume(base, name, window, native, target)
    return Placement(str(target), 0, native, out)   # type: ignore[arg-type]


def volume_shape(ref) -> tuple[int, int, int]:
    """The (z, y, x) shape of the volume or image ``ref`` names (its hyperslab
    applied) from the file's metadata: nothing is mapped or copied."""
    base, name, slab = split_volume_ref(ref)
    with _open(base) as f:
        kind, ds = _kind_of(f, name or "/")
        if kind != "volume":
            raise ValueError(f"{name} is a table, not a volume.")
        out = tuple(c for _, c in _window(tuple(int(n) for n in ds.shape), slab))
    return (1, *out) if len(out) == 2 else out   # type: ignore[return-value]


def volume_units(ref) -> str:
    base, name, _ = split_volume_ref(ref)
    with _open(base) as f:
        return _unit(f[name]) if name in f else ""


def volume_siblings(ref) -> list[dict]:
    """Every volume or image of the file ``ref`` is in, ``{name, path}``."""
    base = ref_base(ref)
    try:
        lay = layout(base)
    except (OSError, ValueError, RuntimeError):
        return []
    return [{"name": c.path, "path": make_ref(base, "volume", c.path)}
            for c in lay.candidates if c.kind != "table"]


def identity_files(ref) -> list[str]:
    """The files whose version is a version of what ``ref`` reads."""
    return [ref_base(ref)]


# ── A table: a component of the particle seam ───────────────────────────────

@dataclass
class _Column:
    name: str
    dataset: str                       # in-file path
    field: str                         # a compound dataset's field, else ""
    dtype: np.dtype
    view: np.ndarray | None            # a view of the file's map, when it can be mapped
    unit: str = ""


class Hdf5Table:
    """Named columns of an HDF5 group or compound dataset, read where they lie:
    the interface app/point_table.py asks of a component (``names``,
    ``num_rows``, ``take``, ``columns``, ``column_dtype``, ``units``,
    ``physical``, ``periods``, ``describe``)."""

    def __init__(self, ref):
        from .gadget_snapshot import piece_cache_bytes   # noqa: PLC0415
        self.ref = str(ref)
        self.base, name, slab = split_table_ref(ref)
        if name is None:
            raise ValueError(f"{ref} names no HDF5 table.")
        self.name = name
        self.slab_text = _slab_text(ref)
        self._budget = piece_cache_bytes()
        self._kept: dict[str, np.ndarray] = {}
        self._kept_bytes = 0
        self._lock = threading.Lock()
        with _open(self.base) as f:
            kind, obj = _kind_of(f, name)
            if kind != "table":
                raise ValueError(f"{name} is a volume, not a table.")
            import h5py   # noqa: PLC0415
            if isinstance(obj, h5py.Group):
                members = _table_members(obj)
                total = int(members[0][1].shape[0])
                (self.start, self.num_rows), = _window((total,), slab)
                self.columns_list = [
                    _Column(n, d.name, "", d.dtype, self._map(d, d.dtype), _unit(d))
                    for n, d in members]
            else:
                total = int(obj.shape[0])
                (self.start, self.num_rows), = _window((total,), slab)
                shared = self._map(obj, obj.dtype)
                unit = _unit(obj)
                self.columns_list = [
                    _Column(n, obj.name, n, obj.dtype[n], None if shared is None else shared[n],
                            unit if len(_compound_fields(obj.dtype)) == 1 else "")
                    for n in _compound_fields(obj.dtype)]
        self.names = [c.name for c in self.columns_list]
        self._by_name = {c.name: c for c in self.columns_list}

    def _map(self, ds, dtype) -> np.ndarray | None:
        offset = _in_place(ds)
        if offset is None:
            return None
        return np.memmap(self.base, dtype=dtype, mode="r", shape=(self.num_rows,),
                         offset=int(offset) + self.start * dtype.itemsize)

    # -- reading ---------------------------------------------------------

    def column_dtype(self, name: str) -> np.dtype:
        if name not in self._by_name:
            raise KeyError(name)
        return self._by_name[name].dtype.newbyteorder("=")

    def _read(self, ds, col: _Column, first: int, last: int) -> np.ndarray:
        block = ds[self.start + first:self.start + last]
        return block[col.field] if col.field else block

    def _whole(self, col: _Column) -> np.ndarray | None:
        """The column read once and kept, when the table's budget allows."""
        with self._lock:
            kept = self._kept.get(col.name)
            if kept is not None:
                return kept
            size = self.num_rows * col.dtype.itemsize
            if self._kept_bytes + size > self._budget:
                return None
            with _open(self.base) as f:
                values = np.asarray(self._read(f[col.dataset], col, 0, self.num_rows))
            values = values.astype(col.dtype.newbyteorder("="), copy=False)
            self._kept[col.name] = values
            self._kept_bytes += values.nbytes
            return values

    def _windowed(self, col: _Column, rows: np.ndarray) -> np.ndarray:
        """The rows of a column too large to keep, sorted, window by window
        (whole HDF5 chunks, at most WINDOW_BYTES), as the Gadget reader does."""
        from .gadget_snapshot import WINDOW_BYTES   # noqa: PLC0415
        unique, inverse = np.unique(rows, return_inverse=True)
        out = np.empty(unique.size, dtype=col.dtype.newbyteorder("="))
        with _open(self.base) as f:
            ds = f[col.dataset]
            chunk = ds.chunks[0] if ds.chunks else 1
            window = max(chunk, (WINDOW_BYTES // max(1, ds.dtype.itemsize)) // chunk * chunk)
            i = 0
            while i < unique.size:
                first = int(unique[i])
                end = min(self.num_rows, (first // window + 1) * window)
                j = int(np.searchsorted(unique, end, side="left"))
                last = int(unique[j - 1]) + 1
                out[i:j] = self._read(ds, col, first, last)[unique[i:j] - first]
                i = j
        return out[inverse]

    def take(self, name: str, rows) -> np.ndarray:
        """``name`` at ``rows`` (an integer array, or one integer)."""
        if name not in self._by_name:
            raise KeyError(name)
        col = self._by_name[name]
        scalar = np.ndim(rows) == 0
        index = np.atleast_1d(np.asarray(rows, dtype=np.int64))
        native = col.dtype.newbyteorder("=")
        if col.view is not None:
            out = np.asarray(col.view[index]).astype(native, copy=False)
        else:
            whole = self._whole(col)
            out = whole[index] if whole is not None else self._windowed(col, index)
        return out[0] if scalar else out

    def column(self, name: str) -> np.ndarray:
        return self.take(name, np.arange(self.num_rows, dtype=np.int64))

    def columns(self):
        from .gadget_snapshot import LazyColumns   # noqa: PLC0415
        return LazyColumns(self)

    # -- what the file states -------------------------------------------

    def periods(self) -> dict[str, float]:
        return {}

    def units(self) -> dict[str, str]:
        return {c.name: c.unit for c in self.columns_list if c.unit}

    def physical(self) -> dict[str, dict]:
        """Each column that has a unit attribute, shown as it is stored."""
        return {c.name: {"factor": 1.0, "unit": c.unit, "source": "HDF5 attribute"}
                for c in self.columns_list if c.unit}

    def describe(self) -> dict:
        return {"format": "hdf5", "file": Path(self.base).name, "dataset": self.name,
                "hyperslab": self.slab_text, "rows": int(self.num_rows),
                "catalogue_kind": "table"}


@lru_cache(maxsize=8)
def _table(ref: str, _size: int, _mtime_ns: int) -> Hdf5Table:
    return Hdf5Table(ref)


def resolve(path) -> Hdf5Table:
    """The table a reference names (kept while the file is unchanged)."""
    size, mtime = _stat(ref_base(path))
    return _table(str(path), size, mtime)


def release(path) -> int:
    """Drop the columns kept in memory for the table ``path`` names (a closed
    dataset's); the bytes freed. A reader that still has it reads them again."""
    try:
        table = resolve(path)
    except (OSError, ValueError, RuntimeError, KeyError):
        return 0
    with table._lock:
        freed, table._kept, table._kept_bytes = table._kept_bytes, {}, 0
    return freed


def components(path) -> list[dict]:
    """A table is one component."""
    return []


def has_family(path, key) -> bool:
    """Whether the file ``path`` has what a table reference suffix names, for a
    series of files (a name of the same shape)."""
    try:
        name = _SLAB.sub("", str(key))
        with _open(path) as f:
            _kind_of(f, name or "/")
        return True
    except (OSError, ValueError, RuntimeError, KeyError):
        return False


def table_dataset_path(path, allowed=None) -> str | None:
    """The reference a table opens by, given a reference or a generic HDF5 file;
    None for anything else, including a file with no table in it. A file with
    several tables raises ValueError listing them. (The file is the one the
    data roots checked: nothing else is read.)"""
    text = str(path)
    if is_volume_ref(text) or not (is_table_ref(text) or is_hdf5_file(text)):
        return None
    return canonical_ref(text, "table")
