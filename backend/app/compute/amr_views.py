"""Views of an AMR hierarchy that need no code's file layout (issue #550): a
uniform cube of one field at a chosen level, the refinement-level map of a
slice, and the finest level a box holds. All of it reads
:class:`app.amr_source.AmrHierarchy`, so RAMSES outputs and the yt-backed
producers of #554 get them alike; the cells as points are app/amr_cells.py.

The uniform cube
----------------
At the target level ``L`` the grid is the lattice of cubes of side
``2**-L`` box units, restricted to a box (the whole run by default) snapped
outward to it. Leaves tile the box, so a voxel is either inside one leaf of
level <= L or tiled by leaves finer than ``L``:

* a leaf at ``l <= L`` gives its value to every voxel it covers (a 2**(L-l)
  cube of them);
* the leaves finer than ``L`` inside a voxel give their mean, weighted by what
  the field is per unit of what (see "Weighting").

A voxel's mass ``density * dx_L^3`` is then exactly the mass of the cells it
holds (the first case: the cell's mass share; the second: their sum), so the
mass cube sums to the gas mass of the region at every level, up to float32
rounding: the cube and the cells as points agree. ``level`` is not an
intensive quantity: a voxel holds the finest level among its leaves (what
shows where the mesh is refined), not a mean.

Weighting
---------
A mean over the leaves inside a voxel is taken with the weight that keeps the
quantity it stands for: ``volume`` (``sum(v dx_leaf^3) / dx_L^3``) for the
density, the pressure and the mass, whose integral over volume is the
conserved total; ``density`` (``sum(rho v dx_leaf^3) / sum(rho dx_leaf^3)``,
the mass-weighted mean, as yt takes it) for the velocities and for the
metallicity and the passive scalars, which are carried by the gas: the
momentum ``rho v`` of a voxel is then the sum of its leaves', and a light
coarse leaf does not drag the velocity of a dense fine one. The level is the
finest among the leaves. :func:`weighting` says which a field gets, the
header's ``AMRWGT`` records it.

Values are written in the run's CODE units (the density in the run's density
unit, coordinates in box units): the physical factors are the run's own
(``AmrHierarchy.scales()``) and the shared units table
(app/physical_units.py) applies them, so Display > Physical and a typed
override work on the cube as on any simulation volume.

Cost
----
The cube is made in slabs of whole planes (``SLAB_VOXELS`` voxels at most, 9
bytes a voxel in the accumulators, 17 when the mean is density-weighted), each from one pass over the leaves that
reach it, and streamed to the file: memory is the slab plus one leaf block,
whatever the cube's size. ``plan_uniform`` states the voxels, bytes and passes
before anything runs, against the limits of the PLUTO Cartesian view.
"""
from __future__ import annotations

import math
import re
import shutil
import tempfile
from collections.abc import Iterator, Sequence
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

from ..amr_source import VIEW_SUFFIX, AmrHierarchy, LeafBlock
from .arepo import _safe_name
from .pluto_cartesian import DEFAULT_MAX_VOXELS, HARD_MAX_VOXELS, _padding

#: The fields that are not a hydro variable of the run.
LEVEL, MASS = "level", "mass"
#: Voxels of a slab at most (a whole plane is the least: a wider one is one slab).
SLAB_VOXELS = 1 << 24
#: Coarse leaves that cover up to this many voxels per side are filled in a
#: vectorised loop over the offsets inside them, larger ones one by one.
_VECTOR_FILL = 8
#: How a field's mean over a voxel is weighted (see the module docstring).
VOLUME, DENSITY, FINEST = "volume", "density", "finest"
_DENSITY_WEIGHTED = re.compile(r"v[xyz]|velocity(_[xyz])?|metallicity|scalar_\d+|passive.*")
#: A level map answers at most this many rectangles unless asked for more.
DEFAULT_MAX_RECTS = 200_000
#: No mesh is deeper than this (an index at the level must fit an int64).
MAX_LEVEL = 40


class AmrViewError(ValueError):
    """The hierarchy cannot be shown this way, or the request is unusable."""


# ── Fields, regions ──────────────────────────────────────────────────────────

def fields_of(h: AmrHierarchy) -> list[str]:
    """What a view can show: the hydro variables, the cell mass (when there is
    a density) and the refinement level."""
    return [*h.fields, *([MASS] if "density" in h.fields else []), LEVEL]


def weighting(h: AmrHierarchy, field: str) -> str:
    """How the leaves finer than a voxel are averaged for ``field``: ``finest``
    for the level, ``density`` for a velocity, the metallicity or a passive
    scalar (when the mesh has a density to weight by), ``volume`` otherwise."""
    if field == LEVEL:
        return FINEST
    if field != "density" and "density" in h.fields and _DENSITY_WEIGHTED.fullmatch(field):
        return DENSITY
    return VOLUME


def _length_factor(h: AmrHierarchy) -> tuple[float, str]:
    s = h.scales().get("x")
    return (float(s["factor"]), str(s["unit"])) if s else (1.0, "box units")


def region_box(h: AmrHierarchy, lo: Sequence[float] | None, hi: Sequence[float] | None,
               unit: str = "box") -> tuple[np.ndarray, np.ndarray]:
    """``(lo, hi)`` in box units of the region asked for (None: the whole
    run), clipped to the run. ``unit``: ``"box"`` (a fraction of the box) or
    ``"physical"`` (the length unit of the run's scales, counted from the
    box's corner, the cube's own axes)."""
    extent = np.asarray(h.extent, dtype=np.float64)
    if lo is None and hi is None:
        return np.zeros(3), extent.copy()
    if lo is None or hi is None or len(lo) != 3 or len(hi) != 3:
        raise AmrViewError("A region is its two corners, lo and hi, three numbers each.")
    if unit not in ("box", "physical"):
        raise AmrViewError("The region's unit is 'box' or 'physical'.")
    a, b = np.asarray(lo, dtype=np.float64), np.asarray(hi, dtype=np.float64)
    if not (np.isfinite(a).all() and np.isfinite(b).all()):
        raise AmrViewError("The region's corners must be finite.")
    if unit == "physical":
        factor = _length_factor(h)[0]
        a, b = a / factor, b / factor
    a, b = np.minimum(a, b), np.maximum(a, b)
    a, b = np.clip(a, 0.0, extent), np.clip(b, 0.0, extent)
    if (b <= a).any():
        raise AmrViewError("The region is empty: it lies outside the run or has no extent.")
    return a, b


# ── What to produce ──────────────────────────────────────────────────────────

@dataclass(frozen=True)
class UniformPlan:
    field: str
    level: int
    i0: tuple[int, int, int]       # first voxel along x, y, z at the level
    shape: tuple[int, int, int]    # voxels along x, y, z (the FITS order)
    weighting: str = VOLUME        # how finer leaves are averaged (see weighting())

    @property
    def dx(self) -> float:
        return 2.0 ** -self.level

    @property
    def voxels(self) -> int:
        return int(np.prod(self.shape, dtype=np.int64))

    @property
    def lower(self) -> tuple[float, float, float]:
        return tuple(i * self.dx for i in self.i0)

    @property
    def upper(self) -> tuple[float, float, float]:
        return tuple((i + n) * self.dx for i, n in zip(self.i0, self.shape))

    @property
    def slab_planes(self) -> int:
        """Planes (along z) of a slab: as many as ``SLAB_VOXELS`` holds, at least one."""
        return max(1, min(self.shape[2], SLAB_VOXELS // max(1, self.shape[0] * self.shape[1])))

    @property
    def passes(self) -> int:
        return -(-self.shape[2] // self.slab_planes)

    @property
    def slab_bytes(self) -> int:
        # value (8) + covered (1), and the weight (8) of a density-weighted mean.
        return self.slab_planes * self.shape[0] * self.shape[1] * (17 if self.weighting == DENSITY
                                                                  else 9)


def _lattice(lo: np.ndarray, hi: np.ndarray, level: int) -> tuple[tuple, tuple]:
    """The voxels at ``level`` that cover ``lo`` .. ``hi``: ``(i0, shape)``."""
    scale = float(2 ** level)
    i0 = np.floor(lo * scale + 1e-9).astype(np.int64)
    i1 = np.ceil(hi * scale - 1e-9).astype(np.int64)
    i1 = np.maximum(i1, i0 + 1)
    return tuple(int(i) for i in i0), tuple(int(n) for n in i1 - i0)


def plan_uniform(h: AmrHierarchy, field: str, level: int | None = None,
                 lo: Sequence[float] | None = None, hi: Sequence[float] | None = None,
                 unit: str = "box", max_voxels: int = DEFAULT_MAX_VOXELS,
                 top: int | None = None) -> UniformPlan:
    """The grid ``field`` would be resampled onto. ``level`` None: the finest
    level whose voxels fit ``max_voxels``, not above ``top`` (the finest level
    the region holds: finer voxels would only repeat its cells; None: the
    mesh's). A cube above the hard limit raises; one above ``max_voxels`` is
    planned and says it does not fit (see :func:`describe_plan`), so the cost
    can be shown before anything runs."""
    if field not in fields_of(h):
        raise AmrViewError(f"The mesh has no field {field!r}: it has {', '.join(fields_of(h))}.")
    levels = h.levels
    if not levels:
        raise AmrViewError("The mesh has no leaf cell.")
    finest = int(levels[-1])
    a, b = region_box(h, lo, hi, unit)
    cap = int(min(max(int(max_voxels), 8), HARD_MAX_VOXELS))
    if level is None:
        level = next((lv for lv in range(finest if top is None else min(finest, int(top)), 0, -1)
                      if int(np.prod(_lattice(a, b, lv)[1], dtype=np.int64)) <= cap), 0)
    level = int(level)
    if not 0 <= level <= finest:
        raise AmrViewError(f"Level {level} is outside the mesh's: 0 to {finest} (the finest "
                           "leaf cells).")
    i0, shape = _lattice(a, b, level)
    plan = UniformPlan(field, level, i0, shape, weighting(h, field))
    if plan.voxels > HARD_MAX_VOXELS:
        raise AmrViewError(f"{' x '.join(map(str, shape))} is more than {HARD_MAX_VOXELS:,} "
                           "voxels: ask for a lower level or a smaller region.")
    return plan


def max_level_within(h: AmrHierarchy, lo: np.ndarray, hi: np.ndarray, cap: int) -> int:
    """The finest level (up to the mesh's) whose cube over ``lo`` .. ``hi`` has
    at most ``cap`` voxels; 0 when none does."""
    finest = int(h.levels[-1]) if h.levels else 0
    return next((lv for lv in range(finest, 0, -1)
                 if int(np.prod(_lattice(lo, hi, lv)[1], dtype=np.int64)) <= cap), 0)


def describe_plan(h: AmrHierarchy, plan: UniformPlan, max_voxels: int = DEFAULT_MAX_VOXELS,
                  finest: int | None = None) -> dict:
    """The plan as the route answers it: the grid in box units and physical
    ones, the cost, whether it fits, and ``finest``, the finest level inside
    the region (what "refine this box" would ask for), as :func:`plan_view`
    found it."""
    factor, length_unit = _length_factor(h)
    cap = int(min(max(int(max_voxels), 8), HARD_MAX_VOXELS))
    lower, upper = np.asarray(plan.lower), np.asarray(plan.upper)
    scale = value_scale(h, plan.field)
    out = {"field": plan.field, "level": plan.level, "shape": list(plan.shape),
           "voxels": plan.voxels, "bytes_estimate": plan.voxels * 4,
           "weighting": plan.weighting, "voxel_size": plan.dx, "voxel_size_physical": plan.dx * factor,
           "lower": list(plan.lower), "upper": list(plan.upper),
           "lower_physical": [v * factor for v in plan.lower],
           "upper_physical": [v * factor for v in plan.upper],
           "length_unit": length_unit, "value_unit": scale["unit"],
           "fits": plan.voxels <= cap, "max_voxels": cap, "hard_max_voxels": HARD_MAX_VOXELS,
           "max_level_within_limit": max_level_within(h, lower, upper, cap),
           "passes": plan.passes, "slab_planes": plan.slab_planes,
           "peak_memory_bytes_estimate": plan.slab_bytes + (1 << 28),
           "finest_level_in_region": finest,
           "levels": [int(v) for v in h.levels], "fields": fields_of(h),
           "extent": list(h.extent)}
    if not out["fits"]:
        out["reason"] = (f"{plan.voxels:,} voxels is more than the {cap:,} allowed: level "
                         f"{out['max_level_within_limit']} fits, or ask for a larger limit "
                         f"(up to {HARD_MAX_VOXELS:,}).")
    return out


def plan_view(h: AmrHierarchy, field: str, level: int | None = None,
              lo: Sequence[float] | None = None, hi: Sequence[float] | None = None,
              unit: str = "box", max_voxels: int = DEFAULT_MAX_VOXELS,
              scan: bool = True) -> tuple[UniformPlan, dict]:
    """``(plan, answer)`` of a view request: the plan and what the route says
    of it. With a region the finest level it holds is found first (one pass
    over the mesh's geometry; ``scan`` False skips it when the level is
    given) and an unset level is that level, or the finest that fits."""
    a, b = region_box(h, lo, hi, unit)
    whole = lo is None and hi is None
    finest = None
    if whole:
        finest = int(h.levels[-1]) if h.levels else None
    elif scan or level is None:
        finest = h.finest_level_in(a, b)
    plan = plan_uniform(h, field, level, lo, hi, unit, max_voxels, top=finest)
    return plan, describe_plan(h, plan, max_voxels, finest)


def value_scale(h: AmrHierarchy, field: str) -> dict:
    """``{"factor", "unit"}`` that makes a cube's values physical: the
    producer's scale of the field, 1 for the level, none-known as code."""
    if field == LEVEL:
        return {"factor": 1.0, "unit": ""}
    s = h.scales().get(field)
    return {"factor": float(s["factor"]), "unit": str(s["unit"])} if s else {
        "factor": 1.0, "unit": "code units"}


# ── Resampling ───────────────────────────────────────────────────────────────

def _slabs(plan: UniformPlan) -> Iterator[tuple[int, int]]:
    """Runs ``[k0, k1)`` of planes along z (local to the region) of a slab each."""
    step = plan.slab_planes
    for k0 in range(0, plan.shape[2], step):
        yield k0, min(plan.shape[2], k0 + step)


def _deposit(acc: np.ndarray, covered: np.ndarray, block: LeafBlock, value: np.ndarray,
             plan: UniformPlan, k0: int, mode: str, weight: np.ndarray | None = None,
             wacc: np.ndarray | None = None) -> int:
    """Put the leaves of one block into the slab starting at plane ``k0``
    (local index): ``acc`` (planes, ny, nx) and ``covered`` (the same, bool).
    With ``weight`` (the leaves' density) and ``wacc`` (the slab's weight sum,
    like ``acc``) the finer leaves of a voxel add ``rho v share`` to ``acc``
    and ``rho share`` to ``wacc``, and the caller divides; a coarser leaf
    gives ``v`` and 1, so the division returns its value. Returns the number
    of leaves that reached the slab."""
    nx, ny, _ = plan.shape
    dz = acc.shape[0]
    ox, oy, oz = plan.i0[0], plan.i0[1], plan.i0[2] + k0
    acc_flat, cov_flat = acc.reshape(-1), covered.reshape(-1)
    wacc_flat = None if wacc is None else wacc.reshape(-1)
    dxl, level, l = plan.dx, plan.level, block.level
    if l >= level:
        # Finer than the voxel (or as fine): inside exactly one, by its centre.
        g = np.floor(block.centre / dxl).astype(np.int64)
        gx, gy, gz = g[:, 0] - ox, g[:, 1] - oy, g[:, 2] - oz
        ok = (gx >= 0) & (gx < nx) & (gy >= 0) & (gy < ny) & (gz >= 0) & (gz < dz)
        if not ok.any():
            return 0
        flat = (gz[ok] * ny + gy[ok]) * nx + gx[ok]
        if mode == "max":
            np.maximum.at(acc_flat, flat, value[ok])
        elif wacc_flat is None:
            # Volume-weighted: the leaf's share of the voxel's volume.
            np.add.at(acc_flat, flat, value[ok] * 8.0 ** (level - l))
        else:
            # Density-weighted: its share of the voxel's mass.
            w = weight[ok] * 8.0 ** (level - l)
            np.add.at(acc_flat, flat, value[ok] * w)
            np.add.at(wacc_flat, flat, w)
        cov_flat[flat] = True
        return int(ok.sum())
    # Coarser than the voxel: it covers m^3 of them from its lower corner.
    m = 1 << (level - l)
    start = np.rint((block.centre - 0.5 * block.dx) / dxl).astype(np.int64)
    sx, sy, sz = start[:, 0] - ox, start[:, 1] - oy, start[:, 2] - oz
    ok = (sx < nx) & (sx + m > 0) & (sy < ny) & (sy + m > 0) & (sz < dz) & (sz + m > 0)
    if not ok.any():
        return 0
    sx, sy, sz, val = sx[ok], sy[ok], sz[ok], value[ok]
    if m <= _VECTOR_FILL:
        for a in range(m):
            for b in range(m):
                for c in range(m):
                    gx, gy, gz = sx + a, sy + b, sz + c
                    inside = (gx >= 0) & (gx < nx) & (gy >= 0) & (gy < ny) & (gz >= 0) & (gz < dz)
                    flat = (gz[inside] * ny + gy[inside]) * nx + gx[inside]
                    acc_flat[flat] = val[inside]
                    if wacc_flat is not None:
                        wacc_flat[flat] = 1.0
                    cov_flat[flat] = True
    else:
        for x0, y0, z0, v in zip(sx.tolist(), sy.tolist(), sz.tolist(), val.tolist()):
            window = (slice(max(z0, 0), min(z0 + m, dz)), slice(max(y0, 0), min(y0 + m, ny)),
                      slice(max(x0, 0), min(x0 + m, nx)))
            acc[window] = v
            if wacc is not None:
                wacc[window] = 1.0
            covered[window] = True
    return int(ok.sum())


def resample_slab(h: AmrHierarchy, plan: UniformPlan, k0: int, k1: int) -> tuple[np.ndarray, int]:
    """The field on planes ``k0`` .. ``k1`` of the plan (local, along z): a
    float64 array (planes, ny, nx) in code units (the level for ``level``,
    the density for ``mass``), NaN where no leaf reaches a voxel (or, in a
    density-weighted mean, where the leaves in it have no mass); and the
    finest level among the leaves that reached them (-1 when none)."""
    nx, ny, _ = plan.shape
    mode = "max" if plan.field == LEVEL else "mean"
    weighted = plan.weighting == DENSITY
    read = [] if plan.field == LEVEL else ["density" if plan.field == MASS else plan.field]
    if weighted:
        read.append("density")
    acc = np.full((k1 - k0, ny, nx), -1.0 if mode == "max" else 0.0, dtype=np.float64)
    wacc = np.zeros(acc.shape, dtype=np.float64) if weighted else None
    covered = np.zeros(acc.shape, dtype=bool)
    dx = plan.dx
    lo = np.array([plan.i0[0], plan.i0[1], plan.i0[2] + k0], dtype=np.float64) * dx
    hi = np.array([plan.i0[0] + nx, plan.i0[1] + ny, plan.i0[2] + k1], dtype=np.float64) * dx
    # A coarse leaf reaches in from up to half its side beyond its centre.
    pad = 0.5 * 2.0 ** -min(h.levels)
    finest = -1
    for block in h.leaf_blocks(read, None, lo - pad, hi + pad):
        value = (np.full(block.size, float(block.level)) if mode == "max"
                 else np.asarray(block.fields[read[0]], dtype=np.float64))
        rho = np.asarray(block.fields["density"], dtype=np.float64) if weighted else None
        if _deposit(acc, covered, block, value, plan, k0, mode, rho, wacc):
            finest = max(finest, block.level)
    if weighted:
        np.divide(acc, wacc, out=acc, where=wacc > 0)
        covered &= wacc > 0
    acc[~covered] = np.nan
    return acc, finest


def resample(h: AmrHierarchy, plan: UniformPlan, sink) -> int:
    """Run the plan: each slab of the cube, a float32 array (planes, ny, nx)
    of values in the run's code units (NaN outside the mesh), goes to ``sink``
    in order along z. Returns the finest level among the leaves that reached
    the cube. A mass voxel is code density times its volume in box units,
    the cell mass's own unit (``AmrHierarchy.scales()["mass"]``)."""
    factor = plan.dx ** 3 if plan.field == MASS else 1.0
    finest = -1
    for k0, k1 in _slabs(plan):
        values, f = resample_slab(h, plan, k0, k1)
        finest = max(finest, f)
        sink((values * factor).astype(np.float32))
    return finest


# ── The FITS cube ────────────────────────────────────────────────────────────

def _code_unit(field: str) -> str:
    """What a stored value is, per field: the run's code unit (the physical one
    is in AMRUNIT and in the units table, :func:`app.amr_source.view_scales`)."""
    low = field.lower()
    if field == LEVEL or low == "metallicity":
        return ""
    if field == MASS:
        return "code density x box volume"
    if re.fullmatch(r"v[xyz]|velocity(_[xyz])?", low):
        return "code velocity"
    return {"density": "code density", "pressure": "code pressure"}.get(low, "code units")


def build_header(h: AmrHierarchy, plan: UniformPlan, source: str):
    """The cube's header: linear X/Y/Z axes in box units (CRVAL the first
    voxel's centre, counted from the box's corner), BUNIT the code unit the
    values are stored in, and what it was made from and how (the AMR* cards:
    ``AMRSRC`` the output the run's units are read from, ``AMRFCT``/``AMRUNIT``
    the factor and unit that make a value physical, ``AMRWGT`` the weighting
    of the mean over finer leaves)."""
    from astropy.io import fits  # noqa: PLC0415

    factor, length_unit = _length_factor(h)
    scale = value_scale(h, plan.field)
    if plan.field == MASS:
        scale = {"factor": float(h.scales()[MASS]["factor"]), "unit": h.scales()[MASS]["unit"]}
    hdr = fits.Header()
    hdr["SIMPLE"] = True
    hdr["BITPIX"] = -32
    hdr["NAXIS"] = 3
    step = plan.dx
    for axis, (name, n, i0) in enumerate(zip("XYZ", plan.shape, plan.i0), start=1):
        hdr[f"NAXIS{axis}"] = int(n)
        hdr[f"CTYPE{axis}"] = name
        hdr[f"CUNIT{axis}"] = "box"
        hdr[f"CRPIX{axis}"] = 1.0
        hdr[f"CRVAL{axis}"] = (i0 + 0.5) * step
        hdr[f"CDELT{axis}"] = step
    # The unit the stored numbers are in: a run's code unit when its scales
    # convert (RAMSES), the physical unit itself when the producer stores
    # physical values (factor 1: a mesh read through yt).
    unit = str(scale.get("unit") or "")
    stored_physical = (scale.get("factor") == 1.0 and plan.field != LEVEL
                       and unit not in ("", "dimensionless", "none")
                       and not unit.startswith("code"))
    if stored_physical:
        hdr["BUNIT"] = scale["unit"]
    elif _code_unit(plan.field):
        hdr["BUNIT"] = _code_unit(plan.field)
    hdr["OBJECT"] = plan.field
    hdr["CREATOR"] = "VisIVO (AMR uniform cube)"
    hdr["DATE"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
    hdr["AMRSRC"] = (source.encode("ascii", "replace").decode(), "source output")
    hdr["AMRCODE"] = (h.code or "unknown", "the code that wrote it")
    hdr["AMRFLD"] = (plan.field, "field resampled")
    hdr["AMRLEV"] = (plan.level, "target level: voxel side 2^-level of the box")
    hdr["AMRMODE"] = ("max" if plan.field == LEVEL else "mean",
                      "leaves finer than a voxel: weighted mean, or finest level")
    hdr["AMRWGT"] = (plan.weighting, "weight of that mean: volume | density | finest")
    hdr["AMRFCT"] = (scale["factor"], "stored value x AMRFCT = value in AMRUNIT")
    hdr["AMRUNIT"] = (scale["unit"] or "none", "physical unit of the stored value")
    hdr["AMRLMAX"] = (int(h.levels[-1]), "finest level of the mesh")
    for d, name in enumerate("XYZ", start=1):
        hdr[f"AMRLO{d}"] = (plan.lower[d - 1], f"first voxel's lower edge, {name}, box units")
        hdr[f"AMRHI{d}"] = (plan.upper[d - 1], f"last voxel's upper edge, {name}, box units")
    hdr["AMRBOXL"] = (factor, f"{length_unit} per box unit (the axes are in box units)")
    description = h.describe()
    for key, card, comment in (("scale_factor", "AEXP", "expansion factor of the output"),
                               ("redshift", "REDSHIFT", "of the output"),
                               ("time", "AMRTIME", "time of the output, code units")):
        value = description.get(key)
        if isinstance(value, (int, float)) and math.isfinite(value):
            hdr[card] = (float(value), comment)
    hdr["HISTORY"] = ("A voxel holds the finest level among its leaves." if plan.weighting == FINEST
                      else "Leaf cells coarser than a voxel fill it; finer ones give their "
                           f"{plan.weighting}-weighted mean.")
    return hdr


def worker_amr_uniform(path: str, field: str, level: int | None, lo, hi, unit: str,
                       max_voxels: int, out_dir: str) -> dict:
    """Resample ``field`` of the AMR output ``path`` onto a uniform grid at
    ``level`` over the region (see :func:`plan_uniform`) into one FITS in
    ``out_dir``. Returns ``{plan, outputs: [{field, path, shape}], finest_level}``.
    The grid must fit ``max_voxels``."""
    from ..amr_source import open_hierarchy  # noqa: PLC0415

    h = open_hierarchy(path)
    plan, described = plan_view(h, field, level, lo, hi, unit, max_voxels, scan=False)
    if not described["fits"]:
        raise AmrViewError(described["reason"])
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S")
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=f"{_safe_name(Path(path).name.split('::')[0])}-{stamp}-",
                                 dir=out_dir))
    target = root / f"{_safe_name(field)}-L{plan.level}{VIEW_SUFFIX}"
    done = False
    try:
        header = build_header(h, plan, str(path))
        with open(target, "wb") as fh:
            fh.write(header.tostring(sep="", endcard=True, padding=True).encode("ascii"))
            finest = resample(h, plan, lambda block: fh.write(block.astype(">f4").tobytes()))
            fh.write(_padding(plan.voxels * 4))
        done = True
    finally:
        if not done:        # no half-written cube left in the exports directory
            shutil.rmtree(root, ignore_errors=True)
    described["finest_level_in_region"] = finest if finest >= 0 else None
    return {"plan": described,
            "outputs": [{"field": field, "path": str(target), "shape": list(plan.shape)}],
            "finest_level": described["finest_level_in_region"]}


# ── The refinement-level map of a slice ──────────────────────────────────────

def _drop_finest(by_level: dict[int, list[np.ndarray]], excess: int) -> int:
    """Drop ``excess`` rectangles from the finest levels of ``by_level`` (the
    last read of a level first, so what stays is its first leaves, as when
    nothing is dropped early); returns how many went."""
    dropped = 0
    while excess > dropped and by_level:
        level = max(by_level)
        chunks = by_level[level]
        need = excess - dropped
        if chunks[-1].shape[0] <= need:
            dropped += chunks.pop().shape[0]
            if not chunks:
                del by_level[level]
        else:
            chunks[-1] = chunks[-1][:chunks[-1].shape[0] - need]
            dropped += need
    return dropped


def level_map(h: AmrHierarchy, axis: str, coordinate: float, unit: str = "box",
              lo: Sequence[float] | None = None, hi: Sequence[float] | None = None,
              region_unit: str = "box", max_rects: int = DEFAULT_MAX_RECTS) -> dict:
    """The leaf cells the plane ``axis = coordinate`` cuts, as rectangles in
    the plane, per level: ``{"levels": {level: [[u0, v0, u1, v1], ...]}}`` (u, v
    the other two axes in x, y, z order, in the unit asked: ``"box"`` or
    ``"physical"``, the cube's own axes), ``counts`` per level (all of them,
    even when ``max_rects`` cuts the rectangles: the coarsest levels are kept
    and the rest is dropped as the leaves are read, so memory is ``max_rects``
    rectangles plus one block, whatever the slice holds),
    and the plane's area covered (the window's, when the leaves tile it).

    A leaf belongs to a plane that passes through its lower face or its
    inside, ``centre - dx/2 <= c < centre + dx/2``, so a plane on the edge
    between two cells is the upper one's."""
    if axis not in ("x", "y", "z"):
        raise AmrViewError("The axis is x, y or z.")
    if unit not in ("box", "physical"):
        raise AmrViewError("The coordinate's unit is 'box' or 'physical'.")
    k = "xyz".index(axis)
    factor = _length_factor(h)[0]
    c = float(coordinate) / (factor if unit == "physical" else 1.0)
    extent = h.extent
    if not (math.isfinite(c) and 0.0 <= c < extent[k]):
        raise AmrViewError(f"The plane {axis} = {coordinate} lies outside the run.")
    a, b = region_box(h, lo, hi, region_unit)
    u, v = [d for d in range(3) if d != k]
    pad = 0.5 * 2.0 ** -min(h.levels)
    wide_lo, wide_hi = a - pad, b + pad
    wide_lo[k], wide_hi[k] = c - pad, c + pad
    by_level: dict[int, list[np.ndarray]] = {}
    counts: dict[int, int] = {}
    kept = 0
    cap = max(int(max_rects), 0)
    area = 0.0
    for block in h.leaf_blocks((), None, wide_lo, wide_hi):
        reach = 0.5 * block.dx
        keep = (block.centre[:, k] - reach <= c) & (c < block.centre[:, k] + reach)
        for d in (u, v):
            keep &= (block.centre[:, d] + reach > a[d]) & (block.centre[:, d] - reach < b[d])
        if not keep.any():
            continue
        cc = block.centre[keep]
        rect = np.stack([cc[:, u] - reach, cc[:, v] - reach, cc[:, u] + reach, cc[:, v] + reach],
                        axis=1)
        counts[block.level] = counts.get(block.level, 0) + int(rect.shape[0])
        by_level.setdefault(block.level, []).append(rect)
        kept += int(rect.shape[0])
        kept -= _drop_finest(by_level, kept - cap)
        # Area inside the window (a cell may reach past it).
        w = np.clip(np.minimum(rect[:, 2], b[u]) - np.maximum(rect[:, 0], a[u]), 0, None)
        t = np.clip(np.minimum(rect[:, 3], b[v]) - np.maximum(rect[:, 1], a[v]), 0, None)
        area += float((w * t).sum())
    out_scale = factor if unit == "physical" else 1.0
    rects = {str(level): (np.concatenate(by_level[level]) * out_scale).tolist()
             if by_level.get(level) else [] for level in sorted(counts)}
    truncated = kept < sum(counts.values())
    counts = {str(level): n for level, n in sorted(counts.items())}
    return {"axis": axis, "coordinate": float(coordinate), "unit": unit,
            "plane_axes": ["xyz"[u], "xyz"[v]], "levels": rects, "counts": counts,
            "cells": int(sum(counts.values())), "truncated": truncated,
            "area": area, "window_area": float((b[u] - a[u]) * (b[v] - a[v])),
            "to_physical": factor, "length_unit": _length_factor(h)[1]}
