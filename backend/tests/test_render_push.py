"""The push transport of the render WebSocket, and the interactive LOD.

GPU-free: a fake session stands in for RenderSession over a real WebSocket
(the protocol, flow control and rendering-on-change), and fake VTK objects
stand in for the mapper and actors (which props are drawn while the view
moves, and that the volume's sampling comes back after it).
"""
from __future__ import annotations

import asyncio
import json

import pytest

import app.routers.render as render_router
from app.render_session._modes import (
    ModesMixin,
    _LOD_LEVELS,
    _LOD_TARGET_MS,
)
from tests.conftest import _TEST_TOKEN


class _PushSession:
    """What render_ws uses of a RenderSession, with a frame per change."""

    def __init__(self) -> None:
        self.changed = asyncio.Event()
        self.version = 0
        self.frames = 0
        self.commands: list[dict] = []

    async def apply_command(self, command: dict) -> None:
        self.commands.append(command)
        self.version += 1
        self.changed.set()

    async def render_frame(self) -> bytes:
        self.frames += 1
        return b"frame-%d" % self.frames

    async def render_frame_if_changed(self, since: int):
        if self.version == since:
            return None, since
        return await self.render_frame(), self.version


@pytest.fixture
def ws_session(monkeypatch):
    from fastapi.testclient import TestClient
    from app.main import app

    sess = _PushSession()

    async def owned(session_id, token):
        return sess

    monkeypatch.setattr(render_router, "_owned_render_session", owned)
    monkeypatch.setattr(render_router, "_PUSH_POLL_S", 0.02)
    return TestClient(app), sess


def _connect(client):
    return client.websocket_connect(f"/v1/render/s1/ws?token={_TEST_TOKEN}")


def test_stream_pushes_the_current_view_then_each_change(ws_session):
    client, sess = ws_session
    with _connect(client) as ws:
        ws.send_text(json.dumps({"type": "stream", "window": 3, "max_fps": 60}))
        assert json.loads(ws.receive_text()) == {"stream": "on", "window": 3, "max_fps": 60.0}
        assert ws.receive_bytes() == b"frame-1"          # unasked: the current view
        ws.send_text(json.dumps({"type": "rotate", "d_azimuth": 5}))
        assert ws.receive_bytes() == b"frame-2"          # the change, unasked
        assert sess.commands == [{"type": "rotate", "d_azimuth": 5}]


def test_unacknowledged_frames_are_capped_by_the_window(ws_session):
    import time
    client, sess = ws_session
    with _connect(client) as ws:
        ws.send_text(json.dumps({"type": "stream", "window": 1, "max_fps": 60}))
        ws.receive_text()
        assert ws.receive_bytes() == b"frame-1"
        # Changes, and an explicit request, while frame-1 is unacknowledged:
        # nothing may be rendered or sent until the ack.
        ws.send_text(json.dumps({"type": "rotate", "d_azimuth": 5}))
        ws.send_text(json.dumps({"type": "request_frame"}))
        time.sleep(0.2)
        assert sess.frames == 1
        ws.send_text(json.dumps({"type": "frame_ack"}))
        assert ws.receive_bytes() == b"frame-2"           # one frame for both
        assert sess.frames == 2


def test_request_frame_still_works_without_a_stream(ws_session):
    client, sess = ws_session
    with _connect(client) as ws:
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert ws.receive_bytes() == b"frame-1"
        ws.send_text(json.dumps({"type": "rotate", "d_azimuth": 5}))
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert ws.receive_bytes() == b"frame-2"


def test_push_state_clamps_its_settings_and_its_count():
    push = render_router._PushState()
    push.configure({"window": 100, "max_fps": 0})
    assert (push.window, push.max_fps) == (8, 1.0)
    push.configure({"window": "x"})
    assert push.window == 8                               # a bad value keeps the last one
    push.ack()
    assert push.in_flight == 0                            # an extra ack never goes negative


async def test_an_ack_during_the_send_is_not_lost():
    # The client can acknowledge a frame while send_bytes() still waits on the
    # transport; counted after the send, that ack would leave a phantom frame
    # in flight and, with a window of 1, stop the stream for good.
    push = render_router._PushState()
    push.window = 1
    sess = _PushSession()
    sess.changed.set()

    class Socket:
        sent: list[bytes] = []

        async def send_bytes(self, data):
            self.sent.append(data)
            await asyncio.sleep(0)
            push.ack()                      # acknowledged before the send returns
            await asyncio.sleep(0)

    sock = Socket()
    task = asyncio.create_task(render_router._push_frames(sock, sess, asyncio.Lock(), push))
    await asyncio.sleep(0.05)
    await sess.apply_command({"type": "rotate"})
    await asyncio.sleep(0.05)
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)
    assert sock.sent == [b"frame-1", b"frame-2"]
    assert push.in_flight == 0


async def test_a_failed_render_closes_the_connection(caplog):
    class Broken(_PushSession):
        async def render_frame_if_changed(self, since: int):
            raise RuntimeError("readback failed")

    class Socket:
        closed_with = None

        async def close(self, code=1000, reason=""):
            self.closed_with = code

    sess, sock = Broken(), Socket()
    sess.changed.set()
    await render_router._push_frames(sock, sess, asyncio.Lock(), render_router._PushState())
    assert sock.closed_with == 1011
    assert any("readback failed" in str(r.exc_info[1]) for r in caplog.records if r.exc_info)


def _bare_session():
    """A RenderSession with its rendering stubbed: the change-tracking API only."""
    from concurrent.futures import ThreadPoolExecutor
    from types import SimpleNamespace
    from app.render_session import RenderSession

    s = object.__new__(RenderSession)
    s.session_id, s._closed, s._cube_reader = "t", False, None
    s._executor = ThreadPoolExecutor(max_workers=1)
    s._render_lock = asyncio.Lock()
    s._scene_dirty, s._scene_version = False, 0
    s._last_rgba_view, s._last_render_ms = "old rgb", 1.0
    s.stats = SimpleNamespace(frames_rendered=0, last_render_ms=0.0)
    s._poll_cube_chunks = lambda: None
    s._render_frame_on_thread = lambda: b"new jpeg"
    s._render_rgb_on_thread = lambda: ("new rgb", 2.0)
    return s


async def test_push_sees_a_change_the_webrtc_path_rendered_first():
    # The WebRTC path consumes its dirty flag; the push stream tracks versions
    # of its own, so a change rendered there first is still pushed.
    s = _bare_session()
    try:
        jpeg, v = await s.render_frame_if_changed(-1)
        s.mark_dirty()
        assert (await s.render_rgb_frame())[0] == "new rgb"
        assert (await s.render_frame_if_changed(v))[0] == b"new jpeg"
    finally:
        s._executor.shutdown()


async def test_two_push_streams_each_get_every_change():
    s = _bare_session()
    try:
        a = b = -1
        (_, a), (_, b) = await asyncio.gather(s.render_frame_if_changed(a),
                                              s.render_frame_if_changed(b))
        s.mark_dirty()
        (ja, a), (jb, b) = await asyncio.gather(s.render_frame_if_changed(a),
                                                s.render_frame_if_changed(b))
        assert ja == jb == b"new jpeg"
        assert (await s.render_frame_if_changed(a))[0] is None   # nothing new
    finally:
        s._executor.shutdown()


# ── Interactive LOD ─────────────────────────────────────────────────────────


class _Prop:
    def __init__(self) -> None:
        self.visible = 1
        self.color = self.width = None

    def SetVisibility(self, v):
        self.visible = v

    def GetProperty(self):
        return self

    def SetColor(self, *c):
        self.color = c

    def SetLineWidth(self, w):
        self.width = w


class _Mapper:
    def __init__(self) -> None:
        self.auto, self.image_sd, self.sample_d = 1, 1.0, 0.5

    def GetAutoAdjustSampleDistances(self):
        return self.auto

    def SetAutoAdjustSampleDistances(self, v):
        self.auto = v

    def GetImageSampleDistance(self):
        return self.image_sd

    def SetImageSampleDistance(self, v):
        self.image_sd = v

    def GetSampleDistance(self):
        return self.sample_d

    def SetSampleDistance(self, v):
        self.sample_d = v


class _Window:
    def __init__(self, w=1200, h=800) -> None:
        self.size = (w, h)

    def GetSize(self):
        return self.size

    def SetSize(self, w, h):
        self.size = (w, h)


class _View:
    cube_render_mode = "volume"
    show_rendering = True
    interactive = False
    cutting_plane_visible = True
    show_outline = True
    show_axes = False
    width = height = 0


class _Session(ModesMixin):
    def __init__(self, mode: str) -> None:
        self.view = _View()
        self.view.cube_render_mode = mode
        self._volume, self._iso_actor, self._iso_lod_actor = _Prop(), _Prop(), _Prop()
        self._cutting_plane_actor, self._outline_actor = _Prop(), _Prop()
        self._axes_renderer = None
        self._render_window = _Window()
        self._volume_mapper = _Mapper()
        self._lod_prev_size = None
        self._lod_prev_sampling = None
        self._lod_level = 0

    def drag(self, on: bool) -> None:
        self.view.interactive = on
        self._apply_interactive_mode_on_thread(on)

    def shown(self):
        return (self._volume.visible, self._iso_actor.visible, self._iso_lod_actor.visible)


def test_a_moving_volume_stays_on_screen_coarsely_sampled():
    s = _Session("volume")
    s.drag(True)
    assert s.shown() == (1, 0, 0)                         # the volume, not a wireframe
    m = s._volume_mapper
    assert (m.auto, m.image_sd) == (0, _LOD_LEVELS[0][0])
    assert s._render_window.size == (600, 400)           # on a window half the size
    assert (s.view.width, s.view.height) == (600, 400)
    assert m.sample_d == pytest.approx(0.5 * _LOD_LEVELS[0][1])
    assert s._cutting_plane_actor.visible == 0
    s.drag(False)
    assert (m.auto, m.image_sd, m.sample_d) == (1, 1.0, 0.5)   # sampling restored
    assert s._render_window.size == (1200, 800)                # and the size
    assert s.shown() == (1, 0, 0)
    assert s._cutting_plane_actor.visible == 1


def test_a_moving_isosurface_is_replaced_by_its_wireframe():
    s = _Session("isosurface")
    s.drag(True)
    assert s.shown() == (0, 0, 1)
    s.drag(False)
    assert s.shown() == (0, 1, 0)


def test_hidden_rendering_stays_hidden_while_dragging():
    s = _Session("volume")
    s.view.show_rendering = False
    s.drag(True)
    assert s.shown() == (0, 0, 0)


def test_a_reset_during_a_drag_restores_the_full_sampling():
    from app.render_session import RenderSession
    from app.render_session._common import SessionViewState

    s = object.__new__(RenderSession)
    s.session_id, s.view = "t", SessionViewState()
    s._volume_mapper = _Mapper()
    for name in ("_volume", "_iso_actor", "_iso_lod_actor", "_cutting_plane_actor",
                 "_outline_actor", "_axes_renderer", "_axes_actor", "_renderer",
                 "_render_window", "_lod_prev_sampling", "_lod_prev_size"):
        setattr(s, name, None)
    s._lod_level = 0
    s._vmin, s._vmax, s._nz = 0.0, 1.0, 1
    for stub in ("_rebuild_volume_tfs_on_thread", "_rebuild_slice_lut_on_thread",
                 "_apply_blend_mode_on_thread", "_update_cutting_plane_slice_on_thread"):
        setattr(s, stub, lambda *a: None)
    for command in ({"type": "set_interactive", "value": True}, {"type": "reset"},
                    {"type": "set_interactive", "value": False}):
        s._apply_command_on_thread(command)
    m = s._volume_mapper
    assert (m.auto, m.image_sd, m.sample_d) == (1, 1.0, 0.5)
    assert s._lod_prev_sampling is None


def test_the_drag_level_follows_the_render_time_and_is_kept():
    s = _Session("volume")
    m = s._volume_mapper
    s.drag(True)
    s._adapt_lod_on_thread(_LOD_TARGET_MS * 3)            # too slow: coarser
    assert m.image_sd == _LOD_LEVELS[1][0]
    assert m.sample_d == pytest.approx(0.5 * _LOD_LEVELS[1][1])
    for _ in range(5):
        s._adapt_lod_on_thread(_LOD_TARGET_MS * 3)        # capped at the coarsest
    assert m.image_sd == _LOD_LEVELS[-1][0]
    s._adapt_lod_on_thread(_LOD_TARGET_MS)                # on target: stays
    assert m.image_sd == _LOD_LEVELS[-1][0]
    s.drag(False)
    assert (m.auto, m.image_sd, m.sample_d) == (1, 1.0, 0.5)   # still frames: full quality
    assert s._render_window.size == (1200, 800)
    s._adapt_lod_on_thread(1.0)                           # not dragging: no effect
    s.drag(True)                                          # the next drag starts where it settled
    assert m.image_sd == _LOD_LEVELS[-1][0]
    s._adapt_lod_on_thread(1.0)                           # fast enough for the finer level
    assert m.image_sd == _LOD_LEVELS[-2][0]


def test_a_constant_workload_settles_instead_of_oscillating():
    # 42 ms at the first level, the same work at the cost ratio of the others.
    s = _Session("volume")
    s.drag(True)
    from app.render_session._modes import _lod_cost
    levels = []
    for _ in range(12):
        ms = 42.0 * _lod_cost(0) / _lod_cost(s._lod_level)
        s._adapt_lod_on_thread(ms)
        levels.append(s._lod_level)
    assert levels[-6:] == [levels[-1]] * 6
    assert levels[-1] == 1


def test_isosurface_or_hidden_frames_do_not_train_the_volume_level():
    for mode, shown in (("isosurface", True), ("volume", False)):
        s = _Session(mode)
        s.view.show_rendering = shown
        s.drag(True)
        s._lod_level = 2
        for _ in range(3):
            s._adapt_lod_on_thread(1.0)
        assert s._lod_level == 2
