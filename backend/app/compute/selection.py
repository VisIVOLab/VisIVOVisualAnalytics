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
    margin_reach: float | None = None,
    smooth_zyx: tuple[float, float, float] | None = None,
) -> np.ndarray:
    """Geodesic, kinematics-aware region grow from ``seed_zyx`` (z=velocity).

    Thin wrapper over :func:`_geodesic_distance`: the selection at budget
    ``reach`` is just the sub-``reach`` level set of the geodesic distance from
    the seed. Keeping the distance field separate is what lets the front end
    re-threshold a single computed field while the user drags the Reach slider
    (see :func:`cube_kinematic_levels`) instead of re-running Dijkstra per step.
    """
    dist, (dz0, dy0, dx0) = _geodesic_distance(
        data, seed_zyx, reach, lambda_v, sigma, roi_radius, nsigma, margin_reach,
        smooth_zyx)
    mask = np.zeros(data.shape, dtype=bool)
    sel = dist <= float(reach)
    mask[dz0:dz0 + dist.shape[0], dy0:dy0 + dist.shape[1], dx0:dx0 + dist.shape[2]] = sel
    return mask


def _geodesic_distance(
    data: np.ndarray,
    seed_zyx: tuple[int, int, int],
    reach: float = 8.0,
    lambda_v: float = 1.0,
    sigma: float | None = None,
    roi_radius: int = 0,
    nsigma: float = 2.5,
    margin_reach: float | None = None,
    smooth_zyx: tuple[float, float, float] | None = None,
) -> tuple[np.ndarray, tuple[int, int, int]]:
    """Geodesic cost from ``seed_zyx``, over the object's bounding box only.

    Returns ``(dist, origin)``: ``dist`` is **float64** over the component bbox
    (``inf`` where unreachable within the budget) and ``origin`` is that box's
    ``(z, y, x)`` offset in cube voxels. The array is bounded by the OBJECT, never
    by the cube. The width is deliberate and must not be "optimised" to float32 to
    match the cost array — see the note at the scatter below: narrowing SciPy's
    distances moves values sitting within an ulp of a budget across the threshold.

    ``reach`` still matters here even though no thresholding happens: it bounds
    Dijkstra's exploration (and sizes the faint-margin shell), so asking for a
    larger budget costs more work. Distances returned are exact for every value
    up to ``reach``.

    ``margin_reach`` sizes the faint-margin shell independently of ``reach``.
    That knob exists because the shell is grown from the budget, so two runs at
    different budgets do not even build the same graph. Usually a larger shell
    only ADDS candidate voxels and can only lower distances — but not always: a
    shell large enough to cross ``_MAX_SELECT_NODES`` triggers the trimming below,
    which REMOVES nodes. So the level set of a big-budget run is not reliably a
    superset of a small-budget one, and the relation must not be assumed. Passing
    the same ``margin_reach`` to both sidesteps the question entirely: identical
    graph, and Dijkstra with a larger limit cannot change any node whose shortest
    distance is already ``<= r``. That is what keeps the interactive slider
    (:func:`cube_kinematic_levels`) and the apply that follows it in agreement.

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
    @param margin_reach shell budget; see above.
    @returns ``(dist, origin)`` — see the opening paragraph. This function does NOT
             return a mask; :func:`cube_kinematic_select` thresholds it into one.
    """
    from scipy import ndimage                     # noqa: PLC0415
    from scipy.sparse import csr_matrix           # noqa: PLC0415
    from scipy.sparse.csgraph import dijkstra      # noqa: PLC0415
    from skimage.segmentation import flood         # noqa: PLC0415

    if data.ndim != 3:
        raise ValueError("Kinematic selection requires a 3-D cube.")
    nz, ny, nx = data.shape
    sz, sy, sx = int(seed_zyx[0]), int(seed_zyx[1]), int(seed_zyx[2])
    if not (0 <= sz < nz and 0 <= sy < ny and 0 <= sx < nx):
        raise ValueError(f"Seed {(sz, sy, sx)} is outside the cube {data.shape}.")
    if not np.isfinite(data[sz, sy, sx]):
        raise ValueError("Seed voxel is blank/NaN (click on visible emission).")

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
    # Threshold straight off the memmap (no persistent float32 copy of the window): np.isfinite
    # and (w > thr) each read pages and yield a bool, so peak memory drops by one window-sized
    # float array on SKA-scale windows. NaN > thr is False, so NaN is excluded as before.
    finite_w = np.isfinite(w)

    # Optional smooth-before-threshold. A per-voxel cut on the raw cube cannot see
    # the smooth faint envelope of an extended source; a smooth-and-clip finder
    # such as SoFiA-2 can, and that — not the geodesic budget — is where the two
    # disagree on extent. Measured on WALLABY's NGC 3137 (SoFiA mask 299,562
    # voxels): the raw 2.5sigma component holding the clicked voxel is 47,994
    # voxels, 16% of it; the same threshold on a (2, 6, 6)-smoothed copy gives
    # 309,858, i.e. 103%.
    #
    # Normalised convolution, not a plain filter: blank voxels are zero-filled for
    # the numerator and the same kernel is run over the finite mask as denominator,
    # so emission beside a blank region is not diluted towards zero. NaN stays a
    # hard barrier either way, because `finite_w` is taken from the RAW window.
    #
    # Only the SELECTION is smoothed. Every measurement downstream runs on the raw
    # cube under the resulting mask, so fluxes are untouched.
    work = w
    if smooth_zyx is not None and any(float(v) > 0.0 for v in smooth_zyx):
        sm = tuple(max(0.0, float(v)) for v in smooth_zyx)
        num = ndimage.gaussian_filter(
            np.where(finite_w, w, 0.0).astype(np.float32), sigma=sm, mode="nearest")
        den = ndimage.gaussian_filter(
            finite_w.astype(np.float32), sigma=sm, mode="nearest")
        work = np.where(den > 1.0e-6, num / np.maximum(den, 1.0e-6), 0.0).astype(np.float32)
        del num, den
        # The noise drops with the kernel, so a threshold in sigma units has to be
        # re-measured on the smoothed data — reusing the raw sigma would make nsigma
        # mean something different for every kernel.
        thr = float(nsigma) * _robust_sigma(work)

    candidate = finite_w & (work > thr)
    candidate[lz, ly, lx] = True               # the clicked voxel always seeds it

    # Connected component (6-connectivity) containing the seed → the object. A seed FLOOD-FILL
    # returns just the seed's component, avoiding the window-sized int32 label array that
    # ndimage.label allocates for EVERY component. (flood still returns a window-shaped bool mask
    # and pads internally, so this trims the CC step's transient memory — it does not by itself
    # make the peak scale with the object; the peak is the geodesic graph over the component bbox.)
    # connectivity=1 == the 6-neighbour structure, and since the seed is forced into `candidate`,
    # the flooded region is exactly the component ndimage.label would tag with the seed's label
    # (result-identical — see test_flood_component_matches_ndimage_label).
    structure = ndimage.generate_binary_structure(3, 1)
    comp = flood(candidate, (lz, ly, lx), connectivity=1)
    del candidate

    # Include a shell of sub-nσ voxels so ``reach`` / ``lambda_v`` can bleed the grow
    # into the faint margin just beyond the nσ contour (the geodesic then bounds it).
    # A near-threshold voxel costs ≈1 per step, so the budget can pay for ≈reach steps
    # (×1/min(lambda_v,1) when velocity steps are cheap) — size the shell for that so
    # the faint outskirts are never clipped before dijkstra decides.
    denom = min(float(lambda_v), 1.0)
    shell_budget = float(reach if margin_reach is None else margin_reach)
    margin = 1 + int(math.ceil(shell_budget / denom)) if denom > 0.0 else 49
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
    # float32 (not float64) for the component's intensities + costs: over the object bbox this
    # halves the largest graph-step arrays. The cost is a soft threshold whose only downstream use
    # is a Dijkstra distance compared against `reach` (≈8); float32 keeps that to ~1e-6, well below
    # any voxel's cost, so the selection is unchanged (validated lasso-vs-SoFiA on HGC 44 — same
    # Dice to 3 dp). Node ids are exact in int32 (n_nodes < _MAX_SELECT_NODES = 4M ≪ 2^31).
    subdata = np.asarray(work[z0:z1, y0:y1, x0:x1], dtype=np.float32)
    dz, dy, dx = sub.shape

    # Node ids for component voxels only (−1 elsewhere).
    idx = np.full(sub.shape, -1, dtype=np.int32)
    n_nodes = int(sub.sum())
    idx[sub] = np.arange(n_nodes, dtype=np.int32)

    # Per-voxel traversal cost: soft threshold at nσ·σ — a voxel at nσ costs 1,
    # brighter voxels are ~free (the grow flows through the object), fainter ones
    # cost exp((nσ − I)/σ) so ``reach`` controls how far into the near-nσ margin
    # the grow bleeds before stopping.
    # Keep the scalars as Python floats: numpy's weak-scalar rule keeps the arrays float32 (no
    # float64 promotion), while sigma/nsigma retain full precision as the scalar operands — this
    # also avoids a np.float32(sigma) underflow to 0 for a pathologically tiny sigma.
    # `cost_sigma` is the scale of the data the cost is applied to: the raw sigma
    # normally, the smoothed one when smoothing is on. Mixing them would make a
    # voxel at the threshold cost something other than 1, which is the whole
    # calibration of `reach`.
    cost_sigma = float(thr) / float(nsigma) if float(nsigma) > 0.0 else float(sigma)
    with np.errstate(over="ignore"):
        costv = np.exp(float(nsigma) - np.clip(subdata, 0.0, None) / cost_sigma)
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
        return np.zeros((1, 1, 1), dtype=np.float64), (sz, sy, sx)

    graph = csr_matrix(
        (np.concatenate(wts), (np.concatenate(rows), np.concatenate(cols))),
        shape=(n_nodes, n_nodes),
    )
    # limit prunes nodes past the budget → bounded work; unreachable → inf.
    dist = dijkstra(graph, directed=True, indices=seed_node, limit=float(reach))

    # Scatter the node distances back onto the bbox; non-component voxels stay
    # inf so a caller thresholding at ANY budget excludes them.
    #
    # float64, deliberately, even though the cost array above is float32: SciPy
    # returns float64 distances and the OLD code compared those directly against
    # ``reach``. Narrowing them here would move values sitting within a float32
    # ulp of the budget (or of an integer level) across the boundary, so the
    # refactor would no longer be behaviour-preserving. This array is bounded by
    # the object bbox and is smaller than the cube-shaped bool mask the caller
    # used to allocate, so the width costs nothing that matters.
    dist_sub = np.full(sub.shape, np.inf, dtype=np.float64)
    dist_sub[np.where(sub)] = dist
    return dist_sub, (wz0 + z0, wy0 + y0, wx0 + x0)


# Sentinel level for "not reachable within the requested budget". uint8 gives us
# 0..254 as real levels, which is why the budget is capped just below it.
_LEVEL_UNREACHABLE = 255
MAX_LEVEL_REACH = 254


def cube_kinematic_levels(
    data: np.ndarray,
    seeds: list[tuple[int, int, int]],
    sub_seeds: list[tuple[int, int, int]],
    reach_max: float,
    lambda_v: float = 1.0,
    sigma: float | None = None,
    roi_radius: int = 0,
    nsigma: float = 2.5,
    smooth_zyx: tuple[float, float, float] | None = None,
) -> dict[str, Any]:
    """Geodesic level fields for an INTERACTIVE reach slider.

    The validation found that the useful ``reach`` is a property of the source's
    surface brightness, not of the instrument, so it cannot be defaulted from the
    header and has to stay a control the user drags (validation/README.md). This
    makes dragging it cheap: one Dijkstra pass up to ``reach_max`` produces a
    level field, and every budget below it is a THRESHOLD on that field — the
    front end re-thresholds locally instead of asking the backend per step.

    Levels are ``ceil(distance)`` in uint8. That quantisation is exact rather than
    approximate for the slider, because for an INTEGER budget r
    ``ceil(d) <= r  ⟺  d <= r`` — so a preview thresholded at level r is the same
    voxel set the exact float path would apply, and preview and apply cannot drift.

    Multiple seeds need no extra fields: a union of level sets is the level set of
    the pointwise minimum, so one ``add`` field (min over ``seeds``) and one
    ``sub`` field (min over ``sub_seeds``) describe every combination. The
    selection at budget r is ``add <= r`` minus ``sub <= r``.

    Returns the two fields over their shared bounding box plus that box's origin.
    """
    if not seeds:
        raise ValueError("At least one selection seed is required.")
    budget = float(reach_max)
    if not np.isfinite(budget) or budget <= 0.0:
        raise ValueError("reach_max must be a positive, finite budget.")
    if budget > MAX_LEVEL_REACH:
        raise ValueError(
            f"reach_max {budget:g} exceeds the {MAX_LEVEL_REACH} the uint8 level "
            f"field can represent; request a smaller budget.")

    def fields(points: list[tuple[int, int, int]], *, tolerate_blank: bool
               ) -> list[tuple[np.ndarray, tuple[int, int, int]]]:
        out = []
        for pt in points:
            try:
                out.append(_geodesic_distance(
                    data, pt, budget, lambda_v, sigma, roi_radius, nsigma,
                    margin_reach=budget, smooth_zyx=smooth_zyx))
            except ValueError:
                # Only a SUB seed may land on blank sky and be ignored (⌥-clicking
                # empty space removes nothing). An add seed that cannot grow is an
                # error, exactly as in cube_kinematic_select_multi() — tolerating it
                # here would let a bad primary seed produce a plausible-looking
                # field built from the other seeds alone.
                if not tolerate_blank:
                    raise
                continue
        return out

    add = fields(list(seeds), tolerate_blank=False)
    sub = fields(list(sub_seeds), tolerate_blank=True)

    # Union bbox of every contributing field, so both outputs share one grid.
    def bounds(fs):
        for d, (z, y, x) in fs:
            yield (z, y, x, z + d.shape[0], y + d.shape[1], x + d.shape[2])
    allb = list(bounds(add)) + list(bounds(sub))
    z0 = min(b[0] for b in allb); y0 = min(b[1] for b in allb); x0 = min(b[2] for b in allb)
    z1 = max(b[3] for b in allb); y1 = max(b[4] for b in allb); x1 = max(b[5] for b in allb)
    shape = (z1 - z0, y1 - y0, x1 - x0)

    def merge(fs: list[tuple[np.ndarray, tuple[int, int, int]]]) -> np.ndarray:
        # float64 like _geodesic_distance(): narrowing here would move a distance
        # sitting just above an integer budget down onto it before ceil(), which
        # is exactly the boundary flip the direct path was fixed to avoid.
        acc = np.full(shape, np.inf, dtype=np.float64)
        for d, (dz, dy, dx) in fs:
            view = acc[dz - z0:dz - z0 + d.shape[0],
                       dy - y0:dy - y0 + d.shape[1],
                       dx - x0:dx - x0 + d.shape[2]]
            np.minimum(view, d, out=view)
        return acc

    def quantise(f: np.ndarray) -> np.ndarray:
        lv = np.full(f.shape, _LEVEL_UNREACHABLE, dtype=np.uint8)
        ok = np.isfinite(f) & (f <= budget)
        # ceil, not round: level r must mean "reachable within a budget of r".
        lv[ok] = np.ceil(f[ok]).astype(np.uint8)
        return lv

    add_lv = quantise(merge(add))
    sub_lv = (quantise(merge(sub)) if sub
              else np.full(shape, _LEVEL_UNREACHABLE, dtype=np.uint8))
    return {
        "add_levels": add_lv, "sub_levels": sub_lv,
        "origin": (z0, y0, x0), "shape": shape,
        "reach_max": int(math.floor(budget)),
        "unreachable": _LEVEL_UNREACHABLE,
    }


def levels_to_mask(levels: dict[str, Any], reach: int, cube_shape: tuple[int, int, int]) -> np.ndarray:
    """Threshold a :func:`cube_kinematic_levels` result at an integer budget —
    the reference implementation of what the front end does locally."""
    z0, y0, x0 = levels["origin"]
    dz, dy, dx = levels["shape"]
    sel = (levels["add_levels"] <= reach) & ~(levels["sub_levels"] <= reach)
    mask = np.zeros(cube_shape, dtype=bool)
    mask[z0:z0 + dz, y0:y0 + dy, x0:x0 + dx] = sel
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
    margin_reach: float | None = None,
    smooth_zyx: tuple[float, float, float] | None = None,
) -> np.ndarray:
    """Union of the geodesic selections grown from each seed in ``seeds``, minus
    the selections grown from each seed in ``sub_seeds`` — so the user can ⇧-click
    to ADD another source and ⌥-click to CARVE a region out. A ``sub_seed`` that
    lands on blank/NaN is skipped (nothing to subtract)."""
    if not seeds:
        raise ValueError("At least one selection seed is required.")
    mask: np.ndarray | None = None
    for s in seeds:
        m = cube_kinematic_select(data, s, reach, lambda_v, sigma, roi_radius, nsigma,
                                  margin_reach, smooth_zyx)
        mask = m if mask is None else (mask | m)
    assert mask is not None
    for s in sub_seeds:
        try:
            m = cube_kinematic_select(data, s, reach, lambda_v, sigma, roi_radius, nsigma,
                                      margin_reach, smooth_zyx)
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
    margin_reach: float | None = None,
    smooth_zyx: tuple[float, float, float] | None = None,
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
            margin_reach=(float(margin_reach) if margin_reach else None),
            smooth_zyx=smooth_zyx,
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


def worker_cube_kinematic_levels(
    path: str,
    seeds: list[tuple[int, int, int]],
    sub_seeds: list[tuple[int, int, int]],
    reach_max: float,
    lambda_v: float,
    sigma: float,
    roi_radius: int,
    nsigma: float = 2.5,
    smooth_zyx: tuple[float, float, float] | None = None,
) -> dict[str, Any]:
    """Top-level worker (picklable) for the interactive reach slider: computes the
    geodesic level fields ONCE up to ``reach_max`` so the front end can re-threshold
    them per slider step instead of asking for a new grow each time.

    The payload is the two uint8 fields concatenated (add, then sub) over their
    shared bounding box — half the bytes of one float32 mask ROI of the same box,
    for every budget instead of one.
    """
    from ..fits_dataset import ScientificFitsDataset  # noqa: PLC0415

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = ScientificFitsDataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Kinematic selection requires a 3-D cube dataset.")
        lv = cube_kinematic_levels(
            data, list(seeds), list(sub_seeds), reach_max=float(reach_max),
            lambda_v=float(lambda_v),
            sigma=float(sigma) if sigma and sigma > 0 else None,
            roi_radius=int(roi_radius), nsigma=float(nsigma),
            smooth_zyx=smooth_zyx,
        )
        nz, ny, nx = data.shape
    finally:
        hdul.close()

    z0, y0, x0 = lv["origin"]
    dz, dy, dx = lv["shape"]
    payload = (np.ascontiguousarray(lv["add_levels"]).tobytes()
               + np.ascontiguousarray(lv["sub_levels"]).tobytes())
    return {
        "raw_bytes": payload,
        "roi_width": int(dx), "roi_height": int(dy), "roi_depth": int(dz),
        "origin_x": int(x0), "origin_y": int(y0), "origin_z": int(z0),
        "full_width": int(nx), "full_height": int(ny), "full_depth": int(nz),
        "reach_max": int(lv["reach_max"]), "unreachable": int(lv["unreachable"]),
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
    margin_reach: float | None = None,
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
            roi_radius=int(roi_radius), nsigma=float(nsigma),
            # Match the shell the preview's level field used, so what the user
            # saw on screen is exactly what gets written out.
            margin_reach=(float(margin_reach) if margin_reach else None))
    finally:
        hdul.close()
    if not mask.any():
        raise ValueError("Selection is empty (nothing to mask).")

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


def worker_cube_mask_apply(
    path: str,
    output_path: str,
    mask_bytes: bytes,
    origin_x: int, origin_y: int, origin_z: int,
    roi_width: int, roi_height: int, roi_depth: int,
    mode: str,
    recipe: str = "",
) -> dict[str, Any]:
    """Apply an EXPLICIT selection mask (a ROI bounding-box binary mask + its origin
    in cube voxels, little-endian float32 0/1 x-fastest = the same wire the select
    preview returns) — NOT re-grown from seeds. This is what lets a hand-refined
    mask be applied exactly as shown. Blanks inside (remove) or outside (isolate)
    the mask, keeps full dims + WCS, writes a new dataset. Out-of-core: only the ROI
    region + memmap writes, never a full-cube bool."""
    import shutil                             # noqa: PLC0415
    from astropy.io import fits as _fits      # noqa: PLC0415

    if mode not in ("blank_inside", "blank_outside"):
        raise ValueError(
            f"Unknown mask mode {mode!r}; expected 'blank_inside' or 'blank_outside'.")
    rw, rh, rd = int(roi_width), int(roi_height), int(roi_depth)
    expected = rw * rh * rd * 4
    if rw <= 0 or rh <= 0 or rd <= 0 or len(mask_bytes) != expected:
        raise ValueError(
            f"Mask payload is {len(mask_bytes)} bytes; expected {expected} "
            f"({rw}×{rh}×{rd} float32).")
    roi_mask = (np.frombuffer(mask_bytes, dtype="<f4").reshape(rd, rh, rw) > 0.5)
    if not roi_mask.any():
        raise ValueError("Mask is empty (nothing to apply).")

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = ScientificFitsDataset.squeeze_to_3d(raw)
        nz, ny, nx = data.shape
    finally:
        hdul.close()
    ox, oy, oz = int(origin_x), int(origin_y), int(origin_z)
    if (ox < 0 or oy < 0 or oz < 0
            or ox + rw > nx or oy + rh > ny or oz + rd > nz):
        raise ValueError("Mask ROI falls outside the cube bounds.")

    shutil.copyfile(path, output_path)
    out_hdul = _fits.open(output_path, mode="update", memmap=True)
    try:
        arr = ScientificFitsDataset.squeeze_to_3d(out_hdul[0].data)
        if arr.shape != (nz, ny, nx):
            raise ValueError("Masked cube shape does not match the source.")
        roi = arr[oz:oz + rd, oy:oy + rh, ox:ox + rw]
        if mode == "blank_inside":
            roi[roi_mask] = np.nan                       # touches only the ROI region
            kept = arr[::max(1, nz // 32), ::max(1, ny // 64), ::max(1, nx // 64)]
        else:  # blank_outside — isolate: keep ONLY the selected voxels
            saved = np.where(roi_mask, roi, np.nan)      # selected values, nan elsewhere
            arr[:] = np.nan                              # blank the whole cube (memmap I/O)
            arr[oz:oz + rd, oy:oy + rh, ox:ox + rw] = saved
            kept = saved[roi_mask]
        out_hdul[0].header.add_history(
            f"VisIVO Kinematic Lasso mask ({mode}) from {Path(path).name}: "
            f"explicit {int(roi_mask.sum())}-voxel mask at ({ox},{oy},{oz})")
        # How the mask was made (seed, reach, coupling, threshold, smoothing,
        # extra seeds, hand edits), as the client sent it: one parameter per
        # HISTORY entry, so the file alone says how to make it again. Nothing is
        # cut: astropy continues a value longer than a card onto further HISTORY
        # cards, and a seed list is exactly the item that must survive whole.
        for item in [part.strip() for part in str(recipe).split(";") if part.strip()]:
            out_hdul[0].header.add_history(f"Lasso {item}")
        range_min, range_max = _finite_range(kept)
        out_hdul.flush()
    finally:
        out_hdul.close()

    return {
        "width": int(nx), "height": int(ny), "depth": int(nz),
        "range_min": float(range_min), "range_max": float(range_max),
        "output_path": output_path,
    }
