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


# ── Region statistics (2D ROI mean/rms/sum/flux) ─────────────────────────────
