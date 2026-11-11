"""Statistics of a selection on the full cube (issue #171).

The headset's panels show what was selected: voxels, the sum, mean, rms and
range of the values, the integrated flux in Jy km/s and the velocity range.

Two kinds of selection:

* an explicit mask, as the select route returns it and as ``mask_apply_bin``
  and ``kinematic_fit_bin`` take it (little-endian float32 0/1, x fastest, over
  a bounding box at an origin in cube voxels), so a lasso selection is posted
  back unchanged;
* a box (no mask: every voxel of the box at the origin), the headset's box
  region, measured in one call instead of one region-statistics call per channel.

Only the box is read from the file, one channel at a time, so the memory is a
plane, not the box: no full-cube array is built, unlike the kinematic fit,
which holds the whole cube to fit rings.

The flux follows the kinematic fit's measures (``kinematic_fit.source_measures``)
and the validation harness: per channel, the selected sum divided by the beam
area in pixels (the channel's own beam when the cube has a BEAMS table, as the
region statistics do), times the channel's own width in km/s (channels uniform
in frequency are not uniform in optical velocity), summed over channels. The
rms is the population standard deviation, as ``region_stats`` gives it.
"""
from __future__ import annotations

import math
from typing import Any

import numpy as np

from ._common import ScientificFitsDataset, beam_area_pixels, is_per_beam_bunit

#: The largest box measured (voxels): 64 Mi float32 is 256 MB of mask body. A
#: selection that large is a cube, not a source. The route checks it before
#: reading the body.
MAX_BOX_VOXELS = 64 * 1024 * 1024


def _finite(v) -> float | None:
    v = float(v)
    return v if math.isfinite(v) else None


def check_box(width: int, height: int, depth: int) -> None:
    if width <= 0 or height <= 0 or depth <= 0:
        raise ValueError("The box needs a positive width, height and depth.")
    if width * height * depth > MAX_BOX_VOXELS:
        raise ValueError(f"The box holds {width * height * depth} voxels, over the "
                         f"{MAX_BOX_VOXELS}-voxel limit of these statistics.")


def worker_cube_mask_stats(path: str, mask_bytes: bytes | None,
                           origin_x: int, origin_y: int, origin_z: int,
                           roi_width: int, roi_height: int, roi_depth: int) -> dict[str, Any]:
    """Voxels, value statistics, flux, velocity range and integrated spectrum of
    the selection ``mask_bytes`` (float32 0/1 over the box at ``origin``), or of
    the whole box when ``mask_bytes`` is None."""
    rw, rh, rd = int(roi_width), int(roi_height), int(roi_depth)
    check_box(rw, rh, rd)
    mask = None
    if mask_bytes is not None:
        if len(mask_bytes) != rw * rh * rd * 4:
            raise ValueError(f"Mask payload is {len(mask_bytes)} bytes; expected "
                             f"{rw * rh * rd * 4} ({rw}x{rh}x{rd} float32).")
        mask = np.frombuffer(mask_bytes, dtype="<f4").reshape(rd, rh, rw)   # a view, no copy

    dataset = ScientificFitsDataset(path)
    kept = dataset.active_axes[:3]
    if len(kept) == 3 and dataset.cube_numpy_spectral_fits_axis() != kept[2]:
        # The velocity and beam of "channel" k would be those of another axis.
        raise ValueError("Selection statistics need the spectral axis to be the cube's last "
                         "axis (e.g. NAXIS3); this cube's spectral axis is elsewhere.")
    ox, oy, oz = int(origin_x), int(origin_y), int(origin_z)
    hdul, raw = dataset.open_memmap()
    try:
        data = ScientificFitsDataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Selection statistics need a 3-D cube.")
        nz, ny, nx = data.shape
        if ox < 0 or oy < 0 or oz < 0 or ox + rw > nx or oy + rh > ny or oz + rd > nz:
            raise ValueError("The box falls outside the cube.")

        selected = valid = 0
        mean = m2 = 0.0               # pooled per channel (Chan et al.)
        total = 0.0
        vmin, vmax, peak = math.inf, -math.inf, None
        per_chan = np.full(rd, np.nan)          # selected sum per channel, NaN: none valid
        any_sel = np.zeros(rd, dtype=bool)
        rows_any = np.zeros(rh, dtype=bool)
        cols_any = np.zeros(rw, dtype=bool)
        for k in range(rd):
            plane = np.asarray(data[oz + k, oy:oy + rh, ox:ox + rw], dtype=np.float64)
            sel = (mask[k] > 0.5) if mask is not None else None
            n_sel = int(sel.sum()) if sel is not None else plane.size
            if not n_sel:
                continue
            any_sel[k] = True
            if sel is not None:
                rows_any |= sel.any(axis=1)
                cols_any |= sel.any(axis=0)
            selected += n_sel
            ok = np.isfinite(plane) if sel is None else (sel & np.isfinite(plane))
            vals = plane[ok]
            if not vals.size:
                continue
            n = vals.size
            s = float(vals.sum())
            mu = s / n
            q = float(((vals - mu) ** 2).sum())
            delta = mu - mean
            m2 += q + delta * delta * valid * n / (valid + n)
            mean += delta * n / (valid + n)
            valid += n
            total += s
            per_chan[k] = s
            lo, hi = float(vals.min()), float(vals.max())
            vmin = min(vmin, lo)
            if hi > vmax:
                vmax = hi
                j = int(np.argmax(np.where(ok, plane, -np.inf)))
                peak = [ox + j % rw, oy + j // rw, oz + k]          # x, y, z
    finally:
        hdul.close()
    if not selected:
        raise ValueError("The selection is empty.")
    if mask is None:
        rows_any[:] = True
        cols_any[:] = True

    out: dict[str, Any] = {
        "selected_voxels": selected, "valid_voxels": valid, "blank_voxels": selected - valid,
        "bunit": dataset.bunit or "",
        "sum": _finite(total) if valid else None,
        "mean": _finite(mean) if valid else None,
        "rms": _finite(math.sqrt(max(0.0, m2 / valid))) if valid else None,
        "min": _finite(vmin) if valid else None, "max": _finite(vmax) if valid else None,
        "peak_voxel": peak,
        "flux_jy_kms": None, "flux_note": "",
        "channel_min": None, "channel_max": None,
        "velocity_min_kms": None, "velocity_max_kms": None, "velocity_convention": "",
        "bbox": None, "spectrum": [], "spectrum_channels": [], "spectrum_velocity_kms": [],
        "spectrum_unit": "",
    }
    zs = np.nonzero(any_sel)[0]
    ys = np.nonzero(rows_any)[0]
    xs = np.nonzero(cols_any)[0]
    out["bbox"] = [int(ox + xs[0]), int(ox + xs[-1]), int(oy + ys[0]), int(oy + ys[-1]),
                   int(oz + zs[0]), int(oz + zs[-1])]
    out["channel_min"], out["channel_max"] = int(oz + zs[0]), int(oz + zs[-1])
    chans = np.arange(oz + zs[0], oz + zs[-1] + 1)
    spectrum = per_chan[zs[0]:zs[-1] + 1]

    vel = dvk = None
    try:
        from .kinematic_fit import cube_velocity_axis  # noqa: PLC0415
        axis = cube_velocity_axis(dataset, nz)
        vel, dvk = axis["vel"], axis["dvk"]
        out["velocity_convention"] = axis["convention"]
    except Exception as exc:  # noqa: BLE001 - no velocity axis: native channels only
        out["flux_note"] = f"no velocity axis ({exc})"
    if vel is not None:
        v_sel = vel[oz + zs]
        out["velocity_min_kms"] = _finite(np.nanmin(v_sel))
        out["velocity_max_kms"] = _finite(np.nanmax(v_sel))
        out["spectrum_velocity_kms"] = [_finite(v) for v in vel[chans]]

    # Beam area per channel (a BEAMS table gives one per plane).
    beam_px = None
    if is_per_beam_bunit(dataset.bunit):
        pix = dataset.spatial_pixel_scale_arcsec()
        pix_area = dataset.spatial_pixel_area_arcsec2()
        areas = []
        for c in chans:
            b = dataset.beam_for_channel(int(c))
            areas.append(beam_area_pixels(b.major, b.minor, pix, pix_area) if b.major and b.minor
                         else None)
        if all(a and a > 0 for a in areas):
            beam_px = np.asarray(areas, dtype=np.float64)
        elif not out["flux_note"]:
            out["flux_note"] = "no beam in the header: no flux in Jy"
    elif not out["flux_note"]:
        out["flux_note"] = f"BUNIT {dataset.bunit or '(none)'} is not per beam: no flux in Jy"
    out["spectrum_channels"] = [int(c) for c in chans]
    if beam_px is not None:
        jy = spectrum / beam_px                     # NaN where a channel has no valid voxel
        out["spectrum"] = [_finite(v) for v in jy]
        out["spectrum_unit"] = "Jy"
        if dvk is not None and valid:
            out["flux_jy_kms"] = _finite(float(np.nansum(jy * dvk[chans])))
        elif not valid:
            out["flux_note"] = "every selected voxel is blank"
    else:
        out["spectrum"] = [_finite(v) for v in spectrum]
        out["spectrum_unit"] = f"sum of {dataset.bunit}" if dataset.bunit else "sum"
    return out
