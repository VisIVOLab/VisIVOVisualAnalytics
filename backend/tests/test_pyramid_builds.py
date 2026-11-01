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
    fits.PrimaryHDU(np.zeros((8, 8), dtype=np.float32)).writeto(path)   # not a cube
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
