"""Ray-traced particle rendering (issue #101): the renderer's model and the
task that renders stills and movie frames."""
from __future__ import annotations

import asyncio
import io

import numpy as np
import pytest

pytest.importorskip("numba")

from app.particle_raytrace import Camera, project, render, smoothing_lengths, tone_map  # noqa: E402

FRONT = Camera((0.0, 0.0, 10.0), (0.0, 0.0, 0.0), (0.0, 1.0, 0.0), 30.0)
FRONT_JSON = {"position": (0, 0, 10), "focal_point": (0, 0, 0), "view_up": (0, 1, 0)}


def test_particles_land_where_vtk_draws_them():
    vtk = pytest.importorskip("vtk")
    ren = vtk.vtkRenderer()
    win = vtk.vtkRenderWindow()
    win.SetOffScreenRendering(1)
    win.SetSize(320, 200)
    win.AddRenderer(ren)
    cam = ren.GetActiveCamera()
    cam.SetPosition(3.0, -2.0, 9.0)
    cam.SetFocalPoint(0.2, 0.1, -0.3)
    cam.SetViewUp(0.1, 1.0, 0.2)
    cam.SetViewAngle(40.0)
    cam.OrthogonalizeViewUp()
    ren.ResetCameraClippingRange(-5, 5, -5, 5, -5, 5)
    pts = np.random.default_rng(1).uniform(-2, 2, (20, 3)).astype(np.float32)
    sx, sy, _, depth = project(pts, np.ones(20), Camera.from_vtk(cam), 320, 200)
    coord = vtk.vtkCoordinate()
    coord.SetCoordinateSystemToWorld()
    for i, p in enumerate(pts):
        coord.SetValue(*map(float, p))
        dx, dy = coord.GetComputedDoubleDisplayValue(ren)
        # VTK counts rows from the bottom, an image from the top.
        assert sx[i] == pytest.approx(dx, abs=1e-2)
        assert sy[i] == pytest.approx(200 - dy, abs=1e-2)
        assert depth[i] > 0


def _one(radius, quantity="per_particle"):
    """The light of a particle at the centre, next to nine of size 0.3 far
    off-screen that set the median size the weighting is relative to."""
    pos = np.zeros((10, 3), np.float32)
    pos[1:, 0] = 100.0
    radii = np.full(10, 0.3)
    radii[0] = radius
    return render(pos, radii, np.ones((10, 3), np.float32), np.ones(10, np.float32), FRONT,
                  96, 96, quantity=quantity)


def test_a_particle_keeps_its_light_whatever_its_size():
    # Per particle: the light a particle sends is its own, spread or not, and
    # one smaller than a pixel is widened with the same light.
    totals = [_one(r).sum() for r in (0.4, 0.2, 0.01, 0.0005)]
    assert np.allclose(totals, totals[0], rtol=0.03)
    # A density: its light is the column through it, h times its surface.
    big, small = _one(0.4, "density").sum(), _one(0.2, "density").sum()
    assert big / small == pytest.approx(8.0, rel=0.05)


def test_emission_alone_does_not_depend_on_the_order():
    pos = np.array([[0, 0, 1.0], [0.05, 0, -1.0], [0.1, 0.05, 0.0]], np.float32)
    colour = np.array([[1, 0, 0], [0, 1, 0], [0, 0, 1]], np.float32)
    args = (np.full(3, 0.3), colour, np.ones(3, np.float32), FRONT, 64, 64)
    a = render(pos, *args)
    b = render(pos[::-1].copy(), args[0], colour[::-1].copy(), *args[2:])
    assert np.allclose(a, b, atol=1e-6)


def test_absorption_dims_what_lies_behind_only():
    # A red emitter and a dark absorber on the same line of sight.
    colour = np.array([[1, 0, 0], [0, 0, 0]], np.float32)
    light = np.ones(2, np.float32)

    def red(absorber_in_front):
        z = (-1.0, 1.0) if absorber_in_front else (1.0, -1.0)
        pos = np.array([[0, 0, z[0]], [0, 0, z[1]]], np.float32)
        img = render(pos, np.full(2, 0.5), colour, light, FRONT, 48, 48, absorption=2.0)
        return img[24, 24, 0]

    emitted = render(np.zeros((1, 3), np.float32), np.array([0.5]), colour[:1], light[:1],
                     FRONT, 48, 48)[24, 24, 0]
    assert red(absorber_in_front=True) < 0.5 * red(absorber_in_front=False)
    # Behind it, the absorber leaves the red as the red's own absorption does.
    assert red(absorber_in_front=False) <= emitted



def test_many_faint_absorbers_dim_what_lies_behind_them():
    from app.particle_raytrace import _bin, _composite
    # One emitter, then 200 000 absorbers in front of it, each with an optical
    # depth of 5e-7: 0.1 together, whatever their number.
    n = 200_001
    sx = np.full(n, 0.5, np.float32)
    sy = np.full(n, 0.5, np.float32)
    sr = np.full(n, 0.5, np.float32)
    weight = np.ones(n, np.float32)
    emission = np.zeros((n, 3), np.float32)
    emission[0] = 1.0
    absorb = np.full((n, 3), 5e-7, np.float32)
    absorb[0] = 0.0
    order = np.arange(n, dtype=np.int64)   # back to front: the emitter first
    counts, items = _bin(sx, sy, sr, order, 1, 1, 1, 1)
    image = _composite(sx, sy, sr, weight, emission, absorb, counts, items, 1, 1, 1, 1)
    assert image[0, 0, 0] == pytest.approx(np.exp(-0.1), rel=2e-3)

def test_the_tone_curve_keeps_the_hue_and_the_order():
    light = np.zeros((2, 4, 3), np.float32)
    light[0, :, 0] = [0.1, 1.0, 10.0, 100.0]
    light[0, :, 1] = light[0, :, 0] * 0.5
    out = tone_map(light, exposure=0.5, curve="asinh", stretch=10.0, gamma=1.0)
    assert (out[1] == 0).all()                       # no light, black
    assert (np.diff(out[0, :, 0].astype(int)) >= 0).all()
    unsaturated = out[0, :2]
    ratio = unsaturated[:, 1] / unsaturated[:, 0]
    assert np.allclose(ratio, 0.5, atol=0.02)          # orange stays orange
    with pytest.raises(ValueError):
        tone_map(light, curve="filmic")


def test_smoothing_lengths_from_the_neighbours():
    g = np.stack(np.meshgrid(*[np.arange(8.0)] * 3, indexing="ij"), -1).reshape(-1, 3)
    h = smoothing_lengths(g, neighbours=6)
    inner = np.all((g > 0) & (g < 7), axis=1)
    # Six neighbours at distance 1 inside the grid: half of it.
    assert np.allclose(h[inner], 0.5)


# ── The task ─────────────────────────────────────────────────────────────


def _vbt(tmp_path, n=3000):
    rng = np.random.default_rng(4)
    cols = np.vstack([rng.normal(0, 1, (3, n)), rng.uniform(1, 2, (1, n)),
                      np.log10(rng.uniform(1, 10, (1, n))), rng.uniform(0.05, 0.2, (1, n))])
    cols.astype("<f4").tofile(tmp_path / "p.bin")
    (tmp_path / "p.bin.head").write_text(f"float\n6\n{n}\nlittle\nX\nY\nZ\nT\nlogM\nH\n")
    return str(tmp_path / "p.bin.head")


class _Session:
    def __init__(self, head):
        self.datasets = {"cat_1": {"kind": "catalogue", "format": "vbt", "path": head},
                         "cube_1": {"kind": "cube", "path": "/d/c.fits"}}


def _run_task(router, request, session):
    loop = asyncio.new_event_loop()

    async def go():
        created = await router.create_particle_render_task(request, token="tok", session=session)
        from app.tasks import TASKS
        for _ in range(1200):
            task = TASKS.get(created.task_id)
            if task.status not in ("pending", "running"):
                return task
            await asyncio.sleep(0.05)
        raise AssertionError("render task did not end")

    try:
        return loop.run_until_complete(go()), loop
    except Exception:
        loop.close()
        raise


def test_a_still_and_a_movie_are_rendered_and_graded(tmp_path, monkeypatch):
    from PIL import Image

    from app.routers import particle_render as router
    monkeypatch.setattr(router, "get_exports_dir", lambda: tmp_path / "exports")
    session = _Session(_vbt(tmp_path))
    cam = {"position": (0, 0, 12), "focal_point": (0, 0, 0), "view_up": (0, 1, 0)}
    still = router.ParticleRenderRequest(
        dataset_id="cat_1", colour={"field": "T", "lut": "Viridis"}, radius_field="H",
        intensity_field="logM", intensity_log10=True, absorption=0.5,
        width=160, height=90, cameras=[cam], filters=[{"field": "T", "op": ">", "value": 1.2}])
    task, loop = _run_task(router, still, session)
    try:
        assert task.status == "completed", task.error
        assert task.result["frames"] == 1 and task.result["hdr"]
        assert 0 < task.result["particles"] < 3000          # the filter applied
        run = loop.run_until_complete
        frame = run(router.particle_render_frame(task.task_id, 0, token="tok"))
        img = Image.open(frame.path)
        assert img.size == (160, 90) and np.asarray(img).max() > 0
        graded = run(router.particle_render_tone(task.task_id, router.RaytraceTone(
            curve="log", exposure=2.0 * task.result["exposure"]), token="tok"))
        assert Image.open(io.BytesIO(graded.body)).size == (160, 90)
        fits_bytes = run(router.particle_render_light_fits(task.task_id, token="tok")).body
        from astropy.io import fits
        with fits.open(io.BytesIO(fits_bytes)) as hdul:
            assert hdul[0].data.shape == (3, 90, 160)
        # Another owner sees nothing.
        with pytest.raises(router.HTTPException):
            run(router.particle_render_frame(task.task_id, 0, token="other"))
        assert run(router.delete_particle_render(task.task_id, token="tok"))["deleted"]
        assert not (tmp_path / "exports" / ".renders" / task.task_id).exists()
    finally:
        loop.close()

    # A movie: one frame per camera, at one exposure, without its light kept.
    orbit = [{"position": (12 * np.sin(a), 0, 12 * np.cos(a)), "focal_point": (0, 0, 0),
              "view_up": (0, 1, 0)} for a in np.linspace(0, np.pi / 2, 3)]
    movie = router.ParticleRenderRequest(dataset_id="cat_1", width=64, height=48, cameras=orbit)
    task, loop = _run_task(router, movie, session)
    loop.close()
    assert task.status == "completed", task.error
    files = sorted((tmp_path / "exports" / ".renders" / task.task_id).iterdir())
    assert [f.name for f in files] == ["frame_00000.png", "frame_00001.png", "frame_00002.png"]


def test_a_render_that_cannot_be_made_fails_or_is_refused(tmp_path, monkeypatch):
    from app.routers import particle_render as router
    monkeypatch.setattr(router, "get_exports_dir", lambda: tmp_path / "exports")
    session = _Session(_vbt(tmp_path))
    cam = {"position": (0, 0, 12), "focal_point": (0, 0, 0), "view_up": (0, 1, 0)}
    bad = router.ParticleRenderRequest(dataset_id="cat_1", intensity_field="nope", cameras=[cam],
                                       width=32, height=32)
    task, loop = _run_task(router, bad, session)
    loop.close()
    assert task.status == "failed" and "nope" in task.error
    with pytest.raises(router.HTTPException) as err:
        asyncio.new_event_loop().run_until_complete(router.create_particle_render_task(
            router.ParticleRenderRequest(dataset_id="cube_1", cameras=[cam]),
            token="tok", session=session))
    assert err.value.status_code == 422


def test_a_named_map_on_a_log_scale_spreads_the_decades():
    # Decades from 1e2 to 1e8 over a column that reaches zero: evenly over the
    # map, not crowded at its foot; zero takes the lowest colour.
    from app.render_session._common import _build_color_tf
    from app.routers.particle_render import RaytraceColour, _colours
    values = np.array([0.0, 1e2, 1e4, 1e6, 1e8])
    rgb = _colours({"T": values}, np.arange(5), RaytraceColour(field="T", lut="Inferno", log=True))
    ctf = _build_color_tf("Inferno", 0.0, 1.0)
    for value, fraction in ((1e2, 0.0), (1e4, 1 / 3), (1e6, 2 / 3), (1e8, 1.0)):
        i = list(values).index(value)
        assert np.allclose(rgb[i], ctf.GetColor(fraction), atol=0.02), value
    assert np.allclose(rgb[0], rgb[1], atol=0.02)


def test_the_preview_follows_the_camera_on_particles_prepared_once(tmp_path, monkeypatch):
    from PIL import Image

    from app.routers import particle_render as router
    session = _Session(_vbt(tmp_path))
    prepared = []
    real = router._prepare
    monkeypatch.setattr(router, "_prepare", lambda e, r: prepared.append(1) or real(e, r))
    monkeypatch.setattr(router, "_PREPARED", router.OrderedDict())
    loop = asyncio.new_event_loop()
    try:
        def frame(angle, exposure=None):
            cam = {"position": (12 * np.sin(angle), 0, 12 * np.cos(angle)),
                   "focal_point": (0, 0, 0), "view_up": (0, 1, 0)}
            req = router.ParticlePreviewRequest(
                dataset_id="cat_1", cameras=[cam], width=96, height=54,
                colour={"field": "T", "lut": "Inferno"}, intensity_field="logM",
                intensity_log10=True, tone={"exposure": exposure})
            return loop.run_until_complete(
                router.particle_render_preview(req, token="tok", session=session))

        first = frame(0.0)
        assert Image.open(io.BytesIO(first.body)).size == (96, 54)
        found = float(first.headers["X-Visivo-Exposure"])
        assert found > 0
        # Turned, at the exposure found: the particles are not prepared again,
        # and the exposure is the one given.
        turned = frame(0.5, exposure=found)
        assert float(turned.headers["X-Visivo-Exposure"]) == pytest.approx(found)
        assert turned.body != first.body and len(prepared) == 1
        # One stop more: brighter, at the same base exposure.
        req = router.ParticlePreviewRequest(
            dataset_id="cat_1", width=96, height=54, intensity_field="logM",
            intensity_log10=True, colour={"field": "T", "lut": "Inferno"},
            cameras=[{"position": (12 * np.sin(0.5), 0, 12 * np.cos(0.5)),
                      "focal_point": (0, 0, 0), "view_up": (0, 1, 0)}],
            tone={"exposure": found, "ev": 1.0})
        brighter = loop.run_until_complete(
            router.particle_render_preview(req, token="tok", session=session))
        assert float(brighter.headers["X-Visivo-Exposure"]) == pytest.approx(found)
        mean = lambda r: np.asarray(Image.open(io.BytesIO(r.body))).mean()  # noqa: E731
        assert mean(brighter) > mean(turned)
    finally:
        loop.close()


# ── Projected maps ───────────────────────────────────────────────────────


def test_a_projected_map_sums_the_mass_and_averages_the_value():
    from app.particle_raytrace import projected_maps
    rng = np.random.default_rng(2)
    pos = rng.normal(0, 1, (4000, 3)).astype(np.float32)
    radius = np.full(4000, 0.1)
    ones = np.ones(4000, np.float32)
    maps = projected_maps(pos, radius, ones, np.full(4000, 7.0), FRONT, 64, 64)
    lit = maps[..., 0] > 0
    assert np.allclose(maps[..., 1][lit], 7.0, rtol=1e-4)      # a constant averages to itself
    assert np.isnan(maps[..., 1][~lit]).all()
    double = projected_maps(pos, radius, 2 * ones, np.full(4000, 7.0), FRONT, 64, 64)
    assert np.allclose(double[..., 0], 2 * maps[..., 0], rtol=1e-5)
    # Two populations: the mean is weighted by the mass.
    value = np.where(np.arange(4000) < 2000, 1.0, 3.0)
    weight = np.where(np.arange(4000) < 2000, 3.0, 1.0).astype(np.float32)
    mixed = projected_maps(np.vstack([pos[:2000], pos[:2000]]), radius, weight, value, FRONT,
                           64, 64)
    assert np.allclose(mixed[..., 1][mixed[..., 0] > 0], 1.5, rtol=1e-4)


def test_a_slab_keeps_what_lies_near_the_focal_plane():
    from app.particle_raytrace import in_slab, parallel_camera
    pos = np.array([[0, 0, 0.0], [0, 0, 0.9], [0, 0, -1.1], [3, 3, 0.2]], np.float32)
    assert in_slab(pos, FRONT, 2.0).tolist() == [True, True, False, True]
    assert in_slab(pos, FRONT, None).all()
    # Without perspective, what the camera sees at its focal plane.
    flat = parallel_camera(FRONT)
    assert flat.parallel and flat.parallel_scale == pytest.approx(10 * np.tan(np.radians(15)))


def test_the_velocity_dispersion_of_the_neighbours():
    from app.particle_raytrace import velocity_dispersion
    rng = np.random.default_rng(6)
    pos = rng.uniform(0, 1, (20000, 3))
    vel = rng.normal(0, 2.0, (20000, 3))
    sigma = velocity_dispersion(pos, vel, neighbours=64)
    assert np.median(sigma) == pytest.approx(2.0, rel=0.05)
    # A cold flow: one velocity everywhere, no dispersion.
    assert np.allclose(velocity_dispersion(pos[:500], np.ones((500, 3)), 16), 0.0)


def test_a_projected_still_is_rendered_and_graded_again(tmp_path, monkeypatch):
    from PIL import Image

    from app.routers import particle_render as router
    monkeypatch.setattr(router, "get_exports_dir", lambda: tmp_path / "exports")
    rng = np.random.default_rng(8)
    n = 3000
    cols = np.vstack([rng.normal(0, 1, (3, n)), rng.normal(0, 3, (3, n)), np.ones((1, n))])
    cols.astype("<f4").tofile(tmp_path / "v.bin")
    (tmp_path / "v.bin.head").write_text(f"float\n7\n{n}\nlittle\nX\nY\nZ\nVX\nVY\nVZ\nM\n")
    session = _Session(str(tmp_path / "v.bin.head"))
    cam = {"position": (0, 0, 12), "focal_point": (0, 0, 0), "view_up": (0, 1, 0)}
    req = router.ParticleRenderRequest(
        dataset_id="cat_1", mode="projected", colour_by="velocity_dispersion", slab=3.0,
        colour={"lut": "Magma"}, intensity_field="M", cameras=[cam], width=120, height=90)
    task, loop = _run_task(router, req, session)
    try:
        assert task.status == "completed", task.error
        run = loop.run_until_complete
        first = np.asarray(Image.open(run(router.particle_render_frame(task.task_id, 0,
                                                                       token="tok")).path))
        assert first.shape == (90, 120, 3) and first.max() > 0
        darker = run(router.particle_render_tone(task.task_id, router.RaytraceTone(ev=-2.0),
                                                 token="tok"))
        assert np.asarray(Image.open(io.BytesIO(darker.body))).mean() < first.mean()
        from astropy.io import fits
        with fits.open(io.BytesIO(run(router.particle_render_light_fits(
                task.task_id, token="tok")).body)) as hdul:
            assert hdul[0].data.shape == (2, 90, 120)
            assert hdul[0].header["CTYPE3"] == "MAP"
    finally:
        loop.close()
    # Without velocity columns, the dispersion is refused with a reason.
    plain = router.ParticleRenderRequest(dataset_id="cat_1", mode="projected",
                                         colour_by="velocity_dispersion", velocity=("M", "M", "Q"),
                                         cameras=[cam], width=32, height=32)
    task, loop = _run_task(router, plain, session)
    loop.close()
    assert task.status == "failed" and "velocity" in task.error


def test_a_draft_sample_carries_the_light_of_the_particles_it_stands_for():
    from app.routers.particle_render import _sampled
    particles = {"positions": np.zeros((1000, 3), np.float32), "radius": np.ones(1000),
                 "intensity": np.ones(1000, np.float32)}
    sample = _sampled(particles, 100)
    assert len(sample["positions"]) == 100
    assert sample["intensity"].sum() == pytest.approx(1000.0)
    assert _sampled(particles, None) is particles


def test_the_particles_for_the_gpu_are_those_the_renderer_draws(tmp_path, monkeypatch):
    from app.particle_raytrace import emission_weights
    from app.routers import particle_render as router
    monkeypatch.setattr(router, "_PREPARED", router.OrderedDict())
    session = _Session(_vbt(tmp_path))
    loop = asyncio.new_event_loop()
    try:
        req = router.ParticleDataRequest(dataset_id="cat_1", intensity_field="logM",
                                         intensity_log10=True, radius_field="H",
                                         colour={"field": "T", "lut": "Inferno"})
        reply = loop.run_until_complete(router.particle_render_particles(
            req, token="tok", session=session))
        assert reply.headers["X-Visivo-Particle-Fields"] == "x,y,z,r,e,red,green,blue"
        n = int(reply.headers["X-Visivo-Particle-Count"])
        rows = np.frombuffer(reply.body, dtype="<f4").reshape(n, 8)
        prepared = router._prepare(session.datasets["cat_1"], req)
        origin, scale = prepared["frame"]
        # Without local_frame, the table's own coordinates (as before).
        assert n == 3000 and np.allclose(rows[:, :3], origin + scale * prepared["positions"])
        assert reply.headers["X-Visivo-Particle-Origin"] == "0.0,0.0,0.0"
        assert reply.headers["X-Visivo-Particle-Scale"] == "1.0"
        assert np.allclose(rows[:, 4], emission_weights(prepared["intensity"], prepared["radius"],
                                                        "per_particle"), rtol=1e-5)
        # A projected map, sampled: the mass of the rows left adds up the same.
        flat = router.ParticleDataRequest(dataset_id="cat_1", mode="projected",
                                          max_particles=1000)
        reply = loop.run_until_complete(router.particle_render_particles(
            flat, token="tok", session=session))
        assert reply.headers["X-Visivo-Particle-Step"] == "3"
        rows = np.frombuffer(reply.body, dtype="<f4").reshape(-1, 6)
        assert len(rows) == 1000 and reply.headers["X-Visivo-Particle-Fields"] == "x,y,z,r,w,v"
    finally:
        loop.close()



def test_a_projected_value_the_gpu_gets_is_never_nan(tmp_path, monkeypatch):
    from app.routers import particle_render as router
    particles = {"positions": np.zeros((4, 3), np.float32), "radius": np.ones(4, np.float32),
                 "intensity": np.ones(4, np.float32),
                 "value": np.array([1.0, np.nan, 3.0, np.nan], np.float32)}
    monkeypatch.setattr(router, "_prepared", lambda entry, req, token: particles)
    session = _Session(_vbt(tmp_path, n=10))
    loop = asyncio.new_event_loop()
    try:
        req = router.ParticleDataRequest(dataset_id="cat_1", mode="projected")
        reply = loop.run_until_complete(router.particle_render_particles(
            req, token="tok", session=session))
    finally:
        loop.close()
    rows = np.frombuffer(reply.body, dtype="<f4").reshape(-1, 6)
    assert np.isfinite(rows).all()
    assert np.array_equal(rows[:, 5], [1.0, 0.0, 3.0, 0.0])

def test_a_render_deleted_while_it_runs_stops_and_leaves_nothing(tmp_path, monkeypatch):
    from app.routers import particle_render as router
    from app.tasks import TASKS
    monkeypatch.setattr(router, "get_exports_dir", lambda: tmp_path / "exports")
    session = _Session(_vbt(tmp_path))
    cams = [{"position": (12 * np.sin(a), 0, 12 * np.cos(a)), "focal_point": (0, 0, 0),
             "view_up": (0, 1, 0)} for a in np.linspace(0, 6, 300)]
    req = router.ParticleRenderRequest(dataset_id="cat_1", cameras=cams, width=200, height=150)
    loop = asyncio.new_event_loop()

    async def go():
        created = await router.create_particle_render_task(req, token="tok", session=session)
        task_id = created.task_id
        frames = tmp_path / "exports" / ".renders" / task_id
        for _ in range(400):      # under way: some frames written
            if frames.is_dir() and any(frames.iterdir()):
                break
            await asyncio.sleep(0.02)
        assert (await router.delete_particle_render(task_id, token="tok"))["deleted"]
        for _ in range(400):
            await asyncio.sleep(0.05)
            if not router._STOPS.get(task_id) and not frames.exists():
                break
        return TASKS.get(task_id), frames

    try:
        task, frames = loop.run_until_complete(go())
    finally:
        loop.close()
    assert task.status == "cancelled"
    assert not frames.exists()


def test_a_render_stopped_during_its_last_frame_is_not_published(tmp_path, monkeypatch):
    import threading

    from app.routers import particle_render as router
    monkeypatch.setattr(router, "get_exports_dir", lambda: tmp_path / "exports")
    session = _Session(_vbt(tmp_path))
    req = router.ParticleRenderRequest(dataset_id="cat_1", cameras=[FRONT_JSON], width=64,
                                       height=48)
    stop = threading.Event()
    real_frame = router._frame

    def frame(*args, **kwargs):   # the delete arrives while the frame renders
        stop.set()
        return real_frame(*args, **kwargs)

    monkeypatch.setattr(router, "_frame", frame)
    with pytest.raises(asyncio.CancelledError):
        router._run("t_last", session.datasets["cat_1"], req, stop)
    assert not (tmp_path / "exports" / ".renders" / "t_last").exists()


def test_a_cancelled_render_keeps_its_stop_until_its_thread_ends(tmp_path, monkeypatch):
    import threading

    from app.routers import particle_render as router
    from app.tasks import TASKS
    release, entered = threading.Event(), threading.Event()

    def run(task_id, entry, req, stop):
        entered.set()
        release.wait(10)
        raise asyncio.CancelledError

    monkeypatch.setattr(router, "_run", run)
    session = _Session(_vbt(tmp_path))
    req = router.ParticleRenderRequest(dataset_id="cat_1", cameras=[FRONT_JSON], width=64,
                                       height=48)
    loop = asyncio.new_event_loop()

    async def go():
        created = await router.create_particle_render_task(req, token="tok", session=session)
        task_id = created.task_id
        while not entered.is_set():
            await asyncio.sleep(0.01)
        TASKS.cancel(task_id)
        await asyncio.sleep(0.1)
        during = task_id in router._STOPS and TASKS.peek(task_id).busy()
        release.set()
        for _ in range(200):
            await asyncio.sleep(0.02)
            if task_id not in router._STOPS:
                break
        return during, task_id in router._STOPS, TASKS.get(task_id).status

    try:
        during, after, status = loop.run_until_complete(go())
    finally:
        release.set()
        loop.close()
    assert during and not after
    assert status == "cancelled"


def test_an_evicted_render_takes_its_frames_with_it(tmp_path, monkeypatch):
    import time
    from types import SimpleNamespace

    from app.routers import particle_render as router
    monkeypatch.setattr(router, "get_exports_dir", lambda: tmp_path / "exports")
    frames = tmp_path / "exports" / ".renders" / "t_old"
    frames.mkdir(parents=True)
    (frames / "frame_00000.png").write_bytes(b"x")
    router._on_evict(SimpleNamespace(operation="particle_render", task_id="t_old"))
    for _ in range(100):
        if not frames.exists():
            break
        time.sleep(0.02)
    assert not frames.exists()


# ── Particles far from zero (issue #208) ─────────────────────────────────


def _far(tmp_path, x, y=None, z=None, h=0.1, name="far"):
    """A float64 VBT: X, Y, Z (zeros when not given) and a smoothing length H."""
    n = len(x)
    cols = [np.asarray(x, float), np.zeros(n) if y is None else np.asarray(y, float),
            np.zeros(n) if z is None else np.asarray(z, float), np.full(n, float(h))]
    np.stack(cols).astype("<f8").tofile(tmp_path / f"{name}.bin")
    (tmp_path / f"{name}.bin.head").write_text(f"double\n4\n{n}\nlittle\nX\nY\nZ\nH\n")
    return {"kind": "catalogue", "format": "vbt", "path": str(tmp_path / f"{name}.bin.head")}


def _peaks(profile: np.ndarray, floor: float) -> list[float]:
    """The centre of each separate run of columns above ``floor``, in pixels
    from the image's left edge (weighted by the profile: a saturated preview
    is flat on top)."""
    lit = np.flatnonzero(profile > floor)
    runs = np.split(lit, np.flatnonzero(np.diff(lit) > 1) + 1) if lit.size else []
    return [float(np.average(r + 0.5, weights=profile[r])) for r in runs]


_FAR_X = 1e9 + np.array([0.0, 1.0, 2.0])
_FAR_Z = -3e9
_FAR_CAM = {"position": (1e9 + 1, 0.0, _FAR_Z + 1), "focal_point": (1e9 + 1, 0.0, _FAR_Z),
            "view_up": (0, 1, 0)}


def _expected_columns(x, width, height):
    """Where the particles at ``x`` (y 0, z _FAR_Z) fall, projected in float64
    from the table's own coordinates."""
    pos = np.stack([np.asarray(x, float), np.zeros(len(x)), np.full(len(x), _FAR_Z)], axis=1)
    sx, _, _, _ = project(pos, np.ones(len(x)), Camera(**_FAR_CAM), width, height)
    return sx


def test_particles_far_from_zero_are_three_in_a_still_and_the_preview(tmp_path, monkeypatch):
    """1e9, 1e9 + 1 and 1e9 + 2 were one float32 value, so one particle: they
    are three, where the table's coordinates put them (the camera in them)."""
    from PIL import Image

    from app.routers import particle_render as router
    monkeypatch.setattr(router, "get_exports_dir", lambda: tmp_path / "exports")
    monkeypatch.setattr(router, "_PREPARED", router.OrderedDict())

    class _S:
        datasets = {"far": _far(tmp_path, _FAR_X, z=np.full(3, _FAR_Z))}
    width, height = 400, 80
    expected = _expected_columns(_FAR_X, width, height)
    assert np.all(np.diff(expected) > 100)          # a hundred pixels apart
    still = router.ParticleRenderRequest(dataset_id="far", radius_field="H", width=width,
                                         height=height, cameras=[_FAR_CAM])
    task, loop = _run_task(router, still, _S())
    try:
        assert task.status == "completed", task.error
        assert task.result["particles"] == 3
        light = router._light(task.task_id, "tok").sum(axis=-1)
        peaks = _peaks(light.sum(axis=0), 1e-3 * light.sum(axis=0).max())
        assert len(peaks) == 3
        assert np.abs(np.array(peaks) - expected).max() <= 1.0

        preview = router.ParticlePreviewRequest(dataset_id="far", radius_field="H", width=width,
                                                height=height, cameras=[_FAR_CAM])
        jpeg = loop.run_until_complete(router.particle_render_preview(preview, token="tok",
                                                                      session=_S()))
        grey = np.asarray(Image.open(io.BytesIO(jpeg.body)).convert("L"), dtype=float)
        peaks = _peaks(grey.max(axis=0), 40.0)
        assert len(peaks) == 3
        assert np.abs(np.array(peaks) - expected).max() <= 1.5
    finally:
        loop.close()


def test_the_gpu_rows_carry_their_frame_far_from_zero(tmp_path, monkeypatch):
    """The desktop's GPU preview: asked for the local frame, float32 offsets
    and their origin rebuild the table's coordinates exactly; a client that
    does not ask gets the table's coordinates in float32, as before."""
    from app.routers import particle_render as router
    monkeypatch.setattr(router, "_PREPARED", router.OrderedDict())

    class _S:
        datasets = {"far": _far(tmp_path, _FAR_X, z=np.full(3, _FAR_Z))}
    loop = asyncio.new_event_loop()
    try:
        def rows(local):
            req = router.ParticleDataRequest(dataset_id="far", radius_field="H",
                                             local_frame=local)
            reply = loop.run_until_complete(router.particle_render_particles(
                req, token="tok", session=_S()))
            origin = [float(v) for v in reply.headers["X-Visivo-Particle-Origin"].split(",")]
            scale = float(reply.headers["X-Visivo-Particle-Scale"])
            return np.frombuffer(reply.body, dtype="<f4").reshape(-1, 8), origin, scale

        local, origin, scale = rows(True)
        table = origin + scale * local[:, :3].astype(float)
        assert table[:, 0].tolist() == _FAR_X.tolist()
        assert table[:, 2].tolist() == [_FAR_Z] * 3
        assert np.allclose(scale * local[:, 3], 0.1)            # the size, in table units
        old, origin, scale = rows(False)
        assert origin == [0.0, 0.0, 0.0] and scale == 1.0
        assert np.array_equal(old[:, :3], (_FAR_X[:, None] * [1, 0, 0]
                                           + [0, 0, _FAR_Z]).astype(np.float32))
        assert np.array_equal(old[:, 3:], local[:, 3:])          # the rest unchanged
    finally:
        loop.close()


def test_smoothing_lengths_do_not_depend_on_where_the_table_lies(tmp_path):
    """The neighbours' smoothing lengths, and the light weighted by them, are
    the same 1e9 away from 0 as near it (a float32 cast put the particles on a
    grid of 64 there)."""
    from app.routers import particle_render as router
    rng = np.random.default_rng(3)
    xyz = rng.normal(0.0, 1.0, (3, 2000))
    near = _far(tmp_path, xyz[0], xyz[1], xyz[2], name="near")
    far = _far(tmp_path, xyz[0] + 1e9, xyz[1] - 2e9, xyz[2] + 3e9, name="far")
    for radius_field in (None, "H"):
        req = router.ParticleRenderRequest(dataset_id="d", radius_field=radius_field,
                                           neighbours=16, cameras=[FRONT_JSON])
        a, b = router._prepare(near, req), router._prepare(far, req)
        assert np.allclose(a["radius"], b["radius"], rtol=1e-4)
        assert np.array_equal(a["intensity"], b["intensity"])
        assert b["frame"][1] == 1.0
        assert np.allclose(b["positions"] + b["frame"][0] - [1e9, -2e9, 3e9], xyz.T, atol=1e-6)


def test_a_projected_slab_far_from_zero_keeps_its_depth(tmp_path):
    """A projected map's slab is measured from the focal plane in the table's
    units: 0.2 behind it is in a slab of 1, 5 behind it out, 3e9 from 0."""
    from app.routers import particle_render as router
    entry = _far(tmp_path, _FAR_X, z=_FAR_Z + np.array([0.0, -0.2, -5.0]))
    req = router.ParticleRenderRequest(dataset_id="far", radius_field="H", mode="projected",
                                       slab=1.0, width=400, height=80, cameras=[_FAR_CAM])
    _, maps, _ = router._frame(router._prepare(entry, req), req, req.cameras[0], 400, 80,
                               req.tone, None)
    column = maps[..., 0].sum(axis=0)
    peaks = _peaks(column, 1e-3 * column.max())
    assert len(peaks) == 2
    # Without perspective: where the table's x puts them, about the centre.
    px_per_unit = 40 / np.tan(np.radians(15))
    expected = 200 + (_FAR_X[:2] - (1e9 + 1)) * px_per_unit
    assert np.abs(np.array(peaks) - expected).max() <= 1.0


def test_an_axis_read_as_fractions_draws_the_same_picture(tmp_path, monkeypatch):
    """The first row (X's reference, 1e7 away) has no Y, so X is read as
    fractions of its range and one scale (X's span, 2) goes on every axis:
    the picture, the sizes and the GPU rows are those of the table without
    that row, whose X keeps scale 1."""
    from app.routers import particle_render as router
    monkeypatch.setattr(router, "_PREPARED", router.OrderedDict())
    x = np.concatenate([[1e9 + 1e7], _FAR_X])
    y = [np.nan, 0.0, 0.3, -0.3]
    entries = {"odd": _far(tmp_path, x, y=y, z=np.full(4, _FAR_Z), name="odd"),
               "plain": _far(tmp_path, x[1:], y=y[1:], z=np.full(3, _FAR_Z), name="plain")}
    req = router.ParticleRenderRequest(dataset_id="odd", radius_field="H", width=400, height=80,
                                       cameras=[_FAR_CAM])
    odd, plain = router._prepare(entries["odd"], req), router._prepare(entries["plain"], req)
    for mode, slab in (("light", None), ("projected", 1.0)):
        r = req.model_copy(update={"mode": mode, "slab": slab})
        a = router._frame(odd, r, r.cameras[0], 400, 80, r.tone, 1.0)[1]
        b = router._frame(plain, r, r.cameras[0], 400, 80, r.tone, 1.0)[1]
        column = np.nan_to_num(a[..., 0]).sum(axis=0)
        assert len(_peaks(column, 1e-3 * column.max())) == 3
        assert np.allclose(np.nan_to_num(a), np.nan_to_num(b), rtol=1e-3,
                           atol=1e-6 * np.nanmax(b))
    assert odd["frame"][1] == pytest.approx(2.0) and plain["frame"][1] == 1.0
    assert np.allclose(odd["radius"] * 2.0, plain["radius"])

    class _S:
        datasets = {"odd": entries["odd"]}
    loop = asyncio.new_event_loop()
    try:
        data = router.ParticleDataRequest(dataset_id="odd", radius_field="H", local_frame=True)
        reply = loop.run_until_complete(router.particle_render_particles(data, token="tok",
                                                                         session=_S()))
    finally:
        loop.close()
    rows = np.frombuffer(reply.body, dtype="<f4").reshape(-1, 8).astype(float)
    origin = np.array([float(v) for v in reply.headers["X-Visivo-Particle-Origin"].split(",")])
    scale = float(reply.headers["X-Visivo-Particle-Scale"])
    assert scale == pytest.approx(2.0)
    assert np.allclose(origin + scale * rows[:, :3],
                       np.stack([_FAR_X, y[1:], np.full(3, _FAR_Z)], axis=1), rtol=0, atol=1e-6)
    assert np.allclose(scale * rows[:, 3], 0.1)


def test_radii_survive_a_scale_beyond_float32(tmp_path):
    """X from -3e38 to 3e38 (both within float32, kept) is read as fractions of
    a span of 6e38, beyond float32: the radius column is divided by that scale
    in float64, so radii stay positive instead of becoming 0 (and every
    particle dropped)."""
    from app.routers import particle_render as router
    x = np.array([-3e38, 0.0, 3e38])
    entry = _far(tmp_path, x, h=1e37, name="huge")
    req = router.ParticleRenderRequest(dataset_id="huge", radius_field="H", cameras=[FRONT_JSON])
    p = router._prepare(entry, req)
    scale = p["frame"][1]
    assert scale > float(np.finfo(np.float32).max)
    assert np.all(np.isfinite(p["radius"])) and np.all(p["radius"] > 0)
    assert np.allclose(p["radius"].astype(float) * scale, 1e37, rtol=1e-5)
