"""Region statistics at full resolution, from the file (issue #95)."""
from __future__ import annotations

import numpy as np
import pytest
from astropy.io import fits

from app import region_stats
from app.region_stats import Region, region_statistics


def _image(ny=300, nx=400, seed=0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    img = rng.normal(5.0, 2.0, size=(ny, nx)).astype(np.float32)
    img[40:60, 100:130] = np.nan
    return img


def _reference(values: np.ndarray) -> dict:
    v = values[np.isfinite(values)].astype(np.float64)
    med = np.median(v)
    return {"valid_count": v.size, "blank_count": values.size - v.size, "min": v.min(),
            "max": v.max(), "sum": v.sum(), "mean": v.mean(), "stddev": v.std(),
            "median": med, "mad_sigma": 1.4826 * np.median(np.abs(v - med))}


def _check(result: dict, values: np.ndarray) -> None:
    ref = _reference(values)
    assert result["total_count"] == values.size
    for key, expected in ref.items():
        assert result[key] == pytest.approx(expected, rel=1e-9, abs=1e-9), key


@pytest.fixture
def image_file(tmp_path):
    img = _image()
    path = tmp_path / "img.fits"
    fits.PrimaryHDU(img).writeto(path)
    return path, img


def _grid(img):
    y, x = np.mgrid[0:img.shape[0], 0:img.shape[1]]
    return x, y


def test_a_box(image_file):
    path, img = image_file
    region = Region({"shape": "box", "center": [170.2, 85.3], "size": [160.4, 130.6]})
    x, y = _grid(img)
    inside = (x >= 90) & (x <= 250.4) & (y >= 20) & (y <= 150.6)
    _check(region_statistics(str(path), region, 0), img[inside])


def test_a_circle_and_an_annulus(image_file):
    path, img = image_file
    x, y = _grid(img)
    d2 = (x - 120.5) ** 2 + (y - 55.25) ** 2
    circle = Region({"shape": "circle", "center": [120.5, 55.25], "radius": 37.7})
    _check(region_statistics(str(path), circle, 0), img[d2 <= 37.7 ** 2])
    ring = Region({"shape": "annulus", "center": [120.5, 55.25], "radii": [10, 37.7]})
    _check(region_statistics(str(path), ring, 0), img[(d2 >= 100) & (d2 <= 37.7 ** 2)])


def test_a_polygon():
    from matplotlib.path import Path as MplPath

    img = _image()
    vertices = [(20.5, 10.5), (300.5, 40.5), (210.5, 250.5), (60.5, 180.5)]
    x, y = _grid(img)
    inside = MplPath(vertices).contains_points(np.c_[x.ravel(), y.ravel()]).reshape(img.shape)
    region = Region({"shape": "polygon", "points": vertices})
    mask = region.mask(x, y)
    # The two may only disagree on pixel centres lying on an edge, where each
    # rule picks a side.
    differ = np.argwhere(mask != inside)
    assert len(differ) < 100
    for yy, xx in differ:
        on_edge = False
        for (x1, y1), (x2, y2) in zip(vertices, vertices[1:] + vertices[:1]):
            cross = (x2 - x1) * (yy - y1) - (y2 - y1) * (xx - x1)
            within = min(x1, x2) <= xx <= max(x1, x2) and min(y1, y2) <= yy <= max(y1, y2)
            on_edge |= abs(cross) < 1e-9 and within
        assert on_edge, (xx, yy)


def test_the_histogram_passes_give_the_same_median_and_mad(image_file, monkeypatch):
    path, img = image_file
    region = Region({"shape": "box", "center": [199.5, 149.5], "size": [399, 299]})
    exact = region_statistics(str(path), region, 0)
    monkeypatch.setattr(region_stats, "_IN_MEMORY_VALUES", 5000)
    monkeypatch.setattr(region_stats, "_BLOCK_PIXELS", 4000)
    passes = region_statistics(str(path), region, 0)
    for key in ("median", "mad_sigma", "sum", "mean", "stddev", "min", "max", "valid_count"):
        assert passes[key] == pytest.approx(exact[key], rel=1e-12, abs=1e-12), key
    _check(passes, img)


def test_integer_data_with_blank_and_scaling(tmp_path, monkeypatch):
    rng = np.random.default_rng(1)
    counts = rng.integers(-50, 50, size=(200, 300)).astype(np.int16)
    counts[5:9, 5:9] = -32768
    hdu = fits.PrimaryHDU(counts)
    hdu.header["BLANK"] = -32768
    hdu.header["BSCALE"] = 0.5
    hdu.header["BZERO"] = 10.0
    path = tmp_path / "counts.fits"
    hdu.writeto(path)
    physical = np.where(counts == -32768, np.nan, counts * 0.5 + 10.0)
    region = Region({"shape": "box", "center": [149.5, 99.5], "size": [299, 199]})
    _check(region_statistics(str(path), region, 0), physical)
    # Many equal values, more than fit in memory: the passes end on one value.
    monkeypatch.setattr(region_stats, "_IN_MEMORY_VALUES", 1000)
    _check(region_statistics(str(path), region, 0), physical)


def test_a_channel_of_a_cube(tmp_path):
    cube = np.stack([_image(seed=s) for s in range(3)])
    path = tmp_path / "cube.fits"
    fits.PrimaryHDU(cube).writeto(path)
    region = Region({"shape": "circle", "center": [200, 150], "radius": 80})
    x, y = _grid(cube[0])
    inside = (x - 200) ** 2 + (y - 150) ** 2 <= 80 ** 2
    _check(region_statistics(str(path), region, 0, channel=2), cube[2][inside])
    with pytest.raises(ValueError, match="channel"):
        region_statistics(str(path), region, 0, channel=3)


def test_a_region_outside_the_image_or_empty(image_file):
    path, img = image_file
    outside = Region({"shape": "box", "center": [1050, 1050], "size": [100, 100]})
    assert region_statistics(str(path), outside, 0)["valid_count"] == 0
    partly = Region({"shape": "box", "center": [-20, -20], "size": [60, 60]})
    _check(region_statistics(str(path), partly, 0), img[0:11, 0:11])
    blank = Region({"shape": "box", "center": [114.5, 49.5], "size": [29, 19]})
    result = region_statistics(str(path), blank, 0)
    assert (result["valid_count"], result["blank_count"]) == (0, 600)


def test_a_degenerate_circle_is_its_centre_pixel(image_file):
    path, img = image_file
    dot = Region({"shape": "circle", "center": [7.2, 3.6], "radius": 0})
    result = region_statistics(str(path), dot, 0)
    assert result["valid_count"] == 1 and result["mean"] == pytest.approx(float(img[4, 7]))


def test_an_ellipse_and_a_rotated_box(image_file):
    path, img = image_file
    x, y = _grid(img)
    ang = np.radians(30.0)
    dx, dy = x - 200.0, y - 150.0
    xr, yr = dx * np.cos(ang) + dy * np.sin(ang), -dx * np.sin(ang) + dy * np.cos(ang)
    ellipse = Region({"shape": "ellipse", "center": [200, 150], "semi_axes": [90, 40],
                      "angle_deg": 30})
    _check(region_statistics(str(path), ellipse, 0), img[(xr / 90) ** 2 + (yr / 40) ** 2 <= 1])
    box = Region({"shape": "box", "center": [200, 150], "size": [120, 50], "angle_deg": 30})
    _check(region_statistics(str(path), box, 0), img[(np.abs(xr) <= 60) & (np.abs(yr) <= 25)])


@pytest.mark.asyncio
async def test_the_route_reads_the_file(client, opened_image, monkeypatch):
    # Out of core: the route's statistics come from blocks of rows.
    monkeypatch.setattr(region_stats, "_BLOCK_PIXELS", 64)
    resp = await client.post(f"/v1/regions/stats/{opened_image['dataset_id']}",
                             json={"region": {"shape": "box", "frame": "image",
                                              "center": [4.5, 4.5], "size": [9, 9]}},
                             headers=opened_image["headers"])
    body = resp.json()
    assert body["valid"], body
    assert body["total_count"] == 100
    resp = await client.post(f"/v1/regions/stats/{opened_image['dataset_id']}",
                             json={"region": {"shape": "polygon", "frame": "image",
                                              "points": [[0, 0], [1, 1]]}},
                             headers=opened_image["headers"])
    assert resp.status_code == 422


def test_double_precision_data_keeps_its_digits(tmp_path):
    rng = np.random.default_rng(3)
    img = 1.0 + 1e-9 * rng.standard_normal((120, 150))          # float64, below float32's ulp
    path = tmp_path / "f64.fits"
    fits.PrimaryHDU(img).writeto(path)
    region = Region({"shape": "box", "center": [74.5, 59.5], "size": [149, 119]})
    _check(region_statistics(str(path), region, 0), img)
    assert region_statistics(str(path), region, 0)["mad_sigma"] == pytest.approx(1e-9, rel=0.05)


def test_a_file_compressed_as_a_whole_is_still_measured(tmp_path):
    img = _image(60, 80)
    path = tmp_path / "img.fits.gz"
    fits.PrimaryHDU(img).writeto(path)
    region = Region({"shape": "circle", "center": [40, 30], "radius": 20})
    x, y = _grid(img)
    _check(region_statistics(str(path), region, 0), img[(x - 40) ** 2 + (y - 30) ** 2 <= 400])


def test_bounds_follow_the_shape_not_its_diagonal():
    strip = Region({"shape": "box", "center": [32767.5, 4], "size": [65535, 8]})
    assert strip.bounds() == (0, 65535, 0, 8)
    tilted = Region({"shape": "box", "center": [0, 0], "size": [10, 4], "angle_deg": 90})
    assert tilted.bounds() == (-2, 2, -5, 5)
    ellipse = Region({"shape": "ellipse", "center": [0, 0], "semi_axes": [30, 5]})
    assert ellipse.bounds() == (-30, 30, -5, 5)


def test_the_scanline_polygon_is_the_pixel_test():
    rng = np.random.default_rng(4)
    points = [(float(x), float(y)) for x, y in rng.uniform(0, 200, size=(40, 2))]   # self-crossing
    region = Region({"shape": "polygon", "points": points})
    y, x = np.mgrid[0:200, 0:200]
    reference = np.zeros(x.shape, dtype=bool)
    for i, (xi, yi) in enumerate(points):
        xj, yj = points[i - 1]
        reference ^= ((yi > y) != (yj > y)) & (x < (xj - xi) * (y - yi) / (yj - yi + 1e-30) + xi)
    np.testing.assert_array_equal(region.mask(x, y), reference)


def test_reopening_the_file_between_blocks_changes_nothing(image_file, monkeypatch):
    path, img = image_file
    region = Region({"shape": "circle", "center": [200, 150], "radius": 120})
    exact = region_statistics(str(path), region, 0)
    monkeypatch.setattr(region_stats, "_BLOCK_PIXELS", 2000)
    monkeypatch.setattr(region_stats, "_REMAP_BYTES", 1)        # reopen before every block
    again = region_statistics(str(path), region, 0)
    for key, value in exact.items():
        assert again[key] == pytest.approx(value, rel=1e-12, abs=1e-12), key


def _counting_reads(monkeypatch) -> list[int]:
    """Count the passes over the file."""
    calls = [0]
    original = region_stats._value_blocks

    class Counted:
        def __init__(self, read):
            self._read = read

        def __call__(self):
            calls[0] += 1
            return self._read()

        @property
        def bound_pixels(self):                 # what the copy sizes itself on
            return self._read.bound_pixels

    monkeypatch.setattr(region_stats, "_value_blocks", lambda *args: Counted(original(*args)))
    return calls


def _small_passes(monkeypatch, spill_dir):
    monkeypatch.setattr(region_stats, "_IN_MEMORY_VALUES", 5000)
    monkeypatch.setattr(region_stats, "_BLOCK_PIXELS", 4000)
    monkeypatch.setattr(region_stats, "_SPILL_MARGIN_BYTES", 0)
    monkeypatch.setenv("VISIVO_REGION_SPILL_DIR", str(spill_dir))


def test_the_histogram_passes_read_a_local_copy_not_the_file(image_file, tmp_path, monkeypatch):
    path, img = image_file
    region = Region({"shape": "box", "center": [199.5, 149.5], "size": [399, 299]})
    exact = region_statistics(str(path), region, 0)
    spill_dir = tmp_path / "spill"
    spill_dir.mkdir()
    _small_passes(monkeypatch, spill_dir)
    reads = _counting_reads(monkeypatch)
    passes = region_statistics(str(path), region, 0)
    assert reads[0] == 1                                    # the file is read once
    for key in ("median", "mad_sigma", "min", "max", "valid_count"):
        assert passes[key] == exact[key], key
    for key in ("sum", "mean", "stddev"):                   # blocks of another size
        assert passes[key] == pytest.approx(exact[key], rel=1e-12), key
    assert list(spill_dir.iterdir()) == []                  # the copy is gone


def test_without_room_for_the_copy_the_passes_read_the_file(image_file, tmp_path, monkeypatch):
    path, img = image_file
    region = Region({"shape": "box", "center": [199.5, 149.5], "size": [399, 299]})
    _small_passes(monkeypatch, tmp_path)
    monkeypatch.setattr(region_stats.shutil, "disk_usage",
                        lambda _: type("U", (), {"free": 0})())
    reads = _counting_reads(monkeypatch)
    _check(region_statistics(str(path), region, 0), img)
    assert reads[0] > 1


def test_double_precision_data_is_copied_as_double(tmp_path, monkeypatch):
    rng = np.random.default_rng(3)
    img = 1.0 + 1e-9 * rng.standard_normal((120, 150))          # float64, below float32's ulp
    path = tmp_path / "f64.fits"
    fits.PrimaryHDU(img).writeto(path)
    region = Region({"shape": "box", "center": [74.5, 59.5], "size": [149, 119]})
    exact = region_statistics(str(path), region, 0)
    _small_passes(monkeypatch, tmp_path)
    reads = _counting_reads(monkeypatch)
    passes = region_statistics(str(path), region, 0)
    assert reads[0] == 1
    assert passes["median"] == exact["median"] and passes["mad_sigma"] == exact["mad_sigma"]


def test_a_value_float32_cannot_hold_drops_the_copy(tmp_path, monkeypatch):
    rng = np.random.default_rng(4)
    img = np.round(rng.normal(0, 100, size=(200, 150)))           # integers: exact in float32
    img[150:] += 1e-9 * rng.standard_normal((50, 150))            # the last rows are not
    path = tmp_path / "mixed.fits"
    fits.PrimaryHDU(img).writeto(path)
    region = Region({"shape": "box", "center": [74.5, 99.5], "size": [149, 199]})
    _small_passes(monkeypatch, tmp_path)
    reads = _counting_reads(monkeypatch)
    _check(region_statistics(str(path), region, 0), img)
    assert reads[0] > 1                                     # back to the file
