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

import math
from pathlib import Path
from typing import Any

import numpy as np

from ._common import _finite_range, ScientificFitsDataset


# Safety ceiling on the geodesic graph: at most this many voxels become nodes, so
# a pathological all-bright cube can't build a multi-GB graph. The real bound is the
# connected object itself, which is far smaller; this only guards the worst case.
_MAX_SELECT_NODES = 4_000_000

# Object-bounded default: with roi_radius=0 the grow is confined to a seed-centred
# window of this half-size, big enough to swallow a large galaxy disk yet capping the
# connected-component / graph memory on datasets far larger than the object.
_SAFE_RADIUS = 256


def _robust_sigma(data: np.ndarray) -> float:
    """Robust noise scale (1.4826·MAD) from a strided sample — never materialises
    the whole cube, so it stays cheap out-of-core on huge memmapped datasets."""
    n = data.size
    step = 1
    if n > 3_000_000:
        step = max(1, int(round((n / 3_000_000) ** (1.0 / 3.0))))
    sample = np.asarray(data[::step, ::step, ::step], dtype=np.float64)
    finite = sample[np.isfinite(sample)]
    if finite.size == 0:
        return 1.0
    med = np.median(finite)
    mad = np.median(np.abs(finite - med))
    s = 1.4826 * float(mad)
    return s if s > 1e-6 else max(float(np.std(finite)), 1e-6)


def cube_kinematic_select(
    data: np.ndarray,
    seed_zyx: tuple[int, int, int],
    reach: float = 8.0,
    lambda_v: float = 1.0,
    sigma: float | None = None,
    roi_radius: int = 0,
    nsigma: float = 2.5,
) -> np.ndarray:
    """Geodesic, kinematics-aware region grow from ``seed_zyx`` (z=velocity).

    The grow is bounded by the OBJECT, not a fixed box: we take the connected
    component of the ~nσ emission that contains the seed (so the whole galaxy /
    clump is grabbed from one click, however large), then run the geodesic only
    over that component's voxels — memory scales with the object, not with a ROI
    cube, so it stays out-of-core on big datasets. Isolated noise specks above nσ
    are their own tiny components and are dropped.

    @param data       3-D cube (nz, ny, nx); NaN voxels act as barriers.
    @param seed_zyx   seed voxel (z, y, x), 0-indexed.
    @param reach      geodesic cost budget — larger selects further along the
                      object (through its fainter, near-nσ extent).
    @param lambda_v   velocity-axis step weight (the kinematic knob); 1 = isotropic.
    @param sigma      intensity scale for the cost; None → robust noise (MAD).
    @param roi_radius optional hard half-box (voxels) around the seed the grow is
                      confined to; ``<= 0`` = object-bounded (no box).
    @returns boolean mask of shape ``data.shape`` (True = selected).
    """
    from scipy import ndimage                     # noqa: PLC0415
    from scipy.sparse import csr_matrix           # noqa: PLC0415
    from scipy.sparse.csgraph import dijkstra      # noqa: PLC0415

    if data.ndim != 3:
        raise ValueError("Kinematic selection requires a 3-D cube.")
    nz, ny, nx = data.shape
    sz, sy, sx = int(seed_zyx[0]), int(seed_zyx[1]), int(seed_zyx[2])
    if not (0 <= sz < nz and 0 <= sy < ny and 0 <= sx < nx):
        raise ValueError(f"Seed {(sz, sy, sx)} is outside the cube {data.shape}.")
    if not np.isfinite(data[sz, sy, sx]):
        raise ValueError("Seed voxel is blank/NaN — click on visible emission.")

    if sigma is None or not np.isfinite(sigma) or sigma <= 0.0:
        sigma = _robust_sigma(data)
    sigma = float(sigma)

    # Work inside a seed-centred WINDOW, so every full-array temporary (candidate,
    # labels, component) is bounded by the window, not the whole cube — this is what
    # keeps the connected-component step out-of-core on datasets larger than the one
    # object being grown. roi_radius>0 = an explicit hard box; 0 = a generous safety
    # radius that still covers a large galaxy while capping the worst case.
    r = int(roi_radius) if (roi_radius and int(roi_radius) > 0) else _SAFE_RADIUS
    wz0, wz1 = max(0, sz - r), min(nz, sz + r + 1)
    wy0, wy1 = max(0, sy - r), min(ny, sy + r + 1)
    wx0, wx1 = max(0, sx - r), min(nx, sx + r + 1)
    w = data[wz0:wz1, wy0:wy1, wx0:wx1]        # memmap view — no full-window copy
    lz, ly, lx = sz - wz0, sy - wy0, sx - wx0  # seed in window-local coords

    # Candidate emission: finite voxels at/above the nσ contour that DEFINES the
    # object. Below that the soft-threshold cost blows up anyway, so seeding the
    # component from ≥nσ both grabs the whole object and keeps the graph small.
    thr = float(nsigma) * sigma
    finite_w = np.isfinite(w)
    candidate = finite_w & (np.asarray(w) > thr)
    candidate[lz, ly, lx] = True               # the clicked voxel always seeds it

    # Connected component (6-connectivity) containing the seed → the object.
    structure = ndimage.generate_binary_structure(3, 1)
    labels, _ = ndimage.label(candidate, structure=structure)
    seed_label = int(labels[lz, ly, lx])
    if seed_label == 0:                        # seed isolated → nothing to grow
        mask = np.zeros((nz, ny, nx), dtype=bool)
        mask[sz, sy, sx] = True
        return mask
    comp = labels == seed_label
    del labels, candidate

    # Include a shell of sub-nσ voxels so ``reach`` / ``lambda_v`` can bleed the grow
    # into the faint margin just beyond the nσ contour (the geodesic then bounds it).
    # A near-threshold voxel costs ≈1 per step, so the budget can pay for ≈reach steps
    # (×1/min(lambda_v,1) when velocity steps are cheap) — size the shell for that so
    # the faint outskirts are never clipped before dijkstra decides.
    denom = min(float(lambda_v), 1.0)
    margin = 1 + int(math.ceil(float(reach) / denom)) if denom > 0.0 else 49
    margin = max(1, min(margin, 48))
    comp = ndimage.binary_dilation(comp, structure=structure, iterations=margin)
    comp &= finite_w                           # NaN stays a hard barrier
    comp[lz, ly, lx] = True

    # Worst-case guard: shrink to a seed-centred box until the node count is sane.
    if int(comp.sum()) > _MAX_SELECT_NODES:
        rr = 128
        while rr >= 8:
            box = np.zeros_like(comp)
            box[max(0, lz - rr):lz + rr + 1,
                max(0, ly - rr):ly + rr + 1,
                max(0, lx - rr):lx + rr + 1] = True
            trimmed = comp & box
            if int(trimmed.sum()) <= _MAX_SELECT_NODES:
                comp = trimmed
                break
            rr //= 2

    # Tight bbox of the component (window-local) → build the graph only over it.
    zc = np.where(comp.any(axis=(1, 2)))[0]
    yc = np.where(comp.any(axis=(0, 2)))[0]
    xc = np.where(comp.any(axis=(0, 1)))[0]
    z0, z1 = int(zc[0]), int(zc[-1]) + 1
    y0, y1 = int(yc[0]), int(yc[-1]) + 1
    x0, x1 = int(xc[0]), int(xc[-1]) + 1
    sub = comp[z0:z1, y0:y1, x0:x1]
    subdata = np.asarray(w[z0:z1, y0:y1, x0:x1], dtype=np.float64)
    dz, dy, dx = sub.shape

    # Node ids for component voxels only (−1 elsewhere).
    idx = np.full(sub.shape, -1, dtype=np.int64)
    n_nodes = int(sub.sum())
    idx[sub] = np.arange(n_nodes)

    # Per-voxel traversal cost: soft threshold at nσ·σ — a voxel at nσ costs 1,
    # brighter voxels are ~free (the grow flows through the object), fainter ones
    # cost exp((nσ − I)/σ) so ``reach`` controls how far into the near-nσ margin
    # the grow bleeds before stopping.
    with np.errstate(over="ignore"):
        costv = np.exp(float(nsigma) - np.clip(subdata, 0.0, None) / sigma)
    costv = np.minimum(costv, 1.0e6)

    rows: list[np.ndarray] = []
    cols: list[np.ndarray] = []
    wts: list[np.ndarray] = []

    def _link(a: np.ndarray, b: np.ndarray, ca: np.ndarray, cb: np.ndarray,
              factor: float) -> None:
        # Directed edges between adjacent COMPONENT voxels only; entering a voxel
        # costs that voxel's cost. Velocity (z) steps are weighted ×factor.
        a = a.ravel(); b = b.ravel(); ca = ca.ravel(); cb = cb.ravel()
        v = (a >= 0) & (b >= 0)
        rows.append(a[v]); cols.append(b[v]); wts.append(cb[v] * factor)
        rows.append(b[v]); cols.append(a[v]); wts.append(ca[v] * factor)

    if dz > 1:
        _link(idx[:-1, :, :], idx[1:, :, :], costv[:-1, :, :], costv[1:, :, :],
              float(lambda_v))                                   # z = velocity axis
    if dy > 1:
        _link(idx[:, :-1, :], idx[:, 1:, :], costv[:, :-1, :], costv[:, 1:, :], 1.0)
    if dx > 1:
        _link(idx[:, :, :-1], idx[:, :, 1:], costv[:, :, :-1], costv[:, :, 1:], 1.0)

    seed_node = int(idx[lz - z0, ly - y0, lx - x0])
    if not rows or not any(r.size for r in rows):   # single-voxel component
        mask = np.zeros((nz, ny, nx), dtype=bool)
        mask[sz, sy, sx] = True
        return mask

    graph = csr_matrix(
        (np.concatenate(wts), (np.concatenate(rows), np.concatenate(cols))),
        shape=(n_nodes, n_nodes),
    )
    # limit prunes nodes past the budget → bounded work; unreachable → inf.
    dist = dijkstra(graph, directed=True, indices=seed_node, limit=float(reach))

    sel_sub = np.zeros(sub.shape, dtype=bool)
    reached = np.asarray(dist <= float(reach))
    node_zyx = np.where(sub)
    sel_sub[node_zyx] = reached

    # Map the window-local bbox selection back to full-cube coordinates.
    mask = np.zeros((nz, ny, nx), dtype=bool)
    mask[wz0 + z0:wz0 + z1, wy0 + y0:wy0 + y1, wx0 + x0:wx0 + x1] = sel_sub
    return mask


def cube_kinematic_select_multi(
    data: np.ndarray,
    seeds: list[tuple[int, int, int]],
    sub_seeds: list[tuple[int, int, int]],
    reach: float = 8.0,
    lambda_v: float = 1.0,
    sigma: float | None = None,
    roi_radius: int = 0,
    nsigma: float = 2.5,
) -> np.ndarray:
    """Union of the geodesic selections grown from each seed in ``seeds``, minus
    the selections grown from each seed in ``sub_seeds`` — so the user can ⇧-click
    to ADD another source and ⌥-click to CARVE a region out. A ``sub_seed`` that
    lands on blank/NaN is skipped (nothing to subtract)."""
    if not seeds:
        raise ValueError("At least one selection seed is required.")
    mask: np.ndarray | None = None
    for s in seeds:
        m = cube_kinematic_select(data, s, reach, lambda_v, sigma, roi_radius, nsigma)
        mask = m if mask is None else (mask | m)
    assert mask is not None
    for s in sub_seeds:
        try:
            m = cube_kinematic_select(data, s, reach, lambda_v, sigma, roi_radius, nsigma)
        except ValueError:
            continue    # sub-seed on blank emission → nothing to remove
        mask &= ~m
    return mask


def worker_cube_kinematic_select(
    path: str,
    seeds: list[tuple[int, int, int]],
    sub_seeds: list[tuple[int, int, int]],
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
        # Runs while the memmap is open; each grow reads ONLY the ROI around its
        # seed (out-of-core on huge cubes). The returned mask is a fresh array,
        # safe to use after the file closes.
        mask = cube_kinematic_select_multi(
            data, list(seeds), list(sub_seeds),
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


def worker_cube_mask_selection(
    path: str,
    output_path: str,
    seeds: list[tuple[int, int, int]],
    sub_seeds: list[tuple[int, int, int]],
    reach: float,
    lambda_v: float,
    sigma: float,
    roi_radius: int,
    nsigma: float,
    mode: str,
) -> dict[str, Any]:
    """Apply a Kinematic Lasso selection to the cube: blank the voxels INSIDE the
    selection (mode="blank_inside", remove the object) or OUTSIDE it
    (mode="blank_outside", isolate the object), keeping full dimensions + WCS, and
    write the masked cube as a new dataset. Out-of-core (copy + memmap update),
    same as the box-mask worker but with the geodesic selection mask."""
    import shutil                             # noqa: PLC0415
    from astropy.io import fits as _fits      # noqa: PLC0415

    if mode not in ("blank_inside", "blank_outside"):
        raise ValueError(
            f"Unknown mask mode {mode!r}; expected 'blank_inside' or 'blank_outside'.")

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = ScientificFitsDataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("3-D masking requires a 3-D cube dataset.")
        mask = cube_kinematic_select_multi(
            data, list(seeds), list(sub_seeds),
            reach=float(reach), lambda_v=float(lambda_v),
            sigma=float(sigma) if sigma and sigma > 0 else None,
            roi_radius=int(roi_radius), nsigma=float(nsigma))
    finally:
        hdul.close()
    if not mask.any():
        raise ValueError("Selection is empty — nothing to mask.")

    shutil.copyfile(path, output_path)
    out_hdul = _fits.open(output_path, mode="update", memmap=True)
    try:
        arr = ScientificFitsDataset.squeeze_to_3d(out_hdul[0].data)
        if arr.shape != mask.shape:
            raise ValueError("Masked cube shape does not match the selection.")
        if mode == "blank_inside":
            arr[mask] = np.nan
            kept = arr[::max(1, arr.shape[0] // 32), ::max(1, arr.shape[1] // 64),
                       ::max(1, arr.shape[2] // 64)]
        else:  # blank_outside — isolate the selection
            arr[~mask] = np.nan
            kept = arr[mask]
        out_hdul[0].header.add_history(
            f"VisIVO Kinematic Lasso mask ({mode}) from {Path(path).name}: "
            f"{len(seeds)} add + {len(sub_seeds)} subtract seeds, "
            f"reach={reach}, lambda_v={lambda_v}")
        range_min, range_max = _finite_range(kept)
        out_hdul.flush()
        nz, ny, nx = arr.shape
    finally:
        out_hdul.close()

    return {
        "width": int(nx), "height": int(ny), "depth": int(nz),
        "range_min": float(range_min), "range_max": float(range_max),
        "output_path": output_path,
    }
