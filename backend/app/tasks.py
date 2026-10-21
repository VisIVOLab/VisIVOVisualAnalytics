from __future__ import annotations

import asyncio
import logging
import os
import threading
import time
import uuid
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

logger = logging.getLogger("visivo.tasks")

_TASK_TTL_SECONDS = max(1, int(os.environ.get("VISIVO_TASK_TTL_SECONDS", "1800")))


def _now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


@dataclass
class TaskRecord:
    task_id: str
    operation: str
    status: str = "pending"
    created_at: str = field(default_factory=_now_iso)
    updated_at: str = field(default_factory=_now_iso)
    progress: float = 0.0
    result: dict[str, Any] | None = None
    error: str = ""
    cache_hit: bool = False
    # The auth token that created this task; another user holding the task_id
    # must not be able to read its result or cancel it.
    owner_token: str | None = None
    last_touched_monotonic: float = field(default_factory=time.monotonic, repr=False)
    # Stored by the task runner so callers can request cancellation.
    asyncio_handle: asyncio.Task | None = field(default=None, repr=False, compare=False)


@dataclass
class OperationRecord:
    """One finished operation that never was a task.

    Only two endpoints (``/v1/tasks/moment`` and ``/v1/tasks/pv``) create
    ``TaskRecord``s — everything else the backend does is synchronous and left
    no trace, so the Data Hub's "Last 5 jobs" panel was permanently empty while
    promising a history. These records are that history: written by the HTTP
    middleware when a compute request finishes, and merged into
    :meth:`TaskRegistry.snapshot` alongside the real tasks.
    """
    operation: str
    status: str
    duration_seconds: float
    finished_at: str
    cache_hit: bool = False


#: How many finished operations to remember. The snapshot shows five; the
#: surplus is slack so a burst of requests does not erase the interesting ones.
_HISTORY_LENGTH = 50


class TaskRegistry:
    def __init__(self, ttl_seconds: int = _TASK_TTL_SECONDS) -> None:
        self._tasks: dict[str, TaskRecord] = {}
        self._history: deque[OperationRecord] = deque(maxlen=_HISTORY_LENGTH)
        self._lock = threading.Lock()
        self._ttl_seconds = max(1, int(ttl_seconds))

    def record_operation(self, operation: str, status: str = "done",
                         duration_seconds: float = 0.0,
                         cache_hit: bool = False) -> None:
        """Remember a synchronous operation that finished.

        Deliberately not a :class:`TaskRecord`: these carry no task id, are
        never polled or cancelled, and must not inflate the registry's entry
        count or its TTL bookkeeping.
        """
        record = OperationRecord(
            operation=str(operation),
            status=str(status),
            duration_seconds=max(0.0, float(duration_seconds)),
            finished_at=_now_iso(),
            cache_hit=bool(cache_hit),
        )
        with self._lock:
            self._history.append(record)

    def _evict_expired_locked(self) -> int:
        now = time.monotonic()
        expired_task_ids = [
            task_id
            for task_id, record in self._tasks.items()
            if (now - record.last_touched_monotonic) > self._ttl_seconds
        ]
        for task_id in expired_task_ids:
            record = self._tasks.pop(task_id)
            logger.info(
                "[tasks] evicted task_id=%s operation=%s age_seconds=%.1f",
                task_id,
                record.operation,
                now - record.last_touched_monotonic,
            )
        return len(expired_task_ids)

    def create(self, operation: str, owner_token: str | None = None) -> TaskRecord:
        record = TaskRecord(task_id=f"task_{uuid.uuid4().hex[:16]}", operation=operation,
                            owner_token=owner_token)
        with self._lock:
            self._evict_expired_locked()
            self._tasks[record.task_id] = record
        return record

    def peek(self, task_id: str) -> TaskRecord | None:
        """Look a task up WITHOUT refreshing its retention timer — used by the
        ownership check so probing a stranger's task_id can't keep it alive."""
        with self._lock:
            return self._tasks.get(task_id)

    def get(self, task_id: str) -> TaskRecord | None:
        with self._lock:
            self._evict_expired_locked()
            record = self._tasks.get(task_id)
            if record is None:
                return None
            record.last_touched_monotonic = time.monotonic()
            return record

    def update(self, task_id: str, **changes: Any) -> TaskRecord | None:
        with self._lock:
            self._evict_expired_locked()
            record = self._tasks.get(task_id)
            if record is None:
                return None
            for key, value in changes.items():
                setattr(record, key, value)
            record.updated_at = _now_iso()
            record.last_touched_monotonic = time.monotonic()
            return record

    def cancel(self, task_id: str) -> bool:
        """Request cancellation of a running task.

        Returns True if the task was found and a cancellation was requested,
        False if the task does not exist or has already finished.
        """
        with self._lock:
            record = self._tasks.get(task_id)
            if record is None:
                return False
            if record.status not in ("pending", "running"):
                return False
            handle = record.asyncio_handle
            record.status = "cancelled"
            record.updated_at = _now_iso()
            record.last_touched_monotonic = time.monotonic()
        if handle is not None and not handle.done():
            handle.cancel()
            logger.info("[tasks] cancel requested task_id=%s operation=%s", task_id, record.operation)
        return True

    def stats(self) -> dict[str, Any]:
        with self._lock:
            self._evict_expired_locked()
            return {
                "entries": len(self._tasks),
                "ttl_enabled": True,
                "ttl_seconds": self._ttl_seconds,
            }

    def snapshot(self, recent_limit: int = 5) -> dict[str, Any]:
        """Live activity snapshot consumed by /v1/health → DataHub UI.

        Returns the count of in-flight tasks plus the most recent finished
        ones (done/failed/cancelled) with timing info. The UI uses this to
        show "what's happening now" + a short trail of recent operations.
        """
        with self._lock:
            self._evict_expired_locked()
            running_count = 0
            pending_count = 0
            finished: list[TaskRecord] = []
            for record in self._tasks.values():
                if record.status in ("pending", "running"):
                    running_count += 1
                    if record.status == "pending":
                        pending_count += 1
                # "completed" as well as "done": the task API's own terminal
                # status is "completed" (that is what /v1/tasks/{id} reports and
                # what the client polls for), so a registry that only knew
                # "done" dropped every long-running job — moments, above all —
                # out of the activity panel entirely. It showed nothing at all
                # for them: not running, not finished.
                elif record.status in ("done", "completed", "failed", "cancelled"):
                    finished.append(record)

            # Sort finished by completion time (descending) and keep top N.
            finished.sort(key=lambda r: r.updated_at, reverse=True)
            finished = finished[: max(0, int(recent_limit))]

            now = datetime.now(tz=timezone.utc)

            def _seconds_between(start_iso: str, end_iso: str) -> float:
                try:
                    return max(0.0, (datetime.fromisoformat(end_iso)
                                      - datetime.fromisoformat(start_iso)).total_seconds())
                except (TypeError, ValueError):
                    return 0.0

            def _seconds_since(stamp_iso: str) -> float:
                try:
                    return max(0.0, (now - datetime.fromisoformat(stamp_iso)).total_seconds())
                except (TypeError, ValueError):
                    return 0.0

            # Tasks and plain synchronous operations are the same thing to the
            # user — "something the backend did for me" — so they share one
            # list, ordered by when they finished.
            recent = [
                {
                    "operation": r.operation,
                    "status": r.status,
                    "duration_seconds": _seconds_between(r.created_at, r.updated_at),
                    "age_seconds": _seconds_since(r.updated_at),
                    "cache_hit": bool(r.cache_hit),
                    "_finished_at": r.updated_at,
                }
                for r in finished
            ] + [
                {
                    "operation": r.operation,
                    "status": r.status,
                    "duration_seconds": r.duration_seconds,
                    "age_seconds": _seconds_since(r.finished_at),
                    "cache_hit": r.cache_hit,
                    "_finished_at": r.finished_at,
                }
                # Newest first: several operations can share a timestamp to the
                # microsecond, and the sort below is stable, so the order the
                # list is built in is the tie-break.
                for r in reversed(self._history)
            ]
            recent.sort(key=lambda entry: entry["_finished_at"], reverse=True)
            recent = recent[: max(0, int(recent_limit))]
            for entry in recent:
                entry.pop("_finished_at", None)
            return {"running_count": running_count, "pending_count": pending_count,
                    "recent_tasks": recent}


TASKS = TaskRegistry()
