"""The VR page's clipping plane on a render session: ``set_clip_plane`` (issue #369).

The headset cuts a simulation volume with a plane (or a slab) and draws the
section on it; the refined frames it blends in must show the same cut and the
same section. The parsing, the planes VTK is given (through a stretched
placement too), the section's geometry and colours and its compositing are
checked without a GPU; real renders of the synthetic sphere, cut in half and
cut facing the eye, run where the backend can draw offscreen.
"""
from __future__ import annotations

import io

import numpy as np
import pytest

from app.render_session import _clip_plane as CP
from app.render_session import _stereo as S
from tests.test_render_stereo import DIST, N, _can_render, _sphere, pose


# ── Parsing ─────────────────────────────────────────────────────────────────

def test_a_plane_is_parsed_its_normal_made_unit_and_null_is_no_plane():
    c = CP.parse_clip_plane({"origin": [1, 2, 3], "normal": [0, 0, -2], "thickness": None})
    assert c.origin == (1.0, 2.0, 3.0) and c.normal == (0.0, 0.0, -1.0)
    assert c.half is None and c.section_opacity == CP.DEFAULT_SECTION_OPACITY
    s = CP.parse_clip_plane({"origin": [0, 0, 0], "normal": [3, 4, 0], "thickness": 6, "section_opacity": 0.5})
    assert s.normal == pytest.approx((0.6, 0.8, 0.0)) and s.half == 3.0 and s.section_opacity == 0.5
    assert CP.parse_clip_plane(None) is None


@pytest.mark.parametrize("spec", [
    [0, 1], {"origin": [0, 0], "normal": [1, 0, 0]}, {"origin": [0, 0, 0], "normal": [0, 0, 0]},
    {"origin": [0, float("nan"), 0], "normal": [1, 0, 0]}, {"origin": [0, 0, 0], "normal": [1, "0", 0]},
    {"origin": [0, 0, 0], "normal": [1, 0, 0], "thickness": 0},
    {"origin": [0, 0, 0], "normal": [1, 0, 0], "thickness": True},
    {"origin": [0, 0, 0], "normal": [1, 0, 0], "section_opacity": 2},
    {"normal": [1, 0, 0]}, {"origin": [0, 0, 0], "normal": [float("inf"), 0, 0]},
])
def test_a_malformed_plane_is_refused(spec):
    with pytest.raises(ValueError):
        CP.parse_clip_plane(spec)


# ── The planes VTK is given ─────────────────────────────────────────────────

def test_a_half_space_is_one_plane_a_slab_two_facing_each_other():
    c = CP.parse_clip_plane({"origin": [10, 5, 5], "normal": [1, 0, 0]})
    assert CP.world_planes(c) == [((10.0, 5.0, 5.0), (1.0, 0.0, 0.0))]
    s = CP.parse_clip_plane({"origin": [10, 5, 5], "normal": [1, 0, 0], "thickness": 4})
    (o1, n1), (o2, n2) = CP.world_planes(s)
    assert o1 == pytest.approx((8, 5, 5)) and n1 == pytest.approx((1, 0, 0))
    assert o2 == pytest.approx((12, 5, 5)) and n2 == pytest.approx((-1, 0, 0))
    assert CP.world_planes(None) == []


def test_through_a_stretched_placement_the_world_planes_cut_the_same_voxels():
    rng = np.random.default_rng(3)
    scale = np.array([1.0, 1.0, 2.5])
    for thickness in (None, 7.0):
        c = CP.parse_clip_plane({"origin": [20, 30, 5], "normal": [1, -2, 3], "thickness": thickness})
        planes = CP.world_planes(c, tuple(scale))
        v = rng.uniform(-10, 60, size=(2000, 3))
        world = v * scale
        kept_world = np.ones(len(v), bool)
        for o, n in planes:
            kept_world &= (world - np.asarray(o)) @ np.asarray(n) >= 0
        assert np.array_equal(kept_world, c.keeps(v))


def test_every_block_out_of_core_is_clipped_those_added_later_too():
    vtk = pytest.importorskip("vtk")
    from app.render_session import _ooc
    blocks = _ooc.BlockVolumes(vtk.vtkVolumeProperty())
    blocks.attach(vtk.vtkRenderer())
    a = (np.zeros((10, 10, 10), np.float32), (-1.0, -1.0, -1.0), 1.0, (0.0, 7.5, 0.0, 7.0, 0.0, 7.0))
    blocks.replace({"a": a})
    planes = CP.vtk_planes(CP.parse_clip_plane({"origin": [4, 4, 4], "normal": [0, 1, 0], "thickness": 2}))
    blocks.set_clip_planes(planes)
    assert blocks._blocks["a"][1].GetClippingPlanes().GetNumberOfItems() == 2
    blocks.replace({"a": a, "b": a})
    assert blocks._blocks["b"][1].GetClippingPlanes().GetNumberOfItems() == 2
    sample, origin, spacing = None, None, None
    assert blocks.drawn_sample() is None, "two blocks: no single one drawn"
    blocks.replace({"a": a})
    sample, origin, spacing = blocks.drawn_sample()
    assert sample.shape == (10, 10, 10) and origin == (-1.0, -1.0, -1.0) and spacing == 1.0
    blocks.set_clip_planes([])
    assert blocks._blocks["a"][1].GetClippingPlanes().GetNumberOfItems() == 0


# ── The section ─────────────────────────────────────────────────────────────

def _eye(size=96, model=None):
    p = S.parse_pose(pose(1, size, ipd=0.0))
    model = p.model if model is None else model
    return S.stereo_eye(p.eyes[0].view, p.eyes[0].projection, model, (N, N, N), 1.0)


COLOUR = [[0.0, 1.0, 0.0, 0.0], [float(N), 0.0, 0.0, 1.0]]     # red at x = 0 to blue at x = N
OPACITY = [[0.49, 0.0], [0.5, 0.1], [float(N), 0.1]]


def test_the_section_is_the_value_on_the_plane_where_the_rays_enter_the_kept_half():
    # A field equal to its voxel's x: the section's colour says where it was read.
    field = np.broadcast_to(np.arange(N, dtype=np.float32), (N, N, N))
    eye, size = _eye(), 96
    c = (N - 1) / 2
    # The plane across z at the centre, keeping the far half: the eye looks down -z at it.
    clip = CP.parse_clip_plane({"origin": [c, c, c], "normal": [0, 0, -1]})
    layer = CP.section_layer(clip, eye, (size, size), (N, N, N), (field, (0.0, 0.0, 0.0), 1.0), COLOUR, OPACITY)
    assert layer is not None
    rgba, depth = layer.dense()
    on = rgba[..., 3] > 0
    # The cube's cross-section seen from 1 m: 0.5 m wide, a quarter of the 90° field.
    cols = np.nonzero(on.any(0))[0]
    assert 0.22 * size < len(cols) <= 0.26 * size
    # The colour: red to blue across x, at the plane's opacity (premultiplied).
    a = CP.DEFAULT_SECTION_OPACITY
    row = on[size // 2]
    left, right = rgba[size // 2, row][0], rgba[size // 2, row][-1]
    assert left[3] == pytest.approx(a) and left[0] > 0.95 * a and right[2] > 0.95 * a
    # Values under the threshold (x < 0.5: the first column) are not drawn.
    field0 = np.zeros((N, N, N), np.float32)
    assert CP.section_layer(clip, eye, (size, size), (N, N, N), (field0, (0.0, 0.0, 0.0), 1.0), COLOUR, OPACITY) is None
    # The window depth is the plane's: 1 m minus nothing (the plane is the cube's centre).
    d = S.eye_distance(depth[on], eye["depth_projection"])
    assert np.allclose(d, DIST, atol=2e-3)
    # The kept half is the near one: the eye is inside it, no face is entered, no section.
    near = CP.parse_clip_plane({"origin": [c, c, c], "normal": [0, 0, 1]})
    assert CP.section_layer(near, eye, (size, size), (N, N, N), (field, (0.0, 0.0, 0.0), 1.0), COLOUR, OPACITY) is None
    # Inside the clipping box only: x below 16 voxels.
    boxed = CP.section_layer(clip, eye, (size, size), (N, N, N), (field, (0.0, 0.0, 0.0), 1.0), COLOUR, OPACITY,
                             crop=(-0.5, 15.5, -0.5, 63.5, -0.5, 63.5))
    assert boxed is not None and 0 < boxed.dense()[0][..., 3].astype(bool).any(0).sum() < 0.08 * size


def test_a_slab_shows_the_face_the_ray_enters_through():
    field = np.full((N, N, N), 10.0, np.float32)
    eye, size = _eye(64), 64
    c = (N - 1) / 2
    slab = CP.parse_clip_plane({"origin": [c, c, c], "normal": [0, 0, 1], "thickness": 8})
    rgba, depth = CP.section_layer(slab, eye, (size, size), (N, N, N), (field, (0.0, 0.0, 0.0), 1.0), COLOUR, OPACITY).dense()
    on = rgba[..., 3] > 0
    # The near face, 4 voxels (4 / 64 * 0.5 m) nearer than the centre.
    assert np.allclose(S.eye_distance(depth[on], eye["depth_projection"]), DIST - 4 / N * 0.5, atol=2e-3)


def test_the_section_goes_in_front_of_the_volume():
    rgba = np.zeros((2, 2, 4), np.uint8)
    rgba[0, 0] = [100, 50, 0, 128]                 # premultiplied volume colour
    rgba[1, 1] = [255, 255, 255, 0]                # the mapper's clear value
    layer = CP.Section((2, 2), np.array([0, 1]), np.array([0, 1]),
                       np.array([[0.5, 0.0, 0.0], [0.0, 0.5, 0.0]], np.float32), 0.5,
                       np.array([0.5, 0.5], np.float32), 4)
    depth = np.full((2, 2), 0.9, np.float32)
    out, d = CP.composite_section(rgba, depth, layer, premultiplied=True)
    assert out[0, 0].tolist() == [round(255 * 0.5 + 50), 25, 0, round(255 * (0.5 + 0.5 * 128 / 255))]
    assert out[1, 1].tolist() == [0, 128, 0, 128], "the clear white is not mixed in"
    assert out[0, 1].tolist() == [0, 0, 0, 0] and d[0, 0] == pytest.approx(0.5) and d[0, 1] == pytest.approx(0.9)
    assert rgba[0, 0].tolist() == [100, 50, 0, 128], "the input is not changed"


def test_only_the_sections_screen_box_is_unprojected_a_tile_at_a_time():
    # A headset-sized eye (2064 x 2208) looking at a small cube: the section
    # covers a few thousand pixels, and only its screen box is examined.
    import time
    import tracemalloc
    size = (2064, 2208)
    p = S.parse_pose(pose(1, 64, ipd=0.0))
    eye = S.stereo_eye(p.eyes[0].view, S.projection_from_fov(
        {"left": -0.9, "right": 0.9, "up": 0.95, "down": -0.95, "near": 0.1, "far": 100.0}),
        p.model, (N, N, N), 1.0)
    field = np.full((N, N, N), 10.0, np.float32)
    c = (N - 1) / 2
    clip = CP.parse_clip_plane({"origin": [c, c, c], "normal": [0.2, 0.1, -1.0]})
    tracemalloc.start()
    t = time.perf_counter()
    sec = CP.section_layer(clip, eye, size, (N, N, N), (field, (0.0, 0.0, 0.0), 1.0), COLOUR, OPACITY)
    elapsed = time.perf_counter() - t
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    assert sec is not None and len(sec.rows) > 100_000
    # The box of the section, not the eye: at most twice the pixels it covers.
    assert sec.examined < 2 * len(sec.rows) and sec.examined < 0.2 * size[0] * size[1]
    # Bounded temporaries (a tile of rows) and time, on any machine.
    assert peak < 200e6, f"{peak / 1e6:.0f} MB"
    assert elapsed < 1.0, f"{elapsed:.2f} s"
    # Whatever the tiling, the same pixels as a single tile.
    CP.TILE_PIXELS, keep = 1 << 30, CP.TILE_PIXELS
    try:
        whole = CP.section_layer(clip, eye, size, (N, N, N), (field, (0.0, 0.0, 0.0), 1.0), COLOUR, OPACITY)
    finally:
        CP.TILE_PIXELS = keep
    assert np.array_equal(np.sort(sec.rows * size[0] + sec.cols), np.sort(whole.rows * size[0] + whole.cols))


def test_the_threshold_is_the_opacity_functions_first_point_above_zero():
    assert CP.tf_threshold([[0.49, 0], [0.5, 0.2], [1, 0.3]]) == 0.5
    assert CP.tf_threshold([[0, 0.1], [1, 0.3]]) == float("-inf")
    assert CP.tf_threshold([[0, 0], [1, 0]]) == float("inf")
    assert CP.tf_colour([[0, 0, 0, 0], [2, 1, 0.5, 0]], np.array([-1.0, 1.0, 3.0])).tolist() == [
        [0, 0, 0], [0.5, 0.25, 0], [1, 0.5, 0]]


# ── Real renders ────────────────────────────────────────────────────────────

@pytest.mark.skipif(not _can_render(), reason="needs an offscreen render window "
                    "(EGL, or macOS with a logged-in user)")
async def test_a_clipped_session_draws_the_kept_half_and_the_section(tmp_path):
    from PIL import Image
    from app.render_session._process import RenderSessionProcess

    path = tmp_path / "sphere.fits"
    _sphere(path)
    size = 128
    c = (N - 1) / 2
    session = RenderSessionProcess("clip369test", str(path))
    await session.initialize()
    frames = {}
    try:
        await session.apply_command({"type": "set_transfer_functions",
                                     "color": [[0, 1, 0, 0], [1, 1, 0, 0]],
                                     "opacity": [[0.49, 0], [0.5, 0.5], [1, 0.5]]})
        steps = [
            ("whole", None),
            # Keep +x (the right half: the eye looks down -z, x to the right).
            ("right", {"origin": [c, c, c], "normal": [1, 0, 0]}),
            # Malformed: refused, the plane kept.
            ("kept", {"origin": [c, c, c], "normal": [0, 0, 0]}),
            ("flipped", {"origin": [c, c, c], "normal": [-1, 0, 0]}),
            # Facing the eye, keeping the far half: the section, at full and at no opacity.
            ("facing", {"origin": [c, c, c], "normal": [0, 0, -1]}),
            ("facing0", {"origin": [c, c, c], "normal": [0, 0, -1], "section_opacity": 0}),
            # A slab 8 voxels thick across x.
            ("slab", {"origin": [c, c, c], "normal": [1, 0, 0], "thickness": 8}),
            ("off", None),
        ]
        for i, (name, plane) in enumerate(steps):
            if name == "facing":
                # Faint emission, so the volume behind the cut does not fill the pixel.
                await session.apply_command({"type": "set_transfer_functions",
                                             "color": [[0, 1, 0, 0], [1, 1, 0, 0]],
                                             "opacity": [[0.49, 0], [0.5, 0.005], [1, 0.005]]})
            if name == "slab":
                await session.apply_command({"type": "set_transfer_functions",
                                             "color": [[0, 1, 0, 0], [1, 1, 0, 0]],
                                             "opacity": [[0.49, 0], [0.5, 0.5], [1, 0.5]]})
            if name != "whole":
                await session.apply_command({"type": "set_clip_plane", "plane": plane})
            await session.apply_command(pose(i + 1, size, ipd=0.0, depth={"scale": 1} if name.startswith("facing") else False))
            frames[name] = await session.render_frame()
    finally:
        await session.close()

    def parsed(frame):
        header, jpeg, depth = S.parse_frame(frame.to_bytes())
        img = np.asarray(Image.open(io.BytesIO(jpeg)).convert("RGB"), dtype=np.float32)[:, :size]
        return header, img, depth

    lit = {k: parsed(f)[1].max(axis=2) > 20 for k, f in frames.items()}
    cols = np.arange(size)
    w = lit["whole"]
    assert w.sum() > 250
    h = lit["right"]
    assert 0.35 < h.sum() / w.sum() < 0.6
    assert (h.sum(0) * cols).sum() / h.sum() > size / 2, "VTK keeps the side the normal points to"
    assert not h[:, : size // 2 - 2].any()
    assert np.array_equal(h, lit["kept"])
    f = lit["flipped"]
    assert (f.sum(0) * cols).sum() / f.sum() < size / 2
    s = lit["slab"]
    assert s.any() and not s[:, : size // 2 - 6].any() and not s[:, size // 2 + 6:].any()
    assert abs(int(lit["off"].sum()) - int(w.sum())) < 0.02 * w.sum()
    # The header says the frame was drawn with the plane; without one it says nothing.
    assert parsed(frames["right"])[0].get("clip_plane") is True
    assert "clip_plane" not in parsed(frames["off"])[0]
    # Facing the eye: the section (red at 0.85) brightens the cut; at opacity 0 only the volume behind.
    hf, img, _ = parsed(frames["facing"])
    _, img0, _ = parsed(frames["facing0"])
    centre = (slice(size // 2 - 3, size // 2 + 3),) * 2
    red, red0 = img[centre][..., 0].mean(), img0[centre][..., 0].mean()
    assert red > 200 and red > red0 + 40, (red, red0)
    assert "section" in hf["eye_ms"][0]
