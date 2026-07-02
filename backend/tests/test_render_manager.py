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
