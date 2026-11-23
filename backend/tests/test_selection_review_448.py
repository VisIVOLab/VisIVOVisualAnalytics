"""Selections and diagnostics after the review of #439-#442 (issue #448).

Box and sphere selections on a big-endian VBT table (one mask, no indexing of
the frame), periodic wrapping across a simulation box's faces (the
out-of-core matcher, the point sessions, the DataFrame path), the
diagnostics as a task with progress and cancellation and a variance that
keeps its digits, and the SubFind types that are offered for opening."""
from __future__ import annotations

import asyncio
import sys
import threading
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from app import gadget_snapshot as gs
from app.periodic import box_inside, selection_period, separation
from app.schemas import TabularBox, TabularFilter, TabularSphere

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_gadget_snapshot import G, _snapshot  # noqa: E402


def _box(fields, lo, hi, op="inside", period=None):
    return TabularFilter(op=op, box=TabularBox(fields=fields, min=lo, max=hi, period=period))


def _sphere(fields, center, radius, op="inside", period=None):
    return TabularFilter(op=op, sphere=TabularSphere(fields=fields, center=center,
                                                     radius=radius, period=period))


# ── The periodic helpers ─────────────────────────────────────────────────────

def test_separation_and_box_wrap_round_the_period():
    v = np.array([0.5, 99.5, 50.0, np.nan])
    assert np.allclose(separation(v, 99.0, 100.0)[:3], [1.5, 0.5, -49.0])
    assert np.array_equal(box_inside(v, 99.0, 101.0, 100.0), [True, True, False, False])
    assert np.array_equal(box_inside(v, -1.0, 1.0, 100.0), [True, True, False, False])
    assert np.array_equal(box_inside(v, 0.0, 100.0, 100.0), [True, True, True, False])
    assert np.array_equal(box_inside(v, 99.0, 101.0, None), [False, True, False, False])
    assert selection_period(None, "x", {"x": 10.0}) == 10.0
    assert selection_period(0, "x", {"x": 10.0}) is None          # wrapping turned off
    assert selection_period(5, "vx", {"x": 10.0}) == 5.0
    assert selection_period(None, "vx", {"x": 10.0}) is None
    with pytest.raises(ValueError):
        selection_period(-1, "x", None)


# ── The DataFrame path: big-endian, no frame indexing, periodic ─────────────

def _big_endian_frame(n=5000, box=100.0):
    rng = np.random.default_rng(4)
    data = {name: rng.uniform(0, box, n).astype(">f4") for name in ("X", "Y", "Z")}
    data["Mass"] = rng.uniform(1, 2, n).astype(">f4")
    return pd.DataFrame(data)


def _brute(frame, center, radius, period=None):
    d2 = np.zeros(len(frame))
    for name, c in zip(("X", "Y", "Z"), center):
        delta = frame[name].to_numpy().astype(np.float64) - c
        if period:
            delta -= period * np.round(delta / period)
        d2 += delta * delta
    return np.flatnonzero(d2 <= radius * radius)


def test_spatial_selections_work_on_a_big_endian_table():
    from app.dependencies import _apply_tabular_query
    from app.schemas import TabularSortSpec
    frame = _big_endian_frame()
    want = _brute(frame, (50, 50, 50), 20)
    page, total = _apply_tabular_query(frame, [_sphere(["X", "Y", "Z"], [50, 50, 50], 20)],
                                       None, 100, 0)
    assert total == want.size and page.index.tolist() == want[:100].tolist()
    assert page["X"].dtype == np.dtype("float32")             # native order out
    # A box composed with a scalar filter, outside, and the general path (a
    # sort, a text filter) on the rows the spatial mask kept.
    flt = TabularFilter(field="Mass", op=">", value="1.5")
    page, total = _apply_tabular_query(
        frame, [_box(["X", "Y", "Z"], [0, 0, 0], [50, 50, 50], op="outside"), flt], None, 0, 0)
    x, y, z = (frame[c].to_numpy() for c in "XYZ")
    inside = (x <= 50) & (y <= 50) & (z <= 50)
    assert total == int((~inside & (frame["Mass"].to_numpy() > 1.5)).sum())
    page, total = _apply_tabular_query(
        frame, [_sphere(["X", "Y", "Z"], [50, 50, 50], 20)],
        TabularSortSpec(field="Mass", direction="desc"), 10, 0)
    assert total == want.size
    assert page["Mass"].tolist() == sorted(frame["Mass"].to_numpy()[want], reverse=True)[:10]
    page, total = _apply_tabular_query(
        frame, [_sphere(["X", "Y", "Z"], [50, 50, 50], 20),
                TabularFilter(field="Mass", op="contains", value="1.")], None, 0, 0)
    assert total == want.size


def test_a_sphere_at_the_box_edge_wraps_when_the_period_is_known():
    from app.dependencies import _apply_tabular_query
    frame = _big_endian_frame()
    corner = [1.0, 99.0, 50.0]
    plain, n_plain = _apply_tabular_query(frame, [_sphere(["X", "Y", "Z"], corner, 15)],
                                          None, 0, 0)
    wrapped, n_wrapped = _apply_tabular_query(
        frame, [_sphere(["X", "Y", "Z"], corner, 15, period=100.0)], None, 0, 0)
    assert n_plain == _brute(frame, corner, 15).size
    assert wrapped.index.tolist() == _brute(frame, corner, 15, 100.0).tolist()
    assert n_wrapped > 3 * n_plain                     # three quarters were lost
    # The dataset's own periods apply unless the selection states 0.
    _, n_default = _apply_tabular_query(frame, [_sphere(["X", "Y", "Z"], corner, 15)],
                                        None, 0, 0, periods={c: 100.0 for c in "XYZ"})
    _, n_off = _apply_tabular_query(frame, [_sphere(["X", "Y", "Z"], corner, 15, period=0)],
                                    None, 0, 0, periods={c: 100.0 for c in "XYZ"})
    assert (n_default, n_off) == (n_wrapped, n_plain)


# ── A Gadget snapshot: its BoxSize is the period ─────────────────────────────

def _edge_sphere(component, radius):
    """A sphere centred on the box face nearest the dark-matter halo."""
    x = component.column("x").astype(np.float64)
    centre = [float(x.min()) - 5.0, 500.0, 500.0]
    return centre, radius


def _gadget_brute(component, centre, radius, period):
    d2 = np.zeros(component.num_rows)
    for name, c in zip("xyz", centre):
        delta = component.column(name).astype(np.float64) - c
        if period:
            delta -= period * np.round(delta / period)
        d2 += delta * delta
    return np.flatnonzero(d2 <= radius * radius)


def test_the_out_of_core_matcher_wraps_round_the_snapshot_box(tmp_path, monkeypatch):
    from app.routers import catalogue as cat
    files, _ = _snapshot(tmp_path)
    ref = gs.gadget_ref(files[0], 1)
    dm = gs.resolve(ref)
    assert dm.periods() == {"x": 1000.0, "y": 1000.0, "z": 1000.0}
    # Move the halo across the x = 0 face: the box holds x in [0, 1000).
    shift = float(dm.column("x").astype(np.float64).mean())
    monkeypatch.setattr(cat, "PARTICLE_CHUNK_ROWS", 700)
    real_take = gs.Component.take

    def shifted(self, name, rows):
        out = real_take(self, name, rows)
        return np.mod(out - shift, 1000.0).astype(out.dtype) if name == "x" else out
    real_column = gs.Component.column

    def shifted_column(self, name):
        out = real_column(self, name)
        return np.mod(out - shift, 1000.0).astype(out.dtype) if name == "x" else out
    monkeypatch.setattr(gs.Component, "take", shifted)
    monkeypatch.setattr(gs.Component, "column", shifted_column)
    dm = gs.resolve(ref)
    centre = [0.0, float(dm.column("y").mean()), float(dm.column("z").mean())]
    want = _gadget_brute(dm, centre, 60.0, 1000.0)
    assert 0 < _gadget_brute(dm, centre, 60.0, None).size < want.size
    matching = cat._particle_matcher(dm, [_sphere(["x", "y", "z"], centre, 60.0)])
    got = np.concatenate([matching(s, min(dm.num_rows, s + 700))
                          for s in range(0, dm.num_rows, 700)])
    assert got.tolist() == want.tolist()
    box = cat._particle_matcher(dm, [_box(["x", "y", "z"],
                                          [-30.0, centre[1] - 30, centre[2] - 30],
                                          [30.0, centre[1] + 30, centre[2] + 30])])
    x = dm.column("x").astype(np.float64)
    in_x = (x >= 970.0) | (x <= 30.0)
    in_yz = np.ones(dm.num_rows, bool)
    for name, c in (("y", centre[1]), ("z", centre[2])):
        v = dm.column(name)
        in_yz &= (v >= c - 30) & (v <= c + 30)
    assert box(0, dm.num_rows).tolist() == np.flatnonzero(in_x & in_yz).tolist()


def test_an_isolated_run_has_no_period(tmp_path):
    e = "<"
    npart = [0, 3, 0, 0, 0, 0]
    head = G._gadget_header(npart, [0.0, 1.0, 0, 0, 0, 0], 2.5, 0.0, npart, 1,
                            100.0, 0.0, 0.0, 1.0, e)
    path = tmp_path / "isolated"
    path.write_bytes(G._gadget_block("HEAD", head, e)
                     + G._gadget_block("POS", np.zeros((3, 3), "<f4").tobytes(), e))
    assert gs.open_snapshot(path).component(1).periods() == {}


def test_the_point_session_masks_wrap_with_the_snapshot(tmp_path):
    from app.point_table import open_point_table
    from app.render_session.points import _filter_mask
    files, _ = _snapshot(tmp_path)
    table = open_point_table(gs.gadget_ref(files[0], 1))
    assert table.columns.column_periods == {"x": 1000.0, "y": 1000.0, "z": 1000.0}
    rows = np.arange(table.num_rows)
    x = np.asarray(table.columns["x"][rows], dtype=np.float64)
    # A box from x = 900 across the face to x = max - 1000 + 1: the halo
    # lies near the middle, so only wrapping reaches nothing here, while a
    # box spanning the whole period reaches every row.
    whole = {"box": {"fields": ["x", "y", "z"], "min": [-500, -500, -500],
                     "max": [500, 500, 500]}, "op": "inside"}
    assert _filter_mask(table.columns, [whole], rows).all()
    plain = {**whole, "box": {**whole["box"], "period": 0}}
    assert not _filter_mask(table.columns, [plain], rows)[x > 500].any()
    centre = [float(x.max()) + 1000.0, float(np.median(table.columns["y"][rows])),
              float(np.median(table.columns["z"][rows]))]
    sphere = {"sphere": {"fields": ["x", "y", "z"], "center": centre, "radius": 50.0},
              "op": "inside"}
    got = _filter_mask(table.columns, [sphere], rows)
    d2 = np.zeros(rows.size)
    for name, c in zip("xyz", centre):
        delta = np.asarray(table.columns[name][rows], dtype=np.float64) - c
        delta -= 1000.0 * np.round(delta / 1000.0)
        d2 += delta * delta
    assert got.any() and np.array_equal(got, d2 <= 2500.0)


# ── Diagnostics: variance, chunks, task ──────────────────────────────────────

def test_the_variance_keeps_its_digits_far_from_zero():
    from app.routers.catalogue import _diagnostic_accumulate
    values = 1e9 + np.random.default_rng(2).normal(0, 1e-3, 30000)

    def chunks():
        for start in range(0, values.size, 7000):
            yield values[start:start + 7000], None
    result = _diagnostic_accumulate(chunks, None, 8, False)
    assert result["stddev"] == pytest.approx(float(np.std(values)), rel=1e-6)
    assert result["mean"] == pytest.approx(float(values.mean()), rel=1e-15)


def test_progress_and_stop_between_chunks():
    from app.routers.catalogue import DiagnosticsCancelled, _diagnostic_accumulate
    values = np.arange(10.0)
    seen = []

    def chunks():
        for k in range(5):
            yield values[2 * k:2 * k + 2], None, (k + 1) / 5
    result = _diagnostic_accumulate(chunks, None, 5, False, progress=seen.append)
    assert result["finite_rows"] == 10 and seen[-1] == 1.0
    assert seen == sorted(seen) and seen[4] == pytest.approx(0.5)   # pass 1 of 2
    stop = threading.Event()
    stop.set()
    with pytest.raises(DiagnosticsCancelled):
        _diagnostic_accumulate(chunks, None, 5, False, stop=stop)


async def _open_vbt(client, auth_headers, tmp_path, n=6000):
    rng = np.random.default_rng(9)
    data = np.stack([rng.uniform(0, 100, n), rng.uniform(0, 100, n), rng.uniform(0, 100, n),
                     rng.uniform(1, 3, n)]).astype(">f4")
    binary = tmp_path / "be.bin"
    data.tofile(binary)
    (tmp_path / "be.bin.head").write_text(f"float\n4\n{n}\nbig\nX\nY\nZ\nMass\n")
    opened = (await client.post("/v1/catalogue/open",
                                json={"path": str(tmp_path / "be.bin.head"), "format": "vbt"},
                                headers=auth_headers)).json()
    assert opened["valid"], opened
    return opened["dataset_id"], data.astype(np.float64)


@pytest.mark.asyncio
async def test_the_diagnostics_route_and_task_agree_on_a_big_endian_vbt(
        client, auth_headers, tmp_path, monkeypatch):
    from app.routers import catalogue as cat
    monkeypatch.setattr(cat, "DIAGNOSTIC_CHUNK_ROWS", 1000)
    dataset_id, data = await _open_vbt(client, auth_headers, tmp_path)
    selection = [{"op": "inside", "sphere": {"fields": ["X", "Y", "Z"],
                                             "center": [50, 50, 50], "radius": 30}}]
    body = {"dataset_id": dataset_id, "field": "Mass", "weight_field": "X", "bins": 8,
            "filters": selection}
    direct = (await client.post("/v1/catalogue/diagnostics", json=body,
                                headers=auth_headers)).json()
    assert direct["valid"], direct
    d2 = ((data[:3] - 50.0) ** 2).sum(axis=0)
    mass = data[3][d2 <= 900.0]
    assert direct["selected_rows"] == mass.size
    assert direct["mean"] == pytest.approx(mass.mean(), rel=1e-9)
    assert direct["stddev"] == pytest.approx(mass.std(), rel=1e-9)
    assert sum(direct["counts"]) == pytest.approx(data[0][d2 <= 900.0].sum(), rel=1e-9)

    created = (await client.post("/v1/tasks/catalogue_diagnostics", json=body,
                                 headers=auth_headers)).json()
    assert created["valid"] and created["task_id"]
    for _ in range(200):
        status = (await client.get(f"/v1/tasks/{created['task_id']}",
                                   headers=auth_headers)).json()
        if status["status"] not in ("pending", "running"):
            break
        await asyncio.sleep(0.02)
    assert status["status"] == "completed" and status["progress"] == 1.0
    assert status["operation"] == "catalogue_diagnostics"
    assert status["result"] == direct

    bad = (await client.post("/v1/tasks/catalogue_diagnostics",
                             json={**body, "field": "nope"}, headers=auth_headers)).json()
    for _ in range(200):
        status = (await client.get(f"/v1/tasks/{bad['task_id']}", headers=auth_headers)).json()
        if status["status"] not in ("pending", "running"):
            break
        await asyncio.sleep(0.02)
    assert not status["result"]["valid"] and "Unknown field" in status["result"]["error"]


@pytest.mark.asyncio
async def test_a_diagnostics_task_can_be_cancelled(client, auth_headers, tmp_path, monkeypatch):
    from app.routers import catalogue as cat
    dataset_id, _ = await _open_vbt(client, auth_headers, tmp_path)
    gate, release = threading.Event(), threading.Event()
    real = cat._diagnostic_chunks

    def slow(entry, request):
        chunks = real(entry, request)

        def gated():
            for item in chunks():
                gate.set()
                release.wait(5)
                yield item
        return gated
    monkeypatch.setattr(cat, "_diagnostic_chunks", slow)
    created = (await client.post("/v1/tasks/catalogue_diagnostics",
                                 json={"dataset_id": dataset_id, "field": "Mass"},
                                 headers=auth_headers)).json()
    await asyncio.to_thread(gate.wait, 5)
    cancelled = await client.delete(f"/v1/tasks/{created['task_id']}", headers=auth_headers)
    assert cancelled.status_code == 200
    release.set()
    status = (await client.get(f"/v1/tasks/{created['task_id']}", headers=auth_headers)).json()
    assert status["status"] == "cancelled"


# ── SubFind: only the types with positions are offered ───────────────────────

def test_subfind_id_and_large_group_types_are_not_offered(tmp_path):
    e = "<"
    npart = [1, 1, 4, 0, 0, 0]
    head = G._gadget_header(npart, [0.0] * 6, 0.5, 1.0, npart, 1, 1000.0, 0.3, 0.7, 0.7, e)
    path = tmp_path / "sub_002.0"
    path.write_bytes(G._gadget_block("HEAD", head, e)
                     + G._gadget_block("GPOS", np.full((1, 3), 500, "<f4").tobytes(), e)
                     + G._gadget_block("SPOS", np.full((1, 3), 500, "<f4").tobytes(), e))
    snap = gs.open_snapshot(path)
    assert snap.is_subfind and snap.counts()[2] == 4
    assert [c["name"] for c in gs.gadget_components(path)] == ["FoF groups", "subhalos"]


# ── What decides that a box is periodic ──────────────────────────────────────

def _hdf5_box(path, *, periodic=None, comoving=True):
    h5py = pytest.importorskip("h5py")
    with h5py.File(path, "w") as h:
        hd = h.create_group("Header")
        hd.attrs["NumPart_ThisFile"] = np.array([0, 4, 0, 0, 0, 0], dtype=np.int32)
        hd.attrs["NumPart_Total"] = np.array([0, 4, 0, 0, 0, 0], dtype=np.uint32)
        hd.attrs["MassTable"] = np.array([0, 1.0, 0, 0, 0, 0])
        hd.attrs["NumFilesPerSnapshot"] = 1
        hd.attrs["BoxSize"] = 50.0
        h.create_group("Parameters").attrs["ComovingIntegrationOn"] = int(comoving)
        if periodic is not None:
            config = h.create_group("Config")
            config.attrs["NTYPES"] = 6
            if periodic:
                config.attrs["PERIODIC"] = 1
        h.create_group("PartType1").create_dataset(
            "Coordinates", data=np.random.default_rng(0).random((4, 3)) * 50)
    return gs.open_snapshot(path).component(1)


def test_gadget4_config_decides_periodicity_before_the_comoving_guess(tmp_path, monkeypatch):
    monkeypatch.delenv(gs.UNITS_ENV, raising=False)
    assert _hdf5_box(tmp_path / "a.hdf5", periodic=True, comoving=False).periods() == {
        "x": 50.0, "y": 50.0, "z": 50.0}
    assert _hdf5_box(tmp_path / "b.hdf5", periodic=False, comoving=True).periods() == {}
    assert _hdf5_box(tmp_path / "c.hdf5", periodic=None, comoving=True).periods() != {}
    assert _hdf5_box(tmp_path / "d.hdf5", periodic=None, comoving=False).periods() == {}


def test_the_environment_can_state_that_a_cosmological_run_is_not_periodic(
        tmp_path, monkeypatch):
    files, _ = _snapshot(tmp_path)
    assert gs.open_snapshot(files[0]).component(1).periods()
    monkeypatch.setenv(gs.UNITS_ENV, "periodic=0")
    assert gs.open_snapshot(files[0]).component(1).periods() == {}


def test_a_subfind_catalogue_without_groups_opens_on_its_subhalos(tmp_path):
    e = "<"
    npart = [0, 2, 5, 0, 0, 0]
    head = G._gadget_header(npart, [0.0] * 6, 0.5, 1.0, npart, 1, 1000.0, 0.3, 0.7, 0.7, e)
    path = tmp_path / "sub_003.0"
    path.write_bytes(G._gadget_block("HEAD", head, e)
                     + G._gadget_block("SPOS", np.full((2, 3), 500, "<f4").tobytes(), e))
    snap = gs.open_snapshot(path)
    assert snap.is_subfind and gs.default_type(snap) == 1
