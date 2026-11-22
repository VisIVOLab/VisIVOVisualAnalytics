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


def page_style(bounds, total_rows, sample_rows, size, intensity, rule="v1"):
    """points.js splatStyle and ruleOpacity, line for line."""
    dims = [bounds[1] - bounds[0], bounds[3] - bounds[2], bounds[5] - bounds[4]]
    longest = max(*dims, 1e-6)
    volume = 1.0
    for d in dims:
        volume *= max(d, longest * 1e-3)
    spacing = (volume / max(1, total_rows)) ** (1 / 3)
    radius = spacing * size / 8
    depth = max(longest / spacing, 1)
    steps = math.floor(1.5 / depth * intensity / 35 * 255 + 0.5)
    opacity = min(max(steps, 8), 64) / 255
    if rule == "v2-f32":
        opacity /= size * size
    elif rule == "v2-f16":
        opacity = max(opacity / (size * size), 1 / 512)
    grow = max(1, total_rows / max(1, sample_rows)) ** (1 / 3)
    return radius, opacity, radius * grow


def page_preview(xyz, bounds, total_rows, size, intensity, model, d2m, view, proj,
                 width, height, background, max_px=256.0, depth=None, rule="v1"):
    """The page's splats of the sample ``xyz`` (table coordinates): the
    vertex shader (sprite clamp(px, 1, maxPx), vSplat = clamp(px, 0, 1)), the
    fragment shader (exp(-d²/2) × opacity × max(vSplat, 0.25), d = 6 ×
    (gl_PointCoord - 0.5)) and additive blending over the clear colour,
    clamped. Points are clipped by the near and far planes only (OpenGL ES
    3.0 §2.17). The splat ``rule`` (points.js SPLAT_RULES) sets the
    opacity and, in a float target (v2-f16, v2-f32), the light accumulated in
    floating point and tone-mapped (points.js toneMap, written out here)
    before the clear colour is added. Rows bottom-up, 0..1, before the 8-bit store. ``depth``, a
    dict: given its ``zsum`` and ``lsum``, each pixel's light (summed over
    the channels) times the splat's eye distance, and that light."""
    _, opacity, radius = page_style(bounds, total_rows, len(xyz), size, intensity, rule)
    tone = rule in ("v2-f16", "v2-f32")
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
    zsum, lsum = np.zeros((height, width)), np.zeros((height, width))
    dist = -eye[:, 2]
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
            if depth is not None:
                gl = g[inside] * sum(PAGE_SOLID)
                np.add.at(lsum, (j[inside], i[inside]), gl)
                np.add.at(zsum, (j[inside], i[inside]), gl * dist[inside])
    if depth is not None:
        depth.update(zsum=zsum, lsum=lsum)
    if tone:
        y = out @ np.array([0.2126, 0.7152, 0.0722])
        f = np.ones_like(y)
        lit = y > 0
        f[lit] = (1 - np.exp(-y[lit])) / y[lit]
        out = out * f[..., None]
        out /= np.maximum(out.max(axis=-1, keepdims=True), 1.0)
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


@pytest.mark.parametrize("renderer, rule", [("splat", "v1"), ("raytrace", "v1"),
                                            ("splat", "v2-f32"), ("raytrace", "v2-f32"),
                                            ("raytrace", "v2-f16"), ("splat", "v2-8bit")])
@pytest.mark.parametrize("frame_scale, held", [(1.0, None), (0.5, None), (0.6, None),
                                               (1.0, 60_000)])
def test_refined_splats_match_the_pages_preview(tmp_path, monkeypatch, frame_scale, held,
                                                renderer, rule):
    """A table 50 units wide (the snapshot of the issue is ~50 Mpc): the
    page draws a quarter of its rows at its sample's splat size; the session's
    frame (every row) has the preview's mean light within 10 % and lights the
    same parts of the view. At 0.5 and 0.6 the frame is smaller than the
    page's view (the page's refinement scale): the page's picture averaged
    over the page pixels each frame pixel covers. With
    ``held`` the session holds an even half of the table
    (VISIVO_POINT_RENDER_MAX_POINTS): the page's style still comes from the
    table's rows. Ray-traced (issue #264): the same light, and a depth
    within the box's distances from the eye. Each splat rule of the page
    (issue #263; size 2 for the float targets, where it divides the
    opacity) is the session's, and the frame says which it drew by."""
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

    n, side, intensity = 120_000, 50.0, 35
    size = 2 if rule in ("v2-f16", "v2-f32") else 1
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
    page_depth = {}
    preview = page_preview(sample, bounds, n, size, intensity, model, d2m, view, proj,
                           page_w, page_h, bg, depth=page_depth, rule=rule)
    w, h = round(page_w * frame_scale), round(page_h * frame_scale)
    s = PointRenderSession("s" * 32, path)
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(s.initialize())
        loop.run_until_complete(s.apply_command({"type": "set_point_style", "mode": "gaussian",
                                                 "size": size, "intensity": intensity,
                                                 "stereo_renderer": renderer,
                                                 "splat_rule": rule}))
        loop.run_until_complete(s.apply_command({"type": "set_show_outline", "visible": False}))
        loop.run_until_complete(s.apply_command(
            _pose(model, d2m, view, proj, w, h, bg, sample_rows=len(sample),
                  viewport_size=[page_w, page_h])))
        header, jpeg, depth = parse_frame(loop.run_until_complete(s.render_frame()).to_bytes())
    finally:
        loop.run_until_complete(s.close())
        loop.close()
    assert header["points"] == (held or n) and header["renderer"] == renderer
    assert header["splat_rule"] == rule
    if renderer == "splat":
        assert depth is None and header["depth"] is None
    else:
        from app.render_session._stereo import decode_depth
        d = header["depth"]
        assert d["source"] == "medium" and d["width"] == 2 * -(-w // 4)
        dist = decode_depth(depth, d["width"], d["height"], d["z_near"], d["z_far"])
        # The box's corners lie between these distances from the eye.
        corners = np.array([[x, y, z, 1.0] for x in (0, side) for y in (0, side)
                            for z in (0, side)])
        ahead = -(view @ model @ d2m @ corners.T)[2]
        lit = dist[np.isfinite(dist)]
        assert lit.size > 0.3 * dist.size
        assert ahead.min() - 0.01 <= lit.min() and lit.max() <= ahead.max() + 0.01
        if frame_scale == 1.0:
            # Each block's light-weighted mean distance of the page's splats
            # (a quarter of the rows: its noise is the spread, ~2 cm) where they
            # light it well: unbiased.
            zb, lb = _blocks(page_depth["zsum"], 4), _blocks(page_depth["lsum"], 4)
            ours = dist[:, :w // 4][::-1]               # left eye, bottom row first
            well = lb > 0.05 * lb.max()
            assert well.mean() > 0.3 and np.isfinite(ours[well]).all()
            diff = ours[well] - zb[well] / lb[well]
            assert abs(np.median(diff)) < 0.005 and np.abs(diff).mean() < 0.04
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
    the desktop's gaussian_splat, its opacity over size² (issue #263: the
    same at size 1, the default)."""
    bounds = (0, 100, 0, 50, 0, 10)
    for total, sample in ((1000, 1000), (3_931_546, 1_000_000), (10, 1)):
        for rule in ("v1", "v2-8bit", "v2-f16", "v2-f32"):
            assert splat_style(bounds, total, sample, 3, 35, rule) == pytest.approx(
                page_style(bounds, total, sample, 3, 35, rule))
    r, op = gaussian_splat(bounds, 1000, 3, 35)
    assert splat_style(bounds, 1000, 1000, 3, 35) == pytest.approx((r, op, r))
    assert splat_style(bounds, 1000, 1000, 3, 35, "v2-f32") == pytest.approx((r, op / 9, r))
    r, op = gaussian_splat(bounds, 1000, 1, 35)
    assert splat_style(bounds, 1000, 1000, 1, 35) == pytest.approx((r, op, r))
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


# ── Ray-traced splats (issue #264) ───────────────────────────────────────────

@pytest.mark.parametrize("radius", [0.0002, 0.001, 0.003, 0.02, 0.2])
def test_ray_traced_splats_carry_the_pages_light(radius):
    """The ray tracer's kernels carry each splat's light as the page's sprite
    does in expectation: sub-pixel (the 0.25 floor), a few pixels, wide, and
    capped at 64 px (0.2: the sprite would be wider). Spread over random
    positions the totals agree within 1.5 %, and where the light lies too."""
    from app import point_splat
    rng = np.random.default_rng(int(radius * 1e5))
    n = 40_000    # a sub-pixel sprite lights one pixel by where it falls: ~140 % spread each
    xyz = np.column_stack([rng.uniform(-0.4, 0.4, n), rng.uniform(-0.3, 0.3, n),
                           rng.uniform(-0.3, 0.3, n)]).astype(np.float32)
    codes = rng.integers(0, 65536, len(xyz)).astype(np.uint16)
    lut = rng.uniform(0.1, 1, (256, 3)).astype(np.float32)
    to_eye = np.eye(4)
    to_eye[2, 3] = -1.5
    proj = np.array([[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, -1.002, -0.2002], [0, 0, -1, 0]])
    page = point_splat.splat_light(xyz, codes, lut, to_eye, proj, 320, 240, radius, 0.001)
    light, zsum = point_splat.raytrace_light(xyz, codes, lut, to_eye, proj, 320, 240, radius, 0.001)
    assert light.shape == page.shape and zsum.shape == page.shape[:2]
    for c in range(3):
        assert light[..., c].sum() == pytest.approx(page[..., c].sum(), rel=0.015)
    lb, pb = _blocks(light.sum(axis=2), 16), _blocks(page.sum(axis=2), 16)
    assert np.corrcoef(lb.ravel(), pb.ravel())[0, 1] > 0.98
    # Each pixel's depth lies within the rows' distances.
    lit = light.sum(axis=2) > 1e-6
    d = zsum[lit] / light.sum(axis=2)[lit]
    assert d.min() >= 1.5 - 0.3 - 1e-3 and d.max() <= 1.5 + 0.3 + 1e-3


def test_ray_traced_solid_colour_and_nothing_in_view():
    """Without a colour column every row takes lut[0]; rows behind the eye
    or past the far plane give no light and no depth."""
    from app import point_splat
    to_eye = np.eye(4)
    proj = np.array([[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, -1.002, -0.2002], [0, 0, -1, 0]])
    lut = np.array([[1.0, 0.5, 0.25]], dtype=np.float32)
    xyz = np.array([[0.0, 0.0, -1.0]], dtype=np.float32)
    light, _ = point_splat.raytrace_light(xyz, None, lut, to_eye, proj, 64, 48, 0.01, 0.4)
    assert light.sum() > 0
    assert np.allclose(light[..., 1].sum() / light[..., 0].sum(), 0.5, rtol=1e-4)
    for gone in ([[0.0, 0.0, 1.0]], [[0.0, 0.0, -2000.0]]):
        light, zsum = point_splat.raytrace_light(np.array(gone, dtype=np.float32), None, lut,
                                                 to_eye, proj, 64, 48, 0.01, 0.4)
        assert light.sum() == 0 and zsum.sum() == 0


def test_block_depth_is_the_light_weighted_mean():
    """Two lights in a block: the mean of their depths weighted by their
    light; a block without light has no depth (+inf); a page larger than the
    frame is first averaged to the frame's size, which keeps the mean."""
    from app.point_splat import block_depth
    lsum = np.zeros((8, 8))
    zsum = np.zeros((8, 8))
    lsum[0, 0], zsum[0, 0] = 3.0, 3.0 * 1.0            # 3 units of light at 1 m
    lsum[1, 1], zsum[1, 1] = 1.0, 1.0 * 2.0            # 1 unit at 2 m
    out = block_depth(zsum, lsum, 8, 8, 4)
    assert out.shape == (2, 2)
    assert out[0, 0] == pytest.approx(1.25)
    assert np.isinf(out[0, 1]) and np.isinf(out[1, 0]) and np.isinf(out[1, 1])
    # Frame 6 × 6 of a page 12 × 12 (2 × 2 page pixels each), blocks of 4:
    # 2 × 2 blocks, the last one partial.
    page_l, page_z = np.zeros((12, 12)), np.zeros((12, 12))
    page_l[:, :], page_z[:, :] = 1.0, 1.5
    page_l[11, 11], page_z[11, 11] = 3.0, 3.0 * 3.0
    out = block_depth(page_z, page_l, 6, 6, 4)
    assert out.shape == (2, 2)
    assert out[0, 0] == pytest.approx(1.5)
    assert out[1, 1] == pytest.approx((15 * 1.5 + 3 * 3.0) / 18)


def test_parallel_binning_keeps_the_order():
    """The particles of each tile in the order given, whatever the chunks:
    the back-to-front order an absorbing render needs."""
    from app.particle_raytrace import _CUTOFF, _bin
    rng = np.random.default_rng(5)
    n = 300_000
    sx = rng.uniform(-10, 330, n).astype(np.float32)
    sy = rng.uniform(-10, 250, n).astype(np.float32)
    sr = rng.uniform(0.5, 20, n).astype(np.float32)
    order = rng.permutation(n).astype(np.int64)
    counts, items = _bin(sx, sy, sr, order, _CUTOFF, 320, 240, 10, 8)
    rank = np.empty(n, dtype=np.int64)
    rank[order] = np.arange(n)
    total = 0
    for t in range(80):
        run = items[counts[t]:counts[t + 1]]
        assert (np.diff(rank[run]) > 0).all()
        tx, ty = t % 10, t // 10
        reach = _CUTOFF * sr[run]
        assert ((sx[run] + reach) // 32 >= tx).all() and ((sx[run] - reach) // 32 <= tx).all()
        assert ((sy[run] + reach) // 32 >= ty).all() and ((sy[run] - reach) // 32 <= ty).all()
        total += len(run)
    assert total == counts[-1] == len(items)


def test_an_unknown_stereo_renderer_is_refused_whole(tmp_path):
    """A style with an unknown stereo renderer changes nothing (the size and
    mode of the same command are not taken either)."""
    from app.render_session.points import PointRenderSession
    from tests.test_point_render import _coloured_table
    s = PointRenderSession("r" * 32, _coloured_table(tmp_path, np.linspace(0.0, 1.0, 50)))
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(s.initialize())
        assert s.view.stereo_renderer == "splat"          # the default (#262's)
        loop.run_until_complete(s.apply_command({"type": "set_point_style", "mode": "gaussian",
                                                 "size": 2, "stereo_renderer": "raytrace"}))
        assert (s.view.mode, s.view.size, s.view.stereo_renderer) == ("gaussian", 2, "raytrace")
        loop.run_until_complete(s.apply_command({"type": "set_point_style", "mode": "points",
                                                 "size": 5, "stereo_renderer": "pathtrace"}))
        assert (s.view.mode, s.view.size, s.view.stereo_renderer) == ("gaussian", 2, "raytrace")
        # Omitted: kept.
        loop.run_until_complete(s.apply_command({"type": "set_point_style", "mode": "gaussian",
                                                 "size": 3}))
        assert s.view.stereo_renderer == "raytrace"
    finally:
        loop.run_until_complete(s.close())
        loop.close()


def test_the_size_step_changes_the_size_not_the_brightness():
    """Issue #263: splats a few pixels wide and up carry the same light at
    sizes 1, 2 and 4 (it grew as size² before)."""
    from app import point_splat
    rng = np.random.default_rng(263)
    xyz = rng.uniform(-0.3, 0.3, (3000, 3)).astype(np.float32)
    to_eye = np.eye(4)
    to_eye[2, 3] = -1.5
    proj = np.array([[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, -1.002, -0.2002], [0, 0, -1, 0]])
    lut = np.array([[1.0, 1.0, 1.0]], dtype=np.float32)
    bounds = (-0.3, 0.3, -0.3, 0.3, -0.3, 0.3)
    totals = []
    for size in (1, 2, 4):
        radius, opacity, _ = point_splat.splat_style(bounds, 2000, 2000, size, 35, "v2-f32")
        totals.append(point_splat.splat_light(xyz, None, lut, to_eye, proj, 640, 480,
                                              radius, opacity).sum())
    assert totals[1] == pytest.approx(totals[0], rel=0.03)
    assert totals[2] == pytest.approx(totals[0], rel=0.03)


def test_the_tone_curve_is_the_pages():
    """tone_map: linear for faint light, the hue kept, never above 1; the
    numbers of points.js toneMap (tests/vr/layers.test.mjs checks the same)."""
    from app.point_splat import page_frame, tone_map
    light = np.array([[[1e-4, 2e-4, 3e-4], [0.5, 0.25, 0.1], [40.0, 20.0, 8.0], [0, 0, 0]]],
                     dtype=np.float32)
    out = tone_map(light)
    assert np.allclose(out[0, 0], light[0, 0], rtol=1e-3)            # faint: as is
    y = 0.2126 * 0.5 + 0.7152 * 0.25 + 0.0722 * 0.1
    assert np.allclose(out[0, 1], light[0, 1] * (1 - np.exp(-y)) / y, rtol=1e-5)
    assert out[0, 2].max() == pytest.approx(1.0) and np.allclose(out[0, 2] / out[0, 2, 0],
                                                                 [1.0, 0.5, 0.2])
    assert (out[0, 3] == 0).all()
    with pytest.raises(ValueError, match="tone"):
        page_frame(light, (0, 0, 0), 4, 1, "filmic")


def test_splats_take_their_opacity_without_a_colour_column(tmp_path):
    """Issue #263: the point session's VTK splats without a colour column
    follow the intensity (they kept the same picture at every intensity:
    vtkPointGaussianMapper applies its opacity function only to mapped
    scalars, which were off)."""
    from tests.test_point_render import _RENDER_PROBE, _table
    import subprocess
    import sys
    from pathlib import Path
    from app.render_session.points import PointRenderSession
    if subprocess.run([sys.executable, "-c", _RENDER_PROBE], capture_output=True, timeout=60,
                      cwd=Path(__file__).resolve().parent.parent).returncode != 0:
        pytest.skip("no offscreen OpenGL here (no GPU or display)")
    s = PointRenderSession("o" * 32, _table(tmp_path, n=20000))
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(s.initialize())
        loop.run_until_complete(s.apply_command({"type": "set_color", "field": None}))
        means = []
        # From the floor of eight 8-bit steps (#447) to 18; size 1, so that
        # the clusters' cores do not saturate at both.
        for intensity in (1, 100):
            loop.run_until_complete(s.apply_command({"type": "set_point_style", "mode": "gaussian",
                                                     "size": 1, "intensity": intensity}))
            jpeg = loop.run_until_complete(s.render_frame())
            grey = np.asarray(Image.open(io.BytesIO(jpeg)).convert("L"), dtype=float)
            # The light over the background (its median: the clusters cover
            # a few per cent of the frame).
            means.append(grey.mean() - np.median(grey))
        assert means[1] > 1.3 * means[0] > 0, means
    finally:
        loop.run_until_complete(s.close())
        loop.close()


def test_large_splats_do_not_vanish_without_a_float_target():
    """Review of #263: over size² a size-12 splat has opacity 0.05 / 144,
    under half an 8-bit step (1/510): a page without a float target drew
    nothing while the backend's float sum showed it. Without one the rule
    keeps the desktop's opacity; in half floats it stops at 1/512; only
    float32 divides freely. The session refuses an unknown rule whole."""
    from app.point_splat import F16_OPACITY_FLOOR, rule_opacity, rule_tone
    assert rule_opacity(0.05, 12, "v2-f32") == pytest.approx(0.05 / 144)
    # Above half a half float's step at a light of 1 (2^-11).
    assert rule_opacity(0.05, 12, "v2-f16") == F16_OPACITY_FLOOR > 2.0 ** -11
    for rule in ("v1", "v2-8bit"):
        assert rule_opacity(0.05, 12, rule) == 0.05
        assert rule_tone(rule) == "none"
    assert rule_tone("v2-f16") == rule_tone("v2-f32") == "exposure"
    with pytest.raises(ValueError, match="splat_rule"):
        rule_opacity(0.05, 1, "v3")


def test_an_unknown_splat_rule_is_refused_whole(tmp_path):
    """An older page sends no rule (v1, the look before #263); a rule the
    session does not know changes nothing."""
    from app.render_session.points import PointRenderSession
    from tests.test_point_render import _coloured_table
    s = PointRenderSession("u" * 32, _coloured_table(tmp_path, np.linspace(0.0, 1.0, 50)))
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(s.initialize())
        assert s.view.splat_rule == "v1"
        loop.run_until_complete(s.apply_command({"type": "set_point_style", "mode": "gaussian",
                                                 "size": 2, "splat_rule": "v2-f32"}))
        loop.run_until_complete(s.apply_command({"type": "set_point_style", "mode": "points",
                                                 "size": 5, "splat_rule": "v9"}))
        assert (s.view.mode, s.view.size, s.view.splat_rule) == ("gaussian", 2, "v2-f32")
    finally:
        loop.run_until_complete(s.close())
        loop.close()


@pytest.mark.parametrize("rule, cap", [(None, 64.0), ("v2-8bit", 256.0)])
def test_a_legacy_page_keeps_its_64_px_cap(tmp_path, rule, cap):
    """Review of #289: a page from before #263 (no splat_rule: v1) drew point
    sprites capped at 64 px; its refined splats must be capped there too, or
    near splats come out larger and brighter than its preview. A v2 page's
    quads go to 256 px. Splats here project to well over 64 px."""
    from tests.test_point_render import _RENDER_PROBE
    import subprocess
    import sys
    from pathlib import Path
    if subprocess.run([sys.executable, "-c", _RENDER_PROBE], capture_output=True, timeout=60,
                      cwd=Path(__file__).resolve().parent.parent).returncode != 0:
        pytest.skip("no offscreen OpenGL here (no GPU or display)")
    from app.render_session._stereo import parse_frame
    from app.render_session._stereo_points import default_data_to_model
    from app.render_session.points import PointRenderSession
    n, side, size, intensity = 4000, 50.0, 12, 35
    path, xyz = _uniform_table(tmp_path, n, side)
    bounds = [0.0, side] * 3
    d2m = default_data_to_model(bounds)
    model = np.eye(4)
    model[:3, 3] = [0.0, 0.0, -1.2]
    view = np.eye(4)
    proj = np.array([[1.2, 0, 0, 0], [0, 1.6, 0, 0], [0, 0, -1.002, -0.2002], [0, 0, -1, 0]])
    page_w, page_h = 320, 240
    bg = (0.05, 0.05, 0.08)
    radius = page_style(bounds, n, n, size, intensity)[2]
    nearest_px = 6 * radius / side * proj[1, 1] * page_h * 0.5 / (1.2 - 0.5)
    assert nearest_px > 2 * 64, nearest_px              # the cap matters here
    refs = {c: page_preview(xyz, bounds, n, size, intensity, model, d2m, view, proj,
                            page_w, page_h, bg, max_px=c) for c in (64.0, 256.0)}
    s = PointRenderSession("l" * 32, path)
    loop = asyncio.new_event_loop()
    style = {"type": "set_point_style", "mode": "gaussian", "size": size, "intensity": intensity}
    if rule:
        style["splat_rule"] = rule
    try:
        loop.run_until_complete(s.initialize())
        loop.run_until_complete(s.apply_command(style))
        loop.run_until_complete(s.apply_command(
            _pose(model, d2m, view, proj, page_w, page_h, bg, sample_rows=n,
                  viewport_size=[page_w, page_h])))
        header, jpeg, _ = parse_frame(loop.run_until_complete(s.render_frame()).to_bytes())
    finally:
        loop.run_until_complete(s.close())
        loop.close()
    assert header["splat_rule"] == (rule or "v1")
    frame = np.asarray(Image.open(io.BytesIO(jpeg)).convert("RGB"), dtype=float)[::-1, :page_w] / 255
    light = (frame - np.asarray(bg)).mean()
    mine, other = refs[cap], refs[256.0 if cap == 64.0 else 64.0]
    assert light == pytest.approx((mine - np.asarray(bg)).mean(), rel=0.08)
    assert abs(light - (other - np.asarray(bg)).mean()) > 0.15 * light
