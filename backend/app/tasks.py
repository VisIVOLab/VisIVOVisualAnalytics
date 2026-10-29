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
from pathlib import Path
from typing import Any, Callable

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
    # The computation behind the task when it can outlive it: a cancelled
    # job's worker that cannot be interrupted still runs, and the node still
    # counts it as running (it holds the CPU and the memory) until it ends.
    work_handle: asyncio.Future | None = field(default=None, repr=False, compare=False)
    # The file the worker of this task polls between its steps (see
    # app/compute/abort.py); touched on cancellation, removed when the work
    # ends. None when the computation cannot be stopped this way.
    stop_file: str | None = field(default=None, repr=False, compare=False)

    def busy(self) -> bool:
        """Whether the node is still doing this task's work: its own runner
        is alive, or the computation it abandoned on cancellation still is.
        A record without a live runner (never started, or ended) is not."""
        return _live(self.asyncio_handle) or _live(self.work_handle)


def _live(handle: asyncio.Future | None) -> bool:
    return handle is not None and not handle.done()


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
        # Called (under the registry lock: keep them short, and never call back
        # into the registry) with each record the TTL evicts, so whoever owns
        # resources tied to a task can release them.
        self._eviction_listeners: list[Any] = []
        # Computations whose record is gone (see track_work), until they end:
        # one list of handles per activity.
        self._detached: list[list[asyncio.Future]] = []
        # Where the stop files go (a directory, made on first use); set by the
        # application once it knows its workspace. Without it, a cancelled
        # computation runs to its end and stays counted until then.
        self.stop_dir_provider: Callable[[], Path] | None = None

    def stop_file(self, task_id: str) -> str | None:
        """The stop file of a task, for its worker to poll (``check_abort``);
        None when no stop directory is configured. Recorded on the task, so
        that ``cancel`` knows what to touch."""
        if self.stop_dir_provider is None:
            return None
        try:
            directory = Path(self.stop_dir_provider())
            directory.mkdir(parents=True, exist_ok=True)
        except OSError:
            logger.warning("[tasks] no stop directory: task %s cannot be interrupted", task_id)
            return None
        path = str(directory / "".join(c for c in task_id if c.isalnum() or c in "-_"))
        with self._lock:
            record = self._tasks.get(task_id)
            if record is None:
                return None
            record.stop_file = path
        return path

    @staticmethod
    def _unlink_stop(stop_file: str | None) -> None:
        if stop_file:
            try:
                os.unlink(stop_file)
            except FileNotFoundError:
                pass
            except OSError:
                logger.warning("[tasks] could not remove stop file %s", stop_file)

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

    def track_work(self, *handles: asyncio.Future | None) -> None:
        """Count a computation that no record holds any more (a deleted job's
        worker, a timed-out or cancelled heavy call's pool task) as running
        until it ends: it still holds a worker and its memory. The handles
        given together are one activity (a runner and the work it shields),
        counted once while any of them lives."""
        with self._lock:
            self._track_locked(handles)

    def _track_locked(self, handles: tuple[asyncio.Future | None, ...]) -> None:
        live = [handle for handle in handles if _live(handle)]
        if live:
            self._detached.append(live)

    def _evict_expired_locked(self) -> int:
        # A record whose work is still going is never evicted, however long
        # it has been since anything touched it: the retention clock is for
        # what has ended; the record lives as long as its work does.
        now = time.monotonic()
        expired_task_ids = [
            task_id
            for task_id, record in self._tasks.items()
            if (now - record.last_touched_monotonic) > self._ttl_seconds and not record.busy()
        ]
        for task_id in expired_task_ids:
            record = self._tasks.pop(task_id)
            self._unlink_stop(record.stop_file)
            for listener in self._eviction_listeners:
                try:
                    listener(record)
                except Exception:  # noqa: BLE001 — a listener never breaks eviction
                    logger.exception("[tasks] eviction listener failed for %s", task_id)
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

    def add_eviction_listener(self, listener) -> None:
        self._eviction_listeners.append(listener)

    def list_owned(self, owner_token: str | None) -> list[TaskRecord]:
        """The tasks created by ``owner_token`` (for a UWS job list), oldest first."""
        import secrets as _sec

        with self._lock:
            self._evict_expired_locked()
            records = list(self._tasks.values())
        return [r for r in records
                if r.owner_token is not None and owner_token is not None
                and _sec.compare_digest(r.owner_token, owner_token)]

    def forget(self, task_id: str) -> bool:
        """Drop a task record (a deleted UWS job). True if it existed. Work
        still going behind it stays counted as running until it ends."""
        with self._lock:
            record = self._tasks.pop(task_id, None)
            if record is not None:
                # In the same locked step, so that no reader sees the work
                # neither on the record nor among the detached.
                self._track_locked((record.asyncio_handle, record.work_handle))
        if record is not None and not record.busy():
            # A busy record's stop file is for the running worker to see; the
            # callback cancel() registered removes it when the work ends.
            self._unlink_stop(record.stop_file)
        return record is not None

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
            stop_file, work = record.stop_file, record.work_handle
        if stop_file:
            # Tell the worker first: it polls the file between its steps and
            # stops within one, so the runner's cancellation below finds the
            # computation ending rather than running on to its end.
            try:
                Path(stop_file).touch()
            except OSError:
                logger.warning("[tasks] could not touch stop file %s", stop_file)
            # The file lives as long as the work does: the runner ends at
            # once on cancellation, the worker behind it within one slab, so
            # the last of the live handles to end removes the file.
            live = [h for h in (work, handle) if _live(h)]
            if live:
                remaining = {"n": len(live)}

                def _ended(_f, path=stop_file) -> None:
                    remaining["n"] -= 1
                    if remaining["n"] == 0:
                        self._unlink_stop(path)

                for h in live:
                    h.add_done_callback(_ended)
            else:
                self._unlink_stop(stop_file)
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
            # Work that outlived its record: counted while it runs, dropped once done.
            self._detached = [group for group in self._detached if any(_live(h) for h in group)]
            running_count += len(self._detached)
            for record in self._tasks.values():
                if record.status in ("pending", "running") or record.busy():
                    # Cancelled with its computation still going: still running
                    # as far as the node's load is concerned.
                    running_count += 1
                    if record.status == "pending":
                        pending_count += 1
                    if record.status not in ("pending", "running"):
                        continue
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
