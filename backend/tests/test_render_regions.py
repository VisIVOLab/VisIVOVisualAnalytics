"""The desktop's regions on a render session's cutting plane (issue #356).

The payload is checked without a GPU. Where the backend can draw offscreen, a
session is rendered with a frontal parallel camera, so world (x, y) lands on
a known pixel, and the drawing is found where the desktop's own overlay puts
it: cube pixel x at world x on the plane, which spans -0.5 .. n - 0.5.
"""
from __future__ import annotations

import io

import numpy as np
import pytest

from app.render_session._regions import MAX_LABELS, MAX_POINTS, parse_regions
from tests.test_render_stereo import _can_render


def test_a_region_is_read_with_its_drawing():
    regions = parse_regions({"regions": [
        {"rgb": [0, 255, 0], "selected": True,
         "loops": [[[8, 4], [16, 4], [16, 12], [8, 12]]],
         "paths": [[[1, 1], [2.5, 3]]], "points": [[5, 5]],
         "labels": [{"text": "Box 1", "at": [12, 12]}]},
        {"rgb": [255, 0, 0]},
    ]})
    first, second = regions
    assert first["selected"] and first["rgb"] == (0.0, 1.0, 0.0)
    assert first["loops"] == [[(8.0, 4.0), (16.0, 4.0), (16.0, 12.0), (8.0, 12.0)]]
    assert first["paths"] == [[(1.0, 1.0), (2.5, 3.0)]]
    assert first["points"] == [(5.0, 5.0)] and first["labels"] == [("Box 1", (12.0, 12.0))]
    assert not second["selected"] and second["loops"] == []
    assert parse_regions({"regions": []}) == []


@pytest.mark.parametrize("command", [
    {}, {"regions": "x"}, {"regions": [1]}, {"regions": [{"rgb": [0, 0]}]},
    {"regions": [{"rgb": [0, 0, 256]}]}, {"regions": [{"rgb": [0, 0, True]}]},
    {"regions": [{"rgb": [0, 0, 0], "loops": [[[0, float("nan")], [1, 1]]]}]},
    {"regions": [{"rgb": [0, 0, 0], "points": [["1", 2]]}]},
    {"regions": [{"rgb": [0, 0, 0], "labels": [{"text": 3, "at": [0, 0]}]}]},
    {"regions": [{"rgb": [0, 0, 0], "points": [[0, 0]] * (MAX_POINTS + 1)}]},
    {"regions": [{"rgb": [0, 0, 0], "points": 5}]},
    {"regions": [{"rgb": [0, 0, 0], "labels": "Box 1"}]},
    {"regions": [{"rgb": [0, 0, 0],
                  "labels": [{"text": "x", "at": [0, 0]}] * (MAX_LABELS + 1)}]},
])
def test_a_malformed_payload_is_refused(command):
    with pytest.raises(ValueError):
        parse_regions(command)


N = 32
SIZE = 128


def _cube(path) -> None:
    from astropy.io import fits
    # A faint ramp: the volume stays under the threshold sent below, and the
    # plane shows a texture.
    data = np.tile(np.linspace(0.0, 1.0, N, dtype=np.float32), (N, N, 1))
    fits.PrimaryHDU(data).writeto(path)


def _rgb(jpeg: bytes) -> np.ndarray:
    from PIL import Image
    return np.asarray(Image.open(io.BytesIO(jpeg)).convert("RGB"), dtype=np.float32)


def _col(x: float) -> int:
    # Frontal parallel camera on the cube's centre, half height N/2: world x
    # from -0.5 to N - 0.5 fills the frame.
    return int((x + 0.5) / N * SIZE)


def _row(y: float) -> int:
    return int((N - 0.5 - y) / N * SIZE)   # rows top first


@pytest.mark.skipif(not _can_render(), reason="needs an offscreen render window "
                    "(EGL, or macOS with a logged-in user)")
async def test_the_regions_sit_where_the_desktop_draws_them(tmp_path):
    from app.render_session._process import RenderSessionProcess

    path = tmp_path / "ramp.fits"
    _cube(path)
    session = RenderSessionProcess("regions356", str(path))
    await session.initialize()
    c = (N - 1) / 2.0
    try:
        for cmd in ({"type": "resize", "width": SIZE, "height": SIZE},
                    {"type": "set_threshold", "value": 10.0},
                    {"type": "set_show_outline", "visible": False},
                    {"type": "set_show_axes", "visible": False},
                    {"type": "set_cutting_plane", "visible": True, "opacity": 1.0,
                     "slice_index": 10},
                    {"type": "set_camera", "position": [c, c, 500.0], "focal_point": [c, c, 10.0],
                     "view_up": [0.0, 1.0, 0.0], "parallel": True, "parallel_scale": N / 2.0},
                    {"type": "set_regions", "regions": [
                        {"rgb": [0, 255, 0], "loops": [[[8, 4], [16, 4], [16, 12], [8, 12]]]}]}):
            await session.apply_command(cmd)
        shown = _rgb(await session.render_frame())
        # While the view is dragged the plane is hidden, and its regions with it.
        await session.apply_command({"type": "set_interactive", "value": True})
        dragged = _rgb(await session.render_frame())
        await session.apply_command({"type": "set_interactive", "value": False})
        released = _rgb(await session.render_frame())
        await session.apply_command({"type": "set_cutting_plane", "visible": False})
        hidden = _rgb(await session.render_frame())
        await session.apply_command({"type": "set_cutting_plane", "visible": True,
                                     "slice_index": 20})
        moved = _rgb(await session.render_frame())
        await session.apply_command({"type": "set_regions", "regions": []})
        cleared = _rgb(await session.render_frame())
    finally:
        await session.close()

    def green(img, x, y):
        r, g, b = img[_row(y), _col(x)]
        # Green over the plane's colours, whatever JPEG blends in at the edge.
        return g > 150 and g > r + 30 and g > b + 60

    # The box's left edge on pixel column x = 8, its bottom on row y = 4,
    # read at the middle of each side.
    for img in (shown, moved, released):
        assert green(img, 8, 8) and green(img, 16, 8)
        assert green(img, 12, 4) and green(img, 12, 12)
        assert not green(img, 12, 8)          # the inside is not filled
        assert not green(img, 6, 8)           # two pixels off the edge: nothing
    for img in (hidden, cleared):
        assert not green(img, 8, 8) and not green(img, 12, 4)
    # The drag frame is half size: nothing green anywhere in it.
    g = dragged[..., 1]
    assert not ((g > 150) & (g > dragged[..., 0] + 30) & (g > dragged[..., 2] + 60)).any()
    # The plane spans -0.5 .. N - 0.5, the volume's extent, so its last texel
    # is centred on voxel N - 1: its red edges run along the frame's right
    # and top borders. Spanning 0 .. N, as it used to, they fell half a voxel
    # (two pixels) outside the frame.
    def red(px):
        r, g, b = px
        return r > 150 and r > g + 80 and r > b + 80
    mid = SIZE // 2
    assert red(shown[mid, SIZE - 1]) or red(shown[mid, SIZE - 2])
    assert red(shown[0, mid]) or red(shown[1, mid])
