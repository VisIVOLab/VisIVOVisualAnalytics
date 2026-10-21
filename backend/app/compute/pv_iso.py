from __future__ import annotations

import base64
import concurrent.futures as _cf  # noqa: F401
import logging  # noqa: F401
import os as _os  # noqa: F401
import shutil  # noqa: F401
import subprocess  # noqa: F401
import warnings  # noqa: F401
import zlib  # noqa: F401
from pathlib import Path  # noqa: F401
from typing import Any  # noqa: F401

import numpy as np

from ..fits_dataset import ScientificFitsDataset  # noqa: F401
from ._common import (  # noqa: F401
    logger,
    _qt_zlib_compress,
    _b64f32,
    _b64i32,
    _b64f32_compressed,
    _b64i32_compressed,
    _finite_range,
    _integrated_spacing,
)


def worker_isosurface(path: str, width: int, height: int, depth: int, threshold: float) -> dict[str, Any]:
    """
    Compute a VTK isosurface (vtkFlyingEdges3D) from a FITS cube.

    The cube is loaded with memmap=True and materialised fully only in the
    worker process; the main FastAPI process never touches the large array.
    """
    try:
        import vtk  # noqa: PLC0415
        from vtk.util.numpy_support import vtk_to_numpy  # noqa: PLC0415
    except ImportError as exc:
        raise RuntimeError(f"VTK Python bindings are required: {exc}") from exc

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Isosurface endpoint requires a 3D cube.")
        cube = np.ascontiguousarray(data, dtype=np.float32)
    finally:
        hdul.close()

    actual_depth, actual_height, actual_width = cube.shape
    if (width, height, depth) != (actual_width, actual_height, actual_depth):
        logger.info(
            "[isosurface] geometry override path=%s requested=(%s,%s,%s) actual=(%s,%s,%s)",
            dataset.path,
            width,
            height,
            depth,
            actual_width,
            actual_height,
            actual_depth,
        )
        width, height, depth = actual_width, actual_height, actual_depth

    cube_bytes = cube.tobytes()
    image_import = vtk.vtkImageImport()
    image_import.CopyImportVoidPointer(cube_bytes, len(cube_bytes))
    image_import.SetDataScalarTypeToFloat()
    image_import.SetNumberOfScalarComponents(1)
    image_import.SetDataExtent(0, width - 1, 0, height - 1, 0, depth - 1)
    image_import.SetWholeExtent(0, width - 1, 0, height - 1, 0, depth - 1)
    image_import.SetDataSpacing(1.0, 1.0, 1.0)
    image_import.SetDataOrigin(0.0, 0.0, 0.0)
    image_import.Update()

    contour = vtk.vtkFlyingEdges3D()
    contour.SetInputConnection(image_import.GetOutputPort())
    contour.SetValue(0, float(threshold))
    contour.ComputeNormalsOff()
    contour.ComputeGradientsOff()
    contour.Update()

    mesh = contour.GetOutput()
    num_points = int(mesh.GetNumberOfPoints())
    num_polys = int(mesh.GetNumberOfPolys())
    if num_points == 0 or num_polys == 0:
        raise ValueError(f"Empty isosurface mesh for threshold {threshold}.")

    pts = vtk_to_numpy(mesh.GetPoints().GetData()).astype(np.float32).reshape(-1)
    polys = vtk_to_numpy(mesh.GetPolys().GetData()).astype(np.int32)
    pts_compression, pts_b64 = _b64f32_compressed(pts)
    polys_compression, polys_b64 = _b64i32_compressed(polys)

    return {
        "compression": pts_compression,
        "points_base64": pts_b64,
        "polys_base64": polys_b64,
        "num_points": num_points,
        "num_polys": num_polys,
    }


# ── Worker: Position-Velocity diagram ────────────────────────────────────────
#
# The cut is sampled at a uniform step of one pixel ALONG the path (arc length),
# not at the integer pixels a line rasteriser visits: the offset axis is then
# linear, so the diagram has an exact FITS WCS (CDELT1 = step), and diagonal or
# curved paths are not stretched. Values are bilinear (
# NaN-aware: a blank neighbour is dropped and the weights renormalised, so a
# sample next to a masked pixel keeps its value) at sub-pixel positions, averaged over ``width_pixels`` samples one
# pixel apart along the local normal. Vertices may be fractional: an edited
# path is not snapped to the pixel grid.

PV_STEP_PIXELS = 1.0


def canonical_pv_vertices(vertices: list[list[float]]) -> list[list[float]]:
    """Vertices rounded to 1/1000 pixel. The product cache keys on them and the
    worker computes on them, so both must see the SAME values: a key built from
    rounded vertices while the cut used the raw ones could serve a diagram of a
    different length."""
    return [[round(float(c), 3) for c in v[:2]] for v in vertices if len(v) >= 2]


def _clean_vertices(vertices: list[list[float]]) -> np.ndarray:
    cleaned: list[tuple[float, float]] = []
    for v in canonical_pv_vertices(vertices):
        if len(v) < 2:
            continue
        pt = (float(v[0]), float(v[1]))
        if not (np.isfinite(pt[0]) and np.isfinite(pt[1])):
            continue
        if cleaned and np.hypot(pt[0] - cleaned[-1][0], pt[1] - cleaned[-1][1]) < 1e-6:
            continue
        cleaned.append(pt)
    if len(cleaned) < 2:
        raise ValueError("At least two distinct vertices required for a PV cut.")
    return np.asarray(cleaned, dtype=np.float64)


def _sample_polyline(vertices: np.ndarray, step: float = PV_STEP_PIXELS) -> tuple[np.ndarray, np.ndarray, float]:
    """Points every ``step`` (in the vertices' units) along the polyline, and the
    unit normal at each.

    Returns ``(points (n,2) x/y, normals (n,2), total_length)``. The samples sit at
    arc lengths ``0, step, 2·step, …`` up to the path length; a remainder shorter
    than one step at the end is dropped, which keeps the offset axis exactly
    linear. The normal of a sample is that of the segment it lies on.
    """
    seg = np.diff(vertices, axis=0)
    seg_len = np.hypot(seg[:, 0], seg[:, 1])
    keep = seg_len > 0
    seg, seg_len, starts = seg[keep], seg_len[keep], vertices[:-1][keep]
    total = float(seg_len.sum())
    cum = np.concatenate([[0.0], np.cumsum(seg_len)])
    n = int(np.floor(total / step + 1e-9)) + 1
    s = np.arange(n, dtype=np.float64) * step
    idx = np.clip(np.searchsorted(cum, s, side="right") - 1, 0, len(seg_len) - 1)
    t = (s - cum[idx]) / seg_len[idx]
    points = starts[idx] + seg[idx] * t[:, None]
    tangents = seg[idx] / seg_len[idx][:, None]
    normals = np.stack([-tangents[:, 1], tangents[:, 0]], axis=1)
    return points, normals, total


def _bilinear_mean(cube: np.ndarray, cx: np.ndarray, cy: np.ndarray, n_samples: int,
                   width_pixels: int, x_limit: int, y_limit: int) -> np.ndarray:
    """(depth, n_samples): per channel, the NaN-aware bilinear value at each
    (cx, cy) (slab coordinates, ``n_samples × width_pixels`` of them), averaged
    over the width. Samples outside the cube (``x_limit``/``y_limit`` = cube
    extent in slab coordinates) are blank."""
    depth, sh, sw = cube.shape
    inside = (cx >= -1e-9) & (cy >= -1e-9) & (cx <= x_limit - 1 + 1e-9) & (cy <= y_limit - 1 + 1e-9)
    ix0 = np.clip(np.floor(cx).astype(np.int64), 0, sw - 1)
    iy0 = np.clip(np.floor(cy).astype(np.int64), 0, sh - 1)
    fx = np.clip(cx - ix0, 0.0, 1.0)
    fy = np.clip(cy - iy0, 0.0, 1.0)
    ix1 = np.minimum(ix0 + 1, sw - 1)
    iy1 = np.minimum(iy0 + 1, sh - 1)
    corners = (
        (iy0, ix0, (1 - fx) * (1 - fy)),
        (iy0, ix1, fx * (1 - fy)),
        (iy1, ix0, (1 - fx) * fy),
        (iy1, ix1, fx * fy),
    )
    n = cx.size
    out = np.full((depth, n_samples), np.nan, dtype=np.float32)
    # ~4 M gathered values per block. ``cube`` stays a lazy memmap slab: the fancy
    # indexing below reads only the neighbour pixels, never whole planes.
    chunk = max(1, 4_000_000 // max(1, n))
    for z0 in range(0, depth, chunk):
        c = cube[z0 : z0 + chunk]
        nz = min(chunk, depth - z0)
        acc = np.zeros((nz, n), dtype=np.float64)
        wsum = np.zeros((nz, n), dtype=np.float64)
        for iy, ix, w in corners:
            v = np.asarray(c[:, iy, ix], dtype=np.float64)
            ok = np.isfinite(v) & (w > 0)
            acc += np.where(ok, v, 0.0) * w
            wsum += ok * w
        with np.errstate(invalid="ignore", divide="ignore"):
            vals = np.where((wsum > 0) & inside, acc / wsum, np.nan)
        vals = vals.reshape(nz, n_samples, width_pixels)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", category=RuntimeWarning)   # all-NaN mean
            out[z0 : z0 + chunk] = np.nanmean(vals, axis=2)
    return out


def _pixel_to_plane_arcsec(dataset: ScientificFitsDataset) -> np.ndarray:
    """2×2 matrix taking a pixel offset (dx, dy) to an offset on the projection
    plane in arcsec (the celestial PC/CD matrix × CDELT). Pixels need not be
    square nor the axes orthogonal: distances and normals measured with this
    matrix are angular, which per-axis scales alone are not on a skewed grid."""
    if dataset.wcs is not None:
        try:
            celestial = dataset.wcs.celestial
            if celestial is not None and celestial.pixel_n_dim >= 2:
                m = np.asarray(celestial.pixel_scale_matrix, dtype=np.float64)[:2, :2] * 3600.0
                if np.all(np.isfinite(m)) and abs(np.linalg.det(m)) > 0:
                    return m
        except Exception:  # noqa: BLE001 — fall back to the dataset's mean scale
            pass
    mean = float(dataset.spatial_pixel_scale_arcsec())
    return np.diag([mean, mean])


def _require_spectral_last(dataset: ScientificFitsDataset) -> None:
    # squeeze_to_3d keeps the FIRST three active FITS axes (a 4th, e.g. a full
    # STOKES axis, is dropped as numpy's leading axis, plane 0 kept) and yields
    # (spectral, y, x) only when the spectral axis is the third of them. A
    # transposed cube would be cut along a spatial axis while being labelled with
    # the spectral one, so refuse it.
    kept = dataset.active_axes[:3]
    if len(kept) == 3 and dataset.cube_numpy_spectral_fits_axis() != kept[2]:
        raise ValueError(
            "PV extraction needs the spectral axis to be the cube's last axis "
            "(e.g. NAXIS3); this cube's spectral axis is elsewhere."
        )


def compute_pv(path: str, vertices: list[list[float]], width_pixels: int) -> dict[str, Any]:
    """The PV diagram as arrays: ``pv`` (depth, n_samples), sample geometry, and
    the spectral axis (coordinates + linear WCS) and cube metadata."""
    verts = _clean_vertices(vertices)
    width_pixels = max(1, int(width_pixels))
    dataset = ScientificFitsDataset(path)
    _require_spectral_last(dataset)
    # Sample in ANGULAR space (arcsec along x and y, locally linear), one step =
    # the finer pixel size: the offset axis is then linear in arcsec whatever the
    # path direction, also when pixels are not square. Square pixels → one pixel.
    m = _pixel_to_plane_arcsec(dataset)
    m_inv = np.linalg.inv(m)
    step_arcsec = float(np.min(np.hypot(m[0], m[1])))   # the shorter pixel side
    points_sky, normals_sky, _ = _sample_polyline(verts @ m.T, step=step_arcsec)
    total_length = float(np.hypot(*np.diff(verts, axis=0).T).sum())   # pixels, informative
    half = 0.5 * float(width_pixels - 1)
    offsets = (np.arange(width_pixels, dtype=np.float64) - half) * step_arcsec
    # (n_samples, width, 2) sample positions on the plane, back to pixels.
    sky = points_sky[:, None, :] + offsets[None, :, None] * normals_sky[:, None, :]
    pix = sky @ m_inv.T
    sx, sy = pix[..., 0], pix[..., 1]
    points = points_sky @ m_inv.T

    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("PV extraction requires a 3D cube.")
        depth, height, width = data.shape
        # Read only the spatial bounding box of the samples (+1 for the bilinear
        # neighbour), across all channels.
        x0 = max(0, int(np.floor(sx.min())) - 1)
        x1 = min(width - 1, int(np.ceil(sx.max())) + 1)
        y0 = max(0, int(np.floor(sy.min())) - 1)
        y1 = min(height - 1, int(np.ceil(sy.max())) + 1)
        if x0 > x1 or y0 > y1:
            raise ValueError("The PV path lies outside the cube.")
        n_samples = points.shape[0]
        # The slab view stays lazy (memmap): _bilinear_mean reads it a block of
        # channels at a time, so a long diagonal never materialises the cube.
        pv = _bilinear_mean(data[:, y0 : y1 + 1, x0 : x1 + 1], (sx - x0).ravel(), (sy - y0).ravel(),
                            n_samples, width_pixels, width - x0, height - y0)
        spectral = dataset.spectral_axis_metadata(0, depth - 1)
    finally:
        hdul.close()
    valid_samples = int(np.isfinite(pv).sum())
    if valid_samples <= 0:
        raise ValueError("No valid data found along the PV cut.")

    coordinates = np.asarray(spectral["coordinates"], dtype=np.float64)
    if coordinates.size >= 2:
        crval, cdelt = float(coordinates[0]), float(coordinates[1] - coordinates[0])
    else:
        crval, cdelt = (float(coordinates[0]) if coordinates.size else 0.0), 1.0
    logger.info(
        "[pv] path=%s samples=%s width=%s slab_x=%s..%s slab_y=%s..%s spectral=%s[%s]",
        dataset.path, n_samples, width_pixels, x0, x1, y0, y1,
        spectral["axis_type"], spectral["axis_unit"] or "-",
    )
    return {
        "pv": pv,
        # Offsets in steps (one step = the finer pixel) and the step's angular size.
        "positions": np.arange(n_samples, dtype=np.float64),
        "step_pixels": PV_STEP_PIXELS,
        "pixel_scale_arcsec": float(step_arcsec),
        "sky_matrix_arcsec": m,
        "vertices": verts,
        "width_pixels": width_pixels,
        "total_length": total_length,
        "valid_samples": valid_samples,
        "spectral_coordinates": coordinates,
        "spectral_crval": crval,
        "spectral_cdelt": cdelt,
        "spectral_ctype": str(spectral["axis_type"]),
        "spectral_cunit": str(spectral["axis_unit"]),
        "dataset": dataset,
    }


def worker_pv(path: str, vertices: list[list[float]], width_pixels: int) -> dict[str, Any]:
    r = compute_pv(path, vertices, width_pixels)
    pv = r["pv"]
    dataset = r["dataset"]
    positions = r["positions"].astype(np.float32)
    positions_arcsec = positions * r["pixel_scale_arcsec"]
    pos_compression, pos_b64 = _b64f32_compressed(positions)
    _, pos_arcsec_b64 = _b64f32_compressed(positions_arcsec)
    _, data_b64 = _b64f32_compressed(pv.reshape(-1))
    return {
        "num_samples": int(pv.shape[1]),
        "depth": int(pv.shape[0]),
        "scalar_type": "float32",
        "compression": pos_compression,
        "positions_base64": pos_b64,
        "positions_arcsec_base64": pos_arcsec_b64,
        "pixel_scale_arcsec_per_pixel": r["pixel_scale_arcsec"],
        "spatial_unit": "arcsec",
        "data_base64": data_b64,
        "computed_on": "spatial_slab",
        "width_pixels": r["width_pixels"],
        "vertex_count": int(r["vertices"].shape[0]),
        "total_length": float(r["total_length"]),
        "valid_samples": r["valid_samples"],
        "step_pixels": r["step_pixels"],
        # Pixel offset → projection-plane arcsec, row-major [[a, b], [c, d]]: the
        # geometry the cut was sampled in, so a client can draw the same band.
        "sky_matrix_arcsec": [float(v) for v in np.asarray(r["sky_matrix_arcsec"]).ravel()],
        "spectral_axis_type": r["spectral_ctype"],
        "spectral_axis_unit": r["spectral_cunit"],
        # Channel coordinates in the axis unit, channel 0 first: the plot's y
        # values. float64 — a float32 frequency in Hz loses the channel width.
        "spectral_coordinates_base64": base64.b64encode(
            np.ascontiguousarray(r["spectral_coordinates"], dtype="<f8").tobytes()).decode("ascii"),
        "spectral_crval": r["spectral_crval"],
        "spectral_cdelt": r["spectral_cdelt"],
        "bunit": dataset.bunit,
        "beam_major": dataset.beam.major,
        "beam_minor": dataset.beam.minor,
        "beam_pa": dataset.beam.pa,
    }


def _copy_spectral_axis(dataset: ScientificFitsDataset, hdr, out_axis: int) -> None:
    """The cube's own spectral-axis WCS, renumbered to ``out_axis``. Every channel
    is kept and in order, so CRPIX carries over unchanged; copying the
    algorithm (e.g. ``VOPT-F2W``), its parameters and the PC/CD scale keeps a
    non-linear axis exact, which a CRVAL/CDELT pair taken from two channels
    would not."""
    n = dataset.cube_numpy_spectral_fits_axis()
    src, raw = dataset.sanitized_header, dataset.header

    def get(key, default=None):
        return src.get(key, raw.get(key, default))

    hdr[f"CTYPE{out_axis}"] = str(get(f"CTYPE{n}", "SPECTRAL"))
    if get(f"CUNIT{n}") is not None:
        hdr[f"CUNIT{out_axis}"] = str(get(f"CUNIT{n}"))
    hdr[f"CRPIX{out_axis}"] = float(get(f"CRPIX{n}", 1.0))
    hdr[f"CRVAL{out_axis}"] = float(get(f"CRVAL{n}", 0.0))
    if get(f"CD{n}_{n}") is not None:
        cdelt = float(get(f"CD{n}_{n}"))
    else:
        cdelt = float(get(f"CDELT{n}", 1.0)) * float(get(f"PC{n}_{n}", 1.0))
    hdr[f"CDELT{out_axis}"] = cdelt
    for kind in ("PV", "PS"):
        for m in range(0, 100):
            if get(f"{kind}{n}_{m}") is not None:
                hdr[f"{kind}{out_axis}_{m}"] = get(f"{kind}{n}_{m}")
    if get(f"CNAME{n}") is not None:
        hdr[f"CNAME{out_axis}"] = get(f"CNAME{n}")


def worker_pv_save_fits(path: str, output_path: str, vertices: list[list[float]],
                        width_pixels: int) -> dict[str, Any]:
    """Compute the PV diagram and write it as a 2-D FITS image with WCS.

    Axis 1 is the offset along the path (CTYPE1 ``OFFSET``, arcsec, 0 at the first
    vertex), axis 2 the cube's own spectral axis WCS (see _copy_spectral_axis). BUNIT, beam, rest frequency and SPECSYS
    come from the cube; the path is written as HISTORY in pixel and sky coordinates.
    """
    from astropy.io import fits  # noqa: PLC0415

    r = compute_pv(path, vertices, width_pixels)
    dataset = r["dataset"]
    src = dataset.header
    hdr = fits.Header()
    hdr["CTYPE1"] = ("OFFSET", "Offset along the PV path")
    hdr["CUNIT1"] = "arcsec"
    hdr["CRPIX1"] = 1.0
    hdr["CRVAL1"] = 0.0
    hdr["CDELT1"] = r["step_pixels"] * r["pixel_scale_arcsec"]
    _copy_spectral_axis(dataset, hdr, out_axis=2)
    for key in ("RESTFRQ", "RESTFREQ", "RESTWAV", "RESTWAVE", "SPECSYS", "SSYSOBS", "VELOSYS",
                "VELREF", "ZSOURCE", "OBJECT", "TELESCOP", "EQUINOX", "RADESYS", "MJD-OBS", "DATE-OBS"):
        if key in src:
            hdr[key] = src[key]
    if dataset.bunit:
        hdr["BUNIT"] = dataset.bunit
    beam = dataset.beam
    if beam.major and beam.minor:
        hdr["BMAJ"] = float(beam.major)
        hdr["BMIN"] = float(beam.minor)
        hdr["BPA"] = float(beam.pa or 0.0)
    hdr["PVWIDTH"] = (int(r["width_pixels"]), "PV path width (cube pixels)")
    hdr["PVSTEP"] = (float(r["step_pixels"] * r["pixel_scale_arcsec"]), "Sample step along the path (arcsec)")
    hdr["HISTORY"] = f"VisIVO PV diagram of {Path(dataset.path).name}"
    sky = None
    if dataset.wcs is not None:
        try:
            sky = dataset.wcs.celestial.pixel_to_world_values(r["vertices"][:, 0], r["vertices"][:, 1])
        except Exception:  # noqa: BLE001 — sky positions are informative only
            sky = None
    # One card per line, each within the 72 characters a HISTORY card holds: a
    # longer line is wrapped by astropy in the middle of a number.
    hdr["HISTORY"] = "PV vertices: 0-based pixel (x, y); world (deg) in the cube's frame"
    for i, (x, y) in enumerate(r["vertices"]):
        line = f"PV vertex {i}: pix {x:.3f} {y:.3f}"
        if sky is not None:
            line += f" world {float(sky[0][i]):.6f} {float(sky[1][i]):.6f}"
        hdr["HISTORY"] = line
    fits.PrimaryHDU(data=r["pv"], header=hdr).writeto(output_path, overwrite=False)
    finite = r["pv"][np.isfinite(r["pv"])]
    return {
        "output_path": output_path,
        "width": int(r["pv"].shape[1]),
        "height": int(r["pv"].shape[0]),
        "range_min": float(finite.min()) if finite.size else 0.0,
        "range_max": float(finite.max()) if finite.size else 0.0,
        "bunit": dataset.bunit or "",
    }


# ── Worker: per-channel noise (MAD → σ) ──────────────────────────────────────
