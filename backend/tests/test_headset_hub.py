"""The headset hub (issue #236): presence, commands (desktop to page) and
activity (page to desktop) per session, their bounds, the long polls ending
at shutdown, the routes through the pairing bridge, and the spectrum table
a headset product is saved as."""
from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits
from httpx import AsyncClient

from app import headset_hub as H
from app import vr_bridge as B
from tests.conftest import _TEST_TOKEN


@pytest.fixture(autouse=True)
def _drop_the_sessions_made():
    """Each test's sessions go with it (the per-token session cap is 32)."""
    from app.sessions import REGISTRY
    before = set(REGISTRY._sessions)
    yield
    with REGISTRY._lock:
        made = [sid for sid in REGISTRY._sessions if sid not in before]
        dropped = [REGISTRY._sessions.pop(sid) for sid in made]
    for s in dropped:
        s.close()
    H.reopen()


async def _session(client: AsyncClient, auth_headers: dict) -> dict:
    r = await client.post("/v1/sessions/new", json={}, headers=auth_headers)
    assert r.status_code == 200, r.text
    return {**auth_headers, "X-Visivo-Session": r.json()["session_id"]}


def _hub(headers: dict) -> H.HeadsetHub:
    from app.sessions import REGISTRY
    return H.hub_for(REGISTRY._sessions[headers["X-Visivo-Session"]])


async def _hello(client, headers, cid="page-1", transport="pair", label="Quest 3"):
    r = await client.post("/v1/vr/headset/hello",
                          json={"client": cid, "transport": transport, "label": label}, headers=headers)
    assert r.status_code == 200, r.text
    return r.json()


async def _show(http, headers, **kw):
    body = {"type": "show", "dataset_id": "ds-1", "path": "/data/cube.fits", **kw}
    return await http.post("/v1/vr/headset/command", json=body, headers=headers)


async def _poll(client, headers, cid="page-1", after=0, wait=0.0):
    r = await client.get("/v1/vr/headset/commands",
                         params={"client": cid, "after": after, "wait": wait}, headers=headers)
    assert r.status_code == 200, r.text
    return r.json()


async def _act(client, headers, kind, cid="page-1", cube_path="/data/cube.fits", summary="s", payload=None):
    return await client.post("/v1/vr/activity", json={
        "client": cid, "kind": kind, "cube_path": cube_path, "summary": summary,
        "payload": payload or {}}, headers=headers)


async def _events(client, headers, after=0, wait=0.0):
    r = await client.get("/v1/vr/activity", params={"after": after, "wait": wait}, headers=headers)
    assert r.status_code == 200, r.text
    return r.json()


# ── Presence ──────────────────────────────────────────────────────────────────


async def test_hello_state_and_listing_newest_first(client, auth_headers):
    h = await _session(client, auth_headers)
    a = await _hello(client, h, "page-a", "adb")
    assert a["client"] == "page-a" and abs(a["server_time"] - time.time()) < 5
    assert a["instance"] == H.INSTANCE
    await asyncio.sleep(0.01)
    await _hello(client, h, "page-b", "lan", "")
    cube = {"dataset_id": "d1", "path": "/data/c.fits", "kind": "cube", "box": [[0, 9], [0, 9], [2, 5]]}
    r = await client.post("/v1/vr/headset/state", json={"client": "page-a", "in_vr": True, "cube": cube},
                          headers=h)
    assert r.status_code == 200, r.text
    listed = (await client.get("/v1/vr/headset", headers=h)).json()
    assert [p["client"] for p in listed["headsets"]] == ["page-b", "page-a"]
    pa = listed["headsets"][1]
    assert pa["transport"] == "adb" and pa["in_vr"] is True and pa["cube"] == cube
    assert pa["staged_cube"] is None and pa["since"] <= pa["last_seen"] <= listed["server_time"]
    # A box is three ordered voxel ranges.
    bad = {**cube, "box": [[5, 1], [0, 1], [0, 1]]}
    r = await client.post("/v1/vr/headset/state", json={"client": "page-a", "in_vr": False, "cube": bad},
                          headers=h)
    assert r.status_code == 422
    # A state from a page the hub does not know: hello first.
    r = await client.post("/v1/vr/headset/state", json={"client": "ghost", "in_vr": False}, headers=h)
    assert r.status_code == 404 and "hello" in r.json()["detail"]
    r = await client.post("/v1/vr/headset/hello", json={"client": "a b", "transport": "pair"}, headers=h)
    assert r.status_code == 422
    r = await client.post("/v1/vr/headset/hello", json={"client": "x", "transport": "usb"}, headers=h)
    assert r.status_code == 422


async def test_presence_expires_and_polling_keeps_a_page_seen(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h, "page-1")
    hub = _hub(h)
    hub.pages["page-1"].last_seen -= H.PRESENT_S + 1
    assert (await client.get("/v1/vr/headset", headers=h)).json()["headsets"] == []
    assert (await _show(client, h)).status_code == 404          # not connected
    # Polling makes it seen again.
    await _poll(client, h)
    assert [p["client"] for p in (await client.get("/v1/vr/headset", headers=h)).json()["headsets"]] == ["page-1"]
    # Silent past FORGET_S: forgotten; its routes answer 404 until it says hello.
    hub.pages["page-1"].last_seen -= H.FORGET_S + 1
    r = await client.get("/v1/vr/headset/commands", params={"client": "page-1"}, headers=h)
    assert r.status_code == 404
    await _hello(client, h, "page-1")
    assert (await client.delete("/v1/vr/headset", params={"client": "page-1"}, headers=h)).json() == {"removed": True}
    assert (await client.delete("/v1/vr/headset", params={"client": "page-1"}, headers=h)).json() == {"removed": False}
    assert (await client.get("/v1/vr/headset", headers=h)).json()["headsets"] == []


async def test_pages_per_session_are_bounded(client, auth_headers):
    h = await _session(client, auth_headers)
    for i in range(H.MAX_PAGES_PER_SESSION + 2):
        await _hello(client, h, f"p{i}")
    hub = _hub(h)
    assert len(hub.pages) == H.MAX_PAGES_PER_SESSION
    assert "p0" not in hub.pages and "p1" not in hub.pages and f"p{H.MAX_PAGES_PER_SESSION + 1}" in hub.pages


# ── Commands ──────────────────────────────────────────────────────────────────


async def test_commands_delivery_replacement_and_ack(client, auth_headers):
    h = await _session(client, auth_headers)
    assert (await _show(client, h)).status_code == 404               # no page yet
    await _hello(client, h, "page-1")
    await asyncio.sleep(0.01)
    await _hello(client, h, "page-2")
    # No client: the newest page; a named one that is not connected: 404.
    r = await _show(client, h, path="/data/a.fits")
    assert r.status_code == 200 and r.json()["client"] == "page-2"
    assert (await _show(client, h, client="nobody")).status_code == 404
    r1 = (await _show(client, h, client="page-1", path="/data/a.fits")).json()
    r2 = (await _show(client, h, client="page-1", path="/data/b.fits", kind="cube",
                      box=[[0, 1], [0, 1], [0, 1]])).json()
    assert r2["seq"] > r1["seq"] and r2["client"] == "page-1"
    got = await _poll(client, h, "page-1")
    # The later show replaced the earlier, unacknowledged one.
    assert got["commands"] == [{"seq": r2["seq"], "type": "show", "dataset_id": "ds-1",
                                "path": "/data/b.fits", "kind": "cube", "box": [[0, 1], [0, 1], [0, 1]]}]
    assert got["next"] == r2["seq"] and got["instance"] == H.INSTANCE
    # Not acknowledged yet: handed again.
    assert (await _poll(client, h, "page-1"))["commands"][0]["seq"] == r2["seq"]
    # Acknowledged by a later after: gone.
    again = await _poll(client, h, "page-1", after=got["next"])
    assert again["commands"] == [] and again["next"] == got["next"]
    assert _hub(h).pages["page-1"].commands == []
    # A cursor from a restarted backend (above every seq issued) counts as 0.
    r3 = (await _show(client, h, client="page-1")).json()
    stale = await _poll(client, h, "page-1", after=H._last_seq + 1000)
    assert [c["seq"] for c in stale["commands"]] == [r3["seq"]]


async def test_commands_are_bounded_per_page():
    hub = H.HeadsetHub()
    hub.hello("p", "pair", "")
    for i in range(H.MAX_COMMANDS_PER_PAGE + 5):
        assert hub.queue_command("p", f"other{i}", {}) is not None
    cmds = hub.pages["p"].commands
    assert len(cmds) == H.MAX_COMMANDS_PER_PAGE and cmds[0].type == "other5"


async def test_a_waiting_poll_gets_the_command_and_a_newer_poll_ends_it(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h, "page-1")
    t0 = time.monotonic()
    waiting = asyncio.ensure_future(_poll(client, h, "page-1", wait=10))
    await asyncio.sleep(0.2)
    # While waiting the page is listed, whatever its last request.
    _hub(h).pages["page-1"].last_seen -= H.PRESENT_S + 5
    assert len((await client.get("/v1/vr/headset", headers=h)).json()["headsets"]) == 1
    seq = (await _show(client, h)).json()["seq"]
    got = await asyncio.wait_for(waiting, 5)
    assert [c["seq"] for c in got["commands"]] == [seq] and time.monotonic() - t0 < 3
    # A newer poll of the same page ends the older one, empty.
    older = asyncio.ensure_future(_poll(client, h, "page-1", after=seq, wait=10))
    await asyncio.sleep(0.2)
    newer = asyncio.ensure_future(_poll(client, h, "page-1", after=seq, wait=1))
    assert (await asyncio.wait_for(older, 3))["commands"] == []
    await newer


async def test_long_poll_waiters_are_bounded(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h, "page-1")
    hub = _hub(h)
    hub.activity_waiters = H.MAX_WAITERS_PER_SESSION
    hub.command_waiters = H.MAX_WAITERS_PER_SESSION
    try:
        r = await client.get("/v1/vr/activity", params={"wait": 1}, headers=h)
        assert r.status_code == 429
        r = await client.get("/v1/vr/headset/commands", params={"client": "page-1", "wait": 1}, headers=h)
        assert r.status_code == 429
        # A poll that does not wait is always answered.
        assert (await client.get("/v1/vr/activity", headers=h)).status_code == 200
    finally:
        hub.activity_waiters = hub.command_waiters = 0
    r = await client.get("/v1/vr/activity", params={"wait": 26}, headers=h)
    assert r.status_code == 422                                     # capped at 25 s


# ── Activity ──────────────────────────────────────────────────────────────────


async def test_activity_coalesces_look_and_clip_per_cube(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    assert (await _act(client, h, "look", cid="ghost")).status_code == 404
    s1 = (await _act(client, h, "look", payload={"colormap": "viridis"})).json()["seq"]
    (await _act(client, h, "clip", payload={"box": [[0, 1], [0, 1], [0, 1]]})).json()
    await _act(client, h, "look", cube_path="/data/other.fits", payload={"colormap": "gray"})
    p1 = (await _act(client, h, "product", payload={"product": "pv", "dataset_id": "d9"})).json()["seq"]
    s2 = (await _act(client, h, "look", payload={"colormap": "inferno", "gamma": 1.2})).json()["seq"]
    await _act(client, h, "clip", payload={"box": None})
    got = await _events(client, h)
    kinds = [(e["kind"], e["cube_path"]) for e in got["events"]]
    assert kinds == [("look", "/data/other.fits"), ("product", "/data/cube.fits"),
                     ("look", "/data/cube.fits"), ("clip", "/data/cube.fits")]
    look = got["events"][2]
    assert look["seq"] == s2 > p1 > s1 and look["payload"] == {"colormap": "inferno", "gamma": 1.2}
    assert got["events"][3]["payload"] == {"box": None} and got["lost"] is False
    assert got["next"] == got["events"][-1]["seq"]
    assert (await _events(client, h, after=got["next"]))["events"] == []


async def test_activity_ring_and_lost(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    first = (await _act(client, h, "cube", payload={"path": "/a"})).json()["seq"]
    for i in range(H.MAX_EVENTS + 4):
        r = await _act(client, h, "cube", payload={"path": f"/c{i}"})
    got = await _events(client, h)
    assert len(got["events"]) == H.MAX_EVENTS and got["lost"] is False     # a first poll
    assert got["events"][-1]["payload"] == {"path": f"/c{H.MAX_EVENTS + 3}"}
    assert (await _events(client, h, after=first))["lost"] is True
    assert (await _events(client, h, after=r.json()["seq"] - 1))["lost"] is False


async def test_activity_payload_limits_and_checks(client, auth_headers, tmp_path):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    r = await _act(client, h, "look", payload={"pad": "x" * (H.MAX_PAYLOAD_BYTES + 1)})
    assert r.status_code == 413
    r = await client.post("/v1/vr/activity", content=b'{"client":"page-1","kind":"look","summary":"",'
                          b'"payload":{"gamma":NaN}}', headers={**h, "Content-Type": "application/json"})
    assert r.status_code == 422
    r = await _act(client, h, "look", payload={"note": f"token {_TEST_TOKEN}"})
    assert r.status_code == 422 and "token" in r.json()["detail"]
    r = await _act(client, h, "look", summary=_TEST_TOKEN)
    assert r.status_code == 422
    r = await _act(client, h, "bogus")
    assert r.status_code == 422
    # A product names its kind; a file is a Workspace Exports file that exists.
    assert (await _act(client, h, "product", payload={"product": "sculpture"})).status_code == 422
    assert (await _act(client, h, "product", payload={"product": "moment",
                                                      "file": "../etc/passwd"})).status_code == 422
    assert (await _act(client, h, "product", payload={"product": "moment",
                                                      "file": "no_such_file_236.fits"})).status_code == 422
    from app.dependencies import make_workspace_path
    made = make_workspace_path("headset_hub_test_236.fits", reserve=True)
    try:
        r = await _act(client, h, "product", payload={"product": "moment", "file": made.name,
                                                      "request": {"moment_order": 0}})
        assert r.status_code == 200, r.text
        ev = (await _events(client, h))["events"][-1]
        assert ev["payload"]["output_path"] == str(made) and ev["payload"]["request"] == {"moment_order": 0}
    finally:
        made.unlink(missing_ok=True)
    # The bytes of a session's payloads are bounded too.
    hub = _hub(h)
    for i in range(80):
        await _act(client, h, "cube", payload={"pad": "y" * 15000, "i": i})
    assert hub.event_bytes <= H.MAX_EVENT_BYTES and len(hub.events) < 80


async def test_bodies_are_bounded_and_never_carry_the_token(client, auth_headers):
    from app.routers import headset as R
    h = await _session(client, auth_headers)
    await _hello(client, h)
    # Refused once over the cap, before it is parsed (declared, or as read).
    big = json.dumps({"client": "page-1", "kind": "look", "summary": "",
                      "payload": {"pad": "z" * R.MAX_BODY_BYTES}})
    r = await client.post("/v1/vr/activity", content=big, headers={**h, "Content-Type": "application/json"})
    assert r.status_code == 413

    async def chunks():
        for _ in range(8):
            yield b" " * (R.MAX_BODY_BYTES // 4)
    r = await client.post("/v1/vr/activity", content=chunks(), headers={**h, "Content-Type": "application/json"})
    assert r.status_code == 413
    for bad in (b"{not json", b"\xff", b'{"client":NaN,"transport":"pair"}'):
        r = await client.post("/v1/vr/headset/hello", content=bad, headers={**h, "Content-Type": "application/json"})
        assert r.status_code == 422, (bad, r.status_code)
        assert "input" not in json.dumps(r.json())          # the rejected input is not echoed
    # The token is refused in every field that is kept, the client id included.
    tok = _TEST_TOKEN.replace("-", "_")
    import os
    os.environ["VISIVO_TOKENS"] = tok
    try:
        r = await client.post("/v1/vr/headset/hello", json={"client": tok, "transport": "pair"}, headers=h)
        assert r.status_code == 422
        cube = {"dataset_id": "d", "path": "/p", "kind": tok}
        r = await client.post("/v1/vr/headset/state", json={"client": "page-1", "in_vr": False, "cube": cube},
                              headers=h)
        assert r.status_code == 422
    finally:
        del os.environ["VISIVO_TOKENS"]
    assert tok not in json.dumps((await client.get("/v1/vr/headset", headers=h)).json())


async def test_a_product_file_stays_in_the_workspace(client, auth_headers, tmp_path):
    from app.dependencies import get_exports_dir
    h = await _session(client, auth_headers)
    await _hello(client, h)
    r = await _act(client, h, "product", payload={"product": "pv", "output_path": "/etc/passwd"})
    assert r.status_code == 422
    outside = tmp_path / "outside_236.fits"
    outside.write_bytes(b"x")
    link = get_exports_dir() / "headset_hub_link_236.fits"
    link.unlink(missing_ok=True)
    link.symlink_to(outside)
    try:
        r = await _act(client, h, "product", payload={"product": "pv", "file": link.name})
        assert r.status_code == 422
    finally:
        link.unlink(missing_ok=True)


async def test_a_superseded_poll_delivers_nothing_even_when_a_command_arrives():
    hub = H.HeadsetHub()
    page = hub.hello("p", "pair", "")
    older = asyncio.ensure_future(hub.wait_commands(page, 0, 5))
    await asyncio.sleep(0)
    newer = asyncio.ensure_future(hub.wait_commands(page, 0, 5))
    await asyncio.sleep(0)
    cmd, _ = hub.queue_command("p", "show", {"path": "/c"})
    a, b = await asyncio.wait_for(asyncio.gather(older, newer), 3)
    assert a == [] and [c.seq for c in b] == [cmd.seq]


async def test_sessions_are_isolated(client, auth_headers):
    a = await _session(client, auth_headers)
    b = await _session(client, auth_headers)
    await _hello(client, a, "page-a")
    await _act(client, a, "look", cid="page-a")
    assert (await client.get("/v1/vr/headset", headers=b)).json()["headsets"] == []
    assert (await _events(client, b))["events"] == []
    assert (await _show(client, b)).status_code == 404
    assert (await client.get("/v1/vr/headset/commands", params={"client": "page-a"}, headers=b)).status_code == 404
    assert (await client.get("/v1/vr/headset", headers={})).status_code in (401, 403)


async def test_a_waiting_activity_poll_wakes_on_an_event(client, auth_headers):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    after = (await _events(client, h))["next"]
    waiting = asyncio.ensure_future(_events(client, h, after=after, wait=10))
    await asyncio.sleep(0.2)
    seq = (await _act(client, h, "cube", payload={"path": "/x"})).json()["seq"]
    got = await asyncio.wait_for(waiting, 3)
    assert [e["seq"] for e in got["events"]] == [seq]


async def test_long_polls_end_at_shutdown(client, auth_headers, monkeypatch):
    h = await _session(client, auth_headers)
    await _hello(client, h)
    t0 = time.monotonic()
    polls = [asyncio.ensure_future(_poll(client, h, wait=20)),
             asyncio.ensure_future(_events(client, h, after=(await _events(client, h))["next"], wait=20))]
    await asyncio.sleep(0.2)
    H.shutdown()
    done = await asyncio.wait_for(asyncio.gather(*polls), 3)
    assert done[0]["commands"] == [] and done[1]["events"] == [] and time.monotonic() - t0 < 3
    H.reopen()
    # uvicorn's own exit (its signal handler's should_exit) ends them too,
    # within a second, before the lifespan's shutdown runs.

    class Server:
        should_exit = False

        def handle_exit(self, *_a):
            pass
    Server.__module__ = "uvicorn.server"
    srv = Server()
    monkeypatch.setattr(H.signal, "getsignal", lambda _s: srv.handle_exit)
    poll = asyncio.ensure_future(_events(client, h, after=H._last_seq, wait=20))
    await asyncio.sleep(0.2)
    t1 = time.monotonic()
    srv.should_exit = True
    assert (await asyncio.wait_for(poll, 3))["events"] == [] and time.monotonic() - t1 < 2


# ── Through the pairing bridge ────────────────────────────────────────────────


async def test_the_routes_work_through_the_bridges_asgi_path(client, auth_headers):
    from app.main import app
    from tests.test_vr_pair import _FakeChannel

    h = await _session(client, auth_headers)
    ch = _FakeChannel()
    B.ApiBridge(ch, app, _TEST_TOKEN)
    ids = iter(range(1, 100))

    async def call(method, path, body=None):
        rid = next(ids)
        start = len(ch.sent)
        # The page's headers: its session, never the token (the bridge adds it).
        ch.emit("message", json.dumps({"t": "req", "id": rid, "method": method, "path": path,
                                       "headers": {"X-Visivo-Session": h["X-Visivo-Session"],
                                                   "Content-Type": "application/json"},
                                       "body": body is not None}))
        if body is not None:
            ch.emit("message", B.api_chunk(rid, json.dumps(body).encode(), True))
        for _ in range(500):
            await asyncio.sleep(0.01)
            frames = ch.sent[start:]
            if frames and isinstance(frames[-1], bytes) and B.parse_api_chunk(frames[-1])[1]:
                break
        res = next(json.loads(f) for f in ch.sent[start:] if isinstance(f, str))
        data = b"".join(B.parse_api_chunk(f)[2] for f in ch.sent[start:] if isinstance(f, bytes))
        ch.drain_all()
        return res["status"], json.loads(data)

    st, body = await call("POST", "/v1/vr/headset/hello", {"client": "paired", "transport": "pair"})
    assert st == 200 and body["client"] == "paired"
    # The desktop, over HTTP, sees the paired page and sends it a cube.
    listed = (await client.get("/v1/vr/headset", headers=h)).json()["headsets"]
    assert [(p["client"], p["transport"]) for p in listed] == [("paired", "pair")]
    seq = (await _show(client, h)).json()["seq"]
    st, body = await call("GET", "/v1/vr/headset/commands?client=paired&wait=2")
    assert st == 200 and [c["seq"] for c in body["commands"]] == [seq]
    st, body = await call("POST", "/v1/vr/activity", {"client": "paired", "kind": "look", "summary": "lut",
                                                      "payload": {"colormap": "viridis"}})
    assert st == 200
    assert (await _events(client, h))["events"][-1]["seq"] == body["seq"]


# ── The spectrum table ────────────────────────────────────────────────────────


def _cube(path: Path) -> np.ndarray:
    rng = np.random.default_rng(3)
    data = rng.normal(0.0, 1.0, (12, 10, 8)).astype(np.float32)
    data[4, 5, 3] = np.nan
    h = fits.Header()
    h.update({"CTYPE1": "RA---SIN", "CTYPE2": "DEC--SIN", "CTYPE3": "VRAD",
              "CRVAL1": 150.0, "CRVAL2": 20.0, "CRVAL3": 1.0e5, "CRPIX1": 4.0, "CRPIX2": 5.0,
              "CRPIX3": 1.0, "CDELT1": -1 / 3600, "CDELT2": 1 / 3600, "CDELT3": 2000.0,
              "CUNIT1": "deg", "CUNIT2": "deg", "CUNIT3": "m/s", "BUNIT": "Jy/beam"})
    fits.PrimaryHDU(data, header=h).writeto(path, overwrite=True)
    return data


async def test_spectrum_table_csv_and_fits(client, auth_headers, tmp_path):
    data = _cube(tmp_path / "spec_cube.fits")
    h = await _session(client, auth_headers)
    r = await client.post("/v1/datasets/open", json={"path": str(tmp_path / "spec_cube.fits")}, headers=h)
    ds = r.json()["dataset_id"]
    probe = (await client.post("/v1/spectral/probe", json={"dataset_id": ds, "x": 3, "y": 5}, headers=h)).json()
    made = []
    try:
        r = (await client.post("/v1/products/spectrum_table",
                               json={"dataset_id": ds, "x": 3, "y": 5}, headers=h)).json()
        assert r["valid"], r
        made.append(Path(r["output_path"]))
        assert r["workspace_filename"] == "spec_cube_spectrum_x3_y5.csv" or \
            r["workspace_filename"].startswith("spec_cube_spectrum_x3_y5_")
        assert r["n_channels"] == 12 and r["bunit"] == "Jy/beam" and r["n_pix"] == 1
        lines = Path(r["output_path"]).read_text().splitlines()
        assert lines[0] == "# VisIVO spectrum export" and lines[2] == "# pixel:   (3, 5)"
        header = lines[4]
        assert header == f"{probe['spectral_type']} ({probe['spectral_unit']}),Intensity (Jy/beam)"
        rows = [tuple(float(v) for v in ln.split(",")) for ln in lines[5:]]
        assert len(rows) == 12 and np.isnan(rows[4][1])
        np.testing.assert_allclose([a for a, _ in rows], probe["spectral_axis"], rtol=1e-10)
        np.testing.assert_allclose([v for _, v in rows], data[:, 5, 3].astype(float), rtol=1e-6)
        # A FITS table of the aperture's mean, under the name asked (its suffix follows the format).
        r = (await client.post("/v1/products/spectrum_table",
                               json={"dataset_id": ds, "x": 3, "y": 5, "radius": 2, "format": "fits",
                                     "output_basename": "../my_spec.csv"}, headers=h)).json()
        assert r["valid"], r
        made.append(Path(r["output_path"]))
        assert r["workspace_filename"].startswith("my_spec") and r["workspace_filename"].endswith(".fits")
        ap = (await client.post("/v1/spectral/aperture", json={"dataset_id": ds, "x": 3, "y": 5, "radius": 2},
                                headers=h)).json()
        with fits.open(r["output_path"]) as hdul:
            t = hdul["SPECTRUM"]
            assert t.header["NPIX"] == ap["n_pix"] == r["n_pix"] and t.columns["VALUE"].unit == "Jy/beam"
            np.testing.assert_allclose(t.data["VALUE"], [np.nan if v is None else v for v in ap["values"]],
                                       rtol=1e-10, equal_nan=True)
        # Two saves of one name at once: two files, neither lost.
        both = await asyncio.gather(*[client.post(
            "/v1/products/spectrum_table",
            json={"dataset_id": ds, "x": 3, "y": 5, "output_basename": "same_236"}, headers=h)
            for _ in range(2)])
        paths = [b.json()["output_path"] for b in both]
        made += [Path(p) for p in paths if p]
        assert all(b.json()["valid"] for b in both) and len(set(paths)) == 2
        assert all(Path(p).read_text().startswith("# VisIVO spectrum export") for p in paths)
        # A failed save leaves no placeholder behind.
        from app.dependencies import get_exports_dir
        before = set(get_exports_dir().iterdir())
        bad = await client.post("/v1/products/spectrum_table",
                                json={"dataset_id": ds, "x": 99, "y": 5}, headers=h)
        assert bad.status_code == 200 and bad.json()["valid"] is False
        assert set(get_exports_dir().iterdir()) == before
        # A 504 once the work was handed to the pool leaves the worker
        # running: its name stays reserved for it.
        from fastapi import HTTPException
        import app.dependencies as D
        import app.routers.products as P

        async def timed_out(fn, *args, dispatch=None, **_k):
            dispatch(fn, *args).close()          # handed over, then the request gives up
            raise HTTPException(504, "slow")
        orig = D._run_heavy
        D._run_heavy = timed_out
        try:
            r = await client.post("/v1/products/spectrum_table",
                                  json={"dataset_id": ds, "x": 3, "y": 5, "output_basename": "slow_236"},
                                  headers=h)
        finally:
            D._run_heavy = orig
        assert r.status_code == 504
        kept = set(get_exports_dir().iterdir()) - before
        made += list(kept)
        assert [p.name for p in kept] == ["slow_236.csv"]
        # Cancelled while waiting for a slot (never handed over): the name goes.
        before = set(get_exports_dir().iterdir())
        orig_slot = P.acquire_task_slot

        async def never(_s):
            await asyncio.sleep(60)
        P.acquire_task_slot = never
        try:
            req = asyncio.ensure_future(client.post(
                "/v1/products/spectrum_table",
                json={"dataset_id": ds, "x": 3, "y": 5, "output_basename": "waiting_236"}, headers=h))
            await asyncio.sleep(0.2)
            assert {p.name for p in set(get_exports_dir().iterdir()) - before} == {"waiting_236.csv"}
            req.cancel()
            with pytest.raises(asyncio.CancelledError):
                await req
        finally:
            P.acquire_task_slot = orig_slot
        assert set(get_exports_dir().iterdir()) == before
        assert (await client.post("/v1/products/spectrum_table",
                                  json={"dataset_id": ds, "x": 1, "y": 1, "format": "xlsx"},
                                  headers=h)).status_code == 422
    finally:
        for p in made:
            p.unlink(missing_ok=True)
