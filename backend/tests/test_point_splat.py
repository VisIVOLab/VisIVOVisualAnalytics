"""Issue #262: the point session's refined stereo frame with splats looks like
the VR page's preview: same splat size, brightness and colours.

The preview is worked out here from the page's own code (static/vr/js/points.js:
``splatStyle``, the vertex shader's sprite size and the fragment shader's
Gaussian with its sub-pixel floor), independently of app/point_splat.py, over
an even sample of the table as the page draws it. The session draws every row
and must show the same light: before the fix VTK's Gaussian mapper drew each
splat smaller by the table-to-metre scale (1/50 here) and the frame was
almost the background alone."""
from __future__ import annotations

import asyncio
import io
import math

import numpy as np
import pytest

pytest.importorskip("vtk")
pytest.importorskip("numba")

from PIL import Image   # noqa: E402

from app.point_splat import splat_style   # noqa: E402
from app.render_session.points import gaussian_splat   # noqa: E402

PAGE_SOLID = (0.8, 0.85, 1.0)        # points.js FS: the colour without a column


def page_style(bounds, total_rows, sample_rows, size, intensity):
    """points.js splatStyle, line for line."""
    dims = [bounds[1] - bounds[0], bounds[3] - bounds[2], bounds[5] - bounds[4]]
    longest = max(*dims, 1e-6)
    volume = 1.0
    for d in dims:
        volume *= max(d, longest * 1e-3)
    spacing = (volume / max(1, total_rows)) ** (1 / 3)
    radius = spacing * size / 8
    depth = max(longest / spacing, 1)
    opacity = min(max(min(0.9, 20 / depth) * intensity / 100, 0.05), 0.9)
    grow = max(1, total_rows / max(1, sample_rows)) ** (1 / 3)
    return radius, opacity, radius * grow


def page_preview(xyz, bounds, total_rows, size, intensity, model, d2m, view, proj,
                 width, height, background, max_px=64.0):
    """The page's splats of the sample ``xyz`` (table coordinates): the
    vertex shader (sprite clamp(px, 1, maxPx), vSplat = clamp(px, 0, 1)), the
    fragment shader (exp(-d²/2) × opacity × max(vSplat, 0.25), d = 6 ×
    (gl_PointCoord - 0.5)) and additive blending over the clear colour,
    clamped. Points are clipped by the near and far planes only (OpenGL ES
    3.0 §2.17). Rows bottom-up, 0..1, before the 8-bit store."""
    _, opacity, radius = page_style(bounds, total_rows, len(xyz), size, intensity)
    placed = model @ d2m
    scale = np.cbrt(np.linalg.det(placed[:3, :3]))
    u_radius = 6 * radius * scale * proj[1, 1] * height * 0.5
    h4 = np.column_stack([xyz, np.ones(len(xyz))])
    eye = (view @ placed @ h4.T).T
    clip = (proj @ eye.T).T
    w = clip[:, 3]
    keep = (w > 0) & (np.abs(clip[:, 2]) <= w)
    eye, clip, w = eye[keep], clip[keep], w[keep]
    px = u_radius / np.maximum(1e-4, -eye[:, 2])
    sprite = np.clip(px, 1.0, max_px)
    amp = opacity * np.maximum(np.clip(px, 0.0, 1.0), 0.25)
    wx = (clip[:, 0] / w + 1) * 0.5 * width
    wy = (clip[:, 1] / w + 1) * 0.5 * height
    out = np.zeros((height, width, 3))
    reach = int(math.ceil(sprite.max() / 2)) + 1
    base_i, base_j = np.floor(wx).astype(int), np.floor(wy).astype(int)
    for dj in range(-reach, reach + 1):
        for di in range(-reach, reach + 1):
            i, j = base_i + di, base_j + dj
            cx, cy = i + 0.5, j + 0.5
            # A fragment for each pixel whose centre is in the sprite.
            inside = ((cx >= wx - sprite / 2) & (cx < wx + sprite / 2)
                      & (cy >= wy - sprite / 2) & (cy < wy + sprite / 2)
                      & (i >= 0) & (i < width) & (j >= 0) & (j < height))
            s = (cx - wx) / sprite * 6
            t = (cy - wy) / sprite * 6
            g = np.exp(-0.5 * (s * s + t * t)) * amp
            for c in range(3):
                np.add.at(out[:, :, c], (j[inside], i[inside]), g[inside] * PAGE_SOLID[c])
    return np.clip(out + np.asarray(background), 0, 1)


def _uniform_table(tmp_path, n, side):
    rng = np.random.default_rng(262)
    xyz = rng.uniform(0.0, side, (n, 3))
    xyz[:8] = [[x, y, z] for x in (0, side) for y in (0, side) for z in (0, side)]
    np.ascontiguousarray(xyz.T).astype("<f4").tofile(tmp_path / "u.bin")
    (tmp_path / "u.bin.head").write_text(f"float\n3\n{n}\nlittle\nX\nY\nZ\n")
    return str(tmp_path / "u.bin.head"), xyz.astype(np.float32).astype(np.float64)


def _pose(model, d2m, view, proj, width, height, background, **extra):
    return {"type": "xr_pose", "pose_id": 1, "model": model.T.ravel().tolist(),
            "views": [{"eye": e, "view": view.T.ravel().tolist(),
                       "projection": proj.T.ravel().tolist(), "width": width, "height": height}
                      for e in ("left", "right")],
            "depth": {"scale": 4}, "quality": 100, "background": list(background),
            "data_to_model": d2m.T.ravel().tolist(), **extra}


def _overlap_weights(n_in, n_out):
    """n_out × n_in: output pixel o's mean over [o, o + 1) × n_in / n_out,
    each input pixel weighted by its overlap (written out, not a cumsum)."""
    f = n_in / n_out
    m = np.zeros((n_out, n_in))
    for o in range(n_out):
        a, b = o * f, (o + 1) * f
        for i in range(int(math.floor(a)), min(n_in, int(math.ceil(b)))):
            m[o, i] = max(0.0, min(b, i + 1) - max(a, i)) / f
    return m


def test_area_average_weighs_partial_pixels():
    """At 0.6× a column of every fifth lit pixel keeps its light (Pillow's
    BOX filter halved part of it)."""
    from app.point_splat import area_average
    row = np.tile([0.0, 1.0, 0.0, 0.0, 0.0], 32)               # 160 pixels, mean 0.2
    out = area_average(row[None, :], 96, 1)[0]
    assert np.allclose(out, _overlap_weights(160, 96) @ row)
    assert out.mean() == pytest.approx(0.2)
    assert np.allclose(out[:3], [0.4, 0.2, 0.0])          # footprints of 5/3 pixels
    rng = np.random.default_rng(9)
    img = rng.uniform(0, 1, (37, 53))
    assert np.allclose(area_average(img, 23, 0), _overlap_weights(37, 23) @ img)


def _blocks(img, b=8):
    h, w = img.shape[0] // b * b, img.shape[1] // b * b
    return img[:h, :w].reshape(h // b, b, w // b, b).mean(axis=(1, 3))


@pytest.mark.parametrize("frame_scale, held", [(1.0, None), (0.5, None), (0.6, None),
                                               (1.0, 60_000)])
def test_refined_splats_match_the_pages_preview(tmp_path, monkeypatch, frame_scale, held):
    """A table 50 units wide (the snapshot of the issue is ~50 Mpc): the
    page draws a quarter of its rows at its sample's splat size; the session's
    frame (every row) has the preview's mean light within 10 % and lights the
    same parts of the view. At 0.5 and 0.6 the frame is smaller than the
    page's view (the page's refinement scale): the page's picture averaged
    over the page pixels each frame pixel covers. With
    ``held`` the session holds an even half of the table
    (VISIVO_POINT_RENDER_MAX_POINTS): the page's style still comes from the
    table's rows."""
    from tests.test_point_render import _RENDER_PROBE
    import subprocess
    import sys
    from pathlib import Path
    backend = Path(__file__).resolve().parent.parent
    if subprocess.run([sys.executable, "-c", _RENDER_PROBE], capture_output=True,
                      timeout=60, cwd=backend).returncode != 0:
        pytest.skip("no offscreen OpenGL here (no GPU or display)")
    from app.render_session._stereo import parse_frame
    from app.render_session._stereo_points import default_data_to_model
    from app.render_session import points
    from app.render_session.points import PointRenderSession
    if held:
        monkeypatch.setattr(points, "MAX_POINTS", held)

    n, side, size, intensity = 120_000, 50.0, 1, 35
    path, xyz = _uniform_table(tmp_path, n, side)
    sample = xyz[::4]                                 # the page's even sample
    bounds = [0.0, side] * 3
    d2m = default_data_to_model(bounds)
    model = np.eye(4)
    model[:3, :3] = np.array([[0.8, 0.0, 0.6], [0.0, 1.0, 0.0], [-0.6, 0.0, 0.8]])  # turned
    model[:3, 3] = [0.05, -0.02, -1.4]
    view = np.eye(4)
    proj = np.array([[1.2, 0, 0, 0], [0, 1.6, 0, 0], [0, 0, -1.002, -0.2002], [0, 0, -1, 0]])
    page_w, page_h = 256, 192
    bg = (0.05, 0.05, 0.08)
    preview = page_preview(sample, bounds, n, size, intensity, model, d2m, view, proj,
                           page_w, page_h, bg)
    w, h = round(page_w * frame_scale), round(page_h * frame_scale)
    s = PointRenderSession("s" * 32, path)
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(s.initialize())
        loop.run_until_complete(s.apply_command({"type": "set_point_style", "mode": "gaussian",
                                                 "size": size, "intensity": intensity}))
        loop.run_until_complete(s.apply_command({"type": "set_show_outline", "visible": False}))
        loop.run_until_complete(s.apply_command(
            _pose(model, d2m, view, proj, w, h, bg, sample_rows=len(sample),
                  viewport_size=[page_w, page_h])))
        header, jpeg, depth = parse_frame(loop.run_until_complete(s.render_frame()).to_bytes())
    finally:
        loop.run_until_complete(s.close())
        loop.close()
    assert header["points"] == (held or n) and depth is None
    frame = np.asarray(Image.open(io.BytesIO(jpeg)).convert("RGB"), dtype=float)[::-1, :w] / 255
    ref = preview
    if frame_scale == 0.5:
        ref = ref.reshape(h, 2, w, 2, 3).mean(axis=(1, 3))
    elif frame_scale != 1.0:     # the mean over a fractional footprint
        ry, rx = _overlap_weights(page_h, h), _overlap_weights(page_w, w)
        ref = np.einsum("yj,jic,xi->yxc", ry, ref, rx)
    light, ref_light = frame - np.asarray(bg), ref - np.asarray(bg)
    # The light: the preview's mean, per channel too (the colour).
    assert light.mean() == pytest.approx(ref_light.mean(), rel=0.10)
    for c in range(3):
        assert light[..., c].mean() == pytest.approx(ref_light[..., c].mean(), rel=0.12)
    # Where: the same blocks lit, and the light laid out alike.
    lb, rb = _blocks(light.mean(axis=2), 4), _blocks(ref_light.mean(axis=2), 4)
    lit, ref_lit = lb > 0.1 * rb.max(), rb > 0.1 * rb.max()
    assert (lit == ref_lit).mean() > 0.97
    assert np.corrcoef(lb.ravel(), rb.ravel())[0, 1] > 0.95


def test_the_pages_splat_style_is_the_desktops_plus_the_samples_growth():
    """splat_style mirrors points.js splatStyle; with every row sampled it is
    the desktop's gaussian_splat."""
    bounds = (0, 100, 0, 50, 0, 10)
    for total, sample in ((1000, 1000), (3_931_546, 1_000_000), (10, 1)):
        assert splat_style(bounds, total, sample, 3, 35) == pytest.approx(
            page_style(bounds, total, sample, 3, 35))
    r, op = gaussian_splat(bounds, 1000, 3, 35)
    assert splat_style(bounds, 1000, 1000, 3, 35) == pytest.approx((r, op, r))
    # The sample's radius is the radius for the sampled rows alone.
    assert splat_style(bounds, 8000, 1000, 2, 35)[2] == pytest.approx(
        gaussian_splat(bounds, 1000, 2, 35)[0])


def test_threads_images_add_up_to_one_threads(monkeypatch):
    """The rows split among threads, each into its own image, give the image
    one thread draws (no lost additions), coloured by their codes."""
    from app import point_splat
    rng = np.random.default_rng(3)
    xyz = rng.uniform(-0.5, 0.5, (20_000, 3)).astype(np.float32)
    codes = rng.integers(0, 65536, len(xyz)).astype(np.uint16)
    lut = rng.uniform(0, 1, (1024, 3)).astype(np.float32)
    to_eye = np.eye(4)
    to_eye[2, 3] = -1.5
    proj = np.array([[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, -1.002, -0.2002], [0, 0, -1, 0]])
    args = (xyz, codes, lut, to_eye, proj, 160, 120, 0.004, 0.05)
    monkeypatch.setattr(point_splat, "_ROWS_PER_CHUNK", 10 ** 9)
    one = point_splat.splat_light(*args)
    monkeypatch.setattr(point_splat, "_ROWS_PER_CHUNK", 1000)
    many = point_splat.splat_light(*args)
    assert one.sum() > 0
    assert np.allclose(one, many, rtol=1e-5, atol=1e-6)


def _scene(centre):
    xyz = np.array([[centre[0], centre[1], 0.0], [0.1, 0.2, 0.05], [-0.3, 0.1, -0.1]]
                   + [[0.2, -0.2, 0.0]] * 100, dtype=np.float32)     # 100 on one spot
    to_eye = np.eye(4)
    to_eye[2, 3] = -1.5
    proj = np.array([[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, -1.002, -0.2002], [0, 0, -1, 0]])
    return xyz, to_eye, proj


@pytest.mark.parametrize("centre", [(0.0, 0.0), (0.37, -0.21)])
def test_a_reduced_frame_is_the_pages_picture_box_filtered(centre):
    """A frame half the page's size is the page's picture (over its
    background, clamped) averaged over 2 x 2 pixels: a sub-pixel splat
    centred on a page pixel keeps its light (widening the sprite to a frame
    pixel made it 9.5 times fainter) and a saturated spot stays as bright as
    the page shows it (averaging before the clamp made it brighter)."""
    from app import point_splat
    xyz, to_eye, proj = _scene(centre)
    lut = np.array([[1.0, 0.5, 0.25]], dtype=np.float32)
    bg = (0.05, 0.05, 0.08)
    for radius in (0.0002, 0.003, 0.02):           # sub-pixel, a few pixels, wide
        light = point_splat.splat_light(xyz, None, lut, to_eye, proj, 128, 96, radius, 0.3)
        page = np.clip(light + np.asarray(bg, dtype=np.float32), 0, 1)
        expected = page.reshape(48, 2, 64, 2, 3).mean(axis=(1, 3)) * 255
        half = point_splat.splat_eye(xyz, None, lut, to_eye, proj, 64, 48, radius, 0.3, bg,
                                     page_size=(128, 96))
        assert light.max() > 1.0                   # the spot saturates on the page
        assert np.abs(half[..., :3].astype(float) - expected).max() <= 1.0
        # At the page's own size: the page's picture, pixel for pixel.
        full = point_splat.splat_eye(xyz, None, lut, to_eye, proj, 128, 96, radius, 0.3, bg)
        assert np.abs(full[..., :3].astype(float) - page * 255).max() <= 0.5 + 1e-3


def test_a_splat_centred_just_outside_the_view_lights_its_edge():
    """Points are clipped by the near and far planes only (OpenGL ES 3.0
    section 2.17): a wide sprite centred just beyond the left edge lights the
    first columns; one behind the eye or past the far plane nothing."""
    from app import point_splat
    to_eye = np.eye(4)
    proj = np.array([[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, -1.002, -0.2002], [0, 0, -1, 0]])
    lut = np.array([[1.0, 1.0, 1.0]], dtype=np.float32)
    # x / -z = -1.02: 1 % of the width beyond the left edge, a ~17 px sprite.
    edge = np.array([[-1.02, 0.0, -1.0]], dtype=np.float32)
    light = point_splat.splat_light(edge, None, lut, to_eye, proj, 128, 96, 0.03, 0.5)
    assert light[:, :3].sum() > 0 and light[:, 12:].sum() == 0
    for gone in ([[0.0, 0.0, 1.0]], [[0.0, 0.0, -2000.0]]):
        assert point_splat.splat_light(np.array(gone, dtype=np.float32), None, lut, to_eye,
                                       proj, 128, 96, 0.03, 0.5).sum() == 0


def test_a_bad_sample_rows_or_viewport_size_is_refused():
    from app.render_session._stereo_points import parse_splat_view
    assert parse_splat_view({}) == (None, None)
    assert parse_splat_view({"sample_rows": 1000, "viewport_size": [1600, 1000]}) == (
        1000, (1600, 1000))
    for bad in (0, -5, 2.5, True, "100"):
        with pytest.raises(ValueError, match="sample_rows"):
            parse_splat_view({"sample_rows": bad})
    for bad in (720, [0, 100], [100], [100, 5000], [100.5, 100], [True, 100], "100,100"):
        with pytest.raises(ValueError, match="viewport_size"):
            parse_splat_view({"viewport_size": bad})


def test_stereo_splats_take_the_mappers_colours(tmp_path):
    """The CPU splats colour a code as the session's VTK mappers do (the
    points of a refined frame, the desktop's view), with a named map and with
    the page's own colour function."""
    from app.render_session.points import PointRenderSession
    from tests.test_point_render import _coloured_table, _mapper_rgb
    s = PointRenderSession("c" * 32, _coloured_table(tmp_path, np.linspace(0.0, 1.0, 50)))
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(s.initialize())
        assert np.allclose(s._splat_lut, [PAGE_SOLID])       # no column: the page's solid
        for cmd in ({"type": "set_color", "field": "C", "lut": "Viridis"},
                    {"type": "set_color", "field": "C", "min": 0.0, "max": 1.0,
                     "points": [[0.0, 1.0, 0.0, 0.0], [1.0, 0.0, 0.0, 1.0]]}):
            loop.run_until_complete(s.apply_command(cmd))
            codes = np.array([0, 1000, 20000, 32768, 65535])
            lut = s._splat_lut
            assert np.allclose(lut[(codes * len(lut)) >> 16], _mapper_rgb(s, codes), atol=1e-6)
    finally:
        loop.run_until_complete(s.close())
        loop.close()
