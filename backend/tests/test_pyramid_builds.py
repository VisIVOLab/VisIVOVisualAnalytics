"""Brick pyramids built in the background when a remote view needs one (#80)."""
from __future__ import annotations

import os
import time

import numpy as np
import pytest
from astropy.io import fits

from app import pyramid_builds
from app.brick_pyramid import BrickPyramid, default_pyramid_path, level_shapes
from app.pyramid_builds import BuildStatus, ensure_pyramid, overall_fraction


@pytest.fixture
def pyramid_dir(tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    monkeypatch.setattr(pyramid_builds, "_builds", {})
    return tmp_path


def _wait(path, hdu, seconds=60.0):
    deadline = time.monotonic() + seconds
    while True:
        status = ensure_pyramid(str(path), hdu)
        if status.state != "building" or time.monotonic() > deadline:
            return status
        time.sleep(0.2)


def test_a_missing_pyramid_is_built_in_another_process(pyramid_dir):
    path = pyramid_dir / "cube.fits"
    data = np.random.default_rng(3).normal(size=(40, 48, 56)).astype(np.float32)
    fits.PrimaryHDU(data).writeto(path)

    first = ensure_pyramid(str(path), 0)
    assert first.state == "building"
    # A second view of the same cube joins the running build.
    assert ensure_pyramid(str(path), 0).state in ("building", "ready")
    assert len(pyramid_builds._builds) <= 1

    done = _wait(path, 0)
    assert done == BuildStatus("ready", 1.0)
    out = default_pyramid_path(path, 0)
    with BrickPyramid(out, source=path) as p:
        assert p.shape(0) == (40, 48, 56)
    assert not out.with_name(out.name + ".progress.json").exists()
    assert ensure_pyramid(str(path), 0) == BuildStatus("ready", 1.0)


def test_a_failed_build_reports_its_error_and_is_not_retried_at_once(pyramid_dir):
    path = pyramid_dir / "flat.fits"
    fits.PrimaryHDU(np.zeros(64, dtype=np.float32)).writeto(path)   # neither image nor cube
    status = _wait(path, 0)
    assert status.state == "failed"
    assert status.error
    again = ensure_pyramid(str(path), 0)
    assert again.state == "failed"   # within the retry delay: no new build
    assert len(pyramid_builds._builds) == 1


def test_progress_weighs_each_level_by_its_voxels():
    shapes = level_shapes((512, 512, 512), 64)
    assert overall_fraction(shapes, 0, 0.0) == 0.0
    assert overall_fraction(shapes, 0, 1.0) == pytest.approx(
        512 ** 3 / sum(a * b * c for a, b, c in shapes))
    assert overall_fraction(shapes, len(shapes) - 1, 1.0) == pytest.approx(1.0)
    assert overall_fraction(shapes, 1, 0.5) > overall_fraction(shapes, 0, 1.0)


@pytest.mark.asyncio
async def test_create_session_answers_202_while_the_pyramid_builds(client, opened_cube, monkeypatch):
    from app.render_session import _ooc
    from app.routers import render

    monkeypatch.setattr(render, "server_side_rendering_available", lambda: True)
    monkeypatch.setattr(_ooc, "OOC_BYTES", 1)   # every cube out of core
    monkeypatch.setattr(pyramid_builds, "ensure_pyramid",
                        lambda path, hdu: BuildStatus("building", 0.42))
    resp = await client.post("/v1/render/create_session",
                             json={"dataset_id": opened_cube["dataset_id"]},
                             headers=opened_cube["headers"])
    assert resp.status_code == 202
    body = resp.json()
    assert body["preparing"] is True and body["valid"] is False
    assert body["progress"] == pytest.approx(0.42)

    monkeypatch.setattr(pyramid_builds, "ensure_pyramid",
                        lambda path, hdu: BuildStatus("failed", 0.0, "disk full"))
    resp = await client.post("/v1/render/create_session",
                             json={"dataset_id": opened_cube["dataset_id"]},
                             headers=opened_cube["headers"])
    assert resp.status_code == 500
    assert "disk full" in resp.json()["detail"]


@pytest.mark.asyncio
async def test_an_in_core_cube_does_not_wait_for_a_pyramid(client, opened_cube, monkeypatch):
    from app.routers import render

    monkeypatch.setattr(render, "server_side_rendering_available", lambda: True)
    called = []
    monkeypatch.setattr(pyramid_builds, "ensure_pyramid", lambda *a: called.append(a))
    resp = await client.post("/v1/render/create_session",
                             json={"dataset_id": opened_cube["dataset_id"]},
                             headers=opened_cube["headers"])
    assert resp.status_code != 202
    assert not called


@pytest.mark.skipif(pyramid_builds.fcntl is None, reason="no cross-process lock without fcntl")
def test_a_build_held_by_another_backend_is_waited_for(pyramid_dir):
    """Two backends sharing VISIVO_PYRAMID_DIR: the one holding the pyramid's
    lock builds; the other reports building until the lock is released."""
    path = pyramid_dir / "shared.fits"
    fits.PrimaryHDU(np.ones((24, 24, 24), dtype=np.float32)).writeto(path)
    out = default_pyramid_path(path, 0)
    out.parent.mkdir(parents=True, exist_ok=True)
    held = pyramid_builds._try_lock(pyramid_builds._lock_path(out))   # "the other backend"
    assert held is not None
    try:
        assert ensure_pyramid(str(path), 0).state == "building"
        deadline = time.monotonic() + 30
        while pyramid_builds._builds[str(out)].process.is_alive() and time.monotonic() < deadline:
            time.sleep(0.1)
        assert pyramid_builds._builds[str(out)].process.exitcode == pyramid_builds._BUILT_ELSEWHERE
        assert ensure_pyramid(str(path), 0).state == "building"   # still the other's build
        assert not out.exists()
    finally:
        import os
        os.close(held)
    # The other backend went away without a pyramid: this one builds it now.
    assert _wait(path, 0).state == "ready"


def test_the_lock_holder_removes_what_killed_builds_left(pyramid_dir):
    path = pyramid_dir / "cube.fits"
    fits.PrimaryHDU(np.ones((24, 24, 24), dtype=np.float32)).writeto(path)
    out = default_pyramid_path(path, 0)
    out.parent.mkdir(parents=True, exist_ok=True)
    stale = out.with_name(out.name + ".dead1234.part")
    stale.write_bytes(b"x" * 1024)
    old = time.time() - 3600
    os.utime(stale, (old, old))
    # A partial file still being written (recent) belongs to a live build that
    # may not take the lock (the CLI, another node): it is left alone.
    live = out.with_name(out.name + ".live5678.part")
    live.write_bytes(b"y")
    assert _wait(path, 0).state == "ready"
    assert not stale.exists()
    assert live.exists()
    assert out.exists()


class _Ended:
    """A build process that has already ended."""
    pid = None

    def __init__(self, exitcode=0):
        self.exitcode = exitcode

    def is_alive(self):
        return False

    def join(self, timeout=None):
        pass


def test_a_build_ending_while_it_is_polled_is_ready_not_failed(pyramid_dir, monkeypatch):
    # Published between the first readiness check and the look at its
    # process: done, not "failed" (the desktop would stop waiting).
    from app.brick_pyramid import build_pyramid
    path = pyramid_dir / "cube.fits"
    fits.PrimaryHDU(np.ones((24, 24, 24), np.float32)).writeto(path)
    out = default_pyramid_path(path, 0)
    build_pyramid(path, out)
    pyramid_builds._builds[str(out)] = pyramid_builds._Build(
        _Ended(0), pyramid_builds._progress_path(out), 0)
    calls = []
    real = pyramid_builds.pyramid_ready

    def first_says_not_yet(p, hdu):
        calls.append(1)
        return False if len(calls) == 1 else real(p, hdu)

    monkeypatch.setattr(pyramid_builds, "pyramid_ready", first_says_not_yet)
    assert ensure_pyramid(str(path), 0).state == "ready"
    assert str(out) not in pyramid_builds._builds


def test_a_build_finished_elsewhere_while_it_waited_for_the_lock_is_kept(pyramid_dir):
    # The child takes the lock after another backend published the pyramid:
    # it neither rebuilds it nor replaces the file.
    from app.brick_pyramid import build_pyramid
    path = pyramid_dir / "cube.fits"
    fits.PrimaryHDU(np.ones((24, 24, 24), np.float32)).writeto(path)
    out = default_pyramid_path(path, 0)
    build_pyramid(path, out)
    inode = out.stat().st_ino
    pyramid_builds._build_child(str(path), str(out), 0, str(pyramid_builds._progress_path(out)))
    assert out.stat().st_ino == inode


def test_a_file_system_that_cannot_lock_does_not_wait_for_ever(pyramid_dir, monkeypatch):
    import errno
    if pyramid_builds.fcntl is None:
        pytest.skip("no fcntl")

    def no_locks(fd, op):
        raise OSError(errno.ENOLCK, "No locks available")

    path = pyramid_dir / "cube.fits"
    fits.PrimaryHDU(np.ones((8, 8, 8), np.float32)).writeto(path)
    out = default_pyramid_path(path, 0)
    monkeypatch.setattr(pyramid_builds.fcntl, "flock", no_locks)
    out.parent.mkdir(parents=True, exist_ok=True)
    assert not pyramid_builds._locked_elsewhere(out)


def test_a_ready_pyramid_sweeps_old_partial_files(pyramid_dir, monkeypatch):
    # A leftover too recent when its build started is removed once old, even
    # though the pyramid is never built again.
    from app.brick_pyramid import build_pyramid
    path = pyramid_dir / "cube.fits"
    fits.PrimaryHDU(np.ones((24, 24, 24), np.float32)).writeto(path)
    out = default_pyramid_path(path, 0)
    build_pyramid(path, out)
    part = out.with_name(out.name + ".crashed.part")
    part.write_bytes(b"left by a killed build")
    assert ensure_pyramid(str(path), 0).state == "ready" and part.exists()
    old = time.time() - pyramid_builds._STALE_PART_S - 10
    os.utime(part, (old, old))
    assert ensure_pyramid(str(path), 0).state == "ready" and not part.exists()


def test_the_shared_memory_check_counts_the_buffers_a_build_makes(monkeypatch):
    # Eight workers, an 8 MiB budget, 64-voxel bricks: each of the 20 buffers
    # is at least a block of bricks, far over twice the budget.
    from types import SimpleNamespace

    from app import brick_pyramid as bp
    workers, brick, budget = 8, 64, 8 * 1024 * 1024
    per_slot = bp._shm_per_slot((512, 512, 512), brick, budget, workers)
    needed = 2 * bp._shm_slots(workers) * per_slot
    assert needed > 2 * budget
    monkeypatch.setattr(bp.os, "statvfs",
                        lambda p: SimpleNamespace(f_bavail=64 * 1024 * 1024, f_frsize=1),
                        raising=False)
    cube = SimpleNamespace(_tile=None)
    assert bp._parallel_ok(cube, budget)               # the old estimate let it through
    assert not bp._parallel_ok(cube, budget, needed)   # the buffers it makes do not fit


def test_a_killed_builds_buffers_go_and_no_other_builds():
    """Named from a random tag, not a process id: two builds in containers
    sharing /dev/shm may have the same pid, and one's cleanup must not take
    the other's live buffers."""
    from multiprocessing import shared_memory

    from app.brick_pyramid import new_shm_tag, shm_buffer_name
    dead, alive = new_shm_tag(), new_shm_tag()
    assert dead != alive and len(shm_buffer_name(dead, "o", 63)) <= 31
    left = shared_memory.SharedMemory(name=shm_buffer_name(dead, "o", 0), create=True, size=64)
    live = shared_memory.SharedMemory(name=shm_buffer_name(alive, "o", 0), create=True, size=64)
    try:
        left.close()
        pyramid_builds._unlink_build_buffers(dead)
        with pytest.raises(FileNotFoundError):
            shared_memory.SharedMemory(name=shm_buffer_name(dead, "o", 0))
        shared_memory.SharedMemory(name=shm_buffer_name(alive, "o", 0)).close()   # still there
    finally:
        live.close()
        live.unlink()
