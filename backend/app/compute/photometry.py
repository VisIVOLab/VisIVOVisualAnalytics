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
    beam_area_pixels,
    is_per_beam_bunit,
    covariance_propagate,
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
    # Resolved plane index actually used for the image — also drives the
    # per-channel beam lookup so image and beam never disagree.
    resolved_channel: int | None = None
    try:
        if raw.ndim == 2:
            image = np.asarray(raw, dtype=np.float64)
        elif raw.ndim == 3:
            ch = 0 if channel is None else int(channel)
            ch = max(0, min(ch, raw.shape[0] - 1))
            resolved_channel = ch
            image = np.asarray(raw[ch], dtype=np.float64)
        elif raw.ndim == 4:
            ch = 0 if channel is None else int(channel)
            ch = max(0, min(ch, raw.shape[1] - 1))
            resolved_channel = ch
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
    # Local noise per pixel from the background annulus (used for the flux error).
    bkg_rms = float(ann_stats.std) if np.isfinite(ann_stats.std) else 0.0

    # Aperture sum
    ap_stats = ApertureStats(image, aperture, sigma_clip=None)
    n_pix = int(ap_stats.sum_aper_area.value) if hasattr(ap_stats.sum_aper_area, "value") else int(np.pi * r_ap ** 2)
    # A non-finite aperture sum means blanked/NaN pixels fall inside the aperture.
    # Report NaN (honest) rather than substituting the WHOLE-IMAGE sum, which
    # would be an order-of-magnitude-wrong aperture flux.
    raw_sum = float(ap_stats.sum) if np.isfinite(ap_stats.sum) else float("nan")
    # Background-subtracted summed brightness. For Jy/beam data this is in
    # (Jy/beam)·pixel and must be divided by the beam area to become Jy; for
    # other BUNIT it is a plain sum in BUNIT·pixel.
    flux_summed = raw_sum - bkg_per_pix * n_pix

    # R-01: beam-correct to Jy only when the data are per-beam AND the beam is
    # known. ``flux_jy`` is the physically-meaningful flux (Jy) or None; ``flux``
    # is ALWAYS the raw summed quantity in ``flux_unit`` (BUNIT·pixel) — never
    # relabelled Jy. Previously the raw sum was returned mislabeled as ``flux_jy``
    # (off by the beam area, 10–50×).
    # R-05: use the per-channel beam when the cube carries a BEAMS table.
    beam = dataset.beam_for_channel(resolved_channel)
    beam_area_px = beam_area_pixels(beam.major, beam.minor, pixel_scale_arcsec, dataset.spatial_pixel_area_arcsec2())
    per_beam = is_per_beam_bunit(dataset.bunit)
    flux_jy: float | None = (flux_summed / beam_area_px) if (per_beam and beam_area_px) else None
    flux_unit = f"{dataset.bunit}*pixel" if dataset.bunit else "pixel_sum"

    # R-09: correlated-noise uncertainty. Pixels within a beam are not
    # independent (n_beam pixels per beam). The error on the raw summed
    # brightness is rms·sqrt(n_pix·n_beam); the physical flux error in Jy is that
    # divided by the beam area, i.e. rms·sqrt(n_pix/n_beam). When the beam is
    # unknown we fall back to the independent-pixel form (n_beam = 1) for the raw
    # sum error and leave flux_err_jy = None. This replaces the old
    # counts/Poisson formula, which is dimensionless-wrong for Jy/beam.
    n_beam = beam_area_px if beam_area_px else 1.0
    err_summed = float(bkg_rms * np.sqrt(max(n_pix, 0) * n_beam)) if bkg_rms > 0 else 0.0
    flux_err_jy = (err_summed / beam_area_px) if (per_beam and beam_area_px) else None

    return {
        # Physical flux in Jy (None when not per-beam data or beam unknown).
        "flux_jy": flux_jy,
        "flux_err_jy": flux_err_jy,
        # Raw background-subtracted sum + its honest unit, always available.
        "flux": flux_summed,
        "flux_err": err_summed,
        "flux_unit": flux_unit,
        "beam_area_pix": beam_area_px,
        "background_per_pix": bkg_per_pix,
        "background_rms_per_pix": bkg_rms,
        "bunit": dataset.bunit,
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
    """Deconvolve a beam from a fitted Gaussian (all FWHM sizes in pixels).

    Full PA-aware Gaussian deconvolution (Wild 1970; the AIPS/CASA/miriad
    formula), NOT a per-axis quadrature subtraction — so when the beam PA differs
    from the source PA the deconvolved major/minor/PA are correct. Returns
    (deconv_major_pix, deconv_minor_pix, deconv_pa_rad), or (None, None, None)
    when the source is unresolved (the beam is at least as large as the source).
    """
    # Work directly with FWHM² projected onto x/y (Wild's α, β, γ). Constant
    # scale factors cancel, so FWHM in, FWHM out.
    a1c, a1s = major_pix * np.cos(pa_rad), major_pix * np.sin(pa_rad)
    b1c, b1s = minor_pix * np.cos(pa_rad), minor_pix * np.sin(pa_rad)
    a2c, a2s = beam_major_pix * np.cos(beam_pa_rad), beam_major_pix * np.sin(beam_pa_rad)
    b2c, b2s = beam_minor_pix * np.cos(beam_pa_rad), beam_minor_pix * np.sin(beam_pa_rad)

    alpha = (a1c ** 2 + b1s ** 2) - (a2c ** 2 + b2s ** 2)
    beta = (a1s ** 2 + b1c ** 2) - (a2s ** 2 + b2c ** 2)
    gamma = 2.0 * ((minor_pix ** 2 - major_pix ** 2) * np.sin(pa_rad) * np.cos(pa_rad)
                   - (beam_minor_pix ** 2 - beam_major_pix ** 2)
                   * np.sin(beam_pa_rad) * np.cos(beam_pa_rad))

    s = alpha + beta
    t = float(np.sqrt((alpha - beta) ** 2 + gamma ** 2))
    if alpha < 0.0 or beta < 0.0 or s <= t:
        return None, None, None   # unresolved / point source (incl. source == beam)

    deconv_maj_pix = float(np.sqrt(0.5 * (s + t)))
    deconv_min_pix = float(np.sqrt(0.5 * (s - t)))
    if abs(gamma) + abs(alpha - beta) == 0.0:
        deconv_pa_rad = 0.0
    else:
        deconv_pa_rad = 0.5 * float(np.arctan2(-gamma, alpha - beta))
    return deconv_maj_pix, deconv_min_pix, deconv_pa_rad


def _beam_pa_in_pixel_frame(celestial, ra_deg: float, dec_deg: float,
                            bpa_sky_deg: float) -> float:
    """Convert a beam position angle (BPA, degrees East of North on the sky) to
    the fitter's PIXEL frame (radians, CCW from +x), using the actual WCS.

    Derives the pixel-space directions of celestial North and East by projecting
    small sky offsets through the WCS, so it is correct for rotated/skewed/parity-
    flipped images — not just north-up ones. The fitted source PA is already in
    this pixel frame, so both go into the deconvolution consistently.
    """
    d = 1.0 / 3600.0  # 1-arcsec offsets
    p0 = celestial.all_world2pix([[ra_deg, dec_deg]], 0)[0]
    p_n = celestial.all_world2pix([[ra_deg, dec_deg + d]], 0)[0]
    cosd = max(1e-6, abs(np.cos(np.radians(dec_deg))))
    p_e = celestial.all_world2pix([[ra_deg + d / cosd, dec_deg]], 0)[0]
    ang_north = np.arctan2(p_n[1] - p0[1], p_n[0] - p0[0])
    ang_east = np.arctan2(p_e[1] - p0[1], p_e[0] - p0[0])
    # Handedness: is East +90° (CCW) or −90° (CW) from North in pixel space?
    sign = 1.0 if np.sin(ang_east - ang_north) >= 0 else -1.0
    return float(ang_north + sign * np.radians(bpa_sky_deg))


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
    # Resolved plane index actually used for the image — also drives the
    # per-channel beam lookup so image and beam never disagree.
    resolved_channel: int | None = None
    try:
        if raw.ndim == 2:
            image = np.asarray(raw, dtype=np.float64)
        elif raw.ndim == 3:
            ch = 0 if channel is None else int(channel)
            ch = max(0, min(ch, raw.shape[0] - 1))
            resolved_channel = ch
            image = np.asarray(raw[ch], dtype=np.float64)
        elif raw.ndim == 4:
            ch = 0 if channel is None else int(channel)
            ch = max(0, min(ch, raw.shape[1] - 1))
            resolved_channel = ch
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

    # R-08: 1-sigma parameter uncertainties from the fit covariance. curve_fit
    # returns an inf/nan covariance when a parameter is unconstrained; guard so
    # a bad fit yields None error bars rather than inf. FUL ordering matches p0:
    # [amp, x0, y0, sigma_x, sigma_y, theta, offset].
    with np.errstate(invalid="ignore"):
        perr = np.sqrt(np.diag(pcov))
    perr = [float(e) if np.isfinite(e) else None for e in perr]
    amp_err, _x0e, _y0e, sx_err, sy_err, theta_err, _bge = perr

    # sigma → FWHM
    fwhm_x_pix = abs(sx_fit) * 2.3548
    fwhm_y_pix = abs(sy_fit) * 2.3548
    # Sort major/minor
    if fwhm_x_pix >= fwhm_y_pix:
        major_pix, minor_pix, pa_rad = fwhm_x_pix, fwhm_y_pix, float(theta_fit)
        major_sig_err, minor_sig_err = sx_err, sy_err
    else:
        major_pix, minor_pix, pa_rad = fwhm_y_pix, fwhm_x_pix, float(theta_fit) + np.pi / 2
        major_sig_err, minor_sig_err = sy_err, sx_err

    major_arcsec = major_pix * pixel_scale
    minor_arcsec = minor_pix * pixel_scale
    pa_deg = float(np.degrees(pa_rad) % 180.0)
    major_err_arcsec = (major_sig_err * 2.3548 * pixel_scale) if major_sig_err is not None else None
    minor_err_arcsec = (minor_sig_err * 2.3548 * pixel_scale) if minor_sig_err is not None else None
    pa_err_deg = float(np.degrees(theta_err)) if theta_err is not None else None

    # Integrated flux = amplitude × 2π × σx × σy  (Gaussian integral), in
    # (BUNIT)·pixel². For Jy/beam data this becomes Jy after dividing by the beam
    # area in pixels; otherwise no beam conversion is possible.
    integrated_raw = float(amp_fit * 2.0 * np.pi * abs(sx_fit) * abs(sy_fit))
    # Full-covariance error propagation for I = A·2π·σx·σy. Gradients (params
    # [amp, x0, y0, σx, σy, θ, offset]): dI/dA = I/A, dI/dσx = I/σx, dI/dσy = I/σy
    # (the sign of the |σ| derivative cancels into I/σ). Keeps the amp–width
    # covariance a sum-of-independent-terms would drop.
    integrated_raw_err = (
        covariance_propagate(
            pcov, (0, 3, 4),
            (integrated_raw / amp_fit if amp_fit else 0.0,
             integrated_raw / sx_fit if sx_fit else 0.0,
             integrated_raw / sy_fit if sy_fit else 0.0),
        )
        if integrated_raw else None
    )

    beam = dataset.beam_for_channel(resolved_channel)  # R-05: per-channel beam when present
    beam_area_px = beam_area_pixels(beam.major, beam.minor, pixel_scale, dataset.spatial_pixel_area_arcsec2())
    per_beam = is_per_beam_bunit(dataset.bunit)
    # ``integrated_flux`` is ALWAYS the raw Gaussian integral in
    # ``integrated_unit`` (BUNIT·pixel²); only ``integrated_flux_jy`` is Jy.
    integrated_unit = f"{dataset.bunit}*pixel^2" if dataset.bunit else "amplitude*pixel^2"
    if per_beam and beam_area_px:
        integrated_flux_jy: float | None = integrated_raw / beam_area_px
        integrated_flux_err_jy = (
            integrated_raw_err / beam_area_px if integrated_raw_err is not None else None
        )
    else:
        integrated_flux_jy = None
        integrated_flux_err_jy = None

    # Fit residual RMS
    residuals = sub_fit.ravel() - _gaussian_2d(
        (xx.ravel(), yy.ravel()), *popt)
    fit_rms = float(np.sqrt(np.mean(residuals ** 2)))

    # Beam deconvolution (optional) — PA-aware (Wild 1970).
    deconv_major: float | None = None
    deconv_minor: float | None = None
    deconv_pa_deg: float | None = None
    if beam.major is not None and beam.minor is not None:
        bm_maj_pix = (beam.major * 3600.0) / pixel_scale   # deg → arcsec → pix
        bm_min_pix = (beam.minor * 3600.0) / pixel_scale
        # Convert BPA (sky, E of N) into the fitter's pixel frame via the WCS so
        # it's consistent with the source's fitted PA (both CCW from +x).
        bm_pa_rad  = _beam_pa_in_pixel_frame(celestial, ra_deg, dec_deg, beam.pa or 0.0)
        dm, dn, dpa = _deconvolve_gaussian_beam(
            major_pix, minor_pix, pa_rad,
            bm_maj_pix, bm_min_pix, bm_pa_rad,
        )
        if dm is not None:
            deconv_major = dm * pixel_scale
            deconv_minor = dn * pixel_scale
            deconv_pa_deg = float(np.degrees(dpa) % 180.0)

    return {
        "peak_jy_beam": float(amp_fit),
        "peak_err": amp_err,
        # Physical integrated flux in Jy (None when not per-beam / beam unknown).
        "integrated_flux_jy": integrated_flux_jy,
        "integrated_flux_err_jy": integrated_flux_err_jy,
        # Raw Gaussian integral + honest unit, always available.
        "integrated_flux": integrated_raw,
        "integrated_flux_err": integrated_raw_err,
        "integrated_flux_unit": integrated_unit,
        "beam_area_pix": beam_area_px,
        "bunit": dataset.bunit,
        "major_arcsec": major_arcsec,
        "minor_arcsec": minor_arcsec,
        "major_err_arcsec": major_err_arcsec,
        "minor_err_arcsec": minor_err_arcsec,
        "pa_deg": pa_deg,
        "pa_err_deg": pa_err_deg,
        "deconvolved_major_arcsec": deconv_major,
        "deconvolved_minor_arcsec": deconv_minor,
        "deconvolved_pa_deg": deconv_pa_deg,
        "fit_rms": fit_rms,
        "center_pix_x": float(x0_fit),
        "center_pix_y": float(y0_fit),
        "background": float(bg_fit),
    }


# ── Region statistics (2D ROI mean/rms/sum/flux) ─────────────────────────────
