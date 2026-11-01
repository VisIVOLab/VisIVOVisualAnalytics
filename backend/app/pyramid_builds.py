"""Brick pyramids built in the background when a view needs one (issue #80).

A remote 3-D view of a cube larger than ``VISIVO_RENDER_OUT_OF_CORE_BYTES``
draws it from its brick pyramid (:mod:`app.render_session._ooc`). When the
pyramid is missing, the render session used to build it before its first
frame: the client waited minutes with no progress, and for cubes whose build
outlasted its watchdog it gave up while the build went on.

Here the build runs in a process of its own, so the render process and the
server's event loop are not held up, and its memory (mostly the FITS file's
pages) is not counted against them. :func:`ensure_pyramid` answers at once:
ready, building with a fraction done, or failed. Every view of the same cube on
this backend shares one build. The child writes its progress to a small JSON
file next to the pyramid's temporary file, read on each query.
"""
from __future__ import annotations

import json
import logging
import multiprocessing
import os
import signal
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

try:
    import fcntl
except ImportError:  # Windows
    fcntl = None

logger = logging.getLogger("visivo.pyramid")

# Exit code of a build process that found the pyramid's lock taken: another
# backend sharing the pyramid directory is building it.
_BUILT_ELSEWHERE = 3
# A partial pyramid untouched for this long belongs to a build that is gone.
_STALE_PART_S = 600.0

# A failed build is not retried on every poll: the next attempt waits this long.
_RETRY_AFTER_FAILURE_S = 60.0


@dataclass(frozen=True)
class BuildStatus:
    state: str            # "ready" | "building" | "failed"
    progress: float = 0.0  # 0..1 while building
    error: str = ""


@dataclass
class _Build:
    process: multiprocessing.process.BaseProcess
    progress_file: Path
    started: float
    ended: float | None = None
    error: str = ""
    elsewhere: bool = False   # another backend holds the build


_lock = threading.Lock()
_builds: dict[str, _Build] = {}


def pyramid_ready(dataset_path: str, hdu: int) -> bool:
    """A usable pyramid of the cube exists at its default place."""
    from .brick_pyramid import BrickPyramid, StalePyramid, default_pyramid_path  # noqa: PLC0415

    path = default_pyramid_path(dataset_path, hdu)
    if not path.exists():
        return False
    try:
        BrickPyramid(path, source=dataset_path).close()
        return True
    except (StalePyramid, ValueError, OSError):
        return False


def ensure_pyramid(dataset_path: str, hdu: int) -> BuildStatus:
    """The pyramid's state, starting its build when it is missing or stale and
    no build of it is running, here or (through its lock) on another backend
    sharing the pyramid directory."""
    from .brick_pyramid import default_pyramid_path  # noqa: PLC0415

    out = default_pyramid_path(dataset_path, hdu)
    key = str(out)
    progress_file = _progress_path(out)
    if pyramid_ready(dataset_path, hdu):
        with _lock:
            done = _builds.pop(key, None)
        if done is not None:
            done.process.join(timeout=1.0)   # reap it: it ends right after the rename
        progress_file.unlink(missing_ok=True)
        return BuildStatus("ready", 1.0)
    with _lock:
        build = _builds.get(key)
        if build is not None:
            if build.process.is_alive():
                return BuildStatus("building", _read_progress(progress_file))
            if build.ended is None:
                build.process.join(timeout=0)
                build.ended = time.monotonic()
                build.elsewhere = build.process.exitcode == _BUILT_ELSEWHERE
                build.error = "" if build.elsewhere else _read_error(progress_file,
                                                                     build.process.exitcode)
            if build.elsewhere:
                # Another backend sharing the directory holds the build.
                if _locked_elsewhere(out):
                    return BuildStatus("building", _read_progress(progress_file))
                build.error = _read_error(progress_file, None)
                build.elsewhere = False
                build.ended = time.monotonic()
                if not build.error:   # it ended without a pyramid or a reason: ours now
                    _builds.pop(key, None)
                    return _start(dataset_path, out, hdu, key, progress_file)
            if time.monotonic() - build.ended < _RETRY_AFTER_FAILURE_S:
                return BuildStatus("failed", 0.0, build.error or "the pyramid build failed")
            _builds.pop(key, None)
        return _start(dataset_path, out, hdu, key, progress_file)


def _start(dataset_path: str, out: Path, hdu: int, key: str, progress_file: Path) -> BuildStatus:
    """Spawn the build (called with ``_lock`` held)."""
    out.parent.mkdir(parents=True, exist_ok=True)
    # spawn: a fresh interpreter, not a fork of a server holding sockets,
    # threads and a GPU context.
    process = multiprocessing.get_context("spawn").Process(
        target=_build_child, args=(str(dataset_path), str(out), int(hdu), str(progress_file)),
        name=f"pyramid-{out.stem}", daemon=True)
    process.start()
    _builds[key] = _Build(process, progress_file, time.monotonic())
    logger.info("[pyramid] building %s for %s (pid %s)", out, dataset_path, process.pid)
    return BuildStatus("building", 0.0)


def _progress_path(out: Path) -> Path:
    return out.with_name(out.name + ".progress.json")


def _lock_path(out: Path) -> Path:
    return out.with_name(out.name + ".lock")


def _try_lock(path: Path) -> int | None:
    """An exclusive lock on ``path`` (an open descriptor; closing it releases
    the lock), or None when another process holds it. Without fcntl (Windows)
    it is always granted: one backend per pyramid directory there."""
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    if fcntl is None:
        return fd
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return fd
    except OSError:
        os.close(fd)
        return None


def _locked_elsewhere(out: Path) -> bool:
    fd = _try_lock(_lock_path(out))
    if fd is None:
        return True
    os.close(fd)
    return False


def _read_progress(progress_file: Path) -> float:
    try:
        return float(json.loads(progress_file.read_text()).get("fraction", 0.0))
    except (OSError, ValueError, TypeError):
        return 0.0


def _read_error(progress_file: Path, exitcode: int | None) -> str:
    try:
        error = str(json.loads(progress_file.read_text()).get("error", ""))
    except (OSError, ValueError, TypeError):
        error = ""
    if not error and exitcode not in (0, None):
        error = f"the pyramid build stopped (exit code {exitcode})"
    return error


def overall_fraction(shapes: list[tuple[int, int, int]], level: int, fraction: float) -> float:
    """The share of the whole build done when ``level`` is ``fraction`` done:
    each level weighs its voxels (level 0, the read of the cube, about 7/8)."""
    sizes = [nz * ny * nx for nz, ny, nx in shapes]
    total = float(sum(sizes)) or 1.0
    return min(1.0, (sum(sizes[:level]) + sizes[level] * fraction) / total)


def _sweep_stale_parts(out_path: Path) -> None:
    """Partial pyramids nobody has written to for ``_STALE_PART_S``: left by
    builds killed outright, so without their own cleanup."""
    for stale in out_path.parent.glob(out_path.name + ".*.part"):
        try:
            if time.time() - stale.stat().st_mtime > _STALE_PART_S:
                stale.unlink(missing_ok=True)
        except OSError:
            pass


def _build_child(dataset_path: str, out: str, hdu: int, progress_file: str) -> None:
    """In the build process: build, and report progress or the error in
    ``progress_file`` (written atomically, at most twice a second)."""
    from astropy.io import fits  # noqa: PLC0415

    from .brick_pyramid import DEFAULT_BRICK, build_pyramid, level_shapes  # noqa: PLC0415

    def write(payload: dict) -> None:
        # Only the lock holder writes it; a failed write never stops the build.
        try:
            tmp = progress_file + f".{os.getpid()}.tmp"
            with open(tmp, "w") as fh:
                json.dump(payload, fh)
            os.replace(tmp, progress_file)
        except OSError:
            pass

    # SIGTERM (the server shutting down, the job ending) unwinds through
    # build_pyramid's cleanup, which removes its partial file.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    out_path = Path(out)
    lock = _try_lock(_lock_path(out_path))
    if lock is None:
        sys.exit(_BUILT_ELSEWHERE)
    # The lock holder removes what earlier builds left (killed outright, so
    # without their cleanup), but only files nobody has written to for a
    # while: a build that does not take the lock (python -m app.brick_pyramid,
    # or a backend on another node where the file system keeps locks per node)
    # writes to its partial file all the time.
    _sweep_stale_parts(out_path)
    Path(progress_file).unlink(missing_ok=True)

    try:
        with fits.open(dataset_path, memmap=True) as hdul:
            shape = tuple(int(n) for n in hdul[hdu].shape[-3:])
        shapes = level_shapes(shape, DEFAULT_BRICK)
        last = [0.0]

        def progress(stage: str, fraction: float) -> None:
            now = time.monotonic()
            if now - last[0] < 0.5 and fraction < 1.0:
                return
            last[0] = now
            level = int(stage.removeprefix("level")) if stage.startswith("level") else 0
            write({"fraction": overall_fraction(shapes, min(level, len(shapes) - 1), fraction)})

        # Nothing is written after this: the pyramid is in place, and the
        # parent removes the progress file when it sees it.
        build_pyramid(dataset_path, out, hdu=hdu, progress=progress)
        # Again at the end, still holding the lock: a leftover too recent to
        # remove when the build started is old enough now.
        _sweep_stale_parts(out_path)
    except BaseException as exc:  # noqa: BLE001 - reported to the parent, then re-raised
        try:
            # A stop (SIGTERM → SystemExit) is not a failure of the build: no
            # error, so a backend waiting on it takes the build over.
            if not isinstance(exc, SystemExit):
                write({"fraction": 0.0, "error": f"{type(exc).__name__}: {exc}"})
        finally:
            raise
