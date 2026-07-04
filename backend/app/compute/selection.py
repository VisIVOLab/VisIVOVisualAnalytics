"""Kinematic Lasso — interactive, kinematics-aware 3-D object selection in a
position–position–velocity (PPV) spectral cube.

The user clicks a source they can SEE in the volume render; the front end
ray-casts that click to a seed voxel; this module grows a **geodesic** region
from the seed whose traversal cost follows the emission (bright voxels are cheap
to cross, faint/NaN voxels are expensive/barriers) so the selection follows the
object's real morphology instead of a hard threshold or a crude box.

The novelty vs a plain intensity flood-fill is the **anisotropic velocity
weight** ``lambda_v``: moving one step along the spectral (velocity) axis costs
``lambda_v`` × the base cost, so the selection can be tuned to follow the
kinematically-coherent structure (a rotating clump / outflow, connected along
velocity) — ``lambda_v < 1`` grows readily along velocity, ``lambda_v > 1`` stays
spatially compact. Growth is bounded to a ``reach`` cost budget and a spatial
ROI so it stays fast and out-of-core on large cubes.

Returns a boolean mask the size of the cube; the caller renders it as an
isosurface preview and/or applies it (isolate / remove) via the mask endpoint.
"""
from __future__ import annotations

from typing import Any

import numpy as np


def cube_kinematic_select(
    data: np.ndarray,
    seed_zyx: tuple[int, int, int],
    reach: float = 8.0,
    lambda_v: float = 1.0,
    sigma: float | None = None,
    roi_radius: int = 48,
    nsigma: float = 2.5,
) -> np.ndarray:
    """Geodesic, kinematics-aware region grow from ``seed_zyx`` (z=velocity).

    @param data       3-D cube (nz, ny, nx); NaN voxels act as barriers.
    @param seed_zyx   seed voxel (z, y, x), 0-indexed.
    @param reach      geodesic cost budget — larger selects further into the
                      faint outskirts of the object.
    @param lambda_v   velocity-axis step weight (the kinematic knob); 1 = isotropic.
    @param sigma      intensity scale for the cost; None → robust noise (MAD).
    @param roi_radius half-size (voxels) of the ROI around the seed the grow is
                      confined to (bounds work on huge cubes).
    @returns boolean mask of shape ``data.shape`` (True = selected).
    """
    from scipy.sparse import csr_matrix          # noqa: PLC0415
    from scipy.sparse.csgraph import dijkstra     # noqa: PLC0415

    if data.ndim != 3:
        raise ValueError("Kinematic selection requires a 3-D cube.")
    nz, ny, nx = data.shape
    sz, sy, sx = int(seed_zyx[0]), int(seed_zyx[1]), int(seed_zyx[2])
    if not (0 <= sz < nz and 0 <= sy < ny and 0 <= sx < nx):
        raise ValueError(f"Seed {(sz, sy, sx)} is outside the cube {data.shape}.")
    if not np.isfinite(data[sz, sy, sx]):
        raise ValueError("Seed voxel is blank/NaN — click on visible emission.")
    r = max(1, int(roi_radius))

    z0, z1 = max(0, sz - r), min(nz, sz + r + 1)
    y0, y1 = max(0, sy - r), min(ny, sy + r + 1)
    x0, x1 = max(0, sx - r), min(nx, sx + r + 1)
    roi = np.asarray(data[z0:z1, y0:y1, x0:x1], dtype=np.float64)
    dz, dy, dx = roi.shape

    # Intensity scale: robust noise (1.4826·MAD) unless the caller pins it.
    if sigma is None or not np.isfinite(sigma) or sigma <= 0.0:
        finite = roi[np.isfinite(roi)]
        if finite.size:
            med = np.median(finite)
            mad = np.median(np.abs(finite - med))
            sigma = max(1.4826 * float(mad), 1e-6)
        else:
            sigma = 1.0

    # Per-voxel traversal cost with a SOFT threshold at ``nsigma``·σ: a voxel at
    # nσ costs 1; brighter voxels are ~free (the grow flows through the object),
    # fainter voxels cost exp((nσ − I)/σ) which blows up below the noise floor so
    # it acts as a barrier — the grow stops at the object's ~nσ contour and the
    # ``reach`` budget controls how far into the faint outskirts it extends. NaN
    # (unobserved) → hard barrier.
    with np.errstate(over="ignore"):
        cost = np.exp(float(nsigma) - np.clip(roi, 0.0, None) / float(sigma))
    cost = np.where(np.isfinite(roi), np.minimum(cost, 1.0e6), 1.0e6).astype(np.float64)
    cost_flat = cost.ravel()

    n = dz * dy * dx
    ids = np.arange(n).reshape(dz, dy, dx)

    rows: list[np.ndarray] = []
    cols: list[np.ndarray] = []
    wts: list[np.ndarray] = []

    def _link(a: np.ndarray, b: np.ndarray, factor: float) -> None:
        # Directed edges both ways; entering a voxel costs that voxel's cost, so
        # the grow flows into bright neighbours cheaply. Velocity steps ×factor.
        a = a.ravel()
        b = b.ravel()
        rows.append(a); cols.append(b); wts.append(cost_flat[b] * factor)
        rows.append(b); cols.append(a); wts.append(cost_flat[a] * factor)

    if dz > 1:
        _link(ids[:-1, :, :], ids[1:, :, :], float(lambda_v))   # z = velocity axis
    if dy > 1:
        _link(ids[:, :-1, :], ids[:, 1:, :], 1.0)
    if dx > 1:
        _link(ids[:, :, :-1], ids[:, :, 1:], 1.0)

    seed_flat = int(ids[sz - z0, sy - y0, sx - x0])
    if not rows:                       # degenerate 1-voxel ROI → seed only
        mask = np.zeros((nz, ny, nx), dtype=bool)
        mask[sz, sy, sx] = True
        return mask

    graph = csr_matrix(
        (np.concatenate(wts), (np.concatenate(rows), np.concatenate(cols))),
        shape=(n, n),
    )
    # limit prunes nodes past the budget → bounded work; unreachable → inf.
    dist = dijkstra(graph, directed=True, indices=seed_flat, limit=float(reach))
    sel_roi = np.asarray(dist <= float(reach)).reshape(dz, dy, dx)

    mask = np.zeros((nz, ny, nx), dtype=bool)
    mask[z0:z1, y0:y1, x0:x1] = sel_roi
    return mask


def worker_cube_kinematic_select(
    path: str,
    seed_z: int, seed_y: int, seed_x: int,
    reach: float,
    lambda_v: float,
    sigma: float,
    roi_radius: int,
    nsigma: float = 2.5,
) -> dict[str, Any]:
    """Top-level worker (picklable). Loads the cube, runs the geodesic selection,
    and serialises the mask as little-endian float32 (0/1, x-fastest = vtkImageData
    point order) so the front end can contour it as an isosurface preview.

    Returns the mask bytes + dims + selected-voxel count + the selection's
    bounding box (for the front end to frame / apply)."""
    from ..fits_dataset import ScientificFitsDataset  # noqa: PLC0415

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = ScientificFitsDataset.squeeze_to_3d(raw)   # memmap-backed view
        if data.ndim != 3:
            raise ValueError("Kinematic selection requires a 3-D cube dataset.")
        # Runs while the memmap is open; cube_kinematic_select reads ONLY the ROI
        # around the seed (out-of-core on huge cubes). The returned mask is a fresh
        # array, safe to use after the file closes.
        mask = cube_kinematic_select(
            data, (seed_z, seed_y, seed_x),
            reach=float(reach), lambda_v=float(lambda_v),
            sigma=float(sigma) if sigma and sigma > 0 else None,
            roi_radius=int(roi_radius), nsigma=float(nsigma),
        )
    finally:
        hdul.close()
    nz, ny, nx = mask.shape
    count = int(mask.sum())
    if count > 0:
        zz, yy, xx = np.where(mask)
        x0, x1 = int(xx.min()), int(xx.max())
        y0, y1 = int(yy.min()), int(yy.max())
        z0, z1 = int(zz.min()), int(zz.max())
    else:
        x0 = x1 = y0 = y1 = z0 = z1 = 0

    # Serialise ONLY the selection's bounding box (+ its origin in cube voxels),
    # not the full cube — the payload/RAM stay bounded by the object, not the
    # cube. The front end places this ROI mask at the origin for the isosurface.
    sub = np.ascontiguousarray(mask[z0:z1 + 1, y0:y1 + 1, x0:x1 + 1].astype("<f4"))
    return {
        "raw_bytes": sub.tobytes(),
        "roi_width": int(sub.shape[2]), "roi_height": int(sub.shape[1]),
        "roi_depth": int(sub.shape[0]),
        "origin_x": x0, "origin_y": y0, "origin_z": z0,
        "full_width": int(nx), "full_height": int(ny), "full_depth": int(nz),
        "count": count, "bbox": [x0, x1, y0, y1, z0, z1],
    }
