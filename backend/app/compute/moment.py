from __future__ import annotations

import base64
import concurrent.futures as _cf  # noqa: F401
import logging  # noqa: F401
import os as _os  # noqa: F401
import shutil  # noqa: F401
import subprocess  # noqa: F401
import warnings  # noqa: F401
import zlib  # noqa: F401
from pathlib import Path  # noqa: F401
from typing import Any  # noqa: F401

import numpy as np

from ..fits_dataset import ScientificFitsDataset, image_hdu_index  # noqa: F401
from .abort import check_abort
from ._common import (  # noqa: F401
    clamped_sqrt,
    logger,
    _qt_zlib_compress,
    _b64f32,
    _b64i32,
    _b64f32_compressed,
    _b64i32_compressed,
    _finite_range,
    _integrated_spacing,
)


_HAS_GPU = False
try:
    import cupy as xp
    xp.zeros(1)  # Test GPU availability
    _HAS_GPU = True
except Exception:
    import numpy as xp

# ── Optional Dask ────────────────────────────────────────────────────────────

try:
    import dask.array as _da  # type: ignore[import-untyped]
    from dask.base import tokenize as _dask_tokenize  # type: ignore[import-untyped]
    _DASK_OK = True
except ImportError:
    _DASK_OK = False

# ── Optional Numba JIT ────────────────────────────────────────────────────────

try:
    from numba import njit, prange as _prange  # type: ignore[import-untyped]
    _NUMBA_OK = True
except ImportError:
    _NUMBA_OK = False

    def njit(*a, **kw):  # type: ignore[misc]
        """No-op decorator used when numba is unavailable."""
        def _wrap(f):  # type: ignore[misc]
            return f
        return _wrap

    _prange = range  # type: ignore[assignment]

# ── Parallel moment configuration ────────────────────────────────────────────

_MOMENT_THREADS = int(_os.environ.get("VISIVO_MOMENT_THREADS", "0"))
# 0 = auto: min(4, cpu_count); positive = explicit count

# Out-of-core moments: above this materialised channel-range size, M0/M1/M2 are
# computed by streaming spectral slabs from disk (constant memory) instead of
# loading the whole range into RAM — so SKA-scale cubes don't OOM. Small cubes
# keep the faster in-memory (GPU/Numba) path.
_MOMENT_STREAM_BYTES = int(_os.environ.get("VISIVO_MOMENT_STREAM_BYTES", str(2 * 1024 ** 3)))
_MOMENT_SLAB_CHANNELS = max(1, int(_os.environ.get("VISIVO_MOMENT_SLAB_CHANNELS", "64")))
# The in-memory path needs several times the channel range: the float32 copy,
# the float64 temporaries of the reduction (8.6 times the range at the peak of
# the numpy M2, measured with tracemalloc), and the mapped pages the kernel
# charges to the process. A range is kept in memory only when twelve times its
# size is free; a 512 MB range on a node limited to 4 GB (3.5 GB free) was
# killed in memory and computed streamed (measured on the SKAVA testbed).
_MOMENT_INMEMORY_FACTOR = 12


def _moment_stream_bytes() -> int:
    """Above this many bytes a channel range is streamed: the configured
    ceiling, and never more than a share of what the node has free right now
    (its cgroup's headroom in a container)."""
    from ..node_status import available_memory_bytes  # noqa: PLC0415

    ceiling = _MOMENT_STREAM_BYTES
    available = available_memory_bytes()
    if available is None:
        return ceiling
    return min(ceiling, available // _MOMENT_INMEMORY_FACTOR)
# Out-of-core means bounded memory, not just lazy reads. The dask path processes
# one block of channels per thread, and each block spawns a few temporaries (the
# finite mask, the clipped copy, the float64 product with the channel widths):
# about seven times the block's float32 size. A fixed 64-channel block is 0.6 GB
# on a 1586² plane and 1.6 GB on a 2466² one, so four threads needed 16-40 GB and
# a service capped at 8 GB was killed (measured, benchmark/remote). The block is
# therefore sized in BYTES: at most this many per block, at least one channel.
_MOMENT_CHUNK_BYTES = int(_os.environ.get("VISIVO_MOMENT_CHUNK_BYTES", str(64 * 1024 ** 2)))


class _FitsCubeSource:
    """The channel range of a FITS cube as dask's source, read block by block.

    ``dask.array.from_array`` copies a NumPy array before chunking it, and the
    array astropy returns for a memory-mapped FITS image is a plain ndarray over
    the mapping: handing it to dask materialised the WHOLE selected range in RAM
    (32 MiB for a 32 MiB cube, measured), so the "out-of-core" path held the
    cube in memory. This object is not an ndarray: it carries only the path and
    the range, and each block dask asks for is read from the mapping on demand.
    It pickles to a few fields, so a distributed worker maps its own slabs.

    It also carries the file's size and modification time when the graph was
    built, and refuses to read a file that has changed since: one computation
    must never mix two versions of a cube.
    """

    ndim = 3

    def __init__(self, path: str, hdu_index: int, z0: int, shape: tuple, dtype) -> None:
        self.path = str(path)
        self.hdu_index = int(hdu_index)
        self.z0 = int(z0)
        self.shape = tuple(int(n) for n in shape)
        self.dtype = np.dtype(dtype)
        st = _os.stat(self.path)
        self.signature = (int(st.st_size), int(st.st_mtime_ns))

    def __getitem__(self, key):
        cube = _mapped_cube(self.path, self.hdu_index, self.signature)
        view = cube[self.z0 : self.z0 + self.shape[0]]
        return np.array(view[key])   # a copy of this block only


_MAPPED_CUBES: dict[tuple, tuple] = {}
_MAPPED_CUBES_LOCK = __import__("threading").Lock()
_MAPPED_CUBES_MAX = 4


def _mapped_cube(path: str, hdu_index: int, signature: tuple[int, int]) -> np.ndarray:
    """The 3-D memory-mapped data of ``path``, opened once per process.

    ``signature`` is the (size, mtime) the caller's graph was built against; a
    file that no longer matches is an error, not a silent switch to the new
    content. The check is made on the descriptor that is then mapped. The
    cache holds at most a few open files and is shared by the threads of the
    local scheduler, so lookup, eviction and opening happen under one lock; the
    caller slices the returned mapping outside it.
    """
    from astropy.io import fits as _fits  # noqa: PLC0415

    want = (int(signature[0]), int(signature[1]))
    key = (path, int(hdu_index), want)
    with _MAPPED_CUBES_LOCK:
        hit = _MAPPED_CUBES.get(key)
        if hit is None:
            # Open the descriptor first and check THAT file, then map the same
            # descriptor: a path checked and opened separately could be replaced
            # in between, and the new content cached under the old signature.
            fh = open(path, "rb")  # noqa: SIM115 - kept open with the mapping
            try:
                st = _os.fstat(fh.fileno())
                if (int(st.st_size), int(st.st_mtime_ns)) != want:
                    raise RuntimeError(f"{path} changed while a moment was being computed.")
                hdul = _fits.open(fh, memmap=True, mode="readonly")
                data = ScientificFitsDataset.squeeze_to_3d(hdul[hdu_index].data)
            except BaseException:
                fh.close()
                raise
            while len(_MAPPED_CUBES) >= _MAPPED_CUBES_MAX:
                # Closing drops only the handles; a block another thread is
                # copying keeps the mapping alive through its own reference.
                _, (old_fh, old_hdul, _old) = _MAPPED_CUBES.popitem()
                old_hdul.close()
                old_fh.close()
            hit = _MAPPED_CUBES[key] = (fh, hdul, data)
        else:
            # A cached mapping is of the right version; the path must still be.
            st = _os.stat(path)
            if (int(st.st_size), int(st.st_mtime_ns)) != want:
                raise RuntimeError(f"{path} changed while a moment was being computed.")
        return hit[2]


def _moment_chunk_z(plane_bytes: int, requested: int) -> int:
    """Channels per dask block: as many as fit in ``_MOMENT_CHUNK_BYTES``, never
    more than ``requested`` and never fewer than one."""
    fit = _MOMENT_CHUNK_BYTES // max(1, int(plane_bytes))
    return int(max(1, min(int(requested), fit)))


# Higher-order moments that the Dask backend supports out-of-core (R-06 fallback).
_DASK_MOMENT_ORDERS = frozenset({3, 4, 5, 6, 8, 10})


def _n_moment_threads() -> int:
    """Return the number of threads to use for parallelisable moment orders."""
    n = _MOMENT_THREADS
    if n <= 0:
        try:
            n = min(4, _os.cpu_count() or 1)
        except Exception:
            n = 1
    return n

# ── Numba JIT moment kernels ──────────────────────────────────────────────────


@njit(parallel=True, cache=True)  # type: ignore[misc]
def _moment1_numba(
    cube: np.ndarray,
    spectral_coords: np.ndarray,
    spectral_delta: np.ndarray,
    valid_mask: np.ndarray,
) -> np.ndarray:
    """Intensity-weighted mean velocity (M1), parallelised over spatial pixels.

    Accepts only numpy primitive arrays (no Python objects) so it can be
    compiled by Numba.  Falls back silently to numpy when _NUMBA_OK is False.
    """
    nz = cube.shape[0]
    ny = cube.shape[1]
    nx = cube.shape[2]
    result = np.zeros((ny, nx), dtype=np.float64)
    weight_sum = np.zeros((ny, nx), dtype=np.float64)
    for y in _prange(ny):
        for x in range(nx):
            for z in range(nz):
                if valid_mask[z, y, x]:
                    # R-04: weight by intensity × channel width (I·Δv), matching
                    # the numpy and Dask paths so M1 is identical across backends
                    # even on a non-uniform spectral axis.
                    w = float(cube[z, y, x]) * spectral_delta[z]
                    result[y, x] += w * spectral_coords[z]
                    weight_sum[y, x] += w
    for y in _prange(ny):
        for x in range(nx):
            if weight_sum[y, x] != 0.0:
                result[y, x] /= weight_sum[y, x]
            else:
                result[y, x] = np.nan
    return result


@njit(parallel=True, cache=True)  # type: ignore[misc]
def _moment2_numba(
    cube: np.ndarray,
    spectral_coords: np.ndarray,
    spectral_delta: np.ndarray,
    valid_mask: np.ndarray,
) -> np.ndarray:
    """Intensity-weighted velocity dispersion (M2), parallelised over spatial pixels."""
    nz = cube.shape[0]
    ny = cube.shape[1]
    nx = cube.shape[2]
    # First pass: compute M1. R-04: weight by I·Δv (matches numpy/Dask paths).
    m1 = np.zeros((ny, nx), dtype=np.float64)
    weight_sum = np.zeros((ny, nx), dtype=np.float64)
    for y in _prange(ny):
        for x in range(nx):
            for z in range(nz):
                if valid_mask[z, y, x]:
                    w = float(cube[z, y, x]) * spectral_delta[z]
                    m1[y, x] += w * spectral_coords[z]
                    weight_sum[y, x] += w
    for y in _prange(ny):
        for x in range(nx):
            if weight_sum[y, x] != 0.0:
                m1[y, x] /= weight_sum[y, x]
            else:
                m1[y, x] = np.nan
    # Second pass: compute M2 numerator.
    result = np.zeros((ny, nx), dtype=np.float64)
    for y in _prange(ny):
        for x in range(nx):
            if weight_sum[y, x] != 0.0:
                for z in range(nz):
                    if valid_mask[z, y, x]:
                        w = float(cube[z, y, x]) * spectral_delta[z]
                        d = spectral_coords[z] - m1[y, x]
                        result[y, x] += w * d * d
                result[y, x] /= weight_sum[y, x]
            else:
                result[y, x] = np.nan
    return result


# ── Encoding helpers ──────────────────────────────────────────────────────────


def _finalize_moment2(
    image: "np.ndarray", order: int, moment2_variance: bool,
    moment_unit: str, spectral_unit: str
) -> tuple["np.ndarray", str]:
    """R-03: for order-2, return the intensity-weighted velocity **dispersion**
    (sigma = sqrt(max(variance, 0))) by default — matching CASA ``immoments``,
    CARTA and SoFiA — with the unit in the native spectral unit (e.g. km/s).

    When ``moment2_variance`` is set the raw variance sigma^2 is preserved (unit
    spectral_unit^2). All moment backends funnel their order-2 map through this
    single function so the two representations never diverge between paths.
    """
    if order != 2:
        return image, moment_unit
    if moment2_variance:
        return image, moment_unit  # keep variance + squared unit
    # NaNs (blanked voxels) propagate through maximum/sqrt and stay NaN.
    disp = clamped_sqrt(image).astype(image.dtype, copy=False)
    return disp, (spectral_unit or "")


def _moment_result_unit(order: int, bunit: str, spectral_unit: str) -> str:
    bunit = (bunit or "").strip()
    spectral_unit = (spectral_unit or "").strip()
    if order == 0:
        if bunit and spectral_unit:
            return f"{bunit} {spectral_unit}"
        return bunit or spectral_unit
    if order == 1:
        return spectral_unit
    if order == 2:
        return _squared_unit(spectral_unit)
    if order == 3:
        return "dimensionless (skewness)"
    if order == 4:
        return "dimensionless (excess kurtosis)"
    if order == 5:
        return "dimensionless (standardised 5th moment)"
    if order in {6, 8, 10}:
        return bunit
    return ""


# ── Worker: cube slice (single spectral plane) ────────────────────────────────


def _as_numpy(array) -> np.ndarray:
    """Helper to safely copy a tensor back to system RAM if it's on the GPU."""
    return array.get() if hasattr(array, 'get') else array


def _chunked_m0(
    cube: np.ndarray, valid_mask: np.ndarray, delta: np.ndarray, n_threads: int
) -> np.ndarray:
    """Moment 0 (nansum over spectral axis), optionally parallelised over CPU threads.

    Accumulates in float64 to avoid floating-point ordering differences
    between chunked and sequential paths; result is cast back to float32.
    Used as CPU fallback when CuPy/GPU is unavailable.
    """
    nz = cube.shape[0]
    if n_threads <= 1 or nz < 2 * n_threads:
        return np.nansum(
            np.where(valid_mask, cube, np.nan).astype(np.float64) * delta[:, None, None],
            axis=0,
        ).astype(np.float32)
    edges = np.linspace(0, nz, n_threads + 1, dtype=int)

    def _chunk(s: int, e: int) -> np.ndarray:
        c = cube[s:e].astype(np.float64)
        d = delta[s:e]
        return np.nansum(np.where(valid_mask[s:e], c, np.nan) * d[:, None, None], axis=0)

    with _cf.ThreadPoolExecutor(max_workers=n_threads) as pool:
        parts = list(pool.map(lambda se: _chunk(*se), zip(edges[:-1], edges[1:])))
    return np.sum(parts, axis=0).astype(np.float32)


def _chunked_m6(cube: np.ndarray, valid_mask: np.ndarray, n_threads: int) -> np.ndarray:
    """Moment 6 (sqrt of nanmean of squares), optionally parallelised over CPU threads."""
    nz = cube.shape[0]
    if n_threads <= 1 or nz < 2 * n_threads:
        safe = np.where(valid_mask, cube, np.nan)
        return np.sqrt(np.nanmean(safe * safe, axis=0, dtype=np.float32))
    edges = np.linspace(0, nz, n_threads + 1, dtype=int)

    def _chunk(s: int, e: int) -> tuple[np.ndarray, np.ndarray]:
        safe = np.where(valid_mask[s:e], cube[s:e], np.nan).astype(np.float64)
        count = np.sum(np.isfinite(safe), axis=0)
        sum_sq = np.nansum(safe * safe, axis=0)
        return sum_sq, count

    with _cf.ThreadPoolExecutor(max_workers=n_threads) as pool:
        results = list(pool.map(lambda se: _chunk(*se), zip(edges[:-1], edges[1:])))
    total_sum = np.sum([r[0] for r in results], axis=0)
    total_cnt = np.sum([r[1] for r in results], axis=0)
    out = np.full(total_cnt.shape, np.nan, dtype=np.float32)
    valid = total_cnt > 0
    out[valid] = np.sqrt(total_sum[valid] / total_cnt[valid]).astype(np.float32)
    return out


def _chunked_m10(cube: np.ndarray, valid_mask: np.ndarray, n_threads: int) -> np.ndarray:
    """Moment 10 (nanmin over spectral axis), optionally parallelised over CPU threads."""
    nz = cube.shape[0]
    if n_threads <= 1 or nz < 2 * n_threads:
        return np.nanmin(np.where(valid_mask, cube, np.nan), axis=0)
    edges = np.linspace(0, nz, n_threads + 1, dtype=int)

    def _chunk(s: int, e: int) -> np.ndarray:
        return np.nanmin(np.where(valid_mask[s:e], cube[s:e], np.nan), axis=0)

    with _cf.ThreadPoolExecutor(max_workers=n_threads) as pool:
        parts = list(pool.map(lambda se: _chunk(*se), zip(edges[:-1], edges[1:])))
    return np.nanmin(parts, axis=0).astype(np.float32)


def _moment_map_from_array(
    cube: np.ndarray,
    order: int,
    spectral_coordinates: np.ndarray,
    mask_enabled: bool,
    threshold_value: float,
) -> np.ndarray:
    """Moment-map computation with GPU acceleration when CuPy is available.

    When _HAS_GPU is True, orders M0, M6, M8, M10 are dispatched to the GPU
    via CuPy.  M1/M2 always go through the Numba JIT path (large cubes) or
    the numpy/cupy fallback; M3/M4/M5 run in float64 on CPU for precision.

    When _HAS_GPU is False, orders M0, M6, M10 are split across
    _n_moment_threads() CPU spectral chunks so that numpy's GIL-releasing
    array operations run in true parallel.
    """
    spectral_values_np = np.asarray(spectral_coordinates, dtype=np.float64)
    spectral_delta_np = _integrated_spacing(spectral_values_np)

    subset_np = np.asarray(cube, dtype=np.float32)
    valid_mask = np.isfinite(subset_np)
    if mask_enabled:
        valid_mask = valid_mask & (subset_np >= float(threshold_value))
    safe_np = np.where(valid_mask, subset_np, np.nan)

    n_threads = _n_moment_threads()

    if order == 0:
        if _HAS_GPU:
            spectral_delta_gpu = xp.asarray(spectral_delta_np, dtype=xp.float64)
            safe_gpu = xp.asarray(safe_np, dtype=xp.float32)
            res = xp.nansum(safe_gpu * spectral_delta_gpu[:, None, None], axis=0, dtype=xp.float32)
            m0 = _as_numpy(res)
        else:
            m0 = _chunked_m0(subset_np, valid_mask, spectral_delta_np, n_threads)
        # A pixel with no valid voxel (all channels NaN or masked out) has an
        # *undefined* integrated intensity, not zero: nansum would otherwise
        # report 0 and silently turn a blank line of sight into a real value.
        # Blank those pixels (matches M1/M2 and CARTA), which also lets the
        # worker-level "no valid voxels" guard fire when the whole map is empty.
        no_valid = ~valid_mask.any(axis=0)
        if no_valid.any():
            m0[no_valid] = np.nan
        return m0

    if order == 1:
        # Use Numba JIT path for large cubes (amortises JIT compilation cost).
        _NUMBA_THRESHOLD = 64
        used_numba = _NUMBA_OK and subset_np.shape[0] > _NUMBA_THRESHOLD
        logger.debug("moment M1: using %s path", "numba" if used_numba else "numpy")
        if used_numba:
            # _moment1_numba expects float32 cube, float64 spectral, bool mask.
            result_f64 = _moment1_numba(
                np.ascontiguousarray(subset_np, dtype=np.float32),
                np.ascontiguousarray(spectral_values_np, dtype=np.float64),
                np.ascontiguousarray(spectral_delta_np, dtype=np.float64),
                np.ascontiguousarray(valid_mask, dtype=np.bool_),
            )
            return result_f64.astype(np.float32)
        # Sequential numpy/cupy fallback.
        weights = spectral_delta_np[:, None, None]
        m0 = np.nansum(safe_np * weights, axis=0, dtype=np.float32)
        num = np.nansum(safe_np * spectral_values_np[:, None, None] * weights, axis=0, dtype=np.float32)
        out = np.full(m0.shape, np.nan, dtype=np.float32)
        valid = np.isfinite(m0) & (m0 != 0.0)
        out[valid] = num[valid] / m0[valid]
        return out

    if order == 2:
        # Use Numba JIT path for large cubes.
        _NUMBA_THRESHOLD = 64
        used_numba = _NUMBA_OK and subset_np.shape[0] > _NUMBA_THRESHOLD
        logger.debug("moment M2: using %s path", "numba" if used_numba else "numpy")
        if used_numba:
            result_f64 = _moment2_numba(
                np.ascontiguousarray(subset_np, dtype=np.float32),
                np.ascontiguousarray(spectral_values_np, dtype=np.float64),
                np.ascontiguousarray(spectral_delta_np, dtype=np.float64),
                np.ascontiguousarray(valid_mask, dtype=np.bool_),
            )
            return result_f64.astype(np.float32)
        # Sequential numpy fallback.
        weights = spectral_delta_np[:, None, None]
        m0 = np.nansum(safe_np * weights, axis=0, dtype=np.float32)
        m1_num = np.nansum(safe_np * spectral_values_np[:, None, None] * weights, axis=0, dtype=np.float32)
        m1 = np.full(m0.shape, np.nan, dtype=np.float32)
        valid = np.isfinite(m0) & (m0 != 0.0)
        m1[valid] = m1_num[valid] / m0[valid]
        diff = spectral_values_np[:, None, None] - m1[None, :, :]
        num = np.nansum(safe_np * diff * diff * weights, axis=0, dtype=np.float32)
        out = np.full(m0.shape, np.nan, dtype=np.float32)
        out[valid] = num[valid] / m0[valid]
        return out

    if order in {3, 4, 5}:
        # Higher-order central moments — share M0/M1/σ computation.
        # Always run on CPU in float64 for numerical precision.
        try:
            # Shape helpers.
            sv = spectral_values_np[:, None, None]   # (nz, 1, 1)
            weights = spectral_delta_np[:, None, None]

            m0 = np.nansum(safe_np * weights, axis=0, dtype=np.float64)  # float64 for precision
            valid_m0 = np.isfinite(m0) & (m0 != 0.0)

            m1 = np.full(m0.shape, np.nan, dtype=np.float64)
            m1_num = np.nansum(safe_np.astype(np.float64) * sv * weights, axis=0)
            m1[valid_m0] = m1_num[valid_m0] / m0[valid_m0]

            diff = sv - m1[None, :, :]            # (nz, ny, nx) velocity deviations

            # M2 = intensity-weighted variance (σ²)
            m2_num = np.nansum(safe_np.astype(np.float64) * diff * diff * weights, axis=0)
            m2 = np.full(m0.shape, np.nan, dtype=np.float64)
            m2[valid_m0] = m2_num[valid_m0] / m0[valid_m0]

            # Guard div-by-zero: replace non-positive variance with NaN
            sigma = np.sqrt(np.where(m2 > 0.0, m2, np.nan))  # (ny, nx), NaN where σ≤0

            out = np.full(m0.shape, np.nan, dtype=np.float32)

            if order == 3:
                # Normalised skewness: M3 / σ³  (dimensionless)
                m3_num = np.nansum(safe_np.astype(np.float64) * diff ** 3 * weights, axis=0)
                m3 = np.full(m0.shape, np.nan, dtype=np.float64)
                m3[valid_m0] = m3_num[valid_m0] / m0[valid_m0]
                sigma3 = sigma ** 3
                valid_s = valid_m0 & np.isfinite(sigma3) & (sigma3 != 0.0)
                out[valid_s] = (m3[valid_s] / sigma3[valid_s]).astype(np.float32)

            elif order == 4:
                # Excess kurtosis: M4 / σ⁴ - 3  (dimensionless)
                m4_num = np.nansum(safe_np.astype(np.float64) * diff ** 4 * weights, axis=0)
                m4 = np.full(m0.shape, np.nan, dtype=np.float64)
                m4[valid_m0] = m4_num[valid_m0] / m0[valid_m0]
                sigma4 = sigma ** 4
                valid_s = valid_m0 & np.isfinite(sigma4) & (sigma4 != 0.0)
                out[valid_s] = (m4[valid_s] / sigma4[valid_s] - 3.0).astype(np.float32)

            else:  # order == 5
                # Standardised 5th moment: μ₅ / σ⁵ (dimensionless, consistent with M3/M4)
                m5_num = np.nansum(safe_np.astype(np.float64) * diff ** 5 * weights, axis=0)
                m5 = np.full(m0.shape, np.nan, dtype=np.float64)
                m5[valid_m0] = m5_num[valid_m0] / m0[valid_m0]
                sigma5 = sigma ** 5
                valid_s = valid_m0 & np.isfinite(sigma5) & (sigma5 != 0.0)
                out[valid_s] = (m5[valid_s] / sigma5[valid_s]).astype(np.float32)

            return out

        except Exception:
            logger.exception("moment order %d failed in _moment_map_from_array", order)
            raise

    if order == 6:
        if _HAS_GPU:
            safe_gpu = xp.asarray(safe_np, dtype=xp.float32)
            res = xp.sqrt(xp.nanmean(safe_gpu * safe_gpu, axis=0, dtype=xp.float32))
            return _as_numpy(res)
        return _chunked_m6(subset_np, valid_mask, n_threads)

    if order == 8:
        if _HAS_GPU:
            safe_gpu = xp.asarray(safe_np, dtype=xp.float32)
            res = xp.nanmax(safe_gpu, axis=0)
            return _as_numpy(res)
        # Sequential: nanmax is harder to chunk correctly; numpy handles it well.
        return np.nanmax(safe_np, axis=0)

    if order == 10:
        if _HAS_GPU:
            safe_gpu = xp.asarray(safe_np, dtype=xp.float32)
            res = xp.nanmin(safe_gpu, axis=0)
            return _as_numpy(res)
        return _chunked_m10(subset_np, valid_mask, n_threads)

    raise ValueError(f"Unsupported moment order: {order}.")


def _moment_map_streaming(
    data: Any,                      # memmap-backed 3-D array [nz, ny, nx] (NOT materialised)
    z0: int,
    z1: int,
    order: int,
    spectral_coordinates: np.ndarray,
    mask_enabled: bool,
    threshold_value: float,
    stop_file: str | None = None,
) -> np.ndarray:
    """Out-of-core M0/M1/M2 — stream spectral slabs and accumulate running sums so
    the whole channel range never sits in RAM (SKA-scale cubes).

    ``stop_file`` (see app/compute/abort.py) is checked before every slab:
    an aborted task ends within one slab, with ``ComputationAborted``.

    Matches the in-memory (Numba) path for uniform spacing:
      M0 = Σ I·Δv, blanked where a pixel has no valid voxel;
      M1 = Σ(I·v) / ΣI  (blanked where ΣI ≤ 0);
      M2 = Σ (I·Δv)·(v − M1)² / Σ(I·Δv)  — a TRUE two-pass (a second slab sweep),
      not the Σ(I·v²)/ΣI − M1² identity: differencing avoids the ~1e22
      cancellation on large-offset (FREQ) axes and needs no non-negativity clamp.
    Memory is O(slab · ny · nx), independent of nz (M2 does two slab passes).
    R-04: M1/M2 weight by I·Δv, matching the Numba, numpy and Dask paths on a
    NON-uniform spectral axis (for a uniform axis the Δv factor cancels).
    """
    sv = np.asarray(spectral_coordinates, dtype=np.float64)
    ny, nx = int(data.shape[1]), int(data.shape[2])
    nch = z1 - z0 + 1
    if sv.shape[0] != nch:
        raise ValueError("spectral_coordinates must be the [z0:z1] slice (length nch).")
    delta = _integrated_spacing(sv)                     # per-channel Δv weight (M0)

    # The slab is sized in bytes, as the dask blocks are: 64 channels of a
    # 1024² plane are 256 MB of float32 and about seven times that in the
    # temporaries below, which a node limited to a few GB cannot hold.
    slab = _moment_chunk_z(ny * nx * 4, _MOMENT_SLAB_CHANNELS)

    def _slabs():
        """Yield (cs, ce, inten64, valid) for each spectral slab — only that slab
        is read from disk, so peak memory is O(slab · ny · nx), not O(nz)."""
        for cs in range(0, nch, slab):
            check_abort(stop_file)
            ce = min(cs + slab, nch)
            block = np.asarray(data[z0 + cs : z0 + ce, :, :], dtype=np.float32)
            valid = np.isfinite(block)
            if mask_enabled:
                valid &= block >= float(threshold_value)
            inten = np.where(valid, block.astype(np.float64), 0.0)  # invalid → 0 (dropped from sums)
            yield cs, ce, inten, valid

    # Pass 1: M0 = Σ(I·Δv), and the M1 weights Σ(I·Δv), Σ(I·Δv·v). R-04: the
    # weighted-intensity denominator is Σ(I·Δv) (= M0), not the raw ΣI.
    s0d = np.zeros((ny, nx), dtype=np.float64)      # Σ(I·Δv) — M0 and the M1/M2 weight
    s_iv = np.zeros((ny, nx), dtype=np.float64)     # Σ(I·Δv·v)
    any_valid = np.zeros((ny, nx), dtype=bool)
    for cs, ce, inten, valid in _slabs():
        d_slab = delta[cs:ce][:, None, None]
        iw = inten * d_slab                          # I·Δv
        s0d += np.sum(iw, axis=0)
        s_iv += np.sum(iw * sv[cs:ce][:, None, None], axis=0)
        any_valid |= valid.any(axis=0)

    if order == 0:
        m0 = s0d.astype(np.float32)
        m0[~any_valid] = np.nan   # no valid voxel → undefined, not zero (matches M1/M2 + CARTA)
        return m0

    pos = np.isfinite(s0d) & (s0d != 0.0)
    m1 = np.full((ny, nx), np.nan, dtype=np.float64)
    m1[pos] = s_iv[pos] / s0d[pos]                   # intensity-weighted mean velocity
    out = np.full((ny, nx), np.nan, dtype=np.float32)
    if order == 1:
        out[pos] = m1[pos].astype(np.float32)
        return out
    if order == 2:
        # Pass 2: Σ (I·Δv)·(v − M1)² / Σ(I·Δv) — the two-pass, computed exactly.
        # The differences are O(channel width), so there is no 1e22-scale
        # cancellation (unlike the ΣI·v² − M1² identity) and no clamp is needed.
        s_num = np.zeros((ny, nx), dtype=np.float64)
        for cs, ce, inten, _valid in _slabs():
            iw = inten * delta[cs:ce][:, None, None]
            dv = sv[cs:ce][:, None, None] - m1[None, :, :]
            s_num += np.sum(iw * dv * dv, axis=0)
        out[pos] = (s_num[pos] / s0d[pos]).astype(np.float32)
        return out
    raise ValueError(f"Streaming moment not supported for order {order}.")


def _auto_mask_threshold(
    data: np.ndarray, z0: int, z1: int, k: float = 3.0, max_samples: int = 4_000_000
) -> float:
    """Robust noise-based mask threshold: median + k·σ with σ = 1.4826·MAD.

    Estimated over the selected channel range [z0, z1]. To stay out-of-core on
    SKA-scale cubes the sample is strided in *both* the spectral and the two
    spatial axes so at most ~``max_samples`` voxels are ever materialised —
    important when a single spatial plane already exceeds the cap. Returns
    ``0.0`` when no finite voxel is available.
    """
    nz = z1 - z0 + 1
    ny, nx = int(data.shape[1]), int(data.shape[2])
    if nz <= 0 or ny == 0 or nx == 0:
        return 0.0
    # Cap sampled channels first, then split the remaining budget spatially so
    # the strided read (channels × pixels) stays within max_samples.
    max_planes = min(nz, 64)
    z_step = max(1, int(np.ceil(nz / max_planes)))
    n_planes = len(range(0, nz, z_step))
    spatial_budget = max(1, max_samples // n_planes)
    sp = max(1, int(np.ceil((float(ny * nx) / spatial_budget) ** 0.5)))
    sample = np.asarray(data[z0 : z1 + 1 : z_step, ::sp, ::sp], dtype=np.float32)
    finite = sample[np.isfinite(sample)]
    if finite.size == 0:
        return 0.0
    med = float(np.median(finite))
    mad = float(np.median(np.abs(finite - med)))
    return med + k * (1.4826 * mad)


def _apply_position_moment_range_guard(
    image: np.ndarray, order: int, spectral_coordinates: np.ndarray
) -> None:
    """Blank physically-impossible M1/M2 voxels in place.

    An intensity-weighted mean velocity (M1) must lie within the sampled
    spectral axis [vmin, vmax], and a weighted variance (M2) within [0, span²].
    Values outside can only arise from mixed-sign (noise) weights when no mask
    is applied; they would otherwise dominate the colour scale (e.g.
    superluminal M1) and hide the real signal. Positive-weight moments are
    bounded by construction, so this never removes a legitimate voxel. No-op for
    orders other than 1 and 2.
    """
    if order not in (1, 2):
        return
    coords = np.asarray(spectral_coordinates, dtype=np.float64)
    if not coords.size:
        return
    vmin = float(np.nanmin(coords))
    vmax = float(np.nanmax(coords))
    if order == 1:
        bad = np.isfinite(image) & ((image < vmin) | (image > vmax))
    else:  # order == 2 — variance
        span = vmax - vmin
        bad = np.isfinite(image) & ((image < 0.0) | (image > span * span))
    if bad.any():
        image[bad] = np.nan


def _moment_map_dask(
    fits_path: str,
    order: int,
    channel_start: int,
    channel_end: int,
    mask_threshold: float,
    mask_enabled: bool,
    chunk_z: int = 64,
    dist_client: Any = None,
    threshold_auto: bool = False,
    moment2_variance: bool = False,
) -> dict[str, Any]:
    """Out-of-core moment computation via Dask arrays.

    Uses ``dask.array.from_array`` so that only the spectral chunks needed for
    each reduction step are loaded from disk at a time.  Designed for cubes
    that do not fit in RAM (SKA-scale).

    Returns the same dict structure as ``worker_moment``.
    """
    from astropy.io import fits as _fits  # noqa: PLC0415 (lazy import in worker)

    dataset = ScientificFitsDataset(fits_path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Dask moment requires a 3D cube.")
        z0 = max(0, min(int(channel_start), data.shape[0] - 1))
        z1 = max(0, min(int(channel_end), data.shape[0] - 1))
        if z0 > z1:
            raise ValueError("Invalid channel range.")
        effective_threshold = float(mask_threshold)
        if mask_enabled and threshold_auto:
            effective_threshold = _auto_mask_threshold(data, z0, z1)
        spectral = dataset.spectral_axis_metadata(z0, z1)
        # Wrap the memmap slice in a Dask array — data is NOT loaded yet.
        source = _FitsCubeSource(fits_path, dataset.hdu_index, z0,
                                 (z1 - z0 + 1, int(data.shape[1]), int(data.shape[2])),
                                 data.dtype)
        chunk_z = _moment_chunk_z(source.shape[1] * source.shape[2] * source.dtype.itemsize,
                                  chunk_z)
        # An explicit name (dask would otherwise tokenise the source object) that
        # changes with anything that changes the blocks: file, version, HDU,
        # channel range and block size.
        da_cube = _da.from_array(
            source, chunks=(chunk_z, -1, -1), lock=False, fancy=False,
            name="fits-" + _dask_tokenize(source.path, source.signature, source.hdu_index,
                                          z0, z1, chunk_z))
        spectral_values = np.asarray(spectral["coordinates"], dtype=np.float64)
        spectral_delta = _integrated_spacing(spectral_values)
        bunit = dataset.bunit
        moment_unit = _moment_result_unit(order, bunit, str(spectral["axis_unit"]))
    finally:
        hdul.close()

    # Apply validity mask lazily.
    da_safe = _da.where(_da.isfinite(da_cube), da_cube, np.nan)
    if mask_enabled:
        da_safe = _da.where(da_safe >= float(effective_threshold), da_safe, np.nan)

    da_delta = _da.from_array(spectral_delta[:, None, None], chunks=(chunk_z, -1, -1))

    def compute(*arrays):
        """Evaluate dask arrays together: on the distributed cluster if one is
        configured (each worker maps its own slabs from the shared filesystem),
        else on the local threaded scheduler."""
        if dist_client is not None:
            return tuple(dist_client.compute(list(arrays), sync=True))
        return _da.compute(*arrays, scheduler="threads", num_workers=_n_moment_threads())

    # Every reduction below consumes each block as soon as it is read. Moments
    # that need the centroid first are computed in two passes, the second
    # re-reading the blocks from disk with the centroid as a plain array: a
    # single graph would keep every block of the first pass in memory until the
    # centroid was known, i.e. the whole cube.
    with np.errstate(invalid="ignore", divide="ignore"):
        if order == 0:
            (result,) = compute(_da.nansum(da_safe * da_delta, axis=0))
        elif order == 6:
            (result,) = compute(_da.sqrt(_da.nanmean(da_safe * da_safe, axis=0)))
        elif order == 8:
            (result,) = compute(_da.nanmax(da_safe, axis=0))
        elif order == 10:
            (result,) = compute(_da.nanmin(da_safe, axis=0))
        elif order in {1, 2, 3, 4, 5}:
            da_sv = _da.from_array(spectral_values[:, None, None], chunks=(chunk_z, -1, -1))
            m0, num = compute(_da.nansum(da_safe * da_delta, axis=0),
                              _da.nansum(da_safe * da_sv * da_delta, axis=0))
            valid_m0 = m0 != 0.0
            m1 = np.where(valid_m0, num / np.where(valid_m0, m0, 1.0), np.nan)
            if order == 1:
                result = m1
            else:
                diff = da_sv - m1[None, :, :]
                if order == 2:
                    (m2_num,) = compute(_da.nansum(da_safe * diff * diff * da_delta, axis=0))
                    result = np.where(valid_m0, m2_num / np.where(valid_m0, m0, 1.0), np.nan)
                else:
                    m2_num, mk_num = compute(
                        _da.nansum(da_safe * diff * diff * da_delta, axis=0),
                        _da.nansum(da_safe * diff ** order * da_delta, axis=0))
                    safe_m0 = np.where(valid_m0, m0, 1.0)
                    m2 = np.where(valid_m0, m2_num / safe_m0, np.nan)
                    mk_raw = np.where(valid_m0, mk_num / safe_m0, np.nan)
                    sigma = np.sqrt(np.where(m2 > 0.0, m2, np.nan))
                    norm = np.where(sigma != 0.0, mk_raw / sigma ** order, np.nan)
                    result = norm - 3.0 if order == 4 else norm
        else:
            raise ValueError(f"Unsupported moment order for Dask path: {order}.")
    image = np.asarray(result).astype(np.float32)

    _apply_position_moment_range_guard(image, order, spectral_values)
    # R-03: order-2 default → velocity dispersion (sqrt of variance), same as
    # the in-memory/streaming path so the two never diverge.
    image, moment_unit = _finalize_moment2(
        image, order, moment2_variance, moment_unit, str(spectral["axis_unit"]))

    if not np.isfinite(image).any():
        raise ValueError("No valid voxels found for the selected moment parameters (dask path).")

    range_min, range_max = _finite_range(image)
    return {
        "width": int(image.shape[1]),
        "height": int(image.shape[0]),
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "spectral_axis_type": str(spectral["axis_type"]),
        "spectral_axis_unit": str(spectral["axis_unit"]),
        "moment_unit": moment_unit,
        "bunit": bunit,
        "moment_threads_used": _n_moment_threads(),
        "threshold_used": float(effective_threshold) if mask_enabled else None,
        "data_base64": _b64f32(image),
    }


def _squared_unit(spectral_unit: str) -> str:
    """Unit of a variance (moment-2 σ²). Parenthesise compound units so a
    ``km/s`` axis reads as ``(km/s)^2`` (a squared velocity) rather than
    ``km/s^2`` (which parses as an acceleration)."""
    su = (spectral_unit or "").strip()
    if not su:
        return ""
    return f"({su})^2" if ("/" in su or " " in su) else f"{su}^2"


def _moment_unit(order: int, bunit: str, spectral_unit: str) -> str:
    """Derive the physical unit of a moment map from the FITS header units."""
    if order == 0:
        parts = [p for p in (bunit, spectral_unit) if p]
        return " ".join(parts)
    if order == 1:
        return spectral_unit
    if order == 2:
        return _squared_unit(spectral_unit)
    # Orders 6, 8, 10 preserve the intensity unit.
    return bunit


def worker_moment(
    path: str,
    order: int,
    channel_start: int,
    channel_end: int,
    mask_enabled: bool,
    threshold_value: float,
    threshold_auto: bool = False,
    moment2_variance: bool = False,
    stop_file: str | None = None,
) -> dict[str, Any]:
    """
    Compute a moment map reading only [channel_start:channel_end] planes.
    With memmap=True the slice read is O(nchannels * ny * nx) – not the
    full cube size.

    When ``mask_enabled`` and ``threshold_auto`` are both set, the mask
    threshold is derived from the data (median + 3·σ via MAD) instead of using
    the supplied ``threshold_value`` — the CASA/SoFiA default that keeps M1/M2
    clean without the caller having to know the noise level.

    ``stop_file`` makes the computation interruptible (app/compute/abort.py):
    the streaming path checks it before every slab, the in-memory and Dask
    paths before they start; an aborted task raises ``ComputationAborted``.
    """
    check_abort(stop_file)
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Moment products require a cube dataset.")
        z0 = max(0, min(int(channel_start), data.shape[0] - 1))
        z1 = max(0, min(int(channel_end), data.shape[0] - 1))
        if z0 > z1:
            raise ValueError("Invalid channel range.")
        effective_threshold = float(threshold_value)
        if mask_enabled and threshold_auto:
            effective_threshold = _auto_mask_threshold(data, z0, z1)
        check_abort(stop_file)
        spectral = dataset.spectral_axis_metadata(z0, z1)
        # Out-of-core path for the big/OOM-prone orders: if the channel range would
        # materialise to more than the stream threshold, stream slabs from disk
        # instead of loading it all. Small cubes keep the in-memory GPU/Numba path.
        subset_bytes = (z1 - z0 + 1) * int(data.shape[1]) * int(data.shape[2]) * 4
        big = subset_bytes > _moment_stream_bytes()
        if order in (0, 1, 2) and big:
            image = _moment_map_streaming(
                data, z0, z1, order, spectral["coordinates"], mask_enabled, effective_threshold,
                stop_file)
        elif order in _DASK_MOMENT_ORDERS and big and _DASK_OK:
            # R-06: higher-order moments (3–10) have no bespoke slab streamer;
            # when the subset would OOM the in-memory path AND Dask is available,
            # compute them via the local-threaded Dask backend, which reduces the
            # cube out-of-core (chunked) instead of materialising several
            # cube-sized float64 temporaries. If Dask is unavailable we fall
            # through to the in-memory path (correct, but may use more memory).
            # _moment_map_dask reopens the file itself and returns the full result
            # dict (range guard + dispersion applied); the finally: closes hdul.
            return _moment_map_dask(
                path, order, z0, z1, effective_threshold, mask_enabled,
                64, None, False, moment2_variance,
            )
        else:
            subset = np.asarray(data[z0 : z1 + 1, :, :], dtype=np.float32)
            image = _moment_map_from_array(
                subset,
                order,
                spectral["coordinates"],
                mask_enabled,
                effective_threshold,
            )
        # Physical-range guard for the position moments (blanks superluminal-M1
        # / out-of-range-M2 noise voxels; shared with the Dask path).
        _apply_position_moment_range_guard(image, order, spectral["coordinates"])
        bunit = dataset.bunit
        moment_unit = _moment_result_unit(order, bunit, str(spectral["axis_unit"]))
        # R-03: order-2 default → velocity dispersion (sqrt of variance).
        image, moment_unit = _finalize_moment2(
            image, order, moment2_variance, moment_unit, str(spectral["axis_unit"]))
        logger.info(
            "[moment] provenance path=%s bunit=%s beam=(%s,%s,%s)",
            dataset.path,
            bunit or "-",
            dataset.beam.major,
            dataset.beam.minor,
            dataset.beam.pa,
        )
    finally:
        hdul.close()

    if not np.isfinite(image).any():
        raise ValueError("No valid voxels found for the selected moment parameters.")

    range_min, range_max = _finite_range(image)
    return {
        "width": int(image.shape[1]),
        "height": int(image.shape[0]),
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "spectral_axis_type": str(spectral["axis_type"]),
        "spectral_axis_unit": str(spectral["axis_unit"]),
        "moment_unit": moment_unit,
        "bunit": bunit,
        "moment_threads_used": _n_moment_threads(),
        "threshold_used": float(effective_threshold) if mask_enabled else None,
        "data_base64": _b64f32(image),
    }


def worker_moment_save_fits(
    src_path: str,
    output_path: str,
    order: int,
    channel_start: int,
    channel_end: int,
    mask_enabled: bool,
    threshold_value: float,
    threshold_auto: bool = False,
    moment2_variance: bool = False,
    stop_file: str | None = None,
) -> dict[str, Any]:
    """Compute a moment map and persist it as a standalone 2-D FITS file at
    ``output_path``.

    Reuses :func:`worker_moment` for the actual computation so the science
    behaviour stays single-sourced (same masking, same units, same channel
    clamping rules). The wrapper additionally:

      * extracts the **celestial** WCS from the source cube and writes a
        valid 2-D header (spectral axis dropped),
      * sets ``BUNIT`` to the derived moment unit (e.g. ``K m/s`` for M0
        of a K cube in velocity, ``m/s`` for M1, ``(m/s)^2`` for M2…),
      * adds HISTORY cards recording the source file, order, channel
        range, and masking parameters so the artefact is self-documenting.
    """
    from astropy.io import fits as _fits  # noqa: PLC0415
    from astropy.wcs import WCS as _WCS   # noqa: PLC0415

    # Decode the array data once via the canonical moment worker.
    payload = worker_moment(
        src_path, int(order), int(channel_start), int(channel_end),
        bool(mask_enabled), float(threshold_value), bool(threshold_auto),
        bool(moment2_variance), stop_file,
    )
    check_abort(stop_file)   # not a byte of an aborted job's product is written
    # The base64 round-trip is unnecessary for the FITS path but keeps a
    # single source of truth — the cost is one float32 array decode, dwarfed
    # by the moment compute itself.
    image_bytes = base64.b64decode(payload["data_base64"])
    image = np.frombuffer(image_bytes, dtype=np.float32).reshape(
        int(payload["height"]), int(payload["width"]),
    ).copy()  # detach from the read-only buffer before writing

    # Build a 2-D header from the source WCS — celestial() projects away the
    # spectral / Stokes axes and gives us a clean (RA, Dec) or (l, b) header.
    with _fits.open(src_path, memmap=True, mode="readonly") as hdul:
        src_header = hdul[image_hdu_index(hdul)].header.copy()
    try:
        wcs2d = _WCS(src_header, relax=True).celestial
        out_header = wcs2d.to_header()
    except Exception as exc:
        logger.warning(
            "[moment_save_fits] WCS extraction failed for %s (%s), "
            "writing array with no celestial WCS.",
            src_path, exc,
        )
        out_header = _fits.Header()

    out_header["NAXIS"]  = 2
    out_header["NAXIS1"] = int(image.shape[1])
    out_header["NAXIS2"] = int(image.shape[0])
    moment_unit = str(payload.get("moment_unit") or "")
    if moment_unit:
        out_header["BUNIT"] = moment_unit
    out_header["OBJECT"] = src_header.get("OBJECT", out_header.get("OBJECT", ""))
    threshold_used = payload.get("threshold_used", float(threshold_value))
    mask_desc = "off"
    if mask_enabled:
        mode = "auto" if threshold_auto else "manual"
        mask_desc = f"on ({mode}, threshold={float(threshold_used):.6g})"
    out_header.add_history(
        f"VisIVO moment-{int(order)} map from {Path(src_path).name}: "
        f"channels {int(channel_start)}..{int(channel_end)} "
        f"mask={mask_desc}"
    )

    logger.info(
        "[moment_save_fits] writing FITS  src=%s  out=%s  order=%s  "
        "channels=%s..%s  shape=%s  bunit=%s",
        src_path, output_path, order, channel_start, channel_end,
        image.shape, moment_unit or "-",
    )
    out_hdu = _fits.PrimaryHDU(data=image, header=out_header)
    out_hdu.writeto(output_path, overwrite=True)
    logger.info("[moment_save_fits] FITS written successfully")

    return {
        "width":       int(image.shape[1]),
        "height":      int(image.shape[0]),
        "range_min":   float(payload["range_min"]),
        "range_max":   float(payload["range_max"]),
        "bunit":       moment_unit,
        "moment_unit": moment_unit,
        "output_path": output_path,
    }


# ── Worker: isosurface ────────────────────────────────────────────────────────
