"""The headset's reduced cube read from the cube's brick pyramid (part of epic
#166): which level is read, that no read is larger than its bound whatever the
factors, that its voxels cover exactly the blocks the page and its selection
mapping assume, that the values are the full cube's block means (borders of
blank regions included), and the routes (202 while the pyramid is built, then
the level; the whole cube when the pyramid cannot be had). With automatic
factors a pyramid level of at most VISIVO_VR_PYRAMID_READ_BYTES is read (up
to ..._MAX_READ_BYTES to keep the spectral axis within 2x of what was asked);
given factors that would need more are refused with the nearest ones that fit."""
from __future__ import annotations

import asyncio
import json
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import cube_reduce
from app.brick_pyramid import BrickPyramid, build_pyramid, default_pyramid_path, level_shapes
from app.compute import reduced as R

HERE = Path(__file__).resolve().parent


# ── Which level ─────────────────────────────────────────────────────────────

def test_the_plan_reads_the_coarsest_level_that_keeps_the_factors():
    shapes = level_shapes((512, 4096, 4096), 64)
    # Auto for 256 per side: 16 x 16 x 2. Level 1 holds the spectral factor.
    level, extra, factors = cube_reduce.pyramid_plan(shapes, 0, 0, 256, read_voxels=10 ** 12)
    assert factors == (16, 16, 2) and level == 1 and extra == (8, 8, 1)


def test_the_plan_keeps_to_its_budget_and_refuses_given_factors_past_it():
    shapes = level_shapes((512, 4096, 4096), 64)            # 8.6 G voxels, 32 GiB
    budget = 256 * 1024 ** 2                                 # voxels
    level, extra, factors = cube_reduce.pyramid_plan(shapes, 0, 0, 256, read_voxels=budget)
    assert int(np.prod(shapes[level])) <= budget
    assert level == 2 and factors == (16, 16, 4) and extra == (4, 4, 1)
    # Given factors are kept exactly, from a level that divides them and fits...
    level, extra, factors = cube_reduce.pyramid_plan(shapes, 16, 4, 256, read_voxels=budget)
    assert factors == (16, 16, 4) and level == 2
    # ...or refused, with the nearest factors that would fit.
    with pytest.raises(ValueError, match=r"factor_xy 12, factor_z 4"):
        cube_reduce.pyramid_plan(shapes, 12, 2, 256, read_voxels=budget)
    with pytest.raises(ValueError, match=r"factor_xy 4, factor_z 4"):
        cube_reduce.pyramid_plan(shapes, 3, 1, 256, read_voxels=budget)
    # The review's case: 33 on 2048 x 16384^2 would have read level 0, 2 TiB.
    big = level_shapes((2048, 16384, 16384), 64)
    with pytest.raises(ValueError, match=r"level 0 .* 2048\.0 GiB.*factor_xy 32"):
        cube_reduce.pyramid_plan(big, 33, 0, 256, read_voxels=budget)
    with pytest.raises(ValueError):
        cube_reduce.pyramid_plan(big, 4096, 3, 256, read_voxels=budget)


def test_a_wide_shallow_cube_keeps_its_spectral_axis_within_twice_the_factor():
    shapes = level_shapes((300, 20000, 20000), 64)          # 4.5e11 bytes
    budget = 256 * 1024 ** 2
    level, extra, factors = cube_reduce.pyramid_plan(shapes, 0, 0, 256, read_voxels=budget,
                                                     max_read_voxels=4 * 1024 ** 3)
    # Asked 79 x 79 x 2: level 3 would give 80 x 80 x 8 (38 channels); level 2,
    # read in bounded pieces, gives 80 x 80 x 4 (75 channels).
    assert level == 2 and factors == (80, 80, 4)
    assert factors[2] <= cube_reduce.MAX_INFLATION * 2
    # Past the larger bound the coarse level stays (and the plan says so).
    level, _, factors = cube_reduce.pyramid_plan(shapes, 0, 0, 256, read_voxels=budget,
                                                 max_read_voxels=budget)
    assert level == 3 and factors == (80, 80, 8)


def test_an_automatic_axis_beside_a_given_one_keeps_the_2x_cap():
    """factor_xy given, factor_z automatic on a wide, shallow cube: the z
    factor stays within twice the asked 2 (80 x 80 x 4 from level 2, read in
    pieces), not 80 x 80 x 8 from level 3; past the larger bound it stays."""
    shapes = level_shapes((300, 20000, 20000), 64)
    budget = 256 * 1024 ** 2
    level, _, factors = cube_reduce.pyramid_plan(shapes, 80, 0, 256, read_voxels=budget,
                                                 max_read_voxels=4 * 1024 ** 3)
    assert factors == (80, 80, 4) and level == 2
    level, _, factors = cube_reduce.pyramid_plan(shapes, 80, 0, 256, read_voxels=budget,
                                                 max_read_voxels=budget)
    assert factors == (80, 80, 8) and level == 3


def test_the_plan_of_a_one_level_pyramid_is_level_zero():
    level, extra, factors = cube_reduce.pyramid_plan([(40, 50, 60)], 0, 0, 16)
    assert level == 0 and factors == extra == (4, 4, 3)


# ── Bounded reads ───────────────────────────────────────────────────────────

class _Recorder:
    """An array-like that records the size of every read."""

    def __init__(self, a):
        self.a, self.shape, self.largest, self.reads = a, a.shape, 0, 0

    def __getitem__(self, key):
        out = self.a[key]
        self.largest = max(self.largest, out.size)
        self.reads += 1
        return out


@pytest.mark.parametrize("factors", [(1, 1, 1), (3, 3, 2), (64, 64, 9), (200, 200, 40)])
def test_no_read_is_larger_than_its_bound_whatever_the_factors(factors):
    rng = np.random.default_rng(2)
    a = rng.normal(size=(40, 150, 170)).astype(np.float32)
    a[3:9, 4:70, :] = np.nan
    bound = 4096                                          # voxels
    rec = _Recorder(a)
    got = cube_reduce.reduce_cube(rec, factors, "mean", max_block_bytes=4 * bound, min_planes=16)
    assert rec.largest <= bound
    np.testing.assert_allclose(got, cube_reduce.reduce_block(a, factors, "mean"),
                               rtol=1e-5, atol=1e-6)
    rec = _Recorder(a)
    got = cube_reduce.reduce_cube(rec, factors, "max", max_block_bytes=4 * bound)
    assert rec.largest <= bound
    np.testing.assert_array_equal(got, cube_reduce.reduce_block(a, factors, "max"))


def test_the_reviews_cases_read_bounded_pieces():
    """1024 x 1024 x 1024 on 32768^2 x 2048 read 128 GiB at once before; now a
    piece is at most the bound (and its accumulators too)."""
    bound = 256 * 1024 ** 2 // 4
    for shape, f in (((2048, 32768, 32768), (1024, 1024, 1024)),
                     ((2048, 16384, 16384), (4096, 4096, 3))):
        lz, ly, lx = cube_reduce.piece_lengths(shape, f, bound, 64)
        assert lz * ly * lx <= bound


def test_reducing_a_brick_deep_gives_the_same_cube():
    rng = np.random.default_rng(2)
    a = rng.normal(size=(37, 29, 23)).astype(np.float32)
    a[3:9, 4:7, :] = np.nan
    for f in ((3, 3, 2), (2, 2, 5), (1, 1, 1)):
        one = cube_reduce.reduce_cube(a, f, "mean")
        deep = cube_reduce.reduce_cube(a, f, "mean", max_block_bytes=4 * 23 * 8, min_planes=16)
        np.testing.assert_allclose(one, deep, rtol=1e-6, atol=1e-7)


# ── Geometry and values from a real pyramid ─────────────────────────────────

def _pyramid_of(tmp_path, data, brick=8):
    path = tmp_path / "cube.fits"
    fits.PrimaryHDU(data).writeto(path, overwrite=True)
    out = tmp_path / "cube.pyr.h5"
    build_pyramid(path, out, brick=brick)
    return path, out


def _from_pyramid(path, out, factor_xy, factor_z, max_dim=16):
    p = R._Pyramid(str(out), str(path), fits.getdata(path).shape, factor_xy, factor_z, max_dim)
    try:
        return p.level, p.factors, p.reduce()
    finally:
        p.close()


@pytest.mark.parametrize("shape, f", [((21, 37, 45), (4, 4, 2)), ((19, 33, 50), (8, 8, 4)),
                                      ((16, 32, 32), (4, 4, 4))])
def test_each_reduced_voxel_holds_exactly_its_block(tmp_path, shape, f):
    """A cube constant on every block of the factors: each reduced voxel from
    the pyramid is its block's value, partial last blocks included, so a
    reduced voxel covers the block the page maps it to and nothing else."""
    fx, fy, fz = f
    d, h, w = shape
    rd, rh, rw = cube_reduce.reduced_shape(shape, f)
    z, y, x = np.mgrid[0:d, 0:h, 0:w]
    ids = ((z // fz) * rh + (y // fy)) * rw + (x // fx)
    path, out = _pyramid_of(tmp_path, ids.astype(np.float32))
    level, factors, red = _from_pyramid(path, out, fx, fz)
    assert factors == f and level >= 1, "read from a coarser level, not level 0"
    want = np.arange(rd * rh * rw, dtype=np.float32).reshape(rd, rh, rw)
    np.testing.assert_array_equal(red, want)


def test_one_voxel_lights_only_its_block(tmp_path):
    shape, f = (23, 35, 41), (4, 4, 2)
    rng = np.random.default_rng(4)
    for _ in range(5):
        data = np.full(shape, np.nan, dtype=np.float32)
        v = tuple(int(rng.integers(0, n)) for n in shape)
        data[v] = 7.0
        path, out = _pyramid_of(tmp_path, data)
        _, _, red = _from_pyramid(path, out, f[0], f[2])
        lit = np.argwhere(np.isfinite(red))
        assert lit.tolist() == [[v[0] // f[2], v[1] // f[1], v[2] // f[0]]]
        assert red[tuple(lit[0])] == 7.0


def test_whole_finite_blocks_are_the_cubes_means(tmp_path):
    rng = np.random.default_rng(6)
    data = rng.normal(size=(32, 48, 64)).astype(np.float32)
    path, out = _pyramid_of(tmp_path, data)
    for f in ((4, 4, 2), (8, 8, 8), (6, 6, 2)):
        _, factors, red = _from_pyramid(path, out, f[0], f[2])
        want = cube_reduce.reduce_cube(data, factors, "mean")
        np.testing.assert_allclose(red, want, rtol=2e-5, atol=2e-6)


@pytest.mark.parametrize("shape", [(128, 128, 128), (120, 125, 131)])
def test_a_blank_cut_gives_the_cubes_block_means_not_single_pixel_noise(tmp_path, shape):
    """Noise cut by a circle of blanks: a level voxel on the cut may hold one
    finite pixel, whose noise the level's mean of means passes on at full
    weight. Repaired from level 0, the reduced cube is the whole cube's own
    block means; without the repair it is visibly noisier."""
    rng = np.random.default_rng(3)
    data = rng.normal(size=shape).astype(np.float32)
    _, y, x = np.mgrid[0:shape[0], 0:shape[1], 0:shape[2]]
    data[(y - 61.3) ** 2 + (x - 57.7) ** 2 > 50.0 ** 2] = np.nan
    path, out = _pyramid_of(tmp_path, data, brick=16)
    with BrickPyramid(out) as pyr:
        for level, extra in ((3, (1, 1, 1)), (3, (2, 2, 1)), (2, (3, 3, 2))):
            factors = tuple(e << level for e in extra)
            want = cube_reduce.reduce_cube(data, factors, "mean")
            args = (pyr._h5[f"level{level}/data"], pyr._h5["level0/data"], level, shape, extra, 16)
            got, stats = cube_reduce.pyramid_reduce(*args)
            assert stats["border"] > 0 and stats["repaired"] == stats["border"]
            np.testing.assert_allclose(got, want, rtol=1e-5, atol=1e-6)
            raw, stats = cube_reduce.pyramid_reduce(*args, repair_voxels=0)
            assert stats["repaired"] == 0
            sigma = np.nanstd(want)
            assert np.nanmax(np.abs(raw - want)) > 3 * sigma
            assert np.nanstd(raw) > 1.05 * sigma
            # All or none: a budget one voxel short of what the border needs
            # repairs nothing (and reads nothing of level 0).
            need = cube_reduce.pyramid_reduce(*args)[1]["repair_read_voxels"]
            short, stats = cube_reduce.pyramid_reduce(*args, repair_voxels=need - 1)
            assert stats["repaired"] == 0 and stats["repair_read_voxels"] == 0
            np.testing.assert_allclose(short, raw, rtol=1e-6, atol=1e-7)
            _, stats = cube_reduce.pyramid_reduce(*args, repair_voxels=need)
            assert stats["repaired"] == stats["border"]


def test_the_page_maps_selections_through_the_pyramids_blocks(tmp_path):
    """The page's own selection mapping (selection.js: reducedVoxelOf,
    blockOfReduced, reduceMask) on a reduced cube read from the pyramid."""
    node = shutil.which("node")
    if node is None:
        pytest.skip("node is not installed")
    shape, f = (21, 37, 45), (4, 4, 2)
    fx, fy, fz = f
    d, h, w = shape
    rd, rh, rw = cube_reduce.reduced_shape(shape, f)
    z, y, x = np.mgrid[0:d, 0:h, 0:w]
    ids = ((z // fz) * rh + (y // fy)) * rw + (x // fx)
    path, out = _pyramid_of(tmp_path, ids.astype(np.float32))
    _, factors, red = _from_pyramid(path, out, fx, fz)
    fixture = tmp_path / "fixture.json"
    fixture.write_text(json.dumps({"fullShape": [w, h, d], "factors": list(factors),
                                   "reducedShape": [rw, rh, rd],
                                   "reduced": red.ravel().tolist()}))
    script = tmp_path / "check.mjs"
    sel = (HERE.parent / "app/static/vr/js/selection.js").as_uri()
    script.write_text(f"""
import {{ readFileSync }} from 'node:fs';
import * as S from '{sel}';
const fx = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const [W, H, D] = fx.fullShape, [rw, rh, rd] = fx.reducedShape, f = fx.factors;
const id = (x, y, z) => (Math.floor(z / f[2]) * rh + Math.floor(y / f[1])) * rw + Math.floor(x / f[0]);
let bad = 0;
for (let rz = 0; rz < rd; rz++) for (let ry = 0; ry < rh; ry++) for (let rx = 0; rx < rw; rx++) {{
  const v = fx.reduced[(rz * rh + ry) * rw + rx];
  const [[x0, x1], [y0, y1], [z0, z1]] = S.blockOfReduced([rx, ry, rz], f, fx.fullShape);
  for (const [x, y, z] of [[x0, y0, z0], [x1, y1, z1], [x0, y1, z1], [x1, y0, z0]]) {{
    if (id(x, y, z) !== v) bad++;
    const r = S.reducedVoxelOf([x, y, z], f);
    if (r[0] !== rx || r[1] !== ry || r[2] !== rz) bad++;
  }}
}}
// A one-voxel mask at the far corner marks the partial last block.
const m = S.reduceMask(new Float32Array([1]), [1, 1, 1], [W - 1, H - 1, D - 1], f, fx.reducedShape);
const k = m.origin;
if (k[0] !== rw - 1 || k[1] !== rh - 1 || k[2] !== rd - 1 || m.count !== 1) bad++;
if (fx.reduced[((rd - 1) * rh + rh - 1) * rw + rw - 1] !== id(W - 1, H - 1, D - 1)) bad++;
console.log(JSON.stringify({{ bad }}));
""")
    r = subprocess.run([node, str(script), str(fixture)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert json.loads(r.stdout.strip().splitlines()[-1]) == {"bad": 0}


# ── Routes ──────────────────────────────────────────────────────────────────

@pytest.fixture
def big_cube(tmp_path, monkeypatch):
    """A cube the routes treat as large (VISIVO_VR_PYRAMID_BYTES below its
    size), with its pyramid in a directory of the test's own."""
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    monkeypatch.setenv("VISIVO_VR_PYRAMID_BYTES", "1000")
    rng = np.random.default_rng(8)
    data = rng.normal(0.0, 1.0, (40, 72, 80)).astype(np.float32)
    data[15:25, 30:40, 30:40] += 20.0
    hdu = fits.PrimaryHDU(data)
    for k, v in {"CTYPE1": "RA---SIN", "CTYPE2": "DEC--SIN", "CTYPE3": "VRAD",
                 "CRPIX1": 40.0, "CRPIX2": 36.0, "CRPIX3": 1.0, "CRVAL1": 150.0,
                 "CRVAL2": 20.0, "CRVAL3": 1000.0, "CDELT1": -0.002, "CDELT2": 0.002,
                 "CDELT3": 5000.0, "CUNIT3": "m/s", "BUNIT": "Jy/beam"}.items():
        hdu.header[k] = v
    path = tmp_path / "big.fits"
    hdu.writeto(path)
    return path, data


async def _open(client, auth_headers, path):
    r = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    assert r.status_code == 200, r.text
    body = r.json()
    return body["dataset_id"], {**auth_headers, "X-Visivo-Session": body["session_id"]}


async def _info_when_ready(client, headers, body, timeout=120.0):
    waited = False
    for _ in range(int(timeout / 0.25)):
        r = await client.post("/v1/cube/reduced_info", json=body, headers=headers)
        if r.status_code != 202:
            return r, waited
        waited = True
        assert r.json()["preparing"] is True and 0.0 <= r.json()["progress"] <= 1.0
        await asyncio.sleep(0.25)
    raise AssertionError("the pyramid was never built")


@pytest.mark.asyncio
async def test_a_large_cube_is_reduced_from_its_pyramid(client, auth_headers, big_cube):
    path, data = big_cube
    dataset, headers = await _open(client, auth_headers, path)
    body = {"dataset_id": dataset, "max_dim": 20}
    r, waited = await _info_when_ready(client, headers, body)
    assert r.status_code == 200, r.text
    assert waited, "the first request found no pyramid and started its build"
    info = r.json()
    assert info["source"] == "pyramid" and info["modes"] == ["mean"]
    # Auto for 20 per side: 4 x 4 x 2, read from level 1 binned 2 x 2 x 1.
    assert info["factors"] == [4, 4, 2]
    assert info["pyramid"]["level"] == 1 and info["pyramid"]["extra_factors"] == [2, 2, 1]
    assert info["reduced_shape"] == [20, 18, 20]
    assert len(info["axes"]["x"]["world"]) == 20 and len(info["axes"]["z"]["velocity_kms"]) == 20
    # The threshold the out-of-core render session starts from (level-0 bricks).
    from app.noise_threshold import default_threshold
    from app.render_session._ooc import level0_sample
    with BrickPyramid(default_pyramid_path(path, 0)) as pyr:
        want = default_threshold(level0_sample(pyr), data.size)
    assert info["default_threshold"] == pytest.approx(want, rel=1e-6)

    r = await client.post("/v1/cube/reduced_bin", json=body, headers=headers)
    assert r.status_code == 200, r.text
    hd = r.headers
    assert hd["X-Visivo-Source"] == "pyramid" and hd["X-Visivo-Pyramid-Level"] == "1"
    assert hd["X-Visivo-Bin-Factors"] == "4,4,2"
    w, h, d = int(hd["X-Visivo-Width"]), int(hd["X-Visivo-Height"]), int(hd["X-Visivo-Depth"])
    lo, hi = float(hd["X-Visivo-Range-Min"]), float(hd["X-Visivo-Range-Max"])
    got = cube_reduce.dequantise(r.content, (d, h, w), lo, hi, "uint8")
    # Every block whole and finite: the cube's own means, to the 8-bit step.
    want = cube_reduce.reduce_cube(data, (4, 4, 2), "mean")
    assert np.max(np.abs(got - want)) <= (hi - lo) / 254 / 2 + 1e-4
    # The same factors as info announced, without them in the request.
    assert [w, h, d] == info["reduced_shape"]


@pytest.mark.asyncio
async def test_the_source_can_be_chosen(client, auth_headers, big_cube):
    path, _ = big_cube
    dataset, headers = await _open(client, auth_headers, path)
    # The whole cube, as before: no pyramid needed.
    r = await client.post("/v1/cube/reduced_info",
                          json={"dataset_id": dataset, "max_dim": 16, "source": "cube"},
                          headers=headers)
    assert r.status_code == 200 and r.json()["source"] == "cube" and r.json()["pyramid"] is None
    assert r.json()["factors"] == [5, 5, 3]
    # Odd automatic factors from the pyramid: exact, from level 0 (cheap here).
    r, _ = await _info_when_ready(client, headers, {"dataset_id": dataset, "max_dim": 16})
    assert r.json()["source"] == "pyramid" and r.json()["factors"] == [5, 5, 3]
    assert r.json()["pyramid"]["level"] == 0
    # Max binning reads the whole cube with auto, and is refused from the pyramid.
    r = await client.post("/v1/cube/reduced_info",
                          json={"dataset_id": dataset, "max_dim": 16, "mode": "max"},
                          headers=headers)
    assert r.status_code == 200 and r.json()["source"] == "cube"
    r = await client.post("/v1/cube/reduced_bin",
                          json={"dataset_id": dataset, "max_dim": 16, "mode": "max",
                                "source": "pyramid"}, headers=headers)
    assert r.status_code == 422


@pytest.mark.asyncio
async def test_a_small_cube_stays_on_the_whole_cube_pass(client, opened_cube) -> None:
    r = await client.post("/v1/cube/reduced_info",
                          json={"dataset_id": opened_cube["dataset_id"], "factor_xy": 2},
                          headers=opened_cube["headers"])
    assert r.status_code == 200 and r.json()["source"] == "cube"


@pytest.mark.asyncio
async def test_auto_falls_back_to_the_whole_cube_when_no_pyramid_can_be_had(
        client, auth_headers, big_cube, monkeypatch):
    from app import pyramid_builds

    def unwritable(*_a, **_k):
        raise PermissionError("the pyramid directory cannot be written")
    monkeypatch.setattr(pyramid_builds, "ensure_pyramid", unwritable)
    path, _ = big_cube
    dataset, headers = await _open(client, auth_headers, path)
    r = await client.post("/v1/cube/reduced_info", json={"dataset_id": dataset, "max_dim": 16},
                          headers=headers)
    assert r.status_code == 200, r.text
    assert r.json()["source"] == "cube" and "whole cube was read" in r.json()["source_note"]
    r = await client.post("/v1/cube/reduced_bin", json={"dataset_id": dataset, "max_dim": 16},
                          headers=headers)
    assert r.status_code == 200 and r.headers["X-Visivo-Source"] == "cube"
    # Asked for the pyramid itself: the reason, and what to choose instead.
    r = await client.post("/v1/cube/reduced_info",
                          json={"dataset_id": dataset, "max_dim": 16, "source": "pyramid"},
                          headers=headers)
    assert r.status_code == 500 and "whole cube" in r.json()["detail"]
