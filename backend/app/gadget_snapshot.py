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
  the size of the block that follows. Its optional INFO block declares dtype,
  dimensions and particle types, so run-specific fields can be read safely.
  Without INFO, unknown blocks are reported but not guessed as integer/float.

A block holds its types one after the other, type 0 first, and only the types
it is for. Known blocks' declared types are checked against their sizes in
every file. When a known block's declared shape does not fit, the consistent
type set can be recovered from all files together; an unknown block without
an INFO declaration is left out and named in :attr:`Snapshot.skipped` because
its numeric dtype cannot be recovered safely. A block of several values per
particle becomes several columns.

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

import math
import os
import re
import struct
import threading
from collections import OrderedDict
from collections.abc import Mapping
from dataclasses import dataclass, field, replace
from functools import lru_cache
from pathlib import Path

import numpy as np

#: ``<first file>`` + REF_SEP + particle type.
REF_SEP = "::gadget-type="
NTYPES = 6
TYPE_NAMES = ("gas", "dark matter", "disk", "bulge", "stars", "black holes")
SUBFIND_TYPE_NAMES = ("FoF groups", "subhalos", "IDs", "large groups", "type 4", "type 5")
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
    if any(n < 0 for n in npart) or num_files < 0:
        raise ValueError("Not a Gadget header: negative particle or file counts.")
    # Initial conditions write 0 files: one file.
    num_files = max(1, num_files)
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
    #: The code that wrote an HDF5 group catalogue ("gadget4", "arepo"),
    #: "" for a snapshot or a binary file.
    producer: str = ""
    #: A Gadget-4 lightcone group catalogue (see _is_lightcone_catalogue).
    lightcone: bool = False


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
            if "Header" not in f:
                return False
            attrs = f["Header"].attrs
            return "NumPart_ThisFile" in attrs or "Ngroups_ThisFile" in attrs
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
        """The 4-byte length marker at ``at``, unsigned: a block of 2 to 4
        GiB has the high bit set."""
        self.fh.seek(at)
        raw = self.fh.read(4)
        if len(raw) != 4:
            raise ValueError(f"{self.path}: truncated at byte {at}")
        return struct.unpack(self.e + "I", raw)[0]

    def _frames(self, at: int) -> bool:
        """Whether a record starts at ``at``: the end of the file, or a
        marker whose closing marker agrees."""
        if at == self.size:
            return True
        try:
            length = self.marker(at)
        except ValueError:
            return False
        return any(True for _ in self._lengths(at, length))

    def _lengths(self, at: int, marker: int):
        """The lengths a record at ``at`` with opening ``marker`` may have:
        the marker, and beyond 4 GiB the marker plus whole multiples of 2^32
        (a 32-bit marker wraps), each with a closing marker equal to it."""
        length = marker
        while at + 8 + length <= self.size:
            if self.marker(at + 4 + length) == marker:
                yield length
            length += 1 << 32

    def record(self, at: int) -> tuple[int, int, int]:
        """``(data offset, length, next record)`` of the record at ``at``.

        A block of 4 GiB or more has a wrapped marker: its true length is the
        one whose closing marker agrees AND after which a record (or the end
        of the file) follows; more than one such length is refused."""
        marker = self.marker(at)
        found = list(self._lengths(at, marker))
        if len(found) > 1:
            found = [n for n in found if self._frames(at + 8 + n)]
        if not found:
            if at + 8 + marker > self.size:
                raise ValueError(f"{self.path}: record at byte {at} claims {marker} bytes, "
                                 f"past the end of the file")
            raise ValueError(f"{self.path}: record at byte {at}: its closing length "
                             f"marker does not match the opening one ({marker})")
        if len(found) > 1:
            raise ValueError(f"{self.path}: record at byte {at}: its length is ambiguous "
                             f"beyond 4 GiB ({', '.join(str(n) for n in found)} bytes)")
        length = found[0]
        return at + 4, length, at + 8 + length


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
                name_raw, size = struct.unpack(e + "4sI", rec.fh.read(8))
                # Padded with spaces, or NULs by some writers.
                name = name_raw.replace(b"\0", b" ").decode("ascii", errors="replace").strip()
                off, length, at = rec.record(at)
                # The announced size is 32 bits too: it wraps as the markers do.
                if size != (length + 8) % (1 << 32):
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
    dims = BLOCKS.get(name, (None, 1))[1]
    if dims is None:                     # several values: any whole number of floats
        return per > 0 and per == int(per) and int(per) % fsize == 0
    return per == fsize * dims


# ── What each block is ───────────────────────────────────────────────────────

#: Known blocks of Gadget-2 and Gadget-3 (and the SubFind group files): their
#: types, values per particle (None: as many as the size says, e.g. one per
#: tracked metal) and column names. Types are "all", "mass" (the types the
#: mass table leaves to the MASS block) or a tuple. A known block is read with
#: its own types; one whose size does not fit them is inferred like an
#: unknown block (and flagged so).
BLOCKS: dict[str, tuple[object, int | None, tuple[str, ...]]] = {
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
    "Z": ((0, 4), None, ("metallicity",)),
    "Zs": ((0, 4), None, ("metals",)),
    "POT": ("all", 1, ("potential",)),
    "ACCE": ("all", 3, ("ax", "ay", "az")),
    "ENDT": ((0,), 1, ("dudt",)),
    "TSTP": ("all", 1, ("timestep",)),
    "BHMA": ((5,), 1, ("bh_mass",)),
    "BHMD": ((5,), 1, ("bh_mdot",)),
    "BHPC": ((5,), 1, ("bh_progenitors",)),
    "ACRB": ((5,), 1, ("bh_accretion_radius",)),
    "iM": ((4,), 1, ("initial_mass",)),
    "HSMS": ((4,), 1, ("hsml_stars",)),
    "TEMP": ((0,), 1, ("temperature_block",)),
    "CLDX": ((0,), 1, ("cold_fraction",)),
    # SubFind group catalogues (issue #392): groups are "type" 0, subhalos 1.
    "GPOS": ((0,), 3, ("x", "y", "z")),
    "MTOT": ((0,), 1, ("group_mass",)),
    "MVIR": ((0,), 1, ("mvir",)),
    "RVIR": ((0,), 1, ("rvir",)),
    "M500": ((0,), 1, ("m500",)),
    "R500": ((0,), 1, ("r500",)),
    "M25K": ((0,), 1, ("m2500",)),
    "R25K": ((0,), 1, ("r2500",)),
    "MGAS": ((0,), 1, ("gas_mass",)),
    "MSTR": ((0,), 1, ("stellar_mass",)),
    "TGAS": ((0,), 1, ("gas_temperature",)),
    "LGAS": ((0,), 1, ("gas_luminosity",)),
    "MCON": ((0,), 1, ("contaminating_mass",)),
    "SPOS": ((1,), 3, ("x", "y", "z")),
    "SVEL": ((1,), 3, ("vx", "vy", "vz")),
    # A group's mass-weighted velocity: in the HDF5 catalogues only, under
    # its HDF5 name (no binary block name has 8 letters).
    "GroupVel": ((0,), 3, ("vx", "vy", "vz")),
    "SCM": ((1,), 3, ("centre_of_mass_x", "centre_of_mass_y", "centre_of_mass_z")),
    "SPIN": ((1,), 3, ("spin_x", "spin_y", "spin_z")),
    "MSUB": ((1,), 1, ("subhalo_mass",)),
    "VMAX": ((1,), 1, ("vmax",)),
    "RMAX": ((1,), 1, ("rmax",)),
    "RHMS": ((1,), 1, ("half_mass_radius",)),
    "SMST": ((1,), 1, ("stellar_mass",)),
    "SSFR": ((1,), 1, ("star_formation_rate",)),
    "SAGE": ((1,), 1, ("stellar_age",)),
    "SZ": ((1,), None, ("metallicity",)),
    "GLEN": ((0,), 1, ("group_length",)),
    "GOFF": ((0,), 1, ("group_offset",)),
    "NSUB": ((0,), 1, ("num_subhalos",)),
    "FSUB": ((0,), 1, ("first_subhalo",)),
    "NCON": ((0,), 1, ("num_contaminated",)),
    "SLEN": ((1,), 1, ("subhalo_length",)),
    "SOFF": ((1,), 1, ("subhalo_offset",)),
    "SSUB": ((1,), 1, ("subhalo_parent",)),
    "MBID": ((1,), 1, ("most_bound_id",)),
    "GRNR": ((1,), 1, ("group_number",)),
}
#: Columns that are positions in the box: periodic in a cosmological run.
PERIODIC_COLUMNS = frozenset({"x", "y", "z", "centre_of_mass_x", "centre_of_mass_y",
                              "centre_of_mass_z"})
#: Blocks of integers (read as unsigned of 4 or 8 bytes).
INT_BLOCKS = {"ID", "BHPC", "GLEN", "GOFF", "SLEN", "SOFF", "SSUB", "NSUB", "FSUB",
              "NCON", "MBID", "GRNR"}
#: An inferred block holds at most this many values per particle: beyond it a
#: set of a few particles (two black holes) would "explain" almost any size.
MAX_DIMS = 32


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


def _element(name: str, types: tuple[int, ...], lengths, fsize: int) -> tuple[str, int] | None:
    """``(dtype, values per particle)`` when block ``name`` holds ``types``
    in every file: in each file of the snapshot, its length is the same
    element size times the particles of ``types`` there, and a file without
    the block has none of them (Gadget writes no empty block). None else."""
    per = None
    for np_, length in lengths:
        count = sum(np_[t] for t in types)
        if count == 0:
            if length:
                return None
            continue
        if length == 0 or length % count:
            return None
        if per is None:
            per = length // count
        elif per != length // count:
            return None
    if not per:
        return None
    if name in INT_BLOCKS:
        return (f"u{per}", 1) if per in (4, 8) else None
    size = fsize if per % fsize == 0 else (4 if per % 4 == 0 else 0)
    if not size or not 1 <= per // size <= MAX_DIMS:
        return None
    return f"f{size}", per // size


def _type_label(types: tuple[int, ...]) -> str:
    return "+".join(TYPE_NAMES[t] for t in types)


def infer_block(name: str, lengths: list[tuple[tuple[int, ...], int]], mass,
                fsize: int) -> tuple[BlockSpec | None, str]:
    """``(spec, why not)`` of block ``name`` from ``(npart of the file, its
    length there)`` for EVERY file of the snapshot (0 where the file has no
    such block): all of them together decide.

    A known block is read with its own types and width. A known block whose
    size does not fit is tried for every set of the types
    present: a set must explain the size in every file with a whole number of
    values per particle, at most MAX_DIMS. Exactly one such reading is taken
    (flagged ``inferred``); none, or several (equal gas and dark-matter counts
    make a gas block of two values and a gas+dark-matter block of one look
    the same), and the block is left out with the reason: a wrong column is
    worse than a missing one."""
    present = tuple(t for t in range(NTYPES) if any(np_[t] for np_, _ in lengths))
    known = BLOCKS.get(name)
    if known is None:
        return None, ("unknown binary block: its bytes do not declare whether values are "
                      "integers or floats; add an explicit block schema before reading it")
    if known is not None:
        types = tuple(t for t in _resolve_types(known[0], None, mass) if t in present)
        element = _element(name, types, lengths, fsize) if types else None
        if element is not None and known[1] in (None, element[1]):
            return BlockSpec(name, types, element[0], element[1], False), ""
    readings = []
    for mask in range(1, 1 << len(present)):
        types = tuple(t for i, t in enumerate(present) if mask >> i & 1)
        element = _element(name, types, lengths, fsize)
        if element is not None:
            readings.append((types, element))
    if len(readings) == 1:
        types, (dtype, dims) = readings[0]
        return BlockSpec(name, types, dtype, dims, True), ""
    if not readings:
        return None, "its size fits no set of particle types"
    options = ", ".join(f"{_type_label(t)} ({d} value{'s' if d > 1 else ''})"
                        for t, (_, d) in readings[:4])
    return None, f"ambiguous: {options}{' ...' if len(readings) > 4 else ''}"


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


#: The groups of a FoF/SubFind HDF5 catalogue (``fof_subhalo_tab_NNN.hdf5``
#: of Gadget-4 and Arepo) -> the "type" the binary catalogues give them.
SUBFIND_HDF5_GROUPS = {"Group": 0, "Subhalo": 1}
#: Their datasets -> the binary SubFind block they are, so both kinds of
#: catalogue have the same columns (and the same framing radii, #441).
#: Any other dataset is a column of its own name.
SUBFIND_HDF5_BLOCKS = {
    "GroupPos": "GPOS", "GroupMass": "MTOT", "GroupLen": "GLEN",
    "GroupNsubs": "NSUB", "GroupFirstSub": "FSUB",
    "Group_M_TopHat200": "MVIR", "Group_R_TopHat200": "RVIR",
    "Group_M_Crit500": "M500", "Group_R_Crit500": "R500",
    "SubhaloPos": "SPOS", "SubhaloVel": "SVEL", "SubhaloCM": "SCM", "SubhaloSpin": "SPIN",
    "SubhaloMass": "MSUB", "SubhaloVmax": "VMAX", "SubhaloVmaxRad": "RMAX",
    "SubhaloHalfmassRad": "RHMS", "SubhaloLen": "SLEN", "SubhaloGroupNr": "GRNR",
    "SubhaloGrNr": "GRNR", "SubhaloIDMostbound": "MBID",
}


def _scan_hdf5(path: str) -> FileLayout:
    import h5py  # noqa: PLC0415
    with h5py.File(path, "r") as f:
        a = f["Header"].attrs
        if "NumPart_ThisFile" not in a and "Ngroups_ThisFile" in a:
            return _scan_hdf5_catalogue(path, f)

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


def _scan_hdf5_catalogue(path: str, f) -> FileLayout:
    """A FoF/SubFind HDF5 catalogue file: groups as type 0 and subhalos as
    type 1, counted by ``Ngroups_*`` and ``Nsubhalos_*`` (Gadget-4) or
    ``Nsubgroups_*`` (Arepo). Gadget-4 keeps the cosmology in
    ``Parameters``, Arepo in the header."""
    import h5py  # noqa: PLC0415
    a = f["Header"].attrs
    params = f["Parameters"].attrs if "Parameters" in f else {}

    def get(key, default=0):
        for attrs in (a, params):
            if key in attrs:
                return np.asarray(attrs[key]).reshape(-1)[0]
        return default
    # Gadget-4 writes Nsubhalos_* (neither count in a FoF-only fof_tab),
    # Arepo Nsubgroups_* and its cosmology in the header.
    gadget4 = "Nsubhalos_ThisFile" in a or (
        "Nsubgroups_ThisFile" not in a and "HubbleParam" not in a)
    sub = "Nsubhalos" if gadget4 else "Nsubgroups"
    npart = (int(get("Ngroups_ThisFile")), int(get(f"{sub}_ThisFile")), 0, 0, 0, 0)
    total = (int(get("Ngroups_Total")), int(get(f"{sub}_Total")), 0, 0, 0, 0)
    time = float(get("Time", 0.0))
    header = Header(npart, (0.0,) * NTYPES, time, float(get("Redshift", 0.0)), 0, 0, total, 0,
                    max(1, int(get("NumFiles", get("NumFilesPerSnapshot", 1)))),
                    float(get("BoxSize", 0.0)), float(get("Omega0", 0.0)),
                    float(get("OmegaLambda", 0.0)), float(get("HubbleParam", 1.0)))
    blocks = []
    for gname, t in SUBFIND_HDF5_GROUPS.items():
        group = f.get(gname)
        if group is None:
            continue
        for dname, dset in group.items():
            if not isinstance(dset, h5py.Dataset) or dset.ndim not in (1, 2):
                continue
            offset = None
            if dset.chunks is None and dset.compression is None:
                offset = dset.id.get_offset()
            blocks.append(H5Block(SUBFIND_HDF5_BLOCKS.get(dname, dname), t, dset.name,
                                  tuple(dset.shape), dset.dtype.str, offset))
    return FileLayout(path, "hdf5", "", 0, header, tuple(blocks),
                      "gadget4" if gadget4 else "arepo",
                      gadget4 and _is_lightcone_catalogue(path, f))


def _is_lightcone_catalogue(path: str, f) -> bool:
    """Whether a Gadget-4 group catalogue was found on the lightcone: its
    header says nothing of it, but Gadget-4 names those ``lc_fof_...`` in
    ``lc_groups_NNN`` (run.cc), and a run that wrote them was compiled with
    LIGHTCONE_PARTICLES_GROUPS (its ``Config``, when written). There the
    particle velocities are already peculiar (subfind_properties.cc,
    fof<lcparticles>) and the positions are around the observer, not in
    the periodic box (fof.cc)."""
    p = Path(path)
    named = p.name.startswith("lc_") or p.parent.name.startswith("lc_groups")
    config = f.get("Config")
    return named and (config is None or "LIGHTCONE_PARTICLES_GROUPS" in config.attrs)


# ── Units ────────────────────────────────────────────────────────────────────

#: ``key=value,...`` with any of length_cm, mass_g, velocity_cm_s, comoving
#: (0/1), hubble_units (0/1), periodic (0/1), svel and srad (SubFind
#: conventions, see UnitSystem): overrides the snapshot's units, for runs
#: whose parameter file is not beside them.
UNITS_ENV = "VISIVO_GADGET_UNITS"
#: The SubFind subhalo velocity conventions: the power of a that makes the
#: stored value peculiar, and how the stored value is described.
SUBFIND_VEL_FACTORS = {"peculiar": (0.0, "peculiar (SubFind convention)"),
                       "snapshot": (0.5, "internal, x sqrt(a)"),
                       "canonical": (-1.0, "a^2 dx/dt, x 1/a"),
                       "comoving": (-1.0, "a^2 dx/dt, x 1/a")}
#: Blocks of SubFind subhalo radii, whose frame (comoving or physical)
#: depends on the writer (Component._subhalo_radii_physical).
SUBHALO_RADIUS_BLOCKS = frozenset({"RHMS", "RMAX", "SubhaloHalfmassRadType"})
_PARAM_FILES = ("parameters-usedvalues", "parameters-usedValues", "param.txt-usedvalues",
                "params.txt-usedvalues", "parameters.txt-usedvalues")


@dataclass(frozen=True)
class UnitSystem:
    """The code units of a snapshot and what makes them physical."""
    length_cm: float = KPC_CM
    mass_g: float = 1.0e10 * MSUN_G
    velocity_cm_s: float = 1.0e5
    #: Lengths and densities comoving, velocities with the internal √a. None
    #: until stated (a parameter file, an HDF5 file, the environment) or
    #: judged from the header (:func:`looks_comoving`).
    comoving: bool | None = None
    #: Lengths and masses in units of 1/h.
    hubble_units: bool = True
    source: str = "Gadget defaults"
    #: The box is periodic: a Gadget-4 HDF5 file's ``Config/PERIODIC``, or
    #: ``periodic=`` in VISIVO_GADGET_UNITS; None when nothing states it
    #: (then a comoving run is taken as periodic, see Component.periods).
    periodic: bool | None = None
    #: How a SubFind catalogue stores subhalo velocities (SVEL, SubhaloVel):
    #: "peculiar" (x 1; what Gadget-4 and Arepo write, and the default for
    #: P-Gadget3 files, whose writer is not public), "snapshot" (with the
    #: particles' internal sqrt(a)) or "comoving" (a^2 dx/dt, x 1/a).
    #: ``svel=`` in VISIVO_GADGET_UNITS.
    subfind_vel: str = "peculiar"
    #: Whether subhalo half-mass and Vmax radii are "comoving" or "physical";
    #: None: as their writer stores them (physical for Gadget-4 HDF5,
    #: comoving for Arepo and the binary catalogues). ``srad=``.
    subfind_radii: str | None = None

    def describe(self) -> dict:
        return {"length_cm": self.length_cm, "mass_g": self.mass_g,
                "velocity_cm_s": self.velocity_cm_s, "comoving": self.comoving,
                "hubble_units": self.hubble_units, "source": self.source,
                "subfind_vel": self.subfind_vel, "subfind_radii": self.subfind_radii,
                "length": _unit_name(self.length_cm, {"kpc": KPC_CM, "Mpc": 1e3 * KPC_CM,
                                                      "pc": 1e-3 * KPC_CM}, "cm"),
                "mass": _unit_name(self.mass_g, {"1e10 Msun": 1e10 * MSUN_G,
                                                 "Msun": MSUN_G}, "g"),
                "velocity": _unit_name(self.velocity_cm_s, {"km/s": 1e5}, "cm/s"),
                # Whether the box wraps, by Component.periods' rule (stated,
                # else a comoving run is): with a positive box size, the
                # selections and neighbour searches wrap round it.
                "periodic": self.periodic if self.periodic is not None
                else bool(self.comoving)}


def _unit_name(value: float, names: dict, base: str) -> str:
    for name, v in names.items():
        if abs(value / v - 1.0) < 1e-3:
            return name
    return f"{value:.6g} {base}"


def _parameter_candidate(first_file: str | Path) -> Path | None:
    """The first parameter file beside the snapshot or one folder up,
    wherever it lies."""
    folder = Path(first_file).parent
    for where in (folder, folder.parent):
        for name in _PARAM_FILES:
            p = where / name
            if p.is_file():
                return p
    return None


def _within_env_roots(path: Path) -> bool:
    """Whether ``path`` (resolved, so a symbolic link counts where it leads)
    lies in VISIVO_DATA_ROOTS; True when no roots are set. The same rule as
    the backend's own path check, without importing it (render and pyramid
    processes read snapshots without the rest of the backend)."""
    raw = os.environ.get("VISIVO_DATA_ROOTS", "").strip()
    roots = [Path(part).expanduser().resolve() for part in raw.split(os.pathsep)
             if part.strip()]
    if not roots:
        return True
    try:
        resolved = path.resolve()
    except OSError:
        return False
    return any(resolved == root or root in resolved.parents for root in roots)


def parameter_file(first_file: str | Path) -> Path | None:
    """The run's parameter file Gadget writes (``parameters-usedvalues`` ...)
    beside the snapshot or one folder up, or None. One outside the data
    roots (the folder above a root, a symbolic link out of it) counts as
    absent: the snapshot opens with the default units and says so
    (:func:`units_for`), rather than being refused for a file it only
    reads units from."""
    p = _parameter_candidate(first_file)
    return p if p is not None and _within_env_roots(p) else None


def looks_comoving(header: Header) -> bool:
    """Whether a header is a cosmological run's: a matter density, a
    positive scale factor, and a redshift that is 1/a - 1. An isolated
    galaxy's header has a time instead (0, 2.5 ...) and no such redshift."""
    a, z = header.time, header.redshift
    if header.omega0 <= 0 or not 0 < a <= 1.0 + 1e-6:
        return False
    return abs(z - (1.0 / a - 1.0)) <= 1e-3 * max(1.0, abs(z))


def units_for(first_file: str | Path, hdf5_units: dict | None = None,
              header: Header | None = None) -> UnitSystem:
    """The snapshot's units: the defaults, then the run's parameter file
    (:func:`parameter_file`), an HDF5 file's own units, then the environment
    (:data:`UNITS_ENV`). Whether the run is comoving, when none of them
    says, is judged from the header (:func:`looks_comoving`)."""
    units = UnitSystem()
    params = parameter_file(first_file)
    if params is not None:
        units = _from_params(params, units)
    elif _parameter_candidate(first_file) is not None:
        units = replace(units, source=units.source + " (the run's parameter file lies "
                        "outside the folders this backend may read and was not used)")
    if hdf5_units:
        units = replace(units, source="the HDF5 file's units", **hdf5_units)
    env = os.environ.get(UNITS_ENV, "").strip()
    if env:
        units = _from_env(env, units)
    if units.comoving is None:
        comoving = header is not None and looks_comoving(header)
        units = replace(units, comoving=comoving,
                        source=units.source + ("; comoving (cosmological header)" if comoving
                                               else "; not comoving (no cosmological header)"))
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
        elif key in ("comoving", "hubble_units", "periodic"):
            kw[key] = value.lower() not in ("0", "false", "no", "")
        elif key == "svel":
            if value.lower() not in SUBFIND_VEL_FACTORS:
                raise ValueError(f"{UNITS_ENV}: svel={value!r}: one of "
                                 + ", ".join(SUBFIND_VEL_FACTORS))
            kw["subfind_vel"] = value.lower()
        elif key == "srad":
            if value.lower() not in ("comoving", "physical"):
                raise ValueError(f"{UNITS_ENV}: srad={value!r}: comoving or physical")
            kw["subfind_radii"] = value.lower()
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

    @property
    def nbytes(self) -> int:
        return self.rows * self.dims * np.dtype(self.dtype).itemsize


# ── Selected rows of a chunked or compressed HDF5 dataset ────────────────────
#
# Such a dataset cannot be memory-mapped: any read decompresses whole HDF5
# chunks. Component.take is called a slice of rows at a time (a filter walks
# the table in chunks, a sample reads x, y and z in pieces), so reading the
# whole dataset per call decompressed a shard dozens of times (issue #445).
# A dataset up to the cache budget is decompressed once and kept, shared by
# every component and bounded in bytes; a larger one is read in windows of
# whole HDF5 chunks spanning only the rows asked for.

#: Megabytes of decompressed HDF5 pieces kept for row reads (0: none).
PIECE_CACHE_ENV = "VISIVO_GADGET_CACHE_MB"
PIECE_CACHE_DEFAULT_MB = 512
#: At most this many bytes are decompressed into one window of a dataset
#: larger than the cache budget (rounded to whole HDF5 chunks).
WINDOW_BYTES = 64 << 20


def piece_cache_bytes() -> int:
    """The cache budget, from the environment when it is a finite number of
    megabytes (0 or more); the default otherwise (nan, inf, text)."""
    try:
        mb = float(os.environ.get(PIECE_CACHE_ENV, PIECE_CACHE_DEFAULT_MB))
        if not math.isfinite(mb) or mb < 0:
            raise ValueError(mb)
        return int(mb * (1 << 20))
    except (ValueError, OverflowError):
        return int(PIECE_CACHE_DEFAULT_MB * (1 << 20))


class _PieceCache:
    """Least recently used decompressed pieces, at most the budget in bytes,
    keyed by the file's identity (a rewritten file is not served stale).

    Concurrent misses on one piece decompress it once: the first reader
    holds that key's lock while it reads, the others wait on it and then
    find the entry (other pieces are read meanwhile)."""

    def __init__(self):
        self._lock = threading.Lock()
        self._items: OrderedDict = OrderedDict()
        self._bytes = 0
        self._loading: dict = {}            # key -> (lock, waiters)

    def get(self, piece: Piece) -> np.ndarray | None:
        """The piece's (rows, dims) values, or None when it exceeds the budget."""
        budget = piece_cache_bytes()
        if piece.nbytes > budget:
            return None
        st = os.stat(piece.path)
        key = (piece.path, piece.dataset, st.st_size, st.st_mtime_ns)
        with self._lock:
            hit = self._items.get(key)
            if hit is not None:
                self._items.move_to_end(key)
                return hit
            entry = self._loading.setdefault(key, [threading.Lock(), 0])
            entry[1] += 1
        try:
            with entry[0]:                  # one reader per piece at a time
                with self._lock:
                    hit = self._items.get(key)
                    if hit is not None:     # read by the reader before us
                        self._items.move_to_end(key)
                        return hit
                values = piece.array()      # outside the cache lock: seconds
                with self._lock:
                    self._items[key] = values
                    self._bytes += values.nbytes
                    while self._bytes > budget and len(self._items) > 1:
                        _, old = self._items.popitem(last=False)
                        self._bytes -= old.nbytes
                return values
        finally:
            with self._lock:
                entry[1] -= 1
                if entry[1] == 0:
                    self._loading.pop(key, None)

    def drop(self, paths) -> int:
        """Forget the pieces read from ``paths`` (a closed dataset's files);
        the number dropped."""
        wanted = {str(p) for p in paths}
        with self._lock:
            keys = [k for k in self._items if k[0] in wanted]
            for k in keys:
                self._bytes -= self._items.pop(k).nbytes
        return len(keys)

    def clear(self):
        with self._lock:
            self._items.clear()
            self._bytes = 0


PIECE_CACHE = _PieceCache()


def _read_window(dataset, start: int, stop: int, column: int) -> np.ndarray:
    """Rows ``start:stop`` of one column of an HDF5 dataset (1-D or 2-D)."""
    if dataset.ndim == 1:
        return dataset[start:stop]
    return dataset[start:stop, column]


def _read_rows_windowed(piece: Piece, local: np.ndarray, column: int) -> np.ndarray:
    """``column`` at rows ``local`` of an HDF5 piece too large for the cache:
    the rows asked for, sorted, are read window by window (whole HDF5 chunks,
    at most WINDOW_BYTES), each window from its first to its last row asked
    for, so a contiguous slice reads only its own chunks and memory stays at
    one window."""
    import h5py  # noqa: PLC0415
    unique, inverse = np.unique(local, return_inverse=True)
    out = np.empty(unique.size, dtype=np.dtype(piece.dtype))
    with h5py.File(piece.path, "r") as f:
        dataset = f[piece.dataset]
        chunk = dataset.chunks[0] if dataset.chunks else 1
        per_row = np.dtype(piece.dtype).itemsize * (piece.dims if dataset.ndim > 1 else 1)
        window = max(chunk, (WINDOW_BYTES // max(1, per_row)) // chunk * chunk)
        i = 0
        while i < unique.size:
            first = int(unique[i])
            end = min(piece.rows, (first // window + 1) * window)
            j = int(np.searchsorted(unique, end, side="left"))
            last = int(unique[j - 1]) + 1
            out[i:j] = _read_window(dataset, first, last, column)[unique[i:j] - first]
            i = j
    return out[inverse]


@dataclass(frozen=True)
class Snapshot:
    files: tuple[FileLayout, ...]
    header: Header                  # the first file's, npart = the totals
    blocks: dict                    # name -> BlockSpec
    pieces: dict                    # (name, type) -> tuple[Piece, ...]
    units: UnitSystem
    skipped: tuple[str, ...] = ()   # blocks left out: no single reading
    fmt: str = ""
    skip_reasons: tuple = ()        # (name, why) of each block left out
    # HDF5 permits the same dataset name to have a different shape or dtype in
    # different PartType groups. Binary files have one spec for every type.
    component_specs: dict = field(default_factory=dict)  # (name, type) -> BlockSpec
    #: The code that wrote an HDF5 group catalogue ("gadget4", "arepo"), "".
    producer: str = ""
    #: A Gadget-4 lightcone group catalogue: velocities already peculiar,
    #: each object at its own scale factor.
    lightcone: bool = False

    @property
    def path(self) -> str:
        return self.files[0].path

    @property
    def scale_factor(self) -> float:
        """a: the header's time for a comoving run (1 when it is not a
        positive number), 1 for any other run."""
        t = self.header.time
        return t if self.units.comoving and t > 0 else 1.0

    @property
    def hubble(self) -> float:
        """h for the /h units: the header's, 1 when the run has none."""
        h = self.header.hubble
        return h if self.units.hubble_units and h > 0 else 1.0

    def counts(self) -> tuple[int, ...]:
        return self.header.npart_total

    def component(self, ptype: int) -> "Component":
        return Component(self, int(ptype))

    def describe(self) -> dict:
        h = self.header
        return {"format": self.fmt, "files": len(self.files),
                "catalogue_kind": "subfind" if self.is_subfind else "particles",
                **({"lightcone": True} if self.lightcone else {}),
                "time": h.time, "scale_factor": self.scale_factor,
                "redshift": h.redshift, "box_size": h.box_size, "omega0": h.omega0,
                "omega_lambda": h.omega_lambda, "hubble_param": h.hubble,
                "mass_table": list(h.mass), "npart_total": list(h.npart_total),
                "units": self.units.describe(),
                "blocks": {name: {"types": list(b.types), "dims": b.dims, "dtype": b.dtype,
                                  "inferred": b.inferred,
                                  **({"by_type": {
                                      str(t): {"dims": self.component_specs[(name, t)].dims,
                                               "dtype": self.component_specs[(name, t)].dtype}
                                      for t in b.types if (name, t) in self.component_specs}}
                                     if self.component_specs else {})}
                           for name, b in self.blocks.items()},
                "skipped_blocks": list(self.skipped),
                "skipped_reasons": dict(self.skip_reasons)}

    @property
    def is_subfind(self) -> bool:
        """Whether this Gadget-family file is a FoF/SubFind group catalogue."""
        return bool(self.producer) or "GPOS" in self.blocks or "SPOS" in self.blocks


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
    header = read_header(p)
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


#: Names probed in a folder, in this order: the first file of a multi-file
#: snapshot or catalogue, a snapshot, a single-file group catalogue
#: (``fof_subhalo_tab_NNN.hdf5``, ``fof_tab_NNN``, ``lc_fof_...``, ``sub_NNN``).
_CATALOGUE_NAME = r"^(lc_)?fof_|^sub_|^subhalo_tab|^group_tab"
_DIR_PATTERNS = (r"\.0(\.hdf5|\.h5)?$", r"^snap", _CATALOGUE_NAME)
#: A later file of a multi-file set (``.1``, ``.7.hdf5``): its ``.0`` file
#: stands for it when the folder holds that.
_LATER_FILE = re.compile(r"\.[1-9]\d*(\.hdf5|\.h5)?$")
#: The output number of a snapshot or catalogue name (``snapshot_004.hdf5``,
#: ``fof_subhalo_tab_004.0.hdf5``, ``sub_002.0``).
_OUTPUT_NUMBER = re.compile(r"_(\d{3,})(\.\d+)?(\.hdf5|\.h5)?$")


def is_companion_name(name: str) -> bool:
    """Whether a file is the most-bound-only companion Gadget-4 writes
    beside a snapshot with SUBFIND_ORPHAN_TREATMENT
    (``<base>-prevmostboundonly_NNN``, snap_io.cc): not the snapshot."""
    return "-prevmostboundonly" in name


def _later_file(name: str, names) -> bool:
    """Whether ``name`` is a later file of a set whose ``.0`` the folder
    holds (a single-file ``snapshot.100`` is not)."""
    m = _LATER_FILE.search(name)
    return m is not None and f"{name[:m.start()]}.0{m.group(1) or ''}" in names


def is_group_catalogue(path: str | Path, found=False) -> bool:
    """Whether a Gadget-family file is a FoF/SubFind group catalogue: an HDF5
    file counting groups (``Ngroups_ThisFile``), a binary one with GPOS or
    SPOS blocks. Only the header (binary: the block markers) is read.
    ``found``: :func:`detect`'s answer when the caller has it."""
    found = detect(path) if found is False else found
    if found is None:
        return False
    try:
        if found[0] == 0:
            import h5py  # noqa: PLC0415
            with h5py.File(path, "r") as f:
                return "Ngroups_ThisFile" in f["Header"].attrs
        return any(b.name in ("GPOS", "SPOS") for b in scan_file(path).blocks)
    except (OSError, ValueError, KeyError):
        return False


def _gadget_files_in_dir(folder: Path, allowed=None):
    """The Gadget files of a folder (see _DIR_PATTERNS), each once, with
    whether it is a group catalogue; at most MAX_PROBES files are read."""
    try:
        with os.scandir(folder) as it:
            # Do not follow a directory child's symlink before the caller's
            # allow-list has had a chance to reject its resolved target.
            names = sorted(e.name for e in it
                           if e.is_file(follow_symlinks=False) or e.is_symlink())
    except OSError:
        return
    probes, seen, present = 0, set(), set(names)
    for pattern in _DIR_PATTERNS:
        for name in names:
            if (name in seen or not re.search(pattern, name) or is_companion_name(name)
                    or _later_file(name, present)):
                continue
            seen.add(name)
            probes += 1
            if probes > MAX_PROBES:
                return
            candidate = folder / name
            if allowed is not None and not allowed(str(candidate)):
                continue
            found = detect(candidate) if candidate.is_file() else None
            if found is not None:
                yield candidate, is_group_catalogue(candidate, found)


def first_in_dir(folder: Path, allowed=None) -> Path | None:
    """The snapshot a folder (a ``snapdir``) holds: its first ``<name>.0``
    (or ``.0.hdf5``) that is a Gadget file, else its first ``snap*`` file
    that is one, else a single-file group catalogue. Particles come first:
    a folder holding a snapshot and its group catalogue opens the snapshot
    (:func:`catalogue_in_dir` names the catalogue). Only names are listed;
    at most MAX_PROBES files are read."""
    catalogue = None
    for candidate, is_catalogue in _gadget_files_in_dir(folder, allowed):
        if not is_catalogue:
            return candidate
        catalogue = catalogue or candidate
    return catalogue


def catalogue_in_dir(folder: Path, snapshot: str | Path, allowed=None) -> Path | None:
    """The group catalogue of the same output as ``snapshot`` (a particle
    snapshot file) in ``folder``, or None. Only names of that output number
    are probed."""
    if is_group_catalogue(snapshot):
        return None
    m = _OUTPUT_NUMBER.search(Path(snapshot).name)
    if m is None:
        return None
    try:
        with os.scandir(folder) as it:
            names = sorted(e.name for e in it
                           if e.is_file(follow_symlinks=False) or e.is_symlink())
    except OSError:
        return None
    present = set(names)
    for name in names:
        number = _OUTPUT_NUMBER.search(name)
        if (number is None or number.group(1) != m.group(1)
                or not re.search(_CATALOGUE_NAME, name) or _later_file(name, present)):
            continue
        candidate = folder / name
        if allowed is not None and not allowed(str(candidate)):
            continue
        if is_gadget(candidate) and is_group_catalogue(candidate):
            return candidate
    return None


def open_snapshot(path: str | Path) -> Snapshot:
    """The snapshot ``path`` belongs to, scanned (headers and block markers
    only: no data is read). Cached while its files, its parameter file and
    the units in the environment are unchanged."""
    files = snapshot_files(path)
    key = tuple((str(f.resolve()), *_stat(f)) for f in files)
    params = parameter_file(files[0])
    units_key = (str(params), *_stat(params)) if params is not None else ()
    return _cached_snapshot(key, units_key, os.environ.get(UNITS_ENV, ""))


def _stat(f: Path) -> tuple[int, int]:
    st = f.stat()
    return st.st_size, st.st_mtime_ns


@lru_cache(maxsize=16)
def _cached_snapshot(key, _units_key=(), _units_env="") -> Snapshot:
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
    info_schemas = [_format2_info(f) for f in layouts]
    declared, info_skips = {}, {}
    for schema in info_schemas:
        for name, candidate in schema.items():
            if isinstance(candidate, str):
                info_skips.setdefault(name, candidate)
                continue
            previous = declared.get(name)
            if previous is not None and (candidate.dtype, candidate.dims) != (
                    previous.dtype, previous.dims):
                raise ValueError(f"The snapshot files declare different INFO schemas for {name}")
            types = tuple(sorted(set((previous.types if previous else ()) + candidate.types)))
            declared[name] = replace(candidate, types=types)
    names: list[str] = []
    for f in layouts:
        for b in f.blocks:
            if b.name not in names:
                names.append(b.name)
    specs, pieces, skipped = {}, {}, []
    for name in names:
        if name == "INFO":
            continue
        # Every file decides, those without the block too (they must have
        # none of its particles).
        lengths = []
        for f in layouts:
            blk = [b for b in f.blocks if b.name == name]
            lengths.append((f.header.npart, sum(b.nbytes for b in blk)))
        spec = declared.get(name)
        why = info_skips.get(name, "")
        if spec is not None:
            spec, why = _info_block(spec, lengths, fsize)
        elif not why:
            spec, why = infer_block(name, lengths, first.mass, fsize)
        if spec is None:
            skipped.append((name, why))
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
    return Snapshot(layouts, header, specs, pieces, units_for(layouts[0].path, None, header),
                    tuple(n for n, _ in skipped), fmt, tuple(skipped))


#: INFO value types -> numpy kind and, when the type fixes it, the bytes per
#: value. FLOAT/FLOATN leave the size open: OpenGadget3 and P-Gadget3 write
#: "FLOAT" for a block of the run's MyFloat, single even when the positions
#: are double (DOUBLEN), so the block's own length decides.
_INFO_TYPES = {"FLOAT": ("f", None), "FLOATN": ("f", None),
               "DOUBLE": ("f", 8), "DOUBLEN": ("f", 8),
               "LLONG": ("u", 8), "ULONG64": ("u", 8),
               "LONG": ("u", 4), "ULONG": ("u", 4), "INT": ("u", 4), "UINT": ("u", 4)}


def _format2_info(layout: FileLayout) -> dict[str, BlockSpec | str]:
    """Read the optional format-2 INFO schema without touching block data.

    Each entry is a BlockSpec, or the reason its block is left out (a value
    type this reader does not know, no particle types, a width beyond
    MAX_DIMS): one odd entry costs its block, not the snapshot. A FLOAT
    entry's dtype is ``"f"``, its size settled by :func:`_info_block`."""
    info = next((block for block in layout.blocks if block.name == "INFO"), None)
    if info is None:
        return {}
    if info.nbytes % 40:
        raise ValueError(f"{Path(layout.path).name}: INFO has {info.nbytes} bytes, not 40-byte entries")
    with open(layout.path, "rb") as handle:
        handle.seek(info.offset)
        raw = handle.read(info.nbytes)
    out: dict[str, BlockSpec | str] = {}
    for offset in range(0, len(raw), 40):
        entry = raw[offset:offset + 40]
        name = entry[:4].decode("ascii", "replace").rstrip(" \0")
        value_type = entry[4:12].decode("ascii", "replace").rstrip(" \0").upper()
        dims, *flags = struct.unpack(f"{layout.endian}i6i", entry[12:])
        if not name:
            raise ValueError(f"{Path(layout.path).name}: INFO entry without a block name")
        kind = _INFO_TYPES.get(value_type)
        if kind is None:
            out[name] = f"INFO declares value type {value_type!r}, which this reader does not know"
            continue
        if not 1 <= dims <= MAX_DIMS:
            out[name] = f"INFO declares {dims} values per particle (1..{MAX_DIMS} supported)"
            continue
        types = tuple(index for index, active in enumerate(flags) if active)
        if name == "MASS":
            types = tuple(index for index in types if layout.header.mass[index] == 0)
            if not types:
                continue
        if not types:
            out[name] = "INFO declares no particle types for it"
            continue
        dtype = f"{kind[0]}{kind[1]}" if kind[1] else kind[0]
        out[name] = BlockSpec(name, types, dtype, dims, False)
    return out


def _info_block(spec: BlockSpec, lengths, fsize: int) -> tuple[BlockSpec | None, str]:
    """``(spec, why not)`` of a block INFO declares, checked against its
    length in every file. A FLOAT block takes 4 or 8 bytes a value from the
    length (the same in every file; the positions' size when no file holds
    its particles); a declared size the length contradicts leaves the block
    out with the reason, rather than read it with the wrong stride."""
    counts = [(sum(npart[t] for t in spec.types) * spec.dims, length)
              for npart, length in lengths]
    dtype = spec.dtype
    if dtype == "f":
        sizes = {length // values for values, length in counts
                 if values and length % values == 0}
        if not any(values for values, _ in counts):
            sizes = {fsize}
        if len(sizes) != 1 or next(iter(sizes)) not in (4, 8):
            found = ", ".join(f"{length} bytes for {values} values"
                              for values, length in counts if values)
            return None, (f"INFO declares FLOAT x {spec.dims} for types {list(spec.types)}; "
                          f"the block has {found}: neither float nor double")
        dtype = f"f{next(iter(sizes))}"
    itemsize = np.dtype(dtype).itemsize
    for values, length in counts:
        if length != values * itemsize:
            return None, (f"INFO declares {dtype} x {spec.dims} for types {list(spec.types)}, "
                          f"requiring {values * itemsize} bytes; the block has {length}")
    return replace(spec, dtype=dtype), ""


def _assemble_hdf5(layouts, header: Header) -> Snapshot:
    specs, component_specs, pieces = {}, {}, {}
    by_file = []
    for f in layouts:
        current = {}
        for b in f.blocks:
            rows = f.header.npart[b.ptype]
            if b.shape[0] != rows:
                raise ValueError(f"{Path(f.path).name}: {b.dataset} has {b.shape[0]} rows; "
                                 f"the header announces {rows} PartType{b.ptype} particles")
            dims = b.shape[1] if len(b.shape) == 2 else 1
            kind = np.dtype(b.dtype)
            key = (b.name, b.ptype)
            if key in current:
                raise ValueError(f"{Path(f.path).name}: more than one dataset maps to "
                                 f"{b.name} for PartType{b.ptype}")
            current[key] = b
            previous = component_specs.get(key)
            dtype = f"{kind.kind}{kind.itemsize}"
            if previous is not None and (previous.dims != dims or previous.dtype != dtype):
                raise ValueError(
                    f"{Path(f.path).name}: {b.dataset} has shape (*, {dims}) and dtype "
                    f"{dtype}; earlier files use (*, {previous.dims}) and {previous.dtype}")
            component_specs[key] = BlockSpec(b.name, (b.ptype,), dtype, dims, False)
            spec = specs.get(b.name)
            types = tuple(sorted(set((spec.types if spec else ()) + (b.ptype,))))
            # The aggregate spec is descriptive only; Component uses the
            # per-PartType spec above so heterogeneous groups cannot corrupt
            # their column layout.
            specs[b.name] = BlockSpec(b.name, types, spec.dtype if spec else dtype,
                                      spec.dims if spec else dims, False)
            pieces.setdefault((b.name, b.ptype), []).append(
                Piece(f.path, b.offset, b.dtype, b.shape[0], dims, b.dataset))
        by_file.append(current)
    all_keys = set().union(*(set(found) for found in by_file))
    for name, ptype in sorted(all_keys):
        for f, found in zip(layouts, by_file):
            if f.header.npart[ptype] and (name, ptype) not in found:
                raise ValueError(f"{Path(f.path).name}: PartType{ptype} is missing {name}, "
                                 "which is present for that particle type in another file")
    pieces = {k: tuple(v) for k, v in pieces.items()}
    producer = layouts[0].producer
    if any(f.producer != producer for f in layouts):
        raise ValueError("The files mix particle snapshots and group catalogues")
    lightcone = layouts[0].lightcone
    fmt = {"gadget4": "Gadget-4 SubFind HDF5", "arepo": "Arepo SubFind HDF5"}.get(
        producer, "Gadget HDF5") + (", lightcone" if lightcone else "")
    return Snapshot(layouts, header, specs, pieces,
                    units_for(layouts[0].path, _hdf5_units(layouts[0].path), header), (),
                    fmt, component_specs=component_specs, producer=producer,
                    lightcone=lightcone)


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
            # Gadget-4 lists its compile-time options as attributes of
            # Config: PERIODIC among them or not says whether the box is.
            if "Config" in f:
                out["periodic"] = "PERIODIC" in f["Config"].attrs
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
        # Each block's pieces as arrays, kept while the component lives: a
        # memory map costs nothing, and a compressed HDF5 dataset is read
        # once for x, y and z, not three times.
        self._arrays: dict[str, list[np.ndarray]] = {}
        self._sources: dict[str, tuple[str, int]] = {}
        for name, spec in snapshot.blocks.items():
            spec = snapshot.component_specs.get((name, ptype), spec)
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
        names = SUBFIND_TYPE_NAMES if self.snapshot.is_subfind else TYPE_NAMES
        return names[self.ptype]

    def column(self, name: str) -> np.ndarray:
        if name in self._sources:
            block, k = self._sources[name]
            arrays = self._arrays.get(block)
            if arrays is None:
                arrays = self._arrays[block] = [
                    p.array() for p in self.snapshot.pieces[(block, self.ptype)]]
            parts = [a[:, k] for a in arrays]
            out = parts[0] if len(parts) == 1 else np.concatenate(parts)
            scale = self._read_scale(name)
            return out if scale == 1.0 else (out * scale).astype(out.dtype)
        if name == "mass" and self._mass_from_table:
            spec = self.snapshot.blocks.get("POS")
            dtype = spec.dtype if spec is not None else "f4"
            return np.full(self.num_rows, self.snapshot.header.mass[self.ptype], dtype=dtype)
        # Derived columns: computed in float64 when asked for, kept in
        # float32 (seven significant digits, ample for a temperature).
        if name == "temperature" and name in self.derived:
            return self.temperature().astype(np.float32)
        if name == "entropy" and name in self.derived:
            return self.entropy().astype(np.float32)
        raise KeyError(name)

    def column_dtype(self, name: str) -> np.dtype:
        """The dtype of a column without reading any particle values."""
        if name in self._sources:
            block, _ = self._sources[name]
            pieces = self.snapshot.pieces[(block, self.ptype)]
            return np.dtype(pieces[0].dtype).newbyteorder("=")
        if name == "mass" and self._mass_from_table:
            spec = self.snapshot.blocks.get("POS")
            return np.dtype(spec.dtype if spec is not None else "f4")
        if name in self.derived:
            return np.dtype("f4")
        raise KeyError(name)

    def take(self, name: str, rows) -> np.ndarray:
        """Read only selected rows across all snapshot files."""
        rows = np.asarray(rows, dtype=np.int64)
        scalar = rows.ndim == 0
        rows = rows.reshape(-1)
        if rows.size and (rows.min() < 0 or rows.max() >= self.num_rows):
            raise IndexError("particle row outside the component")
        if name in self._sources:
            block, column = self._sources[name]
            pieces = self.snapshot.pieces[(block, self.ptype)]
            ends = np.cumsum([piece.rows for piece in pieces], dtype=np.int64)
            which = np.searchsorted(ends, rows, side="right")
            out = np.empty(rows.size, dtype=self.column_dtype(name))
            for index in np.unique(which):
                selected = which == index
                start = 0 if index == 0 else ends[index - 1]
                out[selected] = self._piece_rows(block, int(index), rows[selected] - start,
                                                 column)
        elif name == "mass" and self._mass_from_table:
            out = np.full(rows.size, self.snapshot.header.mass[self.ptype],
                          dtype=self.column_dtype(name))
        elif name == "temperature" and name in self.derived:
            out = self._temperature_rows(rows).astype(np.float32)
        elif name == "entropy" and name in self.derived:
            temperature = self._temperature_rows(rows)
            density = self._physical_density_rows(rows)
            x = HYDROGEN_MASS_FRACTION
            if "ne" in self._sources:
                electrons = (np.asarray(self.take("ne", rows), dtype=np.float64)
                             * x * density / PROTON_MASS_G)
            else:
                electrons = 0.5 * (1.0 + x) * density / PROTON_MASS_G
            out = (BOLTZMANN_ERG_K * temperature / KEV_ERG
                   / electrons ** (2.0 / 3.0)).astype(np.float32)
        else:
            raise KeyError(name)
        scale = self._read_scale(name)
        if scale != 1.0:
            out = (out * scale).astype(out.dtype)
        return out[0] if scalar else out

    def _piece_rows(self, block: str, index: int, local: np.ndarray, column: int) -> np.ndarray:
        """``column`` of piece ``index`` of ``block`` at its rows ``local``:
        from the memory map, from the arrays column() already read, from the
        shared cache of decompressed HDF5 pieces, or window by window for an
        HDF5 piece larger than that cache."""
        piece = self.snapshot.pieces[(block, self.ptype)][index]
        if piece.offset is not None or piece.rows == 0:
            return piece.array()[local, column]
        arrays = self._arrays.get(block)
        if arrays is not None:
            return arrays[index][local, column]
        values = PIECE_CACHE.get(piece)
        if values is not None:
            return values[local, column]
        return _read_rows_windowed(piece, local, column)

    def _temperature_rows(self, rows: np.ndarray) -> np.ndarray:
        u = np.asarray(self.take("u", rows), dtype=np.float64)
        if self.snapshot.header.flag_entropy_instead_u:
            rho = np.asarray(self.take("rho", rows), dtype=np.float64)
            rho /= self.snapshot.scale_factor ** 3
            u = u * rho ** (GAMMA - 1.0) / (GAMMA - 1.0)
        u *= self.snapshot.units.velocity_cm_s ** 2
        if "ne" in self._sources:
            ne = np.asarray(self.take("ne", rows), dtype=np.float64)
            x = HYDROGEN_MASS_FRACTION
            mu = 4.0 / (1.0 + 3.0 * x + 4.0 * x * ne)
        else:
            mu = 4.0 / (3.0 + 5.0 * HYDROGEN_MASS_FRACTION)
        return (GAMMA - 1.0) * mu * PROTON_MASS_G / BOLTZMANN_ERG_K * u

    def _physical_density_rows(self, rows: np.ndarray) -> np.ndarray:
        units, h = self.snapshot.units, self.snapshot.hubble
        factor = units.mass_g / units.length_cm ** 3 * h ** 2
        return (np.asarray(self.take("rho", rows), dtype=np.float64) * factor
                / self.snapshot.scale_factor ** 3)

    def columns(self) -> "LazyColumns":
        return LazyColumns(self)

    def periods(self) -> dict[str, float]:
        """The period of each coordinate column: the BoxSize (positive) of a
        periodic run, {} for any other. Periodic is what the run states (a
        Gadget-4 HDF5 ``Config/PERIODIC``, ``periodic=`` in
        VISIVO_GADGET_UNITS); when nothing states it, a comoving run is taken
        as periodic and any other as not. That guess is wrong for a
        non-periodic cosmological zoom and for an isolated run integrated in
        comoving coordinates: state ``periodic=0`` for those (or send
        ``period: 0`` with a selection). Box and sphere selections wrap on
        these columns (app/periodic.py). A lightcone catalogue's positions
        are around the observer: they never wrap."""
        units = self.snapshot.units
        periodic = units.periodic if units.periodic is not None else bool(units.comoving)
        box = float(self.snapshot.header.box_size)
        if not (periodic and math.isfinite(box) and box > 0) or self.snapshot.lightcone:
            return {}
        return {name: box for name in self.names if name in PERIODIC_COLUMNS}

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
        units, h = self.snapshot.units, self.snapshot.hubble
        a = self.snapshot.scale_factor          # 1 for a run that is not comoving
        factor = units.mass_g / units.length_cm ** 3 * h ** 2
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

    def _velocity_convention(self, name: str) -> tuple[float, str] | None:
        """For a velocity column, the power of a that makes it peculiar and
        what the stored value is: a particle's carries Gadget's internal
        sqrt(a); a SubFind subhalo's is written peculiar by Gadget-4
        (subfind_properties.cc) and Arepo (subfind_properties.c), unless
        ``svel=`` says otherwise; an HDF5 catalogue's GroupVel is the
        members' mass-weighted internal a^2 dx/dt (fof.cc), so 1/a. On the
        lightcone both are peculiar already, whatever ``svel=`` says
        (fac_vel_to_phys = 1). None for any other column."""
        source = self._sources.get(name)
        if source is None or name not in ("vx", "vy", "vz"):
            return None
        block = source[0]
        if self.snapshot.lightcone and block in ("SVEL", "GroupVel"):
            return 0.0, "peculiar (lightcone)"
        if block == "SVEL":
            return SUBFIND_VEL_FACTORS[self.snapshot.units.subfind_vel]
        if block == "GroupVel":
            return -1.0, "a^2 dx/dt, x 1/a"
        return 0.5, "internal, x sqrt(a)"

    def _subhalo_radii_physical(self) -> bool:
        """Whether this SubFind component's half-mass and Vmax radii are
        stored physical: Gadget-4 measures them in physical distances
        (subfind_properties.cc, fac_comov_to_phys); Arepo and the binary
        catalogues keep them comoving. ``srad=`` overrides."""
        stated = self.snapshot.units.subfind_radii
        if stated is not None:
            return stated == "physical"
        return self.snapshot.producer == "gadget4"

    def _is_subhalo_radius(self, name: str) -> bool:
        source = self._sources.get(name)
        return (self.snapshot.is_subfind and source is not None
                and source[0] in SUBHALO_RADIUS_BLOCKS)

    def _read_scale(self, name: str) -> float:
        """The factor a column is multiplied by when read: 1/a for subhalo
        radii stored physical, so that they are comoving like the positions
        whose markers and framing they size (#441); 1 for any other. On the
        lightcone Gadget-4 makes them physical with the group's GroupAscale,
        which it sets to the time the groups were found, the header's
        (fof.cc), so the header's a undoes it there too."""
        if self._is_subhalo_radius(name) and self._subhalo_radii_physical():
            return 1.0 / self.snapshot.scale_factor
        return 1.0

    def _hdf5_catalogue_extras(self, length_phys, vel) -> dict:
        """Columns of the Gadget-4 and Arepo HDF5 catalogues whose units their
        writers state: SubhaloVelDisp in physical km/s (Hubble flow
        included), SubhaloSpin in physical (L/h)(V) (subfind_properties.cc,
        subfind_properties.c). A binary catalogue's SPIN stays undeclared."""
        if not self.snapshot.producer:
            return {}
        return {"subhaloveldisp": vel, "spin_x": length_phys, "spin_y": length_phys,
                "spin_z": length_phys}

    def units(self) -> dict[str, str]:
        """Each column's unit as stored (code units, stated)."""
        u = self.snapshot.units.describe()
        h = "/h" if self.snapshot.units.hubble_units else ""
        comoving = bool(self.snapshot.units.comoving)
        com = " comoving" if comoving else ""
        length = f"{u['length']}{h}{com}"
        mass = f"{u['mass']}{h}"
        vel = u["velocity"]
        sub_radius = (f"{length} (stored physical, x 1/a on read)"
                      if comoving and self._subhalo_radii_physical() else length)
        table = {"x": length, "y": length, "z": length, "hsml": length,
                 "hsml_stars": length, "bh_accretion_radius": length,
                 "rvir": length, "r500": length, "r2500": length,
                 "rmax": sub_radius, "half_mass_radius": sub_radius,
                 "group_r_mean200": length, "group_r_crit200": length,
                 "centre_of_mass_x": length, "centre_of_mass_y": length,
                 "centre_of_mass_z": length,
                 "vmax": f"{vel} physical",
                 "mass": mass, "bh_mass": mass, "initial_mass": mass,
                 "group_mass": mass, "mvir": mass, "m500": mass, "m2500": mass,
                 "group_m_mean200": mass, "group_m_crit200": mass,
                 "gas_mass": mass, "stellar_mass": mass, "subhalo_mass": mass,
                 "contaminating_mass": mass,
                 "rho": f"{mass} / ({length})^3", "u": f"({u['velocity']})^2",
                 "potential": f"({u['velocity']})^2",
                 "temperature": "K", "entropy": "keV cm^2",
                 "ne": "dimensionless", "nh": "dimensionless",
                 "metallicity": "dimensionless", "cold_fraction": "dimensionless",
                 "id": "dimensionless identifier", "most_bound_id": "dimensionless identifier",
                 "group_length": "particle count", "group_offset": "particle index",
                 "num_subhalos": "count", "first_subhalo": "subhalo index",
                 "num_contaminated": "particle count", "subhalo_length": "particle count",
                 "subhalo_offset": "particle index", "subhalo_parent": "subhalo index",
                 "group_number": "group index"}
        table.update(self._hdf5_catalogue_extras(f"{u['length']}{h} {vel} physical",
                                                 f"{vel} physical"))
        for n in self.names:
            if self._is_subhalo_radius(n):
                table[n] = sub_radius
        for n in ("vx", "vy", "vz"):
            convention = self._velocity_convention(n)
            if convention is not None:
                table[n] = f"{vel} ({convention[1]})" if comoving else vel
        return {n: table.get(n, "code units (physical conversion not declared)")
                for n in self.names}

    def physical(self) -> dict[str, dict]:
        """Per column, the factor that makes it physical and the unit then:
        lengths in kpc (x a / h), masses in Msun (/ h), velocities in km/s
        peculiar (see _velocity_convention), density in g/cm^3. What the user
        typed for a column (``app.physical_units``) replaces it, marked
        ``"source": "user"``."""
        units, hh = self.snapshot.units, self.snapshot.hubble
        a = self.snapshot.scale_factor          # never 0: 1 when not comoving
        length = units.length_cm / KPC_CM * a / hh
        mass = units.mass_g / MSUN_G / hh
        kms = units.velocity_cm_s / 1e5
        rho = units.mass_g / units.length_cm ** 3 * hh ** 2 / a ** 3
        table = {"x": (length, "kpc"), "y": (length, "kpc"), "z": (length, "kpc"),
                 "hsml": (length, "kpc"), "hsml_stars": (length, "kpc"),
                 "bh_accretion_radius": (length, "kpc"),
                 "rvir": (length, "kpc"), "r500": (length, "kpc"),
                 "r2500": (length, "kpc"), "rmax": (length, "kpc"),
                 "half_mass_radius": (length, "kpc"),
                 "group_r_mean200": (length, "kpc"), "group_r_crit200": (length, "kpc"),
                 "centre_of_mass_x": (length, "kpc"),
                 "centre_of_mass_y": (length, "kpc"),
                 "centre_of_mass_z": (length, "kpc"),
                 "vmax": (kms, "km/s"),
                 "mass": (mass, "Msun"), "bh_mass": (mass, "Msun"),
                 "initial_mass": (mass, "Msun"),
                 "group_mass": (mass, "Msun"), "mvir": (mass, "Msun"),
                 "group_m_mean200": (mass, "Msun"), "group_m_crit200": (mass, "Msun"),
                 "m500": (mass, "Msun"), "m2500": (mass, "Msun"),
                 "gas_mass": (mass, "Msun"), "stellar_mass": (mass, "Msun"),
                 "subhalo_mass": (mass, "Msun"), "contaminating_mass": (mass, "Msun"),
                 "rho": (rho, "g/cm^3"), "u": (units.velocity_cm_s ** 2, "erg/g"),
                 "ne": (1.0, "dimensionless"), "nh": (1.0, "dimensionless"),
                 "metallicity": (1.0, "dimensionless"),
                 "cold_fraction": (1.0, "dimensionless")}
        table.update(self._hdf5_catalogue_extras(
            (units.length_cm / KPC_CM / hh * kms, "kpc km/s"), (kms, "km/s")))
        for n in self.names:
            if self._is_subhalo_radius(n):
                table[n] = (length, "kpc")
        for n in ("vx", "vy", "vz"):
            convention = self._velocity_convention(n)
            if convention is not None:
                table[n] = (kms * a ** convention[0], "km/s")
        from . import physical_units  # noqa: PLC0415
        return physical_units.with_typed(
            self.snapshot.path,
            {n: {"factor": float(table[n][0]), "unit": table[n][1]}
             for n in self.names if n in table},
            columns=tuple(self.names))

    def _radius_definitions(self) -> dict[str, str]:
        """What each SubFind overdensity radius encloses. An HDF5 catalogue
        maps Group_R_TopHat200 to rvir and Group_R_Crit500 to r500
        (SUBFIND_HDF5_BLOCKS), as Gadget-4 and Arepo define them
        (subfind_so.cc)."""
        if self.snapshot.producer:
            return {"rvir": "Bryan & Norman (1998) virial overdensity Delta_c(z) "
                            "times critical density",
                    "r500": "500 times critical density",
                    "group_r_mean200": "200 times mean background density",
                    "group_r_crit200": "200 times critical density"}
        return {"rvir": "mean background density",
                "r500": "500 times mean background density",
                "r2500": "2500 times mean background density"}

    def describe(self) -> dict:
        return {"type": self.ptype, "name": self.name, "num_rows": self.num_rows,
                "catalogue_kind": "subfind" if self.snapshot.is_subfind else "particles",
                **({"object_kind": "group" if self.ptype == 0 else "subhalo",
                    "radius_definitions": self._radius_definitions()}
                   if self.snapshot.is_subfind and self.ptype in (0, 1) else {}),
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
        self._kept = {}
        # Read by the point sessions' box and sphere filters; a name no
        # column takes (a mapping's keys are its columns).
        self.column_periods = component.periods()

    def __getitem__(self, name: str):
        if name not in self._kept:
            if name not in self._component.names:
                raise KeyError(name)
            self._kept[name] = SegmentedColumn(self._component, name)
        return self._kept[name]

    def __iter__(self):
        return iter(self._component.names)

    def __len__(self) -> int:
        return len(self._component.names)

    def __contains__(self, name) -> bool:
        return name in self._component.names


class SegmentedColumn:
    """Array-like view that reads indexed rows without joining file pieces."""

    def __init__(self, component: Component, name: str):
        self.component = component
        self.name = name
        self.dtype = component.column_dtype(name)
        self.shape = (component.num_rows,)
        self.ndim = 1

    def __len__(self) -> int:
        return self.shape[0]

    def __getitem__(self, rows):
        """Rows by a slice, an integer (negative from the end), an integer
        array, or a boolean mask of the column's length, as numpy indexes;
        anything else (floats, a mask of another length) raises IndexError
        rather than being read as row numbers."""
        n = len(self)
        if isinstance(rows, slice):
            return self.component.take(self.name, np.arange(*rows.indices(n), dtype=np.int64))
        if isinstance(rows, (int, np.integer)) and not isinstance(rows, (bool, np.bool_)):
            row = int(rows)
            if not -n <= row < n:
                raise IndexError(f"row {row} outside a column of {n}")
            return self.component.take(self.name, row % n)
        index = np.asarray(rows)
        if index.size == 0 and index.ndim == 1 and index.dtype != bool:
            index = index.astype(np.int64)      # [] is float64 to numpy
        if index.dtype == bool:
            if index.shape != (n,):
                raise IndexError(f"boolean index of shape {index.shape} for a column of {n}")
            return self.component.take(self.name, np.flatnonzero(index))
        if index.dtype.kind not in "iu" or index.ndim > 1:
            raise IndexError("a particle column is indexed by a slice, integers or a "
                             f"boolean mask, not {index.dtype} of {index.ndim} dimensions")
        index = index.astype(np.int64, copy=False)
        if index.size and (index.min() < -n or index.max() >= n):
            raise IndexError(f"row outside a column of {n}")
        return self.component.take(self.name, np.where(index < 0, index + n, index))

    def __array__(self, dtype=None, copy=None) -> np.ndarray:
        values = self.component.take(self.name, np.arange(len(self), dtype=np.int64))
        if dtype is not None:
            values = values.astype(dtype, copy=False)
        elif copy:
            values = values.copy()
        return values


# ── References ───────────────────────────────────────────────────────────────

def release(path: str | Path) -> int:
    """Drop what is kept in memory for the snapshot ``path`` (a component
    reference or a file of it) once no dataset reads it: its decompressed
    HDF5 pieces. The number of pieces dropped; 0 for anything else."""
    try:
        snap = open_snapshot(split_ref(path)[0])
    except (OSError, ValueError):
        return 0
    return PIECE_CACHE.drop(f.path for f in snap.files)


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


def read_header(path: str | Path) -> Header:
    """The header of one snapshot file, read alone (no block is scanned)."""
    found = detect(path)
    if found is None:
        raise ValueError(f"{path} is not a Gadget snapshot file.")
    fmt, e = found
    if fmt == 0:
        return _scan_hdf5(str(path)).header
    with open(path, "rb") as fh:
        # Format 1: the header record first; format 2: after its 8-byte
        # "HEAD" name record (16 bytes with its markers).
        fh.seek(4 if fmt == 1 else 20)
        return parse_header(fh.read(HEADER_BYTES), e)


def has_type(path: str | Path, ptype: int) -> bool:
    """Whether the snapshot ``path`` names (a file of it, its base name or
    its folder) has particles of type ``ptype``, from its headers: an early
    output without stars, or one that cannot be read (a file missing, not a
    snapshot), has none. Only headers and the files' names are read, and
    the answer is kept while the first file is unchanged, so a whole series
    can be checked at every open."""
    try:
        files = snapshot_files(path)
        size, mtime = _stat(files[0])
        return _has_type(tuple(str(f) for f in files), int(ptype), size, mtime)
    except (OSError, ValueError):
        return False


@lru_cache(maxsize=4096)
def _has_type(files: tuple[str, ...], ptype: int, _size: int, _mtime: int) -> bool:
    try:
        header = read_header(files[0])
        if not 0 <= ptype < len(header.npart_total):
            return False
        if any(header.npart_total):
            return header.npart_total[ptype] > 0
        # Some writers leave the totals at zero: the files' own counts.
        return any(read_header(f).npart[ptype] > 0 for f in files)
    except (OSError, ValueError):
        return False


def _offered(snapshot: Snapshot, ptype: int) -> bool:
    """Whether type ``ptype`` opens as points: it has particles, and in a
    SubFind catalogue positions too (its ID list and large-group table have
    none, issue #448)."""
    if not snapshot.counts()[ptype]:
        return False
    return not snapshot.is_subfind or {"x", "y", "z"} <= set(snapshot.component(ptype).names)


def default_type(snapshot: Snapshot) -> int | None:
    """The type a snapshot opens on: the first offered (gas first)."""
    return next((t for t in range(NTYPES) if _offered(snapshot, t)), None)


def gadget_components(path: str | Path) -> list[dict]:
    """Every particle type with particles: ``{name, path, num_rows}``. Of a
    SubFind catalogue, only the types with positions (groups, subhalos):
    its ID list and large-group table are no points to draw (issue #448)."""
    snap = open_snapshot(split_ref(path)[0])
    names = SUBFIND_TYPE_NAMES if snap.is_subfind else TYPE_NAMES
    return [{"name": names[t], "path": gadget_ref(snap.path, t), "num_rows": int(n)}
            for t, n in enumerate(snap.counts()) if _offered(snap, t)]


#: The name every component module (app/point_table.py COMPONENT_FORMATS) offers.
components = gadget_components
