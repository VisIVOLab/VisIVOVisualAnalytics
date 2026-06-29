"""Velocity-field ingest + vector-field geometry for the cosmic-flow viewer (F1).

A peculiar-velocity / flow field is a 3-component vector field on a 3D grid
(supergalactic Cartesian, Mpc). It is NOT a scalar cube, so it gets its own
loader + canonical representation here, independent of how it arrives on disk:

  * multi-component FITS — a single FITS whose component axis (NAXIS4 = 3) holds
    vx, vy, vz over an (nx, ny, nz) grid. (CF4 reconstructions, e.g.
    CF4_new_64-z008_velocity.fits → FITS dims (64,64,64,3) = numpy (3,64,64,64).)
  * three separate FITS — vx.fits / vy.fits / vz.fits, one component each.

Both funnel into `VelocityField`. The viewer then renders glyphs (arrows) and
streamlines; gravitational-basin/watershed segmentation is a later phase.

The header of a raw reconstruction often lacks WCS / box size / units, so the
physical extent is a caller-supplied parameter (`box_mpc`); when absent we fall
back to unit spacing and document it.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import Any

import numpy as np
from astropy.io import fits

logger = logging.getLogger("visivo.velocityfield")


@dataclass
class VelocityField:
    """Canonical 3-component velocity field on a regular grid.

    Components are numpy arrays of shape (nz, ny, nx) (C order), matching how a
    FITS image deserialises. `vx`/`vy`/`vz` are the Cartesian components in the
    grid's own axis order (assumed supergalactic unless the header says
    otherwise). Spacing/origin are in Mpc (or grid units when box size unknown).
    """

    vx: np.ndarray
    vy: np.ndarray
    vz: np.ndarray
    spacing: tuple[float, float, float]
    origin: tuple[float, float, float]
    unit: str
    frame: str
    box_known: bool

    @property
    def shape(self) -> tuple[int, int, int]:
        return self.vx.shape  # (nz, ny, nx)

    @property
    def magnitude(self) -> np.ndarray:
        return np.sqrt(self.vx * self.vx + self.vy * self.vy + self.vz * self.vz)

    def summary(self) -> dict[str, Any]:
        mag = self.magnitude
        finite = np.isfinite(mag)
        return {
            "shape": [int(s) for s in self.shape],            # (nz, ny, nx)
            "n_cells": int(self.vx.size),
            "spacing": list(self.spacing),
            "origin": list(self.origin),
            "unit": self.unit,
            "frame": self.frame,
            "box_known": self.box_known,
            "speed_min": float(np.nanmin(mag)) if finite.any() else 0.0,
            "speed_max": float(np.nanmax(mag)) if finite.any() else 0.0,
            "speed_mean": float(np.nanmean(mag[finite])) if finite.any() else 0.0,
            "finite_fraction": float(finite.mean()),
        }


def _spacing_origin(n: tuple[int, int, int], box_mpc: float | None):
    """Spacing/origin for an (nx, ny, nz) grid centred on the origin.

    With a physical box size the grid spans [-box/2, +box/2] on each axis
    (reconstructions are centred on the Local Group / observer); without it we
    use unit spacing centred on zero and flag box_known=False.
    """
    nx, ny, nz = n
    if box_mpc and box_mpc > 0:
        sx = box_mpc / nx
        sy = box_mpc / ny
        sz = box_mpc / nz
        return (sx, sy, sz), (-0.5 * box_mpc, -0.5 * box_mpc, -0.5 * box_mpc), True
    return (1.0, 1.0, 1.0), (-0.5 * nx, -0.5 * ny, -0.5 * nz), False


def load_multicomponent_fits(path: str, box_mpc: float | None = None) -> VelocityField:
    """Load a single FITS whose component axis (NAXIS4 == 3) holds vx, vy, vz."""
    with fits.open(path, memmap=False) as hdul:
        hdu = hdul[0]
        data = np.asarray(hdu.data, dtype=np.float32)
        hdr = hdu.header
    if data.ndim != 4 or data.shape[0] != 3:
        raise ValueError(
            f"expected a 4-D FITS with a length-3 component axis (numpy shape "
            f"(3, nz, ny, nx)); got shape {data.shape}"
        )
    vx, vy, vz = data[0], data[1], data[2]
    nz, ny, nx = vx.shape
    spacing, origin, box_known = _spacing_origin((nx, ny, nz), box_mpc)
    unit = str(hdr.get("BUNIT", "") or "").strip() or "km/s (assumed)"
    frame = str(hdr.get("VFRAME", "") or "").strip() or "supergalactic (assumed)"
    logger.info(
        "[velocityfield] loaded %s grid=%dx%dx%d box_known=%s unit=%s",
        path, nx, ny, nz, box_known, unit,
    )
    return VelocityField(vx, vy, vz, spacing, origin, unit, frame, box_known)


def load_component_fits(
    vx_path: str, vy_path: str, vz_path: str, box_mpc: float | None = None
) -> VelocityField:
    """Load three separate single-component FITS files (vx, vy, vz)."""
    comps = []
    for p in (vx_path, vy_path, vz_path):
        with fits.open(p, memmap=False) as hdul:
            comps.append(np.asarray(hdul[0].data, dtype=np.float32))
    vx, vy, vz = comps
    if not (vx.shape == vy.shape == vz.shape and vx.ndim == 3):
        raise ValueError(
            f"the three component FITS must share one 3-D shape; got "
            f"{vx.shape}, {vy.shape}, {vz.shape}"
        )
    nz, ny, nx = vx.shape
    spacing, origin, box_known = _spacing_origin((nx, ny, nz), box_mpc)
    return VelocityField(vx, vy, vz, spacing, origin, "km/s (assumed)",
                         "supergalactic (assumed)", box_known)


def load_velocity_field(path: str, box_mpc: float | None = None) -> VelocityField:
    """Auto-detecting entry point. Currently dispatches the multi-component FITS;
    callers with three files use load_component_fits directly."""
    return load_multicomponent_fits(path, box_mpc)


def to_vtk_image(field: "VelocityField"):
    """Build a vtkImageData carrying a 3-component VECTOR point-data array.

    This is the input both vtkGlyph3D (arrows) and vtkStreamTracer (streamlines)
    consume in the viewer / server-side renderer.
    """
    import vtk
    from vtk.util import numpy_support

    nz, ny, nx = field.shape
    img = vtk.vtkImageData()
    img.SetDimensions(nx, ny, nz)
    img.SetSpacing(*field.spacing)
    img.SetOrigin(*field.origin)
    # Interleave components into (npts, 3), point order = x fastest (VTK/FITS x = axis -1).
    vectors = np.empty((field.vx.size, 3), dtype=np.float32)
    vectors[:, 0] = field.vx.ravel(order="C")
    vectors[:, 1] = field.vy.ravel(order="C")
    vectors[:, 2] = field.vz.ravel(order="C")
    arr = numpy_support.numpy_to_vtk(vectors, deep=True, array_type=vtk.VTK_FLOAT)
    arr.SetName("velocity")
    img.GetPointData().SetVectors(arr)
    return img


def _block_average(a: np.ndarray, factor: int) -> np.ndarray:
    """Block-average a 3-D array by an integer factor per axis (crop to a
    multiple). Preserves the field far better than stride-decimation."""
    if factor <= 1:
        return a
    nz, ny, nx = a.shape
    cz, cy, cx = (nz // factor) * factor, (ny // factor) * factor, (nx // factor) * factor
    if cz == 0 or cy == 0 or cx == 0:
        return a
    a = a[:cz, :cy, :cx]
    return a.reshape(cz // factor, factor, cy // factor, factor, cx // factor, factor).mean(
        axis=(1, 3, 5)
    )


def downsample(field: "VelocityField", max_dim: int) -> "VelocityField":
    """Return a display-resolution copy whose largest dimension is <= max_dim,
    block-averaging the vector components. Spacing grows by the same factor so
    the physical extent is preserved. No-op when already small enough."""
    nz, ny, nx = field.shape
    biggest = max(nx, ny, nz)
    if max_dim <= 0 or biggest <= max_dim:
        return field
    import math
    factor = int(math.ceil(biggest / max_dim))
    sx, sy, sz = field.spacing
    return VelocityField(
        _block_average(field.vx, factor).astype(np.float32),
        _block_average(field.vy, factor).astype(np.float32),
        _block_average(field.vz, factor).astype(np.float32),
        (sx * factor, sy * factor, sz * factor),
        field.origin,
        field.unit,
        field.frame,
        field.box_known,
    )


def grid_raw_payload(field: "VelocityField") -> dict[str, Any]:
    """Serialise the field as the desktop viewer wants it: interleaved
    little-endian float32 vectors (npts × 3, point order x-fastest = vtkImageData
    order) + grid metadata. The desktop rebuilds the vtkImageData locally and
    runs vtkStreamTracer / vtkGlyph3D itself (interactive)."""
    nz, ny, nx = field.shape
    vectors = np.empty((field.vx.size, 3), dtype="<f4")
    vectors[:, 0] = field.vx.ravel(order="C")
    vectors[:, 1] = field.vy.ravel(order="C")
    vectors[:, 2] = field.vz.ravel(order="C")
    mag = field.magnitude
    finite = np.isfinite(mag)
    return {
        "raw_bytes": vectors.tobytes(),
        "nx": int(nx), "ny": int(ny), "nz": int(nz),
        "spacing": field.spacing, "origin": field.origin,
        "unit": field.unit, "frame": field.frame,
        "speed_min": float(np.nanmin(mag)) if finite.any() else 0.0,
        "speed_max": float(np.nanmax(mag)) if finite.any() else 0.0,
    }


# ── Top-level workers (picklable for the ProcessPoolExecutor used by _run) ─────

def velocity_summary_worker(path: str, box_mpc: float | None = None) -> dict[str, Any]:
    """Load + return just the lightweight summary (for /v1/velocity/open)."""
    return load_velocity_field(path, box_mpc).summary()


def velocity_grid_worker(path: str, box_mpc: float | None = None,
                         max_dim: int = 0) -> dict[str, Any]:
    """Load + return the binary grid payload (for /v1/velocity/grid_bin).

    `max_dim` > 0 block-averages the field down so its largest dimension is
    <= max_dim — the display-LOD path that keeps transfer + client memory bounded
    for large (256³/512³) reconstructions. The result records the applied factor.
    """
    field = load_velocity_field(path, box_mpc)
    full = field.shape
    if max_dim and max_dim > 0:
        field = downsample(field, max_dim)
    payload = grid_raw_payload(field)
    payload["full_shape"] = [int(s) for s in full]            # (nz, ny, nx) before LOD
    payload["downsample_factor"] = int(round(full[0] / field.shape[0])) if field.shape[0] else 1
    return payload


def compute_streamlines(field: "VelocityField", n_seeds: int = 400,
                        max_propagation: float | None = None) -> int:
    """Trace streamlines through the field (VTK vtkStreamTracer), seeded on a
    sphere at the grid centre. Returns the number of streamline cells produced —
    used by the headless test; the viewer keeps the polydata for rendering."""
    import vtk

    img = to_vtk_image(field)
    nz, ny, nx = field.shape
    bounds = img.GetBounds()
    cx = 0.5 * (bounds[0] + bounds[1])
    cy = 0.5 * (bounds[2] + bounds[3])
    cz = 0.5 * (bounds[4] + bounds[5])
    radius = 0.25 * max(bounds[1] - bounds[0], bounds[3] - bounds[2], bounds[5] - bounds[4])
    if max_propagation is None:
        max_propagation = 4.0 * max(bounds[1] - bounds[0], bounds[3] - bounds[2],
                                    bounds[5] - bounds[4])

    seeds = vtk.vtkPointSource()
    seeds.SetCenter(cx, cy, cz)
    seeds.SetRadius(radius)
    seeds.SetNumberOfPoints(n_seeds)

    tracer = vtk.vtkStreamTracer()
    tracer.SetInputData(img)
    tracer.SetSourceConnection(seeds.GetOutputPort())
    tracer.SetIntegratorTypeToRungeKutta45()
    tracer.SetIntegrationDirectionToBoth()
    tracer.SetMaximumPropagation(max_propagation)
    tracer.Update()

    out = tracer.GetOutput()
    return int(out.GetNumberOfCells()) if out else 0
