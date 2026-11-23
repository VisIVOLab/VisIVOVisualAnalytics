"""Smoothing lengths of a cut and of a periodic box, and the light of a table
whose intensity spans tens of decades (issue #414, items 4 and 5).

Every particle format reaches the renderer through app/point_table.py, so
the same checks run on a VBT, a CSV catalogue and a Gadget particle type."""
from __future__ import annotations

import asyncio
import importlib.util
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

pytest.importorskip("numba")
pytest.importorskip("scipy")

from app.particle_raytrace import (LIGHT_CEILING_DECADES, normalised_light,  # noqa: E402
                                   smoothing_lengths)
from app.routers import particle_render as router  # noqa: E402

CAM = {"position": (5, 5, 40), "focal_point": (5, 5, 5), "view_up": (0, 1, 0)}
L = 10.0


def _uniform(n=40_000, seed=7):
    return np.random.default_rng(seed).uniform(0.0, L, (n, 3))


def _vbt(tmp_path, columns: dict, name="p"):
    names = list(columns)
    n = len(next(iter(columns.values())))
    np.stack([np.asarray(columns[c], float) for c in names]).astype("<f8").tofile(
        tmp_path / f"{name}.bin")
    (tmp_path / f"{name}.bin.head").write_text(
        f"double\n{len(names)}\n{n}\nlittle\n" + "\n".join(names) + "\n")
    return {"kind": "catalogue", "format": "vbt", "path": str(tmp_path / f"{name}.bin.head")}


def _csv(tmp_path, columns: dict, name="p"):
    path = tmp_path / f"{name}.csv"
    pd.DataFrame(columns).to_csv(path, index=False)
    return {"kind": "catalogue", "format": "csv", "path": str(path),
            "coordinate_fields": {"system": "cartesian", "x": "X", "y": "Y", "z": "Z"}}


def _req(**kw):
    return router.ParticleRenderRequest(dataset_id="d", **{"cameras": [CAM], **kw})


def _box(lo, hi, fields=("X", "Y", "Z")):
    return {"box": {"fields": list(fields), "min": list(lo), "max": list(hi)}, "op": "inside"}


# ── Periodic box ─────────────────────────────────────────────────────────────

def test_in_a_periodic_box_a_face_is_like_the_interior():
    """Uniform particles: without the period those within a kernel of a face
    have fewer neighbours and larger lengths; with it they match the interior
    within 2 %."""
    pts = _uniform()
    face = np.min(np.minimum(pts, L - pts), axis=1) < 0.3
    inner = np.min(np.minimum(pts, L - pts), axis=1) > 2.0
    open_box = smoothing_lengths(pts, 32)
    periodic = smoothing_lengths(pts, 32, boxsize=L)
    assert open_box[face].mean() / open_box[inner].mean() > 1.08
    assert periodic[face].mean() / periodic[inner].mean() == pytest.approx(1.0, abs=0.02)
    assert np.allclose(periodic[inner], open_box[inner])


def test_positions_outside_the_period_are_wrapped():
    pts = _uniform(5000)
    shifted = pts + np.array([L, -3 * L, 0.0])
    assert np.allclose(smoothing_lengths(shifted, 16, boxsize=[L, L, L]),
                       smoothing_lengths(pts, 16, boxsize=[L, L, L]), rtol=1e-5)


def test_the_request_period_reaches_a_vbt(tmp_path):
    pts = _uniform(20_000)
    entry = _vbt(tmp_path, {"X": pts[:, 0], "Y": pts[:, 1], "Z": pts[:, 2]})
    wrapped = router._prepare(entry, _req(neighbours=16, period=L))["radius"]
    plain = router._prepare(entry, _req(neighbours=16))["radius"]
    assert np.allclose(wrapped, smoothing_lengths(pts, 16, boxsize=L), rtol=1e-4)
    assert np.allclose(plain, smoothing_lengths(pts, 16), rtol=1e-4)


# ── A cut keeps the neighbours beyond its faces ─────────────────────────────

@pytest.mark.parametrize("fmt", ["vbt", "csv"])
@pytest.mark.parametrize("cut", ["box", "comparison", "sphere"])
def test_a_cut_has_the_lengths_of_the_whole_table(tmp_path, fmt, cut):
    """The lengths of a sub-box (a box, comparisons, a sphere) are those the
    whole table gives the same rows, within 1 %."""
    pts = _uniform()
    cols = {"X": pts[:, 0], "Y": pts[:, 1], "Z": pts[:, 2]}
    entry = (_vbt if fmt == "vbt" else _csv)(tmp_path, cols)
    if cut == "box":
        filters = [_box((3, 3, 3), (6, 6, 6))]
        inside = np.all((pts >= 3) & (pts <= 6), axis=1)
    elif cut == "comparison":
        filters = [{"field": "X", "op": ">", "value": 4.0},
                   {"field": "Y", "op": "<=", "value": 2.5}]
        inside = (pts[:, 0] > 4.0) & (pts[:, 1] <= 2.5)
    else:
        filters = [{"sphere": {"fields": ["X", "Y", "Z"], "center": [5, 5, 5], "radius": 2.0},
                    "op": "inside"}]
        inside = np.sum((pts - 5.0) ** 2, axis=1) <= 4.0
    whole = smoothing_lengths(pts, 32)
    cut_h = router._prepare(entry, _req(filters=filters))["radius"]
    assert len(cut_h) == inside.sum()
    assert np.abs(cut_h / whole[inside] - 1.0).max() < 0.01
    # The rows near the cut's faces are what changed: the cut alone gave them
    # more than 1 % larger lengths.
    alone = smoothing_lengths(pts[inside], 32)
    assert np.abs(alone / whole[inside] - 1.0).max() > 0.05


def test_a_cut_keeps_its_population(tmp_path):
    """A filter on another column chooses the population the neighbours come
    from; the position filter only cuts it."""
    pts = _uniform()
    kind = np.random.default_rng(1).integers(0, 2, len(pts)).astype(float)
    entry = _vbt(tmp_path, {"X": pts[:, 0], "Y": pts[:, 1], "Z": pts[:, 2], "K": kind})
    gas = kind == 1
    inside = gas & np.all((pts >= 3) & (pts <= 6), axis=1)
    got = router._prepare(entry, _req(filters=[{"field": "K", "op": "=", "value": 1},
                                               _box((3, 3, 3), (6, 6, 6))]))["radius"]
    assert np.abs(got / smoothing_lengths(pts[gas], 32)[inside[gas]] - 1.0).max() < 0.01


def test_a_cut_of_a_periodic_box_reaches_across_the_face(tmp_path):
    """A slab at a face of a periodic box: its lengths are the whole periodic
    box's, its neighbours found past the opposite face."""
    pts = _uniform()
    entry = _vbt(tmp_path, {"X": pts[:, 0], "Y": pts[:, 1], "Z": pts[:, 2]})
    inside = pts[:, 0] < 1.0
    got = router._prepare(entry, _req(period=L, filters=[{"field": "X", "op": "<",
                                                          "value": 1.0}]))["radius"]
    whole = smoothing_lengths(pts, 32, boxsize=L)
    assert np.abs(got / whole[inside] - 1.0).max() < 0.01


def test_a_sparse_particle_at_a_face_is_searched_again():
    """A particle whose neighbours lie further than the first margin (a lone
    one in an empty corner of the cut) still gets the whole table's length."""
    rng = np.random.default_rng(2)
    dense = rng.uniform(0.0, 1.0, (20_000, 3))
    far = rng.uniform(0.0, 1.0, (200, 3)) * [4, 1, 1] + [3.0, 0.0, 0.0]
    pts = np.vstack([dense, far, [[2.0, 0.5, 0.5]]])
    sel = np.flatnonzero(pts[:, 0] <= 2.0)
    whole = smoothing_lengths(pts, 32)

    def region(lo, hi):
        ok = np.all((pts >= lo) & (pts <= hi), axis=1)
        return pts[ok]

    got = smoothing_lengths(pts[sel], 32, region=region)
    assert np.allclose(got, whole[sel], rtol=1e-6)


def test_a_point_cut_grows_its_margin(tmp_path):
    """A cut down to one particle (comparisons equal on all three axes, a
    zero-extent bounding box) still gets the whole table's length."""
    pts = _uniform(5000)
    entry = _vbt(tmp_path, {"X": pts[:, 0], "Y": pts[:, 1], "Z": pts[:, 2]})
    target = pts[123]
    got = router._prepare(entry, _req(filters=[
        {"field": a, "op": "=", "value": float(v)} for a, v in zip("XYZ", target)]))["radius"]
    assert len(got) == 1
    assert got[0] == pytest.approx(smoothing_lengths(pts, 32)[123], rel=1e-5)


def test_a_cut_across_a_periodic_face_reads_only_its_neighbourhood():
    """A slab astride the face x = 0 is one short interval round the box: the
    search reads a thin region about it, not the whole box."""
    pts = _uniform()
    sel = np.flatnonzero((pts[:, 0] < 0.5) | (pts[:, 0] > L - 0.5))
    asked = []

    def region(lo, hi):
        asked.append(float(hi[0] - lo[0]))
        shifted = np.mod(pts[:, 0] - lo[0], L) + lo[0]
        ok = (shifted <= hi[0]) & np.all((pts[:, 1:] >= lo[1:]) & (pts[:, 1:] <= hi[1:]), axis=1)
        return np.column_stack([shifted[ok], pts[ok, 1:]])

    got = smoothing_lengths(pts[sel], 32, boxsize=[L, 0.0, 0.0], region=region)
    whole = smoothing_lengths(pts, 32, boxsize=[L, 0.0, 0.0])
    assert np.allclose(got, whole[sel], rtol=1e-6)
    assert asked[0] < 0.5 * L


def test_the_intensity_does_not_change_the_neighbours(tmp_path):
    """Rows of zero intensity are not drawn but are still neighbours: the
    drawn rows' lengths are those of all positions."""
    pts = _uniform(20_000)
    sfr = np.where(np.random.default_rng(8).random(len(pts)) < 0.5, 0.0, 1.0)
    entry = _vbt(tmp_path, {"X": pts[:, 0], "Y": pts[:, 1], "Z": pts[:, 2], "SFR": sfr})
    got = router._prepare(entry, _req(intensity_field="SFR"))
    assert got["stats"]["dropped_rows"] == int((sfr == 0).sum())
    assert np.allclose(got["radius"], smoothing_lengths(pts, 32)[sfr > 0], rtol=1e-4)


def test_a_box_over_position_and_another_column_is_split():
    position, other = router._split_filters(
        [{"box": {"fields": ["X", "T", "Y"], "min": [0, 1, 2], "max": [3, 4, 5]},
          "op": "inside"},
         {"field": "Z", "op": "<", "value": 1.0}, {"field": "T", "op": ">", "value": 2.0}],
        ("X", "Y", "Z"))
    assert position == [{"box": {"fields": ["X", "Y"], "min": [0, 2], "max": [3, 5]},
                         "op": "inside"}, {"field": "Z", "op": "<", "value": 1.0}]
    assert other == [{"box": {"fields": ["T"], "min": [1], "max": [4]}, "op": "inside"},
                     {"field": "T", "op": ">", "value": 2.0}]


def _moving(tmp_path, n=20_000):
    """A uniform box whose velocities vary along x (a shear, sigma depends
    on which neighbours a particle has) plus noise."""
    pts = _uniform(n)
    rng = np.random.default_rng(12)
    vel = np.column_stack([np.sin(2 * np.pi * pts[:, 0] / L) * 100.0,
                           rng.normal(0, 30, n), rng.normal(0, 30, n)])
    entry = _vbt(tmp_path, {"X": pts[:, 0], "Y": pts[:, 1], "Z": pts[:, 2],
                            "VX": vel[:, 0], "VY": vel[:, 1], "VZ": vel[:, 2],
                            "S": (np.arange(n) % 3 != 0).astype(float)})
    return pts, vel, entry


def test_the_intensity_does_not_change_the_velocity_dispersion(tmp_path):
    """Rows of zero intensity are not drawn but are still neighbours, with or
    without a position filter."""
    from app.particle_raytrace import velocity_dispersion
    pts, vel, entry = _moving(tmp_path)
    drawn = np.arange(len(pts)) % 3 != 0
    whole = velocity_dispersion(pts, vel, 32)[drawn]
    assert np.allclose(_sigma(entry, intensity_field="S"), whole, rtol=1e-4)
    everything = [_box((-1, -1, -1), (L + 1, L + 1, L + 1))]
    assert np.allclose(_sigma(entry, intensity_field="S", filters=everything), whole, rtol=1e-4)


def _sigma(entry, **kw):
    return router._prepare(entry, _req(mode="projected", colour_by="velocity_dispersion",
                                       **kw))["value"]


@pytest.mark.parametrize("cut", ["box", "sphere"])
def test_a_cut_has_the_velocity_dispersion_of_the_whole_table(tmp_path, cut):
    from app.particle_raytrace import velocity_dispersion
    pts, vel, entry = _moving(tmp_path)
    if cut == "box":
        filters = [_box((3, 3, 3), (6, 6, 6))]
        inside = np.all((pts >= 3) & (pts <= 6), axis=1)
    else:
        filters = [{"sphere": {"fields": ["X", "Y", "Z"], "center": [5, 5, 5], "radius": 2.0},
                    "op": "inside"}]
        inside = np.sum((pts - 5.0) ** 2, axis=1) <= 4.0
    whole = velocity_dispersion(pts, vel, 32)
    got = _sigma(entry, filters=filters)
    assert np.abs(got / whole[inside] - 1.0).max() < 0.01
    alone = velocity_dispersion(pts[inside], vel[inside], 32)
    assert np.abs(alone / whole[inside] - 1.0).max() > 0.05


def test_the_velocity_dispersion_wraps_round_a_periodic_box(tmp_path):
    from app.particle_raytrace import velocity_dispersion
    pts, vel, entry = _moving(tmp_path)
    periodic = velocity_dispersion(pts, vel, 32, boxsize=L)
    assert np.allclose(_sigma(entry, period=L), periodic, rtol=1e-4)
    assert not np.allclose(velocity_dispersion(pts, vel, 32), periodic, rtol=1e-3)
    inside = pts[:, 0] < 1.0
    got = _sigma(entry, period=L, filters=[{"field": "X", "op": "<", "value": 1.0}])
    assert np.abs(got / periodic[inside] - 1.0).max() < 0.01


# ── A Gadget particle type: its BoxSize is the period ───────────────────────

def _generator():
    path = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"
    spec = importlib.util.spec_from_file_location("generate_demo_data_414", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_a_gadget_box_wraps_by_itself(tmp_path, monkeypatch):
    from app import gadget_snapshot as gs
    monkeypatch.delenv(gs.UNITS_ENV, raising=False)
    G = _generator()
    n, box = 20_000, 1000.0
    pos = np.random.default_rng(9).uniform(0.0, box, (n, 3))
    files = G.write_gadget(tmp_path / "snap_000", {1: {"POS": pos, "VEL": np.zeros((n, 3)),
                                                      "ID": np.arange(1, n + 1)}},
                           mass_table=[0.0, 1.0, 0.0, 0.0, 0.0, 0.0], box=box)
    entry = {"kind": "catalogue", "format": "gadget", "path": f"{files[0]}{gs.REF_SEP}1"}
    stored = pos.astype(np.float32).astype(np.float64)
    whole = smoothing_lengths(stored, 32, boxsize=box)
    assert np.allclose(router._prepare(entry, _req())["radius"], whole, rtol=1e-4)
    inside = stored[:, 2] > 900.0
    cut = router._prepare(entry, _req(filters=[{"field": "z", "op": ">", "value": 900.0}]))
    assert np.abs(cut["radius"] / whole[inside] - 1.0).max() < 0.01
    # period 0: no wrapping, as for a zoom.
    assert not np.allclose(router._prepare(entry, _req(period=0))["radius"], whole, rtol=1e-3)


# ── Log light ────────────────────────────────────────────────────────────────

def test_the_light_is_set_by_a_percentile_and_capped():
    logs = np.concatenate([np.random.default_rng(3).normal(-29.0, 1.0, 10_000),
                           np.full(20, 1.1), [-np.inf] * 7, [np.nan] * 3])
    light, usable, stats = normalised_light(logs, log10=True)
    assert stats["dropped_rows"] == 10 and usable.sum() == 10_020
    assert stats["clipped_rows"] == 20
    assert stats["light_span_decades"] > 30
    assert np.percentile(light[:10_000], 99.5) == pytest.approx(1.0, rel=1e-3)
    assert light.max() == pytest.approx(10.0 ** LIGHT_CEILING_DECADES)
    # Every gas row keeps a light (the largest value as reference left them
    # 30 decades down, to nothing once raised to a power).
    assert (light[:10_000] > 1e-12).all()
    linear, kept, lin_stats = normalised_light(np.array([0.0, -1.0, 2.0, 4.0, np.inf]))
    assert lin_stats["dropped_rows"] == 3 and kept.tolist() == [False, False, True, True, False]


@pytest.mark.parametrize("power", [1.0, 2.0])
def test_a_table_with_many_stars_keeps_its_gas(power):
    """As in the demo IPAC snapshot, 14 % of the rows are stars 30 decades
    above the gas: the 99.5th percentile of all rows is a star's, so the gas
    sets the reference (the larger side of the gap) and every gas row keeps
    a light in float32, even squared."""
    rng = np.random.default_rng(4)
    gas, stars = rng.normal(-29.0, 1.5, 8600), rng.normal(1.1, 0.01, 1400)
    light, usable, stats = normalised_light(np.concatenate([gas, stars]), log10=True,
                                            power=power)
    assert usable.all() and stats["dropped_rows"] == 0
    assert stats["clipped_rows"] == 1400
    assert np.percentile(light[:8600], 99.5) == pytest.approx(1.0, rel=1e-3)
    assert (light[:8600] > 0).all()


def test_a_single_population_is_not_capped():
    """A gas whose densest rows lie 3 decades above the 99.5th percentile
    keeps them: its contrast is the tone curve's, not a cap's."""
    rng = np.random.default_rng(6)
    logs = np.concatenate([rng.normal(-27.0, 0.5, 10_000), np.linspace(-26, -23, 20)])
    light, _, stats = normalised_light(logs, log10=True, power=2.0)
    assert stats["clipped_rows"] == 0
    assert light.max() == pytest.approx(10.0 ** (2 * (-23 - np.percentile(logs, 99.5))),
                                        rel=1e-6)


def test_the_light_drops_rows_that_vanish_in_float32():
    """A row 60 decades below the reference is 0 in float32: left out and
    counted, not drawn as nothing."""
    logs = np.concatenate([np.zeros(1000), [-60.0]])
    light, usable, stats = normalised_light(logs, log10=True)
    assert stats["dropped_rows"] == 1 and not usable[-1] and (light > 0).all()


def _mixed(tmp_path, star_rows=20):
    """Gas in a cube, its log density near -29, and a few star rows 30
    decades above, in one corner; 7 rows of log 0 (-inf) and 3 NaN."""
    rng = np.random.default_rng(5)
    gas = rng.uniform(0.0, L, (20_000, 3))
    stars = rng.uniform(8.5, 9.5, (star_rows, 3))
    blank = rng.uniform(0.0, L, (10, 3))
    pts = np.vstack([gas, stars, blank])
    logd = np.concatenate([rng.normal(-29.0, 0.3, len(gas)), np.full(star_rows, 1.1),
                           [-np.inf] * 7, [np.nan] * 3])
    return _vbt(tmp_path, {"X": pts[:, 0], "Y": pts[:, 1], "Z": pts[:, 2], "LogD": logd,
                           "H": np.full(len(pts), 0.15)})


def test_a_few_extreme_rows_do_not_take_the_light(tmp_path):
    """The stars' corner holds a small share of the picture's light: with the
    largest value as reference it held nearly all of it."""
    entry = _mixed(tmp_path)
    req = _req(intensity_field="LogD", intensity_log10=True, radius_field="H",
               mode="projected", width=200, height=200,
               cameras=[{**CAM, "parallel": True, "parallel_scale": 5.0}])
    particles = router._prepare(entry, req)
    assert particles["stats"]["dropped_rows"] == 10
    assert particles["stats"]["clipped_rows"] == 20
    _, maps, _ = router._frame(particles, req, req.cameras[0], 200, 200, req.tone, None)
    density = maps[..., 0]
    # The corner the stars project on: x, y in [8.5, 9.5] (pixels 170-190,
    # rows counted from the top).
    corner = density[10:30, 170:190].sum()
    assert corner / density.sum() < 0.5
    gas_only = density.copy()
    gas_only[5:35, 165:195] = 0.0
    assert (gas_only[gas_only > 0].size > 0.8 * gas_only.size)


def test_the_dropped_rows_reach_the_client(tmp_path, monkeypatch):
    from app.tasks import TASKS
    monkeypatch.setattr(router, "_PREPARED", router.OrderedDict())
    entry = _mixed(tmp_path)

    class _Session:
        datasets = {"cat_1": entry}

    loop = asyncio.new_event_loop()
    try:
        common = dict(dataset_id="cat_1", intensity_field="LogD", intensity_log10=True,
                      radius_field="H")
        reply = loop.run_until_complete(router.particle_render_preview(
            router.ParticlePreviewRequest(**common, width=64, height=64, cameras=[CAM]),
            token="tok", session=_Session()))
        assert reply.headers["X-Visivo-Particle-Dropped"] == "10"
        assert reply.headers["X-Visivo-Particle-Clipped"] == "20"
        reply = loop.run_until_complete(router.particle_render_particles(
            router.ParticleDataRequest(**common), token="tok", session=_Session()))
        assert reply.headers["X-Visivo-Particle-Dropped"] == "10"
        assert int(reply.headers["X-Visivo-Particle-Count"]) == 20_020

        async def run():
            created = await router.create_particle_render_task(
                router.ParticleRenderRequest(**common, width=32, height=32, cameras=[CAM]),
                token="tok", session=_Session())
            for _ in range(1200):
                task = TASKS.get(created.task_id)
                if task.status not in ("pending", "running"):
                    return task
                await asyncio.sleep(0.05)
            raise AssertionError("render task did not end")

        task = loop.run_until_complete(run())
        assert task.status == "completed", task.error
        assert task.result["dropped_rows"] == 10 and task.result["clipped_rows"] == 20
        assert task.result["light_span_decades"] > 30
        loop.run_until_complete(router.delete_particle_render(task.task_id, token="tok"))
    finally:
        loop.close()
