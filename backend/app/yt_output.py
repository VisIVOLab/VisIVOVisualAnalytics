"""Meshes of other simulation codes, read through yt (issue #554).

RAMSES has its own reader (app/ramses_output.py); Enzo, FLASH, Athena and
Athena++, Chombo (PLUTO-AMR included), AMReX/BoxLib/Nyx, GAMER and ART are
many formats that yt already reads, so this module asks yt for the hierarchy
and hands it on as an :class:`app.amr_source.AmrHierarchy`. The AMR views
(cells as points, a uniform cube, a level map, "refine this box": issue #550)
then work on them with no code of their own.

yt is an OPTIONAL dependency, as for the AREPO extraction
(app/compute/arepo.py): it is in no requirements file and no release. Without
it nothing here is detected, and a route that is given such a file answers 503
(:class:`YtUnavailable`) saying what to install.

What is mapped
--------------
* Grid codes (Enzo, FLASH, Athena, Chombo, BoxLib, GAMER): every yt grid is a
  patch of cubic cells; a cell is a *leaf* when no finer grid covers it (yt's
  ``child_mask``). Level 0 is yt's root level, whose cell is the unit of the
  box (``dx = 2**-level``); a mesh that does not refine by 2, has cells that
  are not cubes, or is not 3-D is refused, saying so.
* Octree codes (ART, and RAMSES read through yt, which is how the native
  reader is cross-checked): the cells yt selects are the leaves; their level
  follows from their size.
* A mesh whose leaves all lie on one level is a uniform grid
  (:meth:`AmrHierarchy.uniform_level`): the AMR uniform view then writes it
  at its own level, cell for cell, with no resampling.

Values and units
----------------
The fields are yt's ``("gas", ...)`` fields, read in physical units (density
g/cm^3, temperature K, pressure erg/cm^3, velocity km/s, or cgs for a box
below a parsec) so that :meth:`scales` has the factor 1 for every field and
the lengths and the cell mass carry the box unit's size. The box's corner (its
left edge) is the origin of the box units: ``describe()["origin"]`` says
where it lies in the code's own coordinates.

A dataset is opened once per file version and kept (yt datasets are not
thread-safe: every read of one holds its lock).
"""
from __future__ import annotations

import hashlib
import os
import re
import threading
from collections.abc import Iterator, Sequence
from functools import lru_cache
from pathlib import Path

import numpy as np

from .amr_source import AmrHierarchy, LeafBlock
from .gadget_snapshot import KPC_CM, MSUN_G

#: ``<mesh path>`` + REF_SEP + slug: the gas cells of a mesh as a component
#: beside the particle families of the point seam (app/point_table.py).
REF_SEP = "::yt-family="
CELLS = "cells"

#: The yt frontends this module maps, by the name of their module under
#: ``yt.frontends``, and what the user knows them by. RAMSES (native reader),
#: the particle codes (Gadget, AREPO...), FITS and the stream/halo frontends
#: are deliberately not here: a file only they would take is not claimed.
FRONTENDS = {"enzo": "Enzo", "flash": "FLASH", "athena": "Athena", "athena_pp": "Athena++",
             "chombo": "Chombo", "boxlib": "AMReX/BoxLib", "amrex": "AMReX", "gamer": "GAMER",
             "art": "ART"}

#: yt gas fields read as named columns: ``(yt name, column, unit if the box is
#: astronomical, unit if it is not)``. Any other native-field alias yt lists
#: under "gas" follows, as yt names it, in the unit yt has for it.
_CURATED = (("density", "density", "g/cm**3", "g/cm**3"),
            ("temperature", "temperature", "K", "K"),
            ("pressure", "pressure", "erg/cm**3", "erg/cm**3"),
            ("velocity_x", "vx", "km/s", "cm/s"), ("velocity_y", "vy", "km/s", "cm/s"),
            ("velocity_z", "vz", "km/s", "cm/s"),
            ("metallicity", "metallicity", "", ""))
#: yt aliases that are geometry or bookkeeping, not a value of the gas.
_NOT_FIELDS = re.compile(r"^(x|y|z|dx|dy|dz|volume|cell_volume|mass|cell_mass|"
                         r"path_element_[xyz]|[xyz]_.*|.*_(x|y|z)_.*)$")

#: A code's own fluid fields (the field type yt gives them) and the usual
#: ones' names: read only when yt has no "gas" field for the density.
_NATIVE_FTYPES = frozenset(FRONTENDS) | {"boxlib", "nyx"}
_NATIVE_NAMES = {"x_velocity": "vx", "y_velocity": "vy", "z_velocity": "vz",
                 "temp": "temperature", "dens": "density"}

#: The unit a box's lengths are shown in: from this size of the box (cm) up, in
#: the app's own kpc and its multiples (the native readers' constants).
_LENGTH_UNITS = ((0.3e3 * KPC_CM, "Mpc", 1e3 * KPC_CM), (0.3 * KPC_CM, "kpc", KPC_CM),
                 (0.3e-3 * KPC_CM, "pc", 1e-3 * KPC_CM))
#: Cells that are cubes to this relative tolerance (box units are cubic).
_CUBE_RTOL = 1e-6

#: A leaf block of a grid is cut into pieces of this many cells at most.
_BLOCK_ROWS = 1 << 20


class YtUnavailable(RuntimeError):
    """yt is not installed here, or is too broken to import: the routes that
    need it answer 503 with this text."""


def yt_available() -> bool:
    """Whether yt can be used here (the AREPO extraction's check)."""
    from .compute.arepo import yt_available as available   # noqa: PLC0415
    return available()


def _require_yt():
    if not yt_available():
        raise YtUnavailable(
            "This simulation output is read through yt, which is not installed on this "
            "server. Install it (pip install yt) next to the backend to open "
            "Enzo, FLASH, Athena, Chombo, AMReX, GAMER and ART outputs.")
    import yt   # noqa: PLC0415
    yt.set_log_level(50)
    return yt


# ── references ───────────────────────────────────────────────────────────────

def yt_ref(path: str | Path, slug: str = CELLS) -> str:
    return f"{path}{REF_SEP}{slug}"


def split_ref(path: str | Path) -> tuple[str, str | None]:
    """``(mesh path, slug)`` of a reference; ``(path, None)`` for any other."""
    text = str(path)
    base, sep, value = text.rpartition(REF_SEP)
    if sep and value == CELLS:
        return base, value
    return text, None


def mesh_path(path: str | Path) -> str:
    """The file or directory yt is given for ``path`` (a reference's mesh): an
    Enzo output's directory (``DD0011/``) is the parameter file inside it
    (``DD0011/DD0011``), which is what yt loads; an AMReX plot's ``Header``
    is its directory."""
    base = Path(split_ref(path)[0])
    try:
        # An AMReX / BoxLib plot is a directory, and the open dialog picks
        # files: its Header stands for the directory.
        if base.name == "Header" and base.is_file() and _is_boxlib_dir(base.parent):
            return str(base.parent)
        inner = base / base.name
        if base.is_dir() and inner.is_file() and Path(str(inner) + ".hierarchy").is_file():
            return str(inner)
    except OSError:
        pass
    return str(base)


# ── detection (cheap: no dataset is built) ───────────────────────────────────

_HDF5 = b"\x89HDF\r\n\x1a\n"
_VTK = b"# vtk Da"


#: First line of a BoxLib / AMReX plot or checkpoint ``Header`` ("HyperCLaw-V1.1",
#: "NavierStokes-V1.1", "Checkpoint version: 3"): any other file called Header is no mesh.
_BOXLIB_HEADER = re.compile(rb"^(?:[A-Za-z][\w.]*-V\d[\w.]*|Checkpoint version:\s*\d+)\s*$")


def _is_boxlib_dir(p: Path) -> bool:
    """A BoxLib / AMReX plot directory: a ``Header`` whose first line is one of
    the format strings, and a ``Level_0`` beside it."""
    try:
        with open(p / "Header", "rb") as fh:
            first = fh.readline(200)
        return bool(_BOXLIB_HEADER.match(first)) and (p / "Level_0").is_dir()
    except OSError:
        return False


def looks_like_mesh(path: str | Path) -> bool:
    """Whether ``path`` could be a yt-readable mesh, from a few bytes and
    names alone, so that yt is not even imported for a FITS file or a CSV: an
    HDF5 file (FLASH, Chombo, GAMER, AMReX plot files), a legacy VTK file
    (Athena), an Enzo parameter file (its ``.hierarchy`` beside it), a BoxLib
    plot directory (a known ``Header`` and its ``Level_0``) or an ART file."""
    p = Path(mesh_path(path))
    try:
        if p.is_dir():
            return _is_boxlib_dir(p)
        with open(p, "rb") as fh:
            head = fh.read(8)
        return (head == _HDF5 or head == _VTK
                or Path(str(p) + ".hierarchy").is_file()
                or p.suffix.lower() in (".d", ".art"))
    except OSError:
        return False


def _stamp(path: Path) -> str:
    """The version of the mesh ``path`` names, a cache key: the identity (device,
    inode, size, mtime, ctime: product_cache.file_identity's) of every file of
    the mesh (:func:`files`, capped at :data:`MAX_FILES`), because a multi-file
    output (AMReX ``Level_*/Cell_D_*``, Enzo ``.cpu*``, FLASH sidecars) can be
    rewritten in place with its main file untouched. Empty when ``path``
    cannot be stat'ed."""
    try:
        members = files(path)
        os.stat(path)
    except OSError:
        return ""
    digest = hashlib.blake2b(digest_size=16)
    for member in members:
        try:
            st = os.stat(member)
        except OSError:
            continue    # gone between the listing and now: the next stamp differs anyway
        digest.update(f"{member}|{st.st_dev}.{st.st_ino}.{st.st_size}.{st.st_mtime_ns}."
                      f"{st.st_ctime_ns}\n".encode("utf-8", "surrogateescape"))
    return digest.hexdigest()


@lru_cache(maxsize=256)
def _detect(path: str, _stamp_: tuple) -> str | None:
    """The yt frontend that takes ``path`` (a key of :data:`FRONTENDS`), by
    the frontends' own cheap ``_is_valid`` checks; None when none or several
    do. Cached per file version: a checked file is not checked twice."""
    yt = _require_yt()
    from yt.frontends import _all   # noqa: F401,PLC0415 — registers every frontend
    from yt.data_objects.static_output import output_type_registry   # noqa: PLC0415
    del yt
    valid = []
    for cls in output_type_registry.values():
        parts = cls.__module__.split(".")
        if len(parts) < 3 or parts[2] not in FRONTENDS:
            continue
        try:
            if cls._is_valid(path):
                valid.append(cls)
        except Exception:   # noqa: BLE001 — a frontend that cannot judge says no
            continue
    # The most specific class of those that agree (Nyx and BoxLib both take a Nyx output).
    lowest = [c for c in valid if not any(o is not c and issubclass(o, c) for o in valid)]
    if len({c.__module__.split(".")[2] for c in lowest}) != 1:
        return None
    return lowest[0].__module__.split(".")[2]


def detect(path: str | Path) -> str | None:
    """The frontend key a mesh of another code opens with, or None: for a
    RAMSES output, a particle snapshot and any file yt does not take; None
    also when yt is not installed (the verdict needs it)."""
    base = mesh_path(path)
    if not looks_like_mesh(base) or not yt_available():
        return None
    from . import ramses_output as rm   # noqa: PLC0415
    if rm.output_of(base) is not None:
        return None
    return _detect(str(base), _stamp(Path(base)))


# ── the mesh ─────────────────────────────────────────────────────────────────

def _length_unit(box_cm: float) -> tuple[str, float]:
    """The unit the lengths are shown in and its size in cm: pc, kpc or Mpc
    for a box of astronomical size, else cm."""
    for floor, name, cm in _LENGTH_UNITS:
        if box_cm >= floor:
            return name, cm
    return "cm", 1.0


class _YtMesh(AmrHierarchy):
    """What a grid mesh and an octree mesh of yt share: the box, the fields
    and their units, the description."""

    def __init__(self, ds, frontend: str):
        self.ds = ds
        self.frontend = frontend
        self.code = f"yt:{frontend}"
        self._lock = threading.RLock()
        self._counts: dict[int, int] | None = None
        if int(ds.dimensionality) != 3:
            raise ValueError(f"Only 3-D meshes are read ({FRONTENDS.get(frontend, frontend)} "
                             f"output with {int(ds.dimensionality)} dimensions).")
        refine = np.atleast_1d(np.asarray(ds.refine_by)).astype(int)
        if (refine != 2).any():
            raise ValueError("Only meshes that refine by 2 are read (this one refines by "
                             f"{', '.join(map(str, refine))}).")
        self._dims = np.asarray(ds.domain_dimensions, dtype=np.float64)
        self._left = np.asarray(ds.domain_left_edge.d, dtype=np.float64)
        width = np.asarray(ds.domain_width.d, dtype=np.float64)
        cell = width / self._dims
        if not np.allclose(cell, cell[0], rtol=_CUBE_RTOL):
            raise ValueError("The cells of this mesh are not cubes "
                             f"(sides {', '.join(f'{c:.6g}' for c in cell)} in code length).")
        #: The root cell's side in the code's length: one box unit.
        self._dx0 = float(cell[0])
        #: ... in cm.
        self._unit_cm = float(ds.quan(self._dx0, "code_length").to_value("cm"))
        box_cm = self._unit_cm * float(self._dims.max())
        self._length_name, self._length_cm = _length_unit(box_cm)
        self._astro = self._length_name != "cm"
        self._columns = self._pick_columns()

    # -- fields --

    def _pick_columns(self) -> dict[str, tuple[tuple[str, str], str]]:
        """``{column: ((ftype, name), unit)}`` the mesh offers, in order."""
        listed = {f for f in self.ds.derived_field_list if f[0] == "gas"}
        out: dict[str, tuple[tuple[str, str], str]] = {}
        for yt_name, column, astro, plain in _CURATED:
            if ("gas", yt_name) in listed:
                out[column] = (("gas", yt_name), astro if self._astro else plain)
        taken = {key[1] for key, _ in out.values()}
        for f in sorted(listed):
            if f[1] in taken or f[1] in out or _NOT_FIELDS.match(f[1]):
                continue
            info = self.ds.field_info[f]
            if getattr(info, "is_alias", False):
                out[f[1]] = (f, str(info.units))
        if "density" not in out:
            out.update(self._native_columns(out))
        return out

    def _native_columns(self, have) -> dict[str, tuple[tuple[str, str], str]]:
        """The code's own fluid fields, for a frontend that gives yt no "gas"
        names for them (a Nyx plot file): named as in :data:`_NATIVE_NAMES`
        when they are the usual ones, else as the code names them, in the unit
        yt has for each."""
        out = {}
        for f in self.ds.field_list:
            if f[0] not in _NATIVE_FTYPES:
                continue
            column = _NATIVE_NAMES.get(f[1].lower(), f[1])
            if column in have or column in out:
                continue
            unit = str(self.ds.field_info[f].units)
            out[column] = (f, "" if unit in ("dimensionless", "") else unit)
        return out

    @property
    def extent(self):
        return tuple(float(n) for n in self._dims)

    @property
    def fields(self):
        return tuple(self._columns)

    def _unknown(self, fields: Sequence[str]) -> None:
        unknown = [f for f in fields if f not in self._columns]
        if unknown:
            raise ValueError(f"Unknown field: {unknown[0]!r}")

    def _values(self, data, fields: Sequence[str]) -> dict[str, np.ndarray]:
        """The fields of a yt data container or grid, in the columns' units."""
        out = {}
        for name in fields:
            key, unit = self._columns[name]
            try:
                value = data[key]
                out[name] = np.asarray(value.to_value(unit) if unit else value.d,
                                       dtype=np.float64)
            except Exception as exc:   # noqa: BLE001 — a derived field yt cannot make
                raise ValueError(f"yt cannot read the field {name!r}: {exc}") from exc
        return out

    @property
    def levels(self):
        return tuple(sorted(lv for lv, n in self.leaf_counts().items() if n))

    # -- units --

    def scales(self) -> dict[str, dict]:
        length = {"factor": self._unit_cm / self._length_cm, "unit": self._length_name}
        out = {n: dict(length) for n in ("x", "y", "z", "dx")}
        for name, (_, unit) in self._columns.items():
            out[name] = {"factor": 1.0, "unit": unit or "dimensionless"}
        if "density" in out:
            # A cell's mass is density * dx^3 with dx in box units: the box
            # unit's volume goes into the factor.
            if out["density"]["unit"] == "g/cm**3":
                out["mass"] = {"factor": self._unit_cm ** 3 / (MSUN_G if self._astro else 1.0),
                               "unit": "Msun" if self._astro else "g"}
            else:   # a code's own density unit: the mass is in it times cm^3
                unit = out["density"]["unit"]
                out["mass"] = {"factor": self._unit_cm ** 3,
                               "unit": f"{'code density' if unit == 'dimensionless' else unit}"
                                       " cm**3"}
        return out

    def describe(self) -> dict:
        ds, counts = self.ds, self.leaf_counts()
        out = {"format": "yt", "frontend": self.frontend, "yt_class": type(ds).__name__,
               "code": self.code, "extent": list(self.extent),
               "levels": [int(v) for v in self.levels],
               "leaf_counts": {str(k): v for k, v in counts.items()},
               "leaf_cells": int(sum(counts.values())), "fields": list(self.fields),
               "uniform_level": self.uniform_level(),
               "periodic": bool(np.all(ds.periodicity)),
               "domain_dimensions": [int(n) for n in self._dims],
               "box_size": {"value": float(self._dims.max()) * self._unit_cm / self._length_cm,
                            "unit": self._length_name},
               "origin": {"code_length": [float(v) for v in self._left],
                          "note": "box units count from the box's lower corner"}}
        try:
            out["time"] = float(ds.current_time.to_value("code_time"))
            out["time_seconds"] = float(ds.current_time.to_value("s"))
        except Exception:   # noqa: BLE001 — a code with no time
            pass
        if getattr(ds, "cosmological_simulation", 0):
            out["redshift"] = float(ds.current_redshift)
            out["scale_factor"] = 1.0 / (1.0 + float(ds.current_redshift))
        return out

    # -- geometry shared by readers --

    def _inside(self, centre: np.ndarray, lo, hi) -> np.ndarray:
        keep = np.ones(centre.shape[0], dtype=bool)
        for d in range(3):
            if lo is not None and np.isfinite(lo[d]):
                keep &= centre[:, d] >= lo[d]
            if hi is not None and np.isfinite(hi[d]):
                keep &= centre[:, d] <= hi[d]
        return keep


class YtGridAmr(_YtMesh):
    """A block-structured mesh: yt grids, each a patch of cubic cells."""

    def __init__(self, ds, frontend: str):
        super().__init__(ds, frontend)
        index = ds.index
        self._levels = np.asarray(index.grid_levels).reshape(-1).astype(int)
        if self._levels.size == 0:
            raise ValueError("The mesh has no grids.")
        self._by_level = {int(lv): np.flatnonzero(self._levels == lv)
                          for lv in np.unique(self._levels)}
        if min(self._by_level) != 0:
            raise ValueError("The mesh has no root level.")

    def _leaf_mask(self, g) -> np.ndarray:
        return np.asarray(g.child_mask, dtype=bool)

    def _check_grid(self, g, level: int) -> float:
        dx = float(g.dds[0].d) / self._dx0
        if not np.isclose(dx, 2.0 ** -level, rtol=1e-6):
            raise ValueError(f"A grid of level {level} has cells of {dx:g} root cells, not "
                             f"{2.0 ** -level:g}: this mesh does not refine by 2 per level.")
        return dx

    def leaf_counts(self) -> dict[int, int]:
        with self._lock:
            if self._counts is None:
                counts: dict[int, int] = {}
                grids = self.ds.index.grids
                for lv, ids in self._by_level.items():
                    for i in ids:
                        self._check_grid(grids[i], lv)
                        n = int(np.count_nonzero(self._leaf_mask(grids[i])))
                        if n:
                            counts[lv] = counts.get(lv, 0) + n
                self._counts = counts
            return dict(self._counts)

    def leaf_blocks(self, fields: Sequence[str] = (), levels=None, lo=None, hi=None,
                    max_block: int = _BLOCK_ROWS) -> Iterator[LeafBlock]:
        self._unknown(fields)
        want = None if levels is None else {int(v) for v in levels}
        grids = self.ds.index.grids
        for level in sorted(self._by_level):
            if want is not None and level not in want:
                continue
            dx = 2.0 ** -level
            pending: list[tuple[np.ndarray, dict]] = []
            held = 0
            for i in self._by_level[level]:
                with self._lock:
                    g = grids[i]
                    if not self._touches(g, lo, hi, dx):
                        continue
                    self._check_grid(g, level)
                    mask = self._leaf_mask(g)
                    if not mask.any():
                        continue
                    base = (np.asarray(g.LeftEdge.d, dtype=np.float64) - self._left) / self._dx0
                    full = self._values(g, fields) if fields else {}
                    if fields:
                        g.clear_data()
                # The grid is cut into slabs along its slowest axis, so that the
                # cell indices and centres (24 bytes a cell each) that are built
                # never exceed about ``max_block`` rows: a single 512^3 grid
                # would need gigabytes at once.
                plane = max(1, int(np.prod(mask.shape[1:])))
                step = max(1, max_block // plane)
                for a in range(0, mask.shape[0], step):
                    sub = mask[a:a + step]
                    if not sub.any():
                        continue
                    ijk = np.argwhere(sub)
                    ijk[:, 0] += a
                    centre = base[None, :] + (ijk + 0.5) * dx
                    keep = self._inside(centre, lo, hi)
                    if not keep.any():
                        continue
                    values = {n: v[a:a + step][sub][keep] for n, v in full.items()}
                    centre = centre[keep]
                    pending.append((centre, values))
                    held += centre.shape[0]
                    if held >= max_block:
                        pending = yield from self._flush(level, dx, pending, fields, max_block,
                                                         final=False)
                        held = sum(c.shape[0] for c, _ in pending)
            if pending:
                yield from self._flush(level, dx, pending, fields, max_block)

    def _touches(self, g, lo, hi, dx) -> bool:
        """Whether a grid can hold a centre inside ``lo`` .. ``hi`` (cheap
        reject before any cell is looked at)."""
        if lo is None and hi is None:
            return True
        lower = (np.asarray(g.LeftEdge.d, dtype=np.float64) - self._left) / self._dx0
        upper = (np.asarray(g.RightEdge.d, dtype=np.float64) - self._left) / self._dx0
        for d in range(3):
            if lo is not None and np.isfinite(lo[d]) and upper[d] < lo[d]:
                return False
            if hi is not None and np.isfinite(hi[d]) and lower[d] > hi[d]:
                return False
        return True

    @staticmethod
    def _flush(level, dx, pending, fields, max_block, final: bool = True):
        """Yield the pending rows as blocks of ``max_block``; the last, shorter
        one too when ``final``, else it is returned as the new pending list."""
        centre = np.concatenate([c for c, _ in pending])
        values = {n: np.concatenate([v[n] for _, v in pending]) for n in fields}
        stop = centre.shape[0] if final else centre.shape[0] - centre.shape[0] % max_block
        for a in range(0, stop, max_block):
            b = a + max_block
            yield LeafBlock(level, dx, centre[a:b], {n: v[a:b] for n, v in values.items()})
        if final or stop == centre.shape[0]:
            return []
        return [(centre[stop:], {n: v[stop:] for n, v in values.items()})]


class YtOctreeAmr(_YtMesh):
    """An octree mesh (ART, RAMSES through yt): the leaf cells yt selects, with
    their level from their size. Read a chunk of yt's own I/O at a time."""

    def __init__(self, ds, frontend: str):
        super().__init__(ds, frontend)

    def _chunks(self, lo, hi):
        """yt data chunks over the box ``lo`` .. ``hi`` (box units)."""
        if lo is None and hi is None:
            data = self.ds.all_data()
        else:
            # yt selects an octree's cells whose extent the box contains
            # enough of, not by centre: a box narrower than a coarse cell
            # misses it. The box is padded by one root cell (the coarsest
            # cell there is) and clipped to the domain; _inside then keeps
            # exactly the centres in the window.
            top = self._left + self._dims * self._dx0
            a, b = self._left.copy(), top.copy()
            for d in range(3):
                if lo is not None and np.isfinite(lo[d]):
                    a[d] = max(self._left[d], self._left[d] + (lo[d] - 1.0) * self._dx0)
                if hi is not None and np.isfinite(hi[d]):
                    b[d] = min(top[d], self._left[d] + (hi[d] + 1.0) * self._dx0)
            data = self.ds.box(self.ds.arr(a, "code_length"), self.ds.arr(b, "code_length"))
        # yt's chunk iterator advances its index state: one thread at a time.
        chunks = iter(data.chunks([], "io"))
        done = object()
        while True:
            with self._lock:
                chunk = next(chunks, done)
            if chunk is done:
                return
            yield chunk

    def _geometry(self, chunk) -> tuple[np.ndarray, np.ndarray]:
        """``(level (n,), centre (n, 3))`` of the cells of a chunk, box units."""
        size = np.asarray(chunk[("index", "dx")].to_value("code_length"), dtype=np.float64)
        level = np.rint(np.log2(self._dx0 / size)).astype(np.int64)
        centre = np.stack([(np.asarray(chunk[("index", n)].to_value("code_length"),
                                       dtype=np.float64) - self._left[d]) / self._dx0
                           for d, n in enumerate("xyz")], axis=1)
        return level, centre

    def leaf_counts(self) -> dict[int, int]:
        with self._lock:
            if self._counts is None:
                counts: dict[int, int] = {}
                for chunk in self._chunks(None, None):
                    size = np.asarray(chunk[("index", "dx")].to_value("code_length"),
                                      dtype=np.float64)
                    lv, n = np.unique(np.rint(np.log2(self._dx0 / size)).astype(np.int64),
                                      return_counts=True)
                    for a, b in zip(lv, n):
                        counts[int(a)] = counts.get(int(a), 0) + int(b)
                self._counts = counts
            return dict(self._counts)

    def leaf_blocks(self, fields: Sequence[str] = (), levels=None, lo=None, hi=None,
                    max_block: int = _BLOCK_ROWS) -> Iterator[LeafBlock]:
        self._unknown(fields)
        want = None if levels is None else {int(v) for v in levels}
        for chunk in self._chunks(lo, hi):
            with self._lock:
                level, centre = self._geometry(chunk)
                values = self._values(chunk, fields) if fields else {}
            keep = self._inside(centre, lo, hi)
            for lv in np.unique(level[keep]):
                if want is not None and int(lv) not in want:
                    continue
                rows = np.flatnonzero(keep & (level == lv))
                dx = 2.0 ** -int(lv)
                for a in range(0, rows.size, max_block):
                    sel = rows[a:a + max_block]
                    yield LeafBlock(int(lv), dx, centre[sel], {n: v[sel] for n, v in values.items()})


# ── opening ──────────────────────────────────────────────────────────────────

@lru_cache(maxsize=4)
def _cached(path: str, _stamp_: tuple, frontend: str) -> AmrHierarchy:
    yt = _require_yt()
    from yt.frontends import _all   # noqa: F401,PLC0415
    try:
        ds = yt.load(path)
    except Exception as exc:   # noqa: BLE001 — yt raises many kinds for a file it cannot read
        raise ValueError(f"yt cannot read {Path(path).name}: {exc}") from exc
    cls = YtGridAmr if hasattr(ds.index, "grids") and len(ds.index.grids) else YtOctreeAmr
    try:
        return cls(ds, frontend)
    except ValueError:
        raise
    except Exception as exc:   # noqa: BLE001 — a hierarchy yt describes in a way not mapped
        raise ValueError(f"{Path(path).name}: this mesh cannot be mapped ({type(exc).__name__}: "
                         f"{exc}).") from exc


def open_mesh(path: str | Path, allowed=None, *, any_frontend: bool = False) -> AmrHierarchy | None:
    """The mesh of another code ``path`` names, through yt. None when it is
    none (RAMSES, a particle snapshot, a file yt does not take); YtUnavailable
    when it could be one and yt is missing. ``allowed`` (path -> bool): the
    mesh's file must pass it, else PermissionError. ``any_frontend`` opens
    whatever yt takes, RAMSES included (the cross-check of the native reader)."""
    base = mesh_path(path)
    if not any_frontend and not looks_like_mesh(base):
        return None
    check_allowed(base, allowed)
    if not yt_available():
        # Could be an Enzo or FLASH file: say what is missing, not "no mesh".
        from . import ramses_output as rm   # noqa: PLC0415
        if rm.output_of(base) is not None:
            return None
        raise YtUnavailable(_MISSING)
    if any_frontend:
        return _cached(str(base), _stamp(Path(base)), "any")
    frontend = detect(base)
    if frontend is None:
        return None
    return _cached(str(base), _stamp(Path(base)), frontend)


_MISSING = ("This file may be an Enzo, FLASH, Athena, Chombo, AMReX, GAMER or ART output, which "
            "is read through yt; yt is not installed on this server (pip install yt).")


# ── the gas cells as a component of the point seam ───────────────────────────

def has_family(path: str | Path, slug: str) -> bool:
    """Whether the mesh has the component ``slug`` (the cells): True when it
    is a mesh yt takes. Never opens the dataset."""
    return slug == CELLS and detect(path) is not None


def component(path: str | Path):
    """The mesh's gas cells as a component (app/amr_cells.py)."""
    base = mesh_path(path)
    mesh = open_mesh(base)
    if mesh is None:
        raise ValueError(f"{Path(base).name} is no mesh yt reads.")
    if not mesh.fields or "density" not in mesh.fields:
        raise ValueError(f"{Path(base).name} has no gas density to read as cells.")
    return _cells_cached(base, _stamp(Path(base)))


@lru_cache(maxsize=4)
def _cells_cached(base: str, _stamp_: tuple):
    from .amr_cells import AmrCells   # noqa: PLC0415
    mesh = open_mesh(base)
    return AmrCells(mesh, mesh, yt_ref(base, CELLS), bool(np.all(mesh.ds.periodicity)))


def resolve(path: str | Path):
    base, slug = split_ref(path)
    if slug is None:
        raise ValueError(f"{path} names no yt component.")
    return component(base)


def components(path: str | Path) -> list[dict]:
    """The components a mesh offers (``{name, path, num_rows}``): its gas cells."""
    base = mesh_path(path)
    mesh = open_mesh(base)
    if mesh is None or "density" not in mesh.fields:
        return []
    return [{"name": "gas cells", "path": yt_ref(base, CELLS), "num_rows": mesh.n_leaf()}]


def uniform_field(path: str | Path, allowed=None) -> tuple[str, int, str] | None:
    """``(mesh path, level, field)`` when ``path`` is the output of another
    code whose leaves all lie on one level (a classic unigrid output), the
    field being its density, else its first; None for anything else, and for
    any file when yt is not installed (nothing is claimed without it). Such an
    output opens as a plain cube: the AMR uniform view written at that level
    is its cells, one for one."""
    if not yt_available() or not looks_like_mesh(path):
        return None
    mesh = open_mesh(path, allowed)
    if mesh is None or not mesh.fields:
        return None
    level = mesh.uniform_level()
    if level is None:
        return None
    return mesh_path(path), level, "density" if "density" in mesh.fields else mesh.fields[0]


#: ``(mesh path, version, field) -> cube`` already written by the uniform view.
_CUBES: dict[tuple, str] = {}
_CUBES_LOCK = threading.Lock()


def cached_cube(mesh: str, field: str) -> str | None:
    """The cube written for this version of ``mesh`` and ``field``, while its
    file is still there."""
    with _CUBES_LOCK:
        path = _CUBES.get((mesh, _stamp(Path(mesh)), field))
    return path if path and os.path.exists(path) else None


def remember_cube(mesh: str, field: str, cube: str) -> str:
    with _CUBES_LOCK:
        _CUBES[(mesh, _stamp(Path(mesh)), field)] = cube
    return cube


def check_allowed(path: str | Path, allowed=None) -> None:
    """Every file of the mesh ``path`` names must pass ``allowed`` before yt
    parses any (PermissionError naming the file, nothing of its content)."""
    if allowed is None:
        return
    for f in files(path, strict=True):
        if not allowed(f):
            raise PermissionError(f"{Path(f).name}, a file of this output, lies outside the "
                                  "folders this backend may read.")


#: Files of a mesh looked at (the data roots check, the cache identity).
MAX_FILES = 20000


def files(path: str | Path, strict: bool = False) -> list[str]:
    """The files that make up the mesh: the file or directory, its siblings of
    the same name (an Enzo output's ``.hierarchy``, ``.cpu0000``...) and every
    file under a directory, symbolic links to directories followed as yt
    follows them. More than :data:`MAX_FILES`: the first ones, or, ``strict``
    (the data roots check, which must see every file), ValueError."""
    p = Path(mesh_path(path))
    found = []
    if p.is_dir():
        for root, _dirs, names in os.walk(p, followlinks=True):
            found.extend(os.path.join(root, n) for n in names)
            if len(found) > MAX_FILES:
                if strict:
                    raise ValueError(f"{p.name} holds more than {MAX_FILES} files: it cannot be "
                                     "checked against the folders this backend may read.")
                break
    else:
        found.append(str(p))
        try:
            with os.scandir(p.parent) as it:
                found.extend(e.path for e in it if e.name.startswith(p.name + ".")
                             and e.is_file())
        except OSError:
            pass
    return sorted(found)
