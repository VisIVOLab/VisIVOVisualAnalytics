"""PLUTO binary output on the cube path, read in place (issue #315): each
variable a memory-mapped field at its offset in the data file, its grid from
grid.out, its byte order from dbl.out / flt.out."""
from __future__ import annotations

import asyncio
import importlib.util
import os
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import pluto_source as ps
from app.brick_pyramid import BrickPyramid, build_pyramid, default_pyramid_path, source_shape
from app.volume_source import open_fits, raw_field, volume_data, volume_shape

_GEN = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"


def _generator():
    spec = importlib.util.spec_from_file_location("generate_demo_data", _GEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


write_pluto = _generator().write_pluto

SHAPE = (12, 16, 20)     # (nx3, nx2, nx1)


def _fields(shape=SHAPE) -> dict[str, np.ndarray]:
    """Values known per voxel: each variable a different function of (k, j, i)."""
    k, j, i = np.meshgrid(*(np.arange(n, dtype=np.float64) for n in shape), indexing="ij")
    return {"rho": 1.0 + i + 100.0 * j + 10000.0 * k,
            "vx1": -(i * 0.5) + j * 0.25 - k,
            "prs": np.exp(-(i - 7.0) ** 2 / 9.0) * (1.0 + k)}


def _as(precision: str, array: np.ndarray) -> np.ndarray:
    return array.astype(np.float64 if precision == "dbl" else np.float32)


# ── Reading ──────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("mode", ["single_file", "multiple_files"])
@pytest.mark.parametrize("precision", ["dbl", "flt"])
@pytest.mark.parametrize("endian", ["little", "big"])
def test_every_variable_reads_its_own_values(tmp_path, mode, precision, endian):
    fields = _fields()
    first = write_pluto(tmp_path, fields, mode=mode, precision=precision, endian=endian)
    assert ps.is_pluto(first)
    refs = ps.pluto_fields(first)
    assert [f["name"] for f in refs] == list(fields)
    for index, (name, want) in enumerate(fields.items()):
        ref = refs[index]["path"]
        assert ps.split_ref(ref)[1] == name
        assert volume_shape(ref) == (SHAPE, index)
        raw = raw_field(ref)
        assert raw.dtype.byteorder in ("<", ">", "=")
        assert raw.dtype.newbyteorder("=") == np.dtype("f8" if precision == "dbl" else "f4")
        with volume_data(ref) as data:
            np.testing.assert_array_equal(np.asarray(data), _as(precision, want))
        np.testing.assert_array_equal(ps.pluto_field(raw.path, name), _as(precision, want))
        with open_fits(ref) as hdul:
            np.testing.assert_array_equal(hdul[0].data, _as(precision, want))
            assert hdul[0].header["OBJECT"] == name


def test_the_out_line_parses(tmp_path):
    write_pluto(tmp_path, _fields(), mode="multiple_files", precision="flt", endian="big",
                index=3, time=0.358, step=1002)
    (record,) = ps.parse_out(tmp_path / "flt.out")
    assert (record.index, record.step, record.mode, record.endian) == \
        (3, 1002, "multiple_files", "big")
    assert record.time == pytest.approx(0.358)
    assert record.variables == ("rho", "vx1", "prs")
    (tmp_path / "flt.out").write_text("0 0.0 1e-3 0 one_file little rho\n")
    with pytest.raises(ValueError, match="mode"):
        ps.parse_out(tmp_path / "flt.out")


def test_a_uniform_grid_gives_coordinates(tmp_path):
    edges = (np.linspace(-2.0, 2.0, SHAPE[2] + 1), np.linspace(0.0, 3.2, SHAPE[1] + 1),
             np.linspace(5.0, 6.2, SHAPE[0] + 1))
    first = write_pluto(tmp_path, _fields(), edges=edges, time=0.25, step=40)
    grid = ps.parse_grid(tmp_path / "grid.out")
    assert grid.shape == SHAPE and grid.geometry == "CARTESIAN" and grid.index_reason() == ""
    header = ps.pluto_fits_header(ps.pluto_variable(first, "prs"))
    for axis, (e, name) in enumerate(zip(edges, "XYZ"), start=1):
        step = (e[-1] - e[0]) / (e.size - 1)
        assert header[f"CTYPE{axis}"] == name
        assert header[f"CDELT{axis}"] == pytest.approx(step, rel=1e-12)
        assert header[f"CRVAL{axis}"] == pytest.approx(e[0] + step / 2, rel=1e-12)
        assert header[f"CRPIX{axis}"] == 1.0
    assert header["PLUTOGRD"] == "UNIFORM"
    assert header["OBJECT"] == "prs"
    assert header["PLUTOTIM"] == pytest.approx(0.25) and header["PLUTOSTP"] == 40


@pytest.mark.parametrize("geometry, stretched, reason", [
    ("CARTESIAN", True, "non-uniform cells along x1"),
    ("SPHERICAL", False, "spherical geometry"),
])
def test_a_stretched_or_curvilinear_grid_is_shown_on_cell_indices(tmp_path, geometry,
                                                                    stretched, reason):
    x1 = np.concatenate([[0.0], np.cumsum(np.geomspace(0.01, 0.1, SHAPE[2]))]) if stretched \
        else np.linspace(1.0, 2.0, SHAPE[2] + 1)
    edges = (x1, np.linspace(0.0, np.pi, SHAPE[1] + 1), np.linspace(0.0, 2 * np.pi, SHAPE[0] + 1))
    first = write_pluto(tmp_path, _fields(), edges=edges, geometry=geometry)
    header = ps.pluto_fits_header(ps.pluto_variable(first))
    assert header["PLUTOGRD"] == "INDEX" and header["PLUTOWHY"] == reason
    assert header["PLUTOGEO"] == geometry
    for axis, name in enumerate("XYZ", start=1):
        assert (header[f"CTYPE{axis}"], header[f"CDELT{axis}"], header[f"CRVAL{axis}"]) == \
            (name, 1.0, 1.0)


def test_only_the_variables_on_disk_are_offered(tmp_path):
    fields = _fields()
    first = write_pluto(tmp_path, fields, mode="multiple_files", precision="flt",
                        omit=("vx1",))
    assert [f["name"] for f in ps.pluto_fields(first)] == ["rho", "prs"]
    assert ps.pluto_dataset_path(ps.pluto_ref(tmp_path / "vx1.0000.flt", "vx1")) is None
    # The .out opens the first variable present, which need not be the first listed.
    (tmp_path / "rho.0000.flt").unlink()
    assert ps.pluto_dataset_path(tmp_path / "flt.out") == ps.pluto_ref(tmp_path / "prs.0000.flt",
                                                                       "prs")


def test_the_out_file_opens_the_newest_output(tmp_path):
    fields = _fields()
    write_pluto(tmp_path, fields, index=0)
    write_pluto(tmp_path, {k: v * 2 for k, v in fields.items()}, index=1, time=0.5, append=True)
    ref = ps.pluto_dataset_path(tmp_path / "dbl.out")
    assert ref == ps.pluto_ref(tmp_path / "data.0001.dbl", "rho")
    with volume_data(ref) as data:
        np.testing.assert_array_equal(data, fields["rho"] * 2)


def test_a_file_of_the_wrong_size_is_refused(tmp_path):
    first = write_pluto(tmp_path, _fields())
    with open(first, "r+b") as fh:
        fh.truncate(os.path.getsize(first) - 8)
    with pytest.raises(ValueError, match="bytes"):
        raw_field(ps.pluto_ref(first, "rho"))
    assert ps.pluto_dataset_path(first) is None


def test_face_centred_fields_take_their_room_and_are_not_offered(tmp_path):
    """A constrained-transport run writes Bx1s with one more sample along x1:
    the variables after it start further on."""
    rho, prs = _fields()["rho"], _fields()["prs"]
    bx1s = np.arange(SHAPE[0] * SHAPE[1] * (SHAPE[2] + 1), dtype=np.float64)
    path = tmp_path / "data.0000.dbl"
    write_pluto(tmp_path, {"rho": rho, "prs": prs})
    with open(path, "wb") as fh:
        for array in (rho, bx1s, prs):
            array.astype("<f8").tofile(fh)
    (tmp_path / "dbl.out").write_text("0 0.0 1e-3 0 single_file little rho Bx1s prs\n")
    assert [f["name"] for f in ps.pluto_fields(path)] == ["rho", "prs"]
    np.testing.assert_array_equal(ps.pluto_field(path, "prs"), prs)
    with pytest.raises(ValueError, match="face-centred"):
        ps.pluto_variable(path, "Bx1s")


def test_grid_out_of_a_real_layout_parses(tmp_path):
    """PLUTO 4.3's own layout: a starred header, 'N point(s), g ghosts',
    each count followed by 'i x_left x_right' lines, one-cell directions."""
    (tmp_path / "grid.out").write_text(
        "# ******************************************************\n"
        "# PLUTO 4.3 Grid File\n# Generated on  Sun Aug 25 23:11:21 2024\n# \n"
        "# DIMENSIONS: 1\n# GEOMETRY:   CARTESIAN\n"
        "# X1: [ 0.000000,  1.000000], 4 point(s), 2 ghosts\n"
        "# ******************************************************\n"
        "4 \n 1   0.000000000000e+00    2.500000000000e-01\n"
        " 2   2.500000000000e-01    5.000000000000e-01\n"
        " 3   5.000000000000e-01    7.500000000000e-01\n"
        " 4   7.500000000000e-01    1.000000000000e+00\n"
        "1 \n 1   0.000000000000e+00    1.000000000000e+00\n"
        "1 \n 1   0.000000000000e+00    1.000000000000e+00\n")
    grid = ps.parse_grid(tmp_path / "grid.out")
    assert (grid.dimensions, grid.shape) == (1, (1, 1, 4))
    np.testing.assert_allclose(grid.axes[0].centres, [0.125, 0.375, 0.625, 0.875])
    assert grid.axes[0].uniform and grid.axes[0].spacing == pytest.approx(0.25)


# ── The cube path ────────────────────────────────────────────────────────────

def test_the_pyramid_of_a_variable_is_that_of_the_same_cube_in_fits(tmp_path):
    fields = _fields()
    first = write_pluto(tmp_path, fields, mode="multiple_files", precision="flt",
                        endian="big")
    ref = ps.pluto_ref(tmp_path / "prs.0000.flt", "prs")
    cube = tmp_path / "prs.fits"
    fits.PrimaryHDU(fields["prs"].astype(np.float32)).writeto(cube)
    shape, hdu = volume_shape(ref)
    assert source_shape(ref, hdu) == source_shape(cube, 0)
    a = build_pyramid(ref, tmp_path / "pluto.pyr.h5", hdu=hdu, brick=8)
    b = build_pyramid(cube, tmp_path / "fits.pyr.h5", brick=8)
    with BrickPyramid(a, source=ref) as pa, BrickPyramid(b, source=cube) as pb:
        assert pa.levels == pb.levels > 1
        for k in range(pa.levels):
            for item in ("data", "min", "max"):
                np.testing.assert_array_equal(pa._h5[f"level{k}/{item}"][...],
                                              pb._h5[f"level{k}/{item}"][...])
    assert first.exists()


def test_each_variable_has_its_own_pyramid_and_a_new_grid_makes_it_stale(tmp_path, monkeypatch):
    from app.brick_pyramid import StalePyramid

    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    first = write_pluto(tmp_path, _fields())
    rho, prs = ps.pluto_ref(first, "rho"), ps.pluto_ref(first, "prs")
    assert default_pyramid_path(rho, 0) != default_pyramid_path(prs, 0)
    # A reference and its data file with the variable's index: one pyramid.
    assert default_pyramid_path(prs, 0) == default_pyramid_path(first, 2)
    out = build_pyramid(prs, tmp_path / "p.pyr.h5", hdu=2, brick=8)
    # A running simulation appends its next output to the .out: the same
    # values, the same pyramid (not a new 32 GB build at every output).
    before = default_pyramid_path(prs, 0)
    write_pluto(tmp_path, _fields(), index=1, time=0.5, append=True)
    assert default_pyramid_path(prs, 0) == before
    with BrickPyramid(out, source=prs):
        pass
    # Its line rewritten in the other byte order: other values, a stale pyramid.
    out_file = tmp_path / "dbl.out"
    out_file.write_text(out_file.read_text().replace("single_file little", "single_file big", 1))
    with pytest.raises(StalePyramid):
        BrickPyramid(out, source=prs)


def test_a_line_still_being_written_does_not_hide_the_outputs_before_it(tmp_path):
    first = write_pluto(tmp_path, _fields())
    with open(tmp_path / "dbl.out", "a") as fh:
        fh.write("1 5.0")                          # no newline yet
    assert [r.index for r in ps.parse_out(tmp_path / "dbl.out")] == [0]
    assert ps.pluto_dataset_path(tmp_path / "dbl.out") == ps.pluto_ref(first, "rho")
    with open(tmp_path / "dbl.out", "a") as fh:
        fh.write("00000e-01\n")                    # complete, and broken: an error
    with pytest.raises(ValueError, match="line 2"):
        ps.parse_out(tmp_path / "dbl.out")


def test_a_large_2d_output_gets_its_tile_pyramid(tmp_path, monkeypatch):
    from app import image_pyramid

    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    monkeypatch.setenv("VISIVO_IMAGE_PYRAMID_BYTES", "100")
    first = write_pluto(tmp_path, {"rho": _fields()["rho"][:1]}, mode="multiple_files",
                        precision="flt")
    ref = ps.pluto_ref(first, "rho")
    assert image_pyramid._hdu_and_shape(ref) == (0, (1, SHAPE[1], SHAPE[2]))
    monkeypatch.setattr("app.pyramid_builds.ensure_pyramid",
                        lambda path, hdu: type("S", (), {"state": "building", "progress": 0.0,
                                                         "error": ""})())
    assert image_pyramid.tile_source(ref).state == "building"


def test_the_moment_readers_read_the_variable(tmp_path):
    from app.compute.moment import _cube_reader, _mapped_cube

    fields = _fields()
    first = write_pluto(tmp_path, fields, precision="flt", endian="big")
    ref = ps.pluto_ref(first, "vx1")
    st = os.stat(first)
    read = _cube_reader(ref, 1, SHAPE)
    assert read is not None
    np.testing.assert_array_equal(read(3, 7, 0, SHAPE[1]),
                                  fields["vx1"][3:7].astype(np.float32))
    np.testing.assert_array_equal(_mapped_cube(ref, 1, (st.st_size, st.st_mtime_ns)),
                                  fields["vx1"].astype(np.float32))


def test_the_product_cache_identity_follows_the_grid(tmp_path):
    from app.product_cache import file_identity

    first = write_pluto(tmp_path, _fields())
    ref = ps.pluto_ref(first, "rho")
    before = file_identity(ref)
    assert before and before != file_identity(first)
    grid = tmp_path / "grid.out"
    grid.write_text(grid.read_text() + "\n")
    after = file_identity(ref)
    assert after != before
    # The next output of a running simulation changes nothing read here.
    with open(tmp_path / "dbl.out", "a") as fh:
        fh.write("1 1.0e-01 1.0e-03 10 single_file little rho vx1 prs \n")
    assert file_identity(ref) == after
    # Its own line read otherwise: another version.
    out_file = tmp_path / "dbl.out"
    out_file.write_text(out_file.read_text().replace("single_file little", "single_file big", 1))
    assert file_identity(ref) != after


def test_the_chunked_reader_reads_the_variable(tmp_path):
    import time

    from app.cube_reader import CubeReader

    fields = _fields()
    first = write_pluto(tmp_path, fields, endian="big")
    reader = CubeReader(ps.pluto_ref(first, "prs"), shape=SHAPE, chunk_size_z=5)
    try:
        reader.start()
        deadline = time.monotonic() + 60
        events: list[dict] = []
        while time.monotonic() < deadline:
            events += reader.poll_progress()
            if any(e["event"] in ("complete", "error") for e in events):
                break
            time.sleep(0.05)
        assert [e for e in events if e["event"] == "error"] == []
        np.testing.assert_allclose(reader.cube, fields["prs"].astype(np.float32))
    finally:
        reader.close()


# ── Classification ───────────────────────────────────────────────────────────

def test_classification(tmp_path):
    from app.classify import classify_path

    run = tmp_path / "run3d"
    write_pluto(run, _fields(), index=0)
    write_pluto(run, _fields(), index=1, append=True)
    verdict = classify_path(run / "dbl.out")
    assert (verdict["kind"], verdict["opener"]) == ("cube", "dataset")
    assert verdict["open_path"] == ps.pluto_ref(run / "data.0001.dbl", "rho")
    verdict = classify_path(run / "data.0000.dbl")
    assert (verdict["kind"], verdict["open_path"]) == \
        ("cube", ps.pluto_ref(run / "data.0000.dbl", "rho"))

    flat = tmp_path / "run2d"
    two_d = {k: v[:1] for k, v in _fields().items()}
    write_pluto(flat, two_d, mode="multiple_files", precision="flt")
    verdict = classify_path(flat / "vx1.0000.flt")
    assert (verdict["kind"], verdict["opener"]) == ("image", "dataset")
    assert verdict["open_path"] == ps.pluto_ref(flat / "vx1.0000.flt", "vx1")

    line = tmp_path / "run1d"
    write_pluto(line, {"rho": np.ones((1, 1, 30))})
    assert classify_path(line / "dbl.out")["opener"] == "unsupported"

    (run / "grid.out").unlink()
    verdict = classify_path(run / "data.0000.dbl")
    assert verdict["opener"] == "unsupported" and "grid.out" in verdict["detail"]

    # A .dbl without its .out is not taken for PLUTO.
    lone = tmp_path / "lone"
    lone.mkdir()
    np.zeros(10).tofile(lone / "data.0000.dbl")
    assert classify_path(lone / "data.0000.dbl")["kind"] != "cube"


# ── Routes ───────────────────────────────────────────────────────────────────

async def _open(client, auth_headers, path) -> dict:
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    body = resp.json()
    assert body["valid"], body["error"]
    return {**body, "headers": {**auth_headers, "X-Visivo-Session": body["session_id"]}}


@pytest.mark.asyncio
async def test_an_output_opens_as_a_cube_one_variable_at_a_time(client, auth_headers, tmp_path):
    fields = _fields()
    edges = (np.linspace(0.0, 2.0, SHAPE[2] + 1), np.linspace(0.0, 1.6, SHAPE[1] + 1),
             np.linspace(0.0, 0.6, SHAPE[0] + 1))
    first = write_pluto(tmp_path, fields, precision="flt", endian="big", edges=edges)
    for given in (tmp_path / "flt.out", first):
        body = await _open(client, auth_headers, given)
        assert body["kind"] == "cube"
        assert (body["width"], body["height"], body["depth"]) == (20, 16, 12)
        assert body["field_name"] == "rho"
        assert [f["name"] for f in body["fields"]] == ["rho", "vx1", "prs"]
        assert body["cdelt"][:3] == pytest.approx([0.1, 0.1, 0.05])
        assert body["ctype"] == ["X", "Y", "Z"]
    second = await _open(client, auth_headers, body["fields"][2]["path"])
    assert second["field_name"] == "prs"
    resp = await client.post("/v1/cube/slice", headers=second["headers"],
                             json={"dataset_id": second["dataset_id"], "axis": "z", "index": 5})
    sl = resp.json()
    assert sl["valid"], sl["error"]
    import base64
    import zlib
    raw = base64.b64decode(sl["data_base64"])
    if sl.get("compression") == "qt-zlib":
        raw = zlib.decompress(raw[4:])
    plane = np.frombuffer(raw, dtype=np.float32).reshape(sl["height"], sl["width"])
    np.testing.assert_allclose(plane, fields["prs"][5], rtol=1e-6)
    # The byte order is the .out's: none can be chosen.
    resp = await client.post("/v1/datasets/open", headers=auth_headers,
                             json={"path": str(first), "byte_order": "little"})
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_a_large_variable_opens_from_its_headers_for_preview(client, auth_headers,
                                                                   tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_FULL_RES_THRESHOLD_BYTES", "1000")
    first = write_pluto(tmp_path, _fields(), mode="multiple_files", precision="flt")
    body = await _open(client, auth_headers, first)
    assert body["recommended_load_mode"] == "preview_only"
    assert body["file_size_bytes"] == 4 * int(np.prod(SHAPE))


@pytest.mark.asyncio
async def test_a_2d_output_opens_as_an_image(client, auth_headers, tmp_path):
    two_d = {k: v[:1] for k, v in _fields().items()}
    first = write_pluto(tmp_path, two_d, mode="multiple_files", precision="dbl")
    body = await _open(client, auth_headers, first)
    assert body["kind"] == "image"
    assert (body["width"], body["height"]) == (20, 16)


@pytest.mark.asyncio
async def test_missing_variables_and_files_do_not_open(client, auth_headers, tmp_path):
    first = write_pluto(tmp_path, _fields(), mode="multiple_files", precision="flt",
                        omit=("prs",))
    body = await _open(client, auth_headers, first)
    assert [f["name"] for f in body["fields"]] == ["rho", "vx1"]
    for missing in (ps.pluto_ref(tmp_path / "prs.0000.flt", "prs"),
                    ps.pluto_ref(first, "temperature")):
        resp = await client.post("/v1/datasets/open", json={"path": missing},
                                 headers=auth_headers)
        assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_a_variable_reference_classifies_and_shows_its_header(client, auth_headers,
                                                                    tmp_path):
    first = write_pluto(tmp_path, _fields(), time=0.5, step=12)
    ref = ps.pluto_ref(first, "vx1")
    verdict = (await client.post("/v1/files/classify", json={"path": ref},
                                 headers=auth_headers)).json()
    assert verdict["valid"] and (verdict["kind"], verdict["opener"]) == ("cube", "dataset")
    header = (await client.post("/v1/files/header", json={"path": ref},
                                headers=auth_headers)).json()
    assert header["valid"], header["error"]
    cards = "\n".join(header["cards"])
    assert "PLUTOSTP=                   12" in cards and "'vx1" in cards
    verdict = (await client.post("/v1/files/classify", json={"path": str(tmp_path / "dbl.out")},
                                 headers=auth_headers)).json()
    assert verdict["open_path"] == ps.pluto_ref(first, "rho")


@pytest.mark.asyncio
async def test_the_reduced_cube_of_a_variable(client, auth_headers, tmp_path, monkeypatch):
    """The headset's reduced cube, from the variable's brick pyramid."""
    from app import cube_reduce

    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    monkeypatch.setenv("VISIVO_VR_PYRAMID_BYTES", "1000")
    fields = _fields()
    first = write_pluto(tmp_path, fields, precision="flt", endian="big")
    opened = await _open(client, auth_headers, ps.pluto_ref(first, "prs"))
    # Linear on purpose: a PLUTO field is a simulation volume, so under the
    # default ``auto`` a positive field spanning > 3 decades comes back as log10
    # codes (#317); that case is checked at the end.
    body = {"dataset_id": opened["dataset_id"], "factor_xy": 2, "factor_z": 2,
            "scale": "linear"}
    for _ in range(480):
        r = await client.post("/v1/cube/reduced_bin", json=body, headers=opened["headers"])
        if r.status_code != 202:
            break
        await asyncio.sleep(0.25)
    assert r.status_code == 200, r.text
    hd = r.headers
    assert hd["X-Visivo-Source"] == "pyramid"
    w, h, d = int(hd["X-Visivo-Width"]), int(hd["X-Visivo-Height"]), int(hd["X-Visivo-Depth"])
    assert (w, h, d) == (10, 8, 6)
    lo, hi = float(hd["X-Visivo-Range-Min"]), float(hd["X-Visivo-Range-Max"])
    got = cube_reduce.dequantise(r.content, (d, h, w), lo, hi, "uint8")
    want = cube_reduce.reduce_cube(fields["prs"].astype(np.float32), (2, 2, 2), "mean")
    assert np.max(np.abs(got - want)) <= (hi - lo) / 254 / 2 + 1e-5
    # And from the whole cube, read in place.
    r = await client.post("/v1/cube/reduced_bin", json={**body, "source": "cube"},
                          headers=opened["headers"])
    assert r.status_code == 200 and r.headers["X-Visivo-Source"] == "cube"
    # Under the default scale the simulation field is coded as log10 when its
    # positive values span more than three decades; decoded, it is the same
    # block mean within half a code of the log range.
    auto = {k: v for k, v in body.items() if k != "scale"}
    r = await client.post("/v1/cube/reduced_bin", json=auto, headers=opened["headers"])
    assert r.status_code == 200, r.text
    scale = r.headers["X-Visivo-Scale"]
    if scale == "log":
        llo, lhi = (float(v) for v in r.headers["X-Visivo-Log-Range"].split(","))
        got = cube_reduce.dequantise(r.content, (d, h, w), llo, lhi, "uint8", scale="log")
        ok = np.isfinite(got) & (want > 0)
        assert ok.any()
        err = np.max(np.abs(np.log10(got[ok]) - np.log10(want[ok])))
        assert err <= (lhi - llo) / 254 / 2 + 1e-5
    else:
        assert scale == "linear"
