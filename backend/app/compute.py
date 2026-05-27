"""
compute.py – R2 + R4: CPU-bound worker functions for the process pool.

All functions in this module are designed to run inside a
concurrent.futures.ProcessPoolExecutor worker process.  They MUST:

  • be importable at the top level (picklable by multiprocessing)
  • open FITS files themselves with memmap=True so only the needed
    pixels are loaded from disk (R4 – lazy I/O, no full-cube RAM load)
  • accept / return only pickle-serialisable types (str, dict, list,
    numpy dtypes that are base64-encoded before returning)

Functions in this module intentionally do NOT import FastAPI or Pydantic
so they can be reused in batch / headless pipelines without the web stack.
"""

from __future__ import annotations

import base64
import concurrent.futures as _cf
import logging
import os as _os
import zlib
from pathlib import Path
from typing import Any

import numpy as np

_HAS_GPU = False
try:
    import cupy as xp
    xp.zeros(1)  # Test GPU availability
    _HAS_GPU = True
except Exception:
    import numpy as xp

from .fits_dataset import ScientificFitsDataset

logger = logging.getLogger("visivo.compute")

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


def _qt_zlib_compress(raw: bytes) -> bytes:
    """Prepend the uncompressed length as a 4-byte big-endian uint (Qt convention)."""
    return len(raw).to_bytes(4, byteorder="big", signed=False) + zlib.compress(raw)


def _b64f32(array: np.ndarray) -> str:
    return base64.b64encode(np.ascontiguousarray(array, dtype=np.float32).tobytes()).decode("ascii")


def _b64i32(array: np.ndarray) -> str:
    return base64.b64encode(np.ascontiguousarray(array, dtype=np.int32).tobytes()).decode("ascii")


def _b64f32_compressed(array: np.ndarray) -> tuple[str, str]:
    raw = np.ascontiguousarray(array, dtype=np.float32).tobytes()
    return "qt-zlib", base64.b64encode(_qt_zlib_compress(raw)).decode("ascii")


def _b64i32_compressed(array: np.ndarray) -> tuple[str, str]:
    raw = np.ascontiguousarray(array, dtype=np.int32).tobytes()
    return "qt-zlib", base64.b64encode(_qt_zlib_compress(raw)).decode("ascii")


def _finite_range(array: np.ndarray) -> tuple[float, float]:
    finite = np.isfinite(array)
    if not finite.any():
        return 0.0, 0.0
    return float(np.nanmin(array)), float(np.nanmax(array))


def _integrated_spacing(coords: np.ndarray) -> np.ndarray:
    if coords.size == 0:
        return np.zeros(0, dtype=np.float64)
    if coords.size == 1:
        return np.ones(1, dtype=np.float64)
    edges = np.empty(coords.size + 1, dtype=np.float64)
    edges[1:-1] = 0.5 * (coords[:-1] + coords[1:])
    edges[0] = coords[0] - 0.5 * (coords[1] - coords[0])
    edges[-1] = coords[-1] + 0.5 * (coords[-1] - coords[-2])
    return np.abs(np.diff(edges))


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


def worker_cube_slice(path: str, z_index: int) -> dict[str, Any]:
    """
    Extract a single Z-axis slice from a FITS cube.

    With memmap=True astropy reads only the bytes for the requested plane
    from disk – O(width * height * 4 bytes) I/O regardless of cube depth.
    """
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Slice endpoint requires a 3D cube.")
        if z_index < 0 or z_index >= data.shape[0]:
            raise ValueError(f"Slice index {z_index} out of range [0, {data.shape[0] - 1}].")
        # Materialise ONLY this plane – memmap ensures disk read is minimal.
        plane = np.asarray(data[z_index, :, :], dtype=np.float32)
    finally:
        hdul.close()

    range_min, range_max = _finite_range(plane)
    compression, data_b64 = _b64f32_compressed(plane)
    return {
        "width": int(plane.shape[1]),
        "height": int(plane.shape[0]),
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "compression": compression,
        "data_base64": data_b64,
    }


# ── Worker: cube subvolume ────────────────────────────────────────────────────


def worker_cube_subvolume(
    path: str, x0: int, x1: int, y0: int, y1: int, z0: int, z1: int
) -> dict[str, Any]:
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Subvolume endpoint requires a 3D cube.")
        depth, height, width = data.shape
        x0, x1 = max(0, x0), min(x1, width - 1)
        y0, y1 = max(0, y0), min(y1, height - 1)
        z0, z1 = max(0, z0), min(z1, depth - 1)
        if x0 > x1 or y0 > y1 or z0 > z1:
            raise ValueError("Invalid subvolume ROI.")
        sub = np.asarray(data[z0 : z1 + 1, y0 : y1 + 1, x0 : x1 + 1], dtype=np.float32)
    finally:
        hdul.close()

    range_min, range_max = _finite_range(sub)

    # Spectral metadata for each channel in the sub-volume — used by
    # Channel Maps to label each panel with velocity / frequency.
    try:
        spectral = dataset.spectral_axis_metadata(z0, z1)
        spectral_coords = list(spectral.get("coordinates", []))
        spectral_axis_type = str(spectral.get("axis_type", ""))
        spectral_axis_unit = str(spectral.get("axis_unit", ""))
    except Exception:
        spectral_coords = []
        spectral_axis_type = ""
        spectral_axis_unit = ""

    return {
        "width": int(sub.shape[2]),
        "height": int(sub.shape[1]),
        "depth": int(sub.shape[0]),
        "scalar_type": "float32",
        "range_min": float(range_min),
        "range_max": float(range_max),
        "spectral_axis_type": spectral_axis_type,
        "spectral_axis_unit": spectral_axis_unit,
        "bunit": dataset.bunit or "",
        "data_base64": _b64f32(sub),
    }


# ── Worker: save a cube sub-region as a new FITS file ────────────────────────


def worker_cube_save_subregion(
    src_path: str,
    output_path: str,
    x0: int, x1: int,
    y0: int, y1: int,
    z0: int, z1: int,
) -> dict[str, Any]:
    """Crop the source cube to ``[z0..z1, y0..y1, x0..x1]`` (all inclusive,
    0-indexed) and write the result as a standalone FITS file at
    ``output_path``.

    The output preserves the source WCS, only shifting the reference pixels
    (``CRPIX1/2/3``) to account for the new origin so coordinates of any
    pixel in the cropped cube match the same physical position in the
    source. ``BUNIT`` and the spectral keywords (``CTYPE3``, ``CUNIT3``,
    ``CRVAL3``, ``CDELT3``) are kept verbatim.

    Returns a dict with the cropped cube's dimensions and value range plus
    the output path, ready to be registered as a new session dataset.
    """
    from astropy.io import fits as _fits  # noqa: PLC0415 (lazy)

    dataset = ScientificFitsDataset(src_path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Sub-region export requires a 3D cube dataset.")
        depth, height, width = data.shape
        x0c, x1c = max(0, int(x0)), min(int(x1), width - 1)
        y0c, y1c = max(0, int(y0)), min(int(y1), height - 1)
        z0c, z1c = max(0, int(z0)), min(int(z1), depth - 1)
        if x0c > x1c or y0c > y1c or z0c > z1c:
            raise ValueError(
                f"Invalid sub-region: x=({x0c}..{x1c}), y=({y0c}..{y1c}), "
                f"z=({z0c}..{z1c})."
            )
        sub = np.asarray(data[z0c:z1c + 1, y0c:y1c + 1, x0c:x1c + 1],
                         dtype=np.float32)
        # Header is cloned from the source PRIMARY so we keep BUNIT, OBJECT,
        # HISTORY and every WCS keyword by default. NAXIS / CRPIX get the
        # crop-aware updates below.
        header = hdul[0].header.copy()
    finally:
        hdul.close()

    new_depth, new_height, new_width = sub.shape
    header["NAXIS"]  = 3
    header["NAXIS1"] = int(new_width)
    header["NAXIS2"] = int(new_height)
    header["NAXIS3"] = int(new_depth)
    # CRPIX is FITS 1-indexed; cropping at numpy index x0 means the old pixel
    # x0 becomes the new pixel 0 → CRPIX1' = CRPIX1 - x0 (and analogously for
    # the other two axes). The crop preserves orientation, scale and rotation.
    for axis_num, offset in ((1, x0c), (2, y0c), (3, z0c)):
        key = f"CRPIX{axis_num}"
        if key in header:
            header[key] = float(header[key]) - float(offset)

    # Provenance trail — astropy splits long HISTORY lines automatically.
    header.add_history(
        f"VisIVO sub-cube extraction from {Path(src_path).name}: "
        f"x={x0c}..{x1c} y={y0c}..{y1c} z={z0c}..{z1c} (0-indexed, inclusive)"
    )

    logger.info(
        "[save_subregion] writing FITS  src=%s  out=%s  shape=%s  "
        "crop=(x=%s..%s, y=%s..%s, z=%s..%s)",
        src_path, output_path, sub.shape,
        x0c, x1c, y0c, y1c, z0c, z1c,
    )
    primary_hdu = _fits.PrimaryHDU(data=sub, header=header)
    _fits.HDUList([primary_hdu]).writeto(output_path, overwrite=True)
    logger.info("[save_subregion] FITS written successfully")

    range_min, range_max = _finite_range(sub)
    return {
        "width": int(new_width),
        "height": int(new_height),
        "depth": int(new_depth),
        "range_min": float(range_min),
        "range_max": float(range_max),
        "output_path": output_path,
    }


# ── Worker: moment map ────────────────────────────────────────────────────────


# ── Parallelised CPU moment helpers (used when GPU is unavailable) ────────────


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
            return _as_numpy(res)
        return _chunked_m0(subset_np, valid_mask, spectral_delta_np, n_threads)

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


def worker_isosurface(path: str, width: int, height: int, depth: int, threshold: float) -> dict[str, Any]:
    """
    Compute a VTK isosurface (vtkFlyingEdges3D) from a FITS cube.

    The cube is loaded with memmap=True and materialised fully only in the
    worker process; the main FastAPI process never touches the large array.
    """
    try:
        import vtk  # noqa: PLC0415
        from vtk.util.numpy_support import vtk_to_numpy  # noqa: PLC0415
    except ImportError as exc:
        raise RuntimeError(f"VTK Python bindings are required: {exc}") from exc

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Isosurface endpoint requires a 3D cube.")
        cube = np.ascontiguousarray(data, dtype=np.float32)
    finally:
        hdul.close()

    actual_depth, actual_height, actual_width = cube.shape
    if (width, height, depth) != (actual_width, actual_height, actual_depth):
        logger.info(
            "[isosurface] geometry override path=%s requested=(%s,%s,%s) actual=(%s,%s,%s)",
            dataset.path,
            width,
            height,
            depth,
            actual_width,
            actual_height,
            actual_depth,
        )
        width, height, depth = actual_width, actual_height, actual_depth

    cube_bytes = cube.tobytes()
    image_import = vtk.vtkImageImport()
    image_import.CopyImportVoidPointer(cube_bytes, len(cube_bytes))
    image_import.SetDataScalarTypeToFloat()
    image_import.SetNumberOfScalarComponents(1)
    image_import.SetDataExtent(0, width - 1, 0, height - 1, 0, depth - 1)
    image_import.SetWholeExtent(0, width - 1, 0, height - 1, 0, depth - 1)
    image_import.SetDataSpacing(1.0, 1.0, 1.0)
    image_import.SetDataOrigin(0.0, 0.0, 0.0)
    image_import.Update()

    contour = vtk.vtkFlyingEdges3D()
    contour.SetInputConnection(image_import.GetOutputPort())
    contour.SetValue(0, float(threshold))
    contour.ComputeNormalsOff()
    contour.ComputeGradientsOff()
    contour.Update()

    mesh = contour.GetOutput()
    num_points = int(mesh.GetNumberOfPoints())
    num_polys = int(mesh.GetNumberOfPolys())
    if num_points == 0 or num_polys == 0:
        raise ValueError(f"Empty isosurface mesh for threshold {threshold}.")

    pts = vtk_to_numpy(mesh.GetPoints().GetData()).astype(np.float32).reshape(-1)
    polys = vtk_to_numpy(mesh.GetPolys().GetData()).astype(np.int32)
    pts_compression, pts_b64 = _b64f32_compressed(pts)
    polys_compression, polys_b64 = _b64i32_compressed(polys)

    return {
        "compression": pts_compression,
        "points_base64": pts_b64,
        "polys_base64": polys_b64,
        "num_points": num_points,
        "num_polys": num_polys,
    }


# ── Worker: Position-Velocity diagram ────────────────────────────────────────


def _sample_polyline(vertices: list[tuple[int, int]]) -> tuple[list[tuple[int, int]], float]:
    sampled: list[tuple[int, int]] = []
    total = 0.0
    for seg in range(1, len(vertices)):
        s, e = vertices[seg - 1], vertices[seg]
        dx, dy = e[0] - s[0], e[1] - s[1]
        steps = max(abs(dx), abs(dy))
        if steps <= 0:
            if not sampled or sampled[-1] != s:
                sampled.append(s)
            continue
        for step in range(steps + 1):
            t = float(step) / float(steps)
            pt = (int(round(s[0] + t * dx)), int(round(s[1] + t * dy)))
            if sampled and sampled[-1] == pt:
                continue
            if sampled:
                total += float(np.hypot(pt[0] - sampled[-1][0], pt[1] - sampled[-1][1]))
            sampled.append(pt)
    return sampled, total


def _local_normal(pts: list[tuple[int, int]], i: int) -> tuple[float, float]:
    prev = pts[i if i == 0 else i - 1]
    nxt = pts[i + 1 if i + 1 < len(pts) else i]
    tx, ty = float(nxt[0] - prev[0]), float(nxt[1] - prev[1])
    length = float(np.hypot(tx, ty))
    if length <= 0:
        return 0.0, 1.0
    return -ty / length, tx / length


def worker_pv(path: str, vertices: list[list[int]], width_pixels: int) -> dict[str, Any]:
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("PV extraction requires a 3D cube.")

        cleaned = []
        for v in vertices:
            if len(v) >= 2:
                pt = (int(v[0]), int(v[1]))
                if not cleaned or cleaned[-1] != pt:
                    cleaned.append(pt)
        if len(cleaned) < 2:
            raise ValueError("At least two distinct vertices required for a PV cut.")

        sampled, total_length = _sample_polyline(cleaned)
        depth, height, width = data.shape
        half = 0.5 * float(max(1, width_pixels) - 1)
        x_candidates = [pt[0] for pt in sampled]
        y_candidates = [pt[1] for pt in sampled]
        spatial_margin = max(1, int(np.ceil(abs(half))) + 2)
        slab_x0 = max(0, min(x_candidates) - spatial_margin)
        slab_x1 = min(width - 1, max(x_candidates) + spatial_margin)
        slab_y0 = max(0, min(y_candidates) - spatial_margin)
        slab_y1 = min(height - 1, max(y_candidates) + spatial_margin)

        # Materialise only the spatial slab needed by the PV path. This still reads
        # the full spectral depth, but avoids eager loading the entire cube volume.
        cube = np.asarray(data[:, slab_y0 : slab_y1 + 1, slab_x0 : slab_x1 + 1], dtype=np.float32)
        spectral = dataset.spectral_axis_metadata(0, depth - 1)
        pixel_scale_arcsec = dataset.spatial_pixel_scale_arcsec()
        logger.info(
            "[pv] slab read path=%s full_depth=%s slab_x=%s..%s slab_y=%s..%s width_pixels=%s samples=%s spectral=%s[%s]",
            dataset.path,
            depth,
            slab_x0,
            slab_x1,
            slab_y0,
            slab_y1,
            width_pixels,
            len(sampled),
            spectral["axis_type"],
            spectral["axis_unit"] or "-",
        )
    finally:
        hdul.close()

    sampled_local = [(pt[0] - slab_x0, pt[1] - slab_y0) for pt in sampled]
    x_samples = len(sampled_local)
    positions = np.zeros(x_samples, dtype=np.float32)
    pv = np.full((depth, x_samples), np.nan, dtype=np.float32)
    valid_samples = 0

    for i in range(1, x_samples):
        positions[i] = positions[i - 1] + float(
            np.hypot(
                sampled_local[i][0] - sampled_local[i - 1][0],
                sampled_local[i][1] - sampled_local[i - 1][1],
            )
        )

    slab_height = cube.shape[1]
    slab_width = cube.shape[2]
    for si, center in enumerate(sampled_local):
        nx, ny = _local_normal(sampled_local, si)
        for z in range(depth):
            vals: list[float] = []
            for off in range(max(1, width_pixels)):
                cx = int(round(center[0] + (float(off) - half) * nx))
                cy = int(round(center[1] + (float(off) - half) * ny))
                if 0 <= cx < slab_width and 0 <= cy < slab_height:
                    v_val = float(cube[z, cy, cx])
                    if np.isfinite(v_val):
                        vals.append(v_val)
            if vals:
                pv[z, si] = float(np.mean(vals))
                valid_samples += 1

    if valid_samples <= 0:
        raise ValueError("No valid data found along the PV cut.")

    positions_arcsec = positions * pixel_scale_arcsec
    pos_compression, pos_b64 = _b64f32_compressed(positions)
    pos_arcsec_compression, pos_arcsec_b64 = _b64f32_compressed(positions_arcsec)
    data_compression, data_b64 = _b64f32_compressed(pv.reshape(-1))
    return {
        "num_samples": x_samples,
        "depth": int(pv.shape[0]),
        "scalar_type": "float32",
        "compression": pos_compression,
        "positions_base64": pos_b64,
        "positions_arcsec_base64": pos_arcsec_b64,
        "pixel_scale_arcsec_per_pixel": pixel_scale_arcsec,
        "spatial_unit": "arcsec",
        "data_base64": data_b64,
        "computed_on": "spatial_slab",
        "width_pixels": width_pixels,
        "vertex_count": len(cleaned),
        "total_length": float(total_length),
        "valid_samples": valid_samples,
        "spectral_axis_type": str(spectral["axis_type"]),
        "spectral_axis_unit": str(spectral["axis_unit"]),
        "bunit": dataset.bunit,
        "beam_major": dataset.beam.major,
        "beam_minor": dataset.beam.minor,
        "beam_pa": dataset.beam.pa,
    }


# ── Worker: per-channel noise (MAD → σ) ──────────────────────────────────────


def worker_noise_estimate(
    path: str,
    x0: int, x1: int,
    y0: int, y1: int,
    channel_start: int,
    channel_end: int,
) -> dict[str, Any]:
    """
    Estimate per-channel noise in a user-specified emission-free spatial region.

    Returns the per-channel median absolute deviation (MAD) and the derived
    Gaussian sigma (MAD × 1.4826).  The spatial region [x0:x1, y0:y1] should
    be an area of the cube that contains only noise (no astronomical emission).
    """
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Noise estimation requires a 3D cube.")
        depth = data.shape[0]
        height = data.shape[1]
        width = data.shape[2]
        x0c, x1c = max(0, x0), min(x1, width - 1)
        y0c, y1c = max(0, y0), min(y1, height - 1)
        z0 = max(0, min(channel_start, depth - 1))
        z1 = max(0, min(channel_end,   depth - 1))
        if x0c > x1c or y0c > y1c or z0 > z1:
            raise ValueError("Invalid spatial or spectral region for noise estimation.")
        # Materialise only the requested region.
        region = np.asarray(data[z0 : z1 + 1, y0c : y1c + 1, x0c : x1c + 1], dtype=np.float32)
    finally:
        hdul.close()

    mad_values: list[float] = []
    sigma_values: list[float] = []
    total_finite = 0
    total_zero   = 0
    for z in range(region.shape[0]):
        plane = region[z].ravel()
        finite = plane[np.isfinite(plane)]
        total_finite += int(finite.size)
        if finite.size:
            total_zero += int(np.count_nonzero(finite == 0))
        if finite.size < 2:
            mad_values.append(float("nan"))
            sigma_values.append(float("nan"))
            continue
        # Zero-as-blank heuristic: a lot of radio/IR cubes use exactly 0.0 for
        # off-source pixels instead of NaN. If you compute MAD over a region
        # that mostly overlaps that blanked area, the median of the finite
        # pixels is 0 and |finite - 0| also has median 0 — you get a flat
        # zero noise that hides the actual data noise. When more than half
        # the finite pixels in this channel are exactly 0, treat them as
        # blanks and use only the non-zero pixels for the MAD. We keep zeros
        # in when they are a minority because some cubes (post-baseline
        # subtraction, calibrated brightness temperatures) legitimately have
        # noise centred on zero.
        nonzero = finite[finite != 0]
        if nonzero.size >= 2 and nonzero.size < finite.size / 2:
            sample = nonzero
        else:
            sample = finite
        median = float(np.median(sample))
        mad = float(np.median(np.abs(sample - median)))
        mad_values.append(mad)
        sigma_values.append(mad * 1.4826)  # MAD → σ for Gaussian noise

    # Surface what fraction of finite pixels in the whole selection are
    # exactly zero — the dialog uses this to warn the user when the result
    # is suspicious (e.g. "80 % of the region is blanked; pick a tighter
    # box inside the data footprint").
    blank_fraction = float(total_zero / total_finite) if total_finite else 0.0

    return {
        "channel_start": z0,
        "channel_end": z1,
        "num_channels": z1 - z0 + 1,
        "mad": mad_values,
        "sigma": sigma_values,
        "region": {"x0": x0c, "x1": x1c, "y0": y0c, "y1": y1c},
        "blank_fraction": blank_fraction,
    }


# ── Worker: image full / preview ──────────────────────────────────────────────


def _build_preview(image: np.ndarray, max_side: int) -> tuple[np.ndarray, float]:
    h, w = image.shape
    longest = max(w, h)
    limit = max(1, int(max_side))
    if longest <= limit:
        return np.ascontiguousarray(image, dtype=np.float32), 1.0
    scale = float(limit) / float(longest)
    pw = max(1, int(round(w * scale)))
    ph = max(1, int(round(h * scale)))
    xi = np.clip(
        np.floor((np.arange(pw, dtype=np.float64) + 0.5) * (w / pw)).astype(np.int64), 0, w - 1
    )
    yi = np.clip(
        np.floor((np.arange(ph, dtype=np.float64) + 0.5) * (h / ph)).astype(np.int64), 0, h - 1
    )
    preview = np.ascontiguousarray(image[np.ix_(yi, xi)], dtype=np.float32)
    return preview, float(longest) / float(max(preview.shape[0], preview.shape[1]))


def worker_image_full(path: str) -> dict[str, Any]:
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 2:
            raise ValueError("Image endpoint requires a 2D FITS dataset.")
        image = np.asarray(data, dtype=np.float32)
    finally:
        hdul.close()

    range_min, range_max = _finite_range(image)
    compression, data_b64 = _b64f32_compressed(image)
    return {
        "width": int(image.shape[1]),
        "height": int(image.shape[0]),
        "full_width": int(image.shape[1]),
        "full_height": int(image.shape[0]),
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "is_preview": False,
        "preview_scale_factor": 1.0,
        "compression": compression,
        "data_base64": data_b64,
    }


def worker_image_preview(path: str, max_longest_side: int) -> dict[str, Any]:
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 2:
            raise ValueError("Image preview endpoint requires a 2D FITS dataset.")
        full_h, full_w = int(data.shape[0]), int(data.shape[1])
        image = np.asarray(data, dtype=np.float32)
    finally:
        hdul.close()

    preview, scale = _build_preview(image, max_longest_side)
    is_preview = preview.shape != image.shape
    range_min, range_max = _finite_range(preview)
    compression, data_b64 = _b64f32_compressed(preview)
    return {
        "width": int(preview.shape[1]),
        "height": int(preview.shape[0]),
        "full_width": full_w,
        "full_height": full_h,
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "is_preview": is_preview,
        "preview_scale_factor": scale,
        "compression": compression,
        "data_base64": data_b64,
    }


def worker_cube_preview(path: str, downsample: int) -> dict[str, Any]:
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Cube preview requires a 3D cube.")
        stride = max(1, int(downsample))
        preview = np.asarray(data[::stride, ::stride, ::stride], dtype=np.float32)
    finally:
        hdul.close()

    range_min, range_max = _finite_range(preview)
    return {
        "width": int(preview.shape[2]),
        "height": int(preview.shape[1]),
        "depth": int(preview.shape[0]),
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "data_base64": _b64f32(preview),
    }


# ── S-02: Spectral line-width maps ────────────────────────────────────────────


def _gaussian(x: np.ndarray, amplitude: float, mean: float, sigma: float) -> np.ndarray:
    """1-D Gaussian for scipy curve_fit."""
    return amplitude * np.exp(-0.5 * ((x - mean) / sigma) ** 2)


def worker_linewidth(
    path: str,
    channel_start: int,
    channel_end: int,
    mask_enabled: bool,
    threshold_value: float,
    snr_threshold: float = 3.0,
    iy_start: int | None = None,
    iy_end: int | None = None,
) -> dict[str, Any]:
    """Compute per-pixel FWHM (Gaussian fit) and equivalent-width maps.

    FWHM  = 2.3548 × σ_gaussian  (spectral units)
    EW    = M0 / peak_brightness  (spectral units)

    Both maps are returned as float32 arrays encoded in base64.

    Optimisations:
    - ``snr_threshold``: pixels with peak/MAD-noise below this value are
      skipped (no Gauss fit attempted). Default 3.0 — typical "noise pixel"
      detection threshold in radio astronomy. Set to 0 to disable.
    - ``iy_start``/``iy_end``: process only rows in ``[iy_start, iy_end)``;
      used by the parallel orchestrator to spread work across the worker
      pool. When both are None the entire image is processed. The returned
      maps are still full-image shaped (ny × nx) with NaN outside the chunk
      so chunks can be combined by simple nan-aware merge.
    """
    from scipy.optimize import curve_fit  # noqa: PLC0415 (lazy – not in every worker)

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Line-width maps require a 3D spectral cube.")
        nz, ny, nx = data.shape
        z0 = max(0, min(int(channel_start), nz - 1))
        z1 = max(0, min(int(channel_end), nz - 1))
        if z0 > z1:
            raise ValueError("Invalid channel range.")
        spectral = dataset.spectral_axis_metadata(z0, z1)
        bunit = dataset.bunit
        # Subset only the rows we need to keep memory pressure down when the
        # chunk is small. Spectral axis is always loaded in full.
        row_lo = 0 if iy_start is None else max(0, min(int(iy_start), ny))
        row_hi = ny if iy_end is None else max(row_lo, min(int(iy_end), ny))
        subset = np.asarray(data[z0 : z1 + 1, row_lo:row_hi, :], dtype=np.float64)
    finally:
        hdul.close()

    spectral_values = np.asarray(spectral["coordinates"], dtype=np.float64)
    spectral_delta = _integrated_spacing(spectral_values)
    weights = spectral_delta[:, None, None]

    valid_mask = np.isfinite(subset)
    if mask_enabled:
        valid_mask = valid_mask & (subset >= float(threshold_value))
    safe = np.where(valid_mask, subset, np.nan)

    # Pre-compute M0 (integrated flux) and peak per pixel (vectorised)
    m0_chunk = np.nansum(safe * weights, axis=0)
    peak_chunk = np.nanmax(safe, axis=0)

    # Per-pixel noise estimate via MAD across the spectral axis. Used to skip
    # background pixels (no real line) before paying for a Gauss fit. Vectorised
    # over the chunk so this is essentially free vs. the per-pixel fit cost.
    med_chunk = np.nanmedian(safe, axis=0)
    mad_chunk = 1.4826 * np.nanmedian(np.abs(safe - med_chunk[None, :, :]), axis=0)
    # SNR=peak/noise. Where noise is 0 or non-finite, SNR is set to 0 → skipped.
    with np.errstate(divide="ignore", invalid="ignore"):
        snr_chunk = np.where(np.isfinite(mad_chunk) & (mad_chunk > 0),
                             peak_chunk / mad_chunk, 0.0)

    # Allocate FULL-image arrays so chunks can be merged with simple combine.
    fwhm_map = np.full((ny, nx), np.nan, dtype=np.float32)
    ew_map = np.full((ny, nx), np.nan, dtype=np.float32)

    cy = row_hi - row_lo                        # chunk height
    snr_cutoff = max(0.0, float(snr_threshold))
    for iy_local in range(cy):
        iy = row_lo + iy_local
        for ix in range(nx):
            if snr_chunk[iy_local, ix] < snr_cutoff:
                # Still record EW (cheap, vectorised values) even when the
                # Gauss fit is skipped: it does not require a fit.
                pk = float(peak_chunk[iy_local, ix])
                m0v = float(m0_chunk[iy_local, ix])
                if np.isfinite(pk) and pk > 0.0 and np.isfinite(m0v):
                    ew_map[iy, ix] = m0v / pk
                continue
            spectrum = safe[:, iy_local, ix]
            finite_mask = np.isfinite(spectrum)
            if finite_mask.sum() < 4:
                continue
            sv = spectral_values[finite_mask]
            sp = spectrum[finite_mask]
            amp0 = float(np.nanmax(sp))
            if amp0 <= 0.0:
                continue
            mean0 = float(np.average(sv, weights=np.abs(sp)))
            sigma0 = float(np.sqrt(np.average((sv - mean0) ** 2, weights=np.abs(sp))))
            if sigma0 == 0.0:
                sigma0 = (sv[-1] - sv[0]) / 4.0
            try:
                popt, _ = curve_fit(
                    _gaussian,
                    sv,
                    sp,
                    p0=[amp0, mean0, sigma0],
                    maxfev=1000,
                    bounds=([0, sv.min(), 0], [np.inf, sv.max(), np.inf]),
                )
                fwhm_map[iy, ix] = float(2.3548 * abs(popt[2]))
            except Exception:
                pass
            pk = float(peak_chunk[iy_local, ix])
            m0v = float(m0_chunk[iy_local, ix])
            if np.isfinite(pk) and pk > 0.0 and np.isfinite(m0v):
                ew_map[iy, ix] = m0v / pk

    fwhm_range = _finite_range(fwhm_map)
    ew_range = _finite_range(ew_map)
    spectral_unit = str(spectral["axis_unit"])
    return {
        "width": nx,
        "height": ny,
        "scalar_type": "float32",
        "fwhm_unit": spectral_unit,
        "ew_unit": spectral_unit,
        "bunit": bunit,
        "fwhm_range_min": fwhm_range[0],
        "fwhm_range_max": fwhm_range[1],
        "ew_range_min": ew_range[0],
        "ew_range_max": ew_range[1],
        "fwhm_base64": _b64f32(fwhm_map),
        "ew_base64": _b64f32(ew_map),
    }


# ── S-03: Polynomial baseline subtraction ─────────────────────────────────────


def _expand_channel_ranges(
    line_free_channels: list[int] | list[list[int]] | None,
    nchans: int,
) -> np.ndarray:
    """Expand line-free channel specification to a flat index array.

    Accepts either:
    - A flat list of channel indices: [5, 6, 7, 20, 21]
    - A list of [start, end] pairs:   [[5, 7], [20, 21]]

    Raises ValueError if ``line_free_channels`` is None or empty, and if the
    resulting index array is empty after bounds-filtering.  The fallback to
    all channels has been removed — fitting on all channels silently absorbs
    real spectral-line signal, which is scientifically incorrect.
    """
    if not line_free_channels:
        raise ValueError(
            "line_free_channels is required for baseline subtraction. "
            "Provide at least one line-free spectral range so the fit does not "
            "absorb real emission-line signal."
        )
    # Detect format: if first element is a list/tuple use range expansion
    indices: list[int] = []
    for item in line_free_channels:
        if isinstance(item, (list, tuple)):
            start, end = int(item[0]), int(item[1])
            if start > end:
                raise ValueError(
                    f"Invalid line-free range [{start}, {end}]: start must be ≤ end."
                )
            indices.extend(range(start, end + 1))
        else:
            indices.append(int(item))
    fit_idx = np.array([i for i in indices if 0 <= i < nchans], dtype=int)
    if len(fit_idx) == 0:
        raise ValueError(
            f"No valid line-free channels after bounds-checking against cube depth {nchans}. "
            "All provided indices were out of range [0, nchans-1]."
        )
    return fit_idx


def worker_baseline(
    path: str,
    output_path: str,
    channel_start: int,
    channel_end: int,
    poly_order: int,
    line_free_channels: list[int] | list[list[int]] | None,
    method: str = "polynomial",
) -> dict[str, Any]:
    """Fit and subtract a per-pixel baseline from a spectral cube.

    ``method`` is ``"polynomial"`` (default) or ``"median"``.
    ``line_free_channels`` accepts a flat list of indices OR a list of
    [start, end] pairs.

    Returns metadata including rms_before and rms_after.
    """
    from astropy.io import fits as _fits  # noqa: PLC0415

    method = (method or "polynomial").lower()
    if method not in {"polynomial", "median"}:
        raise ValueError(f"Unknown baseline method '{method}'. Choose polynomial or median.")

    # ── Validate line-free channels upfront ──────────────────────────────────────
    # This check is intentionally done before any FITS I/O so the error is fast
    # and unambiguous.  Fitting on all channels silently absorbs real line signal.
    if not line_free_channels:
        raise ValueError(
            "Line-free channels are required for baseline subtraction. "
            "Specify at least one spectral range that contains no emission or "
            "absorption signal so the baseline fit is not contaminated."
        )

    logger.info(
        "[BASELINE] start  path=%s  channel_start=%s  channel_end=%s  "
        "method=%s  poly_order=%s  line_free_channels=%s",
        path, channel_start, channel_end, method, poly_order, line_free_channels,
    )

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        logger.info("[BASELINE] raw FITS shape (memmap): %s  dtype=%s", raw.shape, raw.dtype)
        data = dataset.squeeze_to_3d(raw)
        logger.info("[BASELINE] after squeeze_to_3d: shape=%s  ndim=%s", data.shape, data.ndim)
        if data.ndim != 3:
            raise ValueError("Baseline subtraction requires a 3D spectral cube.")
        nz, ny, nx = data.shape
        logger.info("[BASELINE] cube dims  nz=%s  ny=%s  nx=%s", nz, ny, nx)

        # Resolve channel range.
        # channel_end == 0 is the sentinel meaning "through the last channel"
        # (this is how the Qt client signals "use the full spectral axis").
        z0 = max(0, int(channel_start))
        z1 = int(channel_end) if int(channel_end) > 0 else nz - 1
        z0 = min(z0, nz - 1)
        z1 = min(z1, nz - 1)
        logger.info("[BASELINE] resolved channel range  z0=%s  z1=%s  (nchans=%s)", z0, z1, z1 - z0 + 1)
        if z0 > z1:
            raise ValueError(
                f"Invalid channel range: start={z0} > end={z1} (cube depth={nz})."
            )

        subset = np.asarray(data[z0 : z1 + 1, :, :], dtype=np.float32)
        logger.info("[BASELINE] subset shape after slice: %s", subset.shape)
        header = hdul[0].header.copy()
    finally:
        hdul.close()

    nchans = subset.shape[0]
    # Hard invariant: the extracted subset must span the requested depth.
    if subset.shape != (nchans, ny, nx):
        raise RuntimeError(
            f"[BASELINE] Unexpected subset shape {subset.shape}; expected ({nchans}, {ny}, {nx})."
        )

    # Expand line-free ranges to a sorted index array and validate.
    # _expand_channel_ranges raises ValueError if the result is empty.
    fit_idx = _expand_channel_ranges(line_free_channels, nchans)

    # For polynomial fitting, require enough channels to constrain the fit.
    # A degree-d polynomial needs at least d+2 points to be over-determined.
    if method == "polynomial":
        deg = max(0, min(int(poly_order), 10))
        min_required = deg + 2
        if len(fit_idx) < min_required:
            raise ValueError(
                f"Not enough line-free channels for a degree-{deg} polynomial fit. "
                f"Need at least {min_required} points, but only {len(fit_idx)} "
                f"line-free channel(s) were selected. "
                "Either lower the polynomial order or widen the line-free ranges."
            )
    else:
        deg = 0  # unused for median but kept for symmetry

    logger.info(
        "[BASELINE] fit_idx: %d of %d channels selected  first=%s  last=%s",
        len(fit_idx), nchans,
        int(fit_idx[0]) if len(fit_idx) else "n/a",
        int(fit_idx[-1]) if len(fit_idx) else "n/a",
    )

    all_chan_idx = np.arange(nchans)
    flat = subset.reshape(nchans, ny * nx).astype(np.float64)  # (nchans, npix)

    # RMS before subtraction (finite values only)
    finite_before = np.isfinite(flat)
    rms_before = float(np.sqrt(np.mean(flat[finite_before] ** 2))) if finite_before.any() else float("nan")

    if method == "median":
        # Median of line-free channels per pixel → constant baseline
        baseline_1d = np.nanmedian(flat[fit_idx, :], axis=0)  # (npix,)
        baseline = np.broadcast_to(baseline_1d, (nchans, ny * nx)).copy()
    else:
        # Polynomial least-squares fit constrained to line-free channels only.
        # The fitted model is then evaluated on ALL channels for subtraction.
        V_fit = np.vander(fit_idx.astype(float), N=deg + 1, increasing=True)
        V_all = np.vander(all_chan_idx.astype(float), N=deg + 1, increasing=True)
        flat_fit = flat[fit_idx, :]
        coeffs, _residuals, _rank, _sv = np.linalg.lstsq(V_fit, flat_fit, rcond=None)
        baseline = V_all @ coeffs   # (nchans, npix)

    subtracted = (flat - baseline).astype(np.float32).reshape(nchans, ny, nx)
    logger.info("[BASELINE] subtracted shape: %s", subtracted.shape)

    # Hard invariants before FITS write — any shape mismatch raises immediately.
    if subtracted.ndim != 3:
        raise RuntimeError(
            f"[BASELINE] subtracted cube is not 3D: ndim={subtracted.ndim}  shape={subtracted.shape}"
        )
    if subtracted.shape != (nchans, ny, nx):
        raise RuntimeError(
            f"[BASELINE] shape mismatch after subtraction: {subtracted.shape} != ({nchans}, {ny}, {nx})."
        )

    # RMS after subtraction — finite values only, guard against degenerate output.
    finite_after = np.isfinite(subtracted)
    logger.info(
        "[BASELINE] corrected finite voxels: %d / %d",
        int(finite_after.sum()), int(finite_after.size),
    )
    if not finite_after.any():
        raise RuntimeError(
            "Baseline subtraction produced no finite voxels. "
            "The fit may be numerically ill-conditioned. "
            "Try a lower polynomial order or different line-free ranges."
        )
    rms_after = float(np.sqrt(np.mean(subtracted[finite_after] ** 2)))
    logger.info(
        "[BASELINE] corrected range: min=%s  max=%s  rms_after=%s",
        float(np.min(subtracted[finite_after])),
        float(np.max(subtracted[finite_after])),
        rms_after,
    )

    # Write output FITS, preserving spatial and spectral WCS from the input.
    # Only NAXIS3 and CRPIX3 are updated to reflect the channel sub-range;
    # all other spectral keywords (CTYPE3, CUNIT3, CRVAL3, CDELT3) are kept
    # so that the output cube is correctly located in velocity/frequency space.
    header["NAXIS3"] = nchans
    header["CRPIX3"] = float(header.get("CRPIX3", 1.0)) - z0
    logger.info(
        "[BASELINE] writing FITS  output_path=%s  data.shape=%s  NAXIS3=%s",
        output_path, subtracted.shape, nchans,
    )
    primary_hdu = _fits.PrimaryHDU(data=subtracted, header=header)
    _fits.HDUList([primary_hdu]).writeto(output_path, overwrite=True)
    logger.info("[BASELINE] FITS written successfully")

    range_min, range_max = _finite_range(subtracted)
    return {
        "width": nx,
        "height": ny,
        "depth": nchans,
        "scalar_type": "float32",
        "range_min": float(range_min),
        "range_max": float(range_max),
        "poly_order": int(poly_order) if method == "polynomial" else 0,
        "method": method,
        "fit_channels": len(fit_idx),
        "rms_before": rms_before,
        "rms_after": rms_after,
        "output_path": output_path,
    }


# ── S-04: Spectral stacking ────────────────────────────────────────────────────


def worker_stack(
    paths: list[str],
    method: str,
    weights: list[float] | None,
    weight_by: str = "uniform",
) -> dict[str, Any]:
    """Stack N spectral cubes (same spatial footprint) along the spectral axis.

    ``method``:    "mean" | "median" | "weighted_mean"
    ``weight_by``: "uniform" | "rms" | "peak" — auto-computes per-cube weights
                   when method="weighted_mean" and no explicit weights given.

    Returns rows=1, cols=n_chan binary-compatible result.
    """
    if not paths:
        raise ValueError("No input paths supplied for stacking.")

    method = (method or "mean").lower()
    weight_by = (weight_by or "uniform").lower()
    valid_methods = {"mean", "median", "weighted_mean"}
    if method not in valid_methods:
        raise ValueError(f"Unknown stacking method '{method}'. Choose from {valid_methods}.")

    spectra: list[np.ndarray] = []
    spatial_shapes: list[tuple] = []
    for p in paths:
        ds = ScientificFitsDataset(p)
        hdul, raw = ds.open_memmap()
        try:
            cube = ds.squeeze_to_3d(raw)
            if cube.ndim != 3:
                raise ValueError(f"Stack input '{p}' is not a 3-D cube.")
            arr = np.asarray(cube, dtype=np.float64)
        finally:
            hdul.close()
        spatial_shapes.append((arr.shape[1], arr.shape[2]))
        # Collapse spatial dimensions → mean spectrum (nchans,)
        spec = np.nanmean(arr.reshape(arr.shape[0], -1), axis=1)
        spectra.append(spec)

    # Validate all cubes share the same spatial shape (422 on mismatch)
    if len(set(spatial_shapes)) > 1:
        raise ValueError(
            f"All input cubes must share the same spatial shape. "
            f"Got: {spatial_shapes}."
        )

    # Verify all spectra have the same length
    n_chan = spectra[0].shape[0]
    if any(s.shape[0] != n_chan for s in spectra):
        raise ValueError("All input cubes must have the same number of spectral channels.")

    stack = np.stack(spectra, axis=0)  # (N, n_chan)

    if method == "median":
        result_1d = np.nanmedian(stack, axis=0)
    elif method == "weighted_mean":
        # Use explicit weights if provided; otherwise auto-compute from weight_by strategy
        if weights and len(weights) == len(paths):
            w = np.array(weights, dtype=np.float64)
        elif weight_by == "rms":
            # Weight ∝ 1/RMS² (higher-SNR cubes get more weight)
            rms = np.array([float(np.sqrt(np.nanmean(s ** 2))) for s in spectra])
            rms = np.where(rms > 0, rms, 1.0)
            w = 1.0 / rms ** 2
        elif weight_by == "peak":
            # Weight ∝ peak brightness
            peak = np.array([float(np.nanmax(np.abs(s))) for s in spectra])
            peak = np.where(peak > 0, peak, 1.0)
            w = peak
        else:  # "uniform"
            w = np.ones(len(paths), dtype=np.float64)
        w = w / w.sum()
        result_1d = np.nansum(stack * w[:, None], axis=0)
    else:  # "mean"
        result_1d = np.nanmean(stack, axis=0)

    result_f32 = result_1d.astype(np.float32)
    range_min, range_max = _finite_range(result_f32.reshape(1, -1))
    return {
        "rows": 1,
        "cols": int(n_chan),
        "scalar_type": "float32",
        "method": method,
        "n_cubes": len(paths),
        "range_min": float(range_min),
        "range_max": float(range_max),
        "data_base64": _b64f32(result_f32.reshape(1, n_chan)),
    }


# ── A-01: Aperture photometry ─────────────────────────────────────────────────


def worker_aperture_photometry(
    path: str,
    channel: int | None,
    ra_deg: float,
    dec_deg: float,
    aperture_arcsec: float,
    annulus_inner_arcsec: float,
    annulus_outer_arcsec: float,
) -> dict[str, Any]:
    """Aperture photometry at a sky position using photutils.

    Converts sky coords → pixel via astropy WCS, then sums flux inside
    a circular aperture and subtracts the median background estimated from
    a circular annulus.

    Returns flux, uncertainty, background level, and pixel count.
    """
    try:
        from photutils.aperture import (  # noqa: PLC0415
            CircularAnnulus,
            CircularAperture,
            ApertureStats,
        )
    except ImportError as exc:
        raise ImportError(
            "photutils is required for aperture photometry. "
            "Install with: pip install photutils"
        ) from exc

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        if raw.ndim == 2:
            image = np.asarray(raw, dtype=np.float64)
        elif raw.ndim == 3:
            ch = 0 if channel is None else int(channel)
            ch = max(0, min(ch, raw.shape[0] - 1))
            image = np.asarray(raw[ch], dtype=np.float64)
        elif raw.ndim == 4:
            ch = 0 if channel is None else int(channel)
            ch = max(0, min(ch, raw.shape[1] - 1))
            image = np.asarray(raw[0, ch], dtype=np.float64)
        else:
            raise ValueError(f"Unsupported FITS dimensionality: {raw.ndim}")
    finally:
        hdul.close()

    if dataset.wcs is None:
        raise ValueError("WCS not available for this dataset — cannot convert sky → pixel.")

    pixel_scale_arcsec = dataset.spatial_pixel_scale_arcsec()
    if pixel_scale_arcsec <= 0:
        raise ValueError("Pixel scale is zero or negative.")

    # Convert sky → pixel (0-indexed, row=y, col=x)
    celestial = dataset.wcs.celestial
    if celestial is None:
        raise ValueError("No celestial WCS axes found in this dataset.")
    pix = celestial.all_world2pix([[ra_deg, dec_deg]], 0)[0]
    pix_x, pix_y = float(pix[0]), float(pix[1])

    r_ap   = aperture_arcsec   / pixel_scale_arcsec
    r_ann1 = annulus_inner_arcsec / pixel_scale_arcsec
    r_ann2 = annulus_outer_arcsec / pixel_scale_arcsec

    position = (pix_x, pix_y)
    aperture = CircularAperture(position, r=r_ap)
    annulus  = CircularAnnulus(position, r_in=r_ann1, r_out=r_ann2)

    # Background: median inside annulus
    ann_stats = ApertureStats(image, annulus, sigma_clip=None)
    bkg_per_pix = float(ann_stats.median) if np.isfinite(ann_stats.median) else 0.0

    # Aperture sum
    ap_stats = ApertureStats(image, aperture, sigma_clip=None)
    n_pix = int(ap_stats.sum_aper_area.value) if hasattr(ap_stats.sum_aper_area, "value") else int(np.pi * r_ap ** 2)
    raw_sum = float(ap_stats.sum) if np.isfinite(ap_stats.sum) else float(np.nansum(image))
    flux = raw_sum - bkg_per_pix * n_pix

    # Simple Poisson-like uncertainty: sqrt(|sum| + n_pix * |bkg|)
    flux_err = float(np.sqrt(abs(raw_sum) + n_pix * abs(bkg_per_pix)))

    return {
        "flux_jy": flux,
        "flux_err_jy": flux_err,
        "background_jy_per_pix": bkg_per_pix,
        "n_pix_aperture": n_pix,
        "channel": channel,
        "pix_x": pix_x,
        "pix_y": pix_y,
        "pixel_scale_arcsec": pixel_scale_arcsec,
    }


# ── A-02: 2-D Gaussian source fitting + beam deconvolution ───────────────────


def _gaussian_2d(
    xy: tuple[np.ndarray, np.ndarray],
    amplitude: float, x0: float, y0: float,
    sigma_x: float, sigma_y: float, theta: float, offset: float,
) -> np.ndarray:
    """Elliptical 2-D Gaussian for scipy curve_fit (ravelled output).

    Standard form: A * exp(-[ a(x-x0)² + 2b(x-x0)(y-y0) + c(y-y0)² ]) + bg
    where a = cos²θ/(2σx²) + sin²θ/(2σy²), etc.
    FWHM_x = 2.3548 × σx, FWHM_y = 2.3548 × σy.
    """
    x, y = xy
    cos_t = np.cos(theta)
    sin_t = np.sin(theta)
    sx2, sy2 = sigma_x ** 2, sigma_y ** 2
    a = cos_t ** 2 / (2 * sx2) + sin_t ** 2 / (2 * sy2)
    b = -np.sin(2 * theta) / 4 * (1 / sx2 - 1 / sy2)
    c = sin_t ** 2 / (2 * sx2) + cos_t ** 2 / (2 * sy2)
    dx = x - x0
    dy = y - y0
    return (amplitude * np.exp(-(a * dx * dx + 2 * b * dx * dy + c * dy * dy)) + offset).ravel()


def _deconvolve_gaussian_beam(
    major_pix: float, minor_pix: float, pa_rad: float,
    beam_major_pix: float, beam_minor_pix: float, beam_pa_rad: float,
) -> tuple[float | None, float | None, float | None]:
    """Deconvolve a beam from a fitted Gaussian (all sizes in pixels).

    Returns (deconv_major, deconv_minor, deconv_pa_rad) or (None, None, None)
    if the source is unresolved.
    """
    # Convert to Gaussian standard deviation = FWHM / (2*sqrt(2*ln2))
    f2s = 1.0 / (2.0 * np.sqrt(2.0 * np.log(2.0)))
    s_src_maj = major_pix * f2s
    s_src_min = minor_pix * f2s
    s_bm_maj  = beam_major_pix * f2s
    s_bm_min  = beam_minor_pix * f2s

    # Quadratic deconvolution (approximate, Condon 1997)
    d_maj2 = s_src_maj ** 2 - s_bm_maj ** 2
    d_min2 = s_src_min ** 2 - s_bm_min ** 2

    if d_maj2 < 0 or d_min2 < 0:
        return None, None, None   # unresolved

    deconv_maj_pix = np.sqrt(d_maj2) / f2s
    deconv_min_pix = np.sqrt(d_min2) / f2s
    deconv_pa_rad  = pa_rad   # simplified: keep fitted PA
    return float(deconv_maj_pix), float(deconv_min_pix), float(deconv_pa_rad)


def worker_gauss_fit_2d(
    path: str,
    channel: int | None,
    ra_deg: float,
    dec_deg: float,
    box_arcsec: float,
) -> dict[str, Any]:
    """Fit a 2-D elliptical Gaussian + background on a sub-image centred at (ra_deg, dec_deg).

    If beam keywords (BMAJ/BMIN/BPA) are present in the header the fitted source
    is deconvolved from the beam; otherwise the deconvolved fields are None.

    Returns peak flux, integrated flux, major/minor FWHM and PA (all in arcsec/degrees),
    plus deconvolved sizes and fit RMS.
    """
    from scipy.optimize import curve_fit  # noqa: PLC0415

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        if raw.ndim == 2:
            image = np.asarray(raw, dtype=np.float64)
        elif raw.ndim == 3:
            ch = 0 if channel is None else int(channel)
            ch = max(0, min(ch, raw.shape[0] - 1))
            image = np.asarray(raw[ch], dtype=np.float64)
        elif raw.ndim == 4:
            ch = 0 if channel is None else int(channel)
            ch = max(0, min(ch, raw.shape[1] - 1))
            image = np.asarray(raw[0, ch], dtype=np.float64)
        else:
            raise ValueError(f"Unsupported FITS dimensionality: {raw.ndim}")
    finally:
        hdul.close()

    if dataset.wcs is None:
        raise ValueError("WCS not available for this dataset.")
    celestial = dataset.wcs.celestial
    if celestial is None:
        raise ValueError("No celestial WCS axes in this dataset.")

    pixel_scale = dataset.spatial_pixel_scale_arcsec()
    if pixel_scale <= 0:
        raise ValueError("Pixel scale is zero or negative.")

    pix = celestial.all_world2pix([[ra_deg, dec_deg]], 0)[0]
    cx, cy = float(pix[0]), float(pix[1])

    half_box = int(np.ceil(box_arcsec / (2.0 * pixel_scale)))
    half_box = max(half_box, 5)

    ny, nx = image.shape
    x0 = max(0, int(cx) - half_box)
    x1 = min(nx, int(cx) + half_box + 1)
    y0 = max(0, int(cy) - half_box)
    y1 = min(ny, int(cy) + half_box + 1)

    sub = image[y0:y1, x0:x1]
    if sub.size < 9:
        raise ValueError("Box too small or position outside image.")

    yy, xx = np.mgrid[y0:y1, x0:x1]
    # Replace NaN with local median for fitting
    sub_fit = np.where(np.isfinite(sub), sub, float(np.nanmedian(sub)))

    amp0    = float(np.nanmax(sub_fit) - np.nanmin(sub_fit))
    offset0 = float(np.nanmin(sub_fit))
    # Intensity-weighted spread as initial sigma (more robust than box/2)
    sub_pos = sub_fit - offset0
    norm = np.nansum(sub_pos)
    if norm > 0:
        mu_x = float(np.nansum(xx * sub_pos) / norm)
        mu_y = float(np.nansum(yy * sub_pos) / norm)
        var  = float(np.nansum(((xx - mu_x) ** 2 + (yy - mu_y) ** 2) * sub_pos) / norm)
        sigma0 = max(float(np.sqrt(var / 2.0)), 1.0)
    else:
        sigma0 = half_box / 4.0

    try:
        popt, pcov = curve_fit(
            _gaussian_2d,
            (xx.ravel(), yy.ravel()),
            sub_fit.ravel(),
            p0=[amp0, cx, cy, sigma0, sigma0, 0.0, offset0],
            maxfev=5000,
        )
    except Exception as exc:
        raise RuntimeError(f"2-D Gaussian fit failed: {exc}") from exc

    amp_fit, x0_fit, y0_fit, sx_fit, sy_fit, theta_fit, bg_fit = popt
    # sigma → FWHM
    fwhm_x_pix = abs(sx_fit) * 2.3548
    fwhm_y_pix = abs(sy_fit) * 2.3548
    # Sort major/minor
    if fwhm_x_pix >= fwhm_y_pix:
        major_pix, minor_pix, pa_rad = fwhm_x_pix, fwhm_y_pix, float(theta_fit)
    else:
        major_pix, minor_pix, pa_rad = fwhm_y_pix, fwhm_x_pix, float(theta_fit) + np.pi / 2

    major_arcsec = major_pix * pixel_scale
    minor_arcsec = minor_pix * pixel_scale
    pa_deg = float(np.degrees(pa_rad) % 180.0)

    # Integrated flux = amplitude × 2π × σx × σy  (Gaussian integral)
    integrated_flux = float(amp_fit * 2.0 * np.pi * abs(sx_fit) * abs(sy_fit))

    # Fit residual RMS
    residuals = sub_fit.ravel() - _gaussian_2d(
        (xx.ravel(), yy.ravel()), *popt)
    fit_rms = float(np.sqrt(np.mean(residuals ** 2)))

    # Beam deconvolution (optional)
    deconv_major: float | None = None
    deconv_minor: float | None = None
    if dataset.beam.major is not None and dataset.beam.minor is not None:
        bm_maj_pix = (dataset.beam.major * 3600.0) / pixel_scale   # deg → arcsec → pix
        bm_min_pix = (dataset.beam.minor * 3600.0) / pixel_scale
        bm_pa_rad  = float(np.radians(dataset.beam.pa or 0.0))
        dm, dn, _ = _deconvolve_gaussian_beam(
            major_pix, minor_pix, pa_rad,
            bm_maj_pix, bm_min_pix, bm_pa_rad,
        )
        if dm is not None:
            deconv_major = dm * pixel_scale
            deconv_minor = dn * pixel_scale

    return {
        "peak_jy_beam": float(amp_fit),
        "integrated_flux_jy": integrated_flux,
        "major_arcsec": major_arcsec,
        "minor_arcsec": minor_arcsec,
        "pa_deg": pa_deg,
        "deconvolved_major_arcsec": deconv_major,
        "deconvolved_minor_arcsec": deconv_minor,
        "fit_rms": fit_rms,
        "center_pix_x": float(x0_fit),
        "center_pix_y": float(y0_fit),
        "background": float(bg_fit),
    }


# ── A-03: WCS reprojection ────────────────────────────────────────────────────


def worker_reproject(
    path: str,
    output_path: str,
    target_header_dict: dict[str, Any],
    target_shape: list[int],
    order: int,
) -> dict[str, Any]:
    """Reproject a 2-D FITS image to a new WCS using the ``reproject`` package.

    ``order`` = 1 → bilinear interpolation (reproject_interp)
    ``order`` = 3 → exact flux-conserving reproject (reproject_exact)

    The reprojected array is written to ``output_path`` as a new FITS file and
    registered back in the session registry by the caller.
    """
    try:
        from reproject import reproject_interp, reproject_exact  # noqa: PLC0415
    except ImportError as exc:
        raise ImportError(
            "reproject is required for WCS reprojection. "
            "Install with: pip install reproject"
        ) from exc

    from astropy.io import fits as _fits  # noqa: PLC0415
    from astropy.wcs import WCS as _WCS   # noqa: PLC0415

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        if raw.ndim == 2:
            image = np.asarray(raw, dtype=np.float32)
        else:
            # Use first channel of a cube
            squeezed = np.squeeze(raw)
            image = squeezed[0] if squeezed.ndim == 3 else np.asarray(squeezed, dtype=np.float32)
            image = np.asarray(image, dtype=np.float32)
        src_header = hdul[0].header.copy()
    finally:
        hdul.close()

    target_header = _fits.Header()
    for k, v in target_header_dict.items():
        try:
            target_header[k] = v
        except Exception:
            pass
    target_wcs = _WCS(target_header, relax=True)
    target_shape_tuple = (int(target_shape[0]), int(target_shape[1]))

    if order == 3:
        result_arr, footprint = reproject_exact(
            (image, _WCS(src_header, relax=True).celestial),
            target_wcs.celestial,
            shape_out=target_shape_tuple,
        )
    else:
        result_arr, footprint = reproject_interp(
            (image, _WCS(src_header, relax=True).celestial),
            target_wcs.celestial,
            shape_out=target_shape_tuple,
            order=order,
        )

    result_arr = result_arr.astype(np.float32)
    out_header = target_header.copy()
    out_header["NAXIS"] = 2
    out_header["NAXIS1"] = target_shape_tuple[1]
    out_header["NAXIS2"] = target_shape_tuple[0]
    out_hdu = _fits.PrimaryHDU(data=result_arr, header=out_header)
    out_hdu.writeto(output_path, overwrite=True)

    range_min, range_max = _finite_range(result_arr)
    return {
        "width": int(target_shape_tuple[1]),
        "height": int(target_shape_tuple[0]),
        "scalar_type": "float32",
        "range_min": float(range_min),
        "range_max": float(range_max),
        "output_path": output_path,
        "order": order,
    }


# ── A-04: External catalogue cross-match ─────────────────────────────────────


def worker_crossmatch(
    path: str,
    radius_arcsec: float,
    catalogue: str,
    max_sources: int = 500,
) -> dict[str, Any]:
    """Query an external astronomical catalogue using astroquery.

    Returns sources within the bounding box of the FITS image footprint,
    sorted by angular separation from the image centre.

    Supported catalogue identifiers:
      "simbad"  → astroquery.simbad.Simbad
      "2MASS"   → Vizier II/246 (2MASS PSC)
      "NVSS"    → Vizier VIII/65 (NVSS)
      "FIRST"   → Vizier VIII/92 (FIRST)
    """
    VIZIER_CATALOG_IDS = {
        "2MASS": "II/246",
        "NVSS":  "VIII/65",
        "FIRST": "VIII/92",
    }

    dataset = ScientificFitsDataset(path)
    if dataset.wcs is None:
        raise ValueError("WCS not available — cannot compute image footprint.")
    celestial = dataset.wcs.celestial
    if celestial is None:
        raise ValueError("No celestial WCS axes in this dataset.")

    # Image-centre sky coords
    ny = int(dataset.header.get("NAXIS2", dataset.header.get("NAXIS1", 1)))
    nx = int(dataset.header.get("NAXIS1", 1))
    centre_pix = celestial.all_pix2world([[nx / 2.0, ny / 2.0]], 0)[0]
    ra_cen, dec_cen = float(centre_pix[0]), float(centre_pix[1])

    # Search radius = half-diagonal of image (degrees) + extra margin
    pixel_scale_deg = dataset.spatial_pixel_scale_arcsec() / 3600.0
    search_radius_deg = max(
        float(radius_arcsec) / 3600.0,
        0.5 * pixel_scale_deg * np.sqrt(nx ** 2 + ny ** 2),
    )

    cat_lower = catalogue.lower()
    sources: list[dict[str, Any]] = []
    truncated = False

    if cat_lower == "simbad":
        try:
            from astroquery.simbad import Simbad  # noqa: PLC0415
        except ImportError as exc:
            raise ImportError(
                "astroquery is required for SIMBAD cross-matching. "
                "Install with: pip install astroquery"
            ) from exc
        from astropy.coordinates import SkyCoord  # noqa: PLC0415
        import astropy.units as _u                # noqa: PLC0415

        simbad = Simbad()
        simbad.add_votable_fields("ra(d)", "dec(d)")
        result_table = simbad.query_region(
            SkyCoord(ra_cen, dec_cen, unit="deg"),
            radius=search_radius_deg * _u.deg,
        )
        if result_table is not None:
            centre_coord = SkyCoord(ra_cen, dec_cen, unit="deg")
            for row in result_table:
                ra_s  = float(row["RA_d"])  if "RA_d"  in result_table.colnames else float(row["RA"])
                dec_s = float(row["DEC_d"]) if "DEC_d" in result_table.colnames else float(row["DEC"])
                sep = float(centre_coord.separation(
                    SkyCoord(ra_s, dec_s, unit="deg")
                ).arcsec)
                sources.append({
                    "ra_deg": ra_s,
                    "dec_deg": dec_s,
                    "name": str(row.get("MAIN_ID", "")),
                    "catalogue_id": "SIMBAD",
                    "separation_arcsec": sep,
                })
    else:
        # Vizier
        vizier_cat = VIZIER_CATALOG_IDS.get(catalogue.upper())
        if vizier_cat is None:
            raise ValueError(
                f"Unknown catalogue '{catalogue}'. "
                f"Supported: simbad, 2MASS, NVSS, FIRST."
            )
        try:
            from astroquery.vizier import Vizier  # noqa: PLC0415
        except ImportError as exc:
            raise ImportError(
                "astroquery is required for Vizier cross-matching. "
                "Install with: pip install astroquery"
            ) from exc
        from astropy.coordinates import SkyCoord  # noqa: PLC0415
        import astropy.units as _u                # noqa: PLC0415

        v = Vizier(columns=["*"], row_limit=max_sources + 1)
        result_list = v.query_region(
            SkyCoord(ra_cen, dec_cen, unit="deg"),
            radius=search_radius_deg * _u.deg,
            catalog=vizier_cat,
        )
        if result_list and len(result_list) > 0:
            table = result_list[0]
            centre_coord = SkyCoord(ra_cen, dec_cen, unit="deg")
            ra_col  = next((c for c in table.colnames if c.upper() in ("RAJ2000", "_RAJ2000", "RA")), None)
            dec_col = next((c for c in table.colnames if c.upper() in ("DEJ2000", "_DEJ2000", "DEC")), None)
            if ra_col and dec_col:
                for row in table:
                    ra_s  = float(row[ra_col])
                    dec_s = float(row[dec_col])
                    sep = float(centre_coord.separation(
                        SkyCoord(ra_s, dec_s, unit="deg")
                    ).arcsec)
                    name = str(row.get("_2MASS", row.get("NVSS", row.get("Name", ""))))
                    sources.append({
                        "ra_deg": ra_s,
                        "dec_deg": dec_s,
                        "name": name,
                        "catalogue_id": catalogue.upper(),
                        "separation_arcsec": sep,
                    })

    # Sort by separation, cap at max_sources
    sources.sort(key=lambda s: s["separation_arcsec"])
    if len(sources) > max_sources:
        sources = sources[:max_sources]
        truncated = True

    return {
        "n_sources": len(sources),
        "truncated": truncated,
        "centre_ra_deg": ra_cen,
        "centre_dec_deg": dec_cen,
        "search_radius_deg": search_radius_deg,
        "sources": sources,
    }


# ── P-01: Stokes plane extraction ────────────────────────────────────────────


def worker_stokes_plane(
    path: str,
    plane: str,
    channel: int | None,
) -> dict[str, Any]:
    """Extract a single Stokes plane (I, Q, U or V) from a FITS dataset.

    The dataset must have a Stokes axis (CTYPE == 'STOKES').  If a spectral
    (channel) axis is also present, ``channel`` selects the spectral plane;
    None → channel 0.

    Returns a float32 2-D image encoded in base64, plus shape / range metadata.
    """
    plane = plane.upper()
    valid_planes = {"I", "Q", "U", "V"}
    if plane not in valid_planes:
        raise ValueError(f"Invalid Stokes plane '{plane}'. Must be one of {valid_planes}.")

    dataset = ScientificFitsDataset(path)
    stokes_map = dataset.stokes_planes
    if not stokes_map:
        raise ValueError("This dataset has no Stokes axis (CTYPE == STOKES not found).")
    if plane not in stokes_map:
        available = sorted(stokes_map)
        raise ValueError(
            f"Stokes plane '{plane}' not present in this dataset. "
            f"Available: {available}."
        )

    hdul, raw = dataset.open_memmap()
    try:
        data = np.asarray(raw)
    finally:
        hdul.close()

    stokes_numpy_ax = dataset.stokes_numpy_axis()
    if stokes_numpy_ax is None:
        raise RuntimeError("Stokes axis disappeared after detection — internal error.")

    stokes_idx = stokes_map[plane]
    # Select Stokes plane along its numpy axis
    slices_stokes: list[Any] = [slice(None)] * data.ndim
    slices_stokes[stokes_numpy_ax] = stokes_idx
    data = data[tuple(slices_stokes)]  # now ndim - 1

    # If a spectral axis remains, select channel
    if data.ndim == 3:
        ch = 0 if channel is None else int(channel)
        ch = max(0, min(ch, data.shape[0] - 1))
        data = data[ch]
    elif data.ndim > 3:
        # Squeeze any remaining degenerate axes
        data = np.squeeze(data)
        if data.ndim == 3:
            ch = 0 if channel is None else int(channel)
            ch = max(0, min(ch, data.shape[0] - 1))
            data = data[ch]

    image = np.asarray(data, dtype=np.float32)
    range_min, range_max = _finite_range(image)
    return {
        "plane": plane,
        "channel": channel,
        "width": int(image.shape[1]) if image.ndim == 2 else image.size,
        "height": int(image.shape[0]) if image.ndim == 2 else 1,
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "data_base64": _b64f32(image),
    }


# ── P-02: Polarised intensity and position-angle maps ────────────────────────


def worker_polarisation_pi_pa(
    path: str,
    channel: int | None,
    line_free_channels: list[int] | None,
) -> dict[str, Any]:
    """Compute bias-corrected polarised intensity (PI) and position angle (PA).

    PI_corr = sqrt(max(Q² + U² − σ_QU², 0))
    PA      = 0.5 × atan2(U, Q)  [degrees, range −90..+90]

    σ_QU² is estimated from line-free channels (if provided) as the mean
    of the per-pixel variance of Q and U over those channels; if not provided
    or if the dataset is 2-D, σ_QU² = 0 (no bias correction).

    Both output maps are float32, returned as base64 payloads.
    """
    dataset = ScientificFitsDataset(path)
    stokes_map = dataset.stokes_planes
    for plane in ("Q", "U"):
        if plane not in stokes_map:
            raise ValueError(
                f"Stokes plane '{plane}' required for PI/PA computation but not found. "
                f"Available: {sorted(stokes_map)}."
            )

    hdul, raw = dataset.open_memmap()
    try:
        data = np.asarray(raw, dtype=np.float64)
    finally:
        hdul.close()

    stokes_numpy_ax = dataset.stokes_numpy_axis()
    assert stokes_numpy_ax is not None

    def _extract_plane(arr: np.ndarray, stokes_idx: int, ch: int | None) -> np.ndarray:
        slc: list[Any] = [slice(None)] * arr.ndim
        slc[stokes_numpy_ax] = stokes_idx
        plane_arr = arr[tuple(slc)]
        if plane_arr.ndim == 3:
            c = 0 if ch is None else int(ch)
            c = max(0, min(c, plane_arr.shape[0] - 1))
            return plane_arr[c]
        return np.squeeze(plane_arr) if plane_arr.ndim != 2 else plane_arr

    ch = 0 if channel is None else int(channel)
    Q = _extract_plane(data, stokes_map["Q"], ch)
    U = _extract_plane(data, stokes_map["U"], ch)

    # Bias correction: σ²_QU from line-free channels
    sigma_qu2 = 0.0
    if line_free_channels and len(line_free_channels) >= 2:
        slc_q: list[Any] = [slice(None)] * data.ndim
        slc_q[stokes_numpy_ax] = stokes_map["Q"]
        q_cube = data[tuple(slc_q)]  # (nchans, ny, nx) or (ny, nx)
        slc_u: list[Any] = [slice(None)] * data.ndim
        slc_u[stokes_numpy_ax] = stokes_map["U"]
        u_cube = data[tuple(slc_u)]
        if q_cube.ndim == 3:
            lfc = np.array([i for i in line_free_channels if 0 <= i < q_cube.shape[0]])
            if len(lfc) >= 2:
                q_lf = q_cube[lfc]
                u_lf = u_cube[lfc]
                sigma_qu2 = float(
                    np.nanmean(np.nanvar(q_lf, axis=0))
                    + np.nanmean(np.nanvar(u_lf, axis=0))
                ) / 2.0

    pi2 = Q ** 2 + U ** 2 - sigma_qu2
    pi_corr = np.sqrt(np.where(pi2 > 0.0, pi2, 0.0)).astype(np.float32)
    pa_deg  = (0.5 * np.degrees(np.arctan2(U, Q))).astype(np.float32)

    pi_range = _finite_range(pi_corr)
    pa_range = _finite_range(pa_deg)
    ny, nx = pi_corr.shape
    return {
        "width": nx,
        "height": ny,
        "channel": channel,
        "sigma_qu2": sigma_qu2,
        "scalar_type": "float32",
        "pi_range_min": pi_range[0],
        "pi_range_max": pi_range[1],
        "pa_range_min": pa_range[0],
        "pa_range_max": pa_range[1],
        "pi_base64": _b64f32(pi_corr),
        "pa_base64": _b64f32(pa_deg),
    }


# ── P-03: Rotation Measure synthesis ─────────────────────────────────────────


def worker_rm_synthesis(
    path: str,
    phi_min: float,
    phi_max: float,
    d_phi: float,
) -> dict[str, Any]:
    """Rotation Measure synthesis (Brentjens & de Bruyn 2005).

    Computes the Faraday Dispersion Function (FDF):
      F(φ) = (1/N) Σ_i  P(λ²_i) × exp(−2i φ λ²_i)

    where P = Q + iU and λ² is derived from the frequency axis via WCS.

    The dataset must have both Q and U Stokes planes and a frequency spectral
    axis.  φ is sampled uniformly in [phi_min, phi_max) with step d_phi.

    Returns:
      - |F(φ)| as a 3-D float32 array (n_phi × ny × nx), base64-encoded
      - RMSF (RM spread function) as a 1-D list of floats
      - phi_grid: the φ sample points (rad/m²)
    """
    C_LIGHT = 2.99792458e8  # m/s

    dataset = ScientificFitsDataset(path)
    stokes_map = dataset.stokes_planes
    for plane in ("Q", "U"):
        if plane not in stokes_map:
            raise ValueError(
                f"Stokes plane '{plane}' required for RM synthesis but not found. "
                f"Available: {sorted(stokes_map)}."
            )

    hdul, raw = dataset.open_memmap()
    try:
        data = np.asarray(raw, dtype=np.float64)
    finally:
        hdul.close()

    stokes_numpy_ax = dataset.stokes_numpy_axis()
    assert stokes_numpy_ax is not None

    def _get_stokes_cube(arr: np.ndarray, stokes_idx: int) -> np.ndarray:
        slc: list[Any] = [slice(None)] * arr.ndim
        slc[stokes_numpy_ax] = stokes_idx
        cube = arr[tuple(slc)]
        return np.squeeze(cube)   # should be (nchans, ny, nx) or (ny, nx)

    Q_cube = _get_stokes_cube(data, stokes_map["Q"])
    U_cube = _get_stokes_cube(data, stokes_map["U"])

    if Q_cube.ndim == 2:
        Q_cube = Q_cube[np.newaxis, :, :]
        U_cube = U_cube[np.newaxis, :, :]

    if Q_cube.ndim != 3:
        raise ValueError(f"Expected 3-D (freq, ny, nx) Q/U cube, got shape {Q_cube.shape}.")

    n_freq, ny, nx = Q_cube.shape

    # Derive λ² from frequency axis via WCS spectral axis
    spectral = dataset.spectral_axis_metadata(0, n_freq - 1)
    freq_hz = np.asarray(spectral["coordinates"], dtype=np.float64)
    # Convert to wavelength squared: λ² = (c/f)²
    lambda2 = (C_LIGHT / freq_hz) ** 2   # (n_freq,)

    # φ grid
    phi_grid = np.arange(phi_min, phi_max, d_phi, dtype=np.float64)
    n_phi = len(phi_grid)
    if n_phi == 0:
        raise ValueError("Empty φ grid — check phi_min, phi_max, d_phi.")

    # Complex polarisation cube
    P_cube = (Q_cube + 1j * U_cube).astype(np.complex128)   # (n_freq, ny, nx)

    # FDF: F[phi, y, x] = (1/N) Σ_f P[f,y,x] * exp(-2i*phi*lambda2[f])
    # Shape: (n_phi, n_freq) phase kernel → (n_phi, ny, nx) FDF
    kernel = np.exp(-2j * np.outer(phi_grid, lambda2))  # (n_phi, n_freq)
    # Reshape for broadcasting: (n_phi, n_freq, 1, 1) × (n_freq, ny, nx)
    F_complex = np.einsum("pf,fyx->pyx", kernel, P_cube) / float(n_freq)
    FDF = np.abs(F_complex).astype(np.float32)           # (n_phi, ny, nx)

    # RMSF: theoretical RM spread function as a 1-D array in the same φ space
    # R(φ) = (1/N) Σ_f exp(-2i*φ*λ²_f)
    rmsf_complex = np.einsum("pf->p", kernel) / float(n_freq)
    rmsf = np.abs(rmsf_complex).tolist()

    range_min, range_max = _finite_range(FDF)
    return {
        "n_phi": n_phi,
        "n_freq": n_freq,
        "width": nx,
        "height": ny,
        "scalar_type": "float32",
        "phi_min": float(phi_min),
        "phi_max": float(phi_max),
        "d_phi": float(d_phi),
        "phi_grid": phi_grid.tolist(),
        "rmsf": rmsf,
        "range_min": float(range_min),
        "range_max": float(range_max),
        "data_base64": _b64f32(FDF),
    }


# ── C-01: Angular power spectrum ──────────────────────────────────────────────

def worker_angular_power_spectrum(
    path: str,
    channel: int = 0,
    n_bins: int = 64,
) -> dict[str, Any]:
    """Compute the azimuthally averaged angular power spectrum of a 2-D slice.

    Algorithm:
      1. Extract the requested channel plane (or collapse to 2-D if already 2-D).
      2. Replace NaNs with the finite mean so FFT is well-defined.
      3. Apply a 2-D Hann window to reduce edge ringing.
      4. Compute the 2-D real FFT and its power: |FFT|².
      5. Compute the radial wavenumber |k| = sqrt(kx²+ky²) for each bin.
      6. Bin the power azimuthally into `n_bins` logarithmically spaced bins.
      7. Convert k [pixel⁻¹] → angular multipole ℓ ≈ 2π / (pixel_scale_rad × k).

    Returns:
      k_bins  – 1-D float64 array of bin-centre wavenumbers (pixel⁻¹)
      ell     – 1-D float64 array of corresponding angular multipoles
      power / cl – 1-D float64 mean power per bin
      cl_err  – 1-D float64 standard error of power per bin
    """
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = np.asarray(raw, dtype=np.float64)
    finally:
        hdul.close()

    # Squeeze degenerate axes; select channel from spectral axis if 3-D
    data = np.squeeze(data)
    if data.ndim == 3:
        data = data[channel, :, :]
    elif data.ndim == 4:
        # (stokes, freq, ny, nx) or similar – take first stokes, given channel
        data = data[0, channel, :, :]
    if data.ndim != 2:
        raise ValueError(f"Expected 2-D plane after slicing, got shape {data.shape}.")

    ny, nx = data.shape

    # Replace NaNs with the finite mean
    finite_mask = np.isfinite(data)
    if not finite_mask.any():
        raise ValueError("All pixels are NaN – cannot compute power spectrum.")
    data = data.copy()
    data[~finite_mask] = float(np.nanmean(data[finite_mask]))

    # Pixel scale in radians (for ℓ conversion); fall back to |CDELT1| in degrees
    try:
        pixel_scale_rad = float(dataset.spatial_pixel_scale_arcsec() * np.pi / (180.0 * 3600.0))
    except Exception:
        pixel_scale_rad = abs(float(dataset.header.get("CDELT1", 1.0))) * np.pi / 180.0
    if pixel_scale_rad <= 0.0:
        pixel_scale_rad = 1.0   # fallback: unitless

    # 2-D Hann window
    wy = np.hanning(ny)[:, np.newaxis]
    wx = np.hanning(nx)[np.newaxis, :]
    windowed = data * wy * wx

    # 2-D real FFT
    fft2 = np.fft.rfft2(windowed)
    power2d = np.abs(fft2) ** 2          # (ny, nx//2+1)

    # Wavenumber grids (pixel⁻¹, 0-centred for kx via rfftfreq, ky via fftfreq)
    kx = np.fft.rfftfreq(nx)             # (nx//2+1,) in [0, 0.5]
    ky = np.fft.fftfreq(ny)              # (ny,) in [-0.5, 0.5)
    KX, KY = np.meshgrid(kx, ky)
    K = np.sqrt(KX**2 + KY**2).ravel()
    P = power2d.ravel()

    # Azimuthal binning: log-spaced bins from k_min to 0.5 (Nyquist)
    k_min = max(1.0 / max(ny, nx), 1e-6)
    k_max = 0.5
    bins = np.logspace(np.log10(k_min), np.log10(k_max), n_bins + 1)
    bin_centres = 0.5 * (bins[:-1] + bins[1:])

    k_bins_out: list[float] = []
    ell_out: list[float] = []
    cl_out: list[float] = []
    cl_err_out: list[float] = []
    for i in range(n_bins):
        mask = (K >= bins[i]) & (K < bins[i + 1])
        if mask.any():
            p_bin = P[mask]
            k_c = float(bin_centres[i])
            cl_mean = float(np.mean(p_bin))
            cl_std = float(np.std(p_bin) / np.sqrt(len(p_bin))) if len(p_bin) > 1 else 0.0
            # ℓ ≈ 2π * k_pixel / pixel_scale_rad  (angular multipole)
            ell = float(2.0 * np.pi * k_c / pixel_scale_rad) if pixel_scale_rad > 0 else k_c
            k_bins_out.append(k_c)
            ell_out.append(ell)
            cl_out.append(cl_mean)
            cl_err_out.append(cl_std)

    if not k_bins_out:
        raise ValueError("No valid wavenumber bins – image too small?")

    return {
        "channel": channel,
        "width": nx,
        "height": ny,
        "n_bins": len(k_bins_out),
        "k_bins": k_bins_out,
        "ell": ell_out,
        "cl": cl_out,
        "cl_err": cl_err_out,
        # Legacy aliases kept for backward compatibility
        "power": cl_out,
        "k_unit": "pixel^-1",
        "power_unit": "counts^2",
    }


# ── C-03: Skeleton / filament and void detection ──────────────────────────────

def _skeleton_endpoints(skel: "np.ndarray") -> list[list[int]]:
    """Return [row, col] coordinates of filament endpoints (degree-1 skeleton pixels)."""
    from scipy.ndimage import convolve as _convolve  # noqa: PLC0415
    # Connectivity kernel: count 8-connected neighbours
    kernel = np.ones((3, 3), dtype=np.uint8)
    kernel[1, 1] = 0
    neighbour_count = _convolve(skel.astype(np.uint8), kernel, mode="constant", cval=0)
    # Endpoint = skeleton pixel with exactly 1 neighbour
    ep_mask = skel & (neighbour_count == 1)
    rows, cols = np.where(ep_mask)
    return [[int(r), int(c)] for r, c in zip(rows, cols)]


def worker_skeleton(
    path: str,
    channel: int = 0,
    threshold: float | None = None,
    mode: str = "skeleton",
    smooth_sigma_pix: float = 0.0,
    persistence_threshold: float | None = None,
) -> dict[str, Any]:
    """Compute the morphological skeleton or void mask of a 2-D slice.

    Requires ``scikit-image`` (optional).

    Args:
        threshold:             intensity threshold to binarise the image.
                               If None, uses the mean of finite pixels.
        mode:                  "skeleton" (medial-axis thinning) or "void"
                               (skeleton of low-emission regions).
        smooth_sigma_pix:      sigma (pixels) for Gaussian pre-smoothing.
                               0 disables smoothing.
        persistence_threshold: secondary threshold applied after skeleton
                               extraction to prune faint branches.  None disables.

    Returns:
        data_base64:      float32 binary mask (0 or 255) base64-encoded
        n_skeleton_pixels: number of True pixels in the skeleton
        endpoints:        list of [row, col] filament endpoint coordinates
    """
    try:
        from skimage.morphology import skeletonize as _skeletonize  # noqa: PLC0415
    except ImportError as exc:  # pragma: no cover
        raise ImportError(
            "scikit-image is required for skeleton detection. "
            "Install with: pip install scikit-image"
        ) from exc

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = np.asarray(raw, dtype=np.float64)
    finally:
        hdul.close()

    data = np.squeeze(data)
    if data.ndim == 3:
        data = data[channel, :, :]
    elif data.ndim == 4:
        data = data[0, channel, :, :]
    if data.ndim != 2:
        raise ValueError(f"Expected 2-D plane, got shape {data.shape}.")

    ny, nx = data.shape
    finite = np.isfinite(data)
    if not finite.any():
        raise ValueError("All pixels are NaN.")

    # Optional Gaussian pre-smoothing
    if smooth_sigma_pix and smooth_sigma_pix > 0:
        from scipy.ndimage import gaussian_filter as _gf  # noqa: PLC0415
        filled = np.where(finite, data, 0.0)
        data = _gf(filled, sigma=smooth_sigma_pix)
        finite = np.ones_like(data, dtype=bool)

    if threshold is None:
        threshold = float(np.nanmean(data[finite]))

    binary = (data > threshold) & finite

    if mode == "void":
        binary = ~binary & finite

    skel = _skeletonize(binary)           # bool array

    # Optional persistence-based branch pruning
    if persistence_threshold is not None and persistence_threshold > 0:
        # Prune skeleton pixels whose underlying intensity is below threshold
        prune_mask = (data < persistence_threshold) & skel
        skel = skel & ~prune_mask

    skel_f32 = skel.astype(np.float32) * 255.0   # 0 or 255

    endpoints = _skeleton_endpoints(skel)
    range_min, range_max = _finite_range(skel_f32)
    return {
        "channel": channel,
        "width": nx,
        "height": ny,
        "threshold": float(threshold),
        "mode": mode,
        "smooth_sigma_pix": float(smooth_sigma_pix),
        "n_skeleton_pixels": int(skel.sum()),
        "range_min": float(range_min),
        "range_max": float(range_max),
        "data_base64": _b64f32(skel_f32),
        "endpoints": endpoints,
    }
