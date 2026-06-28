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

from ..fits_dataset import ScientificFitsDataset  # noqa: F401
from ._common import (  # noqa: F401
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
                    w = float(cube[z, y, x])
                    result[y, x] += w * spectral_coords[z]
                    weight_sum[y, x] += w
    for y in _prange(ny):
        for x in range(nx):
            if weight_sum[y, x] > 0.0:
                result[y, x] /= weight_sum[y, x]
            else:
                result[y, x] = np.nan
    return result


@njit(parallel=True, cache=True)  # type: ignore[misc]
def _moment2_numba(
    cube: np.ndarray,
    spectral_coords: np.ndarray,
    valid_mask: np.ndarray,
) -> np.ndarray:
    """Intensity-weighted velocity dispersion (M2), parallelised over spatial pixels."""
    nz = cube.shape[0]
    ny = cube.shape[1]
    nx = cube.shape[2]
    # First pass: compute M1.
    m1 = np.zeros((ny, nx), dtype=np.float64)
    weight_sum = np.zeros((ny, nx), dtype=np.float64)
    for y in _prange(ny):
        for x in range(nx):
            for z in range(nz):
                if valid_mask[z, y, x]:
                    w = float(cube[z, y, x])
                    m1[y, x] += w * spectral_coords[z]
                    weight_sum[y, x] += w
    for y in _prange(ny):
        for x in range(nx):
            if weight_sum[y, x] > 0.0:
                m1[y, x] /= weight_sum[y, x]
            else:
                m1[y, x] = np.nan
    # Second pass: compute M2 numerator.
    result = np.zeros((ny, nx), dtype=np.float64)
    for y in _prange(ny):
        for x in range(nx):
            if weight_sum[y, x] > 0.0:
                for z in range(nz):
                    if valid_mask[z, y, x]:
                        w = float(cube[z, y, x])
                        d = spectral_coords[z] - m1[y, x]
                        result[y, x] += w * d * d
                result[y, x] /= weight_sum[y, x]
            else:
                result[y, x] = np.nan
    return result


# ── Encoding helpers ──────────────────────────────────────────────────────────


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
        return f"{spectral_unit}^2" if spectral_unit else ""
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


def _moment_map_dask(
    fits_path: str,
    order: int,
    channel_start: int,
    channel_end: int,
    mask_threshold: float,
    mask_enabled: bool,
    chunk_z: int = 64,
    dist_client: Any = None,
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
        spectral = dataset.spectral_axis_metadata(z0, z1)
        # Wrap the memmap slice in a Dask array — data is NOT loaded yet.
        sub_memmap = data[z0 : z1 + 1, :, :]
        da_cube = _da.from_array(sub_memmap, chunks=(chunk_z, -1, -1))
        spectral_values = np.asarray(spectral["coordinates"], dtype=np.float64)
        spectral_delta = _integrated_spacing(spectral_values)
        bunit = dataset.bunit
        moment_unit = _moment_result_unit(order, bunit, str(spectral["axis_unit"]))
    finally:
        hdul.close()

    # Apply validity mask lazily.
    da_safe = _da.where(_da.isfinite(da_cube), da_cube, np.nan)
    if mask_enabled:
        da_safe = _da.where(da_safe >= float(mask_threshold), da_safe, np.nan)

    da_delta = _da.from_array(spectral_delta[:, None, None], chunks=(chunk_z, -1, -1))

    if order == 0:
        da_result = _da.nansum(da_safe * da_delta, axis=0)
    elif order == 6:
        da_result = _da.sqrt(_da.nanmean(da_safe * da_safe, axis=0))
    elif order == 8:
        da_result = _da.nanmax(da_safe, axis=0)
    elif order == 10:
        da_result = _da.nanmin(da_safe, axis=0)
    elif order == 1:
        da_sv = _da.from_array(spectral_values[:, None, None], chunks=(chunk_z, -1, -1))
        m0 = _da.nansum(da_safe * da_delta, axis=0)
        num = _da.nansum(da_safe * da_sv * da_delta, axis=0)
        da_result = _da.where(m0 != 0.0, num / m0, np.nan)
    elif order == 2:
        da_sv = _da.from_array(spectral_values[:, None, None], chunks=(chunk_z, -1, -1))
        m0 = _da.nansum(da_safe * da_delta, axis=0)
        m1 = _da.where(m0 != 0.0, _da.nansum(da_safe * da_sv * da_delta, axis=0) / m0, np.nan)
        diff = da_sv - m1[None, :, :]
        da_result = _da.where(m0 != 0.0,
                              _da.nansum(da_safe * diff * diff * da_delta, axis=0) / m0,
                              np.nan)
    elif order in {3, 4, 5}:
        da_sv = _da.from_array(spectral_values[:, None, None], chunks=(chunk_z, -1, -1))
        m0 = _da.nansum(da_safe * da_delta, axis=0)
        valid_m0 = m0 != 0.0
        m1 = _da.where(valid_m0, _da.nansum(da_safe * da_sv * da_delta, axis=0) / m0, np.nan)
        diff = da_sv - m1[None, :, :]
        m2 = _da.where(valid_m0,
                       _da.nansum(da_safe * diff * diff * da_delta, axis=0) / m0,
                       np.nan)
        sigma = _da.sqrt(_da.where(m2 > 0.0, m2, np.nan))
        if order == 3:
            m3_raw = _da.where(valid_m0,
                               _da.nansum(da_safe * diff ** 3 * da_delta, axis=0) / m0,
                               np.nan)
            da_result = _da.where(sigma != 0.0, m3_raw / sigma ** 3, np.nan)
        elif order == 4:
            m4_raw = _da.where(valid_m0,
                               _da.nansum(da_safe * diff ** 4 * da_delta, axis=0) / m0,
                               np.nan)
            da_result = _da.where(sigma != 0.0, m4_raw / sigma ** 4 - 3.0, np.nan)
        else:  # order == 5
            m5_raw = _da.where(valid_m0,
                               _da.nansum(da_safe * diff ** 5 * da_delta, axis=0) / m0,
                               np.nan)
            da_result = _da.where(sigma != 0.0, m5_raw / sigma ** 5, np.nan)
    else:
        raise ValueError(f"Unsupported moment order for Dask path: {order}.")

    # Distributed cluster path: dispatch the graph to all dask workers
    # across all nodes. Each worker mmaps its own slab from the FITS file
    # on the shared filesystem and computes a partial reduction; the
    # final aggregation happens on the scheduler. On SKA-scale cubes the
    # speedup is roughly linear in worker count until BeeGFS sequential
    # throughput saturates.
    if dist_client is not None:
        image = dist_client.compute(da_result, sync=True).astype(np.float32)
    else:
        # Local fallback: in-process thread pool. Fine for small cubes
        # and when no Dask cluster is configured.
        image = da_result.compute(
            scheduler="threads", num_workers=_n_moment_threads()
        ).astype(np.float32)

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
        "data_base64": _b64f32(image),
    }


def _moment_unit(order: int, bunit: str, spectral_unit: str) -> str:
    """Derive the physical unit of a moment map from the FITS header units."""
    if order == 0:
        parts = [p for p in (bunit, spectral_unit) if p]
        return " ".join(parts)
    if order == 1:
        return spectral_unit
    if order == 2:
        return f"{spectral_unit}^2" if spectral_unit else ""
    # Orders 6, 8, 10 preserve the intensity unit.
    return bunit


def worker_moment(
    path: str,
    order: int,
    channel_start: int,
    channel_end: int,
    mask_enabled: bool,
    threshold_value: float,
) -> dict[str, Any]:
    """
    Compute a moment map reading only [channel_start:channel_end] planes.
    With memmap=True the slice read is O(nchannels * ny * nx) – not the
    full cube size.
    """
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
        spectral = dataset.spectral_axis_metadata(z0, z1)
        subset = np.asarray(data[z0 : z1 + 1, :, :], dtype=np.float32)
        image = _moment_map_from_array(
            subset,
            order,
            spectral["coordinates"],
            mask_enabled,
            threshold_value,
        )
        bunit = dataset.bunit
        moment_unit = _moment_result_unit(order, bunit, str(spectral["axis_unit"]))
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
        bool(mask_enabled), float(threshold_value),
    )
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
        src_header = hdul[0].header.copy()
    try:
        wcs2d = _WCS(src_header, relax=True).celestial
        out_header = wcs2d.to_header()
    except Exception as exc:
        logger.warning(
            "[moment_save_fits] WCS extraction failed for %s (%s) — "
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
    out_header.add_history(
        f"VisIVO moment-{int(order)} map from {Path(src_path).name}: "
        f"channels {int(channel_start)}..{int(channel_end)} "
        f"mask={'on' if mask_enabled else 'off'} "
        f"threshold={float(threshold_value):.6g}"
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
