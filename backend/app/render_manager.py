"""Session manager — owns the dict of active :class:`RenderSession`
instances, applies TTL eviction, and exposes telemetry.

There is exactly one ``RenderManager`` per backend process, instantiated
lazily by the render router. Sessions are keyed by ``session_id`` (a
generated uuid). The manager is intentionally thread-safe via an asyncio
lock so the FastAPI event loop can call into it freely while a
background TTL reaper coroutine prunes idle sessions.

Capacity rules
--------------

* ``VISIVO_RENDER_MAX_SESSIONS`` — hard cap, default 8. Above this we
  refuse new sessions with HTTP 503 ("server-side rendering capacity
  reached"). 8 V100s × ~2 sessions per GPU (each ~2-3 GB VRAM for a
  10 GB cube via bricked rendering) is a reasonable starting point on
  the Power9 testbed.
* ``VISIVO_RENDER_IDLE_TTL_SECONDS`` — default 300 s. A session with
  no commands for that long is closed. The client is expected to send
  a ``{"type": "keepalive"}`` no-op when an idle user is still looking
  at the static frame.
"""
from __future__ import annotations

import asyncio
import logging
import os
import time
import uuid
from typing import Any

from .render_session import RenderSession

logger = logging.getLogger("visivo.render")


class RenderManager:
    """Singleton session registry, lifecycle hooks, TTL reaper."""

    def __init__(
        self,
        *,
        max_sessions: int | None = None,
        idle_ttl_seconds: float | None = None,
    ):
        self._sessions: dict[str, RenderSession] = {}
        self._lock = asyncio.Lock()
        self._max_sessions = max_sessions or int(
            os.environ.get("VISIVO_RENDER_MAX_SESSIONS", "8"),
        )
        self._idle_ttl = idle_ttl_seconds or float(
            os.environ.get("VISIVO_RENDER_IDLE_TTL_SECONDS", "300"),
        )
        self._reaper_task: asyncio.Task | None = None

    async def start(self) -> None:
        """Kick off the TTL reaper. Idempotent."""
        if self._reaper_task is None or self._reaper_task.done():
            self._reaper_task = asyncio.create_task(self._reap_loop())

    async def stop(self) -> None:
        """Cancel the reaper and close every active session. App shutdown only."""
        if self._reaper_task is not None:
            self._reaper_task.cancel()
        async with self._lock:
            sessions = list(self._sessions.values())
            self._sessions.clear()
        for s in sessions:
            try:
                await s.close()
            except Exception:
                logger.exception("[render-manager] close on shutdown raised")

    async def create_session(self, dataset_path: str,
                             owner_token: str | None = None) -> RenderSession:
        """Spin up a new session for ``dataset_path``. Returns it, ready to use.

        Raises :class:`RuntimeError` when the max-sessions cap is hit so the
        caller (route handler) can map to HTTP 503.
        """
        async with self._lock:
            if len(self._sessions) >= self._max_sessions:
                raise RuntimeError(
                    f"Server-side rendering capacity reached "
                    f"({self._max_sessions} sessions active)."
                )
            sid = uuid.uuid4().hex
            sess = RenderSession(session_id=sid, dataset_path=dataset_path)
            # Bind the render session to the caller so another token holding
            # the id can't close, WebRTC-connect to or drive this stream.
            sess.owner_token = owner_token
            self._sessions[sid] = sess
        # Initialise OUTSIDE the lock — VTK setup can take seconds for a
        # large cube; holding the manager lock would block new
        # connections during that window.
        try:
            await sess.initialize()
        except Exception:
            await self._evict(sid)
            raise
        logger.info(
            "[render-manager] new session %s for %s (%d/%d active)",
            sid[:8] + "…", dataset_path, len(self._sessions), self._max_sessions,
        )
        return sess

    async def get_session(self, session_id: str) -> RenderSession | None:
        async with self._lock:
            return self._sessions.get(session_id)

    async def close_session(self, session_id: str) -> bool:
        """Explicit close from a DELETE endpoint or WebSocket disconnect.

        Returns ``True`` if the session existed and was closed,
        ``False`` if no such session was registered.
        """
        return await self._evict(session_id)

    # ── Telemetry helpers, surfaced by /v1/render/diagnostics ────────────

    async def snapshot_for_owner(self, token: str | None,
                                 multi_user: bool) -> dict[str, Any]:
        """Snapshot restricted to the render sessions ``token`` owns.

        Done inside the manager (under its own lock) rather than by poking at
        ``_sessions`` from a route. Unowned sessions stay visible only on a
        single-user backend, where there is nothing to isolate.
        """
        import secrets as _sec

        async with self._lock:
            mine = []
            for sess in list(self._sessions.values()):
                owner = getattr(sess, "owner_token", None)
                if owner is None:
                    if not multi_user:
                        mine.append(sess)
                elif token is not None and _sec.compare_digest(owner, token):
                    mine.append(sess)
        return self._render_snapshot(mine)

    async def snapshot(self) -> dict[str, Any]:
        """Return a snapshot summary for ops dashboards / health checks."""
        async with self._lock:
            sessions = list(self._sessions.values())
        return self._render_snapshot(sessions)

    def _render_snapshot(self, sessions: list[Any]) -> dict[str, Any]:
        """Build the snapshot payload for a (possibly filtered) session list."""
        return {
            "active_sessions": len(sessions),
            "capacity": self._max_sessions,
            "idle_ttl_seconds": self._idle_ttl,
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
                }
                for s in sessions
            ],
        }

    # ── Internals ────────────────────────────────────────────────────────

    async def _evict(self, session_id: str) -> bool:
        async with self._lock:
            sess = self._sessions.pop(session_id, None)
        if sess is None:
            return False
        try:
            await sess.close()
        except Exception:
            logger.exception("[render-manager] close raised for %s", session_id[:8] + "…")
        logger.info(
            "[render-manager] closed session %s — %d frames rendered, %.1f MB sent",
            session_id, sess.stats.frames_rendered,
            sess.stats.bytes_sent / 1e6,
        )
        return True

    async def _reap_once(self) -> list[str]:
        """One TTL-eviction pass: close every session idle past the TTL.

        Returns the ids that were evicted. Split out from :meth:`_reap_loop`
        so the eviction policy is unit-testable without waiting on the timer.
        """
        now = time.time()
        to_evict: list[str] = []
        async with self._lock:
            for sid, sess in self._sessions.items():
                if now - sess.stats.last_command_at > self._idle_ttl:
                    to_evict.append(sid)
        for sid in to_evict:
            logger.info(
                "[render-manager] TTL evict %s (idle > %.0fs)",
                sid, self._idle_ttl,
            )
            await self._evict(sid)
        return to_evict

    async def _reap_loop(self) -> None:
        """Background coroutine: every 30 s, evict sessions idle past TTL."""
        try:
            while True:
                await asyncio.sleep(30.0)
                await self._reap_once()
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
