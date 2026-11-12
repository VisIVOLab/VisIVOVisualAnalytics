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

from .velocity_field import VelocityField, downsample, load_velocity_field, lod_factors

logger = logging.getLogger("visivo.velocitybasins")


def compute_basins(field: VelocityField, direction: int = 1, n_steps: int = 500,
                   step: float = 0.5, eps_frac: float = 1e-3,
                   attractor_min_voxels: int = 8) -> tuple[np.ndarray, int]:
    """Segment the field into basins. Returns (labels (nz,ny,nx) int32, n_basins).

    Label 0 = unassigned (streamline never ended at a sink); labels 1..n_basins
    identify the basins.

    A basin is defined by the GEOMETRY of the streamlines, which depends only on
    the direction of the flow, not on its speed: multiplying every velocity by a
    constant must give the same basins. So the path is integrated by arc length,
    a fixed ``step`` (in cells) along the unit flow direction. A path ENDS when

    * ``|v| <= eps``, with ``eps`` relative to the field's median speed;
    * its direction reverses between consecutive steps (it stepped over a
      critical point; usually how a fixed step ends at a sink, though the
      speed and stuck-cell rules also end paths there);
    * it stays in one interior cell for 5 steps (with a fixed step, only while
      it circles within a cell of a critical point).

    Those rules end a path at ANY critical point, a saddle included (a path on
    a saddle's stable manifold, e.g. the separatrix between two wells, runs into
    it). So each candidate attractor is then tested as a sink: probes started
    ``probe_offset`` cells away from it along ±x, ±y, ±z are traced for a few
    more cells, and the attractor is kept only if every probe ends back within
    ``probe_radius`` cells. A saddle sheds the probes along its unstable
    direction however weak that direction is (the step follows the unit
    direction), so it is rejected and the paths that ended there stay
    unassigned (label 0); they are NOT handed to a neighbouring basin.

    NaN voxels (a masked or blank region) are excluded from the interpolation;
    a sample whose valid interpolation weight is below 0.5 is missing data and
    the path that took it is dead: not converged, no part in the attractor
    count, unassigned. Without that, a path stalled on the rim of a NaN hole
    looked "stuck in one cell" and a pile of them made a false basin.

    Velocities are converted to cells per unit time per axis (``v_i / s_i``)
    with the field's spacing, so the direction is the physical one. On a
    non-cubic grid the spacing depends on whether a box size is known (box /
    (n_i - 1) per axis, otherwise 1 cell), so the box size changes the
    segmentation there; on a cubic grid it does not.

    The step used to be ``min(step, |v|)`` with ``|v|`` in km/s compared with a
    length in cells: a slow but perfectly regular flow (the same field x 0.001)
    crawled 0.001 cell per step, the stagnation rule fired everywhere, and a
    one-attractor field came out as 8 basins covering 64 of 4913 voxels.
    """
    nz, ny, nx = field.shape
    sign = 1.0 if direction >= 0 else -1.0
    sx, sy, sz = (float(s) if s and np.isfinite(s) and s > 0 else 1.0
                  for s in field.spacing)
    # Index-space (cells per unit time) components: the physical direction on an
    # anisotropic grid. A common factor (isotropic spacing) cancels out anyway.
    vx = np.asarray(field.vx, dtype=np.float64) / sx
    vy = np.asarray(field.vy, dtype=np.float64) / sy
    vz = np.asarray(field.vz, dtype=np.float64) / sz

    # Speed scale → a convergence threshold relative to the typical speed.
    mag = np.sqrt(vx * vx + vy * vy + vz * vz)
    med = float(np.nanmedian(mag)) if np.isfinite(mag).any() else 0.0
    eps = med * eps_frac if med > 0 else 0.0

    # NaN-aware trilinear sampling. map_coordinates(order=1) returns NaN as soon
    # as a NaN voxel is among the 8 corners, even with zero weight (a NaN at
    # x = 3 makes the sample at x = 2.0 NaN), so a probe next to a small blank
    # patch "died" and a real sink was rejected. Interpolate the field with NaN
    # set to 0 and the validity mask the same way; a sample is valid when at
    # least half of its weight is on valid voxels, and the velocity is
    # renormalised by that weight.
    valid_vox = np.isfinite(vx) & np.isfinite(vy) & np.isfinite(vz)
    wmask = valid_vox.astype(np.float64)
    vx0 = np.where(valid_vox, vx, 0.0)
    vy0 = np.where(valid_vox, vy, 0.0)
    vz0 = np.where(valid_vox, vz, 0.0)
    has_nan = not bool(valid_vox.all())

    def sample(p: np.ndarray) -> np.ndarray:
        coords = np.vstack([p[:, 2], p[:, 1], p[:, 0]])  # (z, y, x) for map_coordinates
        ux = map_coordinates(vx0, coords, order=1, mode="nearest")
        uy = map_coordinates(vy0, coords, order=1, mode="nearest")
        uz = map_coordinates(vz0, coords, order=1, mode="nearest")
        u = np.stack([ux, uy, uz], axis=1)
        if has_nan:
            w = map_coordinates(wmask, coords, order=1, mode="nearest")
            ok = w >= 0.5
            u[ok] /= w[ok, None]
            u[~ok] = np.nan                    # mostly missing data: the path dies
        return u

    def cell_of(p: np.ndarray) -> np.ndarray:
        cx = np.clip(np.round(p[:, 0]).astype(np.int64), 0, nx - 1)
        cy = np.clip(np.round(p[:, 1]).astype(np.int64), 0, ny - 1)
        cz = np.clip(np.round(p[:, 2]).astype(np.int64), 0, nz - 1)
        return (cz * ny + cy) * nx + cx

    def trace(pos: np.ndarray, steps: int) -> tuple[np.ndarray, np.ndarray]:
        """Integrate ``pos`` (N, 3) in place; returns (converged, dead)."""
        converged = np.zeros(pos.shape[0], dtype=bool)
        dead = np.zeros(pos.shape[0], dtype=bool)
        stagnant = np.zeros(pos.shape[0], dtype=np.int32)
        prev_cell = cell_of(pos)
        prev_dir = np.zeros_like(pos)
        have_prev = False
        for _ in range(steps):
            v = sample(pos) * sign
            speed = np.linalg.norm(v, axis=1)
            # A NaN sample kills the path: it is neither moving nor at a sink.
            dead |= ~np.isfinite(speed)
            live = ~dead
            unit = np.zeros_like(v)
            moving = live & (speed > 0)
            unit[moving] = v[moving] / speed[moving, None]

            converged |= live & (speed <= eps)          # at a critical point
            if have_prev:
                # Direction reversal = the path stepped over a critical point.
                converged |= live & (np.einsum("ij,ij->i", unit, prev_dir) < -0.5)
            active = live & ~converged
            if not active.any():
                break
            # Fixed-length step along the flow DIRECTION (arc-length streamline),
            # so the path, and hence the basins, do not depend on the speed.
            pos[active] += unit[active] * step
            np.clip(pos[:, 0], 0, nx - 1, out=pos[:, 0])
            np.clip(pos[:, 1], 0, ny - 1, out=pos[:, 1])
            np.clip(pos[:, 2], 0, nz - 1, out=pos[:, 2])
            prev_dir = unit
            have_prev = True
            # Stuck in one interior cell. Not on a domain-boundary cell, where an
            # outward/clipped path is pinned to the wall, not at an attractor.
            cx = np.clip(np.round(pos[:, 0]).astype(np.int64), 0, nx - 1)
            cy = np.clip(np.round(pos[:, 1]).astype(np.int64), 0, ny - 1)
            cz = np.clip(np.round(pos[:, 2]).astype(np.int64), 0, nz - 1)
            cell = (cz * ny + cy) * nx + cx
            on_boundary = ((cx == 0) | (cx == nx - 1) | (cy == 0) | (cy == ny - 1)
                           | (cz == 0) | (cz == nz - 1))
            stagnant = np.where(cell == prev_cell, stagnant + 1, 0)
            converged |= live & (stagnant >= 5) & ~on_boundary
            prev_cell = cell
        return converged, dead

    # All voxel centres in index space (x, y, z).
    zz, yy, xx = np.mgrid[0:nz, 0:ny, 0:nx]
    pos = np.stack([xx.ravel().astype(np.float64),
                    yy.ravel().astype(np.float64),
                    zz.ravel().astype(np.float64)], axis=1)  # (N, 3)
    converged, dead = trace(pos, n_steps)

    # Only paths that ended at a critical point define attractors. Limit cycles
    # (rotational fields), boundary-clipped, NaN-killed and truncated paths stay 0.
    valid = converged & ~dead & np.isfinite(pos).all(axis=1)
    sink_flat = cell_of(pos)

    density = np.bincount(sink_flat[valid], minlength=nx * ny * nz).reshape(nz, ny, nx)
    attractor_mask = density >= attractor_min_voxels
    # 26-connectivity: quantised attractor cells touching only diagonally are one
    # attractor.
    attr_labels, n_found = label(attractor_mask, structure=np.ones((3, 3, 3), dtype=int))

    # Keep only the attractors that are sinks; renumber them 1..n_basins.
    keep = np.zeros(n_found + 1, dtype=np.int32)
    n_basins = 0
    if n_found:
        comp = attr_labels.ravel()[sink_flat[valid]]
        ends = pos[valid]
        cells_zyx = np.argwhere(attractor_mask)              # (M, 3) z, y, x
        cell_comp = attr_labels[tuple(cells_zyx.T)]
        cells_xyz = cells_zyx[:, ::-1].astype(np.float64)
        for k in range(1, n_found + 1):
            centre = ends[comp == k].mean(axis=0)
            if _is_sink(centre, cells_xyz[cell_comp == k], trace, (nx, ny, nz),
                        step=step):
                n_basins += 1
                keep[k] = n_basins

    voxel_labels = np.zeros(pos.shape[0], dtype=np.int32)
    voxel_labels[valid] = keep[attr_labels.ravel()[sink_flat[valid]]]
    logger.info("[basins] grid=%dx%dx%d dir=%+d basins=%d (of %d critical points) "
                "assigned=%.1f%% converged=%.1f%% nan-dead=%.1f%%",
                nx, ny, nz, direction, n_basins, n_found,
                100.0 * float((voxel_labels > 0).mean()),
                100.0 * float(valid.mean()), 100.0 * float(dead.mean()))
    return voxel_labels.reshape(nz, ny, nx), int(n_basins)


def _is_sink(centre: np.ndarray, cells_xyz: np.ndarray, trace,
             shape_xyz: tuple[int, int, int], probe_offset: float = 1.0,
             probe_radius: float = 2.0, probe_cells: float = 8.0,
             step: float = 0.5) -> bool:
    """True when every probe started ``probe_offset`` cells from ``centre`` along
    ±x, ±y, ±z comes back: it ends within ``probe_radius`` cells of ``centre``,
    or in (or next to) one of the attractor's own cells ``cells_xyz`` (M, 3).

    A probe on the unstable direction of a saddle runs away (to another well or
    to the boundary), at any speed ratio, because the step follows the unit
    direction. The attractor's own cells are accepted as well as the radius so
    that a sink smeared by noise into a cluster of cells, whose mean is not
    where the probes settle, is not rejected. An axis the grid does not have
    (size 1) is not probed. A probe that dies on missing data farther from the
    centre than it started has escaped (the attractor is rejected); one that
    dies at or inside its start distance is neutral. If an axis then has no
    live probe left, the attractor is kept unjudged.
    """
    nx, ny, nz = shape_xyz
    probes, probe_axis = [], []
    for axis, n in enumerate((nx, ny, nz)):
        if n < 2:
            continue
        for s in (-1.0, 1.0):
            p = centre.copy()
            p[axis] = min(max(p[axis] + s * probe_offset, 0.0), n - 1.0)
            probes.append(p)
            probe_axis.append(axis)
    if not probes:
        return True
    pts = np.array(probes, dtype=np.float64)
    axes = np.array(probe_axis)
    _, dead = trace(pts, int(np.ceil(probe_cells / step)))
    live = ~dead
    dist = np.linalg.norm(pts - centre[None, :], axis=1)
    # A probe that died on missing data FARTHER out than it started was moving
    # away: it escaped (a saddle's unstable probes running into a blank patch),
    # so the candidate is not a sink. One that died at or inside its start
    # distance was heading back and says nothing more: neutral.
    if np.any(dead & (dist > probe_offset + 1e-9)):
        return False
    # Judge only when every probed axis kept at least one live probe; otherwise
    # the data cannot tell a sink from a saddle and the attractor is kept (as
    # the end rules found it).
    if not all(live[axes == a].any() for a in np.unique(axes)):
        return True
    near_centre = dist <= probe_radius
    # Chebyshev distance (in cells) from each probe end to the nearest own cell.
    to_cells = np.abs(np.round(pts)[:, None, :] - cells_xyz[None, :, :]).max(axis=2)
    in_attractor = to_cells.min(axis=1) <= 1.0 if cells_xyz.size else near_centre
    returned = near_centre | in_attractor
    return bool(np.all(returned[live]))


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
    full = load_velocity_field(path, box_mpc)
    fz, fy, fx = lod_factors(full.shape, capped)
    field = downsample(full, capped)
    labels, n_basins = compute_basins(field, direction=direction)
    payload = basins_label_payload(labels, n_basins)
    # Geometry of the basin grid, x,y,z: spacing grows by each axis's factor and
    # the origin is the centre of the first block (see velocity_field.downsample).
    payload["spacing"] = [float(s) for s in field.spacing]
    payload["origin"] = [float(o) for o in field.origin]
    payload["downsample_factors"] = [int(fx), int(fy), int(fz)]
    return payload
