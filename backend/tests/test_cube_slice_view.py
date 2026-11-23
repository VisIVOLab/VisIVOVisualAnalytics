"""A channel answered at the resolution of the view (issue #381).

A 7233 x 7233 channel is 209 MB of float32: answered whole it took longer
than the desktop's 30 s on a cluster. /v1/cube/slice now reads it every
``stride`` pixels so neither side exceeds ``max_longest_side``, and a region
of it for a zoomed view. Every sample is a pixel of the cube, so the answers
are compared with numpy slicing of the same data.
"""

from __future__ import annotations

import base64
import time
import zlib
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app.compute.cube import plane_view, worker_cube_slice


def _decode(body: dict) -> np.ndarray:
    raw = base64.b64decode(body["data_base64"])
    if body["compression"] == "qt-zlib":
        raw = zlib.decompress(raw[4:])
    return np.frombuffer(raw, dtype=np.float32).reshape(body["height"], body["width"])


def _cube(tmp_path: Path, shape=(3, 301, 517), name="wide.fits") -> tuple[Path, np.ndarray]:
    rng = np.random.default_rng(381)
    data = rng.normal(0.0, 1.0, shape).astype(np.float32)
    data[1, 10:20, 30:40] = np.nan
    path = tmp_path / name
    fits.PrimaryHDU(data.astype(">f4")).writeto(path)
    return path, data


# ── The sampling rule ────────────────────────────────────────────────────────

@pytest.mark.parametrize("width,height,limit,expected", [
    (7233, 7233, 2048, (0, 7232, 0, 7232, 4)),
    (661, 661, 2048, (0, 660, 0, 660, 1)),
    (2048, 2048, 2048, (0, 2047, 0, 2047, 1)),
    (2049, 10, 2048, (0, 2048, 0, 9, 2)),
    (7233, 7233, 0, (0, 7232, 0, 7232, 1)),
])
def test_the_stride_is_the_smallest_that_fits(width, height, limit, expected):
    assert plane_view(width, height, limit) == expected


def test_a_region_is_clamped_and_sampled_on_its_own_size():
    assert plane_view(7233, 7233, 2048, (-50, 1999, 7000, 9000)) == (0, 1999, 7000, 7232, 1)
    assert plane_view(7233, 7233, 2048, (1000, 5999, 1000, 1999)) == (1000, 5999, 1000, 1999, 3)
    with pytest.raises(ValueError):
        plane_view(100, 100, 64, (200, 300, 0, 10))


# ── The worker against numpy ─────────────────────────────────────────────────

@pytest.mark.parametrize("limit,region", [
    (0, None),
    (128, None),
    (100, None),
    (64, (40, 300, 5, 250)),
    (0, (500, 600, 290, 400)),
])
def test_the_samples_are_the_cubes_own_pixels(tmp_path, limit, region):
    path, data = _cube(tmp_path)
    out = worker_cube_slice(str(path), 1, limit, region)
    x0, x1, y0, y1, s = plane_view(517, 301, limit, region)
    expected = data[1, y0:y1 + 1:s, x0:x1 + 1:s]
    assert (out["x0"], out["y0"], out["stride"]) == (x0, y0, s)
    assert (out["full_width"], out["full_height"]) == (517, 301)
    got = _decode(out)
    np.testing.assert_array_equal(got, expected)
    assert max(out["width"], out["height"]) <= (limit or max(517, 301))
    finite = expected[np.isfinite(expected)]
    assert out["range_min"] == pytest.approx(float(finite.min()))
    assert out["range_max"] == pytest.approx(float(finite.max()))


def test_a_view_sized_answer_of_a_wide_plane_stays_small_and_quick(tmp_path):
    # 4096 x 4096 is 64 MB a channel; at a 512 budget the answer is 1 MB. The
    # bound on time is loose (it takes tens of ms) so it fails only when the
    # whole plane is read and encoded again, which takes seconds.
    path, _ = _cube(tmp_path, shape=(2, 4096, 4096), name="big.fits")
    worker_cube_slice(str(path), 0, 512)
    t0 = time.perf_counter()
    out = worker_cube_slice(str(path), 1, 512)
    elapsed = time.perf_counter() - t0
    assert (out["width"], out["height"], out["stride"]) == (512, 512, 8)
    assert len(out["data_base64"]) < 2 * 512 * 512 * 4
    assert elapsed < 2.0, elapsed


# ── The route ────────────────────────────────────────────────────────────────

async def _open(client, auth_headers, path) -> dict:
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    body = resp.json()
    assert body["valid"], body["error"]
    return {**body, "headers": {**auth_headers, "X-Visivo-Session": body["session_id"]}}


@pytest.mark.asyncio
async def test_the_route_answers_a_reduced_channel_and_a_region(client, auth_headers, tmp_path):
    path, data = _cube(tmp_path)
    cube = await _open(client, auth_headers, path)
    ask = {"dataset_id": cube["dataset_id"], "axis": "z", "index": 2}

    resp = await client.post("/v1/cube/slice", headers=cube["headers"],
                             json={**ask, "max_longest_side": 128})
    body = resp.json()
    assert body["valid"], body["error"]
    assert (body["stride"], body["x0"], body["y0"]) == (5, 0, 0)
    np.testing.assert_array_equal(_decode(body), data[2, ::5, ::5])

    resp = await client.post("/v1/cube/slice", headers=cube["headers"],
                             json={**ask, "max_longest_side": 128,
                                   "x0": 100, "x1": 199, "y0": 50, "y1": 89})
    body = resp.json()
    assert body["valid"], body["error"]
    assert (body["stride"], body["x0"], body["y0"]) == (1, 100, 50)
    np.testing.assert_array_equal(_decode(body), data[2, 50:90, 100:200])


@pytest.mark.asyncio
async def test_a_client_that_asks_nothing_gets_every_pixel(client, auth_headers, tmp_path):
    path, data = _cube(tmp_path)
    cube = await _open(client, auth_headers, path)
    resp = await client.post("/v1/cube/slice", headers=cube["headers"],
                             json={"dataset_id": cube["dataset_id"], "axis": "z", "index": 0})
    body = resp.json()
    assert body["valid"], body["error"]
    assert (body["width"], body["height"], body["stride"]) == (517, 301, 1)
    np.testing.assert_array_equal(_decode(body), data[0])


@pytest.mark.asyncio
async def test_half_a_region_is_refused(client, auth_headers, tmp_path):
    path, _ = _cube(tmp_path)
    cube = await _open(client, auth_headers, path)
    resp = await client.post("/v1/cube/slice", headers=cube["headers"],
                             json={"dataset_id": cube["dataset_id"], "axis": "z", "index": 0,
                                   "x0": 1, "x1": 10})
    assert resp.status_code == 422
    resp = await client.post("/v1/cube/slice", headers=cube["headers"],
                             json={"dataset_id": cube["dataset_id"], "axis": "z", "index": 0,
                                   "x0": 900, "x1": 990, "y0": 0, "y1": 10})
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_a_vbt_field_is_sampled_through_the_same_seam(client, auth_headers, tmp_path):
    from app.volume_source import vbt_field_ref
    from tests.test_vbt_volume import _fields, _write_vbt

    fields = _fields()
    path = _write_vbt(tmp_path, fields, endian="big")
    cube = await _open(client, auth_headers, vbt_field_ref(path, 1))
    resp = await client.post("/v1/cube/slice", headers=cube["headers"],
                             json={"dataset_id": cube["dataset_id"], "axis": "z", "index": 4,
                                   "max_longest_side": 10, "x0": 2, "x1": 25, "y0": 1, "y1": 20})
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["stride"] == 3
    np.testing.assert_allclose(_decode(body), fields[1][4, 1:21:3, 2:26:3], rtol=1e-6)


@pytest.mark.asyncio
async def test_a_pluto_variable_is_sampled_through_the_same_seam(client, auth_headers, tmp_path):
    from tests.test_pluto_volume import _fields, write_pluto

    fields = _fields()
    first = write_pluto(tmp_path, fields, precision="flt", endian="big")
    cube = await _open(client, auth_headers, first)
    resp = await client.post("/v1/cube/slice", headers=cube["headers"],
                             json={"dataset_id": cube["dataset_id"], "axis": "z", "index": 3,
                                   "max_longest_side": 8})
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["stride"] == 3
    np.testing.assert_allclose(_decode(body), fields[cube["field_name"]][3, ::3, ::3], rtol=1e-6)
