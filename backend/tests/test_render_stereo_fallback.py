"""Stereo frames when the GL cannot read depth back (issue #187).

On EGL/GLES (Mesa, some cluster stacks) the volume mapper's render-to-image
may give the colour and not the depth, and a GLES window refuses to read its
depth buffer. These tests drive the stereo mixin with stand-ins for the VTK
window, renderer and volume mapper (no GPU, no render process): the depth the
fake mapper hands back is good, missing, cleared or zero, and the frame must
then carry colour only, say so in its header, and stop asking for depth after
a few bad frames.
"""
from __future__ import annotations

import io
import logging
from types import SimpleNamespace

import numpy as np
import pytest

from app.render_session import _stereo as S
from tests.test_render_stereo import pose

vtk = pytest.importorskip("vtk")
from vtk.util.numpy_support import numpy_to_vtk  # noqa: E402

SIZE = 32


# ── The pure checks ──────────────────────────────────────────────────────────


def _disc(size: int = SIZE):
    """Alpha of a disc drawn in the middle of an eye, and its first-hit depth."""
    y, x = np.mgrid[0:size, 0:size]
    inside = (x - size / 2) ** 2 + (y - size / 2) ** 2 < (size / 4) ** 2
    alpha = np.where(inside, 200, 0).astype(np.uint8)
    depth = np.where(inside, 0.4, 1.0).astype(np.float32)
    return alpha, depth


def test_a_first_hit_depth_that_matches_its_picture_passes():
    alpha, depth = _disc()
    assert S.volume_depth_problem(depth, alpha) is None
    # Faint pixels without a hit (rounding at the silhouette) are allowed.
    alpha2 = alpha.copy()
    alpha2[0, :3] = 20
    assert S.volume_depth_problem(depth, alpha2) is None
    # Nothing drawn, nothing hit: fine as well.
    assert S.volume_depth_problem(np.ones((8, 8), np.float32), np.zeros((8, 8), np.uint8)) is None


def test_the_eye_inside_the_emission_passes():
    # Every ray hits at its first sample: depth near 0, every pixel opaque.
    depth = np.full((SIZE, SIZE), 1e-4, np.float32)
    alpha = np.full((SIZE, SIZE), 255, np.uint8)
    assert S.volume_depth_problem(depth, alpha) is None
    assert S.volume_depth_problem(np.zeros_like(depth), alpha) is None


@pytest.mark.parametrize("make, words", [
    (lambda d: np.ones_like(d), "1 everywhere under a drawn picture"),   # cleared, never written
    (lambda d: np.zeros_like(d), "everywhere, empty pixels included"),  # zeros
    (lambda d: np.full_like(d, 0.3), "everywhere, empty pixels included"),
    (lambda d: np.full_like(d, np.nan), "NaN"),
    (lambda d: d * 3.0, "outside 0..1"),
    (lambda d: d[:-1], "for a picture of"),
])
def test_a_depth_image_that_was_not_written_is_caught(make, words):
    alpha, depth = _disc()
    problem = S.volume_depth_problem(make(depth), alpha)
    assert problem is not None and words in problem and problem.structural


def test_a_depth_that_only_looks_odd_is_not_structural():
    # Most drawn pixels without a hit, but the image was written: this frame
    # goes without depth, nothing is counted against the session.
    alpha, depth = _disc()
    odd = depth.copy()
    odd[alpha > 0] = 1.0
    odd[0, 0] = 0.5
    problem = S.volume_depth_problem(odd, alpha)
    assert problem is not None and "drawn pixels have no depth" in problem
    assert not problem.structural


def test_the_eye_inside_faint_emission_passes():
    # Many transparent pixels at the near plane (faint emission rounds to
    # alpha 0 but has a hit), the rest deeper: a written image, accepted.
    alpha, depth = _disc()
    near = np.where(alpha > 0, 0.2, 0.0).astype(np.float32)
    assert S.volume_depth_problem(near, alpha) is None


def test_a_window_depth_buffer_that_could_not_be_read_is_caught():
    _, depth = _disc()
    assert S.surface_depth_problem(depth, 1) is None
    assert S.surface_depth_problem(np.ones_like(depth), 1) is None         # nothing drawn
    assert "status 2" in S.surface_depth_problem(depth, 2)                 # GLES: VTK_ERROR
    assert "everywhere" in S.surface_depth_problem(np.zeros_like(depth), 1)
    assert S.surface_depth_problem(np.full_like(depth, np.inf), None) is not None


# ── The mixin with stand-ins for VTK's GL objects ────────────────────────────


def _vtk_image(values: np.ndarray, components: int):
    image = vtk.vtkImageData()
    h, w = values.shape[:2]
    image.SetDimensions(w, h, 1)
    arr = numpy_to_vtk(np.ascontiguousarray(values).reshape(-1, components), deep=True)
    image.GetPointData().SetScalars(arr)
    return image


class _Mapper:
    """A GPU volume mapper as the stereo mixin uses it. ``depth``: what its
    render-to-image depth gives back: "good", "missing" (no image), "ones",
    "zeros"."""

    def __init__(self, depth: str = "good", colour: bool = True) -> None:
        self.depth, self.colour = depth, colour
        self.rti = 0
        self.depth_reads = 0
        self.sampling = (1, 1.0, 1.0)

    def SetRenderToImage(self, on): self.rti = on                    # noqa: E704
    def GetRenderToImage(self): return self.rti                      # noqa: E704
    def SetDepthImageScalarTypeToFloat(self): pass                   # noqa: E704
    def GetAutoAdjustSampleDistances(self): return self.sampling[0]  # noqa: E704
    def GetSampleDistance(self): return self.sampling[1]             # noqa: E704
    def GetImageSampleDistance(self): return self.sampling[2]        # noqa: E704
    def SetAutoAdjustSampleDistances(self, v): pass                  # noqa: E704
    def SetSampleDistance(self, v): pass                             # noqa: E704
    def SetImageSampleDistance(self, v): pass                        # noqa: E704

    def GetColorImage(self, image):
        if not self.colour:
            return
        alpha, _ = _disc()
        rgba = np.zeros((SIZE, SIZE, 4), np.uint8)
        rgba[..., 0] = alpha // 2
        rgba[..., 3] = alpha
        image.DeepCopy(_vtk_image(rgba, 4))

    def GetDepthImage(self, image):
        self.depth_reads += 1
        _, depth = _disc()
        if self.depth == "missing":
            return
        if self.depth == "ones":
            depth = np.ones_like(depth)
        elif self.depth == "zeros":
            depth = np.zeros_like(depth)
        image.DeepCopy(_vtk_image(depth, 1))


class _Prop:
    def __init__(self) -> None:
        self.matrix = None

    def GetVisibility(self): return 1              # noqa: E704
    def SetVisibility(self, v): pass               # noqa: E704
    def GetUserMatrix(self): return self.matrix    # noqa: E704
    def SetUserMatrix(self, m): self.matrix = m    # noqa: E704


class _Window:
    def __init__(self) -> None:
        self.size = (0, 0)

    def GetSize(self): return self.size            # noqa: E704
    def SetSize(self, w, h): self.size = (w, h)    # noqa: E704
    def Render(self): pass                         # noqa: E704


class _Renderer:
    def __init__(self) -> None:
        self.camera, self.bg = vtk.vtkCamera(), (0.0, 0.0, 0.0)

    def GetActiveCamera(self): return self.camera      # noqa: E704
    def SetActiveCamera(self, c): self.camera = c      # noqa: E704
    def GetBackground(self): return self.bg            # noqa: E704
    def SetBackground(self, *c): self.bg = c           # noqa: E704


class _FakeSession(S.StereoMixin):
    """The attributes of a RenderSession the stereo mixin reads."""

    def __init__(self, mapper=None, surface_status=None) -> None:
        self.session_id = "fake187"
        self._renderer, self._render_window = _Renderer(), _Window()
        self._volume_mapper = mapper
        self._volume = _Prop() if mapper is not None else None
        self._iso_actor = _Prop() if surface_status is not None else None
        self._iso_lod_actor = self._outline_actor = self._cutting_plane_actor = None
        self._axes_renderer = None
        self._nx = self._ny = self._nz = 16
        self._xr_pose = self._xr_error = self._xr_camera = None
        self._xr_render_to_image = True
        self._xr_depth_failure = None
        self._xr_depth_strikes = 0
        self._ooc = None
        self._surface_status = surface_status
        self.view = SimpleNamespace(jpeg_quality=80, blend_mode=0)
        self._xr_depth_mode = None
        self.stats = SimpleNamespace(frames_rendered=0, last_render_ms=0.0,
                                     last_jpeg_kb=0.0, bytes_sent=0)

    def _poll_cube_chunks(self): pass              # noqa: E704

    # A GLES window: the colour reads back, the depth buffer is refused.
    def _window_rgba(self, win, w, h):
        alpha, _ = _disc(w)
        rgba = np.full((h, w, 4), 255, np.uint8)
        rgba[..., 1] = alpha
        return rgba

    def _window_depth(self, win, w, h):
        _, depth = _disc(w)
        return self._surface_status, depth


def _frame(session: _FakeSession, pose_id: int, **extra):
    session._apply_xr_command_on_thread(pose(pose_id, SIZE, **extra))
    frame = session._render_xr_frame_on_thread()
    assert frame.error is None, frame.error
    header, jpeg, depth = S.parse_frame(frame.to_bytes(ready=1.0, sent=1.0))
    assert jpeg[:2] == b"\xff\xd8"
    from PIL import Image
    assert Image.open(io.BytesIO(jpeg)).size == (2 * SIZE, SIZE)
    return header, depth


def test_a_mapper_that_reads_depth_back_sends_depth_frames():
    session = _FakeSession(_Mapper("good"))
    header, depth = _frame(session, 1, depth={"scale": 1})
    assert header["reprojection"] == "depth" and header["depth_unavailable"] is None
    assert header["depth"]["source"] == "volume-first-hit" and depth
    d = header["depth"]
    dist = S.decode_depth(depth, d["width"], d["height"], d["z_near"], d["z_far"])
    assert np.isfinite(dist).mean() == pytest.approx(np.pi / 16, abs=0.03)


def test_a_frame_without_depth_asked_says_so():
    header, depth = _frame(_FakeSession(_Mapper("good")), 1, depth=False)
    assert header["reprojection"] == "pose" and header["depth_unavailable"] == "not requested"
    assert header["depth"] is None and depth is None


@pytest.mark.parametrize("kind", ["missing", "ones", "zeros"])
def test_a_depth_that_does_not_come_back_gives_colour_only_frames(kind, caplog):
    mapper = _Mapper(kind)
    session = _FakeSession(mapper)
    with caplog.at_level(logging.WARNING, logger="visivo.render"):
        headers = [_frame(session, i, depth={"scale": 2})[0] for i in range(1, 5)]
    for h in headers:
        assert h["reprojection"] == "pose" and h["depth"] is None
        assert h["depth_unavailable"]
    # The colour still comes from the mapper (composited over the background).
    assert mapper.rti == 1 and session._xr_render_to_image
    # Two bad frames in a row, then depth is not read any more.
    assert mapper.depth_reads == S.DEPTH_STRIKES
    assert session._xr_depth_failure == headers[0]["depth_unavailable"]
    assert any("depth readback unusable" in r.getMessage() for r in caplog.records)


def test_one_bad_depth_is_dropped_and_the_next_good_one_used():
    mapper = _Mapper("zeros")
    session = _FakeSession(mapper)
    h1, _ = _frame(session, 1, depth={"scale": 1})
    mapper.depth = "good"
    h2, d2 = _frame(session, 2, depth={"scale": 1})
    assert h1["reprojection"] == "pose" and h2["reprojection"] == "depth" and d2
    assert session._xr_depth_strikes == 0 and session._xr_depth_failure is None


def test_no_render_to_image_at_all_reads_the_window_without_volume_depth():
    session = _FakeSession(_Mapper("good", colour=False))
    header, depth = _frame(session, 1, depth={"scale": 4})
    assert not session._xr_render_to_image
    assert header["reprojection"] == "pose" and depth is None
    assert "cannot render to an image" in header["depth_unavailable"]


def test_a_gles_window_that_refuses_its_depth_buffer_sends_colour_only():
    session = _FakeSession(None, surface_status=2)        # VTK_ERROR
    header, depth = _frame(session, 1, depth={"scale": 1})
    assert header["reprojection"] == "pose" and depth is None
    assert "could not be read" in header["depth_unavailable"]
    ok = _FakeSession(None, surface_status=1)
    header, depth = _frame(ok, 1, depth={"scale": 1})
    assert header["reprojection"] == "depth" and header["depth"]["source"] == "surface"


def test_depth_can_be_turned_off_on_a_backend(monkeypatch):
    monkeypatch.setenv("VISIVO_XR_DEPTH", "0")
    mapper = _Mapper("good")
    header, depth = _frame(_FakeSession(mapper), 1, depth={"scale": 4})
    assert header["reprojection"] == "pose" and depth is None
    assert "VISIVO_XR_DEPTH" in header["depth_unavailable"]
    assert mapper.depth_reads == 0


def test_mip_frames_go_without_depth_and_composite_gets_it_back():
    # The ray caster writes no first-hit depth in MIP or MinIP (its depth
    # image is 1 everywhere): not read, not counted, said so.
    mapper = _Mapper("ones")
    session = _FakeSession(mapper)
    session.view.blend_mode = 1
    headers = [_frame(session, i, depth={"scale": 2})[0] for i in range(1, 4)]
    for h in headers:
        assert h["reprojection"] == "pose" and h["depth_unavailable"] == "no first-hit depth in MIP"
    assert mapper.depth_reads == 0 and session._xr_depth_failure is None
    session.view.blend_mode = 2
    assert _frame(session, 4, depth={"scale": 2})[0]["depth_unavailable"] == "no first-hit depth in MinIP"
    # Back to composite: depth read and used.
    mapper.depth = "good"
    session.view.blend_mode = 0
    h, d = _frame(session, 5, depth={"scale": 2})
    assert h["reprojection"] == "depth" and d and mapper.depth_reads == 2   # one per eye


def test_a_mode_change_gives_depth_another_try():
    mapper = _Mapper("missing")
    session = _FakeSession(mapper)
    for i in range(1, 4):
        _frame(session, i, depth={"scale": 2})
    assert session._xr_depth_failure is not None and mapper.depth_reads == S.DEPTH_STRIKES
    session.view.blend_mode = 1                  # MIP: no depth anyway
    _frame(session, 4, depth={"scale": 2})
    assert session._xr_depth_failure is None
    mapper.depth = "good"
    session.view.blend_mode = 0                  # composite again: tried again
    h, _ = _frame(session, 5, depth={"scale": 2})
    assert h["reprojection"] == "depth"


def test_a_soft_problem_never_latches():
    class _Odd(_Mapper):
        def GetDepthImage(self, image):
            self.depth_reads += 1
            alpha, depth = _disc()
            odd = depth.copy()
            odd[alpha > 0] = 1.0
            odd[0, 0] = 0.5
            image.DeepCopy(_vtk_image(odd, 1))
    mapper = _Odd()
    session = _FakeSession(mapper)
    for i in range(1, 6):
        h, _ = _frame(session, i, depth={"scale": 2})
        assert h["reprojection"] == "pose" and "drawn pixels" in h["depth_unavailable"]
    assert mapper.depth_reads == 5 and session._xr_depth_failure is None
    assert session._xr_depth_strikes == 0
