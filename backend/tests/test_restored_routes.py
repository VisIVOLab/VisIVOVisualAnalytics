"""HTTP smoke tests for routes restored after the router refactor.

The compute workers for these endpoints already have dedicated unit tests; this
file verifies the *route wiring* end-to-end — that each path the desktop client
calls is registered, authenticated, and returns the expected envelope/binary
frame. One round-trip per endpoint is enough to catch a missing/mis-registered
route (the regression this whole effort was about).

Covered:
  POST /v1/cosmology/angular_power/{id}
  POST /v1/analysis/skeleton/{id}  +  /binary
  POST /v1/photometry/aperture/{id}
  POST /v1/photometry/gaussfit/{id}
  POST /v1/astrometry/reproject/{id}
  POST /v1/polarisation/pi/{id}    +  /result/binary
  POST /v1/polarisation/rmsynthesis/{id}  +  /binary
"""
from __future__ import annotations

import os
import struct
import tempfile
import unittest

import numpy as np

_TEST_TOKEN = "test-token-visivo-12345"


def _make_stokes_fits(n_stokes: int = 4, ny: int = 8, nx: int = 8, n_freq: int = 4) -> str:
    """4-D FITS (freq, stokes, dec, ra); each Stokes plane is constant (I=1…V=4)."""
    from astropy.io import fits

    data = np.zeros((n_freq, n_stokes, ny, nx), dtype=np.float32)
    for s in range(n_stokes):
        data[:, s, :, :] = float(s + 1)
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["CTYPE1"] = "RA---SIN"; h["CRPIX1"] = nx / 2; h["CRVAL1"] = 83.8
    h["CDELT1"] = -1e-4;       h["CUNIT1"] = "deg"
    h["CTYPE2"] = "DEC--SIN"; h["CRPIX2"] = ny / 2; h["CRVAL2"] = -5.4
    h["CDELT2"] = 1e-4;        h["CUNIT2"] = "deg"
    h["CTYPE3"] = "STOKES"; h["CRPIX3"] = 1.0; h["CRVAL3"] = 1.0; h["CDELT3"] = 1.0
    h["CTYPE4"] = "FREQ"; h["CRPIX4"] = 1.0; h["CRVAL4"] = 1.4e9
    h["CDELT4"] = 1.0e6; h["CUNIT4"] = "Hz"
    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
    tmp.close()
    return tmp.name


def _parse_frame(payload: bytes, offset: int = 0):
    rows, cols, _flags = struct.unpack_from(">III", payload, offset)
    start = offset + 12
    end = start + rows * cols * 4
    arr = np.frombuffer(payload[start:end], dtype="<f4").reshape(rows, cols)
    return arr, end


def _make_wcs_image_with_source(nx: int = 64, ny: int = 64) -> str:
    """Write a 2-D FITS image with a TAN WCS and a central Gaussian source."""
    from astropy.io import fits

    yy, xx = np.mgrid[0:ny, 0:nx]
    cx, cy, sigma = nx / 2.0, ny / 2.0, 3.0
    data = (np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * sigma ** 2))).astype(np.float32)
    data += 1e-3  # small positive floor so background/annulus is well-defined

    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["CTYPE1"] = "RA---TAN"; h["CRPIX1"] = cx + 1; h["CRVAL1"] = 83.8
    h["CDELT1"] = -1e-3;       h["CUNIT1"] = "deg"
    h["CTYPE2"] = "DEC--TAN"; h["CRPIX2"] = cy + 1; h["CRVAL2"] = -5.4
    h["CDELT2"] = 1e-3;        h["CUNIT2"] = "deg"
    h["BUNIT"] = "Jy/beam"
    h["BMAJ"] = 3e-3; h["BMIN"] = 3e-3; h["BPA"] = 0.0

    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
    tmp.close()
    return tmp.name


class _RoutesBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": _TEST_TOKEN}

    def _open(self, path: str) -> str:
        r = self.client.post("/v1/datasets/open", json={"path": path}, headers=self._auth)
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.session_id = d.get("session_id", "")
        return d["dataset_id"]

    def _headers(self):
        h = dict(self._auth)
        if getattr(self, "session_id", ""):
            h["X-Visivo-Session"] = self.session_id
        return h


class TestImageRoutes(_RoutesBase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.img_path = _make_wcs_image_with_source()

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.img_path)

    def setUp(self):
        self.dataset_id = self._open(self.img_path)

    def test_angular_power(self):
        r = self.client.post(f"/v1/cosmology/angular_power/{self.dataset_id}",
                             json={"channel": 0, "n_bins": 16}, headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        self.assertEqual(len(d["k_bins"]), len(d["power"]))

    def test_skeleton_json(self):
        r = self.client.post(f"/v1/analysis/skeleton/{self.dataset_id}",
                             json={"channel": 0, "mode": "skeleton"}, headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        self.assertIn("data_base64", d)

    def test_skeleton_binary(self):
        r = self.client.post(f"/v1/analysis/skeleton/{self.dataset_id}/binary",
                             json={"channel": 0, "mode": "skeleton"}, headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(r.headers["content-type"], "application/octet-stream")
        arr, _ = _parse_frame(r.content)
        self.assertEqual(arr.shape, (64, 64))

    def test_aperture_photometry(self):
        r = self.client.post(f"/v1/photometry/aperture/{self.dataset_id}",
                             json={"ra_deg": 83.8, "dec_deg": -5.4, "aperture_arcsec": 18.0,
                                   "annulus_inner_arcsec": 25.0, "annulus_outer_arcsec": 40.0},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        self.assertIn("flux_jy", d)

    def test_gaussfit(self):
        r = self.client.post(f"/v1/photometry/gaussfit/{self.dataset_id}",
                             json={"ra_deg": 83.8, "dec_deg": -5.4, "box_arcsec": 60.0},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        # The injected source has peak ~1.0; the fit must recover it within 10%.
        self.assertAlmostEqual(d["peak_jy_beam"], 1.0, delta=0.1)

    def test_reproject_registers_new_dataset(self):
        from astropy.io import fits
        hdr = dict(fits.getheader(self.img_path))
        r = self.client.post(f"/v1/astrometry/reproject/{self.dataset_id}",
                             json={"target_wcs": hdr, "target_shape": [64, 64], "order": 1},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        self.assertTrue(d["new_dataset_id"])


class TestPolarisationRoutes(_RoutesBase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.fits_path = _make_stokes_fits(n_stokes=4, ny=8, nx=8, n_freq=4)

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.fits_path)

    def setUp(self):
        self.dataset_id = self._open(self.fits_path)

    def test_pi_metadata(self):
        r = self.client.post(f"/v1/polarisation/pi/{self.dataset_id}",
                             json={"dataset_id": self.dataset_id, "channel": 0,
                                   "line_free_channels": []}, headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        self.assertIn("pi_range_max", d)

    def test_pi_binary_two_frames(self):
        r = self.client.post("/v1/polarisation/pi/result/binary",
                             json={"dataset_id": self.dataset_id, "channel": 0,
                                   "line_free_channels": []}, headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        pi, off = _parse_frame(r.content)
        pa, _ = _parse_frame(r.content, off)
        self.assertEqual(pi.shape, (8, 8))
        self.assertEqual(pa.shape, (8, 8))

    def test_rmsynthesis_metadata(self):
        r = self.client.post(f"/v1/polarisation/rmsynthesis/{self.dataset_id}",
                             json={"dataset_id": self.dataset_id,
                                   "phi_range_rad_m2": [-100.0, 100.0], "dphi_rad_m2": 10.0},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        self.assertEqual(len(d["phi_grid"]), d["n_phi"])

    def test_rmsynthesis_binary(self):
        r = self.client.post(f"/v1/polarisation/rmsynthesis/{self.dataset_id}/binary",
                             json={"dataset_id": self.dataset_id,
                                   "phi_range_rad_m2": [-100.0, 100.0], "dphi_rad_m2": 10.0},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        arr, _ = _parse_frame(r.content)
        self.assertEqual(arr.shape[1], 8)  # width
        self.assertEqual(arr.shape[0] % 8, 0)  # n_phi * height


if __name__ == "__main__":
    unittest.main()
