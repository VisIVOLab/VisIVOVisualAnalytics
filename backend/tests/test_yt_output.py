"""Other simulation codes through yt (issue #554): app/yt_output.py.

yt is optional, so what needs it is skipped without it. The mapping of yt's
grids to leaf blocks is tested on synthetic datasets (``yt.load_amr_grids``,
``yt.load_uniform_grid``: no downloads); the detection, the point seam, the
classifier and the routes are tested with the mesh patched in, and without yt
for the 503. A run of real outputs of the codes (VISIVO_YT_SAMPLES, a folder of
yt's sample data) is checked when it is there; the native RAMSES reader is
cross-checked against yt's on the writer of tests/_ramses_writer.py.
"""
from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import amr_source, yt_output as yo
from app.amr_cells import AmrCells
from app.compute import amr_views as av

from app import ramses_output as ro

from . import _ramses_writer as W

needs_yt = pytest.mark.skipif(not yo.yt_available(), reason="yt is not installed")
SAMPLES = Path(os.environ.get("VISIVO_YT_SAMPLES", "/nonexistent"))
needs_samples = pytest.mark.skipif(not SAMPLES.is_dir(), reason="VISIVO_YT_SAMPLES is not set")

KPC = 3.085678e21
MSUN = 1.989e33


def _ds(refine_by=2, box=(4, 4, 4), child=True, periodic=False):
    """Four root cells a side (1 kpc each) and a level-1 grid over the middle
    2 x 2 x 2 root cells: 56 root leaves and 64 level-1 leaves."""
    import yt
    yt.set_log_level(50)
    root = dict(left_edge=[0, 0, 0], right_edge=list(box), level=0, dimensions=[4, 4, 4])
    root["density"] = (1.0 + np.arange(64, dtype=float).reshape(4, 4, 4), "g/cm**3")
    root["velocity_x"] = (np.full((4, 4, 4), 1e5), "cm/s")
    grids = [root]
    if child:
        sub = dict(left_edge=[1, 1, 1], right_edge=[3, 3, 3], level=1, dimensions=[4, 4, 4])
        sub["density"] = (np.full((4, 4, 4), 100.0), "g/cm**3")
        sub["velocity_x"] = (np.full((4, 4, 4), 2e5), "cm/s")
        grids.append(sub)
    return yt.load_amr_grids(grids, [4, 4, 4], bbox=np.array([[0, b] for b in box]),
                             length_unit="kpc", refine_by=refine_by,
                             periodicity=(periodic,) * 3)


def _mesh(**kw):
    return yo.YtGridAmr(_ds(**kw), "enzo")


# ── the mapping ──────────────────────────────────────────────────────────────

@needs_yt
def test_leaves_are_the_cells_no_finer_grid_covers_and_tile_the_box():
    h = _mesh()
    assert h.code == "yt:enzo" and h.extent == (4.0, 4.0, 4.0)
    assert h.leaf_counts() == {0: 56, 1: 64} and h.levels == (0, 1)
    assert h.volume() == pytest.approx(64.0) and h.uniform_level() is None
    assert h.fields == ("density", "vx")
    seen = set()
    for block in h.leaf_blocks(["density"]):
        for c in block.centre:
            seen.add((block.level, tuple(float(v) for v in c)))
    assert len(seen) == 120
    # No root cell the child grid covers is a leaf; every level-1 cell is.
    assert not any(lv == 0 and all(1 < v < 3 for v in c) for lv, c in seen)
    assert (1, (1.25, 1.25, 1.25)) in seen and (0, (0.5, 0.5, 0.5)) in seen


@needs_yt
def test_values_follow_the_cells_and_are_read_in_physical_units():
    h = _mesh()
    root = next(h.leaf_blocks(["density", "vx"], levels=[0]))
    # Root cell (i, j, k) holds 1 + 16 i + 4 j + k (x fastest in yt's index).
    first = {tuple(c): v for c, v in zip(root.centre, root.fields["density"])}
    assert first[(0.5, 0.5, 0.5)] == pytest.approx(1.0)
    assert first[(3.5, 3.5, 3.5)] == pytest.approx(64.0)
    assert first[(0.5, 0.5, 3.5)] == pytest.approx(4.0)
    np.testing.assert_allclose(root.fields["vx"], 1.0)            # 1e5 cm/s in km/s
    fine = next(h.leaf_blocks(["density", "vx"], levels=[1]))
    np.testing.assert_allclose(fine.fields["density"], 100.0)
    np.testing.assert_allclose(fine.fields["vx"], 2.0)
    assert fine.dx == 0.5 and fine.size == 64


@needs_yt
def test_a_window_a_level_and_a_block_size_select_leaves_only():
    h = _mesh()
    window = h.leaf_cells(["density"], lo=[0, 0, 0], hi=[1, 1, 1])
    assert set(window.level) == {0} and window.size == 1
    only = h.leaf_cells((), levels=[1])
    assert set(only.level) == {1} and only.size == 64
    parts = list(h.leaf_blocks(["density"], levels=[0], max_block=10))
    assert [p.size for p in parts] == [10, 10, 10, 10, 10, 6]
    assert sum(p.fields["density"].sum() for p in parts) == pytest.approx(
        next(h.leaf_blocks(["density"], levels=[0])).fields["density"].sum())
    with pytest.raises(ValueError, match="Unknown field"):
        next(h.leaf_blocks(["nope"]))
    assert h.finest_level_in([0, 0, 0], [0.5, 0.5, 0.5]) == 0
    assert h.finest_level_in([1.2, 1.2, 1.2], [1.3, 1.3, 1.3]) == 1


@needs_yt
def test_units_the_box_unit_is_a_root_cell_and_a_mass_is_density_times_its_volume():
    import yt
    h = _mesh()
    s = h.scales()
    assert s["x"]["unit"] == "kpc" and s["x"]["factor"] == pytest.approx(1.0, rel=1e-5)
    assert s["dx"] == s["x"] and s["density"] == {"factor": 1.0, "unit": "g/cm**3"}
    assert s["vx"]["unit"] == "km/s" and s["mass"]["unit"] == "Msun"
    cells = h.leaf_cells(["density"])
    mass = float(np.sum(cells.fields["density"] * cells.dx ** 3)) * s["mass"]["factor"]
    truth = float(h.ds.all_data()[("gas", "cell_mass")].sum().to("Msun"))
    assert mass == pytest.approx(truth, rel=2e-3)        # the app's Msun is 1.989e33 g
    # The cell table and the view agree on the units.
    assert yt.__version__
    d = h.describe()
    assert d["box_size"]["unit"] == "kpc" and d["box_size"]["value"] == pytest.approx(4.0, rel=1e-5)
    assert d["leaf_cells"] == 120 and d["uniform_level"] is None and d["code"] == "yt:enzo"


@needs_yt
def test_a_mesh_of_a_unit_the_box_is_not_astronomical_in_keeps_cgs():
    import yt
    root = dict(left_edge=[0, 0, 0], right_edge=[1, 1, 1], level=0, dimensions=[4, 4, 4],
                density=(np.ones((4, 4, 4)), "g/cm**3"),
                velocity_x=(np.full((4, 4, 4), 5.0), "cm/s"))
    ds = yt.load_amr_grids([root], [4, 4, 4], bbox=np.array([[0, 1]] * 3), length_unit="cm")
    s = yo.YtGridAmr(ds, "flash").scales()
    assert s["x"] == {"factor": 0.25, "unit": "cm"} and s["vx"]["unit"] == "cm/s"
    assert s["mass"]["unit"] == "g" and s["mass"]["factor"] == pytest.approx(0.25 ** 3)


@needs_yt
def test_a_uniform_grid_is_one_level_and_its_cube_is_the_cells():
    import yt
    values = np.arange(64, dtype=float).reshape(4, 4, 4) + 1
    ds = yt.load_uniform_grid({"density": (values, "g/cm**3")}, [4, 4, 4],
                              bbox=np.array([[0, 4]] * 3), length_unit="kpc")
    h = yo.YtGridAmr(ds, "flash")
    assert h.uniform_level() == 0 and h.leaf_counts() == {0: 64}
    plan = av.plan_uniform(h, "density", 0)
    slabs = []
    av.resample(h, plan, slabs.append)
    cube = np.concatenate([np.asarray(s_) for s_ in slabs], axis=0)
    assert cube.shape == (4, 4, 4) and h.describe()["uniform_level"] == 0
    np.testing.assert_allclose(np.sort(cube.ravel()), np.arange(1.0, 65.0))
    seen = {}
    for block in h.leaf_blocks(["density"]):
        for c, v in zip(block.centre, block.fields["density"]):
            seen[tuple(float(x) for x in c)] = v
    assert seen[(0.5, 0.5, 0.5)] == 1.0 and seen[(3.5, 3.5, 3.5)] == 64.0
    assert len(seen) == 64
    # yt hands physical values (factor 1): the cube's BUNIT names that unit,
    # not a code unit the numbers are not in.
    hdr = av.build_header(h, plan, "x")
    assert hdr["AMRFCT"] == 1.0 and not str(hdr["BUNIT"]).startswith("code")
    assert hdr["BUNIT"] == hdr["AMRUNIT"]


@needs_yt
def test_the_views_work_on_it_the_cube_conserves_mass_and_the_level_map_tiles():
    h = _mesh()
    cells = h.leaf_cells(["density"])
    mass = float(np.sum(cells.fields["density"] * cells.dx ** 3))
    for level in (0, 1):
        plan = av.plan_uniform(h, "density", level)
        total = 0.0

        def sink(slab):
            nonlocal total
            total += float(slab.astype(np.float64).sum())

        av.resample(h, plan, sink)
        assert total * plan.dx ** 3 == pytest.approx(mass, rel=1e-6), level
    m = av.level_map(h, "z", 2.0)
    assert m["area"] == pytest.approx(16.0) and m["counts"] == {"0": 12, "1": 16}
    box = av.plan_view(h, "density", None, [1.0, 1.0, 1.0], [2.0, 2.0, 2.0], "box", 1 << 20)[1]
    assert box["finest_level_in_region"] == 1


@needs_yt
def test_the_cells_are_a_point_table_with_their_size_and_units():
    h = _mesh()
    cells = AmrCells(h, h, yo.yt_ref("/m/out", yo.CELLS), False)
    assert cells.num_rows == 120 and cells.names[:5] == ["x", "y", "z", "dx", "level"]
    assert "mass" in cells.names and cells.size_column == "dx" and cells.periods() == {}
    rows = np.arange(cells.num_rows)
    np.testing.assert_array_equal(cells.take("dx", rows), 2.0 ** -cells.take("level", rows))
    assert cells.physical()["x"]["unit"] == "kpc"


@needs_yt
@pytest.mark.parametrize("kw, text", [({"refine_by": 4}, "refine by 2"),
                                      ({"box": (4, 4, 8)}, "not cubes")])
def test_a_mesh_that_cannot_be_mapped_is_refused_saying_why(kw, text):
    import yt
    if "refine_by" in kw:
        # refine_by 4: a level-1 grid has cells of a quarter root cell.
        ds = _ds(refine_by=4)
        with pytest.raises(ValueError, match=text):
            yo.YtGridAmr(ds, "enzo")
    else:
        root = dict(left_edge=[0, 0, 0], right_edge=[4, 4, 8], level=0, dimensions=[4, 4, 4],
                    density=(np.ones((4, 4, 4)), "g/cm**3"))
        ds = yt.load_amr_grids([root], [4, 4, 4], bbox=np.array([[0, 4], [0, 4], [0, 8]]))
        with pytest.raises(ValueError, match=text):
            yo.YtGridAmr(ds, "enzo")


@needs_yt
def test_a_two_dimensional_mesh_is_refused():
    import yt
    root = dict(left_edge=[0, 0, 0], right_edge=[1, 1, 1], level=0, dimensions=[4, 4, 1],
                density=(np.ones((4, 4, 1)), "g/cm**3"))
    ds = yt.load_amr_grids([root], [4, 4, 1], bbox=np.array([[0, 1]] * 3))
    with pytest.raises(ValueError, match="3-D"):
        yo.YtGridAmr(ds, "flash")


# ── the cross-check of the native RAMSES reader ──────────────────────────────

@needs_yt
def test_ramses_through_yt_gives_the_native_readers_leaves_and_values(tmp_path):
    directory, exp = W.write_output(tmp_path, particles=None)
    out = ro.open_output(directory)
    native = out.amr()
    yt_mesh = yo.open_mesh(str(directory / "info_00001.txt"), any_frontend=True)
    assert isinstance(yt_mesh, yo.YtOctreeAmr)
    shift = out.info.levelmin                    # yt counts levels from the coarse mesh
    assert {lv: n for lv, n in native.leaf_counts().items() if n} == {
        lv + shift: n for lv, n in yt_mesh.leaf_counts().items()}
    assert yt_mesh.volume() == pytest.approx(float(np.prod(yt_mesh.extent)))

    def table(h, level_shift, scale):
        rows = {}
        for b in h.leaf_blocks(["density", "vx", "pressure"]):
            for k, c in enumerate(b.centre):
                key = (b.level + level_shift, tuple(round(float(v) * scale, 6) for v in c))
                rows[key] = tuple(float(b.fields[f][k]) for f in ("density", "vx", "pressure"))
        return rows

    n = table(native, 0, 2.0 ** shift)           # native boxes: a fraction of the run's
    y = table(yt_mesh, shift, 1.0)
    assert n.keys() == y.keys()
    unit_d, kms = out.info.unit_d, out.info.unit_l / out.info.unit_t / 1e5
    for key in n:
        np.testing.assert_allclose(y[key][0], n[key][0] * unit_d, rtol=1e-6)
        np.testing.assert_allclose(y[key][1], n[key][1] * kms, rtol=1e-6)
    # The mass of the gas, in Msun, agrees to rounding.
    def mass(h):
        s = h.scales()["mass"]["factor"]
        return s * sum(float(np.nansum(b.fields["density"].astype(np.float64)) * b.dx ** 3)
                       for b in h.leaf_blocks(["density"]))
    assert mass(yt_mesh) == pytest.approx(mass(native), rel=1e-6)


@needs_yt
def test_a_window_narrower_than_a_coarse_octree_cell_still_finds_it(tmp_path):
    """yt's own box selector misses a coarse cell a narrow box lies inside: the
    window is padded and the centres are judged here."""
    directory, _ = W.write_output(tmp_path, particles=None)
    mesh = yo.open_mesh(str(directory / "info_00001.txt"), any_frontend=True)
    everything = mesh.leaf_cells(["density"])
    for lo, hi in (([0.3, -np.inf, -np.inf], [0.7, np.inf, np.inf]), ([0.3] * 3, [0.7] * 3),
                   ([0.4, 0, 0], [0.7, 2, 2]), ([0.0, 0.0, 0.0], [1.0, 1.0, 1.0])):
        want = np.ones(everything.size, dtype=bool)
        for d in range(3):
            want &= (everything.centre[:, d] >= lo[d]) & (everything.centre[:, d] <= hi[d])
        got = mesh.leaf_cells(["density"], lo=lo, hi=hi)
        assert got.size == int(want.sum()), (lo, hi)
        assert sorted(map(tuple, got.centre)) == sorted(map(tuple, everything.centre[want]))


@needs_yt
def test_a_mesh_whose_box_does_not_start_at_zero_is_counted_from_its_corner():
    import yt
    root = dict(left_edge=[10, -5, 3], right_edge=[14, -1, 7], level=0, dimensions=[4, 4, 4],
                density=(np.ones((4, 4, 4)), "g/cm**3"))
    ds = yt.load_amr_grids([root], [4, 4, 4], bbox=np.array([[10, 14], [-5, -1], [3, 7]]),
                           length_unit="kpc")
    h = yo.YtGridAmr(ds, "enzo")
    cells = h.leaf_cells(["density"], lo=[0, 0, 0], hi=[1, 1, 1])
    assert cells.size == 1 and tuple(cells.centre[0]) == (0.5, 0.5, 0.5)
    assert h.describe()["origin"]["code_length"] == [10.0, -5.0, 3.0]


def test_the_data_roots_see_every_file_of_a_mesh_and_a_symlinked_folder(tmp_path):
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "cell_D_00000").write_bytes(b"x")
    plot = tmp_path / "root" / "plt00010"
    (plot / "Level_1").mkdir(parents=True)
    (plot / "Header").write_text("HyperCLaw-V1.1\n")
    (plot / "Level_0").symlink_to(outside)
    roots = str(tmp_path / "root")
    allowed = lambda f: os.path.realpath(f).startswith(roots)         # noqa: E731
    with pytest.raises(PermissionError, match="cell_D_00000"):
        yo.check_allowed(plot, allowed)
    yo.check_allowed(plot, None)


def test_a_mesh_with_too_many_files_cannot_be_checked(tmp_path, monkeypatch):
    plot = tmp_path / "plt"
    plot.mkdir()
    for i in range(5):
        (plot / f"f{i}").write_bytes(b"x")
    monkeypatch.setattr(yo, "MAX_FILES", 3)
    with pytest.raises(ValueError, match="cannot be checked"):
        yo.check_allowed(plot, lambda f: True)
    assert len(yo.files(plot)) >= 4                      # an identity takes what it has


# ── detection, the point seam, the classifier and the routes ─────────────────

def _fake_mesh_file(tmp_path, name="flash_hdf5_chk_0001"):
    path = tmp_path / name
    path.write_bytes(b"\x89HDF\r\n\x1a\n" + b"\0" * 64)
    return path


@pytest.fixture
def patched(tmp_path, monkeypatch):
    """A file that looks like an HDF5 mesh, and yt "taking" it as FLASH with
    the synthetic mesh (no yt needed by the patch itself)."""
    pytest.importorskip("yt")
    path = _fake_mesh_file(tmp_path)
    mesh = _mesh()
    monkeypatch.setattr(yo, "detect", lambda p: "flash" if str(p).startswith(str(path)) else None)
    monkeypatch.setattr(yo, "open_mesh", lambda p, allowed=None, **kw: mesh
                        if str(yo.mesh_path(p)) == str(path) else None)
    yo._cells_cached.cache_clear()
    return path, mesh


def test_a_reference_names_the_cells_of_a_mesh():
    ref = yo.yt_ref("/data/chk_0001", yo.CELLS)
    assert yo.split_ref(ref) == ("/data/chk_0001", "cells")
    assert yo.split_ref("/data/chk_0001") == ("/data/chk_0001", None)
    assert yo.split_ref("/data/chk::yt-family=stars") == ("/data/chk::yt-family=stars", None)
    from app.point_table import component_format, split_component
    assert component_format(ref) == "yt" and split_component(ref) == ("/data/chk_0001",
                                                                    "::yt-family=cells")


@needs_yt
def test_the_cells_open_through_the_particle_seam(patched):
    from app import physical_units
    from app.point_table import (has_component, open_point_table, particle_catalogue_metadata,
                                 particle_components, particle_dataset_path, particle_rows)
    path, _ = patched
    ref = yo.yt_ref(path, yo.CELLS)
    assert particle_dataset_path(str(path)) == (ref, "yt")
    assert particle_dataset_path(ref) == (ref, "yt")
    assert [c["name"] for c in particle_components(ref)] == ["gas cells"]
    assert has_component(str(path), "::yt-family=cells")
    table = open_point_table(ref)
    assert table.num_rows == 120 and "dx" in table.columns
    assert list(particle_rows(ref, [0, 1], ["x", "density"]).columns) == ["x", "density"]
    meta = particle_catalogue_metadata(ref)
    assert meta["format"] == "yt" and meta["num_rows"] == 120
    assert meta["component"]["size_column"] == "dx"
    s = physical_units.scales(ref)
    assert s["x"]["unit"] == "kpc" and s["mass"]["unit"] == "Msun"


@needs_yt
def test_a_mesh_without_a_density_has_no_cells(patched, monkeypatch):
    from app.point_table import particle_dataset_path
    path, mesh = patched
    monkeypatch.setattr(type(mesh), "fields", property(lambda self: ("temperature",)))
    with pytest.raises(ValueError, match="no gas density"):
        particle_dataset_path(str(path))


@needs_yt
def test_the_classifier_offers_the_cells_of_a_refined_mesh_and_a_cube_for_a_uniform_one(
        patched, tmp_path, monkeypatch):
    from app.classify import classify_path
    path, mesh = patched
    r = classify_path(path)
    assert (r["kind"], r["opener"], r["open_path"]) == ("particles", "catalogue",
                                                        yo.yt_ref(path, yo.CELLS))
    assert "FLASH output (read through yt)" in r["detail"] and "120 leaf cells" in r["detail"]
    assert [c["name"] for c in r["components"]] == ["gas cells"]
    monkeypatch.setattr(type(mesh), "uniform_level", lambda self: 1)
    cube = classify_path(path)
    assert (cube["kind"], cube["opener"], cube["open_path"]) == ("cube", "dataset", str(path))
    assert "uniform grid of 8 x 8 x 8" in cube["detail"]


def test_nothing_is_claimed_without_yt_and_the_routes_say_503(tmp_path, monkeypatch):
    from app.classify import classify_path
    path = _fake_mesh_file(tmp_path)
    monkeypatch.setattr(yo, "yt_available", lambda: False)
    assert yo.detect(path) is None and yo.uniform_field(path) is None
    assert classify_path(path)["kind"] != "particles"          # the generic HDF5 verdict stands
    with pytest.raises(yo.YtUnavailable, match="not installed"):
        amr_source.open_hierarchy(str(path))
    # A file that cannot be a mesh is no 503: it holds no mesh.
    text = tmp_path / "notes.txt"
    text.write_text("hello")
    with pytest.raises(ValueError, match="no AMR mesh"):
        amr_source.open_hierarchy(str(text))


@pytest.mark.asyncio
async def test_the_amr_routes_answer_503_without_yt(client, auth_headers, tmp_path, monkeypatch):
    path = _fake_mesh_file(tmp_path)
    monkeypatch.setattr(yo, "yt_available", lambda: False)
    for route in ("plan", "uniform", "level-map"):
        body = {"path": str(path)} if route != "level-map" else {"path": str(path), "coordinate": 0.5}
        r = await client.post(f"/v1/simulations/amr/{route}", json=body, headers=auth_headers)
        assert r.status_code == 503 and "yt" in r.json()["detail"], route


@needs_yt
@pytest.mark.asyncio
async def test_the_plan_and_level_map_routes_work_on_a_mesh(
        client, auth_headers, patched, monkeypatch):
    path, mesh = patched
    monkeypatch.setattr(amr_source, "open_hierarchy", lambda p, allowed=None: mesh)
    plan = (await client.post("/v1/simulations/amr/plan", json={"path": str(path),
                                                                  "field": "density"},
                              headers=auth_headers)).json()
    assert plan["valid"] and plan["code"] == "yt:enzo" and plan["plan"]["level"] == 1
    assert plan["plan"]["length_unit"] == "kpc"
    lm = (await client.post("/v1/simulations/amr/level-map", json={
        "path": str(path), "axis": "z", "coordinate": 2.0}, headers=auth_headers)).json()
    assert lm["valid"] and lm["area"] == pytest.approx(16.0)


# ── real outputs of the codes ────────────────────────────────────────────────

REAL = {"flash": "Sedov3D/Sedov_3d/sedov_hdf5_chk_0003",
        "enzo": "ActiveParticleTwoSphere/ActiveParticleTwoSphere/DD0011/DD0011",
        "gamer": "Plummer/Plummer/plummer_000000",
        "athena": "ShockCloud/ShockCloud/id0/Cloud.0050.vtk",
        "amrex": "nyx_sedov_plt00086/nyx_sedov_plt00086"}


@needs_yt
@needs_samples
@pytest.mark.parametrize("frontend", sorted(REAL))
def test_real_outputs_are_detected_tile_the_box_and_conserve_mass(frontend):
    path = SAMPLES / REAL[frontend]
    assert yo.detect(path) == frontend
    h = amr_source.open_hierarchy(str(path))
    assert h.volume() == pytest.approx(float(np.prod(h.extent)))
    mass = sum(float(np.sum(b.fields["density"].astype(np.float64)) * b.dx ** 3)
               for b in h.leaf_blocks(["density"]))
    plan = av.plan_uniform(h, "density", h.levels[-1], max_voxels=1 << 27)
    total = 0.0

    def sink(slab):
        nonlocal total
        total += float(slab.astype(np.float64).sum())

    av.resample(h, plan, sink)
    assert total * plan.dx ** 3 == pytest.approx(mass, rel=1e-6)


@needs_yt
@needs_samples
def test_a_two_dimensional_real_output_is_refused_and_ramses_is_not_claimed():
    wind = SAMPLES / "WindTunnel/WindTunnel/windtunnel_4lev_hdf5_plt_cnt_0030"
    assert yo.detect(wind) == "flash"
    with pytest.raises(ValueError, match="3-D"):
        amr_source.open_hierarchy(str(wind))
    ramses = SAMPLES / "ramses_new_format/ramses_new_format/output_00002"
    assert yo.detect(ramses) is None
