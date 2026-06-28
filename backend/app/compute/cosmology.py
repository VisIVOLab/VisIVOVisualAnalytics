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


# ── S-05: SoFiA-2 source finding (radio HI) ────────────────────────────────────
# Runs SoFiA-2 on a node-local cube and returns the detection catalogue plus the
# path to the binary mask cube (when SoFiA writes one) — the mask cube is what
# the desktop renders as a 3D isosurface overlay on the HI cube.
