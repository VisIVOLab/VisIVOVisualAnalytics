"""The queue of render-session requests above the cap (issue #4).

Unit tests drive the real RenderManager with GPU-free fake sessions and a
fake clock: ordering, the cap, cancel, timeout, stale places, fairness across
tokens, and slots freed when a render process ends or nobody is connected
while others wait. One test kills a real render child process (the fake
session of tests/_fake_render_session.py, spawned as in production).
"""
from __future__ import annotations

import asyncio
import os
import signal
import time

import pytest

import app.render_manager as rm
from app.render_session._process import RenderSessionProcess

_FAKE = "tests._fake_render_session:FakeRenderSession"


class _Stats:
    def __init__(self) -> None:
        self.created_at = self.last_command_at = time.time()
        self.frames_rendered = self.bytes_sent = self.commands_received = 0
        self.last_render_ms = self.last_jpeg_kb = 0.0


class _Session:
    def __init__(self, session_id: str, dataset_path: str, **_) -> None:
        self.session_id = session_id
        self.dataset_path = dataset_path
        self.stats = _Stats()
        self.ended = False
        self.on_ended = None
        self.closed = False

    async def initialize(self) -> None:
        pass

    async def close(self) -> None:
        self.closed = True


class _Clock:
    def __init__(self) -> None:
        self.t = 1000.0

    def __call__(self) -> float:
        return self.t


@pytest.fixture(autouse=True)
def _fake_session(monkeypatch):
    monkeypatch.setattr(rm, "RenderSession", _Session)


@pytest.fixture
def clock():
    return _Clock()


def _manager(clock, **kw):
    kw.setdefault("max_sessions", 1)
    kw.setdefault("queue_max", 8)
    kw.setdefault("queue_timeout_seconds", 600)
    kw.setdefault("queue_stale_seconds", 15)
    kw.setdefault("disconnect_grace_seconds", 30)
    return rm.RenderManager(idle_ttl_seconds=300, clock=clock, **kw)


async def _queue(mgr, path="/d/c.fits", owner="t", ticket=None):
    """The queue status of a request expected to wait."""
    with pytest.raises(rm.RenderQueued) as info:
        await mgr.create_session(path, owner_token=owner, queue=True, ticket_id=ticket)
    return info.value.status


async def test_requests_above_the_cap_wait_in_arrival_order(clock):
    mgr = _manager(clock)
    a = await mgr.create_session("/d/c.fits", owner_token="t", queue=True)
    b = await _queue(mgr)
    c = await _queue(mgr)
    assert (b["position"], c["position"]) == (1, 2)
    assert c["queue_length"] == 2 and c["capacity"] == 1 and c["active_sessions"] == 1
    # Asking again keeps the place.
    assert (await _queue(mgr, ticket=b["queue_id"]))["position"] == 1

    await mgr.close_session(a.session_id)
    # B's turn: its next request starts its session; C moves up.
    sess = await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=b["queue_id"])
    assert await mgr.get_session(sess.session_id) is sess
    assert (await _queue(mgr, ticket=c["queue_id"]))["position"] == 1


async def test_a_newcomer_does_not_take_a_slot_reserved_for_a_waiting_request(clock):
    mgr = _manager(clock)
    a = await mgr.create_session("/d/c.fits", owner_token="t")
    b = await _queue(mgr)
    await mgr.close_session(a.session_id)
    # The slot freed is B's even before B asks again.
    d = await _queue(mgr)
    assert d["position"] == 1
    await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=b["queue_id"])


async def test_without_queue_or_with_the_queue_full_the_request_is_refused(clock):
    mgr = _manager(clock, queue_max=1)
    await mgr.create_session("/d/c.fits", owner_token="t")
    with pytest.raises(rm.RenderCapacityError, match="capacity reached"):
        await mgr.create_session("/d/c.fits", owner_token="t")          # did not ask to wait
    await _queue(mgr)
    with pytest.raises(rm.RenderCapacityError, match="1 requests waiting"):
        await mgr.create_session("/d/c.fits", owner_token="t", queue=True)
    assert (await mgr.snapshot())["active_sessions"] == 1


async def test_queue_off_refuses_as_before(clock):
    mgr = _manager(clock, queue_max=0)
    await mgr.create_session("/d/c.fits", owner_token="t")
    with pytest.raises(rm.RenderCapacityError):
        await mgr.create_session("/d/c.fits", owner_token="t", queue=True)


async def test_cancel_gives_up_the_place(clock):
    mgr = _manager(clock)
    await mgr.create_session("/d/c.fits", owner_token="t")
    b = await _queue(mgr)
    c = await _queue(mgr)
    assert await mgr.cancel_ticket(b["queue_id"], "someone-else") is False
    assert await mgr.cancel_ticket(b["queue_id"], "t") is True
    assert (await _queue(mgr, ticket=c["queue_id"]))["position"] == 1
    with pytest.raises(rm.RenderTicketGone):
        await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=b["queue_id"])


async def test_a_reserved_slot_is_freed_when_its_request_is_cancelled(clock):
    mgr = _manager(clock)
    a = await mgr.create_session("/d/c.fits", owner_token="t")
    b = await _queue(mgr)
    c = await _queue(mgr)
    await mgr.close_session(a.session_id)
    await _queue(mgr, ticket=c["queue_id"])          # B now holds the reservation
    assert await mgr.cancel_ticket(b["queue_id"], "t")
    await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=c["queue_id"])


async def test_a_ticket_is_bound_to_its_token_and_dataset(clock):
    mgr = _manager(clock)
    await mgr.create_session("/d/c.fits", owner_token="t")
    b = await _queue(mgr)
    with pytest.raises(rm.RenderTicketGone):
        await mgr.create_session("/d/c.fits", owner_token="other", ticket_id=b["queue_id"])
    with pytest.raises(rm.RenderTicketGone):
        await mgr.create_session("/d/other.fits", owner_token="t", ticket_id=b["queue_id"])
    with pytest.raises(rm.RenderTicketGone):
        await mgr.create_session("/d/c.fits", owner_token="t", ticket_id="never-issued")


async def test_waiting_past_the_timeout_is_refused(clock):
    mgr = _manager(clock, queue_timeout_seconds=60)
    await mgr.create_session("/d/c.fits", owner_token="t")
    b = await _queue(mgr)
    for _ in range(6):                     # polled every 10 s: alive, but no slot
        clock.t += 10
        await _queue(mgr, ticket=b["queue_id"])
    clock.t += 10
    with pytest.raises(rm.RenderQueueTimeout, match="waited 60 s"):
        await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=b["queue_id"])
    assert (await mgr.snapshot())["queued"] == 0


async def test_a_client_that_stops_asking_loses_its_place_and_its_reserved_slot(clock):
    mgr = _manager(clock, queue_stale_seconds=15)
    a = await mgr.create_session("/d/c.fits", owner_token="t")
    b = await _queue(mgr)                  # will die
    c = await _queue(mgr)
    await mgr.close_session(a.session_id)  # the slot is reserved for B
    clock.t += 9
    assert (await _queue(mgr, ticket=c["queue_id"]))["position"] == 1
    clock.t += 9
    # B has not asked for 18 s: gone, and its reservation passes to C, whose
    # request now starts its session.
    await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=c["queue_id"])
    with pytest.raises(rm.RenderTicketGone):
        await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=b["queue_id"])
    snap = await mgr.snapshot()
    assert (snap["active_sessions"], snap["queued"]) == (1, 0)


async def test_a_freed_slot_goes_first_to_the_token_holding_fewest(clock):
    mgr = _manager(clock, max_sessions=3)
    for _ in range(2):
        await mgr.create_session("/d/c.fits", owner_token="u1")
    light = await mgr.create_session("/d/c.fits", owner_token="u2")
    first = await _queue(mgr, owner="u1")      # u1 asks first, already holds 2
    second = await _queue(mgr, owner="u2")     # u2 holds 1: goes ahead
    first = await _queue(mgr, owner="u1", ticket=first["queue_id"])
    assert (second["position"], first["position"]) == (1, 2)
    await mgr.close_session(light.session_id)  # u2 now holds none, u1 two
    await mgr.create_session("/d/c.fits", owner_token="u2", ticket_id=second["queue_id"])
    assert (await _queue(mgr, owner="u1", ticket=first["queue_id"]))["position"] == 1


async def test_one_token_is_served_in_arrival_order(clock):
    mgr = _manager(clock, max_sessions=2)
    for _ in range(2):
        await mgr.create_session("/d/c.fits", owner_token="t")
    places = [await _queue(mgr) for _ in range(3)]
    assert [p["position"] for p in places] == [1, 2, 3]


async def test_a_session_whose_process_ended_frees_its_slot(clock):
    mgr = _manager(clock)
    a = await mgr.create_session("/d/c.fits", owner_token="t")
    b = await _queue(mgr)
    a.ended = True                         # the child died; nobody called on_ended yet
    sess = await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=b["queue_id"])
    await asyncio.sleep(0)                 # its pipe is released in the background
    assert a.closed and await mgr.get_session(a.session_id) is None
    assert await mgr.get_session(sess.session_id) is sess


async def test_on_ended_evicts_at_once(clock):
    mgr = _manager(clock)
    a = await mgr.create_session("/d/c.fits", owner_token="t")
    a.ended = True
    a.on_ended()                           # what RenderSessionProcess calls
    await asyncio.sleep(0)
    await asyncio.sleep(0)
    assert await mgr.get_session(a.session_id) is None and a.closed


async def test_a_session_nobody_is_connected_to_gives_way_only_when_requests_wait(clock):
    mgr = _manager(clock, max_sessions=2, disconnect_grace_seconds=30)
    left = await mgr.create_session("/d/c.fits", owner_token="t")
    kept = await mgr.create_session("/d/c.fits", owner_token="t")
    mgr.attach(left.session_id)
    mgr.attach(kept.session_id)
    mgr.detach(left.session_id)            # the client switched to Local, or died
    clock.t += 31
    assert await mgr._reap_once() == []    # nobody waits: kept for a quick return
    b = await _queue(mgr)
    clock.t += 1
    assert await mgr._reap_once() == [left.session_id]
    assert left.closed and not kept.closed
    await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=b["queue_id"])


async def test_a_never_connected_session_counts_from_its_creation(clock):
    mgr = _manager(clock, disconnect_grace_seconds=30)
    orphan = await mgr.create_session("/d/c.fits", owner_token="t")
    waiting = await _queue(mgr)
    clock.t += 10
    await _queue(mgr, ticket=waiting["queue_id"])
    assert await mgr._reap_once() == []    # within the grace: the client may be connecting
    for _ in range(2):                     # the waiting client keeps asking
        clock.t += 10.5
        await _queue(mgr, ticket=waiting["queue_id"])
    assert await mgr._reap_once() == [orphan.session_id]


async def test_snapshot_reports_the_queue_and_the_callers_places(clock):
    mgr = _manager(clock)
    await mgr.create_session("/d/c.fits", owner_token="t")
    mine = await _queue(mgr, owner="t")
    await _queue(mgr, owner="u")
    snap = await mgr.snapshot_for_owner("t", multi_user=True)
    assert snap["queued"] == 2 and snap["queue_capacity"] == 8
    assert [q["queue_id"] for q in snap["queue"]] == [mine["queue_id"]]
    assert snap["sessions"][0]["clients"] == 0


async def test_a_killed_render_process_frees_its_slot_for_the_queue(monkeypatch):
    """Real child process: SIGKILL it and the waiting request gets the slot,
    with no idle timeout in between and no process left behind."""
    import functools
    monkeypatch.setattr(rm, "RenderSession",
                        functools.partial(RenderSessionProcess, factory=_FAKE))
    mgr = rm.RenderManager(max_sessions=1, idle_ttl_seconds=300, queue_max=4)
    a = await mgr.create_session("/d/c.fits", owner_token="t")
    pid = a._process.pid
    b = await _queue(mgr)
    os.kill(pid, signal.SIGKILL)
    deadline = time.monotonic() + 10
    while await mgr.get_session(a.session_id) is not None:
        assert time.monotonic() < deadline, "the dead session kept its slot"
        await asyncio.sleep(0.05)
    sess = await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=b["queue_id"])
    assert await sess.render_frame() == b""
    assert a.ended
    await mgr.stop()
    assert not sess._process.is_alive() and not a._process.is_alive()


async def test_a_request_past_its_timeout_gets_no_slot_and_is_told_why(clock):
    mgr = _manager(clock, queue_timeout_seconds=10, queue_stale_seconds=60)
    a = await mgr.create_session("/d/c.fits", owner_token="t")
    b = await _queue(mgr)
    clock.t += 11
    await mgr.close_session(a.session_id)
    await mgr._reap_once()                 # a slot is free, but not for B
    with pytest.raises(rm.RenderQueueTimeout):
        await mgr.create_session("/d/c.fits", owner_token="t", ticket_id=b["queue_id"])
    await mgr.create_session("/d/c.fits", owner_token="t")      # the slot is still free


async def test_a_webrtc_peer_keeps_its_session_and_the_grace_starts_when_it_leaves(
        clock, monkeypatch):
    peer = {"on": True}
    monkeypatch.setattr(rm, "_webrtc_attached", lambda sid: peer["on"])
    mgr = _manager(clock, disconnect_grace_seconds=30, queue_stale_seconds=60)
    a = await mgr.create_session("/d/c.fits", owner_token="t")
    b = await _queue(mgr)
    clock.t += 31
    await _queue(mgr, ticket=b["queue_id"])
    assert await mgr._reap_once() == []    # watched over WebRTC
    peer["on"] = False
    clock.t += 1
    await _queue(mgr, ticket=b["queue_id"])
    assert await mgr._reap_once() == []    # left a second ago: within the grace
    clock.t += 30
    await _queue(mgr, ticket=b["queue_id"])
    assert await mgr._reap_once() == [a.session_id]


async def test_a_process_session_closed_before_it_started_spawns_nothing():
    s = RenderSessionProcess("feedc0de09", "/d/c.fits", factory=_FAKE)
    await s.close()
    with pytest.raises(RuntimeError, match="closed"):
        await s.initialize()
    assert s._process is None


async def test_a_session_closed_while_it_starts_is_not_returned(clock, monkeypatch):
    mgr = _manager(clock)
    started = asyncio.Event()
    go_on = asyncio.Event()

    class _Slow(_Session):
        async def initialize(self) -> None:
            started.set()
            await go_on.wait()

    monkeypatch.setattr(rm, "RenderSession", _Slow)
    creating = asyncio.create_task(mgr.create_session("/d/c.fits", owner_token="t"))
    await started.wait()
    sid = next(iter(mgr._sessions))
    await mgr.close_session(sid)
    go_on.set()
    with pytest.raises(RuntimeError, match="closed while it started"):
        await creating
    assert (await mgr.snapshot())["active_sessions"] == 0


async def test_a_cancelled_start_frees_its_slot(clock, monkeypatch):
    started = asyncio.Event()

    class _Hangs(_Session):
        async def initialize(self) -> None:
            started.set()
            await asyncio.Event().wait()

    monkeypatch.setattr(rm, "RenderSession", _Hangs)
    mgr = _manager(clock)
    creating = asyncio.create_task(mgr.create_session("/d/c.fits", owner_token="t"))
    await started.wait()
    creating.cancel()
    with pytest.raises(asyncio.CancelledError):
        await creating
    assert (await mgr.snapshot())["active_sessions"] == 0


async def test_a_timed_out_request_does_not_count_against_its_owner(clock):
    mgr = _manager(clock, queue_timeout_seconds=10, queue_stale_seconds=60)
    s = await mgr.create_session("/d/c.fits", owner_token="x")
    await _queue(mgr, owner="a")           # will time out
    clock.t += 5
    live_a = await _queue(mgr, owner="a")
    await _queue(mgr, owner="b")
    clock.t += 6
    await mgr.close_session(s.session_id)
    await mgr._reap_once()
    # A's live request came before B's: it gets the slot.
    await mgr.create_session("/d/c.fits", owner_token="a", ticket_id=live_a["queue_id"])


async def test_a_slow_close_does_not_make_the_reaper_take_one_more(clock):
    mgr = _manager(clock, max_sessions=2, queue_stale_seconds=15, disconnect_grace_seconds=30)
    a = await mgr.create_session("/d/c.fits", owner_token="t")
    clock.t += 1                           # A is the longer unattended: taken first
    b = await mgr.create_session("/d/c.fits", owner_token="t")
    clock.t += 30
    await _queue(mgr)
    await _queue(mgr)
    clock.t += 14
    closing, resume = asyncio.Event(), asyncio.Event()

    async def slow_close() -> None:
        closing.set()
        await resume.wait()
        a.closed = True

    a.close = slow_close
    reaping = asyncio.create_task(mgr._reap_once())
    await closing.wait()
    clock.t += 2                           # both waiters went stale meanwhile
    resume.set()
    assert len(await reaping) == 1
    assert not b.closed
