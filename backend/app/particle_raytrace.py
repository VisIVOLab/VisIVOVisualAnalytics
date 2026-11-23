"""Ray-traced particle rendering (issue #101): a still of a simulation snapshot
as a continuous medium that emits and absorbs light.

The method is the ray-traced Gaussian splatting with emission and absorption
described by Dolag et al. 2008 (New J. Phys. 10, 125006), implemented here
from the paper:

- each particle is a Gaussian of width ``h`` (its smoothing length), cut at
  ``_CUTOFF`` h; a ray passing at a distance ``d`` from it crosses the column
  ``s = w exp(-d² / h²)``;
- along a ray the particles are met back to front and each changes the light
  per colour channel as a uniform slab of emission ``e`` and absorption ``a``:
  ``I ← I exp(-a s) + (e / a)(1 - exp(-a s))``, which is ``I + e s`` without
  absorption (then the order does not matter);
- the light is accumulated in floating point, without a ceiling, and turned
  into an image afterwards by a tone curve.

Two adjustments keep a picture faithful at any zoom. A particle smaller than
``_MIN_RADIUS_PX`` pixels is drawn at that size with its light conserved
(``w`` scaled by the square of the shrink), so dense cores neither vanish nor
alias. The intensity is weighted by what it measures: a density (an intensive
quantity) by ``h``, the column a ray crosses, so a gas shows its projected
density; an amount per particle (mass, luminosity: extensive) by ``1 / h²``,
so a particle's light is its amount, however far it is spread.

The work is parallel over image tiles: each tile walks its own list of
particles, sorted back to front, so the composite is exact and deterministic.
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from .numba_parallel import LOCK as PARALLEL

try:   # numba is a backend dependency; the fallback only keeps imports working
    from numba import njit, prange
except ImportError:   # pragma: no cover
    njit = None
    prange = range

_CUTOFF = 2.5           # the kernel is drawn out to 2.5 h (exp(-6.25) = 0.2 %)
_MIN_RADIUS_PX = 1.0    # smaller particles are widened, their light conserved: a
                        # Gaussian 1 px wide sums to its integral on the pixel grid
_TILE = 32              # pixels per tile side
_EDGE = 1.0e-4          # pixels: the cutoff circle's edge is in (see _composite)

QUANTITIES = ("density", "per_particle")
_NO_DEPTH = np.zeros(0, dtype=np.float32)
_NO_CODES = np.zeros(0, dtype=np.uint16)
_NO_ABSORPTION = np.zeros((0, 3), dtype=np.float32)


@dataclass
class Camera:
    """A VTK camera: position, focal point, view up, vertical view angle in
    degrees, and the parallel scale when the projection is parallel."""
    position: tuple[float, float, float]
    focal_point: tuple[float, float, float]
    view_up: tuple[float, float, float]
    view_angle: float = 30.0
    parallel: bool = False
    parallel_scale: float = 1.0

    @classmethod
    def from_vtk(cls, cam) -> "Camera":
        return cls(tuple(cam.GetPosition()), tuple(cam.GetFocalPoint()),
                   tuple(cam.GetViewUp()), float(cam.GetViewAngle()),
                   bool(cam.GetParallelProjection()), float(cam.GetParallelScale()))


def project(positions: np.ndarray, radius: np.ndarray, camera: Camera,
            width: int, height: int):
    """Screen x, y (pixels, origin top left), radius in pixels and depth of
    each particle; depth <= 0 for one behind the camera."""
    pos = np.asarray(camera.position, dtype=np.float64)
    fp = np.asarray(camera.focal_point, dtype=np.float64)
    forward = fp - pos
    forward /= np.linalg.norm(forward) or 1.0
    up = np.asarray(camera.view_up, dtype=np.float64)
    right = np.cross(forward, up)
    right /= np.linalg.norm(right) or 1.0
    up = np.cross(right, forward)
    rel = positions.astype(np.float64, copy=False) - pos
    x, y, z = rel @ right, rel @ up, rel @ forward
    if camera.parallel:
        scale = height / (2.0 * max(camera.parallel_scale, 1e-30))
        sx, sy, sr = x * scale, y * scale, radius * scale
        depth = z
    else:
        f = (height / 2.0) / math.tan(math.radians(camera.view_angle) / 2.0)
        safe = np.where(z > 0, z, 1.0)
        sx, sy, sr = x * f / safe, y * f / safe, radius * f / safe
        depth = z
    return (width / 2.0 + sx).astype(np.float32), (height / 2.0 - sy).astype(np.float32), \
        sr.astype(np.float32), depth


if njit is not None:
    @njit(cache=True, nogil=True)
    def _tile_span(x, y, r, cutoff, tiles_x, tiles_y):
        """The tiles a kernel at (x, y) of width r, drawn out to cutoff r,
        touches: tx0, tx1, ty0, ty1."""
        reach = cutoff * r
        return (max(0, int((x - reach) // _TILE)), min(tiles_x - 1, int((x + reach) // _TILE)),
                max(0, int((y - reach) // _TILE)), min(tiles_y - 1, int((y + reach) // _TILE)))

    @njit(parallel=True, cache=True, nogil=True)
    def _bin(sx, sy, sr, order, cutoff, width, height, tiles_x, tiles_y):
        """Per tile, the particles (in ``order``) whose footprint touches it,
        each tile's in the order given. Parallel over chunks of ``order``:
        each counts its particles per tile, then writes them at its offset
        within each tile's run (chunk after chunk, so the order is kept)."""
        n_tiles = tiles_x * tiles_y
        n = order.size
        n_chunks = max(1, min(64, n // 16384))
        step = (n + n_chunks - 1) // n_chunks
        per = np.zeros((n_chunks, n_tiles), dtype=np.int64)
        for c in prange(n_chunks):
            for k in range(c * step, min(n, (c + 1) * step)):
                i = order[k]
                tx0, tx1, ty0, ty1 = _tile_span(sx[i], sy[i], sr[i], cutoff, tiles_x, tiles_y)
                for ty in range(ty0, ty1 + 1):
                    for tx in range(tx0, tx1 + 1):
                        per[c, ty * tiles_x + tx] += 1
        counts = np.zeros(n_tiles + 1, dtype=np.int64)
        for t in range(n_tiles):
            run = counts[t]
            for c in range(n_chunks):
                here = per[c, t]
                per[c, t] = run           # where chunk c starts in tile t
                run += here
            counts[t + 1] = run
        items = np.empty(counts[-1], dtype=np.int64)
        for c in prange(n_chunks):
            fill = per[c]
            for k in range(c * step, min(n, (c + 1) * step)):
                i = order[k]
                tx0, tx1, ty0, ty1 = _tile_span(sx[i], sy[i], sr[i], cutoff, tiles_x, tiles_y)
                for ty in range(ty0, ty1 + 1):
                    for tx in range(tx0, tx1 + 1):
                        t = ty * tiles_x + tx
                        items[fill[t]] = i
                        fill[t] += 1
        return counts, items

    @njit(parallel=True, fastmath=True, cache=True, nogil=True)
    def _composite(sx, sy, sr, weight, emission, absorption, depth, codes, cutoff, counts,
                   items, width, height, tiles_x, tiles_y):
        """The light per pixel, (height, width, 3), and with ``depth`` (one per
        particle; empty: none) the depth weighted by the light each particle
        leaves in the pixel, composited as the light is (so a particle hidden
        by the absorption of nearer ones weighs less): its sum over the
        pixel's three channels, ``zsum``, (height, width), or (1, 1) without
        depth. ``zsum`` / the light's channel sum is the pixel's mean depth.

        ``emission`` (N, 3) per particle, or with ``codes`` (N 16-bit codes;
        empty: none) a palette that the codes index over 0..65535; one row
        alone gives every particle its emission. ``absorption`` (N, 3), or
        (0, 3) for none. Each kernel is drawn out to ``cutoff`` times its
        width."""
        image = np.zeros((height, width, 3), dtype=np.float32)
        with_depth = depth.size > 0
        absorbs = absorption.shape[0] > 0
        palette = codes.size > 0
        one = emission.shape[0] == 1
        n_pal = emission.shape[0]
        zsum = np.zeros((height, width) if with_depth else (1, 1), dtype=np.float32)
        n_tiles = tiles_x * tiles_y
        cut2 = cutoff * cutoff
        # Tiles dealt out in a stride (a prime, so every tile once), not in
        # rows: the dense middle of a picture would otherwise fall to a few
        # threads, which a static schedule gives consecutive tiles.
        stride = np.int64(7919) if n_tiles % 7919 != 0 else np.int64(1)
        for j in prange(n_tiles):
            t = (np.int64(j) * stride) % n_tiles
            ty, tx = t // tiles_x, t % tiles_x
            x0, y0 = tx * _TILE, ty * _TILE
            x1, y1 = min(width, x0 + _TILE), min(height, y0 + _TILE)
            fx = np.empty(_TILE, dtype=np.float32)   # the kernel along x, per column
            for k in range(counts[t], counts[t + 1]):
                i = items[k]
                r = sr[i]
                reach = cutoff * r
                inv_r2 = 1.0 / (r * r)
                cx, cy = sx[i], sy[i]
                px0 = max(x0, int(math.floor(cx - reach)))
                px1 = min(x1, int(math.ceil(cx + reach)) + 1)
                py0 = max(y0, int(math.floor(cy - reach)))
                py1 = min(y1, int(math.ceil(cy + reach)) + 1)
                if px1 <= px0 or py1 <= py0:
                    continue
                w = weight[i]
                if palette:
                    c = (np.int64(codes[i]) * n_pal) >> 16
                elif one:
                    c = 0
                else:
                    c = i
                e0, e1, e2 = emission[c, 0], emission[c, 1], emission[c, 2]
                ez = (e0 + e1 + e2) * depth[i] if with_depth else 0.0
                # Grey: one absorption for the three channels, one exponential.
                kappa = absorption[i, 0] if absorbs else 0.0
                # exp(-(dx² + dy²) / r²) = exp(-dx² / r²) exp(-dy² / r²): one
                # exponential per column and per row of the footprint.
                for px in range(px0, px1):
                    dx = px + 0.5 - cx
                    fx[px - px0] = math.exp(-dx * dx * inv_r2)
                for py in range(py0, py1):
                    dy = py + 0.5 - cy
                    dy2 = dy * dy * inv_r2
                    if dy2 > cut2 + _EDGE:
                        continue
                    # The row's pixels within the cutoff circle: |dx| <= xr,
                    # widened by _EDGE so a centre exactly on the circle is in
                    # whatever the rounding of the square root (fastmath).
                    xr = math.sqrt(max(cut2 - dy2, 0.0)) * r + _EDGE
                    qx0 = max(px0, int(math.ceil(cx - 0.5 - xr)))
                    qx1 = min(px1, int(math.floor(cx - 0.5 + xr)) + 1)
                    wy = w * math.exp(-dy2)
                    if kappa > 0.0:
                        for px in range(qx0, qx1):
                            a = kappa * wy * fx[px - px0]
                            # expm1: a faint particle still dims what is
                            # behind it, and a million of them add up.
                            lost = -math.expm1(-a)
                            att = 1.0 - lost
                            f = lost / kappa
                            image[py, px, 0] = image[py, px, 0] * att + e0 * f
                            image[py, px, 1] = image[py, px, 1] * att + e1 * f
                            image[py, px, 2] = image[py, px, 2] * att + e2 * f
                            if with_depth:
                                zsum[py, px] = zsum[py, px] * att + ez * f
                    elif with_depth:
                        for px in range(qx0, qx1):
                            s = wy * fx[px - px0]
                            image[py, px, 0] += e0 * s
                            image[py, px, 1] += e1 * s
                            image[py, px, 2] += e2 * s
                            zsum[py, px] += ez * s
                    else:
                        for px in range(qx0, qx1):
                            s = wy * fx[px - px0]
                            image[py, px, 0] += e0 * s
                            image[py, px, 1] += e1 * s
                            image[py, px, 2] += e2 * s
        return image, zsum


def emission_weights(intensity: np.ndarray, radius: np.ndarray, quantity: str) -> np.ndarray:
    """Each particle's emission at the peak of its kernel, from its intensity
    and what that measures (see the module doc): a density by ``h``, an amount
    per particle by ``1 / h²``. Relative to the median size of all the
    particles, not of those in view: a movie's frames weigh every particle
    alike, so nothing flickers. Shared with the desktop's GPU preview, which
    sums the same kernels."""
    if quantity not in QUANTITIES:
        raise ValueError(f"Unknown quantity {quantity!r}: one of {QUANTITIES}.")
    e = np.asarray(intensity, dtype=np.float64)
    radius = np.asarray(radius, dtype=np.float64)
    sized = radius[np.isfinite(radius) & (radius > 0)]
    scale = float(np.median(sized)) if sized.size else 1.0
    if quantity == "density":
        return e * (radius / scale)
    return e * (scale / np.maximum(radius, 1e-30 * scale)) ** 2


def render(positions: np.ndarray, radius: np.ndarray, colour: np.ndarray,
           intensity: np.ndarray, camera: Camera, width: int, height: int, *,
           absorption: float = 0.0, quantity: str = "density") -> np.ndarray:
    """The light reaching each pixel, ``(height, width, 3)`` float32.

    ``colour``: (N, 3) RGB in [0, 1] of each particle; ``intensity``: (N,) its
    emission (any positive scale); ``radius``: (N,) its smoothing length in
    the positions' units; ``absorption``: a grey absorption proportional to
    the intensity, given as the optical depth reached where the emitted light
    is brightest (0: pure emission, the order of the particles does not
    matter; 1: those regions dim their own background by e); ``quantity``:
    what the intensity is, a ``density`` or an amount ``per_particle``."""
    if njit is None:   # pragma: no cover
        raise RuntimeError("Ray-traced particle rendering needs numba.")
    n = len(positions)
    if n == 0:
        return np.zeros((height, width, 3), dtype=np.float32)
    radius = np.asarray(radius, dtype=np.float64)
    sx, sy, sr, depth = project(positions, radius, camera, width, height)
    keep = (depth > 0) & np.isfinite(sx) & np.isfinite(sy) & (sr > 0) \
        & np.isfinite(intensity) & (intensity > 0)
    # Widened to a visible size with the same light: the column scales as the
    # square of the size ratio.
    grown = np.maximum(sr, _MIN_RADIUS_PX).astype(np.float32)
    weight = (sr / grown) ** 2
    reach = _CUTOFF * grown
    keep &= (sx + reach >= 0) & (sx - reach < width) & (sy + reach >= 0) & (sy - reach < height)
    idx = np.flatnonzero(keep)
    # Back to front, the farthest first: needed only when the particles absorb
    # (emission alone adds up in any order).
    order = (idx[np.argsort(-depth[idx], kind="stable")] if absorption > 0 else idx).astype(np.int64)
    e = emission_weights(intensity, radius, quantity)
    emission = (np.asarray(colour, dtype=np.float64) * e[:, None]).astype(np.float32)
    weight = weight.astype(np.float32)
    kappa = 0.0
    if absorption > 0:
        # ``absorption`` is the optical depth where the emitted light is at its
        # brightest (the 99.5th percentile of the lit pixels), whatever the
        # units of the intensity: measured on an emission-only pass at a
        # quarter of the size (its pixels are 16 times larger: the light per
        # pixel is the same, the columns being per unit area).
        qw, qh = max(16, width // 4), max(16, height // 4)
        qx, qy, qr, _ = project(positions, radius, camera, qw, qh)
        qgrown = np.maximum(qr, _MIN_RADIUS_PX).astype(np.float32)
        qweight = ((qr / qgrown) ** 2).astype(np.float32)
        qtx, qty = -(-qw // _TILE), -(-qh // _TILE)
        grey = np.repeat(e.astype(np.float32)[:, None], 3, axis=1)
        with PARALLEL:   # one parallel kernel at a time (app/numba_parallel.py)
            qcounts, qitems = _bin(qx, qy, qgrown, order, _CUTOFF, qw, qh, qtx, qty)
            column = _composite(qx, qy, qgrown, qweight, grey, _NO_ABSORPTION, _NO_DEPTH,
                                _NO_CODES, _CUTOFF, qcounts, qitems, qw, qh, qtx, qty)[0][..., 0]
        lit = column[column > 0]
        ref = float(np.percentile(lit, 99.5)) if lit.size else 0.0
        kappa = float(absorption) / ref if ref > 0 else 0.0
    # Grey: the same in the three channels, so a thick region keeps the colour
    # of what it emits (its source function is colour / absorption).
    absorb = np.repeat((e * kappa)[:, None], 3, axis=1).astype(np.float32)
    return composite(sx, sy, grown, weight, emission, absorb, order, width, height)[0]


def composite(sx, sy, radius, weight, emission, absorption, order, width: int, height: int,
              depth: np.ndarray | None = None, codes: np.ndarray | None = None,
              cutoff: float = _CUTOFF):
    """``(light, zsum)`` of the particles ``order`` (back to front when they
    absorb): ``sx``, ``sy`` their centres in pixels, ``radius`` their kernels'
    widths in pixels (at least ``_MIN_RADIUS_PX``), ``weight`` the column at a
    kernel's peak, ``emission`` and ``absorption`` (N, 3) per unit column,
    float32 (``absorption`` None: none; ``emission`` a palette over
    ``codes``, or one row for all: see :func:`_composite`). ``depth`` (N,):
    the light-weighted depth ``zsum`` too (else None). ``cutoff``: how far
    each kernel is drawn, in kernel widths (the default loses 0.2 % of its
    light)."""
    if njit is None:   # pragma: no cover
        raise RuntimeError("Ray-traced particle rendering needs numba.")
    tiles_x, tiles_y = -(-width // _TILE), -(-height // _TILE)
    z = _NO_DEPTH if depth is None else np.ascontiguousarray(depth, dtype=np.float32)
    k = _NO_ABSORPTION if absorption is None else absorption
    c = _NO_CODES if codes is None else np.ascontiguousarray(codes, dtype=np.uint16)
    with PARALLEL:   # one parallel kernel at a time (app/numba_parallel.py)
        counts, items = _bin(sx, sy, radius, order, float(cutoff), width, height,
                             tiles_x, tiles_y)
        light, zsum = _composite(sx, sy, radius, weight, emission, k, z, c, float(cutoff),
                                 counts, items, width, height, tiles_x, tiles_y)
    return light, (zsum if depth is not None else None)


def smoothing_lengths(positions: np.ndarray, neighbours: int = 32, *,
                      boxsize=None, region=None) -> np.ndarray:
    """Smoothing lengths where a snapshot has none: half the distance to the
    ``neighbours``-th nearest particle, the support an SPH kernel would have
    with that many neighbours.

    ``boxsize``: the period of each axis (0: none), so a periodic box's
    particles find their neighbours across its faces (positions wrapped into
    [0, L)). ``region(lo, hi)``: the positions of every particle of the
    population within [lo, hi] on each axis (round the box on a periodic
    one; a bound may be infinite), these among them. Given, the particles
    are a cut of that population (a box, a sphere, a slab) and their
    neighbours are searched beyond its faces too (issue #414): within a
    margin of the cut's bounds, then, for the particles whose neighbours
    reach past that margin, within their own distance, which makes every
    length the one the whole population gives. None: the neighbours are the
    ``positions``."""
    from scipy.spatial import cKDTree   # noqa: PLC0415
    pts = np.asarray(positions, dtype=np.float64)
    n = len(pts)
    box = _boxsize(boxsize, pts.shape[1] if pts.ndim == 2 else 3)
    if region is None:
        k = min(neighbours + 1, n)
        if k < 2:
            return np.ones(n, dtype=np.float32)
        wrapped = _wrapped(pts, box)
        # Only the k-th neighbour: the whole (N, k) answer is gigabytes for a
        # snapshot of tens of millions.
        dist, _ = cKDTree(wrapped, boxsize=box).query(wrapped, k=[k], workers=-1)
        return _half_distance(dist[:, 0])
    if not n:
        return np.ones(0, dtype=np.float32)
    return _half_distance(_cut_distances(_contiguous(pts, box), neighbours, region, box))


def _cut_distances(pts, neighbours: int, region, box) -> np.ndarray:
    """The ``neighbours``-th neighbour distance of a cut's particles ``pts``
    (contiguous round the box) among the region's (see smoothing_lengths)."""
    lo, hi = pts.min(axis=0), pts.max(axis=0)
    dist, margin = _neighbour_distance(pts, region, lo, hi,
                                       _margin(lo, hi, len(pts), neighbours), neighbours, box)
    short = np.flatnonzero(dist > _inside_distance(pts, lo, hi, margin, box))
    # The k-th distance found so far bounds the true one: every neighbour a
    # particle can have lies within it.
    for idx in _bands(dist, short):
        far = pts[idx]
        dist[idx], _ = _neighbour_distance(far, region, far.min(axis=0), far.max(axis=0),
                                           float(dist[idx].max()), neighbours, box)
    return dist


def _bands(dist: np.ndarray, which: np.ndarray) -> list[np.ndarray]:
    """``which`` (indices into ``dist``) in bands of a factor 2 of their
    distance, so one sparse particle does not widen the search of all the
    others; at most _MAX_BANDS (quantiles of it), each reading the population."""
    if not which.size:
        return []
    logd = np.log2(np.maximum(dist[which], np.finfo(np.float64).tiny))
    band = np.floor(logd)
    if len(np.unique(band)) > _MAX_BANDS:
        band = np.searchsorted(np.quantile(logd, np.linspace(0, 1, _MAX_BANDS + 1)[1:-1]), logd)
    return [which[band == b] for b in np.unique(band)]


def _margin(lo, hi, n: int, neighbours: int) -> float:
    """How far past a cut's bounds its neighbours are searched first: three
    times the radius that holds ``neighbours`` particles at the cut's mean
    density (on a flat extent, its largest side; 0 for a point)."""
    extent = np.asarray(hi, dtype=np.float64) - np.asarray(lo, dtype=np.float64)
    volume = float(np.prod(extent))
    if not (volume > 0 and math.isfinite(volume)):
        return float(extent.max()) if extent.size else 0.0
    return 3.0 * (3.0 * neighbours * volume / (4.0 * math.pi * n)) ** (1.0 / 3.0)


_MARGIN_GROWTHS = 8
_MAX_BANDS = 4


def _neighbour_distance(pts, region, lo, hi, margin: float, neighbours: int,
                        box) -> tuple[np.ndarray, float]:
    """``(distance, margin)``: the ``neighbours``-th neighbour distance of
    ``pts`` among the region's particles within ``margin`` of [lo, hi]
    (``pts`` among them), the margin widened sixteenfold while it holds too few
    (from a millionth of the bounds' size when it is 0), at last to the
    whole population."""
    from scipy.spatial import cKDTree   # noqa: PLC0415
    scale = max(1.0, float(np.abs(np.concatenate([lo, hi])).max()))
    for growth in range(_MARGIN_GROWTHS + 1):
        if growth == _MARGIN_GROWTHS:
            margin = math.inf
        pool = np.asarray(region(lo - margin, hi + margin), dtype=np.float64)[:, :3]
        if len(pool) > neighbours or math.isinf(margin):
            break
        margin = 16.0 * margin if margin > 0 else 1e-6 * scale
    k = min(neighbours + 1, len(pool))
    if k < 2:
        return np.zeros(len(pts)), margin
    dist, _ = cKDTree(_wrapped(pool, box), boxsize=box).query(_wrapped(pts, box), k=[k],
                                                               workers=-1)
    return dist[:, 0], margin


def _inside_distance(pts, lo, hi, margin: float, box) -> np.ndarray:
    """How far each of ``pts`` is from the faces of [lo - margin, hi + margin]:
    a sphere that small around it holds only particles the region gave. An
    axis whose padded interval covers its period has no face."""
    reach = np.full(len(pts), np.inf)
    for i in range(pts.shape[1]):
        if box is not None and box[i] > 0 and hi[i] - lo[i] + 2.0 * margin >= box[i]:
            continue
        reach = np.minimum(reach, np.minimum(pts[:, i] - (lo[i] - margin),
                                             (hi[i] + margin) - pts[:, i]))
    return reach


def _contiguous(pts: np.ndarray, box: np.ndarray | None) -> np.ndarray:
    """``pts`` moved round the box on each periodic axis so that they start
    past the widest empty arc: a cut across a face is one short interval, not
    one spanning the box (whose search would read the whole population)."""
    if box is None:
        return pts
    out = np.array(pts, dtype=np.float64)
    for i in np.flatnonzero(box):
        period = box[i]
        w = np.sort(np.mod(out[:, i], period))
        if len(w) < 2:
            continue
        gaps = np.diff(np.append(w, w[0] + period))
        start = w[(int(np.argmax(gaps)) + 1) % len(w)]
        out[:, i] = start + np.mod(out[:, i] - start, period)
    return out


def _boxsize(boxsize, dims: int) -> np.ndarray | None:
    """The periods as cKDTree takes them, None when no axis has one."""
    if boxsize is None:
        return None
    box = np.broadcast_to(np.asarray(boxsize, dtype=np.float64), (dims,)).copy()
    box[~(np.isfinite(box) & (box > 0))] = 0.0
    return box if box.any() else None


def _wrapped(pts: np.ndarray, box: np.ndarray | None) -> np.ndarray:
    """``pts`` folded into [0, L) on each periodic axis, as cKDTree needs."""
    if box is None:
        return pts
    out = np.array(pts, dtype=np.float64)
    for i in np.flatnonzero(box):
        col = np.mod(out[:, i], box[i])
        col[col >= box[i]] = 0.0
        out[:, i] = col
    return out


def _half_distance(dist: np.ndarray) -> np.ndarray:
    h = 0.5 * np.asarray(dist, dtype=np.float64)
    positive = h > 0
    floor = h[positive].min() if positive.any() else 1.0
    return np.where(positive, h, floor).astype(np.float32)


LIGHT_PERCENTILE = 99.5
#: A column spanning more decades than this may mix two populations (gas and
#: stars in one table), looked for as an empty stretch of
#: ``POPULATION_GAP_DECADES`` in its values.
WIDE_SPAN_DECADES = 20.0
POPULATION_GAP_DECADES = 10.0
#: How far above the reference the other population's light may reach at
#: least, in decades.
LIGHT_CEILING_DECADES = 2.0
_FLOAT32_DECADES = math.log10(float(np.finfo(np.float32).max))


def normalised_light(values, log10: bool = False,
                     power: float = 1.0) -> tuple[np.ndarray, np.ndarray, dict]:
    """``(light, usable, stats)`` of an intensity column: the float32 light
    of the ``usable`` rows raised to ``power``, 1 at a reference, the
    ``LIGHT_PERCENTILE``-th percentile of the values; ``log10``: the column
    holds its log10.

    A percentile, not the largest value: in a table mixing gas and stars the
    star rows lie tens of decades above the gas and, as the reference,
    pushed all of it to nothing. When the column splits into two populations
    (see ``WIDE_SPAN_DECADES``) the reference is the larger one's, and the
    other reaches no higher than that one's brightest row (at least
    ``LIGHT_CEILING_DECADES`` above the reference). A single population is
    only kept within float32's range: its contrast is the tone curve's.

    ``usable``: a finite value (in linear, a positive one too) whose light
    stays above 0 in float32. ``stats``: ``dropped_rows`` (the others),
    ``clipped_rows`` (capped) and ``light_span_decades`` (of the finite
    values), for a client to say what was left out."""
    logs = _log_light(values, log10)
    usable = np.isfinite(logs)
    finite = logs[usable]
    stats = {"dropped_rows": int(len(usable) - len(finite)), "clipped_rows": 0,
             "light_span_decades": 0.0}
    if not len(finite):
        return np.zeros(0, dtype=np.float32), usable, stats
    stats["light_span_decades"] = float(finite.max() - finite.min())
    reference, top = _light_reference(finite, stats["light_span_decades"])
    excess = float(power) * (finite - reference)
    # The sum of every particle's light stays finite in float32.
    ceiling = min(max(LIGHT_CEILING_DECADES, float(power) * (top - reference)),
                  _FLOAT32_DECADES - math.log10(len(finite)) - 2.0)
    stats["clipped_rows"] = int(np.count_nonzero(excess > ceiling))
    with np.errstate(under="ignore"):
        light = (10.0 ** np.minimum(excess, ceiling)).astype(np.float32)
    lit = light > 0
    usable[np.flatnonzero(usable)[~lit]] = False
    stats["dropped_rows"] += int(np.count_nonzero(~lit))
    return light[lit], usable, stats


def _light_reference(logs: np.ndarray, span: float) -> tuple[float, float]:
    """``(reference, top)``: the log10 value the light is 1 at and the
    largest of the population it is taken from (see :func:`normalised_light`)."""
    if span > WIDE_SPAN_DECADES:
        ordered = np.sort(logs)
        gaps = np.diff(ordered)
        cut = int(np.argmax(gaps))
        if gaps[cut] > POPULATION_GAP_DECADES:
            lower, upper = ordered[:cut + 1], ordered[cut + 1:]
            logs = lower if len(lower) >= len(upper) else upper
    return float(np.percentile(logs, LIGHT_PERCENTILE)), float(logs.max())


def _log_light(values, log10: bool) -> np.ndarray:
    v = np.asarray(values, dtype=np.float64)
    if log10:
        return v
    with np.errstate(divide="ignore", invalid="ignore"):
        return np.log10(v)


def velocity_dispersion(positions: np.ndarray, velocities: np.ndarray,
                        neighbours: int = 32, *, boxsize=None, region=None) -> np.ndarray:
    """Each particle's local velocity dispersion: the one-dimensional spread
    of the velocities of its ``neighbours`` nearest particles (itself
    included), sqrt(<|v - <v>|²> / 3).

    ``boxsize`` and ``region`` as for :func:`smoothing_lengths`, the
    region's rows carrying the velocities after the positions: a cut's
    particles take their neighbours from the whole population, past its
    faces and across a periodic box's."""
    pts = np.asarray(positions, dtype=np.float64)
    vel = np.asarray(velocities, dtype=np.float32)
    box = _boxsize(boxsize, pts.shape[1] if pts.ndim == 2 else 3)
    if region is None:
        wrapped = _wrapped(pts, box)
        return _dispersion(wrapped, wrapped, vel, neighbours, box)
    out = np.zeros(len(pts), dtype=np.float32)
    if not len(pts):
        return out
    pts = _contiguous(pts, box)
    dist = _cut_distances(pts, neighbours, region, box)
    # Every neighbour of a band's particles lies within its largest distance.
    for idx in _bands(dist, np.arange(len(pts))):
        near = pts[idx]
        # Widened by float32's rounding of the cut's positions, which the
        # region reads back in float64 (coincident particles: a 0 distance).
        reach = float(dist[idx].max()) + 4.0 * float(np.finfo(np.float32).eps) * max(
            1.0, float(np.abs(near).max()))
        pool = np.asarray(region(near.min(axis=0) - reach, near.max(axis=0) + reach),
                          dtype=np.float64)
        out[idx] = _dispersion(_wrapped(pool[:, :3], box), _wrapped(near, box),
                               pool[:, 3:6].astype(np.float32), neighbours, box)
    return out


def _dispersion(pool: np.ndarray, pts: np.ndarray, vel: np.ndarray, neighbours: int,
                box) -> np.ndarray:
    """The dispersion of ``pts`` among the ``pool``'s particles (velocities
    ``vel``), its tree queried in batches of bounded temporaries."""
    from scipy.spatial import cKDTree   # noqa: PLC0415
    k = min(neighbours + 1, len(pool))
    if k < 2:
        return np.zeros(len(pts), dtype=np.float32)
    tree = cKDTree(pool, boxsize=box)
    out = np.empty(len(pts), dtype=np.float32)
    batch = max(4096, 8_000_000 // k)   # bounded temporaries: (batch, k, 3)
    for start in range(0, len(pts), batch):
        _, idx = tree.query(pts[start:start + batch], k=k, workers=-1)
        v = vel[idx]                                   # (n, k, 3)
        dv = v - v.mean(axis=1, keepdims=True)
        out[start:start + len(v)] = np.sqrt((dv * dv).sum(axis=2).mean(axis=1) / 3.0)
    return out


TONE_CURVES = ("asinh", "log", "exponential", "linear")


def tone_map(image: np.ndarray, *, exposure: float | None = None, curve: str = "asinh",
             stretch: float = 10.0, gamma: float = 2.2, saturation: float = 1.0) -> np.ndarray:
    """An 8-bit RGB picture of the light ``image``.

    The colours keep their hue: the curve maps the luminance and the three
    channels follow it. ``exposure`` None: the 99.5th percentile of the lit
    pixels' luminance maps to 1. ``stretch``: how much the asinh and log
    curves lift the faint light (higher, more)."""
    if curve not in TONE_CURVES:
        raise ValueError(f"Unknown tone curve {curve!r}: one of {TONE_CURVES}.")
    rgb = np.asarray(image, dtype=np.float32)
    lum = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    if exposure is None:
        lit = lum[lum > 0]
        ref = float(np.percentile(lit, 99.5)) if lit.size else 1.0
        exposure = 1.0 / ref if ref > 0 else 1.0
    x = lum * float(exposure)
    if curve == "asinh":
        mapped = np.arcsinh(x * stretch) / math.asinh(stretch)
    elif curve == "log":
        mapped = np.log1p(x * stretch) / math.log1p(stretch)
    elif curve == "exponential":
        mapped = 1.0 - np.exp(-x)
    else:
        mapped = x
    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = np.where(lum > 0, mapped / lum, 0.0).astype(np.float32)
    out = rgb * ratio[..., None]
    if saturation != 1.0:
        grey = (out @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32))[..., None]
        out = grey + (out - grey) * float(saturation)
    # Above 1 in one channel: the colour is kept by scaling all three down.
    peak = out.max(axis=-1, keepdims=True)
    out = np.where(peak > 1.0, out / np.maximum(peak, 1e-30), out)
    out = np.clip(out, 0.0, 1.0)
    if gamma and gamma != 1.0:
        out = out ** (1.0 / gamma)
    return (out * 255.0 + 0.5).astype(np.uint8)


# ── Projected maps, as cosmological simulations are shown ────────────────────
#
# The look of the Millennium Simulation's images (Springel et al. 2005,
# Nature 435, 629: the projected density of a slab, its colour encoding the
# density and the local velocity dispersion): no perspective, no absorption,
# the column of every particle's kernel summed along the line of sight. Its
# brightness follows the logarithm of the projected density; its colour, the
# density itself or the mean of another quantity along the ray, weighted by
# the density (a two-dimensional colour table).

def parallel_camera(camera: Camera) -> Camera:
    """The same view without perspective: what the camera sees at its focal
    plane, at every depth."""
    if camera.parallel:
        return camera
    pos = np.asarray(camera.position, dtype=np.float64)
    fp = np.asarray(camera.focal_point, dtype=np.float64)
    half = np.linalg.norm(fp - pos) * math.tan(math.radians(camera.view_angle) / 2.0)
    return Camera(camera.position, camera.focal_point, camera.view_up, camera.view_angle,
                  True, float(half) or 1.0)


def in_slab(positions: np.ndarray, camera: Camera, thickness: float | None) -> np.ndarray:
    """Particles within ``thickness`` of the focal plane, centred on it."""
    if not thickness:
        return np.ones(len(positions), dtype=bool)
    pos = np.asarray(camera.position, dtype=np.float64)
    fp = np.asarray(camera.focal_point, dtype=np.float64)
    forward = fp - pos
    forward /= np.linalg.norm(forward) or 1.0
    depth = (positions.astype(np.float64, copy=False) - fp) @ forward
    return np.abs(depth) <= thickness / 2.0


def projected_maps(positions: np.ndarray, radius: np.ndarray, weight: np.ndarray,
                   value: np.ndarray | None, camera: Camera, width: int, height: int, *,
                   slab: float | None = None) -> np.ndarray:
    """``(height, width, 2)``: the projected density (the columns of the
    particles' kernels, each of mass ``weight``) and the density-weighted mean
    of ``value`` along each line of sight (NaN where nothing lies)."""
    cam = parallel_camera(camera)
    keep = in_slab(positions, cam, slab)
    w = np.asarray(weight, dtype=np.float32)[keep]
    v = (np.zeros(int(keep.sum()), dtype=np.float32) if value is None
         else np.nan_to_num(np.asarray(value, dtype=np.float32)[keep]))
    # Three additive channels: the mass, the mass times the value, nothing.
    channels = np.stack([np.ones_like(w), v, np.zeros_like(w)], axis=1)
    light = render(positions[keep], np.asarray(radius)[keep], channels, w, cam, width, height,
                   quantity="per_particle")
    density = light[..., 0]
    with np.errstate(divide="ignore", invalid="ignore"):
        mean = np.where(density > 0, light[..., 1] / density, np.nan)
    return np.stack([density, mean], axis=-1).astype(np.float32)


def density_brightness(density: np.ndarray, *, ev: float = 0.0, decades: float | None = None,
                       gamma: float = 1.0) -> np.ndarray:
    """Brightness in [0, 1] from the logarithm of the projected density: the
    window ends at the 99.9th percentile of the lit pixels (``ev`` stops
    brighter moves it down) and spans ``decades`` (None: down to the 1st
    percentile)."""
    lit = density[density > 0]
    if not lit.size:
        return np.zeros_like(density)
    logs = np.log10(lit)
    hi = float(np.percentile(logs, 99.9)) - ev * math.log10(2.0)
    lo = hi - decades if decades else float(np.percentile(logs, 1.0)) - ev * math.log10(2.0)
    if not hi > lo:
        lo = hi - 1.0
    with np.errstate(divide="ignore"):
        b = (np.log10(np.where(density > 0, density, np.nan)) - lo) / (hi - lo)
    b = np.clip(np.nan_to_num(b, nan=0.0), 0.0, 1.0)
    return b ** (1.0 / gamma) if gamma and gamma != 1.0 else b
