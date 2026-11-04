"""Ray-traced volume rendering (issue #106): a still of a 3-D grid as a medium
that emits and absorbs light, graded afterwards, as the particles of #101 are.

A grid holds an emissivity ``e`` per voxel (light per unit length) and, for
the colour, an index ``c`` in [0, 1] into a colour table: brightness from one
quantity, colour from another (a two-field colour table). Each ray is marched
front to back through the grid, sampled trilinearly every ``step`` (half a
voxel by default); a grey absorption ``k = kappa e`` dims what lies behind, so
along the ray

    dI = T rgb(c) e dl,        dT = -T k dl,

integrated exactly over each step for a constant sample (``(1 - exp(-k dl)) /
k``, ``dl`` without absorption). The light is accumulated in floating point,
without a ceiling, and turned into a picture by the particles' tone curves
(app/particle_raytrace.py, ``tone_map``).

The grid's points span ``box`` (x0, x1, y0, y1, z0, z1) in world coordinates,
point-centred as a vtkImageData of origin ``x0`` whose extent fills the box:
the desktop's 3-D view places a cube (or its decimated preview) at voxel
coordinates, and a grid of any resolution laid over the same box draws the
same view through the same camera.

The projected map integrates the emissivity, taken as a density, and the
density-weighted mean of a second quantity along each line of sight, without
perspective and optionally within a slab around the focal plane: the
particles' projected map for a grid.
"""
from __future__ import annotations

import math

import numpy as np

from .numba_parallel import LOCK as PARALLEL
from .particle_raytrace import Camera, parallel_camera

try:   # numba is a backend dependency; the fallback only keeps imports working
    from numba import njit, prange
except ImportError:   # pragma: no cover
    njit = None
    prange = range

# Light below this transmittance is not worth marching on for.
_OPAQUE = 1e-4


def _basis(camera: Camera):
    pos = np.asarray(camera.position, dtype=np.float64)
    fp = np.asarray(camera.focal_point, dtype=np.float64)
    forward = fp - pos
    forward /= np.linalg.norm(forward) or 1.0
    up = np.asarray(camera.view_up, dtype=np.float64)
    right = np.cross(forward, up)
    right /= np.linalg.norm(right) or 1.0
    up = np.cross(right, forward)
    return pos, fp, forward, right, up


def _rays(camera: Camera, width: int, height: int):
    """The rays' frame: origin, forward, right, up, and pixels per world unit
    (parallel) or the focal length in pixels (perspective)."""
    pos, _fp, forward, right, up = _basis(camera)
    if camera.parallel:
        scale = height / (2.0 * max(camera.parallel_scale, 1e-30))
    else:
        scale = (height / 2.0) / math.tan(math.radians(camera.view_angle) / 2.0)
    return pos, forward, right, up, scale


def default_step(shape: tuple[int, int, int], box) -> float:
    """Half the smallest voxel side, in world units."""
    nz, ny, nx = shape
    sides = [(box[1] - box[0]) / max(nx - 1, 1), (box[3] - box[2]) / max(ny - 1, 1),
             (box[5] - box[4]) / max(nz - 1, 1)]
    positive = [s for s in sides if s > 0]
    return 0.5 * (min(positive) if positive else 1.0)


if njit is not None:
    @njit(cache=True, nogil=True, fastmath=True)
    def _sample(grid, gx, gy, gz):
        nz, ny, nx = grid.shape
        if gx < 0.0 or gy < 0.0 or gz < 0.0 or gx > nx - 1 or gy > ny - 1 or gz > nz - 1:
            return 0.0
        x0 = min(int(gx), nx - 2) if nx > 1 else 0
        y0 = min(int(gy), ny - 2) if ny > 1 else 0
        z0 = min(int(gz), nz - 2) if nz > 1 else 0
        fx, fy, fz = gx - x0, gy - y0, gz - z0
        x1 = x0 + 1 if nx > 1 else 0
        y1 = y0 + 1 if ny > 1 else 0
        z1 = z0 + 1 if nz > 1 else 0
        c00 = grid[z0, y0, x0] * (1.0 - fx) + grid[z0, y0, x1] * fx
        c10 = grid[z0, y1, x0] * (1.0 - fx) + grid[z0, y1, x1] * fx
        c01 = grid[z1, y0, x0] * (1.0 - fx) + grid[z1, y0, x1] * fx
        c11 = grid[z1, y1, x0] * (1.0 - fx) + grid[z1, y1, x1] * fx
        return ((c00 * (1.0 - fy) + c10 * fy) * (1.0 - fz)
                + (c01 * (1.0 - fy) + c11 * fy) * fz)

    @njit(cache=True, nogil=True, fastmath=True)
    def _span(ox, oy, oz, dx, dy, dz, box):
        """The ray's parameter range inside the box (t0 > t1: it misses)."""
        t0, t1 = -1e300, 1e300
        o = (ox, oy, oz)
        d = (dx, dy, dz)
        for a in range(3):
            lo, hi = box[2 * a], box[2 * a + 1]
            if abs(d[a]) < 1e-30:
                if o[a] < lo or o[a] > hi:
                    return 1.0, 0.0
                continue
            ta, tb = (lo - o[a]) / d[a], (hi - o[a]) / d[a]
            if ta > tb:
                ta, tb = tb, ta
            t0, t1 = max(t0, ta), min(t1, tb)
        return t0, t1

    @njit(parallel=True, cache=True, nogil=True, fastmath=True)
    def _march(emission, colour, lut, kappa, box, origin, forward, right, up, scale,
               parallel, width, height, step, slab_lo, slab_hi, fp_depth, moments):
        """The light of each pixel, ``(height, width, 3)``. ``moments``: the
        first channel integrates ``e``, the second ``e c`` (the projected
        map's density and weighted sum), without absorption."""
        image = np.zeros((height, width, 3), dtype=np.float32)
        nz, ny, nx = emission.shape
        sx = (nx - 1) / (box[1] - box[0]) if box[1] > box[0] else 0.0
        sy = (ny - 1) / (box[3] - box[2]) if box[3] > box[2] else 0.0
        sz = (nz - 1) / (box[5] - box[4]) if box[5] > box[4] else 0.0
        n_lut = lut.shape[0]
        for py in prange(height):
            for px in range(width):
                u = px + 0.5 - width / 2.0
                v = height / 2.0 - (py + 0.5)
                if parallel:
                    ox = origin[0] + (u * right[0] + v * up[0]) / scale
                    oy = origin[1] + (u * right[1] + v * up[1]) / scale
                    oz = origin[2] + (u * right[2] + v * up[2]) / scale
                    dx, dy, dz = forward[0], forward[1], forward[2]
                else:
                    ox, oy, oz = origin[0], origin[1], origin[2]
                    dx = forward[0] + (u * right[0] + v * up[0]) / scale
                    dy = forward[1] + (u * right[1] + v * up[1]) / scale
                    dz = forward[2] + (u * right[2] + v * up[2]) / scale
                    norm = math.sqrt(dx * dx + dy * dy + dz * dz)
                    dx, dy, dz = dx / norm, dy / norm, dz / norm
                t0, t1 = _span(ox, oy, oz, dx, dy, dz, box)
                t0 = max(t0, 0.0)
                # The slab, along the line of sight: depth = t * cos where cos
                # is the ray's component along forward.
                cos = dx * forward[0] + dy * forward[1] + dz * forward[2]
                if slab_hi > slab_lo and cos > 1e-12:
                    t0 = max(t0, (fp_depth + slab_lo) / cos)
                    t1 = min(t1, (fp_depth + slab_hi) / cos)
                if not t1 > t0:
                    continue
                # Each step's sample at an offset of its own per pixel, so the
                # steps do not band; each sample stands for its whole step. A
                # map is a measurement: its samples at the steps' middles,
                # exact for a field linear along the ray.
                jitter = 0.5 if moments else (
                    (((px * 73856093) ^ (py * 19349663)) & 1023) / 1024.0)
                seg = t0
                r, g, b, trans = 0.0, 0.0, 0.0, 1.0
                while seg < t1:
                    dl = min(step, t1 - seg)
                    t = seg + jitter * dl
                    seg += step
                    wx, wy, wz = ox + t * dx, oy + t * dy, oz + t * dz
                    gx = (wx - box[0]) * sx
                    gy = (wy - box[2]) * sy
                    gz = (wz - box[4]) * sz
                    e = _sample(emission, gx, gy, gz)
                    if e > 0.0:
                        c = _sample(colour, gx, gy, gz)
                        if moments:
                            r += e * dl
                            g += e * c * dl
                        else:
                            k = kappa * e
                            if k > 0.0:
                                lost = -math.expm1(-k * dl)
                                f = trans * e * lost / k
                                trans *= 1.0 - lost
                            else:
                                f = trans * e * dl
                            i = int(c * (n_lut - 1) + 0.5)
                            i = 0 if i < 0 else (n_lut - 1 if i >= n_lut else i)
                            r += f * lut[i, 0]
                            g += f * lut[i, 1]
                            b += f * lut[i, 2]
                            if trans < _OPAQUE:
                                break
                image[py, px, 0] = r
                image[py, px, 1] = g
                image[py, px, 2] = b
        return image


def _call(emission, colour, lut, kappa, box, camera, width, height, step, slab, moments):
    if njit is None:   # pragma: no cover
        raise RuntimeError("Ray-traced volume rendering needs numba.")
    emission = np.ascontiguousarray(emission, dtype=np.float32)
    if colour is None:
        # One colour: any grid of the right shape indexes a one-entry table.
        colour = emission
        lut = np.asarray(lut, dtype=np.float32)[:1]
    colour = np.ascontiguousarray(colour, dtype=np.float32)
    if colour.shape != emission.shape:
        raise ValueError(f"The colour grid {colour.shape} is not the emission's "
                         f"{emission.shape}.")
    box = np.asarray(box, dtype=np.float64)
    if box.shape != (6,):
        raise ValueError("box: x0, x1, y0, y1, z0, z1.")
    origin, forward, right, up, scale = _rays(camera, width, height)
    step = float(step) if step else default_step(emission.shape, box)
    slab_lo = slab_hi = 0.0
    fp_depth = 0.0
    if slab:
        fp_depth = float((np.asarray(camera.focal_point, dtype=np.float64) - origin) @ forward)
        slab_lo, slab_hi = -slab / 2.0, slab / 2.0
    lut = np.ascontiguousarray(lut, dtype=np.float32)
    with PARALLEL:   # one parallel kernel at a time (app/numba_parallel.py)
        return _march(emission, colour, lut, float(kappa), box, origin, forward, right, up,
                      float(scale), bool(camera.parallel), int(width), int(height), step,
                      slab_lo, slab_hi, fp_depth, bool(moments))


def render(emission: np.ndarray, colour: np.ndarray | None, lut: np.ndarray, camera: Camera,
           width: int, height: int, *, box, absorption: float = 0.0,
           step: float | None = None) -> np.ndarray:
    """The light reaching each pixel, ``(height, width, 3)`` float32.

    ``emission``: (nz, ny, nx) light per unit length, >= 0; ``colour``: the
    same shape, indices in [0, 1] into ``lut`` ((K, 3) RGB; None: its first
    entry, a one-colour table); ``absorption``: the optical depth reached
    where the emitted light is brightest (0: emission only), measured on an
    emission-only pass at a quarter of the size, as for particles."""
    kappa = 0.0
    if absorption > 0:
        qw, qh = max(16, width // 4), max(16, height // 4)
        grey = np.ones((1, 3), dtype=np.float32)
        column = _call(emission, None, grey, 0.0, box, camera, qw, qh, step, None, False)[..., 0]
        lit = column[column > 0]
        ref = float(np.percentile(lit, 99.5)) if lit.size else 0.0
        kappa = float(absorption) / ref if ref > 0 else 0.0
    return _call(emission, colour, lut, kappa, box, camera, width, height, step, None, False)


def projected_maps(density: np.ndarray, value: np.ndarray | None, camera: Camera,
                   width: int, height: int, *, box, slab: float | None = None,
                   step: float | None = None) -> np.ndarray:
    """``(height, width, 2)``: the column of ``density`` along each line of
    sight, without perspective (within ``slab`` of the focal plane), and the
    density-weighted mean of ``value`` (NaN where nothing lies); as
    particle_raytrace.projected_maps for particles."""
    cam = parallel_camera(camera)
    v = None if value is None else np.nan_to_num(np.asarray(value, dtype=np.float32))
    light = _call(density, v, np.ones((1, 3), dtype=np.float32), 0.0, box, cam, width, height,
                  step, slab, True)
    column = light[..., 0]
    with np.errstate(divide="ignore", invalid="ignore"):
        mean = np.where(column > 0, light[..., 1] / column, np.nan)
    return np.stack([column, mean], axis=-1).astype(np.float32)


def emission_from(field: np.ndarray, *, log10: bool = False, power: float = 1.0,
                  floor: float | None = None) -> np.ndarray:
    """An emissivity from a quantity: ``10**field`` when it holds a logarithm,
    raised to ``power`` (the particles' contrast), blanks and values at or
    below ``floor`` (default 0) dark; scaled so its 99.5th percentile over
    the lit voxels is 1, so the tone's automatic exposure starts sensibly
    whatever the units."""
    f = np.asarray(field, dtype=np.float32)
    if log10:
        # Scaled in the logarithm: 10 to a log luminosity (44 for erg/s)
        # does not fit a float, 10 to its distance from the reference does.
        logs = f[np.isfinite(f)]
        stride = max(1, logs.size // 2_000_000)
        ref = float(np.percentile(logs[::stride], 99.5)) if logs.size else 0.0
        with np.errstate(over="ignore", invalid="ignore"):
            e = np.power(np.float32(10.0), f - np.float32(ref), dtype=np.float32)
        # The floor on the logarithms themselves: compared after the power,
        # rounding lit some at it, and 10 to a far floor overflowed.
        keep = np.isfinite(e) & (e > 0)
        if floor is not None:
            keep &= f > np.float32(floor)
        e = np.where(keep, e, 0.0).astype(np.float32, copy=False)
        if power != 1.0:
            np.power(e, np.float32(power), out=e)
        return e
    # Light is never negative: a floor below 0 still darkens what is.
    e = np.where(np.isfinite(f) & (f > max(floor or 0.0, 0.0)), f, 0.0)
    e = e.astype(np.float32, copy=False)
    lit = e[e > 0]
    if lit.size:
        # A sample: the percentile of a 512³ grid in full is slow.
        stride = max(1, lit.size // 2_000_000)
        ref = float(np.percentile(lit[::stride], 99.5))
        if ref > 0:
            e /= np.float32(ref)
    if power != 1.0:
        np.power(e, np.float32(power), out=e)
    return e


def colour_index(field: np.ndarray, *, lo: float | None = None, hi: float | None = None,
                 log: bool = False, lit: np.ndarray | None = None
                 ) -> tuple[np.ndarray, float, float]:
    """Indices in [0, 1] into a colour table, from a quantity over [lo, hi];
    with the range used. An open end: the quantity's 1st or 99.9th
    percentile where ``lit`` (a mask: the voxels that emit; None: all) and
    the quantity is finite (positive, on a log scale), so the table spans
    the values that are seen, not an empty background's."""
    f = np.asarray(field, dtype=np.float32)
    seen = np.isfinite(f) if lit is None else (np.isfinite(f) & lit)
    values = f[seen]
    if log:
        values = values[values > 0]
    stride = max(1, values.size // 2_000_000)
    sample = values[::stride]
    if sample.size:
        dlo, dhi = (float(v) for v in np.percentile(sample, [1.0, 99.9]))
    else:
        dlo, dhi = 0.0, 1.0
    lo = dlo if lo is None else float(lo)
    hi = dhi if hi is None else float(hi)
    if log and not lo > 0:
        lo = dlo if dlo > 0 else 1e-30
    if not hi > lo:
        hi = lo + (abs(lo) or 1.0)
    if log:
        with np.errstate(divide="ignore", invalid="ignore"):
            idx = (np.log10(np.maximum(f, lo)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))
    else:
        idx = (f - lo) / (hi - lo)
    idx = np.clip(np.nan_to_num(idx, nan=0.0), 0.0, 1.0).astype(np.float32)
    return idx, lo, hi
