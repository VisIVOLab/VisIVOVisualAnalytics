"""HI column density + mass from a moment-0 map (R-19).

Standard optically-thin HI derivations, built on the already-validated moment-0
machinery:

  * Column density (optically thin):
        N_HI [cm^-2] = 1.10e24 · M0 / (θ_maj · θ_min)
    with M0 the integrated intensity in Jy/beam·km/s and θ in arcsec.
  * Integrated HI mass:
        M_HI [M_sun] = 2.356e5 · D_Mpc² · S_int
    with S_int the total integrated flux in Jy·km/s (M0 summed over the map and
    divided by the beam area in pixels).

Requires a Jy/beam cube with a spectral (velocity) axis and a known beam. For a
frequency axis a rest frequency converts the width to km/s (R-02).

With a region (issue #401) the flux and mass are those of the pixels inside
it, less the mean of a background region when one is given:

    S = (Σ_region − N · mean_bg) / Ω_beam,   σ_S = σ · √(N / Ω_beam)

σ is the standard deviation of the background region's pixels or, without
one, the MAD noise of the whole map. The pixels within a beam are correlated:
N pixels hold N/Ω independent beams, hence √(N/Ω) rather than √N.
"""
from __future__ import annotations

import base64
from typing import Any

import numpy as np

from ..fits_dataset import ScientificFitsDataset
from ._common import (
    beam_area_pixels,
    frequency_width_to_kms,
    is_per_beam_bunit,
    resolve_channel_range,
)

# N_HI [cm^-2] = _NHI_CONST · I[Jy/beam·km/s] / (θ_maj·θ_min [arcsec²]) for the
# 21 cm line, optically thin. Collapses N_HI/T_B (1.823e18) × the Jy/beam→K
# brightness-temperature conversion at ν = 1.4204 GHz (≈6.06e5) into one constant.
_NHI_CONST = 1.104e24
# M_HI [M_sun] = _MHI_CONST · D_Mpc² · S_int[Jy·km/s].
_MHI_CONST = 2.356e5


#: σ from the median absolute deviation of a Gaussian.
_MAD_TO_SIGMA = 1.4826


def _region_pixels(shape: tuple[int, int], spec: dict[str, Any]) -> np.ndarray:
    """Boolean mask of the map pixels whose centres are in ``spec``.

    ``spec`` is a canonical image-frame region (``app.regions``: 0-based
    full-resolution pixels of the cube's sky plane, as the viewers send it);
    the test is ``region_stats.Region``'s, the one the viewers' statistics use.
    """
    from ..region_stats import Region  # noqa: PLC0415

    frame = str(spec.get("frame", "image") or "image").lower()
    if frame != "image":
        raise ValueError(f"A region must be in image pixels (frame 'image'), not {frame!r}.")
    ny, nx = shape
    mask = np.zeros(shape, dtype=bool)
    try:
        region = Region(spec)
        x0, x1, y0, y1 = region.bounds()
        x0, y0 = max(0, x0), max(0, y0)
        x1, y1 = min(nx - 1, x1), min(ny - 1, y1)
        if x0 > x1 or y0 > y1:
            return mask
        xs = np.arange(x0, x1 + 1)[None, :]
        ys = np.arange(y0, y1 + 1)[:, None]
        inside = region.mask(xs, ys)
    except (KeyError, TypeError, ValueError, ZeroDivisionError) as exc:
        # A missing centre, radius or size is the caller's error, said as such.
        raise ValueError(f"Malformed {spec.get('shape', '')!s} region: {exc!r}") from exc
    mask[y0:y1 + 1, x0:x1 + 1] = np.broadcast_to(inside, (y1 - y0 + 1, x1 - x0 + 1))
    return mask


def region_flux(m0_kms: np.ndarray, beam_area_px: float, region: dict[str, Any],
                background: dict[str, Any] | None = None) -> dict[str, Any]:
    """Integrated flux (Jy km/s) of a region of a Jy/beam·km/s moment-0 map.

    ``S = (Σ − N·mean_bg) / Ω`` over the region's N finite pixels, with Ω the
    beam area in pixels, and ``σ_S = σ·√(N/Ω)``: N pixels hold N/Ω independent
    beams, each summed noise σ·Ω, and the sum is divided by Ω. σ is the
    standard deviation (population) of the background region's pixels when
    one is given, else the MAD σ of the whole map (``error_from`` says which).
    Blank pixels count in neither region.
    """
    finite = np.isfinite(m0_kms)
    inside = _region_pixels(m0_kms.shape, region) & finite
    n = int(inside.sum())
    if n == 0:
        raise ValueError("The region holds no valid pixel of the moment-0 map.")
    total = float(m0_kms[inside].sum(dtype=np.float64))
    out: dict[str, Any] = {"n_pixels": n, "sum_jy_beam_kms": total,
                           "beam_area_pix": float(beam_area_px)}
    bg_mean = 0.0
    if background is not None:
        # All of the background region's pixels, as the viewers' statistics
        # of that region count them: a background that overlaps the source is
        # the user's choice, and the numbers stay the ones the viewer shows.
        bg = _region_pixels(m0_kms.shape, background) & finite
        n_bg = int(bg.sum())
        if n_bg < 2:
            raise ValueError("The background region holds fewer than two valid pixels.")
        bg_values = m0_kms[bg].astype(np.float64)
        bg_mean = float(bg_values.mean())
        sigma = float(bg_values.std())
        out.update({"background_n_pixels": n_bg, "background_mean_jy_beam_kms": bg_mean,
                    "background_sigma_jy_beam_kms": sigma, "error_from": "background"})
    else:
        values = m0_kms[finite].astype(np.float64)
        median = float(np.median(values))
        sigma = _MAD_TO_SIGMA * float(np.median(np.abs(values - median)))
        out.update({"map_sigma_jy_beam_kms": sigma, "error_from": "map_noise"})
    out["flux_jy_kms"] = (total - n * bg_mean) / float(beam_area_px)
    out["flux_error_jy_kms"] = sigma * float(np.sqrt(n / float(beam_area_px)))
    return out


def _velocity_unit_to_kms(unit: str) -> float | None:
    """Factor from a velocity ``unit`` (any astropy spelling) to km/s, or
    None when it is not a velocity."""
    from astropy import units as u  # noqa: PLC0415

    try:
        return float(u.Unit(str(unit).strip()).to(u.km / u.s))
    except (ValueError, TypeError, u.UnitsError):
        return None


def luminosity_distance_mpc(redshift: float) -> float:
    """Luminosity distance (Mpc) of ``redshift`` in Planck18, the app's default
    cosmology (``/v1/cosmology/distance``)."""
    from astropy.cosmology import Planck18  # noqa: PLC0415

    z = float(redshift)
    if not np.isfinite(z) or z < 0:
        raise ValueError(f"The redshift must be finite and non-negative, not {redshift!r}.")
    return float(Planck18.luminosity_distance(z).to("Mpc").value)


def worker_hi_products(
    path: str,
    channel_start: int | None,
    channel_end: int | None,
    distance_mpc: float | None = None,
    mask_enabled: bool = False,
    threshold_value: float = 0.0,
    rest_freq_hz: float | None = None,
    threshold_auto: bool = False,
    region: dict[str, Any] | None = None,
    background_region: dict[str, Any] | None = None,
    redshift: float | None = None,
) -> dict[str, Any]:
    """Column-density map summary + integrated HI mass from moment-0.

    IMPORTANT (integration domain): the integrated flux and HI mass sum M0 over
    every finite pixel in the channel range. With no mask that includes off-
    source noise, residual continuum and any OTHER sources in the field, so the
    integrated ``hi_mass_msun`` is a whole-field number, not a single-galaxy mass
    — quote it only over a masked/region-restricted map.

    ``channel_start``/``channel_end`` are inclusive; ``None`` means that end of
    the spectral axis (both ``None`` → whole cube) and ``start == end`` is a
    single channel. Enable ``mask_enabled``
    (with ``threshold_auto`` for a median+3σ MAD cut, or an explicit
    ``threshold_value``) to restrict the integral; the response reports the
    effective mask state, the number of pixels summed, and a warning when off.

    With ``region`` (a canonical image-frame region) the flux and the mass are
    the region's, less the mean of ``background_region`` when given, with an
    error (``region_flux``); the whole-field numbers stay beside them. A
    ``redshift`` without ``distance_mpc`` gives the distance as the Planck18
    luminosity distance.
    """
    if background_region is not None and region is None:
        raise ValueError("A background region needs a source region.")
    if (distance_mpc is None or distance_mpc <= 0) and redshift is not None:
        distance_mpc = luminosity_distance_mpc(redshift)
    from .moment import worker_moment  # noqa: PLC0415

    ds = ScientificFitsDataset(path)
    if not is_per_beam_bunit(ds.bunit):
        raise ValueError(
            f"HI products require a Jy/beam cube; BUNIT is {ds.bunit!r}.")
    # A single integrated N_HI/M_HI over the whole channel range needs ONE beam;
    # a per-channel BEAMS table means the beam varies with channel, so one beam
    # area can't be applied to the integrated map. Reject honestly rather than
    # return a wrong number (a restoring/common-beam cube is required).
    if ds.has_per_channel_beam:
        raise ValueError(
            "This cube has a per-channel beam (BEAMS table); HI column "
            "density/mass over a channel range needs a single common/restoring "
            "beam. Convolve to a common beam first.")
    beam = ds.beam
    if not beam.major or not beam.minor:
        raise ValueError("HI products require a known beam (BMAJ/BMIN).")

    # Resolve the channel range against the cube depth (None → that end of the
    # spectral axis; start == end is one channel), and report the EFFECTIVE
    # (clamped) range used.
    nz = int(ds.axis_sizes[ds.cube_numpy_spectral_fits_axis() - 1]) if ds.axis_sizes else 0
    channel_start, channel_end = resolve_channel_range(nz, channel_start, channel_end)
    theta_maj_arcsec = float(beam.major) * 3600.0
    theta_min_arcsec = float(beam.minor) * 3600.0
    pix_arcsec = ds.spatial_pixel_scale_arcsec()
    beam_area_px = beam_area_pixels(beam.major, beam.minor, pix_arcsec, ds.spatial_pixel_area_arcsec2())
    if not beam_area_px:
        raise ValueError("Could not compute the beam area in pixels.")

    # M0 over the requested channels, via the validated moment worker.
    m0 = worker_moment(path, 0, int(channel_start), int(channel_end),
                       bool(mask_enabled), float(threshold_value), bool(threshold_auto))
    m0_map = np.frombuffer(base64.b64decode(m0["data_base64"]), dtype=np.float32).astype(
        np.float64).reshape(int(m0["height"]), int(m0["width"]))
    spectral_unit = str(m0.get("spectral_axis_unit", ""))
    axis_type = str(m0.get("spectral_axis_type", ""))

    # Convert the integrated intensity to Jy/beam·km/s. Normalise whitespace so
    # astropy's spaced world unit ("m / s") is recognised, not just "m/s".
    su = spectral_unit.strip().lower().replace(" ", "")
    velocity_to_kms = _velocity_unit_to_kms(spectral_unit)
    if su in ("km/s", "kms"):
        to_kms = 1.0
    elif su in ("m/s", "ms"):
        to_kms = 1.0e-3
    elif velocity_to_kms is not None:
        # astropy's own spellings of the world unit ("m.s**-1", "km s-1"):
        # HCG 44's VELO-HEL axis without CUNIT3 comes out as "m.s**-1".
        to_kms = velocity_to_kms
    else:
        # Frequency axis → need a rest frequency to reach km/s; fall back to the
        # header RESTFRQ/RESTFREQ when the caller did not pass one (like the
        # line-width path does).
        rf = rest_freq_hz
        if rf is None:
            for key in ("RESTFRQ", "RESTFREQ"):
                if key in ds.header:
                    try:
                        rf = float(ds.header[key])
                        break
                    except (TypeError, ValueError):
                        pass
        factor, unit = frequency_width_to_kms(axis_type, spectral_unit, rf)
        if unit != "km/s":
            raise ValueError(
                "moment-0 spectral unit is "
                f"{spectral_unit!r}; supply rest_freq_hz for a frequency axis, "
                "or use a velocity cube.")
        to_kms = factor
    m0_kms = m0_map * to_kms  # Jy/beam·km/s

    finite = np.isfinite(m0_kms)
    nhi = np.full(m0_kms.shape, np.nan)
    nhi[finite] = _NHI_CONST * m0_kms[finite] / (theta_maj_arcsec * theta_min_arcsec)
    nhi_finite = nhi[finite & (nhi > 0)]

    # Integrated flux (Jy·km/s) and HI mass — summed over the pixels that
    # survived the (optional) mask. See the domain warning below.
    n_pixels_summed = int(finite.sum())
    s_int_jy_kms = float(np.nansum(m0_kms[finite]) / beam_area_px)
    m_hi = (
        _MHI_CONST * float(distance_mpc) ** 2 * s_int_jy_kms
        if distance_mpc and distance_mpc > 0 else None
    )
    region_result = None
    if region is not None:
        region_result = region_flux(m0_kms, beam_area_px, region, background_region)
        if distance_mpc and distance_mpc > 0:
            region_result["hi_mass_msun"] = (
                _MHI_CONST * float(distance_mpc) ** 2 * region_result["flux_jy_kms"])
            region_result["hi_mass_error_msun"] = (
                _MHI_CONST * float(distance_mpc) ** 2 * region_result["flux_error_jy_kms"])
        else:
            region_result["hi_mass_msun"] = None
            region_result["hi_mass_error_msun"] = None
    warnings: list[str] = []
    if not mask_enabled:
        warnings.append(
            "no mask applied, integrated_flux_jy_kms and hi_mass_msun are a "
            "WHOLE-FIELD integral (all sources + noise + residual continuum), not "
            "a single-source mass. Enable mask_enabled/threshold_auto or restrict "
            "to a source region before quoting the mass.")

    return {
        "channel_start": int(channel_start),   # effective (clamped) range used
        "channel_end": int(channel_end),
        "n_channels_in_cube": nz,
        "beam_arcsec": [theta_maj_arcsec, theta_min_arcsec],
        "beam_area_pix": beam_area_px,
        "mask_enabled": bool(mask_enabled),
        "threshold_auto": bool(threshold_auto),
        "threshold_used": m0.get("threshold_used"),
        "n_pixels_summed": n_pixels_summed,
        "warnings": warnings,
        "nhi_unit": "cm^-2",
        "nhi_min": float(np.nanmin(nhi_finite)) if nhi_finite.size else None,
        "nhi_max": float(np.nanmax(nhi_finite)) if nhi_finite.size else None,
        "nhi_median": float(np.nanmedian(nhi_finite)) if nhi_finite.size else None,
        "integrated_flux_jy_kms": s_int_jy_kms,
        "distance_mpc": distance_mpc,
        "redshift": redshift,
        "region": region_result,
        "hi_mass_msun": m_hi,
        "hi_mass_log10_msun": (float(np.log10(m_hi)) if (m_hi and m_hi > 0) else None),
        "optically_thin": True,
    }
