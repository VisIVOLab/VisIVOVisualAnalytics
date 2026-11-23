"""Radial profiles, phase diagrams and the shrinking-sphere centre on
synthetic particle sets with known answers (issues #426, #414).

The truths and tolerances were set from an independent reference run over
many seeds; every tolerance is at least five times the measured scatter.
"""
from __future__ import annotations

import threading

import numpy as np
import pytest

from app.particle_profiles import (
    ProfileCancelled, phase_diagram, radial_profile, shrinking_sphere_centre, Geometry,
)

POS = ["x", "y", "z"]
VEL = ["vx", "vy", "vz"]


def _chunks(table: dict, size: int | None = None):
    n = len(next(iter(table.values())))
    size = size or n

    def factory():
        for start in range(0, n, size):
            stop = min(n, start + size)
            yield {k: v[start:stop] for k, v in table.items()}, stop / n
    return factory


def _directions(rng, n):
    cos_t = rng.uniform(-1.0, 1.0, n)
    phi = rng.uniform(0.0, 2.0 * np.pi, n)
    sin_t = np.sqrt(1.0 - cos_t ** 2)
    return np.column_stack([sin_t * np.cos(phi), sin_t * np.sin(phi), cos_t])


def _sis(n=300_000, radius=1.0, centre=(3.0, -2.0, 0.5), seed=1, sigma=None):
    rng = np.random.default_rng(seed)
    r = radius * rng.uniform(0.0, 1.0, n)
    unit = _directions(rng, n)
    pos = np.asarray(centre) + r[:, None] * unit
    table = {"x": pos[:, 0], "y": pos[:, 1], "z": pos[:, 2], "mass": np.full(n, 1.0 / n)}
    if sigma is not None:
        v = rng.normal(0.0, sigma, (n, 3))
        table.update(vx=v[:, 0], vy=v[:, 1], vz=v[:, 2])
    return table, r, unit


def _slope(profile, r_max):
    r_lo, r_hi = np.array(profile["r_lo"]), np.array(profile["r_hi"])
    rho, n = np.array(profile["density"]), np.array(profile["count"])
    use = (r_lo >= 0.01 * r_max) & (r_hi <= 0.5 * r_max) & (n > 0)
    x, y = np.log(np.sqrt(r_lo * r_hi))[use], np.log(rho[use])
    return np.polyfit(x, y, 1, w=np.sqrt(n[use]))[0]


def test_singular_isothermal_sphere_has_density_slope_minus_two():
    table, _, _ = _sis()
    profile = radial_profile(_chunks(table), pos_fields=POS, mass_field="mass", vel_fields=[],
                             columns=[], centre=(3.0, -2.0, 0.5), periods=[None] * 3,
                             r_max=1.0, bins=30)
    assert _slope(profile, 1.0) == pytest.approx(-2.0, abs=0.03)
    enclosed = np.array(profile["enclosed_mass"])
    r_hi = np.array(profile["r_hi"])
    p = r_hi
    assert np.all(np.abs(enclosed - p) <= 5 * np.sqrt(p * (1 - p) / 300_000) + 1e-12)
    assert profile["enclosed_mass"][-1] == pytest.approx(
        1.0 - profile["outside_r_max"]["count"] / 300_000, abs=1e-9)


def _nfw(rng, n, c=8.0, r200=1.0):
    from scipy.special import lambertw
    mu_c = np.log(1 + c) - c / (1 + c)
    mu = rng.uniform(0.0, 1.0, n) * mu_c
    xx = -1.0 / np.real(lambertw(-np.exp(-mu - 1.0))) - 1.0
    return (r200 / c) * xx[:, None] * _directions(rng, n)


@pytest.mark.parametrize("seed", [3, 4])
def test_shrinking_sphere_recovers_an_nfw_centre_on_a_periodic_face(seed):
    rng = np.random.default_rng(seed)
    box = 10.0
    true = np.array([0.0, 5.0, 5.0])
    halo = _nfw(rng, 100_000) + true
    background = rng.uniform(0.0, box, (100_000, 3))
    pos = np.mod(np.vstack([halo, background]), box)
    table = {"x": pos[:, 0], "y": pos[:, 1], "z": pos[:, 2], "mass": np.ones(len(pos))}
    found = shrinking_sphere_centre(_chunks(table, 70_001), POS, "mass", [box] * 3,
                                    start=(9.8, 5.1, 4.9), initial_radius=2.0)
    delta = np.asarray(found["centre"]) - true
    delta -= box * np.round(delta / box)
    assert np.linalg.norm(delta) <= 0.01
    unwrapped = shrinking_sphere_centre(_chunks(table, 70_001), POS, "mass", [None] * 3,
                                        start=(9.8, 5.1, 4.9), initial_radius=2.0)
    assert np.linalg.norm(np.asarray(unwrapped["centre"]) - true) > 0.01


def test_shrinking_sphere_from_the_periodic_centre_of_mass():
    rng = np.random.default_rng(5)
    box = 10.0
    true = np.array([0.0, 5.0, 5.0])
    pos = np.mod(_nfw(rng, 100_000) + true, box)
    table = {"x": pos[:, 0], "y": pos[:, 1], "z": pos[:, 2], "mass": np.ones(len(pos))}
    found = shrinking_sphere_centre(_chunks(table), POS, "mass", [box] * 3)
    delta = np.asarray(found["centre"]) - true
    delta -= box * np.round(delta / box)
    assert np.linalg.norm(delta) <= 0.01
    assert found["n_min"] == 1000 and found["particles_inside"] >= 1000


def test_shrinking_sphere_out_of_core_equals_in_memory():
    rng = np.random.default_rng(6)
    pos = _nfw(rng, 60_000) + 2.0
    table = {"x": pos[:, 0], "y": pos[:, 1], "z": pos[:, 2], "mass": rng.uniform(1, 2, 60_000)}
    in_memory = shrinking_sphere_centre(_chunks(table), POS, "mass", [None] * 3)
    out_of_core = shrinking_sphere_centre(_chunks(table, 7_777), POS, "mass", [None] * 3,
                                          memory_rows=2_000)
    assert out_of_core["out_of_core_passes"] > in_memory["out_of_core_passes"]
    assert out_of_core["steps"] == in_memory["steps"]
    assert out_of_core["particles_inside"] == in_memory["particles_inside"]
    np.testing.assert_allclose(out_of_core["centre"], in_memory["centre"], atol=1e-9)


def test_shift_by_the_period_moves_the_centre_and_keeps_the_shells():
    rng = np.random.default_rng(7)
    box = 10.0
    pos = np.mod(_nfw(rng, 50_000) + np.array([0.0, 5.0, 5.0]), box)
    shift = np.array([5.0, 2.5, 7.5])
    moved = np.mod(pos + shift, box)
    results = []
    for p in (pos, moved):
        table = {"x": p[:, 0], "y": p[:, 1], "z": p[:, 2], "mass": np.ones(len(p))}
        centre = shrinking_sphere_centre(_chunks(table), POS, "mass", [box] * 3)["centre"]
        prof = radial_profile(_chunks(table), pos_fields=POS, mass_field="mass", vel_fields=[],
                              columns=[], centre=centre, periods=[box] * 3, r_min=0.01,
                              r_max=1.0, bins=20)
        results.append((np.asarray(centre), prof["count"]))
    delta = results[1][0] - results[0][0] - shift
    delta -= box * np.round(delta / box)
    assert np.linalg.norm(delta) < 1e-9
    assert results[0][1] == results[1][1]


def test_spherical_dispersion_ignores_infall_and_rotation():
    n = 300_000
    table, r, unit = _sis(n=n, centre=(0.0, 0.0, 0.0), sigma=1.0, seed=11)
    v = np.column_stack([table["vx"], table["vy"], table["vz"]])
    cyl = np.sqrt(unit[:, 0] ** 2 + unit[:, 1] ** 2)
    phi_hat = np.column_stack([-unit[:, 1] / cyl, unit[:, 0] / cyl, np.zeros(n)])
    v = v - 1.0 * unit + 0.5 * phi_hat
    table.update(vx=v[:, 0], vy=v[:, 1], vz=v[:, 2])
    prof = radial_profile(_chunks(table), pos_fields=POS, mass_field="mass", vel_fields=VEL,
                          columns=["radial_velocity"], centre=(0, 0, 0), periods=[None] * 3,
                          centre_velocity_value=(0.0, 0.0, 0.0), r_min=0.05, r_max=1.0,
                          bins=8)
    vel = prof["velocity"]
    counts = np.array(prof["count"])
    s1 = np.array(vel["sigma_1d"])
    assert np.all(np.abs(s1 - 1.0) <= 5 / np.sqrt(6 * counts))
    assert np.all(np.abs(np.array(vel["sigma_r"]) - 1.0) <= 5 / np.sqrt(2 * counts))
    assert np.all(np.abs(np.array(vel["mean_radial"]) + 1.0) <= 5 / np.sqrt(counts))
    assert np.all(np.abs(np.array(prof["columns"]["radial_velocity"]["mean"]) + 1.0)
                  <= 5 / np.sqrt(counts))
    cart = np.array(vel["sigma_cartesian_with_streaming"])
    assert np.all(cart > 1.1)


def test_weighted_quantiles_match_sorting_with_zeros():
    rng = np.random.default_rng(12)
    n = 50_000
    table, _, _ = _sis(n=n, centre=(0.0, 0.0, 0.0), seed=12)
    z = np.where(rng.uniform(size=n) < 0.3, 0.0, rng.uniform(0.0, 0.1, n))
    temp = 10 ** rng.normal(5.0, 1.0, n)
    table.update(metallicity=z, temperature=temp, mass=rng.uniform(0.5, 1.5, n))
    prof = radial_profile(_chunks(table, 9_999), pos_fields=POS, mass_field="mass",
                          vel_fields=[], columns=["metallicity", "temperature"],
                          centre=(0, 0, 0), periods=[None] * 3, r_min=0.1, r_max=1.0, bins=4)
    r = np.sqrt(table["x"] ** 2 + table["y"] ** 2 + table["z"] ** 2)
    edges = np.array(prof["r_lo"] + prof["r_hi"][-1:])
    for column in ("metallicity", "temperature"):
        for i in range(4):
            sel = (r >= edges[i]) & (r < edges[i + 1])
            v, w = table[column][sel], table["mass"][sel]
            order = np.argsort(v)
            cum = np.cumsum(w[order])
            for key, q in (("p16", 0.16), ("median", 0.5), ("p84", 0.84)):
                exact = v[order][np.searchsorted(cum, q * cum[-1])]
                got = prof["columns"][column][key][i]
                assert got == pytest.approx(exact, rel=1e-4, abs=1e-6 * v.max()), (column, i, key)


def test_phase_diagram_conserves_mass_and_reports_what_it_drops():
    rng = np.random.default_rng(13)
    n = 20_000
    rho = 10 ** rng.normal(0, 1, n)
    temp = 10 ** rng.normal(4, 0.5, n)
    rho[:3] = [np.nan, 0.0, -1.0]
    mass = rng.uniform(1, 2, n)
    mass[5] = np.nan
    table = {"rho": rho, "temperature": temp, "mass": mass}
    for chunk_size in (None, 3_333, 1):
        if chunk_size == 1:
            table = {k: v[:200] for k, v in table.items()}
        out = phase_diagram(_chunks(table, chunk_size), x="rho", y="temperature",
                            x_log=True, y_log=True, x_bins=32, y_bins=16, weight_field="mass")
        assert out["binned_weight"] + out["outside_weight"] + out["dropped_weight"] == \
            pytest.approx(out["selected_weight"], rel=1e-12)
        assert out["binned_rows"] + out["outside_rows"] + out["dropped_rows"] == \
            out["selected_rows"]
        assert out["dropped_rows"] == 3 and out["outside_rows"] == 0
        assert out["invalid_weight_rows"] == 1
        good = np.isfinite(table["mass"])
        assert out["selected_weight"] == pytest.approx(table["mass"][good].sum(), rel=1e-12)


def test_phase_diagram_with_a_given_range_counts_the_top_edge_and_outside():
    table = {"a": np.array([0.0, 0.5, 1.0, 1.5]), "b": np.array([0.0, 0.5, 1.0, 0.2])}
    out = phase_diagram(_chunks(table), x="a", y="b", x_log=False, y_log=False, x_bins=4,
                        y_bins=4, x_range=(0.0, 1.0), y_range=(0.0, 1.0))
    assert out["binned_rows"] == 3 and out["outside_rows"] == 1
    assert out["counts"][3][3] == 1


def test_phase_diagram_of_radius_and_radial_velocity():
    table, r, unit = _sis(n=20_000, centre=(1.0, 1.0, 1.0), sigma=0.1, seed=14)
    geometry = Geometry(POS, VEL, (1.0, 1.0, 1.0), [None] * 3, (0.0, 0.0, 0.0))
    out = phase_diagram(_chunks(table, 4_096), x="radius", y="radial_velocity", x_log=True,
                        y_log=False, x_bins=16, y_bins=16, weight_field="mass",
                        derive=geometry.derive)
    assert out["binned_weight"] + out["dropped_weight"] + out["outside_weight"] == \
        pytest.approx(1.0, rel=1e-12)


def test_a_profile_can_be_cancelled_between_chunks():
    table, _, _ = _sis(n=10_000)
    stop = threading.Event()
    stop.set()
    with pytest.raises(ProfileCancelled):
        radial_profile(_chunks(table, 1_000), pos_fields=POS, mass_field="mass", vel_fields=[],
                       columns=[], centre=(3.0, -2.0, 0.5), periods=[None] * 3, stop=stop)
    with pytest.raises(ProfileCancelled):
        shrinking_sphere_centre(_chunks(table, 1_000), POS, "mass", [None] * 3, stop=stop)


def test_too_small_a_selection_has_no_centre():
    table, _, _ = _sis(n=50)
    with pytest.raises(ValueError, match="at least 100"):
        shrinking_sphere_centre(_chunks(table), POS, "mass", [None] * 3)
