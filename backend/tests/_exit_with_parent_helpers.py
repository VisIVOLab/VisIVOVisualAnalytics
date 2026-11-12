"""Run by tests/test_exit_with_parent.py as a stand-in backend: it starts one of
each kind of child the backend starts, prints their PIDs as one JSON line, and
waits to be killed. Pool workers import ``hold`` from here by name."""
from __future__ import annotations

import asyncio
import json
import multiprocessing.resource_tracker as resource_tracker
import os
import sys
import time
from pathlib import Path


def hold(marker: str) -> None:
    """A pool task that never ends, once it has said which worker runs it."""
    with open(marker, "a") as fh:
        fh.write(f"{os.getpid()}\n")
    time.sleep(3600)


def _wait_lines(path: Path, count: int, timeout: float = 60.0) -> list[int]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            lines = [int(x) for x in path.read_text().split()]
            if len(lines) >= count:
                return lines
        time.sleep(0.05)
    raise TimeoutError(f"{count} pool tasks did not start")


def _forkserver_pid() -> int | None:
    try:
        import multiprocessing.forkserver as forkserver
    except ImportError:          # Windows
        return None
    return getattr(forkserver._forkserver, "_forkserver_pid", None)


async def main(work: Path) -> None:
    from app import dependencies, pyramid_builds
    from app.cube_reader import CubeReader
    from app.render_session._process import RenderSessionProcess

    # The compute pool, every worker inside a task (a moment map, say).
    marker = work / "pool.txt"
    for _ in range(dependencies._WORKERS):
        dependencies._POOL.submit(hold, str(marker))
    pool = _wait_lines(marker, dependencies._WORKERS)

    # A render child busy in a render that does not return: the pipe's
    # end-of-file alone would never reach it.
    session = RenderSessionProcess("exitwatch", "/data/cube.fits",
                                   factory="tests._fake_render_session:FakeRenderSession")
    await session.initialize()
    await session.apply_command({"type": "hang"})

    # A cube reader and a pyramid build stuck opening their file (a FIFO
    # nobody writes to: a slow file system, for them).
    fifo = work / "cube.fits"
    os.mkfifo(fifo)
    reader = CubeReader(str(fifo), shape=(2, 2, 2))
    reader.start()
    out = work / "pyr" / "cube.h5"
    pyramid_builds._start(str(fifo), out, 0, "k", work / "pyr" / "progress.json")
    build = pyramid_builds._builds["k"].process

    # A file, not stdout: importing the backend may print.
    (work / "pids.tmp").write_text(json.dumps({
        "pool": pool,
        "render": session._process.pid,
        "reader": reader._process.pid,
        "pyramid": build.pid,
        "tracker": resource_tracker._resource_tracker._pid,
        # The pool's start method is VISIVO_MP_START_METHOD: under forkserver
        # its workers are the fork server's children, and it must end too.
        "forkserver": _forkserver_pid(),
    }))
    os.replace(work / "pids.tmp", work / "pids.json")
    await asyncio.sleep(3600)


if __name__ == "__main__":
    asyncio.run(main(Path(sys.argv[1])))
