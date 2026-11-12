"""Stereo frames for a headset's pose (issue #169).

The pose schema, the geometry, the depth encoding and the wire format are
tested without a GPU; so is the WebSocket side, with a fake session. A real
render of a synthetic sphere (both eyes, the depth against the geometry, the
mono view unchanged afterwards) runs in a render child where this backend can
draw offscreen (EGL, or a Mac with a logged-in user) and is skipped elsewhere.
"""
from __future__ import annotations

import asyncio
import io
import json
import math
import os

import numpy as np
import pytest

import app.routers.render as render_router
from app.render_session import _stereo as S
from tests.conftest import _TEST_TOKEN

# The synthetic scene: a sphere of RADIUS voxels in an N^3 cube, drawn as a
# box SIDE metres wide, DIST metres in front of eyes IPD apart.
N, RADIUS, SIDE, DIST, IPD = 64, 20.0, 0.5, 1.0, 0.064
FOV = {"left": -math.pi / 4, "right": math.pi / 4, "up": math.pi / 4,
       "down": -math.pi / 4, "near": 0.1, "far": 100.0}


def _col(m: np.ndarray) -> list[float]:
    """Column-major, as WebXR sends a matrix."""
    return np.asarray(m, dtype=float).T.reshape(-1).tolist()


def _translation(x: float, y: float, z: float) -> np.ndarray:
    m = np.eye(4)
    m[:3, 3] = (x, y, z)
    return m


def pose(pose_id: int = 1, size: int = 64, ipd: float = IPD, dist: float = DIST,
         **extra) -> dict:
    model = _translation(0, 0, -dist) @ np.diag([SIDE, SIDE, SIDE, 1.0])
    views = [{"eye": eye, "view": _col(np.linalg.inv(_translation(dx, 0, 0))),
              "fov": FOV, "width": size, "height": size}
             for eye, dx in (("left", -ipd / 2), ("right", ipd / 2))]
    return {"type": "xr_pose", "pose_id": pose_id, "t_client": 12.5,
            "model": _col(model), "views": views, **extra}


# ── Schema ───────────────────────────────────────────────────────────────────


def test_a_pose_is_parsed_into_row_major_matrices_with_its_defaults():
    p = S.parse_pose(pose(7, 128))
    assert (p.pose_id, p.t_client, p.width, p.height) == (7, 12.5, 128, 128)
    assert [e.eye for e in p.eyes] == ["left", "right"]
    assert p.model[2, 3] == pytest.approx(-DIST)            # translation in the last column
    assert p.eyes[0].view[0, 3] == pytest.approx(IPD / 2)   # world -> left eye
    assert (p.z_stretch, p.depth_scale, p.quality, p.background) == (1.0, 4, None, (0, 0, 0))
    assert p.t_received is None


def test_the_eyes_may_come_in_any_order_and_a_projection_replaces_the_fov():
    c = pose()
    c["views"].reverse()
    proj = S.projection_from_fov(FOV)
    c["views"][0]["projection"] = _col(proj)
    del c["views"][0]["fov"]
    p = S.parse_pose(c)
    assert [e.eye for e in p.eyes] == ["left", "right"]
    np.testing.assert_allclose(p.eyes[1].projection, proj)


@pytest.mark.parametrize("change, message", [
    (lambda c: c.pop("pose_id"), "pose_id"),
    (lambda c: c.update(pose_id=-1), "pose_id"),
    (lambda c: c.update(pose_id=True), "pose_id"),
    (lambda c: c.update(model=[1.0] * 15), "model"),
    (lambda c: c.update(model=[0.0] * 16), "model"),
    (lambda c: c["model"].__setitem__(0, float("nan")), "model"),
    (lambda c: c.update(views=c["views"][:1]), "two"),
    (lambda c: c["views"][1].update(eye="left"), "one 'left'"),
    (lambda c: c["views"][0].update(view=_col(np.diag([2.0, 2.0, 2.0, 1.0]))), "rigid"),
    (lambda c: c["views"][0].pop("fov"), "projection or a fov"),
    (lambda c: c["views"][0].update(fov={**FOV, "near": 0.0}), "near"),
    (lambda c: c["views"][0].update(fov={**FOV, "left": 0.9, "right": 0.1}), "angles"),
    (lambda c: c["views"][0].update(width=8), "width"),
    (lambda c: c["views"][0].update(width=100), "same width"),
    (lambda c: c["views"][0].update(projection=_col(np.eye(4))), "perspective"),
    (lambda c: c.update(z_stretch=0), "z_stretch"),
    (lambda c: c.update(depth={"scale": 0}), "depth.scale"),
    (lambda c: c.update(depth="yes"), "depth"),
    (lambda c: c.update(quality=101), "quality"),
    (lambda c: c.update(background=[0, 0]), "background"),
    (lambda c: c.update(sample_distance=0.01), "sample_distance"),
])
def test_a_malformed_pose_is_refused_with_the_field_named(change, message):
    c = pose()
    change(c)
    with pytest.raises(ValueError, match=message):
        S.parse_pose(c)


def test_depth_can_be_left_out():
    assert S.parse_pose(pose(depth=False)).depth_scale is None


# ── Geometry ─────────────────────────────────────────────────────────────────


def test_the_fov_projection_is_the_opengl_frustum_and_gives_back_its_planes():
    p = S.projection_from_fov({"left": -0.5, "right": 0.3, "up": 0.4, "down": -0.6,
                               "near": 0.1, "far": 50.0})
    assert S.clip_range(p) == pytest.approx((0.1, 50.0))
    # The frustum's corner directions land on the corners of clip space.
    for x, y in ((math.tan(-0.5), math.tan(0.4)), (math.tan(0.3), math.tan(-0.6))):
        v = p @ np.array([x, y, -1.0, 1.0])
        assert v[:2] / v[3] == pytest.approx([math.copysign(1, x), math.copysign(1, y)])


def test_the_camera_of_a_view_matrix_is_its_inverse():
    turn = np.eye(4)
    turn[:3, :3] = [[0, 0, 1], [0, 1, 0], [-1, 0, 0]]     # 90 degrees about y
    eye_to_world = _translation(1, 2, 3) @ turn
    position, focal, up = S.camera_from_view(np.linalg.inv(eye_to_world))
    assert position == pytest.approx([1, 2, 3])
    assert np.subtract(focal, position) == pytest.approx([-1, 0, 0])   # its -z
    assert up == pytest.approx([0, 1, 0])


def test_voxels_land_in_the_vr_pages_box():
    m = S.voxel_to_model(40, 20, 10, z_stretch=2.0)
    # Outer pixel edges at -extent/2 and +extent/2, extent (40, 20, 20) / 40.
    lo = m @ np.array([-0.5, -0.5, -0.5, 1.0])
    hi = m @ np.array([39.5, 19.5, 9.5, 1.0])
    assert lo[:3] == pytest.approx([-0.5, -0.25, -0.25])
    assert hi[:3] == pytest.approx([0.5, 0.25, 0.25])


def test_the_voxel_camera_sees_every_voxel_where_the_headset_does():
    rng = np.random.default_rng(1)
    turn = np.eye(4)
    turn[:3, :3] = np.linalg.qr(rng.normal(size=(3, 3)))[0]
    if np.linalg.det(turn[:3, :3]) < 0:
        turn[:, 0] *= -1
    model = _translation(0.1, 1.4, -0.8) @ turn @ np.diag([0.6, 0.6, 0.6, 1.0])
    view = np.linalg.inv(_translation(0.03, 1.6, 0.1) @ turn.T)
    proj = S.projection_from_fov(FOV)
    shape, zs = (700, 650, 158), 2.5
    rigid, p_vox, stretch, s = S.voxel_camera(view, proj, model, shape, zs)
    assert S._check_rigid(rigid, "rigid") is None        # a camera VTK can take
    assert s == pytest.approx(0.6 / 700)                 # metres per voxel
    for _ in range(5):
        x = np.append(rng.uniform(0, 600, 3), 1.0)
        want = proj @ view @ model @ S.voxel_to_model(*shape, zs) @ x
        got = p_vox @ rigid @ stretch @ x
        assert got == pytest.approx(want, rel=1e-9, abs=1e-12)   # the same clip coordinates
    # Its planes are the headset's, in voxels.
    assert S.clip_range(p_vox) == pytest.approx((0.1 / s, 100.0 / s))


def test_an_eye_starts_its_rays_near_and_ends_them_past_the_box():
    shape = (400, 400, 400)
    model = _translation(0, 0, -0.3) @ np.diag([0.5, 0.5, 0.5, 1.0])   # face 5 cm away
    proj = S.projection_from_fov(FOV)
    g = S.stereo_eye(np.eye(4), proj, model, shape, 1.0)
    s = g["scale"]
    near, far = g["clipping"]
    assert near == pytest.approx(S.RAY_NEAR_VOXELS)      # not the headset's 80 voxels
    assert far * s == pytest.approx(0.55, rel=0.02)      # the back face, 0.55 m
    # The same picture as the headset's projection, only the depth range moves.
    x = np.array([0.03, -0.02, -0.2, 1.0])
    a, b = proj @ x, g["depth_projection"] @ x
    assert a[:2] / a[3] == pytest.approx(b[:2] / b[3])
    assert S.clip_range(g["depth_projection"]) == pytest.approx((near * s, far * s))


def test_a_model_must_keep_the_cube_undistorted():
    c = pose()
    c["model"] = _col(np.diag([0.5, 0.5, 1.0, 1.0]))
    with pytest.raises(ValueError, match="uniform scale"):
        S.parse_pose(c)
    c["model"] = _col(np.diag([-0.5, 0.5, 0.5, 1.0]))        # a mirror
    with pytest.raises(ValueError, match="uniform scale"):
        S.parse_pose(c)


def test_window_depth_becomes_metres_along_the_eye_axis():
    p = S.projection_from_fov(FOV)
    for dist in (0.1, 0.84, 10.0, 99.0):
        v = p @ np.array([0.0, 0.0, -dist, 1.0])
        window = (v[2] / v[3] + 1) / 2
        assert S.eye_distance(np.array([[window]]), p)[0, 0] == pytest.approx(dist, rel=1e-5)
    assert np.isinf(S.eye_distance(np.array([[1.0]]), p)[0, 0])       # nothing drawn


def test_depth_is_reduced_to_the_nearest_sample_of_each_block():
    d = np.full((5, 6), np.inf, dtype=np.float32)
    d[0, 0], d[1, 1], d[4, 5] = 2.0, 1.0, 3.0
    r = S.nearest_per_block(d, 2)
    assert r.shape == (3, 3)
    assert r[0, 0] == 1.0 and r[2, 2] == 3.0 and np.isinf(r[1, 1])
    assert S.nearest_per_block(d, 1) is d


def test_the_depth_encoding_round_trips_within_its_quantisation():
    rng = np.random.default_rng(169)
    d = rng.uniform(0.6, 1.4, size=(30, 40)).astype(np.float32)
    d[rng.random(d.shape) < 0.3] = np.inf
    payload, z0, z1 = S.encode_depth(d)
    back = S.decode_depth(payload, 40, 30, z0, z1)
    hit = np.isfinite(d)
    assert np.array_equal(np.isnan(back), ~hit)
    assert np.abs(back[hit] - d[hit]).max() <= (z1 - z0) / 65534
    empty, n0, n1 = S.encode_depth(np.full((2, 3), np.inf, dtype=np.float32))
    assert (n0, n1) == (0.0, 0.0) and np.isnan(S.decode_depth(empty, 3, 2, 0, 0)).all()


def test_the_volume_image_goes_over_the_background_as_the_window_would():
    # Outside the volume's box: VTK's clear value, transparent white.
    rgba = np.array([[[255, 255, 255, 0], [100, 50, 0, 128], [200, 10, 30, 255]]], np.uint8)
    out = S.over_background(rgba, (0.2, 0.4, 0.6))
    bg = np.array([51, 102, 153])
    assert out[0, 0, :3].tolist() == bg.tolist()
    assert out[0, 1, :3].tolist() == pytest.approx([100, 50, 0] + (127 * bg + 127) // 255, abs=1)
    assert out[0, 2].tolist() == [200, 10, 30, 255]
    assert S.over_background(rgba, (0, 0, 0))[0, 0, :3].tolist() == [0, 0, 0]


# ── The wire format ──────────────────────────────────────────────────────────


def _frame(depth: bytes | None = b"DD") -> S.XRFrame:
    header = {"v": 1, "pose_id": 3, "t_client": 1.0,
              "color": {"format": "jpeg", "width": 4, "height": 2},
              "depth": {"format": "u16-zlib", "width": 1, "height": 1} if depth else None,
              "timing": {"received": 10.0, "render_start": 10.001, "render_end": 10.011,
                         "depth_end": 10.012, "encode_end": 10.015}}
    return S.XRFrame(header, b"JPEGBYTES", depth)


def test_a_frame_carries_its_pose_payloads_and_timings():
    message = _frame().to_bytes(ready=10.016, sent=10.018)
    assert S.is_frame(message) and not S.is_frame(b"\xff\xd8")
    header, color, depth = S.parse_frame(message)
    assert (header["pose_id"], color, depth) == (3, b"JPEGBYTES", b"DD")
    assert header["timing"]["sent"] == 10.018
    assert header["timing_ms"] == pytest.approx({
        "wait": 1.0, "render": 10.0, "depth": 1.0, "encode": 3.0, "handoff": 1.0,
        "queue": 2.0, "total": 18.0})
    # Sent twice (a second client), each copy has its own times.
    again = S.parse_frame(_frame().to_bytes(ready=11.0, sent=11.0))[0]
    assert again["timing"]["sent"] == 11.0


def test_a_frame_without_depth_has_none():
    header, _, depth = S.parse_frame(_frame(None).to_bytes())
    assert header["depth"] is None and depth is None
    assert "sent" not in header["timing"]


def test_a_refused_frame_is_a_text_message():
    msg = json.loads(S.XRFrame({"pose_id": 5}, error="nope").error_message())
    assert msg == {"type": "xr_error", "pose_id": 5, "error": "nope"}


# ── The WebSocket ────────────────────────────────────────────────────────────


class _StereoSession:
    """What render_ws uses of a cube session; stereo frames after a pose."""

    def __init__(self) -> None:
        self.changed = asyncio.Event()
        self.version = 0
        self.commands: list[dict] = []
        self.pose: dict | None = None

    async def apply_command(self, command: dict) -> None:
        self.commands.append(command)
        if command["type"] == "xr_pose":
            self.pose = command
        elif command["type"] == "xr_stop":
            self.pose = None
        self.version += 1
        self.changed.set()

    def _render_xr_frame_on_thread(self):   # what marks a cube session
        raise AssertionError("not called by the router")

    async def render_frame(self):
        if self.pose is None:
            return b"\xff\xd8mono"
        f = _frame()
        f.header["pose_id"] = self.pose["pose_id"]
        f.header["timing"]["received"] = self.pose["t_received"]
        return f

    async def render_frame_if_changed(self, since: int):
        if self.version == since:
            return None, since
        return await self.render_frame(), self.version


@pytest.fixture
def ws_stereo(monkeypatch):
    from fastapi.testclient import TestClient
    from app.main import app

    sess = _StereoSession()

    async def owned(session_id, token):
        return sess

    monkeypatch.setattr(render_router, "_owned_render_session", owned)
    monkeypatch.setattr(render_router, "_PUSH_POLL_S", 0.02)
    return TestClient(app), sess


def _connect(client):
    return client.websocket_connect(f"/v1/render/s1/ws?token={_TEST_TOKEN}")


def test_a_pose_comes_back_as_a_tagged_stereo_frame_on_the_push_stream(ws_stereo):
    import time
    client, sess = ws_stereo
    with _connect(client) as ws:
        ws.send_text(json.dumps({"type": "stream", "window": 2, "max_fps": 90}))
        assert json.loads(ws.receive_text())["max_fps"] == 90.0     # above the old 60 cap
        assert ws.receive_bytes() == b"\xff\xd8mono"                 # the current view
        ws.send_text(json.dumps({"type": "frame_ack"}))
        before = time.time()
        ws.send_text(json.dumps(pose(41)))
        header, color, _ = S.parse_frame(ws.receive_bytes())
        assert header["pose_id"] == 41 and color == b"JPEGBYTES"
        t = header["timing"]
        assert before <= t["received"] <= t["ready"] <= t["sent"] <= time.time()
        assert sess.commands[-1]["t_received"] == t["received"]
    # Leaving: the session is put back to the mono view (the handler's
    # cleanup may finish just after the client has closed).
    _eventually(lambda: sess.commands[-1] == {"type": "xr_stop"})


def _eventually(check, seconds: float = 2.0) -> None:
    import time
    end = time.time() + seconds
    while not check():
        assert time.time() < end, "not within the time allowed"
        time.sleep(0.01)


def test_a_malformed_pose_is_answered_and_never_reaches_the_session(ws_stereo):
    client, sess = ws_stereo
    with _connect(client) as ws:
        bad = pose(9)
        bad["views"][0]["width"] = 3
        ws.send_text(json.dumps(bad))
        reply = json.loads(ws.receive_text())
        assert reply["type"] == "xr_error" and reply["pose_id"] == 9 and "width" in reply["error"]
        # A client's own t_received is replaced by the arrival time.
        ws.send_text(json.dumps({**pose(10), "t_received": 1.0}))
        ws.send_text(json.dumps({"type": "request_frame"}))
        header, _, _ = S.parse_frame(ws.receive_bytes())
        assert header["pose_id"] == 10 and header["timing"]["received"] > 1.0
    _eventually(lambda: [c["type"] for c in sess.commands] == ["xr_pose", "xr_stop"])


def test_a_session_that_refuses_a_frame_sends_the_reason_as_text(ws_stereo):
    client, sess = ws_stereo

    async def refused():
        return S.XRFrame({"pose_id": 4}, error="out of core")

    sess.render_frame = refused
    with _connect(client) as ws:
        ws.send_text(json.dumps(pose(4)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert json.loads(ws.receive_text()) == {"type": "xr_error", "pose_id": 4,
                                                 "error": "out of core"}


def test_a_session_of_another_kind_refuses_poses(ws_stereo, monkeypatch):
    client, sess = ws_stereo
    sess._factory = "app.render_session.other:OtherSession"
    with _connect(client) as ws:
        ws.send_text(json.dumps(pose(2)))
        assert "cubes and point tables only" in json.loads(ws.receive_text())["error"]
    assert sess.commands == []                       # nothing sent, no xr_stop either


def test_a_point_session_takes_poses(ws_stereo, monkeypatch):
    """Issue #190: point sessions draw the headset's stereo frames too."""
    client, sess = ws_stereo
    sess._factory = "app.render_session.points:PointRenderSession"
    with _connect(client) as ws:
        ws.send_text(json.dumps(pose(2)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert S.parse_frame(ws.receive_bytes())[0]["pose_id"] == 2


def test_a_mono_client_is_unaffected(ws_stereo):
    client, sess = ws_stereo
    with _connect(client) as ws:
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert ws.receive_bytes() == b"\xff\xd8mono"
    assert sess.commands == []


def test_stereo_is_one_connections_alone(ws_stereo):
    from starlette.websockets import WebSocketDisconnect
    client, sess = ws_stereo
    with _connect(client) as ws:
        ws.send_text(json.dumps(pose(1)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert S.parse_frame(ws.receive_bytes())[0]["pose_id"] == 1
        assert "s1" in render_router._stereo_owners
        # A second client would get stereo frames: it is refused (1013).
        with pytest.raises(WebSocketDisconnect) as refused:
            with _connect(client) as other:
                other.receive_bytes()
        assert refused.value.code == 1013
    _eventually(lambda: "s1" not in render_router._stereo_owners)
    with _connect(client) as ws:                 # the session is free again
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert ws.receive_bytes() == b"\xff\xd8mono"


def test_a_pose_is_refused_while_another_client_views_the_session(ws_stereo, monkeypatch):
    client, sess = ws_stereo
    monkeypatch.setattr(render_router.get_render_manager(), "clients", lambda sid: 2)
    with _connect(client) as ws:
        ws.send_text(json.dumps(pose(3)))
        assert "Another client" in json.loads(ws.receive_text())["error"]
    assert sess.commands == [] and "s1" not in render_router._stereo_owners


def test_a_webrtc_peer_blocks_stereo(monkeypatch):
    import app.render_manager as rm
    monkeypatch.setattr(rm, "_webrtc_attached", lambda sid: sid == "busy")
    assert render_router._stereo_conflict("busy", object()) is not None
    assert render_router._stereo_conflict("free", object()) is None


def test_the_drag_level_of_detail_is_left_alone_in_stereo():
    from types import SimpleNamespace
    from app.render_session._pipeline import PipelineMixin
    called = []
    s = SimpleNamespace(session_id="t", _xr_pose=object(),
                        _apply_interactive_mode_on_thread=called.append,
                        view=SimpleNamespace(interactive=False))
    PipelineMixin._apply_command_on_thread(s, {"type": "set_interactive", "value": True})
    assert called == [] and s.view.interactive is False


def test_a_run_of_poses_is_merged_into_its_newest():
    from app.render_session._process import merge_poses
    p = lambda i: ("cmd", {"type": "xr_pose", "pose_id": i})          # noqa: E731
    stop, frame, lut = ("cmd", {"type": "xr_stop"}), ("frame_if_changed", 3), ("cmd", {"type": "set_lut"})
    got = merge_poses([p(1), p(2), lut, p(3), p(4), frame, p(5), stop, p(6), p(7)])
    assert got == [p(2), lut, p(4), frame, p(5), stop, p(7)]
    assert merge_poses([]) == []


# ── A real render ────────────────────────────────────────────────────────────


def _can_render() -> bool:
    """A real render child can start here.

    On a Mac this needs a logged-in user (window server session). Elsewhere
    the VTK wheel ships the EGL window class even on hosts with no usable EGL
    device (CI runners), where the child dies at init; so EGL hosts run this
    only when VISIVO_TEST_EGL_RENDER=1 says the GPU is real.
    """
    try:
        from app.render_session import _window
        kind = _window.offscreen_kind()
        if kind == "cocoa":          # already means a logged-in user
            return True
        if kind == "egl":
            return os.environ.get("VISIVO_TEST_EGL_RENDER") == "1"
        return False
    except Exception:   # noqa: BLE001 - no VTK at all
        return False


def _sphere(path) -> None:
    from astropy.io import fits
    z, y, x = np.mgrid[0:N, 0:N, 0:N].astype(np.float32)
    c = (N - 1) / 2.0
    r = np.sqrt((x - c) ** 2 + (y - c) ** 2 + (z - c) ** 2)
    fits.PrimaryHDU(np.where(r < RADIUS, 1.0, 0.0).astype(np.float32)).writeto(path)


def expected_distance(size: int, eye_x: float, dist: float = DIST) -> np.ndarray:
    """Distance along -z of the sphere's front surface per pixel of one eye
    (rows top first), NaN where the ray misses it."""
    r_world = RADIUS / N * SIDE
    u = (np.arange(size) + 0.5) / size * 2 - 1          # 90 degree field of view
    xs, ys = np.meshgrid(u, -u)
    d = np.stack([xs, ys, -np.ones_like(xs)], axis=-1)
    d /= np.linalg.norm(d, axis=-1, keepdims=True)
    centre = np.array([-eye_x, 0.0, -dist])              # in the eye's frame
    b = d @ centre
    disc = b ** 2 - (centre @ centre - r_world ** 2)
    t = b - np.sqrt(np.where(disc >= 0, disc, np.nan))
    return t * -d[..., 2]


@pytest.mark.skipif(not _can_render(), reason="needs macOS with a logged-in user, or "
                    "VISIVO_TEST_EGL_RENDER=1 on a host with a real EGL GPU")
async def test_a_cube_session_renders_both_eyes_with_their_depth(tmp_path):
    from PIL import Image
    from app.render_session._process import RenderSessionProcess

    path = tmp_path / "sphere.fits"
    _sphere(path)
    size = 192
    session = RenderSessionProcess("stereo169test", str(path))
    await session.initialize()
    try:
        await session.apply_command({"type": "set_threshold", "value": 0.5})
        mono = await session.render_frame()
        await session.apply_command(pose(7, size, depth={"scale": 1}))
        frame = await session.render_frame()           # crossed the pipe from the child
        await session.apply_command(pose(8, size, ipd=0.0))
        same = await session.render_frame()
        # An isosurface: its depth is the surface's, read from the window.
        await session.apply_command({"type": "set_cube_render_mode", "mode": "isosurface"})
        await session.apply_command(pose(9, size, depth={"scale": 1}))
        iso = await session.render_frame()
        await session.apply_command({"type": "set_cube_render_mode", "mode": "volume"})
        # Leaning in: the box's face 5 cm away (inside the headset's 0.1 m near
        # plane), the eye inside the emission, the eye at the box's centre.
        close = {}
        for i, dist in enumerate((SIDE / 2 + 0.05, 0.12, 0.0)):
            await session.apply_command(pose(20 + i, size, dist=dist, depth={"scale": 1}))
            close[dist] = await session.render_frame()
        # A burst of poses: one frame, for the newest.
        for i in range(30, 40):
            await session.apply_command(pose(i, size))
        burst = await session.render_frame()
        await session.apply_command({"type": "xr_stop"})
        mono_again = await session.render_frame()
    finally:
        await session.close()

    header, jpeg, depth = S.parse_frame(frame.to_bytes(ready=1.0, sent=1.0))
    assert header["pose_id"] == 7 and header["t_client"] == 12.5
    assert header["color"]["width"] == 2 * size and header["depth"]["source"] == "volume-first-hit"
    assert set(header["timing_ms"]) >= {"render", "depth", "encode"}
    img = np.asarray(Image.open(io.BytesIO(jpeg)).convert("L"), dtype=np.float32)
    assert img.shape == (size, 2 * size)
    left, right = img[:, :size], img[:, size:]
    # The sphere is right of centre for the left eye, left of it for the right.
    cols = np.arange(size)
    lx, rx = ((e.sum(0) * cols).sum() / e.sum() for e in (left, right))
    shift = IPD / 2 / (DIST - RADIUS / N * SIDE) * size / 2     # expected, pixels
    assert lx - size / 2 == pytest.approx(shift, abs=1.5)
    assert size / 2 - rx == pytest.approx(shift, abs=1.5)
    assert np.abs(left - right).mean() > 0.5
    # The depth is the sphere's front surface, within a voxel.
    d = header["depth"]
    dist = S.decode_depth(depth, d["width"], d["height"], d["z_near"], d["z_far"])
    voxel = SIDE / N
    for i, ex in enumerate((-IPD / 2, IPD / 2)):
        got, want = dist[:, i * size:(i + 1) * size], expected_distance(size, ex)
        both = np.isfinite(got) & np.isfinite(want)
        assert both.sum() > 0.9 * np.isfinite(want).sum()
        err = np.abs(got[both] - want[both]) / voxel
        assert np.median(err) < 0.5 and np.percentile(err, 95) < 1.0
    # Zero eye separation: the same picture twice, with a reduced depth.
    h2, j2, _ = S.parse_frame(same.to_bytes())
    z = np.asarray(Image.open(io.BytesIO(j2)).convert("L"), dtype=np.float32)
    assert np.abs(z[:, :size] - z[:, size:]).mean() < 0.5
    assert h2["depth"]["width"] == 2 * size // 4
    # Pixels with no sample: only a handful of rays grazing the silhouette
    # report a hit where the analytic sphere has none (interpolation spreads
    # the surface by up to a voxel).
    for i, ex in enumerate((-IPD / 2, IPD / 2)):
        got, want = dist[:, i * size:(i + 1) * size], expected_distance(size, ex)
        assert (np.isfinite(got) & ~np.isfinite(want)).sum() < 0.03 * np.isfinite(want).sum()
        assert (~np.isfinite(got) & np.isfinite(want)).sum() < 0.03 * np.isfinite(want).sum()
    for d_box, f in close.items():
        hc, jc, dc = S.parse_frame(f.to_bytes())
        lit = np.asarray(Image.open(io.BytesIO(jc)).convert("L"))[:, :size] > 20
        dd = hc["depth"]
        near = S.decode_depth(dc, dd["width"], dd["height"], dd["z_near"], dd["z_far"])[:, :size]
        want = expected_distance(size, -IPD / 2, d_box)
        if d_box > RADIUS / N * SIDE:      # the sphere in front: as seen from outside
            assert abs(lit.mean() - np.isfinite(want).mean()) < 0.03
        else:                              # the eye inside it: emission everywhere,
            assert lit.mean() > 0.99       # from the first sample on
            assert np.nanmax(near) < 2 * S.RAY_NEAR_VOXELS * SIDE / N + 0.01
    assert burst.header["pose_id"] == 39
    hi, _, di = S.parse_frame(iso.to_bytes())
    assert hi["depth"]["source"] == "surface"
    d = hi["depth"]
    got = S.decode_depth(di, d["width"], d["height"], d["z_near"], d["z_far"])[:, :size]
    want = expected_distance(size, -IPD / 2)
    both = np.isfinite(got) & np.isfinite(want)
    assert both.sum() > 0.9 * np.isfinite(want).sum()
    assert np.median(np.abs(got[both] - want[both])) / voxel < 1.0
    # The mono view is the desktop's as before.
    assert mono[:2] == b"\xff\xd8" and mono_again[:2] == b"\xff\xd8"
    a, b = (np.asarray(Image.open(io.BytesIO(j)), dtype=np.float32) for j in (mono, mono_again))
    assert a.shape == b.shape and np.abs(a - b).mean() < 1.0
