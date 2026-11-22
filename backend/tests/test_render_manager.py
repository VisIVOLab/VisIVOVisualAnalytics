"""Unit tests for RenderManager — the server-side rendering concurrency guard.

Covers the capacity cap (HTTP 503 source), explicit close, the TTL-eviction
policy (via the extracted ``_reap_once``), and the telemetry snapshot. Uses a
GPU-free fake RenderSession so the tests run anywhere (no VTK / EGL).
"""
from __future__ import annotations

import time

import pytest

import app.render_manager as rm


class _FakeStats:
    def __init__(self) -> None:
        now = time.time()
        self.created_at = now
        self.last_command_at = now
        self.frames_rendered = 0
        self.bytes_sent = 0
        self.commands_received = 0
        self.last_render_ms = 0.0
        self.last_jpeg_kb = 0.0


class _FakeSession:
    """Stand-in for RenderSession with the surface RenderManager touches."""

    def __init__(self, session_id: str, dataset_path: str) -> None:
        self.session_id = session_id
        self.dataset_path = dataset_path
        self.stats = _FakeStats()
        self.initialized = False
        self.closed = False

    async def initialize(self) -> None:
        self.initialized = True

    async def close(self) -> None:
        self.closed = True


@pytest.fixture(autouse=True)
def _fake_session(monkeypatch):
    """Swap the real (VTK-backed) RenderSession for the fake in the manager."""
    monkeypatch.setattr(rm, "RenderSession", _FakeSession)


async def test_create_registers_and_initializes():
    mgr = rm.RenderManager(max_sessions=3, idle_ttl_seconds=300)
    sess = await mgr.create_session("/data/cube.fits")
    assert sess.initialized
    assert await mgr.get_session(sess.session_id) is sess
    snap = await mgr.snapshot()
    assert snap["active_sessions"] == 1
    assert snap["capacity"] == 3


async def test_capacity_cap_raises_when_full():
    mgr = rm.RenderManager(max_sessions=2, idle_ttl_seconds=300)
    await mgr.create_session("/data/a.fits")
    await mgr.create_session("/data/b.fits")
    with pytest.raises(RuntimeError, match="capacity reached"):
        await mgr.create_session("/data/c.fits")
    # Cap is a hard limit — still exactly 2 registered.
    assert (await mgr.snapshot())["active_sessions"] == 2


async def test_close_frees_a_slot():
    mgr = rm.RenderManager(max_sessions=1, idle_ttl_seconds=300)
    a = await mgr.create_session("/data/a.fits")
    with pytest.raises(RuntimeError):
        await mgr.create_session("/data/b.fits")   # full
    assert await mgr.close_session(a.session_id) is True
    assert a.closed
    # Slot freed → a new session is admitted.
    b = await mgr.create_session("/data/b.fits")
    assert b.initialized
    assert (await mgr.snapshot())["active_sessions"] == 1


async def test_close_unknown_session_returns_false():
    mgr = rm.RenderManager(max_sessions=2)
    assert await mgr.close_session("does-not-exist") is False


async def test_reap_evicts_only_idle_past_ttl():
    mgr = rm.RenderManager(max_sessions=8, idle_ttl_seconds=100)
    idle = await mgr.create_session("/data/idle.fits")
    fresh = await mgr.create_session("/data/fresh.fits")
    # Make one session look idle beyond the TTL.
    idle.stats.last_command_at = time.time() - 200.0

    evicted = await mgr._reap_once()

    assert evicted == [idle.session_id]
    assert idle.closed
    assert not fresh.closed
    assert await mgr.get_session(idle.session_id) is None
    assert await mgr.get_session(fresh.session_id) is fresh


async def test_reap_noop_when_all_active():
    mgr = rm.RenderManager(max_sessions=8, idle_ttl_seconds=100)
    await mgr.create_session("/data/a.fits")
    await mgr.create_session("/data/b.fits")
    assert await mgr._reap_once() == []
    assert (await mgr.snapshot())["active_sessions"] == 2


async def test_reap_does_not_count_a_suspension_as_idle_time(monkeypatch):
    """Issue #411: a laptop that slept (or a stopped backend) wakes with the
    wall clock far ahead. The time nothing ran is not idle time: a view left
    on screen is not closed on waking, one idle before the sleep still is."""
    wall = [time.time()]
    monkeypatch.setattr(rm.time, "time", lambda: wall[0])
    mgr = rm.RenderManager(max_sessions=8, idle_ttl_seconds=300)
    watched = await mgr.create_session("/data/watched.fits")
    forgotten = await mgr.create_session("/data/forgotten.fits")
    watched.stats.last_command_at = wall[0] - 10.0      # a keepalive just before
    forgotten.stats.last_command_at = wall[0] - 298.0   # idle before the sleep
    assert await mgr._reap_once() == []

    wall[0] += 600.0   # ten minutes asleep: no pass in between
    evicted = await mgr._reap_once()

    assert evicted == [forgotten.session_id]
    assert not watched.closed
    # Idle time counts again from the waking pass on.
    wall[0] += 5.0
    assert await mgr._reap_once() == []
    wall[0] += 300.0
    for _ in range(60):   # passes every 5 s: no suspension in between
        wall[0] += 5.0
        await mgr._reap_once()
    assert watched.closed


async def test_a_sleep_before_the_first_pass_is_not_idle_time(monkeypatch):
    """The reaper starts with the first session: a machine that sleeps before
    the reaper's first pass is still told by the start time (issue #411)."""
    wall = [time.time()]
    monkeypatch.setattr(rm.time, "time", lambda: wall[0])
    mgr = rm.RenderManager(max_sessions=8, idle_ttl_seconds=30)
    await mgr.start()
    try:
        sess = await mgr.create_session("/data/a.fits")
        sess.stats.last_command_at = wall[0]
        wall[0] += 50.0   # asleep: the first pass comes after the TTL
        assert await mgr._reap_once() == []
        assert not sess.closed
    finally:
        await mgr.stop()


async def test_regular_passes_are_not_a_suspension(monkeypatch):
    wall = [time.time()]
    monkeypatch.setattr(rm.time, "time", lambda: wall[0])
    mgr = rm.RenderManager(max_sessions=8, idle_ttl_seconds=100)
    sess = await mgr.create_session("/data/a.fits")
    sess.stats.last_command_at = wall[0]
    for _ in range(21):
        wall[0] += rm._REAP_INTERVAL_S
        await mgr._reap_once()
    assert sess.closed


async def test_evicting_a_session_closes_its_webrtc_peer(monkeypatch):
    """Issue #411: a peer left connected to a closed session sends no frame and
    never ends, so the desktop showed a frozen "Live" view for good."""
    import sys
    import types

    closed: list[str] = []

    class _Peer:
        def __init__(self, sid: str) -> None:
            self.sid = sid

        async def close(self) -> None:
            closed.append(self.sid)
            peers.pop(self.sid, None)

    peers: dict[str, _Peer] = {}
    module = types.ModuleType("app.webrtc_session")
    module.get_active_webrtc_session = peers.get
    monkeypatch.setitem(sys.modules, "app.webrtc_session", module)

    mgr = rm.RenderManager(max_sessions=8, idle_ttl_seconds=100)
    idle = await mgr.create_session("/data/idle.fits")
    kept = await mgr.create_session("/data/kept.fits")
    peers[idle.session_id] = _Peer(idle.session_id)
    peers[kept.session_id] = _Peer(kept.session_id)
    idle.stats.last_command_at = time.time() - 200.0

    assert await mgr._reap_once() == [idle.session_id]
    assert closed == [idle.session_id]

    assert await mgr.close_session(kept.session_id) is True
    assert closed == [idle.session_id, kept.session_id]


async def test_init_failure_does_not_leak_a_slot(monkeypatch):
    """If a session fails to initialize, it must be evicted so its slot frees."""
    class _BadSession(_FakeSession):
        async def initialize(self) -> None:
            raise RuntimeError("EGL context creation failed")

    monkeypatch.setattr(rm, "RenderSession", _BadSession)
    mgr = rm.RenderManager(max_sessions=1)
    with pytest.raises(RuntimeError, match="EGL"):
        await mgr.create_session("/data/bad.fits")
    # The failed session must not squat the only slot.
    assert (await mgr.snapshot())["active_sessions"] == 0


async def test_snapshot_reports_session_telemetry():
    mgr = rm.RenderManager(max_sessions=4, idle_ttl_seconds=250)
    sess = await mgr.create_session("/data/cube.fits")
    sess.stats.frames_rendered = 42
    sess.stats.bytes_sent = 1_000_000
    snap = await mgr.snapshot()
    assert snap["capacity"] == 4
    assert snap["idle_ttl_seconds"] == 250
    entry = snap["sessions"][0]
    assert entry["session_id"] == sess.session_id
    assert entry["dataset_path"] == "/data/cube.fits"
    assert entry["frames_rendered"] == 42
    assert entry["bytes_sent"] == 1_000_000
    assert entry["age_seconds"] >= 0.0
    assert entry["idle_seconds"] >= 0.0
