"""A process the backend spawned ends when the backend does (issue #176).

Nothing ends a ``multiprocessing`` child whose parent dies. A pool worker waits
on its task queue, and that queue's pipe never reports end-of-file because the
worker itself holds a write end of it (spawn pickles both ends into every
worker). Observed on a Mac: four compute-pool workers and the resource tracker
still running nine days after their backend was killed, re-parented to PID 1,
each holding its memory. ``shutdown_pool`` and the lifespan hook cover a
backend that exits normally. They cannot cover one that is killed (SIGKILL, the
OOM killer, the end of a SLURM job, a crash), because no code of the backend
runs then.

So each child watches its parent instead. :func:`install`, called first thing
in the child (the pool initializer, the process entry point), starts a daemon
thread that exits the process with ``os._exit`` once the parent is gone. It is
a thread and not a signal into the main loop because the main thread may not
be Python's to interrupt. A render child on a Mac runs Cocoa there, and a cube
reader sits inside astropy's memmap reads.

How the thread knows the parent is gone, on every platform:

* ``multiprocessing.parent_process().sentinel``. On POSIX this is the read end
  of a pipe whose write end only the parent holds (spawn starts children with
  ``close_fds``, so neither siblings nor grandchildren have it). It reaches
  end-of-file the moment the parent dies, however it died. On Windows it is a
  handle to the parent process, signalled when the process ends.
* As a second check on POSIX, ``os.getppid()`` compared with what it answered
  when the watcher started, about once a second. A child is re-parented when
  its parent dies, to PID 1 or to a subreaper (systemd, a container's init),
  so the check looks for a change and not for 1. Under ``forkserver`` the OS
  parent is the fork server, not the backend; it ends with the backend, so a
  change still means the backend is gone. This covers the one case where the pipe
  stays open after the death: with ``VISIVO_MP_START_METHOD=fork``, a later
  sibling inherits the write end of an earlier one's pipe.
* On Linux, optionally, ``prctl(PR_SET_PDEATHSIG, SIGKILL)``. The kernel then
  kills the child, which works even when its main thread holds the GIL inside
  a long native call (a VTK render, a large numpy reduction), where the
  watcher thread cannot run until the call returns. It is opt-in because
  Linux sends the signal when the *thread* that started the child ends, not
  the process. A child started from a worker thread (anyio's request threads
  end after ten idle seconds) would be killed while the backend still runs.
  Callers pass :func:`spawned_from_main_thread` evaluated in the parent at
  start time. The compute pool never sets it, because ``ProcessPoolExecutor``
  starts workers from whichever thread happens to submit.

macOS has no parent-death signal. A child stuck in a native call that holds
the GIL ends when that call returns.

What happens to the resource tracker: it is a separate process started by the
backend the first time a semaphore or shared-memory segment is made, and every
spawned child inherits the write end of its pipe. It ends when that pipe
reaches end-of-file, that is, once the backend *and every child* have exited.
It then unlinks whatever was still registered (shared memory of a cube reader
or a pyramid build killed outright, the pool's semaphores) and exits. The
orphaned workers kept it alive for nine days. Once they end with the backend,
the tracker ends a moment later.
"""
from __future__ import annotations

import multiprocessing
import multiprocessing.connection
import os
import signal
import sys
import threading
import time

#: Seconds between two ``getppid`` checks; the pipe wakes the thread at once.
INTERVAL_S = 1.0
#: Exit status of a child that ended because its parent was gone.
EXIT_CODE = 1

_PR_SET_PDEATHSIG = 1
_installed: threading.Thread | None = None
_install_lock = threading.Lock()


def spawned_from_main_thread() -> bool:
    """In the parent, right before starting a child: whether the child may
    use Linux's parent-death signal (see the module doc). The main thread
    lives as long as the process, so a child started from it is signalled only
    when the process dies."""
    return threading.current_thread() is threading.main_thread()


def parent_gone(sentinel, parent_pid: int | None, timeout: float) -> bool:
    """Wait up to ``timeout`` seconds; True once the parent is gone.

    ``sentinel`` is the parent's sentinel (a pipe fd on POSIX, a process
    handle on Windows) or None. ``parent_pid`` is what ``getppid`` answered
    while the parent lived (see :func:`install`), None to skip the check (Windows, where ``getppid`` keeps
    answering the dead parent's PID).
    """
    if parent_pid is not None and os.getppid() != parent_pid:
        return True
    if sentinel is None:
        time.sleep(timeout)
        return False
    try:
        if sys.platform != "win32":
            # A closed descriptor polls as ready, which would read as the
            # parent's death; one closed by mistake must not end the child.
            os.fstat(sentinel)
        if multiprocessing.connection.wait([sentinel], timeout):
            return True
    except (OSError, ValueError):
        # The sentinel itself is unusable (closed): nothing left to wait on
        # but the PID.
        time.sleep(timeout)
    return parent_pid is not None and os.getppid() != parent_pid


def _set_pdeathsig(parent_pid: int | None) -> bool:
    """Linux only: have the kernel SIGKILL this process when its parent dies.
    False where unavailable."""
    if not sys.platform.startswith("linux"):
        return False
    try:
        import ctypes  # noqa: PLC0415

        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(_PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0) != 0:
            return False
    except (OSError, AttributeError):
        return False
    # The parent may have died between the fork and the prctl: then the
    # signal will never come, and this process is already somebody else's.
    if parent_pid is not None and os.getppid() != parent_pid:
        os._exit(EXIT_CODE)
    return True


def _leave(name: str, grace_s: float) -> None:
    """End this process: ``os._exit``, after asking it to stop politely first
    when ``grace_s`` is given (its SIGTERM handler unwinds, removing partial
    files)."""
    try:
        sys.stderr.write(f"[visivo] {name} (pid {os.getpid()}): the backend that started it "
                         "is gone; exiting\n")
        sys.stderr.flush()
    except Exception:  # noqa: BLE001 - stderr may be a pipe nobody reads any more
        pass
    if grace_s > 0 and sys.platform != "win32":
        try:
            os.kill(os.getpid(), signal.SIGTERM)
        except OSError:
            pass
        time.sleep(grace_s)
    os._exit(EXIT_CODE)


def install(*, pdeathsig: bool = False, grace_s: float = 0.0,
            interval_s: float | None = None, on_gone=None) -> threading.Thread | None:
    """In a child: end this process once its parent is gone. Idempotent.

    Returns the watcher thread, or None when this process is not a
    ``multiprocessing`` child (the backend itself, a script run by hand).

    ``pdeathsig``: also ask Linux to SIGKILL this process when the parent dies;
    pass :func:`spawned_from_main_thread` as the parent saw it. ``grace_s``:
    send this process SIGTERM first and wait that long before ``os._exit``,
    for a child whose SIGTERM handler cleans up. ``on_gone``: replaces the
    exit, for tests.
    """
    global _installed
    with _install_lock:
        if _installed is not None:
            return _installed
        parent = multiprocessing.parent_process()
        if parent is None:
            return None
        sentinel = getattr(parent, "sentinel", None)
        # The PID to watch is the one getppid answers now, not parent.pid:
        # under forkserver the OS parent is the fork server, and comparing
        # with the backend's PID would read as its death at the first look.
        # The fork server ends with the backend, so a change still means the
        # backend is gone.
        parent_pid = os.getppid() if sys.platform != "win32" else None
        direct = parent_pid == parent.pid
        # A backend already dead by now leaves getppid at init or a subreaper,
        # which no later change will reveal, and under fork the sentinel may
        # never reach end-of-file either (a later sibling holds its write
        # end). So whenever the OS parent is not the backend, ask whether the
        # backend still exists; when it does, this is a fork server's child.
        from .parent_watch import process_alive  # noqa: PLC0415 - light, no app imports
        already_gone = not direct and not process_alive(parent.pid)
        if pdeathsig and direct:
            # Only for a child of the backend itself: the signal follows the
            # OS parent, which under forkserver is not the backend.
            _set_pdeathsig(parent_pid)
        interval = INTERVAL_S if interval_s is None else interval_s
        name = multiprocessing.current_process().name

        def watch() -> None:
            while not already_gone and not parent_gone(sentinel, parent_pid, interval):
                pass
            if on_gone is not None:
                on_gone()
            else:
                _leave(name, grace_s)

        _installed = threading.Thread(target=watch, name="visivo-exit-with-parent", daemon=True)
        _installed.start()
        return _installed
