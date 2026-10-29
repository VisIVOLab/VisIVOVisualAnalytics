"""A finder as a child process of the event loop, stopped for good when asked.

Both finders run this way: a cancelled or timed-out task must not leave the
computation running, and only a process can be killed. The child gets its
own session, so what is killed is the whole process group, including any
helper the finder started and anything that outlived the finder itself (a
helper left behind would also hold the pipes open, and asyncio's ``wait()``
does not return until they close).

The child's output is written to the log as it arrives, so a run that is
stopped still leaves its diagnostics behind.
"""
from __future__ import annotations

import asyncio
import logging
import os
import signal
import sys
from pathlib import Path
from typing import IO

logger = logging.getLogger("visivo.backend")


class ChildTimeout(RuntimeError):
    pass


async def _pump(stream: asyncio.StreamReader | None, chunks: list[bytes], log: IO[bytes] | None) -> None:
    if stream is None:
        return
    while True:
        chunk = await stream.read(4096)
        if not chunk:
            return
        chunks.append(chunk)
        if log is not None:
            try:
                log.write(chunk)
                log.flush()
            except OSError:
                log = None


async def run_child(args: list[str], cwd: Path, timeout: float, log_path: Path | None = None,
                    env: dict[str, str] | None = None) -> tuple[int, str, str]:
    """Run ``args`` to completion; returns (exit code, stdout, stderr).

    Raises :class:`ChildTimeout` after ``timeout`` seconds; a cancellation of
    the awaiting task kills the child and propagates. ``log_path`` receives
    the child's output as it is produced, in every case.
    """
    proc = await asyncio.create_subprocess_exec(
        *args, cwd=str(cwd), env=env, start_new_session=True,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    log: IO[bytes] | None = None
    if log_path is not None:
        try:
            log = open(log_path, "wb")
        except OSError:
            log = None
    out_chunks: list[bytes] = []
    err_chunks: list[bytes] = []
    pumps = [asyncio.ensure_future(_pump(proc.stdout, out_chunks, log)),
             asyncio.ensure_future(_pump(proc.stderr, err_chunks, log))]
    # Done means the process has exited AND its pipes have closed: a helper
    # left behind holding them is a run that has not finished. The pumps are
    # shielded from the timeout and the cancellation, so that after the kill
    # they still read the pipes to their end.
    done = asyncio.gather(proc.wait(), *pumps)
    try:
        try:
            await asyncio.wait_for(asyncio.shield(done), timeout)
        except asyncio.TimeoutError:
            await stop(proc)
            raise ChildTimeout(f"exceeded the {timeout:.0f} s time limit and was stopped")
        except asyncio.CancelledError:
            await stop(proc)
            raise
        finally:
            await _drain(done, pumps, proc)
    finally:
        if log is not None:
            log.close()
    return proc.returncode, b"".join(out_chunks).decode(errors="replace"), b"".join(err_chunks).decode(errors="replace")


async def _drain(done: asyncio.Future, pumps: list[asyncio.Future], proc: asyncio.subprocess.Process) -> None:
    """Collect what the pipes still hold; they close as the group exits.

    Should they not (a process that could not be killed), the readers are
    given up and the pipes closed on this side.
    """
    try:
        await asyncio.wait_for(asyncio.shield(done), 5.0)
    except asyncio.TimeoutError:
        _abandon(done, pumps, proc)
    except asyncio.CancelledError:
        _abandon(done, pumps, proc)
        raise
    except Exception:  # noqa: BLE001 — a pump's own failure; the run's result still stands
        pass


def _abandon(done: asyncio.Future, pumps: list[asyncio.Future], proc: asyncio.subprocess.Process) -> None:
    logger.warning("[finders] process %s left its pipes open; closing them", proc.pid)
    for pump in pumps:
        pump.cancel()
    done.cancel()
    transport = getattr(proc, "_transport", None)
    if transport is not None:
        try:
            transport.close()
        except Exception:  # noqa: BLE001
            pass


async def stop(proc: asyncio.subprocess.Process) -> None:
    """Kill the child's whole process tree, whether or not the child is still alive."""
    if sys.platform == "win32":
        # No process groups to signal: taskkill walks the tree instead.
        try:
            killer = await asyncio.create_subprocess_exec(
                "taskkill", "/F", "/T", "/PID", str(proc.pid),
                stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
            await asyncio.wait_for(killer.wait(), 5.0)
        except (OSError, asyncio.TimeoutError):
            _kill_parent(proc)
    elif hasattr(os, "killpg"):
        try:
            os.killpg(proc.pid, signal.SIGKILL)      # its own session: pgid == pid
        except ProcessLookupError:
            pass
        except PermissionError:
            _kill_parent(proc)
    else:
        _kill_parent(proc)
    try:
        await asyncio.wait_for(proc.wait(), 5.0)
    except asyncio.TimeoutError:
        logger.warning("[finders] process %s did not exit after being killed", proc.pid)


def _kill_parent(proc: asyncio.subprocess.Process) -> None:
    if proc.returncode is None:
        try:
            proc.kill()
        except ProcessLookupError:
            pass
