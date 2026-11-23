"""The mean spectrum of a region at full resolution (issue #499)."""

from __future__ import annotations

import warnings

import numpy as np
import pytest
from astropy.io import fits

from app import region_stats
from app.compute import worker_spectral_region
from app.region_stats import Region


def _cube(nz=12, ny=40, nx=50, seed=3) -> np.ndarray:
    data = np.random.default_rng(seed).normal(1.0, 0.5, (nz, ny, nx)).astype(np.float32)
    data[:, 10:14, 20:25] = np.nan                 # a blank patch inside the region
    data[5] = np.nan                               # a fully blank channel
    return data


def _write(tmp_path, data: np.ndarray, stokes_before_freq: bool = False):
    nz, ny, nx = data.shape
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"] = "RA---SIN", "DEC--SIN"
    h["CRVAL1"], h["CRVAL2"], h["CRPIX1"], h["CRPIX2"] = 10.0, -20.0, 25.0, 20.0
    h["CDELT1"], h["CDELT2"], h["CUNIT1"], h["CUNIT2"] = -1e-4, 1e-4, "deg", "deg"
    freq = {"CTYPE": "FREQ", "CRVAL": 100e9, "CRPIX": 1.0, "CDELT": 1e6, "CUNIT": "Hz"}
    stokes = {"CTYPE": "STOKES", "CRVAL": 1.0, "CRPIX": 1.0, "CDELT": 1.0, "CUNIT": ""}
    axes = (stokes, freq) if stokes_before_freq else (freq, stokes)
    for i, axis in enumerate(axes, start=3):
        for key, value in axis.items():
            h[f"{key}{i}"] = value
    h["BUNIT"] = "Jy/beam"
    array = data[:, np.newaxis] if stokes_before_freq else data[np.newaxis]
    path = tmp_path / ("casa.fits" if stokes_before_freq else "cube.fits")
    fits.PrimaryHDU(array, h).writeto(path)
    return path


def _expected(data, region: dict) -> tuple[np.ndarray, int]:
    ny, nx = data.shape[1:]
    yy, xx = np.mgrid[:ny, :nx]
    mask = Region(region).mask(xx, yy)
    with np.errstate(invalid="ignore"), warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        return np.nanmean(data[:, mask], axis=1), int(mask.sum())


BOX = {"shape": "box", "frame": "image", "unit": "pix", "center": [22.0, 15.0],
       "size": [21.0, 13.0], "angle_deg": 30.0}
POLY = {"shape": "polygon", "frame": "image", "unit": "pix",
        "points": [[3.0, 2.0], [40.0, 6.0], [30.0, 35.0], [8.0, 25.0]]}


@pytest.mark.parametrize("region", [BOX, POLY])
def test_the_mean_of_every_pixel_of_the_region(tmp_path, region):
    data = _cube()
    out = worker_spectral_region(str(_write(tmp_path, data)), region)
    want, n = _expected(data, region)
    assert out["n_pix"] == n
    got = np.array([np.nan if v is None else v for v in out["values"]])
    np.testing.assert_allclose(got, want, rtol=1e-6, equal_nan=True)
    assert out["values"][5] is None and out["counts"][5] == 0
    assert out["spectral_type"] == "FREQ"
    assert out["spectral_axis"][1] - out["spectral_axis"][0] == pytest.approx(1e6)


def test_small_blocks_give_the_same_answer(tmp_path, monkeypatch):
    data = _cube()
    path = str(_write(tmp_path, data))
    whole = worker_spectral_region(path, POLY)
    monkeypatch.setattr(region_stats, "_BLOCK_PIXELS", 30)      # one row at a time
    monkeypatch.setattr(region_stats, "_REMAP_BYTES", 1)        # remap every block
    assert worker_spectral_region(path, POLY) == whole


def test_a_casa_cube_with_stokes_before_freq(tmp_path):
    data = _cube()
    out = worker_spectral_region(str(_write(tmp_path, data, stokes_before_freq=True)), BOX)
    want, _ = _expected(data, BOX)
    got = np.array([np.nan if v is None else v for v in out["values"]])
    np.testing.assert_allclose(got, want, rtol=1e-6, equal_nan=True)
    assert out["spectral_type"] == "FREQ" and len(out["spectral_axis"]) == data.shape[0]


def test_a_region_outside_the_cube_is_refused(tmp_path):
    outside = {**BOX, "center": [500.0, 500.0]}
    with pytest.raises(ValueError, match="No pixels"):
        worker_spectral_region(str(_write(tmp_path, _cube())), outside)


async def test_the_route(client, auth_headers, tmp_path):
    data = _cube()
    path = _write(tmp_path, data)
    opened = (await client.post("/v1/datasets/open", json={"path": str(path)},
                                headers=auth_headers)).json()
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    resp = await client.post("/v1/spectral/region",
                             json={"dataset_id": opened["dataset_id"], "region": BOX},
                             headers=headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    want, n = _expected(data, BOX)
    assert body["n_pix"] == n and body["bunit"] == "Jy/beam"
    world = {**BOX, "frame": "fk5", "unit": "deg"}
    resp = await client.post("/v1/spectral/region",
                             json={"dataset_id": opened["dataset_id"], "region": world},
                             headers=headers)
    assert resp.status_code == 422
