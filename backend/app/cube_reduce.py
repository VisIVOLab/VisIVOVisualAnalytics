"""Reduced copies of a cube for a headset to render itself (issue #167).

A Quest 3 cannot hold or ray-march a 700 x 700 x 158 float32 cube at 72-90 Hz,
so the WebXR page (``app/static/vr``) renders a reduced copy: the cube binned by
an integer factor on each axis, quantised to 8 or 16 bits. Everything the
analysis computes stays on the full cube; only what is drawn is reduced.

Two reductions are offered, because they lose different things:

``mean``
    Each reduced voxel is the mean of its block over the block's finite voxels
    (NaN when none is finite), the rule of the brick pyramid's levels
    (``app.brick_pyramid``). Noise falls as the square root of the block, so a
    threshold set on the reduced cube itself reaches fainter extended emission;
    compact emission smaller than a block is diluted.
``max``
    Each reduced voxel is the largest finite value of its block. Compact peaks
    survive at their full brightness, but so do the noise peaks: the noise of
    the reduced cube is the distribution of a block maximum, shifted up.

The fraction of a reference mask's flux and voxels that each one keeps above
the display threshold is measured by ``validation/vr_reduction_fidelity.py``,
which calls the functions below.

The cube is read in bounded memory: slabs of whole blocks along the spectral
axis, cut into bands of rows when a slab would exceed ``max_block_bytes``.
"""
from __future__ import annotations

import math
from typing import Callable

import numpy as np

MODES = ("mean", "max")
ENCODINGS = ("uint8", "float16")

#: Stored value of a blank voxel (all of its block NaN) in a float16 volume. The
#: valid values are normalised to [0, 1], so anything below 0 is blank; with
#: linear texture filtering a blank neighbour pulls a sample down, below any
#: threshold, which draws the edge of a blank region conservatively.
FLOAT16_BLANK = -1.0

#: Reduced voxels per side the automatic factor aims at. 256 voxels per side in
#: 8 bits is 16 MiB of 3-D texture, which the Quest 3 ray-marches at its refresh
#: rate with a few hundred samples per ray (see docs/user-guide/vr-headset.md).
DEFAULT_MAX_DIM = 256


def auto_factors(shape_zyx: tuple[int, int, int], max_dim: int = DEFAULT_MAX_DIM,
                 ) -> tuple[int, int, int]:
    """``(fx, fy, fz)``: the smallest integer factors that bring every axis to at
    most ``max_dim`` reduced voxels. The two spatial axes share one factor, so a
    reduced pixel stays square on the sky; the spectral axis has its own."""
    if max_dim < 1:
        raise ValueError("max_dim must be at least 1.")
    depth, height, width = (int(n) for n in shape_zyx)
    fxy = max(1, math.ceil(max(width, height) / max_dim))
    fz = max(1, math.ceil(depth / max_dim))
    return fxy, fxy, fz


def reduced_shape(shape_zyx: tuple[int, int, int], factors_xyz: tuple[int, int, int],
                  ) -> tuple[int, int, int]:
    """Shape ``(depth, height, width)`` of the reduced cube: each axis divided by
    its factor, rounded up (the last block of an axis may be partial)."""
    fx, fy, fz = factors_xyz
    depth, height, width = shape_zyx
    return (-(-depth // fz), -(-height // fy), -(-width // fx))


#: Reduced voxels per axis a headset takes as one 3-D texture (WebGL2's
#: MAX_3D_TEXTURE_SIZE is 2048 on current Adreno GPUs).
MAX_AXIS = 2048
#: ...and in all: 64 Mi voxels (512 x 512 x 256) is 64 MiB of R8 texture, the
#: most a Quest browser tab can be expected to allocate next to its own buffers,
#: and 256 MiB of float32 while the backend reduces it.
MAX_VOXELS = 64 * 1024 * 1024
#: Texture bytes sent: 64 MiB, so a float16 cube holds half the voxels of a uint8 one.
MAX_TEXTURE_BYTES = 64 * 1024 * 1024
BYTES_PER_VOXEL = {"uint8": 1, "float16": 2}


def check_size(shape_zyx: tuple[int, int, int], encoding: str = "uint8") -> None:
    """Refuse (``ValueError``) a reduced cube no headset texture can hold, before
    any of the cube is read."""
    d, h, w = (int(n) for n in shape_zyx)
    n = w * h * d
    nbytes = n * BYTES_PER_VOXEL.get(encoding, 2)
    if max(w, h, d) > MAX_AXIS or n > MAX_VOXELS or nbytes > MAX_TEXTURE_BYTES:
        raise ValueError(
            f"A reduced cube of {w} x {h} x {d} voxels ({nbytes / 2**20:.0f} MiB in {encoding}) "
            f"is larger than a headset texture ({MAX_AXIS} per axis, {MAX_VOXELS // 2**20} Mi "
            f"voxels, {MAX_TEXTURE_BYTES // 2**20} MiB): use larger factors.")


def reduce_block(block: np.ndarray, factors_xyz: tuple[int, int, int], mode: str) -> np.ndarray:
    """Reduce a ``(z, y, x)`` array by ``factors_xyz`` with the NaN-aware rule
    ``mode``. Partial blocks at the end of an axis are reduced over the voxels
    they hold. Returns float32."""
    if mode not in MODES:
        raise ValueError(f"mode must be one of {MODES}, not {mode!r}.")
    fx, fy, fz = (int(f) for f in factors_xyz)
    if min(fx, fy, fz) < 1:
        raise ValueError("Reduction factors must be at least 1.")
    a = np.asarray(block, dtype=np.float32)
    d, h, w = a.shape
    dr, hr, wr = -(-d // fz), -(-h // fy), -(-w // fx)
    pad = ((0, dr * fz - d), (0, hr * fy - h), (0, wr * fx - w))
    if any(p[1] for p in pad):
        a = np.pad(a, pad, mode="constant", constant_values=np.nan)
    blocks = a.reshape(dr, fz, hr, fy, wr, fx)
    finite = np.isfinite(blocks)
    count = finite.sum(axis=(1, 3, 5))
    if mode == "mean":
        total = np.where(finite, blocks, 0.0).sum(axis=(1, 3, 5), dtype=np.float64)
        with np.errstate(invalid="ignore", divide="ignore"):
            out = (total / count).astype(np.float32)
    else:
        out = np.where(finite, blocks, -np.inf).max(axis=(1, 3, 5)).astype(np.float32)
    out[count == 0] = np.nan
    return out


def _fit(n: int, f: int, room: int) -> int:
    """A piece's length along an axis of ``n`` with factor ``f`` and room for
    ``room`` voxels: the whole axis, else whole blocks, else part of one block."""
    room = max(1, int(room))
    if room >= n:
        return n
    if room >= f:
        return f * (room // f)
    return room


def _segments(n: int, f: int, length: int):
    """``(start, stop, group)`` pieces along an axis: runs of whole blocks
    (``group`` = f, the last block possibly partial) when ``length`` >= f, else
    parts of one block each (``group`` = the part's length: one output)."""
    if length >= f:
        for s in range(0, n, length):
            yield s, min(s + length, n), f
    else:
        for b in range(0, n, f):
            e = min(b + f, n)
            for s in range(b, e, length):
                stop = min(s + length, e)
                yield s, stop, stop - s


def piece_lengths(shape_zyx, factors_xyz, max_voxels: int, min_planes: int = 1,
                  out_plane_voxels: int = 0) -> tuple[int, int, int]:
    """``(lz, ly, lx)``: the largest piece a reduction reads at once, at most
    ``max_voxels`` voxels whatever the factors (a block larger than that is
    read in parts and accumulated). ``min_planes`` deep first when it fits (an
    HDF5 dataset chunked by bricks is read a brick deep, not a few planes at a
    time). ``out_plane_voxels``: voxels of one reduced plane, so that the
    accumulators of the reduced planes a piece spans stay within the bound too."""
    fx, fy, fz = (int(f) for f in factors_xyz)
    d, h, w = (int(n) for n in shape_zyx)
    budget = max(1, int(max_voxels))
    lz0 = _fit(d, fz, min(budget, fz * max(1, -(-int(min_planes) // fz))))
    lx = _fit(w, fx, budget // lz0)
    ly = _fit(h, fy, budget // (lz0 * lx))
    room = max(lz0, budget // (lx * ly))
    if out_plane_voxels:
        # The accumulators: 16 bytes (a float64 sum and weight) per reduced
        # voxel of every reduced plane the piece spans, within the same bound.
        room = min(room, fz * max(1, (4 * budget) // (16 * int(out_plane_voxels))))
    return _fit(d, fz, room), ly, lx


def _group(a: np.ndarray, g: tuple[int, int, int], how: str) -> np.ndarray:
    """``a`` (z, y, x) summed (or maxed) over groups of ``g``, padding the last
    group of each axis (zeros for a sum, -inf for a max)."""
    gz, gy, gx = g
    d, h, w = a.shape
    dr, hr, wr = -(-d // gz), -(-h // gy), -(-w // gx)
    pad = ((0, dr * gz - d), (0, hr * gy - h), (0, wr * gx - w))
    if any(p[1] for p in pad):
        a = np.pad(a, pad, constant_values=(0 if how == "sum" else -np.inf))
    blocks = a.reshape(dr, gz, hr, gy, wr, gx)
    if how == "sum":
        return blocks.sum(axis=(1, 3, 5), dtype=np.float64)
    return blocks.max(axis=(1, 3, 5))


def accumulate(pieces, shape_zyx, factors_xyz, mode: str, lengths,
               should_stop: Callable[[], bool] | None = None,
               weights_out: np.ndarray | None = None) -> np.ndarray:
    """The reduced cube from pieces read one at a time.

    ``pieces(z0, z1, y0, y1, x0, x1)`` returns ``(S, C)`` for that box of the
    input: for ``mean`` the sum of each voxel's values and their count (a
    weight), for ``max`` the value (-inf where blank) and a count > 0 where
    finite. A block larger than a piece is accumulated over several, so no
    read is larger than ``lengths`` (:func:`piece_lengths`). ``weights_out``
    (the reduced shape, float32), when given, receives each reduced voxel's
    total weight (count)."""
    if mode not in MODES:
        raise ValueError(f"mode must be one of {MODES}, not {mode!r}.")
    fx, fy, fz = (int(f) for f in factors_xyz)
    d, h, w = (int(n) for n in shape_zyx)
    rd, rh, rw = reduced_shape((d, h, w), (fx, fy, fz))
    out = np.empty((rd, rh, rw), dtype=np.float32)
    lz, ly, lx = lengths
    acc_z = None
    s_acc = c_acc = None

    def flush():
        oz0, oz1 = acc_z
        with np.errstate(invalid="ignore", divide="ignore"):
            if mode == "mean":
                v = (s_acc / c_acc).astype(np.float32)
            else:
                v = s_acc.astype(np.float32)
        v[c_acc == 0] = np.nan
        out[oz0:oz1] = v
        if weights_out is not None:
            weights_out[oz0:oz1] = c_acc

    for zs, ze, gz in _segments(d, fz, lz):
        span = ((zs // fz), (ze - 1) // fz + 1)
        if acc_z != span:
            if acc_z is not None:
                flush()
            acc_z = span
            n = span[1] - span[0]
            s_acc = (np.zeros if mode == "mean" else lambda s: np.full(s, -np.inf))((n, rh, rw))
            c_acc = np.zeros((n, rh, rw), dtype=np.float64)
        for ys, ye, gy in _segments(h, fy, ly):
            oy0 = ys // fy
            for xs, xe, gx in _segments(w, fx, lx):
                if should_stop is not None and should_stop():
                    raise InterruptedError("Reduction stopped.")
                s, c = pieces(zs, ze, ys, ye, xs, xe)
                gs = _group(s, (gz, gy, gx), "sum" if mode == "mean" else "max")
                gc = _group(c, (gz, gy, gx), "sum")
                ox0 = xs // fx
                sl = (slice(0, gs.shape[0]), slice(oy0, oy0 + gs.shape[1]),
                      slice(ox0, ox0 + gs.shape[2]))
                if mode == "mean":
                    s_acc[sl] += gs
                else:
                    np.maximum(s_acc[sl], gs, out=s_acc[sl])
                c_acc[sl] += gc
        if ze == d or ze % fz == 0:
            flush()
            acc_z = None
    if acc_z is not None:
        flush()
    return out


def _finite_pieces(data, mode: str):
    """``pieces`` for :func:`accumulate` over a cube's own voxels."""
    def read(z0, z1, y0, y1, x0, x1):
        piece = np.array(data[z0:z1, y0:y1, x0:x1], dtype=np.float32)
        finite = np.isfinite(piece)
        piece[~finite] = 0.0 if mode == "mean" else -np.inf
        return piece, finite
    return read


def reduce_cube(data: np.ndarray, factors_xyz: tuple[int, int, int], mode: str,
                max_block_bytes: int = 256 * 1024 * 1024,
                should_stop: Callable[[], bool] | None = None,
                min_planes: int = 1) -> np.ndarray:
    """Reduce a 3-D ``(z, y, x)`` array (a memory map or an HDF5 dataset is
    read piece by piece) by ``factors_xyz`` with ``mode`` (NaN-aware, over each
    block's finite voxels, as :func:`reduce_block`). No piece read is larger
    than about ``max_block_bytes`` of float32, whatever the factors: a block
    larger than that is read in parts and accumulated."""
    if len(data.shape) != 3:
        raise ValueError("A reduction needs a 3-D cube.")
    shape = tuple(int(n) for n in data.shape)
    _, rh, rw = reduced_shape(shape, factors_xyz)
    lengths = piece_lengths(shape, factors_xyz, max_block_bytes // 4, min_planes, rh * rw)
    return accumulate(_finite_pieces(data, mode), shape, factors_xyz, mode, lengths, should_stop)


# ── From a brick pyramid (large cubes) ──────────────────────────────────────
#
# Level K of a brick pyramid (app/brick_pyramid.py) is the cube halved K times
# on every axis, rounded up, each voxel the mean of a 2x2x2 block of level K-1
# over its finite voxels. Its voxel i therefore covers the full-resolution
# pixels [i 2^K, min((i + 1) 2^K, n)) on an axis of n pixels, the block of a
# factor 2^K: reducing level K further by integer factors (a, a, b) gives a
# reduced cube whose voxels cover exactly the blocks of the factors
# (2^K a, 2^K a, 2^K b), the geometry the page and its selection mapping use.
# The values are a mean of means: equal to the block's mean wherever the block
# and its sub-blocks are whole and finite, weighted by sub-block instead of by
# voxel at a partial last block and around blank voxels.


#: Automatic factors read from a coarse level may be rounded up to its blocks,
#: but by at most this much on any axis: past it, a finer level is read (in
#: bounded pieces), up to ``max_read_voxels``.
MAX_INFLATION = 2.0


def pyramid_plan(level_shapes: list[tuple[int, int, int]], factor_xy: int, factor_z: int,
                 max_dim: int = DEFAULT_MAX_DIM, read_voxels: int = 256 * 1024 * 1024,
                 max_read_voxels: int = 4 * 1024 ** 3,
                 ) -> tuple[int, tuple[int, int, int], tuple[int, int, int]]:
    """``(level, extra (ax, ay, az), factors (fx, fy, fz))``: which pyramid level
    a reduced cube is read from and how much further it is binned.

    ``level_shapes`` are the pyramid's (z, y, x) level shapes, level 0 the cube.
    The factors aimed at are ``factor_xy`` and ``factor_z``, or for 0 the
    automatic ones of :func:`auto_factors`.

    - The level is the coarsest one whose 2^K divides both factors, so they
      come out exactly, when it holds at most ``read_voxels`` voxels.
    - Otherwise, with factors given: the coarsest level that both divides them
      and fits ``read_voxels``; when there is none, ValueError naming the
      nearest factors that one would take (a given factor is never rounded,
      and the cube is never read past ``read_voxels`` for it). An automatic
      factor alongside a given one keeps the :data:`MAX_INFLATION` cap below,
      from a finer level that still divides the given one.
    - Otherwise, automatic factors: the coarser of the finest level within
      ``read_voxels`` and the coarsest one not coarser than the smaller factor,
      with the factors rounded up to multiples of 2^K. When that would inflate
      an axis by more than :data:`MAX_INFLATION` (a wide, shallow cube's
      spectral axis), the finest level that keeps every axis within it, if it
      holds at most ``max_read_voxels``; else the coarse level, inflated (the
      caller reports it)."""
    if not level_shapes:
        raise ValueError("A pyramid has at least one level.")
    shape0 = tuple(int(n) for n in level_shapes[0])
    auto_xy, _, auto_z = auto_factors(shape0, max_dim)
    given_xy, given_z = bool(factor_xy and factor_xy > 0), bool(factor_z and factor_z > 0)
    want_xy = int(factor_xy) if given_xy else auto_xy
    want_z = int(factor_z) if given_z else auto_z
    top = len(level_shapes) - 1
    size = [int(np.prod([int(n) for n in s])) for s in level_shapes]
    quality = min(top, int(math.floor(math.log2(max(1, min(want_xy, want_z))))))

    def divides(k: int) -> bool:
        return all(f % (1 << k) == 0 for f, g in ((want_xy, given_xy), (want_z, given_z)) if g)

    def factors_at(k: int) -> tuple[int, int]:
        s = 1 << k
        up = lambda f, g: f if g else s * max(1, -(-f // s))  # noqa: E731
        return up(want_xy, given_xy), up(want_z, given_z)

    # The coarsest level from which the factors come out exactly.
    exact = quality
    while exact > 0 and (want_xy % (1 << exact) or want_z % (1 << exact)):
        exact -= 1
    affordable = next((k for k, n in enumerate(size) if n <= read_voxels), top)
    if size[exact] <= read_voxels:
        level = exact
    elif given_xy or given_z:
        fits = [k for k in range(affordable, top + 1) if divides(k)]
        if not fits:
            k = max(quality, affordable)
            s = 1 << k
            near = lambda f: max(s, int(round(f / s)) * s)  # noqa: E731
            hint = (f"factor_xy {near(want_xy)}" if given_xy else "") + \
                   (", " if given_xy and given_z else "") + \
                   (f"factor_z {near(want_z)}" if given_z else "")
            kd = max(j for j in range(quality + 1) if divides(j))
            raise ValueError(
                f"The factors {want_xy} x {want_xy} x {want_z} would read level {kd} of the "
                f"brick pyramid, {size[kd] * 4 / 2**30:.1f} GiB, more than the "
                f"{read_voxels * 4 / 2**30:.1f} GiB read for a headset: use factors that "
                f"{s} divides ({hint}), automatic factors, or source 'cube'.")
        level = fits[0]
        # One factor given, the other automatic: the automatic axis keeps the
        # same 2x cap, from a finer level that still divides the given one,
        # if it holds at most max_read_voxels.
        if not (given_xy and given_z):
            fxy, fz = factors_at(level)
            if max(fxy / want_xy, fz / want_z) > MAX_INFLATION:
                finer = [k for k in range(level - 1, -1, -1) if divides(k)
                         and max(factors_at(k)[0] / want_xy, factors_at(k)[1] / want_z) <= MAX_INFLATION]
                if finer and size[finer[0]] <= max_read_voxels:
                    level = finer[0]
    else:
        level = max(quality, affordable)
        fxy, fz = factors_at(level)
        if max(fxy / want_xy, fz / want_z) > MAX_INFLATION:
            finer = [k for k in range(level - 1, -1, -1)
                     if max(factors_at(k)[0] / want_xy, factors_at(k)[1] / want_z) <= MAX_INFLATION]
            if finer and size[finer[0]] <= max_read_voxels:
                level = finer[0]
    s = 1 << level
    fxy, fz = factors_at(level)
    a, b = fxy // s, fz // s
    return level, (a, a, b), (fxy, fxy, fz)


def _pixel_counts(n_level: int, n0: int, s: int) -> np.ndarray:
    """Full-resolution pixels under each level voxel along an axis (the last
    one may hold fewer)."""
    i = np.arange(n_level, dtype=np.int64)
    return (np.minimum((i + 1) * s, n0) - i * s).astype(np.float64)


#: The most border voxels recorded for a repair: past it, none is repaired.
#: Each record is a flat level index (int64) and the level's sum and weight
#: (two float64), 24 bytes: 48 MiB at the cap while the level is reduced. The
#: repair planned afterwards expands the indices to z, y, x and sorts them by
#: brick (corners and sorted copies, 24 bytes each per record), about 300 MiB
#: at most at the cap, freed before level 0 is read.
MAX_BORDER_RECORDS = 2 * 1024 * 1024


def _repair_boxes(idx: np.ndarray, s: int, b: int, n0: np.ndarray):
    """The level-0 boxes to read for the suspect level voxels ``idx`` (N x 3,
    z y x), and for each box the slice of ``idx`` (sorted by box) it serves,
    computed from the coordinates alone. ``s`` <= ``b``: one box per brick,
    the bounding box of its suspects' blocks (an HDF5 hyperslab of a chunk);
    ``s`` > ``b``: each suspect's whole block. Returns (order, boxes) with
    boxes = [(lo, hi, start, stop)], hi exclusive."""
    lo = idx * s
    hi = np.minimum((idx + 1) * s, n0)
    if s > b:
        order = np.arange(len(idx))
        return order, [(l_, h_, i, i + 1) for i, (l_, h_) in enumerate(zip(lo.tolist(), hi.tolist()))]
    cell = lo // b
    key = np.ravel_multi_index(cell.T, tuple(int(n) for n in cell.max(axis=0) + 1))
    order = np.argsort(key, kind="stable")
    key, lo_s, hi_s = key[order], lo[order], hi[order]
    starts = np.flatnonzero(np.r_[True, key[1:] != key[:-1]])
    stops = np.r_[starts[1:], len(key)]
    blo = np.minimum.reduceat(lo_s, starts, axis=0)
    bhi = np.maximum.reduceat(hi_s, starts, axis=0)
    return order, [(l_, h_, int(a), int(z)) for l_, h_, a, z in
                   zip(blo.tolist(), bhi.tolist(), starts, stops)]


def pyramid_reduce(level_data, level0, level: int, shape0, extra_xyz, brick: int,
                   max_block_bytes: int = 128 * 1024 * 1024,
                   repair_voxels: int = 256 * 1024 * 1024,
                   should_stop: Callable[[], bool] | None = None,
                   ) -> tuple[np.ndarray, dict[str, int]]:
    """Mean binning of pyramid level ``level`` (array-like, z y x) by
    ``extra_xyz``, weighted so the result is the full cube's block mean.

    A level voxel's value is a mean of means (each 2x2x2 sub-block weighted
    alike, whatever it holds). Each is weighted here by the number of
    full-resolution pixels under it, so whole blocks are combined as pixels
    are. Next to blank voxels a level voxel may hold only a few finite pixels,
    and its value is then their noise, over-weighted inside the level: every
    finite level voxel with a blank one among its 26 neighbours (a border
    voxel) is recorded while the level is reduced, and afterwards repaired
    from level 0 (``level0``): its exact finite sum and count replace the
    level's in its reduced voxel.

    The repair is all or none, decided before anything of level 0 is read:
    the level-0 voxels it needs are counted from the border voxels'
    coordinates (one box per brick), and when they exceed ``repair_voxels``
    (or the border voxels exceed :data:`MAX_BORDER_RECORDS`, or their blocks
    alone exceed the budget), no border voxel is repaired: they keep the
    level's value, weighted by their pixel count. Returns the reduced cube and
    ``{"border": n, "repaired": n (0 or all), "repair_read_voxels": n}``. Not
    repaired either way: blank pixels inside a level voxel with no blank
    neighbour (isolated blanks), and the equal weights of the sub-blocks of a
    partial last level voxel."""
    from scipy.ndimage import maximum_filter  # noqa: PLC0415

    s = 1 << int(level)
    kz, ky, kx = (int(n) for n in level_data.shape)
    n0 = np.array([int(n) for n in shape0], dtype=np.int64)
    cz, cy, cx = (_pixel_counts(kz, int(n0[0]), s), _pixel_counts(ky, int(n0[1]), s),
                  _pixel_counts(kx, int(n0[2]), s))
    stats = {"border": 0, "repaired": 0, "repair_read_voxels": 0}
    b = int(brick)
    block_voxels = s ** 3
    rec = {"idx": [], "S": [], "C": [], "n": 0, "over": False}

    def pieces(z0, z1, y0, y1, x0, x1):
        hz0, hz1 = max(0, z0 - 1), min(kz, z1 + 1)
        hy0, hy1 = max(0, y0 - 1), min(ky, y1 + 1)
        hx0, hx1 = max(0, x0 - 1), min(kx, x1 + 1)
        v = np.array(level_data[hz0:hz1, hy0:hy1, hx0:hx1], dtype=np.float32)
        finite = np.isfinite(v)
        core = (slice(z0 - hz0, z1 - hz0), slice(y0 - hy0, y1 - hy0), slice(x0 - hx0, x1 - hx0))
        vc, fc = v[core], finite[core]
        w = cz[z0:z1, None, None] * cy[None, y0:y1, None] * cx[None, None, x0:x1]
        S = np.where(fc, vc, 0.0) * w
        C = np.where(fc, w, 0.0)
        if s > 1 and not finite.all():
            sus = fc & maximum_filter(~finite, size=3, mode="constant", cval=False)[core]
            n = int(sus.sum())
            if n:
                stats["border"] += n
                if not rec["over"]:
                    rec["n"] += n
                    # A lower bound of the repair's reads: their own blocks.
                    if rec["n"] > MAX_BORDER_RECORDS or rec["n"] * block_voxels > repair_voxels:
                        rec.update(over=True, idx=[], S=[], C=[])
                    else:
                        loc = np.nonzero(sus)
                        rec["idx"].append(np.ravel_multi_index(
                            (loc[0] + z0, loc[1] + y0, loc[2] + x0), (kz, ky, kx)).astype(np.int64))
                        rec["S"].append(S[sus])
                        rec["C"].append(C[sus])
        return S, C

    ax, ay, az = (int(f) for f in extra_xyz)
    rd, rh, rw = reduced_shape((kz, ky, kx), (ax, ay, az))
    # Per voxel of a piece: its value with a halo, masks, the float64 sum and
    # weight and their temporaries: about 64 bytes, so a piece stays within
    # max_block_bytes.
    lengths = piece_lengths((kz, ky, kx), (ax, ay, az), max_block_bytes // 64, b, rh * rw)
    weights = np.empty((rd, rh, rw), dtype=np.float32)
    out = accumulate(pieces, (kz, ky, kx), (ax, ay, az), "mean", lengths, should_stop,
                     weights_out=weights)
    if rec["over"] or not rec["idx"]:
        return out, stats
    flat = np.concatenate(rec["idx"])
    rec["idx"] = None
    s_lvl = np.concatenate(rec["S"])
    rec["S"] = None
    c_lvl = np.concatenate(rec["C"])
    rec.clear()
    idx = np.stack(np.unravel_index(flat, (kz, ky, kx)), axis=1)
    del flat
    order, boxes = _repair_boxes(idx, s, b, n0)
    cost = sum(int(np.prod(np.subtract(h, l_))) for l_, h, _, _ in boxes)
    if cost > repair_voxels:
        return out, stats
    idx, s_lvl, c_lvl = idx[order], s_lvl[order], c_lvl[order]
    del order
    s_new = np.zeros(len(idx), dtype=np.float64)
    c_new = np.zeros(len(idx), dtype=np.float64)
    step = max(1, min(s, b))
    for lo, hi, a0, a1 in boxes:
        if should_stop is not None and should_stop():
            raise InterruptedError("Reduction stopped.")
        mine = idx[a0:a1]
        if s > b:
            # One suspect's block, read a brick-sized box at a time.
            tot_s = tot_c = 0.0
            for z in range(lo[0], hi[0], b):
                for y in range(lo[1], hi[1], b):
                    for x in range(lo[2], hi[2], b):
                        part = np.array(level0[z:min(z + b, hi[0]), y:min(y + b, hi[1]),
                                               x:min(x + b, hi[2])], dtype=np.float32)
                        fin = np.isfinite(part)
                        tot_s += float(part[fin].sum(dtype=np.float64))
                        tot_c += float(fin.sum())
            s_new[a0], c_new[a0] = tot_s, tot_c
            continue
        block = np.array(level0[lo[0]:hi[0], lo[1]:hi[1], lo[2]:hi[2]], dtype=np.float32)
        fin = np.isfinite(block)
        block[~fin] = 0.0
        gs, gc = _group(block, (step,) * 3, "sum"), _group(fin, (step,) * 3, "sum")
        loc = mine - np.asarray(lo) // s
        s_new[a0:a1] = gs[loc[:, 0], loc[:, 1], loc[:, 2]]
        c_new[a0:a1] = gc[loc[:, 0], loc[:, 1], loc[:, 2]]
    stats["repair_read_voxels"] = cost
    stats["repaired"] = len(idx)
    # Into the reduced voxels: sum = mean x weight, corrected and divided again.
    ridx = np.ravel_multi_index((idx // np.array([az, ay, ax])).T, (rd, rh, rw))
    uniq, inv = np.unique(ridx, return_inverse=True)
    d_s = np.bincount(inv, weights=s_new - s_lvl)
    d_c = np.bincount(inv, weights=c_new - c_lvl)
    flat_out, flat_w = out.reshape(-1), weights.reshape(-1)
    w0 = flat_w[uniq].astype(np.float64)
    s0 = np.where(w0 > 0, np.nan_to_num(flat_out[uniq].astype(np.float64)) * w0, 0.0)
    w1 = w0 + d_c
    with np.errstate(invalid="ignore", divide="ignore"):
        v = ((s0 + d_s) / w1).astype(np.float32)
    v[w1 <= 0] = np.nan
    flat_out[uniq] = v
    return out, stats


def finite_range(volume: np.ndarray) -> tuple[float, float]:
    """``(min, max)`` of the finite values, ``(0, 0)`` when there are none.
    Plane by plane, so no copy of the finite values is made."""
    lo, hi = math.inf, -math.inf
    for plane in volume:
        fin = np.isfinite(plane)
        if fin.any():
            lo = min(lo, float(np.min(plane, where=fin, initial=np.inf)))
            hi = max(hi, float(np.max(plane, where=fin, initial=-np.inf)))
    return (0.0, 0.0) if lo > hi else (lo, hi)


def quantise(volume: np.ndarray, lo: float, hi: float, encoding: str) -> bytes:
    """The reduced volume as the headset's 3-D texture, little-endian, x fastest.

    ``uint8``: code 0 is blank, codes 1..255 map ``lo..hi`` linearly (clamped):
    value = lo + (code - 1) / 254 * (hi - lo).
    ``float16``: value = lo + stored * (hi - lo), not clamped; a blank voxel is
    :data:`FLOAT16_BLANK`. Half floats keep about three significant digits at
    every level, so a threshold a few noise sigma above ``lo`` is resolved as
    finely as one near ``hi``.
    """
    if encoding not in ENCODINGS:
        raise ValueError(f"encoding must be one of {ENCODINGS}, not {encoding!r}.")
    v = np.asarray(volume, dtype=np.float32)
    span = float(hi) - float(lo)
    if not math.isfinite(span) or span <= 0.0:
        span = 1.0
    out = np.empty(v.shape, dtype=np.uint8 if encoding == "uint8" else "<f2")
    # One plane at a time: the temporaries are a plane, not the cube (a 64 Mi
    # voxel cube in one go needed four cube-sized float32 temporaries).
    lo32, span32 = np.float32(lo), np.float32(span)
    for k in range(v.shape[0]):
        plane = v[k]
        blank = ~np.isfinite(plane)
        norm = (plane - lo32) / span32
        if encoding == "uint8":
            norm = np.nan_to_num(norm, nan=0.0, posinf=1.0, neginf=0.0)
            codes = np.clip(np.rint(norm * 254.0), 0.0, 254.0) + 1.0
            codes[blank] = 0.0
            out[k] = codes
        else:
            o = norm.astype(np.float16)
            o[blank] = np.float16(FLOAT16_BLANK)
            out[k] = o
    return out.tobytes()


def dequantise(raw: bytes, shape_zyx: tuple[int, int, int], lo: float, hi: float,
               encoding: str) -> np.ndarray:
    """Inverse of :func:`quantise` (NaN for blank), for tests and for checking
    what the headset is given."""
    span = float(hi) - float(lo)
    if not math.isfinite(span) or span <= 0.0:
        span = 1.0
    if encoding == "uint8":
        codes = np.frombuffer(raw, dtype=np.uint8).reshape(shape_zyx).astype(np.float32)
        out = np.float32(lo) + (codes - 1.0) / 254.0 * np.float32(span)
        out[codes == 0] = np.nan
        return out
    stored = np.frombuffer(raw, dtype="<f2").reshape(shape_zyx).astype(np.float32)
    out = np.float32(lo) + stored * np.float32(span)
    out[stored < 0.0] = np.nan
    return out


def block_centres(n_full: int, factor: int) -> np.ndarray:
    """0-based full-resolution pixel coordinate of each reduced voxel's centre
    along one axis (a partial last block is centred on the pixels it holds)."""
    starts = np.arange(0, n_full, factor, dtype=np.float64)
    ends = np.minimum(starts + factor, n_full) - 1.0
    return 0.5 * (starts + ends)
