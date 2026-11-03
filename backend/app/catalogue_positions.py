"""Scene positions of a sky catalogue, as the desktop's 3-D catalogue viewer
places its sources (issue #83), so that a catalogue drawn on the backend shows
what the desktop would.

The rules are those of ``Catalogue3DParser::detail::entryDistanceMpc`` and
``vtkWindowCatalogue3D::applyFrameToEntries`` (src/gui), in their order:

- the radial distance, in Mpc:
  1. redshift-space mode and a redshift > 0: the Hubble distance c z / H0;
  2. a cosmology other than the local Planck18 and a redshift > 0: that model's
     comoving distance (astropy, as /v1/cosmology/distance/batch);
  3. a distance column > 0: comoving as is, a luminosity distance / (1 + z), an
     angular-diameter distance × (1 + z) when a redshift > 0 is known;
  4. a redshift > 0: the Planck18 comoving distance, integrated as the desktop
     integrates it;
  5. otherwise 300 Mpc;
- the direction: RA/Dec, or galactic or supergalactic longitude and latitude;
- the position: d (cos b cos l, cos b sin l, sin b).
"""
from __future__ import annotations

import numpy as np
import pandas as pd

_H0 = 67.66                 # Planck18, as the desktop's local integrator
_C_KMS = 2.99792458e5
_OMEGA_M = 0.3111

_REDSHIFT_ALIASES = ("Z", "REDSHIFT", "REDSHIFTZ", "ZSPEC", "ZMEAN", "ZPHOT", "ZPHOTOZ")
_DISTANCE_ALIASES = ("COMOVINGDISTANCE", "COMOVINGDIST", "DC", "DCMPC", "DISTANCE", "DIST",
                     "DMPC", "DISTMPC", "DL", "DLMPC", "LUMINOSITYDISTANCE", "DA", "DAMPC",
                     "ANGULARDIAMETERDISTANCE")

FRAMES = ("fk5", "galactic", "supergalactic")
DISTANCE_MODES = ("comoving", "redshift_space")


def _normalize(name: str) -> str:
    return "".join(ch.upper() for ch in str(name) if ch.isalnum())


def _first(frame: pd.DataFrame, aliases) -> str:
    # The last column of a normalised name wins, as in the desktop's QMap.
    by_name = {}
    for column in frame.columns:
        by_name[_normalize(column)] = str(column)
    return next((by_name[a] for a in aliases if a in by_name), "")


def _numeric(frame: pd.DataFrame, column: str) -> np.ndarray:
    if not column:
        return np.full(len(frame), np.nan)
    return pd.to_numeric(frame[column], errors="coerce").to_numpy(dtype=float)


def planck18_comoving_mpc(z: np.ndarray) -> np.ndarray:
    """The desktop's local Planck18 comoving distance: the trapezoid rule with
    1000 steps from 0 to z (Catalogue3DParser::detail::comovingDistanceMpc)."""
    z = np.asarray(z, dtype=float)
    out = np.zeros_like(z)
    positive = np.isfinite(z) & (z > 0)
    if not positive.any():
        return out
    steps = 1000
    zs = z[positive]
    grid = zs[:, None] * (np.arange(steps + 1)[None, :] / steps)
    e_inv = 1.0 / np.sqrt(_OMEGA_M * (1.0 + grid) ** 3 + (1.0 - _OMEGA_M))
    integral = (e_inv.sum(axis=1) - 0.5 * (e_inv[:, 0] + e_inv[:, -1])) * (zs / steps)
    out[positive] = _C_KMS / _H0 * integral
    return out


def _model_comoving_mpc(model: str, z: np.ndarray) -> np.ndarray:
    from .routers.cosmology import _load_cosmology_models  # noqa: PLC0415
    models = _load_cosmology_models()
    if model not in models:
        raise ValueError(f"Unknown cosmology model {model!r}.")
    out = np.full(len(z), np.nan)
    positive = np.isfinite(z) & (z > 0)
    if positive.any():
        out[positive] = models[model].comoving_distance(z[positive]).to("Mpc").value
    return out


def distances_mpc(frame: pd.DataFrame, distance_mode: str = "comoving",
                  cosmology: str = "Planck18") -> np.ndarray:
    """The radial distance of each source, by the rules in the module doc."""
    z = _numeric(frame, _first(frame, _REDSHIFT_ALIASES))
    has_z = np.isfinite(z) & (z > 0)
    d = np.full(len(frame), 300.0)
    resolved = np.zeros(len(frame), dtype=bool)

    def take(mask, values):
        nonlocal resolved
        mask = mask & ~resolved & np.isfinite(values)
        d[mask] = values[mask]
        resolved = resolved | mask

    if distance_mode == "redshift_space":
        take(has_z, _C_KMS * z / _H0)
    if cosmology != "Planck18":
        take(has_z, _model_comoving_mpc(cosmology, z))
    column = _first(frame, _DISTANCE_ALIASES)
    if column:
        dist = _numeric(frame, column)
        kind = _normalize(column)
        converted = dist.copy()
        if kind.startswith("DL") or kind == "LUMINOSITYDISTANCE":
            converted = np.where(has_z, dist / (1.0 + z), dist)
        elif kind.startswith("DA") or kind == "ANGULARDIAMETERDISTANCE":
            converted = np.where(has_z, dist * (1.0 + z), dist)
        take(np.isfinite(dist) & (dist > 0), converted)
    take(has_z, planck18_comoving_mpc(z))
    return d


def sky_positions(frame: pd.DataFrame, ra_field: str, dec_field: str, *,
                  frame_name: str = "fk5", distance_mode: str = "comoving",
                  cosmology: str = "Planck18") -> np.ndarray:
    """(N, 3) scene positions, NaN for a source without a finite RA/Dec."""
    if frame_name not in FRAMES:
        raise ValueError(f"Unknown frame {frame_name!r}: one of {FRAMES}.")
    if distance_mode not in DISTANCE_MODES:
        raise ValueError(f"Unknown distance mode {distance_mode!r}: one of {DISTANCE_MODES}.")
    ra = _numeric(frame, ra_field)
    dec = _numeric(frame, dec_field)
    lon, lat = ra, dec
    if frame_name != "fk5":
        from astropy import units as u  # noqa: PLC0415
        from astropy.coordinates import SkyCoord  # noqa: PLC0415
        ok = np.isfinite(ra) & np.isfinite(dec)
        lon, lat = np.full(len(ra), np.nan), np.full(len(dec), np.nan)
        if ok.any():
            sky = SkyCoord(ra=ra[ok] * u.deg, dec=dec[ok] * u.deg, frame="fk5", equinox="J2000")
            if frame_name == "galactic":
                g = sky.galactic
                lon[ok], lat[ok] = g.l.deg, g.b.deg
            else:
                sg = sky.supergalactic
                lon[ok], lat[ok] = sg.sgl.deg, sg.sgb.deg
    d = distances_mpc(frame, distance_mode, cosmology)
    lon_r, lat_r = np.radians(lon), np.radians(lat)
    return np.stack([d * np.cos(lat_r) * np.cos(lon_r),
                     d * np.cos(lat_r) * np.sin(lon_r),
                     d * np.sin(lat_r)], axis=1)
