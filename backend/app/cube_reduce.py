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


def reduce_cube(data: np.ndarray, factors_xyz: tuple[int, int, int], mode: str,
                max_block_bytes: int = 256 * 1024 * 1024,
                should_stop: Callable[[], bool] | None = None) -> np.ndarray:
    """Reduce a 3-D ``(z, y, x)`` array (a memory map is read piece by piece)
    by ``factors_xyz`` with ``mode``. Reads slabs of ``fz`` planes, in bands of
    whole ``fy`` rows, of at most about ``max_block_bytes`` of float32 each."""
    if data.ndim != 3:
        raise ValueError("A reduction needs a 3-D cube.")
    fx, fy, fz = (int(f) for f in factors_xyz)
    depth, height, width = data.shape
    out = np.empty(reduced_shape(data.shape, (fx, fy, fz)), dtype=np.float32)
    row_bytes = max(1, fz * fy * width * 4)
    rows_per_band = max(1, int(max_block_bytes // row_bytes)) * fy
    for kz, z0 in enumerate(range(0, depth, fz)):
        for y0 in range(0, height, rows_per_band):
            if should_stop is not None and should_stop():
                raise InterruptedError("Reduction stopped.")
            piece = np.asarray(data[z0:z0 + fz, y0:y0 + rows_per_band, :], dtype=np.float32)
            red = reduce_block(piece, (fx, fy, fz), mode)
            out[kz, y0 // fy:y0 // fy + red.shape[1], :] = red[0]
    return out


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
