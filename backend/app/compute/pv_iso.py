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


def _sample_polyline(vertices: list[tuple[int, int]]) -> tuple[list[tuple[int, int]], float]:
    sampled: list[tuple[int, int]] = []
    total = 0.0
    for seg in range(1, len(vertices)):
        s, e = vertices[seg - 1], vertices[seg]
        dx, dy = e[0] - s[0], e[1] - s[1]
        steps = max(abs(dx), abs(dy))
        if steps <= 0:
            if not sampled or sampled[-1] != s:
                sampled.append(s)
            continue
        for step in range(steps + 1):
            t = float(step) / float(steps)
            pt = (int(round(s[0] + t * dx)), int(round(s[1] + t * dy)))
            if sampled and sampled[-1] == pt:
                continue
            if sampled:
                total += float(np.hypot(pt[0] - sampled[-1][0], pt[1] - sampled[-1][1]))
            sampled.append(pt)
    return sampled, total


def _local_normal(pts: list[tuple[int, int]], i: int) -> tuple[float, float]:
    prev = pts[i if i == 0 else i - 1]
    nxt = pts[i + 1 if i + 1 < len(pts) else i]
    tx, ty = float(nxt[0] - prev[0]), float(nxt[1] - prev[1])
    length = float(np.hypot(tx, ty))
    if length <= 0:
        return 0.0, 1.0
    return -ty / length, tx / length


def worker_pv(path: str, vertices: list[list[int]], width_pixels: int) -> dict[str, Any]:
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("PV extraction requires a 3D cube.")

        cleaned = []
        for v in vertices:
            if len(v) >= 2:
                pt = (int(v[0]), int(v[1]))
                if not cleaned or cleaned[-1] != pt:
                    cleaned.append(pt)
        if len(cleaned) < 2:
            raise ValueError("At least two distinct vertices required for a PV cut.")

        sampled, total_length = _sample_polyline(cleaned)
        depth, height, width = data.shape
        half = 0.5 * float(max(1, width_pixels) - 1)
        x_candidates = [pt[0] for pt in sampled]
        y_candidates = [pt[1] for pt in sampled]
        spatial_margin = max(1, int(np.ceil(abs(half))) + 2)
        slab_x0 = max(0, min(x_candidates) - spatial_margin)
        slab_x1 = min(width - 1, max(x_candidates) + spatial_margin)
        slab_y0 = max(0, min(y_candidates) - spatial_margin)
        slab_y1 = min(height - 1, max(y_candidates) + spatial_margin)

        # Materialise only the spatial slab needed by the PV path. This still reads
        # the full spectral depth, but avoids eager loading the entire cube volume.
        cube = np.asarray(data[:, slab_y0 : slab_y1 + 1, slab_x0 : slab_x1 + 1], dtype=np.float32)
        spectral = dataset.spectral_axis_metadata(0, depth - 1)
        pixel_scale_arcsec = dataset.spatial_pixel_scale_arcsec()
        logger.info(
            "[pv] slab read path=%s full_depth=%s slab_x=%s..%s slab_y=%s..%s width_pixels=%s samples=%s spectral=%s[%s]",
            dataset.path,
            depth,
            slab_x0,
            slab_x1,
            slab_y0,
            slab_y1,
            width_pixels,
            len(sampled),
            spectral["axis_type"],
            spectral["axis_unit"] or "-",
        )
    finally:
        hdul.close()

    sampled_local = [(pt[0] - slab_x0, pt[1] - slab_y0) for pt in sampled]
    x_samples = len(sampled_local)
    positions = np.zeros(x_samples, dtype=np.float32)
    pv = np.full((depth, x_samples), np.nan, dtype=np.float32)
    valid_samples = 0

    for i in range(1, x_samples):
        positions[i] = positions[i - 1] + float(
            np.hypot(
                sampled_local[i][0] - sampled_local[i - 1][0],
                sampled_local[i][1] - sampled_local[i - 1][1],
            )
        )

    slab_height = cube.shape[1]
    slab_width = cube.shape[2]
    for si, center in enumerate(sampled_local):
        nx, ny = _local_normal(sampled_local, si)
        for z in range(depth):
            vals: list[float] = []
            for off in range(max(1, width_pixels)):
                cx = int(round(center[0] + (float(off) - half) * nx))
                cy = int(round(center[1] + (float(off) - half) * ny))
                if 0 <= cx < slab_width and 0 <= cy < slab_height:
                    v_val = float(cube[z, cy, cx])
                    if np.isfinite(v_val):
                        vals.append(v_val)
            if vals:
                pv[z, si] = float(np.mean(vals))
                valid_samples += 1

    if valid_samples <= 0:
        raise ValueError("No valid data found along the PV cut.")

    positions_arcsec = positions * pixel_scale_arcsec
    pos_compression, pos_b64 = _b64f32_compressed(positions)
    pos_arcsec_compression, pos_arcsec_b64 = _b64f32_compressed(positions_arcsec)
    data_compression, data_b64 = _b64f32_compressed(pv.reshape(-1))
    return {
        "num_samples": x_samples,
        "depth": int(pv.shape[0]),
        "scalar_type": "float32",
        "compression": pos_compression,
        "positions_base64": pos_b64,
        "positions_arcsec_base64": pos_arcsec_b64,
        "pixel_scale_arcsec_per_pixel": pixel_scale_arcsec,
        "spatial_unit": "arcsec",
        "data_base64": data_b64,
        "computed_on": "spatial_slab",
        "width_pixels": width_pixels,
        "vertex_count": len(cleaned),
        "total_length": float(total_length),
        "valid_samples": valid_samples,
        "spectral_axis_type": str(spectral["axis_type"]),
        "spectral_axis_unit": str(spectral["axis_unit"]),
        "bunit": dataset.bunit,
        "beam_major": dataset.beam.major,
        "beam_minor": dataset.beam.minor,
        "beam_pa": dataset.beam.pa,
    }


# ── Worker: per-channel noise (MAD → σ) ──────────────────────────────────────
