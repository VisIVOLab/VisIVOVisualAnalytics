"""Physical totals of a selection on particle sets with known answers
(issue #426)."""
from __future__ import annotations

import threading

import numpy as np
import pytest

from app.gadget_snapshot import MSUN_G
from app.particle_profiles import ProfileCancelled
from app.particle_totals import selection_totals

VEL = ["vx", "vy", "vz"]
STORED = {"mass": "1e10 Msun/h", "vx": "km/s", "vy": "km/s", "vz": "km/s",
          "u": "(km/s)^2"}
PHYSICAL = {"mass": {"factor": 1e10, "unit": "Msun"},
            "vx": {"factor": 2.0, "unit": "km/s"}, "vy": {"factor": 2.0, "unit": "km/s"},
            "vz": {"factor": 2.0, "unit": "km/s"},
            "u": {"factor": 1e10, "unit": "erg/g"}}


def _chunks(table: dict, size: int | None = None):
    n = len(next(iter(table.values())))
    size = size or n

    def factory():
        for start in range(0, n, size):
            stop = min(n, start + size)
            yield {k: v[start:stop] for k, v in table.items()}, stop / n
    return factory


def _totals(table, size=None, **kw):
    kw.setdefault("mass_field", "mass")
    kw.setdefault("vel_fields", VEL)
    kw.setdefault("energy_field", "u")
    kw.setdefault("stored_units", STORED)
    kw.setdefault("physical", PHYSICAL)
    return selection_totals(_chunks(table, size), **kw)


def _small():
    return {"mass": np.array([1.0, 2.0, 3.0]), "vx": np.array([1.0, 0.0, -1.0]),
            "vy": np.array([0.0, 2.0, 0.0]), "vz": np.array([0.0, 0.0, 3.0]),
            "u": np.array([4.0, 5.0, 6.0])}


def test_a_known_set_has_known_totals_in_physical_units():
    q = _totals(_small())["quantities"]
    # Stored units: M = 6, P = (1 - 3, 4, 9) = (-2, 4, 9), v_com = P / 6.
    assert q["mass"]["stored"]["value"] == 6.0 and q["mass"]["value"] == 6e10
    assert q["mass"]["unit"] == "Msun"
    np.testing.assert_allclose(q["centre_of_mass_velocity"]["stored"]["value"],
                               np.array([-2.0, 4.0, 9.0]) / 6.0)
    np.testing.assert_allclose(q["centre_of_mass_velocity"]["value"],
                               2.0 * np.array([-2.0, 4.0, 9.0]) / 6.0)     # factor 2 km/s
    assert q["centre_of_mass_velocity"]["unit"] == "km/s"
    np.testing.assert_allclose(q["momentum"]["value"], 2e10 * np.array([-2.0, 4.0, 9.0]))
    assert q["momentum"]["unit"] == "Msun km/s"
    assert q["momentum_magnitude"]["value"] == pytest.approx(2e10 * np.sqrt(4 + 16 + 81))
    # 0.5 sum m v^2 = 0.5 (1*1 + 2*4 + 3*(1 + 9)) = 19.5 in (stored)^2.
    assert q["kinetic_energy"]["stored"]["value"] == pytest.approx(19.5)
    assert q["kinetic_energy"]["value"] == pytest.approx(
        19.5 * 1e10 * 4.0 * MSUN_G * 1e10)
    assert q["kinetic_energy"]["unit"] == "erg"
    # In the COM frame: 19.5 - 0.5 M v_com^2 = 19.5 - 0.5 * 101 / 6.
    assert q["kinetic_energy_com_frame"]["stored"]["value"] == pytest.approx(
        19.5 - 0.5 * 101.0 / 6.0)
    assert q["kinetic_energy_com_frame"]["value"] == pytest.approx(
        (19.5 - 0.5 * 101.0 / 6.0) * 1e10 * 4.0 * MSUN_G * 1e10)
    # Thermal: sum m u = 4 + 10 + 18 = 32 (1e10 Msun) (km/s)^2.
    assert q["thermal_energy"]["stored"]["value"] == 32.0
    assert q["thermal_energy"]["value"] == pytest.approx(32.0 * 1e10 * 1e10 * MSUN_G)
    assert all(v["reason"] is None for v in q.values())


def test_chunking_does_not_change_totals_and_the_bulk_motion_does_not_cancel():
    """A halo whose bulk motion (1e6) is 1e6 times its dispersion: the naive
    sum m v^2 - M v_com^2 has lost every digit of the dispersion energy."""
    rng = np.random.default_rng(4)
    n = 120_000
    v = 1.0e6 + rng.normal(0.0, 1.0, (n, 3))
    table = {"mass": rng.uniform(0.5, 1.5, n), "vx": v[:, 0], "vy": v[:, 1], "vz": v[:, 2],
             "u": rng.uniform(0.0, 3.0, n)}
    w = table["mass"]
    v_com = (w[:, None] * v).sum(0) / w.sum()
    exact = 0.5 * float(np.sum(w[:, None] * (v - v_com) ** 2))        # two passes, centred
    naive = 0.5 * float(np.sum(w * np.sum(v ** 2, axis=1)) - w.sum() * v_com @ v_com)
    assert abs(naive - exact) / exact > 1e-6                            # the trap is real
    for size in (None, 7_001, 999):
        got = _totals(table, size)
        ke = got["quantities"]["kinetic_energy_com_frame"]["stored"]["value"]
        assert ke == pytest.approx(exact, rel=1e-9)
        assert got["quantities"]["kinetic_energy"]["stored"]["value"] == pytest.approx(
            0.5 * float(np.sum(w * np.sum(v ** 2, axis=1))), rel=1e-12)
        assert got["quantities"]["thermal_energy"]["stored"]["value"] == pytest.approx(
            float(np.dot(w, table["u"])), rel=1e-12)
        assert got["quantities"]["velocity_dispersion_1d"]["stored"]["value"] == pytest.approx(
            np.sqrt(2 * exact / (3 * w.sum())), rel=1e-9)
        assert got["count"] == n


def test_a_known_centre_of_mass_velocity_and_momentum():
    rng = np.random.default_rng(5)
    n = 60_000
    bulk = np.array([120.0, -35.0, 7.0])
    table = {"mass": rng.uniform(1.0, 2.0, n)}
    v = bulk + rng.normal(0.0, 10.0, (n, 3))
    table.update(vx=v[:, 0], vy=v[:, 1], vz=v[:, 2], u=np.ones(n))
    q = _totals(table, 10_000)["quantities"]
    np.testing.assert_allclose(q["centre_of_mass_velocity"]["stored"]["value"], bulk,
                               atol=5 * 10 / np.sqrt(n))
    total = table["mass"].sum()
    np.testing.assert_allclose(q["momentum"]["stored"]["value"],
                               total * np.array(q["centre_of_mass_velocity"]["stored"]["value"]),
                               rtol=1e-12)
    assert q["velocity_dispersion_1d"]["stored"]["value"] == pytest.approx(10.0, rel=0.02)


def test_a_missing_energy_field_disables_only_the_thermal_energy():
    table = _small()
    del table["u"]
    out = _totals(table, energy_field="", missing={"energy": "No u: only gas has one."})
    q = out["quantities"]
    assert q["thermal_energy"]["value"] is None and q["thermal_energy"]["stored"] is None
    assert q["thermal_energy"]["reason"] == "No u: only gas has one."
    assert q["kinetic_energy"]["value"] is not None and q["mass"]["value"] == 6e10


def test_missing_velocities_disable_every_velocity_total_with_a_reason():
    table = {"mass": np.array([1.0, 2.0]), "u": np.array([1.0, 1.0])}
    q = _totals(table, vel_fields=[], missing={"velocity": "No velocities here."})["quantities"]
    for name in ("centre_of_mass_velocity", "momentum", "momentum_magnitude",
                 "kinetic_energy", "kinetic_energy_com_frame", "velocity_dispersion_1d"):
        assert q[name]["value"] is None and q[name]["reason"] == "No velocities here."
    assert q["mass"]["value"] == 3e10 and q["thermal_energy"]["value"] is not None


def test_no_mass_field_disables_every_mass_weighted_total():
    table = {"vx": np.ones(3), "vy": np.ones(3), "vz": np.ones(3)}
    out = _totals(table, mass_field="", energy_field="")
    assert out["count"] == 3
    for name, quantity in out["quantities"].items():
        assert quantity["value"] is None and "mass" in quantity["reason"], name


def test_an_undeclared_unit_keeps_the_stored_value_and_says_what_is_missing():
    physical = {k: v for k, v in PHYSICAL.items() if k != "vy"}
    q = _totals(_small(), physical=physical)["quantities"]
    for name in ("centre_of_mass_velocity", "momentum", "kinetic_energy"):
        assert q[name]["value"] is None and "'vy'" in q[name]["reason"]
        assert q[name]["stored"]["value"] is not None
    assert q["mass"]["value"] == 6e10 and q["thermal_energy"]["value"] is not None
    # A velocity declared in another unit is refused too, not silently scaled.
    other = dict(PHYSICAL, vx={"factor": 1.0, "unit": "kpc/Gyr"})
    q = _totals(_small(), physical=other)["quantities"]
    assert q["momentum"]["value"] is None and "kpc/Gyr" in q["momentum"]["reason"]
    # Velocity fields with different factors have no single conversion.
    uneven = dict(PHYSICAL, vx={"factor": 3.0, "unit": "km/s"})
    assert "different physical factors" in _totals(
        _small(), physical=uneven)["quantities"]["momentum"]["reason"]
    # No physical table at all (a loaded VBT): every value null, stored kept.
    q = _totals(_small(), physical={})["quantities"]
    assert all(v["value"] is None and v["stored"] is not None and v["reason"]
               for v in q.values())


def test_rows_without_a_valid_weight_velocity_or_energy_are_counted_apart():
    table = {"mass": np.array([1.0, np.nan, -1.0, 2.0, 3.0]),
             "vx": np.array([1.0, 1.0, 1.0, np.nan, 2.0]),
             "vy": np.zeros(5), "vz": np.zeros(5),
             "u": np.array([1.0, 1.0, 1.0, 1.0, -2.0])}
    out = _totals(table)
    assert out["rows"] == {"selected": 5, "with_mass": 3, "with_velocity": 2,
                           "with_energy": 2}
    assert out["quantities"]["mass"]["stored"]["value"] == 6.0
    assert out["mass_with_velocity"] == 4.0
    assert out["quantities"]["centre_of_mass_velocity"]["stored"]["value"][0] == \
        pytest.approx((1.0 + 6.0) / 4.0)
    assert out["quantities"]["thermal_energy"]["stored"]["value"] == 3.0   # 1*1 + 2*1


def test_an_empty_selection_has_zero_mass_and_no_velocity():
    table = {k: np.empty(0) for k in ("mass", "vx", "vy", "vz", "u")}
    out = selection_totals(lambda: iter([(table, 1.0)]), mass_field="mass", vel_fields=VEL,
                           energy_field="u", stored_units=STORED, physical=PHYSICAL)
    assert out["count"] == 0 and out["quantities"]["mass"]["value"] == 0.0
    assert out["quantities"]["momentum"]["value"] is None
    assert "finite velocities" in out["quantities"]["momentum"]["reason"]
    assert out["quantities"]["thermal_energy"]["value"] == 0.0


def test_a_run_stops_between_two_chunks():
    stop = threading.Event()
    table = _small()

    def chunks():
        yield {k: v[:2] for k, v in table.items()}, 0.5
        stop.set()
        yield {k: v[2:] for k, v in table.items()}, 1.0
    with pytest.raises(ProfileCancelled):
        selection_totals(chunks, mass_field="mass", vel_fields=VEL, energy_field="u",
                         stored_units=STORED, physical=PHYSICAL, stop=stop)
