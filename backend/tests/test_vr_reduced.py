"""The headset's reduced cube (issue #167): app/cube_reduce.py, the routes
/v1/cube/reduced_info and /v1/cube/reduced_bin, the /vr page, and the page's
JavaScript unit tests (run with node when it is installed)."""
from __future__ import annotations

import math
import re
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS
from httpx import AsyncClient

from app import cube_reduce

C_KMS = 299792.458


# ── Reduction rules ─────────────────────────────────────────────────────────

def _brute(a: np.ndarray, f, mode):
    fx, fy, fz = f
    d, h, w = a.shape
    out = np.full((-(-d // fz), -(-h // fy), -(-w // fx)), np.nan, dtype=np.float64)
    for k in range(out.shape[0]):
        for j in range(out.shape[1]):
            for i in range(out.shape[2]):
                blk = a[k * fz:(k + 1) * fz, j * fy:(j + 1) * fy, i * fx:(i + 1) * fx]
                fin = blk[np.isfinite(blk)]
                if fin.size:
                    out[k, j, i] = fin.mean() if mode == "mean" else fin.max()
    return out


@pytest.mark.parametrize("mode", ["mean", "max"])
@pytest.mark.parametrize("factors", [(1, 1, 1), (2, 2, 1), (3, 3, 2), (4, 4, 4)])
def test_reduce_block_matches_a_brute_force_reduction(mode, factors):
    """NaN-aware over each block's finite voxels, partial blocks at the edges
    reduced over what they hold, all-blank blocks blank."""
    rng = np.random.default_rng(3)
    a = rng.normal(size=(7, 10, 11)).astype(np.float32)
    a[0:4, 0:4, 0:4] = np.nan        # an all-blank block for every factor <= 4
    a[3, 5, 6] = np.nan
    got = cube_reduce.reduce_block(a, factors, mode)
    want = _brute(a, factors, mode)
    assert got.shape == want.shape
    np.testing.assert_allclose(got, want, rtol=1e-5, atol=1e-6, equal_nan=True)
    if max(factors[:2]) == 4:
        assert np.isnan(got[0, 0, 0])


def test_reduce_cube_in_bands_equals_one_block():
    """The bounded-memory path (slabs of fz planes in bands of rows) gives the
    same cube as reducing it in one piece."""
    rng = np.random.default_rng(5)
    a = rng.normal(size=(9, 37, 23)).astype(np.float32)
    for mode in ("mean", "max"):
        whole = cube_reduce.reduce_block(a, (3, 3, 2), mode)
        banded = cube_reduce.reduce_cube(a, (3, 3, 2), mode, max_block_bytes=1)
        np.testing.assert_array_equal(whole, banded)


def test_auto_factors_bring_every_axis_under_max_dim():
    assert cube_reduce.auto_factors((158, 700, 700), 256) == (3, 3, 1)
    assert cube_reduce.auto_factors((109, 280, 196), 256) == (2, 2, 1)
    assert cube_reduce.auto_factors((1000, 100, 100), 256) == (1, 1, 4)
    d, h, w = cube_reduce.reduced_shape((158, 700, 700), (3, 3, 1))
    assert (w, h, d) == (234, 234, 158)


def test_reduced_info_revision_detects_same_shape_replacement_with_preserved_mtime(synthetic_fits_cube, tmp_path):
    import os
    from astropy.io import fits
    from app.compute.reduced import worker_cube_reduced_info

    path = tmp_path / "replaced.fits"
    data, header = fits.getdata(synthetic_fits_cube, header=True)
    fits.writeto(path, data, header)
    before = worker_cube_reduced_info(str(path), 0, 0, 256)
    assert before["file_revision"] == worker_cube_reduced_info(str(path), 0, 0, 256)["file_revision"]
    stat = path.stat()
    header["CRVAL1"] += 1.0
    replacement = tmp_path / "replacement.fits"
    fits.writeto(replacement, data, header)
    os.replace(replacement, path)
    os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    after = worker_cube_reduced_info(str(path), 0, 0, 256)
    assert after["full_shape"] == before["full_shape"]
    assert after["file_revision"] != before["file_revision"]
    assert after["axes"]["x"]["world"] != before["axes"]["x"]["world"]


@pytest.mark.asyncio
async def test_automatic_factors_above_64_work_for_metadata_and_binary(client, auth_headers, tmp_path):
    from astropy.io import fits

    path = tmp_path / "wide.fits"
    fits.writeto(path, np.ones((3, 4, 17000), dtype=np.float32))
    opened = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    assert opened.status_code == 200, opened.text
    request = {"dataset_id": opened.json()["dataset_id"], "max_dim": 256, "factor_xy": 0, "factor_z": 0}
    info = await client.post("/v1/cube/reduced_info", json=request, headers=auth_headers)
    assert info.status_code == 200, info.text
    assert info.json()["factors"] == [67, 67, 1]
    binary = await client.post("/v1/cube/reduced_bin", json=request, headers=auth_headers)
    assert binary.status_code == 200, binary.text
    assert binary.headers["X-Visivo-Bin-Factors"] == "67,67,1"


def test_check_size_refuses_what_no_headset_texture_holds():
    cube_reduce.check_size((256, 256, 256))
    cube_reduce.check_size((256, 512, 512))                    # 64 Mi voxels, 64 MiB of uint8
    with pytest.raises(ValueError):
        cube_reduce.check_size((10, 10, 4096))                 # one side over 2048
    with pytest.raises(ValueError):
        cube_reduce.check_size((1024, 1024, 1024))
    # The review's case: 661 x 661 x 1218 at 1 x 1 x 2 is 266 M voxels, every
    # side under 2048: 1 GB of float32 on the backend, 266 MB of texture.
    with pytest.raises(ValueError):
        cube_reduce.check_size(cube_reduce.reduced_shape((1218, 661, 661), (1, 1, 2)))
    # The byte cap halves the voxels of a float16 texture.
    cube_reduce.check_size((160, 512, 512), "uint8")           # 40 Mi voxels, 40 MiB
    with pytest.raises(ValueError):
        cube_reduce.check_size((160, 512, 512), "float16")     # 80 MiB


@pytest.mark.parametrize("encoding", ["uint8", "float16"])
def test_quantise_peak_memory_is_a_plane_not_the_cube(encoding):
    """Quantising used to hold four cube-sized float32 temporaries at once."""
    import tracemalloc
    v = np.random.default_rng(0).normal(size=(64, 256, 256)).astype(np.float32)
    out_bytes = v.size * (1 if encoding == "uint8" else 2)
    tracemalloc.start()
    raw = cube_reduce.quantise(v, -5.0, 5.0, encoding)
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    assert len(raw) == out_bytes
    # The output array, its bytes copy, and a few plane temporaries.
    assert peak < 2 * out_bytes + 16 * v[0].nbytes, (peak, out_bytes)


def test_max_binning_threshold_on_pure_noise_draws_no_more_than_the_cube():
    """The noise rule reads the lower tail, which a block maximum compresses, so
    on the reduced cube it set too low a cut in max mode. The max-mode threshold
    is the full cube's: a block is drawn only if one of its voxels would be."""
    from app.compute.reduced import binning_threshold
    from app.noise_threshold import default_threshold, stride_for
    # HCG 44-like size: on it the old rule drew 0.15 % of a 3x3x1 max cube.
    data = np.random.default_rng(0).normal(size=(158, 300, 300)).astype(np.float32)
    st = stride_for(data.size)
    full_thr = default_threshold(data[::st, ::st, ::st], data.size)
    full_above = int((data > full_thr).sum())
    for factors in [(3, 3, 1), (4, 4, 2)]:
        red = cube_reduce.reduce_cube(data, factors, "max")
        thr = binning_threshold(data, red, "max")
        assert thr == pytest.approx(full_thr)
        drawn = int((red >= thr).sum())
        assert drawn <= full_above
        assert drawn / red.size < 5e-4
        # The rule applied to the reduced cube itself drew far more.
        rst = stride_for(red.size)
        own = default_threshold(red[::rst, ::rst, ::rst], red.size)
        assert int((red >= own).sum()) > 5 * max(1, drawn)
    # Mean binning keeps the reduced cube's own rule: lower noise, lower cut.
    red = cube_reduce.reduce_cube(data, (3, 3, 1), "mean")
    assert binning_threshold(data, red, "mean") < 0.5 * full_thr


@pytest.mark.parametrize("encoding", ["uint8", "float16"])
def test_quantise_round_trip(encoding):
    """uint8: within half a code of the range; float16: three significant
    digits; blank stays blank either way."""
    rng = np.random.default_rng(1)
    v = rng.normal(size=(4, 5, 6)).astype(np.float32)
    v[1, 2, 3] = np.nan
    lo, hi = cube_reduce.finite_range(v)
    raw = cube_reduce.quantise(v, lo, hi, encoding)
    assert len(raw) == v.size * (1 if encoding == "uint8" else 2)
    back = cube_reduce.dequantise(raw, v.shape, lo, hi, encoding)
    assert np.isnan(back[1, 2, 3])
    fin = np.isfinite(v)
    tol = (hi - lo) / 254 / 2 + 1e-6 if encoding == "uint8" else (hi - lo) * 1e-3
    assert np.max(np.abs(back[fin] - v[fin])) <= tol
    if encoding == "uint8":
        codes = np.frombuffer(raw, dtype=np.uint8)
        assert codes.min() == 0 and codes[fin.ravel()].min() >= 1, "0 is reserved for blank"


def test_block_centres():
    np.testing.assert_array_equal(cube_reduce.block_centres(7, 3), [1.0, 4.0, 6.0])
    np.testing.assert_array_equal(cube_reduce.block_centres(4, 1), [0, 1, 2, 3])


# ── Routes ──────────────────────────────────────────────────────────────────

def _source(path) -> np.ndarray:
    with fits.open(str(path)) as hdul:
        return np.asarray(np.squeeze(hdul[0].data), dtype=np.float32)


@pytest.mark.asyncio
async def test_reduced_info_gives_factors_threshold_and_world_axes(
        client: AsyncClient, opened_cube: dict, synthetic_fits_cube) -> None:
    resp = await client.post("/v1/cube/reduced_info",
                             json={"dataset_id": opened_cube["dataset_id"], "factor_xy": 2,
                                   "factor_z": 1}, headers=opened_cube["headers"])
    assert resp.status_code == 200, resp.text
    info = resp.json()
    assert info["full_shape"] == [64, 64, 32]
    assert info["render_out_of_core"] is False      # 512 KB, far below the in-core limit
    assert info["factors"] == [2, 2, 1]
    assert info["reduced_shape"] == [32, 32, 32]
    assert info["default_threshold"] is not None and info["default_threshold"] > 0
    ax = info["axes"]
    assert ax["x"]["name"] == "RA" and ax["y"]["name"] == "DEC"
    assert len(ax["x"]["world"]) == 32 and len(ax["z"]["world"]) == 32

    # x: RA of each reduced column centre along the field's central row, with
    # the file's full WCS (SIN), and decreasing because CDELT1 < 0.
    with fits.open(str(synthetic_fits_cube)) as hdul:
        w = WCS(hdul[0].header)
    centres = cube_reduce.block_centres(64, 2)
    ra, _, _ = w.pixel_to_world_values(centres, np.full(32, 31.5), np.full(32, 15.0))
    np.testing.assert_allclose(ax["x"]["world"], ra, rtol=0, atol=1e-9)
    assert ax["x"]["world"][0] > ax["x"]["world"][-1]
    # z: FREQ with RESTFRQ -> optical velocity cz = c (nu0/nu - 1), channel 0 first.
    h = hdul[0].header
    nu = h["CRVAL3"] + (np.arange(32) + 1 - h["CRPIX3"]) * h["CDELT3"]
    v = C_KMS * (h["RESTFRQ"] / nu - 1.0)
    np.testing.assert_allclose(ax["z"]["velocity_kms"], v, rtol=1e-9)
    assert ax["z"]["velocity_kms"][0] < ax["z"]["velocity_kms"][-1], "CDELT3 < 0: velocity rises"
    assert "optical" in ax["z"]["velocity_convention"]
    assert math.isclose(ax["z"]["channel_width_kms"], float(np.mean(np.abs(np.diff(v)))), rel_tol=1e-9)


@pytest.mark.asyncio
@pytest.mark.parametrize("mode", ["mean", "max"])
@pytest.mark.parametrize("encoding", ["uint8", "float16"])
async def test_reduced_bin_is_the_reduced_cube(
        client: AsyncClient, opened_cube: dict, synthetic_fits_cube, mode, encoding) -> None:
    resp = await client.post("/v1/cube/reduced_bin",
                             json={"dataset_id": opened_cube["dataset_id"], "factor_xy": 3,
                                   "factor_z": 2, "mode": mode, "encoding": encoding},
                             headers=opened_cube["headers"])
    assert resp.status_code == 200, resp.text
    hd = resp.headers
    w, h, d = int(hd["X-Visivo-Width"]), int(hd["X-Visivo-Height"]), int(hd["X-Visivo-Depth"])
    assert (w, h, d) == (22, 22, 16)
    assert hd["X-Visivo-Scalar-Type"] == encoding
    assert hd["X-Visivo-Bin-Factors"] == "3,3,2"
    assert hd["X-Visivo-Bin-Mode"] == mode
    assert hd["X-Visivo-Full-Shape"] == "64,64,32"
    assert hd["X-Visivo-Bunit"] == "Jy/beam"
    assert len(resp.content) == w * h * d * (1 if encoding == "uint8" else 2)
    lo, hi = float(hd["X-Visivo-Range-Min"]), float(hd["X-Visivo-Range-Max"])
    got = cube_reduce.dequantise(resp.content, (d, h, w), lo, hi, encoding)
    source = _source(synthetic_fits_cube)
    want = _brute(source, (3, 3, 2), mode)
    # The threshold: max -> the full cube's default threshold (the one
    # reduced_info reports); mean -> the noise rule on the reduced cube.
    from app.noise_threshold import default_threshold, stride_for
    if mode == "max":
        st = stride_for(source.size)
        expected = default_threshold(source[::st, ::st, ::st], source.size)
    else:
        red = want.astype(np.float32)
        st = stride_for(red.size)
        expected = default_threshold(red[::st, ::st, ::st], red.size)
    assert float(hd["X-Visivo-Threshold"]) == pytest.approx(expected, rel=1e-5)
    tol = (hi - lo) / 254 / 2 + 1e-6 if encoding == "uint8" else (hi - lo) * 1e-3
    fin = np.isfinite(want)
    assert np.array_equal(np.isfinite(got), fin)
    assert np.max(np.abs(got[fin] - want[fin])) <= tol
    assert math.isclose(lo, np.nanmin(want), rel_tol=1e-5) and math.isclose(hi, np.nanmax(want), rel_tol=1e-5)


@pytest.mark.asyncio
async def test_reduced_routes_refuse_bad_requests(
        client: AsyncClient, opened_cube: dict, opened_image: dict) -> None:
    r = await client.post("/v1/cube/reduced_bin",
                          json={"dataset_id": opened_cube["dataset_id"], "mode": "median"},
                          headers=opened_cube["headers"])
    assert r.status_code == 422
    r = await client.post("/v1/cube/reduced_info",
                          json={"dataset_id": opened_image["dataset_id"]},
                          headers=opened_image["headers"])
    assert r.status_code == 422, "an image is not a cube"
    r = await client.post("/v1/cube/reduced_info", json={"dataset_id": opened_cube["dataset_id"]},
                          headers={"X-Visivo-Session": opened_cube["session_id"]})
    assert r.status_code == 401


async def _open(client: AsyncClient, auth_headers: dict, path: Path) -> dict:
    r = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"], body["error"]
    return {"dataset_id": body["dataset_id"],
            "headers": {**auth_headers, "X-Visivo-Session": body["session_id"]}}


def _header(ctypes, shape_fits, **extra):
    h = fits.Header()
    for i, ct in enumerate(ctypes, start=1):    # NAXISn come from the data
        h[f"CTYPE{i}"] = ct
    h.update(extra)
    return h


@pytest.mark.asyncio
async def test_reduced_bin_blanks_an_all_nan_block(client, auth_headers, tmp_path) -> None:
    data = np.random.default_rng(2).normal(size=(8, 12, 12)).astype(np.float32)
    data[0:2, 0:3, 0:3] = np.nan                 # one whole 3 x 3 x 2 block
    data[4, 7, 7] = np.nan                       # one voxel of another block
    path = tmp_path / "nan_cube.fits"
    fits.PrimaryHDU(data).writeto(path)
    ds = await _open(client, auth_headers, path)
    r = await client.post("/v1/cube/reduced_bin",
                          json={"dataset_id": ds["dataset_id"], "factor_xy": 3, "factor_z": 2},
                          headers=ds["headers"])
    assert r.status_code == 200, r.text
    codes = np.frombuffer(r.content, dtype=np.uint8).reshape(4, 4, 4)
    assert codes[0, 0, 0] == 0, "an all-blank block is blank"
    assert codes[2, 2, 2] > 0, "a block with one blank voxel is the mean of the others"
    assert (codes > 0).sum() == codes.size - 1


@pytest.mark.asyncio
async def test_reduced_info_car_with_a_degenerate_stokes_axis(client, auth_headers, tmp_path) -> None:
    """RA/Dec CAR, a degenerate STOKES axis third and FREQ fourth: the spatial
    labels are astropy's, the spectral axis is the FREQ one, in km/s."""
    nx, ny, nz = 20, 16, 10
    h = _header(["RA---CAR", "DEC--CAR", "STOKES", "FREQ"], [nx, ny, 1, nz],
                CRVAL1=150.0, CRPIX1=10.0, CDELT1=-0.01, CUNIT1="deg",
                CRVAL2=-30.0, CRPIX2=8.0, CDELT2=0.01, CUNIT2="deg",
                CRVAL3=1.0, CRPIX3=1.0, CDELT3=1.0,
                CRVAL4=1.4e9, CRPIX4=1.0, CDELT4=1e5, CUNIT4="Hz", RESTFRQ=1.420405752e9)
    data = np.random.default_rng(4).normal(size=(nz, 1, ny, nx)).astype(np.float32)
    path = tmp_path / "car_stokes.fits"
    fits.PrimaryHDU(data, header=h).writeto(path)
    ds = await _open(client, auth_headers, path)
    r = await client.post("/v1/cube/reduced_info",
                          json={"dataset_id": ds["dataset_id"], "factor_xy": 2, "factor_z": 1},
                          headers=ds["headers"])
    assert r.status_code == 200, r.text
    ax = r.json()["axes"]
    w = WCS(fits.getheader(path))
    cx = cube_reduce.block_centres(nx, 2)
    ra = w.pixel_to_world_values(cx, np.full(cx.size, (ny - 1) / 2), np.zeros(cx.size), np.zeros(cx.size))[0]
    np.testing.assert_allclose(ax["x"]["world"], ra, atol=1e-9)
    cy = cube_reduce.block_centres(ny, 2)
    dec = w.pixel_to_world_values(np.full(cy.size, (nx - 1) / 2), cy, np.zeros(cy.size), np.zeros(cy.size))[1]
    np.testing.assert_allclose(ax["y"]["world"], dec, atol=1e-9)
    assert ax["x"]["name"] == "RA" and ax["z"]["name"] == "FREQ"
    nu = 1.4e9 + np.arange(nz) * 1e5
    np.testing.assert_allclose(ax["z"]["velocity_kms"], C_KMS * (1.420405752e9 / nu - 1.0), rtol=1e-9)


@pytest.mark.asyncio
async def test_reduced_info_transposed_cube_keeps_linear_axes(client, auth_headers, tmp_path) -> None:
    """FREQ first, RA and Dec after: the third numpy axis is not spectral, so no
    velocity is claimed; the route still answers."""
    h = _header(["FREQ", "RA---SIN", "DEC--SIN"], [12, 14, 10],
                CRVAL1=1.4e9, CRPIX1=1.0, CDELT1=1e5, CUNIT1="Hz",
                CRVAL2=150.0, CRPIX2=7.0, CDELT2=-0.01, CUNIT2="deg",
                CRVAL3=-30.0, CRPIX3=5.0, CDELT3=0.01, CUNIT3="deg")
    path = tmp_path / "transposed.fits"
    fits.PrimaryHDU(np.ones((10, 14, 12), dtype=np.float32), header=h).writeto(path)
    ds = await _open(client, auth_headers, path)
    r = await client.post("/v1/cube/reduced_info", json={"dataset_id": ds["dataset_id"]},
                          headers=ds["headers"])
    assert r.status_code == 200, r.text
    ax = r.json()["axes"]
    assert ax["z"]["velocity_kms"] is None
    assert len(ax["x"]["world"]) == 12 and len(ax["z"]["world"]) == 10


# ── The page ────────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_vr_page_is_served_without_a_token(client: AsyncClient) -> None:
    """The page holds no data and no token, so it is served like any static
    file; the token travels only on its API calls."""
    r = await client.get("/vr/")
    assert r.status_code == 200
    assert r.headers["content-type"].startswith("text/html")
    assert r.headers["cache-control"] == "no-cache"
    # The modules under this build's address (epic #166), never the placeholder.
    assert re.search(r'src="b/[0-9a-f]{12}/js/main\.js"', r.text)
    assert "__VR_BUILD__" not in r.text
    js = await client.get("/vr/js/renderer.js")
    assert js.status_code == 200
    assert "javascript" in js.headers["content-type"]
    assert (await client.get("/vr/js/missing.js")).status_code == 404


@pytest.mark.asyncio
async def test_vr_page_modules_are_all_served(client: AsyncClient) -> None:
    """Every module a page module imports is served (a missing one would stop
    the page loading in the headset with no message), at a relative address:
    the page works on whatever port or tunnel the backend is reached by."""
    import re
    root = Path(__file__).resolve().parents[1] / "app" / "static" / "vr" / "js"
    seen = set()
    for path in sorted(root.glob("*.js")):
        text = path.read_text(encoding="utf-8")
        assert "localhost:8000" not in text and "127.0.0.1:8000" not in text, path.name
        for name in re.findall(r"""from\s+['"]\./([\w.-]+\.js)['"]""", text):
            seen.add(name)
    assert {"pairing.js", "settings.js", "vr-panel.js"} <= seen
    for name in sorted(seen):
        r = await client.get(f"/vr/js/{name}")
        assert r.status_code == 200, name


def test_vr_javascript_unit_tests():
    node = shutil.which("node")
    if node is None:
        pytest.skip("node is not installed: the page's JS unit tests need it")
    tests = Path(__file__).resolve().parent / "vr"
    files = sorted(str(p) for p in tests.glob("*.test.mjs"))
    assert files
    r = subprocess.run([node, "--test", *files], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout[-4000:] + r.stderr[-2000:]
