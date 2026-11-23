"""Statistics of a selection on the full cube (issues #171, #280).

The headset's panels show what was selected: voxels, the sum, mean, rms and
range of the values, the integrated flux in Jy km/s and the velocity range,
and the standard HI measurements (``hi``: flux uncertainty, peak S/N, W20 and
W50, systemic velocity, flux-weighted centroid, moment-0 size; definitions in
``hi_measures``).

Two kinds of selection:

* an explicit mask, as the select route returns it and as ``mask_apply_bin``
  and ``kinematic_fit_bin`` take it (little-endian float32 0/1, x fastest, over
  a bounding box at an origin in cube voxels), so a lasso selection is posted
  back unchanged;
* a box (no mask: every voxel of the box at the origin), the headset's box
  region, measured in one call instead of one region-statistics call per channel.

Only the box is read from the file, one channel at a time, so the memory is a
plane, not the box: no full-cube array is built, unlike the kinematic fit,
which holds the whole cube to fit rings. The local noise reads the box widened
by 3 beams and 5 channels, a plane at a time too.

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

from ..fits_dataset import pixel_to_icrs
from ._common import ScientificFitsDataset, beam_area_pixels, is_per_beam_bunit
from .hi_measures import (NOISE_PAD_CHANNELS, RHO_LAGS, channel_rho, correlation_factor,
                          finite_or_none, flux_uncertainty, line_widths, m0_ellipse,
                          mad_about_zero, noise_padding, sky_position_angle)

#: The largest box measured (voxels): 64 Mi float32 is 256 MB of mask body. A
#: selection that large is a cube, not a source. The route checks it before
#: reading the body.
MAX_BOX_VOXELS = 64 * 1024 * 1024

#: The local-noise sample is thinned to about this many voxels (a stride over
#: the frame's unselected voxels): the median of 4 M values is exact enough.
NOISE_SAMPLE_VOXELS = 4_000_000


def _finite(v) -> float | None:
    v = float(v)
    return v if math.isfinite(v) else None


def check_box(width: int, height: int, depth: int) -> None:
    if width <= 0 or height <= 0 or depth <= 0:
        raise ValueError("The box needs a positive width, height and depth.")
    if width * height * depth > MAX_BOX_VOXELS:
        raise ValueError(f"The box holds {width * height * depth} voxels, over the "
                         f"{MAX_BOX_VOXELS}-voxel limit of these statistics.")


def worker_cube_packed_mask_stats(path: str, packed: bytes | None,
                                  origin_x: int, origin_y: int, origin_z: int,
                                  roi_width: int, roi_height: int, roi_depth: int,
                                  sigma: float | None = None) -> dict[str, Any]:
    """:func:`worker_cube_mask_stats` of a bit-packed mask (one bit per voxel,
    x fastest, as ``np.packbits`` of ``mask > 0.5``), or of the whole box when
    ``packed`` is None. The shared-selection publish hands over these n / 8
    bytes instead of its float32 body, and the worker unpacks them."""
    if packed is None:
        return worker_cube_mask_stats(path, None, origin_x, origin_y, origin_z,
                                      roi_width, roi_height, roi_depth, sigma)
    n = int(roi_width) * int(roi_height) * int(roi_depth)
    if len(packed) != (n + 7) // 8:
        raise ValueError(f"Packed mask is {len(packed)} bytes; expected {(n + 7) // 8}.")
    bits = np.unpackbits(np.frombuffer(packed, dtype=np.uint8), count=n)
    return worker_cube_mask_stats(path, bits.astype("<f4").tobytes(), origin_x, origin_y,
                                  origin_z, roi_width, roi_height, roi_depth, sigma)


def noise_frame_plan(frame_shape: tuple[int, int, int],
                     budget: int = NOISE_SAMPLE_VOXELS) -> tuple[int, list[int]]:
    """How the noise frame (planes, rows, columns) is sampled within
    ``budget`` voxels: a spatial stride s (every s-th row and column) and the
    frame-relative planes read. Every plane while that fits; otherwise runs
    of RHO_LAGS + 1 consecutive planes spread over the frame, so the channel
    correlation still has its lag pairs."""
    nzf, nyf, nxf = (int(v) for v in frame_shape)
    s = max(1, math.ceil(math.sqrt(nzf * nyf * nxf / budget)))
    per_plane = (-(-nyf // s)) * (-(-nxf // s))
    if per_plane * nzf <= budget:
        return s, list(range(nzf))
    run = RHO_LAGS + 1
    runs = max(1, budget // (per_plane * run))
    starts = np.unique(np.linspace(0, max(0, nzf - run), runs).round().astype(int))
    planes = sorted({int(z) for st in starts for z in range(st, min(nzf, st + run))})
    return s, planes


def _local_noise(data, mask, bbox: list[int], box: tuple[int, int, int, int, int, int],
                 pad_xy: int) -> tuple[float | None, int, list[float | None]]:
    """SoFiA-2's local noise of a selection: 1.4826 x median |value| of the
    voxels that are NOT selected in the selection's tight box ``bbox`` [x0, x1,
    y0, y1, z0, z1] widened by ``pad_xy`` pixels and 5 channels (clipped to the
    cube), and the noise correlation between channels 1..RHO_LAGS apart from
    the same voxels (``hi_measures.channel_rho``). ``mask`` (z, y, x) covers
    the box ``box`` = (ox, oy, oz, w, h, d); None means every voxel of that
    box is selected. The frame is read already thinned (``noise_frame_plan``:
    strided rows and columns, at most NOISE_SAMPLE_VOXELS voxels), one plane
    at a time. Returns (sigma or None, voxels sampled, [rho_1, ...])."""
    nz, ny, nx = data.shape
    x0, x1 = max(0, bbox[0] - pad_xy), min(nx - 1, bbox[1] + pad_xy)
    y0, y1 = max(0, bbox[2] - pad_xy), min(ny - 1, bbox[3] + pad_xy)
    z0, z1 = max(0, bbox[4] - NOISE_PAD_CHANNELS), min(nz - 1, bbox[5] + NOISE_PAD_CHANNELS)
    ox, oy, oz, rw, rh, rd = box
    s, planes = noise_frame_plan((z1 - z0 + 1, y1 - y0 + 1, x1 - x0 + 1))
    ys = np.arange(y0, y1 + 1, s)
    xs = np.arange(x0, x1 + 1, s)
    ry = (ys >= oy) & (ys < oy + rh)          # sampled rows and columns inside the box
    rx = (xs >= ox) & (xs < ox + rw)
    in_box = bool(ry.any() and rx.any())
    parts: list[np.ndarray] = []
    lag_diffs: list[list[np.ndarray]] = [[] for _ in range(RHO_LAGS)]
    recent: dict[int, np.ndarray] = {}        # the last planes read, NaN where not noise
    for zr in planes:
        z = z0 + zr
        plane = np.array(data[z, y0:y1 + 1:s, x0:x1 + 1:s], dtype=np.float64)   # the sample only
        k = z - oz
        if in_box and 0 <= k < rd:
            excl = np.zeros(plane.shape, dtype=bool)
            excl[np.ix_(ry, rx)] = True if mask is None else (
                mask[k][np.ix_(ys[ry] - oy, xs[rx] - ox)] > 0.5)
            plane[excl] = np.nan
        good = np.isfinite(plane)
        if good.any():
            parts.append(plane[good])
        for j in range(1, RHO_LAGS + 1):
            prev = recent.get(z - j)
            if prev is not None:
                d = plane - prev
                lag_diffs[j - 1].append(d[np.isfinite(d)])
        recent[z] = plane
        recent.pop(z - RHO_LAGS, None)
    if not parts:
        return None, 0, [None] * RHO_LAGS
    sample = np.concatenate(parts)
    sigma = mad_about_zero(sample)
    rho = channel_rho(sigma, [np.concatenate(d) if d else np.empty(0) for d in lag_diffs]) \
        if sigma else [None] * RHO_LAGS
    return sigma, int(sample.size), rho


def worker_cube_mask_stats(path: str, mask_bytes: bytes | None,
                           origin_x: int, origin_y: int, origin_z: int,
                           roi_width: int, roi_height: int, roi_depth: int,
                           sigma: float | None = None) -> dict[str, Any]:
    """Voxels, value statistics, flux, velocity range, integrated spectrum and
    the standard HI measurements (``hi``) of the selection ``mask_bytes``
    (float32 0/1 over the box at ``origin``), or of the whole box when
    ``mask_bytes`` is None. ``sigma`` > 0 is the session's noise per voxel (in
    BUNIT); otherwise the noise is measured around the selection."""
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
    supplied = float(sigma) if sigma is not None and math.isfinite(float(sigma)) and sigma > 0 else None
    hdul, raw = dataset.open_memmap()
    try:
        data = ScientificFitsDataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Selection statistics need a 3-D cube.")
        nz, ny, nx = data.shape
        if ox < 0 or oy < 0 or oz < 0 or ox + rw > nx or oy + rh > ny or oz + rd > nz:
            raise ValueError("The box falls outside the cube.")

        # The velocity axis first: the moment-0 map weights each channel by its
        # own width.
        vel = dvk = axis = None
        velocity_convention = velocity_frame = flux_note = ""
        try:
            from .kinematic_fit import cube_velocity_axis  # noqa: PLC0415
            axis = cube_velocity_axis(dataset, nz)
            vel, dvk = axis["vel"], axis["dvk"]
            velocity_convention, velocity_frame = axis["convention"], axis["frame"]
        except Exception as exc:  # noqa: BLE001 - no velocity axis: native channels only
            flux_note = f"no velocity axis ({exc})"

        selected = valid = 0
        mean = m2 = 0.0               # pooled per channel (Chan et al.)
        total = 0.0
        vmin, vmax, peak = math.inf, -math.inf, None
        per_chan = np.full(rd, np.nan)          # selected sum per channel, NaN: none valid
        n_chan = np.zeros(rd, dtype=np.int64)   # valid selected voxels per channel
        pos_chan = np.zeros(rd)                 # sum of the positive selected values
        any_sel = np.zeros(rd, dtype=bool)
        rows_any = np.zeros(rh, dtype=bool)
        cols_any = np.zeros(rw, dtype=bool)
        m0 = np.zeros((rh, rw))                 # sum_k value x dv_k over the selection
        footprint = np.zeros((rh, rw), dtype=bool)   # pixels with a valid selected voxel
        pos_x = np.zeros(rw)                    # positive selected values per column, per row
        pos_y = np.zeros(rh)
        for k in range(rd):
            # A copy always (np.array), never a view of the file: it is scaled in place.
            plane = np.array(data[oz + k, oy:oy + rh, ox:ox + rw], dtype=np.float64)
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
            n_chan[k] = n
            # In place, no plane-sized temporaries: the plane is this loop's own
            # float64 copy (scaled by the channel width where selected), and
            # the positive values are taken from the selected ones.
            if dvk is not None:
                np.multiply(plane, float(dvk[oz + k]), out=plane, where=ok)
            np.add(m0, plane, out=m0, where=ok)
            footprint |= ok
            rows, cols = np.nonzero(ok)                  # the order of plane[ok]
            pos = vals > 0.0
            pos_chan[k] = float(vals[pos].sum())
            pos_x += np.bincount(cols[pos], weights=vals[pos], minlength=rw)
            pos_y += np.bincount(rows[pos], weights=vals[pos], minlength=rh)
            lo, hi = float(vals.min()), float(vals.max())
            vmin = min(vmin, lo)
            if hi > vmax:
                vmax = hi
                j = int(np.argmax(vals))                     # the first maximum, in C order
                peak = [ox + int(cols[j]), oy + int(rows[j]), oz + k]          # x, y, z
        if not selected:
            raise ValueError("The selection is empty.")
        if mask is None:
            rows_any[:] = True
            cols_any[:] = True
        zs = np.nonzero(any_sel)[0]
        ys = np.nonzero(rows_any)[0]
        xs = np.nonzero(cols_any)[0]
        bbox = [int(ox + xs[0]), int(ox + xs[-1]), int(oy + ys[0]), int(oy + ys[-1]),
                int(oz + zs[0]), int(oz + zs[-1])]

        # The noise: the session's when supplied, else around the selection.
        noise, noise_voxels, rho = supplied, 0, None
        if noise is None and valid:
            bmaj = dataset.beam.major
            if not bmaj and dataset.has_per_channel_beam:
                bmaj = max((dataset.beam_for_channel(int(c)).major or 0.0)
                           for c in range(bbox[4], bbox[5] + 1))
            pad = noise_padding(bmaj, dataset.spatial_pixel_scale_arcsec() / 3600.0)
            noise, noise_voxels, rho = _local_noise(data, mask, bbox, (ox, oy, oz, rw, rh, rd), pad)
        # The mean number of channels summed per pixel of the footprint: N of
        # the correlated-channel correction.
        n_footprint = int(footprint.sum())
        n_per_pixel = valid / n_footprint if n_footprint else 0.0
    finally:
        hdul.close()

    out: dict[str, Any] = {
        "selected_voxels": selected, "valid_voxels": valid, "blank_voxels": selected - valid,
        "bunit": dataset.intensity_unit or "",
        "sum": _finite(total) if valid else None,
        "mean": _finite(mean) if valid else None,
        "rms": _finite(math.sqrt(max(0.0, m2 / valid))) if valid else None,
        "min": _finite(vmin) if valid else None, "max": _finite(vmax) if valid else None,
        "peak_voxel": peak,
        "flux_jy_kms": None, "flux_note": flux_note,
        "channel_min": bbox[4], "channel_max": bbox[5],
        "velocity_min_kms": None, "velocity_max_kms": None,
        "velocity_convention": velocity_convention,
        "bbox": bbox, "spectrum": [], "spectrum_channels": [], "spectrum_velocity_kms": [],
        "spectrum_unit": "",
    }
    chans = np.arange(bbox[4], bbox[5] + 1)
    spectrum = per_chan[zs[0]:zs[-1] + 1]
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
    shape = spectrum
    if beam_px is not None:
        jy = spectrum / beam_px                     # NaN where a channel has no valid voxel
        shape = jy
        out["spectrum"] = [_finite(v) for v in jy]
        out["spectrum_unit"] = "Jy"
        if dvk is not None and valid:
            out["flux_jy_kms"] = _finite(float(np.nansum(jy * dvk[chans])))
        elif not valid:
            out["flux_note"] = "every selected voxel is blank"
    else:
        out["spectrum"] = [_finite(v) for v in spectrum]
        out["spectrum_unit"] = f"sum of {dataset.intensity_unit}" if dataset.intensity_unit else "sum"

    out["hi"] = _hi_measures(
        dataset, out, shape, vel, dvk, chans, beam_px, n_chan[zs[0]:zs[-1] + 1],
        pos_chan, pos_x, pos_y, m0, (ox, oy, oz), noise, supplied is not None, noise_voxels,
        axis, rho, n_per_pixel)
    return out


#: What the size and its position angle are, for the answer.
SIZE_DEFINITION = ("semi-axes of the uniform ellipse with the moment-0 map's second moments "
                   "(2 sqrt(eigenvalue), positive pixels, about the centroid), not deconvolved "
                   "from the beam; pa_deg east of north (ICRS, from the WCS at the centroid), "
                   "pa_grid_deg from +y towards -x on the pixel grid")


def _hi_measures(dataset, out: dict[str, Any], spectrum: np.ndarray, vel, dvk, chans: np.ndarray,
                 beam_px, n_chan: np.ndarray, pos_chan: np.ndarray, pos_x: np.ndarray,
                 pos_y: np.ndarray, m0: np.ndarray, origin: tuple[int, int, int],
                 noise: float | None, noise_supplied: bool, noise_voxels: int,
                 axis: dict[str, Any] | None, rho: list[float | None] | None,
                 n_per_pixel: float) -> dict[str, Any]:
    """The standard HI measurements (``hi_measures``) from the sums the worker
    accumulated: every number with its unit in its name, None where it cannot
    be measured, and ``notes`` saying why. ``rho``: the channel correlation
    measured in the noise frame (None when the noise was supplied)."""
    ox, oy, oz = origin
    notes: list[str] = []
    bunit = dataset.intensity_unit or ""
    axis = axis or {}
    hi: dict[str, Any] = {
        "flux_jy_kms": out["flux_jy_kms"], "flux_err_jy_kms": None,
        "flux_err_indep_jy_kms": None, "flux_err_factor": None,
        "channel_rho": None, "channel_correlation": "", "snr_integrated": None,
        "noise_sigma": finite_or_none(noise), "noise_unit": bunit,
        "noise_source": "", "noise_voxels": int(noise_voxels),
        "peak": out["max"], "peak_unit": bunit, "peak_snr": None,
        "w20_kms": None, "w50_kms": None, "vsys_kms": None,
        "vsys_definition": "midpoint of the two W20 crossings of the integrated spectrum",
        "velocity_convention": out["velocity_convention"],
        "velocity_convention_name": axis.get("convention_name", ""),
        "velocity_frame": axis.get("frame", ""), "velocity_frame_source": axis.get("frame_source", ""),
        "centroid_x": None, "centroid_y": None, "centroid_z": None,
        "centroid_ra_deg": None, "centroid_dec_deg": None, "centroid_frame": "",
        "centroid_velocity_kms": None,
        "major_axis_arcsec": None, "minor_axis_arcsec": None, "pa_deg": None, "pa_grid_deg": None,
        "major_axis_px": None, "minor_axis_px": None,
        "size_definition": SIZE_DEFINITION,
        "beam_major_arcsec": None, "beam_minor_arcsec": None,
        "notes": notes,
    }
    if vel is not None and not hi["velocity_frame"]:
        notes.append("the cube names no spectral frame (no SPECSYS, no CTYPE suffix)")
    elif hi["velocity_frame"] == "topocentric":
        notes.append("topocentric velocities: not corrected for the Earth's motion")
    if noise is not None:
        hi["noise_source"] = ("supplied (the session's noise estimate)" if noise_supplied
                              else "local: 1.4826 x median |value| of the unselected voxels within "
                                   "3 beam majors and 5 channels of the selection (SoFiA-2's rule)")
        if out["max"] is not None:
            hi["peak_snr"] = finite_or_none(out["max"] / noise)
        if beam_px is not None and dvk is not None and out["flux_jy_kms"] is not None:
            indep = flux_uncertainty(noise, n_chan, dvk[chans], beam_px)
            hi["flux_err_indep_jy_kms"] = finite_or_none(indep)
            if rho is None:
                # A supplied sigma: the frame is not read, so no correlation.
                hi["flux_err_jy_kms"] = hi["flux_err_indep_jy_kms"]
                hi["channel_correlation"] = "channel correlation not measured"
            else:
                factor = correlation_factor(rho, n_per_pixel)
                hi["channel_rho"] = [finite_or_none(r) for r in rho]
                hi["flux_err_factor"] = factor
                hi["flux_err_jy_kms"] = finite_or_none(indep * factor)
                r1 = rho[0]
                if r1 is None:
                    hi["channel_correlation"] = "channel correlation not measured (too few noise pairs)"
                else:
                    hi["channel_correlation"] = (
                        f"channels correlated (ρ1 {r1:.2f}): error corrected ×{factor:.1f}"
                        if factor > 1.05 else f"channels independent (ρ1 {r1:.2f})")
            if hi["flux_err_jy_kms"]:
                hi["snr_integrated"] = finite_or_none(out["flux_jy_kms"] / hi["flux_err_jy_kms"])
    else:
        notes.append("no noise measured around the selection: no flux uncertainty or S/N")
    b = dataset.beam
    if b.major and b.minor:
        hi["beam_major_arcsec"], hi["beam_minor_arcsec"] = b.major * 3600.0, b.minor * 3600.0

    if vel is not None:
        w = line_widths(spectrum, vel[chans])
        hi["w20_kms"], hi["w50_kms"], hi["vsys_kms"] = (finite_or_none(w[k]) for k in ("w20", "w50", "vsys"))
        if hi["w20_kms"] is None:
            notes.append("the integrated spectrum has no positive peak: no line widths")
        elif w["edge20"] or w["edge50"]:
            notes.append("width is a lower limit: the line reaches the selection's edge")
    else:
        notes.append("no velocity axis: no line widths, systemic or centroid velocity")

    spos = float(pos_chan.sum())
    if spos <= 0:
        notes.append("no positive selected voxel: no centroid or size")
        return hi
    cx = float((pos_x * (ox + np.arange(pos_x.size))).sum() / spos)
    cy = float((pos_y * (oy + np.arange(pos_y.size))).sum() / spos)
    cz = float((pos_chan * (oz + np.arange(pos_chan.size))).sum() / spos)
    hi["centroid_x"], hi["centroid_y"], hi["centroid_z"] = cx, cy, cz
    if vel is not None:
        hi["centroid_velocity_kms"] = finite_or_none(
            float((pos_chan * vel[oz:oz + pos_chan.size]).sum() / spos))
    def to_sky(x: float, y: float):
        try:
            s = pixel_to_icrs(dataset.wcs, dataset.client_axis_to_fits_axis, x, y, cz)
        except Exception:  # noqa: BLE001 - a WCS that cannot convert gives no sky position
            return None
        return (s["ra_deg"], s["dec_deg"]) if s is not None else None

    sky = to_sky(cx, cy)
    if sky is not None:
        hi["centroid_ra_deg"], hi["centroid_dec_deg"] = sky
        hi["centroid_frame"] = "ICRS"
    else:
        notes.append("no sky coordinates for the centroid")
    ell = m0_ellipse(m0, (cx - ox, cy - oy))
    if ell is not None:
        if ell["note"]:
            notes.append(ell["note"])
        pix_arcsec = dataset.spatial_pixel_scale_arcsec()
        hi["major_axis_px"], hi["minor_axis_px"] = ell["major_px"], ell["minor_px"]
        hi["pa_grid_deg"] = ell["pa_grid_deg"]
        if ell["theta"] is not None and sky is not None:
            # East of north, from the WCS at the centroid: the grid's north may be tilted.
            hi["pa_deg"] = sky_position_angle(to_sky, cx, cy, ell["theta"])
        if ell["major_px"] is not None and pix_arcsec and pix_arcsec > 0:
            hi["major_axis_arcsec"] = ell["major_px"] * pix_arcsec
            hi["minor_axis_arcsec"] = ell["minor_px"] * pix_arcsec
    return hi
