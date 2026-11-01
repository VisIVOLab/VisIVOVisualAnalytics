"""Which blocks of a brick pyramid a view draws, and at which level (issue #70).

The renderer draws a cube larger than its GPU from the brick pyramid
(:mod:`app.brick_pyramid`): fine blocks where the view needs detail, coarse ones
elsewhere, within a memory budget. This module chooses them; it reads nothing
and draws nothing.

The blocks form an octree over the pyramid: a node is a cube of ``node``
voxels per side at some level (a multiple of the brick, so a node reads whole
chunks), its children are the up to eight nodes of the level below that cover
the same region. Starting from the coarsest nodes, the node whose voxels look
largest on screen is replaced by its children while the budget allows and its
voxels are larger than ``target_px`` pixels. Nodes outside the view frustum,
nodes with no finite voxel, and nodes entirely below the opacity threshold are
left out: they would draw nothing.

Coordinates are those of level 0 in voxels, axis order (x, y, z) as VTK and the
camera use them; the pyramid's arrays are (z, y, x).
"""
from __future__ import annotations

import heapq
import math
from dataclasses import dataclass, field
from typing import Sequence

import numpy as np


@dataclass(frozen=True, order=True)
class Node:
    """A block of the octree: ``level`` and its index along x, y, z at that
    level, in nodes."""
    level: int
    ix: int
    iy: int
    iz: int


@dataclass(frozen=True)
class Camera:
    """The view, in level-0 voxel coordinates (x, y, z).

    ``view_angle`` is the vertical field of view in degrees (perspective); with
    ``parallel_scale`` set the projection is parallel and it is half the height
    of the view in world units. ``planes`` are the six frustum planes as
    (a, b, c, d) with a*x + b*y + c*z + d >= 0 inside (VTK's
    ``GetFrustumPlanes`` order and sign), or None for no culling."""
    position: tuple[float, float, float]
    viewport_height_px: int
    view_angle: float = 30.0
    parallel_scale: float | None = None
    planes: Sequence[tuple[float, float, float, float]] | None = None
    # The direction of projection (unit vector, from the camera into the
    # scene): sizes are measured at a block's nearest depth along it. Without
    # it, the distance to the block's nearest point is used, which
    # underestimates blocks seen off-axis.
    direction: tuple[float, float, float] | None = None


@dataclass
class PyramidLayout:
    """What the choice needs from a pyramid: the brick, the shape of every level
    (z, y, x) and, optionally, the per-brick maxima to leave out what the
    threshold hides."""
    brick: int
    shapes: list[tuple[int, int, int]]
    brick_max: list[np.ndarray] | None = None       # per level, (gz, gy, gx)
    _node_max: dict = field(default_factory=dict, repr=False)

    @classmethod
    def of(cls, pyramid, with_stats: bool = True) -> "PyramidLayout":
        """The layout of an opened :class:`~app.brick_pyramid.BrickPyramid`.

        The maxima are not the pyramid's own per-level ones: those are of the
        averaged data, and an average can hide a bright voxel that the finer
        levels show. Each level's maxima are instead the largest of level 0
        under each brick (max-pooled up from level 0), so a block is left out
        only if nothing beneath it reaches the threshold."""
        shapes = [pyramid.shape(level) for level in range(pyramid.levels)]
        maxima = None
        if with_stats:
            maxima = [pyramid.stats(0)[1]]
            for level in range(1, pyramid.levels):
                maxima.append(_pool_max(maxima[-1], tuple(-(-n // pyramid.brick) for n in shapes[level])))
        return cls(pyramid.brick, shapes, maxima)


def _pool_max(grid: np.ndarray, out_shape: tuple[int, int, int]) -> np.ndarray:
    """The maximum of each 2x2x2 group of ``grid`` over its finite values (NaN
    where none is finite), cut to ``out_shape``."""
    pad = tuple((0, 2 * o - n) if 2 * o >= n else (0, 0) for n, o in zip(grid.shape, out_shape))
    padded = np.pad(grid, pad, constant_values=np.nan)[:2 * out_shape[0], :2 * out_shape[1], :2 * out_shape[2]]
    cells = padded.reshape(out_shape[0], 2, out_shape[1], 2, out_shape[2], 2)
    finite = np.isfinite(cells)
    pooled = np.where(finite, cells, -np.inf).max(axis=(1, 3, 5))
    return np.where(finite.any(axis=(1, 3, 5)), pooled, np.nan).astype(np.float32)


def node_box(layout: PyramidLayout, node_size: int, node: Node) -> tuple[np.ndarray, np.ndarray]:
    """The node's extent in level-0 voxels, (x, y, z) lower and upper corners,
    clipped to the cube."""
    scale = 2 ** node.level
    nz, ny, nx = layout.shapes[0]
    lo = np.array([node.ix, node.iy, node.iz], dtype=float) * node_size * scale
    hi = np.minimum(lo + node_size * scale, [nx, ny, nz])
    return lo, hi


def node_region(layout: PyramidLayout, node_size: int, node: Node) -> tuple[slice, slice, slice]:
    """The node's (z, y, x) slices into its level's array."""
    nz, ny, nx = layout.shapes[node.level]
    return (slice(node.iz * node_size, min(nz, (node.iz + 1) * node_size)),
            slice(node.iy * node_size, min(ny, (node.iy + 1) * node_size)),
            slice(node.ix * node_size, min(nx, (node.ix + 1) * node_size)))


def _grid(layout: PyramidLayout, node_size: int, level: int) -> tuple[int, int, int]:
    nz, ny, nx = layout.shapes[level]
    return (math.ceil(nx / node_size), math.ceil(ny / node_size), math.ceil(nz / node_size))


def _children(layout: PyramidLayout, node_size: int, node: Node) -> list[Node]:
    if node.level == 0:
        return []
    gx, gy, gz = _grid(layout, node_size, node.level - 1)
    return [Node(node.level - 1, x, y, z)
            for z in (2 * node.iz, 2 * node.iz + 1) if z < gz
            for y in (2 * node.iy, 2 * node.iy + 1) if y < gy
            for x in (2 * node.ix, 2 * node.ix + 1) if x < gx]


def _node_voxels(layout: PyramidLayout, node_size: int, node: Node, halo: int = 0) -> int:
    zs, ys, xs = node_region(layout, node_size, node)
    return ((zs.stop - zs.start + 2 * halo) * (ys.stop - ys.start + 2 * halo)
            * (xs.stop - xs.start + 2 * halo))


def _node_max(layout: PyramidLayout, node_size: int, node: Node, halo: int = 0) -> float:
    """The largest finite value in the node (NaN if none), from the bricks it
    covers; with ``halo``, also the bricks around it, whose values the drawn
    node interpolates up to its faces."""
    key = (node_size, node, bool(halo))
    if key not in layout._node_max:
        b = layout.brick
        per = node_size // b
        m = 1 if halo else 0
        grid = layout.brick_max[node.level]
        tile = grid[max(0, node.iz * per - m):(node.iz + 1) * per + m,
                    max(0, node.iy * per - m):(node.iy + 1) * per + m,
                    max(0, node.ix * per - m):(node.ix + 1) * per + m]
        finite = tile[np.isfinite(tile)]
        layout._node_max[key] = float(finite.max()) if finite.size else math.nan
    return layout._node_max[key]


def _visible(camera: Camera, lo: np.ndarray, hi: np.ndarray) -> bool:
    """False when the box is entirely outside one of the frustum planes."""
    if not camera.planes:
        return True
    for a, b, c, d in camera.planes:
        # The corner farthest along the plane's normal: if even it is
        # outside, the whole box is.
        px = hi[0] if a >= 0 else lo[0]
        py = hi[1] if b >= 0 else lo[1]
        pz = hi[2] if c >= 0 else lo[2]
        if a * px + b * py + c * pz + d < 0:
            return False
    return True


def projected_voxel_px(layout: PyramidLayout, node_size: int, node: Node, camera: Camera) -> float:
    """How many screen pixels one voxel of ``node`` spans, at the node's point
    nearest the camera."""
    voxel = float(2 ** node.level)
    if camera.parallel_scale:
        return voxel * camera.viewport_height_px / (2.0 * camera.parallel_scale)
    lo, hi = node_box(layout, node_size, node)
    eye = np.asarray(camera.position, dtype=float)
    if camera.direction is not None:
        # The smallest view-space depth of the box: perspective scales by
        # depth, not by distance, and the nearest depth gives the largest
        # projected voxel. A box reaching the camera's plane gets the floor.
        d = np.asarray(camera.direction, dtype=float)
        d = d / np.linalg.norm(d)
        corner = np.where(d >= 0, lo, hi)          # the corner least far along d
        distance = max(float(np.dot(corner - eye, d)), 1e-6)
    else:
        nearest = np.clip(eye, lo, hi)
        distance = max(float(np.linalg.norm(eye - nearest)), 1e-6)
    world_per_px = 2.0 * distance * math.tan(math.radians(camera.view_angle) / 2.0) \
        / camera.viewport_height_px
    return voxel / world_per_px


def choose_nodes(layout: PyramidLayout, camera: Camera, *, budget_voxels: int,
                 node_size: int = 256, target_px: float = 1.0,
                 threshold: float | None = None, max_nodes: int | None = None,
                 halo: int = 0) -> list[Node]:
    """The nodes to draw: non-overlapping, covering what the view can show,
    each as fine as ``target_px`` asks while the total stays within
    ``budget_voxels`` (and ``max_nodes`` blocks, when given).

    Refinement is greedy: the node whose voxels look largest on screen is split
    first, so a budget too small for the whole view still spends its voxels
    where they show most. A split is taken only if it fits; otherwise the node
    stays at its level. Nodes whose maximum is below ``threshold`` (nothing
    above the opacity ramp) or that hold no finite voxel are left out.
    A node costs its voxels plus ``halo`` samples on every side, as uploaded,
    and is kept if the bricks around it hold values above ``threshold``.
    """
    if node_size % layout.brick:
        raise ValueError("node_size must be a multiple of the pyramid's brick")

    def keep(node: Node) -> bool:
        lo, hi = node_box(layout, node_size, node)
        # Voxel i spans [i - 0.5, i + 0.5]: the node's extent, as drawn.
        if not _visible(camera, lo - 0.5, hi - 0.5):
            return False
        if layout.brick_max is not None:
            peak = _node_max(layout, node_size, node, halo)
            if math.isnan(peak) or (threshold is not None and peak < threshold):
                return False
        return True

    # The roots are the nodes of the coarsest level: the cheapest cover of the
    # whole cube, refined from there.
    top = len(layout.shapes) - 1
    gx, gy, gz = _grid(layout, node_size, top)
    chosen = [Node(top, x, y, z) for z in range(gz) for y in range(gy) for x in range(gx)]
    chosen = [n for n in chosen if keep(n)]
    used = sum(_node_voxels(layout, node_size, n, halo) for n in chosen)
    count = len(chosen)
    if used > budget_voxels or (max_nodes is not None and count > max_nodes):
        raise ValueError(f"the coarsest cover needs {used} voxels in {count} blocks, more than the "
                         f"budget ({budget_voxels} voxels, {max_nodes} blocks)")

    heap = [(-projected_voxel_px(layout, node_size, n, camera), n) for n in chosen]
    heapq.heapify(heap)
    final: list[Node] = []
    while heap:
        neg_px, node = heapq.heappop(heap)
        if node.level == 0 or -neg_px <= target_px:
            final.append(node)
            continue
        kids = [k for k in _children(layout, node_size, node) if keep(k)]
        extra = (sum(_node_voxels(layout, node_size, k, halo) for k in kids)
                 - _node_voxels(layout, node_size, node, halo))
        extra_count = len(kids) - 1
        if used + extra > budget_voxels or (max_nodes is not None and count + extra_count > max_nodes):
            final.append(node)
            continue
        used += extra
        count += extra_count
        for kid in kids:
            heapq.heappush(heap, (-projected_voxel_px(layout, node_size, kid, camera), kid))
    return sorted(final)
