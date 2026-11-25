"""The legacy VisIVO Importer's raw formats read in place (issue #551): the
rawPointsDesc, rawGridsDesc and flyDesc descriptors and a bare FLY file. The
files are written here, in both byte orders, to the layout the importer's own
readers specify (Importers/rawpointssource.cpp, rawgridsource.cpp,
flysource.cpp): the values read back are the values written."""
from __future__ import annotations

import base64
import zlib

import numpy as np
import pytest

from app import raw_descriptor as rd
from app.classify import classify_path
from app.point_table import (component_format, is_particle_format, open_point_table,
                             particle_dataset_path, particle_frame, particle_metadata,
                             split_component)
from app.snapshot_series import snapshot_series
from app.volume_source import (field_dataset_path, field_listing, open_fits, raw_field,
                               volume_data, volume_shape)

N = 700
BOXES = "50 50 50"


def _order(endian: str, code: str) -> np.dtype:
    return np.dtype(("<" if endian == "little" else ">") + code)


def _flag(endian: str) -> str:
    return "l" if endian == "little" else "b"


def _points(seed: int, scalars: int = 2, n: int = N):
    rng = np.random.default_rng(seed)
    pos = rng.uniform(0.0, 50.0, (n, 3))
    fields = [rng.lognormal(0.0, 1.0, n) for _ in range(scalars)]
    return pos, fields


def write_rawpoints(directory, names, endian, kind="Float", scalars=2, header="rawPointsDesc"):
    """One file per name (xyz interleaved, then each scalar as a block of N), and
    the descriptor listing them with times 0.0, 0.5, ..."""
    code = "f8" if kind.lower().startswith("d") else "f4"
    dt = _order(endian, code)
    truth = {}
    lines = [header, "dark", kind, "time", str(N), BOXES, _flag(endian)]
    for k, name in enumerate(names):
        pos, fields = _points(k + 1, scalars)
        with open(directory / name, "wb") as fh:
            fh.write(pos.astype(dt).tobytes())
            for f in fields:
                fh.write(f.astype(dt).tobytes())
        truth[name] = (pos.astype(code), [f.astype(code) for f in fields])
        lines.append(f"{k * 0.5} {name}")
    desc = directory / "points.desc"
    desc.write_text("\n".join(lines) + "\n")
    return desc, truth


def write_rawgrids(directory, names, endian, kind="Float", dims=(5, 4, 3), header="rawGridsDesc"):
    code = "f8" if kind.lower().startswith("d") else "f4"
    dt = _order(endian, code)
    truth = {}
    lines = [header, "density", str(len(dims)), kind, *map(str, dims), "time", _flag(endian)]
    for k, name in enumerate(names):
        a = np.random.default_rng(10 + k).normal(size=tuple(reversed(dims))).astype(code)
        a.astype(dt).tofile(directory / name)
        truth[name] = a
        lines.append(f"{0.7 * k} {name}")
    desc = directory / "grid.desc"
    desc.write_text("\n".join(lines) + "\n")
    return desc, truth


def write_fly(path, endian, code="f4", n=N):
    """x y z of every particle interleaved, then vx vy vz."""
    rng = np.random.default_rng(3)
    pos, vel = rng.uniform(0.0, 50.0, (n, 3)), rng.normal(0.0, 100.0, (n, 3))
    dt = _order(endian, code)
    with open(path, "wb") as fh:
        fh.write(pos.astype(dt).tobytes())
        fh.write(vel.astype(dt).tobytes())
    return pos.astype(code), vel.astype(code)


# ── rawpoints ────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("endian", ["little", "big"])
@pytest.mark.parametrize("kind", ["Float", "Double"])
def test_rawpoints_round_trip(tmp_path, endian, kind):
    desc, truth = write_rawpoints(tmp_path, ["a_096", "a_104", "a_112"], endian, kind)
    d = rd.descriptor(desc)
    assert (d.kind, d.rows, d.box, d.endian) == ("rawpoints", N, (50.0, 50.0, 50.0), endian)
    assert [t for t, _ in d.entries] == [0.0, 0.5, 1.0]
    for k, name in enumerate(["a_096", "a_104", "a_112"]):
        rf = rd.points_file(rd.points_ref(desc, k))
        assert rf.names == ["X", "Y", "Z", "scalar0", "scalar1"] and rf.time == 0.5 * k
        cols = rf.columns()
        pos, fields = truth[name]
        for j, axis in enumerate("XYZ"):
            np.testing.assert_array_equal(cols[axis], pos[:, j])
        for j, f in enumerate(fields):
            np.testing.assert_array_equal(cols[f"scalar{j}"], f)
        assert cols["X"].dtype.str[1:] == ("f8" if kind == "Double" else "f4")


def test_the_scalar_count_is_what_the_size_holds(tmp_path):
    desc, _ = write_rawpoints(tmp_path, ["a"], "little", scalars=0)
    assert rd.points_file(rd.points_ref(desc)).names == ["X", "Y", "Z"]
    desc, _ = write_rawpoints(tmp_path, ["a"], "little", scalars=5)
    assert len(rd.points_file(rd.points_ref(desc)).names) == 8
    (tmp_path / "a").write_bytes(b"\0" * (4 * N * 3 + 7))
    with pytest.raises(ValueError, match="holds"):
        rd.points_file(rd.points_ref(desc))
    (tmp_path / "a").write_bytes(b"\0" * (4 * N * 2))
    with pytest.raises(ValueError, match="3 \\+ n blocks"):
        rd.points_file(rd.points_ref(desc))


def test_the_manuals_example_with_a_leading_id_reads(tmp_path):
    """The importer's manual has an id line before the variable's name."""
    desc, _ = write_rawpoints(tmp_path, ["a"], "big")
    text = desc.read_text().replace("rawPointsDesc\n", "rawPointsDesc\n20\n")
    desc.write_text(text)
    d = rd.descriptor(desc)
    assert (d.name, d.rows, d.endian) == ("dark", N, "big")


def test_a_table_of_points_opens_through_the_particle_layer(tmp_path):
    desc, truth = write_rawpoints(tmp_path, ["a", "b"], "big", "Double")
    found = particle_dataset_path(desc)
    assert found == (rd.points_ref(desc, 0), "rawpoints")
    ref = found[0]
    assert component_format(ref) == "rawpoints" and is_particle_format("rawpoints")
    assert split_component(ref) == (str(desc), rd.POINTS_SEP + "0")
    table = open_point_table(ref)
    assert (table.num_rows, table.axes) == (N, ("X", "Y", "Z"))
    np.testing.assert_array_equal(table.columns["Y"], truth["a"][0][:, 1])
    frame = particle_frame(ref)
    assert list(frame.columns) == ["X", "Y", "Z", "scalar0", "scalar1"]
    np.testing.assert_array_equal(frame["scalar1"].to_numpy(), truth["a"][1][1])
    meta = particle_metadata(rd.points_ref(desc, 1))["snapshot"]
    assert (meta["time"], meta["step"], meta["steps"], meta["byte_order"]) == (0.5, 2, 2, "big")
    # A grid's descriptor is not a table of points.
    gdesc, _ = write_rawgrids(tmp_path, ["g"], "little")
    assert particle_dataset_path(gdesc) is None
    assert particle_dataset_path(tmp_path / "a") is None


def test_the_series_is_the_descriptors_list(tmp_path):
    names = ["16ml_096", "16ml_104", "16ml_112"]
    desc, _ = write_rawpoints(tmp_path, names, "little")
    s = snapshot_series(rd.points_ref(desc, 1))
    assert s["paths"] == [rd.points_ref(desc, k) for k in range(3)]
    assert (s["numbers"], s["index"], s["count"]) == ([0.0, 0.5, 1.0], 1, 3)
    (tmp_path / "16ml_104").unlink()
    s = snapshot_series(rd.points_ref(desc, 2))
    assert (s["count"], s["index"]) == (2, 1)
    s = snapshot_series(rd.points_ref(desc, 2), allowed=lambda p: not p.endswith("16ml_096"))
    assert (s["count"], s["index"]) == (1, 0)


def test_a_file_outside_the_allowed_roots_is_refused(tmp_path):
    desc, _ = write_rawpoints(tmp_path, ["a"], "little")
    with pytest.raises(PermissionError):
        particle_dataset_path(desc, allowed=lambda p: False)
    assert particle_dataset_path(desc, allowed=lambda p: True) is not None


def test_remote_files_are_not_fetched(tmp_path):
    desc, _ = write_rawpoints(tmp_path, ["a"], "little")
    desc.write_text(desc.read_text() + "1.0 http://example.org/x\n")
    with pytest.raises(ValueError, match="remote"):
        rd.descriptor(desc)


# ── rawgrids ─────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("endian", ["little", "big"])
@pytest.mark.parametrize("kind", ["Float", "Double"])
def test_rawgrids_round_trip_as_volumes(tmp_path, endian, kind):
    names = ["j.f064.a", "j.f064.b"]
    desc, truth = write_rawgrids(tmp_path, names, endian, kind)
    for k, name in enumerate(names):
        ref = rd.grid_ref(desc, k)
        assert volume_shape(ref) == ((3, 4, 5), k)
        raw = raw_field(ref)
        assert raw.kind == "rawgrid" and raw.shape == (3, 4, 5)
        with volume_data(ref) as data:
            np.testing.assert_array_equal(np.asarray(data), truth[name])
        with open_fits(ref) as hdul:
            assert hdul[0].header["CTYPE1"] == "X" and hdul[0].header["RAWTIME"] == 0.7 * k
            np.testing.assert_array_equal(np.asarray(hdul[0].data), truth[name])
    fields, name = field_listing(rd.grid_ref(desc, 1))
    assert [f["path"] for f in fields] == [rd.grid_ref(desc, 0), rd.grid_ref(desc, 1)]
    assert name == fields[1]["name"] and "t=0.7" in name
    assert field_dataset_path(rd.grid_ref(desc, 1)) == rd.grid_ref(desc, 1)
    assert field_dataset_path(rd.grid_ref(desc, 5)) is None


def test_a_grids_type_is_checked_against_its_size(tmp_path):
    desc, _ = write_rawgrids(tmp_path, ["g"], "little", "Double")
    desc.write_text(desc.read_text().replace("Double", "Float"))
    with pytest.raises(ValueError, match="float64"):
        rd.grid_file(rd.grid_ref(desc))


def test_a_two_dimensional_grid_is_an_image_and_a_line_is_refused(tmp_path):
    desc, truth = write_rawgrids(tmp_path, ["g"], "big", dims=(6, 4))
    assert volume_shape(rd.grid_ref(desc)) == ((1, 4, 6), 0)
    assert classify_path(desc)["kind"] == "image"
    desc, _ = write_rawgrids(tmp_path, ["g"], "big", dims=(9,))
    assert classify_path(desc)["opener"] == "unsupported"


def test_a_grid_physical_units_are_neutral(tmp_path):
    from app import physical_units as pu
    desc, _ = write_rawgrids(tmp_path, ["g"], "little")
    ref = rd.grid_ref(desc)
    assert pu.scales(ref) == {}
    assert pu.is_simulation_volume(ref, "cube", ["X", "Y", "Z"])
    pu.set_user(ref, columns={"value": (2.5, "g/cm^3")}, by="test")
    assert pu.scales(ref)["value"]["factor"] == 2.5
    pu.clear_store()


def test_a_grids_pyramid_is_that_of_the_same_cube_in_fits(tmp_path, monkeypatch):
    from astropy.io import fits

    from app.brick_pyramid import (BrickPyramid, StalePyramid, build_pyramid,
                                   default_pyramid_path)
    from app.product_cache import file_identity

    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    desc, truth = write_rawgrids(tmp_path, ["g1", "g2"], "big", dims=(20, 18, 16))
    ref0, ref1 = rd.grid_ref(desc, 0), rd.grid_ref(desc, 1)
    assert default_pyramid_path(ref0, 0) != default_pyramid_path(ref1, 0)
    assert ":" not in default_pyramid_path(ref0, 0).name
    cube = tmp_path / "g1.fits"
    fits.PrimaryHDU(truth["g1"]).writeto(cube)
    a = build_pyramid(ref0, tmp_path / "raw.pyr.h5", hdu=0, brick=8)
    b = build_pyramid(cube, tmp_path / "fits.pyr.h5", brick=8)
    with BrickPyramid(a, source=ref0) as pa, BrickPyramid(b, source=cube) as pb:
        assert pa.levels == pb.levels
        for k in range(pa.levels):
            np.testing.assert_array_equal(pa._h5[f"level{k}/data"][...],
                                          pb._h5[f"level{k}/data"][...])
    # The descriptor read another way (its byte order edited): another source.
    before = file_identity(ref0)
    assert before and file_identity(ref0) == before != file_identity(ref1)
    desc.write_text(desc.read_text().replace("\nb\n", "\nl\n"))
    assert file_identity(ref0) != before
    with pytest.raises(StalePyramid):
        BrickPyramid(a, source=ref0)


def test_the_stats_and_moment_readers_read_the_data_file_not_the_descriptor(tmp_path):
    """A descriptor longer than the grid must not be read as its samples."""
    from app.compute.moment import _cube_reader, _mapped_cube
    from app.cube_stats import _byte_reader

    names = [f"g{k}" for k in range(40)]      # a long list: a descriptor of 400+ bytes
    desc, truth = write_rawgrids(tmp_path, names, "big", dims=(3, 2, 2))
    assert desc.stat().st_size > 12 * 4
    ref = rd.grid_ref(desc, 3)
    st = (tmp_path / "g3").stat()
    np.testing.assert_array_equal(_mapped_cube(ref, 3, (st.st_size, st.st_mtime_ns)),
                                  truth["g3"])
    read = _cube_reader(ref, 3, (2, 2, 3))
    assert read is not None
    np.testing.assert_array_equal(read(0, 2, 0, 2), truth["g3"])
    reader = _byte_reader(ref, 3)
    assert reader is not None
    reader.close()


def test_a_malformed_reference_or_size_is_refused_not_a_crash(tmp_path):
    assert rd.is_points_ref("a.desc::raw-points=²") is False
    desc, _ = write_rawpoints(tmp_path, ["a"], "little")
    with pytest.raises(ValueError, match="scalar fields"):
        (tmp_path / "a").write_bytes(b"\0" * (4 * 3 * N + 4 * N * (rd.MAX_SCALARS + 1)))
        rd.points_file(rd.points_ref(desc))


# ── FLY ──────────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("endian", ["little", "big"])
@pytest.mark.parametrize("code", ["f4", "f8"])
def test_a_fly_descriptor_lists_snapshots(tmp_path, endian, code):
    truth = [write_fly(tmp_path / f"s{k}", endian, code) for k in range(2)]
    kind = "double" if code == "f8" else "float"
    (tmp_path / "run.desc").write_text(
        f"flyDesc\n2m_test\n{kind}\ntime\n{N}\n{BOXES}\n{_flag(endian)}\n0.0 s0\n1.0 s1\n")
    d = rd.descriptor(tmp_path / "run.desc")
    assert (d.kind, d.rows, len(d.entries)) == ("fly", N, 2)
    for k, (pos, vel) in enumerate(truth):
        rf = rd.points_file(rd.points_ref(tmp_path / "run.desc", k))
        cols = rf.columns()
        assert rf.names == ["X", "Y", "Z", "Vx", "Vy", "Vz"]
        for j in range(3):
            np.testing.assert_array_equal(cols["XYZ"[j]], pos[:, j])
            np.testing.assert_array_equal(cols[("Vx", "Vy", "Vz")[j]], vel[:, j])
    assert rd.points_file(rd.points_ref(tmp_path / "run.desc", 1)).time == 1.0
    (tmp_path / "s0").write_bytes(b"\0" * 10)
    with pytest.raises(ValueError, match="6 x"):
        rd.points_file(rd.points_ref(tmp_path / "run.desc", 0))


@pytest.mark.parametrize("endian", ["little", "big"])
@pytest.mark.parametrize("code", ["f4", "f8"])
def test_a_bare_fly_file_is_read_when_one_reading_is_plausible(tmp_path, endian, code):
    n = N + 1      # odd: 6 n floats are not a whole number of doubles
    pos, vel = write_fly(tmp_path / "snap.fly", endian, code, n)
    ref = particle_dataset_path(tmp_path / "snap.fly")[0]
    d = rd.descriptor(tmp_path / "snap.fly")
    assert (d.bare, d.rows, d.endian, d.dtype.itemsize) == (
        True, n, endian, 8 if code == "f8" else 4)
    table = open_point_table(ref)
    np.testing.assert_array_equal(table.columns["Vz"], vel[:, 2])
    verdict = classify_path(tmp_path / "snap.fly")
    assert (verdict["opener"], verdict["open_path"]) == ("catalogue", ref)


def test_a_float_file_that_could_also_be_double_asks(tmp_path):
    """Read as double it still looks plausible: the size fits both types."""
    write_fly(tmp_path / "even.fly", "little", "f4", N)
    assert (6 * N * 4) % 48 == 0
    verdict = classify_path(tmp_path / "even.fly")
    assert (verdict["opener"], verdict["confident"]) == ("ask", False)
    with pytest.raises(ValueError, match="flyDesc"):
        particle_dataset_path(tmp_path / "even.fly")


def test_an_ambiguous_bare_fly_asks(tmp_path):
    (tmp_path / "zeros.fly").write_bytes(b"\0" * (6 * 8 * 100))
    verdict = classify_path(tmp_path / "zeros.fly")
    assert (verdict["opener"], verdict["confident"]) == ("ask", False)
    assert "flyDesc" in verdict["detail"]
    # A size that is no multiple of a FLY record is not one at all.
    (tmp_path / "odd.fly").write_bytes(b"\1" * 25)
    assert classify_path(tmp_path / "odd.fly")["opener"] == "ask"
    # Not a .fly, not a descriptor: not claimed.
    (tmp_path / "x.dat").write_bytes(b"\1" * 48)
    assert rd.sniff(tmp_path / "x.dat") is None


# ── classify ─────────────────────────────────────────────────────────────────

def test_classify_answers_for_each_descriptor(tmp_path):
    pdesc, _ = write_rawpoints(tmp_path, ["a", "b"], "little")
    v = classify_path(pdesc)
    assert (v["kind"], v["opener"], v["confident"]) == ("particles", "catalogue", True)
    assert v["open_path"] == rd.points_ref(pdesc, 0) and "2 files as a series" in v["detail"]
    gdesc, _ = write_rawgrids(tmp_path, ["g1", "g2", "g3"], "big")
    v = classify_path(gdesc)
    assert (v["kind"], v["opener"]) == ("cube", "dataset")
    assert v["open_path"] == rd.grid_ref(gdesc, 0) and "3 files" in v["detail"]
    (tmp_path / "f").write_bytes(b"\0")
    fdesc = tmp_path / "f.desc"
    fdesc.write_text(f"flyDesc\nx\nfloat\ntime\n{N}\n{BOXES}\nl\n0.0 f\n")
    v = classify_path(fdesc)
    assert v["opener"] == "unsupported" and "holds" in v["detail"]
    # A missing data file is said, not a crash.
    pdesc.write_text(pdesc.read_text() + "2.0 gone\n")
    assert classify_path(pdesc)["opener"] == "catalogue"
    # Other files keep their verdicts.
    (tmp_path / "t.csv").write_text("a,b\n" + "1,2\n" * 12)
    assert classify_path(tmp_path / "t.csv")["opener"] == "catalogue"


# ── Routes ───────────────────────────────────────────────────────────────────

def _plane(sl):
    raw = base64.b64decode(sl["data_base64"])
    if sl.get("compression") == "qt-zlib":
        raw = zlib.decompress(raw[4:])
    return np.frombuffer(raw, dtype=np.float32).reshape(sl["height"], sl["width"])


@pytest.mark.asyncio
async def test_a_grid_opens_through_the_volume_routes(client, auth_headers, tmp_path):
    desc, truth = write_rawgrids(tmp_path, ["g1", "g2"], "big", "Double", dims=(8, 6, 5))
    verdict = (await client.post("/v1/files/classify", json={"path": str(desc)},
                                 headers=auth_headers)).json()
    assert (verdict["kind"], verdict["opener"]) == ("cube", "dataset")
    for given in (str(desc), verdict["open_path"]):
        r = (await client.post("/v1/datasets/open", json={"path": given},
                               headers=auth_headers)).json()
        assert r["valid"], r["error"]
    assert r["kind"] == "cube" and (r["width"], r["height"], r["depth"]) == (8, 6, 5)
    assert [f["path"] for f in r["fields"]] == [rd.grid_ref(desc, 0), rd.grid_ref(desc, 1)]
    assert r["simulation_volume"] is True
    headers = {**auth_headers, "X-Visivo-Session": r["session_id"]}
    sl = (await client.post("/v1/cube/slice", headers=headers,
                            json={"dataset_id": r["dataset_id"], "axis": "z", "index": 2})).json()
    assert sl["valid"], sl["error"]
    np.testing.assert_allclose(_plane(sl), truth["g1"][2], rtol=1e-6)
    # The second step is its own dataset.
    r2 = (await client.post("/v1/datasets/open", json={"path": rd.grid_ref(desc, 1)},
                            headers=auth_headers)).json()
    assert r2["valid"]
    sl = (await client.post("/v1/cube/slice", headers={**auth_headers,
                                                      "X-Visivo-Session": r2["session_id"]},
                            json={"dataset_id": r2["dataset_id"], "axis": "z", "index": 4})).json()
    np.testing.assert_allclose(_plane(sl), truth["g2"][4], rtol=1e-6)
    # byte_order is the descriptor's, not a choice.
    r3 = (await client.post("/v1/datasets/open", headers=auth_headers,
                            json={"path": str(desc), "byte_order": "little"})).json()
    assert r3["valid"] is False
    header = (await client.post("/v1/files/header", json={"path": rd.grid_ref(desc, 1)},
                                headers=auth_headers)).json()
    assert header["valid"] and any("RAWTIME" in c for c in header["cards"])
    ref_verdict = (await client.post("/v1/files/classify", json={"path": rd.grid_ref(desc, 1)},
                                     headers=auth_headers)).json()
    assert ref_verdict["valid"] and ref_verdict["kind"] == "cube"


@pytest.mark.asyncio
async def test_a_table_opens_through_the_particle_routes_and_steps(client, auth_headers, tmp_path):
    names = ["p_096", "p_104", "p_112"]
    desc, truth = write_rawpoints(tmp_path, names, "big")
    verdict = (await client.post("/v1/files/classify", json={"path": str(desc)},
                                 headers=auth_headers)).json()
    assert verdict["opener"] == "catalogue" and verdict["open_path"] == rd.points_ref(desc, 0)
    cat = (await client.post("/v1/catalogue/open", json={"path": str(desc), "format": "csv"},
                             headers=auth_headers)).json()
    assert cat["valid"], cat["error"]
    assert (cat["format"], cat["particles"], cat["num_rows"]) == ("rawpoints", True, N)
    assert cat["path"] == rd.points_ref(desc, 0)
    assert [f["name"] for f in cat["fields"]] == ["X", "Y", "Z", "scalar0", "scalar1"]
    assert cat["coordinate_fields"]["system"] == "cartesian"
    assert cat["snapshot"]["time"] == 0.0
    headers = {**auth_headers, "X-Visivo-Session": cat["session_id"]}
    did = cat["dataset_id"]
    info = (await client.post("/v1/points/info", json={"dataset_id": did}, headers=headers)).json()
    assert info["valid"] and info["format"] == "rawpoints" and info["particles"] is True
    assert info["num_rows"] == N and info["axes"] == ["X", "Y", "Z"]
    s = await client.post("/v1/points/sample_bin", json={"dataset_id": did, "max_points": 500,
                                                         "color_field": "scalar0"},
                          headers=headers)
    assert s.status_code == 200 and len(s.content) > 0
    series = (await client.post("/v1/points/series", json={"dataset_id": did},
                                headers=headers)).json()
    assert series["valid"] and series["count"] == 3 and series["index"] == 0
    assert series["numbers"] == [0.0, 0.5, 1.0]
    # Stepping: each member is a dataset of its own.
    cat2 = (await client.post("/v1/catalogue/open", headers=auth_headers,
                              json={"path": series["paths"][2], "format": "csv"})).json()
    assert cat2["valid"], cat2["error"]
    assert cat2["snapshot"]["step"] == 3
    rows = (await client.post("/v1/catalogue/rows", headers={
        **auth_headers, "X-Visivo-Session": cat2["session_id"]},
        json={"dataset_id": cat2["dataset_id"], "limit": 5, "offset": 0})).json()
    if rows.get("valid"):
        assert len(rows["rows"]) == 5


@pytest.mark.asyncio
@pytest.mark.parametrize("kind,endian", [("Double", "big"), ("Float", "little")])
async def test_the_ray_traced_particle_render_reads_the_columns(client, auth_headers, tmp_path,
                                                                kind, endian):
    """The positions are strided over interleaved blocks, and may be big-endian
    doubles: the renderer's readers take them as they take a VBT's."""
    desc, _ = write_rawpoints(tmp_path, ["a"], endian, kind)
    cat = (await client.post("/v1/catalogue/open", json={"path": str(desc), "format": "csv"},
                             headers=auth_headers)).json()
    assert cat["valid"], cat["error"]
    headers = {**auth_headers, "X-Visivo-Session": cat["session_id"]}
    cam = {"position": [25.0, 25.0, 150.0], "focal_point": [25.0, 25.0, 25.0],
           "view_up": [0.0, 1.0, 0.0]}
    r = await client.post("/v1/particle_render/preview", headers=headers, json={
        "dataset_id": cat["dataset_id"], "cameras": [cam], "width": 64, "height": 48,
        "colour": {"field": "scalar0"}})
    assert r.status_code == 200, r.text
    assert r.headers["content-type"] == "image/jpeg" and r.content[:2] == b"\xff\xd8"


@pytest.mark.asyncio
async def test_a_fly_table_opens_with_its_velocities(client, auth_headers, tmp_path):
    write_fly(tmp_path / "s0", "big", "f8")
    (tmp_path / "run.desc").write_text(
        f"flyDesc\nrun\ndouble\ntime\n{N}\n{BOXES}\nb\n0.0 s0\n")
    cat = (await client.post("/v1/catalogue/open", headers=auth_headers,
                             json={"path": str(tmp_path / "run.desc"), "format": "csv"})).json()
    assert cat["valid"], cat["error"]
    assert [f["name"] for f in cat["fields"]] == ["X", "Y", "Z", "Vx", "Vy", "Vz"]
    assert cat["snapshot"]["format"] == "fly" and cat["particles"] is True
