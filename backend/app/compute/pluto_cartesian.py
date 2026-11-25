"""PLUTO polar, cylindrical, spherical and stretched grids, resampled onto a
uniform Cartesian grid (issue #527).

A PLUTO run writes its variables on the cells of its own coordinates: (R, phi,
z), (R, z[, phi]) or (r, theta, phi), or a Cartesian grid whose cells grow with
the distance. ``pluto_source`` shows those as stored, in cell indices, which is
the truth of the file and no picture of the object. This module makes the
picture: it samples the variable, in the native coordinates, at the voxels of a
regular x/y(/z) grid, and writes a FITS cube that opens like any other volume.

What it produces, by the shape of the run:

* a 3-D polar, cylindrical or spherical run: a Cartesian cube (x, y, z);
* a 2-D polar run (R, phi): the x-y image;
* a 2-D axisymmetric run, (R, z) or (r, theta): the meridional x-z image, x
  being the cylindrical radius, optionally mirrored to negative x;
* a spherical run with a single theta cell and phi cells: the equatorial x-y
  image;
* a Cartesian run on stretched cells: the same axes, uniform.

Same pattern as the AREPO extraction (``arepo.py``): a worker the router runs
as a heavy job, a derived regular cube in the exports directory with its
provenance in the header, opened through the ordinary dataset route. Voxels
outside the domain (the hole inside rmin, the corners of the box, the part of a
wedge not covered) are NaN. Interpolation is trilinear between cell centres in
the native coordinates (cell centres taken half-way between the edges, which is
second-order equivalent to PLUTO's volume-weighted ones), periodic across the
phi seam of a full circle, constant inside the half cell next to the domain
edge. Vector components keep the native basis (``vx1`` is still the radial
velocity of a spherical run): a variable is resampled, not rotated.

Memory is bounded by the chunk: the output is produced CHUNK_VOXELS voxels at a
time (a run of the file's own order, so a plane larger than the chunk is split
too), straight into the file, and the source is read through its memory map.
"""
from __future__ import annotations

import itertools
import math
import re
import shutil
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

from .. import pluto_source as ps
from .arepo import _safe_name

#: Voxels of the default grid at most (float32: 4 bytes each). The user may ask
#: for more, up to HARD_MAX_VOXELS.
DEFAULT_MAX_VOXELS = 1 << 26
HARD_MAX_VOXELS = 1 << 30
#: Output voxels computed at a time: ~25 temporaries of 8 bytes each (about
#: 120 bytes a voxel in all), whatever the shape of the output.
CHUNK_VOXELS = 1 << 20
#: Samples per native axis when the bounding box of the domain is searched.
_BOX_SAMPLES = 65

GEOMETRIES = ("CARTESIAN", "POLAR", "CYLINDRICAL", "SPHERICAL")
#: What each native direction (x1, x2, x3) of a geometry is.
_NATIVE = {"CARTESIAN": ("x", "y", "z"), "POLAR": ("R", "phi", "z"),
           "CYLINDRICAL": ("R", "z", "phi"), "SPHERICAL": ("r", "theta", "phi")}
_ANGLES = {"phi", "theta"}


class PlutoCartesianError(ValueError):
    """The run cannot be shown this way, or the request is unusable."""


# ── What to produce ──────────────────────────────────────────────────────────

@dataclass(frozen=True)
class CartesianPlan:
    geometry: str
    mode: str                         # "cube" | "plane" | "meridional" | "grid"
    axes: tuple[str, ...]             # the output axes, FITS order: ("X", "Z")
    lower: tuple[float, ...]          # edge of the first voxel, per axis
    upper: tuple[float, ...]
    shape: tuple[int, ...]            # voxels per axis, FITS order
    mirror: bool                      # meridional image mirrored to negative x
    interpolation: str = "trilinear"

    @property
    def cell(self) -> tuple[float, ...]:
        return tuple((u - l) / n for l, u, n in zip(self.lower, self.upper, self.shape))

    @property
    def voxels(self) -> int:
        return int(np.prod(self.shape, dtype=np.int64))

    def as_dict(self) -> dict:
        return {"geometry": self.geometry, "mode": self.mode, "axes": list(self.axes),
                "lower": list(self.lower), "upper": list(self.upper),
                "shape": list(self.shape), "cell": list(self.cell), "mirror": self.mirror,
                "interpolation": self.interpolation, "voxels": self.voxels,
                "bytes_estimate": self.voxels * 4}


def _counts(grid: ps.PlutoGrid) -> tuple[int, int, int]:
    return tuple(a.n for a in grid.axes)   # type: ignore[return-value]


def view_mode(grid: ps.PlutoGrid) -> tuple[str, tuple[str, ...]]:
    """``(mode, output axes)`` for the run, from its geometry and which of its
    directions have more than one cell."""
    geometry = grid.geometry
    if geometry not in GEOMETRIES:
        raise PlutoCartesianError(
            f"{geometry.lower()} geometry has no Cartesian view; PLUTO's are "
            f"{', '.join(g.lower() for g in GEOMETRIES)}.")
    n1, n2, n3 = _counts(grid)
    if geometry == "CARTESIAN":
        active = tuple("XYZ"[k] for k in range(3) if _counts(grid)[k] > 1)
        if len(active) < 2:
            raise PlutoCartesianError("a 1-D run has no Cartesian view.")
        return ("cube" if len(active) == 3 else "grid"), active
    if sum(1 for n in (n1, n2, n3) if n > 1) < 2:
        raise PlutoCartesianError("a 1-D run has no Cartesian view.")
    if geometry == "POLAR":                      # (R, phi, z)
        if n2 == 1:
            return "meridional", ("X", "Z")
        return ("plane", ("X", "Y")) if n3 == 1 else ("cube", ("X", "Y", "Z"))
    if geometry == "CYLINDRICAL":                # (R, z, phi)
        if n3 == 1:
            return "meridional", ("X", "Z")
        return ("plane", ("X", "Y")) if n2 == 1 else ("cube", ("X", "Y", "Z"))
    if n3 == 1:                                  # SPHERICAL (r, theta, phi)
        return "meridional", ("X", "Z")
    return ("plane", ("X", "Y")) if n2 == 1 else ("cube", ("X", "Y", "Z"))


def _forward(geometry: str, native: list[np.ndarray]):
    """Cartesian ``(X, Y, Z)`` of native ``(a1, a2, a3)`` (broadcast arrays);
    for a meridional image X is the cylindrical radius and Y is unused."""
    a1, a2, a3 = native
    if geometry == "CARTESIAN":
        return a1, a2, a3
    if geometry == "POLAR":
        return a1 * np.cos(a2), a1 * np.sin(a2), a3
    if geometry == "CYLINDRICAL":
        return a1 * np.cos(a3), a1 * np.sin(a3), a2
    s = a1 * np.sin(a2)
    return s * np.cos(a3), s * np.sin(a3), a1 * np.cos(a2)


def _axis_samples(axis: ps.PlutoAxis, role: str) -> np.ndarray:
    """Native positions that bracket the domain along one direction: its edges
    and a regular fill, plus the angles where a trigonometric extreme falls."""
    lo, hi = float(axis.left[0]), float(axis.right[-1])
    if axis.n == 1 and role in _ANGLES:
        # An axisymmetric run has one phi cell; a spherical image of the
        # equator has one theta cell, and is read at theta = pi/2.
        return np.array([0.0 if role == "phi" else math.pi / 2])
    pts = [np.linspace(lo, hi, _BOX_SAMPLES)]
    if role in _ANGLES:
        step = math.pi / 2
        pts.append(np.arange(math.ceil(lo / step), math.floor(hi / step) + 1) * step)
    return np.unique(np.concatenate(pts))


def _bounds(grid: ps.PlutoGrid, mode: str, axes: tuple[str, ...], mirror: bool):
    """The box of the domain in the output axes."""
    roles = _NATIVE[grid.geometry]
    samples = [_axis_samples(a, r) for a, r in zip(grid.axes, roles)]
    mesh = np.meshgrid(*samples, indexing="ij")
    X, Y, Z = _forward(grid.geometry, mesh)
    if mode == "meridional":
        # Axisymmetric: the cylindrical radius is the horizontal axis whatever
        # the azimuth of the samples.
        X = np.hypot(X, Y)
    by_name = {"X": X, "Y": Y, "Z": Z}
    lower, upper = [], []
    for name in axes:
        values = by_name[name][np.isfinite(by_name[name])]
        lower.append(float(values.min()))
        upper.append(float(values.max()))
    if mode == "meridional":
        # sin(pi) is 1e-16, not 0: the image of a full sphere starts at x = 0.
        if abs(lower[0]) < 1e-9 * (upper[0] - lower[0]):
            lower[0] = 0.0
        if mirror:
            upper[0] = max(upper[0], -lower[0])
            lower[0] = -upper[0]
    return tuple(lower), tuple(upper)


def _finest_length(grid: ps.PlutoGrid) -> float:
    """The smallest cell width along the directions that are lengths (not the
    angles: their arc length shrinks to nothing at the centre, and a grid
    sized to it would be unbounded)."""
    roles = _NATIVE[grid.geometry]
    widths = [float(np.min(a.right - a.left)) for a, r in zip(grid.axes, roles)
              if r not in _ANGLES and a.n > 1]
    return min(widths) if widths else 0.0


def plan_cartesian(grid: ps.PlutoGrid, shape=None, max_voxels: int = DEFAULT_MAX_VOXELS,
                   mirror: bool = False) -> CartesianPlan:
    """The grid the run is resampled onto. ``shape`` (voxels per output axis,
    FITS order) wins; else the cells are as fine as the finest of the run's
    length cells, and the grid is coarsened to fit ``max_voxels``."""
    mode, axes = view_mode(grid)
    mirror = bool(mirror) and mode == "meridional"
    lower, upper = _bounds(grid, mode, axes, mirror)
    extent = [u - l for l, u in zip(lower, upper)]
    if not all(e > 0 and math.isfinite(e) for e in extent):
        raise PlutoCartesianError("the grid has no extent to resample.")
    if shape is not None:
        shape = tuple(int(n) for n in shape)
        if len(shape) != len(axes) or any(n < 1 for n in shape):
            raise PlutoCartesianError(
                f"give {len(axes)} voxel counts ({', '.join(axes)}), each at least 1.")
        if int(np.prod(shape, dtype=np.int64)) > HARD_MAX_VOXELS:
            raise PlutoCartesianError(
                f"{' x '.join(map(str, shape))} is more than {HARD_MAX_VOXELS:,} voxels.")
    else:
        h = _finest_length(grid) or max(extent) / 128.0
        shape = [max(2, math.ceil(e / h)) for e in extent]
        total = int(np.prod(shape, dtype=np.int64))
        cap = int(min(max(int(max_voxels), 8), HARD_MAX_VOXELS))
        if total > cap:
            scale = (cap / total) ** (1.0 / len(shape))
            shape = [max(2, int(n * scale)) for n in shape]
        shape = tuple(shape)
    return CartesianPlan(grid.geometry, mode, axes, lower, upper, shape, mirror)


# ── Where each voxel falls in the native grid ────────────────────────────────

def _periodic(axis: ps.PlutoAxis) -> bool:
    return abs((axis.right[-1] - axis.left[0]) - 2.0 * math.pi) < 1e-6


def _locate(axis: ps.PlutoAxis, a: np.ndarray | None, periodic: bool, azimuth: bool = False):
    """``(i0, i1, w1, inside)`` of positions ``a`` along one direction: the two
    cells to blend and the weight of the second; None positions (a direction
    the image does not use) sit on its only cell."""
    n = axis.n
    if a is None or n == 1:
        return None
    lo, hi = float(axis.left[0]), float(axis.right[-1])
    eps = 1e-9 * (hi - lo)
    c = axis.centres
    if azimuth and not periodic:
        # arctan2 gives (-pi, pi]; a wedge or half disc may lie anywhere in
        # [lo, lo + 2 pi), so bring the angle there (a hair below lo is lo).
        a = lo + np.mod(a - lo + eps, 2.0 * math.pi) - eps
    if periodic:
        a = lo + np.mod(a - lo, 2.0 * math.pi)
        ext = np.concatenate([[c[-1] - 2.0 * math.pi], c, [c[0] + 2.0 * math.pi]])
        j = np.clip(np.searchsorted(ext, a, side="right") - 1, 0, n)
        t = (a - ext[j]) / (ext[j + 1] - ext[j])
        return (j - 1) % n, j % n, t, np.ones(a.shape, bool)
    inside = (a >= lo - eps) & (a <= hi + eps)
    i0 = np.clip(np.searchsorted(c, a, side="right") - 1, 0, n - 2)
    t = np.clip((a - c[i0]) / (c[i0 + 1] - c[i0]), 0.0, 1.0)
    return i0, i0 + 1, t, inside


def _native(grid: ps.PlutoGrid, plan: CartesianPlan, out: dict[str, np.ndarray]):
    """Native coordinates of the output voxels ``out`` ({"X": …}), a list of
    three arrays (None for a direction the image does not use)."""
    geometry, mode = grid.geometry, plan.mode
    if geometry == "CARTESIAN":
        return [out.get("XYZ"[k]) for k in range(3)]
    with np.errstate(invalid="ignore", divide="ignore"):
        if mode == "meridional":
            horizontal = np.abs(out["X"]) if plan.mirror else out["X"]
            if geometry == "SPHERICAL":
                r = np.hypot(horizontal, out["Z"])
                return [r, np.where(r > 0, np.arccos(out["Z"] / r), np.nan), None]
            if geometry == "POLAR":                  # (R, phi, z): z is the third
                return [horizontal, None, out["Z"]]
            return [horizontal, out["Z"], None]      # CYLINDRICAL (R, z, phi)
        radius = np.hypot(out["X"], out["Y"])
        phi = np.arctan2(out["Y"], out["X"])
        z = out.get("Z")
        if geometry == "POLAR":
            return [radius, phi, z]
        if geometry == "CYLINDRICAL":
            return [radius, z, phi]
        if mode == "plane":
            return [radius, None, phi]
        r = np.sqrt(radius ** 2 + z ** 2)
        return [r, np.where(r > 0, np.arccos(z / r), np.nan), phi]


def sample_chunk(grid: ps.PlutoGrid, plan: CartesianPlan, out: dict[str, np.ndarray]):
    """For the voxels ``out``: ``(inside, corners)`` — a mask of those in the
    domain, and for each of the (up to eight) neighbouring cells the flat index
    into a (nx3, nx2, nx1) variable and its weight, both over the inside voxels
    only."""
    native = _native(grid, plan, out)
    roles = _NATIVE[grid.geometry]
    located = [_locate(ax, a, r == "phi" and _periodic(ax), r == "phi")
               for ax, a, r in zip(grid.axes, native, roles)]
    inside = np.ones(next(a for a in out.values()).shape, bool)
    for loc, a in zip(located, native):
        if loc is not None:
            inside &= loc[3]
        elif a is not None and a.size:
            inside &= np.isfinite(a)
    # A direction with one cell or unused takes that cell with weight 1; its
    # 'corners' are one.
    per_axis = []
    for ax, loc in zip(grid.axes, located):
        if loc is None:
            per_axis.append([(np.zeros(int(inside.sum()), np.int64), None)])
        else:
            i0, i1, t, _ = loc
            per_axis.append([(i0[inside], 1.0 - t[inside]), (i1[inside], t[inside])])
    n1, n2 = grid.axes[0].n, grid.axes[1].n
    corners = []
    for (k1, w1), (k2, w2), (k3, w3) in itertools.product(*per_axis):
        weight = 1.0
        for w in (w1, w2, w3):
            if w is not None:
                weight = weight * w
        corners.append(((k3 * n2 + k2) * n1 + k1, weight))
    return inside, corners


# ── The resampling ───────────────────────────────────────────────────────────

def _voxel_axes(plan: CartesianPlan, lo: int, hi: int) -> dict[str, np.ndarray]:
    """Cartesian centres of the voxels ``lo`` to ``hi`` - 1 in the file's order
    (C order, slowest axis outermost), as flat arrays. A run of that order, not
    whole planes, so the chunk is the same size whatever the shape."""
    slow_first = list(zip(plan.axes[::-1], plan.lower[::-1], plan.upper[::-1], plan.shape[::-1]))
    index = np.unravel_index(np.arange(lo, hi, dtype=np.int64), [n for *_, n in slow_first])
    return {name: l + (i + 0.5) * ((u - l) / n)
            for (name, l, u, n), i in zip(slow_first, index)}


def _chunk_bounds(plan: CartesianPlan) -> list[tuple[int, int]]:
    """Runs of at most CHUNK_VOXELS voxels covering the output, in file order."""
    total, step = plan.voxels, max(1, int(CHUNK_VOXELS))
    return [(a, min(total, a + step)) for a in range(0, total, step)]


def resample(grid: ps.PlutoGrid, plan: CartesianPlan, fields: list[np.ndarray], sinks):
    """Resample each of ``fields`` ((nx3, nx2, nx1) arrays or memory maps) onto
    the plan's grid; each chunk of float32 values, NaN outside the domain,
    goes to the matching callable of ``sinks`` in turn, slowest axis first."""
    flat = [f.reshape(-1) for f in fields]
    for lo, hi in _chunk_bounds(plan):
        out = _voxel_axes(plan, lo, hi)
        count = next(iter(out.values())).size
        inside, corners = sample_chunk(grid, plan, out)
        for src, sink in zip(flat, sinks):
            values = np.full(count, np.nan, dtype=np.float32)
            if corners and inside.any():
                acc = np.zeros(int(inside.sum()), dtype=np.float64)
                for index, weight in corners:
                    w = np.broadcast_to(np.asarray(weight, dtype=np.float64), acc.shape)
                    # A neighbour with weight exactly 0 contributes nothing: NaN * 0
                    # is NaN, and a blanked cell would smear onto voxels that sit
                    # on the cell next to it.
                    live = w != 0.0
                    if live.all():
                        acc += src[index] * w
                    else:
                        acc[live] += src[index[live]] * w[live]
                values[inside] = acc
            sink(values)


def build_header(plan: CartesianPlan, var: ps.PlutoVariable, source_ref: str):
    """The header of the cube: linear X/Y/Z axes at the voxel spacing (CRVAL
    the first centre, as the Cartesian PLUTO header has it), the variable's
    own keywords, and what it was made from and how."""
    from astropy.io import fits  # noqa: PLC0415

    # The variable's own cards (name, time, step, output number, source
    # geometry) are written by pluto_source.pluto_fits_header: start from them
    # and replace what a view changes, the axes, the bit depth and the grid kind.
    base = ps.pluto_fits_header(var)
    hdr = fits.Header()
    hdr["SIMPLE"] = True
    hdr["BITPIX"] = -32
    hdr["NAXIS"] = len(plan.shape)
    for axis, (name, n, l, h) in enumerate(zip(plan.axes, plan.shape, plan.lower, plan.cell),
                                           start=1):
        hdr[f"NAXIS{axis}"] = int(n)
        hdr[f"CTYPE{axis}"] = name
        hdr[f"CRPIX{axis}"] = 1.0
        hdr[f"CRVAL{axis}"] = l + 0.5 * h
        hdr[f"CDELT{axis}"] = h
    for card in base.cards:
        if card.keyword in ("SIMPLE", "BITPIX", "NAXIS", "PLUTOWHY") or re.fullmatch(
                r"(NAXIS|CTYPE|CRPIX|CRVAL|CDELT|CNAME)\d", card.keyword):
            continue
        hdr.append(card)
    hdr["PLUTOGRD"] = ("UNIFORM", "axes are coordinates (code units)")
    hdr["CREATOR"] = "VisIVO (PLUTO Cartesian view)"
    hdr["DATE"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
    hdr[ps.SOURCE_KEY] = (source_ref, "PLUTO variable this was resampled from")
    hdr["PLUTOVEW"] = ("CARTESIAN", "resampled onto a uniform Cartesian grid")
    hdr["PLUTOMOD"] = (plan.mode, "cube | plane | meridional | grid")
    hdr["PLUTOINT"] = (plan.interpolation, "interpolation in the native coordinates")
    hdr["PLUTOMIR"] = (plan.mirror, "meridional image mirrored to negative x")
    hdr["HISTORY"] = ("Cells outside the source domain are NaN; vector components keep "
                      "the native basis.")
    return hdr


def _padding(nbytes: int) -> bytes:
    return b"\0" * (-nbytes % 2880)


def worker_pluto_cartesian(path: str, fields: list[str] | None, shape, max_voxels: int,
                           mirror: bool, out_dir: str) -> dict:
    """Resample variables of the PLUTO output ``path`` (a variable reference or
    a data file) onto a uniform Cartesian grid, one FITS per variable.

    ``fields`` None: the variable ``path`` names; ``["*"]``: every cell
    variable of the output on disk. Returns ``{plan, outputs:[{field, path,
    shape}], source_geometry}``."""
    first = ps.resolve(path)
    if first is None:
        raise PlutoCartesianError(f"{path} is not a PLUTO output.")
    grid = first.grid
    plan = plan_cartesian(grid, shape, max_voxels, mirror)
    present = {f["name"]: f["path"] for f in ps.pluto_fields(ps.pluto_ref(first.data_path, first.name))}
    names = [first.name] if not fields else (list(present) if list(fields) == ["*"] else list(fields))
    names = list(dict.fromkeys(names))          # a variable once, not twice
    unknown = [n for n in names if n not in present]
    if unknown:
        raise PlutoCartesianError(f"output {first.record.index} has no cell variable "
                                  f"{', '.join(unknown)} on disk (it has {', '.join(present)}).")
    variables = [ps.resolve(present[n]) for n in names]

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S")
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=f"{_safe_name(Path(first.data_path).stem)}-{stamp}-",
                                 dir=out_dir))
    files, outputs, handles = [], [], []
    done = False
    try:
        for var in variables:
            target = root / f"{_safe_name(var.name)}{ps.VIEW_SUFFIX}"
            header = build_header(plan, var, ps.pluto_ref(var.data_path, var.name))
            fh = open(target, "wb")
            handles.append(fh)
            fh.write(header.tostring(sep="", endcard=True, padding=True).encode("ascii"))
            files.append(target)
            outputs.append({"field": var.name, "path": str(target), "shape": list(plan.shape)})
        sources = [np.memmap(v.data_path, dtype=v.dtype, mode="r", shape=v.shape, offset=v.offset)
                   for v in variables]
        resample(grid, plan, sources,
                 [(lambda block, fh=fh: fh.write(block.astype(">f4").tobytes())) for fh in handles])
        for fh in handles:
            fh.write(_padding(plan.voxels * 4))
        done = True
    finally:
        for fh in handles:
            fh.close()
        if not done:        # no half-written cubes left in the exports directory
            shutil.rmtree(root, ignore_errors=True)
    return {"plan": plan.as_dict(), "outputs": outputs, "source_geometry": grid.geometry}
