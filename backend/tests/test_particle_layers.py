"""Several particle components in one picture (issue #394): the layer maps,
their composition rules, the black holes' markers, and the routes on a Gadget
snapshot's particle types, an IPAC table and a VBT with a type column."""
from __future__ import annotations

import asyncio
import importlib.util
import io
import json
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("numba")

from app import particle_layers as pl  # noqa: E402
from app.particle_raytrace import Camera  # noqa: E402

TOP = Camera((0.0, 0.0, 10.0), (0.0, 0.0, 0.0), (0.0, 1.0, 0.0), 30.0, True, 2.0)
TOP_JSON = {"position": (0, 0, 10), "focal_point": (0, 0, 0), "view_up": (0, 1, 0),
            "parallel": True, "parallel_scale": 2.0}
W, H = 96, 96


def _blob(seed, n, centre, spread=0.25):
    rng = np.random.default_rng(seed)
    return (rng.normal(0.0, spread, (n, 3)) + np.asarray(centre)).astype(np.float32)


# ── Maps ─────────────────────────────────────────────────────────────────────

def test_the_column_density_holds_the_mass_whatever_the_number_of_particles():
    # The same Plummer-like blob of total mass 1 in 2 000 and 16 000 particles,
    # their sizes shrinking as n^(-1/3): the same map (issue #457's rule), and
    # its integral over the picture is the mass.
    maps = []
    for n in (2000, 16000):
        pos = _blob(1, n, (0, 0, 0))
        h = np.full(n, 0.08 * (2000 / n) ** (1 / 3))
        maps.append(pl.layer_maps(pos, h, np.full(n, 1.0 / n), None, TOP, W, H)[..., 0])
    pixel = (2 * TOP.parallel_scale / H) ** 2
    assert maps[0].sum() * pixel == pytest.approx(1.0, rel=0.01)
    assert maps[1].sum() * pixel == pytest.approx(1.0, rel=0.01)
    assert maps[1].mean() / maps[0].mean() == pytest.approx(1.0, abs=0.02)
    centre = (slice(40, 56), slice(40, 56))
    assert maps[1][centre].mean() / maps[0][centre].mean() == pytest.approx(1.0, abs=0.05)


def test_the_mean_quantity_is_weighted_by_mass():
    pos = np.zeros((2, 3), np.float32)
    m = pl.layer_maps(pos, np.full(2, 0.3), np.array([3.0, 1.0]), np.array([10.0, 50.0]),
                      TOP, W, H)
    lit = m[..., 0] > 0
    assert np.allclose(m[..., 1][lit], (3 * 10 + 50) / 4.0, rtol=1e-4)
    assert np.isnan(m[..., 1][~lit]).all()
    # A particle without a value adds its mass but not to the mean.
    pos3 = np.zeros((3, 3), np.float32)
    m3 = pl.layer_maps(pos3, np.full(3, 0.3), np.array([3.0, 1.0, 5.0]),
                       np.array([10.0, 50.0, np.nan]), TOP, W, H)
    assert np.allclose(m3[..., 1][lit], 20.0, rtol=1e-4)
    assert np.allclose(m3[..., 0][lit], m[..., 0][lit] * 9 / 4, rtol=1e-4)


# ── Composition ──────────────────────────────────────────────────────────────

def _two_layers():
    gas = pl.layer_maps(_blob(2, 3000, (-0.6, 0, 0)), np.full(3000, 0.1),
                        np.full(3000, 1e-3), None, TOP, W, H)
    stars = pl.layer_maps(_blob(3, 1500, (0.6, 0, 0)), np.full(1500, 0.05),
                          np.full(1500, 2e-3), None, TOP, W, H)
    return gas, stars


def _light(maps, colormap, intensity=1.0):
    window = pl.auto_window(maps[..., 0], "log")
    return pl.layer_light(maps, pl.colour_table(colormap), window, "log", intensity), window


def test_emission_layers_add_up():
    gas, stars = _two_layers()
    a, _ = _light(gas, [[0, 0, 0, 0], [1, 1, 0.5, 0.2]])
    b, _ = _light(stars, [[0, 0, 0, 0], [1, 0.8, 0.9, 1.0]], intensity=0.7)
    both = pl.compose([{"light": a, "composite": "emission"},
                       {"light": b, "composite": "emission"}], (H, W))
    alone = [pl.compose([{"light": x, "composite": "emission"}], (H, W)) for x in (a, b)]
    assert np.allclose(both, alone[0] + alone[1], atol=1e-12)
    # In either order.
    flipped = pl.compose([{"light": b, "composite": "emission"},
                          {"light": a, "composite": "emission"}], (H, W))
    assert np.allclose(both, flipped, atol=1e-12)


def test_an_absorption_layer_dims_only_what_lies_behind_it():
    gas, stars = _two_layers()
    # Stars behind the gas, both in the middle: make them overlap.
    stars_here = pl.layer_maps(_blob(3, 1500, (-0.6, 0, 0)), np.full(1500, 0.05),
                               np.full(1500, 2e-3), None, TOP, W, H)
    s, _ = _light(stars_here, "Gray")
    window = pl.auto_window(gas[..., 0], "log")
    tau = pl.optical_depth(gas[..., 0], window, 2.0)
    dark = np.zeros_like(s)
    behind = pl.compose([{"light": s, "composite": "emission"},
                         {"light": dark, "composite": "absorption", "tau": tau}], (H, W))
    in_front = pl.compose([{"light": dark, "composite": "absorption", "tau": tau},
                           {"light": s, "composite": "emission"}], (H, W))
    assert np.allclose(behind, s * np.exp(-tau)[..., None], atol=1e-12)
    assert np.allclose(in_front, s, atol=1e-12)
    thick = tau > 1.0
    assert thick.any() and (behind[thick].sum() < 0.5 * s[thick].sum())
    # Its own light is that of a uniform slab: L (1 - e^-τ) / τ, L where τ → 0.
    glow, _ = _light(gas, "afmhot")
    alone = pl.compose([{"light": glow, "composite": "absorption", "tau": tau}], (H, W))
    with np.errstate(divide="ignore", invalid="ignore"):
        slab = np.where(tau > 0, -np.expm1(-tau) / tau, 1.0)
    assert np.allclose(alone, glow * slab[..., None], atol=1e-12)
    # The depth is the opacity at the top of the window, proportional to the column.
    assert pl.optical_depth(np.array([window[1], window[1] / 4]), window, 2.0) \
        == pytest.approx([2.0, 0.5])


def test_hiding_a_layer_removes_only_its_own_light():
    from app.routers.particle_layers import ParticleLayer, picture, _windows
    gas, stars = _two_layers()
    layers = [ParticleLayer(colormap="afmhot"), ParticleLayer(colormap="Gray", intensity=0.8)]
    windows = _windows([gas, stars], [None, None], layers)
    _, both = picture([gas, stars], layers, windows, (H, W), 1.0)
    hidden = [layers[0], layers[1].model_copy(update={"visible": False})]
    _, gas_only = picture([gas, None], hidden, windows, (H, W), 1.0)
    _, stars_only = picture([None, stars], [layers[0].model_copy(update={"visible": False}),
                                            layers[1]], windows, (H, W), 1.0)
    assert np.allclose(both, gas_only + stars_only, atol=1e-6)
    stars_lit = (stars[..., 0] > 0)
    # Where the stars do not reach, nothing changed.
    assert np.array_equal(both[~stars_lit], gas_only[~stars_lit])
    assert (both[stars_lit] - gas_only[stars_lit]).sum() > 0


def test_a_fixed_window_is_kept_and_a_brighter_layer_looks_brighter():
    gas, _ = _two_layers()
    window = pl.auto_window(gas[..., 0], "log")
    b = pl.brightness(gas[..., 0], window, "log")
    b2 = pl.brightness(2 * gas[..., 0], window, "log")
    assert (b2 >= b).all() and (b2 > b).any()
    assert pl.brightness(np.array([window[0], window[1], 0.0]), window, "log").tolist() \
        == [0.0, 1.0, 0.0]


# ── Markers ──────────────────────────────────────────────────────────────────

def test_black_hole_markers_grow_with_the_log_of_their_mass():
    window = (1e6, 1e9)
    r = pl.marker_radius(np.array([1e5, 1e6, 10 ** 7.5, 1e9, 1e10]), window, 3.0, 15.0)
    assert r.tolist() == pytest.approx([3.0, 3.0, 9.0, 15.0, 15.0])
    assert pl.marker_radius(np.array([5.0, 5.0]), (5.0, 5.0), 3.0, 15.0).tolist() == [15.0, 15.0]
    pos = np.array([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, 20.0]], np.float32)
    marks = pl.place_markers(pos, np.array([1e6, 1e9, 1e9]), TOP, W, H, window, 3.0, 15.0)
    # The one behind the camera is left out; the largest first.
    assert len(marks) == 2 and marks[0, 2] == pytest.approx(15.0)
    assert marks[0, :2] == pytest.approx([W / 2 + 24.0, H / 2], abs=1e-3)
    img = pl.draw_markers(np.zeros((H, W, 3), np.uint8), marks, (0.0, 1.0, 1.0))
    assert img[H // 2, int(W / 2 + 24 + 15), 1] > 200      # on the ring
    assert img[H // 2, int(W / 2 + 24), 1] == 0            # not inside it
    disc = pl.draw_markers(np.zeros((H, W, 3), np.uint8), marks, (1.0, 1.0, 1.0), "disc")
    assert disc[H // 2, int(W / 2 + 24)].min() == 255


def test_presets_are_valid_layers():
    from app.routers.particle_layers import ParticleLayer
    names = set()
    for preset in pl.PRESETS:
        ParticleLayer(**preset["layer"])
        names.add(preset["name"])
    assert {"gas-density-hot", "dark-matter-blue", "stars-white-yellow",
            "black-holes"} <= names


# ── Routes ───────────────────────────────────────────────────────────────────

class _Session:
    def __init__(self, datasets):
        self.datasets = datasets


def _run_task(router, request, session):
    loop = asyncio.new_event_loop()

    async def go():
        created = await router.create_particle_layers_task(request, token="tok", session=session)
        from app.tasks import TASKS
        for _ in range(1200):
            task = TASKS.get(created.task_id)
            if task.status not in ("pending", "running"):
                return task
            await asyncio.sleep(0.05)
        raise AssertionError("render task did not end")

    return loop.run_until_complete(go()), loop


def _gadget(tmp_path):
    spec = importlib.util.spec_from_file_location(
        "generate_demo_data_layers",
        Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py")
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)
    parts = gen.gadget_halo_particles(np.random.default_rng(5))
    files = gen.write_gadget(tmp_path / "snapdir_002" / "snap_002", parts, num_files=1,
                             mass_table=[0.0, gen.GADGET_DM_MASS, 0.0, 0.0, 0.0, 0.0])
    return files[0], parts


def test_a_gadget_snapshot_draws_its_types_together(tmp_path, monkeypatch):
    from PIL import Image

    from app import gadget_snapshot as gs
    from app.routers import particle_layers as router
    from app.routers import particle_render as single
    monkeypatch.delenv(gs.UNITS_ENV, raising=False)
    monkeypatch.setattr(single, "get_exports_dir", lambda: tmp_path / "exports")
    first, parts = _gadget(tmp_path)
    ref = f"{first}{gs.REF_SEP}0"
    session = _Session({"cat_1": {"kind": "catalogue", "format": "gadget", "path": ref,
                                  "paths": [str(first)]}})
    cam = {"position": (500, 500, 2500), "focal_point": (500, 500, 500), "view_up": (0, 1, 0)}
    layers = [{"name": "dm", "gadget_type": 1, "colormap": "Gray"},
              {"name": "gas", "gadget_type": 0, "colormap": "afmhot",
               "quantity_field": "temperature", "quantity_log": True,
               "composite": "absorption", "opacity": 1.0},
              {"name": "stars", "gadget_type": 4, "colormap": "Gray"},
              {"name": "bh", "gadget_type": 5, "kind": "markers"}]
    req = router.ParticleLayersRequest(dataset_id="cat_1", layers=layers, width=96, height=64,
                                       cameras=[cam])
    task, loop = _run_task(router, req, session)
    try:
        assert task.status == "completed", task.error
        result = task.result["layers"]
        assert [r["particles"] for r in result] == [4000, 2000, 500, 2]
        assert [r["weight"] for r in result] == ["mass", "mass", "mass", "bh_mass"]
        assert result[1]["quantity_range"][0] > 0
        bh = np.asarray(parts[5]["BHMA"], dtype=np.float32).astype(np.float64)
        assert result[3]["range"] == pytest.approx([bh.min(), bh.max()])
        run = loop.run_until_complete
        frame = run(single.particle_render_frame(task.task_id, 0, token="tok"))
        assert Image.open(frame.path).size == (96, 64)
        # Styles change without a render: the gas hidden, the stars brighter.
        restyled = [dict(layer) for layer in layers]
        restyled[1]["visible"] = False
        restyled[2]["intensity"] = 3.0
        png = run(router.particle_layers_compose(
            task.task_id, router.ParticleLayersCompose(layers=restyled), token="tok")).body
        assert Image.open(io.BytesIO(png)).size == (96, 64)
        # Another scale, its window measured again for it.
        relinear = [dict(layer) for layer in layers]
        relinear[0]["scale"] = "linear"
        relinear[1]["quantity_log"] = False
        png = run(router.particle_layers_compose(
            task.task_id, router.ParticleLayersCompose(layers=relinear), token="tok")).body
        assert np.asarray(Image.open(io.BytesIO(png))).max() > 0
        moved = [dict(layer) for layer in layers]
        moved[2]["gadget_type"] = 1
        with pytest.raises(router.HTTPException) as err:
            run(router.particle_layers_compose(
                task.task_id, router.ParticleLayersCompose(layers=moved), token="tok"))
        assert err.value.status_code == 409
        from astropy.io import fits
        body = run(router.particle_layers_maps_fits(task.task_id, token="tok")).body
        with fits.open(io.BytesIO(body)) as hdul:
            assert [h.name for h in hdul[1:]] == ["DM", "GAS", "STARS"]
            assert hdul["GAS"].data.shape == (2, 64, 96)
        with pytest.raises(single.HTTPException) as err:
            run(single.particle_render_tone(task.task_id, single.RaytraceTone(), token="tok"))
        assert err.value.status_code == 422
        assert run(single.delete_particle_render(task.task_id, token="tok"))["deleted"]
    finally:
        loop.close()


def test_a_gadget_type_needs_a_snapshot(tmp_path):
    from app.routers import particle_layers as router
    session = _Session({"cat_1": {"kind": "catalogue", "format": "vbt", "path": _vbt(tmp_path)}})
    req = router.ParticleLayersRequest(dataset_id="cat_1", layers=[{"gadget_type": 0}],
                                       cameras=[TOP_JSON])
    with pytest.raises(router.HTTPException) as err:
        asyncio.new_event_loop().run_until_complete(
            router.create_particle_layers_task(req, token="tok", session=session))
    assert err.value.status_code == 422


def _typed_rows(n=4000):
    rng = np.random.default_rng(7)
    ptype = (np.arange(n) % 2).astype(int)
    xyz = rng.normal(0.0, 0.4, (n, 3))
    xyz[:, 0] += np.where(ptype == 0, -0.6, 0.6)
    mass = np.where(ptype == 0, 1.0, 2.0) * 1e-3
    return xyz, ptype, mass


def _ipac(tmp_path):
    from astropy.table import Table
    xyz, ptype, mass = _typed_rows()
    path = tmp_path / "mixed.tbl"
    Table({"x": xyz[:, 0], "y": xyz[:, 1], "z": xyz[:, 2], "ptype": ptype, "mass": mass}
          ).write(path, format="ascii.ipac")
    return {"kind": "catalogue", "format": "ipac", "path": str(path), "paths": [str(path)],
            "coordinate_fields": {"system": "cartesian", "x": "x", "y": "y", "z": "z"}}


def _vbt(tmp_path):
    xyz, ptype, mass = _typed_rows()
    cols = np.vstack([xyz.T, ptype[None, :], mass[None, :]])
    cols.astype("<f4").tofile(tmp_path / "mixed.bin")
    (tmp_path / "mixed.bin.head").write_text(
        f"float\n5\n{xyz.shape[0]}\nlittle\nX\nY\nZ\nptype\nmass\n")
    return str(tmp_path / "mixed.bin.head")


@pytest.mark.parametrize("kind", ["ipac", "vbt"])
def test_a_table_with_a_type_column_draws_each_type(tmp_path, kind):
    from app.routers import particle_layers as router
    entry = (_ipac(tmp_path) if kind == "ipac"
             else {"kind": "catalogue", "format": "vbt", "path": _vbt(tmp_path)})
    session = _Session({"cat_1": entry})
    styles = [{"type_field": "ptype", "type_value": 0, "colormap": "afmhot",
               "range": [1e-3, 1.0]},
              {"type_field": "ptype", "type_value": 1, "colormap": "Gray",
               "range": [1e-3, 1.0], "intensity": 0.5}]
    entries = router._check_entries(session, router.ParticleLayersRequest(
        dataset_id="cat_1", layers=styles, cameras=[TOP_JSON]))
    layers = [router.ParticleLayer(**s) for s in styles]
    prepared = [router.prepare_layer(e, layer) for e, layer in zip(entries, layers)]
    assert [len(p["positions"]) for p in prepared] == [2000, 2000]
    assert [p["weight"] for p in prepared] == ["mass", "mass"]
    assert prepared[1]["mass"] == pytest.approx(2e-3)
    cam = router.RaytraceCamera(**TOP_JSON)
    maps = router._maps(prepared, layers, cam, W, H)
    windows = router._windows(maps, prepared, layers)
    _, both = router.picture(maps, layers, windows, (H, W), 1.0)
    parts = [router.picture([m if i == j else None for j, m in enumerate(maps)], layers,
                            windows, (H, W), 1.0)[1] for i in range(2)]
    assert both.max() > 0
    assert np.allclose(both, parts[0] + parts[1], atol=1e-6)


def test_the_preview_keeps_the_windows_and_reuses_the_maps(tmp_path, monkeypatch):
    from app.routers import particle_layers as router
    session = _Session({"cat_1": {"kind": "catalogue", "format": "vbt", "path": _vbt(tmp_path)}})
    monkeypatch.setattr(router, "_PREPARED", router.OrderedDict())
    monkeypatch.setattr(router, "_MAPS", {"view": None, "layers": {}})
    drawn = []
    real = router.layer_frame
    monkeypatch.setattr(router, "layer_frame", lambda *a: drawn.append(1) or real(*a))
    layers = [{"type_field": "ptype", "type_value": 0, "colormap": "afmhot"},
              {"type_field": "ptype", "type_value": 1, "colormap": "Gray"}]
    loop = asyncio.new_event_loop()
    try:
        def preview(layers, **kw):
            req = router.ParticleLayersPreviewRequest(dataset_id="cat_1", layers=layers,
                                                      width=64, height=48,
                                                      cameras=[TOP_JSON], **kw)
            return loop.run_until_complete(router.particle_layers_preview(
                req, token="tok", session=session))

        first = preview(layers)
        windows = json.loads(first.headers["X-Visivo-Layer-Windows"])
        assert len(windows) == 2 and all(w["range"][1] > w["range"][0] > 0 for w in windows)
        assert len(drawn) == 2
        fixed = [{**layer, "range": w["range"]} for layer, w in zip(layers, windows)]
        restyled = [{**fixed[0], "colormap": "Viridis"}, {**fixed[1], "visible": False}]
        again = preview(restyled)
        assert len(drawn) == 2                           # composed from the same maps
        assert json.loads(again.headers["X-Visivo-Layer-Windows"])[0]["range"] == windows[0]["range"]
        preview(fixed)
        assert len(drawn) == 2                           # shown again: not drawn again
        preview(fixed, max_particles=1000)
        assert len(drawn) == 4                           # a draft: drawn again
    finally:
        loop.close()


def test_a_layer_that_cannot_be_drawn_names_itself(tmp_path, monkeypatch):
    from app.routers import particle_layers as router
    from app.routers import particle_render as single
    monkeypatch.setattr(single, "get_exports_dir", lambda: tmp_path / "exports")
    session = _Session({"cat_1": {"kind": "catalogue", "format": "vbt", "path": _vbt(tmp_path)}})
    req = router.ParticleLayersRequest(
        dataset_id="cat_1", width=32, height=32, cameras=[TOP_JSON],
        layers=[{"name": "gas"}, {"name": "hot", "quantity_field": "nope"}])
    task, loop = _run_task(router, req, session)
    loop.close()
    assert task.status == "failed" and "hot" in task.error and "nope" in task.error
    with pytest.raises(ValueError):
        router.ParticleLayer(range=(0.0, 1.0))           # a log window starts above 0
    with pytest.raises(ValueError):
        router.ParticleLayer(type_field="ptype")
