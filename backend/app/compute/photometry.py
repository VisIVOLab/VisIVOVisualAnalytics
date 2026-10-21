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


def _require_position_inside(
    cx: float, cy: float, shape: tuple[int, int], ra_deg: float, dec_deg: float
) -> None:
    """Reject a sky position that does not land on the image.

    Both workers index the image with the pixel coordinates derived here, and a
    NEGATIVE coordinate is not caught by clamping the far edge: ``image[y0:y1,
    0:-1393]`` is a valid slice counting back from the right, so a position off
    the left edge silently measured a strip on the OPPOSITE side of the image,
    and a slightly different offset instead surfaced as numpy's own "negative
    dimensions are not allowed". Mosaic cutouts make this routine — their CRPIX
    often sits thousands of pixels outside the extracted footprint.
    """
    ny, nx = shape
    if not (np.isfinite(cx) and np.isfinite(cy)):
        raise ValueError(
            f"Sky position ({ra_deg}, {dec_deg}) deg does not map to a finite "
            "pixel coordinate in this image's WCS."
        )
    # A pixel of index i covers [i - 0.5, i + 0.5], so the image spans
    # [-0.5, n - 0.5) and the CENTRE of the corner pixel is a valid position.
    # Testing 0 <= c instead rejected it: a round trip through the WCS puts the
    # corner centre at -1.5e-11 rather than exactly 0, and the refusal then
    # quoted "pixel (-0.0, -0.0)".
    if not (-0.5 <= cx < nx - 0.5 and -0.5 <= cy < ny - 0.5):
        raise ValueError(
            f"Sky position ({ra_deg}, {dec_deg}) deg falls outside the image: it "
            f"maps to pixel ({cx:.1f}, {cy:.1f}) and the image is {nx} x {ny}."
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
        raise ValueError("WCS not available for this dataset, cannot convert sky → pixel.")

    pixel_scale_arcsec = dataset.spatial_pixel_scale_arcsec()
    if pixel_scale_arcsec <= 0:
        raise ValueError("Pixel scale is zero or negative.")

    # Convert sky → pixel (0-indexed, row=y, col=x)
    celestial = dataset.wcs.celestial
    if celestial is None:
        raise ValueError("No celestial WCS axes found in this dataset.")
    pix = celestial.all_world2pix([[ra_deg, dec_deg]], 0)[0]
    pix_x, pix_y = float(pix[0]), float(pix[1])
    _require_position_inside(pix_x, pix_y, image.shape, ra_deg, dec_deg)

    r_ap   = aperture_arcsec   / pixel_scale_arcsec
    r_ann1 = annulus_inner_arcsec / pixel_scale_arcsec
    r_ann2 = annulus_outer_arcsec / pixel_scale_arcsec

    position = (pix_x, pix_y)
    aperture = CircularAperture(position, r=r_ap)

    # The annulus is OPTIONAL (both radii default to zero = no local background).
    # Handing photutils r_in == r_out == 0 raised its own ValueError, which the
    # router turned into a 422 reading "'r_out' must be greater than 'r_in'" —
    # an error for a request the API documents as valid. No annulus now means no
    # background subtraction, and the flux error falls back to the aperture's own
    # pixel scatter so the uncertainty is still reported rather than zero.
    # "No annulus" is the both-radii-zero case only. A half-specified or
    # inverted annulus is a mistake in the request, so it is reported instead of
    # being quietly downgraded to "no background".
    if annulus_inner_arcsec or annulus_outer_arcsec:
        if annulus_inner_arcsec <= 0 or annulus_outer_arcsec <= annulus_inner_arcsec:
            raise ValueError(
                "Background annulus must have annulus_outer_arcsec > "
                "annulus_inner_arcsec > 0 (got inner="
                f"{annulus_inner_arcsec} arcsec, outer={annulus_outer_arcsec} arcsec). "
                "Leave both at 0 for no background subtraction."
            )
        use_annulus = True
    else:
        use_annulus = False
    if use_annulus:
        annulus = CircularAnnulus(position, r_in=r_ann1, r_out=r_ann2)
        # Background: median inside annulus
        ann_stats = ApertureStats(image, annulus, sigma_clip=None)
        bkg_per_pix = float(ann_stats.median) if np.isfinite(ann_stats.median) else 0.0
        # Local noise per pixel from the background annulus (used for the flux error).
        bkg_rms = float(ann_stats.std) if np.isfinite(ann_stats.std) else 0.0
    else:
        bkg_per_pix = 0.0
        # No annulus means no background measurement, and therefore no noise
        # estimate. The aperture's OWN scatter is not a substitute: it is
        # dominated by the source, so on a noiseless Gaussian it reported an
        # uncertainty that scaled with the peak (4.3 at amplitude 1, 427 at
        # amplitude 100). "Unknown" is the honest answer.
        bkg_rms = 0.0

    # Aperture sum
    ap_stats = ApertureStats(image, aperture, sigma_clip=None)
    # Fractional geometric overlap area, falling back to the analytic circle when
    # photutils reports a non-finite one (an aperture hanging off the edge) —
    # int(nan) raises, and it used to raise all the way out as a 422 reading
    # "cannot convert float NaN to integer".
    _area = ap_stats.sum_aper_area
    _area = float(_area.value) if hasattr(_area, "value") else float(_area)
    if not np.isfinite(_area):
        _area = float(np.pi * r_ap ** 2)
    # The background is removed over the FRACTIONAL area. Truncating to whole
    # pixels first biased the result by up to one pixel of background: on a
    # constant image of 10 with an 8.65-pixel overlap the source flux came out
    # 6.47 instead of 0.
    n_pix = int(_area)
    # A non-finite aperture sum means blanked/NaN pixels fall inside the aperture.
    # Report it as unavailable (honest) rather than substituting the WHOLE-IMAGE
    # sum, which would be an order-of-magnitude-wrong aperture flux. It is None
    # rather than NaN because NaN is not representable in JSON and made the
    # endpoint answer 500.
    raw_sum = float(ap_stats.sum) if np.isfinite(ap_stats.sum) else None
    # Background-subtracted summed brightness. For Jy/beam data this is in
    # (Jy/beam)·pixel and must be divided by the beam area to become Jy; for
    # other BUNIT it is a plain sum in BUNIT·pixel.
    flux_summed = None if raw_sum is None else raw_sum - bkg_per_pix * _area

    # R-01: beam-correct to Jy only when the data are per-beam AND the beam is
    # known. ``flux_jy`` is the physically-meaningful flux (Jy) or None; ``flux``
    # is ALWAYS the raw summed quantity in ``flux_unit`` (BUNIT·pixel) — never
    # relabelled Jy. Previously the raw sum was returned mislabeled as ``flux_jy``
    # (off by the beam area, 10–50×).
    # R-05: use the per-channel beam when the cube carries a BEAMS table.
    beam = dataset.beam_for_channel(resolved_channel)
    beam_area_px = beam_area_pixels(beam.major, beam.minor, pixel_scale_arcsec, dataset.spatial_pixel_area_arcsec2())
    per_beam = is_per_beam_bunit(dataset.bunit)
    flux_jy: float | None = (
        (flux_summed / beam_area_px)
        if (flux_summed is not None and per_beam and beam_area_px) else None)
    flux_unit = f"{dataset.bunit}*pixel" if dataset.bunit else "pixel_sum"

    # R-09: correlated-noise uncertainty. Pixels within a beam are not
    # independent (n_beam pixels per beam). The error on the raw summed
    # brightness is rms·sqrt(n_pix·n_beam); the physical flux error in Jy is that
    # divided by the beam area, i.e. rms·sqrt(n_pix/n_beam). When the beam is
    # unknown we fall back to the independent-pixel form (n_beam = 1) for the raw
    # sum error and leave flux_err_jy = None. This replaces the old
    # counts/Poisson formula, which is dimensionless-wrong for Jy/beam.
    # With no annulus there is no bkg_rms, so the uncertainty is unknown (None)
    # rather than 0.0 — a zero error on an unmeasured background reads as a
    # precise answer.
    n_beam = beam_area_px if beam_area_px else 1.0
    err_summed: float | None = (
        float(bkg_rms * np.sqrt(max(_area, 0.0) * n_beam)) if bkg_rms > 0 else None)
    flux_err_jy = ((err_summed / beam_area_px)
                   if (err_summed is not None and per_beam and beam_area_px) else None)

    return {
        # Physical flux in Jy (None when not per-beam data or beam unknown).
        "flux_jy": flux_jy,
        "flux_err_jy": flux_err_jy,
        # Raw background-subtracted sum + its honest unit. None when blanked
        # pixels made the aperture sum unavailable.
        "flux": flux_summed,
        "flux_err": err_summed,
        "flux_unit": flux_unit,
        "beam_area_pix": beam_area_px,
        "background_per_pix": bkg_per_pix,
        "background_rms_per_pix": bkg_rms,
        "bunit": dataset.bunit,
        "n_pix_aperture": n_pix,
        # The fractional overlap area actually used for the background
        # subtraction; n_pix_aperture is its truncated whole-pixel count.
        "aperture_area_pix": _area,
        "channel": channel,
        "pix_x": pix_x,
        "pix_y": pix_y,
        "pixel_scale_arcsec": pixel_scale_arcsec,
    }


# ── A-02: 2-D Gaussian source fitting + beam deconvolution ───────────────────


def empirical_fit_errors(image, model, p0, fitter, rng, *,
                         source_sigma_px: float = 2.0,
                         centre_xy: tuple[float, float] | None = None,
                         n_trials: int = 60, min_trials: int = 20,
                         detect_sigma: float = 4.0):
    """1-sigma parameter errors for a 2-D fit, measured on the image's own noise.

    ``curve_fit``'s covariance assumes independent pixels. Interferometric images
    violate that badly — noise is correlated across the beam — and the error is
    large: Monte Carlo on beam-correlated noise gives reported errors optimistic
    by 3.3x at a 2.5 px beam FWHM, 4.8x at 4 px and 5.8x at 6 px (worse for the
    integrated flux, which compounds through the amp-sigma_x-sigma_y covariance).

    Two analytic corrections were measured and rejected before this one. Scaling
    by the beam area predicts 2.66 / 4.26 / 6.39 against those measurements, and
    summing the residual autocorrelation over the lag plane over-corrects and even
    inflates WHITE noise by 1.4x, because thresholded lags accumulate estimator
    scatter. So the error is measured instead:

      inject the FITTED model at source-free positions near the source, refit,
      and take the scatter of the recovered parameters.

    It needs no covariance model at all: the noise realisations are the actual
    data. Validated against the true scatter at 0.88-1.06 for correlated noise.

    **Source-free is decided by a matched filter**, not by counting bright pixels.
    A fraction-of-bright-pixels gate has to be calibrated against a source shape,
    and compact ones slip under it: a FWHM-4 px source at peak SNR 20 puts only
    ~2 % of a 41x41 window beyond 3 sigma, below any threshold that pure noise
    (which reaches ~1.8 %) can also pass. Smoothing by the source's own scale is
    the shape-matched test — a Gaussian smoothing IS the matched filter for a
    Gaussian source — and it is thresholded against the smoothed map's own MAD, so
    it self-calibrates for however correlated the noise is.

    Its residual limit, measured: a FWHM-4 px source at peak SNR 8 sitting in
    noise correlated on the same 4 px scale is NOT flagged — at that scale such a
    source and a noise bump carry comparable flux, so no window statistic can
    separate them. The consequence is a placement that inflates the scatter, i.e.
    an error bar too large. That is the safe direction; the bug being fixed here
    was error bars 3-8x too small.

    The search is confined to a neighbourhood of the source rather than the whole
    plane. That bounds memory on a large image, and it samples the noise where the
    source actually is, which is the assumption the estimate rests on anyway.

    @param image   the full 2-D plane to draw noise from.
    @param model   the fitted model evaluated on the fitting window (2-D array).
    @param p0      initial guess for the refits, in the fitter's own coordinates.
    @param fitter  ``f(window_data) -> params | None``; None = a refit to discard.
    @param rng     seeded generator, so a repeated fit reports the same error.
    @param source_sigma_px  fitted source scale; sets the matched filter.
    @param centre_xy  (x, y) of the source, so the search stays near it.
    @returns ``(covariance_matrix_or_None, n_accepted)`` — the FULL empirical
             parameter covariance, not just its diagonal: the integrated flux is
             propagated through the amp-sigma_x-sigma_y off-diagonals, and those
             are exactly where correlated noise does its worst. None when too few
             clean positions exist (a crowded field), which the caller must report
             rather than silently fall back to the optimistic covariance.
    """
    from scipy import ndimage  # noqa: PLC0415

    h, w = model.shape
    ny, nx = image.shape
    if h > ny or w > nx:
        return None, 0

    # Search neighbourhood: big enough for many independent placements, bounded so
    # the smoothed map below never scales with a survey-sized plane.
    reach = max(6 * max(h, w), 256)
    cx, cy = (centre_xy if centre_xy is not None else (nx / 2.0, ny / 2.0))
    ry0 = max(0, int(cy) - reach); ry1 = min(ny, int(cy) + reach)
    rx0 = max(0, int(cx) - reach); rx1 = min(nx, int(cx) + reach)
    region = np.asarray(image[ry0:ry1, rx0:rx1], dtype=np.float32)
    rh, rw = region.shape
    if rh < h or rw < w:
        return None, 0

    finite = region[np.isfinite(region)]
    if finite.size < 100:
        return None, 0
    med = float(np.median(finite))
    rms = float(1.4826 * np.median(np.abs(finite - med)))
    if not np.isfinite(rms) or rms <= 0.0:
        return None, 0

    # Matched filter: smoothing by the source's own scale, thresholded against the
    # SMOOTHED map's noise. NaN is filled with the background first so a blank
    # region does not smear a hole across the detection map; those pixels are
    # excluded from placements separately.
    filled = np.where(np.isfinite(region), region - med, 0.0)
    smooth = ndimage.gaussian_filter(filled, max(float(source_sigma_px), 0.5),
                                     mode="nearest")
    smooth_rms = float(1.4826 * np.median(np.abs(smooth - np.median(smooth))))
    if not np.isfinite(smooth_rms) or smooth_rms <= 0.0:
        return None, 0
    contaminated = np.abs(smooth) > detect_sigma * smooth_rms
    contaminated |= ~np.isfinite(region)

    rows = []
    for _ in range(n_trials * 12):
        if len(rows) >= n_trials:
            break
        oy = int(rng.integers(0, rh - h + 1))
        ox = int(rng.integers(0, rw - w + 1))
        if contaminated[oy:oy + h, ox:ox + w].any():
            continue
        params = fitter(model + region[oy:oy + h, ox:ox + w] - med)
        if params is None:
            continue
        rows.append(params)
    if len(rows) < min_trials:
        return None, len(rows)
    samples = np.asarray(rows, dtype=float)
    if samples.shape[0] <= samples.shape[1]:
        # Fewer realisations than parameters gives a singular covariance that
        # would propagate as a spuriously small error. min_trials normally
        # prevents this; the guard is here because silently returning it would be
        # worse than returning nothing.
        return None, len(rows)
    return np.cov(samples, rowvar=False), len(rows)


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
    _require_position_inside(cx, cy, image.shape, ra_deg, dec_deg)

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

    # R-24: those covariance errors assume independent pixels, which an
    # interferometric image is not — they come out 3–8x too small on beam-scale
    # correlated noise. Measure them instead, by injecting THIS fitted model into
    # source-free parts of the same image and refitting.
    model_window = _gaussian_2d((xx.ravel(), yy.ravel()), *popt).reshape(sub_fit.shape)
    def _refit(window: np.ndarray):
        try:
            q, _ = curve_fit(_gaussian_2d, (xx.ravel(), yy.ravel()), window.ravel(),
                             p0=list(popt), maxfev=5000)
        except Exception:  # noqa: BLE001 – a failed refit is simply not a sample
            return None
        # A refit that ran away is not a noise realisation of this source, and one
        # bad sample dominates a covariance. Guard BOTH widths, the amplitude and
        # the centre — checking sigma_x alone let a blown-up sigma_y through, and
        # the integrated flux is the product of the two.
        amp_ref = abs(amp_fit) or 1.0
        sx_ref, sy_ref = abs(sx_fit) or 1.0, abs(sy_fit) or 1.0
        if abs(q[0]) > 5.0 * amp_ref:
            return None
        if not (0.1 * sx_ref < abs(q[3]) < 10.0 * sx_ref):
            return None
        if not (0.1 * sy_ref < abs(q[4]) < 10.0 * sy_ref):
            return None
        # The refit must still be describing the injected source, not drifting off
        # to some noise peak elsewhere in the window.
        if abs(q[1] - x0_fit) > 0.5 * sub_fit.shape[1] or abs(q[2] - y0_fit) > 0.5 * sub_fit.shape[0]:
            return None
        return q
    # Seed derived from WHERE the fit is. Reproducibility is the property this
    # buys and the one the tests pin: refitting a source must not move its error
    # bar. It also decorrelates sources whose search neighbourhoods overlap, which
    # a global constant would not — but that is a small effect and is NOT
    # demonstrated by any test here, because each search already runs in its own
    # neighbourhood, so distinct sources differ either way.
    seed = (int(round(cx)) * 73856093) ^ (int(round(cy)) * 19349663) ^ int(half_box)
    emp_cov, n_emp = empirical_fit_errors(
        image, model_window, list(popt), _refit,
        np.random.default_rng(abs(seed) % (2**32)),
        source_sigma_px=max(abs(sx_fit), abs(sy_fit)),
        centre_xy=(x0_fit, y0_fit))
    errors_are_empirical = emp_cov is not None
    if not errors_are_empirical:
        # No clean placements — a crowded field. The curve_fit covariance is known
        # to be 3-8x optimistic on correlated data, so it is NOT reported under the
        # science-facing error keys; those become None and the caller sees
        # error_method="covariance". The optimistic numbers stay available under
        # explicitly named diagnostic keys for anyone who wants them.
        covariance_only_err = list(perr)
        perr = [None] * len(perr)
    else:
        covariance_only_err = None
    if errors_are_empirical:
        # Replace the WHOLE covariance, so the propagated integrated flux inherits
        # the empirical off-diagonals too, not just the diagonal errors.
        pcov = np.asarray(emp_cov, dtype=float)
        with np.errstate(invalid="ignore"):
            diag = np.sqrt(np.diag(pcov))
        perr = [float(e) if np.isfinite(e) else None for e in diag]

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
    # In the fallback the propagation source (pcov) is the optimistic covariance,
    # so its product must go the same way as the diagonal errors: into the
    # diagnostic bundle, not into the reported uncertainty.
    integrated_raw_err = (
        covariance_propagate(
            pcov, (0, 3, 4),
            (integrated_raw / amp_fit if amp_fit else 0.0,
             integrated_raw / sx_fit if sx_fit else 0.0,
             integrated_raw / sy_fit if sy_fit else 0.0),
        )
        if integrated_raw else None
    )
    if not errors_are_empirical:
        diagnostic_integrated_err = integrated_raw_err
        integrated_raw_err = None
    else:
        diagnostic_integrated_err = None

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
        # How the error bars above were obtained. "empirical" = measured by
        # injecting this model into source-free parts of the image and refitting
        # (correct for beam-correlated noise); "covariance" = the curve_fit
        # covariance, which assumes independent pixels and is optimistic by 3–8x
        # on interferometric data — the fallback when the field is too crowded to
        # find clean positions. A caller quoting the errors should say which.
        "error_method": "empirical" if errors_are_empirical else "covariance",
        "error_trials": int(n_emp),
        # Present only in the fallback: the curve_fit diagonal, named so it cannot
        # be mistaken for a calibrated uncertainty.
        "diagnostic_covariance_err": covariance_only_err,
        "diagnostic_covariance_integrated_err": diagnostic_integrated_err,
        "center_pix_x": float(x0_fit),
        "center_pix_y": float(y0_fit),
        "background": float(bg_fit),
    }


# ── Region statistics (2D ROI mean/rms/sum/flux) ─────────────────────────────
