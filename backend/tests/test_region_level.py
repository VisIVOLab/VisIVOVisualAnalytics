"""A box of one level of detail of a large cube (issue #321): a pyramid level
with its ghosts equals the cube's block means computed here independently,
the edge repeated at the cube's border; level 0 is the cube as subvolume_bin
reads it, for FITS cubes, VBT fields and PLUTO variables, with no pyramid; a
level with no current pyramid is a strided read; nothing ever starts a build
unless pyramid_status is asked to."""
from __future__ import annotations

import json
import warnings
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import pyramid_builds, region_level
from app.brick_pyramid import BrickPyramid, build_pyramid, default_pyramid_path
from app.compute import worker_cube_region_level_raw, worker_cube_subvolume_raw
from app.volume_source import volume_shape
from tests.test_pluto_volume import write_pluto
from tests.test_vbt_volume import _write_vbt

SHAPE = (24, 32, 40)          # z, y, x
ODD = (13, 17, 22)
#: Large enough along x for three levels with the default 64-voxel brick: the
#: routes run their workers in other processes, where no fixture reaches.
WIDE = (24, 32, 140)


def _cube(shape=SHAPE, seed=0, blanks=False) -> np.ndarray:
    rng = np.random.default_rng(seed)
    a = rng.normal(0.0, 1.0, shape).astype(np.float32)
    a[shape[0] // 3:shape[0] // 2, 4:9, 5:11] += 5.0
    if blanks:
        a[:, :5, :7] = np.nan
        a[3, 11, 13] = np.nan
    return a


def _halve(a: np.ndarray) -> np.ndarray:
    """One level up, written independently of the pyramid's builder: each
    2x2x2 block's mean over its finite voxels (the cube's border padded with
    blanks, so a partial block is the mean of what it holds)."""
    pad = [(0, n % 2) for n in a.shape]
    p = np.pad(a.astype(np.float64), pad, constant_values=np.nan)
    nz, ny, nx = (n // 2 for n in p.shape)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        return np.nanmean(p.reshape(nz, 2, ny, 2, nx, 2), axis=(1, 3, 5)).astype(np.float32)


def _level(a: np.ndarray, level: int) -> np.ndarray:
    for _ in range(level):
        a = _halve(a)
    return a


def _expected(level_data: np.ndarray, box, ghost: int) -> np.ndarray:
    """The box (x0, x1, y0, y1, z0, z1) of ``level_data`` with ``ghost``
    samples on every side, the edge repeated outside it."""
    x0, x1, y0, y1, z0, z1 = box
    g = ghost
    p = np.pad(level_data, g, mode="edge") if g else level_data
    return p[z0:z1 + 1 + 2 * g, y0:y1 + 1 + 2 * g, x0:x1 + 1 + 2 * g]


def _decode(answer: dict) -> np.ndarray:
    return np.frombuffer(answer["raw_bytes"], dtype="<f4").reshape(
        answer["depth"], answer["height"], answer["width"])


def _fits(tmp_path: Path, data: np.ndarray, name="cube.fits") -> Path:
    path = tmp_path / name
    fits.PrimaryHDU(data).writeto(path)
    return path


def _build(path, hdu=0) -> Path:
    return build_pyramid(path, default_pyramid_path(path, hdu), hdu=hdu, brick=8)


@pytest.fixture(autouse=True)
def _pyramid_dir(tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))


@pytest.fixture
def small_brick(monkeypatch):
    """A default brick of 8 voxels, so the small test cubes have the levels a
    large cube has with 64 (the levels a strided read offers)."""
    from app import brick_pyramid
    monkeypatch.setattr(brick_pyramid, "default_brick", lambda shape: 8)


@pytest.fixture
def no_build(monkeypatch):
    """Any start of a pyramid build fails the test."""
    monkeypatch.setattr(pyramid_builds, "ensure_pyramid",
                        lambda *a, **k: pytest.fail("a pyramid build was started"))


# ── read_region ─────────────────────────────────────────────────────────────

INTERIOR = (3, 6, 2, 5, 1, 3)                 # x0, x1, y0, y1, z0, z1 at level 1


@pytest.mark.parametrize("shape", [SHAPE, ODD])
@pytest.mark.parametrize("blanks", [False, True])
@pytest.mark.parametrize("level", [1, 2])
@pytest.mark.parametrize("ghost", [0, 1])
def test_a_pyramid_box_is_the_block_mean_with_its_ghosts(tmp_path, shape, blanks, level, ghost):
    data = _cube(shape, blanks=blanks)
    path = _fits(tmp_path, data)
    want = _level(data, level)
    nz, ny, nx = want.shape
    boxes = [
        (1, 2, 1, 2, 1, 2),                                   # interior
        (0, nx - 1, 0, ny - 1, 0, nz - 1),                    # the whole level
        (0, 1, ny - 2, ny - 1, 0, nz - 1),                    # touching four faces
        (nx - 1, nx - 1, 0, 0, nz - 1, nz - 1),               # one corner voxel
    ]
    with BrickPyramid(_build(path), source=path) as pyr:
        assert pyr.shape(level) == want.shape
        for box in boxes:
            x0, x1, y0, y1, z0, z1 = box
            got = pyr.read_region(level, (slice(z0, z1 + 1), slice(y0, y1 + 1),
                                          slice(x0, x1 + 1)), ghost=ghost)
            assert got.dtype == np.float32
            np.testing.assert_allclose(got, _expected(want, box, ghost), rtol=0, atol=2e-6,
                                       equal_nan=True)


def test_read_region_pads_with_blanks_on_request_and_refuses_boxes_outside(tmp_path):
    data = _cube()
    path = _fits(tmp_path, data)
    with BrickPyramid(_build(path), source=path) as pyr:
        got = pyr.read_region(0, (slice(0, 2), slice(0, 2), slice(38, 40)), ghost=1, edge="nan")
        assert got.shape == (4, 4, 4)
        assert np.isnan(got[0]).all() and np.isnan(got[:, :, -1]).all()
        np.testing.assert_array_equal(got[1:, 1:, :-1], data[0:3, 0:3, 37:40])
        for region in [(slice(0, 2), slice(0, 2), slice(39, 41)),
                       (slice(-1, 2), slice(0, 2), slice(0, 2)),
                       (slice(2, 2), slice(0, 2), slice(0, 2))]:
            with pytest.raises(IndexError):
                pyr.read_region(0, region)
        with pytest.raises(IndexError):
            pyr.read_region(pyr.levels, (slice(0, 1),) * 3)
        with pytest.raises(ValueError):
            pyr.read_region(0, (slice(0, 1),) * 3, edge="mirror")


# ── The worker: level 0, the pyramid, the strided read ──────────────────────

def _subvolume(path, box) -> np.ndarray:
    answer = worker_cube_subvolume_raw(str(path), *box)
    return _decode(answer)


@pytest.mark.parametrize("box", [(0, 39, 0, 31, 0, 23), (5, 17, 3, 9, 2, 20), (39, 39, 0, 4, 23, 23)])
def test_level_0_is_the_cube_as_subvolume_bin_reads_it(tmp_path, box, no_build):
    data = _cube(blanks=True)
    path = _fits(tmp_path, data)
    answer = worker_cube_region_level_raw(str(path), 0, *box, 0, None)
    assert answer["source"] == "cube" and answer["voxel_offset"] == 0.0
    assert answer["raw_bytes"] == worker_cube_subvolume_raw(str(path), *box)["raw_bytes"]
    # With its ghosts: the cube around it, the edge repeated outside the cube.
    answer = worker_cube_region_level_raw(str(path), 0, *box, 1, None)
    np.testing.assert_array_equal(_decode(answer), _expected(data, box, 1))
    x0, _, y0, _, z0, _ = box
    assert answer["level_origin"] == (x0 - 1, y0 - 1, z0 - 1)
    assert not (tmp_path / "pyr").exists()


def test_level_0_of_a_vbt_field(tmp_path, no_build):
    from app.volume_source import vbt_field_ref

    fields = [_cube(seed=1), _cube(seed=2, blanks=True)]
    ref = vbt_field_ref(_write_vbt(tmp_path, fields, endian="big"), 1)
    box = (2, 30, 0, 31, 4, 23)
    answer = worker_cube_region_level_raw(ref, 0, *box, 0, None)
    assert answer["source"] == "cube"
    assert answer["raw_bytes"] == worker_cube_subvolume_raw(ref, *box)["raw_bytes"]
    np.testing.assert_array_equal(_decode(answer), fields[1][4:24, 0:32, 2:31])
    answer = worker_cube_region_level_raw(ref, 0, *box, 1, None)
    np.testing.assert_array_equal(_decode(answer), _expected(fields[1], box, 1))


def _pluto(tmp_path, values: np.ndarray) -> str:
    ps = pytest.importorskip("app.pluto_source")
    first = write_pluto(tmp_path, {"rho": values.astype(np.float64)}, mode="multiple_files",
                        precision="flt", endian="little")
    return ps.pluto_ref(first, "rho")


def test_level_0_of_a_pluto_variable(tmp_path, no_build):
    rho = np.abs(_cube(seed=3)) + 1.0
    ref = _pluto(tmp_path, rho)
    box = (0, 9, 7, 31, 0, 5)
    answer = worker_cube_region_level_raw(ref, 0, *box, 0, None)
    assert answer["source"] == "cube"
    assert answer["raw_bytes"] == worker_cube_subvolume_raw(ref, *box)["raw_bytes"]
    answer = worker_cube_region_level_raw(ref, 0, *box, 1, None)
    np.testing.assert_array_equal(_decode(answer), _expected(rho.astype(np.float32), box, 1))


@pytest.mark.parametrize("kind", ["fits", "vbt", "pluto"])
def test_a_higher_level_comes_from_the_pyramid_of_every_kind(tmp_path, kind):
    data = np.abs(_cube(ODD, seed=4)) + 1.0
    if kind == "fits":
        path = str(_fits(tmp_path, data))
    elif kind == "vbt":
        from app.volume_source import vbt_field_ref
        path = vbt_field_ref(_write_vbt(tmp_path, [data, data * 2]), 1)
        data = data * 2
    else:
        path = _pluto(tmp_path, data)
    _, hdu = volume_shape(path)
    pyramid = _build(path, hdu)
    box = (0, 5, 2, 8, 0, 6)
    answer = worker_cube_region_level_raw(path, 1, *box, 1, str(pyramid), "pyramid")
    assert answer["source"] == "pyramid" and answer["voxel_offset"] == 0.5
    np.testing.assert_allclose(_decode(answer), _expected(_level(data.astype(np.float32), 1),
                                                          box, 1), rtol=1e-6, atol=1e-5)


@pytest.mark.parametrize("shape", [SHAPE, ODD])
@pytest.mark.parametrize("level", [1, 2, 3])
def test_without_a_pyramid_a_level_is_a_strided_read(tmp_path, shape, level, no_build,
                                                      small_brick):
    if level >= len(region_level.default_level_shapes(shape)):
        pytest.skip("past the cube's levels")
    data = _cube(shape, blanks=True)
    path = _fits(tmp_path, data)
    f = 1 << level
    strided = data[::f, ::f, ::f]
    nz, ny, nx = strided.shape
    for box in [(0, nx - 1, 0, ny - 1, 0, nz - 1), (nx - 1, nx - 1, 0, 0, 0, nz - 1)]:
        for ghost in (0, 1):
            answer = worker_cube_region_level_raw(str(path), level, *box, ghost, None)
            assert answer["source"] == "strided" and answer["voxel_offset"] == 0.0
            np.testing.assert_array_equal(_decode(answer), _expected(strided, box, ghost))
    # source "strided" ignores a pyramid that is there.
    pyramid = _build(path)
    answer = worker_cube_region_level_raw(str(path), level, 0, 0, 0, 0, 0, 0, 0,
                                          str(pyramid), "strided")
    assert answer["source"] == "strided"
    assert not np.isnan(_decode(answer)).any() or np.isnan(data[0, 0, 0])


def test_a_stale_pyramid_falls_back_to_the_strided_read(tmp_path, small_brick):
    import os
    data = _cube()
    path = _fits(tmp_path, data)
    built = _build(path)
    changed = data + 1.0
    fits.PrimaryHDU(changed).writeto(path, overwrite=True)
    st = os.stat(path)
    os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns + 10 ** 9))
    answer = worker_cube_region_level_raw(str(path), 1, 0, 3, 0, 3, 0, 3, 1, str(built))
    assert answer["source"] == "strided"
    np.testing.assert_array_equal(_decode(answer), _expected(changed[::2, ::2, ::2],
                                                             (0, 3, 0, 3, 0, 3), 1))
    with pytest.raises(region_level.NoPyramid):
        worker_cube_region_level_raw(str(path), 1, 0, 3, 0, 3, 0, 3, 1, str(built), "pyramid")
    with pytest.raises(region_level.NoPyramid):
        worker_cube_region_level_raw(str(path), 1, 0, 3, 0, 3, 0, 3, 1, None, "pyramid")


@pytest.mark.parametrize("level, box", [
    (6, (0, 0, 0, 0, 0, 0)),               # past the cube's levels (8³ bricks: 0..2)
    (0, (0, 40, 0, 0, 0, 0)),              # x past the cube
    (1, (0, 0, 0, 16, 0, 0)),              # y past level 1
    (0, (3, 2, 0, 0, 0, 0)),               # inverted
    (0, (-1, 2, 0, 0, 0, 0)),              # negative
])
def test_a_box_outside_its_level_is_refused(tmp_path, level, box):
    path = _fits(tmp_path, _cube())
    with pytest.raises(ValueError):
        worker_cube_region_level_raw(str(path), level, *box, 1, None)


# ── The routes ──────────────────────────────────────────────────────────────

async def _open(client, auth_headers, path) -> tuple[dict, str]:
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    opened = resp.json()
    assert opened["valid"], opened["error"]
    return {**auth_headers, "X-Visivo-Session": opened["session_id"]}, opened["dataset_id"]


def _body(dataset_id, level, box, ghost=1, source="auto") -> dict:
    x0, x1, y0, y1, z0, z1 = box
    return {"dataset_id": dataset_id, "level": level, "x0": x0, "x1": x1, "y0": y0, "y1": y1,
            "z0": z0, "z1": z1, "ghost": ghost, "source": source}


def _body_array(resp) -> np.ndarray:
    h = resp.headers
    return np.frombuffer(resp.content, dtype="<f4").reshape(
        int(h["X-Visivo-Depth"]), int(h["X-Visivo-Height"]), int(h["X-Visivo-Width"]))


async def test_the_route_answers_each_source_with_its_headers(client, auth_headers, tmp_path,
                                                             no_build):
    data = _cube(WIDE, blanks=True)
    path = _fits(tmp_path, data)
    headers, did = await _open(client, auth_headers, path)
    box = (0, 3, 1, 4, 0, 2)

    resp = await client.post("/v1/cube/region_level_bin", json=_body(did, 2, box), headers=headers)
    assert resp.status_code == 200, resp.text
    h = resp.headers
    assert h["content-type"] == "application/octet-stream"
    assert (h["X-Visivo-Source"], h["X-Visivo-Level"]) == ("strided", "2")
    assert h["X-Visivo-Level-Origin"] == "-1,0,-1"
    assert float(h["X-Visivo-Voxel-Offset"]) == 0.0
    assert (h["X-Visivo-Width"], h["X-Visivo-Height"], h["X-Visivo-Depth"]) == ("6", "6", "5")
    assert h["X-Visivo-Byte-Order"] == "little"
    want = _expected(data[::4, ::4, ::4], box, 1)
    np.testing.assert_array_equal(_body_array(resp), want)
    assert float(h["X-Visivo-Range-Min"]) == float(np.nanmin(want))
    assert float(h["X-Visivo-Range-Max"]) == float(np.nanmax(want))
    assert not (tmp_path / "pyr").exists()                 # asking built nothing

    resp = await client.post("/v1/cube/region_level_bin", json=_body(did, 2, box, source="pyramid"),
                             headers=headers)
    assert resp.status_code == 409

    _build(path)
    resp = await client.post("/v1/cube/region_level_bin", json=_body(did, 2, box), headers=headers)
    assert resp.status_code == 200, resp.text
    assert resp.headers["X-Visivo-Source"] == "pyramid"
    assert float(resp.headers["X-Visivo-Voxel-Offset"]) == 1.5
    np.testing.assert_allclose(_body_array(resp), _expected(_level(data, 2), box, 1),
                               rtol=0, atol=2e-6, equal_nan=True)

    resp = await client.post("/v1/cube/region_level_bin",
                             json=_body(did, 2, box, source="strided"), headers=headers)
    assert resp.headers["X-Visivo-Source"] == "strided"
    np.testing.assert_array_equal(_body_array(resp), want)

    resp = await client.post("/v1/cube/region_level_bin", json=_body(did, 0, box, ghost=0),
                             headers=headers)
    assert resp.headers["X-Visivo-Source"] == "cube"
    assert resp.headers["X-Visivo-Level-Origin"] == "0,1,0"
    np.testing.assert_array_equal(_body_array(resp), data[0:3, 1:5, 0:4])


async def test_the_route_refuses_bad_levels_boxes_and_oversized_answers(client, auth_headers,
                                                                       tmp_path, monkeypatch):
    path = _fits(tmp_path, _cube())
    headers, did = await _open(client, auth_headers, path)
    post = client.post
    for level, box in [(9, (0, 0, 0, 0, 0, 0)), (0, (0, 40, 0, 0, 0, 0)),
                       (2, (0, 0, 0, 8, 0, 0)), (0, (2, 1, 0, 0, 0, 0))]:
        resp = await post("/v1/cube/region_level_bin", json=_body(did, level, box), headers=headers)
        assert resp.status_code == 422, (level, box, resp.text)
    resp = await post("/v1/cube/region_level_bin", json=_body(did, -1, (0, 0, 0, 0, 0, 0)),
                      headers=headers)
    assert resp.status_code == 422
    resp = await post("/v1/cube/region_level_bin", json={**_body(did, 0, (0, 0, 0, 0, 0, 0)),
                                                         "ghost": 2}, headers=headers)
    assert resp.status_code == 422
    # 4 * 6³ = 864 bytes with the ghosts: over a limit of 800.
    monkeypatch.setenv("VISIVO_REGION_LEVEL_MAX_BYTES", "800")
    resp = await post("/v1/cube/region_level_bin", json=_body(did, 0, (0, 3, 0, 3, 0, 3)),
                      headers=headers)
    assert resp.status_code == 413
    resp = await post("/v1/cube/region_level_bin", json=_body(did, 0, (0, 3, 0, 3, 0, 3), ghost=0),
                      headers=headers)
    assert resp.status_code == 200


# ── pyramid_status ──────────────────────────────────────────────────────────

async def _status(client, headers, did, build=False):
    resp = await client.post("/v1/cube/pyramid_status", json={"dataset_id": did, "build": build},
                             headers=headers)
    return resp.status_code, resp.json()


async def test_pyramid_status_missing_then_ready(client, auth_headers, tmp_path, no_build):
    path = _fits(tmp_path, _cube(ODD))
    headers, did = await _open(client, auth_headers, path)
    code, answer = await _status(client, headers, did)
    assert code == 200 and answer["state"] == "missing" and not answer["preparing"]
    # The levels a default build has: 64-voxel bricks, one level for this cube.
    assert answer["brick"] == 64 and answer["levels"] == 1
    assert answer["shapes"] == [list(ODD)]
    assert answer["bytes_estimate"] == 4 * int(np.prod(ODD)) + 8
    assert not (tmp_path / "pyr").exists()

    built = _build(path)
    code, answer = await _status(client, headers, did)
    assert code == 200 and answer["state"] == "ready" and answer["progress"] == 1.0
    assert answer["brick"] == 8 and answer["levels"] == 3
    assert answer["shapes"] == [[13, 17, 22], [7, 9, 11], [4, 5, 6]]
    assert answer["bytes_estimate"] == built.stat().st_size


async def test_pyramid_status_builds_only_when_asked(client, auth_headers, tmp_path, monkeypatch):
    path = _fits(tmp_path, _cube())
    headers, did = await _open(client, auth_headers, path)
    calls = []

    def ensure(dataset_path, hdu):
        calls.append((dataset_path, hdu))
        return pyramid_builds.BuildStatus("building", 0.25)

    monkeypatch.setattr(pyramid_builds, "ensure_pyramid", ensure)
    code, answer = await _status(client, headers, did)
    assert answer["state"] == "missing" and not calls
    code, answer = await _status(client, headers, did, build=True)
    assert code == 202 and calls == [(str(path), 0)]
    assert answer["state"] == "building" and answer["preparing"] and answer["progress"] == 0.25

    monkeypatch.setattr(pyramid_builds, "ensure_pyramid",
                        lambda *a: pyramid_builds.BuildStatus("failed", 0.0, "disk full"))
    code, answer = await _status(client, headers, did, build=True)
    assert code == 200 and answer["state"] == "failed" and answer["error"] == "disk full"

    def refuse(*a):
        raise PermissionError("read-only directory")

    monkeypatch.setattr(pyramid_builds, "ensure_pyramid", refuse)
    code, answer = await _status(client, headers, did, build=True)
    assert code == 200 and answer["state"] == "failed" and "read-only" in answer["error"]


async def test_pyramid_status_reports_a_build_without_starting_one(client, auth_headers, tmp_path,
                                                                   no_build):
    path = _fits(tmp_path, _cube())
    headers, did = await _open(client, auth_headers, path)
    out = default_pyramid_path(path, 0)
    progress = pyramid_builds._progress_path(out)
    out.parent.mkdir(parents=True)

    class Alive:
        exitcode = None

        def is_alive(self):
            return True

    key = str(out)
    pyramid_builds._builds[key] = pyramid_builds._Build(Alive(), progress, 0.0)
    try:
        progress.write_text(json.dumps({"fraction": 0.5}))
        code, answer = await _status(client, headers, did)
        assert code == 202 and answer["state"] == "building" and answer["progress"] == 0.5
    finally:
        pyramid_builds._builds.pop(key, None)
    # A build that failed (here, or on another backend sharing the directory)
    # left its error in the progress file.
    progress.write_text(json.dumps({"fraction": 0.0, "error": "MemoryError: "}))
    code, answer = await _status(client, headers, did)
    assert code == 200 and answer["state"] == "failed" and answer["error"] == "MemoryError: "


async def test_pyramid_status_of_a_cube_no_pyramid_holds(client, auth_headers, tmp_path,
                                                         no_build):
    # A cube whose last three axes are not its cube (an axis of one plane
    # among them): no pyramid holds it, the desktop reads it squeezed.
    cube = _cube(WIDE)
    path = _fits(tmp_path, cube[:, np.newaxis], "spare.fits")      # (z, 1, y, x)
    headers, did = await _open(client, auth_headers, path)
    code, answer = await _status(client, headers, did, build=True)
    assert code == 200 and answer["state"] == "unsupported"
    # Its levels are still offered, strided, on the cube the desktop reads.
    assert answer["shapes"] == [[24, 32, 140], [12, 16, 70], [6, 8, 35]]
    resp = await client.post("/v1/cube/region_level_bin",
                             json=_body(did, 1, (0, 1, 0, 1, 0, 1)), headers=headers)
    assert resp.status_code == 200 and resp.headers["X-Visivo-Source"] == "strided"
    np.testing.assert_array_equal(_body_array(resp),
                                  _expected(cube[::2, ::2, ::2], (0, 1, 0, 1, 0, 1), 1))
