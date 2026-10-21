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
import os
from dataclasses import dataclass, field
from typing import Any

import numpy as np
from astropy.io import fits

from .fits_dataset import image_hdu_index

logger = logging.getLogger("visivo.velocityfield")

# R-16: above this materialised field size, the full-resolution divergence is
# computed by streaming z-slabs from a memmapped 4-D FITS instead of loading all
# three components + gradient temporaries into RAM (~4–5× the field). Keeps peak
# memory O(slab·ny·nx) so 512³ fields don't OOM. Set to 0 to always stream.
_VELOCITY_STREAM_BYTES = int(os.environ.get("VISIVO_VELOCITY_STREAM_BYTES", str(512 * 1024 ** 2)))
_VELOCITY_SLAB = max(1, int(os.environ.get("VISIVO_VELOCITY_SLAB", "32")))

# Planck18 for the linear-theory density calibration δ ≈ −∇·v / (f·a·H0). At
# z ≈ 0 (a=1): f = Ωm^0.55 (growth rate), H0 in km/s/Mpc. A client can turn the
# raw ∇·v [(km/s)/Mpc] grid into a dimensionless overdensity by multiplying by
# −``linear_density_factor``. We do NOT bake it into the grid (the viewer expects
# ∇·v), we just expose the factor + mark the grid uncalibrated (R-15-style honesty).
_PLANCK18_OM = 0.3111
_PLANCK18_H0 = 67.66
_GROWTH_F = _PLANCK18_OM ** 0.55


def _linear_density_factor(box_known: bool) -> float | None:
    """1/(f·H0) [Mpc·s/km] so δ ≈ −∇·v · factor. None when the box (hence the
    Mpc spacing that makes ∇·v physical) is unknown."""
    return (1.0 / (_GROWTH_F * _PLANCK18_H0)) if box_known else None


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
    # R-11: non-fatal caveats about the grid's geometry/orientation (assumed
    # frame, flipped axes, missing WCS). Surfaced so the viewer can flag that a
    # figure may be mirror-imaged rather than presenting it as ground truth.
    warnings: list[str] = field(default_factory=list)

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
            "warnings": list(self.warnings),
            "speed_min": float(np.nanmin(mag)) if finite.any() else 0.0,
            "speed_max": float(np.nanmax(mag)) if finite.any() else 0.0,
            "speed_mean": float(np.nanmean(mag[finite])) if finite.any() else 0.0,
            "finite_fraction": float(finite.mean()),
        }


def _spacing_origin(n: tuple[int, int, int], box_mpc: float | None):
    """Spacing/origin for an (nx, ny, nz) grid centred on the origin.

    Point-centred model (matches the desktop viewer): the grid POINTS span
    exactly [-box/2, +box/2] per axis, so spacing = box/(n-1) and the centred
    origin is -box/2. Without a physical box we use unit spacing, origin
    -(n-1)/2 (still centred) and flag box_known=False.
    """
    nx, ny, nz = n
    if box_mpc and box_mpc > 0:
        sx = box_mpc / max(1, nx - 1)
        sy = box_mpc / max(1, ny - 1)
        sz = box_mpc / max(1, nz - 1)
        return (sx, sy, sz), (-0.5 * box_mpc, -0.5 * box_mpc, -0.5 * box_mpc), True
    return (1.0, 1.0, 1.0), (-0.5 * (nx - 1), -0.5 * (ny - 1), -0.5 * (nz - 1)), False


def _geometry_warnings(hdr: "fits.Header", frame: str) -> list[str]:
    """R-11: non-fatal geometry/orientation caveats for a velocity grid.

    The loader places data[0]→+x, data[1]→+y, data[2]→+z in raw array order and
    assumes a right-handed supergalactic frame. If the header's CDELT signs say
    an axis is flipped, or the frame is only assumed, the rendered streamlines /
    basins / vorticity could be a MIRROR of reality with no other symptom. We
    do not silently reorient (that risks a wrong "fix"); we warn.
    """
    warnings: list[str] = []
    # Spatial axes are CDELT1..3 (the component axis is 4). A negative CDELT
    # means that axis decreases with array index — a handedness flip vs the
    # assumed +x/+y/+z mapping.
    flipped = [i for i in (1, 2, 3)
               if _safe_float(hdr.get(f"CDELT{i}")) is not None
               and _safe_float(hdr.get(f"CDELT{i}")) < 0.0]
    if flipped:
        warnings.append(
            "axes with negative CDELT ("
            + ", ".join(f"CDELT{i}" for i in flipped)
            + ") - the velocity field may be mirror-imaged relative to the "
            "assumed +x/+y/+z orientation; verify handedness."
        )
    # A parity flip can also be encoded in a CD/PC rotation matrix with positive
    # (or absent) CDELT — check the sign of the spatial transform determinant.
    det = _spatial_transform_determinant(hdr)
    if det is not None and det < 0.0:
        warnings.append(
            "the spatial CD/PC transform has a negative determinant - a parity "
            "flip (mirror) that CDELT signs alone don't reveal; verify handedness."
        )
    if "assumed" in frame:
        warnings.append(
            "reference frame is assumed supergalactic (no VFRAME header) - "
            "attractor positions and vorticity sign are only trustworthy if the "
            "reconstruction really is right-handed supergalactic."
        )
    if not any(f"CTYPE{i}" in hdr for i in (1, 2, 3)):
        warnings.append("no spatial WCS (CTYPE) on the grid; axis order is assumed.")
    return warnings


def _safe_float(v: Any) -> float | None:
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _spatial_transform_determinant(hdr: "fits.Header") -> float | None:
    """Determinant of the 3×3 spatial CD (or PC·diag(CDELT)) matrix, or None when
    no such matrix is present. A negative determinant is a parity flip (mirror)
    that per-axis CDELT signs don't reveal (R-11)."""
    def _matrix(prefix: str) -> np.ndarray | None:
        present = any(f"{prefix}{i}_{j}" in hdr for i in (1, 2, 3) for j in (1, 2, 3))
        if not present:
            return None
        m = np.zeros((3, 3), dtype=np.float64)
        for i in (1, 2, 3):
            for j in (1, 2, 3):
                default = 1.0 if i == j else 0.0
                m[i - 1, j - 1] = _safe_float(hdr.get(f"{prefix}{i}_{j}", default)) or 0.0
        return m
    cd = _matrix("CD")
    if cd is not None:
        return float(np.linalg.det(cd))
    pc = _matrix("PC")
    if pc is not None:
        cdelt = np.array([_safe_float(hdr.get(f"CDELT{i}", 1.0)) or 1.0 for i in (1, 2, 3)])
        return float(np.linalg.det(pc * cdelt[None, :]))
    return None


def load_multicomponent_fits(path: str, box_mpc: float | None = None) -> VelocityField:
    """Load a single FITS whose component axis (NAXIS4 == 3) holds vx, vy, vz."""
    with fits.open(path, memmap=False) as hdul:
        hdu = hdul[image_hdu_index(hdul)]
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
    warnings = _geometry_warnings(hdr, frame)
    for w in warnings:
        logger.warning("[velocityfield] %s: %s", path, w)
    logger.info(
        "[velocityfield] loaded %s grid=%dx%dx%d box_known=%s unit=%s warnings=%d",
        path, nx, ny, nz, box_known, unit, len(warnings),
    )
    return VelocityField(vx, vy, vz, spacing, origin, unit, frame, box_known, warnings)


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
    return VelocityField(
        vx, vy, vz, spacing, origin, "km/s (assumed)",
        "supergalactic (assumed)", box_known,
        ["reference frame is assumed supergalactic (three-component load carries "
         "no frame metadata), verify handedness before trusting vorticity sign."],
    )


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


def velocity_subgrid_worker(path: str, x0: int, x1: int, y0: int, y1: int,
                            z0: int, z1: int, box_mpc: float | None = None) -> dict[str, Any]:
    """Return a FULL-resolution sub-region [x0:x1, y0:y1, z0:z1] of the field
    (the ROI-on-demand path). Indices are full-res grid indices, clamped to the
    grid (start in [0, n-1], end in [start+1, n]). The desktop places the
    sub-grid from the requested indices, so no spacing/origin is needed here.

    Uses memmap slicing so only the ROI is read from disk — backend memory/I/O
    stay bounded even for a 512³ source.
    """
    with fits.open(path, memmap=True) as hdul:
        data = hdul[image_hdu_index(hdul)].data  # memmap, shape (3, nz, ny, nx)
        if data is None or data.ndim != 4 or data.shape[0] != 3:
            raise ValueError(
                f"expected a 4-D FITS with a length-3 component axis; got "
                f"{None if data is None else data.shape}"
            )
        _, nz, ny, nx = data.shape
        x0 = max(0, min(int(x0), nx - 1)); x1 = max(x0 + 1, min(int(x1), nx))
        y0 = max(0, min(int(y0), ny - 1)); y1 = max(y0 + 1, min(int(y1), ny))
        z0 = max(0, min(int(z0), nz - 1)); z1 = max(z0 + 1, min(int(z1), nz))
        sub = np.asarray(data[:, z0:z1, y0:y1, x0:x1], dtype=np.float32)  # reads only the ROI

    subfield = VelocityField(
        sub[0].copy(), sub[1].copy(), sub[2].copy(),
        (1.0, 1.0, 1.0), (0.0, 0.0, 0.0), "km/s (assumed)",
        "supergalactic (assumed)", False,
    )
    payload = grid_raw_payload(subfield)
    payload["x0"] = x0; payload["y0"] = y0; payload["z0"] = z0
    payload["downsample_factor"] = 1
    payload["full_shape"] = [nz, ny, nx]
    return payload


def velocity_difference_worker(path_a: str, path_b: str,
                               box_mpc: float | None = None,
                               max_dim: int = 0,
                               box_mpc_b: float | None = None) -> dict[str, Any]:
    """Multi-field compare (R-20): the magnitude of the vector difference of two
    velocity fields, |v_a − v_b|, per cell — e.g. CF3 vs CF4, or a reconstruction
    vs a simulation. Returns a scalar grid (little-endian float32, vtkImageData
    order) + its range, matching the velocity_density_worker payload shape so the
    viewer can render it with the existing machinery. The two fields must share a
    grid shape; ``max_dim`` block-averages both first (LOD). ``box_mpc``/``box_mpc_b``
    are the two fields' physical box sizes (each may be opened with its own).

    Memory: like velocity_density_worker this loads BOTH full fields (3 components
    each) plus float64 difference temporaries before any ``max_dim`` block-average,
    so peak RSS scales with the full grid, not the LOD. For very large (≥256³)
    reconstructions run it out-of-process (it already does, via the pool) and pass
    a bounding ``max_dim``; a streaming/slab path is deferred (see R-20 backlog)."""
    fa = load_velocity_field(path_a, box_mpc)
    fb = load_velocity_field(path_b, box_mpc_b)
    if fa.shape != fb.shape:
        raise ValueError(
            f"velocity fields have different shapes {fa.shape} vs {fb.shape}; "
            "compare requires the same grid.")
    warnings: list[str] = list(fa.warnings) + list(fb.warnings)
    # The cell-wise diff is index-based, so it is only physically meaningful when
    # both fields cover the same physical box — a different box_mpc means cell
    # [i,j,k] sits at a different Mpc position in each field. Warn (don't reject:
    # a header-less field legitimately falls back to box-unknown).
    if (box_mpc or None) != (box_mpc_b or None):
        warnings.append(
            f"the two fields were opened with different box sizes "
            f"({box_mpc} vs {box_mpc_b} Mpc); a cell-wise difference compares "
            "different physical positions.")
    # A cell-wise |v_a − v_b| is only meaningful if both fields are in the same
    # velocity unit and frame — warn loudly when they differ (the diff is then a
    # mix of units/orientations, not a physical velocity difference).
    if fa.unit != fb.unit:
        warnings.append(f"the two fields have different units ({fa.unit!r} vs "
                        f"{fb.unit!r}); the difference mixes units.")
    if fa.frame != fb.frame:
        warnings.append(f"the two fields have different frames ({fa.frame!r} vs "
                        f"{fb.frame!r}); verify they share an orientation.")
    if max_dim and max_dim > 0:
        fa = downsample(fa, max_dim)
        fb = downsample(fb, max_dim)
    dvx = fa.vx.astype(np.float64) - fb.vx.astype(np.float64)
    dvy = fa.vy.astype(np.float64) - fb.vy.astype(np.float64)
    dvz = fa.vz.astype(np.float64) - fb.vz.astype(np.float64)
    diff = np.sqrt(dvx * dvx + dvy * dvy + dvz * dvz)
    out = np.ascontiguousarray(diff, dtype="<f4")
    nz, ny, nx = out.shape
    finite = np.isfinite(out)
    dmin = float(out[finite].min()) if finite.any() else 0.0
    dmax = float(out[finite].max()) if finite.any() else 0.0
    dmean = float(out[finite].mean()) if finite.any() else 0.0
    return {
        "raw_bytes": out.tobytes(),
        "nx": int(nx), "ny": int(ny), "nz": int(nz),
        "spacing": list(fa.spacing),
        "origin": [float(o) for o in fa.origin],
        "diff_min": dmin, "diff_max": dmax, "diff_mean": dmean,
        "unit": fa.unit,
        "warnings": warnings,
    }


def velocity_grid_worker(path: str, box_mpc: float | None = None,
                         max_dim: int = 0) -> dict[str, Any]:
    """Load + return the binary grid payload (for /v1/velocity/grid_bin).

    `max_dim` > 0 block-averages the field down so its largest dimension is
    <= max_dim — the display-LOD path that keeps transfer + client memory bounded
    for large (256³/512³) reconstructions. The result records the applied factor.
    """
    field = load_velocity_field(path, box_mpc)
    full = field.shape
    # Full-resolution speed range (block-averaging lowers the LOD max), so the
    # viewer's LUT/glyph scale still covers values a full-res ROI may contain.
    full_mag = field.magnitude
    full_finite = np.isfinite(full_mag)
    full_speed_min = float(np.nanmin(full_mag)) if full_finite.any() else 0.0
    full_speed_max = float(np.nanmax(full_mag)) if full_finite.any() else 0.0
    if max_dim and max_dim > 0:
        field = downsample(field, max_dim)
    payload = grid_raw_payload(field)
    payload["full_shape"] = [int(s) for s in full]            # (nz, ny, nx) before LOD
    payload["downsample_factor"] = int(round(full[0] / field.shape[0])) if field.shape[0] else 1
    payload["full_speed_min"] = full_speed_min
    payload["full_speed_max"] = full_speed_max
    return payload


def _density_streaming(path: str, box_mpc: float | None) -> dict[str, Any] | None:
    """R-16: full-res divergence by streaming z-slabs from a memmapped 4-D FITS.

    Reads each z-slab of the three components with a ±1-voxel halo so the z
    gradient (axis 0) is a central difference in the interior and one-sided only
    at the global boundaries — byte-for-byte identical to the in-memory
    ``np.gradient`` result. Peak memory is O(slab·ny·nx), not O(nz·ny·nx)·(3+temps).

    Returns None when the file is not a single 4-D (3, nz, ny, nx) cube (the
    three-file loader path falls back to the in-memory worker).
    """
    with fits.open(path, memmap=True, mode="readonly") as hdul:
        hdu = hdul[image_hdu_index(hdul)]
        data = hdu.data
        hdr = hdu.header
        if data is None or data.ndim != 4 or data.shape[0] != 3:
            return None
        _, nz, ny, nx = (int(d) for d in data.shape)
        spacing, origin, box_known = _spacing_origin((nx, ny, nz), box_mpc)
        sx, sy, sz = (float(s) for s in spacing)
        unit = str(hdr.get("BUNIT", "") or "").strip() or "km/s (assumed)"
        out = np.empty((nz, ny, nx), dtype="<f4")
        dmin, dmax = np.inf, -np.inf
        for z0 in range(0, nz, _VELOCITY_SLAB):
            z1 = min(z0 + _VELOCITY_SLAB, nz)
            zlo, zhi = max(0, z0 - 1), min(nz, z1 + 1)
            vx = np.asarray(data[0, zlo:zhi], dtype=np.float64)
            vy = np.asarray(data[1, zlo:zhi], dtype=np.float64)
            vz = np.asarray(data[2, zlo:zhi], dtype=np.float64)
            div = (np.gradient(vx, sx, axis=2)
                   + np.gradient(vy, sy, axis=1)
                   + np.gradient(vz, sz, axis=0))
            interior = div[z0 - zlo: z0 - zlo + (z1 - z0)]
            out[z0:z1] = interior.astype("<f4")
            finite = np.isfinite(interior)
            if finite.any():
                dmin = min(dmin, float(interior[finite].min()))
                dmax = max(dmax, float(interior[finite].max()))
    if not np.isfinite(dmin):
        dmin, dmax = 0.0, 0.0
    per = "Mpc" if box_known else "pixel"
    return {
        "raw_bytes": np.ascontiguousarray(out).tobytes(),
        "nx": nx, "ny": ny, "nz": nz,
        "spacing": [sx, sy, sz],
        "origin": [float(o) for o in origin],
        "div_min": float(dmin), "div_max": float(dmax),
        "unit": f"{unit}/{per}",
        "calibrated": False,
        "density_note": ("raw velocity divergence ∇·v; linear-theory overdensity "
                         "δ ≈ -∇·v · linear_density_factor (Planck18 f·H0)"),
        "linear_density_factor": _linear_density_factor(box_known),
    }


def velocity_density_worker(path: str, box_mpc: float | None = None,
                            max_dim: int = 0) -> dict[str, Any]:
    """Density proxy from the velocity divergence (δ ∝ −∇·v, linear theory).

    Returns the ∇·v scalar grid as little-endian float32 in vtkImageData order
    (x fastest). ``max_dim`` > 0 block-averages first (LOD); 0 = full resolution —
    the point of this endpoint is a *full-res* density for large (256³/512³)
    fields, where the display LOD is too coarse to resolve the convergence
    (overdensity) / divergence (void) cores. Convergent flow (∇·v < 0) marks
    overdensities; divergent flow (∇·v > 0) marks voids / repellers.

    R-16: at full resolution (max_dim == 0) a large single-file field is computed
    out-of-core by streaming z-slabs (see ``_density_streaming``); small fields
    and the LOD/three-file paths use the in-memory computation.
    """
    if not (max_dim and max_dim > 0):
        try:
            field_bytes = 3 * os.path.getsize(path)
        except OSError:
            field_bytes = 0
        if field_bytes > _VELOCITY_STREAM_BYTES:
            streamed = _density_streaming(path, box_mpc)
            if streamed is not None:
                return streamed

    field = load_velocity_field(path, box_mpc)
    if max_dim and max_dim > 0:
        field = downsample(field, max_dim)
    sx, sy, sz = (float(s) for s in field.spacing)
    # Component arrays are indexed [z, y, x]; ∂/∂x is axis 2, ∂/∂y axis 1, ∂/∂z 0.
    # Compute in float64 (components are float32) so this matches the streaming
    # path bit-for-bit — otherwise the same endpoint would return slightly
    # different bits depending only on whether the field crossed the stream
    # threshold (a reproducibility hazard).
    div = (np.gradient(field.vx.astype(np.float64), sx, axis=2)
           + np.gradient(field.vy.astype(np.float64), sy, axis=1)
           + np.gradient(field.vz.astype(np.float64), sz, axis=0))
    # C-contiguous [z, y, x] little-endian float32 == vtkImageData scalar order.
    div = np.ascontiguousarray(div, dtype="<f4")
    nz, ny, nx = div.shape
    finite = np.isfinite(div)
    dmin = float(div[finite].min()) if finite.any() else 0.0
    dmax = float(div[finite].max()) if finite.any() else 0.0
    # ∇·v is velocity per distance — the density-proxy unit, not the field's.
    per = "Mpc" if field.box_known else "pixel"
    return {
        "raw_bytes": div.tobytes(),
        "nx": int(nx), "ny": int(ny), "nz": int(nz),
        "spacing": [sx, sy, sz],
        "origin": [float(o) for o in field.origin],
        "div_min": dmin,
        "div_max": dmax,
        "unit": f"{field.unit}/{per}",
        "calibrated": False,
        "density_note": ("raw velocity divergence ∇·v; linear-theory overdensity "
                         "δ ≈ -∇·v · linear_density_factor (Planck18 f·H0)"),
        "linear_density_factor": _linear_density_factor(field.box_known),
    }


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
