"""The desktop's cube preview read from the cube's brick pyramid (issue #320):
block means of a level, of the strided preview's shape, without reading the
cube's file; the strided preview unchanged when no current pyramid exists; and
opening or previewing a cube never building one. FITS cubes, VBT fields and
PLUTO variables."""
from __future__ import annotations

import base64
import importlib.util
import os
import zlib
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import cube_reduce, preview_pyramid as pp
from app.brick_pyramid import BrickPyramid, build_pyramid, default_pyramid_path
from app.compute import worker_cube_preview
from app.compute._common import _b64f32_compressed
from app.fits_dataset import ScientificFitsDataset

SHAPE = (24, 32, 40)          # z, y, x: every axis divisible by 8


def _cube(shape=SHAPE, seed=0, blanks=False) -> np.ndarray:
    rng = np.random.default_rng(seed)
    a = rng.normal(0.0, 1.0, shape).astype(np.float32)
    a[8:16, 8:12, 20:28] += 5.0                     # a source, so the range is not noise
    if blanks:
        a[:, :5, :7] = np.nan                       # a blanked corner, not block-aligned
        a[3, 17, 29] = np.nan                       # an isolated blank
    return a


def _decode(answer: dict) -> np.ndarray:
    assert answer["compression"] == "qt-zlib"
    raw = base64.b64decode(answer["data_base64"])
    data = np.frombuffer(zlib.decompress(raw[4:]), dtype=np.float32)
    return data.reshape(answer["depth"], answer["height"], answer["width"])


def _fits(tmp_path: Path, data: np.ndarray, name="cube.fits") -> Path:
    path = tmp_path / name
    fits.PrimaryHDU(data).writeto(path)
    return path


def _build(path, hdu=0) -> Path:
    return build_pyramid(path, default_pyramid_path(path, hdu), hdu=hdu, brick=8)


def _strided_shape(shape, s):
    return tuple(-(-n // s) for n in shape)


@pytest.fixture(autouse=True)
def _pyramid_dir(tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))


@pytest.fixture
def no_source_read(monkeypatch):
    """Any read of a cube's values through the dataset reader fails."""
    def refuse(self, *a, **k):
        raise AssertionError("the cube's file was read")
    monkeypatch.setattr(ScientificFitsDataset, "open_memmap", refuse)


# ── The reduction ───────────────────────────────────────────────────────────

@pytest.mark.parametrize("n0, s, f", [(40, 3, 2), (41, 5, 4), (7, 8, 8), (100, 7, 4),
                                      (33, 6, 4), (16, 4, 2)])
def test_each_cell_shares_its_voxels_with_the_level_voxels_it_overlaps(n0, s, f):
    index, weight = pp.cell_weights(n0, s, f)
    n_out = -(-n0 // s)
    assert index.shape == weight.shape and index.shape[0] == n_out
    # Every cell's weights add up to its own length (the last may be partial).
    lengths = np.minimum(np.arange(n_out) * s + s, n0) - np.arange(n_out) * s
    np.testing.assert_array_equal(weight.sum(axis=1), lengths)
    # And every cube voxel is counted once, in the level voxel holding it.
    per_level = np.zeros(-(-n0 // f))
    np.add.at(per_level, index.ravel(), weight.ravel())
    np.testing.assert_array_equal(per_level, np.minimum(np.arange(len(per_level)) * f + f, n0)
                                  - np.arange(len(per_level)) * f)


def test_the_level_is_the_coarsest_whose_block_fits_the_stride():
    shapes = [(1,)] * 6
    assert [pp.choose_level(shapes, s) for s in (1, 2, 3, 4, 7, 8, 9, 31, 32, 64)] == \
        [0, 1, 1, 2, 2, 3, 3, 4, 5, 5]


@pytest.mark.parametrize("s", [2, 4, 8])
def test_a_power_of_two_stride_is_the_cubes_block_mean(tmp_path, s):
    data = _cube()
    out = _build(_fits(tmp_path, data))
    with BrickPyramid(out) as pyr:
        level = pp.choose_level([pyr.shape(k) for k in range(pyr.levels)], s)
        assert 1 << level == s
        got = pp.pyramid_preview(pyr._h5[f"level{level}/data"], SHAPE, level, s)
        # ...which is the level itself.
        np.testing.assert_array_equal(got, pyr._h5[f"level{level}/data"][...])
    want = cube_reduce.reduce_block(data, (s, s, s), "mean")
    np.testing.assert_allclose(got, want, rtol=0, atol=1e-5)


def test_blank_voxels_follow_the_pyramids_own_means(tmp_path):
    """Blank voxels are left out of each 2x2x2 mean, level after level: the
    preview at stride 4 is the mean of the means of the finite voxels."""
    data = _cube(blanks=True)
    out = _build(_fits(tmp_path, data))
    with BrickPyramid(out) as pyr:
        got = pp.pyramid_preview(pyr._h5["level2/data"], SHAPE, 2, 4)
    half = cube_reduce.reduce_block(data, (2, 2, 2), "mean")
    want = cube_reduce.reduce_block(half, (2, 2, 2), "mean")
    np.testing.assert_allclose(got, want, rtol=0, atol=1e-5)
    assert np.isnan(got[:, 0, 0]).all() and np.isfinite(got[:, 2:, 2:]).all()


def test_a_stride_past_the_coarsest_level_bins_that_level_further(tmp_path):
    """A small cube's pyramid stops early: stride 8 on 24x32x40 with 16-voxel
    bricks reads level 1 (its top) in 4x4x4 blocks: still the exact block mean."""
    data = _cube()
    out = build_pyramid(_fits(tmp_path, data), tmp_path / "p.pyr.h5", brick=32)
    with BrickPyramid(out) as pyr:
        shapes = [pyr.shape(k) for k in range(pyr.levels)]
        assert len(shapes) == 2 and pp.choose_level(shapes, 8) == 1
        got = pp.pyramid_preview(pyr._h5["level1/data"], SHAPE, 1, 8)
    np.testing.assert_allclose(got, cube_reduce.reduce_block(data, (8, 8, 8), "mean"),
                               rtol=0, atol=1e-5)


@pytest.mark.parametrize("shape", [SHAPE, (23, 31, 45), (9, 70, 66)])
@pytest.mark.parametrize("s", [3, 5, 6, 7])
def test_any_other_stride_has_the_strided_shape_and_a_smooth_value(tmp_path, shape, s):
    """A stride that is no power of two: the strided preview's shape, each
    voxel its cell's overlap-weighted mean of the level, so a linear ramp
    comes out as the cell's mean everywhere a cell is cut uniformly."""
    z, y, x = np.meshgrid(*(np.arange(n, dtype=np.float64) for n in shape), indexing="ij")
    data = (x + 3.0 * y + 7.0 * z).astype(np.float32)
    out = _build(_fits(tmp_path, data))
    with BrickPyramid(out) as pyr:
        shapes = [pyr.shape(k) for k in range(pyr.levels)]
        level = pp.choose_level(shapes, s)
        got = pp.pyramid_preview(pyr._h5[f"level{level}/data"], shape, level, s,
                                 slab_bytes=4096)            # many slabs
    assert got.shape == _strided_shape(shape, s)
    assert np.isfinite(got).all()
    want = cube_reduce.reduce_block(data, (s, s, s), "mean")
    # Full cells inside the cube: the ramp's mean, to the level's partial-voxel
    # weighting (a cell boundary inside a level voxel takes its mean).
    inner = tuple(slice(0, n // s) for n in shape)
    f = 1 << level
    assert np.max(np.abs(got[inner] - want[inner])) <= 11.0 * f / 2


@pytest.mark.parametrize("s", [3, 6, 7])
@pytest.mark.parametrize("blanks", [False, True])
def test_a_cell_is_the_mean_of_the_level_spread_over_its_voxels(tmp_path, s, blanks):
    """Independently: every cube voxel takes the value of the level voxel
    holding it, and each cell is the mean of its finite ones."""
    shape = (23, 31, 45)
    data = _cube(shape, blanks=blanks)
    out = _build(_fits(tmp_path, data))
    with BrickPyramid(out) as pyr:
        level = pp.choose_level([pyr.shape(k) for k in range(pyr.levels)], s)
        lvl = pyr._h5[f"level{level}/data"][...]
        got = pp.pyramid_preview(pyr._h5[f"level{level}/data"], shape, level, s,
                                 slab_bytes=8192)
    f = 1 << level
    spread = lvl.repeat(f, 0).repeat(f, 1).repeat(f, 2)[:shape[0], :shape[1], :shape[2]]
    want = cube_reduce.reduce_block(spread, (s, s, s), "mean")
    np.testing.assert_allclose(got, want, rtol=1e-6, atol=1e-6)
    assert np.array_equal(np.isnan(got), np.isnan(want))
    if blanks and s == 3:            # a cell wholly inside the blanked corner
        assert np.isnan(got).any()


# ── The worker ──────────────────────────────────────────────────────────────

def test_with_a_pyramid_the_preview_is_its_level_and_the_file_is_not_read(tmp_path,
                                                                          no_source_read):
    data = _cube(blanks=True)
    path = _fits(tmp_path, data)
    _build(path)
    pyramid = pp.ready_pyramid(str(path))
    assert pyramid == str(default_pyramid_path(path, 0))
    answer = worker_cube_preview(str(path), 4, pyramid)
    assert answer["source"] == "pyramid" and answer["level"] == 2
    assert answer["factors"] == [4, 4, 4] and answer["level_factors"] == [4, 4, 4]
    assert answer["voxel_offset"] == 1.5
    got = _decode(answer)
    with BrickPyramid(pyramid) as pyr:
        np.testing.assert_array_equal(got, pyr._h5["level2/data"][...])
    assert answer["range_min"] == float(np.nanmin(got))
    assert answer["default_threshold"] is not None
    # Stride 7 too: the strided shape.
    answer = worker_cube_preview(str(path), 7, pyramid)
    assert answer["source"] == "pyramid" and answer["level"] == 2
    assert (answer["depth"], answer["height"], answer["width"]) == _strided_shape(SHAPE, 7)


def _legacy(data: np.ndarray, s: int) -> dict:
    """What the preview answered before issue #320."""
    from app.noise_threshold import default_threshold, stride_for
    preview = np.asarray(data[::s, ::s, ::s], dtype=np.float32)
    st = stride_for(preview.size)
    compression, b64 = _b64f32_compressed(preview, level=1)
    return {"width": preview.shape[2], "height": preview.shape[1], "depth": preview.shape[0],
            "default_threshold": default_threshold(preview[::st, ::st, ::st], data.size),
            "compression": compression, "data_base64": b64}


@pytest.mark.parametrize("s", [1, 3, 4])
def test_without_a_pyramid_the_preview_is_unchanged(tmp_path, s):
    data = _cube(blanks=True)
    path = _fits(tmp_path, data)
    assert pp.ready_pyramid(str(path)) is None
    answer = worker_cube_preview(str(path), s, None)
    for key, value in _legacy(data, s).items():
        assert answer[key] == value, key
    assert answer["source"] == "strided" and answer["level"] is None
    assert answer["factors"] == [s] * 3 and answer["voxel_offset"] == 0.0
    # Looking for a pyramid built none.
    assert not (tmp_path / "pyr").exists()


def test_a_stale_pyramid_is_not_used(tmp_path):
    data = _cube()
    path = _fits(tmp_path, data)
    built = _build(path)
    pyramid = pp.ready_pyramid(str(path))
    assert pyramid is not None
    # The file written again (same size, a later time): another cube.
    changed = data + 1.0
    fits.PrimaryHDU(changed).writeto(path, overwrite=True)
    st = os.stat(path)
    os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns + 10 ** 9))
    assert pp.ready_pyramid(str(path)) is None
    # And a worker handed the old pyramid falls back to the strided preview.
    answer = worker_cube_preview(str(path), 4, str(built))
    assert answer["source"] == "strided"
    np.testing.assert_array_equal(_decode(answer), changed[::4, ::4, ::4])


def test_a_cube_with_a_spare_axis_and_an_image_are_not_previewed_from_a_pyramid(tmp_path,
                                                                             monkeypatch):
    data = _cube()
    four = _fits(tmp_path, data[np.newaxis], "four.fits")          # STOKES-like axis of 1
    _build(four)
    assert pp.ready_pyramid(str(four)) is not None
    image = _fits(tmp_path, data[0], "image.fits")
    _build(image)
    assert pp.ready_pyramid(str(image)) is None
    # The lookup never builds one, whatever the cube.
    from app import pyramid_builds
    monkeypatch.setattr(pyramid_builds, "ensure_pyramid",
                        lambda *a, **k: pytest.fail("a pyramid build was started"))
    assert pp.ready_pyramid(str(_fits(tmp_path, data, "new.fits"))) is None


# ── VBT fields and PLUTO variables ──────────────────────────────────────────

def _write_vbt(tmp_path: Path, fields: list[np.ndarray]) -> Path:
    nz, ny, nx = fields[0].shape
    data = tmp_path / "v.bin"
    with open(data, "wb") as fh:
        for f in fields:
            f.astype(">f4").tofile(fh)
    (tmp_path / "v.bin.head").write_text("\n".join(
        ["float", str(len(fields)), f"{nx * ny * nz} {nx} {ny} {nz} 1 1 1", "big",
         *[f"F{i}" for i in range(len(fields))]]) + "\n")
    return data


def test_a_vbt_field_is_previewed_from_its_pyramid(tmp_path, no_source_read):
    from app.volume_source import vbt_field_ref, volume_shape

    fields = [_cube(seed=1), _cube(seed=2, blanks=True)]
    ref = vbt_field_ref(_write_vbt(tmp_path, fields), 1)
    _, hdu = volume_shape(ref)
    _build(ref, hdu)
    pyramid = pp.ready_pyramid(ref)
    assert pyramid is not None
    answer = worker_cube_preview(ref, 2, pyramid)
    assert answer["source"] == "pyramid" and answer["level"] == 1
    half = cube_reduce.reduce_block(fields[1], (2, 2, 2), "mean")
    np.testing.assert_allclose(_decode(answer), half, rtol=0, atol=1e-5)


def _write_pluto():
    gen = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"
    spec = importlib.util.spec_from_file_location("generate_demo_data", gen)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.write_pluto


def test_a_pluto_variable_is_previewed_from_its_pyramid(tmp_path, no_source_read):
    ps = pytest.importorskip("app.pluto_source")
    rho = np.abs(_cube(seed=3)) + 1.0
    first = _write_pluto()(tmp_path, {"rho": rho.astype(np.float64)}, mode="multiple_files",
                           precision="flt", endian="little")
    ref = ps.pluto_ref(first, "rho")
    from app.volume_source import volume_shape
    _, hdu = volume_shape(ref)
    _build(ref, hdu)
    pyramid = pp.ready_pyramid(ref)
    assert pyramid is not None
    answer = worker_cube_preview(ref, 8, pyramid)
    assert answer["source"] == "pyramid" and answer["level"] == 3
    np.testing.assert_allclose(_decode(answer), cube_reduce.reduce_block(rho, (8, 8, 8), "mean"),
                               rtol=1e-6, atol=1e-5)


# ── The route ───────────────────────────────────────────────────────────────

async def test_the_route_answers_from_the_pyramid_only_when_it_is_ready(client, auth_headers,
                                                                        tmp_path):
    data = _cube()
    path = _fits(tmp_path, data)
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    opened = resp.json()
    assert opened["valid"], opened["error"]
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    body = {"dataset_id": opened["dataset_id"], "downsample": 4}

    resp = await client.post("/v1/cube/preview", json=body, headers=headers)
    first = resp.json()
    assert first["valid"] and first["source"] == "strided"
    np.testing.assert_array_equal(_decode(first), data[::4, ::4, ::4])
    assert not (tmp_path / "pyr").exists()          # previewing built nothing

    _build(path)
    resp = await client.post("/v1/cube/preview", json=body, headers=headers)
    answer = resp.json()
    assert answer["valid"] and answer["source"] == "pyramid" and answer["level"] == 2
    assert (answer["width"], answer["height"], answer["depth"]) == \
        (first["width"], first["height"], first["depth"])
    np.testing.assert_allclose(_decode(answer), cube_reduce.reduce_block(data, (4, 4, 4), "mean"),
                               rtol=0, atol=1e-5)

    resp = await client.post("/v1/cube/preview", json={**body, "source": "strided"},
                             headers=headers)
    assert resp.json()["source"] == "strided"
    assert resp.json()["data_base64"] == first["data_base64"]
