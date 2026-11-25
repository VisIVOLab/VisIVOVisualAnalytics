"""RAMSES outputs read in place (issue #549).

Synthetic outputs written as RAMSES writes them (tests/_ramses_writer.py: two
processes, a refined mesh with the virtual grids RAMSES also writes, hydro
variables, particles in the old fixed layout, with descriptors and with the
header's field line; both byte orders) read back exactly; the same outputs go
through the routes a VBT point table or a Gadget snapshot takes. When the real
2018 output_00185 is at hand (a cosmological zoom, 16 processes, levels 7-15,
2,097,152 dark-matter particles, no descriptors) it is checked against what its
files must satisfy, and against yt when yt is installed."""
from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

from app import ramses_output as ro
from app.amr_source import AmrHierarchy

from . import _ramses_writer as W

REAL = Path("/Users/fvitello/Downloads/DemoData/Ramses/output_00185")


# ── Particles ────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("endian", ["<", ">"])
@pytest.mark.parametrize("layout", ["old", "header", "descriptor"])
def test_particles_round_trip_exactly(tmp_path, endian, layout):
    directory, exp = W.write_output(tmp_path, endian=endian, particles=layout)
    out = ro.open_output(directory)
    assert list(out.families()) == list(exp.particles)
    for slug, cols in exp.particles.items():
        comp = out.component(slug)
        assert comp.num_rows == len(cols["x"])
        for name in comp.names:
            got = np.asarray(comp.column(name))
            np.testing.assert_array_equal(got, cols[name].astype(comp.column_dtype(name)),
                                          err_msg=f"{slug}.{name}")
        # Rows out of order, repeated, and a scalar: as the catalogue routes ask.
        rows = np.array([comp.num_rows - 1, 0, comp.num_rows - 1, 0])
        np.testing.assert_array_equal(comp.take("x", rows), cols["x"][rows])
        assert comp.take("id", comp.num_rows - 1) == cols["id"][-1]
        lazy = comp.columns()
        np.testing.assert_array_equal(lazy["y"][:], cols["y"])
        np.testing.assert_array_equal(lazy["z"][np.arange(comp.num_rows) % 2 == 0],
                                      cols["z"][::2])


def test_the_layouts_name_their_columns(tmp_path):
    old = ro.open_output(W.write_output(tmp_path / "a", particles="old")[0]).component("dm")
    assert old.names == ["x", "y", "z", "vx", "vy", "vz", "mass", "id", "level", "birth_time",
                         "metallicity"]
    full = ro.open_output(W.write_output(tmp_path / "b", particles="descriptor")[0]).component("dm")
    assert full.names == ["x", "y", "z", "vx", "vy", "vz", "mass", "id", "level", "family",
                          "tag", "birth_time", "metallicity"]
    assert str(full.column_dtype("id")) == "int64" and str(old.column_dtype("id")) == "int32"
    assert str(full.column_dtype("family")) == "int8"


def test_families_come_from_the_family_record_or_the_birth_time(tmp_path):
    old = ro.open_output(W.write_output(tmp_path / "a", particles="old")[0])
    assert {k: f.total for k, f in old.families().items()} == {"dm": 7, "stars": 4}
    new = ro.open_output(W.write_output(tmp_path / "b", particles="descriptor")[0])
    assert {k: f.total for k, f in new.families().items()} == {"dm": 6, "stars": 4, "sinks": 1}
    assert [c["name"] for c in ro.components(new.path)] == ["dark matter", "stars", "sinks",
                                                            "gas cells"]
    assert new.describe()["families"] == {"dm": 6, "stars": 4, "sinks": 1}


def test_a_family_reference_names_the_output_and_the_family(tmp_path):
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    ref = ro.ramses_ref(directory, "stars")
    assert ro.split_ref(ref) == (str(directory), "stars")
    assert ro.split_ref(str(directory)) == (str(directory), None)
    assert ro.split_ref("/a/b::gadget-type=1")[1] is None
    assert ro.resolve(ref).num_rows == 4
    with pytest.raises(ValueError):
        ro.resolve(ro.ramses_ref(directory, "debris"))


def test_periods_and_units_of_a_component(tmp_path):
    directory, _ = W.write_output(tmp_path, particles="descriptor", aexp=0.5)
    comp = ro.open_output(directory).component("dm")
    assert comp.periods() == {"x": 1.0, "y": 1.0, "z": 1.0}
    phys = comp.physical()
    kpc = 3.085678e21
    assert phys["x"]["factor"] == pytest.approx(W.BOXLEN * W.UNIT_L / kpc)
    assert phys["vx"]["factor"] == pytest.approx(W.UNIT_L / W.UNIT_T / 1e5)
    assert phys["mass"]["factor"] == pytest.approx(W.UNIT_D * W.UNIT_L ** 3 / 1.989e33)
    assert phys["mass"]["unit"] == "Msun" and phys["x"]["unit"] == "kpc"
    assert "level" not in phys


# ── The mesh ─────────────────────────────────────────────────────────────────

def _leaf_map(blocks, fields):
    got = {}
    for b in blocks:
        for i in range(b.size):
            key = (b.level, tuple(float(c) for c in b.centre[i]))
            assert key not in got, f"leaf {key} twice"
            got[key] = [float(b.fields[f][i]) for f in fields]
    return got


@pytest.mark.parametrize("endian", ["<", ">"])
@pytest.mark.parametrize("descriptor", [False, True])
def test_leaf_cells_round_trip_exactly(tmp_path, endian, descriptor):
    directory, exp = W.write_output(tmp_path, endian=endian, hydro_descriptor=descriptor,
                                    particles=None)
    amr = ro.open_output(directory).amr()
    assert isinstance(amr, AmrHierarchy)
    fields = list(amr.fields)
    assert fields == (["density", "vx", "vy", "vz", "pressure", "metallicity"])
    got = _leaf_map(amr.leaf_blocks(fields), fields)
    want = {(lv, c): v for lv, c, v in exp.leaves}
    assert got == want
    assert amr.leaf_counts() == {1: 6, 2: 16}
    assert amr.n_leaf() == 22 and amr.n_leaf([2]) == 16
    assert amr.volume() == pytest.approx(1.0) and amr.levels == (1, 2)
    assert amr.extent == (1.0, 1.0, 1.0) and amr.uniform_level() is None


def test_a_hydro_descriptor_names_the_variables(tmp_path):
    directory, _ = W.write_output(tmp_path, hydro_descriptor=True, nvar=8, particles=None)
    names = ro.open_output(directory).amr().fields
    assert names == ("density", "vx", "vy", "vz", "pressure", "metallicity", "scalar_00",
                     "scalar_01")
    plain, _ = W.write_output(tmp_path / "p", nvar=8, particles=None)
    assert ro.open_output(plain).amr().fields == (
        "density", "vx", "vy", "vz", "pressure", "metallicity", "scalar_2", "scalar_3")


def test_selecting_levels_boxes_and_fields(tmp_path):
    directory, exp = W.write_output(tmp_path, particles=None)
    amr = ro.open_output(directory).amr()
    only2 = amr.leaf_cells(["pressure"], levels=[2])
    assert only2.size == 16 and set(only2.dx) == {0.25} and set(only2.level) == {2}
    want = sorted(v[4] for lv, _, v in exp.leaves if lv == 2)
    assert sorted(only2.fields["pressure"]) == want
    box = amr.leaf_cells([], lo=[0.0] * 3, hi=[0.5] * 3)
    assert box.size == 8 and (box.centre <= 0.5).all()     # the eight cells of the level-2 grid there
    assert amr.leaf_cells([], levels=[3]).size == 0
    with pytest.raises(ValueError, match="Unknown field"):
        list(amr.leaf_blocks(["temperature"]))
    with pytest.raises(ValueError, match="more than 5 leaf"):
        amr.leaf_cells([], max_cells=5)
    small = list(amr.leaf_blocks([], max_block=8))
    assert sum(b.size for b in small) == 22 and max(b.size for b in small) <= 8


def test_an_unrefined_mesh_is_a_uniform_grid(tmp_path):
    directory, exp = W.write_output(tmp_path, refine=False, particles=None)
    amr = ro.open_output(directory).amr()
    assert amr.leaf_counts() == {1: 8} and amr.uniform_level() == 1
    cells = amr.leaf_cells(["density"])
    # The cube's index along an axis is floor(centre / dx).
    idx = np.floor(cells.centre / cells.dx[:, None]).astype(int)
    assert sorted(map(tuple, idx)) == sorted((i, j, k) for i in (0, 1) for j in (0, 1)
                                             for k in (0, 1))


def test_mesh_units_are_the_info_files(tmp_path):
    directory, _ = W.write_output(tmp_path, particles=None)
    scales = ro.open_output(directory).amr().scales()
    assert scales["density"] == {"factor": pytest.approx(W.UNIT_D), "unit": "g/cm^3"}
    assert scales["pressure"]["factor"] == pytest.approx(W.UNIT_D * (W.UNIT_L / W.UNIT_T) ** 2)
    assert scales["dx"]["unit"] == "kpc"
    d = ro.open_output(directory).amr().describe()
    assert d["leaf_cells"] == 22 and d["levels"] == [1, 2] and d["redshift"] == pytest.approx(1.0)


def test_the_unit_table_is_what_scales_returns(tmp_path):
    from app import physical_units as pu
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    by_ref = pu.scales(ro.ramses_ref(directory, "dm"))
    by_dir = pu.scales(str(directory))
    assert by_ref["x"] == by_dir["x"] and by_dir["density"]["unit"] == "g/cm^3"
    assert pu.dataset_key(ro.ramses_ref(directory, "dm")) == pu.dataset_key(
        ro.ramses_ref(directory, "stars")) == os.path.realpath(directory)


# ── Damage ───────────────────────────────────────────────────────────────────

def test_a_missing_process_file_is_named(tmp_path):
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    (directory / "part_00001.out00002").unlink()
    with pytest.raises(ValueError, match="part file.*process 2 missing"):
        ro.open_output(directory).families()


def test_a_broken_record_is_refused(tmp_path):
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    f = directory / "part_00001.out00001"
    raw = bytearray(f.read_bytes())
    raw[12:16] = b"\xff\xff\xff\x7f"          # the second record's length marker
    f.write_bytes(raw)
    with pytest.raises(ValueError, match="broken record|unexpected end"):
        ro.open_output(directory).families()


def test_a_file_that_is_not_ramses_is_refused(tmp_path):
    d = tmp_path / "output_00003"
    d.mkdir()
    (d / "info_00003.txt").write_text("hello\n")
    assert not ro.is_ramses(d)
    assert ro.output_of(tmp_path) is None
    # An info file stating a absurd process count is no run (and is not looped over).
    directory, _ = W.write_output(tmp_path / "x", particles=None)
    info = directory / "info_00001.txt"
    info.write_text(info.read_text().replace("ncpu        =          2",
                                             "ncpu        =  1000000000"))
    with pytest.raises(ValueError, match="no RAMSES run"):
        ro.open_output(directory)


# ── Detection and the routes ─────────────────────────────────────────────────

def test_classify_takes_the_directory_or_any_file_of_it(tmp_path):
    from app.classify import classify_path
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    ref = ro.ramses_ref(directory, "dm")
    for target in (directory, directory / "info_00001.txt", directory / "amr_00001.out00002",
                   directory / "part_00001.out00001", directory / "header_00001.txt"):
        r = classify_path(target)
        assert (r["kind"], r["opener"], r["open_path"]) == ("particles", "catalogue", ref), target
        assert [c["name"] for c in r["components"]] == ["dark matter", "stars", "sinks",
                                                        "gas cells"]
    assert "RAMSES output 00001 (2 files)" in r["detail"] and "mesh of levels 1-2" in r["detail"]
    # A folder that is not one is still a folder; a file beside it is unaffected.
    plain = tmp_path / "plain"
    plain.mkdir()
    assert classify_path(plain)["kind"] == "directory"


def test_a_mesh_with_gas_opens_as_cells_and_one_without_has_no_viewer(tmp_path):
    from app.classify import classify_path
    directory, _ = W.write_output(tmp_path, particles=None)
    r = classify_path(directory)          # the gas cells of the mesh (issue #550)
    assert (r["kind"], r["opener"]) == ("particles", "catalogue")
    assert r["open_path"] == ro.ramses_ref(directory, "cells") and "gas cells" in r["detail"]
    for f in directory.glob("hydro_*"):
        f.unlink()
    r = classify_path(directory)
    assert (r["kind"], r["opener"]) == ("amr", "unsupported") and "no particles" in r["detail"]


def test_a_damaged_output_is_reported_not_raised(tmp_path):
    from app.classify import classify_path
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    (directory / "part_00001.out00002").unlink()
    r = classify_path(directory)
    assert r["opener"] == "unsupported" and "missing" in r["detail"]


def test_the_data_roots_cover_every_particle_file(tmp_path):
    from app.point_table import particle_dataset_path
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    refused = {str(directory / "part_00001.out00002")}
    with pytest.raises(PermissionError, match="outside"):
        particle_dataset_path(directory, lambda p: p not in refused)
    # The mesh files too: the classifier reads their headers.
    hydro = str(directory / "hydro_00001.out00001")
    with pytest.raises(PermissionError, match="outside"):
        particle_dataset_path(directory, lambda p: p != hydro)
    assert particle_dataset_path(directory, lambda p: True) == (
        ro.ramses_ref(directory, "dm"), "ramses")


def test_the_particle_seam_reads_a_family_like_a_snapshot_type(tmp_path):
    from app.point_sample import sample_points
    from app.point_table import (component_format, open_point_table, particle_catalogue_metadata,
                                 particle_components, particle_frame, particle_metadata,
                                 particle_rows, split_component)
    directory, exp = W.write_output(tmp_path, particles="descriptor")
    ref = ro.ramses_ref(directory, "dm")
    assert component_format(ref) == "ramses" and component_format(str(directory)) is None
    assert split_component(ref) == (str(directory), "::ramses-family=dm")
    assert [c["path"] for c in particle_components(ref)][0] == ref
    table = open_point_table(ref)
    assert table.axes == ("x", "y", "z") and table.num_rows == 6
    assert sample_points(table, 100).encode()
    rows = particle_rows(ref, [5, 0], ["x", "mass"])
    np.testing.assert_array_equal(rows["x"], exp.particles["dm"]["x"][[5, 0]])
    frame = particle_frame(ref)
    assert list(frame["id"]) == list(exp.particles["dm"]["id"])
    assert frame.attrs["units"]["x"].startswith("box units")
    meta = particle_metadata(ref)
    assert meta["snapshot"]["format"] == "ramses" and meta["component"]["family"] == "dm"
    assert meta["snapshot"]["redshift"] == pytest.approx(1.0)
    cat = particle_catalogue_metadata(ref)
    assert cat["format"] == "ramses" and cat["num_rows"] == 6
    assert {f["name"] for f in cat["fields"]} >= {"x", "id", "family"}


def test_series_and_availability_over_outputs(tmp_path):
    from app.point_table import has_component
    from app.snapshot_series import snapshot_series
    d1, _ = W.write_output(tmp_path, 1, particles="descriptor")
    d2, _ = W.write_output(tmp_path, 2, particles="old")
    d3, _ = W.write_output(tmp_path, 3, particles="old")
    s = snapshot_series(str(d2))
    assert s["paths"] == [str(d1), str(d2), str(d3)] and s["index"] == 1 and s["numbers"] == [1, 2, 3]
    assert [has_component(p, "::ramses-family=sinks") for p in s["paths"]] == [True, False, False]
    assert [has_component(p, "::ramses-family=dm") for p in s["paths"]] == [True, True, True]
    assert not has_component(tmp_path / "nowhere", "::ramses-family=dm")


@pytest.mark.asyncio
async def test_routes_open_and_draw_an_output(client, auth_headers, tmp_path):
    d1, exp = W.write_output(tmp_path / "run", 1, particles="descriptor")
    W.write_output(tmp_path / "run", 2, particles="descriptor", seed=3)
    r = (await client.post("/v1/files/classify", json={"path": str(d1)},
                           headers=auth_headers)).json()
    assert r["opener"] == "catalogue" and len(r["components"]) == 4, r
    cat = (await client.post("/v1/catalogue/open", json={"path": str(d1), "format": "csv"},
                             headers=auth_headers)).json()
    assert cat["valid"], cat
    assert cat["format"] == "ramses" and cat["particles"] and cat["num_rows"] == 6
    assert cat["path"] == ro.ramses_ref(d1, "dm")
    assert cat["coordinate_fields"]["system"] == "cartesian"
    assert cat["snapshot"]["box_size"] == W.BOXLEN
    assert {f["name"] for f in cat["fields"]} >= {"x", "vx", "mass", "family"}
    did = cat["dataset_id"]
    stars = (await client.post("/v1/catalogue/open",
                               json={"path": ro.ramses_ref(d1, "stars"), "format": "csv"},
                               headers=auth_headers)).json()
    assert stars["valid"] and stars["num_rows"] == 4
    info = (await client.post("/v1/points/info", json={"dataset_id": did},
                              headers=auth_headers)).json()
    assert info["particles"] and info["format"] == "ramses" and info["axes"] == ["x", "y", "z"]
    r = await client.post("/v1/points/sample_bin", json={"dataset_id": did, "max_points": 100,
                                                          "color_field": "mass"},
                          headers=auth_headers)
    assert r.status_code == 200 and len(r.content) == 6 * 8
    s = (await client.post("/v1/points/series", json={"dataset_id": did},
                           headers=auth_headers)).json()
    assert s["count"] == 2 and s["index"] == 0 and s["available"] == [True, True]
    assert s["paths"][1] == ro.ramses_ref(tmp_path / "run" / "output_00002", "dm")
    q = (await client.post("/v1/catalogue/query", json={
        "dataset_id": did, "filters": [{"field": "mass", "op": ">", "value": "0"}],
        "limit": 10}, headers=auth_headers)).json()
    assert q["valid"] and q["total_rows"] == 6, q
    both = (await client.post("/v1/catalogue/open", json={"path": str(d1), "paths": [str(d1), str(d1)],
                                                           "format": "csv"},
                              headers=auth_headers)).json()
    assert not both["valid"]


# ── The real output ──────────────────────────────────────────────────────────

needs_real = pytest.mark.skipif(not REAL.is_dir(), reason="the real RAMSES output is not here")


@needs_real
def test_the_real_output_particles():
    out = ro.open_output(REAL)
    assert out.info.ncpu == 16 and (out.info.levelmin, out.info.levelmax) == (7, 15)
    assert not out.layout()[1][0].fields.get("family")          # old layout: no descriptors
    assert {k: f.total for k, f in out.families().items()} == {"dm": 2_097_152}
    comp = out.component("dm")
    assert comp.num_rows == 2_097_152 == out.header.total
    pos = [np.asarray(comp.column(c)) for c in "xyz"]
    assert all(p.min() >= 0.0 and p.max() < 1.0 for p in pos)
    assert len(np.unique(np.asarray(comp.column("id")))) == comp.num_rows
    # The box's mass is omega_m in these units: the dark matter holds omega_m - omega_b of it.
    assert float(np.asarray(comp.column("mass")).sum()) == pytest.approx(
        (0.308 - 0.045) / 0.308, rel=1e-2)


@needs_real
def test_the_real_output_mesh():
    out = ro.open_output(REAL)
    amr = out.amr()
    assert out.hydro_names() == ("density", "vx", "vy", "vz", "pressure", "metallicity")
    counts = amr.leaf_counts()
    assert all(7 <= lv <= 15 for lv, n in counts.items() if n)
    assert sum(counts.values()) == 15_974_645
    # The leaves tile the box: their volumes sum to boxlen^3 (1 in box units).
    assert amr.volume() * out.info.boxlen ** 3 == pytest.approx(out.info.boxlen ** 3, rel=1e-12)
    # A bounded read of a level subset: the gas of the deepest level.
    deep = amr.leaf_cells(["density"], levels=[13])
    assert deep.size == counts[13] and (deep.fields["density"] > 0).all()
    assert ((deep.centre > 0) & (deep.centre < 1)).all()
    # Baryon fraction: sum(rho dV) over the box is omega_b / omega_m of the mass.
    gas = sum(float(np.sum(b.fields["density"]) * b.dx ** 3)
              for b in amr.leaf_blocks(["density"]))
    assert gas == pytest.approx(0.045 / 0.308, rel=1e-2)


@needs_real
def test_the_real_output_against_yt():
    yt = pytest.importorskip("yt")
    ds = yt.load(str(REAL / "info_00185.txt"))
    ad = ds.all_data()
    out = ro.open_output(REAL)
    pos = np.asarray(out.component("dm").columns()["x"][:1000])
    ytx = ad["DM", "particle_position_x"].to("code_length").d
    assert set(np.round(pos, 12)) <= set(np.round(ytx / ds.domain_width[0].to("code_length").d, 12))
    assert ad["gas", "cell_volume"].shape[0] == out.amr().n_leaf()


# ── Review fixes (PR #559) ───────────────────────────────────────────────────

def _entry(directory, slug="dm"):
    return {"kind": "catalogue", "format": "ramses", "path": ro.ramses_ref(directory, slug),
            "paths": [str(directory)]}


def test_a_ramses_projected_map_is_mass_weighted_in_physical_units(tmp_path):
    pytest.importorskip("numba")
    from app.routers.particle_render import ParticleRenderRequest, _prepare
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    cam = {"position": (0, 0, 10), "focal_point": (0, 0, 0), "view_up": (0, 1, 0)}
    req = ParticleRenderRequest(dataset_id="d", mode="projected", cameras=[cam], x="x", y="y",
                                z="z")
    units = _prepare(_entry(directory), req)["map_units"]
    assert units["weighting"] == "mass" and units["density_unit"] == "Msun/kpc^2"
    assert units["comoving"] is False and units["hubble_units"] is False
    assert units["scale_factor"] == pytest.approx(0.5)
    assert units["redshift"] == pytest.approx(1.0)
    assert units["kpc_per_unit"] > 0


def test_a_layer_names_a_ramses_family(tmp_path):
    from app.routers import particle_layers as router
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    session = None                                 # a layer by family never asks the session
    entry = _entry(directory)
    layers = [router.ParticleLayer(family=slug) for slug in ("stars", "sinks")]
    paths = [router.layer_entry(entry, lay, session)["path"] for lay in layers]
    assert paths == [ro.ramses_ref(directory, "stars"), ro.ramses_ref(directory, "sinks")]
    with pytest.raises(ValueError, match="only one"):
        router.ParticleLayer(family="dm", gadget_type=1)
    with pytest.raises(router.HTTPException) as err:     # a Gadget type on a RAMSES output
        router.layer_entry(entry, router.ParticleLayer(gadget_type=1), session)
    assert err.value.status_code == 422
    other = {**entry, "path": str(tmp_path / "plain.vbt")}
    with pytest.raises(router.HTTPException) as err:     # a family on anything else
        router.layer_entry(other, router.ParticleLayer(family="dm"), session)
    assert err.value.status_code == 422
    with pytest.raises(router.HTTPException):            # a name that is no family
        router.layer_entry(entry, router.ParticleLayer(family="nonsense"), session)


def test_a_refused_path_is_not_parsed(tmp_path, monkeypatch):
    from app.classify import _classify_ramses, classify_path
    from app.point_table import particle_dataset_path
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    parsed = []
    for name in ("parse_info", "parse_header", "parse_descriptor"):
        real = getattr(ro, name)
        monkeypatch.setattr(ro, name, lambda *a, _r=real, _n=name, **k:
                            parsed.append(_n) or _r(*a, **k))
    ro._cached.cache_clear()
    for refused in ("info_00001.txt", "header_00001.txt", "part_file_descriptor.txt",
                    "amr_00001.out00001"):
        target = str(directory / refused)
        with pytest.raises(PermissionError) as err:
            particle_dataset_path(directory, lambda p, t=target: p != t)
        assert refused in str(err.value) and str(tmp_path) not in str(err.value)
        with pytest.raises(PermissionError):
            _classify_ramses(directory / "info_00001.txt", lambda p, t=target: p != t)
    # The folder is shown as a plain directory, nothing of it read.
    assert classify_path(directory, lambda p: "part_" not in p)["kind"] == "directory"
    assert parsed == []


def test_a_rewritten_file_is_not_served_from_the_cache(tmp_path):
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    first = ro.open_output(directory)
    assert ro.open_output(directory) is first
    part = directory / "part_00001.out00001"
    data = part.read_bytes()
    os.utime(part, ns=(1, 1))                     # same size, another time
    assert ro.open_output(directory) is not first
    again = ro.open_output(directory)
    part.write_bytes(data + b"\0" * 8)            # another size
    assert ro.open_output(directory) is not again


def test_a_stray_info_or_header_file_is_no_ramses_output(tmp_path):
    from app.classify import classify_path
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    stray = tmp_path / "stray"
    stray.mkdir()
    for name in ("info_00007.txt", "header_00007.txt"):
        (stray / name).write_bytes((directory / "info_00001.txt").read_bytes())
    assert ro.output_of(stray / "info_00007.txt") is None
    assert ro.output_of(stray) is None
    assert classify_path(stray / "info_00007.txt")["kind"] != "particles"
    # With a data file of its own number it is one; of another number it is not.
    (stray / "part_00008.out00001").write_bytes(b"x")
    assert ro.output_of(stray / "info_00007.txt") is None
    (stray / "amr_00007.out00001").write_bytes(b"x")
    assert ro.output_of(stray / "info_00007.txt") == stray


def test_one_single_precision_particle_keeps_its_fields(tmp_path):
    recs = [(0, 4)] * 7 + [(0, 4), (0, 4)] + [(0, 4)] * 12
    class Reader:
        order = "<"
        map = np.array([1.0], dtype="<f4").tobytes()
        def array(self, offset, dtype, count):
            return np.frombuffer(self.map, dtype=dtype, count=count, offset=offset)
    has = ro.RamsesOutput._has_nsink
    assert has(recs, 1, Reader()) is False      # a position's bits
    Reader.map = np.array([0], dtype="<i4").tobytes()
    assert has(recs, 1, Reader()) is True       # nsink = 0
    assert has(recs, 5, None) is True
    assert has([(0, 4)] * 7 + [(0, 40)], 5, None) is False
