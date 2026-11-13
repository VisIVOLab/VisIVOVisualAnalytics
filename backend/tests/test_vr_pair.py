"""Headset pairing by code and the WebRTC data-channel bridge (issue #212).

The framing, the routes without a pairing service and the bridge's pacing
run everywhere. End to end, in this process: the pairing service of #211 on
a free port of 127.0.0.1, this backend as its host, and an aiortc peer as
the headset's page, which joins with a code and then fetches a reduced cube,
streams 100 MB (memory measured), drives a render session's WebSocket
(stereo WebRTC signalling included, and a frame over 1 MiB), cancels,
and is revoked, expired, refused a used code and resumed after its
connection dropped. Those are skipped where aiortc or PyAV is missing.
"""
from __future__ import annotations

import asyncio
import importlib.util
import json
import resource
import sys
import time
import tracemalloc
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest
import pytest_asyncio
from fastapi import WebSocket
from httpx import ASGITransport, AsyncClient

from app import vr_bridge as B
from app import vr_pair_host as H
from app.main import app
from tests.conftest import _TEST_TOKEN

AUTH = {"X-Visivo-Token": _TEST_TOKEN}
SERVICE = Path(__file__).resolve().parents[2] / "services" / "vr-pair" / "vr_pair_service.py"
PAGE_ORIGIN = "http://localhost:8811"


# ── Framing ───────────────────────────────────────────────────────────────────


def test_api_chunks_carry_a_big_endian_id_and_the_last_flag():
    frame = B.api_chunk(0x01020304, b"abc", True)
    assert frame == b"\x01\x02\x03\x04\x01abc"
    assert B.parse_api_chunk(frame) == (0x01020304, True, b"abc")
    assert B.parse_api_chunk(B.api_chunk(7, b"", False)) == (7, False, b"")
    with pytest.raises(ValueError):
        B.parse_api_chunk(b"\x00\x00")


def test_a_ws_message_is_fragmented_in_16k_frames_and_joined_again():
    data = bytes(range(256)) * 6000                     # 1.5 MB, over the 256 KiB a browser takes
    frames = B.ws_frames(B.WS_BINARY, data)
    assert len(frames) == -(-len(data) // B.CHUNK)
    assert all(len(f) <= B.CHUNK + 2 for f in frames)
    assert [f[1] for f in frames] == [0] * (len(frames) - 1) + [1]
    asm = B.WsAssembler()
    got = [asm.feed(f) for f in frames]
    assert got[:-1] == [None] * (len(frames) - 1) and got[-1] == (B.WS_BINARY, data)
    assert B.ws_frames(B.WS_TEXT, b"") == [b"\x01\x01"]
    assert asm.feed(b"\x01\x01") == (B.WS_TEXT, b"")
    with pytest.raises(ValueError):                      # a message changing type
        asm.feed(b"\x01\x00a")
        asm.feed(b"\x02\x01b")
    with pytest.raises(OverflowError):
        small = B.WsAssembler(limit=10)
        small.feed(b"\x02\x00" + b"x" * 11)


def test_paths_headers_and_tokens_from_the_page():
    assert B.split_path("/v1/cube/x?a=1&b=2") == ("/v1/cube/x", b"a=1&b=2")
    assert B.split_path("vr/") == ("/vr/", b"")
    for bad in ("", "//evil.example/x", "http://x/y", None):
        with pytest.raises(ValueError):
            B.split_path(bad)
    assert B.denied("/v1/vr/pair") and B.denied("/v1/vr/pair/ABCDEF") and B.denied("/v1/vr/%70air")
    assert not B.denied("/v1/vr/pairing-notes") and not B.denied("/v1/vr/info")
    hdrs = B.page_headers({"X-Visivo-Token": "stolen", "Content-Type": "application/json", "Host": "x"})
    assert hdrs == [(b"content-type", b"application/json")]
    assert B.page_headers([["accept", "*/*"], ["bad"]]) == [(b"accept", b"*/*")]
    assert B.without_token_query(b"token=abc&x=1") == b"x=1"


class _FakeChannel:
    """A data channel whose buffer drains only when told to."""

    def __init__(self) -> None:
        self.readyState = "open"
        self.bufferedAmount = 0
        self.bufferedAmountLowThreshold = 0
        self.sent: list = []
        self._handlers: dict[str, list] = {}

    def on(self, event, fn=None):
        self._handlers.setdefault(event, []).append(fn)
        return fn

    def remove_listener(self, event, fn):
        self._handlers.get(event, []).remove(fn)

    def emit(self, event, *args):
        for fn in list(self._handlers.get(event, [])):
            fn(*args)

    def send(self, data):
        self.sent.append(data)
        self.bufferedAmount += len(data)

    def drain_all(self):
        self.bufferedAmount = 0
        self.emit("bufferedamountlow")

    def close(self):
        self.readyState = "closed"
        self.emit("close")


async def test_a_response_waits_while_the_channel_holds_over_a_mebibyte():
    """The route's body iterator is not asked for more while the channel is
    full: what the bridge holds stays near 1 MiB whatever the response's
    size."""
    produced = []

    async def asgi(scope, receive, send):
        assert dict(scope["headers"])[b"x-visivo-token"] == b"tok"
        await send({"type": "http.response.start", "status": 200, "headers": [(b"x-a", b"1")]})
        for i in range(8):
            produced.append(i)
            await send({"type": "http.response.body", "body": b"x" * (512 * 1024), "more_body": True})
        await send({"type": "http.response.body", "body": b"", "more_body": False})

    ch = _FakeChannel()
    B.ApiBridge(ch, asgi, "tok")
    ch.emit("message", json.dumps({"t": "req", "id": 5, "method": "GET", "path": "/x",
                                   "headers": {"X-Visivo-Token": "page"}, "body": False}))
    await asyncio.sleep(0.05)
    assert json.loads(ch.sent[0]) == {"t": "res", "id": 5, "status": 200, "headers": {"x-a": "1"}}
    # 1 MiB plus one frame buffered, then the sender waits.
    assert produced == [0, 1, 2] and ch.bufferedAmount <= B.HIGH_WATER + B.CHUNK + 5
    for _ in range(10):
        ch.drain_all()
        await asyncio.sleep(0.01)
    assert produced == list(range(8))
    last = B.parse_api_chunk(ch.sent[-1])
    assert last == (5, True, b"")
    total = sum(len(B.parse_api_chunk(f)[2]) for f in ch.sent[1:])
    assert total == 8 * 512 * 1024


async def test_a_cancel_stops_the_route_and_pairing_routes_are_denied():
    stopped = asyncio.Event()

    async def asgi(scope, receive, send):
        await send({"type": "http.response.start", "status": 200, "headers": []})
        try:
            while True:
                await send({"type": "http.response.body", "body": b"y" * 100, "more_body": True})
                await asyncio.sleep(0.01)
        finally:
            stopped.set()

    ch = _FakeChannel()
    bridge = B.ApiBridge(ch, asgi, "tok")
    ch.emit("message", json.dumps({"t": "req", "id": 1, "method": "GET", "path": "/s", "body": False}))
    await asyncio.sleep(0.05)
    ch.emit("message", json.dumps({"t": "cancel", "id": 1}))
    await asyncio.wait_for(stopped.wait(), 1)
    assert not bridge.requests
    ch.emit("message", json.dumps({"t": "req", "id": 2, "method": "POST", "path": "/v1/vr/pair", "body": False}))
    await asyncio.sleep(0.05)
    res = [json.loads(m) for m in ch.sent if isinstance(m, str)]
    assert res[-1]["id"] == 2 and res[-1]["status"] == 403
    assert B.parse_api_chunk(ch.sent[-1]) == (2, True, b"")


async def test_an_encoded_body_reaches_the_page_decoded():
    import gzip

    payload = np.random.default_rng(2).integers(0, 4, 3 << 20, dtype=np.uint8).tobytes()
    packed = gzip.compress(payload)
    seen_headers = {}

    async def asgi(scope, receive, send):
        seen_headers.update(dict(scope["headers"]))
        await send({"type": "http.response.start", "status": 200,
                    "headers": [(b"Content-Encoding", b"gzip"), (b"Content-Length", str(len(packed)).encode()),
                                (b"Vary", b"a"), (b"Vary", b"b")]})
        for at in range(0, len(packed), 100_000):
            await send({"type": "http.response.body", "body": packed[at:at + 100_000],
                        "more_body": at + 100_000 < len(packed)})

    ch = _FakeChannel()
    B.ApiBridge(ch, asgi, "tok")
    ch.emit("message", json.dumps({"t": "req", "id": 3, "method": "GET", "path": "/g",
                                   "headers": {"accept-encoding": "gzip"}, "body": False}))
    got = bytearray()
    for _ in range(400):
        await asyncio.sleep(0.005)
        ch.drain_all()
    res = json.loads(ch.sent[0])
    assert res["headers"] == {"vary": "a, b"}             # no encoding, no encoded length
    for frame in ch.sent[1:]:
        got += B.parse_api_chunk(frame)[2]
    assert bytes(got) == payload
    assert b"accept-encoding" not in seen_headers
    assert all(len(f) <= B.CHUNK + 5 for f in ch.sent[1:])


async def test_what_the_page_sends_and_the_route_does_not_read_is_bounded(monkeypatch):
    monkeypatch.setattr(B, "MAX_WS_QUEUED", 100_000)
    monkeypatch.setattr(B, "MAX_API_QUEUED", 100_000)

    async def never_reads(scope, receive, send):
        if scope["type"] == "websocket":
            await receive()                               # the connect
            await send({"type": "websocket.accept"})
        await asyncio.Event().wait()

    ch = _FakeChannel()
    ch.protocol = json.dumps({"path": "/slow", "protocols": []})
    ws = B.WsBridge(ch, never_reads, "tok")
    await asyncio.sleep(0.02)
    for _ in range(10):
        ch.emit("message", bytes([B.WS_BINARY, B.LAST]) + b"x" * 16384)
    closes = [f for f in ch.sent if isinstance(f, bytes) and f[0] == B.WS_CLOSE]
    assert closes and json.loads(closes[0][2:])["code"] == 1009
    assert ws.queued <= 100_000
    ws.close()

    api = _FakeChannel()
    bridge = B.ApiBridge(api, never_reads, "tok")
    for rid in (1, 3):
        api.emit("message", json.dumps({"t": "req", "id": rid, "method": "POST", "path": "/u", "body": True}))
        for _ in range(4):
            api.emit("message", B.api_chunk(rid, b"y" * 16384, False))
    texts = [json.loads(m) for m in api.sent if isinstance(m, str)]
    assert {"t": "cancel", "id": 3} in texts and bridge.queued <= 100_000
    bridge.close()
    await asyncio.sleep(0.02)
    assert bridge.queued == 0

    # Empty frames cost nothing to send, so they are not kept: an empty API
    # chunk that is not the last is dropped, an empty WebSocket fragment that
    # is not the last closes the socket.
    api = _FakeChannel()
    bridge = B.ApiBridge(api, never_reads, "tok")
    api.emit("message", json.dumps({"t": "req", "id": 5, "method": "POST", "path": "/u", "body": True}))
    for _ in range(1000):
        api.emit("message", B.api_chunk(5, b"", False))
    assert bridge.requests[5].body.qsize() == 0
    bridge.close()
    ch = _FakeChannel()
    ch.protocol = json.dumps({"path": "/slow", "protocols": []})
    ws = B.WsBridge(ch, never_reads, "tok")
    await asyncio.sleep(0.02)
    ch.emit("message", bytes([B.WS_BINARY, 0]))
    closes = [f for f in ch.sent if isinstance(f, bytes) and f[0] == B.WS_CLOSE]
    assert closes and json.loads(closes[0][2:])["code"] == 1002
    ws.close()
    await asyncio.sleep(0.02)
    assert ws.queued == 0 and ws.incoming.qsize() == 0


async def test_a_finished_socket_is_let_go_and_sockets_per_headset_are_bounded(monkeypatch):
    async def accept_and_end(scope, receive, send):
        await receive()
        await send({"type": "websocket.accept"})

    host = H.PairHost("ws://test")
    h = H.Headset("h", "tok", {}, accept_and_end)
    bridges: list = []

    def channel():
        ch = _FakeChannel()
        ch.label = "ws"
        ch.protocol = json.dumps({"path": "/x", "protocols": []})
        return ch

    host._attach(h, None, channel(), bridges)
    assert len(bridges) == 1
    await asyncio.sleep(0.05)
    assert bridges == []                                 # the route returned: let go
    monkeypatch.setattr(H, "MAX_WS_PER_HEADSET", 2)

    async def hold(scope, receive, send):
        await receive()
        await send({"type": "websocket.accept"})
        await asyncio.Event().wait()
    h.app = hold
    extra = [channel() for _ in range(3)]
    for ch in extra:
        host._attach(h, None, ch, bridges)
    assert len(bridges) == 2 and extra[2].readyState == "closed"
    for b in list(bridges):
        b.close()

    # One api channel per connection: a second would bring a budget of its own.
    pc = object()
    first, second = _FakeChannel(), _FakeChannel()
    first.label = second.label = "api"
    host._attach(h, pc, first, bridges)
    host._attach(h, pc, second, bridges)
    assert first.readyState == "open" and second.readyState == "closed"
    first.close()                                         # gone: a new one may come
    third = _FakeChannel()
    third.label = "api"
    host._attach(h, pc, third, bridges)
    assert third.readyState == "open"


async def test_empty_websocket_messages_count_against_the_budget(monkeypatch):
    monkeypatch.setattr(B, "MAX_WS_QUEUED", 100 * B.MESSAGE_COST)

    async def never_reads(scope, receive, send):
        await receive()
        await send({"type": "websocket.accept"})
        await asyncio.Event().wait()

    ch = _FakeChannel()
    ch.protocol = json.dumps({"path": "/slow", "protocols": []})
    ws = B.WsBridge(ch, never_reads, "tok")
    await asyncio.sleep(0.02)
    for _ in range(10_000):
        ch.emit("message", bytes([B.WS_BINARY, B.LAST]))
    assert ws.incoming.qsize() <= 102
    closes = [f for f in ch.sent if isinstance(f, bytes) and f[0] == B.WS_CLOSE]
    assert closes and json.loads(closes[0][2:])["code"] == 1009
    ws.close()


class _FakePC:
    def __init__(self, *_a) -> None:
        self.handlers: dict = {}
        self.connectionState = "new"
        self.closed = False
        self.localDescription = SimpleNamespace(sdp="v=0 answer")

    def on(self, event):
        def add(fn):
            self.handlers[event] = fn
            return fn
        return add

    async def setRemoteDescription(self, _d):
        pass

    async def createAnswer(self):
        return None

    async def setLocalDescription(self, _d):
        pass

    async def close(self):
        self.closed = True
        self.connectionState = "closed"


async def test_a_disconnected_headset_cannot_come_back_and_resumes_replace_each_other(monkeypatch):
    monkeypatch.setitem(sys.modules, "aiortc", SimpleNamespace(
        RTCPeerConnection=_FakePC, RTCSessionDescription=lambda **_k: None))
    host = H.PairHost("ws://test")
    host._rtc_configuration = lambda: None

    async def send(_m):
        pass
    host._send = send

    async def up(peer):
        peer.pc.connectionState = "connected"
        await peer.pc.handlers["connectionstatechange"]()

    # Disconnected while a resume negotiates: the resume never comes up.
    h = H.Headset("h1", "tok", {}, None)
    host.headsets[h.id] = h
    peer = H._Peer(headset=h)
    host.peers["p"] = peer
    await host._answer("p", peer, h, "v=0 offer")
    await host.disconnect(h, "disconnected")
    await up(peer)
    await asyncio.sleep(0)
    assert peer.pc.closed and not h.connected and h.pc is None

    # Two resumes coming up together: the last one is kept, the others closed.
    h = H.Headset("h2", "tok", {}, None)
    host.headsets[h.id] = h
    peers = [H._Peer(headset=h) for _ in range(3)]
    for i, p in enumerate(peers):
        host.peers[f"q{i}"] = p
        await host._answer(f"q{i}", p, h, "v=0 offer")
    await asyncio.gather(*(up(p) for p in peers))
    assert h.pc is peers[-1].pc and not peers[-1].pc.closed
    assert all(p.pc.closed for p in peers[:-1])
    await host.close()


# ── Bundling aiortc changes nothing for the desktop ───────────────────────────


@pytest.mark.skipif(H.aiortc_missing() is not None, reason="needs aiortc importable")
async def test_a_local_backend_with_aiortc_still_advertises_jpeg_only(monkeypatch):
    """The release bundles carry aiortc (>= 1.10) for pairing. Before, a
    desktop's local backend had none and advertised JPEG only, so the remote
    view stayed on JPEG; it must still, unless the H.264 stream is asked for
    or aiortc's encoder factory was patched for NVENC (the cluster image)."""
    import app.main as main_module
    if not main_module._render_mounted:
        pytest.skip("server-side rendering is not mounted here")
    from app import webrtc_session as W
    from app.render_session import _stereo_webrtc as SW

    from aiortc.codecs import h264 as aiortc_h264

    monkeypatch.delenv(W.H264_OPT_IN_ENV, raising=False)
    # aiortc as the bundles carry it: no encoder factory to patch, and the
    # installer not run yet in this process.
    monkeypatch.delattr(aiortc_h264, "create_encoder_context", raising=False)
    monkeypatch.setattr(W, "_NVENC_INSTALLED", False)
    monkeypatch.setattr(W, "_ENCODER_FACTORY_PATCHED", False)
    jpeg_only = ["jpeg-ws", "jpeg-ws-push"]
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as http:
        async def transports():
            r = await http.get("/v1/render/diagnostics", headers=AUTH)
            assert r.status_code == 200
            return r.json()["transports"]

        assert await transports() == jpeg_only
        # Offers for the H.264 stream are refused as before (503).
        r = await http.post("/v1/render/any/sdp/offer", json={"sdp": "v=0", "type": "offer"}, headers=AUTH)
        assert r.status_code == 503
        monkeypatch.setenv(W.H264_OPT_IN_ENV, "1")
        assert await transports() == jpeg_only + ["h264-webrtc", "h265-webrtc"]
        monkeypatch.delenv(W.H264_OPT_IN_ENV)
        assert await transports() == jpeg_only
        # aiortc < 1.10, as the cluster image: the factory is there, the
        # installer patches it for NVENC, and the stream is advertised.
        monkeypatch.setattr(aiortc_h264, "create_encoder_context", lambda *a: None, raising=False)
        monkeypatch.setattr(W, "_NVENC_INSTALLED", False)
        assert await transports() == jpeg_only + ["h264-webrtc", "h265-webrtc"]
        assert W._ENCODER_FACTORY_PATCHED
    # The headset's stereo video (#189) has its own link: available with aiortc.
    assert SW.rtc_available() is None


# ── Routes without a pairing service ──────────────────────────────────────────


@pytest_asyncio.fixture
async def http(monkeypatch):
    monkeypatch.setattr(H, "_HOST", None)
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        yield client
    await H.shutdown()


async def test_pairing_is_off_without_a_service_address(http, monkeypatch):
    monkeypatch.delenv(H.PAIR_URL_ENV, raising=False)
    r = await http.post("/v1/vr/pair", json={"session": "s", "dataset": "d", "path": "/p"}, headers=AUTH)
    assert r.status_code == 409 and r.json()["reason"] == "not-configured"
    assert H.PAIR_URL_ENV in r.json()["detail"]
    r = await http.get("/v1/vr/pair", headers=AUTH)
    assert r.json() == {"enabled": False, "pending": [], "connected": []}
    assert (await http.delete("/v1/vr/pair/ABCDEF", headers=AUTH)).status_code == 404
    assert (await http.get("/v1/vr/pair")).status_code == 401


async def test_pairing_is_off_without_webrtc_support(http, monkeypatch):
    monkeypatch.setenv(H.PAIR_URL_ENV, "ws://127.0.0.1:8810")
    monkeypatch.setattr(H, "aiortc_missing", lambda: "aiortc is not installed")
    r = await http.post("/v1/vr/pair", json={"session": "s", "dataset": "d"}, headers=AUTH)
    assert r.status_code == 409 and r.json()["reason"] == "not-configured"
    assert "aiortc" in r.json()["detail"]
    assert (await http.get("/v1/vr/pair", headers=AUTH)).json()["enabled"] is False


async def test_an_unreachable_service_answers_409(http, monkeypatch):
    if H.aiortc_missing():
        pytest.skip(H.aiortc_missing())
    monkeypatch.setenv(H.PAIR_URL_ENV, "ws://127.0.0.1:8819/")       # nothing listens there
    monkeypatch.setattr(H, "CONNECT_WAIT_S", 0.5)
    r = await http.post("/v1/vr/pair", json={"session": "s", "dataset": "d"}, headers=AUTH)
    assert r.status_code == 409 and r.json()["reason"] == "unreachable"


async def test_a_pairing_request_never_carries_the_token_and_keeps_extra_params(http):
    bad = await http.post("/v1/vr/pair", json={"session": "s", "token": "x"}, headers=AUTH)
    assert bad.status_code == 422
    assert (await http.post("/v1/vr/pair", json={"session": "s", "nested": {"a": 1}},
                            headers=AUTH)).status_code == 422
    from app.routers.vr_pair import PairRequest
    req = PairRequest(session="s", dataset="d", path="/p", kind="velocity", box=125.0)
    assert req.params() == {"session": "s", "dataset": "d", "path": "/p", "kind": "velocity", "box": 125.0}


def test_the_host_endpoint_is_under_the_service_address():
    assert H.host_endpoint("wss://pair.example.org") == "wss://pair.example.org/v1/host"
    assert H.host_endpoint("wss://www.example.org/vr-pair/") == "wss://www.example.org/vr-pair/v1/host"
    assert H.host_endpoint("ws://127.0.0.1:8810/v1/host") == "ws://127.0.0.1:8810/v1/host"
    # A host key the service requires rides in the one address.
    assert H.host_endpoint("wss://pair.example.org/?key=k1") == "wss://pair.example.org/v1/host?key=k1"
    assert "k1" not in H._service_label("wss://pair.example.org/?key=k1")


def test_page_url_is_the_loader_with_the_code():
    assert H.page_url("https://vr.example.org/", "K7Q4MX") == "https://vr.example.org/#pair=K7Q4MX"
    assert H.page_url("https://vr.example.org/vr/#old", "K7Q4MX") == "https://vr.example.org/vr/#pair=K7Q4MX"
    assert H.page_url("", "K7Q4MX") is None


# ── End to end over WebRTC ────────────────────────────────────────────────────

needs_rtc = pytest.mark.skipif(H.aiortc_missing() is not None, reason=str(H.aiortc_missing()))


def _service_module():
    spec = importlib.util.spec_from_file_location("vr_pair_service", SERVICE)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["vr_pair_service"] = mod
    spec.loader.exec_module(mod)
    return mod


class _Clock:
    def __init__(self) -> None:
        self.t = 5000.0

    def __call__(self) -> float:
        return self.t


@pytest_asyncio.fixture
async def paired(monkeypatch, tmp_path):
    if not SERVICE.is_file():
        pytest.skip("the pairing service is not in this tree")
    svc = _service_module()
    clock = _Clock()
    service = svc.PairService(svc.Config(), clock=clock)
    async with service.serve("127.0.0.1", 0) as server:
        url = f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}"
        monkeypatch.setenv(H.PAIR_URL_ENV, url)
        monkeypatch.setenv(H.PAGE_URL_ENV, "https://vr.example.test/")
        monkeypatch.setattr(H, "_HOST", None)
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test",
                               timeout=60) as client:
            pages: list[Page] = []
            yield SimpleNamespace(service=service, clock=clock, url=url, client=client, pages=pages,
                                  tmp=tmp_path)
            for p in pages:
                await p.close()
        await H.shutdown()


class Page:
    """The headset's page, as an aiortc peer speaking the contract."""

    def __init__(self, env) -> None:
        from aiortc import RTCPeerConnection  # noqa: PLC0415

        env.pages.append(self)
        self.env = env
        self.pc = RTCPeerConnection()
        self.ctl = self.pc.createDataChannel("ctl")
        self.api = self.pc.createDataChannel("api")
        self.ctl_in: asyncio.Queue = asyncio.Queue()
        self.ctl_seen: list[str] = []
        self.api_in: dict[int, asyncio.Queue] = {}
        self.api_seen_text: list[str] = []
        self.next_id = 1
        self.ctl.on("message", self._on_ctl)
        self.api.on("message", self._on_api)

    def _on_ctl(self, m) -> None:
        self.ctl_seen.append(m)
        self.ctl_in.put_nowait(json.loads(m))

    def _on_api(self, m) -> None:
        if isinstance(m, str):
            self.api_seen_text.append(m)
            msg = json.loads(m)
            q = self.api_in.get(msg.get("id"))
            if q is not None:
                q.put_nowait(msg)
        else:
            rid, last, payload = B.parse_api_chunk(m)
            q = self.api_in.get(rid)
            if q is not None:
                q.put_nowait(("chunk", payload, last))

    async def join(self, code: str) -> str | None:
        """None when connected, else the service's error reason."""
        from aiortc import RTCSessionDescription  # noqa: PLC0415
        from websockets.asyncio.client import connect  # noqa: PLC0415

        await self.pc.setLocalDescription(await self.pc.createOffer())
        async with connect(f"{self.env.url}/v1/join", origin=PAGE_ORIGIN, proxy=None) as ws:
            first = json.loads(await ws.recv())
            assert first["type"] == "ice_servers"
            await ws.send(json.dumps({"v": 1, "type": "join", "code": code,
                                      "offer": self.pc.localDescription.sdp}))
            msg = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if msg["type"] == "error":
                return msg["reason"]
            assert msg["type"] == "answer"
            await self.pc.setRemoteDescription(RTCSessionDescription(sdp=msg["sdp"], type="answer"))
        await self._until(lambda: self.ctl.readyState == "open" and self.api.readyState == "open")
        return None

    @staticmethod
    async def _until(cond, timeout: float = 20.0) -> None:
        end = time.monotonic() + timeout
        while not cond():
            if time.monotonic() > end:
                raise AssertionError("timed out")
            await asyncio.sleep(0.01)

    async def hello(self, resume=None) -> dict:
        self.ctl.send(json.dumps({"t": "hello", "resume": resume}))
        return await asyncio.wait_for(self.ctl_in.get(), 20)

    async def start(self, method: str, path: str, body: bytes | None = None, headers=None):
        rid = self.next_id
        self.next_id += 1
        q: asyncio.Queue = asyncio.Queue()
        self.api_in[rid] = q
        self.api.send(json.dumps({"t": "req", "id": rid, "method": method, "path": path,
                                  "headers": headers or {}, "body": body is not None}))
        if body is not None:
            frames = [B.api_chunk(rid, body[i:i + B.CHUNK], i + B.CHUNK >= len(body))
                      for i in range(0, len(body), B.CHUNK)] or [B.api_chunk(rid, b"", True)]
            await B.send_frames(self.api, frames)
        return rid, q

    async def fetch(self, method: str, path: str, body: bytes | None = None, headers=None):
        _, q = await self.start(method, path, body, headers)
        res = await asyncio.wait_for(q.get(), 30)
        assert res.get("t") == "res", res
        data = bytearray()
        while True:
            item = await asyncio.wait_for(q.get(), 30)
            assert isinstance(item, tuple), item
            data += item[1]
            if item[2]:
                break
        return res["status"], dict(res["headers"]), bytes(data)

    async def open_ws(self, path: str, protocols: list[str]):
        ch = self.pc.createDataChannel("ws", protocol=json.dumps({"path": path, "protocols": protocols}))
        asm = B.WsAssembler()
        q: asyncio.Queue = asyncio.Queue()

        def on_message(m):
            done = asm.feed(m)
            if done is not None:
                q.put_nowait(done)
        ch.on("message", on_message)
        # A refused socket may be closed by the backend as soon as it opens.
        await self._until(lambda: ch.readyState != "connecting")
        return ch, q

    async def close(self) -> None:
        await self.pc.close()


async def _code(env, **params) -> dict:
    body = {"session": "S1", "dataset": "D1", "path": "/data/cube.fits", **params}
    r = await env.client.post("/v1/vr/pair", json=body, headers=AUTH)
    assert r.status_code == 200, r.text
    return r.json()


@needs_rtc
async def test_a_page_pairs_and_fetches_a_reduced_cube(paired, synthetic_fits_cube):
    env = paired
    opened = (await env.client.post("/v1/datasets/open", json={"path": str(synthetic_fits_cube)},
                                    headers=AUTH)).json()
    t0 = time.perf_counter()
    got = await _code(env, session=opened["session_id"], dataset=opened["dataset_id"],
                      path=str(synthetic_fits_cube), kind="cube")
    assert got["page_url"] == f"https://vr.example.test/#pair={got['code']}"
    assert got["expires_at"] > time.time() + 250
    listing = (await env.client.get("/v1/vr/pair", headers=AUTH)).json()
    assert listing["enabled"] and listing["pending"] == [{"code": got["code"], "expires_at": got["expires_at"]}]

    page = Page(env)
    t_join = time.perf_counter()
    assert await page.join(got["code"]) is None
    welcome = await page.hello()
    t_welcome = time.perf_counter()
    assert welcome["t"] == "welcome"
    assert (welcome["session"], welcome["dataset"]) == (opened["session_id"], opened["dataset_id"])
    assert welcome["params"]["kind"] == "cube" and welcome["params"]["path"] == str(synthetic_fits_cube)
    assert welcome["resume_code"] and welcome["resume_expires_in"] > 3600
    assert welcome["page_build"]

    # GET first, through the same channel as the page's assets.
    status, headers, body = await page.fetch("GET", "/v1/vr/info")
    t_first = time.perf_counter()
    assert status == 200 and json.loads(body)["vr_api"] >= 1
    status, _, html = await page.fetch("GET", "/vr/")
    assert status in (200, 404)
    # The reduced cube: a POST with a JSON body, as the page asks for it.
    req = {"dataset_id": opened["dataset_id"], "max_dim": 32, "factor_xy": 0, "factor_z": 0}
    status, headers, body = await page.fetch(
        "POST", "/v1/cube/reduced_bin", json.dumps(req).encode(),
        {"Content-Type": "application/json", "X-Visivo-Session": opened["session_id"]})
    assert status == 200, body[:200]
    direct = await env.client.post("/v1/cube/reduced_bin", json=req,
                                   headers={**AUTH, "X-Visivo-Session": opened["session_id"]})
    assert body == direct.content and len(body) > 1000
    assert headers["x-visivo-bin-factors"] == direct.headers["x-visivo-bin-factors"]

    listing = (await env.client.get("/v1/vr/pair", headers=AUTH)).json()
    assert listing["pending"] == []
    [headset] = listing["connected"]
    assert headset["dataset"] == opened["dataset_id"] and headset["transport"] == "webrtc-direct"

    # The token never reached the page.
    seen = "".join(page.ctl_seen + page.api_seen_text)
    assert _TEST_TOKEN not in seen
    print(f"\n[vr-pair] join to welcome {1000 * (t_welcome - t_join):.0f} ms, "
          f"join to first API byte {1000 * (t_first - t_join):.0f} ms "
          f"(code issued in {1000 * (t_join - t0):.0f} ms)")


@needs_rtc
async def test_100_mb_stream_through_the_bridge_with_bounded_memory(paired):
    from fastapi.responses import StreamingResponse

    block = np.random.default_rng(1).integers(0, 256, 1 << 20, dtype=np.uint8).tobytes()
    progress = {"blocks": 0, "closed": False}

    async def big():
        async def gen():
            try:
                for _ in range(100):
                    progress["blocks"] += 1
                    yield block
            finally:
                progress["closed"] = True
        return StreamingResponse(gen(), media_type="application/octet-stream")

    app.add_api_route("/v1/test-vr-pair/big", big, methods=["GET"])
    try:
        env = paired
        page = Page(env)
        assert await page.join((await _code(env))["code"]) is None
        await page.hello()

        async def pull() -> tuple[int, float, float]:
            _, q = await page.start("GET", "/v1/test-vr-pair/big")
            t0 = time.perf_counter()
            res = await asyncio.wait_for(q.get(), 30)
            assert res["status"] == 200
            n, first = 0, None
            while True:
                _, payload, last = await asyncio.wait_for(q.get(), 30)
                if first is None:
                    first = time.perf_counter() - t0
                n += len(payload)
                if last:
                    return n, time.perf_counter() - t0, first

        rss0 = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        n, dt, first = await pull()
        assert n == 100 << 20
        rss1 = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        tracemalloc.start()
        base = tracemalloc.get_traced_memory()[0]
        n2, dt2, _ = await pull()
        peak = tracemalloc.get_traced_memory()[1] - base
        tracemalloc.stop()
        assert n2 == 100 << 20
        # Far below the 100 MB that went through: one 1 MiB block of the
        # route, about 1 MiB waiting in the channel, and the SCTP windows.
        assert peak < 24 << 20, peak
        scale = 1 if sys.platform == "darwin" else 1024
        print(f"\n[vr-pair] 100 MiB in {dt:.2f} s ({100 / dt:.1f} MiB/s), first byte after "
              f"{1000 * first:.0f} ms; traced peak {peak / 2 ** 20:.1f} MiB (traced run "
              f"{100 / dt2:.1f} MiB/s); max RSS growth {(rss1 - rss0) * scale / 2 ** 20:.1f} MiB")
    finally:
        app.router.routes[:] = [r for r in app.router.routes if getattr(r, "path", "") != "/v1/test-vr-pair/big"]


@needs_rtc
async def test_a_cancel_stops_a_stream_in_flight(paired):
    from fastapi.responses import StreamingResponse

    state = {"sent": 0, "closed": asyncio.Event()}

    async def endless():
        async def gen():
            try:
                while True:
                    state["sent"] += 1
                    yield b"z" * 65536
                    await asyncio.sleep(0)
            finally:
                state["closed"].set()
        return StreamingResponse(gen())

    app.add_api_route("/v1/test-vr-pair/endless", endless, methods=["GET"])
    try:
        env = paired
        page = Page(env)
        assert await page.join((await _code(env))["code"]) is None
        rid, q = await page.start("GET", "/v1/test-vr-pair/endless")
        assert (await asyncio.wait_for(q.get(), 20))["status"] == 200
        await asyncio.wait_for(q.get(), 20)
        page.api.send(json.dumps({"t": "cancel", "id": rid}))
        # The route is no longer asked for its body (its generator is closed
        # when it is collected; what matters is that nothing pulls it).
        await asyncio.sleep(0.3)
        sent = state["sent"]
        await asyncio.sleep(0.5)
        assert state["sent"] == sent
        assert not H.existing_host().headsets[next(iter(H.existing_host().headsets))].bridges[0].requests
        # The channel serves the next request at once.
        status, _, _ = await page.fetch("GET", "/v1/vr/info")
        assert status == 200
    finally:
        app.router.routes[:] = [r for r in app.router.routes
                                if getattr(r, "path", "") != "/v1/test-vr-pair/endless"]


@needs_rtc
async def test_a_render_session_websocket_with_stereo_signalling_and_big_frames(paired, monkeypatch):
    import app.main as main_module
    if not main_module._render_mounted:
        pytest.skip("server-side rendering is not mounted here")
    import app.routers.render as render_router
    from app.render_session import _stereo as S
    from app.render_session import _stereo_webrtc as W
    from tests.test_render_stereo import pose
    from tests.test_render_stereo_webrtc import OFFER, _FakeLink, _Session

    big = bytes(range(256)) * 6000                    # 1.5 MB: a VXR1 stereo frame's size

    class BigSession(_Session):
        async def render_frame(self):
            frame = await super().render_frame()
            if isinstance(frame, bytes):
                return b"\xff\xd8" + big
            if frame.header["color"]["format"] == "jpeg":
                return S.XRFrame(frame.header, b"\xff\xd8" + big, b"DD")
            return frame

    sess = BigSession()
    tokens = []

    async def owned(session_id, token):
        tokens.append(token)
        return sess if session_id == "s1" and token == _TEST_TOKEN else None

    monkeypatch.setattr(render_router, "_owned_render_session", owned)
    monkeypatch.setattr(render_router, "_PUSH_POLL_S", 0.02)
    monkeypatch.setattr(W, "rtc_available", lambda: None)
    monkeypatch.setattr(W, "StereoRtcLink", _FakeLink)
    _FakeLink.made.clear()

    env = paired
    page = Page(env)
    assert await page.join((await _code(env))["code"]) is None
    ch, q = await page.open_ws("/v1/render/s1/ws", ["visivo.v1"])
    kind, payload = await asyncio.wait_for(q.get(), 20)
    assert (kind, json.loads(payload)) == (B.WS_OPEN, {"protocol": "visivo.v1"})
    assert tokens == [_TEST_TOKEN]                 # the bridge's token, not one from the page

    async def send(kind, data: bytes):
        await B.send_frames(ch, B.ws_frames(kind, data))

    await send(B.WS_TEXT, json.dumps({"type": "request_frame"}).encode())
    kind, payload = await asyncio.wait_for(q.get(), 20)
    assert kind == B.WS_BINARY and payload == b"\xff\xd8" + big

    # Stereo WebRTC (#189) negotiates over this very socket.
    await send(B.WS_TEXT, json.dumps({"type": "xr_rtc_offer", "sdp": OFFER, "crf": 24}).encode())
    kind, payload = await asyncio.wait_for(q.get(), 20)
    answer = json.loads(payload)
    assert kind == B.WS_TEXT and answer["type"] == "xr_rtc_answer" and answer["sdp"] == "v=0 answer"
    await send(B.WS_TEXT, json.dumps(pose(8, 256, frame_color="jpeg")).encode())
    await send(B.WS_TEXT, json.dumps({"type": "request_frame"}).encode())
    kind, payload = await asyncio.wait_for(q.get(), 20)
    header, color, depth = S.parse_frame(payload)
    assert kind == B.WS_BINARY and len(payload) > 1 << 20
    assert header["pose_id"] == 8 and color == b"\xff\xd8" + big and depth == b"DD"

    # A big text message from the page crosses too (here: an unknown command,
    # which the route ignores), and the socket goes on.
    await send(B.WS_TEXT, json.dumps({"type": "noop", "pad": "x" * (1 << 20)}).encode())
    await send(B.WS_TEXT, json.dumps({"type": "xr_stop"}).encode())
    await send(B.WS_TEXT, json.dumps({"type": "request_frame"}).encode())
    kind, payload = await asyncio.wait_for(q.get(), 20)
    assert kind == B.WS_BINARY

    # A session that is not this token's is refused with the route's own code.
    ch2, q2 = await page.open_ws("/v1/render/other/ws", ["visivo.v1"])
    kind, payload = await asyncio.wait_for(q2.get(), 20)
    assert kind == B.WS_CLOSE and json.loads(payload)["code"] == 1011

    # The page closes: its close is echoed, and the route sees the disconnect.
    await send(B.WS_CLOSE, json.dumps({"code": 1000, "reason": "bye"}).encode())
    kind, payload = await asyncio.wait_for(q.get(), 20)
    assert kind == B.WS_CLOSE and json.loads(payload)["code"] == 1000
    await Page._until(lambda: _FakeLink.made and _FakeLink.made[0].closed, 10)
    await Page._until(lambda: ch.readyState == "closed", 10)


@needs_rtc
async def test_an_echo_websocket_carries_big_messages_both_ways(paired):
    async def echo(websocket: WebSocket):
        await websocket.accept(subprotocol="visivo.v1")
        while True:
            m = await websocket.receive()
            if m["type"] == "websocket.disconnect":
                return
            if m.get("text") is not None:
                await websocket.send_text(m["text"])
            else:
                await websocket.send_bytes(m["bytes"])

    app.add_api_websocket_route("/v1/test-vr-pair/echo", echo)
    try:
        env = paired
        page = Page(env)
        assert await page.join((await _code(env))["code"]) is None
        ch, q = await page.open_ws("/v1/test-vr-pair/echo?token=from-page", ["visivo.v1", "visivo-token.x"])
        opened = await asyncio.wait_for(q.get(), 20)
        assert opened[0] == B.WS_OPEN, opened
        blob = np.random.default_rng(3).integers(0, 256, 3 << 19, dtype=np.uint8).tobytes()
        text = "ä" * (700 * 1024)
        await B.send_frames(ch, B.ws_frames(B.WS_BINARY, blob))
        await B.send_frames(ch, B.ws_frames(B.WS_TEXT, text.encode()))
        assert await asyncio.wait_for(q.get(), 30) == (B.WS_BINARY, blob)
        kind, payload = await asyncio.wait_for(q.get(), 30)
        assert kind == B.WS_TEXT and payload.decode() == text
    finally:
        app.router.routes[:] = [r for r in app.router.routes
                                if getattr(r, "path", "") != "/v1/test-vr-pair/echo"]


@needs_rtc
async def test_used_revoked_and_expired_codes_fail_cleanly(paired):
    env = paired
    code = (await _code(env))["code"]
    first = Page(env)
    assert await first.join(code) is None
    assert await Page(env).join(code) == "used"

    revoked = (await _code(env))["code"]
    r = await env.client.delete(f"/v1/vr/pair/{revoked}", headers=AUTH)
    assert r.json() == {"result": "revoked"}
    assert await Page(env).join(revoked) == "expired"
    assert (await env.client.get("/v1/vr/pair", headers=AUTH)).json()["pending"] == []

    late = (await _code(env))["code"]
    env.clock.t += 301
    assert await Page(env).join(late) == "expired"
    assert await Page(env).join("ZZZZZZ") == "unknown-code"


@needs_rtc
async def test_the_desktop_disconnects_a_headset(paired):
    env = paired
    page = Page(env)
    assert await page.join((await _code(env))["code"]) is None
    welcome = await page.hello()
    [headset] = (await env.client.get("/v1/vr/pair", headers=AUTH)).json()["connected"]
    r = await env.client.delete(f"/v1/vr/pair/{headset['id']}", headers=AUTH)
    assert r.json() == {"result": "disconnected"}
    bye = await asyncio.wait_for(page.ctl_in.get(), 10)
    assert bye == {"t": "bye", "reason": "disconnected"}
    assert (await env.client.get("/v1/vr/pair", headers=AUTH)).json()["connected"] == []
    # Its resume code went with it.
    assert await Page(env).join(welcome["resume_code"]) == "expired"


@needs_rtc
async def test_a_page_resumes_after_its_connection_dropped(paired):
    env = paired
    page = Page(env)
    assert await page.join((await _code(env, kind="velocity", box=125.0))["code"]) is None
    welcome = await page.hello()
    [before] = (await env.client.get("/v1/vr/pair", headers=AUTH)).json()["connected"]
    await page.pc.close()                      # Wi-Fi gone, headset asleep
    await Page._until(lambda: not H.existing_host().listing(None)["connected"], 20)

    again = Page(env)
    assert await again.join(welcome["resume_code"]) is None
    back = await again.hello(resume=welcome["resume_code"])
    assert back["resume_code"] == welcome["resume_code"]
    assert back["params"] == welcome["params"] and back["params"]["box"] == 125.0
    [after] = (await env.client.get("/v1/vr/pair", headers=AUTH)).json()["connected"]
    assert after["id"] == before["id"]
    status, _, _ = await again.fetch("GET", "/v1/vr/info")
    assert status == 200

    # The backend loses the pairing service and comes back: the resume code
    # is registered again, and a page can still use it.
    host = H.existing_host()
    await host.ws.close()
    await Page._until(lambda: host.ws is not None and host._connected.is_set(), 30)
    await asyncio.sleep(0.2)
    await again.pc.close()
    third = Page(env)
    assert await third.join(welcome["resume_code"]) is None
    assert (await third.hello(resume=welcome["resume_code"]))["session"] == "S1"
