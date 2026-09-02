"""
sessions.py – R3: Per-session dataset registry with TTL eviction.

Each call to POST /v1/datasets/open returns a dataset_id that belongs
to a Session.  Sessions are identified by a UUID returned in the
OpenDatasetResponse and must be included in the X-Visivo-Session header
of all subsequent requests for that dataset.

If the session header is absent a default "anonymous" session is used
for backward compatibility with the current Qt client, but a warning is
emitted.  Future client versions should always send the session header.

Stale sessions (idle beyond VISIVO_SESSION_TTL seconds, default 1800)
are evicted by a background daemon thread.

Anonymous session resource management
--------------------------------------
The anonymous session is never *removed* from the registry (backward-compat
clients expect it to be permanently available), but its datasets ARE flushed
on the same TTL schedule as normal sessions.  Flushing means:

  1. Under the registry lock, the ``datasets`` dict is swapped out atomically
     so concurrent requests cannot acquire a stale entry.
  2. Outside the lock, every VBT ``np.memmap`` in the evicted snapshot is
     explicitly closed via ``_safe_close_memmap()``, releasing OS file
     descriptors immediately.

This prevents unbounded FD accumulation in long-running server processes
while preserving the anonymous session identity.
"""

from __future__ import annotations

import logging
import os
import threading
import time
import uuid
from pathlib import Path
from typing import Any

_MAX_CONCURRENT_TASKS = int(os.environ.get("VISIVO_MAX_CONCURRENT_TASKS", "3"))

logger = logging.getLogger("visivo.sessions")

_TTL_SECONDS = int(os.environ.get("VISIVO_SESSION_TTL", "1800"))
_ANON_SESSION_ID = "anonymous"


def _safe_close_memmap(arr: object) -> None:
    """Close a numpy memmap's underlying file descriptor safely.

    Calling ``del arr`` alone is not sufficient on non-CPython runtimes because
    the garbage collector may not run immediately.  Explicitly closing the
    ``_mmap`` attribute releases the OS file descriptor right away.

    Safe to call multiple times (idempotent) and on non-memmap objects (no-op).
    """
    try:
        mmap_obj = getattr(arr, "_mmap", None)
        if mmap_obj is not None:
            mmap_obj.close()
    except Exception:
        pass


class Session:
    """Holds all datasets opened by a single client connection."""

    def __init__(self, session_id: str) -> None:
        self.session_id = session_id
        # dataset_id → {path, kind, width, height, depth, …}
        self.datasets: dict[str, dict[str, Any]] = {}
        self.created_at: float = time.monotonic()
        self.last_accessed: float = time.monotonic()
        # R-02/R-03: operation provenance history (append-only, newest last)
        # Each entry is a dict with at least: operation, dataset_id, timestamp, params
        self.history: list[dict[str, Any]] = []
        # R-03 enhanced: HMAC-signed named snapshots, snapshot_id → record
        # {state, hmac, label}. Captures session datasets + history at snapshot time.
        self.snapshots: dict[str, dict[str, Any]] = {}
        # Per-session concurrency limit (HPC safety)
        self._active_tasks: int = 0
        self._task_lock: threading.Lock = threading.Lock()
        # The auth token that created this session. When multiple per-user tokens
        # are configured, a request presenting a DIFFERENT token is denied access
        # (defence-in-depth over the unguessable session id).
        self.owner_token: str | None = None
        # Per-session scientific-copilot LLM provider config + built provider, so
        # users sharing one backend configure independent providers/keys. Lazily
        # initialised (a fresh env-seeded copy) on first access.
        self.assistant_config: dict[str, Any] | None = None
        self.assistant_provider: Any = None

    def touch(self) -> None:
        self.last_accessed = time.monotonic()

    def idle_seconds(self) -> float:
        return time.monotonic() - self.last_accessed

    def close(self) -> None:
        """Release all resources held by this session.

        Explicitly closes any ``np.memmap`` objects stored in VBT dataset
        entries so that OS file descriptors are freed immediately rather than
        waiting for the garbage collector.
        """
        for dataset_id, entry in self.datasets.items():
            if entry.get("kind") == "vbt":
                mm = entry.get("memmap")
                if mm is not None:
                    _safe_close_memmap(mm)
                    logger.debug(
                        "[sessions] closed memmap dataset_id=%s session_id=%s",
                        dataset_id,
                        self.session_id,
                    )

    def try_acquire_task_slot(self) -> bool:
        """Atomically increment active-task counter if below the limit.
        Returns True if the slot was acquired, False if the session is at capacity."""
        with self._task_lock:
            if self._active_tasks >= _MAX_CONCURRENT_TASKS:
                return False
            self._active_tasks += 1
            return True

    def release_task_slot(self) -> None:
        with self._task_lock:
            self._active_tasks = max(0, self._active_tasks - 1)

    def has_active_tasks(self) -> bool:
        """True while a compute task (e.g. a copilot run) holds a slot — used to
        avoid evicting/flushing datasets a running task may still be reading."""
        with self._task_lock:
            return self._active_tasks > 0


class SessionRegistry:
    """Thread-safe registry of active sessions with background TTL eviction."""

    def __init__(self, ttl_seconds: int = _TTL_SECONDS) -> None:
        self._sessions: dict[str, Session] = {}
        self._lock = threading.Lock()
        self._ttl = ttl_seconds
        self._start_eviction_thread()

    # ── Public API ────────────────────────────────────────────────────────────

    def create(self, session_id: str | None = None,
               owner_token: str | None = None) -> Session:
        """Create a new session and register it. A ``session_id`` may be supplied
        so a client can ADOPT its own (unguessable) id for isolation — matched
        against future requests — instead of a server-minted one. ``owner_token``
        binds the session to the caller's identity for cross-user access control."""
        session_id = session_id or str(uuid.uuid4())
        with self._lock:
            # Idempotent: two concurrent first-requests for the same adopted id
            # must share one session, not clobber each other's datasets.
            existing = self._sessions.get(session_id)
            if existing is not None:
                existing.touch()
                return existing
            session = Session(session_id)
            session.owner_token = owner_token
            self._sessions[session_id] = session
        logger.info("[sessions] created session_id=%s", session_id)
        return session

    def get(self, session_id: str) -> Session | None:
        """Return the session or None if it does not exist / has been evicted."""
        with self._lock:
            session = self._sessions.get(session_id)
            if session is not None:
                session.touch()
            return session

    def get_or_create_anon(self) -> Session:
        """Return (or lazily create) the anonymous backward-compat session."""
        with self._lock:
            if _ANON_SESSION_ID not in self._sessions:
                self._sessions[_ANON_SESSION_ID] = Session(_ANON_SESSION_ID)
                logger.warning(
                    "[sessions] Created anonymous session (no X-Visivo-Session header). "
                    "Multi-user isolation is disabled for this request."
                )
            session = self._sessions[_ANON_SESSION_ID]
            session.touch()
            return session

    def stats(self) -> dict[str, Any]:
        with self._lock:
            return {
                "active_sessions": len(self._sessions),
                "ttl_seconds": self._ttl,
                "sessions": [
                    {
                        "session_id": s.session_id,
                        "datasets": len(s.datasets),
                        "idle_seconds": round(s.idle_seconds()),
                    }
                    for s in self._sessions.values()
                ],
            }

    # ── Eviction ─────────────────────────────────────────────────────────────

    def _flush_anon_datasets(self) -> int:
        """Flush stale datasets from the anonymous session without removing it.

        Atomically swaps out the anonymous session's ``datasets`` dict under the
        registry lock, then closes every VBT memmap in the snapshot outside the
        lock (so the lock is not held during I/O).

        Returns the number of dataset entries that were evicted (0 if the
        anonymous session does not exist or was not yet stale).

        Thread-safe.  Safe to call from the eviction thread or from tests.
        """
        evicted: dict[str, dict] = {}

        with self._lock:
            anon = self._sessions.get(_ANON_SESSION_ID)
            if anon is None or anon.idle_seconds() <= self._ttl:
                return 0
            # A running compute task (e.g. a copilot run that outlived its request
            # deadline) may still be reading a VBT memmap in these datasets — never
            # flush them out from under it.
            if anon.has_active_tasks():
                return 0
            # Swap out the datasets dict atomically while holding the lock so
            # concurrent requests cannot acquire a reference to a stale entry.
            evicted = anon.datasets
            anon.datasets = {}
            # Reset idle timer so the next eviction cycle does not immediately
            # re-flush a session that now has zero datasets.
            anon.touch()

        # Close VBT memmaps outside the lock — I/O must not hold registry lock.
        for dataset_id, entry in evicted.items():
            if entry.get("kind") == "vbt":
                mm = entry.get("memmap")
                if mm is not None:
                    _safe_close_memmap(mm)
                    logger.debug(
                        "[sessions] closed anonymous memmap dataset_id=%s",
                        dataset_id,
                    )

        if evicted:
            logger.info(
                "[sessions] flushed %d stale dataset(s) from anonymous session",
                len(evicted),
            )
        return len(evicted)

    def _evict_stale(self) -> None:
        while True:
            time.sleep(min(300, self._ttl // 2))

            # Collect all stale normal sessions and pop them under a single
            # lock acquisition so the lock is not re-taken per session.
            stale_sessions: list[Session] = []
            with self._lock:
                stale_ids = [
                    sid
                    for sid, s in self._sessions.items()
                    if sid != _ANON_SESSION_ID and s.idle_seconds() > self._ttl
                    and not s.has_active_tasks()   # don't evict under a running task
                ]
                for sid in stale_ids:
                    stale_sessions.append(self._sessions.pop(sid))

            # Close resources outside the lock.
            for session in stale_sessions:
                session.close()
                logger.info("[sessions] evicted stale session_id=%s", session.session_id)

            # Anonymous session: flush stale datasets (FD fix — M3).
            # The session itself stays in the registry.
            self._flush_anon_datasets()

    def _start_eviction_thread(self) -> None:
        t = threading.Thread(target=self._evict_stale, daemon=True, name="visivo-session-eviction")
        t.start()


# Module-level singleton.
REGISTRY: SessionRegistry = SessionRegistry()


# ── FastAPI dependency ────────────────────────────────────────────────────────

import secrets as _secrets  # noqa: E402

from fastapi import Depends, Header, HTTPException, status  # noqa: E402 – avoid circular imports

from .auth import verify_token  # noqa: E402


async def get_session(x_visivo_session: str | None = Header(None),
                      token: str = Depends(verify_token)) -> Session:
    """
    FastAPI dependency: resolve (or lazily create) the caller's session, enforcing
    session ownership.

    The Qt client sends its private session id in the X-Visivo-Session header. If
    absent the shared anonymous session is used (single-user / backward compat).
    A session created by one token can't be accessed by another (multi-user).
    """
    if x_visivo_session is None:
        return REGISTRY.get_or_create_anon()

    session = REGISTRY.get(x_visivo_session)
    if session is not None:
        # Deny cross-user access. Report 404 (not 403) so the mere existence of
        # another user's session isn't confirmable.
        if session.owner_token is not None \
                and not _secrets.compare_digest(session.owner_token, token):
            raise HTTPException(status.HTTP_404_NOT_FOUND, "Session not found.")
        return session

    # First request from a client that minted its own id (multi-desktop
    # isolation), or a session that expired: create+register UNDER THAT id, owned
    # by this token, so the client's subsequent requests all land here.
    logger.info("[sessions] adopting client session_id=%s", x_visivo_session)
    return REGISTRY.create(x_visivo_session, owner_token=token)
