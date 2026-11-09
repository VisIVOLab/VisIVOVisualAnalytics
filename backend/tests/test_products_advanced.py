"""Advanced 2-D products: map math, mosaic, publication figure, image quality.

Back SKAVA visualization use cases UC 3.2 / 3.3 / 1.3 / 4.2.
"""
import numpy as np
import pytest
from astropy.io import fits

from app.products_advanced import (
    STRIPING_METRIC_MAX,
    spectral_index,
    striping_ratio,
    worker_image_quality,
    worker_map_math,
)


async def _open(client, headers, path):
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"], body["error"]
    return body["dataset_id"]


# ── UC 3.2: map math / spectral index ───────────────────────────────────────


@pytest.mark.asyncio
async def test_map_math_ratio(client, opened_image, synthetic_fits_image):
    h = opened_image["headers"]
    b = await _open(client, h, synthetic_fits_image)
    resp = await client.post(
        "/v1/products/map_math", headers=h,
        json={"dataset_id": opened_image["dataset_id"], "op": "ratio", "dataset_id_b": b},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["output_path"].endswith(".fits")
    assert body["info"]["op"] == "ratio"


@pytest.mark.asyncio
async def test_map_math_spectral_index(client, opened_image, synthetic_fits_image):
    h = opened_image["headers"]
    b = await _open(client, h, synthetic_fits_image)
    resp = await client.post(
        "/v1/products/map_math", headers=h,
        json={
            "dataset_id": opened_image["dataset_id"], "op": "spectral_index",
            "dataset_id_b": b, "freq_a": 1.4e9, "freq_b": 3.0e9,
        },
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["valid"], resp.json()["error"]


@pytest.mark.asyncio
async def test_map_math_spectral_index_requires_freqs(client, opened_image, synthetic_fits_image):
    h = opened_image["headers"]
    b = await _open(client, h, synthetic_fits_image)
    resp = await client.post(
        "/v1/products/map_math", headers=h,
        json={"dataset_id": opened_image["dataset_id"], "op": "spectral_index", "dataset_id_b": b},
    )
    # Missing frequencies → graceful failure, not a crash.
    assert resp.status_code == 200
    assert resp.json()["valid"] is False


# ── UC 3.3: mosaic ──────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_mosaic_two_tiles(client, opened_image, synthetic_fits_image):
    h = opened_image["headers"]
    b = await _open(client, h, synthetic_fits_image)
    resp = await client.post(
        "/v1/products/mosaic", headers=h,
        json={"dataset_ids": [opened_image["dataset_id"], b], "weight_mode": "noise"},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["info"]["n_inputs"] == 2
    assert 0.0 < body["info"]["coverage_fraction"] <= 1.0


@pytest.mark.asyncio
async def test_mosaic_requires_two(client, opened_image):
    h = opened_image["headers"]
    resp = await client.post(
        "/v1/products/mosaic", headers=h,
        json={"dataset_ids": [opened_image["dataset_id"]]},
    )
    assert resp.status_code == 422


# ── UC 1.3: publication figure ──────────────────────────────────────────────


@pytest.mark.asyncio
async def test_publication_figure(client, opened_image):
    h = opened_image["headers"]
    resp = await client.post(
        "/v1/products/publication_figure", headers=h,
        json={"dataset_id": opened_image["dataset_id"], "stretch": "log", "title": "M-test"},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["output_path"].endswith(".png")
    assert body["has_wcs"] is True


# ── UC 4.2: image quality ───────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_image_quality_metrics(client, opened_image):
    h = opened_image["headers"]
    resp = await client.post(
        "/v1/products/image_quality", headers=h,
        json={"dataset_id": opened_image["dataset_id"]},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"]
    m = body["metrics"]
    # The synthetic image carries injected NaNs → non-zero blanked fraction.
    assert m["blanked_fraction"] > 0.0
    for key in ("dynamic_range", "robust_std", "striping_metric", "negative_bowl", "flags"):
        assert key in m


# ── Regressions: alignment, spectral-index sign, perfect stripes ────────────


def _sky_header(crpix1=32.0, crpix2=32.0):
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"] = "RA---TAN", "DEC--TAN"
    h["CRVAL1"], h["CRVAL2"] = 150.0, 2.0
    h["CRPIX1"], h["CRPIX2"] = crpix1, crpix2
    h["CDELT1"], h["CDELT2"] = -1.0 / 3600, 1.0 / 3600
    h["CUNIT1"], h["CUNIT2"] = "deg", "deg"
    h["BUNIT"] = "Jy/beam"
    return h


def _gaussian(shape, x0, y0, sigma=2.0):
    yy, xx = np.mgrid[:shape[0], :shape[1]]
    return np.exp(-((xx - x0) ** 2 + (yy - y0) ** 2) / (2 * sigma ** 2))


def _write_map(path, data, header=None):
    fits.PrimaryHDU(np.asarray(data, dtype=np.float32), header=header).writeto(path)
    return str(path)


def test_map_math_reprojects_a_shifted_map_onto_a(tmp_path):
    # The same source at the same sky position, but B's grid is shifted by 4
    # pixels: the source sits at column 35 of B and column 31 of A. Combined
    # pixel by pixel, A − B is a dipole; on A's grid it is ~0.
    shape = (64, 64)
    a = _write_map(tmp_path / "a.fits", _gaussian(shape, 31, 30), _sky_header(32.0, 32.0))
    b = _write_map(tmp_path / "b.fits", _gaussian(shape, 35, 30), _sky_header(36.0, 32.0))
    out = tmp_path / "diff.fits"
    result = worker_map_math(str(out), path_a=a, op="subtract", path_b=b)
    assert result["reprojected"] is True
    diff = fits.getdata(out)
    # Bilinear interpolation of an integer shift is exact.
    assert np.nanmax(np.abs(diff[10:50, 10:50])) < 1e-5
    history = " ".join(str(card) for card in fits.getheader(out)["HISTORY"])
    assert "reprojected" in history


def test_map_math_reprojects_shifted_sip_cubes(tmp_path):
    # astropy refuses SIP on a 3-D header; the loader must still find the sky
    # WCS of the plane, or the two cubes would be combined unaligned.
    def header(crpix1):
        h = _sky_header(crpix1, 32.0)
        h["CTYPE1"], h["CTYPE2"] = "RA---TAN-SIP", "DEC--TAN-SIP"
        h["A_ORDER"] = h["B_ORDER"] = 2
        h["A_2_0"] = h["B_0_2"] = 1.0e-5
        h["CTYPE3"], h["CRVAL3"], h["CRPIX3"], h["CDELT3"] = "FREQ", 1.4e9, 1.0, 1.0e6
        return h

    shape = (64, 64)
    a = _write_map(tmp_path / "a.fits", _gaussian(shape, 31, 30)[np.newaxis], header(32.0))
    b = _write_map(tmp_path / "b.fits", _gaussian(shape, 35, 30)[np.newaxis], header(36.0))
    out = tmp_path / "diff.fits"
    result = worker_map_math(str(out), path_a=a, op="subtract", path_b=b)
    assert result["reprojected"] is True
    assert np.nanmax(np.abs(np.squeeze(fits.getdata(out))[10:50, 10:50])) < 0.05


def test_same_sky_grid_sees_sip_differences_between_coarse_samples():
    # A 4th-order SIP term placed to cancel at the corners, edge midpoints and
    # centre still moves interior pixels by ~0.24 px: not the same grid.
    from astropy.wcs import WCS  # noqa: PLC0415
    from app.products_advanced import _same_sky_grid  # noqa: PLC0415
    plain = _sky_header(32.5, 32.5)
    sip = plain.copy()
    sip["CTYPE1"], sip["CTYPE2"] = "RA---TAN-SIP", "DEC--TAN-SIP"
    sip["A_ORDER"] = sip["B_ORDER"] = 4
    sip["A_2_0"] = -(31.5 ** 2) * 1e-6
    sip["A_4_0"] = 1e-6
    assert _same_sky_grid(WCS(plain), WCS(plain), (64, 64))
    assert not _same_sky_grid(WCS(plain), WCS(sip), (64, 64))


def test_map_math_reprojects_maps_with_lookup_table_distortion(tmp_path):
    # CPDIS tables live in extension HDUs; two files whose tables differ by
    # 4 pixels must not be combined pixel by pixel.
    from astropy.wcs import WCS, DistortionLookupTable  # noqa: PLC0415
    paths = []
    for i, offset in enumerate([0.0, 4.0]):
        w = WCS(naxis=2)
        w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
        w.wcs.crval = [150.0, 2.0]
        w.wcs.crpix = [32.0, 32.0]
        w.wcs.cdelt = [-1.0 / 3600, 1.0 / 3600]
        w.cpdis1 = DistortionLookupTable(
            np.full((64, 64), offset, dtype=np.float32), (1.0, 1.0), (1.0, 1.0), (1.0, 1.0))
        # astropy cannot read back a file with a table on one axis only.
        w.cpdis2 = DistortionLookupTable(
            np.zeros((64, 64), dtype=np.float32), (1.0, 1.0), (1.0, 1.0), (1.0, 1.0))
        hdul = w.to_fits(relax=True)
        hdul[0].data = np.ones((64, 64), dtype=np.float32)
        path = tmp_path / f"{i}.fits"
        hdul.writeto(path)
        paths.append(str(path))
    out = tmp_path / "o.fits"
    result = worker_map_math(str(out), path_a=paths[0], op="subtract", path_b=paths[1])
    assert result["reprojected"] is True
    # The output keeps A's distortion tables, so its sky coordinates are A's.
    with fits.open(paths[0]) as src, fits.open(out) as dst:
        wa = WCS(src[0].header, fobj=src)
        wo = WCS(dst[0].header, fobj=dst)
        assert np.allclose(wa.all_pix2world([[5.0, 7.0], [40.0, 50.0]], 0),
                           wo.all_pix2world([[5.0, 7.0], [40.0, 50.0]], 0), atol=1e-10)


def test_map_math_refuses_a_sky_wcs_it_cannot_read(tmp_path):
    # A table on one axis only: astropy cannot parse it, and the map must not
    # then be combined by shape as if it had no sky coordinates.
    from astropy.wcs import WCS, DistortionLookupTable  # noqa: PLC0415
    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crval = [150.0, 2.0]
    w.wcs.crpix = [32.0, 32.0]
    w.wcs.cdelt = [-1.0 / 3600, 1.0 / 3600]
    w.cpdis1 = DistortionLookupTable(
        np.full((64, 64), 4.0, dtype=np.float32), (1.0, 1.0), (1.0, 1.0), (1.0, 1.0))
    hdul = w.to_fits(relax=True)
    hdul[0].data = np.ones((64, 64), dtype=np.float32)
    hdul.writeto(tmp_path / "bad.fits")
    good = _write_map(tmp_path / "good.fits", np.ones((64, 64)), _sky_header(32.0, 32.0))
    with pytest.raises(ValueError, match="cannot be read"):
        worker_map_math(str(tmp_path / "o.fits"), path_a=good, op="add",
                        path_b=str(tmp_path / "bad.fits"))


def test_map_math_keeps_aligned_maps_untouched(tmp_path):
    shape = (32, 32)
    rng = np.random.default_rng(1)
    a_data = rng.normal(size=shape).astype(np.float32)
    b_data = rng.normal(size=shape).astype(np.float32)
    # The same grid written as a CD matrix instead of CDELT: equivalent.
    hb = _sky_header(16.0, 16.0)
    hb["CD1_1"] = hb.pop("CDELT1")
    hb["CD2_2"] = hb.pop("CDELT2")
    hb["CD1_2"] = hb["CD2_1"] = 0.0
    a = _write_map(tmp_path / "a.fits", a_data, _sky_header(16.0, 16.0))
    b = _write_map(tmp_path / "b.fits", b_data, hb)
    out = tmp_path / "sum.fits"
    result = worker_map_math(str(out), path_a=a, op="add", path_b=b)
    assert result["reprojected"] is False
    assert np.array_equal(fits.getdata(out), a_data + b_data)


def test_map_math_refuses_one_map_with_wcs_and_one_without(tmp_path):
    shape = (16, 16)
    a = _write_map(tmp_path / "a.fits", np.ones(shape), _sky_header(8.0, 8.0))
    b = _write_map(tmp_path / "b.fits", np.ones(shape))
    with pytest.raises(ValueError, match="celestial WCS"):
        worker_map_math(str(tmp_path / "o.fits"), path_a=a, op="add", path_b=b)
    with pytest.raises(ValueError, match="celestial WCS"):
        worker_map_math(str(tmp_path / "o.fits"), path_a=b, op="add", path_b=a)


def test_map_math_without_any_wcs_combines_by_shape(tmp_path):
    a = _write_map(tmp_path / "a.fits", np.full((8, 8), 3.0))
    b = _write_map(tmp_path / "b.fits", np.full((8, 8), 1.0))
    out = tmp_path / "o.fits"
    result = worker_map_math(str(out), path_a=a, op="subtract", path_b=b)
    assert result["reprojected"] is False
    assert np.all(fits.getdata(out) == 2.0)
    c = _write_map(tmp_path / "c.fits", np.ones((4, 4)))
    with pytest.raises(ValueError, match="differ in shape"):
        worker_map_math(str(tmp_path / "o2.fits"), path_a=a, op="add", path_b=c)


@pytest.mark.asyncio
async def test_map_math_route_reports_reprojection(client, opened_image, tmp_path):
    h = opened_image["headers"]
    shape = (32, 32)
    a = await _open(client, h, _write_map(tmp_path / "a.fits", np.ones(shape),
                                          _sky_header(16.0, 16.0)))
    b = await _open(client, h, _write_map(tmp_path / "b.fits", np.ones(shape),
                                          _sky_header(18.0, 16.0)))
    resp = await client.post("/v1/products/map_math", headers=h,
                             json={"dataset_id": a, "op": "ratio", "dataset_id_b": b})
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["info"] == {"op": "ratio", "reprojected": True}


def test_spectral_index_is_nan_unless_both_fluxes_are_positive():
    a = np.array([-2.0, 2.0, -2.0, 0.0, np.nan, 4.0])
    b = np.array([-1.0, -1.0, 1.0, 1.0, 1.0, 1.0])
    alpha = spectral_index(a, b, 2.0e9, 1.0e9)
    assert np.isnan(alpha[:5]).all()
    assert alpha[5] == pytest.approx(2.0)


def test_map_math_spectral_index_of_negative_fluxes_is_nan(tmp_path):
    # −2 and −1 at 2 and 1 GHz used to give a plausible α = 1.
    a = _write_map(tmp_path / "a.fits", np.full((4, 4), -2.0))
    b = _write_map(tmp_path / "b.fits", np.full((4, 4), -1.0))
    out = tmp_path / "alpha.fits"
    worker_map_math(str(out), path_a=a, op="spectral_index", path_b=b,
                    freq_a=2.0e9, freq_b=1.0e9)
    assert np.isnan(fits.getdata(out)).all()


def test_spectral_index_refuses_equal_frequencies(tmp_path):
    a = _write_map(tmp_path / "a.fits", np.ones((4, 4)))
    with pytest.raises(ValueError, match="different"):
        worker_map_math(str(tmp_path / "o.fits"), path_a=a, op="spectral_index",
                        path_b=a, freq_a=1.4e9, freq_b=1.4e9)


@pytest.mark.parametrize("horizontal", [True, False])
def test_perfect_stripes_are_flagged(tmp_path, horizontal):
    bands = np.tile(np.arange(32.0) % 4, (32, 1))  # constant down each column
    data = bands.T if horizontal else bands
    metrics = worker_image_quality(_write_map(tmp_path / "s.fits", data))
    assert metrics["striping_metric"] == STRIPING_METRIC_MAX
    assert "striping" in metrics["flags"]


def test_flat_image_is_not_striped(tmp_path):
    metrics = worker_image_quality(_write_map(tmp_path / "f.fits", np.full((16, 16), 5.0)))
    assert metrics["striping_metric"] == 0.0
    assert "striping" not in metrics["flags"]


def test_striping_ratio_is_finite_and_capped():
    assert striping_ratio(0.0, 0.0) == 0.0
    assert striping_ratio(2.0, 0.0) == STRIPING_METRIC_MAX
    assert striping_ratio(0.0, 2.0) == STRIPING_METRIC_MAX
    assert striping_ratio(1e-30, 1.0) == STRIPING_METRIC_MAX
    assert striping_ratio(6.0, 2.0) == pytest.approx(3.0)
