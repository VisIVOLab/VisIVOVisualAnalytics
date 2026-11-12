"""Stereo frames as WebRTC (H.264) video for a headset (issue #189).

The pose id stripe and its reading are tested without aiortc, against the
same check values as the page's test (backend/tests/vr/refine-webrtc.test.mjs).
The WebSocket side (the offer, which frames go as video, the fallback to
JPEG) runs with a fake session and a fake link. Through a real H.264 encoder
and decoder (PyAV), and through a real aiortc peer connection on this
machine, every frame's stripe must give back its pose id: these two are
skipped where PyAV or aiortc is not installed.
"""
from __future__ import annotations

import asyncio
import json

import numpy as np
import pytest

import app.routers.render as render_router
from app.render_session import _stereo as S
from app.render_session import _stereo_webrtc as W
from tests.conftest import _TEST_TOKEN
from tests.test_render_stereo import _eventually, pose

ID = 0x12345678
BITS_OF_ID = [1, 1, 0, 1,
              0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1, 0, 1,
              0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0,
              0, 0, 0, 1, 1, 1, 0, 0,            # CRC-8 of 12 34 56 78: 0x1c
              0, 1, 0, 0]


# ── The stripe ───────────────────────────────────────────────────────────────


def test_the_stripe_is_start_32_id_bits_crc8_end():
    assert W.crc8(b"123456789") == 0xF4             # CRC-8/SMBUS check value
    assert W.stripe_bits(ID) == BITS_OF_ID
    assert W.STRIPE_CELLS == 48
    assert (W.stripe_cell(383), W.stripe_cell(384), W.stripe_cell(2048)) == (0, 8, 42)
    assert W.stripe_bits(2 ** 40 + 7) == W.stripe_bits(7)     # the low 32 bits


@pytest.mark.parametrize("width", [384, 1000, 2048, 3360, 5120])
def test_a_drawn_stripe_reads_back_through_blur_and_noise(width):
    rng = np.random.default_rng(width)
    for pid in rng.integers(0, 2 ** 32, 100):
        row = W.draw_stripe(int(pid), width)[8].astype(float)
        row = np.convolve(row, np.ones(7) / 7, mode="same") + rng.normal(0, 12, width) - 10
        assert W.read_stripe(np.clip(row, 0, 255)) == int(pid)


def test_a_damaged_stripe_reads_as_nothing():
    width = 2048
    cell = W.stripe_cell(width)
    for k in range(W.STRIPE_CELLS):
        row = W.draw_stripe(ID, width)[0].copy()
        row[k * cell:(k + 1) * cell] ^= 255
        assert W.read_stripe(row) is None, k
    assert W.read_stripe(np.zeros(width)) is None
    assert W.read_stripe(np.full(width, 255)) is None
    assert W.read_stripe(W.draw_stripe(ID, width)[0][::-1]) is None
    assert W.read_stripe(np.zeros(100)) is None      # too narrow
    with pytest.raises(ValueError):
        W.draw_stripe(ID, 100)


@pytest.mark.parametrize("eye_h", [256, 257])
def test_the_video_frame_is_the_eyes_over_the_stripe_with_an_even_height(eye_h):
    rgb = np.full((eye_h, 512, 3), 77, np.uint8)
    img = W.video_image(rgb, ID)
    assert img.shape == (eye_h + W.stripe_rows(eye_h), 512, 3) and img.shape[0] % 2 == 0
    assert (img[:eye_h] == 77).all()
    # The page reads 8 rows from the bottom, inside the stripe either way.
    assert W.read_stripe(img[-8].mean(axis=1)) == ID
    assert W.stripe_layout(512, eye_h) == {"y": eye_h, "rows": W.stripe_rows(eye_h),
                                           "cells": 48, "cell": 10, "id_bits": 32}


def test_a_pose_may_ask_for_a_jpeg_or_video():
    assert S.parse_pose(pose(1)).frame_color == "jpeg"
    assert S.parse_pose(pose(1, frame_color="h264")).frame_color == "h264"
    with pytest.raises(ValueError, match="frame_color"):
        S.parse_pose(pose(1, frame_color="png"))


def test_offer_settings_are_bounded():
    assert W._offer_settings({}) == (W.DEFAULT_CRF, W.DEFAULT_MAX_KBPS)
    assert W._offer_settings({"crf": 1, "max_kbps": 10 ** 9}) == (W.MIN_CRF, W.MAX_KBPS)
    assert W._offer_settings({"crf": "x", "max_kbps": True}) == (W.DEFAULT_CRF, W.DEFAULT_MAX_KBPS)


# ── Through a real H.264 encoder and decoder ─────────────────────────────────


def _scene(w: int, h: int, t: float) -> np.ndarray:
    y, x = np.mgrid[0:h, 0:2 * w].astype(np.float32)
    r = np.hypot((x % w) - w / 2 - 30 * np.sin(t), y - h / 2) / (0.35 * h)
    v = np.clip(1 - r, 0, 1) * (0.6 + 0.4 * np.sin(x / 9.0 + 3 * t) * np.cos(y / 11.0))
    return (np.stack([v ** 0.5, v, v ** 2], -1) * 255).astype(np.uint8)


def _rgba_bottom_up(rgb: np.ndarray) -> np.ndarray:
    """As the render gives the eyes: RGBA, rows bottom first."""
    a = np.full(rgb.shape[:2] + (1,), 255, np.uint8)
    return np.ascontiguousarray(np.concatenate([rgb, a], -1)[::-1])


def test_every_encoded_frame_gives_back_its_pose_id():
    av = pytest.importorskip("av")
    enc = W.H264Encoder(crf=W.DEFAULT_CRF, candidates=["libx264"])
    dec = av.CodecContext.create("h264", "r")
    eye_w, eye_h = 320, 241                       # odd height: a 17-row stripe
    got, psnr = [], []
    for i in range(40):
        rgb = _scene(eye_w, eye_h, i * 0.4)
        pid = 2 ** 33 + 1000 + 7 * i
        for packet in enc.encode(W.video_image(rgb, pid), i * 1500, keyframe=(i == 0)):
            for frame in dec.decode(packet):
                out = frame.to_ndarray(format="rgb24")
                assert out.shape == (eye_h + 17, 2 * eye_w, 3)
                got.append(W.read_stripe(out[-8].mean(axis=1)))
                err = np.mean((out[:eye_h].astype(float) - rgb) ** 2)
                psnr.append(10 * np.log10(255 ** 2 / err))
    assert enc.name == "libx264"
    # One frame out per frame in (zerolatency: none held back), each its own id.
    assert got == [(2 ** 33 + 1000 + 7 * i) & W.ID_MASK for i in range(40)]
    assert np.median(psnr) > 35


def test_the_render_process_encodes_its_frames_upright_with_their_ids():
    """The render's RGBA (rows bottom first) as YUV 4:2:0 rows top first, the
    stripe in the Y plane, encoded in the render process: the decoded frames
    read their pose ids and show the eyes upright, in their colours."""
    av = pytest.importorskip("av")
    from types import SimpleNamespace
    sess = SimpleNamespace()
    eye_w, eye_h = 320, 242                       # not a multiple of 4 rows
    rgb = _scene(eye_w, eye_h, 0.7)
    assert W.encode_eyes(sess, _rgba_bottom_up(rgb), 1) is None      # no link: JPEG
    W.apply_video_command(sess, {"type": "xr_video", "crf": 14})
    dec = av.CodecContext.create("h264", "r")
    out = []
    for pid in (11, 12, 13):
        data, info = W.encode_eyes(sess, _rgba_bottom_up(rgb), pid)
        assert info["keyframe"] == (pid == 11) and info["encoder"] == "libx264"
        out += [f.to_ndarray(format="rgb24") for f in dec.decode(av.Packet(data))]
    W.apply_video_command(sess, {"type": "xr_video_keyframe"})
    data, info = W.encode_eyes(sess, _rgba_bottom_up(rgb), 14)
    assert info["keyframe"]
    out += [f.to_ndarray(format="rgb24") for f in dec.decode(av.Packet(data))]
    assert [W.read_stripe(o[-8].mean(axis=1)) for o in out] == [11, 12, 13, 14]
    err = np.mean((out[0][:eye_h].astype(float) - rgb) ** 2)
    assert 10 * np.log10(255 ** 2 / err) > 35
    # An odd eye height: a 17-row stripe (its first row black), the eyes upright.
    odd = _scene(eye_w, 241, 0.2)
    data, info = W.encode_eyes(sess, _rgba_bottom_up(odd), 15)
    (frame,) = [f.to_ndarray(format="rgb24") for f in dec.decode(av.Packet(data))]
    assert frame.shape == (241 + W.stripe_rows(241), 2 * eye_w, 3)
    assert W.read_stripe(frame[-8].mean(axis=1)) == 15
    err = np.mean((frame[:241].astype(float) - odd) ** 2)
    assert 10 * np.log10(255 ** 2 / err) > 35
    assert W.encode_eyes(sess, _rgba_bottom_up(_scene(90, 64, 0)), 16) is None       # too narrow
    W.apply_video_command(sess, {"type": "xr_video", "off": True})
    assert W.encode_eyes(sess, _rgba_bottom_up(rgb), 17) is None


def test_an_encoder_that_holds_frames_back_is_given_up():
    pytest.importorskip("av")

    class Holding:                     # as VideoToolbox: no packet for the first frame
        def encode(self, frame):
            return []

    class Late(W.H264Encoder):
        def _open(self, name, width, height):
            return Holding() if name == "late" else super()._open(name, width, height)

    enc = Late(candidates=["late", "libx264"])
    packets = enc.encode(W.video_image(_scene(200, 64, 0), 5), 0)
    assert packets and enc.name == "libx264"


# ── The WebSocket side ───────────────────────────────────────────────────────


class _Session:
    """A cube session whose stereo frames are H.264 while it has a link and
    the pose asks for video, as the render process makes them."""

    def __init__(self) -> None:
        self.changed = asyncio.Event()
        self.version = 0
        self.commands: list[dict] = []
        self.pose: dict | None = None
        self.video = False

    async def apply_command(self, command: dict) -> None:
        self.commands.append(command)
        if command["type"] == "xr_pose":
            self.pose = command
        elif command["type"] == "xr_stop":
            self.pose = None
        elif command["type"] == "xr_video":
            self.video = not command.get("off")
        self.version += 1
        self.changed.set()

    def _render_xr_frame_on_thread(self):
        raise AssertionError("not called by the router")

    async def render_frame(self):
        if self.pose is None:
            return b"\xff\xd8mono"
        w = self.pose["views"][0]["width"]
        video = self.video and self.pose.get("frame_color") == "h264"
        header = {"v": 1, "pose_id": self.pose["pose_id"], "t_client": None,
                  "color": {"format": "h264" if video else "jpeg", "width": 2 * w, "height": w},
                  "depth": {"format": "u16-zlib", "width": 1, "height": 1},
                  "timing": {"received": self.pose["t_received"]}}
        if video and getattr(self, "encoder_fails", False):
            # As the render process does: video off, a JPEG with the reason.
            self.video = False
            header["color"]["format"] = "jpeg"
            header["video_error"] = "H.264 encoding failed: RuntimeError: no encoder"
            return S.XRFrame(header, b"\xff\xd8JPEG", b"DD")
        if video:
            header["color"].update(keyframe=True, encoder="libx264")
            return S.XRFrame(header, b"\x00\x00\x00\x01H264", b"DD")
        return S.XRFrame(header, b"\xff\xd8JPEG", b"DD")

    async def render_frame_if_changed(self, since: int):
        if self.version == since:
            return None, since
        return await self.render_frame(), self.version


#: A browser's offer as the page makes it: one video section, receive only.
OFFER = ("v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
         "m=video 9 UDP/TLS/RTP/SAVPF 102\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=recvonly\r\n"
         "a=rtpmap:102 H264/90000\r\n"
         "a=candidate:1 1 udp 2122260223 192.168.1.20 50000 typ host\r\n"
         "a=candidate:2 1 udp 2122260223 3f2b-11.local 50001 typ host\r\n"
         "a=candidate:3 1 udp 1686052607 93.184.216.34 50002 typ srflx\r\n")


class _FakeTransceiver:
    def __init__(self, track):
        self.sender = type("Sender", (), {"track": track})()
        self.currentDirection = "sendonly"


class _FakePeer:
    def __init__(self, track):
        self.connectionState = "connected"
        self.transceivers = [_FakeTransceiver(track)]

    def getTransceivers(self):
        return self.transceivers


class _FakeTrack:
    def __init__(self):
        self.pushed, self.reject, self.gaps = [], 0, 0

    def push(self, data, keyframe):
        if self.reject:
            self.reject -= 1
            return False
        self.pushed.append((data, keyframe))
        return True

    def gap(self):
        self.gaps += 1

    def stop(self):
        pass


class _FakeLink(W.StereoRtcLink):
    """The real link with no aiortc: answers at once, connected at once."""
    made: list["_FakeLink"] = []

    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        _FakeLink.made.append(self)

    async def answer(self, sdp, sdp_type="offer"):
        if "a=broken" in sdp:
            raise ValueError("bad sdp")
        self.track = _FakeTrack()
        self.pc = _FakePeer(self.track)
        return {"sdp": "v=0 answer", "sdp_type": "answer"}


@pytest.fixture
def ws_rtc(monkeypatch):
    from fastapi.testclient import TestClient
    from app.main import app

    sess = _Session()

    async def owned(session_id, token):
        return sess

    monkeypatch.setattr(render_router, "_owned_render_session", owned)
    monkeypatch.setattr(render_router, "_PUSH_POLL_S", 0.02)
    monkeypatch.setattr(W, "rtc_available", lambda: None)
    monkeypatch.setattr(W, "StereoRtcLink", _FakeLink)
    _FakeLink.made.clear()
    return TestClient(app), sess


def _connect(client):
    return client.websocket_connect(f"/v1/render/s1/ws?token={_TEST_TOKEN}")


def _offer(ws, sdp=OFFER, **extra):
    ws.send_text(json.dumps({"type": "xr_rtc_offer", "sdp": sdp, **extra}))
    return json.loads(ws.receive_text())


def test_with_the_link_up_poses_are_encoded_and_their_colour_goes_as_video(ws_rtc):
    client, sess = ws_rtc
    with _connect(client) as ws:
        answer = _offer(ws, crf=24)
        assert answer == {"type": "xr_rtc_answer", "sdp": "v=0 answer", "sdp_type": "answer",
                          "crf": 24, "max_kbps": W.DEFAULT_MAX_KBPS}
        # The render process is told first, with the link's settings.
        assert sess.commands[0] == {"type": "xr_video", "crf": 24, "max_kbps": W.DEFAULT_MAX_KBPS}
        ws.send_text(json.dumps(pose(7, 256)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        header, color, depth = S.parse_frame(ws.receive_bytes())
        assert sess.commands[-1]["frame_color"] == "h264"
        assert header["pose_id"] == 7 and color == b"" and depth == b"DD"
        assert header["color"]["format"] == "h264-webrtc"
        assert header["color"]["stripe"]["y"] == 256
        assert header["video"] == {"frame": 1, "id": 7, "keyframe": True, "bytes": 8,
                                   "encoder": "libx264"}
        assert _FakeLink.made[0].track.pushed == [(b"\x00\x00\x00\x01H264", True)]
        # A pose that asks for a JPEG (a reference) gets one, whole.
        ws.send_text(json.dumps(pose(8, 256, frame_color="jpeg")))
        ws.send_text(json.dumps({"type": "request_frame"}))
        header, color, _ = S.parse_frame(ws.receive_bytes())
        assert header["pose_id"] == 8 and color == b"\xff\xd8JPEG"
        assert "frame_color" not in sess.commands[-1]
    # Closed with the connection, and the render process back to JPEG.
    _eventually(lambda: _FakeLink.made[0].closed)
    _eventually(lambda: {"type": "xr_video", "off": True} in sess.commands)


def test_a_client_cannot_ask_for_video_frames_without_a_link(ws_rtc):
    client, sess = ws_rtc
    with _connect(client) as ws:
        ws.send_text(json.dumps(pose(3, 64, frame_color="h264")))
        ws.send_text(json.dumps({"type": "request_frame"}))
        header, color, _ = S.parse_frame(ws.receive_bytes())
        assert "frame_color" not in sess.commands[-1]
        assert header["color"]["format"] == "jpeg"


def test_keyframe_requests_reach_the_session_once_in_a_while():
    sent = []
    link = W.StereoRtcLink("s", send_command=sent.append)
    times = iter([10.0, 10.1, 10.3])
    link.clock = lambda: next(times)
    link.request_keyframe()
    link.request_keyframe()               # 0.1 s later: a browser repeating itself
    link.request_keyframe()
    assert sent == [{"type": "xr_video_keyframe"}] * 2




def test_a_frame_encoded_for_a_link_that_dropped_is_not_sent_and_jpeg_follows(ws_rtc):
    client, sess = ws_rtc
    with _connect(client) as ws:
        _offer(ws)
        link = _FakeLink.made[0]
        ws.send_text(json.dumps(pose(1, 256)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert S.parse_frame(ws.receive_bytes())[0]["color"]["format"] == "h264-webrtc"
        # The link drops (ICE failed): a frame encoded for it meanwhile is
        # not sent, and the next pose is not asked for as video.
        link.pc.connectionState = "failed"
        frame = asyncio.run(sess.render_frame())
        assert frame.header["color"]["format"] == "h264"
        assert render_router._stereo_message(frame, None, link) is None
        assert render_router._stereo_message(frame, None, None) is None
        ws.send_text(json.dumps(pose(2, 256)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        header, color, _ = S.parse_frame(ws.receive_bytes())
        assert header["pose_id"] == 2 and header["color"]["format"] == "jpeg"
        assert "frame_color" not in sess.commands[-1]
        ws.send_text(json.dumps({"type": "xr_rtc_stop"}))
        _eventually(lambda: link.closed and {"type": "xr_video", "off": True} in sess.commands)


def test_the_offer_is_refused_with_a_reason_and_jpeg_goes_on(ws_rtc, monkeypatch):
    client, sess = ws_rtc
    with _connect(client) as ws:
        assert _offer(ws, sdp="")["error"] == "xr_rtc_offer needs an sdp."
        assert "negotiation failed" in _offer(ws, sdp=OFFER + "a=broken\r\n")["error"]
        assert _FakeLink.made[-1].closed
        monkeypatch.setattr(W, "rtc_available", lambda: "WebRTC transport not built in this backend.")
        assert _offer(ws) == {"type": "xr_rtc_error",
                              "error": "WebRTC transport not built in this backend."}
        ws.send_text(json.dumps(pose(5, 256)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert S.parse_frame(ws.receive_bytes())[0]["color"]["format"] == "jpeg"


def test_the_link_is_refused_for_a_point_session_or_with_another_client(ws_rtc, monkeypatch):
    client, sess = ws_rtc
    sess._factory = "app.render_session.points:PointRenderSession"
    with _connect(client) as ws:
        assert "cubes only" in _offer(ws)["error"]
        ws.send_text(json.dumps(pose(5, 256)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert S.parse_frame(ws.receive_bytes())[0]["color"]["format"] == "jpeg"
    del sess._factory
    monkeypatch.setattr(render_router.get_render_manager(), "clients", lambda sid: 2)
    with _connect(client) as ws:
        assert "Another client" in _offer(ws)["error"]
    assert _FakeLink.made == []


def test_the_link_is_not_a_mono_webrtc_peer(ws_rtc):
    """The desktop's offer is still refused while the headset holds stereo,
    and the stereo link is in no mono registry."""
    client, sess = ws_rtc
    with _connect(client) as ws:
        _offer(ws)
        ws.send_text(json.dumps(pose(1, 256)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        ws.receive_bytes()
        from app.render_manager import _webrtc_attached
        assert not _webrtc_attached("s1")
        assert "s1" in render_router._stereo_owners


# ── Through a real peer connection ───────────────────────────────────────────


async def test_a_real_peer_receives_every_frame_with_its_pose_id():
    """aiortc on both ends, on this machine: frames encoded as the render
    process does, sent by the link's track, decoded by the receiver; the
    stripe of every frame that arrives is its pose id, in order."""
    pytest.importorskip("aiortc")
    from types import SimpleNamespace
    from aiortc import RTCConfiguration, RTCPeerConnection, RTCSessionDescription

    sess = SimpleNamespace()
    link = W.StereoRtcLink("test-session", send_command=lambda c: W.apply_video_command(sess, c))
    W.apply_video_command(sess, link.video_command())
    rx = RTCPeerConnection(RTCConfiguration(iceServers=[]))
    rx.addTransceiver("video", direction="recvonly")
    received: asyncio.Queue = asyncio.Queue()

    @rx.on("track")
    def _on_track(track):
        async def pump():
            while True:
                try:
                    frame = await track.recv()
                except Exception:
                    return
                await received.put(frame.to_ndarray(format="rgb24"))
        asyncio.ensure_future(pump())

    try:
        await rx.setLocalDescription(await rx.createOffer())
        answer = await link.answer(rx.localDescription.sdp, "offer")
        await rx.setRemoteDescription(RTCSessionDescription(answer["sdp"], answer["sdp_type"]))
        for _ in range(100):
            if link.ready:
                break
            await asyncio.sleep(0.05)
        assert link.ready
        eye_w, eye_h, sent = 320, 240, []
        for i in range(30):
            pid = 500 + 3 * i
            data, info = W.encode_eyes(sess, _rgba_bottom_up(_scene(eye_w, eye_h, i * 0.3)), pid)
            header = {"pose_id": pid, "depth": None, "timing": {},
                      "color": {"format": "h264", "width": 2 * eye_w, "height": eye_h, **info}}
            msg = link.frame_message(S.XRFrame(header, data, None), None, None)
            assert S.parse_frame(msg)[0]["color"]["format"] == "h264-webrtc"
            sent.append(pid)
            await asyncio.sleep(0.03)
        got = []
        while True:
            try:
                img = await asyncio.wait_for(received.get(), 2.0)
            except asyncio.TimeoutError:
                break
            got.append(W.read_stripe(img[-8].mean(axis=1)))
        assert None not in got
        # Every frame, in order: aiortc's own receiver gives a frame out when
        # the next one starts, so the last may stay in its jitter buffer (a
        # browser completes a frame on its RTP marker bit).
        assert got in (sent, sent[:-1]), (got, link.stats())
    finally:
        await rx.close()
        await link.close()
    assert sess._xr_video is None        # the link's close turned the encoder off


# ── Review follow-ups ────────────────────────────────────────────────────────


def test_an_offer_is_checked_and_its_unusable_candidates_dropped():
    extra = "".join(f"a=candidate:{i} 1 udp 1 {a} 5000{i} typ host\r\n" for i, a in enumerate(
        ["127.0.0.1", "::1", "224.0.0.251", "0.0.0.0", "100.64.3.4", "2001:db8::7", "fe80::1%en0",
         "not-an-address"], start=4))
    sdp, why = W.check_offer(OFFER + extra)
    cands = "\n".join(ln for ln in sdp.split("\r\n") if ln.startswith("a=candidate:")) + "\n"
    assert why is None
    for kept in ("192.168.1.20", "3f2b-11.local", "93.184.216.34", "100.64.3.4", "2001:db8::7",
                 "fe80::1%en0"):
        assert kept in cands, kept                # private, mDNS, public, CGNAT, link-local
    for dropped in (" 127.0.0.1 ", " ::1 ", " 224.0.0.251 ", " 0.0.0.0 ", " not-an-address "):
        assert dropped not in cands, dropped      # loopback, multicast, unspecified, junk
    assert W.check_offer("")[1] == "xr_rtc_offer needs an sdp."
    assert W.check_offer(None)[1] == "xr_rtc_offer needs an sdp."
    assert "larger" in W.check_offer(OFFER + "a=x:" + "y" * W.MAX_OFFER_BYTES)[1]
    two = OFFER + "m=video 9 UDP/TLS/RTP/SAVPF 103\r\na=recvonly\r\n"
    assert "exactly one video" in W.check_offer(two)[1]
    audio = OFFER.replace("m=video", "m=audio")
    assert "exactly one video" in W.check_offer(audio)[1]
    send = OFFER.replace("a=recvonly", "a=sendrecv")
    assert "only receive" in W.check_offer(send)[1]
    both = OFFER.replace("a=recvonly", "a=recvonly\r\na=sendonly")
    assert "only receive" in W.check_offer(both)[1]


def test_offers_are_rate_limited_and_client_video_commands_ignored(ws_rtc):
    client, sess = ws_rtc
    with _connect(client) as ws:
        for _ in range(6):
            assert _offer(ws)["type"] == "xr_rtc_answer"
        assert "Too many offers" in _offer(ws)["error"]
        # Every link but the newest was closed, its "off" before the next "on".
        assert [m.closed for m in _FakeLink.made] == [True] * 5 + [False]
        video = [c for c in sess.commands if c["type"] == "xr_video"]
        assert [bool(c.get("off")) for c in video] == [False, True] * 5 + [False]
        n = len(sess.commands)
        ws.send_text(json.dumps({"type": "xr_video", "off": True}))
        ws.send_text(json.dumps({"type": "xr_video_keyframe"}))
        ws.send_text(json.dumps({"type": "request_frame"}))
        ws.receive_bytes()
        assert len(sess.commands) == n          # neither reached the session


def test_an_encoder_failure_is_told_to_the_page_and_jpeg_goes_on(ws_rtc):
    client, sess = ws_rtc
    with _connect(client) as ws:
        _offer(ws)
        sess.encoder_fails = True
        ws.send_text(json.dumps(pose(1, 256)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        assert json.loads(ws.receive_text()) == {
            "type": "xr_rtc_error", "error": "H.264 encoding failed: RuntimeError: no encoder"}
        header, color, _ = S.parse_frame(ws.receive_bytes())
        assert header["color"]["format"] == "jpeg" and color == b"\xff\xd8JPEG"
        assert "video_error" not in header
        assert _FakeLink.made[0].closed
        # The connection stays open: the next pose is a JPEG.
        ws.send_text(json.dumps(pose(2, 256)))
        ws.send_text(json.dumps({"type": "request_frame"}))
        header, _, _ = S.parse_frame(ws.receive_bytes())
        assert header["pose_id"] == 2 and header["color"]["format"] == "jpeg"


def _next(ws, want):
    """Messages until one ``want(text_dict | header)`` accepts; every frame on
    the way is acknowledged (the push stream's window)."""
    while True:
        m = ws.receive()
        if m.get("text") is not None:
            msg = json.loads(m["text"])
        else:
            ws.send_text(json.dumps({"type": "frame_ack"}))
            data = m["bytes"]
            msg = S.parse_frame(data)[0] if S.is_frame(data) else {"mono": True}
        if want(msg):
            return msg


def test_a_video_frame_not_sent_frees_its_place_in_a_window_of_one(ws_rtc):
    client, sess = ws_rtc
    with _connect(client) as ws:
        ws.send_text(json.dumps({"type": "stream", "window": 1, "max_fps": 120}))
        _next(ws, lambda m: m.get("mono"))
        ws.send_text(json.dumps({"type": "xr_rtc_offer", "sdp": OFFER}))
        _next(ws, lambda m: m.get("type") == "xr_rtc_answer")
        link = _FakeLink.made[0]
        ws.send_text(json.dumps(pose(1, 256)))
        assert _next(ws, lambda m: "pose_id" in m)["pose_id"] == 1
        link.track.reject = 1                # the track drops the next frame (a gap)
        ws.send_text(json.dumps(pose(2, 256)))
        _eventually(lambda: link.track.reject == 0)
        assert link.track.pushed == [(b"\x00\x00\x00\x01H264", True)]
        # Without its place given back, a window of one would wait forever.
        ws.send_text(json.dumps(pose(3, 256)))
        header = _next(ws, lambda m: "pose_id" in m)
        assert header["pose_id"] == 3 and header["color"]["format"] == "h264-webrtc"


def test_while_not_connected_no_keyframe_is_asked_for():
    """Asking now would re-render the pose and drop it again, in a loop."""
    sent = []
    link = W.StereoRtcLink("s", send_command=sent.append)
    link.track = _FakeTrack()
    link.track.need_keyframe = False
    link.pc = _FakePeer(link.track)
    header = {"pose_id": 4, "depth": None, "timing": {},
              "color": {"format": "h264", "width": 640, "height": 240, "keyframe": True}}
    link.pc.connectionState = "disconnected"      # a moment without a connection
    for _ in range(5):
        assert link.frame_message(S.XRFrame(header, b"K", None), None, None) is None
    assert sent == [] and link.track.need_keyframe and link.track.gaps == 0
    link.pc.transceivers[0].currentDirection = "inactive"
    link.pc.connectionState = "connected"
    assert not link.ready                         # connected, but not sending the track


def test_keyframe_requests_are_rate_limited_and_retried_while_the_track_waits():
    async def run():
        sent = []
        link = W.StereoRtcLink("s", send_command=sent.append)
        link.track = type("T", (), {"need_keyframe": True, "stop": lambda self: None})()
        link.request_keyframe()                   # at once
        link.request_keyframe()                   # held back, retried at the window's end
        link.request_keyframe()                   # (one retry pending, not two)
        assert len(sent) == 1
        await asyncio.sleep(W.KEYFRAME_REQUEST_S + 0.1)
        assert len(sent) == 2
        link.request_keyframe()
        link.track.need_keyframe = False          # the keyframe came meanwhile
        await asyncio.sleep(W.KEYFRAME_REQUEST_S + 0.1)
        assert len(sent) == 2
        await link.close()
    asyncio.run(run())


def test_the_track_keeps_its_reference_chain_whole():
    pytest.importorskip("aiortc")
    gaps = []
    track = W._track_class()(lambda: gaps.append(1))
    assert not track.push(b"P0", False)           # nothing before the first keyframe:
    assert gaps == [1]                            # dropped, and a keyframe asked for
    assert track.push(b"K1", True)
    for i in range(W.MAX_QUEUED_FRAMES - 1):
        assert track.push(b"P", False)
    # Full (nothing taken by aiortc): all dropped, a keyframe asked for, and
    # P-frames refused (asking again) until it comes.
    assert not track.push(b"P", False)
    assert gaps == [1, 1] and len(track._queue) == 0
    assert not track.push(b"P", False)
    assert gaps == [1, 1, 1]
    assert track.push(b"K2", True)
    assert track.push(b"P", False)
    track.gap()
    assert len(gaps) == 4 and not track.push(b"P", False)
    assert track.stats["overflows"] == 1


def test_a_first_keyframe_dropped_before_the_link_was_up_does_not_stall_it():
    """The render process sends its first frame as a keyframe; if the link
    was not connected then, the frames after it are P-frames: the track's
    gate drops them and asks for a keyframe, so the video starts anyway."""
    pytest.importorskip("aiortc")
    sent = []
    link = W.StereoRtcLink("s", send_command=sent.append)
    link.track = W._track_class()(link.force_keyframe)
    link.pc = _FakePeer(link.track)

    def frame(key):
        header = {"pose_id": 1, "depth": None, "timing": {},
                  "color": {"format": "h264", "width": 640, "height": 240, "keyframe": key}}
        return S.XRFrame(header, b"K" if key else b"P", None)

    link.pc.connectionState = "connecting"
    assert link.frame_message(frame(True), None, None) is None
    assert sent == []
    link.pc.connectionState = "connected"
    assert link.frame_message(frame(False), None, None) is None
    assert sent == [{"type": "xr_video_keyframe"}]
    assert link.frame_message(frame(True), None, None) is not None


def test_a_still_view_gets_its_keyframe_when_the_link_connects():
    """The keyframe rendered for the link was dropped before it connected,
    and a still view pushes no further frame that would meet the gate: the
    link asks for a keyframe itself when the peer connects."""
    sent = []
    link = W.StereoRtcLink("s", send_command=sent.append)
    link.track = _FakeTrack()
    link.track.need_keyframe = True
    link.pc = _FakePeer(link.track)
    link._on_connected()
    assert sent == [{"type": "xr_video_keyframe"}]
    sent.clear()
    link.track.need_keyframe = False              # already has its keyframe
    link._last_keyframe_request = -1e9
    link._on_connected()
    assert sent == []


def test_an_encoder_failure_turns_the_video_off_once_with_its_reason(monkeypatch):
    pytest.importorskip("av")
    from types import SimpleNamespace
    sess = SimpleNamespace()
    W.apply_video_command(sess, {"type": "xr_video"})

    def broken(self, image, pts, keyframe=False):
        raise RuntimeError("encoder gone")
    monkeypatch.setattr(W.H264Encoder, "encode", broken)
    assert W.encode_eyes(sess, _rgba_bottom_up(_scene(320, 240, 0)), 1) is None
    assert sess._xr_video is None
    assert W.take_video_error(sess) == "H.264 encoding failed: RuntimeError: encoder gone"
    assert W.take_video_error(sess) is None
    assert W.encode_eyes(sess, _rgba_bottom_up(_scene(320, 240, 0)), 2) is None   # off now
    assert W.take_video_error(sess) is None


def test_eyes_too_large_for_h264_decoders_go_as_jpeg():
    from types import SimpleNamespace
    sess = SimpleNamespace()
    W.apply_video_command(sess, {"type": "xr_video"})
    wide = np.zeros((64, W.MAX_VIDEO_SIDE + 2, 4), np.uint8)
    assert W.encode_eyes(sess, wide, 1) is None and sess._xr_video is not None


def test_a_size_change_tries_every_encoder_again():
    pytest.importorskip("av")

    class Small(W.H264Encoder):
        """'small' takes frames up to 400 pixels wide (as NVENC up to 4096)."""
        def _open(self, name, width, height):
            if name == "small" and width > 400:
                raise ValueError("too wide")
            return super()._open("libx264", width, height)

    enc = Small(candidates=["small", "libx264"])
    assert enc.encode(W.video_image(_scene(200, 64, 0), 1), 0) and enc.name == "small"
    assert enc.encode(W.video_image(_scene(300, 64, 0), 2), 1) and enc.name == "libx264"
    assert enc.last_keyframe                       # a new size starts with a keyframe
    assert enc.encode(W.video_image(_scene(200, 64, 0), 3), 2) and enc.name == "libx264"


def test_every_close_waits_for_the_encoder_off_before_a_new_link_turns_it_on():
    """The peer failing closes the link (connectionstatechange) while the
    connection's own close comes too. The second close returned at once while
    the first still awaited the encoder's off, so a replacement offer could
    turn the encoder on before the old link turned it off. Both callers now
    await the same close: the old off always comes first."""
    async def run():
        order = []
        off_may_finish = asyncio.Event()

        async def slow_off(cmd):
            order.append(("start", cmd))
            await off_may_finish.wait()
            order.append(("done", cmd))

        link = W.StereoRtcLink("s", send_command=slow_off)
        first = asyncio.create_task(link.close())        # the peer failed
        await asyncio.sleep(0)

        async def replacement():
            await link.close()                           # the connection's close
            order.append(("on", "new"))                  # the new link's encoder on

        second = asyncio.create_task(replacement())
        await asyncio.sleep(0.05)
        assert ("on", "new") not in order                # still waiting for the off
        assert link.closed
        off_may_finish.set()
        await asyncio.gather(first, second)
        off = {"type": "xr_video", "off": True}
        assert order == [("start", off), ("done", off), ("on", "new")]
        await link.close()                               # once closed, at once
        assert order.count(("start", off)) == 1
    asyncio.run(run())


def test_a_cancelled_caller_does_not_cut_the_close_short():
    async def run():
        done = []
        gate = asyncio.Event()

        async def off(cmd):
            await gate.wait()
            done.append(cmd)

        link = W.StereoRtcLink("s", send_command=off)
        t = asyncio.create_task(link.close())
        await asyncio.sleep(0)
        t.cancel()
        with pytest.raises(asyncio.CancelledError):
            await t
        gate.set()
        await link.close()                               # the same close, finished
        assert done == [{"type": "xr_video", "off": True}]
    asyncio.run(run())


def test_a_second_connection_close_waits_for_the_link_another_caller_took():
    """The frame sender closes the link after an encoder error (taking it off
    the connection); a replacement offer's own close then found no link and
    turned its encoder on before the old link's off was applied."""
    async def run():
        gate, started = asyncio.Event(), asyncio.Event()
        order = []

        async def slow_off(cmd):
            started.set()
            await gate.wait()
            order.append("old off applied")

        link = W.StereoRtcLink("s", send_command=slow_off)
        push = type("P", (), {})()
        push.rtc, push.rtc_closing = link, set()
        first = asyncio.create_task(render_router._xr_rtc_close(push))   # the frame sender
        await started.wait()

        async def replacement():
            await render_router._xr_rtc_close(push)                       # the new offer
            order.append("replacement on")

        second = asyncio.create_task(replacement())
        await asyncio.sleep(0.05)
        assert order == []
        first.cancel()                       # the first caller gone: the close goes on
        gate.set()
        await second
        assert order == ["old off applied", "replacement on"]
        await asyncio.sleep(0)
        assert push.rtc_closing == set()
    asyncio.run(run())
