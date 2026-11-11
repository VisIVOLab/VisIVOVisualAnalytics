"""A backend started by the desktop client ends with it.

The client stops the backend it started when it quits normally. When the client
dies instead (a crash, a force quit), the backend was left running with its
output pipe broken: the next client found it on the port and used it, and every
request that wrote a log line failed with "[Errno 32] Broken pipe" (a 2-D slice
that would not load, a preview that never came). It also kept serving whatever
code it had been started with, long after the files changed.

The client names itself in ``VISIVO_EXIT_WITH_PID``; while that process lives
the backend runs, and once it is gone the backend shuts down the way a quit
does (the lifespan hook stops the worker pool and render sessions). A backend
started by hand, without the variable, is not affected.
"""
from __future__ import annotations

import logging
import os
import signal
import sys
import threading
import time

logger = logging.getLogger("visivo.backend")

#: Seconds between two looks at the client.
_INTERVAL_S = 2.0
#: Seconds the graceful shutdown is given before the process exits anyway.
#: uvicorn first waits for open connections, without limit; then the lifespan
#: hook needs at most about 7 s (render sessions 3 s, then the pool 4 s).
_HARD_EXIT_S = 10.0
#: How much longer a lifespan shutdown already under way is waited for at that
#: point, so that the exit never cuts a render child's GPU teardown short.
_LIFESPAN_MAX_S = 8.0

#: Set by the lifespan hook (app/main.py) when its shutdown starts and ends.
SHUTDOWN_STARTED = threading.Event()
SHUTDOWN_DONE = threading.Event()


def process_alive(pid: int) -> bool:
    """Whether process ``pid`` exists."""
    if pid <= 0:
        return False
    if sys.platform == "win32":
        import ctypes  # noqa: PLC0415

        synchronize, wait_timeout = 0x00100000, 0x00000102
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(synchronize, False, pid)
        if not handle:
            return False
        try:
            return kernel32.WaitForSingleObject(handle, 0) == wait_timeout
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True   # someone else's process: it exists
    return True


def start(stop=None) -> threading.Thread | None:
    """Watch the client named in ``VISIVO_EXIT_WITH_PID``; ``stop`` is called
    once it is gone (by default, this process is asked to terminate)."""
    raw = os.environ.get("VISIVO_EXIT_WITH_PID", "").strip()
    if not raw.isdigit():
        return None
    pid = int(raw)

    def terminate() -> None:
        os.kill(os.getpid(), signal.SIGTERM)
        # uvicorn's graceful shutdown waits for open connections (a render
        # stream, a long request) with no limit of its own: a backend whose
        # client is gone must not wait on them for ever (#176). Its children
        # end with it (app/exit_with_parent.py).
        time.sleep(_HARD_EXIT_S)
        if SHUTDOWN_STARTED.is_set() and not SHUTDOWN_DONE.is_set():
            # Connections held it past the limit, and the lifespan hook is now
            # closing render sessions and the pool: let it finish (it is bounded).
            SHUTDOWN_DONE.wait(_LIFESPAN_MAX_S)
        logger.warning("[shutdown] still running %.0f s after the client went: exiting",
                       _HARD_EXIT_S)
        os._exit(1)

    action = stop or terminate
    halt = threading.Event()

    def watch() -> None:
        while not halt.wait(_INTERVAL_S):
            if not process_alive(pid):
                logger.warning("[startup] the client that started this backend (pid %d) is "
                               "gone: shutting down", pid)
                action()
                return

    thread = threading.Thread(target=watch, name="visivo-parent-watch", daemon=True)
    thread.halt = halt   # type: ignore[attr-defined]  # lets a test stop it
    thread.start()
    return thread
