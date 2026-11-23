"""A box of a cube at one level of detail, for the desktop's progressive view
of a large cube (issue #321).

The desktop draws a coarse preview of the whole cube (``/v1/cube/preview``)
and, where the camera looks, a finer set of blocks of one level: level 0 is
the cube itself, level ``l`` is the cube halved ``l`` times on every axis
(rounded up), as the levels of its brick pyramid (``app.brick_pyramid``). A
block is a box of the level with ``ghost`` samples of its neighbours on every
side, the edge sample repeated where the ghost falls outside the cube, so the
desktop interpolates across block seams and at the border without a step.
Blank voxels stay NaN.

Where a level comes from (the ``source`` the answer reports):

* ``cube``: level 0 is always read from the cube's file, as
  ``/v1/cube/subvolume_bin`` reads it, with no pyramid (a FITS cube, a VBT
  field, a PLUTO variable).
* ``pyramid``: a level of the cube's brick pyramid when one is complete and
  current. Each voxel is the mean of the ``2^l`` block of the cube it stands
  for, over its finite voxels; its centre is ``(2^l - 1) / 2`` cube voxels
  past the block's first voxel (the ``voxel offset``; the last block of an
  axis may be partial, its centre then nearer).
* ``strided``: with no usable pyramid, every ``2^l``-th voxel of the cube:
  voxel ``i`` is the cube's voxel ``i * 2^l`` (voxel offset 0). It has the
  level's shape but is a sample, not a block mean: thin or noisy structure
  aliases, which is why the pyramid is preferred whenever it is there.

Nothing here builds a pyramid.
"""
from __future__ import annotations

import os
from typing import Any

import numpy as np

#: The largest answer one request may ask for (bytes of float32, ghosts
#: included): ``VISIVO_REGION_LEVEL_MAX_BYTES``, by default 512 MiB.
DEFAULT_MAX_BYTES = 512 * 1024 ** 2


class NoPyramid(LookupError):
    """The level was asked from the pyramid and the cube has no usable one."""


def max_bytes() -> int:
    return int(os.environ.get("VISIVO_REGION_LEVEL_MAX_BYTES", str(DEFAULT_MAX_BYTES)))


def box_bytes(box, ghost: int) -> int:
    """Bytes of the float32 answer for the inclusive ``box`` ``(x0, x1, y0,
    y1, z0, z1)`` with ``ghost`` samples on every side (0 for an inverted
    box, which is refused later as outside the level)."""
    x0, x1, y0, y1, z0, z1 = (int(v) for v in box)
    n = 4
    for a, b in ((x0, x1), (y0, y1), (z0, z1)):
        n *= max(0, b - a + 1 + 2 * int(ghost))
    return n


def cube_shape(full) -> tuple[int, ...]:
    """The (z, y, x) shape the desktop's cube readers see for a file of shape
    ``full``: ``fits_dataset.cube_shape``, the one rule for the cube's axes
    (``ScientificFitsDataset.squeeze_to_3d``). Fewer than three entries for an
    image."""
    from .fits_dataset import cube_shape as rule  # noqa: PLC0415

    return rule(full)


def pyramid_eligible(full) -> bool:
    """A pyramid holds the cube the desktop reads (the same axes, issue #506)
    when the file has exactly three axes longer than one: beyond three the
    desktop shows plane 0 of the rest, which no pyramid is asked to stand for
    (the rule of ``preview_pyramid.ready_pyramid``)."""
    return sum(1 for n in full if int(n) > 1) == 3


def default_level_shapes(shape0) -> list[tuple[int, int, int]]:
    """The levels a pyramid of the cube has (or would have) with the default
    brick: the levels a strided read offers too."""
    from .brick_pyramid import default_brick, level_shapes  # noqa: PLC0415
    shape0 = tuple(int(n) for n in shape0)
    return level_shapes(shape0, default_brick(shape0))


def level_region(box, shape) -> tuple[slice, slice, slice]:
    """The (z, y, x) half-open slices of the inclusive ``box`` ``(x0, x1, y0,
    y1, z0, z1)``, refused (ValueError) unless it lies inside a level of
    ``shape`` (z, y, x)."""
    x0, x1, y0, y1, z0, z1 = (int(v) for v in box)
    region = (slice(z0, z1 + 1), slice(y0, y1 + 1), slice(x0, x1 + 1))
    for r, n, name in zip(region, shape, "zyx"):
        if not 0 <= r.start < r.stop <= int(n):
            raise ValueError(f"The box's {name} range {r.start}..{r.stop - 1} is not inside "
                             f"the level's 0..{int(n) - 1}.")
    return region


def check_level(level: int, shapes) -> tuple[int, int, int]:
    if not 0 <= int(level) < len(shapes):
        raise ValueError(f"Level {level} is not one of the cube's levels 0..{len(shapes) - 1}.")
    return tuple(int(n) for n in shapes[int(level)])


def strided_region(data, level: int, region, ghost: int) -> np.ndarray:
    """The box ``region`` (half-open slices of level ``level``) of the cube
    ``data`` (array-like, z y x, level 0) taken every ``2^level`` voxels: the
    level voxel ``i`` is the cube voxel ``i * 2^level``. With ``ghost`` samples
    on every side, the edge repeated outside the cube; float32, NaN kept.
    Only the samples of the padded box are read."""
    from .brick_pyramid import repeat_edges  # noqa: PLC0415
    f = 1 << int(level)
    g = int(ghost)
    dims = [-(-int(n) // f) for n in data.shape]
    lo = [max(0, r.start - g) for r in region]
    hi = [min(n, r.stop + g) for r, n in zip(region, dims)]
    out = np.empty([r.stop - r.start + 2 * g for r in region], dtype=np.float32)
    inner = tuple(slice(a - (r.start - g), b - (r.start - g)) for r, a, b in zip(region, lo, hi))
    out[inner] = data[lo[0] * f:(hi[0] - 1) * f + 1:f,
                      lo[1] * f:(hi[1] - 1) * f + 1:f,
                      lo[2] * f:(hi[2] - 1) * f + 1:f]
    repeat_edges(out, [a - (r.start - g) for r, a in zip(region, lo)],
                 [(r.stop + g) - b for r, b in zip(region, hi)])
    return out


def voxel_offset(level: int, source: str) -> float:
    """Cube voxels from a level voxel's first cube voxel to the point it
    stands for: the centre of its block for a pyramid level, the voxel itself
    for the cube and for a strided sample."""
    return ((1 << int(level)) - 1) / 2.0 if source == "pyramid" else 0.0


def pyramid_bytes_estimate(shapes, brick: int) -> int:
    """About the bytes a pyramid of these levels takes on disk: float32 data
    and two float32 statistics per brick, uncompressed."""
    total = 0
    for shape in shapes:
        total += 4 * int(np.prod(shape))
        total += 8 * int(np.prod([-(-int(n) // int(brick)) for n in shape]))
    return total


def pyramid_status(path: str, build: bool) -> dict[str, Any]:
    """The state of the cube's brick pyramid, for the desktop to choose its
    levels: ``ready``, ``building`` (with ``progress``), ``missing``,
    ``failed`` (with ``error``) or ``unsupported`` (a cube no pyramid holds:
    a Stokes cube, an image). ``levels``, ``brick`` and ``shapes`` (z, y, x
    per level) are the pyramid's when ready, else those a default build would
    have (the levels a strided read offers). Starts a build only with
    ``build``; runs in a thread (it stats files, may start a process)."""
    from .brick_pyramid import BrickPyramid, default_brick, default_pyramid_path  # noqa: PLC0415
    from .pyramid_builds import BuildStatus, ensure_pyramid, peek_pyramid  # noqa: PLC0415
    from .volume_source import volume_shape  # noqa: PLC0415

    full, hdu = volume_shape(path, None)
    shape0 = cube_shape(full)
    answer: dict[str, Any] = {"state": "unsupported", "progress": 0.0, "levels": 0,
                              "brick": 0, "shapes": [], "bytes_estimate": 0, "error": ""}
    if len(shape0) == 3:
        shapes = default_level_shapes(shape0)
        brick = default_brick(shape0)
        answer.update(levels=len(shapes), brick=brick, shapes=[list(s) for s in shapes],
                      bytes_estimate=pyramid_bytes_estimate(shapes, brick))
    if not pyramid_eligible(full):
        return answer
    if build:
        try:
            status = ensure_pyramid(path, hdu)
        except Exception as exc:  # noqa: BLE001 - e.g. a pyramid directory that cannot be written
            status = BuildStatus("failed", 0.0, f"The brick pyramid could not be built: {exc}")
    else:
        status = peek_pyramid(path, hdu)
    answer.update(state=status.state, progress=float(status.progress), error=status.error)
    if status.state == "ready":
        out = default_pyramid_path(path, hdu)
        try:
            with BrickPyramid(out, source=path) as pyr:
                shapes = [pyr.shape(k) for k in range(pyr.levels)]
                answer.update(levels=pyr.levels, brick=pyr.brick,
                              shapes=[list(s) for s in shapes])
            answer["bytes_estimate"] = int(out.stat().st_size)
        except Exception as exc:  # noqa: BLE001 - changed since the check: not ready
            answer.update(state="missing", progress=0.0, error=str(exc))
    return answer
