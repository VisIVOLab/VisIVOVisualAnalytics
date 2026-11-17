"""Gadget snapshots read in place as tables of particles (issue #31).

A snapshot is one file (``snap_010``) or several (``snapdir_010/snap_010.0``
... ``.N``), each a Fortran-record binary or an HDF5 file. Every particle type
(0 gas, 1 dark matter, 2 and 3 the extra collisionless types, 4 stars, 5 black
holes) is one table of points, a *component*, named by a reference
``<first file>::gadget-type=<N>``: the same kind of plain string as a VBT
field or a PLUTO variable (app/volume_source.py), so it pickles to the render
processes, which reopen it. app/point_table.py opens it as it opens a VBT
point table, so every point renderer, the VR page and the catalogue tools read
a snapshot without knowing it is one.

The binary formats
------------------
Every record is framed by its length (a 4-byte integer before and after), in
the byte order of the machine that wrote it: the first marker is 256 (format
1, the header record) or 8 (format 2, a ``HEAD`` name record) read in one of
the two orders, which tells the order. Every marker is checked.

* Format 1 has no block names: the blocks follow Gadget-2's fixed order (POS,
  VEL, ID, MASS, then the gas blocks U, RHO, NE and NH with cooling, HSML, SFR
  with star formation, AGE, Z, POT, ACCE, ENDT, TSTP) and a block with no
  particles in a file is not written there. Each record is matched to the next
  name in that order whose size it has; a record that matches none is kept as
  ``BLK<n>``.
* Format 2 puts an 8-byte record before each block: its 4-character name and
  the size of the block that follows. Any block is read, known or not.

A block holds its types one after the other, type 0 first, and only the types
it is for. Which types those are is inferred from the sizes: the block's
length in every file must be one element size times the particles of one set
of types in that file (:data:`TYPE_SETS`, the known blocks' own set first).
Files with different mixtures of types leave one answer; a block whose size
fits no set is left out and named in :attr:`Snapshot.skipped`. The element
type follows from the positions (float or double) and from the block (IDs are
integers); a block of several values per particle becomes several columns.

Masses absent from the MASS block come from the header's mass table: a column
of that constant.

HDF5 snapshots (``PartType<N>`` groups, a ``Header`` group of attributes) map
their datasets to the same blocks; a contiguous, unfiltered dataset is
memory-mapped at its offset in the file, any other is read.

Units
-----
Columns keep the code's units; :class:`UnitSystem` says what they are and how
to make them physical. The defaults are Gadget's (lengths in comoving kpc/h,
masses in 1e10 Msun/h, velocities in km/s with the √a of the internal
velocity), changed by the run's own ``parameters-usedvalues`` (or ``-usedvalues``
file) beside the snapshot or one folder up, a ``Units`` / ``Parameters`` group
of an HDF5 file, or the environment (:data:`UNITS_ENV`). Derived gas columns:
the temperature from the internal energy (and the electron abundance NE when
the snapshot has it, else a fully ionised gas, which the metadata states), and
the entropy K = kT / n_e^(2/3) in keV cm^2.
"""
from __future__ import annotations

import os
import re
import struct
from collections.abc import Mapping
from dataclasses import dataclass, replace
from functools import lru_cache
from pathlib import Path

import numpy as np

#: ``<first file>`` + REF_SEP + particle type.
REF_SEP = "::gadget-type="
NTYPES = 6
TYPE_NAMES = ("gas", "dark matter", "disk", "bulge", "stars", "black holes")
HEADER_BYTES = 256

# ── Physical constants (cgs) ─────────────────────────────────────────────────
PROTON_MASS_G = 1.67262192e-24
BOLTZMANN_ERG_K = 1.380649e-16
KEV_ERG = 1.602176634e-9
KPC_CM = 3.085678e21
MSUN_G = 1.989e33
GAMMA = 5.0 / 3.0
#: Hydrogen mass fraction assumed for the temperature and the entropy.
HYDROGEN_MASS_FRACTION = 0.76


# ── Header ───────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Header:
    """The snapshot header. ``npart`` is this file's; ``npart_total`` the
    snapshot's (the high word included)."""
    npart: tuple[int, ...]
    mass: tuple[float, ...]
    time: float
    redshift: float
    flag_sfr: int
    flag_feedback: int
    npart_total: tuple[int, ...]
    flag_cooling: int
    num_files: int
    box_size: float
    omega0: float
    omega_lambda: float
    hubble: float
    flag_stellarage: int = 0
    flag_metals: int = 0
    flag_entropy_instead_u: int = 0
    flag_doubleprecision: int = 0


def parse_header(raw: bytes, endian: str) -> Header:
    """The 256-byte header record of a binary snapshot, in byte order
    ``endian`` ("<" or ">")."""
    if len(raw) < 200:
        raise ValueError(f"Gadget header too short: {len(raw)} bytes")
    e = endian
    npart = struct.unpack_from(e + "6i", raw, 0)
    mass = struct.unpack_from(e + "6d", raw, 24)
    time, redshift = struct.unpack_from(e + "2d", raw, 72)
    flag_sfr, flag_feedback = struct.unpack_from(e + "2i", raw, 88)
    low = struct.unpack_from(e + "6I", raw, 96)
    flag_cooling, num_files = struct.unpack_from(e + "2i", raw, 120)
    box, omega0, omega_l, hubble = struct.unpack_from(e + "4d", raw, 128)
    flag_age, flag_metals = struct.unpack_from(e + "2i", raw, 160)
    high = struct.unpack_from(e + "6I", raw, 168)
    flag_entropy, flag_double = struct.unpack_from(e + "2i", raw, 192)
    total = tuple(int(lo) + (int(hi) << 32) for lo, hi in zip(low, high))
    if any(n < 0 for n in npart) or num_files < 1:
        raise ValueError("Not a Gadget header: negative particle counts or no files.")
    return Header(tuple(int(n) for n in npart), tuple(float(m) for m in mass), float(time),
                  float(redshift), int(flag_sfr), int(flag_feedback), total,
                  int(flag_cooling), int(num_files), float(box), float(omega0),
                  float(omega_l), float(hubble), int(flag_age), int(flag_metals),
                  int(flag_entropy), int(flag_double))


# ── Blocks ───────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class RawBlock:
    """A block of one binary file: its name and where its data lies."""
    name: str           # stripped of its padding: "POS", "ID", "BHMA"
    offset: int         # of the data, past its leading record marker
    nbytes: int


@dataclass(frozen=True)
class FileLayout:
    """One file of a snapshot: its header, its blocks, how it is written."""
    path: str
    kind: str                       # "binary" | "hdf5"
    endian: str                     # "<" | ">" ("" for HDF5)
    fmt: int                        # 1 | 2 (0 for HDF5)
    header: Header
    blocks: tuple[RawBlock, ...]


def detect(path: str | Path) -> tuple[int, str] | None:
    """``(format, byte order)`` of a binary Gadget file from its first
    record marker (and, for format 2, the ``HEAD`` name); (0, "") for an
    HDF5 snapshot; None for anything else."""
    try:
        with open(path, "rb") as fh:
            head = fh.read(16)
    except OSError:
        return None
    if head.startswith(b"\x89HDF\r\n\x1a\n"):
        return (0, "") if _is_hdf5_snapshot(path) else None
    if len(head) < 16:
        return None
    for e in ("<", ">"):
        first = struct.unpack(e + "i", head[:4])[0]
        if first == HEADER_BYTES:
            # Format 1: the header record's closing marker must agree.
            try:
                with open(path, "rb") as fh:
                    fh.seek(4 + HEADER_BYTES)
                    closing = fh.read(4)
            except OSError:
                return None
            if len(closing) == 4 and struct.unpack(e + "i", closing)[0] == HEADER_BYTES:
                return 1, e
        if first == 8 and head[4:8] == b"HEAD":
            return 2, e
    return None


def _is_hdf5_snapshot(path) -> bool:
    try:
        import h5py  # noqa: PLC0415
        with h5py.File(path, "r") as f:
            return "Header" in f and "NumPart_ThisFile" in f["Header"].attrs
    except Exception:   # noqa: BLE001 - not readable as one: not a snapshot
        return False


def is_gadget(path: str | Path) -> bool:
    return Path(path).is_file() and detect(path) is not None


class _Records:
    """Reads the Fortran records of one file, checking every marker."""

    def __init__(self, path: str, endian: str):
        self.path, self.e = path, endian
        self.size = os.path.getsize(path)
        self.fh = open(path, "rb")

    def close(self):
        self.fh.close()

    def marker(self, at: int) -> int:
        self.fh.seek(at)
        raw = self.fh.read(4)
        if len(raw) != 4:
            raise ValueError(f"{self.path}: truncated at byte {at}")
        return struct.unpack(self.e + "i", raw)[0]

    def record(self, at: int) -> tuple[int, int, int]:
        """``(data offset, length, next record)`` of the record at ``at``."""
        length = self.marker(at)
        end = at + 4 + length
        if length < 0 or end + 4 > self.size:
            raise ValueError(f"{self.path}: record at byte {at} claims {length} bytes, "
                             f"past the end of the file")
        if self.marker(end) != length:
            raise ValueError(f"{self.path}: record at byte {at}: its closing length "
                             f"marker does not match the opening one ({length})")
        return at + 4, length, end + 4


#: Gadget-2's block order in format 1, with the header flag a block needs.
FORMAT1_ORDER = (("POS", None), ("VEL", None), ("ID", None), ("MASS", None),
                 ("U", None), ("RHO", None), ("NE", "flag_cooling"),
                 ("NH", "flag_cooling"), ("HSML", None), ("SFR", "flag_sfr"),
                 ("AGE", "flag_stellarage"), ("Z", "flag_metals"), ("POT", None),
                 ("ACCE", None), ("ENDT", None), ("TSTP", None))


def scan_file(path: str | Path) -> FileLayout:
    """The layout of one file of a snapshot (binary or HDF5)."""
    path = str(path)
    found = detect(path)
    if found is None:
        raise ValueError(f"{path} is not a Gadget snapshot file.")
    fmt, e = found
    if fmt == 0:
        return _scan_hdf5(path)
    rec = _Records(path, e)
    try:
        blocks: list[RawBlock] = []
        at = 0
        header = None
        if fmt == 1:
            off, length, at = rec.record(0)
            rec.fh.seek(off)
            header = parse_header(rec.fh.read(length), e)
            raw = []
            while at < rec.size:
                off, length, at = rec.record(at)
                raw.append((off, length))
            blocks = _name_format1(raw, header)
        else:
            while at < rec.size:
                off, length, at = rec.record(at)
                if length != 8:
                    raise ValueError(f"{path}: expected an 8-byte block name record at "
                                     f"byte {off - 4}, found {length} bytes")
                rec.fh.seek(off)
                name_raw, size = struct.unpack(e + "4si", rec.fh.read(8))
                name = name_raw.decode("ascii", errors="replace").strip()
                off, length, at = rec.record(at)
                if size != length + 8:
                    raise ValueError(f"{path}: block {name!r} announces {size - 8} bytes, "
                                     f"holds {length}")
                if name == "HEAD":
                    rec.fh.seek(off)
                    header = parse_header(rec.fh.read(length), e)
                else:
                    blocks.append(RawBlock(name, off, length))
            if header is None:
                raise ValueError(f"{path}: no HEAD block")
    finally:
        rec.close()
    return FileLayout(path, "binary", e, fmt, header, tuple(blocks))


def _name_format1(raw: list[tuple[int, int]], header: Header) -> list[RawBlock]:
    """Names for format 1's records, by Gadget-2's order and their sizes."""
    if not raw:
        return []
    n = header.npart
    ntot = sum(n)
    out = []
    # The float size from the positions (the first block after the header).
    fsize = raw[0][1] // (3 * ntot) if ntot else 4
    if fsize not in (4, 8):
        fsize = 4
    order = [name for name, flag in FORMAT1_ORDER
             if flag is None or getattr(header, flag)]
    k = 0
    for i, (off, length) in enumerate(raw):
        name = None
        while k < len(order):
            cand = order[k]
            k += 1
            if _format1_size_fits(cand, length, n, header.mass, fsize):
                name = cand
                break
        out.append(RawBlock(name or f"BLK{i + 1}", off, length))
    return out


def _format1_size_fits(name: str, length: int, n, mass, fsize: int) -> bool:
    types = _known_types(name, n, mass)
    count = sum(n[t] for t in types)
    if count == 0:
        return False
    per = length / count
    if name == "ID":
        return per in (4, 8)
    return per == fsize * BLOCKS.get(name, (None, 1))[1]


# ── What each block is ───────────────────────────────────────────────────────

#: Known blocks: (their types, values per particle, column names). Types are
#: "all", "mass" (the types the mass table leaves to the MASS block) or a
#: tuple. A block not here is inferred from its size and named after itself.
BLOCKS: dict[str, tuple[object, int, tuple[str, ...]]] = {
    "POS": ("all", 3, ("x", "y", "z")),
    "VEL": ("all", 3, ("vx", "vy", "vz")),
    "ID": ("all", 1, ("id",)),
    "MASS": ("mass", 1, ("mass",)),
    "U": ((0,), 1, ("u",)),
    "RHO": ((0,), 1, ("rho",)),
    "NE": ((0,), 1, ("ne",)),
    "NH": ((0,), 1, ("nh",)),
    "HSML": ((0,), 1, ("hsml",)),
    "SFR": ((0,), 1, ("sfr",)),
    "AGE": ((4,), 1, ("age",)),
    "Z": ((0, 4), 1, ("metallicity",)),
    "POT": ("all", 1, ("potential",)),
    "ACCE": ("all", 3, ("ax", "ay", "az")),
    "ENDT": ((0,), 1, ("dudt",)),
    "TSTP": ("all", 1, ("timestep",)),
    "BHMA": ((5,), 1, ("bh_mass",)),
    "BHMD": ((5,), 1, ("bh_mdot",)),
    "BHPC": ((5,), 1, ("bh_progenitors",)),
    "iM": ((4,), 1, ("initial_mass",)),
    "TEMP": ((0,), 1, ("temperature_block",)),
    # SubFind group catalogues (issue #392): groups are "type" 0, subhalos 1.
    "GPOS": ((0,), 3, ("x", "y", "z")),
    "MVIR": ((0,), 1, ("mvir",)),
    "RVIR": ((0,), 1, ("rvir",)),
    "SPOS": ((1,), 3, ("x", "y", "z")),
}
#: Blocks of integers (read as unsigned of 4 or 8 bytes).
INT_BLOCKS = {"ID", "BHPC", "GLEN", "GOFF", "SLEN", "SOFF", "SSUB", "NSUB", "FSUB"}

#: The sets of types an unknown block is tried for, in order (after a known
#: block's own): "all" types present, then the common subsets.
TYPE_SETS: tuple[object, ...] = ("all", (0,), (4,), (5,), (0, 4), "mass", (1,), (2,), (3,),
                                 (0, 4, 5), (4, 5))


def _known_types(name: str, n, mass) -> tuple[int, ...]:
    spec = BLOCKS.get(name, ("all",))[0]
    return _resolve_types(spec, n, mass)


def _resolve_types(spec, n, mass) -> tuple[int, ...]:
    if spec == "all":
        return tuple(range(NTYPES))
    if spec == "mass":
        return tuple(t for t in range(NTYPES) if mass[t] == 0)
    return tuple(spec)


@dataclass(frozen=True)
class BlockSpec:
    """A block across the snapshot: its types, element type and width."""
    name: str
    types: tuple[int, ...]
    dtype: str                      # numpy type without byte order: "f4", "u8"
    dims: int
    inferred: bool                  # its types came from the sizes, not the table

    @property
    def columns(self) -> tuple[str, ...]:
        known = BLOCKS.get(self.name)
        if known is not None and len(known[2]) == self.dims:
            return known[2]
        base = known[2][0] if known is not None else self.name.lower()
        return (base,) if self.dims == 1 else tuple(f"{base}_{k}" for k in range(self.dims))


def infer_block(name: str, lengths: list[tuple[tuple[int, ...], int]], mass,
                fsize: int) -> BlockSpec | None:
    """The spec of block ``name`` from ``(npart of the file, length)`` of
    every file that has it; None when no set of types explains its size in
    every file."""
    present = [any(np_[t] for np_, _ in lengths) for t in range(NTYPES)]
    candidates = []
    if name in BLOCKS:
        candidates.append((BLOCKS[name][0], False))
    candidates += [(s, True) for s in TYPE_SETS]
    seen = set()
    for spec, inferred in candidates:
        types = tuple(t for t in _resolve_types(spec, lengths[0][0] if lengths else (0,) * 6,
                                                mass) if present[t])
        if not types or types in seen:
            continue
        seen.add(types)
        per = None
        ok = True
        for np_, length in lengths:
            count = sum(np_[t] for t in types)
            if count == 0:
                if length:
                    ok = False
                    break
                continue
            if length % count:
                ok = False
                break
            if per is None:
                per = length // count
            elif per != length // count:
                ok = False
                break
        if not ok or not per:
            continue
        if name in INT_BLOCKS:
            if per not in (4, 8):
                continue
            return BlockSpec(name, types, f"u{per}", 1, inferred)
        size = fsize if per % fsize == 0 else (4 if per % 4 == 0 else 0)
        if not size:
            continue
        dims = per // size
        if name in BLOCKS and not inferred and BLOCKS[name][1] != dims and \
                BLOCKS[name][1] != 1:
            continue
        return BlockSpec(name, types, f"f{size}", dims, inferred)
    return None


# ── HDF5 ─────────────────────────────────────────────────────────────────────

#: HDF5 dataset names -> the binary block they are.
HDF5_BLOCKS = {
    "Coordinates": "POS", "Velocities": "VEL", "ParticleIDs": "ID", "Masses": "MASS",
    "InternalEnergy": "U", "Density": "RHO", "ElectronAbundance": "NE",
    "NeutralHydrogenAbundance": "NH", "SmoothingLength": "HSML",
    "StarFormationRate": "SFR", "StellarFormationTime": "AGE", "Metallicity": "Z",
    "Potential": "POT", "Acceleration": "ACCE", "BH_Mass": "BHMA", "BH_Mdot": "BHMD",
}


@dataclass(frozen=True)
class H5Block:
    """A dataset of one HDF5 file, for one type."""
    name: str                       # the block it is ("POS", or its own name)
    ptype: int
    dataset: str                    # its path in the file
    shape: tuple[int, ...]
    dtype: str                      # numpy type with byte order
    offset: int | None              # in the file, when memory-mappable


def _scan_hdf5(path: str) -> FileLayout:
    import h5py  # noqa: PLC0415
    with h5py.File(path, "r") as f:
        a = f["Header"].attrs

        def get(key, default=0):
            return a[key] if key in a else default
        npart = tuple(int(v) for v in get("NumPart_ThisFile", [0] * NTYPES))
        low = [int(v) for v in get("NumPart_Total", [0] * NTYPES)]
        high = [int(v) for v in get("NumPart_Total_HighWord", [0] * NTYPES)]
        mass = tuple(float(v) for v in get("MassTable", [0.0] * NTYPES))
        header = Header(
            npart + (0,) * (NTYPES - len(npart)), mass + (0.0,) * (NTYPES - len(mass)),
            float(get("Time", 0.0)), float(get("Redshift", 0.0)),
            int(get("Flag_Sfr")), int(get("Flag_Feedback")),
            tuple(lo + (hi << 32) for lo, hi in zip(low, high + [0] * NTYPES)),
            int(get("Flag_Cooling")), int(get("NumFilesPerSnapshot", 1)),
            float(get("BoxSize", 0.0)), float(get("Omega0", 0.0)),
            float(get("OmegaLambda", 0.0)), float(get("HubbleParam", 1.0)),
            # Flag_Entropy_ICs is about the initial conditions: a snapshot's
            # InternalEnergy is the energy.
            int(get("Flag_StellarAge")), int(get("Flag_Metals")), 0,
            int(get("Flag_DoublePrecision")))
        blocks = []
        for t in range(NTYPES):
            group = f.get(f"PartType{t}")
            if group is None:
                continue
            for dname, dset in group.items():
                if not isinstance(dset, h5py.Dataset) or dset.ndim not in (1, 2):
                    continue
                offset = None
                if dset.chunks is None and dset.compression is None:
                    offset = dset.id.get_offset()
                blocks.append(H5Block(HDF5_BLOCKS.get(dname, dname), t, dset.name,
                                      tuple(dset.shape), dset.dtype.str, offset))
    return FileLayout(path, "hdf5", "", 0, header, tuple(blocks))


# ── Units ────────────────────────────────────────────────────────────────────

#: ``key=value,...`` with any of length_cm, mass_g, velocity_cm_s, comoving
#: (0/1), hubble_units (0/1): overrides the snapshot's units, for runs whose
#: parameter file is not beside them.
UNITS_ENV = "VISIVO_GADGET_UNITS"
_PARAM_FILES = ("parameters-usedvalues", "parameters-usedValues", "param.txt-usedvalues",
                "params.txt-usedvalues", "parameters.txt-usedvalues")


@dataclass(frozen=True)
class UnitSystem:
    """The code units of a snapshot and what makes them physical."""
    length_cm: float = KPC_CM
    mass_g: float = 1.0e10 * MSUN_G
    velocity_cm_s: float = 1.0e5
    #: Lengths and densities comoving, velocities with the internal √a.
    comoving: bool = True
    #: Lengths and masses in units of 1/h.
    hubble_units: bool = True
    source: str = "Gadget defaults"

    def describe(self) -> dict:
        return {"length_cm": self.length_cm, "mass_g": self.mass_g,
                "velocity_cm_s": self.velocity_cm_s, "comoving": self.comoving,
                "hubble_units": self.hubble_units, "source": self.source,
                "length": _unit_name(self.length_cm, {"kpc": KPC_CM, "Mpc": 1e3 * KPC_CM,
                                                      "pc": 1e-3 * KPC_CM}, "cm"),
                "mass": _unit_name(self.mass_g, {"1e10 Msun": 1e10 * MSUN_G,
                                                 "Msun": MSUN_G}, "g"),
                "velocity": _unit_name(self.velocity_cm_s, {"km/s": 1e5}, "cm/s")}


def _unit_name(value: float, names: dict, base: str) -> str:
    for name, v in names.items():
        if abs(value / v - 1.0) < 1e-3:
            return name
    return f"{value:.6g} {base}"


def units_for(first_file: str | Path, hdf5_units: dict | None = None) -> UnitSystem:
    """The snapshot's units: the defaults, then the run's parameter file
    (beside the snapshot or one folder up), an HDF5 file's own units, then
    the environment (:data:`UNITS_ENV`)."""
    units = UnitSystem()
    folder = Path(first_file).parent
    for where in (folder, folder.parent):
        for name in _PARAM_FILES:
            p = where / name
            if p.is_file():
                units = _from_params(p, units)
                break
        else:
            continue
        break
    if hdf5_units:
        units = replace(units, source="the HDF5 file's units", **hdf5_units)
    env = os.environ.get(UNITS_ENV, "").strip()
    if env:
        units = _from_env(env, units)
    return units


def _from_params(path: Path, units: UnitSystem) -> UnitSystem:
    values = {}
    try:
        for line in path.read_text(errors="replace").splitlines():
            tokens = line.split("%")[0].split()
            if len(tokens) >= 2:
                values[tokens[0]] = tokens[1]
    except OSError:
        return units
    kw = {}
    for key, attr in (("UnitLength_in_cm", "length_cm"), ("UnitMass_in_g", "mass_g"),
                      ("UnitVelocity_in_cm_per_s", "velocity_cm_s")):
        try:
            kw[attr] = float(values[key])
        except (KeyError, ValueError):
            pass
    if "ComovingIntegrationOn" in values:
        kw["comoving"] = values["ComovingIntegrationOn"].strip() not in ("0", "")
    if not kw:
        return units
    return replace(units, source=f"{path.name} beside the snapshot", **kw)


def _from_env(text: str, units: UnitSystem) -> UnitSystem:
    kw = {}
    for item in text.split(","):
        key, _, value = item.partition("=")
        key, value = key.strip(), value.strip()
        if key in ("length_cm", "mass_g", "velocity_cm_s"):
            kw[key] = float(value)
        elif key in ("comoving", "hubble_units"):
            kw[key] = value.lower() not in ("0", "false", "no", "")
        elif key:
            raise ValueError(f"{UNITS_ENV}: unknown key {key!r}")
    return replace(units, source=f"{UNITS_ENV}", **kw)


# ── The snapshot ─────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Piece:
    """The rows of one type in one file for one block."""
    path: str
    offset: int | None              # bytes; None: an HDF5 dataset read whole
    dtype: str                      # with byte order
    rows: int
    dims: int
    dataset: str = ""               # HDF5 only

    def array(self) -> np.ndarray:
        """(rows, dims), memory-mapped in place where it can be."""
        if self.rows == 0:
            return np.empty((0, self.dims), dtype=self.dtype)
        if self.offset is not None:
            return np.memmap(self.path, dtype=np.dtype(self.dtype), mode="r",
                             offset=self.offset, shape=(self.rows, self.dims))
        import h5py  # noqa: PLC0415
        with h5py.File(self.path, "r") as f:
            return np.asarray(f[self.dataset][()]).reshape(self.rows, self.dims)


@dataclass(frozen=True)
class Snapshot:
    files: tuple[FileLayout, ...]
    header: Header                  # the first file's, npart = the totals
    blocks: dict                    # name -> BlockSpec
    pieces: dict                    # (name, type) -> tuple[Piece, ...]
    units: UnitSystem
    skipped: tuple[str, ...] = ()   # blocks no set of types explains
    fmt: str = ""

    @property
    def path(self) -> str:
        return self.files[0].path

    @property
    def scale_factor(self) -> float:
        return self.header.time if self.units.comoving else 1.0

    def counts(self) -> tuple[int, ...]:
        return self.header.npart_total

    def component(self, ptype: int) -> "Component":
        return Component(self, int(ptype))

    def describe(self) -> dict:
        h = self.header
        return {"format": self.fmt, "files": len(self.files),
                "time": h.time, "scale_factor": self.scale_factor,
                "redshift": h.redshift, "box_size": h.box_size, "omega0": h.omega0,
                "omega_lambda": h.omega_lambda, "hubble_param": h.hubble,
                "mass_table": list(h.mass), "npart_total": list(h.npart_total),
                "units": self.units.describe(),
                "blocks": {name: {"types": list(b.types), "dims": b.dims, "dtype": b.dtype,
                                  "inferred": b.inferred}
                           for name, b in self.blocks.items()},
                "skipped_blocks": list(self.skipped)}


def snapshot_files(path: str | Path) -> list[Path]:
    """The files of the snapshot ``path`` (a file of it, the base name of a
    multi-file one, or its ``snapdir``), first file first."""
    p = Path(path)
    if p.is_dir():
        first = first_in_dir(p)
        if first is None:
            raise ValueError(f"{p} holds no Gadget snapshot.")
        p = first
    if not p.exists():
        for suffix in (".0", ".0.hdf5"):
            cand = p.with_name(p.name + suffix)
            if cand.is_file():
                p = cand
                break
    header = scan_file(p).header
    if header.num_files <= 1:
        return [p]
    m = re.match(r"^(?P<base>.*)\.(?P<k>\d+)(?P<ext>\.hdf5|\.h5)?$", p.name)
    if m is None:
        raise ValueError(f"{p.name}: its header says {header.num_files} files, but "
                         "the name does not end in .<number>")
    ext = m.group("ext") or ""
    files = [p.with_name(f"{m.group('base')}.{k}{ext}") for k in range(header.num_files)]
    missing = [f.name for f in files if not f.is_file()]
    if missing:
        raise ValueError(f"The snapshot has {header.num_files} files; missing: "
                         + ", ".join(missing[:5]) + (" ..." if len(missing) > 5 else ""))
    return files


#: At most this many files of a folder are probed for a snapshot.
MAX_PROBES = 64


def first_in_dir(folder: Path) -> Path | None:
    """The snapshot a folder (a ``snapdir``) holds: its first ``<name>.0``
    (or ``.0.hdf5``) that is a Gadget file, else its first ``snap*`` file
    that is one. Only names are listed; at most MAX_PROBES files are read."""
    try:
        with os.scandir(folder) as it:
            names = sorted(e.name for e in it if e.is_file())
    except OSError:
        return None
    probes = 0
    for pattern in (r"\.0(\.hdf5|\.h5)?$", r"^snap"):
        for name in names:
            if not re.search(pattern, name):
                continue
            probes += 1
            if probes > MAX_PROBES:
                return None
            if is_gadget(folder / name):
                return folder / name
    return None


def open_snapshot(path: str | Path) -> Snapshot:
    """The snapshot ``path`` belongs to, scanned (headers and block markers
    only: no data is read). Cached while its files are unchanged."""
    files = snapshot_files(path)
    key = tuple((str(f.resolve()), *_stat(f)) for f in files)
    return _cached_snapshot(key)


def _stat(f: Path) -> tuple[int, int]:
    st = f.stat()
    return st.st_size, st.st_mtime_ns


@lru_cache(maxsize=16)
def _cached_snapshot(key) -> Snapshot:
    layouts = tuple(scan_file(p) for p, _s, _m in key)
    return _assemble(layouts)


def _assemble(layouts: tuple[FileLayout, ...]) -> Snapshot:
    first = layouts[0].header
    totals = tuple(sum(f.header.npart[t] for f in layouts) for t in range(NTYPES))
    if any(totals[t] != first.npart_total[t] for t in range(NTYPES)) and any(first.npart_total):
        raise ValueError(f"The files hold {list(totals)} particles per type; the header "
                         f"announces {list(first.npart_total)}: a file is missing or "
                         "belongs to another snapshot")
    header = replace(first, npart=totals, npart_total=totals)
    if layouts[0].kind == "hdf5":
        return _assemble_hdf5(layouts, header)
    # The float size from the positions.
    fsize = 4
    for f in layouts:
        pos = next((b for b in f.blocks if b.name == "POS"), None)
        n = sum(f.header.npart)
        if pos is not None and n:
            fsize = pos.nbytes // (3 * n)
            break
    if fsize not in (4, 8):
        raise ValueError(f"POS holds {fsize} bytes per coordinate: not float or double")
    names: list[str] = []
    for f in layouts:
        for b in f.blocks:
            if b.name not in names:
                names.append(b.name)
    specs, pieces, skipped = {}, {}, []
    for name in names:
        lengths = []
        for f in layouts:
            blk = [b for b in f.blocks if b.name == name]
            lengths.append((f.header.npart, sum(b.nbytes for b in blk)))
        spec = infer_block(name, [lv for lv in lengths if lv[1]] or lengths, first.mass, fsize)
        # Files without the block must have none of its particles.
        if spec is not None and any(
                length == 0 and sum(np_[t] for t in spec.types)
                for np_, length in lengths):
            spec = None
        if spec is None:
            skipped.append(name)
            continue
        specs[name] = spec
        size = np.dtype(spec.dtype).itemsize * spec.dims
        for t in spec.types:
            parts = []
            for f in layouts:
                blk = next((b for b in f.blocks if b.name == name), None)
                rows = f.header.npart[t]
                if blk is None or rows == 0:
                    continue
                before = sum(f.header.npart[u] for u in spec.types if u < t)
                parts.append(Piece(f.path, blk.offset + before * size, f.endian + spec.dtype,
                                   rows, spec.dims))
            pieces[(name, t)] = tuple(parts)
    fmt = f"Gadget format {layouts[0].fmt}, {'big' if layouts[0].endian == '>' else 'little'}-endian"
    return Snapshot(layouts, header, specs, pieces, units_for(layouts[0].path),
                    tuple(skipped), fmt)


def _assemble_hdf5(layouts, header: Header) -> Snapshot:
    specs, pieces = {}, {}
    for f in layouts:
        for b in f.blocks:
            dims = b.shape[1] if len(b.shape) == 2 else 1
            kind = np.dtype(b.dtype)
            spec = specs.get(b.name)
            types = tuple(sorted(set((spec.types if spec else ()) + (b.ptype,))))
            specs[b.name] = BlockSpec(b.name, types, kind.str[1:], dims, False)
            pieces.setdefault((b.name, b.ptype), []).append(
                Piece(f.path, b.offset, b.dtype, b.shape[0], dims, b.dataset))
    pieces = {k: tuple(v) for k, v in pieces.items()}
    return Snapshot(layouts, header, specs, pieces,
                    units_for(layouts[0].path, _hdf5_units(layouts[0].path)), (),
                    "Gadget HDF5")


def _hdf5_units(path: str) -> dict:
    import h5py  # noqa: PLC0415
    out = {}
    try:
        with h5py.File(path, "r") as f:
            for group in ("Units", "Parameters", "Header"):
                if group not in f:
                    continue
                a = f[group].attrs
                for key, attr in (("UnitLength_in_cm", "length_cm"),
                                  ("UnitMass_in_g", "mass_g"),
                                  ("UnitVelocity_in_cm_per_s", "velocity_cm_s")):
                    if key in a and attr not in out:
                        out[attr] = float(a[key])
                if "ComovingIntegrationOn" in a and "comoving" not in out:
                    out["comoving"] = bool(int(a["ComovingIntegrationOn"]))
    except Exception:   # noqa: BLE001 - units are optional
        return {}
    return out


# ── A component: one particle type as a table ────────────────────────────────

class Component:
    """One particle type of a snapshot as named columns, in code units.

    Each column is the type's rows of its block: a view of the memory map
    for a single-file snapshot (no copy), the files' rows joined for a
    multi-file one (built when first asked for). Derived columns
    (``temperature``, ``entropy``) are computed when asked for."""

    def __init__(self, snapshot: Snapshot, ptype: int):
        if not 0 <= ptype < NTYPES:
            raise ValueError(f"Gadget particle type {ptype} outside 0..5")
        self.snapshot = snapshot
        self.ptype = ptype
        self.num_rows = int(snapshot.header.npart_total[ptype])
        self._sources: dict[str, tuple[str, int]] = {}
        for name, spec in snapshot.blocks.items():
            if ptype not in spec.types or not snapshot.pieces.get((name, ptype)):
                continue
            for k, col in enumerate(spec.columns):
                col = col if col not in self._sources else f"{col}_{name.lower()}"
                self._sources[col] = (name, k)
        # The mass table's value when the MASS block has none for this type
        # (Gadget leaves it out then); a zero there with no block is no mass.
        self._mass_from_table = ("mass" not in self._sources and self.num_rows > 0
                                 and snapshot.header.mass[ptype] != 0)
        self.derived: dict[str, str] = {}
        if ptype == 0 and "u" in self._sources:
            if not snapshot.header.flag_entropy_instead_u or "rho" in self._sources:
                self.derived["temperature"] = (
                    "from U and NE" if "ne" in self._sources else
                    "from U, gas assumed fully ionised (no NE block)")
            if "rho" in self._sources:
                self.derived["entropy"] = self.derived.get("temperature", "")
        self.names = list(self._sources) + (["mass"] if self._mass_from_table else []) \
            + list(self.derived)

    @property
    def name(self) -> str:
        return TYPE_NAMES[self.ptype]

    def column(self, name: str) -> np.ndarray:
        if name in self._sources:
            block, k = self._sources[name]
            parts = [p.array()[:, k] for p in self.snapshot.pieces[(block, self.ptype)]]
            return parts[0] if len(parts) == 1 else np.concatenate(parts)
        if name == "mass" and self._mass_from_table:
            spec = self.snapshot.blocks.get("POS")
            dtype = spec.dtype if spec is not None else "f4"
            return np.full(self.num_rows, self.snapshot.header.mass[self.ptype], dtype=dtype)
        if name == "temperature" and name in self.derived:
            return self.temperature()
        if name == "entropy" and name in self.derived:
            return self.entropy()
        raise KeyError(name)

    def columns(self) -> "LazyColumns":
        return LazyColumns(self)

    # ── physics ──
    def _u_cgs(self) -> np.ndarray:
        """Specific internal energy in erg/g."""
        u = np.asarray(self.column("u"), dtype=np.float64)
        units = self.snapshot.units
        if self.snapshot.header.flag_entropy_instead_u:
            # The block holds the entropic function A = P / rho^gamma of the
            # physical code density: u = A rho^(gamma-1) / (gamma - 1).
            a = self.snapshot.scale_factor
            rho = np.asarray(self.column("rho"), dtype=np.float64) / a ** 3
            u = u * rho ** (GAMMA - 1.0) / (GAMMA - 1.0)
        return u * units.velocity_cm_s ** 2

    def mean_molecular_weight(self) -> np.ndarray | float:
        x = HYDROGEN_MASS_FRACTION
        if "ne" in self._sources:
            ne = np.asarray(self.column("ne"), dtype=np.float64)
            return 4.0 / (1.0 + 3.0 * x + 4.0 * x * ne)
        return 4.0 / (3.0 + 5.0 * x)        # fully ionised H and He

    def temperature(self) -> np.ndarray:
        """Gas temperature in K: (gamma - 1) mu m_p u / k_B."""
        mu = self.mean_molecular_weight()
        return ((GAMMA - 1.0) * mu * PROTON_MASS_G / BOLTZMANN_ERG_K * self._u_cgs())

    def physical_density(self) -> np.ndarray:
        """Gas density in g/cm^3 (physical, h removed)."""
        units, h = self.snapshot.units, self.snapshot.header.hubble
        a = self.snapshot.scale_factor
        factor = units.mass_g / units.length_cm ** 3
        if units.hubble_units:
            factor *= h ** 2
        return np.asarray(self.column("rho"), dtype=np.float64) * factor / a ** 3

    def electron_density(self) -> np.ndarray:
        """n_e in cm^-3: NE n_H, or a fully ionised gas's (1 + X)/2 rho/m_p."""
        x = HYDROGEN_MASS_FRACTION
        rho = self.physical_density()
        if "ne" in self._sources:
            return np.asarray(self.column("ne"), dtype=np.float64) * x * rho / PROTON_MASS_G
        return 0.5 * (1.0 + x) * rho / PROTON_MASS_G

    def entropy(self) -> np.ndarray:
        """K = k_B T / n_e^(2/3) in keV cm^2."""
        kt_kev = BOLTZMANN_ERG_K * self.temperature() / KEV_ERG
        return kt_kev / self.electron_density() ** (2.0 / 3.0)

    def units(self) -> dict[str, str]:
        """Each column's unit as stored (code units, stated)."""
        u = self.snapshot.units.describe()
        h = "/h" if self.snapshot.units.hubble_units else ""
        com = " comoving" if self.snapshot.units.comoving else ""
        length = f"{u['length']}{h}{com}"
        mass = f"{u['mass']}{h}"
        vel = f"{u['velocity']} (internal, x sqrt(a))" if self.snapshot.units.comoving \
            else u["velocity"]
        table = {"x": length, "y": length, "z": length, "hsml": length,
                 "vx": vel, "vy": vel, "vz": vel, "mass": mass, "bh_mass": mass,
                 "rho": f"{mass} / ({length})^3", "u": f"({u['velocity']})^2",
                 "temperature": "K", "entropy": "keV cm^2"}
        return {n: table[n] for n in self.names if n in table}

    def physical(self) -> dict[str, dict]:
        """Per column, the factor that makes it physical and the unit then:
        lengths in kpc (x a / h), masses in Msun (/ h), velocities in km/s
        peculiar (x sqrt(a)), density in g/cm^3."""
        units, h = self.snapshot.units, self.snapshot.header.hubble
        a = self.snapshot.scale_factor
        hh = h if units.hubble_units else 1.0
        length = units.length_cm / KPC_CM * a / hh
        mass = units.mass_g / MSUN_G / hh
        vel = units.velocity_cm_s / 1e5 * (np.sqrt(a) if units.comoving else 1.0)
        rho = units.mass_g / units.length_cm ** 3 * hh ** 2 / a ** 3
        table = {"x": (length, "kpc"), "y": (length, "kpc"), "z": (length, "kpc"),
                 "hsml": (length, "kpc"), "vx": (vel, "km/s"), "vy": (vel, "km/s"),
                 "vz": (vel, "km/s"), "mass": (mass, "Msun"), "bh_mass": (mass, "Msun"),
                 "rho": (rho, "g/cm^3"), "u": (units.velocity_cm_s ** 2, "erg/g")}
        return {n: {"factor": float(table[n][0]), "unit": table[n][1]}
                for n in self.names if n in table}

    def describe(self) -> dict:
        return {"type": self.ptype, "name": self.name, "num_rows": self.num_rows,
                "columns": list(self.names), "units": self.units(),
                "physical": self.physical(), "derived": dict(self.derived),
                "mass_from_table": self._mass_from_table,
                **({"mass_table_value": self.snapshot.header.mass[self.ptype]}
                   if self._mass_from_table else {})}


class LazyColumns(Mapping):
    """A component's columns, each built when first read and then kept: a
    point renderer reads three or four of them."""

    def __init__(self, component: Component):
        self._component = component
        self._kept: dict[str, np.ndarray] = {}

    def __getitem__(self, name: str) -> np.ndarray:
        if name not in self._kept:
            if name not in self._component.names:
                raise KeyError(name)
            self._kept[name] = self._component.column(name)
        return self._kept[name]

    def __iter__(self):
        return iter(self._component.names)

    def __len__(self) -> int:
        return len(self._component.names)

    def __contains__(self, name) -> bool:
        return name in self._component.names


# ── References ───────────────────────────────────────────────────────────────

def gadget_ref(first_file: str | Path, ptype: int) -> str:
    return f"{first_file}{REF_SEP}{int(ptype)}"


def split_ref(path: str | Path) -> tuple[str, int | None]:
    text = str(path)
    base, sep, value = text.rpartition(REF_SEP)
    if sep and value.isdigit():
        return base, int(value)
    return text, None


def resolve(path: str | Path) -> Component:
    base, ptype = split_ref(path)
    if ptype is None:
        raise ValueError(f"{path} names no Gadget particle type")
    return open_snapshot(base).component(ptype)


def default_type(snapshot: Snapshot) -> int | None:
    """The type a snapshot opens on: the first with particles (gas first)."""
    return next((t for t, n in enumerate(snapshot.counts()) if n), None)


def gadget_components(path: str | Path) -> list[dict]:
    """Every particle type with particles: ``{name, path, num_rows}``."""
    snap = open_snapshot(split_ref(path)[0])
    return [{"name": TYPE_NAMES[t], "path": gadget_ref(snap.path, t), "num_rows": int(n)}
            for t, n in enumerate(snap.counts()) if n]
