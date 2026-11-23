"""Projected particle maps in physical units (issue #393): the surface
density against analytic truths, the mass it holds, a mass-weighted mean,
and the FITS header of a Gadget snapshot's map (h = 0.6774, z = 0.5)."""
from __future__ import annotations

import asyncio
import io
import math
import sys
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("numba")

from app.particle_raytrace import Camera, projected_maps  # noqa: E402
from app.projected_units import physical_planes  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_gadget_snapshot import G, _snapshot  # noqa: E402


def _top(half: float) -> Camera:
    return Camera((0.0, 0.0, 50.0), (0.0, 0.0, 0.0), (0.0, 1.0, 0.0), 30.0, True, half)


def _plummer(n: int, a: float, rng, rmax: float | None = None) -> np.ndarray:
    x = rng.random(n)
    if rmax is not None:   # truncated: M(<r) / M = r^3 / (r^2 + a^2)^(3/2)
        x *= rmax ** 3 / (rmax ** 2 + a ** 2) ** 1.5
    r = a / np.sqrt(x ** (-2.0 / 3.0) - 1.0)
    d = rng.normal(size=(n, 3))
    return (d / np.linalg.norm(d, axis=1)[:, None] * r[:, None]).astype(np.float32)


def _surface_density(maps, camera, height, mass_per_weight=1.0):
    units = {"density_unit": "Msun/kpc^2", "density_factor": mass_per_weight,
             "value_factor": 1.0, "kpc_per_unit": 1.0}
    return physical_planes(maps, units, camera, height)


def test_a_plummer_sphere_has_its_analytic_surface_density():
    """Sigma(R) = M a^2 / (pi (a^2 + R^2)^2), as the mass between two radii
    over the ring's area, within 3 % from 4 h to 3 a."""
    rng = np.random.default_rng(1)
    n, a, total, h = 1_000_000, 1.0, 1.0e12, 0.05
    pos = _plummer(n, a, rng)
    cam = _top(4.0 * a)
    maps = projected_maps(pos, np.full(n, h), np.ones(n, np.float32), None, cam, 800, 800)
    sigma, _ = _surface_density(maps, cam, 800, total / n)
    centres = (np.arange(800) + 0.5) * 0.01 - 4.0
    r = np.hypot(*np.meshgrid(centres, centres))
    edges = np.array([0.2, 0.3, 0.45, 0.65, 1.0, 1.4, 2.0, 3.0]) * a
    within = total * edges ** 2 / (a ** 2 + edges ** 2)
    for lo, hi, m_lo, m_hi in zip(edges[:-1], edges[1:], within[:-1], within[1:]):
        ring = (r >= lo) & (r < hi)
        truth = (m_hi - m_lo) / (math.pi * (hi ** 2 - lo ** 2))
        assert sigma[ring].mean() == pytest.approx(truth, rel=0.03), (lo, hi)


def test_the_map_holds_the_mass_of_its_particles():
    rng = np.random.default_rng(2)
    n = 72_000
    pos = _plummer(n, 1.0, rng, rmax=2.0)
    mass = rng.lognormal(0.0, 0.5, n).astype(np.float32)
    radius = rng.uniform(0.02, 0.15, n)
    for size in (64, 512):
        cam = _top(2.0 + 2.5 * radius.max() + 0.1)
        maps = projected_maps(pos, radius, mass, None, cam, size, size)
        sigma, _ = _surface_density(maps, cam, size)
        pixel = 2.0 * cam.parallel_scale / size
        assert sigma.sum(dtype=np.float64) * pixel ** 2 == pytest.approx(
            float(mass.sum(dtype=np.float64)), rel=1e-3)


def test_the_mass_weighted_temperature_of_an_isothermal_blob():
    rng = np.random.default_rng(3)
    n = 50_000
    pos = rng.normal(0.0, 1.0, (n, 3)).astype(np.float32)
    mass = rng.lognormal(0.0, 1.0, n).astype(np.float32)
    t0 = np.float32(1.23e7)
    temperature = np.full(n, t0, np.float32)
    temperature[::10] = np.nan         # no value: left out of the mean, not 0
    maps = projected_maps(pos, np.full(n, 0.1), mass, temperature, _top(4.0), 256, 256)
    lit = maps[..., 0] > 0
    valued = np.isfinite(maps[..., 1])
    assert lit.sum() > 10_000 and valued.sum() > 0.98 * lit.sum()
    assert np.allclose(maps[..., 1][valued], t0, rtol=2e-5)
    assert not valued[~lit].any()


# ── A Gadget snapshot: h = 0.6774 at z = 0.5 ────────────────────────────────

H, A = 0.6774, 2.0 / 3.0


def _cosmological(tmp_path, n=20_000):
    from app import gadget_snapshot as gs
    pos = np.random.default_rng(4).normal(500.0, 40.0, (n, 3))
    files = G.write_gadget(tmp_path / "snap_005", {1: {"POS": pos, "VEL": np.zeros((n, 3)),
                                                      "ID": np.arange(1, n + 1)}},
                           mass_table=[0.0, 1.0, 0.0, 0.0, 0.0, 0.0], box=1000.0,
                           time=A, redshift=0.5, hubble=H)
    return {"kind": "catalogue", "format": "gadget", "path": f"{files[0]}{gs.REF_SEP}1"}, n


def test_a_gadget_map_is_written_in_physical_units(tmp_path, monkeypatch):
    from astropy.io import fits

    from app import gadget_snapshot as gs
    from app.routers import particle_render as router
    from app.tasks import TASKS
    monkeypatch.delenv(gs.UNITS_ENV, raising=False)
    entry, n = _cosmological(tmp_path)

    class _Session:
        datasets = {"g": entry}

    cam = {"position": (500, 500, 2000), "focal_point": (500, 500, 500), "view_up": (0, 1, 0),
           "parallel": True, "parallel_scale": 250.0}
    req = router.ParticleRenderRequest(dataset_id="g", mode="projected", width=500, height=500,
                                       slab=300.0, cameras=[cam])
    loop = asyncio.new_event_loop()
    try:
        async def run():
            created = await router.create_particle_render_task(req, token="tok",
                                                               session=_Session())
            for _ in range(1200):
                task = TASKS.get(created.task_id)
                if task.status not in ("pending", "running"):
                    return task
                await asyncio.sleep(0.05)
            raise AssertionError("render task did not end")

        task = loop.run_until_complete(run())
        assert task.status == "completed", task.error
        units = task.result["map_units"]
        assert units["density_unit"] == "Msun/kpc^2" and units["weighting"] == "mass"
        assert units["comoving"] and units["redshift"] == pytest.approx(0.5)
        body = loop.run_until_complete(router.particle_render_light_fits(task.task_id,
                                                                         token="tok")).body
        with fits.open(io.BytesIO(body)) as hdul:
            head = hdul[0].header
            assert head["BUNIT"] == "solMass kpc-2"
            # 1 comoving kpc/h a pixel: a / h physical kpc, 1 / h comoving kpc.
            assert head["CDELT1"] == pytest.approx(A / H, rel=1e-6)
            assert head["CDELT2"] == pytest.approx(A / H, rel=1e-6)
            assert head["CUNIT1"] == "kpc"
            assert head["CDELTCOM"] == pytest.approx(1.0 / H, rel=1e-6)
            assert head["REDSHIFT"] == pytest.approx(0.5)
            assert head["HUBBLE"] == pytest.approx(H)
            assert head["SCALEFAC"] == pytest.approx(A)
            assert head["COMOVING"] is True
            assert head["DEPTH"] == pytest.approx(300.0 * A / H, rel=1e-6)
            assert head["PROJAXIS"] == "0 0 -1"
            # Every particle lies in the slab and the view: their mass, 1e10/h
            # Msun each, is the map's sum over its pixels' area.
            held = hdul[0].data.sum(dtype=np.float64) * head["CDELT1"] * head["CDELT2"]
            assert held == pytest.approx(n * 1e10 / H, rel=5e-3)
        loop.run_until_complete(router.delete_particle_render(task.task_id, token="tok"))
        monkeypatch.setattr(router, "_PREPARED", router.OrderedDict())
        preview = loop.run_until_complete(router.particle_render_preview(
            router.ParticlePreviewRequest(dataset_id="g", mode="projected", width=64,
                                          height=64, cameras=[cam]),
            token="tok", session=_Session()))
        assert preview.headers["X-Visivo-Map-Unit"] == "Msun/kpc^2"
        assert preview.headers["X-Visivo-Map-Value-Unit"] == "none"
    finally:
        loop.close()


def test_a_gas_map_states_its_mean_in_physical_units(tmp_path, monkeypatch):
    from app import gadget_snapshot as gs
    from app.projected_units import map_units
    monkeypatch.delenv(gs.UNITS_ENV, raising=False)
    files, _ = _snapshot(tmp_path)
    gas = gs.resolve(f"{files[0]}{gs.REF_SEP}0")
    common = dict(axes=("x", "y", "z"), weight="mass", power=1.0, clipped=0, light_unit=2.0,
                  velocity=("vx", "vy", "vz"))
    t = map_units(gas, colour_by="colour", value_field="temperature", **common)
    assert t["value_unit"] == "K" and t["value_factor"] == 1.0
    assert t["density_unit"] == "Msun/kpc^2"
    physical = gas.physical()
    assert t["density_factor"] == pytest.approx(2.0 * physical["mass"]["factor"])
    assert t["kpc_per_unit"] == pytest.approx(physical["x"]["factor"])
    k = map_units(gas, colour_by="colour", value_field="entropy", **common)
    assert k["value_unit"] == "keV cm^2"
    v = map_units(gas, colour_by="velocity_dispersion", value_field=None, **common)
    assert v["value_unit"] == "km/s" and v["value_factor"] == pytest.approx(
        physical["vx"]["factor"])
    # A light raised to a power, or capped, is no mass; a table without
    # units stays relative.
    assert map_units(gas, colour_by="density", value_field=None,
                     **{**common, "power": 2.0})["density_unit"] == "relative"
    assert map_units(None, colour_by="density", value_field=None,
                     **common)["density_unit"] == "relative"
