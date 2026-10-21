"""Kinematic fit of a selected source — the comparator's input, measured in the app.

From a cube and a selection mask (the Kinematic Lasso's, as shown), on the RAW cube
under the mask:

* source measures — moment 0 (Jy km/s per pixel), an S/N-cut moment-1 velocity
  field, the integrated spectrum and flux (Jy km/s), the w20 / w50 widths and the
  systemic velocity at the w20 midpoint;
* geometry from the moment-0 light distribution — centre, position angle, axis
  ratio and inclination, disc radius;
* a flat tilted ring fitted to the velocity field — kinematic PA, V_sys, V_rot —
  in exactly the frame the model is evaluated in;
* a per-ring profile — PA and V_rot per annulus, inclination per ring from the
  moment-0 isophotes (never from the velocity field, where V_rot and sin i are
  degenerate);
* the model velocity fields and the residual |v_data − v_model| for both.

The kinematic model (``app.kinematic_model``) and this fit share one velocity axis,
:func:`velocity_axis_kms`, so what is fitted here is what the overlay draws. A
frequency axis is converted with the optical convention (cz = c (ν₀/ν − 1)), the
one HI galaxy catalogues quote.

This is an exploratory comparator, not a 3-D fit: the model is a kinematic
footprint, not convolved with the beam, so inner rings are beam-smeared.
"""
from __future__ import annotations

import base64
import math
from typing import Any

import numpy as np

C_KMS = 299792.458

# A ring is fitted only where it has enough pixels to constrain two parameters
# (about five synthesised beams of area on a WALLABY cubelet).
DEFAULT_MIN_RING_PIX = 150


# ── spectral axis ──────────────────────────────────────────────────────────────

def velocity_axis_kms(coords: np.ndarray, axis_unit: str, axis_type: str,
                      restfreq_hz: float) -> tuple[np.ndarray, str, str]:
    """Native spectral coordinates → velocity in km/s, ``(values, unit, mode)``.

    A velocity axis is read as it is (its own convention); a frequency axis with a
    rest frequency becomes optical velocity, cz = c (ν₀/ν − 1). Otherwise the axis
    stays native (``mode="native"``) and the caller must say so.
    """
    coords = np.asarray(coords, dtype=np.float64).ravel()
    try:
        import astropy.units as u  # noqa: PLC0415
        unit = u.Unit(axis_unit) if axis_unit else u.dimensionless_unscaled
        if unit.is_equivalent(u.km / u.s):
            return (coords * unit).to(u.km / u.s).value, "km/s", "velocity"
        if unit.is_equivalent(u.Hz) and math.isfinite(restfreq_hz) and restfreq_hz > 0:
            freq_hz = (coords * unit).to(u.Hz).value
            with np.errstate(divide="ignore", invalid="ignore"):
                return C_KMS * (restfreq_hz / freq_hz - 1.0), "km/s", "velocity"
    except Exception:  # noqa: BLE001 — an unparsable unit stays native
        pass
    return coords, (axis_unit or ""), "native"


def channel_edges(coords: np.ndarray) -> np.ndarray:
    """Half-channel edges of a (locally linear) native axis: n + 1 values."""
    c = np.asarray(coords, dtype=np.float64).ravel()
    if c.size == 1:
        return np.array([c[0] - 0.5, c[0] + 0.5])
    mid = 0.5 * (c[1:] + c[:-1])
    return np.concatenate([[c[0] - (mid[0] - c[0])], mid, [c[-1] + (c[-1] - mid[-1])]])


def cube_velocity_axis(dataset, depth: int) -> dict[str, Any]:
    """Velocity per channel, each channel's own width (km/s) and the convention,
    for a :class:`ScientificFitsDataset`. Channels uniform in frequency are not
    uniform in optical velocity, so integrals weight each channel by its width."""
    spectral = dataset.spectral_axis_metadata(0, depth - 1)
    unit = str(spectral.get("axis_unit", "") or "")
    atype = str(spectral.get("axis_type", "") or "")
    coords = np.asarray(spectral.get("coordinates", []), dtype=np.float64).ravel()
    if coords.size != depth or not np.all(np.isfinite(coords)):
        raise ValueError("The cube's spectral axis has no usable WCS.")
    restfreq = 0.0
    for key in ("RESTFRQ", "RESTFREQ"):
        try:
            restfreq = float(dataset.header.get(key, 0.0) or 0.0)
        except (TypeError, ValueError):
            restfreq = 0.0
        if restfreq:
            break
    vel, vunit, mode = velocity_axis_kms(coords, unit, atype, restfreq)
    if mode != "velocity":
        raise ValueError(
            "The spectral axis cannot be expressed in km/s (a frequency axis needs RESTFRQ).")
    edges, _, _ = velocity_axis_kms(channel_edges(coords), unit, atype, restfreq)
    convention = ("optical (cz), from frequency" if _is_frequency(unit)
                  else f"as in the cube ({atype or unit})")
    return {"vel": vel, "dvk": np.abs(np.diff(edges)), "unit": vunit, "convention": convention}


def _is_frequency(unit: str) -> bool:
    try:
        import astropy.units as u  # noqa: PLC0415
        return u.Unit(unit).is_equivalent(u.Hz)
    except Exception:  # noqa: BLE001
        return False


# ── measures ───────────────────────────────────────────────────────────────────

def robust_sigma(data: np.ndarray) -> float:
    """1.4826 × MAD of the cube — the lasso's own estimator (a strided sample on a
    large cube, every voxel on a small one), so the fit and the selection agree."""
    from .selection import _robust_sigma  # noqa: PLC0415
    return float(_robust_sigma(data))


def source_measures(data: np.ndarray, mask: np.ndarray, vel: np.ndarray, dvk: np.ndarray,
                    beam_px: float | None, sigma: float, snr: float = 3.0) -> dict[str, Any]:
    """Moments, spectrum, flux and widths on the RAW cube under ``mask``.

    The velocity field keeps a pixel only where its summed emission clears
    ``snr`` × σ × √(channels summed) — the noise of a sum grows as the square root
    of its terms — and where the mean velocity lies inside the band: a pixel whose
    masked channels sum to a hair above zero would give an arbitrary velocity and
    dominate any fit.
    """
    beam = float(beam_px) if beam_px and beam_px > 0 else 1.0
    d = np.where(mask & np.isfinite(data), data, np.nan)
    m0 = np.nansum(d * dvk[:, None, None], axis=0) / beam          # Jy km/s per pixel
    with np.errstate(invalid="ignore", divide="ignore"):
        wsum = np.nansum(d, axis=0)
        m1 = np.nansum(d * vel[:, None, None], axis=0) / wsum
    nchan = np.nansum(mask, axis=0)
    snr_ok = wsum > snr * sigma * np.sqrt(np.maximum(nchan, 1))
    in_band = (m1 >= vel.min()) & (m1 <= vel.max())
    m1 = np.where(np.isfinite(wsum) & snr_ok & in_band, m1, np.nan)

    spectrum = np.nansum(d, axis=(1, 2)) / beam                   # Jy per channel
    flux = float(np.nansum(spectrum * dvk))                       # Jy km/s
    peak = float(np.nanmax(spectrum)) if spectrum.size else float("nan")

    def width_at(frac: float) -> tuple[float, float]:
        sel = spectrum > frac * peak
        if not np.any(sel):
            return float("nan"), float("nan")
        lo, hi = float(vel[sel].min()), float(vel[sel].max())
        return hi - lo, 0.5 * (lo + hi)

    w20, mid20 = width_at(0.2)
    w50, _ = width_at(0.5)
    return {"m0": m0, "m1": m1, "spectrum": spectrum, "flux": flux,
            "flux_is_jy": beam_px is not None and beam_px > 0,
            "w20": w20, "w50": w50, "vsys_w20": mid20,
            "n_velocity_field": int(np.isfinite(m1).sum()), "sigma": float(sigma)}


# ── geometry ───────────────────────────────────────────────────────────────────

def _second_moments(weights: np.ndarray, xx: np.ndarray, yy: np.ndarray):
    tot = float(weights.sum())
    cx = float((weights * xx).sum() / tot)
    cy = float((weights * yy).sum() / tot)
    dx, dy = xx - cx, yy - cy
    mxx = float((weights * dx * dx).sum() / tot)
    myy = float((weights * dy * dy).sum() / tot)
    mxy = float((weights * dx * dy).sum() / tot)
    common, diff = mxx + myy, math.hypot(mxx - myy, 2 * mxy)
    a = math.sqrt(max(0.5 * (common + diff), 1e-9))
    b = math.sqrt(max(0.5 * (common - diff), 1e-9))
    theta = 0.5 * math.atan2(2 * mxy, mxx - myy)
    return cx, cy, a, b, theta


def geometry_from_m0(m0: np.ndarray) -> dict[str, float]:
    """Centre, morphological PA, axis ratio, inclination (cos i = b/a) and disc
    radius (2.5 σ of the light) from the moment-0 second moments."""
    ny, nx = m0.shape
    good = np.isfinite(m0) & (m0 > 0)
    if not np.any(good):
        raise ValueError("The selection has no positive moment-0 pixels.")
    yy, xx = np.mgrid[0:ny, 0:nx]
    cx, cy, a, b, theta = _second_moments(np.where(good, m0, 0.0), xx, yy)
    ratio = b / a
    return {"x0": cx, "y0": cy, "pa_morph": float((math.degrees(theta) + 90.0) % 180.0),
            "axis_ratio": float(ratio),
            "incl": float(math.degrees(math.acos(min(max(ratio, 0.0), 1.0)))),
            "rmax": float(2.5 * a)}


def isophotal_inclinations(m0: np.ndarray, min_pix: int = 60) -> tuple[np.ndarray, np.ndarray]:
    """(radius, inclination) of moment-0 isophotes at a falling series of levels:
    the shape of the emission, the only honest source of a per-ring inclination."""
    ny, nx = m0.shape
    good = np.isfinite(m0) & (m0 > 0)
    yy, xx = np.mgrid[0:ny, 0:nx]
    levels = np.percentile(m0[good], [92, 84, 76, 68, 60, 52, 44, 36, 28, 20])
    radii, incls = [], []
    for lev in levels:
        sel = good & (m0 >= lev)
        if int(sel.sum()) < min_pix:
            continue
        _, _, a, b, _ = _second_moments(np.where(sel, m0, 0.0), xx, yy)
        radii.append(2.0 * a)
        incls.append(float(math.degrees(math.acos(min(max(b / a, 0.0), 1.0)))))
    r, i = np.asarray(radii), np.asarray(incls)
    order = np.argsort(r)
    return r[order], i[order]


# ── kinematics ─────────────────────────────────────────────────────────────────

def deproject(dxc: np.ndarray, dyc: np.ndarray, pa_deg: float, incl_deg: float):
    """cos θ and radius in the disc for a PA / inclination pair — the deprojection
    :func:`app.kinematic_model.tilted_ring_model` applies."""
    par = math.radians(pa_deg)
    ci = max(math.cos(math.radians(incl_deg)), 1e-3)
    xd = (-dxc * math.sin(par) + dyc * math.cos(par))
    yd = (-dxc * math.cos(par) - dyc * math.sin(par)) / ci
    rr = np.hypot(xd, yd)
    with np.errstate(invalid="ignore", divide="ignore"):
        cost = np.where(rr > 0, xd / rr, 0.0)
    return cost, rr


def fit_flat(m1: np.ndarray, m0: np.ndarray, geom: dict[str, float]) -> dict[str, float]:
    """Kinematic PA, V_sys and V_rot of a flat disc on the velocity field.

    The position angle is scanned over the full circle in the model's own frame
    and, at each, V_sys and V_rot solved by least squares; a negative amplitude is
    the same disc seen from the other side and is folded into the PA.
    """
    ny, nx = m1.shape
    yy, xx = np.mgrid[0:ny, 0:nx]
    dxc, dyc = xx - geom["x0"], yy - geom["y0"]
    ok = np.isfinite(m1) & np.isfinite(m0) & (m0 > 0)
    if int(ok.sum()) < 10:
        raise ValueError("Too few velocity-field pixels above the S/N cut to fit.")
    si = math.sin(math.radians(geom["incl"]))
    best = None
    for pa_try in np.arange(0.0, 360.0, 1.0):
        cost, _ = deproject(dxc, dyc, float(pa_try), geom["incl"])
        a = np.column_stack([np.ones(int(ok.sum())), cost[ok] * si])
        sol, *_ = np.linalg.lstsq(a, m1[ok], rcond=None)
        rms = float(np.sqrt(np.mean((a @ sol - m1[ok]) ** 2)))
        if best is None or rms < best[0]:
            best = (rms, float(pa_try), float(sol[0]), float(sol[1]))
    rms, pa, vsys, vrot = best
    if vrot < 0.0:
        pa, vrot = (pa + 180.0) % 360.0, -vrot
    return {"pa": pa, "vsys": vsys, "vrot": vrot, "vrot_sini": vrot * si, "rms": rms}


def fit_rings(m1: np.ndarray, m0: np.ndarray, geom: dict[str, float], flat: dict[str, float],
              min_ring_pix: int = DEFAULT_MIN_RING_PIX) -> list[dict[str, float]]:
    """PA and V_rot per annulus around the flat solution; inclination per ring from
    the isophotes (V_rot re-solved at it, since V_rot and sin i trade off)."""
    ny, nx = m1.shape
    yy, xx = np.mgrid[0:ny, 0:nx]
    dxc, dyc = xx - geom["x0"], yy - geom["y0"]
    ok = np.isfinite(m1) & np.isfinite(m0) & (m0 > 0)
    pa, incl, vsys, rmax = flat["pa"], geom["incl"], flat["vsys"], geom["rmax"]
    si = math.sin(math.radians(incl))
    iso_r, iso_i = isophotal_inclinations(m0)
    _, rad = deproject(dxc, dyc, pa, incl)
    edges = np.arange(0.0, rmax + 1e-9, max(rmax / 8.0, 4.0))
    rings = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        sel = ok & (rad >= lo) & (rad < hi)
        if int(sel.sum()) < min_ring_pix:
            continue
        best = None
        for pa_try in np.arange(pa - 40.0, pa + 40.0, 2.0):
            ct, _ = deproject(dxc, dyc, float(pa_try), incl)
            basis = ct[sel] * si
            denom = float(np.sum(basis * basis))
            if denom <= 1e-9:
                continue
            vr = float(np.sum((m1[sel] - vsys) * basis) / denom)
            r_ = float(np.sqrt(np.mean((vsys + vr * basis - m1[sel]) ** 2)))
            if best is None or r_ < best[0]:
                best = (r_, float(pa_try), vr)
        if best is None:
            continue
        r_mid = 0.5 * (lo + hi)
        incl_r = float(np.interp(r_mid, iso_r, iso_i)) if iso_r.size >= 3 else incl
        ct, _ = deproject(dxc, dyc, best[1], incl_r)
        basis = ct[sel] * math.sin(math.radians(incl_r))
        den = float(np.sum(basis * basis))
        vr_r = float(np.sum((m1[sel] - vsys) * basis) / den) if den > 1e-9 else best[2]
        # The PA stays continuous across rings (e.g. 356 → 362, not → 2): the
        # profile model interpolates it between rings.
        rings.append({"r": float(r_mid), "pa": best[1], "incl": incl_r,
                      "vrot": vr_r, "n_pix": int(sel.sum())})
    return rings


def model_velocity_field(model: np.ndarray, vel: np.ndarray) -> np.ndarray:
    """Intensity-weighted mean velocity of a model cube, NaN where it is empty."""
    mw = np.nansum(model, axis=0)
    with np.errstate(invalid="ignore", divide="ignore"):
        v = np.nansum(model * vel[:, None, None], axis=0) / mw
    return np.where(mw > 0, v, np.nan)


def residual_stats(m1: np.ndarray, vmod: np.ndarray, dv: float) -> dict[str, Any]:
    both = np.isfinite(m1) & np.isfinite(vmod)
    r = np.abs(m1[both] - vmod[both])
    if r.size == 0:
        return {"n_pix": 0, "median": float("nan"), "p90": float("nan"),
                "within_channel": float("nan")}
    return {"n_pix": int(r.size), "median": float(np.median(r)),
            "p90": float(np.percentile(r, 90)), "within_channel": float(np.mean(r <= dv))}


def kinematic_fit(data: np.ndarray, mask: np.ndarray, vel: np.ndarray, dvk: np.ndarray,
                  beam_px: float | None, sigma: float,
                  min_ring_pix: int = DEFAULT_MIN_RING_PIX,
                  cap: int | None = None) -> dict[str, Any]:
    """The whole comparator input from a selection: measures, geometry, flat and
    per-ring solutions, model velocity fields and residuals (arrays included)."""
    from ..kinematic_model import tilted_ring_model, tilted_ring_model_profile  # noqa: PLC0415

    nz, ny, nx = data.shape
    meas = source_measures(data, mask, vel, dvk, beam_px, sigma)
    geom = geometry_from_m0(meas["m0"])
    flat = fit_flat(meas["m1"], meas["m0"], geom)
    dv = float(np.mean(np.abs(np.diff(vel)))) if vel.size > 1 else 1.0
    line_sigma = max(2.0 * dv, 8.0)
    model = tilted_ring_model(nx, ny, vel, geom["x0"], geom["y0"], flat["pa"], geom["incl"],
                              flat["vsys"], flat["vrot"], geom["rmax"], sigma=line_sigma,
                              cap=cap)
    vmod_flat = model_velocity_field(model, vel)
    del model
    rings = fit_rings(meas["m1"], meas["m0"], geom, flat, min_ring_pix)
    vmod_warp = None
    if len(rings) >= 3:
        prof = tilted_ring_model_profile(
            nx, ny, vel, geom["x0"], geom["y0"],
            [r["r"] for r in rings], [r["pa"] for r in rings],
            [r["incl"] for r in rings], [r["vrot"] for r in rings],
            vsys=flat["vsys"], sigma=line_sigma, cap=cap)
        vmod_warp = model_velocity_field(prof, vel)
        del prof
    return {"measures": meas, "geometry": geom, "flat": flat, "rings": rings,
            "line_sigma": line_sigma, "dv": dv,
            "vmod_flat": vmod_flat, "resid_flat": residual_stats(meas["m1"], vmod_flat, dv),
            "vmod_warp": vmod_warp,
            "resid_warp": (residual_stats(meas["m1"], vmod_warp, dv)
                           if vmod_warp is not None else None)}


def model_residual(data: np.ndarray, mask: np.ndarray, vel: np.ndarray, dvk: np.ndarray,
                   sigma: float, x0: float, y0: float, vsys: float, line_sigma: float,
                   pa: float | None = None, incl: float | None = None,
                   vrot: float | None = None, rmax: float | None = None,
                   rings: list[tuple[float, float, float, float]] | None = None,
                   cap: int | None = None) -> dict[str, Any]:
    """The residual of a GIVEN model — the one in the comparator's fields, fitted
    or edited — against the selection's velocity field: so editing a ring (or
    dropping one) says what it does to the residual."""
    from ..kinematic_model import tilted_ring_model, tilted_ring_model_profile  # noqa: PLC0415

    nz, ny, nx = data.shape
    m1 = source_measures(data, mask, vel, dvk, None, sigma)["m1"]
    if rings:
        model = tilted_ring_model_profile(
            nx, ny, vel, x0, y0, [r[0] for r in rings], [r[1] for r in rings],
            [r[2] for r in rings], [r[3] for r in rings], vsys=vsys, sigma=line_sigma, cap=cap)
    else:
        if None in (pa, incl, vrot, rmax):
            raise ValueError("A single-disc model needs pa, incl, vrot and rmax.")
        model = tilted_ring_model(nx, ny, vel, x0, y0, float(pa), float(incl), vsys,
                                  float(vrot), float(rmax), sigma=line_sigma, cap=cap)
    vmod = model_velocity_field(model, vel)
    del model
    dv = float(np.mean(np.abs(np.diff(vel)))) if vel.size > 1 else 1.0
    return {"m1": m1, "vmod": vmod, "stats": residual_stats(m1, vmod, dv)}


# ── worker ─────────────────────────────────────────────────────────────────────

def _json_safe(value: Any) -> Any:
    """NaN/inf → None, recursively: JSON has no NaN, and a width or a residual
    that could not be measured is "unknown", not a number."""
    if isinstance(value, dict):
        return {k: _json_safe(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(v) for v in value]
    if isinstance(value, (float, np.floating)):
        return float(value) if math.isfinite(float(value)) else None
    if isinstance(value, np.integer):
        return int(value)
    if isinstance(value, np.bool_):
        return bool(value)
    return value


def _b64(a: np.ndarray | None) -> str:
    if a is None:
        return ""
    return base64.b64encode(np.ascontiguousarray(a, dtype="<f4").tobytes()).decode("ascii")


def _load_selection(path: str, mask_bytes: bytes, origin_x: int, origin_y: int, origin_z: int,
                    roi_width: int, roi_height: int, roi_depth: int,
                    sigma: float | None, cap: int | None):
    """The cube (refusing transposed or oversized ones), the selection as a full
    mask, the velocity axis, the noise σ and its source, the beam in pixels."""
    from ..fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    from ._common import beam_area_pixels, is_per_beam_bunit  # noqa: PLC0415

    rw, rh, rd = int(roi_width), int(roi_height), int(roi_depth)
    if rw <= 0 or rh <= 0 or rd <= 0 or len(mask_bytes) != rw * rh * rd * 4:
        raise ValueError(f"Mask payload is {len(mask_bytes)} bytes; expected {rw * rh * rd * 4}.")
    roi = np.frombuffer(mask_bytes, dtype="<f4").reshape(rd, rh, rw) > 0.5
    if not roi.any():
        raise ValueError("The selection is empty.")

    from ..kinematic_model import ModelTooLargeError, max_voxels  # noqa: PLC0415

    ds = ScientificFitsDataset(path)
    # squeeze_to_3d yields (spectral, y, x) only when the spectral axis is the third
    # kept FITS axis; a transposed cube would be fitted along a spatial axis.
    kept = ds.active_axes[:3]
    if len(kept) == 3 and ds.cube_numpy_spectral_fits_axis() != kept[2]:
        raise ValueError("A kinematic fit needs the spectral axis to be the cube's last "
                         "axis (e.g. NAXIS3); this cube's spectral axis is elsewhere.")
    cap = max_voxels() if cap is None else int(cap)   # callers pass it resolved
    hdul, raw = ds.open_memmap()
    try:
        view = ds.squeeze_to_3d(raw)
        if view.ndim != 3:
            raise ValueError("A kinematic fit needs a 3-D cube.")
        # The fit holds the cube and builds model cubes of the same size: the
        # model's own voxel cap bounds it, checked before anything is read.
        if view.size > cap:
            raise ModelTooLargeError(
                f"The cube has {view.size} voxels, over the {cap}-voxel limit of the "
                "kinematic fit; crop it to the source (Export Sub-Cube) or raise "
                "VISIVO_KINEMATIC_MAX_VOXELS.")
        data = np.asarray(view, dtype=np.float32)
    finally:
        hdul.close()
    nz, ny, nx = data.shape
    mask = np.zeros(data.shape, dtype=bool)
    z0, y0, x0 = int(origin_z), int(origin_y), int(origin_x)
    z1, y1, x1 = min(nz, z0 + rd), min(ny, y0 + rh), min(nx, x0 + rw)
    if z0 < 0 or y0 < 0 or x0 < 0 or z1 <= z0 or y1 <= y0 or x1 <= x0:
        raise ValueError("The selection lies outside the cube.")
    mask[z0:z1, y0:y1, x0:x1] = roi[: z1 - z0, : y1 - y0, : x1 - x0]

    axis = cube_velocity_axis(ds, nz)
    beam_px = None
    beam = ds.beam
    if is_per_beam_bunit(ds.bunit) and beam.major and beam.minor:
        beam_px = beam_area_pixels(beam.major, beam.minor, ds.spatial_pixel_scale_arcsec(),
                                   ds.spatial_pixel_area_arcsec2())
    noise = float(sigma) if sigma and sigma > 0 and math.isfinite(sigma) else robust_sigma(data)
    sigma_source = "noise estimate" if sigma and sigma > 0 else "robust, whole cube"

    return data, mask, axis, noise, sigma_source, beam_px


def worker_kinematic_fit(path: str, mask_bytes: bytes,
                         origin_x: int, origin_y: int, origin_z: int,
                         roi_width: int, roi_height: int, roi_depth: int,
                         sigma: float | None = None,
                         min_ring_pix: int = DEFAULT_MIN_RING_PIX,
                         cap: int | None = None) -> dict[str, Any]:
    """Fit a selection of ``path``: the lasso's explicit mask (ROI float32 0/1,
    x-fastest, at ``origin``) on the raw cube. ``sigma`` is the noise to cut the
    velocity field with — the session's estimate when the client has one — else
    the cube's own 1.4826·MAD over every finite voxel."""
    from ..kinematic_model import max_voxels  # noqa: PLC0415
    cap = max_voxels() if cap is None else int(cap)
    data, mask, axis, noise, sigma_source, beam_px = _load_selection(
        path, mask_bytes, origin_x, origin_y, origin_z, roi_width, roi_height, roi_depth,
        sigma, cap)
    fit = kinematic_fit(data, mask, axis["vel"], axis["dvk"], beam_px, noise, int(min_ring_pix),
                        cap=cap)
    meas = fit["measures"]
    ny, nx = data.shape[1:]
    return _json_safe({
        "width": nx, "height": ny,
        "velocity_unit": axis["unit"], "velocity_convention": axis["convention"],
        "sigma": noise, "sigma_source": sigma_source,
        "beam_area_px": beam_px if beam_px else 0.0,
        "measures": {k: meas[k] for k in ("flux", "flux_is_jy", "w20", "w50", "vsys_w20",
                                          "n_velocity_field")},
        "geometry": fit["geometry"], "flat": fit["flat"], "rings": fit["rings"],
        "line_sigma": fit["line_sigma"], "channel_width": fit["dv"],
        "residual_flat": fit["resid_flat"], "residual_warp": fit["resid_warp"],
        "spectrum": [float(v) for v in meas["spectrum"]],
        "spectrum_velocity": [float(v) for v in axis["vel"]],
        "velocity_field_base64": _b64(meas["m1"]),
        "model_flat_base64": _b64(fit["vmod_flat"]),
        "model_warp_base64": _b64(fit["vmod_warp"]),
    })


def worker_kinematic_residual(path: str, mask_bytes: bytes,
                              origin_x: int, origin_y: int, origin_z: int,
                              roi_width: int, roi_height: int, roi_depth: int,
                              sigma: float | None, x0: float, y0: float, vsys: float,
                              line_sigma: float, pa: float | None, incl: float | None,
                              vrot: float | None, rmax: float | None,
                              rings: list[tuple[float, float, float, float]] | None,
                              cap: int | None = None) -> dict[str, Any]:
    """Residual of the model given by the parameters against the selection's
    velocity field (same selection wire and same σ rule as worker_kinematic_fit)."""
    from ..kinematic_model import max_voxels  # noqa: PLC0415
    cap = max_voxels() if cap is None else int(cap)
    data, mask, axis, noise, sigma_source, _ = _load_selection(
        path, mask_bytes, origin_x, origin_y, origin_z, roi_width, roi_height, roi_depth,
        sigma, cap)
    out = model_residual(data, mask, axis["vel"], axis["dvk"], noise, x0, y0, vsys, line_sigma,
                         pa, incl, vrot, rmax, rings, cap=cap)
    ny, nx = out["m1"].shape
    return _json_safe({
        "width": nx, "height": ny, "sigma": noise, "sigma_source": sigma_source,
        "velocity_convention": axis["convention"], "residual": out["stats"],
        "model_base64": _b64(out["vmod"]),
        "velocity_field_base64": _b64(out["m1"]),
    })
