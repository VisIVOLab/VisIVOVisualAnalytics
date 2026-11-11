"""The VR page's clipping box on a render session: ``set_crop`` (issue #170).

The refined frames a headset blends in must show what its own rendering
shows, so the session crops the cube to the page's clipping box. The planes
are checked without a GPU; a real render of a synthetic sphere, cropped to its
left half, runs where the backend can draw offscreen.
"""
from __future__ import annotations

import io

import numpy as np
import pytest

from app.render_session._pipeline import crop_planes
from tests.test_render_stereo import N, _can_render, _sphere, pose


def test_the_crop_planes_are_the_box_faces_in_voxel_coordinates():
    planes = crop_planes({"min": [0.25, 0, 0.5], "max": [0.75, 1, 1]}, (40, 20, 10))
    # Voxel i at x = i, its faces at i -+ 0.5: the box's faces at t * n - 0.5.
    assert planes == pytest.approx((9.5, 29.5, -0.5, 19.5, 4.5, 9.5))


def test_the_whole_cube_or_none_is_no_crop():
    assert crop_planes(None, (4, 4, 4)) is None
    assert crop_planes({"min": [0, 0, 0], "max": [1, 1, 1]}, (4, 4, 4)) is None


@pytest.mark.parametrize("box", [
    [0, 1], {"min": [0, 0], "max": [1, 1, 1]}, {"min": [0.5, 0, 0], "max": [0.4, 1, 1]},
    {"min": [0, 0, 0], "max": [1, 1, 1.5]}, {"min": [0, "a", 0], "max": [1, 1, 1]},
    {"min": [0, float("nan"), 0], "max": [1, 1, 1]}, {"max": [1, 1, 1]},
    {"min": ["0", 0, 0], "max": [1, 1, 1]}, {"min": [0, 0, 0], "max": [1, True, 1]},
    {"min": "000", "max": [1, 1, 1]},
])
def test_a_malformed_box_is_refused(box):
    with pytest.raises(ValueError):
        crop_planes(box, (4, 4, 4))


@pytest.mark.skipif(not _can_render(), reason="needs an offscreen render window "
                    "(EGL, or macOS with a logged-in user)")
async def test_a_cropped_session_draws_only_the_box(tmp_path):
    from PIL import Image
    from app.render_session import _stereo as S
    from app.render_session._process import RenderSessionProcess

    path = tmp_path / "sphere.fits"
    _sphere(path)
    size = 128
    session = RenderSessionProcess("crop170test", str(path))
    await session.initialize()
    try:
        # White and nearly opaque inside the sphere, as the VR page sends its look.
        await session.apply_command({"type": "set_transfer_functions",
                                     "color": [[0, 1, 1, 1], [1, 1, 1, 1]],
                                     "opacity": [[0.49, 0], [0.5, 0.5], [1, 0.5]]})
        await session.apply_command(pose(1, size, ipd=0.0, depth=False))
        whole = await session.render_frame()
        # The left half of the cube (x = NAXIS1 below its middle).
        await session.apply_command({"type": "set_crop", "box": {"min": [0, 0, 0], "max": [0.5, 1, 1]}})
        await session.apply_command(pose(2, size, ipd=0.0, depth=False))
        half = await session.render_frame()
        # Malformed: refused, the crop kept.
        await session.apply_command({"type": "set_crop", "box": {"min": [0.9, 0, 0], "max": [0.1, 1, 1]}})
        await session.apply_command(pose(3, size, ipd=0.0, depth=False))
        kept = await session.render_frame()
        await session.apply_command({"type": "set_crop", "box": None})
        await session.apply_command(pose(4, size, ipd=0.0, depth=False))
        again = await session.render_frame()
    finally:
        await session.close()

    def lit(frame):
        _, jpeg, _ = S.parse_frame(frame.to_bytes())
        img = np.asarray(Image.open(io.BytesIO(jpeg)).convert("L"), dtype=np.float32)[:, :size]
        return img > 20

    w, h, k, a = (lit(f) for f in (whole, half, kept, again))
    cols = np.arange(size)
    assert w.sum() > 250          # the sphere: about 10 pixels in radius here
    # Half of the sphere, all of it left of the middle (the eye looks down -z,
    # x to the right, as on the VR page).
    assert 0.35 < h.sum() / w.sum() < 0.6
    assert (h.sum(0) * cols).sum() / h.sum() < size / 2
    assert not h[:, size // 2 + 2:].any()
    assert np.array_equal(h, k)
    assert abs(int(a.sum()) - int(w.sum())) < 0.02 * w.sum()
    assert N == 64
