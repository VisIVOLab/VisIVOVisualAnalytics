"""Workers behind ``/v1/cube/reduced_info`` and ``/v1/cube/reduced_bin``: the
reduced cube a headset renders itself (issue #167), and the world coordinates
of its voxels. The reduction rules are in :mod:`app.cube_reduce`."""
from __future__ import annotations

import math
from typing import Any

import numpy as np

from .. import cube_reduce
from ..noise_threshold import default_threshold, stride_for
from ._common import ScientificFitsDataset, logger


def _finite_list(values: np.ndarray) -> list[float | None]:
    """JSON-safe list: NaN and infinities become null."""
    return [float(v) if math.isfinite(float(v)) else None for v in np.asarray(values).ravel()]


def _short_ctype(ctype: str) -> str:
    """``RA---NCP`` -> ``RA``, ``GLON-CAR`` -> ``GLON``, ``VELO-HEL`` -> ``VELO``."""
    return str(ctype or "").strip().split("-")[0].strip()


def _spatial_axis(dataset: ScientificFitsDataset, which: int, shape_zyx, centres) -> dict[str, Any]:
    """World coordinate of each reduced column (``which`` = 0) or row (1).

    Read with the file's full WCS along the central row (for x) or column (for
    y) of the field, so the labels on a projected sky axis (NCP, SIN) are the
    coordinates of those pixels, not a linear approximation of them. Falls back
    to the header's linear axis when the WCS cannot be evaluated."""
    fits_axis = dataset.active_axes[which]
    other_axis = dataset.active_axes[1 - which]
    depth, height, width = shape_zyx
    ctype = str(dataset.header.get(f"CTYPE{fits_axis}", "")).strip()
    unit = str(dataset.sanitized_header.get(f"CUNIT{fits_axis}", "")).strip()
    values = None
    method = "linear"
    if dataset.wcs is not None:
        try:
            n_pix = int(dataset.wcs.pixel_n_dim)
            pix = []
            for p in range(n_pix):
                crpix = float(dataset.header.get(f"CRPIX{p + 1}", 1.0))
                pix.append(np.full(centres.size, crpix - 1.0, dtype=np.float64))
            pix[fits_axis - 1] = centres.astype(np.float64)
            centre_other = 0.5 * ((width if which == 1 else height) - 1)
            pix[other_axis - 1] = np.full(centres.size, centre_other, dtype=np.float64)
            world = dataset.wcs.pixel_to_world_values(*pix)
            values = np.asarray(world[fits_axis - 1], dtype=np.float64).ravel()
            if values.size != centres.size or not np.all(np.isfinite(values)):
                values = None
            else:
                method = "wcs"
                wunits = getattr(dataset.wcs, "world_axis_units", None) or []
                if fits_axis - 1 < len(wunits) and wunits[fits_axis - 1]:
                    unit = str(wunits[fits_axis - 1])
        except Exception as exc:  # noqa: BLE001 - fall back to the linear header axis
            logger.info("[reduced] spatial WCS evaluation failed (%s): linear axis", exc)
            values = None
    if values is None:
        crpix = float(dataset.sanitized_header.get(f"CRPIX{fits_axis}", 1.0))
        crval = float(dataset.sanitized_header.get(f"CRVAL{fits_axis}", 0.0))
        cdelt = dataset._cdelt(fits_axis)  # noqa: SLF001 - the CD-matrix-aware scale
        values = crval + (centres + 1.0 - crpix) * cdelt
    if unit in ("", "deg") and _short_ctype(ctype).upper() in ("RA", "DEC", "GLON", "GLAT",
                                                                "ELON", "ELAT"):
        unit = "deg"
    return {
        "name": _short_ctype(ctype) or ("x" if which == 0 else "y"),
        "ctype": ctype,
        "unit": unit,
        "method": method,
        "world": _finite_list(values),
    }


def _spectral_axis(dataset: ScientificFitsDataset, shape_zyx, centres) -> dict[str, Any]:
    """Spectral coordinate of each reduced channel, native and (when it can be
    expressed so) as velocity in km/s with the cube's own convention, or
    optical velocity cz = c (nu0/nu - 1) from a frequency axis and RESTFRQ, the
    conventions of the kinematic fit and model (``compute.kinematic_fit``)."""
    from .kinematic_fit import velocity_axis_kms  # noqa: PLC0415
    depth = int(shape_zyx[0])
    fits_axis = dataset.active_axes[2]
    ctype = str(dataset.header.get(f"CTYPE{fits_axis}", "")).strip()
    out: dict[str, Any] = {"name": _short_ctype(ctype) or "z", "ctype": ctype,
                           "unit": "", "world": [], "velocity_kms": None,
                           "velocity_convention": "", "channel_width_kms": None,
                           "full_velocity_kms": None, "full_channel_width_kms": None,
                           "specsys": str(dataset.header.get("SPECSYS", "") or "").strip()}
    if dataset.cube_numpy_spectral_fits_axis() != fits_axis:
        # A transposed cube: the third numpy axis is not the spectral one.
        crpix = float(dataset.sanitized_header.get(f"CRPIX{fits_axis}", 1.0))
        crval = float(dataset.sanitized_header.get(f"CRVAL{fits_axis}", 0.0))
        cdelt = float(dataset.sanitized_header.get(f"CDELT{fits_axis}", 1.0))
        out["unit"] = str(dataset.sanitized_header.get(f"CUNIT{fits_axis}", "")).strip()
        out["world"] = _finite_list(crval + (centres + 1.0 - crpix) * cdelt)
        return out
    spectral = dataset.spectral_axis_metadata(0, depth - 1)
    coords = np.asarray(spectral.get("coordinates", []), dtype=np.float64).ravel()
    unit = str(spectral.get("axis_unit", "") or "")
    out["unit"] = unit
    if coords.size != depth:
        out["world"] = _finite_list(centres)
        out["unit"] = "channel"
        return out
    channels = np.arange(depth, dtype=np.float64)
    native = np.interp(centres, channels, coords)
    out["world"] = _finite_list(native)
    rest = dataset._rest_freq_hz() or 0.0  # noqa: SLF001
    vel_full, vunit, mode = velocity_axis_kms(coords, unit, ctype, float(rest))
    if mode == "velocity":
        out["velocity_kms"] = _finite_list(np.interp(centres, channels, vel_full))
        is_freq = False
        try:
            import astropy.units as u  # noqa: PLC0415
            is_freq = u.Unit(unit).is_equivalent(u.Hz)
        except Exception:  # noqa: BLE001
            is_freq = False
        out["velocity_convention"] = ("optical (cz), from frequency and RESTFRQ" if is_freq
                                      else f"as in the cube ({ctype or unit})")
        finite = vel_full[np.isfinite(vel_full)]
        if finite.size > 1:
            out["channel_width_kms"] = float(np.mean(np.abs(np.diff(finite))))
        # Every full-resolution channel too (issue #171: the velocity of a picked
        # channel, of a spectrum, of a selection's channel range), with each
        # channel's own width from its half-channel edges, the widths the
        # kinematic fit and the selection statistics integrate with.
        from .kinematic_fit import channel_edges  # noqa: PLC0415
        edges, _, _ = velocity_axis_kms(channel_edges(coords), unit, ctype, float(rest))
        out["full_velocity_kms"] = _finite_list(vel_full)
        out["full_channel_width_kms"] = _finite_list(np.abs(np.diff(edges)))
    return out


def _open_cube(path: str):
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    data = dataset.squeeze_to_3d(raw)
    if data.ndim != 3 or len(dataset.active_axes) < 3:
        hdul.close()
        raise ValueError("A reduced cube needs a 3-D cube.")
    return dataset, hdul, data


def _factors(shape_zyx, factor_xy: int, factor_z: int, max_dim: int,
             encoding: str = "uint8", limit: bool = True) -> tuple[int, int, int]:
    auto_xy, _, auto_z = cube_reduce.auto_factors(shape_zyx, max_dim)
    fxy = int(factor_xy) if factor_xy and factor_xy > 0 else auto_xy
    fz = int(factor_z) if factor_z and factor_z > 0 else auto_z
    if limit:
        cube_reduce.check_size(cube_reduce.reduced_shape(shape_zyx, (fxy, fxy, fz)), encoding)
    return fxy, fxy, fz


def _full_threshold(data: np.ndarray) -> float | None:
    """The cube's default 3-D threshold, the rule the desktop's view starts from
    (issue #93): on a stride sample, for the voxels of the WHOLE cube."""
    n_voxels = int(np.prod(data.shape))
    st = stride_for(n_voxels)
    sample = np.asarray(data[::st, ::st, ::st], dtype=np.float32)
    if not np.isfinite(sample).any():
        return None
    t = default_threshold(sample, n_voxels)
    return float(t) if t is not None and math.isfinite(t) else None


def binning_threshold(data: np.ndarray, reduced: np.ndarray, mode: str) -> float | None:
    """The default threshold of a reduced cube.

    ``mean``: the noise rule applied to the reduced cube itself, whose noise is
    lower than the cube's (it falls as the square root of the block).
    ``max``: the full cube's threshold. A block maximum is above it exactly when
    some voxel of the block is, so the reduced cube draws the blocks holding a
    voxel the desktop would draw. The noise rule on the reduced cube reads the
    noise from the lower tail, which a block maximum compresses, and set too low
    a cut: on pure Gaussian noise binned 3x3x1 it drew 0.15 % of the voxels
    instead of the rule's 1e-4.
    """
    if mode == "max":
        return _full_threshold(data)
    st = stride_for(reduced.size)
    sample = reduced[::st, ::st, ::st]
    if not np.isfinite(sample).any():
        return None
    t = default_threshold(sample, int(reduced.size))
    return float(t) if t is not None and math.isfinite(t) else None


def worker_cube_reduced_info(path: str, factor_xy: int, factor_z: int,
                             max_dim: int, encoding: str = "uint8",
                             limit: bool = True) -> dict[str, Any]:
    """Shape, factors, default threshold and world axes of a reduced cube,
    without reducing it."""
    dataset, hdul, data = _open_cube(path)
    try:
        shape = tuple(int(n) for n in data.shape)
        factors = _factors(shape, factor_xy, factor_z, max_dim, encoding, limit)
        threshold = _full_threshold(data)
    finally:
        hdul.close()
    fx, fy, fz = factors
    depth, height, width = shape
    cx = cube_reduce.block_centres(width, fx)
    cy = cube_reduce.block_centres(height, fy)
    cz = cube_reduce.block_centres(depth, fz)
    axes = {
        "x": _spatial_axis(dataset, 0, shape, cx),
        "y": _spatial_axis(dataset, 1, shape, cy),
        "z": _spectral_axis(dataset, shape, cz),
    }
    # Pixel scale on the sky (arcsec), for the explicit velocity-to-space scale.
    try:
        pixel_arcsec = float(dataset.spatial_pixel_scale_arcsec())
    except Exception:  # noqa: BLE001
        pixel_arcsec = None
    rd, rh, rw = cube_reduce.reduced_shape(shape, factors)
    return {
        "full_shape": [width, height, depth],
        "factors": [fx, fy, fz],
        "reduced_shape": [rw, rh, rd],
        "default_threshold": threshold,
        "bunit": dataset.bunit or "",
        "pixel_scale_arcsec": pixel_arcsec if pixel_arcsec and math.isfinite(pixel_arcsec) else None,
        "axes": axes,
        "modes": list(cube_reduce.MODES),
        "encodings": list(cube_reduce.ENCODINGS),
    }


def worker_cube_reduced(path: str, factor_xy: int, factor_z: int, max_dim: int,
                        mode: str, encoding: str, limit: bool = True) -> dict[str, Any]:
    """The reduced cube as the headset's 3-D texture, plus its value range and
    its default threshold (:func:`binning_threshold`). ``limit=False`` lifts the
    headset size cap, for measurements on the backend (the validation scripts)."""
    dataset, hdul, data = _open_cube(path)
    try:
        shape = tuple(int(n) for n in data.shape)
        factors = _factors(shape, factor_xy, factor_z, max_dim, encoding, limit)
        reduced = cube_reduce.reduce_cube(data, factors, mode)
        own = binning_threshold(data, reduced, mode)
    finally:
        hdul.close()
    lo, hi = cube_reduce.finite_range(reduced)
    raw = cube_reduce.quantise(reduced, lo, hi, encoding)
    rd, rh, rw = reduced.shape
    return {
        "width": int(rw), "height": int(rh), "depth": int(rd),
        "full_shape": [shape[2], shape[1], shape[0]],
        "factors": list(factors),
        "mode": mode,
        "encoding": encoding,
        "range_min": lo, "range_max": hi,
        "threshold": own,
        "bunit": dataset.bunit or "",
        "raw_bytes": raw,
    }
