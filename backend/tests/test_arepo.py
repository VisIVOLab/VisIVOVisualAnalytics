"""AREPO snapshot inspection, extraction planning, and the routes.

The extraction itself needs yt, which is optional; everything up to it — the
HDF5 tree, the box size, the grid arithmetic and the FITS header — is exercised
here against a synthetic snapshot, because those are the parts that were wrong
in the legacy extractor and the parts a deployment without yt still runs.
"""
from __future__ import annotations

import os

import numpy as np
import pytest

from app.compute.arepo import (
    KPC_IN_CM,
    ArepoError,
    build_header,
    domain_width_kpc,
    inspect_snapshot,
    plan_extraction,
)

_TEST_TOKEN = "test-token-visivo-12345"


def _client():
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient

    from app.main import app
    return TestClient(app), {"X-Visivo-Token": _TEST_TOKEN}


@pytest.fixture
def snapshot(tmp_path):
    """A minimal AREPO-shaped HDF5: a 100 kpc box with one gas particle type."""
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "snap_000.hdf5"
    with h5py.File(path, "w") as handle:
        parameters = handle.create_group("Parameters")
        # 100 kpc expressed in code units of 1 kpc.
        parameters.attrs["BoxSize"] = np.float64(100.0)
        parameters.attrs["UnitLength_in_cm"] = np.float64(KPC_IN_CM)
        parameters.attrs["ComovingIntegrationOn"] = np.int32(0)

        header = handle.create_group("Header")
        header.attrs["NumPart_Total"] = np.array([8, 0, 0, 0, 0, 0], dtype=np.int32)
        header.attrs["Time"] = np.float64(0.5)
        header.attrs["Composition"] = np.bytes_(b"gas")

        gas = handle.create_group("PartType0")
        gas.create_dataset("Coordinates", data=np.zeros((8, 3), dtype=np.float32))
        gas.create_dataset("Density", data=np.ones(8, dtype=np.float32))
        gas.create_dataset("Masses", data=np.ones(8, dtype=np.float32))
    return path


# ── inspection ────────────────────────────────────────────────────────────────

def test_inspect_reads_the_tree_box_size_and_particle_types(snapshot):
    result = inspect_snapshot(str(snapshot))
    assert result["box_size_kpc"] == pytest.approx(100.0)
    assert result["unit_length_cm"] == pytest.approx(KPC_IN_CM)
    assert result["particle_types"]["PartType0"] == ["Coordinates", "Density", "Masses"]

    names = {child["name"] for child in result["tree"]["children"]}
    assert {"Parameters", "Header", "PartType0"} <= names
    gas = next(c for c in result["tree"]["children"] if c["name"] == "PartType0")
    coordinates = next(c for c in gas["children"] if c["name"] == "Coordinates")
    assert coordinates["kind"] == "dataset"
    assert coordinates["shape"] == [8, 3]


def test_inspect_carries_attributes_in_a_json_safe_form(snapshot):
    result = inspect_snapshot(str(snapshot))
    header = next(c for c in result["tree"]["children"] if c["name"] == "Header")
    # numpy scalars, byte strings and small arrays must all survive as JSON.
    assert header["attributes"]["Time"] == pytest.approx(0.5)
    assert header["attributes"]["Composition"] == "gas"
    assert header["attributes"]["NumPart_Total"] == [8, 0, 0, 0, 0, 0]


def test_inspect_refuses_a_file_that_is_not_hdf5(tmp_path):
    path = tmp_path / "not-a-snapshot.hdf5"
    path.write_text("this is not HDF5")
    with pytest.raises(ArepoError):
        inspect_snapshot(str(path))


def test_domain_width_is_the_box_in_kpc(snapshot):
    assert domain_width_kpc(str(snapshot)) == pytest.approx((100.0, 100.0, 100.0))


def test_a_snapshot_without_a_declared_box_is_refused(tmp_path):
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "headerless.hdf5"
    with h5py.File(path, "w") as handle:
        handle.create_group("PartType0")
    with pytest.raises(ArepoError) as exc:
        domain_width_kpc(str(path))
    assert "BoxSize" in str(exc.value)


# ── planning ──────────────────────────────────────────────────────────────────

def test_the_cell_size_is_the_level_not_the_window():
    """A covering grid's cell is domain / (base · 2^level); ``dims`` only says
    how many of them to take. Dividing the window by the rounded cell count —
    which the legacy script did, and this module first copied — gives 769 pc
    where yt builds 781 pc cells, so the WCS does not describe the data."""
    plan = plan_extraction(7, [20.0, 20.0, 20.0], [0.0, 0.0, 0.0], [100.0, 100.0, 100.0])
    assert plan.cell_size_pc[0] == pytest.approx(100_000.0 / 128)   # 781.25 pc
    # 20 kpc / 0.78125 kpc = 25.6 → 26 cells, so the extent is 20.3125 kpc.
    assert plan.shape == (26, 26, 26)
    assert plan.cells == 26**3
    assert plan.extent_kpc[0] == pytest.approx(26 * 100.0 / 128)
    # The request is reported too, so the rounding is visible rather than silent.
    assert plan.window_kpc[0] == pytest.approx(20.0)


def test_a_finer_base_grid_gives_finer_cells():
    coarse = plan_extraction(7, [20.0, 20.0, 20.0], [0, 0, 0], [100.0, 100.0, 100.0],
                             base_dimensions=(1, 1, 1))
    fine = plan_extraction(7, [20.0, 20.0, 20.0], [0, 0, 0], [100.0, 100.0, 100.0],
                           base_dimensions=(2, 2, 2))
    assert fine.cell_size_pc[0] == pytest.approx(coarse.cell_size_pc[0] / 2)
    assert fine.shape[0] == pytest.approx(2 * coarse.shape[0], abs=1)


def test_a_window_larger_than_the_box_is_refused():
    with pytest.raises(ArepoError) as exc:
        plan_extraction(5, [200.0, 20.0, 20.0], [0, 0, 0], [100.0, 100.0, 100.0])
    assert "larger than the simulation box" in str(exc.value)


def test_an_absurd_request_cannot_overflow_past_the_cell_limit():
    # np.prod on int64 wrapped to zero here, which sailed straight through the
    # limit; math.prod on Python ints cannot.
    with pytest.raises(ArepoError):
        plan_extraction(14, [100.0, 100.0, 100.0], [0, 0, 0], [100.0, 100.0, 100.0],
                        base_dimensions=(256, 256, 256))


def test_a_level_means_the_same_thing_for_any_window():
    # The point of scaling by the window fraction: doubling the window at the
    # same level doubles the cell count, leaving the resolution alone.
    small = plan_extraction(7, [10.0, 10.0, 10.0], [0, 0, 0], [100.0, 100.0, 100.0])
    large = plan_extraction(7, [20.0, 20.0, 20.0], [0, 0, 0], [100.0, 100.0, 100.0])
    assert large.shape[0] == pytest.approx(2 * small.shape[0], abs=1)
    assert large.cell_size_pc[0] == pytest.approx(small.cell_size_pc[0], rel=0.05)


def test_an_anisotropic_window_gives_an_anisotropic_grid():
    plan = plan_extraction(7, [20.0, 20.0, 5.0], [0, 0, 0], [100.0, 100.0, 100.0])
    assert plan.shape[0] == plan.shape[1]
    assert plan.shape[2] < plan.shape[0]
    assert plan.cell_size_pc[2] == pytest.approx(plan.cell_size_pc[0], rel=0.1)


def test_a_window_smaller_than_one_cell_still_gives_a_cell():
    plan = plan_extraction(0, [0.001, 0.001, 0.001], [0, 0, 0], [100.0, 100.0, 100.0])
    assert plan.shape == (1, 1, 1)


@pytest.mark.parametrize("kwargs, message", [
    ({"level": -1}, "zero or more"),
    ({"level": 99}, "limit"),
    ({"window_kpc": [0.0, 10.0, 10.0]}, "greater than zero"),
    ({"window_kpc": [-5.0, 10.0, 10.0]}, "greater than zero"),
    ({"window_kpc": [10.0, 10.0]}, "three numbers"),
    ({"window_kpc": [float("nan"), 10.0, 10.0]}, "finite"),
    ({"domain_width_kpc": [0.0, 0.0, 0.0]}, "no size"),
])
def test_an_unusable_plan_is_refused_with_a_reason(kwargs, message):
    call = dict(level=7, window_kpc=[20.0, 20.0, 20.0], offset_kpc=[0.0, 0.0, 0.0],
                domain_width_kpc=[100.0, 100.0, 100.0])
    call.update(kwargs)
    with pytest.raises(ArepoError) as exc:
        plan_extraction(**call)
    assert message in str(exc.value)


def test_an_oversized_grid_is_refused_before_anything_is_allocated():
    with pytest.raises(ArepoError) as exc:
        plan_extraction(13, [100.0, 100.0, 100.0], [0, 0, 0], [100.0, 100.0, 100.0])
    assert "cells" in str(exc.value)
    assert "GiB" in str(exc.value)


# ── the FITS header, which the legacy got wrong ───────────────────────────────

def test_the_header_describes_the_window_that_was_actually_extracted():
    """The legacy wrote a hardcoded 25 × 25 × 5 kpc box into every header."""
    plan = plan_extraction(7, [20.0, 20.0, 5.0], [1.0, 2.0, 3.0], [100.0, 100.0, 100.0])
    header = build_header(plan, source="/data/snap_000.hdf5", particle="gas",
                          field_name="density", bunit="g/cm**3")

    for axis in (1, 2, 3):
        assert header[f"CUNIT{axis}"] == "pc"
        assert header[f"CDELT{axis}"] == pytest.approx(plan.cell_size_pc[axis - 1])
        # The full axis spans the extent that was extracted, not somebody
        # else's box — and not the requested window either, which is rounded.
        span = header[f"CDELT{axis}"] * plan.shape[axis - 1]
        assert span == pytest.approx(plan.extent_kpc[axis - 1] * 1000.0)


def test_the_reference_pixel_is_the_centre_of_the_array():
    # CRPIX = 0 (the legacy's value) is outside a 1-based FITS array.
    plan = plan_extraction(7, [20.0, 20.0, 20.0], [0, 0, 0], [100.0, 100.0, 100.0])
    header = build_header(plan, source="s.hdf5", particle="gas", field_name="density", bunit="")
    for axis in (1, 2, 3):
        n = plan.shape[axis - 1]
        assert header[f"CRPIX{axis}"] == pytest.approx((n + 1) / 2.0)
        assert header[f"CRVAL{axis}"] == 0.0
        assert 1 <= header[f"CRPIX{axis}"] <= n


def test_the_header_records_where_the_cube_came_from():
    plan = plan_extraction(6, [10.0, 12.0, 4.0], [0.5, -1.5, 2.0], [100.0, 100.0, 100.0])
    header = build_header(plan, source="/data/snap_042.hdf5", particle="gas",
                          field_name="temperature", bunit="K")
    assert header["AREPOSRC"] == "snap_042.hdf5"
    assert header["AREPOLVL"] == 6
    assert header["AREPOPRT"] == "gas"
    assert header["AREPOFLD"] == "temperature"
    assert header["BUNIT"] == "K"
    # AREPOW* is what was extracted, AREPOR* what was asked for.
    assert header["AREPOWX"] == pytest.approx(plan.extent_kpc[0])
    assert header["AREPOWZ"] == pytest.approx(plan.extent_kpc[2])
    assert header["AREPORX"] == pytest.approx(10.0)
    assert header["AREPORZ"] == pytest.approx(4.0)
    assert header["AREPOOY"] == pytest.approx(-1.5)


def test_a_field_without_a_unit_gets_no_bunit():
    plan = plan_extraction(5, [10.0, 10.0, 10.0], [0, 0, 0], [100.0, 100.0, 100.0])
    header = build_header(plan, source="s.hdf5", particle="gas", field_name="f", bunit="")
    assert "BUNIT" not in header


# ── routes ────────────────────────────────────────────────────────────────────

def test_inspect_route_returns_the_tree(snapshot, monkeypatch):
    client, auth = _client()
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(snapshot.parent))
    r = client.post("/v1/simulations/arepo/inspect", json={"path": str(snapshot)}, headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"] is True
    assert body["box_size_kpc"] == pytest.approx(100.0)
    assert "PartType0" in body["particle_types"]
    assert isinstance(body["yt_available"], bool)


def test_plan_route_reports_the_grid(snapshot, monkeypatch):
    client, auth = _client()
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(snapshot.parent))
    r = client.post("/v1/simulations/arepo/plan",
                    json={"path": str(snapshot), "level": 7,
                          "window_kpc": [20.0, 20.0, 20.0], "offset_kpc": [0.0, 0.0, 0.0]},
                    headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["shape"] == [26, 26, 26]
    assert body["cells"] == 26**3
    assert body["cell_size_pc"][0] == pytest.approx(100_000.0 / 128)
    assert body["domain_width_kpc"] == pytest.approx([100.0, 100.0, 100.0])


def test_plan_route_refuses_an_oversized_grid(snapshot, monkeypatch):
    client, auth = _client()
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(snapshot.parent))
    r = client.post("/v1/simulations/arepo/plan",
                    json={"path": str(snapshot), "level": 13,
                          "window_kpc": [100.0, 100.0, 100.0]},
                    headers=auth)
    assert r.status_code == 422


def test_extract_route_without_yt_says_so(snapshot, monkeypatch):
    from app.compute.arepo import yt_available

    if yt_available():
        pytest.skip("yt is installed here, so the 503 path cannot be exercised")
    client, auth = _client()
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(snapshot.parent))
    r = client.post("/v1/simulations/arepo/extract",
                    json={"path": str(snapshot), "level": 5, "fields": ["density"]},
                    headers=auth)
    assert r.status_code == 503
    assert "yt" in r.json()["detail"]


def test_routes_require_auth(snapshot):
    client, _ = _client()
    assert client.post("/v1/simulations/arepo/inspect",
                       json={"path": str(snapshot)}).status_code in (401, 403)


# ── regressions from the review passes ────────────────────────────────────────

def test_a_recursive_hard_link_does_not_hang_the_walk(tmp_path):
    """A group linked into itself is an infinite tree. It used to recurse until
    Python gave up: the handle id differs between two aliases of one group, so
    the identity has to be the object's address in the file."""
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "cyclic.hdf5"
    with h5py.File(path, "w") as handle:
        parameters = handle.create_group("Parameters")
        parameters.attrs["BoxSize"] = np.float64(10.0)
        parameters.attrs["UnitLength_in_cm"] = np.float64(KPC_IN_CM)
        group = handle.create_group("loop")
        group["self"] = group          # hard link to itself

    result = inspect_snapshot(str(path))
    assert result["valid"] if "valid" in result else True
    loop = next(c for c in result["tree"]["children"] if c["name"] == "loop")
    inner = next(c for c in loop["children"] if c["name"] == "self")
    assert "…" in inner["attributes"]


def test_the_same_group_linked_twice_is_still_listed_twice(tmp_path):
    # Cycle detection must not suppress a legitimate alias: the identity is
    # tracked as an ancestor set, not as "seen anywhere".
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "aliased.hdf5"
    with h5py.File(path, "w") as handle:
        shared = handle.create_group("shared")
        shared.create_dataset("values", data=np.zeros(4, dtype=np.float32))
        handle["alias"] = shared

    result = inspect_snapshot(str(path))
    named = {c["name"]: c for c in result["tree"]["children"]}
    assert [child["name"] for child in named["shared"]["children"]] == ["values"]
    assert [child["name"] for child in named["alias"]["children"]] == ["values"]


def test_an_external_link_is_named_but_not_followed(tmp_path):
    """An external link points into another file, which this request was never
    authorised to read."""
    h5py = pytest.importorskip("h5py")
    other = tmp_path / "elsewhere.hdf5"
    with h5py.File(other, "w") as handle:
        handle.create_group("secret").create_dataset("values", data=np.ones(3))

    path = tmp_path / "linking.hdf5"
    with h5py.File(path, "w") as handle:
        handle["outside"] = h5py.ExternalLink(str(other), "/secret")

    result = inspect_snapshot(str(path))
    outside = next(c for c in result["tree"]["children"] if c["name"] == "outside")
    assert outside["children"] == []
    assert "external link" in outside["attributes"]
    assert "not followed" in outside["attributes"]["external link"]


def test_an_attribute_that_json_cannot_carry_is_neutralised(tmp_path):
    import json

    h5py = pytest.importorskip("h5py")
    path = tmp_path / "attrs.hdf5"
    with h5py.File(path, "w") as handle:
        group = handle.create_group("odd")
        group.attrs["infinite"] = np.array([1.0, np.inf, np.nan])
        group.attrs["compound"] = np.array([(1, 2.5)], dtype=[("a", "i4"), ("b", "f8")])

    result = inspect_snapshot(str(path))
    # The whole reply has to survive serialisation, which is where a NaN or a
    # numpy record shows up — after all the work is done.
    json.dumps(result)
    odd = next(c for c in result["tree"]["children"] if c["name"] == "odd")
    assert odd["attributes"]["infinite"] == [1.0, None, None]


def test_the_tree_listing_is_bounded(tmp_path):
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "wide.hdf5"
    with h5py.File(path, "w") as handle:
        group = handle.create_group("many")
        for i in range(6000):
            group.create_group(f"g{i:05d}")

    result = inspect_snapshot(str(path))
    assert result["tree_truncated"] is True
