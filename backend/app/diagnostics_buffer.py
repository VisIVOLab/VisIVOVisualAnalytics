"""In-process diagnostics ring buffer for the FastAPI backend.

A ``logging.Handler`` captures everything written to ``logger.*`` across the
backend, FastAPI/Starlette and Astropy (anything that uses the stdlib
``logging`` module) and appends a monotonically-id'd entry to a bounded
in-memory ring. The desktop client polls ``GET /v1/diagnostics/events?since=<id>``
to ingest the entries and surface them in its own Diagnostics window, so
the user sees one unified table covering both sides of the wire.

Design constraints:
- Thread-safe (Uvicorn uses multiple threads / event-loop workers).
- Non-blocking: full ring drops oldest entries silently.
- No external dependencies — stdlib only.
- Survives the full process lifetime; not per-request.
"""

from __future__ import annotations

import logging
import threading
import time
from collections import deque
from typing import Any, Iterable


# Default ring capacity. Override via env (read once at module import).
import os as _os
_BUFFER_CAPACITY = max(
    16, int(_os.environ.get("VISIVO_DIAGNOSTICS_BUFFER", "2000"))
)


def _level_to_str(level_no: int) -> str:
    """Map a Python logging level number to the four DiagnosticsLevel names
    used by the C++ side. Anything ≥ ERROR is "error"; ≥ WARNING is "warning";
    ≥ INFO is "info"; everything else is "debug"."""
    if level_no >= logging.ERROR:
        return "error"
    if level_no >= logging.WARNING:
        return "warning"
    if level_no >= logging.INFO:
        return "info"
    return "debug"


def _logger_to_category(name: str) -> str:
    """Heuristic mapping from a logger name to a DiagnosticsCategory used
    by the desktop client. The C++ side accepts the names in DiagnosticsManager
    (Scientific / Client / Backend / Task / WCS / Remote / Rendering /
    Performance); anything we don't recognise is mapped to "Backend"."""
    lname = name.lower()
    if "wcs" in lname or "astropy.wcs" in lname:
        return "WCS"
    if "samp" in lname:
        return "Remote"
    if "task" in lname:
        return "Task"
    if "moment" in lname or "spectral" in lname or "linewidth" in lname \
            or "baseline" in lname or "stack" in lname or "compute" in lname:
        return "Scientific"
    if "perf" in lname or "metric" in lname:
        return "Performance"
    if "render" in lname:
        return "Rendering"
    return "Backend"


class _DiagnosticsBuffer:
    """Thread-safe append-only ring buffer with monotonic IDs.

    Producers add entries via ``add()``; consumers (the diagnostics endpoint)
    fetch entries with ID strictly greater than a watermark via
    ``snapshot_since()``.
    """

    def __init__(self, capacity: int = _BUFFER_CAPACITY) -> None:
        self._capacity = max(1, capacity)
        self._entries: deque[dict[str, Any]] = deque(maxlen=self._capacity)
        self._next_id = 1
        self._lock = threading.Lock()

    def add(self, *, level: str, category: str, source: str, message: str,
            dataset_id: str = "", session_id: str = "",
            operation_tag: str = "") -> int:
        with self._lock:
            entry = {
                "id": self._next_id,
                "ts": time.time(),                       # UNIX epoch (s, float)
                "level": level,
                "category": category,
                "source": source,
                "message": message,
                "dataset_id": dataset_id,
                "session_id": session_id,
                "operation_tag": operation_tag,
            }
            self._next_id += 1
            self._entries.append(entry)
            return entry["id"]

    def snapshot_since(self, since: int = 0,
                       limit: int | None = None) -> list[dict[str, Any]]:
        """Return entries with ``id > since`` in insertion order."""
        with self._lock:
            # Skip entries we've already delivered (deque is in insert order).
            if since <= 0:
                items: Iterable[dict[str, Any]] = self._entries
            else:
                items = (e for e in self._entries if e["id"] > since)
            if limit is None:
                return [dict(e) for e in items]
            out: list[dict[str, Any]] = []
            for e in items:
                out.append(dict(e))
                if len(out) >= limit:
                    break
            return out

    def latest_id(self) -> int:
        with self._lock:
            return self._next_id - 1

    def stats(self) -> dict[str, Any]:
        with self._lock:
            return {
                "entries": len(self._entries),
                "capacity": self._capacity,
                "last_id": self._next_id - 1,
            }


# Process-wide singleton.
DIAGNOSTICS_BUFFER = _DiagnosticsBuffer()


class _DiagnosticsLoggingHandler(logging.Handler):
    """``logging.Handler`` that mirrors every emitted record into the ring.

    Attached once at app startup to the root logger so it captures records
    from FastAPI, Uvicorn, Astropy, and our own ``visivo.*`` loggers. The
    handler's level defaults to INFO (filterable via env).
    """

    def __init__(self, buffer: _DiagnosticsBuffer = DIAGNOSTICS_BUFFER) -> None:
        super().__init__(level=getattr(
            logging,
            _os.environ.get("VISIVO_DIAGNOSTICS_LEVEL", "INFO").upper(),
            logging.INFO,
        ))
        self._buffer = buffer

    def emit(self, record: logging.LogRecord) -> None:
        try:
            self._buffer.add(
                level=_level_to_str(record.levelno),
                category=_logger_to_category(record.name),
                source=record.name,
                message=self.format(record) if self.formatter else record.getMessage(),
            )
        except Exception:                                  # noqa: BLE001
            # Logging handlers must never raise back into the logger call.
            self.handleError(record)


def install_logging_handler() -> _DiagnosticsLoggingHandler:
    """Attach the diagnostics handler to the root logger.

    Idempotent — calling twice does not register two handlers. Call once
    at module import time of the FastAPI app.
    """
    root = logging.getLogger()
    for h in root.handlers:
        if isinstance(h, _DiagnosticsLoggingHandler):
            return h
    handler = _DiagnosticsLoggingHandler()
    handler.setFormatter(logging.Formatter("%(message)s"))
    root.addHandler(handler)
    return handler


def publish_event(*, level: str, category: str, source: str, message: str,
                  dataset_id: str = "", session_id: str = "",
                  operation_tag: str = "") -> int:
    """Manual event publish (bypasses the logging system).

    Use this from request handlers when the message is a structured event
    rather than a free-form log line — e.g. "Open dataset succeeded, id=X".
    """
    return DIAGNOSTICS_BUFFER.add(
        level=level, category=category, source=source, message=message,
        dataset_id=dataset_id, session_id=session_id, operation_tag=operation_tag,
    )
