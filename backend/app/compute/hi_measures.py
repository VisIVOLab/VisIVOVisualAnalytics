"""The standard HI measurements of a selection (issue #280), as pure functions.

An HI astronomer reads a source by its integrated flux with its uncertainty,
its peak and peak S/N, its line widths and systemic velocity, its flux-weighted
centroid and its size. The definitions follow SoFiA-2's parameterisation
(``DataCube_parameterise`` and ``statistics_dbl.c`` of SoFiA-2 2.x), so a
selection given the same mask as a SoFiA-2 catalogue row gives the same
numbers; where this module converts to physical units (km/s, Jy km/s, arcsec)
it says how:

* line widths: moving inwards from both ends of the integrated spectrum to the
  first channel at or above 20 % (50 %) of its peak, the crossing linearly
  interpolated between that channel and the one outside it
  (:func:`line_widths`), the width in km/s between the two crossings;
* size: the second moments of the positive moment-0 pixels about the source
  centroid, as the semi-axes of the uniform ellipse with the same moments,
  ``2 sqrt(lambda)`` (SoFiA-2's ``ell_maj`` / ``ell_min``) (:func:`m0_ellipse`),
  its position angle east of north from the WCS (:func:`sky_position_angle`);
* the flux uncertainty from the noise, beams independent
  (:func:`flux_uncertainty`), corrected for correlated channels
  (:func:`correlation_factor`, with :func:`channel_rho` from the noise frame);
* the local noise: 1.4826 x the median absolute value (about zero) of the
  voxels around the selection that are not selected (:func:`mad_about_zero`).

The kinematic fit's measures (``kinematic_fit.source_measures``) take their
widths from :func:`line_widths` too: one definition in the app.
"""
from __future__ import annotations

import math
from typing import Any

import numpy as np

#: 1 / Phi^-1(3/4): sigma of a Gaussian from its median absolute deviation.
MAD_TO_SIGMA = 1.482602218505602

#: SoFiA-2's frame for the local noise: the source's box widened by 3 beam
#: majors on the sky (10 pixels without a beam) and 5 channels.
NOISE_PAD_BEAMS = 3.0
NOISE_PAD_PIXELS_NO_BEAM = 10
NOISE_PAD_CHANNELS = 5

#: Channel lags whose noise correlation the flux uncertainty corrects for.
RHO_LAGS = 3
#: Fewest pairs of unselected voxels a lag's correlation is measured from.
RHO_MIN_PAIRS = 200

#: Standard deviation of a uniform pixel: a footprint whose second moment is
#: below it along an axis is narrower than one pixel there.
MIN_AXIS_SIGMA_PX = 1.0 / math.sqrt(12.0)
#: (lambda1 - lambda2) / (lambda1 + lambda2) below this: a round footprint,
#: whose position angle is noise.
ROUND_FOOTPRINT = 0.01


def _crossings(spectrum: np.ndarray, frac: float) -> tuple[float, float, bool] | None:
    """Fractional channel positions (lo, hi) where ``spectrum`` crosses
    ``frac`` x its peak, moving inwards from both ends, and whether a crossing
    lies on the first or last channel (the line reaches the spectrum's edge);
    None when no channel reaches it (SoFiA-2's ``spectral_line_width_dbl``)."""
    s = np.asarray(spectrum, dtype=np.float64)
    n = s.size
    if n == 0:
        return None
    peak = float(np.max(s))
    if not math.isfinite(peak) or peak <= 0.0:
        return None
    level = frac * peak
    above = np.nonzero(s >= level)[0]
    if not above.size:
        return None
    i, j = int(above[0]), int(above[-1])
    lo, hi = float(i), float(j)
    if i > 0 and s[i] != s[i - 1]:
        lo -= (s[i] - level) / (s[i] - s[i - 1])
    if j < n - 1 and s[j] != s[j + 1]:
        hi += (s[j] - level) / (s[j] - s[j + 1])
    return lo, hi, (i == 0 or j == n - 1)


def line_widths(spectrum: np.ndarray, vel: np.ndarray) -> dict[str, Any]:
    """W20, W50 (km/s) and the systemic velocity (the midpoint of the W20
    crossings) of an integrated spectrum on the velocity axis ``vel`` (one
    value per channel, any direction).

    A channel with no valid voxel counts as zero (it is not emission). The
    crossings are fractional channels; their velocities are interpolated
    linearly between channel centres, so on a uniform axis the widths are
    SoFiA-2's ``w20`` x |channel width|. NaN where no channel reaches the level.
    ``edge20`` / ``edge50``: a crossing is on the first or last channel, so
    the width is a lower limit.
    """
    s = np.nan_to_num(np.asarray(spectrum, dtype=np.float64), nan=0.0, posinf=0.0, neginf=0.0)
    v = np.asarray(vel, dtype=np.float64).ravel()
    idx = np.arange(v.size, dtype=np.float64)
    out: dict[str, Any] = {}
    for name, frac in (("20", 0.2), ("50", 0.5)):
        c = _crossings(s, frac)
        if c is None or v.size != s.size:
            out[f"w{name}"] = out[f"mid{name}"] = math.nan
            out[f"edge{name}"] = False
            continue
        vlo, vhi = (float(np.interp(p, idx, v)) for p in c[:2])
        out[f"w{name}"] = abs(vhi - vlo)
        out[f"mid{name}"] = 0.5 * (vlo + vhi)
        out[f"edge{name}"] = bool(c[2])
    out["vsys"] = out["mid20"]
    return out


def moment_eigen(weights: np.ndarray, xx: np.ndarray, yy: np.ndarray,
                 centre: tuple[float, float] | None = None):
    """(cx, cy, lambda1, lambda2, theta): the weighted centre (or ``centre``
    when given), the eigenvalues of the weighted second-moment matrix about it
    (lambda1 >= lambda2 >= 0, unclamped) and the major axis angle from +x
    towards +y (radians)."""
    tot = float(weights.sum())
    if centre is None:
        cx = float((weights * xx).sum() / tot)
        cy = float((weights * yy).sum() / tot)
    else:
        cx, cy = float(centre[0]), float(centre[1])
    dx, dy = xx - cx, yy - cy
    mxx = float((weights * dx * dx).sum() / tot)
    myy = float((weights * dy * dy).sum() / tot)
    mxy = float((weights * dx * dy).sum() / tot)
    common, diff = mxx + myy, math.hypot(mxx - myy, 2 * mxy)
    lam1 = max(0.5 * (common + diff), 0.0)
    lam2 = max(0.5 * (common - diff), 0.0)
    theta = 0.5 * math.atan2(2 * mxy, mxx - myy)
    return cx, cy, lam1, lam2, theta


def second_moments(weights: np.ndarray, xx: np.ndarray, yy: np.ndarray,
                   centre: tuple[float, float] | None = None):
    """(cx, cy, a, b, theta) of :func:`moment_eigen`, a and b the square roots
    of the eigenvalues kept above 3e-5 so that b / a is defined (the
    kinematic fit's geometry)."""
    cx, cy, lam1, lam2, theta = moment_eigen(weights, xx, yy, centre)
    return cx, cy, math.sqrt(max(lam1, 1e-9)), math.sqrt(max(lam2, 1e-9)), theta


def m0_ellipse(m0: np.ndarray, centre_xy: tuple[float, float]) -> dict[str, Any] | None:
    """Semi-major and semi-minor axes (pixels), the major axis angle ``theta``
    (radians from +x towards +y) and the position angle on the pixel grid
    (``pa_grid_deg``, [0, 180), from +y towards -x) of the moment-0 map ``m0``
    (y, x), from the second moments of its positive pixels about
    ``centre_xy`` (the source's flux-weighted centroid, in the map's pixels).
    The axes are those of the uniform ellipse with the same moments, 2
    sqrt(lambda), as SoFiA-2's ell_maj / ell_min. Not deconvolved from the
    beam. None when no pixel is positive.

    A footprint narrower than a pixel along its minor axis (sqrt(lambda2) <
    1/sqrt(12)) has no size (axes None); a round one (lambda1 ~ lambda2) no
    position angle. ``note`` says which."""
    good = np.isfinite(m0) & (m0 > 0)
    if not np.any(good):
        return None
    ny, nx = m0.shape
    yy, xx = np.mgrid[0:ny, 0:nx]
    _, _, lam1, lam2, theta = moment_eigen(np.where(good, m0, 0.0), xx, yy, centre_xy)
    out: dict[str, Any] = {"major_px": 2.0 * math.sqrt(lam1), "minor_px": 2.0 * math.sqrt(lam2),
                           "theta": theta, "pa_grid_deg": float((math.degrees(theta) + 90.0) % 180.0),
                           "note": ""}
    if math.sqrt(lam2) < MIN_AXIS_SIGMA_PX:
        out.update(major_px=None, minor_px=None, theta=None, pa_grid_deg=None,
                   note="the moment-0 footprint is narrower than a pixel: no size")
    elif lam1 + lam2 <= 0 or (lam1 - lam2) / (lam1 + lam2) < ROUND_FOOTPRINT:
        out.update(theta=None, pa_grid_deg=None,
                   note="the moment-0 footprint is round: no position angle")
    return out


def sky_position_angle(to_sky, cx: float, cy: float, theta: float) -> float | None:
    """Position angle (degrees, [0, 180), east of north) of the pixel-grid
    direction ``theta`` (radians from +x towards +y) at pixel (cx, cy):
    ``to_sky(x, y)`` gives (RA, Dec) in degrees; the direction is followed
    for one pixel and its bearing taken on the sphere, so a grid whose north
    is tilted (projection, rotation) is read correctly. None when either
    point has no sky position."""
    p0 = to_sky(cx, cy)
    p1 = to_sky(cx + math.cos(theta), cy + math.sin(theta))
    if p0 is None or p1 is None:
        return None
    ra0, dec0 = (math.radians(v) for v in p0)
    ra1, dec1 = (math.radians(v) for v in p1)
    dra = ra1 - ra0
    pa = math.atan2(math.sin(dra) * math.cos(dec1),
                    math.cos(dec0) * math.sin(dec1) - math.sin(dec0) * math.cos(dec1) * math.cos(dra))
    return float(math.degrees(pa) % 180.0)


def flux_uncertainty(sigma: float, n_per_channel: np.ndarray, dv_kms: np.ndarray,
                     beam_px: np.ndarray) -> float:
    """Uncertainty (Jy km/s) of the integrated flux sum_k (S_k / Omega_k) dv_k
    from noise sigma (Jy/beam) per voxel: sqrt(sum_k n_k dv_k^2 / Omega_k) x
    sigma, with n_k the valid selected voxels of channel k and Omega_k its
    beam area in pixels. The noise is taken independent between beams on the
    sky and between channels (SoFiA-2's err_f_sum, sigma sqrt(N) dv /
    sqrt(Omega) on a uniform axis); :func:`correlation_factor` corrects for
    correlated channels. A selection narrower than a beam (n_k < Omega_k)
    still underestimates it, by about sqrt(Omega_k / n_k)."""
    n = np.asarray(n_per_channel, dtype=np.float64)
    var = np.sum(n * np.asarray(dv_kms, dtype=np.float64) ** 2 / np.asarray(beam_px, dtype=np.float64))
    return float(sigma) * math.sqrt(max(float(var), 0.0))


def channel_rho(sigma: float, lag_diffs: list[np.ndarray]) -> list[float | None]:
    """Noise correlation between channels j apart, j = 1, 2, ..., from the
    differences of unselected voxel pairs at the same pixel: rho_j = 1 -
    sigma_d^2 / (2 sigma^2), sigma_d = 1.4826 x median |d| (about zero, as
    sigma). Robust to faint emission, unlike a Pearson coefficient (#64's
    rule). None for a lag with fewer than RHO_MIN_PAIRS pairs; clipped to
    [-1, 1]."""
    out: list[float | None] = []
    for d in lag_diffs:
        d = np.asarray(d, dtype=np.float64)
        d = d[np.isfinite(d)]
        if d.size < RHO_MIN_PAIRS or not sigma or sigma <= 0:
            out.append(None)
            continue
        sd = MAD_TO_SIGMA * float(np.median(np.abs(d)))
        out.append(float(min(1.0, max(-1.0, 1.0 - sd * sd / (2.0 * sigma * sigma)))))
    return out


def correlation_factor(rho: list[float | None], n_channels: float) -> float:
    """sqrt(1 + 2 sum_j rho_j (1 - j / N)): the factor on the uncertainty of a
    sum of N channels whose noise is correlated with lag-j coefficient rho_j
    (stationary noise; lags beyond the measured ones taken as uncorrelated).
    N is the selection's mean number of channels per spatial pixel. Never
    below 0 under the root; 1 without a measured lag or with N <= 1."""
    if not n_channels or n_channels <= 1:
        return 1.0
    s = 0.0
    for j, r in enumerate(rho, start=1):
        if r is not None and j < n_channels:
            s += r * (1.0 - j / n_channels)
    return math.sqrt(max(1.0 + 2.0 * s, 0.0))


#: SPECSYS values (FITS Paper III) as an astronomer names the frame.
_FRAMES = {
    "BARYCENT": "barycentric", "HELIOCEN": "heliocentric", "TOPOCENT": "topocentric",
    "GEOCENTR": "geocentric", "LSRK": "LSRK", "LSRD": "LSRD (dynamical)",
    "GALACTOC": "galactocentric", "LOCALGRP": "Local Group", "CMBDIPOL": "CMB dipole",
    "SOURCE": "source rest frame",
}
#: The frame an AIPS CTYPE suffix (VELO-HEL, FELO-LSR, FREQ-OBS) names.
_AIPS_FRAMES = {"HEL": "barycentric (heliocentric)", "BAR": "barycentric",
                "LSR": "LSRK", "LSD": "LSRD (dynamical)", "OBS": "topocentric",
                "GEO": "geocentric", "TOP": "topocentric"}


def spectral_frame(specsys: str | None, ctype: str | None) -> tuple[str, str]:
    """(readable frame, where it came from) of a spectral axis: SPECSYS when
    the header has it, else the AIPS CTYPE suffix (VELO-HEL → barycentric
    (heliocentric), -LSR → LSRK, -OBS → topocentric); ('', '') when neither
    says."""
    key = str(specsys or "").strip().upper()
    if key:
        return _FRAMES.get(key, key), f"SPECSYS {key}"
    c = str(ctype or "").strip().upper()
    if len(c) > 5 and c[4] == "-":
        suffix = c[5:8]
        if suffix in _AIPS_FRAMES:
            return _AIPS_FRAMES[suffix], f"CTYPE {c}"
    return "", ""


def mad_about_zero(values: np.ndarray) -> float | None:
    """1.4826 x median(|x|) of the finite ``values``: the noise of a
    continuum-subtracted cube, measured about zero as SoFiA-2 measures its
    local rms (``mad_val_dbl`` about 0). None without a finite value."""
    v = np.asarray(values, dtype=np.float64)
    v = v[np.isfinite(v)]
    if not v.size:
        return None
    s = MAD_TO_SIGMA * float(np.median(np.abs(v)))
    return s if s > 0 else None


def noise_padding(bmaj_deg: float | None, pixel_deg: float | None) -> int:
    """Pixels the local-noise frame extends the source's box by on the sky."""
    if bmaj_deg and pixel_deg and bmaj_deg > 0 and pixel_deg > 0:
        return int(math.ceil(NOISE_PAD_BEAMS * bmaj_deg / pixel_deg))
    return NOISE_PAD_PIXELS_NO_BEAM


def finite_or_none(v: Any) -> float | None:
    try:
        f = float(v)
    except (TypeError, ValueError, OverflowError):
        return None
    return f if math.isfinite(f) else None
