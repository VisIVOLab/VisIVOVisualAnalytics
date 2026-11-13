"""Load test of the render queue (issue #4): N+2 clients on a backend capped at N.

Over the real HTTP routes and WebSockets, with the real RenderManager and a
real child process per session. The child runs the GPU-free fake session of
tests/_fake_render_session.py instead of VTK (this test must run on a laptop
without EGL); everything around it (spawn, pipe, reaper, queue, slot
accounting) is production code. Clients are "killed" by dropping their
WebSocket and no longer asking, without DELETE: the slots and processes must
come back anyway.
"""
from __future__ import annotations

import contextlib
import functools
import json
import time

import anyio.from_thread
import pytest

import app.render_manager as rm
import app.routers.render as render_router
from app.render_session._process import _LIVE, RenderSessionProcess
from tests.conftest import _TEST_TOKEN

_FAKE = "tests._fake_render_session:FakeRenderSession"
N = 2


@pytest.fixture
def backend(monkeypatch, synthetic_fits_cube):
    from fastapi.testclient import TestClient
    from app.main import app

    monkeypatch.setattr(render_router, "server_side_rendering_available", lambda: True)

    async def ready(path, hdu):
        return None
    monkeypatch.setattr(render_router, "_pyramid_preparing", ready)
    monkeypatch.setattr(rm, "RenderSession", functools.partial(RenderSessionProcess, factory=_FAKE))
    monkeypatch.setattr(rm, "POLL_AFTER_SECONDS", 0.2)       # allows a short stale time
    monkeypatch.setattr(rm, "_REAP_INTERVAL_S", 0.1)
    # Places go stale after 3 s: a waiter is not polled while another
    # request's render process starts (the test client is serial), and a
    # spawn can take over a second on a loaded machine. The idle timeout
    # outlasts the whole sequence before the last step.
    manager = rm.RenderManager(max_sessions=N, idle_ttl_seconds=10, queue_max=8,
                               queue_stale_seconds=3.0, disconnect_grace_seconds=0.5)
    monkeypatch.setattr(rm, "_MANAGER", manager)
    # One event loop for every request and socket (the manager's reaper and
    # the sessions' pipes live on it), without the app's lifespan: its
    # shutdown would take down the worker pool the rest of the suite uses.
    client = TestClient(app)
    with anyio.from_thread.start_blocking_portal(**client.async_backend) as portal:
        client.portal = portal
        try:
            headers = {"X-Visivo-Token": _TEST_TOKEN}
            opened = client.post("/v1/datasets/open", json={"path": str(synthetic_fits_cube)},
                                 headers=headers).json()
            headers["X-Visivo-Session"] = opened["session_id"]
            yield client, manager, headers, opened["dataset_id"]
        finally:
            portal.call(manager.stop)
            client.portal = None


def _ask(client, headers, dataset_id, queue_id=None):
    body = {"dataset_id": dataset_id, "queue": True}
    if queue_id:
        body["queue_id"] = queue_id
    return client.post("/v1/render/create_session", json=body, headers=headers)


def _wait_for_session(client, headers, dataset_id, queue_id, others=(), timeout=15.0):
    """Poll as a client does until the session starts; the positions seen.
    The ``others`` waiting meanwhile keep asking too, as live clients do."""
    positions = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for other in others:
            assert _ask(client, headers, dataset_id, other).status_code == 202
        resp = _ask(client, headers, dataset_id, queue_id)
        if resp.status_code == 200:
            return resp.json()["session_id"], positions
        assert resp.status_code == 202, resp.text
        positions.append(resp.json()["position"])
        time.sleep(0.1)
    raise AssertionError(f"never admitted; positions {positions}")


def _render(ws) -> bytes:
    ws.send_text(json.dumps({"type": "rotate", "d_azimuth": 5}))
    ws.send_text(json.dumps({"type": "request_frame"}))
    return ws.receive_bytes()


def _snapshot(client, headers):
    return client.get("/v1/render/diagnostics", headers=headers).json()


def _wait(predicate, timeout=10.0):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, "timed out"
        time.sleep(0.05)


def test_n_plus_two_clients_wait_their_turn_and_leave_nothing_behind(backend):
    client, manager, headers, dataset_id = backend
    with contextlib.ExitStack() as stack:
        # N clients render.
        running = {}
        for _ in range(N):
            resp = _ask(client, headers, dataset_id)
            assert resp.status_code == 200, resp.text
            sid = resp.json()["session_id"]
            ws = stack.enter_context(client.websocket_connect(
                f"/v1/render/{sid}/ws?token={_TEST_TOKEN}"))
            assert _render(ws) == b"rotate"
            running[sid] = ws
        processes = [s._process for s in manager._sessions.values()]

        # Two more wait, in order, and are told so.
        waiting = []
        for expected in (1, 2):
            resp = _ask(client, headers, dataset_id)
            assert resp.status_code == 202
            assert int(resp.headers["Retry-After"]) >= 1
            body = resp.json()
            assert (body["queued"], body["position"], body["capacity"]) == (True, expected, N)
            waiting.append(body["queue_id"])
        snap = _snapshot(client, headers)
        assert (snap["active_sessions"], snap["queued"]) == (N, 2)

        # Kill client 1: its socket drops, no DELETE. Its slot passes to the
        # first waiting request after the disconnect grace.
        first_sid, first_ws = next(iter(running.items()))
        first_ws.close()
        sid3, seen3 = _wait_for_session(client, headers, dataset_id, waiting[0],
                                        others=[waiting[1]])
        assert set(seen3) <= {0, 1}
        processes.append(manager._sessions[sid3]._process)
        ws3 = stack.enter_context(client.websocket_connect(
            f"/v1/render/{sid3}/ws?token={_TEST_TOKEN}"))
        assert _render(ws3) == b"rotate"
        # The fourth moved up.
        resp = _ask(client, headers, dataset_id, waiting[1])
        assert resp.status_code == 202 and resp.json()["position"] == 1

        # Client 2 closes its view properly: the fourth starts.
        second_sid = next(s for s in running if s != first_sid)
        assert client.delete(f"/v1/render/{second_sid}", headers=headers).status_code == 200
        sid4, _ = _wait_for_session(client, headers, dataset_id, waiting[1])
        processes.append(manager._sessions[sid4]._process)
        ws4 = stack.enter_context(client.websocket_connect(
            f"/v1/render/{sid4}/ws?token={_TEST_TOKEN}"))
        assert _render(ws4) == b"rotate"
        snap = _snapshot(client, headers)
        assert (snap["active_sessions"], snap["queued"]) == (N, 0)
        assert set(manager._sessions) == {sid3, sid4}
        _wait(lambda: sum(p.is_alive() for p in processes) == N)   # the first two ended

        # A fifth client queues and dies without asking again: its place goes.
        resp = _ask(client, headers, dataset_id)
        assert resp.status_code == 202
        _wait(lambda: _snapshot(client, headers)["queued"] == 0)
        assert _ask(client, headers, dataset_id, resp.json()["queue_id"]).status_code == 410

        # Kill the last two clients.
        stack.close()

    # Nobody waits now: their sessions end at the idle timeout (10 s here),
    # and with them their processes.
    _wait(lambda: _snapshot(client, headers)["active_sessions"] == 0, timeout=30)
    _wait(lambda: not any(p.is_alive() for p in processes), timeout=10)
    # The child exits a moment before its release thread drops the proxy
    # from _LIVE (after closing the pipe): wait for that too.
    _wait(lambda: not [p for p in _LIVE if p.session_id in (sid3, sid4)], timeout=10)
    assert manager._attached == {} and manager._detached_at == {}
    assert manager._tickets == {}


def test_without_queue_a_client_is_refused_as_before(backend):
    client, _, headers, dataset_id = backend
    for _ in range(N):
        assert _ask(client, headers, dataset_id).status_code == 200
    resp = client.post("/v1/render/create_session", json={"dataset_id": dataset_id},
                       headers=headers)
    assert resp.status_code == 503 and "capacity reached" in resp.json()["detail"]


def test_cancel_route_gives_up_the_place(backend):
    client, _, headers, dataset_id = backend
    for _ in range(N):
        assert _ask(client, headers, dataset_id).status_code == 200
    queue_id = _ask(client, headers, dataset_id).json()["queue_id"]
    assert client.delete(f"/v1/render/queue/{queue_id}", headers=headers).json() == {
        "cancelled": True}
    assert client.delete(f"/v1/render/queue/{queue_id}", headers=headers).status_code == 404
    assert _ask(client, headers, dataset_id, queue_id).status_code == 410
