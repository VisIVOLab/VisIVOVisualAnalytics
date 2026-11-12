"""Session manager — owns the dict of active :class:`RenderSession`
instances, the queue of requests waiting for one, applies TTL eviction,
and exposes telemetry.

There is exactly one ``RenderManager`` per backend process, instantiated
lazily by the render router. Sessions are keyed by ``session_id`` (a
generated uuid). The manager is intentionally thread-safe via an asyncio
lock so the FastAPI event loop can call into it freely while a
background reaper coroutine prunes idle sessions and stale queue places.

Capacity rules
--------------

* ``VISIVO_RENDER_MAX_SESSIONS`` — hard cap, default 8. 8 V100s × ~2
  sessions per GPU (each ~2-3 GB VRAM for a 10 GB cube via bricked
  rendering) is a reasonable starting point on the Power9 testbed.
* ``VISIVO_RENDER_IDLE_TTL_SECONDS`` — default 300 s. A session with
  no commands for that long is closed. The client is expected to send
  a ``{"type": "keepalive"}`` no-op when an idle user is still looking
  at the static frame.

The queue (issue #4)
--------------------

A request above the cap that asks to wait (``queue=True``) gets a queue
place instead of a refusal: a ticket id and its position. The client asks
again with the ticket (the same ``create_session`` request), which both
reports the position and proves the client is still there; when the
ticket's turn comes, a slot is *reserved* for it and its next poll turns
the reservation into a session. Polling, not a socket of its own: the
create path is plain HTTP (the client already polls it while a pyramid is
built), it passes SSH tunnels and proxies unchanged, and a client that dies
simply stops asking, which is how its place is reclaimed.

* ``VISIVO_RENDER_QUEUE_MAX`` — waiting requests, default 16; above it a
  request is refused as before (503). 0 turns the queue off.
* ``VISIVO_RENDER_QUEUE_TIMEOUT_SECONDS`` — default 600 s: a request that
  waited this long without its turn coming is refused (503).
* ``VISIVO_RENDER_QUEUE_STALE_SECONDS`` — default 15 s: a ticket not asked
  for this long is dropped (its client is gone), reservation included.
* ``VISIVO_RENDER_DISCONNECT_GRACE_SECONDS`` — default 30 s: while someone
  waits, a session that has had no client connected (no WebSocket, no
  WebRTC peer) for this long is closed and its slot passed on. Without
  waiters only the idle TTL applies, so a desktop that switched a view to
  Local keeps its session for a quick switch back.

Order: a freed slot goes to the waiting request whose token holds the
fewest sessions (active or reserved), ties by arrival. With one token (the
local backend, a single-user cluster job) that is plain FIFO; on a shared
backend a user with one view is served before a second view of a user
who already has several.

A session whose render process ended (a crash in VTK or the driver, a
kill) is removed at once, not at its idle timeout, so its slot is free.
"""
from __future__ import annotations

import asyncio
import functools
import logging
import os
import secrets
import sys
import time
import uuid
from collections import Counter
from dataclasses import dataclass
from typing import Any, Callable

from .render_session import RenderSession as _InProcessSession
from .render_session._process import RenderSessionProcess

# Each session renders in a process of its own (see render_session._process),
# so VTK and the JPEG encoder never hold the GIL of the event loop that serves
# the clients. VISIVO_RENDER_IN_PROCESS=1 keeps them in this process.
RenderSession = (_InProcessSession if os.environ.get("VISIVO_RENDER_IN_PROCESS") == "1"
                 else RenderSessionProcess)

logger = logging.getLogger("visivo.render")

# How often a waiting client is told to ask again, and how often the reaper
# runs (it also passes on the slots of sessions nobody is connected to).
POLL_AFTER_SECONDS = 2.0
_REAP_INTERVAL_S = 5.0


class RenderCapacityError(RuntimeError):
    """No slot, and no queue place either (queue off, full, or not asked for)."""


class RenderQueueTimeout(RenderCapacityError):
    """The request waited ``VISIVO_RENDER_QUEUE_TIMEOUT_SECONDS`` in vain."""


class RenderTicketGone(LookupError):
    """The queue ticket is unknown: never issued, dropped as stale, cancelled,
    already used, or another token's or another dataset's."""


class RenderQueued(Exception):
    """The request waits in the queue; ``status`` is what the client is told."""

    def __init__(self, status: dict[str, Any]) -> None:
        super().__init__(f"queued at position {status['position']}")
        self.status = status


@dataclass
class _Ticket:
    ticket_id: str
    owner: str | None
    key: tuple                  # (dataset path, hdu, factory): what it waits for
    seq: int                    # arrival order
    enqueued_at: float
    last_seen: float
    reserved_at: float | None = None   # a slot is held for it


def _env_float(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, default))
    except ValueError:
        logger.warning("[render-manager] %s is not a number, using %s", name, default)
        return float(default)


def _same_owner(a: str | None, b: str | None) -> bool:
    if a is None or b is None:
        return a is b
    return secrets.compare_digest(a, b)


def _webrtc_attached(session_id: str) -> bool:
    """A WebRTC peer is connected to the session. Only looked up when the
    WebRTC module was imported (an offer was made): importing it here would
    pull in aiortc for nothing, and without it no peer can exist."""
    mod = sys.modules.get(f"{__package__}.webrtc_session")
    return mod is not None and mod.get_active_webrtc_session(session_id) is not None


class RenderManager:
    """Singleton session registry, queue, lifecycle hooks, reaper."""

    def __init__(
        self,
        *,
        max_sessions: int | None = None,
        idle_ttl_seconds: float | None = None,
        queue_max: int | None = None,
        queue_timeout_seconds: float | None = None,
        queue_stale_seconds: float | None = None,
        disconnect_grace_seconds: float | None = None,
        clock: Callable[[], float] = time.monotonic,
    ):
        self._sessions: dict[str, RenderSession] = {}
        self._lock = asyncio.Lock()
        self._max_sessions = max_sessions or int(
            os.environ.get("VISIVO_RENDER_MAX_SESSIONS", "8"),
        )
        self._idle_ttl = idle_ttl_seconds or float(
            os.environ.get("VISIVO_RENDER_IDLE_TTL_SECONDS", "300"),
        )
        self._queue_max = max(0, int(queue_max if queue_max is not None
                                     else _env_float("VISIVO_RENDER_QUEUE_MAX", 16)))
        self._queue_timeout = (queue_timeout_seconds if queue_timeout_seconds is not None
                               else _env_float("VISIVO_RENDER_QUEUE_TIMEOUT_SECONDS", 600))
        # Never shorter than a few polls: a live client must not lose its place
        # to one slow answer.
        self._queue_stale = max(3 * POLL_AFTER_SECONDS, (
            queue_stale_seconds if queue_stale_seconds is not None
            else _env_float("VISIVO_RENDER_QUEUE_STALE_SECONDS", 15)))
        self._disconnect_grace = (
            disconnect_grace_seconds if disconnect_grace_seconds is not None
            else _env_float("VISIVO_RENDER_DISCONNECT_GRACE_SECONDS", 30))
        self._clock = clock
        self._tickets: dict[str, _Ticket] = {}
        self._seq = 0
        # Clients connected to each session (WebSockets), and since when a
        # session has had none; a session nobody ever connected to counts
        # from its creation.
        self._attached: Counter[str] = Counter()
        self._detached_at: dict[str, float] = {}
        self._background: set[asyncio.Task] = set()
        self._reaper_task: asyncio.Task | None = None

    async def start(self) -> None:
        """Kick off the reaper. Idempotent."""
        if self._reaper_task is None or self._reaper_task.done():
            self._reaper_task = asyncio.create_task(self._reap_loop())

    async def stop(self, budget_s: float | None = None) -> None:
        """Cancel the reaper and close every active session. App shutdown only.

        ``budget_s`` bounds the close of each session in a child process (they
        close at the same time, so it bounds the whole stop for them); an
        in-process session closes as it always does."""
        if self._reaper_task is not None:
            self._reaper_task.cancel()
        async with self._lock:
            sessions = list(self._sessions.values())
            self._sessions.clear()
            self._tickets.clear()
            self._attached.clear()
            self._detached_at.clear()
        # All at once: one after the other, a few slow children add up to a
        # shutdown the client kills half-done.
        results = await asyncio.gather(
            *(s.close(budget_s=budget_s) if isinstance(s, RenderSessionProcess) else s.close()
              for s in sessions),
            return_exceptions=True)
        for result in results:
            if isinstance(result, BaseException) and not isinstance(result, asyncio.CancelledError):
                logger.error("[render-manager] close on shutdown raised", exc_info=result)

    async def create_session(self, dataset_path: str,
                             owner_token: str | None = None,
                             hdu: int | None = None,
                             factory: str | None = None,
                             *,
                             queue: bool = False,
                             ticket_id: str | None = None) -> RenderSession:
        """Spin up a new session for ``dataset_path``. Returns it, ready to use.

        Above the cap: raises :class:`RenderQueued` with a queue place when
        ``queue`` (or a ``ticket_id``) asks to wait and the queue has room,
        else :class:`RenderCapacityError`. A ``ticket_id`` asks again for a
        place given before: :class:`RenderQueued` with the new position until
        its turn comes, then the session; :class:`RenderTicketGone` for a
        ticket that is no longer in the queue, :class:`RenderQueueTimeout`
        when it waited too long.
        """
        key = (dataset_path, hdu, factory)
        async with self._lock:
            now = self._clock()
            ended = self._pop_ended_locked()
            self._expire_tickets_locked(now)
            ticket: _Ticket | None = None
            if ticket_id is not None:
                ticket = self._tickets.get(ticket_id)
                if (ticket is None or not _same_owner(ticket.owner, owner_token)
                        or ticket.key != key):
                    raise RenderTicketGone("This request is no longer in the rendering queue.")
                ticket.last_seen = now
                if ticket.reserved_at is None and now - ticket.enqueued_at > self._queue_timeout:
                    del self._tickets[ticket.ticket_id]
                    raise RenderQueueTimeout(
                        f"Server-side rendering capacity reached: waited "
                        f"{self._queue_timeout:.0f} s in the queue without a free slot.")
            self._promote_locked(now)
            if ticket is not None and ticket.reserved_at is None:
                raise RenderQueued(self._status_locked(ticket, now))
            if ticket is None and self._free_locked() <= 0:
                if not queue or self._queue_max == 0:
                    raise RenderCapacityError(
                        f"Server-side rendering capacity reached "
                        f"({self._max_sessions} sessions active).")
                if len(self._tickets) >= self._queue_max:
                    raise RenderCapacityError(
                        f"Server-side rendering capacity reached ({self._max_sessions} "
                        f"sessions active, {len(self._tickets)} requests waiting).")
                self._seq += 1
                ticket = _Ticket(uuid.uuid4().hex, owner_token, key, self._seq, now, now)
                self._tickets[ticket.ticket_id] = ticket
                logger.info("[render-manager] queued a request for %s (%d waiting)",
                            dataset_path, len(self._tickets))
                raise RenderQueued(self._status_locked(ticket, now))
            if ticket is not None:
                # Its reservation becomes this session.
                del self._tickets[ticket.ticket_id]
            sid = uuid.uuid4().hex
            # ``hdu``: the HDU, or a VBT volume's field (app/volume_source.py);
            # None, the file's first image HDU.
            # ``factory``: the session class, "module:Class"; None, a cube's
            # (point data: app.render_session.points, issue #83).
            kwargs = {} if hdu is None else {"hdu": hdu}
            if factory is None:
                sess = RenderSession(session_id=sid, dataset_path=dataset_path, **kwargs)
            elif RenderSession is RenderSessionProcess:
                sess = RenderSessionProcess(sid, dataset_path, factory=factory, **kwargs)
            else:
                module, _, name = factory.partition(":")
                cls = getattr(__import__(module, fromlist=[name]), name)
                sess = cls(sid, dataset_path, **kwargs)
            # Bind the render session to the caller so another token holding
            # the id can't close, WebRTC-connect to or drive this stream.
            sess.owner_token = owner_token
            if hasattr(sess, "on_ended"):
                # A render process that dies frees its slot now.
                sess.on_ended = functools.partial(self._session_ended, sid)
            self._sessions[sid] = sess
        if ended:
            # In the background: the new session starts now, and nothing can
            # evict it before its initialize below has begun.
            self._in_background(self._close_all(ended))
        # Initialise OUTSIDE the lock — VTK setup can take seconds for a
        # large cube; holding the manager lock would block new
        # connections during that window.
        try:
            await sess.initialize()
        except BaseException:
            # Cancelled too (the request went away): the slot must not stay
            # taken by a session nobody will ever connect to.
            await asyncio.shield(self._evict(sid))
            raise
        if sid not in self._sessions:
            # Closed while it started (a DELETE, shutdown, its process died).
            raise RuntimeError(f"Render session {sid[:8]} was closed while it started.")
        # The disconnect grace counts from here, not from while it started:
        # a session still starting is never taken for one nobody watches.
        if self._attached.get(sid, 0) == 0:
            self._detached_at[sid] = self._clock()
        logger.info(
            "[render-manager] new session %s for %s (%d/%d active)",
            sid[:8] + "…", dataset_path, len(self._sessions), self._max_sessions,
        )
        return sess

    async def cancel_ticket(self, ticket_id: str, owner_token: str | None) -> bool:
        """Give up a queue place (the client stopped waiting). False when the
        ticket is unknown or another token's."""
        async with self._lock:
            ticket = self._tickets.get(ticket_id)
            if ticket is None or not _same_owner(ticket.owner, owner_token):
                return False
            del self._tickets[ticket_id]
            self._promote_locked(self._clock())
        return True

    async def get_session(self, session_id: str) -> RenderSession | None:
        async with self._lock:
            return self._sessions.get(session_id)

    async def close_session(self, session_id: str) -> bool:
        """Explicit close from a DELETE endpoint.

        Returns ``True`` if the session existed and was closed,
        ``False`` if no such session was registered.
        """
        return await self._evict(session_id)

    def attach(self, session_id: str) -> None:
        """A client connected to the session (a WebSocket opened)."""
        if session_id in self._sessions:
            self._attached[session_id] += 1
            self._detached_at.pop(session_id, None)

    def clients(self, session_id: str) -> int:
        """WebSockets connected to the session now."""
        return self._attached.get(session_id, 0)

    def detach(self, session_id: str) -> None:
        """A client went away; the last one starts the disconnect grace."""
        if self._attached.get(session_id, 0) <= 0:
            return
        self._attached[session_id] -= 1
        if self._attached[session_id] == 0:
            del self._attached[session_id]
            if session_id in self._sessions:
                self._detached_at[session_id] = self._clock()

    # ── Telemetry helpers, surfaced by /v1/render/diagnostics ────────────

    async def snapshot_for_owner(self, token: str | None,
                                 multi_user: bool) -> dict[str, Any]:
        """Snapshot restricted to the render sessions and queue places
        ``token`` owns.

        Done inside the manager (under its own lock) rather than by poking at
        ``_sessions`` from a route. Unowned sessions stay visible only on a
        single-user backend, where there is nothing to isolate.
        """
        def mine(owner: str | None) -> bool:
            if owner is None:
                return not multi_user
            return token is not None and secrets.compare_digest(owner, token)

        async with self._lock:
            sessions = [s for s in self._sessions.values()
                        if mine(getattr(s, "owner_token", None))]
            tickets = [t for t in self._tickets.values() if mine(t.owner)]
            return self._render_snapshot(sessions, tickets)

    async def snapshot(self) -> dict[str, Any]:
        """Return a snapshot summary for ops dashboards / health checks."""
        async with self._lock:
            return self._render_snapshot(list(self._sessions.values()),
                                         list(self._tickets.values()))

    def _render_snapshot(self, sessions: list[Any], tickets: list[_Ticket]) -> dict[str, Any]:
        """Build the snapshot payload for a (possibly filtered) session list.
        Under the lock: positions are read from the queue."""
        now = self._clock()
        return {
            "active_sessions": len(sessions),
            "capacity": self._max_sessions,
            "idle_ttl_seconds": self._idle_ttl,
            # The whole backend's, as ``capacity``: how busy it is is not
            # anyone's private data.
            "queued": len(self._tickets),
            "queue_capacity": self._queue_max,
            "queue_timeout_seconds": self._queue_timeout,
            "queue": [self._status_locked(t, now) for t in tickets],
            "sessions": [
                {
                    "session_id": s.session_id,
                    "dataset_path": s.dataset_path,
                    "frames_rendered": s.stats.frames_rendered,
                    "bytes_sent": s.stats.bytes_sent,
                    "commands_received": s.stats.commands_received,
                    "last_render_ms": s.stats.last_render_ms,
                    "last_jpeg_kb": s.stats.last_jpeg_kb,
                    "age_seconds": time.time() - s.stats.created_at,
                    "idle_seconds": time.time() - s.stats.last_command_at,
                    "clients": self._attached.get(s.session_id, 0),
                }
                for s in sessions
            ],
        }

    # ── Queue internals (all under the lock) ─────────────────────────────

    def _free_locked(self) -> int:
        reserved = sum(1 for t in self._tickets.values() if t.reserved_at is not None)
        return self._max_sessions - len(self._sessions) - reserved

    def _waiting_order_locked(self, now: float) -> list[_Ticket]:
        """The tickets without a slot, in the order slots will go to them:
        the owner holding the fewest sessions (active or reserved) first,
        counting each slot handed out on the way, ties by arrival. One that
        waited past the timeout gets no slot (its next request is refused
        with the reason, see create_session) and is left out."""
        holdings: Counter = Counter(getattr(s, "owner_token", None)
                                    for s in self._sessions.values())
        holdings.update(t.owner for t in self._tickets.values() if t.reserved_at is not None)
        waiting = sorted((t for t in self._tickets.values() if t.reserved_at is None
                          and now - t.enqueued_at <= self._queue_timeout),
                         key=lambda t: t.seq)
        order: list[_Ticket] = []
        while waiting:
            best = min(waiting, key=lambda t: (holdings[t.owner], t.seq))
            waiting.remove(best)
            holdings[best.owner] += 1
            order.append(best)
        return order

    def _promote_locked(self, now: float) -> None:
        """Reserve each free slot for the next waiting ticket."""
        free = self._free_locked()
        if free <= 0:
            return
        for ticket in self._waiting_order_locked(now)[:free]:
            ticket.reserved_at = now
            logger.info("[render-manager] a slot is reserved for a waiting request")

    def _expire_tickets_locked(self, now: float) -> None:
        """Drop the tickets whose client stopped asking: it is gone, and so
        is a slot reserved for it."""
        for tid, ticket in list(self._tickets.items()):
            if now - ticket.last_seen > self._queue_stale:
                del self._tickets[tid]
                logger.info("[render-manager] dropped a queue place not asked for in %.0f s%s",
                            self._queue_stale, " (its reserved slot is free)"
                            if ticket.reserved_at is not None else "")

    def _status_locked(self, ticket: _Ticket, now: float) -> dict[str, Any]:
        order = self._waiting_order_locked(now)
        # A reserved ticket is next in line: its next poll starts its session.
        # One past the timeout is behind everyone (refused when it asks).
        if ticket.reserved_at is not None:
            position = 0
        elif ticket in order:
            position = order.index(ticket) + 1
        else:
            position = len(order) + 1
        return {
            "queue_id": ticket.ticket_id,
            "position": position,
            "queue_length": len(order),
            "active_sessions": len(self._sessions),
            "capacity": self._max_sessions,
            "waited_seconds": round(now - ticket.enqueued_at, 1),
            "wait_timeout_seconds": self._queue_timeout,
            "poll_after_seconds": POLL_AFTER_SECONDS,
            "expires_after_seconds": self._queue_stale,
        }

    def _pop_ended_locked(self) -> list[Any]:
        """Remove the sessions whose render process has ended; the caller
        closes them (releases their pipe) once the lock is let go."""
        ended = [sid for sid, s in self._sessions.items() if getattr(s, "ended", False)]
        return [self._forget_locked(sid) for sid in ended]

    def _forget_locked(self, session_id: str) -> Any:
        self._attached.pop(session_id, None)
        self._detached_at.pop(session_id, None)
        return self._sessions.pop(session_id, None)

    # ── Internals ────────────────────────────────────────────────────────

    def _in_background(self, coro) -> None:
        task = asyncio.ensure_future(coro)
        self._background.add(task)
        task.add_done_callback(self._background.discard)

    def _session_ended(self, session_id: str) -> None:
        """Called on the event loop when a session's render process ended."""
        self._in_background(self._evict(session_id))

    async def _close_all(self, sessions: list[Any],
                         reason: str = "ended with its render process") -> None:
        """Close sessions already removed from the registry."""
        for sess in sessions:
            if sess is None:
                continue
            logger.info("[render-manager] session %s closed: %s",
                        sess.session_id[:8] + "…", reason)
            try:
                await sess.close()
            except Exception:
                logger.exception("[render-manager] close raised for %s",
                                 sess.session_id[:8] + "…")

    async def _evict(self, session_id: str) -> bool:
        async with self._lock:
            sess = self._forget_locked(session_id)
        if sess is None:
            return False
        try:
            await sess.close()
        except Exception:
            logger.exception("[render-manager] close raised for %s", session_id[:8] + "…")
        logger.info(
            "[render-manager] closed session %s - %d frames rendered, %.1f MB sent",
            session_id, sess.stats.frames_rendered,
            sess.stats.bytes_sent / 1e6,
        )
        return True

    async def _reap_once(self) -> list[str]:
        """One reaper pass: close every session idle past the TTL, and, while
        requests wait, the sessions no client has been connected to for the
        disconnect grace (oldest first, one per waiting request). Drops
        stale queue places and reserves the slots freed.

        Returns the ids that were evicted. Split out from :meth:`_reap_loop`
        so the policy is unit-testable without waiting on the timer.
        """
        wall, now = time.time(), self._clock()
        to_evict: list[str] = []
        async with self._lock:
            ended = self._pop_ended_locked()
            self._expire_tickets_locked(now)
            for sid, sess in self._sessions.items():
                if wall - sess.stats.last_command_at > self._idle_ttl:
                    logger.info("[render-manager] TTL evict %s (idle > %.0fs)",
                                sid, self._idle_ttl)
                    to_evict.append(sid)
            # A WebRTC peer is not counted by attach(): while one is seen, the
            # session is not unattended, and its grace counts from the last
            # pass that saw it.
            for sid in self._detached_at:
                if _webrtc_attached(sid):
                    self._detached_at[sid] = now
        await self._close_all(ended)
        for sid in to_evict:
            await self._evict(sid)
        # While requests wait, pass on the slots of sessions nobody watches,
        # one at a time: each is chosen and removed under the lock, so one
        # whose client came back meanwhile is not taken.
        while True:
            async with self._lock:
                sess = self._pop_unattended_locked(self._clock())
            if sess is None:
                break
            to_evict.append(sess.session_id)
            await self._close_all([sess], reason="no client connected while requests wait")
        async with self._lock:
            now = self._clock()
            self._expire_tickets_locked(now)
            self._promote_locked(now)
        return to_evict

    def _pop_unattended_locked(self, now: float) -> Any:
        """The session no client has been connected to for longest past the
        disconnect grace, removed, if a waiting request needs its slot."""
        self._expire_tickets_locked(now)
        waiting = len(self._waiting_order_locked(now))
        if waiting - max(0, self._free_locked()) <= 0:
            return None
        unattended = sorted((since, sid) for sid, since in self._detached_at.items()
                            if now - since > self._disconnect_grace
                            and not _webrtc_attached(sid))
        if not unattended:
            return None
        return self._forget_locked(unattended[0][1])

    async def _reap_loop(self) -> None:
        """Background coroutine: a reaper pass every few seconds."""
        try:
            while True:
                await asyncio.sleep(_REAP_INTERVAL_S)
                try:
                    await self._reap_once()
                except Exception:
                    logger.exception("[render-manager] reaper pass failed")
        except asyncio.CancelledError:
            return


# Module-level singleton. The router creates / starts it lazily so a
# CPU-only backend (no EGL) never even imports this module.
_MANAGER: RenderManager | None = None


def get_render_manager() -> RenderManager:
    """Return the process-wide singleton, creating it on first use."""
    global _MANAGER
    if _MANAGER is None:
        _MANAGER = RenderManager()
    return _MANAGER
