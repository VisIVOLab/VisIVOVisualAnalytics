"""2-D images larger than memory: an averaged tile pyramid (issue #85)."""
from __future__ import annotations

import base64
import time
import warnings
import zlib

import numpy as np
import pytest
from astropy.io import fits

from app import image_pyramid, pyramid_builds
from app.brick_pyramid import DEFAULT_IMAGE_BRICK, BrickPyramid, build_pyramid, level_shapes
from app.compute.cube import worker_image_tile


@pytest.fixture
def pyramid_dir(tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    monkeypatch.setattr(pyramid_builds, "_builds", {})
    monkeypatch.setattr(image_pyramid, "_ready", set())
    return tmp_path


def _image(ny=777, nx=1000, seed=0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    img = rng.normal(size=(ny, nx)).astype(np.float32)
    img[100:140, 200:260] = np.nan                     # a blank patch
    return img


def _halved(img: np.ndarray, times: int) -> np.ndarray:
    """The reference, written apart from the builder: ``times`` halvings, each
    the mean of 2x2 blocks over their finite pixels, odd edges padded with NaN."""
    out = img.astype(np.float64)
    for _ in range(times):
        ny, nx = out.shape
        padded = np.full((ny + ny % 2, nx + nx % 2), np.nan)
        padded[:ny, :nx] = out
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", RuntimeWarning)     # all-NaN blocks
            out = np.nanmean(padded.reshape(padded.shape[0] // 2, 2, padded.shape[1] // 2, 2),
                             axis=(1, 3))
    return out


def _pixels(result) -> np.ndarray:
    raw = base64.b64decode(result["data_base64"])
    if result["compression"] == "qt-zlib":
        raw = zlib.decompress(raw[4:])
    return np.frombuffer(raw, dtype=np.float32).reshape(result["height"], result["width"])


def test_an_image_pyramid_halves_y_and_x_only(tmp_path):
    img = _image()
    path = tmp_path / "img.fits"
    fits.PrimaryHDU(img).writeto(path)
    out = build_pyramid(path, tmp_path / "img.pyr.h5")
    with BrickPyramid(out) as pyr:
        assert pyr.brick == DEFAULT_IMAGE_BRICK
        assert [pyr.shape(k) for k in range(pyr.levels)] == level_shapes((1, 777, 1000), 256)
        assert pyr.shape(2) == (1, 195, 250)
        np.testing.assert_array_equal(pyr._h5["level0/data"][0], img)
        for k in range(1, pyr.levels):
            np.testing.assert_allclose(pyr._h5[f"level{k}/data"][0], _halved(img, k),
                                       rtol=1e-5, atol=1e-6, equal_nan=True)


@pytest.mark.parametrize("level", [0, 1, 3, 4])
def test_a_tile_from_the_pyramid_is_averaged_and_tiled_as_from_the_file(tmp_path, level):
    img = _image()
    path = tmp_path / "img.fits"
    fits.PrimaryHDU(img).writeto(path)
    pyr = str(build_pyramid(path, tmp_path / "img.pyr.h5"))
    # Tiles of 32: the pyramid (bricks of 256) stops at level 2, so levels 3
    # and 4 are halved from its last level on the fly.
    sampled = worker_image_tile(str(path), level, 1, 0, 32)
    averaged = worker_image_tile(str(path), level, 1, 0, 32, pyr)
    for key in ("width", "height", "full_width", "full_height", "num_levels"):
        assert averaged[key] == sampled[key]
    assert sampled["averaged"] is False
    assert averaged["averaged"] is (level > 0)
    expected = _halved(img, level)[0:32, 32:64]
    np.testing.assert_allclose(_pixels(averaged), expected[:averaged["height"], :averaged["width"]],
                               rtol=1e-5, atol=1e-6, equal_nan=True)


def test_an_edge_tile_and_an_out_of_range_tile(tmp_path):
    img = _image()
    path = tmp_path / "img.fits"
    fits.PrimaryHDU(img).writeto(path)
    pyr = str(build_pyramid(path, tmp_path / "img.pyr.h5"))
    edge = worker_image_tile(str(path), 1, 7, 6, 64, pyr)      # level 1 is 389 x 500
    assert (edge["width"], edge["height"]) == (500 - 7 * 64, 389 - 6 * 64)
    with pytest.raises(ValueError, match="out of range"):
        worker_image_tile(str(path), 1, 8, 0, 64, pyr)


def test_a_tile_compressed_scaled_image_gives_the_same_pyramid(tmp_path):
    img = np.round(_image() * 100).astype(np.float32)
    plain = tmp_path / "plain.fits"
    fits.PrimaryHDU(np.nan_to_num(img, nan=-32768)).writeto(plain)
    counts = np.nan_to_num(img, nan=-32768).astype(np.int16)
    packed = tmp_path / "packed.fits"
    fits.HDUList([fits.PrimaryHDU(),
                  fits.CompImageHDU(counts, compression_type="RICE_1", tile_shape=(64, 64))]
                 ).writeto(packed)
    a = build_pyramid(plain, tmp_path / "a.pyr.h5")
    b = build_pyramid(packed, tmp_path / "b.pyr.h5", hdu=1)
    with BrickPyramid(a) as pa, BrickPyramid(b) as pb:
        for k in range(pa.levels):
            np.testing.assert_array_equal(pa._h5[f"level{k}/data"][...], pb._h5[f"level{k}/data"][...])


def test_a_large_image_is_served_from_its_pyramid_once_built(pyramid_dir, monkeypatch):
    monkeypatch.setattr(image_pyramid, "image_pyramid_bytes", lambda: 1024)
    img = _image()
    path = pyramid_dir / "big.fits"
    fits.PrimaryHDU(img).writeto(path)
    first = image_pyramid.tile_source(str(path))
    assert first.state in ("building", "ready")
    deadline = time.monotonic() + 60
    source = first
    while source.state == "building" and time.monotonic() < deadline:
        time.sleep(0.2)
        source = image_pyramid.tile_source(str(path))
    assert source.state == "ready" and source.pyramid
    tile = worker_image_tile(str(path), 2, 0, 0, 64, source.pyramid)
    assert tile["averaged"] is True


def test_a_small_image_has_no_pyramid(pyramid_dir):
    path = pyramid_dir / "small.fits"
    fits.PrimaryHDU(_image(64, 64)).writeto(path)
    assert image_pyramid.tile_source(str(path)).state == "none"
    assert not (pyramid_dir / "pyr").exists()


@pytest.mark.asyncio
async def test_the_tile_route_says_where_the_tile_comes_from(client, opened_image):
    resp = await client.post("/v1/image/tile",
                             json={"dataset_id": opened_image["dataset_id"], "level": 0,
                                   "tile_x": 0, "tile_y": 0, "tile_size": 32},
                             headers=opened_image["headers"])
    body = resp.json()
    assert body["valid"], body["error"]
    assert (body["pyramid"], body["averaged"]) == ("none", False)


def test_an_image_built_by_several_processes_is_the_same(tmp_path):
    img = _image(1033, 1501)
    path = tmp_path / "img.fits"
    fits.PrimaryHDU(img[np.newaxis, np.newaxis]).writeto(path)     # 4-D, degenerate axes
    budget = 4 * 256 * 256 * 4                                      # the smallest allowed
    one = build_pyramid(path, tmp_path / "one.pyr.h5", max_block_bytes=budget)
    three = build_pyramid(path, tmp_path / "three.pyr.h5", max_block_bytes=budget, workers=3)
    with BrickPyramid(one) as a, BrickPyramid(three) as b:
        assert a.levels == b.levels
        for k in range(a.levels):
            for item in ("data", "min", "max"):
                np.testing.assert_array_equal(a._h5[f"level{k}/{item}"][...],
                                              b._h5[f"level{k}/{item}"][...])


def test_a_pyramid_that_cannot_start_leaves_the_tiles_sampled(pyramid_dir, monkeypatch):
    monkeypatch.setattr(image_pyramid, "image_pyramid_bytes", lambda: 1024)

    def broken(path, hdu):
        raise PermissionError("pyramid directory not writable")
    monkeypatch.setattr(pyramid_builds, "ensure_pyramid", broken)
    path = pyramid_dir / "big.fits"
    fits.PrimaryHDU(_image()).writeto(path)
    source = image_pyramid.tile_source(str(path))
    assert (source.state, source.pyramid) == ("failed", None)
    assert "not writable" in source.error


def test_a_failed_build_is_not_tried_again_for_the_same_image(pyramid_dir, monkeypatch):
    monkeypatch.setattr(image_pyramid, "image_pyramid_bytes", lambda: 1024)
    monkeypatch.setattr(image_pyramid, "_failed", {})
    calls = []

    def failing(path, hdu):
        calls.append(path)
        return pyramid_builds.BuildStatus("failed", 0.0, "no room left")
    monkeypatch.setattr(pyramid_builds, "ensure_pyramid", failing)
    path = pyramid_dir / "big.fits"
    fits.PrimaryHDU(_image()).writeto(path)
    for _ in range(3):
        assert image_pyramid.tile_source(str(path)).state == "failed"
    assert len(calls) == 1


@pytest.mark.asyncio
async def test_the_tile_route_serves_averaged_tiles_once_the_pyramid_is_ready(
        client, opened_image, pyramid_dir, monkeypatch):
    monkeypatch.setattr(image_pyramid, "image_pyramid_bytes", lambda: 1024)
    body = {"dataset_id": opened_image["dataset_id"], "level": 1, "tile_x": 0, "tile_y": 0,
            "tile_size": 16}
    deadline = time.monotonic() + 60
    while True:
        tile = (await client.post("/v1/image/tile", json=body,
                                  headers=opened_image["headers"])).json()
        assert tile["valid"], tile["error"]
        if tile["pyramid"] != "building" or time.monotonic() > deadline:
            break
        assert tile["averaged"] is False                # sampled while it builds
        time.sleep(0.2)
    assert (tile["pyramid"], tile["averaged"]) == ("ready", True)
