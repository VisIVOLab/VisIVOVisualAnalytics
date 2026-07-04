"""Tilted-ring kinematic model of a rotating gas disk — F2.

Builds a model position–position–velocity (PPV) cube from a compact tilted-ring
parameter set. Overlaid on an observed HI cube it shows where a regularly
rotating disk *should* emit, so gas that deviates (extra-planar / anomalous /
inflow-outflow) stands out — the SlicerAstro / van der Hulst use case.

Phase-1 model: a single inclination/PA disk with a flat (constant) rotation
velocity out to ``rmax`` and a Gaussian line profile of width ``sigma``. The
emission at sky pixel (x, y) sits at the line-of-sight velocity

    Vlos(x, y) = Vsys + Vrot · sin(incl) · cos(θ)

where r, θ are the galactocentric radius / azimuth from the standard tilted-ring
deprojection (centre x0,y0; position angle pa; inclination incl).

Conventions / scope (phase-1):
  * The model is built in **velocity** (km/s): the worker converts the cube's
    spectral axis (FREQ→radio-convention velocity via RESTFRQ, or a native
    velocity axis) to km/s, so ``vsys/vrot/sigma`` are always km/s. The GUI is
    told the working unit via the response and labels its fields accordingly.
  * ``pa_deg`` is measured in **image-pixel** coordinates (PA=0 → disk major
    axis along +y of the array; increasing toward +x), not sky north-through-
    east. The viewer tunes PA visually against the data, so this is sufficient
    here; deriving sky PA from the celestial WCS is a later refinement.
  * Per-ring rotation curves and a non-uniform disk intensity are later work.
"""
from __future__ import annotations

import math
import os
from typing import Any

import numpy as np

_C_KMS = 299792.458  # speed of light, km/s

# Hard cap on the generated model size (voxels). A model cube is materialised in
# full as float32 plus a raw bytes copy, so peak memory ≈ 2× this × 4 bytes.
# Default 256 M voxels ≈ 1 GiB cube + 1 GiB wire buffer. Override with
# VISIVO_KINEMATIC_MAX_VOXELS. Oversized requests are rejected (HTTP 413) rather
# than OOMing the worker.
_DEFAULT_MAX_VOXELS = 256 * 1024 * 1024


class ModelTooLargeError(ValueError):
    """Raised when the requested model cube exceeds the voxel cap."""


def max_voxels() -> int:
    """Resolve the voxel cap from the environment (read in the PARENT process so
    it survives the pre-forked worker pool), falling back to the default."""
    try:
        v = int(os.environ.get("VISIVO_KINEMATIC_MAX_VOXELS", ""))
        return v if v > 0 else _DEFAULT_MAX_VOXELS
    except (TypeError, ValueError):
        return _DEFAULT_MAX_VOXELS


def tilted_ring_model(nx: int, ny: int, spectral: np.ndarray, x0: float, y0: float,
                      pa_deg: float, incl_deg: float, vsys: float, vrot: float,
                      rmax_pix: float, sigma: float, cap: int | None = None) -> np.ndarray:
    """Return a model cube of shape (nchan, ny, nx), float32.

    `spectral` is the per-channel velocity axis, same unit as vsys/vrot/sigma
    (km/s in normal use). `x0,y0,rmax_pix` are in pixels; `pa_deg,incl_deg` in
    degrees. Each pixel carries a unit-amplitude Gaussian line centred on its
    line-of-sight velocity; the cube is NOT renormalised, so the realised peak
    depends on how close a channel falls to vlos. Raises
    :class:`ModelTooLargeError` if the cube would exceed ``cap`` voxels.
    """
    spectral = np.asarray(spectral, dtype=np.float64).ravel()
    nchan = spectral.size
    nvox = int(nchan) * int(ny) * int(nx)
    cap = _DEFAULT_MAX_VOXELS if cap is None else int(cap)
    if nvox > cap:
        raise ModelTooLargeError(
            f"Kinematic model cube too large: {nvox} voxels "
            f"({nx}×{ny}×{nchan}) exceeds the {cap}-voxel cap. "
            "Crop the cube (sub-region) or raise VISIVO_KINEMATIC_MAX_VOXELS."
        )

    pa = math.radians(pa_deg)
    incl = math.radians(incl_deg)
    cos_i = max(math.cos(incl), 1e-3)          # avoid /0 at edge-on
    sin_i = math.sin(incl)
    sigma = max(float(sigma), 1e-6)

    yy, xx = np.mgrid[0:ny, 0:nx].astype(np.float64)
    dx = xx - x0
    dy = yy - y0
    # Standard tilted-ring deprojection: rotate to the disk major/minor axes,
    # then stretch the minor axis by 1/cos(incl).
    x_d = (-dx * math.sin(pa) + dy * math.cos(pa))
    y_d = (-dx * math.cos(pa) - dy * math.sin(pa)) / cos_i
    r = np.sqrt(x_d * x_d + y_d * y_d)
    with np.errstate(invalid="ignore", divide="ignore"):
        cos_theta = np.where(r > 0, x_d / r, 0.0)
    vlos = vsys + vrot * sin_i * cos_theta      # (ny, nx)
    disk = r <= rmax_pix

    model = np.zeros((nchan, ny, nx), dtype=np.float32)
    # Gaussian line per pixel, centred on vlos. Loop channels (nchan small).
    inv = 1.0 / (2.0 * sigma * sigma)
    for c in range(nchan):
        dvel = spectral[c] - vlos
        plane = np.exp(-(dvel * dvel) * inv)
        model[c] = np.where(disk, plane, 0.0).astype(np.float32)
    return model


def tilted_ring_model_profile(nx: int, ny: int, spectral: np.ndarray, x0: float, y0: float,
                              ring_r: np.ndarray, ring_pa_deg: np.ndarray, ring_incl_deg: np.ndarray,
                              ring_vrot: np.ndarray, vsys: float, sigma: float,
                              cap: int | None = None, iterations: int = 2) -> np.ndarray:
    """Per-ring (warped) tilted-ring model → cube of shape (nchan, ny, nx), float32.

    Generalises :func:`tilted_ring_model` so the position angle, inclination and
    rotation velocity each vary with galactocentric radius (the 3DBarolo /
    TiRiFiC picture): ``ring_r`` are the ring radii in pixels (ascending) and
    ``ring_pa_deg / ring_incl_deg / ring_vrot`` the value at each ring. Values
    between rings are linearly interpolated; outside the ring range they are held
    at the nearest ring (``np.interp`` clamps). ``rmax`` = the outermost ring.

    Because a warp makes each pixel's deprojected radius depend on the *local*
    inclination/PA — which themselves depend on radius — the deprojection is
    solved by a few fixed-point iterations (``iterations``), starting from the
    innermost ring's geometry. A constant profile reduces exactly to the scalar
    :func:`tilted_ring_model`.
    """
    spectral = np.asarray(spectral, dtype=np.float64).ravel()
    nchan = spectral.size
    nvox = int(nchan) * int(ny) * int(nx)
    cap = _DEFAULT_MAX_VOXELS if cap is None else int(cap)
    if nvox > cap:
        raise ModelTooLargeError(
            f"Kinematic model cube too large: {nvox} voxels "
            f"({nx}×{ny}×{nchan}) exceeds the {cap}-voxel cap. "
            "Crop the cube (sub-region) or raise VISIVO_KINEMATIC_MAX_VOXELS."
        )

    ring_r = np.asarray(ring_r, dtype=np.float64).ravel()
    ring_pa = np.asarray(ring_pa_deg, dtype=np.float64).ravel()
    ring_incl = np.asarray(ring_incl_deg, dtype=np.float64).ravel()
    ring_v = np.asarray(ring_vrot, dtype=np.float64).ravel()
    if not (ring_r.size == ring_pa.size == ring_incl.size == ring_v.size) or ring_r.size == 0:
        raise ValueError("Per-ring profile arrays must be non-empty and the same length.")
    # np.interp requires ascending sample points.
    order = np.argsort(ring_r, kind="stable")
    ring_r, ring_pa, ring_incl, ring_v = ring_r[order], ring_pa[order], ring_incl[order], ring_v[order]
    rmax_pix = float(ring_r[-1])
    sigma = max(float(sigma), 1e-6)

    yy, xx = np.mgrid[0:ny, 0:nx].astype(np.float64)
    dx = xx - x0
    dy = yy - y0

    def _deproject(pa_map: np.ndarray, incl_map: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        cos_i = np.clip(np.cos(incl_map), 1e-3, None)
        x_d = -dx * np.sin(pa_map) + dy * np.cos(pa_map)
        y_d = (-dx * np.cos(pa_map) - dy * np.sin(pa_map)) / cos_i
        r = np.sqrt(x_d * x_d + y_d * y_d)
        with np.errstate(invalid="ignore", divide="ignore"):
            cos_theta = np.where(r > 0, x_d / r, 0.0)
        return r, cos_theta

    # Fixed-point deprojection: start from the innermost ring's geometry, then
    # re-evaluate PA/incl at each pixel's current radius and repeat.
    pa_map = np.full((ny, nx), math.radians(ring_pa[0]))
    incl_map = np.full((ny, nx), math.radians(ring_incl[0]))
    for _ in range(max(1, iterations)):
        r, _ = _deproject(pa_map, incl_map)
        rc = np.clip(r, ring_r[0], ring_r[-1])
        pa_map = np.radians(np.interp(rc, ring_r, ring_pa))
        incl_map = np.radians(np.interp(rc, ring_r, ring_incl))

    # Final deprojection with the converged geometry maps, so r / cos_theta are
    # consistent with the SAME pa_map / incl_map used for the velocity projection
    # (without this, r came from the previous iteration's geometry — wrong on a
    # warped disk). vrot is then read at that final radius.
    r, cos_theta = _deproject(pa_map, incl_map)
    rc = np.clip(r, ring_r[0], ring_r[-1])
    vrot_map = np.interp(rc, ring_r, ring_v)
    vlos = vsys + vrot_map * np.sin(incl_map) * cos_theta   # (ny, nx)
    disk = r <= rmax_pix

    model = np.zeros((nchan, ny, nx), dtype=np.float32)
    inv = 1.0 / (2.0 * sigma * sigma)
    for c in range(nchan):
        dvel = spectral[c] - vlos
        plane = np.exp(-(dvel * dvel) * inv)
        model[c] = np.where(disk, plane, 0.0).astype(np.float32)
    return model


def _spectral_axis_kms(coords: np.ndarray, axis_unit: str, axis_type: str,
                       restfreq_hz: float) -> tuple[np.ndarray, str, str]:
    """Convert a native spectral axis to radio-convention velocity in km/s.

    Returns ``(values, unit, mode)``:
      * mode="velocity", unit="km/s" — converted from a velocity axis, or from a
        frequency axis via ``restfreq_hz`` (radio convention v=c·(f0−f)/f0).
      * mode="native" — axis kept in its own unit (no rest frequency for a FREQ
        axis, or an unrecognised spectral type). Caller should surface this so
        the GUI can label/​warn correctly.
    """
    coords = np.asarray(coords, dtype=np.float64).ravel()
    try:
        import astropy.units as u  # noqa: PLC0415
        unit = u.Unit(axis_unit) if axis_unit else u.dimensionless_unscaled
        if unit.is_equivalent(u.km / u.s):
            return (coords * unit).to(u.km / u.s).value, "km/s", "velocity"
        if unit.is_equivalent(u.Hz) and math.isfinite(restfreq_hz) and restfreq_hz > 0:
            freq_hz = (coords * unit).to(u.Hz).value
            return _C_KMS * (restfreq_hz - freq_hz) / restfreq_hz, "km/s", "velocity"
    except Exception:
        pass
    return coords, (axis_unit or ""), "native"


def kinematic_model_payload(model: np.ndarray) -> dict[str, Any]:
    """Serialise the model cube as little-endian float32 (x-fastest = vtkImageData
    point order), with its peak value for the caller's contour scaling."""
    nchan, ny, nx = model.shape
    raw = np.ascontiguousarray(model, dtype="<f4").tobytes()
    return {"raw_bytes": raw, "width": int(nx), "height": int(ny), "depth": int(nchan),
            "peak": float(model.max()) if model.size else 0.0}


def kinematic_model_worker(path: str, x0: float, y0: float, pa_deg: float, incl_deg: float,
                           vsys: float, vrot: float, rmax_pix: float, sigma: float,
                           cap: int,
                           rings: list[tuple[float, float, float, float]] | None = None,
                           ) -> dict[str, Any]:
    """Top-level worker (picklable for the ProcessPoolExecutor).

    Reads the cube's geometry and per-channel spectral axis from ``path``,
    converts the axis to radio-convention velocity (km/s) when possible, then
    builds the model. ``vsys/vrot/sigma`` are interpreted in the reported
    ``spectral_axis_unit`` (km/s for ``spectral_mode="velocity"``). ``cap`` is
    the voxel limit resolved in the parent (the env var does not survive the
    pre-forked pool). Raises :class:`ModelTooLargeError` (mapped to HTTP 413).
    """
    from .fits_dataset import ScientificFitsDataset  # noqa: PLC0415

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Kinematic model requires a 3D cube.")
        depth, height, width = data.shape
    finally:
        hdul.close()

    # Early voxel-cap check before doing any spectral work.
    nvox = int(depth) * int(height) * int(width)
    if nvox > cap:
        raise ModelTooLargeError(
            f"Kinematic model cube too large: {nvox} voxels "
            f"({width}×{height}×{depth}) exceeds the {cap}-voxel cap. "
            "Crop the cube (sub-region) or raise VISIVO_KINEMATIC_MAX_VOXELS."
        )

    axis_unit = ""
    axis_type = ""
    try:
        spectral = dataset.spectral_axis_metadata(0, depth - 1)
        axis_unit = str(spectral.get("axis_unit", "") or "")
        axis_type = str(spectral.get("axis_type", "") or "")
        coords = np.asarray(spectral.get("coordinates", []), dtype=np.float64).ravel()
    except Exception:
        coords = np.empty(0)

    if coords.size != depth or not np.all(np.isfinite(coords)):  # WCS unusable → channel index
        coords = np.arange(depth, dtype=np.float64)
        velocity, unit, mode = coords, "channel", "channel"
    else:
        restfreq = 0.0
        for key in ("RESTFRQ", "RESTFREQ"):
            try:
                restfreq = float(dataset.header.get(key, 0.0) or 0.0)
            except (TypeError, ValueError):
                restfreq = 0.0
            if restfreq:
                break
        velocity, unit, mode = _spectral_axis_kms(coords, axis_unit, axis_type, restfreq)

    if rings:
        ring_r = [float(r[0]) for r in rings]
        ring_pa = [float(r[1]) for r in rings]
        ring_incl = [float(r[2]) for r in rings]
        ring_v = [float(r[3]) for r in rings]
        model = tilted_ring_model_profile(width, height, velocity, x0, y0,
                                          ring_r, ring_pa, ring_incl, ring_v,
                                          vsys, sigma, cap=cap)
    else:
        model = tilted_ring_model(width, height, velocity, x0, y0, pa_deg, incl_deg,
                                  vsys, vrot, rmax_pix, sigma, cap=cap)
    payload = kinematic_model_payload(model)
    payload["spectral_axis_unit"] = unit
    payload["spectral_mode"] = mode
    return payload
