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
import secrets as _secrets
import threading
import time
import uuid
from pathlib import Path
from typing import Any

_MAX_CONCURRENT_TASKS = int(os.environ.get("VISIVO_MAX_CONCURRENT_TASKS", "3"))

logger = logging.getLogger("visivo.sessions")

_TTL_SECONDS = int(os.environ.get("VISIVO_SESSION_TTL", "1800"))
_ANON_SESSION_ID = "anonymous"
# Strict sessions: refuse to ADOPT a client-invented id — clients must ask the
# server to mint one (POST /v1/sessions/new). This makes ids fully
# non-enumerable: an unknown id and someone else's id both answer 404, whereas
# adoption answers 200 for unknown ids. Defaults ON once per-user tokens exist;
# VISIVO_STRICT_SESSIONS=0/1 overrides either way.
_STRICT_SESSIONS_ENV = os.environ.get("VISIVO_STRICT_SESSIONS", "").strip()
# Cap live sessions per auth token so one caller can't spam adopted ids.
_MAX_SESSIONS_PER_TOKEN = int(os.environ.get("VISIVO_MAX_SESSIONS_PER_TOKEN", "32"))


_MIN_ADOPTABLE_ID_LEN = 32
_ADOPTABLE_ID_CHARS = set(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")


def _id_is_high_entropy(session_id: str) -> bool:
    """A client may only ADOPT an id that is long and random-looking (a UUID or
    equivalent). A short/guessable id could be PRE-CLAIMED by another user before
    its owner's first request (session fixation), so we refuse those outright."""
    return (len(session_id) >= _MIN_ADOPTABLE_ID_LEN
            and all(c in _ADOPTABLE_ID_CHARS for c in session_id))


def strict_sessions() -> bool:
    """Whether client-invented session ids are refused (see _STRICT_SESSIONS_ENV)."""
    env = os.environ.get("VISIVO_STRICT_SESSIONS", _STRICT_SESSIONS_ENV).strip()
    if env:
        return env not in ("0", "false", "False", "no")
    from .auth import multi_user_mode
    return multi_user_mode()


def _redact(session_id: str) -> str:
    """Session ids are bearer-like secrets — log only a short, non-reusable prefix."""
    return f"{session_id[:8]}…" if len(session_id) > 8 else "…"


class SessionQuotaExceeded(RuntimeError):
    """Raised when a token already holds the maximum number of live sessions."""


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
                        _redact(self.session_id),
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

    def __init__(self, ttl_seconds: int = _TTL_SECONDS,
                 start_eviction: bool = True) -> None:
        self._sessions: dict[str, Session] = {}
        self._lock = threading.Lock()
        self._ttl = ttl_seconds
        # Tests build registries with a tiny TTL to exercise flushing; leaving the
        # background thread running would race their explicit flush call.
        if start_eviction:
            self._start_eviction_thread()

    # ── Public API ────────────────────────────────────────────────────────────

    def _enforce_quota_locked(self, owner_token: str | None) -> None:
        """Raise SessionQuotaExceeded if ``owner_token`` is at its session cap.
        Caller must hold ``self._lock``."""
        if owner_token is None:
            return
        mine = sum(1 for x in self._sessions.values()
                   if x.owner_token is not None
                   and _secrets.compare_digest(x.owner_token, owner_token))
        if mine >= _MAX_SESSIONS_PER_TOKEN:
            raise SessionQuotaExceeded(
                f"Too many live sessions for this token ({_MAX_SESSIONS_PER_TOKEN}).")

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
            self._enforce_quota_locked(owner_token)
            session = Session(session_id)
            session.owner_token = owner_token
            self._sessions[session_id] = session
        logger.info("[sessions] created session_id=%s", _redact(session_id))
        return session

    def get_or_adopt(self, session_id: str, owner_token: str | None,
                     require_owned: bool = False,
                     allow_create: bool = True) -> Session | None:
        """Atomically resolve a client-supplied session id, enforcing ownership.

        Under a SINGLE lock acquisition (so two users racing the same brand-new id
        cannot end up sharing one session): returns the existing session when it is
        unowned or owned by ``owner_token``; returns ``None`` when it belongs to a
        DIFFERENT token (caller reports 404); otherwise creates + registers it
        owned by ``owner_token``.
        """
        with self._lock:
            existing = self._sessions.get(session_id)
            if existing is not None:
                if existing.owner_token is None:
                    # Unowned (anonymous / legacy). Shareable only in single-user
                    # mode; with several tokens configured it must not be a
                    # back door into someone else's datasets.
                    if require_owned:
                        return None
                elif owner_token is None or not _secrets.compare_digest(
                        existing.owner_token, owner_token):
                    return None          # another user's session
                existing.touch()
                return existing
            if not allow_create:
                # Strict mode: unknown ids are NOT adopted, and answer exactly
                # like someone else's id (None → 404), so ids stay unguessable
                # AND non-enumerable.
                return None
            self._enforce_quota_locked(owner_token)
            session = Session(session_id)
            session.owner_token = owner_token
            self._sessions[session_id] = session
        logger.info("[sessions] adopted client session_id=%s", _redact(session_id))
        return session

    def peek(self, session_id: str) -> Session | None:
        """Look a session up WITHOUT touching its idle timer — used by
        ownership checks so a stranger holding a leaked id can't keep another
        user's session alive just by probing it."""
        with self._lock:
            return self._sessions.get(session_id)

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
                        # Redacted: a session id is bearer-like (it grants access
                        # to that session's datasets), so diagnostics must not
                        # hand out usable ids.
                        "session_id": _redact(s.session_id),
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
                logger.info("[sessions] evicted stale session_id=%s", _redact(session.session_id))

            # Anonymous session: flush stale datasets (FD fix — M3).
            # The session itself stays in the registry.
            self._flush_anon_datasets()

    def _start_eviction_thread(self) -> None:
        t = threading.Thread(target=self._evict_stale, daemon=True, name="visivo-session-eviction")
        t.start()


# Module-level singleton.
REGISTRY: SessionRegistry = SessionRegistry()


# ── FastAPI dependency ────────────────────────────────────────────────────────

from fastapi import Depends, Header, HTTPException, status  # noqa: E402 – avoid circular imports

from .auth import multi_user_mode, verify_token  # noqa: E402


async def get_session(x_visivo_session: str | None = Header(None),
                      token: str = Depends(verify_token)) -> Session:
    """
    FastAPI dependency: resolve (or lazily create) the caller's session, enforcing
    session ownership.

    The Qt client sends its private session id in the X-Visivo-Session header.
    With a SINGLE token (the default, single-user backend) an absent header falls
    back to the shared anonymous session for backward compatibility. As soon as
    several per-user tokens are configured that shared bucket would defeat
    isolation — any user could simply omit the header and reach everyone's
    datasets — so the header becomes mandatory.

    A session created by one token is invisible to another (404, not 403, so the
    existence of someone else's session can't be probed).
    """
    if x_visivo_session is None:
        if multi_user_mode():
            raise HTTPException(
                status.HTTP_400_BAD_REQUEST,
                "X-Visivo-Session header is required when multiple tokens are "
                "configured (the shared anonymous session is disabled).",
            )
        return REGISTRY.get_or_create_anon()

    # Atomic: resolve-or-adopt under one lock, with the ownership check inside, so
    # two users racing the same brand-new id can't end up sharing a session.
    # The backend hands back "anonymous" as the session id to clients that opened
    # without a header, and they echo it — so it stays valid in single-user mode.
    # With several tokens configured that shared bucket is a cross-user back door,
    # so it is refused there (as is omitting the header entirely).
    if x_visivo_session == _ANON_SESSION_ID:
        if multi_user_mode():
            raise HTTPException(
                status.HTTP_400_BAD_REQUEST,
                "The shared anonymous session is disabled when multiple tokens "
                "are configured; send your own X-Visivo-Session id.",
            )
        return REGISTRY.get_or_create_anon()

    # Reject any other guessable id BEFORE a lookup: a short id could be
    # pre-claimed by another user (session fixation), and answering identically
    # whether or not it exists keeps the response from confirming a session.
    if not _id_is_high_entropy(x_visivo_session):
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            "X-Visivo-Session must be a high-entropy id (>= 32 URL-safe chars), "
            "e.g. a UUID.",
        )
    try:
        session = REGISTRY.get_or_adopt(x_visivo_session, owner_token=token,
                                        require_owned=multi_user_mode(),
                                        allow_create=not strict_sessions())
    except SessionQuotaExceeded as exc:
        raise HTTPException(status.HTTP_429_TOO_MANY_REQUESTS, str(exc)) from exc
    if session is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "Session not found.")
    return session


def session_for_owner(session_id: str, token: str) -> Session | None:
    """Owner-checked lookup for endpoints that resolve a session id from the PATH
    (rather than the header). Returns None when the session is missing OR is not
    this token's, so callers answer 404 without leaking existence.

    Deliberately a NO-TOUCH peek: probing must not refresh another user's idle
    timer. An UNOWNED session (the shared anonymous one, or a legacy session) is
    only visible while the backend is single-user.
    """
    session = REGISTRY.peek(session_id)
    if session is None:
        return None
    if session.owner_token is None:
        return None if multi_user_mode() else session
    if token is None or not _secrets.compare_digest(session.owner_token, token):
        return None
    session.touch()
    return session
