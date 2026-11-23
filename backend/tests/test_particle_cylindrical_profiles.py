"""Cylindrical and along-axis profiles on synthetic particle sets with known
answers (issue #426): a uniform cylinder, rigid rotation about an oblique
axis, a cylinder across a periodic boundary. Tolerances are 5 sigma of the
Poisson noise of the bin they check."""
from __future__ import annotations

import numpy as np
import pytest

from app.particle_profiles import radial_profile

POS = ["x", "y", "z"]
VEL = ["vx", "vy", "vz"]
RADIUS, HALF_HEIGHT = 1.0, 2.0
AXIS = np.array([1.0, 1.0, 1.0]) / np.sqrt(3.0)


def _frame(axis):
    """Orthonormal e1, e2 perpendicular to ``axis``."""
    ref = np.array([1.0, 0.0, 0.0]) if abs(axis[0]) < 0.9 else np.array([0.0, 1.0, 0.0])
    e1 = np.cross(ref, axis)
    e1 /= np.linalg.norm(e1)
    return e1, np.cross(axis, e1)


def _chunks(table: dict, size: int | None = None):
    n = len(next(iter(table.values())))
    size = size or n

    def factory():
        for start in range(0, n, size):
            stop = min(n, start + size)
            yield {k: v[start:stop] for k, v in table.items()}, stop / n
    return factory


def _cylinder(n=300_000, centre=(3.0, -2.0, 0.5), axis=AXIS, spin=None, seed=1, box=None):
    """Uniform density in R < RADIUS, |z| < HALF_HEIGHT about ``axis`` through
    ``centre``; total mass 1; ``spin``: rigid rotation rate about the axis."""
    rng = np.random.default_rng(seed)
    r = RADIUS * np.sqrt(rng.uniform(0.0, 1.0, n))
    phi = rng.uniform(0.0, 2.0 * np.pi, n)
    z = rng.uniform(-HALF_HEIGHT, HALF_HEIGHT, n)
    e1, e2 = _frame(axis)
    radial = np.cos(phi)[:, None] * e1 + np.sin(phi)[:, None] * e2
    azimuthal = -np.sin(phi)[:, None] * e1 + np.cos(phi)[:, None] * e2
    pos = np.asarray(centre) + r[:, None] * radial + z[:, None] * axis
    if box:
        pos = np.mod(pos, box)
    table = {"x": pos[:, 0], "y": pos[:, 1], "z": pos[:, 2], "mass": np.full(n, 1.0 / n)}
    if spin is not None:
        v = (spin * r)[:, None] * azimuthal
        table.update(vx=v[:, 0], vy=v[:, 1], vz=v[:, 2])
    return table


def _run(table, size=None, vel=False, **kw):
    kw.setdefault("centre", (3.0, -2.0, 0.5))
    kw.setdefault("periods", [None] * 3)
    return radial_profile(_chunks(table, size), pos_fields=POS, mass_field="mass",
                          vel_fields=VEL if vel else [], columns=kw.pop("columns", []), **kw)


RHO = 1.0 / (np.pi * RADIUS ** 2 * 2 * HALF_HEIGHT)


def test_a_uniform_cylinder_has_uniform_density_in_annuli():
    prof = _run(_cylinder(), geometry="cylindrical", axis=AXIS, r_min=0.1, r_max=1.0,
                z_min=-HALF_HEIGHT, z_max=HALF_HEIGHT, bins=8)
    count = np.array(prof["count"])
    assert np.all(np.abs(np.array(prof["density"]) / RHO - 1) <= 5 / np.sqrt(count))
    assert np.all(np.abs(np.array(prof["surface_density"]) / (RHO * 2 * HALF_HEIGHT) - 1)
                  <= 5 / np.sqrt(count))
    # Closure: every usable particle is inside r_min, in a bin, or outside.
    assert (prof["inside_r_min"]["count"] + sum(prof["count"])
            + prof["outside_r_max"]["count"] + prof["outside_z_range"]["count"]) == 300_000
    assert prof["enclosed_mass"][-1] + prof["outside_r_max"]["mass"] == pytest.approx(1.0)
    assert prof["geometry"] == "cylindrical" and prof["z_range"] == [-2.0, 2.0]
    assert prof["axis"]["from"] == "given"
    np.testing.assert_allclose(prof["axis"]["vector"], AXIS, atol=1e-12)


def test_a_cylinder_without_a_slab_gives_the_surface_density_only():
    prof = _run(_cylinder(), geometry="cylindrical", axis=AXIS, r_min=0.1, r_max=1.0, bins=8)
    assert prof["density"] is None and prof["z_range"] is None
    count = np.array(prof["count"])
    assert np.all(np.abs(np.array(prof["surface_density"]) / (RHO * 2 * HALF_HEIGHT) - 1)
                  <= 5 / np.sqrt(count))


def test_a_slab_keeps_only_its_heights_and_the_rest_closes():
    table = _cylinder(seed=2)
    prof = _run(table, geometry="cylindrical", axis=AXIS, r_min=0.1, r_max=1.0, z_min=-1.0,
                z_max=0.5, bins=6)
    delta = np.column_stack([table[f] for f in POS]) - np.array([3.0, -2.0, 0.5])
    height = delta @ AXIS
    kept = (height >= -1.0) & (height < 0.5)
    assert prof["outside_z_range"]["count"] == int((~kept).sum())
    assert (sum(prof["count"]) + prof["inside_r_min"]["count"]
            + prof["outside_r_max"]["count"]) == int(kept.sum())
    count = np.array(prof["count"])
    assert np.all(np.abs(np.array(prof["density"]) / RHO - 1) <= 5 / np.sqrt(count))


def test_axial_slabs_of_a_uniform_cylinder():
    prof = _run(_cylinder(seed=3), geometry="axial", axis=AXIS, z_min=-HALF_HEIGHT,
                z_max=HALF_HEIGHT, r_max=RADIUS, bins=8)
    count = np.array(prof["count"])
    assert np.all(np.abs(np.array(prof["density"]) / RHO - 1) <= 5 / np.sqrt(count))
    assert np.all(np.abs(np.array(prof["linear_density"]) / (RHO * np.pi * RADIUS ** 2) - 1)
                  <= 5 / np.sqrt(count))
    np.testing.assert_allclose(np.diff(prof["z_lo"]), HALF_HEIGHT / 4)
    assert prof["z_lo"][0] == -HALF_HEIGHT and prof["z_hi"][-1] == HALF_HEIGHT
    assert prof["below_z_min"]["count"] == 0 and prof["above_z_max"]["count"] == 0


def test_an_axial_profile_with_a_radius_limit_cuts_the_cylinder():
    table = _cylinder(seed=4)
    half = _run(table, geometry="axial", axis=AXIS, z_min=-HALF_HEIGHT, z_max=HALF_HEIGHT,
                r_max=0.5 * RADIUS, bins=4)
    count = np.array(half["count"])
    # Same density in the narrower cylinder, a quarter of the particles.
    assert np.all(np.abs(np.array(half["density"]) / RHO - 1) <= 5 / np.sqrt(count))
    assert sum(half["count"]) + half["outside_r_limit"]["count"] == 300_000
    assert sum(half["count"]) == pytest.approx(75_000, rel=0.02)
    assert half["r_limit"] == 0.5


def test_an_axial_profile_without_a_radius_limit_has_no_volume_density():
    prof = _run(_cylinder(seed=5), geometry="axial", axis=AXIS, bins=10)
    assert prof["density"] is None and prof["r_limit"] is None
    assert prof["z_lo"][0] == pytest.approx(-prof["z_hi"][-1])
    assert sum(prof["count"]) + prof["below_z_min"]["count"] + prof["above_z_max"]["count"] \
        == 300_000
    assert prof["enclosed_mass"][-1] + prof["above_z_max"]["mass"] == pytest.approx(1.0)


def test_chunking_does_not_change_the_result():
    table = _cylinder(n=50_000, seed=6)
    kw = dict(geometry="cylindrical", axis=AXIS, r_min=0.1, r_max=1.0, bins=5)
    whole, parts = _run(table, **kw), _run(table, size=7_001, **kw)
    assert whole["count"] == parts["count"]
    np.testing.assert_allclose(whole["mass"], parts["mass"], rtol=1e-12)


def test_rigid_rotation_is_azimuthal_about_the_derived_axis():
    omega = 3.0
    table = _cylinder(n=200_000, spin=omega, seed=7)
    prof = _run(table, vel=True, geometry="cylindrical", r_min=0.1, r_max=1.0, bins=6,
                z_min=-HALF_HEIGHT, z_max=HALF_HEIGHT)
    velocity = prof["velocity"]
    assert "angular momentum" in prof["axis"]["from"]
    assert abs(float(np.dot(velocity["axis"], AXIS))) > 0.999
    sign = np.sign(np.dot(velocity["axis"], AXIS))   # the derived axis follows L
    r_mean = np.array(prof["r_mean"])
    np.testing.assert_allclose(sign * np.array(velocity["mean_azimuthal"]), omega * r_mean,
                               rtol=1e-2)   # the derived axis is tilted by the noise
    # Components the tilt of the derived axis (< 0.05 rad) leaks into.
    assert np.all(np.abs(velocity["mean_cylindrical_radial"]) < 0.05)
    assert np.all(np.abs(velocity["mean_vertical"]) < 0.15)
    assert np.all(np.array(velocity["sigma_R"]) < 0.15)


def test_axial_profile_velocities_are_the_cylindrical_components():
    table = _cylinder(n=100_000, spin=2.0, seed=8)
    # Add streaming along the axis and radial infall, which do not rotate.
    delta = np.column_stack([table[f] for f in POS]) - np.array([3.0, -2.0, 0.5])
    height = delta @ AXIS
    perpendicular = delta - height[:, None] * AXIS
    table["vx"] = table["vx"] + 0.7 * AXIS[0] - 0.4 * perpendicular[:, 0]
    table["vy"] = table["vy"] + 0.7 * AXIS[1] - 0.4 * perpendicular[:, 1]
    table["vz"] = table["vz"] + 0.7 * AXIS[2] - 0.4 * perpendicular[:, 2]
    prof = _run(table, vel=True, geometry="axial", axis=AXIS, z_min=-HALF_HEIGHT,
                z_max=HALF_HEIGHT, r_max=RADIUS, bins=4, centre_velocity_value=(0, 0, 0))
    v = prof["velocity"]
    np.testing.assert_allclose(v["mean_vertical"], 0.7, atol=2e-3)
    np.testing.assert_allclose(v["mean_azimuthal"], 2.0 * 2 / 3, rtol=0.01)   # <R> = 2/3
    np.testing.assert_allclose(v["mean_cylindrical_radial"], -0.4 * 2 / 3, rtol=0.01)
    assert "sigma_theta" not in v and "beta" not in v


def test_without_velocities_or_an_axis_the_table_z_is_the_axis():
    table = _cylinder(n=20_000, axis=np.array([0.0, 0.0, 1.0]), seed=9)
    prof = _run(table, geometry="cylindrical", r_min=0.1, r_max=1.0, bins=4)
    assert prof["axis"]["from"] == "the table's z"
    assert prof["axis"]["vector"] == [0.0, 0.0, 1.0] and "velocity" not in prof


def test_a_cylinder_across_a_periodic_boundary_is_whole():
    box = 10.0
    table = _cylinder(n=200_000, centre=(0.3, 5.0, 9.8), axis=np.array([0.0, 0.0, 1.0]),
                      seed=10, box=box)
    assert table["z"].min() < 1.0 < table["z"].max() - 8.0      # it wraps
    prof = _run(table, centre=(0.3, 5.0, 9.8), periods=[box] * 3, geometry="cylindrical",
                axis=(0, 0, 1), r_min=0.1, r_max=1.0, z_min=-HALF_HEIGHT, z_max=HALF_HEIGHT,
                bins=6)
    count = np.array(prof["count"])
    assert np.all(np.abs(np.array(prof["density"]) / RHO - 1) <= 5 / np.sqrt(count))
    assert prof["outside_z_range"]["count"] == 0


def test_completeness_is_conservative_across_the_axis_extent():
    table = _cylinder(n=20_000, axis=np.array([0.0, 0.0, 1.0]), seed=11)
    kw = dict(geometry="cylindrical", axis=(0, 0, 1), r_min=0.1, r_max=1.0, bins=4)
    assert all(_run(table, **kw)["complete"])
    bounded = _run(table, region_limit=1.5, **kw)
    assert not any(bounded["complete"])        # no slab: reaches past the region
    slab = _run(table, region_limit=2.5, z_min=-1.0, z_max=1.0, **kw)
    assert all(slab["complete"])               # sqrt(1^2 + 1^2) < 2.5
    tight = _run(table, region_limit=1.2, z_min=-1.0, z_max=1.0, **kw)
    assert not tight["complete"][-1]           # sqrt(1^2 + 1^2) > 1.2


@pytest.mark.parametrize("kw, message", [
    (dict(geometry="spherical", z_min=-1.0, z_max=1.0), "cylindrical or axial"),
    (dict(geometry="axial", r_min=0.1), "no r_min"),
    (dict(geometry="cylindrical", z_min=-1.0), "both z_min and z_max"),
    (dict(geometry="axial", z_min=1.0, z_max=-1.0), "z_min < z_max"),
    (dict(geometry="axial", z_max=float("nan")), "finite"),
    (dict(geometry="axial", r_max=-1.0), "positive"),
    (dict(geometry="conical"), "geometry must be"),
])
def test_refusals_are_explained(kw, message):
    table = _cylinder(n=2_000, seed=12)
    with pytest.raises(ValueError, match=message):
        _run(table, **kw)
