"""The spectator view (issue #281): the desktop watches a headset page of
its session. The watch command, the lease, the frame routes and their
bounds, the snapshot PNG, and the frame post through the pairing bridge."""
from __future__ import annotations

import asyncio
import io
import json
import os
import time

import pytest
from httpx import AsyncClient
from PIL import Image

from app import headset_hub as H
from app import headset_spectator as S
from app import vr_bridge as B
from tests.conftest import _TEST_TOKEN


@pytest.fixture(autouse=True)
def _drop_the_sessions_made():
    from app.sessions import REGISTRY
    before = set(REGISTRY._sessions)
    yield
    with REGISTRY._lock:
        made = [sid for sid in REGISTRY._sessions if sid not in before]
        dropped = [REGISTRY._sessions.pop(sid) for sid in made]
    for s in dropped:
        s.close()
    H.reopen()


def _jpeg(w=64, h=48, colour=(200, 30, 30), progressive=False) -> bytes:
    buf = io.BytesIO()
    Image.new("RGB", (w, h), colour).save(buf, format="JPEG", quality=80, progressive=progressive)
    return buf.getvalue()


async def _session(client: AsyncClient, auth_headers: dict) -> dict:
    r = await client.post("/v1/sessions/new", json={}, headers=auth_headers)
    assert r.status_code == 200, r.text
    return {**auth_headers, "X-Visivo-Session": r.json()["session_id"]}


def _hub(headers: dict) -> H.HeadsetHub:
    from app.sessions import REGISTRY
    return H.hub_for(REGISTRY._sessions[headers["X-Visivo-Session"]])


async def _hello(client, headers, cid="page-1", label="Quest 3"):
    r = await client.post("/v1/vr/headset/hello",
                          json={"client": cid, "transport": "adb", "label": label}, headers=headers)
    assert r.status_code == 200, r.text


async def _poll(client, headers, cid="page-1", after=0):
    r = await client.get("/v1/vr/headset/commands", params={"client": cid, "after": after, "wait": 0},
                         headers=headers)
    assert r.status_code == 200, r.text
    return r.json()


async def _watch(http, headers, **body):
    return await http.post("/v1/vr/headset/spectator", json={"on": True, **body}, headers=headers)


async def _post_frame(client, headers, data, cid="page-1", ctype="image/jpeg", **params):
    return await client.post("/v1/vr/headset/spectator/frame", params={"client": cid, **params},
                             content=data, headers={**headers, "Content-Type": ctype})


async def _read(client, headers, cid="page-1", after=0, wait=0.0):
    return await client.get("/v1/vr/headset/spectator/frame",
                            params={"client": cid, "after": after, "wait": wait}, headers=headers)


def _next_frame_allowed(headers, cid="page-1"):
    """The rate cap counts from the last frame stored: move it back."""
    f = _hub(headers).pages[cid].feed.frame
    if f is not None:
        f.received -= 1.0


# ── Watching ──────────────────────────────────────────────────────────────────


async def test_watch_queues_a_command_for_the_page_and_stop_takes_it_back(client, auth_headers):
    h = await _session(client, auth_headers)
    assert (await _watch(client, h)).status_code == 404          # no page yet
    await _hello(client, h)
    r = await _watch(client, h)
    assert r.status_code == 200, r.text
    assert r.json() == {"client": "page-1", "on": True, "fps": S.DEFAULT_FPS, "width": S.DEFAULT_WIDTH,
                        "lease_s": S.LEASE_S, "instance": H.INSTANCE}
    # Retuned before the page polled: only the latest watch is handed over.
    assert (await _watch(client, h, client="page-1", fps=24, width=640)).json()["fps"] == 24
    cmds = (await _poll(client, h))["commands"]
    assert [(c["type"], c["on"], c["fps"], c["width"]) for c in cmds] == [("watch", True, 24, 640)]
    r = await client.post("/v1/vr/headset/spectator", json={"on": False}, headers=h)
    assert r.status_code == 200 and r.json()["on"] is False
    cmds = (await _poll(client, h, after=cmds[0]["seq"]))["commands"]
    assert [(c["type"], c["on"]) for c in cmds] == [("watch", False)]
    assert (await _read(client, h)).status_code == 409           # not watched: nothing to read


async def test_watch_parameters_are_bounded(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    for body in ({"fps": 0}, {"fps": S.MAX_FPS + 1}, {"width": S.MIN_WIDTH - 1}, {"width": S.MAX_WIDTH + 1},
                 {"client": "bad client"}):
        assert (await _watch(client, h, **body)).status_code == 422, body
    assert (await _watch(client, h, client="page-2")).status_code == 404


async def test_a_page_that_says_hello_again_while_watched_is_told_again(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    await _watch(client, h, fps=10)
    first = (await _poll(client, h))["commands"][0]
    await _poll(client, h, after=first["seq"])                   # acknowledged
    await _hello(client, h)                                      # reloaded
    again = (await _poll(client, h, after=first["seq"]))["commands"]
    assert [(c["type"], c["on"], c["fps"]) for c in again] == [("watch", True, 10)]
    # Not watched (or the lease over): a hello brings no watch command.
    _hub(h).pages["page-1"].feed.lease_until = 0.0
    await _poll(client, h, after=again[0]["seq"])
    await _hello(client, h)
    assert (await _poll(client, h, after=again[0]["seq"]))["commands"] == []


# ── Frames ────────────────────────────────────────────────────────────────────


async def test_a_frame_goes_from_the_page_to_the_desktop(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    jpg = _jpeg(96, 100)
    r = await _post_frame(client, h, jpg)
    assert r.status_code == 200 and r.json()["watched"] is False and r.json()["stored"] is False
    await _watch(client, h)
    r = await _post_frame(client, h, jpg, eye="left", cost_ms=0.42)
    assert r.status_code == 200, r.text
    assert r.json() == {"watched": True, "seq": 1, "stored": True, "fps": S.DEFAULT_FPS, "width": S.DEFAULT_WIDTH}
    r = await _read(client, h)
    assert r.status_code == 200 and r.content == jpg and r.headers["content-type"] == "image/jpeg"
    assert r.headers["x-visivo-frame-seq"] == "1"
    assert (r.headers["x-visivo-frame-width"], r.headers["x-visivo-frame-height"]) == ("96", "100")
    assert r.headers["x-visivo-frame-eye"] == "left" and r.headers["x-visivo-frame-cost-ms"] == "0.420"
    assert int(r.headers["x-visivo-frame-age-ms"]) >= 0 and r.headers["x-visivo-instance"] == H.INSTANCE
    # Nothing newer: 204 at once, or after the wait.
    assert (await _read(client, h, after=1)).status_code == 204
    t0 = time.monotonic()
    assert (await _read(client, h, after=1, wait=0.3)).status_code == 204
    assert time.monotonic() - t0 >= 0.25
    # A cursor from before a backend restart (above the last frame) counts as 0.
    assert (await _read(client, h, after=99)).headers["x-visivo-frame-seq"] == "1"


async def test_a_waiting_read_wakes_with_the_next_frame(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    await _watch(client, h)
    read = asyncio.ensure_future(_read(client, h, wait=4))
    await asyncio.sleep(0.1)
    t0 = time.monotonic()
    assert (await _post_frame(client, h, _jpeg())).json()["stored"]
    r = await asyncio.wait_for(read, 3)
    assert r.status_code == 200 and time.monotonic() - t0 < 1.0
    # A stop ends a waiting read at once (empty).
    read = asyncio.ensure_future(_read(client, h, after=1, wait=4))
    await asyncio.sleep(0.1)
    await client.post("/v1/vr/headset/spectator", json={"on": False}, headers=h)
    assert (await asyncio.wait_for(read, 3)).status_code == 204


async def test_only_the_latest_frame_is_kept_and_the_rate_is_capped(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    await _watch(client, h)
    a, b = _jpeg(colour=(255, 0, 0)), _jpeg(colour=(0, 0, 255))
    assert (await _post_frame(client, h, a)).json()["stored"]
    # Sooner than 30 frames a second allow: dropped, the first one stays.
    r = await _post_frame(client, h, b)
    assert r.status_code == 200 and r.json() == {**r.json(), "watched": True, "stored": False, "seq": 1}
    assert _hub(h).pages["page-1"].feed.refused == 1
    _next_frame_allowed(h)
    assert (await _post_frame(client, h, b)).json()["seq"] == 2
    _next_frame_allowed(h)
    assert (await _post_frame(client, h, a)).json()["seq"] == 3
    # A reader that missed frame 2 gets the latest only.
    r = await _read(client, h, after=1)
    assert r.headers["x-visivo-frame-seq"] == "3" and r.content == a


async def test_frames_are_bounded_and_checked(client, auth_headers):
    h = await _session(client, auth_headers)
    assert (await _post_frame(client, h, _jpeg())).status_code == 404       # no hello
    await _hello(client, h)
    await _watch(client, h)
    assert (await _post_frame(client, h, _jpeg(), ctype="image/png")).status_code == 415
    assert (await _post_frame(client, h, b"\xff\xd8\xff\xe0 not a jpeg")).status_code == 415
    assert (await _post_frame(client, h, b"x" * (S.MAX_FRAME_BYTES + 1))).status_code == 413
    assert (await _post_frame(client, h, _jpeg(S.MAX_FRAME_SIDE + 1, 8))).status_code == 413
    assert (await _post_frame(client, h, _jpeg(), eye="both")).status_code == 422
    assert (await _post_frame(client, h, _jpeg(), cost_ms=-1)).status_code == 422
    # The token is never relayed, not even inside an image.
    assert (await _post_frame(client, h, _jpeg() + _TEST_TOKEN.encode())).status_code == 422
    assert (await _post_frame(client, h, _jpeg(8, 8))).json()["stored"]
    assert (await _read(client, h, cid="nobody")).status_code == 404
    assert (await _read(client, h, wait=S.MAX_WAIT_S + 1)).status_code == 422


async def test_the_lease_ends_a_watch_the_desktop_left(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    await _watch(client, h)
    feed = _hub(h).pages["page-1"].feed
    # A frame read renews it.
    feed.lease_until = time.monotonic() + 0.5
    await _read(client, h)
    assert feed.lease_until > time.monotonic() + S.LEASE_S - 1
    # Over: the page is told it is no longer watched, and its frame not kept.
    feed.lease_until = time.monotonic() - 0.1
    r = await _post_frame(client, h, _jpeg())
    assert r.json()["watched"] is False and r.json()["stored"] is False and feed.frame is None


async def test_reads_waiting_at_once_are_bounded_and_end_at_shutdown(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    await _watch(client, h)
    _hub(h).frame_waiters = H.MAX_WAITERS_PER_SESSION
    assert (await _read(client, h, wait=1)).status_code == 429
    assert (await _read(client, h, wait=0)).status_code == 204       # an immediate read is never refused
    _hub(h).frame_waiters = 0
    read = asyncio.ensure_future(_read(client, h, wait=4))
    await asyncio.sleep(0.1)
    t0 = time.monotonic()
    H.shutdown()
    assert (await asyncio.wait_for(read, 3)).status_code == 204 and time.monotonic() - t0 < 0.5


async def test_a_dropped_page_takes_its_frame_along(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    await _watch(client, h)
    await _post_frame(client, h, _jpeg())
    read = asyncio.ensure_future(_read(client, h, after=1, wait=4))
    await asyncio.sleep(0.1)
    r = await client.delete("/v1/vr/headset", params={"client": "page-1"}, headers=h)
    assert r.json() == {"removed": True}
    assert (await asyncio.wait_for(read, 3)).status_code == 204
    assert (await _read(client, h)).status_code == 404


# ── Snapshot ──────────────────────────────────────────────────────────────────


async def test_snapshot_saves_the_latest_frame_as_png_in_the_exports(client, auth_headers, _isolated_exports):
    h = await _session(client, auth_headers)
    await _hello(client, h, label="Quest 3")
    snap = {"client": "page-1"}
    assert (await client.post("/v1/vr/headset/spectator/snapshot", json=snap, headers=h)).status_code == 409
    await _watch(client, h)
    assert (await client.post("/v1/vr/headset/spectator/snapshot", json=snap, headers=h)).status_code == 409
    await _post_frame(client, h, _jpeg(80, 60, colour=(10, 200, 10)))
    r = await client.post("/v1/vr/headset/spectator/snapshot", json=snap, headers=h)
    assert r.status_code == 200, r.text
    out = r.json()
    assert out["valid"] and out["seq"] == 1 and (out["width"], out["height"]) == (80, 60)
    assert out["workspace_filename"].startswith("headset_quest_3_") and out["workspace_filename"].endswith(".png")
    path = _isolated_exports / out["workspace_filename"]
    with Image.open(path) as im:
        assert im.format == "PNG" and im.size == (80, 60)
        r_, g_, b_ = im.convert("RGB").getpixel((40, 30))
        assert g_ > 150 and r_ < 60 and b_ < 60
    # A name asked for; taken already, it gets a suffix; never a path out of the exports.
    names = []
    for _ in range(2):
        r = await client.post("/v1/vr/headset/spectator/snapshot",
                              json={"client": "page-1", "basename": "../demo"}, headers=h)
        names.append(r.json()["workspace_filename"])
    assert names == ["demo.png", "demo_1.png"]
    assert (await client.post("/v1/vr/headset/spectator/snapshot",
                              json={"client": "page-1", "basename": _TEST_TOKEN}, headers=h)).status_code == 422


# ── Through the pairing bridge ────────────────────────────────────────────────


async def test_the_page_posts_a_frame_through_the_bridges_asgi_path(client, auth_headers):
    from app.main import app
    from tests.test_vr_pair import _FakeChannel

    h = await _session(client, auth_headers)
    ch = _FakeChannel()
    B.ApiBridge(ch, app, _TEST_TOKEN)
    ids = iter(range(1, 100))

    async def call(method, path, body: bytes | None = None, ctype="application/json"):
        rid = next(ids)
        start = len(ch.sent)
        ch.emit("message", json.dumps({"t": "req", "id": rid, "method": method, "path": path,
                                       "headers": {"X-Visivo-Session": h["X-Visivo-Session"],
                                                   "Content-Type": ctype},
                                       "body": body is not None}))
        if body is not None:
            # Chunks of at most 16 KiB, the last flagged (pair-wire.js bodyChunks).
            for off in range(0, max(1, len(body)), 16384):
                part = body[off:off + 16384]
                ch.emit("message", B.api_chunk(rid, part, off + 16384 >= len(body)))
        for _ in range(500):
            await asyncio.sleep(0.01)
            frames = ch.sent[start:]
            if frames and isinstance(frames[-1], bytes) and B.parse_api_chunk(frames[-1])[1]:
                break
        res = next(json.loads(f) for f in ch.sent[start:] if isinstance(f, str))
        data = b"".join(B.parse_api_chunk(f)[2] for f in ch.sent[start:] if isinstance(f, bytes))
        ch.drain_all()
        return res["status"], json.loads(data)

    st, _ = await call("POST", "/v1/vr/headset/hello", json.dumps({"client": "paired", "transport": "pair"}).encode())
    assert st == 200
    assert (await _watch(client, h, client="paired")).status_code == 200
    buf = io.BytesIO()
    Image.frombytes("RGB", (200, 210), os.urandom(200 * 210 * 3)).save(buf, format="JPEG", quality=90)
    jpg = buf.getvalue()
    assert len(jpg) > 2 * 16384                            # several 16 KiB chunks
    st, body = await call("POST", "/v1/vr/headset/spectator/frame?client=paired&eye=left&cost_ms=0.5",
                          jpg, ctype="image/jpeg")
    assert st == 200 and body["stored"] is True
    r = await _read(client, h, cid="paired")
    assert r.status_code == 200 and r.content == jpg


# ── The JPEG header ───────────────────────────────────────────────────────────


def test_jpeg_size_reads_baseline_and_progressive_frames():
    assert S.jpeg_size(_jpeg(123, 45)) == (123, 45)
    assert S.jpeg_size(_jpeg(31, 77, progressive=True)) == (31, 77)
    assert S.jpeg_size(b"") is None
    assert S.jpeg_size(b"\x89PNG\r\n\x1a\n") is None
    assert S.jpeg_size(b"\xff\xd8\xff\xe0\x00\x10JFIF") is None      # cut before any frame header
    assert S.jpeg_size(b"\xff\xd8\xff\xda\x00\x08abcdef") is None    # a scan before the frame header
