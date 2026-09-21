from __future__ import annotations

import base64
import contextlib
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
    """Azimuthally averaged 2-D power spectrum of a slice — a RELATIVE texture
    spectrum, NOT a calibrated angular power spectrum Cℓ.

    R-15 — honesty: this is a windowed |FFT|² binned in |k|. The Hann window's
    power loss IS corrected (divide by ⟨w²⟩), but there is no mask/apodization
    mode-coupling deconvolution and no beam/pixel-window correction, so the
    absolute normalisation and the low-ℓ shape are NOT science-grade. Use it to
    compare the relative structure of two maps, not as a cosmological Cℓ.

    The ERROR BAR does account for mode coupling, which the Cℓ values themselves
    do not: the modes inside a |k| bin are correlated by the window, so the naive
    std/√N counts more independent samples than exist. Measured over Gaussian
    random fields it was ~2x too small, matching √(⟨w⁴⟩/⟨w²⟩²) = 1.94 for this
    window, and that factor is now applied. The correction is derived from the
    window array itself rather than hard-coded, but it is an ASYMPTOTIC
    populated-bin result: the exact band-power variance depends on the bin and the
    window kernel, so it is not a guarantee for every binning. Measured at the
    default binning it holds to 0.5–1.6x on bins with more than 8 modes and
    0.3–2.0x on sparser ones — hence ``n_modes`` alongside every error, and a null
    error for a bin with fewer than 3 modes.

    Algorithm:
      1. Extract the requested channel plane (or collapse to 2-D if already 2-D).
      2. Replace NaNs with the finite mean so FFT is well-defined.
      3. Apply a 2-D Hann window to reduce edge ringing; normalise by ⟨w²⟩.
      4. Compute the 2-D real FFT and its power: |FFT|².
      5. Compute the radial wavenumber |k| = sqrt(kx²+ky²) for each bin.
      6. Bin the power azimuthally into `n_bins` logarithmically spaced bins.
      7. Convert k [pixel⁻¹] → angular multipole ℓ ≈ 2π · k / pixel_scale_rad.

    Returns:
      k_bins  – 1-D float64 array of bin-centre wavenumbers (pixel⁻¹)
      ell     – 1-D float64 array of corresponding angular multipoles
      power / cl – 1-D float64 mean power per bin (relative, uncalibrated)
      cl_err  – standard error of power per bin, corrected for the window's mode
                coupling (the modes in a bin are not independent; see the note
                below). NULL for a bin holding fewer than 3 modes, where no
                standard error is estimable.
      n_modes – unique Fourier modes per bin, so a caller can see how well
                determined each error is
      mode_coupling – the window's effective-mode penalty. A bin's EFFECTIVE mode
                count is ``n_modes / mode_coupling**2``, which is far smaller than
                the raw count suggests: with this window's 1.94, eight raw modes
                are only about two effective ones. Judge a bin by that, not by
                n_modes.
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

    # Pixel scale in radians, for the ℓ conversion — and ONLY when the header
    # actually carries one. `spatial_pixel_scale_arcsec()` answers 3600 arcsec
    # for a map with no WCS at all (astropy's 1 degree per pixel default), so
    # asking it unconditionally produced a multipole axis computed from an
    # invented pixel size: ℓ looked calibrated and was wrong by whatever the
    # real scale is. A map with no angular information gets ℓ = k and says so.
    header = dataset.header
    # The question is whether the map has a CELESTIAL scale, which is not the
    # same as "the header has a CDELT". A linear axis in metres has one too, and
    # a map with no WCS at all is answered 3600 arcsec by the pixel-scale helper
    # (astropy's one-degree-per-pixel default) — either way ℓ came back looking
    # calibrated and wrong. astropy's own `has_celestial` decides it, which also
    # covers the scale being carried by a PC matrix or a CD matrix instead of
    # CDELT; the header check below is only for a file astropy could not parse.
    has_angular_scale = False
    wcs = getattr(dataset, "wcs", None)
    if wcs is not None:
        with contextlib.suppress(Exception):
            # And the celestial pair has to be the plane that was SLICED OUT.
            # The slice above always takes the leading numpy axis, i.e. the
            # last FITS axis, so the plane is celestial only when FITS axes 1
            # and 2 are. A cube written (FREQ, RA, DEC) — spectral first — hands
            # back an RA/frequency plane, and calling its pixel scale an angle
            # would put a multipole axis on a spectrum.
            celestial_axes = {
                index + 1
                for index, ctype in enumerate(wcs.wcs.ctype)
                if str(ctype).strip().upper().startswith(
                    ("RA--", "DEC-", "GLON", "GLAT", "ELON", "ELAT",
                     "SLON", "SLAT", "HPLN", "HPLT"))
            }
            has_angular_scale = bool(wcs.has_celestial) and celestial_axes == {1, 2}
    if not has_angular_scale and wcs is None:
        ctype1 = str(header.get("CTYPE1", "")).strip().upper()
        has_angular_scale = (
            ctype1.startswith(("RA--", "DEC-", "GLON", "GLAT", "ELON", "ELAT",
                               "SLON", "SLAT", "HPLN", "HPLT"))
            and abs(float(header.get("CDELT1", 0.0) or 0.0)) > 0.0)

    pixel_scale_rad = 0.0
    if has_angular_scale:
        with contextlib.suppress(Exception):
            pixel_scale_rad = float(
                dataset.spatial_pixel_scale_arcsec() * np.pi / (180.0 * 3600.0))
    if pixel_scale_rad < 0.0:
        pixel_scale_rad = 0.0

    # 2-D Hann window
    wy = np.hanning(ny)[:, np.newaxis]
    wx = np.hanning(nx)[np.newaxis, :]
    window = wy * wx
    windowed = data * window
    # R-15: correct the window's power loss so the amplitude isn't suppressed by
    # ⟨w²⟩ (the standard windowed-periodogram normalisation).
    w2_mean = float(np.mean(window ** 2)) or 1.0

    # 2-D real FFT
    fft2 = np.fft.rfft2(windowed)
    power2d = (np.abs(fft2) ** 2) / w2_mean   # (ny, nx//2+1)

    # Wavenumber grids (pixel⁻¹, 0-centred for kx via rfftfreq, ky via fftfreq)
    kx = np.fft.rfftfreq(nx)             # (nx//2+1,) in [0, 0.5]
    ky = np.fft.fftfreq(ny)              # (ny,) in [-0.5, 0.5)
    KX, KY = np.meshgrid(kx, ky)
    # rfft2 keeps only half the plane, but the kx = 0 column — and, for even nx,
    # the Nyquist column — still hold BOTH members of each Hermitian pair, so
    # (0, ky) and (0, -ky) are the same mode twice. Counting them twice inflates
    # the mode count and shrinks the error; drop the negative-ky half of those
    # columns. Everywhere else rfft2's modes are already unique.
    duplicate = (KY < 0) & ((KX == 0.0) | (np.isclose(KX, 0.5) if nx % 2 == 0 else False))
    keep = ~duplicate.ravel()
    K = np.sqrt(KX**2 + KY**2).ravel()[keep]
    P = power2d.ravel()[keep]

    # Azimuthal binning: log-spaced bins from k_min to 0.5 (Nyquist)
    k_min = max(1.0 / max(ny, nx), 1e-6)
    k_max = 0.5
    bins = np.logspace(np.log10(k_min), np.log10(k_max), n_bins + 1)
    bin_centres = 0.5 * (bins[:-1] + bins[1:])

    # Effective-mode penalty from the window's own shape rather than a hard-coded
    # constant: sqrt of ⟨w⁴⟩/⟨w²⟩², clipped at 1 (an unwindowed map gives exactly
    # 1 and no correction). Asymptotic and populated-bin — the exact band-power
    # variance is bin- and kernel-dependent, so this is not a guarantee for an
    # arbitrary window or binning; see the docstring for the measured range.
    mode_coupling = float(np.sqrt(max(
        np.mean(window ** 4) / max(np.mean(window ** 2) ** 2, 1e-30), 1.0)))

    k_bins_out: list[float] = []
    ell_out: list[float] = []
    cl_out: list[float] = []
    cl_err_out: list[float | None] = []
    n_modes_out: list[int] = []
    for i in range(n_bins):
        mask = (K >= bins[i]) & (K < bins[i + 1])
        if mask.any():
            p_bin = P[mask]
            k_c = float(bin_centres[i])
            cl_mean = float(np.mean(p_bin))
            # Standard error of the bin mean, corrected for the fact that the
            # Fourier modes in a bin are NOT independent samples: the Hann window
            # couples neighbouring ones. The effective count is reduced by
            # ⟨w⁴⟩/⟨w²⟩² — 1.94² for a separable 2-D Hann — so the naive
            # std/√N understates the error by ~1.94x. Measured over 40 Gaussian
            # random fields of both white and red spectra, the uncorrected value
            # came out 1.7–2.5x too small, matching that factor (see
            # tests/test_power_spectrum_errors.py).
            # Below three modes the sample standard deviation estimates nothing —
            # a 2-point std carries ~50 % uncertainty itself — so report null
            # rather than a number the correction cannot rescue. Measured, these
            # are exactly the bins that came out worst (ratio ~2 against a median
            # of ~1), and they are always the sparse ones; `n_modes` says which.
            cl_std = (float(np.std(p_bin) / np.sqrt(len(p_bin))) * mode_coupling
                      if len(p_bin) >= 3 else None)
            # ℓ ≈ 2π * k_pixel / pixel_scale_rad  (angular multipole)
            ell = float(2.0 * np.pi * k_c / pixel_scale_rad) if pixel_scale_rad > 0 else k_c
            k_bins_out.append(k_c)
            ell_out.append(ell)
            cl_out.append(cl_mean)
            cl_err_out.append(cl_std)
            n_modes_out.append(int(len(p_bin)))

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
        # Unique Fourier modes per bin. The mode-coupling correction below is an
        # asymptotic, populated-bin result; a bin holding a handful of modes has a
        # poorly determined error whatever scalar is applied, so the count is
        # returned rather than left for the caller to guess at.
        "n_modes": n_modes_out,
        # Whether `ell` is an angular multipole at all. False for a map with no
        # spatial WCS, where it repeats `k_bins` (pixel⁻¹) rather than being
        # scaled by a pixel size nobody stated.
        "ell_is_angular": bool(pixel_scale_rad > 0.0),
        "pixel_scale_arcsec": (float(pixel_scale_rad * 180.0 * 3600.0 / np.pi)
                               if pixel_scale_rad > 0.0 else None),
        # The window's effective-mode penalty. A bin's EFFECTIVE mode count is
        # n_modes / mode_coupling² — which is much smaller than it looks: with the
        # Hann window's 1.94, eight raw modes are only about two effective ones.
        "mode_coupling": mode_coupling,
        # Legacy aliases kept for backward compatibility
        "power": cl_out,
        "k_unit": "pixel^-1",
        "power_unit": "counts^2",
        # R-15: relative texture spectrum, not a calibrated Cℓ (no mask/beam
        # deconvolution). Clients must not present it as a cosmological Cℓ.
        "calibrated": False,
        "note": ("relative (uncalibrated) power spectrum: Hann-windowed |FFT|² "
                 "with window-power correction; no mask/beam deconvolution"),
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
