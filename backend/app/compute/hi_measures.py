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
  ``2 sqrt(lambda)`` (SoFiA-2's ``ell_maj`` / ``ell_min``) (:func:`m0_ellipse`);
* the flux uncertainty from the noise, beams independent
  (:func:`flux_uncertainty`);
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


def _crossings(spectrum: np.ndarray, frac: float) -> tuple[float, float] | None:
    """Fractional channel positions (lo, hi) where ``spectrum`` crosses
    ``frac`` x its peak, moving inwards from both ends; None when no channel
    reaches it (SoFiA-2's ``spectral_line_width_dbl``)."""
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
    return lo, hi


def line_widths(spectrum: np.ndarray, vel: np.ndarray) -> dict[str, float]:
    """W20, W50 (km/s) and the systemic velocity (the midpoint of the W20
    crossings) of an integrated spectrum on the velocity axis ``vel`` (one
    value per channel, any direction).

    A channel with no valid voxel counts as zero (it is not emission). The
    crossings are fractional channels; their velocities are interpolated
    linearly between channel centres, so on a uniform axis the widths are
    SoFiA-2's ``w20`` x |channel width|. NaN where no channel reaches the level.
    """
    s = np.nan_to_num(np.asarray(spectrum, dtype=np.float64), nan=0.0, posinf=0.0, neginf=0.0)
    v = np.asarray(vel, dtype=np.float64).ravel()
    idx = np.arange(v.size, dtype=np.float64)
    out: dict[str, float] = {}
    for name, frac in (("20", 0.2), ("50", 0.5)):
        c = _crossings(s, frac)
        if c is None or v.size != s.size:
            out[f"w{name}"] = out[f"mid{name}"] = math.nan
            continue
        vlo, vhi = (float(np.interp(p, idx, v)) for p in c)
        out[f"w{name}"] = abs(vhi - vlo)
        out[f"mid{name}"] = 0.5 * (vlo + vhi)
    out["vsys"] = out["mid20"]
    return out


def second_moments(weights: np.ndarray, xx: np.ndarray, yy: np.ndarray,
                   centre: tuple[float, float] | None = None):
    """(cx, cy, a, b, theta): the weighted centre (or ``centre`` when given),
    the square roots of the eigenvalues of the weighted second-moment matrix
    about it (a >= b) and the major axis angle from +x towards +y (radians)."""
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
    a = math.sqrt(max(0.5 * (common + diff), 1e-9))
    b = math.sqrt(max(0.5 * (common - diff), 1e-9))
    theta = 0.5 * math.atan2(2 * mxy, mxx - myy)
    return cx, cy, a, b, theta


def m0_ellipse(m0: np.ndarray, centre_xy: tuple[float, float]) -> dict[str, float] | None:
    """Semi-major and semi-minor axes (pixels) and position angle (degrees,
    [0, 180), from +y towards -x: north through east when north is up and
    east left) of the moment-0 map ``m0`` (y, x), from the second moments of
    its positive pixels about ``centre_xy`` (the source's flux-weighted
    centroid, in the map's pixels). The axes are those of the uniform ellipse
    with the same moments, 2 sqrt(lambda), as SoFiA-2's ell_maj / ell_min. Not
    deconvolved from the beam. None when no pixel is positive."""
    good = np.isfinite(m0) & (m0 > 0)
    if not np.any(good):
        return None
    ny, nx = m0.shape
    yy, xx = np.mgrid[0:ny, 0:nx]
    _, _, a, b, theta = second_moments(np.where(good, m0, 0.0), xx, yy, centre_xy)
    return {"major_px": 2.0 * a, "minor_px": 2.0 * b,
            "pa_deg": float((math.degrees(theta) + 90.0) % 180.0)}


def flux_uncertainty(sigma: float, n_per_channel: np.ndarray, dv_kms: np.ndarray,
                     beam_px: np.ndarray) -> float:
    """Uncertainty (Jy km/s) of the integrated flux sum_k (S_k / Omega_k) dv_k
    from noise sigma (Jy/beam) per voxel: sqrt(sum_k n_k dv_k^2 / Omega_k) x
    sigma, with n_k the valid selected voxels of channel k and Omega_k its
    beam area in pixels. The noise is taken independent between beams on the
    sky and between channels (SoFiA-2's err_f_sum, sigma sqrt(N) dv /
    sqrt(Omega) on a uniform axis); correlated channels make it a lower bound."""
    n = np.asarray(n_per_channel, dtype=np.float64)
    var = np.sum(n * np.asarray(dv_kms, dtype=np.float64) ** 2 / np.asarray(beam_px, dtype=np.float64))
    return float(sigma) * math.sqrt(max(float(var), 0.0))


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
    except (TypeError, ValueError):
        return None
    return f if math.isfinite(f) else None
