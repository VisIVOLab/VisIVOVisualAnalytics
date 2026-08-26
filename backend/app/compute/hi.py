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
"""
from __future__ import annotations

import base64
from typing import Any

import numpy as np

from ..fits_dataset import ScientificFitsDataset
from ._common import beam_area_pixels, frequency_width_to_kms, is_per_beam_bunit

# N_HI [cm^-2] = _NHI_CONST · I[Jy/beam·km/s] / (θ_maj·θ_min [arcsec²]) for the
# 21 cm line, optically thin (1.823e18 · 1.222e6·λ²… collapsed).
_NHI_CONST = 1.104e24
# M_HI [M_sun] = _MHI_CONST · D_Mpc² · S_int[Jy·km/s].
_MHI_CONST = 2.356e5


def worker_hi_products(
    path: str,
    channel_start: int,
    channel_end: int,
    distance_mpc: float | None = None,
    mask_enabled: bool = False,
    threshold_value: float = 0.0,
    rest_freq_hz: float | None = None,
) -> dict[str, Any]:
    """Column-density map summary + integrated HI mass from moment-0."""
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

    # Resolve the channel range against the cube depth; default/empty → full
    # spectral depth, and report the EFFECTIVE (clamped) range used.
    nz = int(ds.axis_sizes[ds.cube_numpy_spectral_fits_axis() - 1]) if ds.axis_sizes else 0
    cs = int(channel_start)
    ce = int(channel_end)
    if ce <= cs:
        cs, ce = 0, max(0, nz - 1)
    cs = max(0, min(cs, max(0, nz - 1)))
    ce = max(cs, min(ce, max(0, nz - 1)))
    channel_start, channel_end = cs, ce
    theta_maj_arcsec = float(beam.major) * 3600.0
    theta_min_arcsec = float(beam.minor) * 3600.0
    pix_arcsec = ds.spatial_pixel_scale_arcsec()
    beam_area_px = beam_area_pixels(beam.major, beam.minor, pix_arcsec)
    if not beam_area_px:
        raise ValueError("Could not compute the beam area in pixels.")

    # M0 over the requested channels, via the validated moment worker.
    m0 = worker_moment(path, 0, int(channel_start), int(channel_end),
                       bool(mask_enabled), float(threshold_value))
    m0_map = np.frombuffer(base64.b64decode(m0["data_base64"]), dtype=np.float32).astype(np.float64)
    spectral_unit = str(m0.get("spectral_axis_unit", ""))
    axis_type = str(m0.get("spectral_axis_type", ""))

    # Convert the integrated intensity to Jy/beam·km/s.
    su = spectral_unit.strip().lower()
    if su in ("km/s", "kms"):
        to_kms = 1.0
    elif su in ("m/s", "ms"):
        to_kms = 1.0e-3
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

    # Integrated flux (Jy·km/s) and HI mass.
    s_int_jy_kms = float(np.nansum(m0_kms[finite]) / beam_area_px)
    m_hi = (
        _MHI_CONST * float(distance_mpc) ** 2 * s_int_jy_kms
        if distance_mpc and distance_mpc > 0 else None
    )

    return {
        "channel_start": int(channel_start),   # effective (clamped) range used
        "channel_end": int(channel_end),
        "n_channels_in_cube": nz,
        "beam_arcsec": [theta_maj_arcsec, theta_min_arcsec],
        "beam_area_pix": beam_area_px,
        "nhi_unit": "cm^-2",
        "nhi_min": float(np.nanmin(nhi_finite)) if nhi_finite.size else None,
        "nhi_max": float(np.nanmax(nhi_finite)) if nhi_finite.size else None,
        "nhi_median": float(np.nanmedian(nhi_finite)) if nhi_finite.size else None,
        "integrated_flux_jy_kms": s_int_jy_kms,
        "distance_mpc": distance_mpc,
        "hi_mass_msun": m_hi,
        "hi_mass_log10_msun": (float(np.log10(m_hi)) if (m_hi and m_hi > 0) else None),
        "optically_thin": True,
    }
