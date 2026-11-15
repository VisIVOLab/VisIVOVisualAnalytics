"""The desktop's cube preview read from the cube's brick pyramid (issue #320).

The preview of a large cube (``/v1/cube/preview``) is the cube seen every
``stride`` voxels along each axis. Taken with a stride, ``data[::s, ::s, ::s]``,
it decimates: thin or noisy structures alias and come and go between strides,
and the read touches the whole file sparsely. When the cube's brick pyramid
already exists (built for a headset or a remote view, ``app.brick_pyramid``),
the preview is read from it instead: each preview voxel is the mean of the
``s x s x s`` cell of the cube it stands for, read from one pyramid level in
slabs of whole planes, with no pass over the cube's file (the default
threshold takes a sample of level-0 bricks, as the out-of-core view does).

The preview has the shape the strided one has, ``ceil(n / s)`` per axis (the
last cell of an axis may be partial), so the desktop sizes and places it as
before. Its voxel ``i`` is the cell from ``i * s`` to ``i * s + s - 1``, whose
centre is ``(s - 1) / 2`` cube voxels past the strided sample (``voxel_offset``:
the grid's origin; the last cell of an axis may be shorter, its centre then
nearer).

The level is the coarsest whose block, ``2^k`` voxels per axis, is at most
``s``: for ``s = 2^k`` the preview is that level itself. Otherwise each cell is
the mean of the level voxels it overlaps, each weighted by the number of cube
voxels it shares with the cell, over the finite ones (NaN where none is). That
is the cell's mean when the level voxels it cuts are uniform; a level voxel
straddling a cell boundary contributes its own mean in proportion. Level
voxels next to blank ones are the pyramid's own means of their finite voxels
(a mean of means, as the levels are built); they are not repaired from level 0
as the headset's reduced cube is (``cube_reduce.pyramid_reduce``), which
would read level 0 around every blank region.

Nothing here builds a pyramid: :func:`ready_pyramid` only finds one that is
already complete and current.
"""
from __future__ import annotations

import math
from typing import Any

import numpy as np

#: The memory one slab of a level takes while it is reduced: its values, the
#: finite mask and the float64 sums of the three axes' passes.
SLAB_BYTES = 128 * 1024 * 1024
#: Bytes per level voxel of a slab while it is reduced (float32 value, mask,
#: float64 sum and weight, and the first pass's results).
_BYTES_PER_VOXEL = 40


def ready_pyramid(path: str) -> str | None:
    """The path of the cube's brick pyramid when one is complete and current
    (built from the file as it is now), else None. Never starts a build, and
    never fails: a cube with no usable pyramid is previewed with a stride.

    A pyramid holds the HDU's last three axes; it is the cube the preview
    reads only when every other axis is one plane and none of these three is
    (the rule of the headset's reduced cube, ``routers/cube._reduced_source``).
    """
    from .brick_pyramid import default_pyramid_path  # noqa: PLC0415
    from .pyramid_builds import pyramid_ready  # noqa: PLC0415
    from .volume_source import volume_shape  # noqa: PLC0415

    try:
        full, hdu = volume_shape(path, None)
        full = [int(n) for n in full]
        shape = full[-3:]
        if len(shape) != 3 or min(shape) <= 1 or any(n != 1 for n in full[:-3]):
            return None
        if not pyramid_ready(path, hdu):
            return None
        return str(default_pyramid_path(path, hdu))
    except Exception:  # noqa: BLE001 - no pyramid is the old preview, never an error
        return None


def choose_level(level_shapes: list[tuple[int, int, int]], stride: int) -> int:
    """The coarsest level whose block is at most ``stride`` voxels on every
    axis: ``floor(log2 stride)``, within the pyramid's levels."""
    if stride < 1:
        raise ValueError("The stride must be at least 1.")
    return min(len(level_shapes) - 1, int(math.floor(math.log2(stride))))


def level_factors(shape0, level_shape, level: int) -> tuple[int, int, int]:
    """``(fz, fy, fx)``: the block of the cube one voxel of ``level`` stands
    for along each axis, checked against the level's shape. The levels halve
    every axis (rounded up), so ``2^level`` on each; an axis of one plane (an
    image's) stays one plane whatever the factor."""
    f = 1 << int(level)
    for n0, nk in zip(shape0, level_shape):
        if int(nk) != -(-int(n0) // f):
            raise ValueError(f"A level-{level} shape {tuple(level_shape)} is not the "
                             f"{tuple(shape0)} cube halved {level} times.")
    return f, f, f


def cell_weights(n0: int, stride: int, f: int) -> tuple[np.ndarray, np.ndarray]:
    """Along one axis of ``n0`` cube voxels: for each preview cell ``i`` (the
    voxels ``i * stride`` to ``min((i + 1) * stride, n0) - 1``) the level voxels
    of block ``f`` it overlaps and the cube voxels it shares with each.

    Returns ``(index, weight)``, both ``(n_out, T)``: the first level voxel of
    the cell plus 0..T-1, and the shared voxel count (0 for the columns past a
    cell's last level voxel, whose index is clipped to the axis)."""
    n_out = -(-n0 // stride)
    nk = -(-n0 // f)
    lo = np.arange(n_out, dtype=np.int64) * stride
    hi = np.minimum(lo + stride, n0)
    first = lo // f
    last = (hi - 1) // f
    taps = int((last - first).max()) + 1
    index = first[:, None] + np.arange(taps, dtype=np.int64)[None, :]
    a = np.maximum(lo[:, None], index * f)
    b = np.minimum(hi[:, None], np.minimum((index + 1) * f, n0))
    weight = np.clip(b - a, 0, None).astype(np.float64)
    return np.minimum(index, nk - 1), weight


def _pass(values: np.ndarray, axis: int, index: np.ndarray, weight: np.ndarray) -> np.ndarray:
    """Weighted sums along ``axis``: ``out[..., i, ...] = sum_t weight[i, t] *
    values[..., index[i, t], ...]``."""
    shape = [1] * values.ndim
    shape[axis] = weight.shape[0]
    out = None
    for t in range(weight.shape[1]):
        term = np.take(values, index[:, t], axis=axis) * weight[:, t].reshape(shape)
        out = term if out is None else out + term
    return out


def block_mean(level_slab: np.ndarray, weights_zyx) -> np.ndarray:
    """The cells' means of a slab of a level: the weighted mean of its finite
    voxels over the cells of :func:`cell_weights` (NaN where a cell holds no
    finite voxel). ``weights_zyx`` are the three axes' ``(index, weight)``,
    the z one relative to the slab."""
    v = np.asarray(level_slab, dtype=np.float32)
    if all(weights_zyx[axis][1].shape[1] == 1 for axis in range(3)):
        # One level voxel per cell (the stride is the level's block): the level.
        for axis in (2, 1, 0):
            v = np.take(v, weights_zyx[axis][0][:, 0], axis=axis)
        return np.ascontiguousarray(v, dtype=np.float32)
    finite = np.isfinite(v)
    if finite.all():
        # No blank voxel: the weight of a cell is the product of its axes'
        # (its voxel count), so only the sums take the three passes.
        del finite
        total = v.astype(np.float64)
        del v
        for axis in (2, 1, 0):
            total = _pass(total, axis, *weights_zyx[axis])
        wz, wy, wx = (weights_zyx[axis][1].sum(axis=1) for axis in range(3))
        return (total / (wz[:, None, None] * wy[None, :, None] * wx[None, None, :])
                ).astype(np.float32)
    total = np.where(finite, v, 0.0).astype(np.float64)
    count = finite.astype(np.float64)
    del v, finite
    for axis in (2, 1, 0):
        index, weight = weights_zyx[axis]
        total = _pass(total, axis, index, weight)
        count = _pass(count, axis, index, weight)
    with np.errstate(invalid="ignore", divide="ignore"):
        out = (total / count).astype(np.float32)
    out[count <= 0] = np.nan
    return out


def pyramid_preview(level_data, shape0, level: int, stride: int,
                    slab_bytes: int = SLAB_BYTES) -> np.ndarray:
    """The preview at ``stride`` from pyramid level ``level`` (array-like,
    z y x, e.g. an h5py dataset) of a cube of ``shape0``: shape ``ceil(n /
    stride)`` per axis, each voxel its cell's mean. The level is read in slabs
    of whole preview planes, each at most about ``slab_bytes`` of work."""
    shape0 = tuple(int(n) for n in shape0)
    kz, ky, kx = (int(n) for n in level_data.shape)
    fz, fy, fx = level_factors(shape0, (kz, ky, kx), level)
    s = int(stride)
    wz = cell_weights(shape0[0], s, fz)
    wy = cell_weights(shape0[1], s, fy)
    wx = cell_weights(shape0[2], s, fx)
    out = np.empty((wz[0].shape[0], wy[0].shape[0], wx[0].shape[0]), dtype=np.float32)
    # Level planes per preview plane: at most ceil(s / fz) + 1.
    per_plane = -(-s // fz) + 1
    planes = max(1, slab_bytes // max(1, per_plane * ky * kx * _BYTES_PER_VOXEL))
    for i0 in range(0, out.shape[0], planes):
        i1 = min(out.shape[0], i0 + planes)
        index, weight = wz[0][i0:i1], wz[1][i0:i1]
        z0 = int(index[:, 0].min())
        z1 = int(index.max()) + 1
        slab = np.asarray(level_data[z0:z1], dtype=np.float32)
        out[i0:i1] = block_mean(slab, ((index - z0, weight), wy, wx))
    return out


def describe(level: int, stride: int, levels: int) -> dict[str, Any]:
    """The answer's fields naming a preview from the pyramid."""
    f = 1 << int(level)
    return {"source": "pyramid", "level": int(level), "levels": int(levels),
            "level_factors": [f, f, f], "factors": [int(stride)] * 3,
            "voxel_offset": (int(stride) - 1) / 2.0}
