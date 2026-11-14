"""Additive splats drawn by the VR page's rule (issue #262), for the point
session's refined stereo frames (render_session/_stereo_points.py).

While the view moves, the VR page draws a sample of a point table itself
(static/vr/js/points.js); when it holds still the backend draws every row and
the page blends that frame in. With splats the backend's frame has to look
like the page's: the same splat size, brightness and colours. VTK's Gaussian
mapper could not give that. From VTK 9.4 it projects each splat with the
camera's view-to-clip matrix divided by the view-space depth (EWA splatting),
which assumes the view is in the projection's units: the stereo camera has a
view in table units and a projection that scales them to metres, so every
splat came out smaller by that scale (1/20 for a box 20 units wide, 1/50 for a
50 Mpc snapshot: almost nothing drawn, the frame nearly black). Older VTK (the
POWER9 EGL image's 9.3.1) sizes them otherwise, and none of them has the
page's floor for sub-pixel splats. So the stereo splats are drawn here, on the
CPU, by the page's own shader:

- a splat is a square point sprite ``clamp(px, 1, MAX_PX)`` page pixels wide,
  ``px = 6 r s P11 H / 2 / depth`` (r the splat radius in table units, s the
  table-to-metre scale, P11 H / 2 the projection's pixels per metre at 1 m,
  depth in metres): out to 3 sigma of a Gaussian of sigma r; a point is
  clipped by the near and far planes only (OpenGL ES 3.0 §2.17), so a sprite
  centred just outside the view still lights its edge;
- each covered pixel adds ``colour × exp(-d² / 2) × opacity × max(min(px, 1), 0.25)``,
  ``d`` = 6 × (pixel centre - sprite centre) / sprite width: sub-pixel splats
  keep their light instead of dropping out;
- the splat radius follows the page's sample (``splat_style``): the page sizes
  its splats for the rows it draws (radius × ∛(table rows / sampled rows)), and
  the backend, which draws every row, uses that radius and weights each row by
  sampled rows / rows drawn, so its frame shows what the preview shows in
  expectation, without the sample's noise.

The picture is drawn at the page's own eye size, over the page's background
and clamped as the page's framebuffer clamps it; a smaller frame (refinement
at 0.6×, an eye capped by the link) is that picture averaged over the page
pixels each frame pixel covers. The light is summed in floating point, where
the page rounds every addition to 8 bits: the two agree in expectation, not
pixel for pixel.
"""
from __future__ import annotations

import math

import numpy as np

try:   # numba is a backend dependency; the fallback only keeps imports working
    from numba import get_num_threads, njit, prange
except ImportError:   # pragma: no cover
    njit = None
    prange = range

from .numba_parallel import LOCK as PARALLEL

#: The page's largest point sprite (points.js PointCloud.maxPx, page pixels).
MAX_PX = 64.0
#: The page's floor on a sub-pixel splat's light (points.js FS, max(vSplat, 0.25)).
SUBPIXEL_FLOOR = 0.25
#: The threads' own images, at most: their memory (here 256 MB) and count.
_CHUNK_BYTES = 256 << 20
_MAX_CHUNKS = 8
_ROWS_PER_CHUNK = 65536


def splat_style(bounds, total_rows: int, sample_rows: int, size: float,
                intensity: float) -> tuple[float, float, float]:
    """``(radius, opacity, sample_radius)`` in table units, as the VR page's
    ``splatStyle`` (static/vr/js/points.js) works them out: spacing ∛(volume /
    total rows) of the box the points fill (a flat axis counted as a
    thousandth of the longest), radius = spacing × size / 8, opacity =
    min(0.9, 20 / D) × intensity / 100 in [0.05, 0.9] with D the longest side
    over the spacing, and the sample's radius larger by ∛(total / sample rows).
    It is :func:`app.render_session.points.gaussian_splat` (the desktop's
    rule) plus the sample's growth."""
    dims = [bounds[1] - bounds[0], bounds[3] - bounds[2], bounds[5] - bounds[4]]
    longest = max(max(dims), 1.0e-6)
    volume = math.prod(max(d, longest * 1.0e-3) for d in dims)
    spacing = math.cbrt(volume / max(1, int(total_rows)))
    radius = spacing * float(size) / 8.0
    depth = max(longest / spacing, 1.0)
    opacity = min(max(min(0.9, 20.0 / depth) * float(intensity) / 100.0, 0.05), 0.9)
    grow = math.cbrt(max(1.0, total_rows / max(1, int(sample_rows))))
    return radius, opacity, radius * grow


if njit is not None:
    @njit(parallel=True, cache=True, nogil=True)
    def _splat(xyz, codes, lut, solid, clip, eye_z, diameter, amp, max_px, out):
        """Adds every row's splat to ``out`` (chunks × height × width × 3,
        float32, bottom row first, the page's pixels): chunk c takes the c-th
        share of the rows into its own image, so no two threads add to one
        pixel. ``clip``: 4 × 4, the rows' local coordinates to clip space;
        ``eye_z``: 4 numbers, to the eye's z in metres; ``diameter``: the
        sprite width in pixels at 1 m; ``amp``: opacity × the row weight.
        ``codes`` index ``lut`` (n × 3) over 0..65535, or ``solid`` colours
        every row."""
        n_chunks, height, width = out.shape[0], out.shape[1], out.shape[2]
        n = xyz.shape[0]
        n_lut = lut.shape[0]
        step = (n + n_chunks - 1) // n_chunks
        reach = int(max_px) + 2
        for chunk in prange(n_chunks):
            img = out[chunk]
            fx = np.empty(reach, dtype=np.float64)
            for i in range(chunk * step, min(n, (chunk + 1) * step)):
                x = np.float64(xyz[i, 0])
                y = np.float64(xyz[i, 1])
                z = np.float64(xyz[i, 2])
                cw = clip[3, 0] * x + clip[3, 1] * y + clip[3, 2] * z + clip[3, 3]
                if not cw > 0.0:
                    continue
                cz = clip[2, 0] * x + clip[2, 1] * y + clip[2, 2] * z + clip[2, 3]
                # Points are clipped by the near and far planes alone.
                if cz < -cw or cz > cw:
                    continue
                cx = clip[0, 0] * x + clip[0, 1] * y + clip[0, 2] * z + clip[0, 3]
                cy = clip[1, 0] * x + clip[1, 1] * y + clip[1, 2] * z + clip[1, 3]
                wx = (cx / cw + 1.0) * 0.5 * width
                wy = (cy / cw + 1.0) * 0.5 * height
                ez = eye_z[0] * x + eye_z[1] * y + eye_z[2] * z + eye_z[3]
                px = diameter / max(1.0e-4, -ez)
                sprite = min(max(px, 1.0), max_px)
                half = 0.5 * sprite
                # The pixels whose centre lies in [w - half, w + half); a
                # centre far outside the view (its NDC beyond ±1e6) has none.
                if not (abs(wx) < 1.0e9 and abs(wy) < 1.0e9):
                    continue
                i0 = max(0, int(math.ceil(wx - half - 0.5)))
                i1 = min(width - 1, int(math.ceil(wx + half - 0.5)) - 1)
                j0 = max(0, int(math.ceil(wy - half - 0.5)))
                j1 = min(height - 1, int(math.ceil(wy + half - 0.5)) - 1)
                if i1 < i0 or j1 < j0:
                    continue
                light = amp * max(min(max(px, 0.0), 1.0), SUBPIXEL_FLOOR)
                if solid:
                    r, g, b = lut[0, 0], lut[0, 1], lut[0, 2]
                else:
                    c = (np.int64(codes[i]) * n_lut) >> 16
                    r, g, b = lut[c, 0], lut[c, 1], lut[c, 2]
                # exp(-(s² + t²) / 2) = exp(-s² / 2) exp(-t² / 2): one factor
                # per column and per row of the sprite.
                for ii in range(i0, i1 + 1):
                    s = 6.0 * (ii + 0.5 - wx) / sprite
                    fx[ii - i0] = math.exp(-0.5 * s * s)
                for jj in range(j0, j1 + 1):
                    t = 6.0 * (jj + 0.5 - wy) / sprite
                    vy = light * math.exp(-0.5 * t * t)
                    for ii in range(i0, i1 + 1):
                        v = vy * fx[ii - i0]
                        img[jj, ii, 0] += v * r
                        img[jj, ii, 1] += v * g
                        img[jj, ii, 2] += v * b


def splat_light(xyz: np.ndarray, codes: np.ndarray | None, lut: np.ndarray,
                local_to_eye: np.ndarray, projection: np.ndarray, width: int, height: int,
                radius_m: float, amp: float) -> np.ndarray:
    """One eye's splats at ``width × height`` (the page's eye size), float32
    light ``height × width × 3``, bottom row first, before the background.

    ``xyz``: the rows' local float32 coordinates; ``local_to_eye``: them to the
    eye's view space in metres (view @ model @ data_to_model @ local_to_table);
    ``projection``: the page's for that eye; ``radius_m``: the splat radius in
    metres (sample radius × table-to-metre scale); ``amp``: opacity × the row
    weight; ``codes`` index ``lut`` (n × 3, 0..1) over 0..65535, None for one
    colour (``lut[0]``)."""
    if njit is None:   # pragma: no cover
        raise RuntimeError("Splatting needs numba.")
    height, width = int(height), int(width)
    clip = np.ascontiguousarray(projection @ local_to_eye, dtype=np.float64)
    eye_z = np.ascontiguousarray(local_to_eye[2], dtype=np.float64)
    # The page: uRadius = 6 r (model) × model scale × P11 × H / 2.
    diameter = 6.0 * radius_m * float(projection[1, 1]) * height * 0.5
    lut = np.ascontiguousarray(lut, dtype=np.float32)
    solid = codes is None
    if solid:
        codes = np.zeros(1, dtype=np.uint16)
    # One image per thread (no two threads add to a pixel), summed after.
    chunks = max(1, min(get_num_threads(), _MAX_CHUNKS, len(xyz) // _ROWS_PER_CHUNK,
                        _CHUNK_BYTES // max(1, 12 * height * width)))
    out = np.zeros((chunks, height, width, 3), dtype=np.float32)
    with PARALLEL:   # one parallel kernel at a time (app/numba_parallel.py)
        _splat(np.ascontiguousarray(xyz, dtype=np.float32), np.ascontiguousarray(codes),
               lut, solid, clip, eye_z, float(diameter), float(amp), MAX_PX, out)
    return out[0] if chunks == 1 else out.sum(axis=0, dtype=np.float32)


def area_average(a: np.ndarray, n_out: int, axis: int) -> np.ndarray:
    """``a`` reduced to ``n_out`` samples along ``axis`` (n_out ≤ its length),
    each the mean of the input over its footprint, partial pixels weighted by
    their overlap: differences of the running integral (a cumulative sum,
    linear within a pixel) at the footprints' edges. Pillow's BOX filter
    weighs whole pixels instead, which at 0.6× lost a sixth of a thin
    line's light."""
    a = np.moveaxis(np.asarray(a, dtype=np.float64), axis, 0)
    n = a.shape[0]
    integral = np.concatenate([np.zeros((1,) + a.shape[1:]), np.cumsum(a, axis=0)])
    edges = np.arange(n_out + 1) * (n / n_out)
    whole = np.minimum(np.floor(edges).astype(np.int64), n - 1)
    frac = (edges - whole).reshape((-1,) + (1,) * (a.ndim - 1))
    at = integral[whole] + frac * a[whole]
    return np.moveaxis((at[1:] - at[:-1]) * (n_out / n), 0, axis)


def page_frame(light: np.ndarray, background, width: int, height: int) -> np.ndarray:
    """The page's picture of ``light`` (its eye size): added to
    ``background`` (0..1) and clamped, as the page's additive blend over its
    clear colour; then, for a smaller frame, averaged over the page pixels
    each frame pixel covers (:func:`area_average`). RGBX uint8
    ``height × width × 4``, the stereo frame's layout."""
    rgb = light + np.asarray(background, dtype=np.float32)
    np.clip(rgb, 0.0, 1.0, out=rgb)
    if rgb.shape[:2] != (height, width):
        # A channel at a time (the running integral is float64).
        rgb = np.stack([area_average(area_average(rgb[..., c], height, 0), width, 1)
                        for c in range(3)], axis=-1)
    out = np.empty((height, width, 4), dtype=np.uint8)
    out[..., :3] = np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8)
    out[..., 3] = 255
    return out


def splat_eye(xyz, codes, lut, local_to_eye, projection, width: int, height: int,
              radius_m: float, amp: float, background,
              page_size: tuple[int, int] | None = None) -> np.ndarray:
    """One eye's frame (RGBX uint8, bottom row first): the splats drawn at the
    page's eye size ``page_size`` (width, height; default, or when not larger,
    the frame's) by :func:`splat_light`, then :func:`page_frame`."""
    pw, ph = page_size if page_size else (width, height)
    pw, ph = int(round(pw)), int(round(ph))
    if pw < width or ph < height:      # never drawn below the frame's size
        pw, ph = width, height
    light = splat_light(xyz, codes, lut, local_to_eye, projection, pw, ph, radius_m, amp)
    return page_frame(light, background, width, height)
