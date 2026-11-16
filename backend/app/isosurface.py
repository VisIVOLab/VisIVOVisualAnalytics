"""Isosurfaces of a cube's field at chosen levels, for the headset (issue #370).

A surface is extracted on the server from the original data: the cube itself
(level 0) or a level of its brick pyramid (``app.brick_pyramid``), the finest
whose voxels fit ``max_voxels`` unless a level is asked for. The level is read
slab by slab along z (never the whole cube in memory), each slab contoured with
VTK's flying edges, the slabs' surfaces joined (their shared planes' vertices
merged), then decimated to a triangle budget (quadric decimation) and given
vertex normals. Each surface is cached under (the file's version, the field,
the level value, the scale, the level of detail, the budget).

Levels are values in the field's units. A field on a log scale (a simulation's
density over many decades) is contoured on log10 of its values at log10 of the
level: the surface crosses the same voxel edges as on the values themselves,
and its vertices are placed along each edge in log space, where such a field
varies smoothly. Blank voxels (NaN, and values at or below zero on a log scale)
count as below every level.

Where the vertices are (``coordinates``):

* in the mesh sent to the headset (:func:`encode_mesh`), box coordinates: 0..1
  over the cube's full-resolution extent per axis (x, y, z), voxel ``i`` of the
  cube at ``(i + 0.5) / N``, as the headset's volume is drawn; quantised to 16
  bits over the mesh's own bounding box, normals to 8 bits;
* in an export (:func:`to_ply`, :func:`to_glb`), the cube's world coordinates
  when its three axes are linear lengths (a simulation's X, Y, Z: CRVAL +
  (pixel + 1 - CRPIX) * CDELT), else its full-resolution voxel indices
  (0-based, x, y, z). A pyramid level's voxel ``i`` stands for the centre of
  its block, ``i * 2^l + (2^l - 1) / 2`` cube voxels (``region_level``).
"""
from __future__ import annotations

import hashlib
import io
import json
import math
import secrets
import struct
import threading
import time
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Any, Callable

import numpy as np

#: The finest level of detail whose voxels fit this many is contoured unless
#: a level is asked for: 512³ is level 2 of a 2048³ cube (three surfaces of
#: the PLUTO density in 3.0 s, issue #370), a 256³ cube is its own grid.
DEFAULT_MAX_VOXELS = 512 ** 3
#: A level asked for explicitly may hold at most this many voxels.
HARD_MAX_VOXELS = 1024 ** 3
#: The triangles a surface is decimated to by default, and the bounds of the
#: request's budget. 200 k triangles a surface keep three surfaces under the
#: headset's mesh capacity (vr/js/isosurface.js MAX_TRIANGLES).
DEFAULT_MAX_TRIANGLES = 200_000
MIN_TRIANGLES = 1_000
MAX_TRIANGLES = 1_000_000
#: The surfaces of one request before decimation may hold at most this many
#: triangles together (every level's slabs are held until the last slab,
#: about 30 bytes a triangle: 480 MB): an automatic level of detail is
#: coarsened until they fit, an asked-for one is refused.
RAW_MAX_TRIANGLES = 16_000_000
#: A surface of more than this many times its budget is first clustered to
#: about this many times it (quadric clustering), then decimated.
CLUSTER_ABOVE = 3
#: The clustering grid's cells at most (dense, 72 bytes each: 288 MB).
MAX_CLUSTER_CELLS = 4_000_000
#: At most this many levels in one request.
MAX_LEVELS = 8
#: Slabs are read and contoured at most this many bytes (float32) at a time.
SLAB_BYTES = 64 * 1024 ** 2
#: The cache's bytes (wire meshes and the arrays kept for exports).
CACHE_BYTES = 512 * 1024 ** 2
#: Blank voxels: far below every level, finite so that flying edges'
#: interpolation never meets an infinity (a vertex beside a blank voxel sits
#: on the finite one).
_BLANK = np.float32(-1.0e30)

MAGIC = b"VISO"
WIRE_VERSION = 1
#: Bytes of the wire header: magic, version, vertices, triangles, bbox lo, bbox hi.
HEADER_BYTES = 4 + 4 + 4 + 4 + 12 + 12


class IsosurfaceTooLarge(ValueError):
    """The surface is too large to decimate at the level of detail asked for."""


class IsosurfaceCancelled(Exception):
    """The client cancelled the extraction."""


# ── Levels of detail ────────────────────────────────────────────────────────


def choose_lod(level_shapes: list[tuple[int, int, int]], max_voxels: int, lod: int = -1) -> int:
    """The level to contour: ``lod`` when it is one (>= 0), else the finest
    level whose voxels are at most ``max_voxels`` (the coarsest when none is)."""
    if lod >= 0:
        if lod >= len(level_shapes):
            raise ValueError(f"Level of detail {lod} is not one of the cube's levels 0..{len(level_shapes) - 1}.")
        return int(lod)
    for k, shape in enumerate(level_shapes):
        if int(np.prod(shape, dtype=np.int64)) <= int(max_voxels):
            return k
    return len(level_shapes) - 1


def level_scale(lod: int, source: str) -> tuple[int, float]:
    """(factor, offset): level voxel ``i`` is cube voxel ``i * factor + offset``."""
    from .region_level import voxel_offset  # noqa: PLC0415
    return 1 << int(lod), voxel_offset(lod, source)


class LevelReader:
    """Slabs of one level of detail of a cube: from its brick pyramid when one
    is complete and current (``pyramid_path``), else the cube's own samples
    every ``2^lod`` voxels (``strided``; level 0 is the cube). Never builds a
    pyramid, never holds the level whole."""

    def __init__(self, path: str, lod: int, pyramid_path: str | None) -> None:
        from .region_level import cube_shape, default_level_shapes  # noqa: PLC0415
        from .volume_source import volume_shape  # noqa: PLC0415

        self.path = path
        self.lod = int(lod)
        self._pyr = None
        self._data = None
        self._ctx = None
        full, _hdu = volume_shape(path, None)
        self.shape0 = tuple(int(n) for n in cube_shape(full))
        if len(self.shape0) != 3:
            raise ValueError("An isosurface needs a 3-D cube.")
        self.source = "cube" if self.lod == 0 else "strided"
        if self.lod > 0 and pyramid_path:
            from .brick_pyramid import BrickPyramid  # noqa: PLC0415
            try:
                pyr = BrickPyramid(pyramid_path, source=path)
            except (RuntimeError, ValueError, OSError):
                pyr = None   # stale or unreadable since it was looked for: strided
            if pyr is not None and self.lod < pyr.levels:
                self._pyr = pyr
                self.source = "pyramid"
                self.shape = pyr.shape(self.lod)
            elif pyr is not None:
                pyr.close()
        if self._pyr is None:
            shapes = default_level_shapes(self.shape0)
            if self.lod >= len(shapes):
                raise ValueError(f"Level of detail {self.lod} is not one of the cube's levels.")
            self.shape = tuple(int(n) for n in shapes[self.lod])
            from .volume_source import volume_data  # noqa: PLC0415
            self._ctx = volume_data(path, None)
            data = self._ctx.__enter__()
            while data.ndim > 3:
                data = data[0]
            self._data = data

    def slab(self, z0: int, z1: int) -> np.ndarray:
        """Planes ``z0 .. z1 - 1`` of the level (z, y, x), NaN blank: float32
        from a pyramid, the file's own float type from the cube (a float64
        PLUTO .dbl is not cast here: transform_block takes its logarithm
        first), other types as float64."""
        if self._pyr is not None:
            ny, nx = self.shape[1], self.shape[2]
            return self._pyr.read_region(self.lod, (slice(z0, z1), slice(0, ny), slice(0, nx)),
                                         ghost=0, edge="nan")
        f = 1 << self.lod
        block = np.asarray(self._data[z0 * f:(z1 - 1) * f + 1:f, ::f, ::f])
        if block.dtype.kind != "f":
            block = block.astype(np.float64)
        return block

    def close(self) -> None:
        if self._pyr is not None:
            self._pyr.close()
            self._pyr = None
        if self._ctx is not None:
            self._ctx.__exit__(None, None, None)
            self._ctx = None
            self._data = None

    def __enter__(self) -> "LevelReader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def slab_ranges(depth: int, plane_bytes: int, max_bytes: int | None = None) -> list[tuple[int, int]]:
    """Slabs ``[z0, z1)`` covering planes ``0 .. depth - 1``, each sharing its
    first plane with the previous one's last (the cells between two slabs
    belong to one of them only), at most ``max_bytes`` (:data:`SLAB_BYTES`)
    each."""
    max_bytes = SLAB_BYTES if max_bytes is None else int(max_bytes)
    planes = max(2, int(max_bytes // max(1, plane_bytes)))
    out = []
    z0 = 0
    while z0 < depth - 1:
        z1 = min(depth, z0 + planes)
        out.append((z0, z1))
        z0 = z1 - 1
    return out or [(0, depth)]


# ── Values ──────────────────────────────────────────────────────────────────


def transform_level(level: float, scale: str) -> float:
    """The level in the space the field is contoured in."""
    level = float(level)
    if not math.isfinite(level):
        raise ValueError(f"The level {level} is not a finite number.")
    if scale == "log":
        if level <= 0.0:
            raise ValueError(f"The level {level:g} has no logarithm: a field on a log "
                             "scale takes levels above zero.")
        return math.log10(level)
    return level


_F32_MAX = float(np.finfo(np.float32).max)


def transform_block(block: np.ndarray, scale: str) -> np.ndarray:
    """The block in contouring space as float32, blank voxels at ``_BLANK``.
    Non-finite values (NaN, ±inf) are blank on both scales. The logarithm is
    taken in the block's own precision before the cast, so a float64 value
    beyond float32's range or below its smallest keeps its logarithm; on a
    linear scale such values are clamped to float32's range, not made
    infinite."""
    a = np.asarray(block)
    if a.dtype.kind != "f":
        a = a.astype(np.float64)
    fin = np.isfinite(a)
    out = np.full(a.shape, _BLANK, dtype=np.float32)
    if scale == "log":
        pos = fin & (a > 0)
        out[pos] = np.log10(a[pos]).astype(np.float32)
        return out
    out[fin] = np.clip(a[fin], -_F32_MAX, _F32_MAX).astype(np.float32)
    return out


def field_range(path: str, pyramid_path: str | None) -> tuple[float, float]:
    """(min, max) of the field's finite values, cheaply: from the brick
    statistics of the pyramid's level 0 when there is one, else from its
    coarsest default level read every ``2^l`` voxels (an estimate; used only to
    resolve the ``auto`` scale)."""
    if pyramid_path:
        from .brick_pyramid import BrickPyramid  # noqa: PLC0415
        try:
            with BrickPyramid(pyramid_path, source=path) as pyr:
                lo, hi = pyr.stats(0)
            with np.errstate(invalid="ignore"):
                if np.isfinite(lo).any():
                    return float(np.nanmin(lo)), float(np.nanmax(hi))
        except (RuntimeError, ValueError, OSError):
            pass
    from .region_level import cube_shape, default_level_shapes  # noqa: PLC0415
    from .volume_source import volume_shape  # noqa: PLC0415
    full, _ = volume_shape(path, None)
    top = len(default_level_shapes(cube_shape(full))) - 1
    with LevelReader(path, top, None) as reader:
        block = reader.slab(0, reader.shape[0])
    fin = block[np.isfinite(block)]
    return (float(fin.min()), float(fin.max())) if fin.size else (0.0, 0.0)


def resolve_scale(path: str, requested: str, pyramid_path: str | None) -> str:
    """``linear`` or ``log``: as asked, or for ``auto`` the reduced cube's rule
    (cube_reduce.resolve_scale: log for a simulation volume whose values are
    positive over more than three decades)."""
    from . import cube_reduce  # noqa: PLC0415
    if requested != "auto":
        return cube_reduce.resolve_scale(requested, 0.0, 0.0)
    from .compute.reduced import is_simulation  # noqa: PLC0415
    from .fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    try:
        simulation = is_simulation(ScientificFitsDataset(path))
    except Exception:  # noqa: BLE001 - a header that cannot be read: not a simulation
        simulation = False
    if not simulation:
        return "linear"
    lo, hi = field_range(path, pyramid_path)
    return cube_reduce.resolve_scale("auto", lo, hi, True)


# ── Extraction ──────────────────────────────────────────────────────────────


@dataclass
class Mesh:
    """One surface: vertices in box coordinates (x, y, z), unit normals in box
    coordinates, triangles (vertex indices, counter-clockwise seen from the
    side the field is above the level)."""
    level: float
    points: np.ndarray            # (n, 3) float32, box coordinates
    normals: np.ndarray           # (n, 3) float32, unit
    triangles: np.ndarray         # (m, 3) uint32
    raw_triangles: int = 0        # before decimation
    meta: dict[str, Any] = field(default_factory=dict)


def _vtk():
    import vtk  # noqa: PLC0415
    from vtk.util.numpy_support import numpy_to_vtk, numpy_to_vtkIdTypeArray, vtk_to_numpy  # noqa: PLC0415
    return vtk, numpy_to_vtk, numpy_to_vtkIdTypeArray, vtk_to_numpy


def contour_slab(block: np.ndarray, value: float, z0: int) -> tuple[np.ndarray, np.ndarray]:
    """The surface at ``value`` of a slab (z, y, x float32 in contouring
    space) whose first plane is level plane ``z0``: (points (n, 3) float32 in
    level voxels x, y, z; triangles (m, 3) int32)."""
    vtk, _np2vtk, _ids, vtk_to_numpy = _vtk()
    depth, height, width = block.shape
    if depth < 2 or height < 2 or width < 2:
        return np.zeros((0, 3), np.float32), np.zeros((0, 3), np.int32)
    raw = np.ascontiguousarray(block, dtype=np.float32)
    image = vtk.vtkImageImport()
    image.SetImportVoidPointer(raw, 1)   # raw outlives the filter below
    image.SetDataScalarTypeToFloat()
    image.SetNumberOfScalarComponents(1)
    image.SetDataExtent(0, width - 1, 0, height - 1, 0, depth - 1)
    image.SetWholeExtent(0, width - 1, 0, height - 1, 0, depth - 1)
    image.SetDataSpacing(1.0, 1.0, 1.0)
    image.SetDataOrigin(0.0, 0.0, float(z0))
    contour = vtk.vtkFlyingEdges3D()
    contour.SetInputConnection(image.GetOutputPort())
    contour.SetValue(0, float(value))
    contour.ComputeNormalsOff()
    contour.ComputeGradientsOff()
    contour.ComputeScalarsOff()
    contour.Update()
    out = contour.GetOutput()
    n = out.GetNumberOfPoints()
    if n == 0 or out.GetNumberOfPolys() == 0:
        return np.zeros((0, 3), np.float32), np.zeros((0, 3), np.int32)
    pts = vtk_to_numpy(out.GetPoints().GetData()).astype(np.float32, copy=True).reshape(-1, 3)
    # int32 while the slabs are kept (a slab has far fewer than 2^31 vertices).
    conn = vtk_to_numpy(out.GetPolys().GetConnectivityArray()).astype(np.int32, copy=True)
    return pts, conn.reshape(-1, 3)


def _polydata(points: np.ndarray, triangles: np.ndarray):
    vtk, numpy_to_vtk, numpy_to_vtkIdTypeArray, _ = _vtk()
    poly = vtk.vtkPolyData()
    vpts = vtk.vtkPoints()
    vpts.SetData(numpy_to_vtk(np.ascontiguousarray(points, dtype=np.float32), deep=True))
    poly.SetPoints(vpts)
    m = len(triangles)
    offsets = numpy_to_vtkIdTypeArray(np.arange(0, 3 * m + 1, 3, dtype=np.int64), deep=True)
    conn = numpy_to_vtkIdTypeArray(np.ascontiguousarray(triangles, dtype=np.int64).reshape(-1), deep=True)
    cells = vtk.vtkCellArray()
    cells.SetData(offsets, conn)
    poly.SetPolys(cells)
    return poly


def _arrays(poly) -> tuple[np.ndarray, np.ndarray]:
    _vtk_mod, _a, _b, vtk_to_numpy = _vtk()
    if poly.GetNumberOfPoints() == 0 or poly.GetNumberOfPolys() == 0:
        return np.zeros((0, 3), np.float32), np.zeros((0, 3), np.int64)
    pts = vtk_to_numpy(poly.GetPoints().GetData()).astype(np.float32, copy=True).reshape(-1, 3)
    conn = vtk_to_numpy(poly.GetPolys().GetConnectivityArray()).astype(np.int64, copy=True)
    return pts, conn.reshape(-1, 3)


def merge_and_decimate(points: np.ndarray, triangles: np.ndarray, budget: int) -> tuple[np.ndarray, np.ndarray]:
    """The slabs' surface with coincident vertices merged (the planes slabs
    share), decimated to at most ``budget`` triangles (quadric decimation,
    in level voxels so the error is isotropic)."""
    vtk = _vtk()[0]
    if not len(triangles):
        return points, triangles
    clean = vtk.vtkStaticCleanPolyData()
    clean.SetInputData(_polydata(points, triangles))
    clean.SetTolerance(0.0)
    clean.SetAbsoluteTolerance(0.0)
    clean.ToleranceIsAbsoluteOn()
    clean.ConvertLinesToPointsOff()
    clean.ConvertPolysToLinesOff()
    clean.ConvertStripsToPolysOff()
    clean.Update()
    poly = clean.GetOutput()
    n = poly.GetNumberOfPolys()
    if n > CLUSTER_ABOVE * budget:
        # Quadric decimation of millions of triangles takes seconds a million
        # (8.4 s for 2.4 M on the PLUTO shell); quadric clustering on a grid
        # of cells about sqrt(n / (3 budget)) voxels wide brings it to about
        # three times the budget in a fraction of that, for the decimation to
        # finish (0.5 + 1.2 s there, issue #370).
        # Its grid is dense (72 bytes a cell): the cells are made larger when
        # more than MAX_CLUSTER_CELLS would be needed, and the clustering is
        # left out when that would leave fewer triangles than the budget
        # (the decimation alone then; 708 k triangles on a 512³ grid would
        # otherwise have asked for 104 M cells, 7.5 GB).
        b = poly.GetBounds()
        ext = [max(1.0, b[2 * i + 1] - b[2 * i]) for i in range(3)]
        cell = max(math.sqrt(n / (CLUSTER_ABOVE * budget)),
                   (ext[0] * ext[1] * ext[2] / MAX_CLUSTER_CELLS) ** (1.0 / 3.0))
        if n / (cell * cell) >= budget:
            divs = [max(2, int(math.ceil(e / cell)) + 1) for e in ext]
            qc = vtk.vtkQuadricClustering()
            qc.SetInputData(poly)
            qc.AutoAdjustNumberOfDivisionsOff()
            qc.SetNumberOfDivisions(*divs)
            qc.Update()
            poly = qc.GetOutput()
            n = poly.GetNumberOfPolys()
    if n > budget:
        dec = vtk.vtkQuadricDecimation()
        dec.SetInputData(poly)
        dec.SetTargetReduction(1.0 - budget / float(n))
        dec.VolumePreservationOn()
        dec.AttributeErrorMetricOff()
        dec.Update()
        poly = dec.GetOutput()
        # The target is approximate: a little over the budget is trimmed by a
        # second, smaller pass.
        if poly.GetNumberOfPolys() > budget:
            dec2 = vtk.vtkQuadricDecimation()
            dec2.SetInputData(poly)
            dec2.SetTargetReduction(1.0 - 0.98 * budget / float(poly.GetNumberOfPolys()))
            dec2.VolumePreservationOn()
            dec2.Update()
            poly = dec2.GetOutput()
    # The budget is a bound: a decimation that stopped above it (it may, on a
    # surface whose edges it cannot collapse) is followed by clustering on
    # cells sized from the surface's area, coarser each time, until it fits.
    cell = None
    for _ in range(8):
        n = poly.GetNumberOfPolys()
        if n <= budget:
            break
        pts, tris = _arrays(poly)
        b = poly.GetBounds()
        ext = [max(1.0, b[2 * i + 1] - b[2 * i]) for i in range(3)]
        want = math.sqrt(2.0 * max(surface_area(pts, tris), 1e-9) / (0.8 * budget))
        cell = max(want if cell is None else cell * 1.25, want,
                   (ext[0] * ext[1] * ext[2] / MAX_CLUSTER_CELLS) ** (1.0 / 3.0))
        qc = vtk.vtkQuadricClustering()
        qc.SetInputData(poly)
        qc.AutoAdjustNumberOfDivisionsOff()
        qc.SetNumberOfDivisions(*[max(2, int(math.ceil(e / cell)) + 1) for e in ext])
        qc.Update()
        poly = qc.GetOutput()
    if poly.GetNumberOfPolys() > budget:
        raise IsosurfaceTooLarge(f"The surface could not be brought to {budget:,} triangles "
                                 f"({poly.GetNumberOfPolys():,} left): ask a larger budget.")
    return _arrays(poly)


def vertex_normals(points: np.ndarray, triangles: np.ndarray) -> np.ndarray:
    """Area-weighted vertex normals from the triangles' winding (unit; a
    vertex of no triangle gets +z)."""
    p = points.astype(np.float64)
    t = triangles
    face = np.cross(p[t[:, 1]] - p[t[:, 0]], p[t[:, 2]] - p[t[:, 0]])
    acc = np.zeros_like(p)
    for k in range(3):
        np.add.at(acc, t[:, k], face)
    norm = np.linalg.norm(acc, axis=1)
    out = np.zeros_like(p)
    ok = norm > 0
    out[ok] = acc[ok] / norm[ok, None]
    out[~ok] = (0.0, 0.0, 1.0)
    return out.astype(np.float32)


def extract(path: str, levels: list[float], *, scale: str = "auto", lod: int = -1,
            max_voxels: int = DEFAULT_MAX_VOXELS, max_triangles: int = DEFAULT_MAX_TRIANGLES,
            pyramid_path: str | None = None,
            progress: Callable[[float, str], None] | None = None,
            should_stop: Callable[[], bool] | None = None) -> list[Mesh]:
    """The surfaces of the cube ``path`` at ``levels`` (field units), one
    :class:`Mesh` each (empty when a level is outside the field's values).

    With ``lod`` -1 the level of detail is the finest whose voxels fit
    ``max_voxels``, coarsened while a surface has more than
    :data:`RAW_MAX_TRIANGLES` before decimation; an asked-for ``lod`` with
    such a surface is refused (:class:`IsosurfaceTooLarge`)."""
    from .region_level import cube_shape, default_level_shapes  # noqa: PLC0415
    from .volume_source import volume_shape  # noqa: PLC0415

    t_start = time.perf_counter()
    report = progress or (lambda f, s: None)
    stop = should_stop or (lambda: False)
    full, _hdu = volume_shape(path, None)
    shape0 = tuple(int(n) for n in cube_shape(full))
    if len(shape0) != 3:
        raise ValueError("An isosurface needs a 3-D cube.")
    shapes = [tuple(int(n) for n in s) for s in default_level_shapes(shape0)]
    if pyramid_path:
        from .brick_pyramid import BrickPyramid  # noqa: PLC0415
        try:
            with BrickPyramid(pyramid_path, source=path) as pyr:
                shapes = [pyr.shape(k) for k in range(pyr.levels)]
        except (RuntimeError, ValueError, OSError):
            pyramid_path = None
    auto = lod < 0
    chosen = choose_lod(shapes, max_voxels, lod)
    if not auto and int(np.prod(shapes[chosen], dtype=np.int64)) > HARD_MAX_VOXELS:
        raise IsosurfaceTooLarge(
            f"Level of detail {chosen} is {'x'.join(map(str, shapes[chosen][::-1]))} voxels, more "
            f"than the {HARD_MAX_VOXELS:,} an isosurface may be extracted from: ask a coarser level.")
    used_scale = resolve_scale(path, scale, pyramid_path)
    values = [transform_level(v, used_scale) for v in levels]
    budget = int(max_triangles)
    nz0, ny0, nx0 = shape0
    while True:
        try:
            return _extract_at(path, chosen, levels, values, used_scale, budget, pyramid_path,
                               (nx0, ny0, nz0), report, stop, t_start)
        except IsosurfaceTooLarge:
            if not auto or chosen >= len(shapes) - 1:
                raise
            chosen += 1
            report(0.0, f"coarser level {chosen}: the surface was too large")


def _extract_at(path, lod, levels, values, scale, budget, pyramid_path, full_xyz,
                report, stop, t_start) -> list[Mesh]:
    t0 = time.perf_counter()
    with LevelReader(path, lod, pyramid_path) as reader:
        nz, ny, nx = reader.shape
        slabs = slab_ranges(nz, 4 * ny * nx)
        parts: list[list[tuple[np.ndarray, np.ndarray]]] = [[] for _ in values]
        counts = [0] * len(values)
        read_s = contour_s = 0.0
        for i, (z0, z1) in enumerate(slabs):
            if stop():
                raise IsosurfaceCancelled("Cancelled by the client.")
            ta = time.perf_counter()
            block = transform_block(reader.slab(z0, z1), scale)
            tb = time.perf_counter()
            for k, v in enumerate(values):
                pts, tris = contour_slab(block, v, z0)
                if len(tris):
                    parts[k].append((pts, tris))
                    counts[k] += len(tris)
                    # Every level's slabs are held until the last slab is
                    # read (about 30 bytes a triangle): the cap is on their sum.
                    if sum(counts) > RAW_MAX_TRIANGLES:
                        many = len(values) > 1
                        which = ", ".join(f"{x:g}" for x in levels)
                        raise IsosurfaceTooLarge(
                            f"The surface{'s' if many else ''} at {which} {'have' if many else 'has'} "
                            f"more than {RAW_MAX_TRIANGLES:,} triangles{' together' if many else ''} "
                            f"at level of detail {lod} ({nx}x{ny}x{nz} voxels): ask a coarser "
                            "level, fewer levels or other values.")
            read_s += tb - ta
            contour_s += time.perf_counter() - tb
            report(0.7 * (i + 1) / len(slabs), f"contouring {i + 1}/{len(slabs)}")
        source = reader.source
    factor, offset = level_scale(lod, source)
    full = np.array(full_xyz, dtype=np.float64)
    meshes = []
    for k, v in enumerate(values):
        if stop():
            raise IsosurfaceCancelled("Cancelled by the client.")
        td = time.perf_counter()
        if parts[k]:
            base = 0
            pts_all, tris_all = [], []
            for pts, tris in parts[k]:
                pts_all.append(pts)
                tris_all.append(tris.astype(np.int64) + base)
                base += len(pts)
            points = np.concatenate(pts_all)
            triangles = np.concatenate(tris_all)
        else:
            points = np.zeros((0, 3), np.float32)
            triangles = np.zeros((0, 3), np.int64)
        parts[k] = []
        raw = int(len(triangles))
        points, triangles = merge_and_decimate(points, triangles, budget)
        # Level voxels → cube voxels → box coordinates.
        cube_vox = points.astype(np.float64) * factor + offset
        box = ((cube_vox + 0.5) / full).astype(np.float32)
        normals = vertex_normals(box, triangles) if len(triangles) else np.zeros((0, 3), np.float32)
        meshes.append(Mesh(
            level=float(levels[k]), points=box, normals=normals,
            triangles=triangles.astype(np.uint32), raw_triangles=raw,
            meta={"lod": int(lod), "source": source, "scale": scale, "contour_value": float(v),
                  "grid": [int(nx), int(ny), int(nz)], "factor": int(factor), "voxel_offset": float(offset),
                  "full_shape": [int(n) for n in full_xyz],
                  "decimate_s": round(time.perf_counter() - td, 3)}))
        report(0.7 + 0.3 * (k + 1) / len(values), f"decimating {k + 1}/{len(values)}")
    total = time.perf_counter() - t_start
    for m in meshes:
        m.meta.update(read_s=round(read_s, 3), contour_s=round(contour_s, 3),
                      seconds=round(total, 3), extract_s=round(time.perf_counter() - t0, 3))
    return meshes


# ── Geometry checks (tests, the report) ─────────────────────────────────────


def surface_area(points: np.ndarray, triangles: np.ndarray) -> float:
    p = points.astype(np.float64)
    t = triangles.astype(np.int64)
    return float(0.5 * np.linalg.norm(np.cross(p[t[:, 1]] - p[t[:, 0]], p[t[:, 2]] - p[t[:, 0]]), axis=1).sum())


def enclosed_volume(points: np.ndarray, triangles: np.ndarray) -> float:
    """|Σ v0 · (v1 × v2)| / 6: the volume a closed, consistently wound surface encloses."""
    p = points.astype(np.float64)
    t = triangles.astype(np.int64)
    return float(abs(np.einsum("ij,ij->i", p[t[:, 0]], np.cross(p[t[:, 1]], p[t[:, 2]])).sum()) / 6.0)


# ── The headset's mesh (wire format) ────────────────────────────────────────


def encode_mesh(mesh: Mesh) -> bytes:
    """The compact mesh the headset draws, little-endian:

    * header (40 bytes): ``b"VISO"``, uint32 version (1), uint32 vertices
      ``n``, uint32 triangles ``m``, float32 ``lo[3]``, float32 ``hi[3]`` (the
      bounding box in box coordinates);
    * positions: ``n * 3`` uint16, x y z, ``lo + q / 65535 * (hi - lo)``;
      then zero padding to a multiple of 4 bytes;
    * normals: ``n * 4`` int8, x y z and 0, ``q / 127`` (box coordinates);
    * triangles: ``m * 3`` uint32 vertex indices.
    """
    n, m = len(mesh.points), len(mesh.triangles)
    if n:
        lo = mesh.points.min(axis=0).astype(np.float32)
        hi = mesh.points.max(axis=0).astype(np.float32)
    else:
        lo = hi = np.zeros(3, np.float32)
    span = np.where(hi > lo, hi - lo, 1.0).astype(np.float64)
    q = np.rint((mesh.points.astype(np.float64) - lo) / span * 65535.0)
    pos = np.clip(q, 0, 65535).astype("<u2")
    nrm = np.zeros((n, 4), dtype=np.int8)
    if n:
        nrm[:, :3] = np.clip(np.rint(mesh.normals * 127.0), -127, 127).astype(np.int8)
    out = io.BytesIO()
    out.write(MAGIC)
    out.write(struct.pack("<III", WIRE_VERSION, n, m))
    out.write(np.asarray(lo, "<f4").tobytes())
    out.write(np.asarray(hi, "<f4").tobytes())
    out.write(pos.tobytes())
    pad = (-(6 * n)) % 4
    out.write(b"\0" * pad)
    out.write(nrm.tobytes())
    out.write(np.ascontiguousarray(mesh.triangles, dtype="<u4").tobytes())
    return out.getvalue()


def decode_mesh(raw: bytes) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """(points, normals, triangles) of :func:`encode_mesh`'s bytes."""
    if raw[:4] != MAGIC:
        raise ValueError("Not an isosurface mesh.")
    version, n, m = struct.unpack_from("<III", raw, 4)
    if version != WIRE_VERSION:
        raise ValueError(f"Mesh version {version}, this reader knows {WIRE_VERSION}.")
    lo = np.frombuffer(raw, "<f4", 3, 16).astype(np.float64)
    hi = np.frombuffer(raw, "<f4", 3, 28).astype(np.float64)
    at = HEADER_BYTES
    q = np.frombuffer(raw, "<u2", 3 * n, at).reshape(n, 3).astype(np.float64)
    at += 6 * n + ((-(6 * n)) % 4)
    nrm = np.frombuffer(raw, "i1", 4 * n, at).reshape(n, 4)[:, :3].astype(np.float32) / 127.0
    at += 4 * n
    tris = np.frombuffer(raw, "<u4", 3 * m, at).reshape(m, 3)
    span = np.where(hi > lo, hi - lo, 1.0)
    return (lo + q / 65535.0 * span).astype(np.float32), nrm, tris


# ── Exports ─────────────────────────────────────────────────────────────────

_LINEAR_CTYPES = {"X", "Y", "Z", ""}


def world_axes(path: str) -> list[dict[str, Any]] | None:
    """The cube's three axes (x, y, z) as {crval, crpix, cdelt, ctype, cunit}
    when every one is a linear length (a simulation's X, Y, Z), else None (sky
    or spectral axes: the export stays in voxels)."""
    from .fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    try:
        ds = ScientificFitsDataset(path)
        header = ds.header
        axes = list(ds.active_axes)[:3]
    except Exception:  # noqa: BLE001 - no header: voxels
        return None
    if len(axes) < 3:
        return None
    out = []
    for a in axes:
        ctype = str(header.get(f"CTYPE{a}", "")).strip().upper()
        if ctype not in _LINEAR_CTYPES or str(header.get("PLUTOGRD", "")).upper() == "INDEX":
            return None
        out.append({"crval": float(header.get(f"CRVAL{a}", 0.0)), "crpix": float(header.get(f"CRPIX{a}", 1.0)),
                    "cdelt": float(header.get(f"CDELT{a}", 1.0)), "ctype": ctype,
                    "cunit": str(header.get(f"CUNIT{a}", "")).strip()})
    return out


def export_points(mesh: Mesh, axes: list[dict[str, Any]] | None) -> tuple[np.ndarray, np.ndarray, str]:
    """(points, normals, frame): world coordinates with ``axes``, else
    full-resolution voxel indices (0-based)."""
    full = np.array(mesh.meta["full_shape"], dtype=np.float64)
    vox = mesh.points.astype(np.float64) * full - 0.5
    if axes is None:
        out, frame, per_box = vox, "voxel", full
    else:
        crval = np.array([a["crval"] for a in axes])
        crpix = np.array([a["crpix"] for a in axes])
        cdelt = np.array([a["cdelt"] for a in axes])
        out, frame, per_box = crval + (vox + 1.0 - crpix) * cdelt, "world", full * cdelt
    # Normals are covectors: a box unit is per_box output units, so they are
    # divided by it (a negative CDELT turns them with the axis).
    n = mesh.normals.astype(np.float64) / per_box[None, :]
    norm = np.linalg.norm(n, axis=1)
    n[norm > 0] /= norm[norm > 0, None]
    return out.astype(np.float32), n.astype(np.float32), frame


def to_ply(mesh: Mesh, axes: list[dict[str, Any]] | None, comment: str = "") -> bytes:
    """Binary little-endian PLY: float x y z nx ny nz per vertex, one uchar-
    counted int list per face."""
    pts, nrm, frame = export_points(mesh, axes)
    n, m = len(pts), len(mesh.triangles)
    lines = ["ply", "format binary_little_endian 1.0",
             "comment VisIVO Visual Analytics isosurface",
             f"comment level {mesh.level!r} scale {mesh.meta.get('scale', '')} "
             f"lod {mesh.meta.get('lod', '')} source {mesh.meta.get('source', '')}",
             f"comment coordinates {frame}" + ("" if axes is None else " " + " ".join(
                 f"{'xyz'[i]}:{a['ctype'] or '-'}[{a['cunit'] or '-'}]" for i, a in enumerate(axes)))]
    if comment:
        lines.append("comment " + comment.replace("\n", " ")[:200])
    lines += [f"element vertex {n}", "property float x", "property float y", "property float z",
              "property float nx", "property float ny", "property float nz",
              f"element face {m}", "property list uchar int vertex_indices", "end_header"]
    head = ("\n".join(lines) + "\n").encode("ascii", "replace")
    verts = np.empty((n, 6), dtype="<f4")
    verts[:, :3] = pts
    verts[:, 3:] = nrm
    faces = np.empty(m, dtype=[("n", "u1"), ("i", "<i4", (3,))])
    faces["n"] = 3
    faces["i"] = mesh.triangles.astype(np.int32)
    return head + verts.tobytes() + faces.tobytes()


def to_glb(mesh: Mesh, axes: list[dict[str, Any]] | None, color=(0.8, 0.8, 0.8, 1.0)) -> bytes:
    """A binary glTF 2.0 (.glb): one mesh, positions, normals and uint32
    indices, a double-sided material of ``color``. glTF's axes are y up; the
    cube's x, y, z are written as they are (a viewer may need to turn it)."""
    pts, nrm, frame = export_points(mesh, axes)
    pts = np.ascontiguousarray(pts, "<f4")
    nrm = np.ascontiguousarray(nrm, "<f4")
    idx = np.ascontiguousarray(mesh.triangles, "<u4").reshape(-1)
    blobs = [pts.tobytes(), nrm.tobytes(), idx.tobytes()]
    views, offset = [], 0
    data = b""
    for i, b in enumerate(blobs):
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(b),
                      "target": 34963 if i == 2 else 34962})
        data += b + b"\0" * ((-len(b)) % 4)
        offset = len(data)
    lo = pts.min(axis=0).tolist() if len(pts) else [0, 0, 0]
    hi = pts.max(axis=0).tolist() if len(pts) else [0, 0, 0]
    gltf = {
        "asset": {"version": "2.0", "generator": "VisIVO Visual Analytics"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": f"isosurface {mesh.level:g}"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}],
                    "extras": {"level": mesh.level, "coordinates": frame, **{k: mesh.meta[k] for k in ("scale", "lod", "source") if k in mesh.meta}}}],
        "materials": [{"doubleSided": True, "pbrMetallicRoughness": {
            "baseColorFactor": [float(c) for c in color], "metallicFactor": 0.0, "roughnessFactor": 0.8}}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(pts), "type": "VEC3", "min": lo, "max": hi},
            {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5125, "count": int(idx.size), "type": "SCALAR"},
        ],
        "bufferViews": views, "buffers": [{"byteLength": len(data)}],
    }
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js += b" " * ((-len(js)) % 4)
    total = 12 + 8 + len(js) + 8 + len(data)
    return (struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(js), b"JSON") + js
            + struct.pack("<I4s", len(data), b"BIN\0") + data)


# ── Cache ───────────────────────────────────────────────────────────────────


@dataclass
class Cached:
    mesh_id: str
    mesh: Mesh
    wire: bytes
    path: str
    describe: dict[str, Any]

    @property
    def nbytes(self) -> int:
        return (len(self.wire) + self.mesh.points.nbytes + self.mesh.normals.nbytes
                + self.mesh.triangles.nbytes)


class MeshCache:
    """The surfaces made, by key and by mesh id, least recently used dropped
    past ``max_bytes``."""

    def __init__(self, max_bytes: int = CACHE_BYTES) -> None:
        self.max_bytes = int(max_bytes)
        self._by_id: OrderedDict[str, Cached] = OrderedDict()
        self._lock = threading.Lock()
        self.hits = 0
        self.misses = 0

    @staticmethod
    def key(identity: str, path: str, level: float, scale: str, lod: str, budget: int) -> str:
        """The surface's key: the file's version and path, the level, the
        scale, the level of detail asked for (with where it is read from) and
        the budget."""
        return json.dumps([identity, path, repr(float(level)), scale, str(lod), int(budget)])

    @staticmethod
    def mesh_id(key: str) -> str:
        """The surface's id: a hash of its key salted per process, so an id
        cannot be made up from a path and a level by someone who was not
        given it."""
        return hashlib.sha256((_SALT + key).encode("utf-8")).hexdigest()[:24]

    def get(self, mesh_id: str) -> Cached | None:
        with self._lock:
            c = self._by_id.get(mesh_id)
            if c is not None:
                self._by_id.move_to_end(mesh_id)
            return c

    def lookup(self, key: str) -> Cached | None:
        c = self.get(self.mesh_id(key))
        with self._lock:
            if c is None:
                self.misses += 1
            else:
                self.hits += 1
        return c

    def put(self, key: str, mesh: Mesh, path: str, describe: dict[str, Any]) -> Cached:
        mid = self.mesh_id(key)
        c = Cached(mid, mesh, encode_mesh(mesh), path, {**describe, "mesh_id": mid})
        with self._lock:
            self._by_id[mid] = c
            self._by_id.move_to_end(mid)
            total = sum(x.nbytes for x in self._by_id.values())
            while total > self.max_bytes and len(self._by_id) > 1:
                _, old = self._by_id.popitem(last=False)
                total -= old.nbytes
        return c

    def clear(self) -> None:
        with self._lock:
            self._by_id.clear()
            self.hits = self.misses = 0


_SALT = secrets.token_hex(16)
CACHE = MeshCache()
#: One extraction at a time: each reads a level of the cube and holds its
#: surfaces until they are decimated.
EXTRACT_LOCK = threading.Lock()


def describe(mesh: Mesh, wire_bytes: int) -> dict[str, Any]:
    return {"level": mesh.level, "vertices": int(len(mesh.points)), "triangles": int(len(mesh.triangles)),
            "raw_triangles": int(mesh.raw_triangles), "bytes": int(wire_bytes), **mesh.meta}
