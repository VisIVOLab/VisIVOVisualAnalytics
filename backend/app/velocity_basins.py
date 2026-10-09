"""Gravitational-basin (watershed) segmentation of a velocity field — F1 phase-2.

The cosmic-flow definition of a supercluster (e.g. Laniakea; Dupuy et al. 2019,
Courtois et al. 2013) is a *basin of attraction*: the set of points whose
velocity streamlines converge to the same attractor (a critical point where
v = 0). We compute it by integrating every grid voxel's streamline through the
field until it converges, then labelling voxels by the attractor they reach.

The integration is vectorised over all voxels (scipy.ndimage.map_coordinates for
trilinear sampling), so it is tractable on a coarse grid — basins are computed on
a downsampled field (basin_max_dim, default 64), which is also what the original
analyses do (smoothed fields).

`direction = +1` gives basins of attraction (superclusters); `-1` gives basins of
repulsion (voids / repellers).
"""
from __future__ import annotations

import logging
from typing import Any

import numpy as np
from scipy.ndimage import label, map_coordinates

from .velocity_field import VelocityField, downsample, load_velocity_field

logger = logging.getLogger("visivo.velocitybasins")


def compute_basins(field: VelocityField, direction: int = 1, n_steps: int = 500,
                   step: float = 0.5, eps_frac: float = 1e-3,
                   attractor_min_voxels: int = 8) -> tuple[np.ndarray, int]:
    """Segment the field into basins. Returns (labels (nz,ny,nx) int32, n_basins).

    Label 0 = unassigned (streamline never converged / outside any attractor);
    labels 1..n_basins identify the basins.
    """
    nz, ny, nx = field.shape
    vx, vy, vz = field.vx, field.vy, field.vz
    sign = 1.0 if direction >= 0 else -1.0

    # Speed scale → a convergence threshold relative to the typical speed.
    mag = np.sqrt(vx * vx + vy * vy + vz * vz)
    eps = max(float(np.nanmedian(mag)) * eps_frac, 1e-20)

    # All voxel centres in index space (x, y, z).
    zz, yy, xx = np.mgrid[0:nz, 0:ny, 0:nx]
    pos = np.stack([xx.ravel().astype(np.float64),
                    yy.ravel().astype(np.float64),
                    zz.ravel().astype(np.float64)], axis=1)  # (N, 3)

    def sample(p: np.ndarray) -> np.ndarray:
        coords = np.vstack([p[:, 2], p[:, 1], p[:, 0]])  # (z, y, x) for map_coordinates
        sx = map_coordinates(vx, coords, order=1, mode="nearest")
        sy = map_coordinates(vy, coords, order=1, mode="nearest")
        sz = map_coordinates(vz, coords, order=1, mode="nearest")
        return np.stack([sx, sy, sz], axis=1)

    def cell_of(p: np.ndarray) -> np.ndarray:
        cx = np.clip(np.round(p[:, 0]).astype(np.int64), 0, nx - 1)
        cy = np.clip(np.round(p[:, 1]).astype(np.int64), 0, ny - 1)
        cz = np.clip(np.round(p[:, 2]).astype(np.int64), 0, nz - 1)
        return (cz * ny + cy) * nx + cx

    converged = np.zeros(pos.shape[0], dtype=bool)
    stagnant = np.zeros(pos.shape[0], dtype=np.int32)
    prev_cell = cell_of(pos)
    prev_dir = np.zeros_like(pos)
    have_prev = False
    for _ in range(n_steps):
        v = sample(pos) * sign
        speed = np.linalg.norm(v, axis=1)
        unit = np.zeros_like(v)
        nz_mask = speed > 0
        unit[nz_mask] = v[nz_mask] / speed[nz_mask, None]

        converged |= speed <= eps              # at a critical point (|v|→0)
        if have_prev:
            # Direction reversal = the path stepped past a critical point (a sink
            # it overshoots/orbits on a coarse grid). Robust where |v| never dips
            # below `step` near the sink.
            reversed_dir = np.einsum("ij,ij->i", unit, prev_dir) < -0.5
            converged |= reversed_dir
        active = ~converged
        if not active.any():
            break
        # Adaptive step: move by min(step, |v|) along the flow direction — caps the
        # jump far from a sink, shrinks near it so the path settles onto the
        # critical point instead of overshooting it.
        scale = np.zeros_like(speed)
        scale[active] = np.minimum(step, speed[active]) / speed[active]
        pos[active] += v[active] * scale[active, None]
        np.clip(pos[:, 0], 0, nx - 1, out=pos[:, 0])
        np.clip(pos[:, 1], 0, ny - 1, out=pos[:, 1])
        np.clip(pos[:, 2], 0, nz - 1, out=pos[:, 2])
        prev_dir = unit
        have_prev = True
        # Stagnation: a path stuck in one cell also counts as converged — but NOT
        # on a domain-boundary cell, where an outward/clipped path is pinned to the
        # wall rather than at a real attractor (that would make a false boundary
        # basin).
        cx = np.clip(np.round(pos[:, 0]).astype(np.int64), 0, nx - 1)
        cy = np.clip(np.round(pos[:, 1]).astype(np.int64), 0, ny - 1)
        cz = np.clip(np.round(pos[:, 2]).astype(np.int64), 0, nz - 1)
        cell = (cz * ny + cy) * nx + cx
        on_boundary = ((cx == 0) | (cx == nx - 1) | (cy == 0) | (cy == ny - 1)
                       | (cz == 0) | (cz == nz - 1))
        stagnant = np.where(cell == prev_cell, stagnant + 1, 0)
        converged |= (stagnant >= 5) & ~on_boundary
        prev_cell = cell

    # Only CONVERGED streamlines define attractors / get a basin. Limit cycles
    # (rotational fields), boundary-clipped and n_steps-truncated paths stay 0.
    finite = np.isfinite(pos).all(axis=1)
    valid = converged & finite
    sink_flat = cell_of(pos)

    density = np.bincount(sink_flat[valid], minlength=nx * ny * nz).reshape(nz, ny, nx)
    attractor_mask = density >= attractor_min_voxels
    # 26-connectivity: quantised attractor cells touching only diagonally are one
    # attractor.
    attr_labels, n_basins = label(attractor_mask, structure=np.ones((3, 3, 3), dtype=int))

    voxel_labels = np.zeros(pos.shape[0], dtype=np.int32)
    voxel_labels[valid] = attr_labels.ravel()[sink_flat[valid]]
    logger.info("[basins] grid=%dx%dx%d dir=%+d basins=%d assigned=%.1f%% converged=%.1f%%",
                nx, ny, nz, direction, n_basins,
                100.0 * float((voxel_labels > 0).mean()),
                100.0 * float(valid.mean()))
    return voxel_labels.reshape(nz, ny, nx), int(n_basins)


def basins_label_payload(labels: np.ndarray, n_basins: int) -> dict[str, Any]:
    """Serialise the basin labels as little-endian int32 (x-fastest, vtkImageData
    point order)."""
    nz, ny, nx = labels.shape
    raw = np.ascontiguousarray(labels, dtype="<i4").tobytes()
    return {"raw_bytes": raw, "nx": int(nx), "ny": int(ny), "nz": int(nz),
            "n_basins": int(n_basins)}


# Basin integration is O(N_voxels × n_steps); always run on a coarse grid so a
# direct API caller can't force a full-resolution sweep.
BASIN_MAX_DIM_CAP = 96


def velocity_basins_worker(path: str, box_mpc: float | None = None, basin_max_dim: int = 64,
                           direction: int = 1) -> dict[str, Any]:
    """Load → downsample to a capped basin grid → segment → serialise (picklable)."""
    capped = 64 if basin_max_dim is None or basin_max_dim <= 0 else int(basin_max_dim)
    capped = max(8, min(capped, BASIN_MAX_DIM_CAP))
    direction = 1 if direction >= 0 else -1
    field = downsample(load_velocity_field(path, box_mpc), capped)
    labels, n_basins = compute_basins(field, direction=direction)
    payload = basins_label_payload(labels, n_basins)
    payload["spacing"] = list(field.spacing)
    payload["origin"] = list(field.origin)
    return payload
