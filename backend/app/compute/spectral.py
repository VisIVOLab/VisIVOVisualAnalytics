from __future__ import annotations

import base64
import concurrent.futures as _cf  # noqa: F401
import logging  # noqa: F401
import re
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
    clamped_sqrt,
    logger,
    _qt_zlib_compress,
    _b64f32,
    _b64i32,
    _b64f32_compressed,
    _b64i32_compressed,
    _finite_range,
    _integrated_spacing,
    frequency_width_to_kms,
    covariance_propagate,
)


def effective_sample_factor(residuals: np.ndarray, max_lag: int = 5) -> float:
    """Variance inflation for noise correlated along the spectral axis.

    ``curve_fit`` assumes independent samples. When adjacent channels are
    correlated — Hanning smoothing, any spectral taper, or a cube resampled onto a
    finer grid than its true resolution — N points carry only about N/F
    independent ones, with ``F = 1 + 2·Σ_k ρ_k`` over the positive lags. The
    parameter covariance is then too small by that factor, and every error derived
    from it too small by √F.

    ρ_k is measured from the fit RESIDUALS, so no extra input is needed and a cube
    that is genuinely uncorrelated gets F = 1 and is unaffected. Validated by Monte
    Carlo (see tests/test_line_fit_correlated_noise.py): over smoothing kernels
    giving ρ₁ = 0 … 0.96, the uncorrected errors are optimistic by 1.0×, 1.7×,
    2.3× and 3.0×, while √F predicts 1.00, 1.64, 2.25 and 2.72 — and F recovered
    from residuals lands within ~7 % of the true value in √ terms, always on the
    conservative-to-slightly-under side.

    The sum stops at the first negligible or non-positive lag (ρ ≤ 0.02, or
    ``max_lag``) so estimator scatter is not accumulated as if it were signal.
    """
    r = np.asarray(residuals, dtype=np.float64)
    r = r - r.mean()
    denom = float(np.dot(r, r))
    if r.size < 8 or denom <= 0.0:
        return 1.0
    total = 1.0
    for lag in range(1, min(max_lag, r.size - 1) + 1):
        rho = float(np.dot(r[:-lag], r[lag:])) / denom
        if not np.isfinite(rho) or rho <= 0.02:
            break
        total += 2.0 * rho
    return max(total, 1.0)


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
    rest_freq_hz: float | None = None,
    fast: bool = False,
) -> dict[str, Any]:
    """Compute per-pixel FWHM (Gaussian fit) and equivalent-width maps.

    FWHM  = 2.3548 × σ_gaussian  (spectral units)
    EW    = M0 / peak_brightness  (spectral units)

    R-02: for a frequency axis, when a rest frequency is available (the
    ``rest_freq_hz`` argument or the header RESTFRQ/RESTFREQ) the FWHM and EW
    widths are converted from Hz to km/s (|Δv| = c·|Δν|/ν₀) and the returned
    ``fwhm_unit``/``ew_unit`` become ``km/s``. Without a rest frequency the
    widths stay in the native axis unit (e.g. Hz) and are labelled as such —
    never silently reported as km/s.

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

    if fast:
        # R-07: SKA-scale vectorised FWHM — the intensity-weighted velocity
        # dispersion (moment 2), FWHM = 2.3548·σ, computed over the whole chunk
        # at once instead of one scipy curve_fit per pixel (O(cube) vs O(npix)
        # fits). Less exact than the Gaussian fit for a non-Gaussian profile, but
        # orders of magnitude faster on dense bright cubes.
        sv3 = spectral_values[:, None, None]
        with np.errstate(invalid="ignore", divide="ignore"):
            nz_m0 = m0_chunk != 0.0
            m1 = np.where(nz_m0, np.nansum(safe * weights * sv3, axis=0) / m0_chunk, np.nan)
            diff = sv3 - m1[None, :, :]
            m2 = np.where(nz_m0,
                          np.nansum(safe * weights * diff * diff, axis=0) / m0_chunk, np.nan)
            fwhm_full = (2.3548 * clamped_sqrt(m2)).astype(np.float32)
            ew_full = np.where(np.isfinite(m0_chunk) & (peak_chunk > 0.0),
                               m0_chunk / peak_chunk, np.nan).astype(np.float32)
        # Blank FWHM below the SNR cutoff (the fit path skips those pixels), and
        # where the weighted M0 ≤ 0 (absorption / mixed-sign noise) — there the
        # dispersion is undefined and max(m2,0) would spuriously report FWHM=0.
        # Keep EW everywhere (cheap, no fit) to match the per-pixel path.
        fwhm_full[(snr_chunk < snr_cutoff) | ~(m0_chunk > 0.0)] = np.nan
        fwhm_map[row_lo:row_hi, :] = fwhm_full
        ew_map[row_lo:row_hi, :] = ew_full
        cy = 0  # skip the per-pixel loop below

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

    # R-02: convert Hz widths to km/s when a rest frequency is known. The rest
    # frequency comes from the caller (rest_freq_hz) or the FITS header.
    hdr = dataset.header
    rf = rest_freq_hz
    if rf is None:
        for key in ("RESTFRQ", "RESTFREQ"):
            if key in hdr:
                try:
                    rf = float(hdr[key])
                    break
                except (TypeError, ValueError):
                    pass
    conv_factor, spectral_unit = frequency_width_to_kms(
        str(spectral.get("axis_type", "")), str(spectral["axis_unit"]), rf
    )
    if conv_factor != 1.0:
        np.multiply(fwhm_map, np.float32(conv_factor), out=fwhm_map,
                    where=np.isfinite(fwhm_map))
        np.multiply(ew_map, np.float32(conv_factor), out=ew_map,
                    where=np.isfinite(ew_map))

    fwhm_range = _finite_range(fwhm_map)
    ew_range = _finite_range(ew_map)
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


def _fit_poly_baseline(
    x_fit: "np.ndarray",
    x_all: "np.ndarray",
    block: "np.ndarray",
    deg: int,
) -> "np.ndarray":
    """Per-pixel polynomial baseline, evaluated on every channel. NaN-safe.

    ``block`` is (n_fit_channels, npix) — the line-free channels of the flattened
    cube; the return is (len(x_all), npix).

    ``np.linalg.lstsq`` cannot be used directly on cube data: it has no notion of
    a masked sample, so ANY column carrying a non-finite value comes back with
    NaN coefficients. One fully flagged channel — routine in real
    interferometric data — therefore puts a NaN in every column and blanks the
    entire cube, which was reported to the user as an ill-conditioned fit. (The
    columns do not contaminate each other; each is independently ruined.)

    So the samples are masked per pixel instead, and columns sharing a mask are
    fitted together. Cheapest path first, with pixels that have no finite sample
    at all set aside as blank before the choice is made:

    * all samples finite → one shared-SVD ``lstsq``, exactly as before;
    * flagging is per-CHANNEL (whole channels flagged, so every surviving pixel
      has the same gaps — the common real case, once the blanked footprint is
      excluded) → drop those channels, one reduced shared-SVD ``lstsq``;
    * otherwise (per-pixel flagging, e.g. a mask cube) → group the columns by
      identical finite-sample pattern and take one shared-SVD ``lstsq`` per
      group. The loop runs over PATTERNS, not pixels, and each iteration is
      vectorised across that pattern's columns.

    Every path therefore solves the least-squares problem through an SVD of the
    masked design matrix. Forming the normal equations (VᵀWV)c = VᵀWy instead
    would vectorise across all pixels at once, but it squares the condition
    number: at degree 10 a fully finite pixel came out with a 3e-2 baseline
    error against 2e-8 from the direct solve, and — because a pseudo-inverse
    silently truncates the lost rank — its answer depended on whether some OTHER
    pixel happened to be flagged.

    Columns left with fewer than ``deg + 2`` finite samples cannot over-determine
    the polynomial, so they come out NaN — blank, rather than fitted against a
    fabricated baseline.
    """
    # Map the channel indices onto [-1, 1] before building the Vandermonde. In
    # absolute channel index a degree-10 term on a 1000-channel cube spans ~1e30,
    # which is unconditioned before any of the algebra below starts; the fitted
    # polynomial SPACE is identical either way, and the coefficients themselves
    # are never returned (only V_all @ coeffs).
    #
    # The scale comes from the FIT range, not the full axis: line-free channels
    # often occupy one end of the cube, and normalising by the whole axis left
    # the fitted interval as a sub-interval of [-1, 1] — half of it in the case
    # above — which is exactly the conditioning the normalisation is here to fix.
    lo, hi = float(np.min(x_fit)), float(np.max(x_fit))
    half = (hi - lo) / 2.0 or 1.0
    mid = (hi + lo) / 2.0
    v_fit = np.vander((x_fit - mid) / half, N=deg + 1, increasing=True)
    v_all = np.vander((x_all - mid) / half, N=deg + 1, increasing=True)

    finite = np.isfinite(block)
    if finite.all():
        coeffs, *_ = np.linalg.lstsq(v_fit, block, rcond=None)
        return v_all @ coeffs

    # Allocated only now: on the all-finite path above it would be a second
    # full-size cube held alive while `v_all @ coeffs` allocates the real one —
    # 2 GB of nothing for 1000 channels x 250k pixels.
    baseline = np.full((len(x_all), block.shape[1]), np.nan)

    # Pixels with NO finite sample at all — the blanked footprint outside an
    # interferometric primary beam — are starved whatever happens, so they are
    # set aside FIRST and left NaN. Doing that before classifying the flagging
    # pattern is what makes the cheap path below reachable on real data: those
    # dead pixels put a NaN in every row, so without this step a cube with a
    # blanked edge (i.e. most of them) never looks per-channel and every pixel
    # goes through the grouped path.
    live = finite.any(axis=0)
    if not live.any():
        return baseline
    live_cols = np.flatnonzero(live)
    finite_live = finite[:, live_cols]

    # Per-channel flagging: among the live pixels a row of the fit block is
    # either wholly good or wholly bad, so ONE reduced system serves them all.
    good_rows = finite_live.all(axis=1)
    if bool((good_rows | ~finite_live.any(axis=1)).all()):
        if int(good_rows.sum()) < deg + 2:
            return baseline
        coeffs, *_ = np.linalg.lstsq(
            v_fit[good_rows], block[np.ix_(good_rows, live_cols)], rcond=None)
        baseline[:, live_cols] = v_all @ coeffs
        return baseline

    # General per-pixel flagging. Columns are bucketed by their finite-sample
    # pattern (packed to bytes so the comparison is one row per 8 channels), then
    # visited in sorted order so each bucket is a contiguous run — O(npix log
    # npix) once, rather than a scan of every column per bucket.
    groups = np.unique(np.packbits(finite_live, axis=0), axis=1,
                       return_inverse=True)[1].ravel()
    order = np.argsort(groups, kind="stable")
    bounds = np.concatenate(([0], np.flatnonzero(np.diff(groups[order])) + 1,
                             [len(order)]))
    logger.info("[BASELINE] per-pixel flagging: %d distinct patterns over %d pixels",
                len(bounds) - 1, len(live_cols))
    for begin, stop in zip(bounds[:-1], bounds[1:], strict=True):
        cols = live_cols[order[begin:stop]]
        mask = finite[:, cols[0]]
        if int(mask.sum()) < deg + 2:
            continue          # starved: those columns stay NaN
        coeffs, *_ = np.linalg.lstsq(
            v_fit[mask], block[np.ix_(mask, cols)], rcond=None)
        baseline[:, cols] = v_all @ coeffs
    return baseline


# How many times noisier the fitted baseline may be, at the worst channel, than
# it is where it was actually fitted, before the user is told. Calibrated against
# measured residuals: a one-sided selection over half a 1000-channel cube gives
# 84x at degree 3 (residual RMS ~= the input noise, fine) and 436x at degree 4
# (residual 5x the noise), so the boundary sits between them.
_BASELINE_NOISE_INFLATION_LIMIT = 100.0


# Keywords that carry a 1-based axis index, and the two matrix forms. Used to
# drop the WCS description of axes the output no longer has.
_AXIS_KEYWORD = re.compile(
    r"^(NAXIS|CTYPE|CRVAL|CRPIX|CDELT|CUNIT|CROTA|CNAME|CRDER|CSYER|CSPEC|PS|PV)"
    r"(\d+)(?:_\d+)?[A-Z]?$")
_AXIS_MATRIX_KEYWORD = re.compile(r"^(PC|CD)(\d+)_(\d+)[A-Z]?$")
# The pre-1999 fixed-width matrix form, still legal: PC004004, CD001001.
_AXIS_MATRIX_LEGACY = re.compile(r"^(PC|CD)(\d{3})(\d{3})$")


def _drop_surplus_axis_keywords(header, keep_naxis: int) -> None:
    """Strip the WCS keywords of axes beyond ``keep_naxis``, in place.

    A cube stored as 4-D with a degenerate Stokes axis (NAXIS4 = 1, CTYPE4 =
    'STOKES') is written back as 3-D, and astropy rewrites NAXIS/NAXIS1..3 from
    the data — but everything ELSE about the fourth axis survived, so the output
    header said NAXIS = 3 while still describing a CRPIX4/CDELT4/CRVAL4/CTYPE4
    that no longer exists (user report). That is an internally inconsistent
    header: readers that trust NAXIS ignore it, readers that scan for CTYPEn
    find a Stokes axis in a 3-D cube.

    ``BLANK`` goes too when the data are floating point: FITS defines it only
    for integer arrays, where it names the value standing in for undefined
    pixels. Carried onto float data (whose blanks are NaN) it is at best ignored
    and at worst applied, turning every -1 into a blank.
    """
    for key in list(header.keys()):
        if not key:
            continue
        name = key.upper()
        m = _AXIS_KEYWORD.match(name)
        if m and int(m.group(2)) > keep_naxis:
            del header[key]
            continue
        m = _AXIS_MATRIX_KEYWORD.match(name) or _AXIS_MATRIX_LEGACY.match(name)
        if m and (int(m.group(2)) > keep_naxis or int(m.group(3)) > keep_naxis):
            del header[key]
            continue
        # WCSAXES counts the axes the WCS describes, so leaving it at 4 keeps
        # astropy reading a 4-axis WCS off a 3-D image and warning about the
        # mismatch, however many CTYPEn are gone.
        if name == "WCSAXES" or (name.startswith("WCSAXES") and len(name) == 8):
            try:
                if int(header[key]) > keep_naxis:
                    header[key] = keep_naxis
            except (TypeError, ValueError):
                del header[key]
    if header.get("BITPIX", 0) < 0 and "BLANK" in header:
        del header["BLANK"]
    # Extrema of the INPUT, meaningless once a baseline has been subtracted:
    # the tested output ranged -4.16..4.14 while the copied cards still said
    # -5.72..54.19.
    for stale in ("DATAMIN", "DATAMAX"):
        if stale in header:
            del header[stale]

def _repair_celestial_units(dataset, header) -> None:
    """Write back the celestial-unit repair the reader already does in memory.

    ScientificFitsDataset overrides a celestial CUNITn that is not an angular
    unit so it can build a WCS at all (wcs_status='sanitized'), but that repair
    lived only in the loaded object: products copied the ORIGINAL header, so a
    cube whose CUNIT1/2 say 'km/s' on the RA/Dec axes produced an output that
    astropy refuses outright, with InvalidTransformError "In CUNIT1: Mismatched
    units type 'plane angle': have 'km/s', want 'deg'". A derived
    product no WCS library can read is not a product.
    """
    for axis in (1, 2):
        key = f"CUNIT{axis}"
        if key not in header:
            continue
        ctype = str(header.get(f"CTYPE{axis}", "")).strip()
        unit = str(header.get(key, "")).strip()
        if dataset._is_celestial_ctype(ctype) and not dataset._is_angular_unit(unit):
            header[key] = "deg"
            header.add_history(
                f"VisIVO: CUNIT{axis} was {unit!r} on a celestial axis; corrected to 'deg'.")


def _baseline_conditioning_warning(
    x_fit: "np.ndarray", x_all: "np.ndarray", deg: int
) -> str:
    """Warn when the fitted baseline is poorly constrained where it is SUBTRACTED.

    The fit is pinned only where the line-free channels are. Subtracting it
    everywhere else is extrapolation, and a polynomial's prediction error grows
    without bound outside the fitted interval — nothing in the algebra can
    prevent that, because it is the model the caller asked for. Fitting degree
    10 over channels 0-99 of a 1000-channel cube injected a false signal of ~4
    into a cube whose noise is 0.0026, silently.

    The measure is the LEVERAGE of the fit, h(x) = v(x)ᵀ(VᵀV)⁻¹v(x), which is
    how much the fit amplifies noise at channel x; the reported figure is
    sqrt(max h over the subtracted channels / mean h over the fitted ones), i.e.
    how many times noisier the baseline is at its worst channel than where it
    was fitted. That is the quantity the user is exposed to, and unlike a
    monomial-growth proxy it is degree-aware for the right reason: on the same
    geometry it separates a harmless degree-3 fit (84x) from a degree-4 one that
    measurably degrades the cube (436x), which ``reach ** deg`` did not.
    """
    if deg < 1 or len(x_fit) < deg + 2 or len(x_all) == 0:
        return ""
    lo, hi = float(np.min(x_fit)), float(np.max(x_fit))
    half = (hi - lo) / 2.0
    if half <= 0.0:
        return ""
    mid = (hi + lo) / 2.0
    v_fit = np.vander((x_fit - mid) / half, N=deg + 1, increasing=True)
    v_all = np.vander((x_all - mid) / half, N=deg + 1, increasing=True)
    gram_inv = np.linalg.pinv(v_fit.T @ v_fit)
    h_all = np.einsum("ik,kl,il->i", v_all, gram_inv, v_all, optimize=True)
    h_fit = np.einsum("ik,kl,il->i", v_fit, gram_inv, v_fit, optimize=True)
    mean_fit = float(np.mean(h_fit))
    if not (mean_fit > 0) or not np.isfinite(h_all).any():
        return ""
    inflation = float(np.sqrt(np.max(h_all) / mean_fit))
    if not np.isfinite(inflation) or inflation < _BASELINE_NOISE_INFLATION_LIMIT:
        return ""
    worst = int(x_all[int(np.argmax(h_all))])
    return (
        f"The degree-{deg} baseline is only constrained by channels "
        f"{int(lo)}-{int(hi)}; where it is subtracted furthest from those it is "
        f"~{inflation:.3g}x more uncertain than where it was fitted (worst at "
        f"channel {worst}), so those channels can carry a large false signal. "
        "Put line-free ranges at BOTH ends of the range you subtract over, or "
        "lower the polynomial order."
    )


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
        header = dataset.header.copy()
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
        # Median of line-free channels per pixel → constant baseline.
        # nanmedian ignores NaN but KEEPS infinities, so a single +inf sample
        # subtracted an infinite baseline from otherwise good channels and
        # silently blanked them. Mask every non-finite sample.
        fit_block = flat[fit_idx, :]
        fit_block = np.where(np.isfinite(fit_block), fit_block, np.nan)
        baseline_1d = np.nanmedian(fit_block, axis=0)  # (npix,)
        baseline = np.broadcast_to(baseline_1d, (nchans, ny * nx)).copy()
    else:
        # Polynomial least-squares fit constrained to line-free channels only.
        # The fitted model is then evaluated on ALL channels for subtraction.
        baseline = _fit_poly_baseline(fit_idx.astype(float),
                                      all_chan_idx.astype(float),
                                      flat[fit_idx, :], deg)   # (nchans, npix)

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
            "Baseline subtraction produced no finite voxels: no pixel had at "
            f"least {deg + 2} unflagged channel(s) inside the line-free "
            "selection. Widen the line-free ranges, exclude flagged channels, "
            "or lower the polynomial order."
        )
    # float64 BEFORE squaring: subtracted is float32, so a 1e20 voxel squared
    # overflowed to inf and the reported rms_after was inf — not representable
    # in JSON, so the endpoint answered 500.
    rms_after = float(np.sqrt(np.mean(
        subtracted[finite_after].astype(np.float64) ** 2)))
    logger.info(
        "[BASELINE] corrected range: min=%s  max=%s  rms_after=%s",
        float(np.min(subtracted[finite_after])),
        float(np.max(subtracted[finite_after])),
        rms_after,
    )

    # Caveats worth telling the user about a fit that SUCCEEDED. Extrapolation is
    # the cause worth naming; a residual larger than the input is the symptom,
    # and it is reported whatever the cause, because subtracting a baseline that
    # made the cube noisier is never what the user wanted.
    warnings_out = []
    if method == "polynomial":
        # The EFFECTIVE fit channels, not the requested ones: a sideband that is
        # entirely flagged constrains nothing, so a selection that looks
        # two-sided can be a one-sided fit in practice — which is exactly the
        # case worth warning about, and it went unreported.
        effective = fit_idx[finite_before[fit_idx, :].any(axis=1)]
        conditioning = _baseline_conditioning_warning(
            effective.astype(float), all_chan_idx.astype(float), deg)
        if conditioning:
            warnings_out.append(conditioning)
    if np.isfinite(rms_before) and rms_before > 0 and rms_after > rms_before:
        # Stated as an observation, not a diagnosis: a CORRECT continuum
        # subtraction on a cube with broad absorption raises the RMS (10 with
        # absorption to zero over 80 % of channels goes 4.47 -> 8.94), so
        # "this added signal" would misdiagnose it.
        warnings_out.append(
            f"The residual RMS ({rms_after:.4g}) came out HIGHER than the "
            f"input's ({rms_before:.4g}). That is expected when the subtracted "
            "continuum was large compared with the line, but it is also what a "
            "badly conditioned or over-fitted baseline looks like. Worth a "
            "look at the output before you build on it."
        )
    warning = " ".join(warnings_out)
    if warning:
        logger.warning("[BASELINE] %s", warning)

    # Write output FITS, preserving spatial and spectral WCS from the input.
    # Only NAXIS3 and CRPIX3 are updated to reflect the channel sub-range;
    # all other spectral keywords (CTYPE3, CUNIT3, CRVAL3, CDELT3) are kept
    # so that the output cube is correctly located in velocity/frequency space.
    header["NAXIS3"] = nchans
    header["CRPIX3"] = float(header.get("CRPIX3", 1.0)) - z0
    # Miriad writes its own description of the spectral axis alongside the FITS
    # one (LTYPE/LSTART/LSTEP/LWIDTH), and cropping the channel range left it
    # describing the UNCROPPED cube: a crop from channel 10 shifted CRPIX3 and
    # left LSTART at the original value. LSTEP is in LSTART's own units,
    # so the correction needs no unit conversion.
    _repair_celestial_units(dataset, header)
    if z0 and "LSTART" in header and "LSTEP" in header:
        try:
            header["LSTART"] = float(header["LSTART"]) + z0 * float(header["LSTEP"])
        except (TypeError, ValueError):
            del header["LSTART"]
    # The output is 3-D even when the input was 4-D with a degenerate Stokes
    # axis, so the fourth axis's WCS keywords must go with it.
    header["BITPIX"] = -32
    _drop_surplus_axis_keywords(header, 3)
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
        # Non-fatal caveats about the fit the caller asked for; the client folds
        # these into the summary it shows and records in the product's Provenance.
        "warning": warning,
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


def _cube_and_spectral_axis(path: str):
    """Return (cube[spec,y,x] float view, hdul, spectral_axis_values, unit, ctype).

    Assumes NAXIS3 is the spectral axis (numpy axis 0 after squeeze) — the
    standard radio-cube layout.
    """
    from astropy.wcs import WCS  # noqa: PLC0415
    import astropy.units as u    # noqa: PLC0415

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    cube = np.squeeze(np.asarray(raw))
    if cube.ndim != 3:
        hdul.close()
        raise ValueError("Spectral probe requires a 3-D cube.")
    nspec = cube.shape[0]
    try:
        wcs = WCS(dataset.header)
        if wcs.has_spectral and wcs.spectral.naxis == 1:
            world = u.Quantity(wcs.spectral.pixel_to_world(np.arange(nspec)))
            axis_vals = [float(v) for v in np.asarray(world.value)]
            unit = str(world.unit)
            ctype = str(wcs.spectral.wcs.ctype[0] or "").strip()
        else:
            axis_vals, unit, ctype = list(range(nspec)), "pix", "CHANNEL"
    except Exception:  # noqa: BLE001 — WCS may be absent/odd
        axis_vals, unit, ctype = list(range(nspec)), "pix", "CHANNEL"
    return cube, hdul, axis_vals, unit, ctype


def _json_safe(values) -> list:
    return [None if not np.isfinite(v) else float(v) for v in values]


def worker_spectral_probe(path: str, x: float, y: float) -> dict[str, Any]:
    cube, hdul, axis_vals, unit, ctype = _cube_and_spectral_axis(path)
    try:
        xi, yi = int(round(x)), int(round(y))
        ny, nx = cube.shape[1], cube.shape[2]
        if not (0 <= yi < ny and 0 <= xi < nx):
            raise ValueError(f"pixel ({xi},{yi}) out of range ({nx}x{ny})")
        values = np.asarray(cube[:, yi, xi], dtype=float)
        return {
            "valid": True, "error": "", "x": xi, "y": yi,
            "spectral_axis": axis_vals, "spectral_unit": unit, "spectral_type": ctype,
            "values": _json_safe(values),
        }
    finally:
        hdul.close()


def worker_spectral_aperture(path: str, x: float, y: float, radius: float) -> dict[str, Any]:
    cube, hdul, axis_vals, unit, ctype = _cube_and_spectral_axis(path)
    try:
        ny, nx = cube.shape[1], cube.shape[2]
        yy, xx = np.ogrid[:ny, :nx]
        mask = (xx - x) ** 2 + (yy - y) ** 2 <= float(radius) ** 2
        npix = int(mask.sum())
        if npix == 0:
            raise ValueError("aperture covers no pixels")
        region = cube[:, mask]  # (nspec, npix)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", RuntimeWarning)
            values = np.nanmean(region, axis=1)
        return {
            "valid": True, "error": "", "x": float(x), "y": float(y),
            "radius": float(radius), "n_pix": npix,
            "spectral_axis": axis_vals, "spectral_unit": unit, "spectral_type": ctype,
            "values": _json_safe(values),
        }
    finally:
        hdul.close()


def worker_line_fit(
    values: list[float],
    axis: list[float],
    with_baseline: bool = True,
) -> dict[str, Any]:
    """Fit a single Gaussian (+ optional linear baseline) to a 1-D spectrum.

    Returns the fitted peak, line centre, σ and FWHM (in the supplied axis
    units), the integrated line flux (amp·σ·√(2π)), the baseline coefficients,
    the continuum level under the line, an equivalent width when a continuum is
    present, and the residual RMS. Works for emission (positive amp) and
    absorption (negative amp) lines.
    """
    try:
        from scipy.optimize import curve_fit  # noqa: PLC0415
    except ImportError as exc:  # pragma: no cover
        raise ImportError("scipy is required for spectral line fitting.") from exc

    x = np.asarray(axis, dtype=np.float64)
    y = np.asarray(values, dtype=np.float64)
    good = np.isfinite(x) & np.isfinite(y)
    x, y = x[good], y[good]
    if x.size < 4:
        raise ValueError("Need at least 4 finite samples to fit a Gaussian line.")

    base0 = float(np.median(y))
    resid0 = y - base0
    i_peak = int(np.argmax(np.abs(resid0)))
    amp0 = float(resid0[i_peak])
    mu0 = float(x[i_peak])
    span = float(x.max() - x.min()) or 1.0
    sigma0 = span / 6.0

    def model(xx, amp, mu, sigma, a, b):
        baseline = (a + b * xx) if with_baseline else 0.0
        return baseline + amp * np.exp(-((xx - mu) ** 2) / (2.0 * sigma * sigma))

    p0 = [amp0, mu0, sigma0, base0, 0.0]
    try:
        popt, pcov = curve_fit(model, x, y, p0=p0, maxfev=20000)
    except Exception as exc:  # noqa: BLE001
        raise ValueError(f"Gaussian line fit did not converge: {exc}") from exc

    amp, mu, sigma, a, b = (float(v) for v in popt)
    sigma = abs(sigma)
    fwhm_factor = 2.0 * np.sqrt(2.0 * np.log(2.0))
    fwhm = fwhm_factor * sigma
    integrated = amp * sigma * np.sqrt(2.0 * np.pi)
    continuum = (a + b * mu) if with_baseline else 0.0
    ew = float(integrated / continuum) if (with_baseline and continuum) else None
    resid = y - model(x, *popt)
    rms = float(np.std(resid))

    # R-23: curve_fit's covariance assumes independent channels. Inflate it by the
    # measured effective-sample factor so EVERY error below — the diagonal ones and
    # the propagated integrated flux alike — carries the same correction. F = 1 on
    # an uncorrelated spectrum, so this is a no-op there.
    corr_factor = effective_sample_factor(resid)
    pcov = np.asarray(pcov, dtype=np.float64) * corr_factor
    # A large factor has two very different causes and this estimator cannot tell
    # them apart: genuinely correlated channels, or a model that does not fit —
    # an unresolved second component, broad wings, a standing-wave baseline, a
    # window that clips the line. In the second case wider error bars are not the
    # honest answer, a better model is. Say so rather than letting an inflated
    # error read as a considered result.
    corr_note = (
        "Residual autocorrelation is high (F = %.1f), so the reported errors are "
        "inflated by %.1fx. That is correct for spectrally smoothed data, but the "
        "same signature is produced by a model that does not fit - an unresolved "
        "component, broad wings, a curved baseline, or a window that clips the "
        "line. Inspect the residuals before trusting the widened errors."
        % (corr_factor, np.sqrt(corr_factor))
        # F = 2.25 is a 1.5x inflation — already material, and comfortably clear of
        # finite-sample noise: over 500 white-noise fits of this length the measured
        # F peaked at 1.91 and never reached the threshold.
        if corr_factor >= 2.25 else ""
    )

    # R-08: 1-sigma parameter uncertainties from the fit covariance. Undefined
    # (inf) for a perfect fit → reported as None. p order: [amp, mu, sigma, a, b].
    with np.errstate(invalid="ignore"):
        perr = np.sqrt(np.diag(pcov))
    perr = [float(e) if np.isfinite(e) else None for e in perr]
    amp_err, mu_err, sigma_err, _a_err, _b_err = perr
    fwhm_err = (sigma_err * fwhm_factor) if sigma_err is not None else None
    # I = amp·σ·√(2π): full-covariance propagation over params [amp, mu, sigma,
    # a, b]. dI/dA = I/amp, dI/dσ = I/σ. Keeps the amp–σ covariance.
    integrated_err = (
        covariance_propagate(
            pcov, (0, 2),
            (integrated / amp if amp else 0.0,
             integrated / sigma if sigma else 0.0),
        )
        if integrated else None
    )

    return {
        "peak": amp,
        "peak_err": amp_err,
        "center": mu,
        "center_err": mu_err,
        "sigma": sigma,
        "sigma_err": sigma_err,
        "fwhm": float(fwhm),
        "fwhm_err": fwhm_err,
        "integrated_flux": float(integrated),
        "integrated_flux_err": integrated_err,
        "continuum": float(continuum),
        "equivalent_width": ew,
        "baseline": [a, b],
        "rms": rms,
        "n_points": int(x.size),
        # Surfaced so a user can see WHY the errors are what they are, and audit
        # them: 1.0 means the channels looked independent.
        "spectral_correlation_factor": float(corr_factor),
        "spectral_correlation_note": corr_note,
    }
