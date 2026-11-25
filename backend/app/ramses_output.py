"""RAMSES outputs read in place (issue #549), without yt and without converting.

An output is a directory ``output_NNNNN`` holding, for the ``ncpu`` processes
of the run, ``amr_NNNNN.outCCCCC`` (the mesh), ``hydro_NNNNN.outCCCCC`` (the
gas), ``grav_...`` (the potential, not read here) and ``part_NNNNN.outCCCCC``
(the particles), plus ``info_NNNNN.txt`` (time, units, levels, domain
decomposition) and ``header_NNNNN.txt`` (particle totals). Every ``.outCCCCC``
file is Fortran unformatted: records framed by their length, which both gives
the byte order (the first record of every file is one integer, so its marker
is 4 in the writer's order) and lets every record's offset be found by hopping
marker to marker, without reading the data between. Every marker is checked.

Particles
---------
One component per *family* (dark matter, stars, sinks, ...), named by a
reference ``<output directory>::ramses-family=<slug>`` as a Gadget particle
type is by ``::gadget-type=N`` (app/gadget_snapshot.py): the same duck-typed
:class:`Component` the particle seam in app/point_table.py reads, so the point
renderers, ray tracing, the catalogue tools, the copilot and the VR page open
it unchanged. Columns: ``x y z`` (box units, [0, 1) for the usual
``nx = ny = nz = 1``), ``vx vy vz``, ``mass``, ``id``, ``level`` and, when
the file has them, ``family``, ``tag``, ``birth_time``, ``metallicity``.

Which record is which comes from ``part_file_descriptor.txt`` when the run
wrote one, else from the ``Particle fields`` line of ``header_NNNNN.txt``,
else from the order RAMSES used before descriptors: ``pos vel mass id level``,
then what follows by its size (1-byte records are ``family`` and ``tag``,
floating-point ones ``birth_time`` and ``metallicity``). Widths come from the
record lengths (single or double precision, 4- or 8-byte identifiers).
Families come from the ``family`` record; without one, the header's counts and,
when particles carry a birth time, a zero birth time marks dark matter.

Gas and mesh
------------
:meth:`RamsesOutput.amr` is the code-independent :class:`app.amr_source.
AmrHierarchy` of the run: leaf cells (a cell with son index 0) with centre,
level and the hydro fields, read one CPU file and one level at a time. Field
names come from ``hydro_file_descriptor.txt``, else the old order: ``density
vx vy vz pressure`` then passive scalars (``metallicity``, ``scalar_2``, ...).
The leaves also open as one more component, ``<directory>::ramses-family=cells``
(:meth:`RamsesOutput.cells`, app/amr_cells.py), a table of points whose
columns include the cell size ``dx`` and mass (issue #550).

Units
-----
``unit_l``, ``unit_d`` and ``unit_t`` of ``info_NNNNN.txt`` make code values
physical, as RAMSES's own utilities and yt use them: a length in box units is
``x * boxlen * unit_l`` cm, a mass ``m * unit_d * unit_l**3`` g, a velocity
``v * unit_l / unit_t`` cm/s, a density ``unit_d`` g/cm^3. In a cosmological
run ``unit_l`` already contains the expansion factor, so these are *proper*
(physical) quantities at ``aexp``, the convention of Gadget's
``Component.physical()`` here; the comoving length is that divided by ``aexp``.
"""
from __future__ import annotations

import mmap
import os
import re
import struct
import threading
from collections import OrderedDict
from collections.abc import Iterator, Sequence
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path

import numpy as np

from .amr_source import AmrHierarchy, LeafBlock
from .gadget_snapshot import KPC_CM, MSUN_G, LazyColumns

#: ``<output directory>`` + REF_SEP + family slug.
REF_SEP = "::ramses-family="

#: Family codes of RAMSES (pm/pm_commons.f90): slug and label. Negative
#: codes are the tracers that follow one gas, DM, star or sink particle.
FAMILIES = {1: ("dm", "dark matter"), 2: ("stars", "stars"), 3: ("sinks", "sinks"),
            4: ("debris", "debris"), 5: ("other", "other"),
            0: ("tracer-gas", "gas tracers"), -1: ("tracer-dm", "dark matter tracers"),
            -2: ("tracer-stars", "star tracers"), -3: ("tracer-sinks", "sink tracers")}
_SLUG_CODE = {slug: code for code, (slug, _) in FAMILIES.items()}
#: The slug of the gas cells of the mesh (issue #550), a component beside the
#: particle families: ``<output directory>::ramses-family=cells``.
CELLS = "cells"

_FILE = re.compile(r"^(amr|hydro|grav|part|sink)_(\d+)\.out(\d+)$")
_INFO = re.compile(r"^info_(\d+)\.txt$")
_SIDECAR = re.compile(r"^(info|header)_\d+\.txt$|^(hydro|part)_file_descriptor\.txt$")

#: A mapped file is kept open until this many others have been used since.
OPEN_FILES = 96
#: The most processes an info file may state (RAMSES runs use thousands).
MAX_NCPU = 1 << 20
#: Grids read at a time: a block of cells never exceeds 8 x this.
GRID_CHUNK = 1 << 17

_PART_NAMES = {"position_x": "x", "position_y": "y", "position_z": "z",
               "velocity_x": "vx", "velocity_y": "vy", "velocity_z": "vz",
               "mass": "mass", "identity": "id", "levelp": "level", "family": "family",
               "tag": "tag", "birth_time": "birth_time", "metallicity": "metallicity"}
_HEADER_FIELDS = {"pos": ("x", "y", "z"), "vel": ("vx", "vy", "vz"), "mass": ("mass",),
                  "iord": ("id",), "level": ("level",), "family": ("family",),
                  "tag": ("tag",), "birth": ("birth_time",), "metal": ("metallicity",)}
_HYDRO_NAMES = {"velocity_x": "vx", "velocity_y": "vy", "velocity_z": "vz"}
_INT_COLUMNS = frozenset({"id", "level", "family", "tag"})
_HEADER_RECORDS = 8        # ncpu ndim npart localseed nstar_tot mstar_tot mstar_lost nsink


# ── Fortran unformatted records ──────────────────────────────────────────────

class _Reader:
    """One Fortran unformatted file mapped read-only: records are found by
    their length markers (4 or 8 bytes, either byte order) and read as numpy
    arrays straight from the map."""

    def __init__(self, path: str | Path, order: str | None = None, width: int | None = None):
        self.path = str(path)
        with open(self.path, "rb") as fh:
            size = os.fstat(fh.fileno()).st_size
            if size < 12:
                raise ValueError(f"{Path(self.path).name} is too short to be a RAMSES file.")
            self.map = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        self.size = size
        if order is None:
            order, width = _detect_order(self.map, size, Path(self.path).name)
        self.order, self.width = order, width
        self._fmt = struct.Struct(f"{order}{'i' if width == 4 else 'q'}")

    def record(self, pos: int) -> tuple[int, int, int]:
        """``(data offset, length in bytes, next record's position)`` of the
        record at ``pos``; ValueError when its two markers differ or it runs
        past the end (a file cut short, or not a RAMSES file)."""
        w = self.width
        if pos + 2 * w > self.size:
            raise ValueError(f"{Path(self.path).name}: unexpected end of file at byte {pos}.")
        n = self._fmt.unpack_from(self.map, pos)[0]
        end = pos + w + n
        if n < 0 or end + w > self.size or self._fmt.unpack_from(self.map, end)[0] != n:
            raise ValueError(f"{Path(self.path).name}: broken record at byte {pos} (the length "
                             "markers disagree): not a RAMSES file or one written in another "
                             "layout.")
        return pos + w, n, end + w

    def array(self, offset: int, dtype, count: int) -> np.ndarray:
        return np.frombuffer(self.map, dtype=dtype, count=count, offset=offset)

    def records(self, pos: int = 0, count: int | None = None) -> list[tuple[int, int]]:
        """``(offset, length)`` of ``count`` records from ``pos`` (None: to the end)."""
        out = []
        while pos < self.size and (count is None or len(out) < count):
            off, n, pos = self.record(pos)
            out.append((off, n))
        return out

    def ints(self, pos: int, dtype="i4") -> tuple[np.ndarray, int]:
        off, n, nxt = self.record(pos)
        size = np.dtype(dtype).itemsize
        if n < size:
            raise ValueError(f"{Path(self.path).name}: an empty record at byte {pos}.")
        return self.array(off, np.dtype(dtype).newbyteorder(self.order), n // size), nxt


def _detect_order(buf, size: int, name: str) -> tuple[str, int]:
    """The byte order and marker width of a RAMSES file: its first record is
    one integer, so its length is 4, in the writer's order, at both ends."""
    for width in (4, 8):
        for order in ("<", ">"):
            code = "i" if width == 4 else "q"
            need = 2 * width + 4
            if size < need:
                continue
            first, = struct.unpack_from(f"{order}{code}", buf, 0)
            last, = struct.unpack_from(f"{order}{code}", buf, width + 4)
            if first == 4 and last == 4:
                return order, width
    raise ValueError(f"{name} is not a RAMSES Fortran file (its first record is not one integer).")


class _Files:
    """The mapped files of every output, the least recently used dropped when
    more than OPEN_FILES are open (each map holds a descriptor). A dropped map
    closes once nothing reads it: no array kept beyond a call may be a view of
    one (copy what is kept)."""

    def __init__(self):
        self._lock = threading.Lock()
        self._open: OrderedDict[str, _Reader] = OrderedDict()

    def get(self, path: str, order=None, width=None) -> _Reader:
        with self._lock:
            reader = self._open.pop(path, None)
            if reader is None:
                reader = _Reader(path, order, width)
            self._open[path] = reader
            while len(self._open) > OPEN_FILES:
                self._open.popitem(last=False)
            return reader


# ── info, header, descriptors ────────────────────────────────────────────────

@dataclass(frozen=True)
class Info:
    """``info_NNNNN.txt``: the run's constants at this output."""
    ncpu: int
    ndim: int
    levelmin: int
    levelmax: int
    boxlen: float
    time: float
    aexp: float
    h0: float
    omega_m: float
    omega_l: float
    omega_k: float
    omega_b: float
    unit_l: float
    unit_d: float
    unit_t: float
    ordering: str
    raw: dict = field(default_factory=dict, compare=False)

    @property
    def cosmological(self) -> bool:
        """A run with an expansion (RAMSES writes H0 = 1, omega_m = 1,
        aexp = 1 for any other)."""
        return not (abs(self.h0 - 1.0) < 1e-9 and abs(self.omega_m - 1.0) < 1e-9
                    and abs(self.aexp - 1.0) < 1e-9)

    @property
    def redshift(self) -> float:
        return 1.0 / self.aexp - 1.0 if self.aexp > 0 else 0.0


def _number(text: str) -> float:
    return float(text.strip().replace("D", "E").replace("d", "e"))


def parse_info(path: str | Path) -> Info:
    """Read ``info_NNNNN.txt``. ValueError when it lacks what a RAMSES info
    file always has (``ncpu``, ``ndim``, ``levelmax``, ``boxlen``, the units)."""
    raw: dict[str, float] = {}
    ordering = ""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.lstrip().startswith("ordering type"):
                ordering = line.split("=", 1)[1].strip().lower()
                break                       # the DOMAIN table follows
            key, sep, value = line.partition("=")
            if not sep:
                continue
            try:
                raw[key.strip()] = _number(value)
            except ValueError:
                continue
    missing = [k for k in ("ncpu", "ndim", "levelmax", "boxlen", "unit_l", "unit_d", "unit_t")
               if k not in raw]
    if missing:
        raise ValueError(f"{Path(path).name} lacks {', '.join(missing)}: not a RAMSES info file.")
    if not 1 <= raw["ncpu"] <= MAX_NCPU or raw["ndim"] not in (1, 2, 3):
        raise ValueError(f"{Path(path).name}: ncpu = {raw['ncpu']:g}, ndim = {raw['ndim']:g} "
                         "is no RAMSES run.")
    return Info(ncpu=int(raw["ncpu"]), ndim=int(raw["ndim"]),
                levelmin=int(raw.get("levelmin", 1)), levelmax=int(raw["levelmax"]),
                boxlen=raw["boxlen"], time=raw.get("time", 0.0), aexp=raw.get("aexp", 1.0),
                h0=raw.get("H0", 1.0), omega_m=raw.get("omega_m", 1.0),
                omega_l=raw.get("omega_l", 0.0), omega_k=raw.get("omega_k", 0.0),
                omega_b=raw.get("omega_b", 0.0), unit_l=raw["unit_l"], unit_d=raw["unit_d"],
                unit_t=raw["unit_t"], ordering=ordering, raw=raw)


@dataclass(frozen=True)
class Header:
    """``header_NNNNN.txt``: particle totals and, in newer outputs, the
    names of the particle fields."""
    total: int | None = None
    dm: int | None = None
    stars: int | None = None
    sinks: int | None = None
    fields: tuple[str, ...] = ()


def parse_header(path: Path | None) -> Header:
    if path is None:
        return Header()
    try:
        lines = [ln.strip() for ln in path.read_text("utf-8", errors="replace").splitlines()]
    except OSError:
        return Header()
    counts: dict[str, int] = {}
    fields: tuple[str, ...] = ()
    lines = [ln for ln in lines if ln]
    for i, line in enumerate(lines[:-1]):
        low = line.lower()
        nxt = lines[i + 1]
        if low.startswith("total number of"):
            key = ("dm" if "dark matter" in low else "stars" if "star" in low
                   else "sinks" if "sink" in low else "total")
            if nxt.lstrip("+-").isdigit():
                counts[key] = int(nxt)
        elif low.startswith("particle fields"):
            names: list[str] = []
            for token in nxt.split():
                names.extend(_HEADER_FIELDS.get(token.lower(), (token.lower(),)))
            fields = tuple(names)
    return Header(counts.get("total"), counts.get("dm"), counts.get("stars"),
                  counts.get("sinks"), fields)


_DESCRIPTOR_KINDS = {"d": "f", "i": "i", "l": "i", "q": "i", "b": "i", "s": "i"}


def parse_descriptor(path: Path | None) -> list[tuple[str, str]] | None:
    """``[(name, kind)]`` of a ``*_file_descriptor.txt`` (``ivar, name, type``
    per line, ``#`` comments), in record order; None when there is none."""
    if path is None or not path.is_file():
        return None
    out: list[tuple[int, str, str]] = []
    for line in path.read_text("utf-8", errors="replace").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 3 or not parts[0].isdigit():
            continue
        out.append((int(parts[0]), parts[1], parts[2].lower()))
    out.sort()
    return [(name, _DESCRIPTOR_KINDS.get(kind, "f")) for _, name, kind in out] or None


# ── the output ───────────────────────────────────────────────────────────────

def output_of(path: str | Path, allowed=None) -> Path | None:
    """The output directory a path names (the directory, or any RAMSES file
    in it: ``info_*.txt``, ``header_*.txt``, ``amr_*.out*``, ``part_*.out*``,
    a descriptor), a reference's directory included; None for anything else.
    Judged by content: the directory must hold an info file that reads as one,
    and AMR or particle files of its output number (a stray ``info_*.txt``
    beside other data is no output).

    ``allowed`` (path -> bool): an info file it refuses raises PermissionError
    before a byte of it is read."""
    base = split_ref(path)[0]
    p = Path(base)
    try:
        if p.is_file():
            if not (_FILE.match(p.name) or _SIDECAR.match(p.name)):
                return None
            p = p.parent
        if not p.is_dir():
            return None
        info = _info_file(p, allowed)
        if info is None or not _holds_data(p, _INFO.match(info.name).group(1)):
            return None
        return p
    except PermissionError:
        raise
    except OSError:
        return None


def _holds_data(directory: Path, number: str) -> bool:
    """Whether ``directory`` has an AMR or particle file of output ``number``."""
    try:
        with os.scandir(directory) as it:
            for e in it:
                m = _FILE.match(e.name)
                if m and m.group(1) in ("amr", "part") and m.group(2) == number:
                    return True
    except OSError:
        pass
    return False


def output_paths(directory: Path, number: str) -> list[str]:
    """Every file of output ``number`` a reader may open (its info and header
    files, the descriptors, the AMR, hydro and particle files), found by name
    alone: nothing is opened, so the data roots can be checked before any is
    parsed. No check that the set is complete."""
    names = {f"info_{number}.txt", f"header_{number}.txt", "part_file_descriptor.txt",
             "hydro_file_descriptor.txt"}
    found = []
    try:
        with os.scandir(directory) as it:
            for e in it:
                m = _FILE.match(e.name)
                if e.name in names or (m and m.group(1) in ("amr", "hydro", "part")
                                       and m.group(2) == number):
                    found.append(e.path)
    except OSError:
        pass
    return sorted(found)


def checked_output(path: str | Path, allowed=None) -> Path | None:
    """``output_of`` with the data roots checked first: every file of the
    output must pass ``allowed`` before any is parsed (the info and header
    files too), else PermissionError (naming the file, nothing of its
    content)."""
    directory = output_of(path, allowed)
    if directory is not None and allowed is not None:
        number = _INFO.match(_info_file(directory, allowed).name).group(1)
        for f in output_paths(directory, number):
            if not allowed(f):
                raise PermissionError(f"{Path(f).name}, a file of this output, lies outside "
                                      "the folders this backend may read.")
    return directory


def _info_file(directory: Path, allowed=None) -> Path | None:
    try:
        with os.scandir(directory) as it:
            names = sorted(e.name for e in it if _INFO.match(e.name))
    except OSError:
        return None
    for name in names:
        if allowed is not None and not allowed(str(directory / name)):
            raise PermissionError(f"{name}, a file of this output, lies outside the "
                                  "folders this backend may read.")
        try:
            with open(directory / name, "rb") as fh:
                head = fh.read(2048).decode("utf-8", errors="replace")
        except OSError:
            continue
        if "ncpu" in head and "levelmax" in head and "boxlen" in head:
            return directory / name
    return None


def is_ramses(path: str | Path) -> bool:
    return output_of(path) is not None


def ramses_ref(directory: str | Path, slug: str) -> str:
    return f"{directory}{REF_SEP}{slug}"


def split_ref(path: str | Path) -> tuple[str, str | None]:
    """``(directory, family slug)`` of a family reference; ``(path, None)``
    for any other path."""
    text = str(path)
    base, sep, value = text.rpartition(REF_SEP)
    if sep and (value in _SLUG_CODE or value == CELLS
                or re.fullmatch(r"family--?\d+", value)):
        return base, value
    return text, None


@dataclass(frozen=True)
class Family:
    slug: str
    code: int
    label: str
    counts: tuple[int, ...]          # particles per part file

    @property
    def total(self) -> int:
        return int(sum(self.counts))


@dataclass
class _PartFile:
    """What one part file holds: its particle count and, per column, where it
    is and how it is stored."""
    npart: int
    fields: dict[str, tuple[int, np.dtype]]


_FILES = _Files()


class RamsesOutput:
    """One output directory. Cheap to build (the info and header files and a
    listing); every array is read when asked for."""

    def __init__(self, directory: Path):
        self.path = str(directory)
        self.directory = directory
        info_file = _info_file(directory)
        if info_file is None:
            raise ValueError(f"{directory.name} is not a RAMSES output (no info_*.txt).")
        self.number = _INFO.match(info_file.name).group(1)
        self.info = parse_info(info_file)
        self.header = parse_header(self._find(f"header_{self.number}.txt"))
        self._files = _FILES
        self._found = self._listing()
        self._part_descriptor = parse_descriptor(self._find("part_file_descriptor.txt"))
        self._hydro_descriptor = parse_descriptor(self._find("hydro_file_descriptor.txt"))
        self._lock = threading.RLock()
        self._layout: tuple[list[str], list[_PartFile]] | None = None
        self._families: dict[str, Family] | None = None
        self._family_rows: dict[tuple[str, int], np.ndarray | None] = {}
        self._amr_domains: dict[int, _AmrDomain] = {}
        self._hydro_domains: dict[int, _HydroDomain] = {}
        self._periodic: bool | None = None
        self._hierarchy: RamsesAmr | None = None
        self._cells = None

    # ── files ──

    def _find(self, name: str) -> Path | None:
        p = self.directory / name
        return p if p.is_file() else None

    def _listing(self) -> dict[str, dict[int, str]]:
        found: dict[str, dict[int, str]] = {"amr": {}, "hydro": {}, "part": {}}
        try:
            with os.scandir(self.directory) as it:
                for e in it:
                    m = _FILE.match(e.name)
                    if m and m.group(1) in found and m.group(2) == self.number:
                        found[m.group(1)][int(m.group(3))] = e.path
        except OSError:
            pass
        return found

    def _cpu_files(self, kind: str) -> list[str]:
        """The ``kind`` files in the order of their process; [] when the output
        has none, ValueError when some process's file is missing (info says
        how many there are)."""
        found = self._found[kind]
        if not found:
            return []
        missing = [c for c in range(1, self.info.ncpu + 1) if c not in found]
        if missing:
            raise ValueError(f"{self.directory.name}: {kind} file(s) of process "
                             f"{', '.join(str(c) for c in missing[:5])} missing "
                             f"({len(found)} of {self.info.ncpu}).")
        return [found[c] for c in range(1, self.info.ncpu + 1)]

    @property
    def amr_files(self) -> list[str]:
        return self._cpu_files("amr")

    @property
    def hydro_files(self) -> list[str]:
        return self._cpu_files("hydro")

    @property
    def part_files(self) -> list[str]:
        return self._cpu_files("part")

    def has(self, kind: str) -> bool:
        """Whether the output has files of ``kind`` ("amr", "hydro", "part")."""
        return bool(self._found[kind])

    def all_paths(self) -> list[str]:
        """Every file of the output a reader may open, found by name (no
        check that the set is complete): what the data roots must allow."""
        return output_paths(self.directory, self.number)

    def files(self, kinds: Sequence[str] = ("amr", "hydro", "part")) -> list[str]:
        """The files of the output that are read: info and header, the
        descriptors and those of ``kinds``: what a data-root check or a cache
        key covers."""
        extras = [self._find(f"info_{self.number}.txt"), self._find(f"header_{self.number}.txt"),
                  self._find("part_file_descriptor.txt"), self._find("hydro_file_descriptor.txt")]
        return [str(p) for p in extras if p is not None] + [
            f for kind in kinds for f in self._cpu_files(kind)]

    def _reader(self, path: str) -> _Reader:
        return self._files.get(path)

    @property
    def periodic(self) -> bool:
        """Periodic boundaries: no boundary grids (``nboundary`` = 0). True
        when there is no mesh file to say."""
        if self._periodic is None:
            self._periodic = (self._amr_domain(0).nboundary == 0) if self.has("amr") else True
        return self._periodic

    # ── particles ──

    def layout(self) -> tuple[list[str], list[_PartFile]]:
        """``(column names, per part file layout)``."""
        with self._lock:
            if self._layout is None:
                self._layout = self._scan_particles()
            return self._layout

    def _scan_particles(self) -> tuple[list[str], list[_PartFile]]:
        files = [self._scan_part_file(p) for p in self.part_files]
        names: list[str] = []
        for f in files:
            if f.npart:
                names = list(f.fields)
                break
        return names, files

    def _scan_part_file(self, path: str) -> _PartFile:
        rd = self._reader(path)
        recs = rd.records()
        if len(recs) <= _HEADER_RECORDS - 1:
            raise ValueError(f"{Path(path).name}: too few records for a RAMSES particle file.")
        i4 = np.dtype("i4").newbyteorder(rd.order)
        npart = int(rd.array(recs[2][0], i4, 1)[0])
        if rd.array(recs[0][0], i4, 1)[0] != self.info.ncpu:
            raise ValueError(f"{Path(path).name}: written for another number of processes "
                             "than info says.")
        if not npart:
            return _PartFile(0, {})
        names = self._field_names(recs, npart, rd)
        head = len(recs) - len(names)
        if head < _HEADER_RECORDS - 1:
            raise ValueError(f"{Path(path).name}: {len(recs)} records for {len(names)} fields "
                             "and a header of at least 7.")
        fields: dict[str, tuple[int, np.dtype]] = {}
        for k, name in enumerate(names):
            off, n = recs[head + k]
            if n % npart:
                raise ValueError(f"{Path(path).name}: record {head + k} holds {n} bytes for "
                                 f"{npart} particles.")
            fields[name] = (off, _dtype(name, n // npart, rd.order, self._kind_of(name, k)))
        return _PartFile(npart, fields)

    def _kind_of(self, name: str, index: int) -> str:
        if self._part_descriptor and index < len(self._part_descriptor):
            return self._part_descriptor[index][1]
        return "i" if name in _INT_COLUMNS else "f"

    def _field_names(self, recs, npart: int, rd: _Reader | None = None) -> list[str]:
        """The names of a part file's field records, in order: from the
        descriptor, the header's ``Particle fields`` line, else positionally."""
        ndim = self.info.ndim
        if self._part_descriptor:
            return _unique([_PART_NAMES.get(n, n) for n, _ in self._part_descriptor])
        if self.header.fields:
            return _unique(list(self.header.fields))
        head = _HEADER_RECORDS if self._has_nsink(recs, npart, rd) else _HEADER_RECORDS - 1
        names = [*"xyz"[:ndim], *("vx", "vy", "vz")[:ndim], "mass", "id", "level"]
        extra_floats = iter(("birth_time", "metallicity"))
        extra_bytes = iter(("family", "tag"))
        for off, n in recs[head + len(names):]:
            size = n // npart if npart else 0
            names.append(next(extra_bytes, f"extra_{len(names)}") if size == 1
                         else next(extra_floats, f"extra_{len(names)}"))
        return names

    @staticmethod
    def _has_nsink(recs, npart: int, rd: _Reader | None) -> bool:
        """Whether the header's eighth record is ``nsink`` (4 bytes) rather
        than the first field. One single-precision particle makes a field
        record 4 bytes too: then its value tells, a count being small and a
        position's bits (a float above 1e-38) at least 2^23."""
        if recs[7][1] != 4:
            return False
        if npart != 1 or rd is None:
            return True
        value = int(rd.array(recs[7][0], np.dtype("i4").newbyteorder(rd.order), 1)[0])
        return 0 <= value < (1 << 20)

    @property
    def num_particles(self) -> int:
        return int(sum(f.npart for f in self.layout()[1]))

    def families(self) -> dict[str, Family]:
        """The families that have particles, by slug, in family-code order
        with dark matter first."""
        with self._lock:
            if self._families is None:
                self._families = self._scan_families()
            return self._families

    def _scan_families(self) -> dict[str, Family]:
        names, files = self.layout()
        per_code: dict[int, list[int]] = {}
        n = len(files)
        for k, f in enumerate(files):
            if not f.npart:
                continue
            codes = self._codes(k)
            if codes is None:
                per_code.setdefault(1, [0] * n)[k] = f.npart
                continue
            for code, count in zip(*np.unique(codes, return_counts=True)):
                per_code.setdefault(int(code), [0] * n)[k] = int(count)
        order = sorted(per_code, key=lambda c: (c != 1, c <= 0, abs(c)))
        out = {}
        for code in order:
            slug, label = FAMILIES.get(code, (f"family-{code}", f"family {code}"))
            out[slug] = Family(slug, code, label, tuple(per_code[code]))
        return out

    def _codes(self, k: int) -> np.ndarray | None:
        """The family code of every particle of part file ``k``: from the
        ``family`` record; without one, 2 (star) for a particle with a birth
        time and 1 (dark matter) for one with none when the header says
        there are stars; None (all dark matter) otherwise."""
        names, files = self.layout()
        f = files[k]
        rd = self._reader(self.part_files[k])
        if "family" in f.fields:
            off, dt = f.fields["family"]
            return rd.array(off, dt, f.npart)
        if "birth_time" in f.fields and (self.header.stars or 0) > 0:
            off, dt = f.fields["birth_time"]
            return np.where(rd.array(off, dt, f.npart) != 0, 2, 1).astype(np.int8)
        return None

    def family_rows(self, slug: str, k: int) -> np.ndarray | None:
        """The rows of part file ``k`` that belong to family ``slug``; None
        when all of them do (nothing to look up)."""
        fam = self.families()[slug]
        if fam.counts[k] == self.layout()[1][k].npart:
            return None
        key = (slug, k)
        with self._lock:
            if key not in self._family_rows:
                codes = self._codes(k)
                self._family_rows[key] = np.flatnonzero(codes == fam.code).astype(np.int32)
            return self._family_rows[key]

    def has_cells(self) -> bool:
        """Whether the mesh and its gas are there to read as cells (issue #550)."""
        return self.has("amr") and self.has("hydro") and self.info.ndim == 3

    def cells(self):
        """The gas cells as a component (app/amr_cells.py)."""
        if not self.has_cells():
            raise ValueError(f"{self.directory.name} has no mesh with gas to read as cells.")
        from .amr_cells import AmrCells  # noqa: PLC0415
        with self._lock:
            if self._cells is None:
                self._cells = AmrCells(self.amr(), self, ramses_ref(self.path, CELLS),
                                       self.periodic)
            return self._cells

    def component(self, slug: str):
        if slug == CELLS:
            return self.cells()
        if slug not in self.families():
            raise ValueError(f"{self.directory.name} has no {slug!r} particles.")
        return Component(self, slug)

    # ── units ──

    def unit_table(self) -> dict[str, tuple[float, str]]:
        """``{column: (factor, unit)}`` of every column the output has, from
        the info file (see the module docstring): physical (proper) lengths in
        kpc, masses in Msun, velocities in km/s, density in g/cm^3, pressure
        in erg/cm^3."""
        i = self.info
        length = i.boxlen * i.unit_l / KPC_CM
        kms = i.unit_l / i.unit_t / 1e5
        mass = i.unit_d * i.unit_l ** 3 / MSUN_G
        table = {"x": (length, "kpc"), "y": (length, "kpc"), "z": (length, "kpc"),
                 "dx": (length, "kpc"),
                 "vx": (kms, "km/s"), "vy": (kms, "km/s"), "vz": (kms, "km/s"),
                 "mass": (mass, "Msun"),
                 "density": (i.unit_d, "g/cm^3"),
                 "pressure": (i.unit_d * (i.unit_l / i.unit_t) ** 2, "erg/cm^3"),
                 "metallicity": (1.0, "dimensionless")}
        return table

    def describe(self) -> dict:
        i = self.info
        return {"format": "ramses", "output": self.number, "files": len(self.part_files),
                "ncpu": i.ncpu, "ndim": i.ndim, "levelmin": i.levelmin,
                "levelmax": i.levelmax, "ordering": i.ordering, "periodic": self.periodic,
                "time": i.time, "scale_factor": i.aexp, "redshift": i.redshift,
                "cosmological": i.cosmological, "box_size": i.boxlen,
                "hubble_param": i.h0 / 100.0 if i.cosmological else 1.0,
                "omega_m": i.omega_m, "omega_lambda": i.omega_l, "omega_b": i.omega_b,
                "units": {"unit_l": i.unit_l, "unit_d": i.unit_d, "unit_t": i.unit_t,
                          "length": "box units x boxlen x unit_l cm (proper at aexp)",
                          "comoving_length_divisor": i.aexp},
                "families": {slug: f.total for slug, f in self.families().items()}
                if self.has("part") else {},
                "hydro_fields": list(self.hydro_names()) if self.has("hydro") else []}

    # ── mesh ──

    def hydro_names(self) -> tuple[str, ...]:
        """The hydro variables, in record order (see the module docstring)."""
        if not self.has("hydro"):
            return ()
        nvar = self._hydro_domain(0).nvar
        if self._hydro_descriptor:
            names = _unique([_HYDRO_NAMES.get(n, n) for n, _ in self._hydro_descriptor])
            if len(names) != nvar:
                raise ValueError(f"hydro_file_descriptor.txt lists {len(names)} variables, "
                                 f"the files hold {nvar}.")
            return tuple(names)
        base = ["density", "vx", "vy", "vz", "pressure"]
        names = base[:nvar]
        for k in range(1, nvar - len(base) + 1):
            names.append("metallicity" if k == 1 else f"scalar_{k}")
        return tuple(names)

    def _amr_domain(self, k: int) -> "_AmrDomain":
        with self._lock:
            if k not in self._amr_domains:
                self._amr_domains[k] = _AmrDomain(self, k)
            return self._amr_domains[k]

    def _hydro_domain(self, k: int) -> "_HydroDomain":
        with self._lock:
            if k not in self._hydro_domains:
                self._hydro_domains[k] = _HydroDomain(self, k)
            return self._hydro_domains[k]

    def amr(self) -> "RamsesAmr":
        """The run's mesh as an :class:`app.amr_source.AmrHierarchy`."""
        if not self.has("amr"):
            raise ValueError(f"{self.directory.name} has no amr files: no mesh to read.")
        if self.info.ndim != 3:
            raise ValueError(f"Only 3-D RAMSES meshes are read (this one has ndim = "
                             f"{self.info.ndim}).")
        with self._lock:
            if self._hierarchy is None:
                self._hierarchy = RamsesAmr(self)
            return self._hierarchy


def _unique(names: list[str]) -> list[str]:
    seen: dict[str, int] = {}
    out = []
    for n in names:
        seen[n] = seen.get(n, 0) + 1
        out.append(n if seen[n] == 1 else f"{n}_{seen[n]}")
    return out


def _dtype(name: str, size: int, order: str, kind: str) -> np.dtype:
    """A record's dtype from the bytes it holds per value and what it is."""
    if kind == "f":
        if size not in (4, 8):
            raise ValueError(f"column {name!r}: {size}-byte floating-point values.")
        return np.dtype(f"{order}f{size}")
    if size not in (1, 4, 8):
        raise ValueError(f"column {name!r}: {size}-byte integers.")
    return np.dtype(f"{order}i{size}")


class _AmrDomain:
    """The mesh of one process: where each level's grid arrays lie in its amr
    file. Only the processes' own grids are kept (RAMSES also writes none of
    the virtual grids other processes see, but a count for them is skipped
    when present)."""

    def __init__(self, out: RamsesOutput, k: int):
        self.path = out.amr_files[k]
        rd = out._reader(self.path)
        self.k = k
        ndim = out.info.ndim
        pos = 0
        rec = rd.record
        i4 = np.dtype("i4").newbyteorder(rd.order)

        ncpu_a, pos = rd.ints(pos)
        ndim_a, pos = rd.ints(pos)
        if int(ncpu_a[0]) != out.info.ncpu or int(ndim_a[0]) != ndim:
            raise ValueError(f"{Path(self.path).name} disagrees with info about ncpu or ndim.")
        nxyz, pos = rd.ints(pos)
        self.nx, self.ny, self.nz = (int(v) for v in nxyz[:3])
        nlevelmax, pos = rd.ints(pos)
        self.nlevelmax = int(nlevelmax[0])
        _, pos = rd.ints(pos)                           # ngridmax
        nboundary, pos = rd.ints(pos)
        self.nboundary = int(nboundary[0])
        _, pos = rd.ints(pos)                           # ngrid_current
        # boxlen, noutput/iout/ifout, tout, aout, t, dtold, dtnew, nstep, einit/mass/rho,
        # the cosmology (omega.. h0 aexp_ini), aexp.., mass_sph: twelve records.
        for _ in range(12):
            pos = rec(pos)[2]
        ncpu, nlev = out.info.ncpu, self.nlevelmax
        off, n, pos = rec(pos)                          # headl
        if n != 4 * ncpu * nlev:
            raise ValueError(f"{Path(self.path).name}: unsupported amr header (the level "
                             "table has an unexpected size).")
        pos = rec(pos)[2]                               # taill
        off, n, pos = rec(pos)                          # numbl
        numbl = rd.array(off, i4, ncpu * nlev).reshape(nlev, ncpu)
        pos = rec(pos)[2]                               # numbtot
        numbb = None
        if self.nboundary:
            pos = rec(pos)[2]                           # headb
            pos = rec(pos)[2]                           # tailb
            off, n, pos = rec(pos)                      # numbb
            numbb = rd.array(off, i4, self.nboundary * nlev).reshape(nlev, self.nboundary)
        pos = rec(pos)[2]                               # headf tailf numbf used_mem ...
        ordering_off, n, pos = rec(pos)
        order_name = bytes(rd.map[ordering_off:ordering_off + n]).decode("ascii", "replace").strip()
        if order_name == "bisection":
            for _ in range(5):
                pos = rec(pos)[2]
        else:
            pos = rec(pos)[2]                           # bound_key
        nco = self.nx * self.ny * self.nz
        son_off, n, pos = rec(pos)
        pos = rec(pos)[2]                               # coarse flag
        cpu_off, n, pos = rec(pos)                      # coarse cpu map
        # Copies: a view would pin the mapping (and its descriptor) for as long
        # as the domain is kept.
        self.coarse = (rd.array(son_off, i4, nco).copy(), rd.array(cpu_off, i4, nco).copy())
        nrec = 3 + ndim + 1 + 2 * ndim + 3 * 2 ** ndim
        self.levels: dict[int, _AmrLevel] = {}
        myid = k + 1
        for ilevel in range(1, nlev + 1):
            for ibound in range(ncpu + self.nboundary):
                ncache = int(numbl[ilevel - 1, ibound]) if ibound < ncpu \
                    else int(numbb[ilevel - 1, ibound - ncpu])
                if ncache <= 0:
                    continue
                offs = []
                for _ in range(nrec):
                    off, n, pos = rec(pos)
                    offs.append((off, n))
                if ibound + 1 != myid:
                    continue
                if offs[0][1] != 4 * ncache:
                    raise ValueError(f"{Path(self.path).name}: level {ilevel} holds "
                                     f"{offs[0][1] // 4} grids, the table says {ncache}.")
                xg = offs[3:3 + ndim]
                son0 = 4 + 3 * ndim
                son = offs[son0:son0 + 2 ** ndim]
                self.levels[ilevel] = _AmrLevel(
                    ncache, [(o, _fdtype(n, ncache, rd.order)) for o, n in xg],
                    [(o, np.dtype(f"{rd.order}i{n // ncache}")) for o, n in son])


@dataclass
class _AmrLevel:
    ncache: int
    xg: list      # [(offset, dtype)] per axis
    son: list     # [(offset, dtype)] per cell of a grid


def _fdtype(nbytes: int, count: int, order: str) -> np.dtype:
    size = nbytes // count
    if size not in (4, 8) or nbytes != size * count:
        raise ValueError(f"a record of {nbytes} bytes does not hold {count} floating-point "
                         "values.")
    return np.dtype(f"{order}f{size}")


class _HydroDomain:
    """The gas of one process: per level, where each cell's variables lie."""

    def __init__(self, out: RamsesOutput, k: int):
        self.path = out.hydro_files[k]
        rd = out._reader(self.path)
        ndim = out.info.ndim
        pos = 0
        ncpu_a, pos = rd.ints(pos)
        nvar, pos = rd.ints(pos)
        ndim_a, pos = rd.ints(pos)
        nlevelmax, pos = rd.ints(pos)
        nboundary, pos = rd.ints(pos)
        pos = rd.record(pos)[2]                         # gamma
        if int(ncpu_a[0]) != out.info.ncpu or int(ndim_a[0]) != ndim:
            raise ValueError(f"{Path(self.path).name} disagrees with info about ncpu or ndim.")
        self.nvar = int(nvar[0])
        ncpu = out.info.ncpu
        self.levels: dict[int, _HydroLevel] = {}
        myid = k + 1
        for ilevel in range(1, int(nlevelmax[0]) + 1):
            for ibound in range(ncpu + int(nboundary[0])):
                lv, pos = rd.ints(pos)
                nc, pos = rd.ints(pos)
                if int(lv[0]) != ilevel:
                    raise ValueError(f"{Path(self.path).name}: expected level {ilevel}, found "
                                     f"{int(lv[0])}.")
                ncache = int(nc[0])
                if ncache <= 0:
                    continue
                offs = []
                for _ in range(2 ** ndim * self.nvar):
                    off, n, pos = rd.record(pos)
                    offs.append((off, n))
                if ibound + 1 == myid:
                    self.levels[ilevel] = _HydroLevel(
                        ncache, [(o, _fdtype(n, ncache, rd.order)) for o, n in offs])


@dataclass
class _HydroLevel:
    ncache: int
    values: list  # [(offset, dtype)] indexed cell * nvar + variable


class RamsesAmr(AmrHierarchy):
    """The leaf cells of a RAMSES output (see app/amr_source.py)."""
    code = "ramses"

    def __init__(self, out: RamsesOutput):
        self.out = out
        self._counts: dict[int, int] | None = None
        self._lock = threading.Lock()

    @property
    def extent(self):
        d = self.out._amr_domain(0)
        return (float(d.nx), float(d.ny), float(d.nz))

    @property
    def levels(self):
        return tuple(sorted(lv for lv, n in self.leaf_counts().items() if n))

    @property
    def fields(self):
        return self.out.hydro_names()

    def _selected(self, levels):
        return None if levels is None else {int(lv) for lv in levels}

    def _grid_cells(self, rd, level: _AmrLevel, a: int, b: int):
        """``(xg (n, 3), son (n, 8))`` of grids ``a:b`` of one level block."""
        xg = np.stack([rd.array(o, dt, level.ncache)[a:b] for o, dt in level.xg], axis=1)
        son = np.stack([rd.array(o, dt, level.ncache)[a:b] for o, dt in level.son], axis=1)
        return xg, son

    def leaf_counts(self) -> dict[int, int]:
        with self._lock:
            if self._counts is None:
                counts: dict[int, int] = {}
                for k in range(self.out.info.ncpu):
                    dom = self.out._amr_domain(k)
                    rd = self.out._reader(dom.path)
                    for ilevel, lev in dom.levels.items():
                        for a in range(0, lev.ncache, GRID_CHUNK):
                            _, son = self._grid_cells(rd, lev, a, min(lev.ncache, a + GRID_CHUNK))
                            counts[ilevel] = counts.get(ilevel, 0) + int(np.count_nonzero(son == 0))
                    cson, ccpu = dom.coarse
                    nc = int(np.count_nonzero((cson == 0) & (ccpu == k + 1)))
                    if nc:
                        counts[0] = counts.get(0, 0) + nc
                self._counts = counts
            return dict(self._counts)

    def leaf_blocks(self, fields: Sequence[str] = (), levels=None, lo=None, hi=None,
                    max_block: int = 1 << 20) -> Iterator[LeafBlock]:
        known = self.fields
        unknown = [f for f in fields if f not in known]
        if unknown:
            raise ValueError(f"Unknown field: {unknown[0]!r}")
        want = self._selected(levels)
        index = [known.index(f) for f in fields]
        step = max(1, min(GRID_CHUNK, max_block // 8))
        offsets = np.array([[(ind >> d) & 1 for d in range(3)] for ind in range(8)],
                           dtype=np.float64) - 0.5
        for k in range(self.out.info.ncpu):
            dom = self.out._amr_domain(k)
            rd = self.out._reader(dom.path)
            hyd = self.out._hydro_domain(k) if fields else None
            hrd = self.out._reader(hyd.path) if hyd is not None else None
            if want is None or 0 in want:
                block = self._coarse_block(dom, k, fields, lo, hi)
                if block is not None:
                    yield block
            for ilevel in sorted(dom.levels):
                if want is not None and ilevel not in want:
                    continue
                lev = dom.levels[ilevel]
                dx = 2.0 ** -ilevel
                hlev = None
                if hyd is not None:
                    hlev = hyd.levels.get(ilevel)
                    if hlev is None or hlev.ncache != lev.ncache:
                        raise ValueError(f"{Path(hyd.path).name} and the amr file disagree about "
                                         f"level {ilevel}.")
                for a in range(0, lev.ncache, step):
                    b = min(lev.ncache, a + step)
                    xg, son = self._grid_cells(rd, lev, a, b)
                    leaf = son == 0
                    centre = (xg[:, None, :] + offsets[None, :, :] * dx)[leaf]
                    keep = _inside(centre, lo, hi)
                    if not keep.any():
                        continue
                    values = {}
                    for name, v in zip(fields, index):
                        cols = np.stack([hrd.array(*_pick(hlev.values, c * hyd.nvar + v),
                                                   hlev.ncache)[a:b] for c in range(8)], axis=1)
                        values[name] = np.asarray(cols[leaf][keep], dtype=_native(cols.dtype))
                    yield LeafBlock(ilevel, dx, centre[keep], values)

    def _coarse_block(self, dom: _AmrDomain, k: int, fields, lo, hi) -> LeafBlock | None:
        cson, ccpu = dom.coarse
        mine = np.flatnonzero((cson == 0) & (ccpu == k + 1))
        if not mine.size:
            return None
        iz, rem = np.divmod(mine, dom.nx * dom.ny)
        iy, ix = np.divmod(rem, dom.nx)
        centre = np.stack([ix, iy, iz], axis=1).astype(np.float64) + 0.5
        keep = _inside(centre, lo, hi)
        if not keep.any():
            return None
        # The coarse level carries no hydro values in a RAMSES output.
        return LeafBlock(0, 1.0, centre[keep], {f: np.full(int(keep.sum()), np.nan) for f in fields})

    def scales(self) -> dict[str, dict]:
        table = self.out.unit_table()
        names = ("x", "y", "z", "dx", *self.fields)
        out = {n: {"factor": float(table[n][0]), "unit": table[n][1]} for n in names if n in table}
        # A cell's mass is density * dx^3 with dx in box units: the box
        # length cubed goes into its factor (a particle's mass is in code
        # mass, unit_d * unit_l^3, with no boxlen).
        if "density" in out:
            i = self.out.info
            out["mass"] = {"factor": float(i.unit_d * (i.boxlen * i.unit_l) ** 3 / MSUN_G),
                           "unit": "Msun"}
        return out

    def describe(self) -> dict:
        counts = self.leaf_counts()
        return {**self.out.describe(), "code": self.code, "extent": list(self.extent),
                "levels": list(self.levels), "leaf_counts": {str(k): v for k, v in counts.items()},
                "leaf_cells": int(sum(counts.values())), "fields": list(self.fields),
                "uniform_level": self.uniform_level()}


def _pick(values, i):
    return values[i]


def _native(dtype) -> np.dtype:
    return np.dtype(dtype).newbyteorder("=")


def _inside(centre: np.ndarray, lo, hi) -> np.ndarray:
    keep = np.ones(centre.shape[0], dtype=bool)
    for d in range(3):
        if lo is not None and np.isfinite(lo[d]):
            keep &= centre[:, d] >= lo[d]
        if hi is not None and np.isfinite(hi[d]):
            keep &= centre[:, d] <= hi[d]
    return keep


# ── a family of particles, as a table of points ──────────────────────────────

class Component:
    """One particle family of an output as named columns, in code units: the
    interface of ``gadget_snapshot.Component`` the point tables, the catalogue
    routes and the renderers read. Columns are read per CPU file when
    asked for; an index into a mixed output is built once per file."""

    def __init__(self, output: RamsesOutput, slug: str):
        self.output = output
        self.slug = slug
        family = output.families()[slug]
        self.family = family
        self.num_rows = family.total
        self.names = list(output.layout()[0])
        self.derived: dict[str, str] = {}
        self._ends = np.cumsum(family.counts, dtype=np.int64)

    @property
    def snapshot(self) -> RamsesOutput:
        return self.output

    @property
    def name(self) -> str:
        return self.family.label

    def column_dtype(self, name: str) -> np.dtype:
        """The dtype of a column, native order, without reading any value."""
        for f in self.output.layout()[1]:
            if f.npart and name in f.fields:
                return _native(f.fields[name][1])
        raise KeyError(name)

    def take(self, name: str, rows) -> np.ndarray:
        """Selected rows of a column, read across the CPU files."""
        rows = np.asarray(rows, dtype=np.int64)
        scalar = rows.ndim == 0
        rows = rows.reshape(-1)
        if rows.size and (rows.min() < 0 or rows.max() >= self.num_rows):
            raise IndexError("particle row outside the component")
        if name not in self.names:
            raise KeyError(name)
        out = np.empty(rows.size, dtype=self.column_dtype(name))
        which = np.searchsorted(self._ends, rows, side="right")
        files = self.output.layout()[1]
        for k in np.unique(which):
            sel = which == k
            start = 0 if k == 0 else int(self._ends[k - 1])
            local = rows[sel] - start
            sub = self.output.family_rows(self.slug, int(k))
            if sub is not None:
                local = sub[local]
            off, dt = files[int(k)].fields[name]
            column = self.output._reader(self.output.part_files[int(k)]).array(
                off, dt, files[int(k)].npart)
            out[sel] = column[local]
        return out[0] if scalar else out

    def column(self, name: str) -> np.ndarray:
        return self.take(name, np.arange(self.num_rows, dtype=np.int64))

    def columns(self) -> LazyColumns:
        return LazyColumns(self)

    def periods(self) -> dict[str, float]:
        """Positions wrap at the box (box units: ``nx``, ``ny``, ``nz``) in a
        periodic run (no boundary grids); none otherwise."""
        if not self.output.periodic:
            return {}
        if self.output.has("amr"):
            d = self.output._amr_domain(0)
            box = {"x": d.nx, "y": d.ny, "z": d.nz}
        else:
            box = {"x": 1, "y": 1, "z": 1}
        return {n: float(box[n]) for n in self.names if n in box}

    def units(self) -> dict[str, str]:
        """Each column's unit as stored (code units, stated)."""
        table = {"x": "box units (fraction of the box)", "y": "box units (fraction of the box)",
                 "z": "box units (fraction of the box)",
                 "vx": "code velocity (unit_l / unit_t)", "vy": "code velocity (unit_l / unit_t)",
                 "vz": "code velocity (unit_l / unit_t)",
                 "mass": "code mass (unit_d unit_l^3)", "id": "dimensionless identifier",
                 "level": "refinement level", "family": "family code", "tag": "code tag",
                 "metallicity": "dimensionless", "birth_time": "code time (unit_t)"}
        return {n: table.get(n, "code units (physical conversion not declared)")
                for n in self.names}

    def physical(self) -> dict[str, dict]:
        """Per column, the factor that makes it physical and the unit then
        (RamsesOutput.unit_table), what the user typed replacing it."""
        from . import physical_units  # noqa: PLC0415
        table = self.output.unit_table()
        return physical_units.with_typed(
            self.output.path,
            {n: {"factor": float(table[n][0]), "unit": table[n][1]}
             for n in self.names if n in table},
            columns=tuple(self.names))

    def describe(self) -> dict:
        return {"type": self.family.code, "name": self.name, "family": self.slug,
                "num_rows": self.num_rows, "catalogue_kind": "particles",
                "columns": list(self.names), "units": self.units(),
                "physical": self.physical(), "derived": {}, "mass_from_table": False}


# ── references and what a route asks ─────────────────────────────────────────

@lru_cache(maxsize=8)
def _cached(directory: str, _stamp: tuple) -> RamsesOutput:
    return RamsesOutput(Path(directory))


def open_output(path: str | Path) -> RamsesOutput:
    """The output a path names (its directory, a file of it or a family
    reference), kept while its files are unchanged: the key holds the size and
    modification time of every file it reads (a rewritten particle or mesh
    file would otherwise be served with the old offsets)."""
    directory = output_of(path)
    if directory is None:
        raise ValueError(f"{path} is not a RAMSES output.")
    info = _info_file(directory)
    stamp = []
    for f in output_paths(directory, _INFO.match(info.name).group(1)):
        try:
            st = os.stat(f)
        except OSError:
            continue
        stamp.append((f, st.st_size, st.st_mtime_ns))
    return _cached(str(directory), tuple(stamp))


def resolve(path: str | Path) -> Component:
    base, slug = split_ref(path)
    if slug is None:
        raise ValueError(f"{path} names no RAMSES particle family")
    return open_output(base).component(slug)


def default_family(out: RamsesOutput) -> str | None:
    """The family an output opens on: dark matter, else the first with particles."""
    families = out.families()
    return next(iter(families), None)


def components(path: str | Path) -> list[dict]:
    """Every family with particles, then the gas cells of a mesh with gas
    (issue #550): ``{name, path, num_rows}``."""
    out = open_output(path)
    found = [{"name": f.label, "path": ramses_ref(out.path, slug), "num_rows": f.total}
             for slug, f in out.families().items()]
    if out.has_cells():
        found.append({"name": "gas cells", "path": ramses_ref(out.path, CELLS),
                      "num_rows": out.amr().n_leaf()})
    return found


def has_family(path: str | Path, slug: str) -> bool:
    """Whether the output ``path`` names has particles of family ``slug``;
    False for one that cannot be read. The header's totals answer for dark
    matter, stars and sinks when it has them (a series of outputs is checked
    at every open, so no particle file is read); without a count the member
    is taken to have it."""
    try:
        directory = output_of(path)
        if directory is None:
            return False
        if slug == CELLS:
            number = _INFO.match(_info_file(directory).name).group(1)
            return any(Path(f).name.startswith("hydro_") for f in output_paths(directory, number))
        header = parse_header(next(iter(sorted(directory.glob("header_*.txt"))), None))
        known = {"dm": header.dm, "stars": header.stars, "sinks": header.sinks}.get(slug)
        if known is not None:
            return known > 0
        # No count to say (an output without a header, another family): the
        # member is offered, and opening it says what is wrong.
        return True
    except (OSError, ValueError):
        return False
