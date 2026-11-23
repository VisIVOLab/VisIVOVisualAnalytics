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
    lo, hi = np.array(profile["r_lo"]), np.array(profile["r_hi"])
    n = np.array(profile["count"])
    truth = 3.0 * (hi - lo) / (4.0 * np.pi * (hi ** 3 - lo ** 3))
    pull = np.array(profile["density"]) / truth - 1.0
    big = n >= 100
    assert np.all(np.abs(pull[big]) <= 5.0 / np.sqrt(n[big]))


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
    assert np.linalg.norm(delta) <= 0.005
    wrapped = shrinking_sphere_centre(_chunks(table, 70_001), POS, "mass", [box] * 3)
    delta = np.asarray(wrapped["centre"]) - true
    delta -= box * np.round(delta / box)
    assert np.linalg.norm(delta) <= 0.005
    unwrapped = shrinking_sphere_centre(_chunks(table, 70_001), POS, "mass", [None] * 3)
    miss = np.asarray(unwrapped["centre"]) - true
    miss -= box * np.round(miss / box)
    assert np.linalg.norm(miss) > 0.05


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
    z = np.where(rng.uniform(size=n) < 0.3, 0.0,
                 np.minimum(10 ** rng.normal(-3.5, 0.7, n), 0.1))
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
                assert got == pytest.approx(exact, rel=1e-3, abs=1e-6 * v.max()), (column, i, key)


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
        assert out["binned_rows"] + out["outside_rows"] + out["dropped_rows"] + \
            out["invalid_weight_rows"] == out["selected_rows"] == len(table["mass"])
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
    v = np.column_stack([table["vx"], table["vy"], table["vz"]])
    delta = np.column_stack([table["x"], table["y"], table["z"]]) - 1.0
    r = np.sqrt(np.sum(delta * delta, axis=1))
    v_r = np.sum(v * delta, axis=1) / r
    want, _, _ = np.histogram2d(r, v_r, bins=[out["x_edges"], out["y_edges"]],
                                weights=table["mass"])
    np.testing.assert_allclose(out["weights"], want, rtol=1e-9, atol=1e-12)


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


def test_a_start_sphere_with_too_few_particles_is_refused():
    table, _, _ = _sis(n=20_000)
    with pytest.raises(ValueError, match="start sphere"):
        shrinking_sphere_centre(_chunks(table), POS, "mass", [None] * 3,
                                start=(3.0, -2.0, 0.5), initial_radius=0.001)


def test_the_first_step_is_the_centre_of_the_start_sphere():
    rng = np.random.default_rng(15)
    pos = _directions(rng, 150)
    table = {"x": pos[:, 0], "y": pos[:, 1], "z": pos[:, 2], "mass": np.ones(150)}
    found = shrinking_sphere_centre(_chunks(table), POS, "mass", [None] * 3,
                                    start=(0.5, 0.0, 0.0), initial_radius=1.6, shrink=0.5)
    assert found["steps"] == 1 and found["particles_inside"] == 150
    np.testing.assert_allclose(found["centre"], pos.mean(axis=0), atol=1e-12)


def test_non_finite_velocities_do_not_change_the_density():
    table, _, _ = _sis(n=50_000, centre=(0.0, 0.0, 0.0), sigma=1.0, seed=16)
    common = dict(pos_fields=POS, mass_field="mass", columns=[], centre=(0, 0, 0),
                  periods=[None] * 3, r_min=0.05, r_max=1.0, bins=6)
    plain = radial_profile(_chunks(table), vel_fields=[], **common)
    table["vx"][::7] = np.nan
    moving = radial_profile(_chunks(table), vel_fields=VEL,
                            centre_velocity_value=(0.0, 0.0, 0.0), **common)
    assert moving["count"] == plain["count"]
    np.testing.assert_allclose(moving["density"], plain["density"], rtol=1e-12)
    assert all(abs(s - 1.0) < 0.05 for s in moving["velocity"]["sigma_1d"])


def test_the_default_r_max_keeps_the_farthest_particle():
    table, _, _ = _sis(n=5_000, centre=(0.0, 0.0, 0.0))
    prof = radial_profile(_chunks(table), pos_fields=POS, mass_field="mass", vel_fields=[],
                          columns=[], centre=(0, 0, 0), periods=[None] * 3, bins=8)
    assert prof["outside_r_max"]["count"] == 0
    assert prof["enclosed_mass"][-1] == pytest.approx(1.0, rel=1e-12)


def test_the_default_centre_velocity_recovers_a_bulk_motion():
    table, _, _ = _sis(n=50_000, centre=(0.0, 0.0, 0.0), sigma=1.0, seed=17)
    for name, bulk in zip(VEL, (30.0, -20.0, 5.0)):
        table[name] = table[name] + bulk
    prof = radial_profile(_chunks(table, 9_000), pos_fields=POS, mass_field="mass",
                          vel_fields=VEL, columns=[], centre=(0, 0, 0), periods=[None] * 3,
                          r_min=0.05, r_max=1.0, bins=6)
    found = prof["velocity"]["centre_velocity"]
    np.testing.assert_allclose(found, [30.0, -20.0, 5.0], atol=5.0 / np.sqrt(1000))
    assert prof["velocity"]["centre_velocity_from"]["particles"] >= 1000


def _rotating_sis(axis, seed, n=200_000):
    table, r, unit = _sis(n=n, centre=(0.0, 0.0, 0.0), sigma=1.0, seed=seed)
    axis = np.asarray(axis, dtype=np.float64) / np.linalg.norm(axis)
    tangent = np.cross(axis, unit)
    norm = np.linalg.norm(tangent, axis=1)
    tangent /= np.where(norm > 0, norm, 1.0)[:, None]
    v = np.column_stack([table["vx"], table["vy"], table["vz"]]) + 0.5 * tangent
    table.update(vx=v[:, 0], vy=v[:, 1], vz=v[:, 2])
    return table


@pytest.mark.parametrize("axis", [(1.0, 0.0, 0.0), (1.0, 1.0, 1.0)])
def test_rotation_about_any_axis_is_not_dispersion(axis):
    table = _rotating_sis(axis, seed=18)
    prof = radial_profile(_chunks(table), pos_fields=POS, mass_field="mass", vel_fields=VEL,
                          columns=[], centre=(0, 0, 0), periods=[None] * 3,
                          centre_velocity_value=(0.0, 0.0, 0.0), r_min=0.05, r_max=1.0,
                          bins=6)
    vel = prof["velocity"]
    counts = np.array(prof["count"])
    unit = np.asarray(axis) / np.linalg.norm(axis)
    assert abs(float(np.dot(vel["axis"], unit))) > 0.99
    assert np.all(np.abs(np.array(vel["sigma_1d"]) - 1.0) <= 5 / np.sqrt(6 * counts))
    mean_phi = np.array(vel["mean_azimuthal"]) * np.sign(np.dot(vel["axis"], unit))
    assert np.all(np.abs(mean_phi - 0.5) <= 5 / np.sqrt(counts))


def test_a_moving_halo_in_a_background_keeps_its_own_centre_velocity():
    rng = np.random.default_rng(19)
    box = 100.0
    halo, _, _ = _sis(n=20_000, centre=(50.0, 50.0, 50.0), sigma=1.0, seed=19)
    for name in VEL:
        halo[name] = halo[name] + (3.0 if name == "vx" else 0.0)
    m = 200_000
    bg = rng.uniform(0.0, box, (m, 3))
    bv = rng.normal(0.0, 1.0, (m, 3))
    table = {k: np.concatenate([halo[k], col]) for k, col in
             (("x", bg[:, 0]), ("y", bg[:, 1]), ("z", bg[:, 2]), ("vx", bv[:, 0]),
              ("vy", bv[:, 1]), ("vz", bv[:, 2]), ("mass", np.full(m, 1.0 / 20_000)))}
    prof = radial_profile(_chunks(table, 50_000), pos_fields=POS, mass_field="mass",
                          vel_fields=VEL, columns=[], centre=(50.0, 50.0, 50.0),
                          periods=[box] * 3, r_min=0.05, r_max=1.0, bins=6)
    vel = prof["velocity"]
    assert vel["centre_velocity"][0] == pytest.approx(3.0, abs=5 / np.sqrt(1000))
    counts = np.array(prof["count"])
    assert np.all(np.abs(np.array(vel["sigma_1d"]) - 1.0) <= 5 / np.sqrt(6 * counts))


def test_a_nan_velocity_component_leaves_the_dispersions_unbiased():
    table, _, _ = _sis(n=40_000, centre=(0.0, 0.0, 0.0), sigma=1.0, seed=20)
    common = dict(pos_fields=POS, mass_field="mass", vel_fields=VEL, columns=[],
                  centre=(0, 0, 0), periods=[None] * 3, centre_velocity_value=(0, 0, 0),
                  axis=(0, 0, 1), r_min=0.05, r_max=1.0, bins=5)
    half = {k: v[1::2] for k, v in table.items()}
    clean = radial_profile(_chunks(half), **common)
    table["vz"][::2] = np.nan
    holed = radial_profile(_chunks(table), **common)
    for key in ("sigma_r", "sigma_phi", "sigma_theta", "sigma_cartesian_with_streaming"):
        np.testing.assert_allclose(holed["velocity"][key], clean["velocity"][key], rtol=1e-9)


def test_comparisons_and_holes_on_the_positions_bound_the_complete_shells():
    from app.routers.particle_profiles import _region_limit
    from app.schemas import TabularBox, TabularFilter, TabularSphere
    cuts = [TabularFilter(field="x", op=">", value="-0.3"),
            TabularFilter(field="x", op="<", value="0.5")]
    assert _region_limit(cuts, POS, (0.0, 0.0, 0.0), [None] * 3) == pytest.approx(0.3)
    hole = [TabularFilter(op="outside", sphere=TabularSphere(fields=POS, center=[1, 0, 0],
                                                             radius=0.4))]
    assert _region_limit(hole, POS, (0.0, 0.0, 0.0), [None] * 3) == pytest.approx(0.6)
    box = [TabularFilter(op="outside", box=TabularBox(fields=POS, min=[0.5, -1, -1],
                                                      max=[1, 1, 1]))]
    assert _region_limit(box, POS, (0.0, 0.0, 0.0), [None] * 3) == pytest.approx(0.5)
    assert _region_limit(box, POS, (0.7, 0.0, 0.0), [None] * 3) == 0.0
    assert _region_limit(cuts, POS, (0.9, 0.0, 0.0), [None] * 3) == 0.0


def test_the_centre_particle_has_no_radial_velocity():
    table = {"x": np.array([0.0, 1.0]), "y": np.zeros(2), "z": np.zeros(2),
             "vx": np.array([5.0, 2.0]), "vy": np.zeros(2), "vz": np.zeros(2)}
    out = Geometry(POS, VEL, (0, 0, 0), [None] * 3, (0, 0, 0)).derive(table)
    assert np.isnan(out["radial_velocity"][0]) and out["radial_velocity"][1] == 2.0
