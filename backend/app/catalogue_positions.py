"""Scene positions of a sky catalogue, as the desktop's 3-D catalogue viewer
places its sources (issue #83), so that a catalogue drawn on the backend shows
what the desktop would.

The rules are those of ``Catalogue3DParser::detail::entryDistance`` and
``vtkWindowCatalogue3D::applyFrameToEntries`` (src/gui), in their order:

- the radial distance, in Mpc:
  1. redshift-space mode and a redshift > 0: the Hubble distance c z / H0,
     with the H0 of the cosmology chosen (issue #260);
  2. a distance column > 0: comoving as is, a luminosity distance / (1 + z), an
     angular-diameter distance × (1 + z) when a redshift > 0 is known. A
     measured distance wins under every cosmology (issue #265);
  3. a redshift > 0: the comoving distance of the cosmology chosen: Planck18
     integrated as the desktop integrates it, another model by astropy (as
     /v1/cosmology/distance/batch);
  4. otherwise no distance: the source is not placed (NaN) and is counted
     apart (issue #265). A redshift of 0 means "not measured" in catalogues,
     and a negative one (a Local Group galaxy's peculiar velocity) gives no
     distance either; a default shell would invent one.
- the direction: RA/Dec, or galactic or supergalactic longitude and latitude;
- the position: d (cos b cos l, cos b sin l, sin b).

A source placed from a redshift with 0 < cz < 1000 km/s is flagged: peculiar
velocities of about 300 km/s (Strauss & Willick 1995) make its distance 30 %
uncertain or worse.
"""
from __future__ import annotations

from dataclasses import dataclass

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


_GRID_NODES = 8192          # a distance is smooth in z: interpolated on this grid


def _planck18_trapezoid_mpc(zs: np.ndarray) -> np.ndarray:
    """The desktop's integral itself, for z > 0: the trapezoid rule with 1000
    steps from 0 to z (Catalogue3DParser::detail::comovingDistanceMpc)."""
    steps = 1000
    out = np.empty(len(zs))
    for start in range(0, len(zs), 4096):          # bounded temporaries
        chunk = zs[start:start + 4096]
        grid = chunk[:, None] * (np.arange(steps + 1)[None, :] / steps)
        e_inv = 1.0 / np.sqrt(_OMEGA_M * (1.0 + grid) ** 3 + (1.0 - _OMEGA_M))
        integral = (e_inv.sum(axis=1) - 0.5 * (e_inv[:, 0] + e_inv[:, -1])) * (chunk / steps)
        out[start:start + len(chunk)] = _C_KMS / _H0 * integral
    return out


def _on_a_grid(z: np.ndarray, distance) -> np.ndarray:
    """``distance`` (z > 0 → Mpc) at each finite z > 0, NaN elsewhere: computed
    on a grid of z and interpolated, when there are more sources than nodes.
    A comoving distance is smooth in z. The nodes are even in ln(1 + z), so a
    far outlier does not coarsen the grid near z = 0: up to z = 3 the linear
    interpolation differs by less than 100 pc, and by less than 1 kpc with a
    source at z = 1000 in the table."""
    out = np.full(len(z), np.nan)
    positive = np.isfinite(z) & (z > 0)
    if not positive.any():
        return out
    zs = z[positive]
    if len(zs) <= _GRID_NODES:
        out[positive] = distance(zs)
    else:
        nodes = np.expm1(np.linspace(0.0, np.log1p(zs.max()), _GRID_NODES))
        nodes[-1] = zs.max()   # expm1(log1p(z)) may round below z
        values = np.concatenate([[0.0], distance(nodes[1:])])
        out[positive] = np.interp(zs, nodes, values)
    return out


def planck18_comoving_mpc(z: np.ndarray) -> np.ndarray:
    """The desktop's local Planck18 comoving distance, 0 where z is not > 0."""
    return np.nan_to_num(_on_a_grid(np.asarray(z, dtype=float), _planck18_trapezoid_mpc),
                         nan=0.0)


#: The name a custom cosmology travels under in the desktop. The backend has
#: no custom model for a scene: H0, Ωm and ΩΛ are not sent.
CUSTOM_COSMOLOGY = "Custom"
#: Below this recession velocity a distance from the redshift is uncertain:
#: peculiar velocities are about 300 km/s (Strauss & Willick 1995, Phys. Rep.
#: 261, 271), 30 % of it or more.
UNCERTAIN_CZ_KMS = 1000.0

#: Where a source's distance came from (``Placement.origin``).
FROM_NOTHING, FROM_REDSHIFT_SPACE, FROM_COLUMN, FROM_REDSHIFT = 0, 1, 2, 3
#: Why a source has no distance (``Placement.reason``), in the desktop's words.
REASONS = {1: "z ≤ 0", 2: "z = 0 (not measured)", 3: "no redshift"}


def _cosmology_model(model: str):
    from .routers.cosmology import _load_cosmology_models  # noqa: PLC0415
    if model == CUSTOM_COSMOLOGY:
        raise ValueError("A custom cosmology is not known to the backend, which knows "
                         "Planck18, Planck15, Planck13 and WMAP9 only.")
    models = _load_cosmology_models()
    if model not in models:
        raise ValueError(f"Unknown cosmology model {model!r}.")
    return models[model]


def hubble_constant(model: str = "Planck18") -> float:
    """H0 in km/s/Mpc of a named model, for the redshift-space distance
    c z / H0 (it was Planck18's under every model, issue #260)."""
    if model == "Planck18":
        return _H0
    return float(_cosmology_model(model).H0.value)


def _model_comoving_mpc(model: str, z: np.ndarray) -> np.ndarray:
    cosmo = _cosmology_model(model)
    return _on_a_grid(z, lambda zs: cosmo.comoving_distance(zs).to("Mpc").value)


@dataclass
class Placement:
    """The radial distance of each source and where it came from."""
    distances: np.ndarray    # Mpc, NaN where the source is not placed
    origin: np.ndarray       # int8, FROM_* per source (-1: no direction, sky_placement)
    reason: np.ndarray       # int8, a REASONS key where not placed, else 0
    uncertain: np.ndarray    # bool: placed from a redshift with 0 < cz < UNCERTAIN_CZ_KMS

    def counts(self) -> dict:
        """Placed, unplaced and uncertain sources, for a status line (an
        origin of -1, a row with no direction, is in none of them)."""
        return {"placed": int(np.count_nonzero(self.origin > FROM_NOTHING)),
                "unplaced": int(np.count_nonzero(self.origin == FROM_NOTHING)),
                "uncertain": int(np.count_nonzero(self.uncertain))}


def placement(frame: pd.DataFrame, distance_mode: str = "comoving",
              cosmology: str = "Planck18") -> Placement:
    """The radial distance of each source, by the rules in the module doc."""
    if distance_mode not in DISTANCE_MODES:
        raise ValueError(f"Unknown distance mode {distance_mode!r}: one of {DISTANCE_MODES}.")
    # Refused before anything is computed, whether or not a row needs the model.
    h0 = hubble_constant(cosmology)
    n = len(frame)
    z = _numeric(frame, _first(frame, _REDSHIFT_ALIASES))
    with np.errstate(invalid="ignore"):
        has_z = np.isfinite(z) & (z > 0)
    d = np.full(n, np.nan)
    origin = np.zeros(n, dtype=np.int8)

    def take(mask, values, code):
        mask = mask & (origin == FROM_NOTHING) & np.isfinite(values)
        d[mask] = values[mask]
        origin[mask] = code

    if distance_mode == "redshift_space":
        take(has_z, _C_KMS * z / h0, FROM_REDSHIFT_SPACE)
    column = _first(frame, _DISTANCE_ALIASES)
    if column:
        dist = _numeric(frame, column)
        kind = _normalize(column)
        converted = dist.copy()
        if kind.startswith("DL") or kind == "LUMINOSITYDISTANCE":
            converted = np.where(has_z, dist / (1.0 + z), dist)
        elif kind.startswith("DA") or kind == "ANGULARDIAMETERDISTANCE":
            converted = np.where(has_z, dist * (1.0 + z), dist)
        with np.errstate(invalid="ignore"):
            take(np.isfinite(dist) & (dist > 0), converted, FROM_COLUMN)
    # The model's comoving distance only where nothing above placed the row.
    need = has_z & (origin == FROM_NOTHING)
    if need.any():
        zn = np.where(need, z, np.nan)
        comoving = (planck18_comoving_mpc(zn) if cosmology == "Planck18"
                    else _model_comoving_mpc(cosmology, zn))
        take(need, comoving, FROM_REDSHIFT)
    none = origin == FROM_NOTHING
    reason = np.zeros(n, dtype=np.int8)
    with np.errstate(invalid="ignore"):
        reason[none & (z < 0)] = 1
        reason[none & (z == 0)] = 2
    reason[none & ~np.isfinite(z)] = 3
    from_z = (origin == FROM_REDSHIFT_SPACE) | (origin == FROM_REDSHIFT)
    with np.errstate(invalid="ignore"):
        uncertain = from_z & (_C_KMS * z < UNCERTAIN_CZ_KMS)
    return Placement(d, origin, reason, uncertain)


def distances_mpc(frame: pd.DataFrame, distance_mode: str = "comoving",
                  cosmology: str = "Planck18") -> np.ndarray:
    """The radial distance of each source, NaN where it has none."""
    return placement(frame, distance_mode, cosmology).distances


def sky_positions(frame: pd.DataFrame, ra_field: str, dec_field: str, *,
                  frame_name: str = "fk5", distance_mode: str = "comoving",
                  cosmology: str = "Planck18") -> np.ndarray:
    """(N, 3) scene positions, NaN for a source without a finite RA/Dec, with
    a Dec beyond the poles, or without a distance."""
    return sky_placement(frame, ra_field, dec_field, frame_name=frame_name,
                         distance_mode=distance_mode, cosmology=cosmology)[0]


def sky_placement(frame: pd.DataFrame, ra_field: str, dec_field: str, *,
                  frame_name: str = "fk5", distance_mode: str = "comoving",
                  cosmology: str = "Planck18") -> tuple[np.ndarray, Placement]:
    """:func:`sky_positions` and the :class:`Placement` of the sources that
    have a direction (a row without one is in none of its counts)."""
    if frame_name not in FRAMES:
        raise ValueError(f"Unknown frame {frame_name!r}: one of {FRAMES}.")
    if distance_mode not in DISTANCE_MODES:
        raise ValueError(f"Unknown distance mode {distance_mode!r}: one of {DISTANCE_MODES}.")
    # In the unit the table declares (hours of RA, radians...), as the cube
    # overlay reads it (app/cube_catalogue.py angle_degrees); else degrees.
    from .cube_catalogue import angle_degrees  # noqa: PLC0415
    units = (getattr(frame, "attrs", {}) or {}).get("units") or {}
    ra = angle_degrees(_numeric(frame, ra_field), ra_field, units.get(ra_field))
    dec = angle_degrees(_numeric(frame, dec_field), dec_field, units.get(dec_field))
    # One row past a pole would make SkyCoord refuse the whole column.
    with np.errstate(invalid="ignore"):
        ok = np.isfinite(ra) & (np.abs(dec) <= 90.0)
    lon, lat = np.where(ok, ra, np.nan), np.where(ok, dec, np.nan)
    if frame_name != "fk5":
        from astropy import units as u  # noqa: PLC0415
        from astropy.coordinates import SkyCoord  # noqa: PLC0415
        lon, lat = np.full(len(ra), np.nan), np.full(len(dec), np.nan)
        if ok.any():
            sky = SkyCoord(ra=ra[ok] * u.deg, dec=dec[ok] * u.deg, frame="fk5", equinox="J2000")
            if frame_name == "galactic":
                g = sky.galactic
                lon[ok], lat[ok] = g.l.deg, g.b.deg
            else:
                sg = sky.supergalactic
                lon[ok], lat[ok] = sg.sgl.deg, sg.sgb.deg
    placed = placement(frame, distance_mode, cosmology)
    d = placed.distances
    lon_r, lat_r = np.radians(lon), np.radians(lat)
    xyz = np.stack([d * np.cos(lat_r) * np.cos(lon_r),
                    d * np.cos(lat_r) * np.sin(lon_r),
                    d * np.sin(lat_r)], axis=1)
    # Counted among the sources with a direction only.
    placed.origin = np.where(ok, placed.origin, -1).astype(np.int8)
    placed.reason = np.where(ok, placed.reason, 0).astype(np.int8)
    placed.uncertain = placed.uncertain & ok
    return xyz, placed
