"""A small RAMSES output written the way RAMSES writes one (issue #549), for
the tests: two processes, a mesh of one level-1 grid whose cells 0 and 7 are
refined into two level-2 grids (one per process), every file also holding the
*virtual* copies of the grids of the other process (RAMSES writes them, and
the reader must leave them out), hydro variables with a value that names the
grid, cell and variable, and particles in the old fixed layout, with a
``part_file_descriptor.txt``, or with the header's ``Particle fields`` line.

The layout is the one of RAMSES's ``amr2cube.f90`` / ``part2cube.f90`` and of
a real 2018 output (checked against it by tests/test_ramses_output.py when
that output is at hand): Fortran records framed by their length in the
writer's byte order, 4-byte markers."""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

NLEVELMAX = 2
BOXLEN = 2.0
UNIT_L, UNIT_D, UNIT_T = 3.0e24, 1.0e-28, 5.0e16


def _rec(fh, payload: bytes, e: str) -> None:
    mark = struct.pack(e + "i", len(payload))
    fh.write(mark + payload + mark)


def _i(e: str, *v: int) -> bytes:
    return struct.pack(f"{e}{len(v)}i", *v)


def _d(e: str, *v: float) -> bytes:
    return struct.pack(f"{e}{len(v)}d", *v)


def _a(e: str, kind: str, size: int, values) -> bytes:
    return np.asarray(values, dtype=np.dtype(f"{e}{kind}{size}")).tobytes()


@dataclass(frozen=True)
class Grid:
    gid: int
    level: int
    centre: tuple
    owner: int
    sons: tuple          # the cells (0..7) that are refined


#: The mesh: grid 1 (level 1, process 1) refined at cells 0 and 7 into grids
#: 2 (process 1) and 3 (process 2), both level 2.
GRIDS = (Grid(1, 1, (0.5, 0.5, 0.5), 1, (0, 7)),
         Grid(2, 2, (0.25, 0.25, 0.25), 1, ()),
         Grid(3, 2, (0.75, 0.75, 0.75), 2, ()))


def hydro_value(grid: int, ind: int, var: int) -> float:
    return 1000.0 * grid + 10.0 * ind + var + 0.5


@dataclass
class Expected:
    """What the test output holds, for the tests to compare with."""
    leaves: list = field(default_factory=list)     # (level, centre tuple, [values])
    particles: dict = field(default_factory=dict)  # family slug -> {column: array}
    info: dict = field(default_factory=dict)


def _grids(refine: bool):
    return GRIDS if refine else (Grid(1, 1, (0.5, 0.5, 0.5), 1, ()),)


def _expected_leaves(refine: bool, nvar: int):
    out = []
    for g in _grids(refine):
        dx = 0.5 ** g.level
        for ind in range(8):
            if ind in g.sons:
                continue
            off = [(ind >> d) & 1 for d in range(3)]
            centre = tuple(g.centre[d] + (off[d] - 0.5) * dx for d in range(3))
            out.append((g.level, centre, [hydro_value(g.gid, ind, v) for v in range(nvar)]))
    return out


def _write_info(path: Path, number: int, ncpu: int, nlevelmax: int, aexp: float) -> None:
    path.write_text(
        f"ncpu        =          {ncpu}\nndim        =          3\nlevelmin    =          1\n"
        f"levelmax    =          {nlevelmax}\nngridmax    =       1000\nnstep_coarse=          7\n \n"
        f"boxlen      =  {BOXLEN:.15E}\ntime        =  0.250000000000000E+00\n"
        f"aexp        =  {aexp:.15E}\nH0          =  0.700000000000000E+02\n"
        "omega_m     =  0.300000000000000E+00\nomega_l     =  0.700000000000000E+00\n"
        "omega_k     =  0.000000000000000E+00\nomega_b     =  0.450000000000000E-01\n"
        f"unit_l      =  {UNIT_L:.15E}\nunit_d      =  {UNIT_D:.15E}\nunit_t      =  {UNIT_T:.15E}\n"
        " \nordering type=hilbert\n   DOMAIN   ind_min                 ind_max\n"
        + "".join(f"   {c + 1:4d}   {c:.15E}   {c + 1:.15E}\n" for c in range(ncpu)))


def _write_amr(path: Path, cpu: int, ncpu: int, e: str, grids, nlevelmax: int) -> None:
    owned = np.zeros((nlevelmax, ncpu), dtype=np.int32)       # numbl[level, cpu]
    for g in grids:
        owned[g.level - 1, g.owner - 1] += 1
    with open(path, "wb") as fh:
        w = lambda payload: _rec(fh, payload, e)             # noqa: E731
        w(_i(e, ncpu))
        w(_i(e, 3))
        w(_i(e, 1, 1, 1))
        w(_i(e, nlevelmax))
        w(_i(e, 1000))
        w(_i(e, 0))
        w(_i(e, len(grids)))
        w(_d(e, BOXLEN))
        w(_i(e, 1, 1, 1))
        w(_d(e, 0.0))
        w(_d(e, 1.0))
        w(_d(e, 0.25))
        w(_d(e, *([0.01] * nlevelmax)))
        w(_d(e, *([0.01] * nlevelmax)))
        w(_i(e, 7, 7))
        w(_d(e, 0.0, 1.0, 1.0))
        w(_d(e, 0.3, 0.7, 0.0, 0.045, 70.0, 0.5, BOXLEN))
        w(_d(e, 0.5, 1.0, 0.5, 0.0, 0.0))
        w(_d(e, 1.0))
        w(_a(e, "i", 4, np.zeros(ncpu * nlevelmax)))           # headl
        w(_a(e, "i", 4, np.zeros(ncpu * nlevelmax)))           # taill
        w(_a(e, "i", 4, owned.reshape(-1)))                    # numbl: [level][cpu] row-major
        w(_a(e, "i", 4, np.zeros(10 * nlevelmax)))             # numbtot
        w(_i(e, 0, 0, 0, 0, 0))                                # headf tailf numbf used_mem ..
        w(b"hilbert".ljust(128))
        w(_d(e, *[float(c) for c in range(ncpu + 1)]))         # bound_key
        w(_i(e, 1))                                            # coarse son: refined
        w(_i(e, 0))
        w(_i(e, 1))                                            # coarse cpu map
        for level in range(1, nlevelmax + 1):
            for owner in range(1, ncpu + 1):
                block = [g for g in grids if g.level == level and g.owner == owner]
                if not block:
                    continue
                n = len(block)
                mine = owner == cpu
                w(_a(e, "i", 4, [g.gid for g in block]))       # ind_grid
                w(_a(e, "i", 4, np.zeros(n)))                  # next
                w(_a(e, "i", 4, np.zeros(n)))                  # prev
                for d in range(3):
                    w(_a(e, "f", 8, [g.centre[d] for g in block]))
                w(_a(e, "i", 4, np.zeros(n)))                  # father
                for _ in range(6):
                    w(_a(e, "i", 4, np.zeros(n)))              # nbor
                for ind in range(8):
                    # A virtual copy states no refinement: reading it as a leaf
                    # would double-count its cells.
                    w(_a(e, "i", 4, [(g.gid + 1) if (mine and ind in g.sons) else 0
                                     for g in block]))
                for _ in range(8):
                    w(_a(e, "i", 4, [owner] * n))              # cpu_map
                for ind in range(8):
                    w(_a(e, "i", 4, [int(mine and ind in g.sons) for g in block]))


def _write_hydro(path: Path, cpu: int, ncpu: int, e: str, grids, nlevelmax: int,
                 nvar: int) -> None:
    with open(path, "wb") as fh:
        w = lambda payload: _rec(fh, payload, e)             # noqa: E731
        w(_i(e, ncpu))
        w(_i(e, nvar))
        w(_i(e, 3))
        w(_i(e, nlevelmax))
        w(_i(e, 0))
        w(_d(e, 1.4))
        for level in range(1, nlevelmax + 1):
            for owner in range(1, ncpu + 1):
                block = [g for g in grids if g.level == level and g.owner == owner]
                w(_i(e, level))
                w(_i(e, len(block)))
                for ind in range(8 if block else 0):
                    for v in range(nvar):
                        # The other process's copy holds values that are not its own.
                        w(_a(e, "f", 8, [hydro_value(g.gid, ind, v) if owner == cpu else -1.0
                                         for g in block]))


# ── particles ────────────────────────────────────────────────────────────────

#: Particles per process: (family, ...) in file order. Codes: 1 dm, 2 stars, 3 sinks.
PARTICLE_FAMILIES = {1: [1, 2, 1, 1, 2, 1], 2: [1, 2, 2, 1, 3]}


def _particle_data(cpu: int, with_family: bool, rng) -> dict:
    fam = np.array(PARTICLE_FAMILIES[cpu] if with_family else
                   [c if c != 3 else 1 for c in PARTICLE_FAMILIES[cpu]])
    n = len(fam)
    data = {"x": rng.random(n), "y": rng.random(n), "z": rng.random(n),
            "vx": rng.normal(size=n), "vy": rng.normal(size=n), "vz": rng.normal(size=n),
            "mass": rng.random(n) + 0.5,
            "id": np.arange(n) + 1000 * cpu, "level": np.full(n, 3) + cpu,
            "family": fam.astype(np.int8), "tag": np.zeros(n, dtype=np.int8),
            "birth_time": np.where(fam == 2, -0.125 * (np.arange(n) + 1), 0.0),
            "metallicity": np.where(fam == 2, 0.02, 0.0)}
    return data


def _write_particles(path: Path, cpu: int, e: str, data: dict, columns, int_size: int,
                     stars_total: int) -> None:
    n = len(data["x"])
    with open(path, "wb") as fh:
        w = lambda payload: _rec(fh, payload, e)             # noqa: E731
        w(_i(e, 2))
        w(_i(e, 3))
        w(_i(e, n))
        w(_i(e, 1, 2, 3, 4))
        w(_i(e, stars_total))
        w(_d(e, 0.0))
        w(_d(e, 0.0))
        w(_i(e, 0))
        for name in columns:
            if name in ("id", "level"):
                w(_a(e, "i", int_size if name == "id" else 4, data[name]))
            elif name in ("family", "tag"):
                w(_a(e, "i", 1, data[name]))
            else:
                w(_a(e, "f", 8, data[name]))


def write_output(parent: Path, number: int = 1, *, endian: str = "<", particles: str | None = "old",
                 hydro_descriptor: bool = False, mesh: bool = True, refine: bool = True,
                 nvar: int = 6, aexp: float = 0.5, seed: int = 0) -> tuple[Path, Expected]:
    """Write ``parent/output_NNNNN`` and what it holds.

    ``particles``: "old" (positions, velocities, mass, id, level, birth,
    metallicity: no descriptor, families by birth time), "descriptor"
    (``part_file_descriptor.txt``, a ``family`` record, 8-byte identifiers),
    "header" (no descriptor, the header's ``Particle fields`` line), or None
    (no particle files)."""
    e = endian
    ncpu = 2
    directory = Path(parent) / f"output_{number:05d}"
    directory.mkdir(parents=True)
    tag = f"{number:05d}"
    grids = _grids(refine)
    nlevelmax = NLEVELMAX if refine else 1
    expected = Expected(info={"ncpu": ncpu, "boxlen": BOXLEN, "aexp": aexp, "nvar": nvar})
    _write_info(directory / f"info_{tag}.txt", number, ncpu, nlevelmax, aexp)
    if mesh:
        for cpu in (1, 2):
            _write_amr(directory / f"amr_{tag}.out{cpu:05d}", cpu, ncpu, e, grids, nlevelmax)
            _write_hydro(directory / f"hydro_{tag}.out{cpu:05d}", cpu, ncpu, e, grids, nlevelmax,
                         nvar)
        expected.leaves = _expected_leaves(refine, nvar)
        if hydro_descriptor:
            names = ["density", "velocity_x", "velocity_y", "velocity_z", "pressure",
                     "metallicity"] + [f"scalar_{k:02d}" for k in range(nvar - 5)]
            lines = ["# version:  1", "# ivar, variable_name, variable_type"]
            lines += [f"{k + 1:3d}, {n}, d" for k, n in enumerate(names[:nvar])]
            (directory / "hydro_file_descriptor.txt").write_text("\n".join(lines) + "\n")
    if particles:
        rng = np.random.default_rng(seed)
        with_family = particles != "old"
        per_cpu = {cpu: _particle_data(cpu, with_family, rng) for cpu in (1, 2)}
        if particles == "old":
            columns = ["x", "y", "z", "vx", "vy", "vz", "mass", "id", "level", "birth_time",
                       "metallicity"]
        else:
            columns = ["x", "y", "z", "vx", "vy", "vz", "mass", "id", "level", "family", "tag",
                       "birth_time", "metallicity"]
        int_size = 8 if particles == "descriptor" else 4
        stars = sum(int((d["birth_time"] != 0).sum()) for d in per_cpu.values())
        for cpu, data in per_cpu.items():
            _write_particles(directory / f"part_{tag}.out{cpu:05d}", cpu, e, data, columns,
                             int_size, stars)
        if particles == "descriptor":
            kinds = {"x": ("position_x", "d"), "y": ("position_y", "d"), "z": ("position_z", "d"),
                     "vx": ("velocity_x", "d"), "vy": ("velocity_y", "d"),
                     "vz": ("velocity_z", "d"), "mass": ("mass", "d"), "id": ("identity", "l"),
                     "level": ("levelp", "i"), "family": ("family", "b"), "tag": ("tag", "b"),
                     "birth_time": ("birth_time", "d"), "metallicity": ("metallicity", "d")}
            lines = ["# version:  1", "# ivar, variable_name, variable_type"]
            lines += [f"{k + 1:3d}, {kinds[c][0]}, {kinds[c][1]}" for k, c in enumerate(columns)]
            (directory / "part_file_descriptor.txt").write_text("\n".join(lines) + "\n")
        total = sum(len(d["x"]) for d in per_cpu.values())
        header = (f" Total number of particles\n {total}\n Total number of dark matter particles\n"
                  f" {sum(int((d['birth_time'] == 0).sum() - (d['family'] == 3).sum()) for d in per_cpu.values())}\n"
                  f" Total number of star particles\n {stars}\n Total number of sink particles\n"
                  f" {sum(int((d['family'] == 3).sum()) for d in per_cpu.values())}\n")
        if particles == "header":
            header += (" Particle fields\n pos vel mass iord level family tag birth metal\n")
        (directory / f"header_{tag}.txt").write_text(header)
        # What each family holds, concatenated over the processes in file order.
        for code, slug in ((1, "dm"), (2, "stars"), (3, "sinks")):
            parts = []
            for cpu in (1, 2):
                d = per_cpu[cpu]
                if particles == "old":
                    sel = (d["birth_time"] != 0) if code == 2 else (d["birth_time"] == 0)
                    if code == 3:
                        continue
                else:
                    sel = d["family"] == code
                parts.append({k: v[sel] for k, v in d.items()})
            if not parts or not sum(len(p["x"]) for p in parts):
                continue
            expected.particles[slug] = {k: np.concatenate([p[k] for p in parts])
                                        for k in parts[0]}
    return directory, expected
