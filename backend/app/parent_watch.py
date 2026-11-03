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

logger = logging.getLogger("visivo.backend")

#: Seconds between two looks at the client.
_INTERVAL_S = 2.0


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
