"""POST /v1/datasets/pixel_to_sky (issue #130): the ICRS position of a clicked
pixel, which the desktop sends as SAMP coord.pointAt.sky.

The client knows a header only by CRVAL/CRPIX/CDELT, so it cannot place a
pixel of a SIP-distorted, rotated or exotic-projection image; the backend
uses the file's full WCS. Each case is checked against astropy directly.
"""
from __future__ import annotations

import os
import warnings

import numpy as np
import pytest
from astropy.coordinates import SkyCoord
from astropy.io import fits
from astropy.wcs import WCS

os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")

AUTH = {"X-Visivo-Token": "test-token-visivo-12345"}


@pytest.fixture()
def client():
    from fastapi.testclient import TestClient
    from app.main import app
    return TestClient(app, raise_server_exceptions=True)


def _write(tmp_path, name, header_cards, shape=(64, 64)):
    header = fits.Header()
    for key, value in header_cards.items():
        header[key] = value
    path = tmp_path / name
    fits.PrimaryHDU(np.zeros(shape, dtype=np.float32), header=header).writeto(path)
    return path


def _open(client, path):
    r = client.post("/v1/datasets/open", json={"path": str(path)}, headers=AUTH)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"], body
    return body["dataset_id"], r.headers.get("X-Visivo-Session")


def _ask(client, dataset_id, session, x, y, z=0.0):
    headers = dict(AUTH)
    if session:
        headers["X-Visivo-Session"] = session
    r = client.post("/v1/datasets/pixel_to_sky",
                    json={"dataset_id": dataset_id, "x": x, "y": y, "z": z}, headers=headers)
    assert r.status_code == 200, r.text
    return r.json()


def _expected(path, x, y):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        w = WCS(fits.getheader(path), relax=True, fix=True).celestial
        return w.pixel_to_world(x, y).icrs


TAN = {"CTYPE1": "RA---TAN", "CTYPE2": "DEC--TAN", "CRVAL1": 150.0, "CRVAL2": 60.0,
       "CRPIX1": 32.5, "CRPIX2": 32.5, "CDELT1": -0.01, "CDELT2": 0.01, "RADESYS": "ICRS"}

SIP = {**TAN, "CTYPE1": "RA---TAN-SIP", "CTYPE2": "DEC--TAN-SIP",
       "A_ORDER": 2, "B_ORDER": 2, "A_2_0": 2e-4, "A_0_2": -1e-4, "B_1_1": 3e-4,
       "B_2_0": 1e-4}

ROTATED_CD = {"CTYPE1": "RA---TAN", "CTYPE2": "DEC--TAN", "CRVAL1": 210.0, "CRVAL2": -20.0,
              "CRPIX1": 32.5, "CRPIX2": 32.5, "RADESYS": "ICRS",
              # 1"/px rotated by 30°.
              "CD1_1": -np.cos(np.radians(30)) / 3600, "CD1_2": np.sin(np.radians(30)) / 3600,
              "CD2_1": np.sin(np.radians(30)) / 3600, "CD2_2": np.cos(np.radians(30)) / 3600}

GAL_CAR = {"CTYPE1": "GLON-CAR", "CTYPE2": "GLAT-CAR", "CRVAL1": 30.0, "CRVAL2": 10.0,
           "CRPIX1": 32.5, "CRPIX2": 32.5, "CDELT1": -0.02, "CDELT2": 0.02}

EQ_CAR = {"CTYPE1": "RA---CAR", "CTYPE2": "DEC--CAR", "CRVAL1": 150.0, "CRVAL2": 40.0,
          "CRPIX1": 32.5, "CRPIX2": 32.5, "CDELT1": -0.02, "CDELT2": 0.02, "RADESYS": "ICRS"}


@pytest.mark.parametrize("name,cards", [
    ("tan", TAN), ("tan_sip", SIP), ("rotated_cd", ROTATED_CD),
    ("glon_car_b10", GAL_CAR), ("ra_car_dec40", EQ_CAR),
])
def test_pixel_matches_astropy(client, tmp_path, name, cards):
    path = _write(tmp_path, f"{name}.fits", cards)
    dataset_id, session = _open(client, path)
    for x, y in ((0.0, 0.0), (63.0, 10.0), (31.5, 31.5), (5.0, 60.0)):
        body = _ask(client, dataset_id, session, x, y)
        assert body["valid"], body
        truth = _expected(path, x, y)
        got = SkyCoord(body["ra_deg"], body["dec_deg"], unit="deg", frame="icrs")
        assert got.separation(truth).arcsec < 1e-3, (name, x, y, body)


def test_galactic_frame_is_reported(client, tmp_path):
    path = _write(tmp_path, "gal.fits", GAL_CAR)
    dataset_id, session = _open(client, path)
    body = _ask(client, dataset_id, session, 31.5, 31.5)
    assert body["frame"] == "galactic"
    assert body["lon_deg"] == pytest.approx(30.0, abs=1e-9)
    assert body["lat_deg"] == pytest.approx(10.0, abs=1e-9)


def test_cube_with_a_degenerate_axis(client, tmp_path):
    # NAXIS = 4 (x, y, freq, a one-plane Stokes): the viewer's z is the channel.
    cards = {**TAN, "CTYPE3": "FREQ", "CRVAL3": 1.4e9, "CRPIX3": 1.0, "CDELT3": 1e5,
             "CUNIT3": "Hz", "CTYPE4": "STOKES", "CRVAL4": 1.0, "CRPIX4": 1.0, "CDELT4": 1.0}
    path = _write(tmp_path, "cube.fits", cards, shape=(1, 8, 64, 64))
    dataset_id, session = _open(client, path)
    body = _ask(client, dataset_id, session, 10.0, 50.0, 3.0)
    assert body["valid"], body
    truth = _expected(path, 10.0, 50.0)
    got = SkyCoord(body["ra_deg"], body["dec_deg"], unit="deg", frame="icrs")
    assert got.separation(truth).arcsec < 1e-3


def test_no_sky_axes(client, tmp_path):
    cards = {"CTYPE1": "OFFSET", "CTYPE2": "VRAD", "CRVAL1": 0.0, "CRVAL2": 0.0,
             "CRPIX1": 1.0, "CRPIX2": 1.0, "CDELT1": 1.0, "CDELT2": 1.0}
    path = _write(tmp_path, "pv.fits", cards)
    dataset_id, session = _open(client, path)
    body = _ask(client, dataset_id, session, 1.0, 1.0)
    assert body["valid"] is False
    assert body["error"]


def test_unknown_dataset_is_404(client):
    r = client.post("/v1/datasets/pixel_to_sky",
                    json={"dataset_id": "ds_nope", "x": 0, "y": 0}, headers=AUTH)
    assert r.status_code == 404
    assert r.json()["detail"] != "Not Found"   # a route's own 404, not a missing route
