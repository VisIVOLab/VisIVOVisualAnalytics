"""Generic HDF5 tables and volumes read in place (issue #552). The files are
written here (contiguous, chunked and gzip, compound, several 1-D columns,
rank-2 and rank-3 datasets, both byte orders) and read back through the same
seams the other simulation formats use: the values read are the values
written."""
from __future__ import annotations

import base64
import zlib
from pathlib import Path

import numpy as np
import pytest

h5py = pytest.importorskip("h5py")

from app import hdf5_source as hs  # noqa: E402
from app.classify import classify_path  # noqa: E402
from app.physical_units import scales  # noqa: E402
from app.point_table import (component_format, open_point_table, particle_dataset_path,  # noqa: E402
                             particle_frame, particle_metadata, split_component)
from app.product_cache import file_identity  # noqa: E402
from app.volume_source import (field_dataset_path, field_listing, open_fits, raw_field,  # noqa: E402
                               volume_data, volume_shape)

CUBE = np.arange(6 * 5 * 4, dtype=np.float32).reshape(6, 5, 4) * 0.5


@pytest.fixture(autouse=True)
def _copies_in_tmp(monkeypatch, tmp_path):
    monkeypatch.setattr(hs, "copy_dir", lambda: tmp_path / "copies")


def _file(path, build):
    with h5py.File(path, "w") as f:
        build(f)
    return str(path)


def cube_file(tmp_path, name="cube.h5", **kwargs):
    def build(f):
        ds = f.create_dataset("grid/rho", data=CUBE, **kwargs)
        ds.attrs["units"] = "g/cm^3"
    return _file(tmp_path / name, build)


# ── Volumes ──────────────────────────────────────────────────────────────────

def test_a_contiguous_cube_is_mapped_in_place(tmp_path):
    path = cube_file(tmp_path)
    ref = f"{path}::hdf5=/grid/rho"
    raw = raw_field(ref)
    assert raw.kind == "hdf5" and raw.path == path and raw.shape == (6, 5, 4)
    np.testing.assert_array_equal(np.asarray(raw.memmap()), CUBE)
    assert not (tmp_path / "copies").exists()          # nothing was copied
    with volume_data(ref) as data:
        np.testing.assert_array_equal(np.asarray(data), CUBE)


def test_a_big_endian_cube_keeps_its_byte_order(tmp_path):
    path = _file(tmp_path / "be.h5", lambda f: f.create_dataset("v", data=CUBE.astype(">f4")))
    raw = raw_field(f"{path}::hdf5=/v")
    assert raw.dtype.str == ">f4"
    np.testing.assert_array_equal(np.asarray(raw.memmap()), CUBE)


def test_a_chunked_gzip_cube_is_unpacked_once(tmp_path):
    path = cube_file(tmp_path, chunks=(2, 3, 2), compression="gzip")
    ref = f"{path}::hdf5=/grid/rho"
    raw = raw_field(ref)
    assert raw.path != path and raw.path.startswith(str(tmp_path / "copies"))
    assert raw.offset == 0 and raw.shape == (6, 5, 4)
    np.testing.assert_array_equal(np.asarray(raw.memmap()), CUBE)
    stamp = (tmp_path / "copies").rglob("data.bin").__next__().stat().st_mtime_ns
    assert raw_field(ref).path == raw.path
    assert next((tmp_path / "copies").rglob("data.bin")).stat().st_mtime_ns == stamp


def test_a_rewritten_file_is_not_served_from_the_old_copy(tmp_path):
    path = cube_file(tmp_path, chunks=(2, 3, 2), compression="gzip")
    first = raw_field(f"{path}::hdf5=/grid/rho").path
    _file(path, lambda f: f.create_dataset("grid/rho", data=CUBE + 1, chunks=(2, 3, 2),
                                           compression="gzip"))
    second = raw_field(f"{path}::hdf5=/grid/rho")
    assert second.path != first
    np.testing.assert_array_equal(np.asarray(second.memmap()), CUBE + 1)


@pytest.mark.parametrize("kwargs", [{}, {"chunks": (2, 3, 2), "compression": "gzip"}])
@pytest.mark.parametrize("slab,expect", [
    ("[2:3,:,:]", (slice(2, 5), slice(None), slice(None))),      # a run of planes
    ("[1:2,1:3,0:2]", (slice(1, 3), slice(1, 4), slice(0, 2))),  # a box
    ("[:,2:2,:]",(slice(None), slice(2, 4), slice(None))),
])
def test_a_hyperslab_reads_the_box(tmp_path, kwargs, slab, expect):
    path = cube_file(tmp_path, **kwargs)
    ref = f"{path}::hdf5=/grid/rho{slab}"
    raw = raw_field(ref)
    np.testing.assert_array_equal(np.asarray(raw.memmap()), CUBE[expect])
    assert volume_shape(ref)[0] == CUBE[expect].shape


def test_a_hyperslab_of_whole_planes_stays_in_place(tmp_path):
    path = cube_file(tmp_path)
    raw = raw_field(f"{path}::hdf5=/grid/rho[2:3,:,:]")
    assert raw.path == path and not (tmp_path / "copies").exists()
    # Not contiguous: a box inside the planes is copied.
    assert raw_field(f"{path}::hdf5=/grid/rho[:,1:2,:]").path != path


def test_a_rank_2_dataset_is_an_image_one_plane_deep(tmp_path):
    image = np.arange(30, dtype=np.float64).reshape(5, 6)
    path = _file(tmp_path / "img.h5", lambda f: f.create_dataset("map", data=image))
    ref = f"{path}::hdf5=/map"
    assert volume_shape(ref)[0] == (1, 5, 6)
    with open_fits(ref) as hdul:
        assert hdul[0].data.shape == (1, 5, 6)
        np.testing.assert_array_equal(np.asarray(hdul[0].data)[0], image)
        assert hdul[0].header["H5DSET"] == "/map"
    verdict = classify_path(path)
    assert (verdict["kind"], verdict["opener"]) == ("image", "dataset")


def test_a_bad_hyperslab_or_name_is_refused(tmp_path):
    path = cube_file(tmp_path)
    for bad in ("[9:2,:,:]", "[0:2,0:2,0:2,0:2]", "[a:b]", "[-1:2]", "[0:0]"):
        with pytest.raises(ValueError):
            raw_field(f"{path}::hdf5=/grid/rho{bad}")
    with pytest.raises(ValueError):
        raw_field(f"{path}::hdf5=/nothing")
    assert field_dataset_path(f"{path}::hdf5=/nothing") is None


def test_the_unit_attribute_gives_the_value_scale(tmp_path):
    path = cube_file(tmp_path)
    ref = f"{path}::hdf5=/grid/rho"
    assert scales(ref)["value"] == {"factor": 1.0, "unit": "g/cm^3", "source": "HDF5 attribute"}
    for key in ("unit", "BUNIT"):
        p = _file(tmp_path / f"{key}.h5", lambda f, k=key: f.create_dataset(
            "v", data=CUBE).attrs.__setitem__(k, np.bytes_(b"K")))
        assert scales(f"{p}::hdf5=/v")["value"]["unit"] == "K"
    bare = _file(tmp_path / "bare.h5", lambda f: f.create_dataset("v", data=CUBE))
    assert "value" not in scales(f"{bare}::hdf5=/v")


def test_the_fields_of_a_file_are_its_volumes(tmp_path):
    def build(f):
        f.create_dataset("a", data=CUBE)
        f.create_dataset("b", data=CUBE * 2)
        f.create_dataset("img", data=CUBE[0])
    path = _file(tmp_path / "many.h5", build)
    fields, name = field_listing(f"{path}::hdf5=/b")
    assert [f["name"] for f in fields] == ["/a", "/b", "/img"] and name == "/b"


def test_the_identity_names_the_dataset_and_the_file_version(tmp_path):
    path = cube_file(tmp_path)
    a, b = f"{path}::hdf5=/grid/rho", f"{path}::hdf5=/grid/rho[1:2,:,:]"
    assert file_identity(a) and file_identity(a) != file_identity(b)


@pytest.mark.parametrize("kwargs", [{}, {"chunks": (3, 3, 2), "compression": "gzip"}])
def test_the_pyramid_and_readers_take_it_as_the_same_cube_in_fits(tmp_path, monkeypatch, kwargs):
    from astropy.io import fits

    from app.brick_pyramid import (BrickPyramid, StalePyramid, build_pyramid,
                                   default_pyramid_path)
    from app.compute.moment import _cube_reader, _mapped_cube
    from app.cube_stats import _byte_reader

    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    big = np.random.default_rng(5).normal(size=(12, 18, 20)).astype(np.float32)
    path = _file(tmp_path / "big.h5", lambda f: f.create_dataset("grid/rho", data=big, **kwargs))
    ref, slab = f"{path}::hdf5=/grid/rho", f"{path}::hdf5=/grid/rho[1:9,:,:]"
    assert default_pyramid_path(ref, 0) != default_pyramid_path(slab, 0)
    assert ":" not in default_pyramid_path(ref, 0).name
    cube = tmp_path / "cube.fits"
    fits.PrimaryHDU(big).writeto(cube)
    a = build_pyramid(ref, tmp_path / "h5.pyr.h5", hdu=0, brick=8)
    b = build_pyramid(cube, tmp_path / "fits.pyr.h5", brick=8)
    with BrickPyramid(a, source=ref) as pa, BrickPyramid(b, source=cube) as pb:
        assert pa.levels == pb.levels
        for k in range(pa.levels):
            np.testing.assert_array_equal(pa._h5[f"level{k}/data"][...],
                                          pb._h5[f"level{k}/data"][...])
    # The explicit readers of the moments and the statistics read the data.
    from pathlib import Path

    from app.volume_source import field_file
    st = Path(field_file(ref)).stat()
    np.testing.assert_array_equal(_mapped_cube(ref, 0, (st.st_size, st.st_mtime_ns)), big)
    assert _cube_reader(ref, 0, (12, 18, 20)) is not None
    reader = _byte_reader(ref, 0)
    assert reader is not None
    reader.close()
    # The dataset rewritten: another source, and the old pyramid is stale.
    before = file_identity(ref)
    _file(path, lambda f: f.create_dataset("grid/rho", data=big * 2))
    assert file_identity(ref) != before
    with pytest.raises(StalePyramid):
        BrickPyramid(a, source=ref)


# ── Tables ───────────────────────────────────────────────────────────────────

def columns_file(tmp_path, name="cols.h5", **kwargs):
    rng = np.random.default_rng(3)
    data = {k: rng.normal(size=300) for k in ("x", "y", "z")}
    data["mass"] = rng.lognormal(size=300)

    def build(f):
        g = f.create_group("halos")
        for k, v in data.items():
            ds = g.create_dataset(k, data=v, **kwargs)
            if k == "mass":
                ds.attrs["units"] = "Msun"
        g.create_dataset("short", data=np.arange(7.0))     # another length: left out
        f.create_dataset("note", data=np.bytes_(b"text"))
    return _file(tmp_path / name, build), data


def test_the_1d_datasets_of_a_group_are_a_table(tmp_path):
    path, data = columns_file(tmp_path)
    ref = f"{path}::hdf5-table=/halos"
    assert component_format(ref) == "hdf5" and particle_dataset_path(ref)[1] == "hdf5"
    assert particle_dataset_path(path) == (ref, "hdf5")          # the one table in the file
    table = hs.resolve(ref)
    assert table.names == ["mass", "x", "y", "z"] and table.num_rows == 300
    assert "short" not in table.names
    for name, values in data.items():
        np.testing.assert_array_equal(table.take(name, np.arange(300)), values)
        # A view of the file's map: nothing was read to open it.
        assert isinstance(table._by_name[name].view, np.memmap)
    assert table.units() == {"mass": "Msun"}
    assert scales(ref)["mass"]["unit"] == "Msun"
    assert particle_metadata(ref)["snapshot"]["format"] == "hdf5"
    pt = open_point_table(ref)
    np.testing.assert_array_equal(np.asarray(pt.columns["x"][5:9]), data["x"][5:9])
    assert pt.axes == ("x", "y", "z")
    frame = particle_frame(ref)
    assert list(frame.columns) == table.names and frame.attrs["units"] == {"mass": "Msun"}


def test_a_compound_dataset_is_a_table(tmp_path):
    rows = np.zeros(120, dtype=[("id", "<i8"), ("x", ">f8"), ("y", "<f4"), ("name", "S4"),
                                ("v", "<f8", (3,))])
    rows["id"], rows["x"], rows["y"] = np.arange(120), np.linspace(0, 1, 120), 2.5
    path = _file(tmp_path / "comp.h5", lambda f: f.create_dataset("catalog/stars", data=rows))
    ref = f"{path}::hdf5-table=/catalog/stars"
    table = hs.resolve(ref)
    assert table.names == ["id", "x", "y"]          # numeric scalar fields only
    np.testing.assert_array_equal(table.take("id", [4, 9]), [4, 9])
    np.testing.assert_array_equal(table.take("x", np.arange(120)), rows["x"])
    assert table.take("x", 0).dtype.isnative            # a big-endian field reads native
    verdict = classify_path(path)
    assert (verdict["kind"], verdict["opener"], verdict["open_path"]) == ("catalogue", "catalogue", ref)


@pytest.mark.parametrize("budget", ["512", "0"])
def test_a_chunked_compressed_table_reads_rows_not_the_file(tmp_path, monkeypatch, budget):
    """Kept whole when the cache budget allows, window by window when it does
    not; either way the rows asked for are the rows written."""
    monkeypatch.setenv("VISIVO_GADGET_CACHE_MB", budget)
    path, data = columns_file(tmp_path, chunks=(32,), compression="gzip")
    table = hs.resolve(f"{path}::hdf5-table=/halos")
    assert table._by_name["x"].view is None
    rows = np.array([299, 0, 33, 33, 150])
    np.testing.assert_array_equal(table.take("x", rows), data["x"][rows])
    np.testing.assert_array_equal(np.asarray(table.columns()["y"][10:20]), data["y"][10:20])
    np.testing.assert_array_equal(np.asarray(table.columns()["z"][np.arange(300) % 2 == 0]),
                                  data["z"][::2])


def test_a_closed_table_drops_the_columns_it_kept(tmp_path):
    path, data = columns_file(tmp_path, chunks=(32,), compression="gzip")
    ref = f"{path}::hdf5-table=/halos"
    table = hs.resolve(ref)
    table.take("x", [1, 2])
    assert table._kept_bytes == 300 * 8
    assert hs.release(ref) == 300 * 8 and table._kept_bytes == 0
    np.testing.assert_array_equal(table.take("x", [1, 2]), data["x"][[1, 2]])


def test_a_hyperslab_of_a_table_is_a_row_range(tmp_path):
    path, data = columns_file(tmp_path)
    for kwargs in ({}, {"chunks": (32,), "compression": "gzip"}):
        p, _ = columns_file(tmp_path, name=f"slab{len(kwargs)}.h5", **kwargs)
        table = hs.resolve(f"{p}::hdf5-table=/halos[100:50]")
        assert table.num_rows == 50
        np.testing.assert_array_equal(table.take("x", np.arange(50)), data["x"][100:150])
    with pytest.raises(ValueError):
        hs.resolve(f"{path}::hdf5-table=/halos[290:50]")


def test_a_volume_is_not_a_table_and_the_reverse(tmp_path):
    path = cube_file(tmp_path)
    with pytest.raises(ValueError):
        hs.canonical_ref(f"{path}::hdf5-table=/grid/rho", "table")
    cols, _ = columns_file(tmp_path)
    with pytest.raises(ValueError):
        hs.canonical_ref(f"{cols}::hdf5=/halos", "volume")


def test_a_series_member_has_the_dataset_or_not(tmp_path):
    path, _ = columns_file(tmp_path)
    assert hs.has_family(path, "/halos[0:5]") and not hs.has_family(path, "/other")
    assert split_component(f"{path}::hdf5-table=/halos") == (path, "::hdf5-table=/halos")


def test_a_projected_map_of_a_table_has_no_snapshot_frame(tmp_path):
    from app.projected_units import map_units
    path, _ = columns_file(tmp_path)
    table = hs.resolve(f"{path}::hdf5-table=/halos")
    out = map_units(table, axes=("x", "y", "z"), weight="mass", power=1.0, clipped=0,
                    light_unit=1.0, colour_by="", value_field=None, velocity=None)
    assert "comoving" not in out and out["density_unit"]


def test_a_dataset_that_cannot_be_read_in_place_is_not_offered(tmp_path):
    def build(f):
        f.create_dataset("half", data=np.ones((4, 4), np.float16))
        f.create_dataset("ok", data=np.ones((4, 4), np.float32))
        f["elsewhere"] = h5py.ExternalLink("other.h5", "/ok")
    path = _file(tmp_path / "odd.h5", build)
    assert [c.path for c in hs.layout(path).candidates] == ["/ok"]
    with pytest.raises(ValueError, match="link"):
        hs.canonical_ref(f"{path}::hdf5=/elsewhere")
    with pytest.raises(ValueError, match="outside"):
        hs.canonical_ref(f"{path}::hdf5=/ok[0:9,:]")


def test_a_bare_dt_attribute_is_not_a_dynamic_spectrum(tmp_path):
    path = _file(tmp_path / "sim.h5", lambda f: (f.create_dataset("rho", data=CUBE),
                                                  f.attrs.__setitem__("dt", 0.01)))
    assert hs.generic(path) is not None
    assert classify_path(path)["kind"] == "cube"


# ── What a file is: the classifier ──────────────────────────────────────────

def test_one_thing_that_opens_is_opened_and_several_are_asked(tmp_path):
    path = cube_file(tmp_path)
    v = classify_path(path)
    assert (v["kind"], v["opener"], v["confident"]) == ("cube", "dataset", True)
    assert v["open_path"] == f"{path}::hdf5=/grid/rho"

    def build(f):
        f.create_dataset("cube", data=CUBE)
        f.create_dataset("cube2", data=CUBE).attrs["units"] = "K"
        f.create_dataset("map", data=CUBE[0])
        t = f.create_group("t")
        t.create_dataset("a", data=np.arange(9.0))
        t.create_dataset("b", data=np.arange(9.0))
    many = _file(tmp_path / "many.h5", build)
    v = classify_path(many)
    assert (v["kind"], v["opener"], v["confident"]) == ("hdf5", "ask", False)
    names = {c["path"].split("=", 1)[1]: c["name"] for c in v["components"]}
    assert set(names) == {"/cube", "/cube2", "/map", "/t"}
    assert "K" in names["/cube2"] and "6 x 5 x 4" in names["/cube2"] and "9 rows" in names["/t"]
    assert v["detail"].count("hyperslab")


def test_a_file_with_nothing_to_show_says_so(tmp_path):
    path = _file(tmp_path / "none.h5", lambda f: f.create_dataset("s", data=np.bytes_(b"x")))
    v = classify_path(path)
    assert (v["kind"], v["opener"]) == ("hdf5", "unsupported")


def test_dynamic_spectra_keep_their_own_detection(tmp_path):
    spectrum = np.ones((6, 8), np.float32)
    by_attr = _file(tmp_path / "attr.h5", lambda f: (f.create_dataset("dynspec", data=spectrum),
                                                     f.attrs.__setitem__("TSAMP", 0.01)))
    by_name = _file(tmp_path / "name.h5", lambda f: f.create_dataset("waterfall", data=spectrum))
    lofar = _file(tmp_path / "bf.h5", lambda f: f.create_dataset(
        "SUB_ARRAY_POINTING_000/BEAM_000/STOKES_0", data=spectrum))
    for path in (by_attr, by_name, lofar):
        assert hs.generic(path) is None
        v = classify_path(path)
        assert (v["kind"], v["opener"]) == ("dynspec", "dataset")
    assert hs.canonical_ref(by_attr, "volume") is None


def test_a_snapshot_keeps_its_own_detection(tmp_path):
    def build(f):
        h = f.create_group("Header")
        h.attrs["NumPart_ThisFile"] = np.array([20, 0, 0, 0, 0, 0], dtype=np.int32)
        f.create_dataset("PartType0/Coordinates", data=np.zeros((20, 3), np.float32))
        f.create_dataset("PartType0/Masses", data=np.ones(20, np.float32))
    path = _file(tmp_path / "snap_000.hdf5", build)
    assert hs.generic(path) is None and hs.table_dataset_path(path) is None
    assert classify_path(path)["kind"] == "particles"


def test_an_unreadable_hdf5_keeps_the_legacy_answer(tmp_path):
    path = tmp_path / "beam.h5"
    path.write_bytes(b"\x89HDF\r\n\x1a\n" + b"\x00" * 64)
    assert classify_path(path)["kind"] == "dynspec"
    assert hs.generic(path) is None


# ── Routes ───────────────────────────────────────────────────────────────────

async def _post(client, headers, route, body):
    return (await client.post(route, json=body, headers=headers)).json()


@pytest.mark.asyncio
async def test_a_cube_opens_slices_and_classifies(client, auth_headers, tmp_path):
    path = cube_file(tmp_path, chunks=(2, 3, 2), compression="gzip")
    ref = f"{path}::hdf5=/grid/rho[1:4,:,1:3]"
    v = await _post(client, auth_headers, "/v1/files/classify", {"path": ref})
    assert v["valid"] and (v["kind"], v["opener"]) == ("cube", "dataset")
    # The plain file has one dataset to open: it opens.
    body = await _post(client, auth_headers, "/v1/datasets/open", {"path": ref})
    assert body["valid"], body["error"]
    assert body["kind"] == "cube" and body["field_name"] == "/grid/rho"
    headers = {**auth_headers, "X-Visivo-Session": body["session_id"]}
    sl = await _post(client, headers, "/v1/cube/slice",
                     {"dataset_id": body["dataset_id"], "axis": "z", "index": 1})
    assert sl["valid"], sl["error"]
    raw = base64.b64decode(sl["data_base64"])
    if sl.get("compression") == "qt-zlib":
        raw = zlib.decompress(raw[4:])
    plane = np.frombuffer(raw, dtype=np.float32).reshape(sl["height"], sl["width"])
    np.testing.assert_allclose(plane, CUBE[1:4, :, 1:4][1], rtol=1e-6)
    plain = await _post(client, auth_headers, "/v1/datasets/open", {"path": path})
    assert plain["valid"] and plain["kind"] == "cube"


@pytest.mark.asyncio
async def test_several_datasets_are_named_not_guessed(client, auth_headers, tmp_path):
    path = _file(tmp_path / "two.h5", lambda f: (f.create_dataset("a", data=CUBE),
                                                 f.create_dataset("b", data=CUBE)))
    body = await _post(client, auth_headers, "/v1/datasets/open", {"path": path})
    assert not body["valid"] and "::hdf5=/a" in body["error"] and "::hdf5=/b" in body["error"]
    one = await _post(client, auth_headers, "/v1/datasets/open", {"path": f"{path}::hdf5=/b"})
    assert one["valid"], one["error"]
    refused = await _post(client, auth_headers, "/v1/datasets/open",
                          {"path": f"{path}::hdf5=/b", "byte_order": "big"})
    assert not refused["valid"] and "byte_order" in refused["error"]
    missing = await _post(client, auth_headers, "/v1/datasets/open", {"path": f"{path}::hdf5=/c"})
    assert not missing["valid"] and "not in the file" in missing["error"]


@pytest.mark.asyncio
async def test_a_table_opens_through_the_particle_routes(client, auth_headers, tmp_path):
    path, data = columns_file(tmp_path, chunks=(32,), compression="gzip")
    v = await _post(client, auth_headers, "/v1/files/classify", {"path": path})
    assert (v["opener"], v["open_path"]) == ("catalogue", f"{path}::hdf5-table=/halos")
    ref = f"{path}::hdf5=/halos[10:100]"        # either separator names a group
    v = await _post(client, auth_headers, "/v1/files/classify", {"path": ref})
    assert v["valid"] and v["opener"] == "catalogue"
    cat = await _post(client, auth_headers, "/v1/catalogue/open", {"path": path, "format": "csv"})
    assert cat["valid"], cat["error"]
    assert (cat["format"], cat["num_rows"]) == ("hdf5", 300)
    assert [f["name"] for f in cat["fields"]] == ["mass", "x", "y", "z"]
    assert cat["coordinate_fields"]["x"] == "mass" or cat["coordinate_fields"]["system"] == "cartesian"
    headers = {**auth_headers, "X-Visivo-Session": cat["session_id"]}
    did = cat["dataset_id"]
    info = await _post(client, headers, "/v1/points/info", {"dataset_id": did})
    assert info["valid"] and info["num_rows"] == 300
    s = await client.post("/v1/points/sample_bin", headers=headers,
                          json={"dataset_id": did, "max_points": 100, "x": "x", "y": "y",
                                "z": "z", "color_field": "mass"})
    assert s.status_code == 200 and len(s.content) > 0
    rows = await _post(client, headers, "/v1/catalogue/rows",
                       {"dataset_id": did, "limit": 3, "offset": 5})
    if rows.get("valid"):
        assert len(rows["rows"]) == 3
    slab = await _post(client, auth_headers, "/v1/catalogue/open",
                       {"path": f"{path}::hdf5-table=/halos[100:50]", "format": "csv"})
    assert slab["valid"] and slab["num_rows"] == 50


@pytest.mark.asyncio
async def test_a_file_with_two_tables_lists_them(client, auth_headers, tmp_path):
    def build(f):
        for g in ("a", "b"):
            for k in ("x", "y"):
                f.create_dataset(f"{g}/{k}", data=np.arange(10.0))
    path = _file(tmp_path / "two_tables.h5", build)
    cat = await _post(client, auth_headers, "/v1/catalogue/open", {"path": path, "format": "csv"})
    assert not cat["valid"] and "hdf5-table=/a" in cat["error"] and "hdf5-table=/b" in cat["error"]
    ok = await _post(client, auth_headers, "/v1/catalogue/open",
                     {"path": f"{path}::hdf5-table=/b", "format": "csv"})
    assert ok["valid"], ok["error"]


def test_old_copies_make_room_under_the_cache_limit(tmp_path, monkeypatch):
    import os
    import time
    monkeypatch.setenv("VISIVO_DECOMPRESS_CACHE_MAX_BYTES", str(CUBE.nbytes + 16))
    old = cube_file(tmp_path, "old.h5", chunks=(2, 3, 2), compression="gzip")
    first = Path(raw_field(f"{old}::hdf5=/grid/rho").path)
    # Last used well before the grace period.
    past = time.time() - 3600
    os.utime(first, (past, past))
    new = cube_file(tmp_path, "new.h5", chunks=(2, 3, 2), compression="gzip")
    second = Path(raw_field(f"{new}::hdf5=/grid/rho").path)
    assert second.is_file() and not first.exists()
    np.testing.assert_array_equal(np.asarray(raw_field(f"{new}::hdf5=/grid/rho").memmap()), CUBE)


def test_a_copy_in_use_is_kept_over_the_limit(tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_DECOMPRESS_CACHE_MAX_BYTES", str(CUBE.nbytes + 16))
    a = cube_file(tmp_path, "a.h5", chunks=(2, 3, 2), compression="gzip")
    first = Path(raw_field(f"{a}::hdf5=/grid/rho").path)
    b = cube_file(tmp_path, "b.h5", chunks=(2, 3, 2), compression="gzip")
    raw_field(f"{b}::hdf5=/grid/rho")
    assert first.is_file()   # written a moment ago: within the grace period
