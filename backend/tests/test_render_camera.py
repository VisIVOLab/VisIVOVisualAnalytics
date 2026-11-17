"""The Remote 3-D view's camera (issue #390).

The backend frame must show the scene the desktop expects at every camera:
nothing cut off by the camera's near and far planes, which a rotation used to
leave where the first fit (before the default azimuth and elevation) put them,
and the desktop's Camera presets applied to the backend's own camera.

The preset's geometry is checked without a GPU. Where the backend can draw
offscreen, a small cube is rendered with only its cutting plane on show (one
solid colour), and the plane's corners are found in the frame where the
session's camera, projected here by hand, puts them.
"""
from __future__ import annotations

import io
import math

import numpy as np
import pytest

from app.render_session._common import camera_preset
from tests.test_render_stereo import _can_render

NX, NY, NZ = 24, 20, 8
W, H = 320, 240


def _box_renderer():
    """A renderer holding the cube's box (voxel centres 0 .. n - 1), as the
    session's outline is."""
    import vtk
    src = vtk.vtkCubeSource()
    src.SetBounds(0.0, NX - 1.0, 0.0, NY - 1.0, 0.0, NZ - 1.0)
    mapper = vtk.vtkPolyDataMapper()
    mapper.SetInputConnection(src.GetOutputPort())
    actor = vtk.vtkActor()
    actor.SetMapper(mapper)
    ren = vtk.vtkRenderer()
    ren.AddActor(actor)
    ren.ResetCamera()
    return ren


def _sight(cam) -> tuple[np.ndarray, np.ndarray]:
    """Unit direction from the focal point to the camera, and the view up."""
    d = np.subtract(cam.GetPosition(), cam.GetFocalPoint())
    return d / np.linalg.norm(d), np.asarray(cam.GetViewUp())


@pytest.mark.parametrize("azimuth, elevation, direction, up", [
    (0.0, 0.0, (0, 0, 1), (0, 1, 0)),        # Front
    (-180.0, 0.0, (0, 0, -1), (0, 1, 0)),    # Back
    (90.0, 0.0, (1, 0, 0), (0, 1, 0)),       # Right
    (-90.0, 0.0, (-1, 0, 0), (0, 1, 0)),     # Left
    (0.0, 90.0, (0, 1, 0), (0, 0, -1)),      # Top: the front of the cube at the bottom
    (0.0, -90.0, (0, -1, 0), (0, 0, 1)),     # Bottom
])
def test_a_preset_looks_at_the_cube_from_its_side(azimuth, elevation, direction, up):
    ren = _box_renderer()
    cam = ren.GetActiveCamera()
    cam.Azimuth(37.0)            # wherever the view was turned before
    cam.Elevation(-12.0)
    cam.Zoom(2.5)
    camera_preset(ren, {"type": "camera_preset", "azimuth": azimuth, "elevation": elevation})
    d, u = _sight(cam)
    assert d == pytest.approx(direction, abs=1e-9)
    assert u == pytest.approx(up, abs=1e-9)
    # Fitted again, on the cube's centre: the zoom of the view before is gone.
    assert cam.GetFocalPoint() == pytest.approx(((NX - 1) / 2, (NY - 1) / 2, (NZ - 1) / 2))
    fresh = _box_renderer().GetActiveCamera()
    assert cam.GetDistance() == pytest.approx(fresh.GetDistance())


def test_a_preset_turns_as_the_desktops_camera_does():
    # The desktop resets its camera to the front and calls Azimuth or
    # Elevation: away from the poles the backend's camera is that one.
    for az, el in ((30.0, 0.0), (0.0, 40.0), (0.0, -65.0), (135.0, 0.0)):
        ref = _box_renderer()
        rc = ref.GetActiveCamera()
        rc.Azimuth(az)
        rc.Elevation(el)
        rc.OrthogonalizeViewUp()
        ren = _box_renderer()
        camera_preset(ren, {"azimuth": az, "elevation": el})
        d, u = _sight(ren.GetActiveCamera())
        rd, ru = _sight(rc)
        assert d == pytest.approx(rd, abs=1e-9) and u == pytest.approx(ru, abs=1e-9)


def test_a_cube_session_turns_to_a_preset_and_refuses_a_bad_one():
    # Through the session's command path: a malformed preset is logged and
    # leaves the camera where it was (the stream goes on).
    from app.render_session import RenderSession
    sess = RenderSession("cafe0390", "/nonexistent/cube.fits")
    try:
        sess._renderer = _box_renderer()
        sess._apply_command_on_thread({"type": "camera_preset", "azimuth": 90.0})
        d, _ = _sight(sess._renderer.GetActiveCamera())
        assert d == pytest.approx((1.0, 0.0, 0.0), abs=1e-9)
        before = sess._renderer.GetActiveCamera().GetPosition()
        sess._apply_command_on_thread({"type": "camera_preset", "elevation": "up"})
        assert sess._renderer.GetActiveCamera().GetPosition() == before
    finally:
        sess._renderer = None
        sess._executor.shutdown(wait=False)


@pytest.mark.parametrize("command", [{"azimuth": "x"}, {"elevation": None},
                                     {"azimuth": float("nan")}, {"elevation": float("inf")}])
def test_a_malformed_preset_is_refused_and_changes_nothing(command):
    ren = _box_renderer()
    before = ren.GetActiveCamera().GetPosition()
    with pytest.raises(ValueError):
        camera_preset(ren, command)
    assert ren.GetActiveCamera().GetPosition() == before


def _project(camera: dict, points: np.ndarray) -> np.ndarray:
    """Pixel (column, row from the top) of world ``points`` in a W x H frame,
    for the session's perspective camera (vertical view angle)."""
    pos = np.asarray(camera["position"], dtype=float)
    fwd = np.subtract(camera["focal_point"], pos)
    fwd /= np.linalg.norm(fwd)
    up = np.asarray(camera["view_up"], dtype=float)
    up = up - fwd * (up @ fwd)
    up /= np.linalg.norm(up)
    right = np.cross(fwd, up)
    t = math.tan(math.radians(camera["view_angle"]) / 2.0)
    rel = points - pos
    z = rel @ fwd
    ndc_x = (rel @ right) / (z * t * (W / H))
    ndc_y = (rel @ up) / (z * t)
    return np.stack([(ndc_x + 1.0) / 2.0 * W, (1.0 - ndc_y) / 2.0 * H], axis=1)


def _rgb(jpeg: bytes) -> np.ndarray:
    from PIL import Image
    return np.asarray(Image.open(io.BytesIO(jpeg)).convert("RGB"), dtype=np.float32)


def _green(img: np.ndarray, px: np.ndarray) -> bool:
    col, row = int(round(px[0])), int(round(px[1]))
    assert 0 <= col < W and 0 <= row < H, f"{px} is outside the frame"
    r, g, b = img[row, col]
    return g > 150 and g > r + 60 and g > b + 60


def _plane_probes(z: float, inset: float) -> np.ndarray:
    """The cutting plane's four corners at channel ``z`` (it spans -0.5 ..
    n - 0.5), moved ``inset`` voxels towards its centre on both axes (a
    negative inset: away from it)."""
    x0, x1, y0, y1 = -0.5 + inset, NX - 0.5 - inset, -0.5 + inset, NY - 0.5 - inset
    return np.array([[x0, y0, z], [x1, y0, z], [x0, y1, z], [x1, y1, z]], dtype=float)


@pytest.mark.skipif(not _can_render(), reason="needs an offscreen render window "
                    "(EGL, or macOS with a logged-in user)")
async def test_the_plane_is_whole_and_where_the_camera_puts_it(tmp_path):
    from astropy.io import fits
    from app.render_session._process import RenderSessionProcess

    rng = np.random.default_rng(390)
    path = tmp_path / "cube.fits"
    fits.PrimaryHDU(rng.uniform(0.0, 1.0, size=(NZ, NY, NX)).astype(np.float32)).writeto(path)
    session = RenderSessionProcess("camera390", str(path))
    await session.initialize()
    seen = []
    try:
        for cmd in ({"type": "resize", "width": W, "height": H},
                    {"type": "set_show_rendering", "visible": False},
                    {"type": "set_show_outline", "visible": False},
                    {"type": "set_show_axes", "visible": False},
                    {"type": "set_slice_lut", "name": "AllGreen"}):
            await session.apply_command(cmd)
        # The default view (azimuth 30, elevation 20), a drag from it, and
        # the presets the desktop's Camera menu sends: each at the cube's
        # first and last channel, the two faces of the box.
        for camera_cmd in (None,
                           {"type": "rotate", "d_azimuth": -55.0, "d_elevation": 25.0},
                           {"type": "camera_preset", "azimuth": 0.0, "elevation": 0.0},
                           {"type": "camera_preset", "azimuth": -180.0, "elevation": 0.0},
                           {"type": "camera_preset", "azimuth": 0.0, "elevation": 60.0}):
            if camera_cmd is not None:
                await session.apply_command(camera_cmd)
            for z in (0, NZ - 1):
                await session.apply_command({"type": "set_cutting_plane", "visible": True,
                                             "opacity": 1.0, "slice_index": z})
                img = _rgb(await session.render_frame())
                await session.refresh_levels()
                seen.append((camera_cmd, z, img, session.levels()["camera"]))
    finally:
        await session.close()

    for camera_cmd, z, img, camera in seen:
        where = f"camera {camera_cmd}, channel {z}"
        # Every corner of the plane is drawn (none cut off by the near or far
        # plane), and nothing of it beyond: it ends where the camera puts it.
        inside = _project(camera, _plane_probes(z, 1.0))
        outside = _project(camera, _plane_probes(z, -1.0))
        assert all(_green(img, p) for p in inside), where
        assert not any(_green(img, p) for p in outside), where

    # The Front preset looks down -z with y up: the plane is an upright
    # rectangle, its left and right corners in one column each.
    front = [c for cmd, _, _, c in seen if cmd and cmd.get("azimuth") == 0.0
             and cmd.get("elevation") == 0.0][0]
    px = _project(front, _plane_probes(0, 0.0))
    assert px[0, 0] == pytest.approx(px[2, 0], abs=1e-6)
    assert px[0, 1] == pytest.approx(px[1, 1], abs=1e-6)
    d = np.subtract(front["position"], front["focal_point"])
    assert d / np.linalg.norm(d) == pytest.approx((0.0, 0.0, 1.0), abs=1e-9)
