"""Region measurements for continuum images (issue #503): sum and unweighted
RMS beside the standard deviation, the peak and its position, point and line
regions (value, profile, file round trip), and the noise of an image over
source-free pixels, on synthetic images whose answers are known."""
from __future__ import annotations

import math

import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS

from app import region_stats
from app import regions as R
from app.image_noise import auto_noise, region_noise
from app.line_profile import line_profile
from app.region_stats import Region, region_statistics

_TOKEN = "test-token-visivo-12345"


def _wcs_header(nx, ny, beam=False):
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"] = "RA---TAN", "DEC--TAN"
    h["CRPIX1"], h["CRPIX2"] = nx / 2.0, ny / 2.0
    h["CRVAL1"], h["CRVAL2"] = 202.0, 47.0
    h["CDELT1"], h["CDELT2"] = -1.0 / 3600.0, 1.0 / 3600.0     # 1 arcsec pixels
    h["CUNIT1"] = h["CUNIT2"] = "deg"
    h["RADESYS"] = "FK5"
    if beam:
        h["BUNIT"] = "Jy/beam"
        h["BMAJ"] = h["BMIN"] = 5.0 / 3600.0
        h["BPA"] = 0.0
    return h


def _write(tmp_path, data, name="img.fits", beam=False):
    path = tmp_path / name
    hdu = fits.PrimaryHDU(np.asarray(data, dtype=np.float32), header=_wcs_header(
        data.shape[-1], data.shape[-2], beam))
    hdu.writeto(path)
    return str(path)


# ── Sum, RMS, standard deviation, peak ───────────────────────────────────────

def test_sum_rms_and_std_are_distinct(tmp_path):
    rng = np.random.default_rng(1)
    img = rng.normal(3.0, 0.5, size=(80, 90)).astype(np.float32)
    path = _write(tmp_path, img)
    region = Region({"shape": "box", "center": [44.5, 39.5], "size": [89, 79]})
    s = region_statistics(path, region, 0)
    v = img.astype(np.float64)
    assert s["sum"] == pytest.approx(v.sum(), rel=1e-12)
    assert s["rms_unweighted"] == pytest.approx(math.sqrt((v ** 2).mean()), rel=1e-12)
    assert s["stddev"] == pytest.approx(v.std(), rel=1e-9)
    # With a mean of 3 the two differ by far more than rounding.
    assert s["rms_unweighted"] > 2.9 and s["stddev"] < 0.6


def test_rms_keeps_its_digits_when_the_mean_dwarfs_sigma(tmp_path):
    img = np.full((50, 50), 1.0e4, dtype=np.float64)
    img[::2] += 1.0
    path = tmp_path / "f64.fits"
    fits.PrimaryHDU(img).writeto(path)
    s = region_statistics(str(path), Region({"shape": "box", "center": [24.5, 24.5],
                                             "size": [49, 49]}), 0)
    assert s["rms_unweighted"] == pytest.approx(math.sqrt((img ** 2).mean()), rel=1e-14)
    assert s["stddev"] == pytest.approx(0.5, rel=1e-6)


def test_peak_pixel_is_the_first_maximum_and_skips_blanks(tmp_path):
    img = np.zeros((60, 70), dtype=np.float32)
    img[20, 33] = 5.0
    img[41, 12] = 5.0            # a tie, later in scan order
    img[10, 10] = np.nan
    path = _write(tmp_path, img)
    region = Region({"shape": "box", "center": [34.5, 29.5], "size": [69, 59]})
    s = region_statistics(path, region, 0)
    assert s["max"] == 5.0 and s["peak_pixel"] == [33, 20]


def test_peak_position_across_blocks(tmp_path, monkeypatch):
    rng = np.random.default_rng(2)
    img = rng.normal(0, 1, size=(200, 120)).astype(np.float32)
    img[137, 91] = 50.0
    path = _write(tmp_path, img)
    monkeypatch.setattr(region_stats, "_BLOCK_PIXELS", 700)      # a few rows per block
    region = Region({"shape": "circle", "center": [60.0, 100.0], "radius": 75.0})
    s = region_statistics(path, region, 0)
    assert s["peak_pixel"] == [91, 137]


def test_a_point_region_is_the_value_at_the_pixel(tmp_path):
    img = np.arange(60 * 70, dtype=np.float32).reshape(60, 70)
    path = _write(tmp_path, img)
    s = region_statistics(path, Region({"shape": "point", "center": [12.2, 30.4]}), 0)
    assert s["valid_count"] == 1 and s["mean"] == img[30, 12]
    assert s["peak_pixel"] == [12, 30] and s["sum"] == img[30, 12]


# ── Line profile ─────────────────────────────────────────────────────────────

def test_profile_of_a_ramp_is_the_ramp_in_arcsec(tmp_path):
    ny, nx = 120, 150
    yy, xx = np.mgrid[0:ny, 0:nx]
    img = (2.0 * xx + 3.0 * yy).astype(np.float32)               # linear: bilinear is exact
    path = _write(tmp_path, img)
    wcs = WCS(_wcs_header(nx, ny)).celestial
    p = line_profile(path, 0, 0, [(10.0, 20.0), (110.0, 70.0)], 1, wcs)
    length = math.hypot(100.0, 50.0)
    assert p["offset_unit"] == "arcsec"
    assert p["length_pix"] == pytest.approx(length)
    # 1-arcsec pixels: the offset is the length in pixels (a TAN field this
    # small is flat to well under 1e-3 arcsec).
    assert p["offsets"][0] == 0.0 and p["offsets"][-1] == pytest.approx(length, abs=0.01)
    assert p["n_samples"] == math.ceil(length) + 1
    expected = 2.0 * np.array(p["x"]) + 3.0 * np.array(p["y"])
    assert np.allclose(p["values"], expected, atol=1e-3)
    assert p["x"][0] == 10.0 and p["y"][-1] == pytest.approx(70.0)


def test_profile_width_averages_parallel_lines(tmp_path):
    ny, nx = 100, 100
    yy, _ = np.mgrid[0:ny, 0:nx]
    img = (yy ** 2).astype(np.float32)           # across a horizontal line, y varies
    path = _write(tmp_path, img)
    one = line_profile(path, 0, 0, [(10.0, 50.0), (80.0, 50.0)], 1)
    three = line_profile(path, 0, 0, [(10.0, 50.0), (80.0, 50.0)], 3)
    assert one["offset_unit"] == "pix" and np.allclose(one["values"], 2500.0)
    # mean of 49², 50², 51²
    assert np.allclose(three["values"], (49 ** 2 + 50 ** 2 + 51 ** 2) / 3.0)


def test_profile_blank_and_outside_samples_are_none(tmp_path):
    img = np.ones((40, 60), dtype=np.float32)
    img[20, 30] = np.nan
    path = _write(tmp_path, img)
    p = line_profile(path, 0, 0, [(-5.0, 20.0), (65.0, 20.0)], 1)
    v = p["values"]
    assert v[0] is None and v[-1] is None                         # outside the image
    assert v[35] is None                                          # x = 30 on the blank
    assert v[10] == pytest.approx(1.0) and p["n_valid"] < p["n_samples"]


def test_a_blank_beside_a_sample_on_a_pixel_does_not_blank_it(tmp_path):
    img = np.full((40, 60), 2.0, dtype=np.float32)
    img[10, 21] = np.nan                  # next to the pixel at x = 20, weight 0
    path = _write(tmp_path, img)
    p = line_profile(path, 0, 0, [(10.0, 10.0), (30.0, 10.0)], 1)
    assert p["values"][10] == pytest.approx(2.0)          # x = 20 exactly
    assert p["values"][11] is None                        # x = 21: the blank itself


def test_even_width_is_centred_on_the_line(tmp_path):
    img = np.ones((60, 80), dtype=np.float32)
    path = _write(tmp_path, img)
    p = line_profile(path, 0, 0, [(10.0, 30.0), (50.0, 30.0)], 2)
    assert np.allclose(p["y"], 30.0) and p["width_pix"] == 2


def test_a_line_far_beyond_the_image_is_refused_not_allocated(tmp_path):
    path = _write(tmp_path, np.ones((20, 30), dtype=np.float32))
    with pytest.raises(ValueError):
        line_profile(path, 0, 0, [(0.0, 0.0), (1e9, 0.0)], 1)


def test_offsets_stay_finite_for_samples_off_the_field(tmp_path):
    path = _write(tmp_path, np.ones((20, 30), dtype=np.float32))
    wcs = WCS(_wcs_header(30, 20)).celestial
    p = line_profile(path, 0, 0, [(-40.0, 5.0), (70.0, 5.0)], 1, wcs)
    assert np.all(np.isfinite(p["offsets"])) and p["offsets"][-1] == pytest.approx(110.0, rel=1e-3)


def test_the_clip_window_is_the_one_the_sample_was_kept_in(tmp_path, monkeypatch):
    monkeypatch.setattr("app.image_noise._MAX_ITERATIONS", 1)       # out of iterations
    path = _write(tmp_path, _field())
    r = auto_noise(path, 0, 0, 3.0)
    assert r["used_count"] > 0.9 * r["valid_count"] and r["rms"] > 0


def test_profile_reads_windows_not_the_plane(tmp_path, monkeypatch):
    img = np.arange(1200 * 1600, dtype=np.float32).reshape(1200, 1600)
    path = _write(tmp_path, img)
    sizes = []
    original = region_stats._cube_opener

    def spy(p, h):
        opener = original(p, h)

        def open_source():
            cube = opener()
            block = cube.block

            def measured(z0, z1, y0, y1, x0, x1, dtype=np.float64):
                sizes.append((y1 - y0) * (x1 - x0))
                return block(z0, z1, y0, y1, x0, x1, dtype=dtype)
            cube.block = measured
            return cube
        open_source.whole = opener.whole
        return open_source

    monkeypatch.setattr("app.line_profile._cube_opener", spy)
    p = line_profile(path, 0, 0, [(0.0, 0.0), (1599.0, 1199.0)], 1)
    assert p["n_valid"] == p["n_samples"]
    assert max(sizes) < 1200 * 1600 / 20


def test_profile_of_a_cube_channel(tmp_path):
    cube = np.zeros((4, 30, 40), dtype=np.float32)
    cube[2] = 7.0
    path = tmp_path / "cube.fits"
    fits.PrimaryHDU(cube).writeto(path)
    p = line_profile(str(path), 0, 2, [(2.0, 5.0), (30.0, 25.0)], 1)
    assert np.allclose(p["values"], 7.0)
    with pytest.raises(ValueError):
        line_profile(str(path), 0, 9, [(2.0, 5.0), (30.0, 25.0)], 1)


# ── Noise ────────────────────────────────────────────────────────────────────

def _field(sigma=2.0, ny=400, nx=500, seed=5):
    rng = np.random.default_rng(seed)
    img = rng.normal(0.0, sigma, size=(ny, nx)).astype(np.float32)
    yy, xx = np.mgrid[0:ny, 0:nx]
    img += (400.0 * np.exp(-((xx - 250) ** 2 + (yy - 200) ** 2) / (2 * 12.0 ** 2))).astype(np.float32)
    return img


def test_auto_noise_ignores_the_source(tmp_path):
    img = _field()
    path = _write(tmp_path, img)
    r = auto_noise(path, 0, 0, 3.0)
    # 3σ clipping of a Gaussian keeps 99.73 % and shrinks σ by 1.3 %.
    assert r["exact"] and r["rms"] == pytest.approx(2.0, rel=0.03)
    assert r["mad_sigma"] == pytest.approx(2.0, rel=0.03)
    assert r["used_count"] < r["valid_count"]
    # The plain standard deviation is dominated by the source.
    assert img.std() > 2 * r["rms"]


def test_auto_noise_sampled_plane_matches_the_exact_one(tmp_path, monkeypatch):
    img = _field()
    path = _write(tmp_path, img)
    exact = auto_noise(path, 0, 0, 3.0)
    monkeypatch.setattr("app.image_noise._SAMPLE_VALUES", 20000)
    monkeypatch.setattr(region_stats, "_BLOCK_PIXELS", 30000)
    sampled = auto_noise(path, 0, 0, 3.0)
    assert not sampled["exact"] and sampled["sample_count"] < 30000
    assert sampled["rms"] == pytest.approx(exact["rms"], rel=0.02)
    assert sampled["mad_sigma"] == pytest.approx(exact["mad_sigma"], rel=0.05)
    assert sampled["valid_count"] == exact["valid_count"]


def test_region_noise_averages_the_regions_and_lists_each(tmp_path):
    img = _field()
    path = _write(tmp_path, img)
    quiet = {"shape": "box", "center": [60.0, 60.0], "size": [80.0, 80.0], "name": "a"}
    quiet2 = {"shape": "box", "center": [420.0, 330.0], "size": [80.0, 80.0], "name": "b"}
    r = region_noise(path, [quiet, quiet2], 0, 0)
    assert r["used_regions"] == 2 and len(r["regions"]) == 2
    assert r["rms"] == pytest.approx(np.mean([e["rms"] for e in r["regions"]]))
    assert r["rms"] == pytest.approx(2.0, rel=0.05)
    assert r["rms_scatter"] is not None and r["rms_scatter"] < 0.2
    # A region on the source stands out in the list.
    r = region_noise(path, [quiet, {"shape": "circle", "center": [250.0, 200.0], "radius": 15.0}], 0, 0)
    assert r["regions"][1]["rms"] > 10 * r["regions"][0]["rms"]


def test_region_noise_without_valid_pixels_is_refused(tmp_path):
    img = np.full((50, 50), np.nan, dtype=np.float32)
    path = _write(tmp_path, img)
    with pytest.raises(ValueError):
        region_noise(path, [{"shape": "box", "center": [25.0, 25.0], "size": [20.0, 20.0]}], 0, 0)


# ── Routes ───────────────────────────────────────────────────────────────────

@pytest.fixture
def api(tmp_path):
    from fastapi.testclient import TestClient
    from app.main import app
    client = TestClient(app, raise_server_exceptions=True)
    auth = {"X-Visivo-Token": _TOKEN}

    def open_file(path):
        d = client.post("/v1/datasets/open", json={"path": path}, headers=auth).json()
        return d["dataset_id"], {**auth, "X-Visivo-Session": d.get("session_id", "")}

    return client, open_file


def test_stats_route_gives_sum_rms_peak_and_its_sky_position(tmp_path, api):
    client, open_file = api
    img = np.full((64, 64), 2.0, dtype=np.float32)
    img[40, 21] = 9.0
    ds, headers = open_file(_write(tmp_path, img, beam=True))
    r = client.post(f"/v1/regions/stats/{ds}", headers=headers, json={"region": {
        "shape": "box", "frame": "image", "unit": "pix", "center": [32, 32], "size": [40, 40]}})
    assert r.status_code == 200, r.text
    d = r.json()
    assert d["peak_pixel"] == [21, 40] and d["max"] == 9.0
    assert d["sum"] == pytest.approx(2.0 * d["valid_count"] + 7.0)
    assert d["rms_unweighted"] == pytest.approx(math.sqrt((img[12:53, 12:53].astype(float) ** 2).mean()))
    assert d["stddev"] == d["rms"] and d["rms_unweighted"] > d["stddev"]
    w = d["peak_world"]
    wcs = WCS(_wcs_header(64, 64)).celestial
    lon, lat = wcs.pixel_to_world_values(21, 40)
    assert w["lon"] == pytest.approx(float(lon) % 360.0) and w["lat"] == pytest.approx(float(lat))
    assert w["frame"] in ("fk5", "icrs", "FK5", "ICRS")


def test_stats_route_point_region(tmp_path, api):
    client, open_file = api
    img = np.arange(64 * 64, dtype=np.float32).reshape(64, 64)
    ds, headers = open_file(_write(tmp_path, img))
    d = client.post(f"/v1/regions/stats/{ds}", headers=headers, json={"region": {
        "shape": "point", "frame": "image", "unit": "pix", "center": [10, 20]}}).json()
    assert d["valid_count"] == 1 and d["mean"] == img[20, 10] and d["peak_pixel"] == [10, 20]


def test_stats_route_refuses_a_line(tmp_path, api):
    client, open_file = api
    ds, headers = open_file(_write(tmp_path, np.ones((64, 64))))
    r = client.post(f"/v1/regions/stats/{ds}", headers=headers, json={"region": {
        "shape": "line", "frame": "image", "unit": "pix", "points": [[1, 1], [20, 20]]}})
    assert r.status_code == 422


def test_profile_route(tmp_path, api):
    client, open_file = api
    yy, xx = np.mgrid[0:64, 0:64]
    ds, headers = open_file(_write(tmp_path, (xx + 2 * yy).astype(np.float32), beam=True))
    r = client.post(f"/v1/regions/profile/{ds}", headers=headers, json={"region": {
        "shape": "line", "frame": "image", "unit": "pix", "points": [[5, 5], [45, 35]]}})
    assert r.status_code == 200, r.text
    d = r.json()
    assert d["offset_unit"] == "arcsec" and d["bunit"] == "Jy/beam"
    assert d["offsets"][-1] == pytest.approx(50.0, abs=0.05)         # 3-4-5 triangle x 10
    assert d["values"][0] == pytest.approx(15.0) and d["values"][-1] == pytest.approx(115.0)
    bad = client.post(f"/v1/regions/profile/{ds}", headers=headers, json={"region": {
        "shape": "circle", "frame": "image", "unit": "pix", "center": [5, 5], "radius": 3}})
    assert bad.status_code == 422
    wide = client.post(f"/v1/regions/profile/{ds}", headers=headers, json={"width": 999, "region": {
        "shape": "line", "frame": "image", "unit": "pix", "points": [[5, 5], [45, 35]]}})
    assert wide.status_code == 422


def test_profile_route_takes_a_line_in_world_coordinates(tmp_path, api):
    client, open_file = api
    ds, headers = open_file(_write(tmp_path, np.ones((64, 64), dtype=np.float32)))
    wcs = WCS(_wcs_header(64, 64)).celestial
    a = wcs.pixel_to_world_values(10, 10)
    b = wcs.pixel_to_world_values(50, 10)
    r = client.post(f"/v1/regions/profile/{ds}", headers=headers, json={"region": {
        "shape": "line", "frame": "fk5", "unit": "deg",
        "points": [[float(a[0]), float(a[1])], [float(b[0]), float(b[1])]]}})
    assert r.status_code == 200, r.text
    assert r.json()["offsets"][-1] == pytest.approx(40.0, abs=0.05)


def test_noise_route_auto_and_with_regions(tmp_path, api):
    client, open_file = api
    ds, headers = open_file(_write(tmp_path, _field(), beam=True))
    auto = client.post(f"/v1/regions/noise/{ds}", headers=headers, json={}).json()
    assert auto["mode"] == "auto" and auto["rms"] == pytest.approx(2.0, rel=0.03)
    assert auto["bunit"] == "Jy/beam"
    # A 5 arcsec round beam on 1 arcsec pixels: pi * 5^2 / (4 ln 2) pixels.
    omega = math.pi * 25.0 / (4.0 * math.log(2.0))
    assert auto["beam_area_pix"] == pytest.approx(omega, rel=1e-3)
    assert auto["rms_per_pixel"] == pytest.approx(auto["rms"] / omega, rel=1e-3)
    regs = [{"shape": "box", "frame": "image", "unit": "pix", "center": [60, 60], "size": [80, 80],
             "name": "north"},
            {"shape": "box", "frame": "image", "unit": "pix", "center": [420, 330], "size": [80, 80],
             "name": "south"}]
    d = client.post(f"/v1/regions/noise/{ds}", headers=headers, json={"regions": regs}).json()
    assert d["mode"] == "regions" and [e["name"] for e in d["regions"]] == ["north", "south"]
    assert d["rms"] == pytest.approx(2.0, rel=0.05)


def test_noise_route_without_a_beam_has_no_conversion(tmp_path, api):
    client, open_file = api
    ds, headers = open_file(_write(tmp_path, _field()))
    d = client.post(f"/v1/regions/noise/{ds}", headers=headers, json={}).json()
    assert d["beam_area_pix"] is None and d["rms_per_pixel"] is None


def test_noise_route_refuses_a_malformed_region(tmp_path, api):
    client, open_file = api
    ds, headers = open_file(_write(tmp_path, _field()))
    r = client.post(f"/v1/regions/noise/{ds}", headers=headers, json={"regions": [
        {"shape": "box", "frame": "image", "unit": "pix", "size": [10, 10]}]})
    assert r.status_code == 422


def test_noise_route_refuses_a_bad_clip(tmp_path, api):
    client, open_file = api
    ds, headers = open_file(_write(tmp_path, _field()))
    r = client.post(f"/v1/regions/noise/{ds}", headers=headers, json={"clip_sigma": 0})
    assert r.status_code == 422


# ── Region files: point and line round trip ──────────────────────────────────

def test_point_and_line_round_trip_ds9_and_crtf():
    regions = [
        {"shape": "point", "frame": "image", "unit": "pix", "center": [100.5, 200.25],
         "props": {"text": "p1"}},
        {"shape": "line", "frame": "image", "unit": "pix",
         "points": [[10.0, 20.0], [110.0, 220.0]], "props": {"text": "l1"}},
    ]
    for fmt in ("ds9", "crtf"):
        text = R.export_regions(regions, fmt)
        back = R.parse_regions(text, fmt)
        assert [r["shape"] for r in back] == ["point", "line"], fmt
        assert back[0]["center"] == pytest.approx([100.5, 200.25]), fmt
        assert np.allclose(back[1]["points"], [[10.0, 20.0], [110.0, 220.0]]), fmt
        assert back[0]["props"]["text"] == "p1" and back[1]["props"]["text"] == "l1", fmt
    assert "point(" in R.export_regions(regions, "ds9") and "line(" in R.export_regions(regions, "ds9")
    crtf = R.export_regions(regions, "crtf")
    assert "symbol[" in crtf and "line[" in crtf
