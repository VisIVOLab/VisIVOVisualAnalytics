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

try:   # numba is a backend dependency; the fallback only keeps imports working
    from numba import njit, prange
except ImportError:   # pragma: no cover
    njit = None
    prange = range

_CUTOFF = 2.5           # the kernel is drawn out to 2.5 h (exp(-6.25) = 0.2 %)
_MIN_RADIUS_PX = 1.0    # smaller particles are widened, their light conserved: a
                        # Gaussian 1 px wide sums to its integral on the pixel grid
_TILE = 32              # pixels per tile side

QUANTITIES = ("density", "per_particle")


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
    def _bin(sx, sy, sr, order, width, height, tiles_x, tiles_y):
        """Per tile, the particles (in ``order``) whose footprint touches it."""
        n_tiles = tiles_x * tiles_y
        counts = np.zeros(n_tiles + 1, dtype=np.int64)
        for k in range(order.size):
            i = order[k]
            reach = _CUTOFF * sr[i]
            tx0 = max(0, int((sx[i] - reach) // _TILE))
            tx1 = min(tiles_x - 1, int((sx[i] + reach) // _TILE))
            ty0 = max(0, int((sy[i] - reach) // _TILE))
            ty1 = min(tiles_y - 1, int((sy[i] + reach) // _TILE))
            for ty in range(ty0, ty1 + 1):
                for tx in range(tx0, tx1 + 1):
                    counts[ty * tiles_x + tx + 1] += 1
        for t in range(n_tiles):
            counts[t + 1] += counts[t]
        fill = counts[:-1].copy()
        items = np.empty(counts[-1], dtype=np.int64)
        for k in range(order.size):
            i = order[k]
            reach = _CUTOFF * sr[i]
            tx0 = max(0, int((sx[i] - reach) // _TILE))
            tx1 = min(tiles_x - 1, int((sx[i] + reach) // _TILE))
            ty0 = max(0, int((sy[i] - reach) // _TILE))
            ty1 = min(tiles_y - 1, int((sy[i] + reach) // _TILE))
            for ty in range(ty0, ty1 + 1):
                for tx in range(tx0, tx1 + 1):
                    t = ty * tiles_x + tx
                    items[fill[t]] = i
                    fill[t] += 1
        return counts, items

    @njit(parallel=True, fastmath=True, cache=True, nogil=True)
    def _composite(sx, sy, sr, weight, emission, absorption, counts, items,
                   width, height, tiles_x, tiles_y):
        image = np.zeros((height, width, 3), dtype=np.float32)
        n_tiles = tiles_x * tiles_y
        # Tiles dealt out in a stride (a prime, so every tile once), not in
        # rows: the dense middle of a picture would otherwise fall to a few
        # threads, which a static schedule gives consecutive tiles.
        stride = np.int64(7919) if n_tiles % 7919 != 0 else np.int64(1)
        for j in prange(n_tiles):
            t = (np.int64(j) * stride) % n_tiles
            ty, tx = t // tiles_x, t % tiles_x
            x0, y0 = tx * _TILE, ty * _TILE
            x1, y1 = min(width, x0 + _TILE), min(height, y0 + _TILE)
            for k in range(counts[t], counts[t + 1]):
                i = items[k]
                r = sr[i]
                reach = _CUTOFF * r
                inv_r2 = 1.0 / (r * r)
                px0 = max(x0, int(math.floor(sx[i] - reach)))
                px1 = min(x1, int(math.ceil(sx[i] + reach)) + 1)
                py0 = max(y0, int(math.floor(sy[i] - reach)))
                py1 = min(y1, int(math.ceil(sy[i] + reach)) + 1)
                w = weight[i]
                e0, e1, e2 = emission[i, 0], emission[i, 1], emission[i, 2]
                # Grey: one absorption for the three channels, one exponential.
                kappa = absorption[i, 0]
                for py in range(py0, py1):
                    dy = py + 0.5 - sy[i]
                    for px in range(px0, px1):
                        dx = px + 0.5 - sx[i]
                        d2 = (dx * dx + dy * dy) * inv_r2
                        if d2 > _CUTOFF * _CUTOFF:
                            continue
                        s = w * math.exp(-d2)
                        a = kappa * s
                        if a > 0.0:
                            # expm1: a faint particle still dims what is behind
                            # it, and a million of them add up.
                            lost = -math.expm1(-a)
                            att = 1.0 - lost
                            f = lost / kappa
                            image[py, px, 0] = image[py, px, 0] * att + e0 * f
                            image[py, px, 1] = image[py, px, 1] * att + e1 * f
                            image[py, px, 2] = image[py, px, 2] * att + e2 * f
                        else:
                            image[py, px, 0] += e0 * s
                            image[py, px, 1] += e1 * s
                            image[py, px, 2] += e2 * s
        return image


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
        qcounts, qitems = _bin(qx, qy, qgrown, order, qw, qh, qtx, qty)
        grey = np.repeat(e.astype(np.float32)[:, None], 3, axis=1)
        column = _composite(qx, qy, qgrown, qweight, grey, np.zeros_like(grey),
                            qcounts, qitems, qw, qh, qtx, qty)[..., 0]
        lit = column[column > 0]
        ref = float(np.percentile(lit, 99.5)) if lit.size else 0.0
        kappa = float(absorption) / ref if ref > 0 else 0.0
    # Grey: the same in the three channels, so a thick region keeps the colour
    # of what it emits (its source function is colour / absorption).
    absorb = np.repeat((e * kappa)[:, None], 3, axis=1).astype(np.float32)
    tiles_x, tiles_y = -(-width // _TILE), -(-height // _TILE)
    counts, items = _bin(sx, sy, grown, order, width, height, tiles_x, tiles_y)
    return _composite(sx, sy, grown, weight, emission, absorb,
                      counts, items, width, height, tiles_x, tiles_y)


def smoothing_lengths(positions: np.ndarray, neighbours: int = 32) -> np.ndarray:
    """Smoothing lengths where a snapshot has none: half the distance to the
    ``neighbours``-th nearest particle, the support an SPH kernel would have
    with that many neighbours."""
    from scipy.spatial import cKDTree   # noqa: PLC0415
    pts = np.asarray(positions, dtype=np.float64)
    k = min(neighbours + 1, len(pts))
    if k < 2:
        return np.ones(len(pts), dtype=np.float32)
    # Only the k-th neighbour: the whole (N, k) answer is gigabytes for a
    # snapshot of tens of millions.
    dist, _ = cKDTree(pts).query(pts, k=[k], workers=-1)
    h = 0.5 * dist[:, 0]
    positive = h > 0
    floor = h[positive].min() if positive.any() else 1.0
    return np.where(positive, h, floor).astype(np.float32)


def velocity_dispersion(positions: np.ndarray, velocities: np.ndarray,
                        neighbours: int = 32) -> np.ndarray:
    """Each particle's local velocity dispersion: the one-dimensional spread
    of the velocities of its ``neighbours`` nearest particles (itself
    included), sqrt(<|v - <v>|²> / 3)."""
    from scipy.spatial import cKDTree   # noqa: PLC0415
    pts = np.asarray(positions, dtype=np.float64)
    vel = np.asarray(velocities, dtype=np.float32)
    k = min(neighbours + 1, len(pts))
    if k < 2:
        return np.zeros(len(pts), dtype=np.float32)
    tree = cKDTree(pts)
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
