"""VBT volumes on the cube path, read in place (issue #83): no conversion, the
field memory-mapped at its offset in the ``.bin``, its HDU the field's index."""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app.brick_pyramid import BrickPyramid, build_pyramid, default_pyramid_path, source_shape
from app.volume_source import is_vbt, vbt_field, volume_data, volume_shape


def _fields(seed=0, shape=(20, 24, 28)) -> list[np.ndarray]:
    rng = np.random.default_rng(seed)
    a = rng.normal(0.0, 1.0, shape).astype(np.float32)
    a[0:4, 0:4, 0:4] = np.nan
    b = rng.lognormal(0.0, 1.0, shape).astype(np.float32)
    return [a, b]


def _write_vbt(tmp_path: Path, fields: list[np.ndarray], endian="little", scalar="float",
               spacing=(1.0, 1.0, 1.0), names=None) -> Path:
    """A VBT volume (``v.bin`` + ``v.bin.head``); returns the ``.bin``."""
    nz, ny, nx = fields[0].shape
    code = ("<" if endian == "little" else ">") + ("f4" if scalar == "float" else "f8")
    data = tmp_path / "v.bin"
    with open(data, "wb") as fh:
        for f in fields:
            f.astype(code).tofile(fh)
    names = names or [f"F{i}" for i in range(len(fields))]
    sx, sy, sz = spacing
    (tmp_path / "v.bin.head").write_text("\n".join(
        [scalar, str(len(fields)), f"{nx * ny * nz} {nx} {ny} {nz} {sx} {sy} {sz}", endian,
         *names]) + "\n")
    return data


@pytest.mark.parametrize("endian, scalar", [("little", "float"), ("big", "float"),
                                            ("big", "double")])
def test_each_field_is_read_in_place(tmp_path, endian, scalar):
    fields = _fields()
    path = _write_vbt(tmp_path, fields, endian, scalar)
    assert is_vbt(path)
    assert volume_shape(path) == ((20, 24, 28), 0)
    assert volume_shape(path, 1) == ((20, 24, 28), 1)
    for i, f in enumerate(fields):
        np.testing.assert_array_equal(np.asarray(vbt_field(path, i), dtype=np.float32), f)
        with volume_data(path, i) as data:
            np.testing.assert_array_equal(np.asarray(data, dtype=np.float32), f)


def test_a_fits_file_with_a_head_beside_it_is_fits(tmp_path):
    path = tmp_path / "c.fits"
    fits.PrimaryHDU(_fields()[0]).writeto(path)
    (tmp_path / "c.fits.head").write_text("float\n1\n1\nlittle\nX\n")
    assert not is_vbt(path)
    assert volume_shape(path) == ((20, 24, 28), 0)


def test_points_a_wrong_field_or_a_short_file_are_refused(tmp_path):
    path = _write_vbt(tmp_path, _fields())
    with pytest.raises(ValueError, match="field 2"):
        vbt_field(path, 2)
    with open(path, "r+b") as fh:
        fh.truncate(100)
    with pytest.raises(ValueError, match="bytes"):
        vbt_field(path, 0)
    points = tmp_path / "p.bin"
    np.zeros(6, "<f4").tofile(points)
    (tmp_path / "p.bin.head").write_text("float\n2\n3\nlittle\nX\nY\n")
    with pytest.raises(ValueError, match="points"):
        vbt_field(points, 0)


def test_the_pyramid_of_a_field_is_that_of_the_same_cube_in_fits(tmp_path):
    fields = _fields()
    path = _write_vbt(tmp_path, fields, endian="big")
    cube = tmp_path / "b.fits"
    fits.PrimaryHDU(fields[1]).writeto(cube)
    assert source_shape(path, 1) == source_shape(cube, 0)
    a = build_pyramid(path, tmp_path / "vbt.pyr.h5", hdu=1, brick=8)
    b = build_pyramid(cube, tmp_path / "fits.pyr.h5", brick=8)
    with BrickPyramid(a, source=path) as pa, BrickPyramid(b, source=cube) as pb:
        assert pa.levels == pb.levels > 1
        for k in range(pa.levels):
            for item in ("data", "min", "max"):
                np.testing.assert_array_equal(pa._h5[f"level{k}/{item}"][...],
                                              pb._h5[f"level{k}/{item}"][...])


def test_each_field_has_its_own_pyramid(tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    path = _write_vbt(tmp_path, _fields())
    assert default_pyramid_path(path, 0) != default_pyramid_path(path, 1)


def test_out_of_core_rendering_reads_the_field(tmp_path, monkeypatch):
    pytest.importorskip("vtk")
    from app.render_session import _ooc

    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    fields = _fields()
    path = _write_vbt(tmp_path, fields)
    build_pyramid(path, default_pyramid_path(path, 1), hdu=1, brick=8)
    ooc = _ooc.OutOfCoreCube(str(path), 1, "test")
    try:
        assert ooc.shape == fields[1].shape
        np.testing.assert_allclose(ooc.plane(7), fields[1][7])
    finally:
        ooc.close()


@pytest.mark.parametrize("endian", ["little", "big"])
def test_the_chunked_reader_reads_the_field(tmp_path, endian):
    from app.cube_reader import CubeReader

    fields = _fields()
    path = _write_vbt(tmp_path, fields, endian=endian)
    reader = CubeReader(str(path), shape=fields[1].shape, chunk_size_z=6, hdu=1)
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
        assert any(e["event"] == "complete" for e in events)
        np.testing.assert_array_equal(reader.cube, np.nan_to_num(fields[1], nan=0.0))
    finally:
        reader.close()


# ── Routes ───────────────────────────────────────────────────────────────────

async def _open(client, auth_headers, head: Path) -> tuple[str, dict]:
    resp = await client.post("/v1/vbt/open", json={"path": str(head)}, headers=auth_headers)
    body = resp.json()
    assert body["valid"], body["error"]
    return body["dataset_id"], {**auth_headers, "X-Visivo-Session": body["session_id"]}


@pytest.fixture
def remote(monkeypatch):
    """A render manager that records what it was asked to draw."""
    from app.routers import render

    asked: dict = {}

    class _Session:
        session_id, owner_token = "c0ffee01", None
        view = type("View", (), {"width": 4, "height": 3})()

        async def render_frame(self):
            return b"jpeg"

        def levels(self):
            return {"threshold": 1.0, "data_min": 0.0, "data_max": 2.0, "pending": False}

    class _Manager:
        async def start(self):
            pass

        async def create_session(self, dataset_path, owner_token=None, hdu=None):
            asked.update(path=dataset_path, hdu=hdu)
            return _Session()

    async def ready(path, hdu):
        asked.update(prepared=(path, hdu))
        return None
    monkeypatch.setattr(render, "server_side_rendering_available", lambda: True)
    monkeypatch.setattr(render, "_pyramid_preparing", ready)
    monkeypatch.setattr(render, "get_render_manager", lambda: _Manager())
    return asked


@pytest.mark.asyncio
async def test_a_field_opened_as_a_cube_is_rendered_from_the_file(client, auth_headers, tmp_path,
                                                                  remote):
    from app.volume_source import vbt_field_ref, with_order

    path = _write_vbt(tmp_path, _fields(), names=["Density", "Temperature"])
    cube = await _open_cube(client, auth_headers, vbt_field_ref(path, 1))
    resp = await client.post("/v1/render/create_session", headers=cube["headers"],
                             json={"dataset_id": cube["dataset_id"]})
    assert resp.status_code == 200, resp.text
    # In the byte order it was opened in (issue #165): the header's here.
    assert remote["path"] == with_order(vbt_field_ref(path, 1), "little")
    # The session reads that field; out of core, from the field's pyramid.
    assert volume_shape(remote["path"]) == ((20, 24, 28), 1)
    assert default_pyramid_path(remote["path"], 0) == default_pyramid_path(path, 1)


def test_a_header_edited_beside_the_same_data_makes_the_pyramid_stale(tmp_path):
    import os

    from app.brick_pyramid import StalePyramid

    path = _write_vbt(tmp_path, _fields())
    out = build_pyramid(path, tmp_path / "v.pyr.h5", hdu=0, brick=8)
    head = Path(str(path) + ".head")
    head.write_text(head.read_text().replace("little", "big"))
    st = os.stat(head)
    os.utime(head, ns=(st.st_atime_ns, st.st_mtime_ns + 10_000_000))
    with pytest.raises(StalePyramid):
        BrickPyramid(out, source=path)


def test_values_that_look_like_gzip_are_not_refused(tmp_path):
    field = np.frombuffer(b"\x1f\x8b\x08\x00" * (10 * 12 * 16), dtype="<f4").reshape(10, 12, 16)
    path = _write_vbt(tmp_path, [field.copy()])
    out = build_pyramid(path, tmp_path / "g.pyr.h5", hdu=0, brick=8)
    with BrickPyramid(out, source=path) as pyr:
        np.testing.assert_array_equal(pyr._h5["level0/data"][...], field)


# ── A field opened as a cube (issue #83) ─────────────────────────────────────

async def _open_cube(client, auth_headers, path) -> dict:
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    body = resp.json()
    assert body["valid"], body["error"]
    return {**body, "headers": {**auth_headers, "X-Visivo-Session": body["session_id"]}}


@pytest.mark.asyncio
async def test_a_vbt_volume_opens_as_a_cube_one_field_at_a_time(client, auth_headers, tmp_path):
    from app.volume_source import vbt_field_ref

    fields = _fields()
    path = _write_vbt(tmp_path, fields, endian="big", spacing=(2.0, 1.0, 0.5),
                      names=["Density", "Temperature"])
    for given in (Path(str(path) + ".head"), path):
        body = await _open_cube(client, auth_headers, given)
        assert body["kind"] == "cube"
        assert (body["width"], body["height"], body["depth"]) == (28, 24, 20)
        assert body["field_name"] == "Density"
        assert [f["name"] for f in body["fields"]] == ["Density", "Temperature"]
        assert body["fields"][1]["path"] == vbt_field_ref(path, 1)
        assert body["cdelt"][:3] == [2.0, 1.0, 0.5]
    second = await _open_cube(client, auth_headers, vbt_field_ref(path, 1))
    assert second["field_name"] == "Temperature"
    # The cube viewer's own routes read the field: a slice is its plane.
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
    np.testing.assert_allclose(plane, fields[1][5], rtol=1e-6)


@pytest.mark.asyncio
async def test_a_field_outside_the_file_or_a_vbt_of_points_does_not_open(client, auth_headers,
                                                                        tmp_path):
    from app.volume_source import vbt_field_ref

    path = _write_vbt(tmp_path, _fields())
    resp = await client.post("/v1/datasets/open", json={"path": vbt_field_ref(path, 5)},
                             headers=auth_headers)
    assert resp.json()["valid"] is False
    np.zeros(6, "<f4").tofile(tmp_path / "p.bin")
    (tmp_path / "p.bin.head").write_text("float\n2\n3\nlittle\nX\nY\n")
    resp = await client.post("/v1/datasets/open", json={"path": str(tmp_path / "p.bin.head")},
                             headers=auth_headers)
    assert resp.json()["valid"] is False


def test_out_of_core_rendering_from_a_field_reference(tmp_path, monkeypatch):
    pytest.importorskip("vtk")
    from app.render_session import _ooc
    from app.volume_source import vbt_field_ref

    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    fields = _fields()
    path = _write_vbt(tmp_path, fields, endian="big")
    ref = vbt_field_ref(path, 1)
    shape, hdu = volume_shape(ref)
    build_pyramid(ref, default_pyramid_path(ref, hdu), hdu=hdu, brick=8)
    ooc = _ooc.OutOfCoreCube(ref, hdu, "test")
    try:
        np.testing.assert_allclose(ooc.plane(9), fields[1][9])
    finally:
        ooc.close()


def test_a_vbt_volume_is_opened_as_a_dataset_and_a_table_of_points_as_a_catalogue(tmp_path):
    from app.classify import classify_path

    path = _write_vbt(tmp_path, _fields())
    for given in (path, Path(str(path) + ".head")):
        verdict = classify_path(given)
        assert (verdict["kind"], verdict["opener"]) == ("cube", "dataset")
    np.zeros(6, "<f4").tofile(tmp_path / "p.bin")
    (tmp_path / "p.bin.head").write_text("float\n2\n3\nlittle\nX\nY\n")
    assert classify_path(tmp_path / "p.bin")["opener"] == "catalogue"


@pytest.mark.asyncio
async def test_a_field_reference_classifies_and_shows_its_header(client, auth_headers, tmp_path):
    from app.volume_source import vbt_field_ref

    path = _write_vbt(tmp_path, _fields(), spacing=(2.0, 1.0, 0.5), names=["Density", "Gas"])
    ref = vbt_field_ref(path, 1)
    verdict = (await client.post("/v1/files/classify", json={"path": ref},
                                 headers=auth_headers)).json()
    assert verdict["valid"] and (verdict["kind"], verdict["opener"]) == ("cube", "dataset")
    header = (await client.post("/v1/files/header", json={"path": ref},
                                headers=auth_headers)).json()
    assert header["valid"], header["error"]
    cards = "\n".join(header["cards"])
    assert "CDELT1  =                  2.0" in cards and "'Gas" in cards
    missing = (await client.post("/v1/files/classify", json={"path": vbt_field_ref(path, 7)},
                                 headers=auth_headers)).json()
    assert missing["valid"] is False
