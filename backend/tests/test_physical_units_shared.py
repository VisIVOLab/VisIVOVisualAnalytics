"""One physical-units table for every reader (issue #524): Gadget's unit system,
PLUTO's run units and typed factors all give ``{column: {factor, unit}}`` through
``app.physical_units.scales``, and what the user typed replaces any of them."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_gadget_snapshot import _snapshot  # noqa: E402
from test_particle_profile_routes import _gadget, _halo, _open, _wait  # noqa: E402

from app import gadget_snapshot as gs  # noqa: E402
from app import physical_units as pu  # noqa: E402


@pytest.fixture(autouse=True)
def _clean_store():
    pu.clear_store()
    yield
    pu.clear_store()


def _first(tmp_path):
    files, _ = _snapshot(tmp_path)
    return files[0]


def test_gadget_columns_get_the_same_scales_through_the_shared_function(tmp_path):
    first = _first(tmp_path)
    component = gs.open_snapshot(first).component(0)
    ref = gs.gadget_ref(first, 0)
    shared = pu.scales(ref)
    assert shared == component.physical()                  # the reader's table, as before
    assert set(shared) <= set(component.names)
    for column, entry in shared.items():                   # the producer's shape: no extras
        assert set(entry) == {"factor", "unit"}, column
        assert pu.column_scale(ref, column) == entry
    a = component.snapshot.scale_factor
    h = component.snapshot.hubble
    assert shared["x"]["unit"] == "kpc"
    assert shared["x"]["factor"] == pytest.approx(
        component.snapshot.units.length_cm / 3.0856775814913673e21 * a / h)
    assert pu.column_scale(ref, "not-a-column") is None


def test_a_typed_factor_replaces_one_gadget_column_only(tmp_path):
    first = _first(tmp_path)
    ref = gs.gadget_ref(first, 0)
    before = pu.scales(ref)
    pu.set_user(ref, columns={"mass": (123.0, "10^10 Msun")})
    after = gs.resolve(ref).physical()                      # the table every route reads
    assert after["mass"] == {"factor": 123.0, "unit": "10^10 Msun", "source": "user"}
    assert after == pu.scales(ref)
    assert {c: e for c, e in after.items() if c != "mass"} == {
        c: e for c, e in before.items() if c != "mass"}
    # The snapshot's other particle types are the snapshot's: the same override.
    other = next(t for t in range(6) if t != 0 and gs.open_snapshot(first).counts()[t])
    if "mass" in gs.open_snapshot(first).component(other).names:
        assert pu.scales(gs.gadget_ref(first, other))["mass"]["factor"] == 123.0
    pu.set_user(ref, clear=True)
    assert pu.scales(ref) == before


def test_a_typed_column_unknown_to_the_snapshot_is_ignored(tmp_path):
    first = _first(tmp_path)
    ref = gs.gadget_ref(first, 0)
    pu.set_user(ref, columns={"nope": (2.0, "x")})
    assert "nope" not in pu.scales(ref)


@pytest.mark.asyncio
async def test_the_units_route_overrides_a_gadget_column_for_the_profile_routes(
        client, auth_headers, tmp_path):
    pos, vel, mass = _halo()
    path = _gadget(tmp_path, pos, vel, mass)
    dataset = await _open(client, auth_headers, path, "gadget")
    sibling = await _open(client, auth_headers, path, "gadget")   # same snapshot, opened twice
    resp = await client.post("/v1/datasets/units", headers=auth_headers, json={
        "dataset_id": dataset, "columns": {"x": {"factor": 2.0, "unit": "Mpc"},
                                           "y": {"factor": 2.0, "unit": "Mpc"},
                                           "z": {"factor": 2.0, "unit": "Mpc"}}})
    body = resp.json()
    assert body["valid"], body["error"]
    columns = body["physical_units"]["columns"]
    assert columns["x"] == {"factor": 2.0, "unit": "Mpc", "source": "user"}
    assert columns["vx"]["source"] == ""                    # the reader's own, untouched
    created = (await client.post("/v1/tasks/catalogue_profile", headers=auth_headers, json={
        "dataset_id": sibling, "columns": ["radial_velocity"], "bins": 6,
        "centre": {"mode": "shrinking_sphere"}})).json()
    status = await _wait(client, auth_headers, created["task_id"])
    physical = status["result"]["units"]["physical"]
    assert physical["radius"]["unit"] == "Mpc" and physical["radius"]["factor"] == 2.0
    resp = await client.post("/v1/datasets/units", headers=auth_headers,
                             json={"dataset_id": dataset, "clear": True})
    assert resp.json()["physical_units"]["columns"]["x"]["unit"] == "kpc"
