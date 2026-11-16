"""Several fields of a simulation volume on one grid, for the headset's
overlay (issue #357): /v1/cube/reduced_fields_bin reduces two or three fields
of a VBT volume or a PLUTO output with the same factors, each on its own value
scale, and interleaves their codes (the channel innermost). Each channel is
byte for byte what /v1/cube/reduced_bin gives for that field."""
from __future__ import annotations

import asyncio
import importlib.util
import json
from pathlib import Path

import numpy as np
import pytest

from app import pluto_source as ps
from app.volume_source import vbt_field_ref

_GEN = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"

SHAPE = (12, 16, 20)       # (z, y, x)
NAMES = ("rho", "T", "vx")


def _write_pluto():
    spec = importlib.util.spec_from_file_location("generate_demo_data", _GEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.write_pluto


def _fields() -> dict[str, np.ndarray]:
    """A density over about seven decades, a temperature 1e3..1e7 K and a
    signed velocity: auto gives log, log, linear."""
    k, j, i = np.meshgrid(*(np.arange(n, dtype=np.float64) for n in SHAPE), indexing="ij")
    r = np.sqrt((i - 10) ** 2 + (j - 8) ** 2 + (k - 6) ** 2)
    rho = 1e-12 * 10.0 ** (7.0 * np.exp(-r / 4.0))
    temp = 1e3 * 10.0 ** (4.0 * (i + j + k) / (sum(SHAPE) - 3))
    vx = np.sin(i / 3.0) * 50.0 - j
    return {"rho": rho, "T": temp, "vx": vx}


def _write_vbt(tmp_path: Path) -> Path:
    nz, ny, nx = SHAPE
    data = tmp_path / "sim.bin"
    with open(data, "wb") as fh:
        for f in _fields().values():
            f.astype("<f4").tofile(fh)
    (tmp_path / "sim.bin.head").write_text(
        f"float\n{len(NAMES)}\n{nx * ny * nz} {nx} {ny} {nz} 1 1 1\nlittle\n" + "\n".join(NAMES) + "\n")
    return data


async def _open(client, auth_headers, path):
    r = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    assert r.status_code == 200, r.text
    body = r.json()
    return body["dataset_id"], {**auth_headers, "X-Visivo-Session": body["session_id"]}


async def _post_ready(client, url, body, headers, timeout=120.0):
    waited = False
    for _ in range(int(timeout / 0.25)):
        r = await client.post(url, json=body, headers=headers)
        if r.status_code != 202:
            return r, waited
        waited = True
        got = r.json()
        assert got["preparing"] is True and 0.0 <= got["progress"] <= 1.0
        await asyncio.sleep(0.25)
    raise AssertionError("the pyramids were never built")


def _channels(r):
    hd = r.headers
    w, h, d = int(hd["X-Visivo-Width"]), int(hd["X-Visivo-Height"]), int(hd["X-Visivo-Depth"])
    c = int(hd["X-Visivo-Channels"])
    dtype = np.dtype("<u2") if hd["X-Visivo-Scalar-Type"] == "float16" else np.dtype("u1")
    arr = np.frombuffer(r.content, dtype=dtype)
    assert arr.size == w * h * d * c
    return arr.reshape(d, h, w, c), json.loads(hd["X-Visivo-Fields"])


async def _same_as_reduced_bin(client, auth_headers, r, fields_meta, body, session_headers):
    """Each channel equals reduced_bin of its field with the same factors,
    mode, encoding and the scale the overlay used."""
    chans, meta = _channels(r)
    fx, _fy, fz = (int(v) for v in r.headers["X-Visivo-Bin-Factors"].split(","))
    for c, m in enumerate(meta):
        ds = await client.post("/v1/datasets/open", json={"path": m["path"]}, headers=session_headers)
        assert ds.status_code == 200, ds.text
        single = {"dataset_id": ds.json()["dataset_id"], "factor_xy": fx, "factor_z": fz,
                  "max_dim": body.get("max_dim", 256), "encoding": body.get("encoding", "uint8"),
                  "scale": m["scale"], "source": body.get("source", "auto")}
        one, _ = await _post_ready(client, "/v1/cube/reduced_bin", single, session_headers)
        assert one.status_code == 200, one.text
        assert one.headers["X-Visivo-Bin-Factors"] == r.headers["X-Visivo-Bin-Factors"]
        assert one.headers["X-Visivo-Scale"] == m["scale"]
        assert float(one.headers["X-Visivo-Range-Min"]) == pytest.approx(m["range_min"])
        assert float(one.headers["X-Visivo-Range-Max"]) == pytest.approx(m["range_max"])
        want = np.frombuffer(one.content, dtype=chans.dtype).reshape(chans.shape[:3])
        assert np.array_equal(chans[..., c], want), f"channel {c} ({m['name']}) differs"
    return chans, meta


@pytest.mark.asyncio
async def test_three_vbt_fields_on_one_grid_each_on_its_own_scale(client, auth_headers, tmp_path):
    vbt = _write_vbt(tmp_path)
    dataset, headers = await _open(client, auth_headers, vbt_field_ref(vbt, 0))
    body = {"dataset_id": dataset, "max_dim": 16, "fields": list(NAMES)}
    r = await client.post("/v1/cube/reduced_fields_bin", json=body, headers=headers)
    assert r.status_code == 200, r.text
    hd = r.headers
    assert hd["X-Visivo-Channels"] == "3" and hd["X-Visivo-Scalar-Type"] == "uint8"
    assert hd["X-Visivo-Full-Shape"] == "20,16,12"
    assert hd["X-Visivo-Bin-Factors"] == "2,2,1"          # auto for 16 a side
    chans, meta = await _same_as_reduced_bin(client, auth_headers, r, None, body, headers)
    assert [m["name"] for m in meta] == list(NAMES)
    assert [m["scale"] for m in meta] == ["log", "log", "linear"]
    assert meta[0]["log_min"] < meta[0]["log_max"] and meta[2]["log_min"] is None
    assert meta[2]["range_min"] < 0 < meta[2]["range_max"]
    assert all(m["threshold"] is not None for m in meta)
    assert all(m["source"] == "cube" and m["pyramid_level"] is None for m in meta)
    assert chans.shape == (12, 8, 10, 3)
    # The overlay of the cube on show's own grid: the factors reduced_bin gives it.
    one = await client.post("/v1/cube/reduced_bin", json={"dataset_id": dataset, "max_dim": 16}, headers=headers)
    assert one.headers["X-Visivo-Bin-Factors"] == hd["X-Visivo-Bin-Factors"]


@pytest.mark.asyncio
async def test_given_factors_explicit_scales_float16_and_padding(client, auth_headers, tmp_path):
    vbt = _write_vbt(tmp_path)
    dataset, headers = await _open(client, auth_headers, vbt_field_ref(vbt, 2))
    body = {"dataset_id": dataset, "factor_xy": 4, "factor_z": 3, "fields": ["vx", "rho"],
            "scales": ["linear", "linear"], "channels": 3, "encoding": "float16"}
    r = await client.post("/v1/cube/reduced_fields_bin", json=body, headers=headers)
    assert r.status_code == 200, r.text
    assert r.headers["X-Visivo-Bin-Factors"] == "4,4,3" and r.headers["X-Visivo-Channels"] == "3"
    chans, meta = await _same_as_reduced_bin(client, auth_headers, r, None, body, headers)
    assert [m["scale"] for m in meta] == ["linear", "linear"]
    assert chans.shape == (4, 4, 5, 3)
    assert not chans[..., 2].any(), "the padding channel holds zero codes"
    # One scale for all: the request's own.
    r = await client.post("/v1/cube/reduced_fields_bin",
                          json={"dataset_id": dataset, "factor_xy": 4, "factor_z": 3,
                                "fields": ["rho", "T"], "scale": "log"}, headers=headers)
    assert r.status_code == 200, r.text
    _, meta = _channels(r)
    assert [m["scale"] for m in meta] == ["log", "log"]


@pytest.mark.asyncio
async def test_pluto_variables_on_one_grid(client, auth_headers, tmp_path):
    first = _write_pluto()(tmp_path / "p", _fields(), mode="multiple_files", precision="flt", endian="little")
    # Opened by its data file, as the desktop does.
    dataset, headers = await _open(client, auth_headers, first)
    body = {"dataset_id": dataset, "max_dim": 16, "fields": ["T", "vx", "rho"]}
    r = await client.post("/v1/cube/reduced_fields_bin", json=body, headers=headers)
    assert r.status_code == 200, r.text
    _, meta = await _same_as_reduced_bin(client, auth_headers, r, None, body, headers)
    assert [m["name"] for m in meta] == ["T", "vx", "rho"]
    assert [m["scale"] for m in meta] == ["log", "linear", "log"]
    listed = {f["name"]: f["path"] for f in ps.pluto_fields(first)}
    assert [m["path"] for m in meta] == [listed[n] for n in ("T", "vx", "rho")]


@pytest.mark.asyncio
async def test_bad_requests_are_refused(client, auth_headers, tmp_path, opened_cube):
    vbt = _write_vbt(tmp_path)
    dataset, headers = await _open(client, auth_headers, vbt_field_ref(vbt, 0))
    url = "/v1/cube/reduced_fields_bin"
    base = {"dataset_id": dataset, "max_dim": 16}
    for extra in ({"fields": ["rho"]},                                   # one field
                  {"fields": ["rho", "T", "vx", "rho"]},                 # four
                  {"fields": ["rho", "rho"]},                            # twice
                  {"fields": ["rho", "pressure"]},                       # unknown
                  {"fields": ["rho", "T"], "scales": ["log"]},           # scales length
                  {"fields": ["rho", "T", "vx"], "channels": 2},         # channels < fields
                  {"fields": ["rho", "T"], "channels": 5}):              # past four
        r = await client.post(url, json={**base, **extra}, headers=headers)
        assert r.status_code == 422, (extra, r.status_code, r.text)
    # A cube that is not a field of a multi-field source.
    r = await client.post(url, json={"dataset_id": opened_cube["dataset_id"], "fields": ["a", "b"]},
                          headers=opened_cube["headers"])
    assert r.status_code == 422 and "not a field" in r.text
    # An answer larger than the cap: refused after the first field, the others not reduced.
    from app.routers import cube as cube_router
    monkeypatch_size = 16 * 20 * 12 * 3 - 1      # one byte under three uint8 fields of SHAPE
    old = cube_router.OVERLAY_MAX_BYTES
    cube_router.OVERLAY_MAX_BYTES = monkeypatch_size
    try:
        r = await client.post(url, json={**base, "fields": ["rho", "T", "vx"], "factor_xy": 1, "factor_z": 1}, headers=headers)
    finally:
        cube_router.OVERLAY_MAX_BYTES = old
    assert r.status_code == 422 and "MiB" in r.text, r.text


@pytest.mark.asyncio
async def test_fields_from_their_brick_pyramids(client, auth_headers, tmp_path, monkeypatch):
    """Each field read from its own pyramid: a 202 while any is built (all
    builds started), then one grid, each channel as reduced_bin gives it."""
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    monkeypatch.setenv("VISIVO_VR_PYRAMID_BYTES", "1000")
    first = _write_pluto()(tmp_path / "p", _fields(), mode="multiple_files", precision="flt", endian="little")
    dataset, headers = await _open(client, auth_headers, ps.pluto_ref(first, "rho"))
    body = {"dataset_id": dataset, "max_dim": 16, "fields": ["rho", "T", "vx"]}
    r, waited = await _post_ready(client, "/v1/cube/reduced_fields_bin", body, headers)
    assert r.status_code == 200, r.text
    assert waited, "the first request found no pyramid and answered 202"
    chans, meta = await _same_as_reduced_bin(client, auth_headers, r, None, body, headers)
    assert all(m["source"] == "pyramid" and m["pyramid_level"] is not None for m in meta)
    assert chans.shape[3] == 3


@pytest.mark.asyncio
async def test_fields_of_different_shapes_are_refused_before_any_reduction(
        client, auth_headers, tmp_path, monkeypatch):
    from app import volume_source
    from app.routers import cube as cube_router
    vbt = _write_vbt(tmp_path)
    other = tmp_path / "other"
    other.mkdir()
    small = other / "small.bin"
    np.ones((4, 5, 6), dtype="<f4").tofile(small)
    (other / "small.bin.head").write_text("float\n1\n120 6 5 4 1 1 1\nlittle\nrho\n")
    listed = [{"name": "rho", "path": vbt_field_ref(vbt, 0)},
              {"name": "small", "path": vbt_field_ref(small, 0)}]
    dataset, headers = await _open(client, auth_headers, vbt_field_ref(vbt, 0))
    monkeypatch.setattr(volume_source, "field_listing", lambda _p: (listed, "rho"))
    calls = []
    real = cube_router._run_heavy_with_limit

    async def counted(*a):
        calls.append(a)
        return await real(*a)
    monkeypatch.setattr(cube_router, "_run_heavy_with_limit", counted)
    r = await client.post("/v1/cube/reduced_fields_bin",
                          json={"dataset_id": dataset, "max_dim": 16, "fields": ["rho", "small"]},
                          headers=headers)
    assert r.status_code == 422 and "one grid" in r.text, r.text
    assert not calls, "nothing was reduced"
