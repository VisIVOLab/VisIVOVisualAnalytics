"""Ray-traced volume rendering (issue #106): the render task and the grid
for the desktop's GPU view."""
from __future__ import annotations

import asyncio
import io

import numpy as np
import pytest
from fastapi import HTTPException

pytest.importorskip("numba")

from app.volume_source import vbt_field_ref  # noqa: E402

N = 24
CAM = {"position": (11.5 + 40, 11.5 + 25, 11.5 + 50), "focal_point": (11.5, 11.5, 11.5),
       "view_up": (0, 1, 0)}


def _volume(tmp_path):
    """A two-field VBT volume: a density blob, and a temperature rising
    along x (from 1e4 to 1e7)."""
    z, y, x = np.mgrid[0:N, 0:N, 0:N].astype(np.float32)
    density = np.exp(-((x - 12) ** 2 + (y - 12) ** 2 + (z - 12) ** 2) / 30.0).astype(np.float32)
    temperature = (10.0 ** (4.0 + 3.0 * x / (N - 1))).astype(np.float32)
    data = tmp_path / "grid.bin"
    np.concatenate([density.ravel(), temperature.ravel()]).astype("<f4").tofile(data)
    (tmp_path / "grid.bin.head").write_text(
        f"float\n2\n{N ** 3} {N} {N} {N} 1 1 1\nlittle\nDensity\nTemperature\n")
    return str(data), density, temperature


class _Session:
    def __init__(self, path):
        self.datasets = {"vol": {"kind": "cube", "path": vbt_field_ref(path, 0)},
                         "fits": {"kind": "cube", "path": path + ".fits"},
                         "points": {"kind": "catalogue", "format": "vbt", "path": path}}


def _run(coro_factory):
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro_factory()), loop
    except Exception:
        loop.close()
        raise


def _render(router, request, session):
    from app.tasks import TASKS

    async def go():
        created = await router.create_volume_render_task(request, token="tok", session=session)
        for _ in range(1200):
            task = TASKS.get(created.task_id)
            if task.status not in ("pending", "running"):
                return task
            await asyncio.sleep(0.05)
        raise AssertionError("render task did not end")
    return _run(go)


def test_a_still_is_rendered_graded_and_kept(tmp_path, monkeypatch):
    from astropy.io import fits
    from PIL import Image

    from app.routers import particle_render as results
    from app.routers import volume_render as router
    monkeypatch.setattr(results, "get_exports_dir", lambda: tmp_path / "exports")
    path, _, _ = _volume(tmp_path)
    req = router.VolumeRenderRequest(
        dataset_id="vol", colour_by="colour", colour_field=1,
        colour={"lut": "Inferno", "log": True}, absorption=0.5, width=96, height=64,
        cameras=[CAM])
    task, loop = _render(router, req, _Session(path))
    try:
        assert task.status == "completed", task.error
        assert task.result["frames"] == 1 and task.result["hdr"]
        lo, hi = task.result["colour_range"]
        assert lo == pytest.approx(1e4, rel=1e-3) and hi == pytest.approx(1e7, rel=1e-3)
        run = loop.run_until_complete
        frame = run(results.particle_render_frame(task.task_id, 0, token="tok"))
        img = np.asarray(Image.open(frame.path))
        assert img.shape == (64, 96, 3) and img.max() > 0
        graded = run(results.particle_render_tone(task.task_id, results.RaytraceTone(
            curve="linear", ev=1.0), token="tok"))
        assert Image.open(io.BytesIO(graded.body)).size == (96, 64)
        cube = run(results.particle_render_light_fits(task.task_id, token="tok")).body
        with fits.open(io.BytesIO(cube)) as hdul:
            assert hdul[0].data.shape == (3, 64, 96)
        run(results.delete_particle_render(task.task_id, token="tok"))
        assert not (tmp_path / "exports" / ".renders" / task.task_id).exists()
    finally:
        loop.close()


def test_the_colour_follows_the_second_field(tmp_path, monkeypatch):
    # Seen down -z, the temperature rises to the right: the hue does too.
    from PIL import Image

    from app.routers import particle_render as results
    from app.routers import volume_render as router
    monkeypatch.setattr(results, "get_exports_dir", lambda: tmp_path / "exports")
    path, _, _ = _volume(tmp_path)
    top = {"position": (11.5, 11.5, 60.0), "focal_point": (11.5, 11.5, 11.5),
           "view_up": (0, 1, 0), "parallel": True, "parallel_scale": 12.0}
    req = router.VolumeRenderRequest(
        dataset_id="vol", colour_by="colour", colour_field=1,
        colour={"points": [[1e4, 1, 0, 0], [1e7, 0, 0, 1]], "log": True},
        width=64, height=64, cameras=[top], tone={"saturation": 1.0})
    task, loop = _render(router, req, _Session(path))
    try:
        assert task.status == "completed", task.error
        img = np.asarray(Image.open(
            loop.run_until_complete(results.particle_render_frame(task.task_id, 0,
                                                                  token="tok")).path),
            dtype=float)
        left, right = img[32, 22], img[32, 42]
        assert left[0] > left[2] and right[2] > right[0]
    finally:
        loop.close()


def test_a_projected_map_is_rendered_and_graded(tmp_path, monkeypatch):
    from PIL import Image

    from app.routers import particle_render as results
    from app.routers import volume_render as router
    monkeypatch.setattr(results, "get_exports_dir", lambda: tmp_path / "exports")
    path, _, _ = _volume(tmp_path)
    req = router.VolumeRenderRequest(dataset_id="vol", mode="projected", colour_by="colour",
                                     colour_field=1, colour={"lut": "Viridis", "log": True},
                                     slab=10.0, width=80, height=60, cameras=[CAM])
    task, loop = _render(router, req, _Session(path))
    try:
        assert task.status == "completed", task.error
        run = loop.run_until_complete
        light = results._light(task.task_id, "tok")
        assert light.shape == (60, 80, 2)
        lit = np.isfinite(light[..., 1])
        assert lit.any() and np.all((light[..., 1][lit] >= 0) & (light[..., 1][lit] <= 1))
        graded = run(results.particle_render_tone(task.task_id, results.RaytraceTone(
            decades=2.0), token="tok"))
        assert Image.open(io.BytesIO(graded.body)).size == (80, 60)
    finally:
        loop.close()


def test_the_grid_for_the_gpu_is_what_a_render_draws(tmp_path):
    from app.routers import volume_render as router
    from app.volume_raytrace import colour_index, emission_from
    path, density, temperature = _volume(tmp_path)
    req = router.VolumeGridRequest(dataset_id="vol", colour_by="colour", colour_field=1,
                                   colour={"lut": "Viridis", "log": True},
                                   box=(0, 5, 0, 5, 0, 5))
    reply, loop = _run(lambda: router.volume_render_grid(req, token="tok",
                                                         session=_Session(path)))
    loop.close()
    nz, ny, nx = map(int, reply.headers["X-Visivo-Volume-Shape"].split(","))
    assert (nz, ny, nx) == (N, N, N)
    assert [float(v) for v in reply.headers["X-Visivo-Volume-Box"].split(",")] == [0, 5] * 3
    lo, hi, log = reply.headers["X-Visivo-Colour-Range"].split(",")
    assert float(lo) == pytest.approx(1e4, rel=1e-3) and log == "1"
    n = N ** 3
    body = reply.body
    assert len(body) == n * 4 + n * 2 + 256 * 3 * 4
    e = np.frombuffer(body[:n * 4], "<f4").reshape(N, N, N)
    c = np.frombuffer(body[n * 4:n * 6], "<f2").reshape(N, N, N)
    lut = np.frombuffer(body[n * 6:], "<f4").reshape(256, 3)
    assert np.allclose(e, emission_from(density))
    assert np.allclose(c, colour_index(temperature, log=True)[0], atol=1e-3)
    assert lut.min() >= 0 and lut.max() <= 1


def test_a_large_grid_is_read_with_a_stride(tmp_path):
    from app.routers import volume_render as router
    path, _, _ = _volume(tmp_path)
    req = router.VolumeGridRequest(dataset_id="vol", max_voxels=16 ** 3)
    reply, loop = _run(lambda: router.volume_render_grid(req, token="tok",
                                                         session=_Session(path)))
    loop.close()
    assert reply.headers["X-Visivo-Volume-Shape"] == "12,12,12"
    # Its points are the cube's voxels 0, 2, …, 22: its box ends at 22, not
    # stretched over the whole cube's 0..23.
    assert [float(v) for v in reply.headers["X-Visivo-Volume-Box"].split(",")] == [0, 22] * 3


def test_a_gpu_grid_keeps_each_axis_within_a_texture(tmp_path):
    from app.routers import volume_render as router
    path, _, _ = _volume(tmp_path)
    req = router.VolumeGridRequest(dataset_id="vol", max_axis=16)
    reply, loop = _run(lambda: router.volume_render_grid(req, token="tok",
                                                         session=_Session(path)))
    loop.close()
    assert reply.headers["X-Visivo-Volume-Shape"] == "12,12,12"


def test_what_cannot_be_rendered_is_refused(tmp_path):
    from astropy.io import fits

    from app.routers import volume_render as router
    path, _, _ = _volume(tmp_path)
    session = _Session(path)
    plane = tmp_path / "plane.fits"
    fits.PrimaryHDU(np.ones((1, 16, 16), np.float32)).writeto(plane)
    session.datasets["plane"] = {"kind": "cube", "path": str(plane)}
    for request in (router.VolumeRenderRequest(dataset_id="points", cameras=[CAM]),
                    # One plane deep: no depth for a ray to cross.
                    router.VolumeRenderRequest(dataset_id="plane", cameras=[CAM]),
                    router.VolumeRenderRequest(dataset_id="vol", colour_by="colour",
                                               cameras=[CAM]),
                    router.VolumeRenderRequest(dataset_id="vol", colour_by="colour",
                                               colour_field=5, cameras=[CAM]),
                    router.VolumeRenderRequest(dataset_id="fits", colour_by="colour",
                                               colour_field=1, cameras=[CAM])):
        with pytest.raises(HTTPException) as err:
            _run(lambda: router.create_volume_render_task(request, token="tok",
                                                          session=session))
        assert err.value.status_code == 422
