"""POST /v1/regions/convert: a region moved between pixels and sky frames.

The route behind the region properties editor of the desktop client (issue
#341): the viewer holds regions in 0-based pixels, the user reads and types
them in a sky frame. Positions are checked against astropy on the same WCS,
on a rotated SIN image so that a CDELT-only approximation would fail.
"""
from __future__ import annotations

import os
import tempfile
import unittest

import numpy as np

from tests.test_regions import _TEST_TOKEN, _wcs_header


class TestRegionConvertHttp(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        from astropy.io import fits
        from astropy.wcs import WCS
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        auth = {"X-Visivo-Token": _TEST_TOKEN}
        header = _wcs_header(crval=(202.47, 47.195), crpix=(20.5, 40.0), scale_arcsec=2.0,
                             rot_deg=25.0, ctype=("RA---SIN", "DEC--SIN"), radesys="ICRS")
        cls.wcs = WCS(header).celestial
        cls.sky = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
        cls.sky.close()
        fits.PrimaryHDU(np.ones((64, 64), np.float32), header).writeto(cls.sky.name, overwrite=True)
        cls.plain = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
        cls.plain.close()
        fits.PrimaryHDU(np.ones((16, 16), np.float32)).writeto(cls.plain.name, overwrite=True)
        cls.h = dict(auth)
        cls.ids = {}
        for key, path in (("sky", cls.sky.name), ("plain", cls.plain.name)):
            d = cls.client.post("/v1/datasets/open", json={"path": path}, headers=cls.h).json()
            cls.ids[key] = d["dataset_id"]
            if d.get("session_id"):
                cls.h["X-Visivo-Session"] = d["session_id"]

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.sky.name)
        os.unlink(cls.plain.name)

    def post(self, **body):
        return self.client.post("/v1/regions/convert", headers=self.h, json=body)

    def test_native_frame_without_regions(self):
        r = self.post(to_frame="image", dataset_id=self.ids["sky"])
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(r.json()["native_frame"], "icrs")
        self.assertEqual(r.json()["regions"], [])

    def test_no_celestial_wcs_reports_empty_native_frame(self):
        r = self.post(to_frame="image", dataset_id=self.ids["plain"])
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(r.json()["native_frame"], "")
        r = self.post(to_frame="icrs", dataset_id=self.ids["plain"], origin=0, regions=[
            {"shape": "point", "frame": "image", "center": [3, 4], "unit": "pix"}])
        self.assertEqual(r.status_code, 422, r.text)

    def test_pixels_to_icrs_matches_astropy_zero_based(self):
        reg = {"shape": "circle", "frame": "image", "center": [10.25, 31.5], "radius": 4.0, "unit": "pix"}
        r = self.post(to_frame="icrs", dataset_id=self.ids["sky"], origin=0, regions=[reg])
        self.assertEqual(r.status_code, 200, r.text)
        out = r.json()["regions"][0]
        ra, dec = self.wcs.all_pix2world([10.25], [31.5], 0)
        self.assertEqual(out["frame"], "icrs")
        self.assertAlmostEqual(out["center"][0], float(ra[0]), places=9)
        self.assertAlmostEqual(out["center"][1], float(dec[0]), places=9)
        self.assertAlmostEqual(out["radius"], 8.0, places=6)          # 2"/pix
        self.assertEqual(out["unit"], "arcsec")

    def test_icrs_to_pixels_round_trip_with_rotated_box(self):
        x, y = 30.0, 12.0
        ra, dec = self.wcs.all_pix2world([x], [y], 0)
        box = {"shape": "box", "frame": "icrs", "center": [float(ra[0]), float(dec[0])],
               "size": [20.0, 10.0], "angle_deg": 30.0, "unit": "arcsec"}
        r = self.post(to_frame="image", dataset_id=self.ids["sky"], origin=0, regions=[box])
        self.assertEqual(r.status_code, 200, r.text)
        pix = r.json()["regions"][0]
        self.assertAlmostEqual(pix["center"][0], x, places=6)
        self.assertAlmostEqual(pix["center"][1], y, places=6)
        np.testing.assert_allclose(pix["size"], [10.0, 5.0], atol=1e-6)
        # The image is rotated by 25 degrees on the grid: the angle follows it.
        self.assertNotAlmostEqual(pix["angle_deg"], 30.0, places=1)
        back = self.post(to_frame="icrs", dataset_id=self.ids["sky"], origin=0, regions=[pix]).json()
        b = back["regions"][0]
        np.testing.assert_allclose(b["center"], box["center"], atol=1e-10)
        self.assertAlmostEqual(b["angle_deg"], 30.0, places=6)

    def test_one_based_origin_shifts_by_one(self):
        reg = {"shape": "point", "frame": "image", "center": [5.0, 6.0], "unit": "pix"}
        zero = self.post(to_frame="galactic", dataset_id=self.ids["sky"], origin=0, regions=[reg]).json()
        one = self.post(to_frame="galactic", dataset_id=self.ids["sky"], origin=1,
                        regions=[dict(reg, center=[6.0, 7.0])]).json()
        np.testing.assert_allclose(zero["regions"][0]["center"], one["regions"][0]["center"], atol=1e-12)

    def test_unit_and_native(self):
        reg = {"shape": "annulus", "frame": "image", "center": [32, 32], "radii": [3, 6], "unit": "pix"}
        r = self.post(to_frame="native", dataset_id=self.ids["sky"], origin=0, unit="deg", regions=[reg])
        self.assertEqual(r.status_code, 200, r.text)
        out = r.json()["regions"][0]
        self.assertEqual(out["frame"], "icrs")
        self.assertEqual(out["unit"], "deg")
        # Sizes go through the projection's local Jacobian at the centre, not
        # the nominal CDELT: a few parts in 1e9 off the round values here.
        np.testing.assert_allclose(out["radii"], [6 / 3600.0, 12 / 3600.0], rtol=1e-7)

    def test_polygon_to_image(self):
        xs, ys = [3.0, 20.0, 11.5], [4.0, 6.0, 30.0]
        ra, dec = self.wcs.all_pix2world(xs, ys, 0)
        poly = {"shape": "polygon", "frame": "icrs", "unit": "deg",
                "points": [[float(a), float(b)] for a, b in zip(ra, dec)]}
        out = self.post(to_frame="image", dataset_id=self.ids["sky"], origin=0, regions=[poly]).json()
        np.testing.assert_allclose(out["regions"][0]["points"], list(zip(xs, ys)), atol=1e-6)

    def test_pixels_to_pixels_needs_no_dataset(self):
        reg = {"shape": "circle", "frame": "image", "center": [5.0, 6.0], "radius": 2, "unit": "pix"}
        r = self.post(to_frame="image", origin=0, regions=[reg])
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(r.json()["regions"][0]["center"], [5.0, 6.0])

    def test_malformed_region_is_422(self):
        r = self.post(to_frame="icrs", dataset_id=self.ids["sky"], origin=0,
                      regions=[{"shape": "circle", "frame": "image", "radius": 2}])
        self.assertEqual(r.status_code, 422, r.text)

    def test_unknown_region_unit_is_422(self):
        reg = {"shape": "circle", "frame": "icrs", "center": [202.47, 47.195], "radius": 2,
               "unit": "furlong"}
        r = self.post(to_frame="galactic", dataset_id=self.ids["sky"], unit="deg", regions=[reg])
        self.assertEqual(r.status_code, 422, r.text)

    def test_bad_requests(self):
        reg = {"shape": "point", "frame": "image", "center": [5.0, 6.0], "unit": "pix"}
        self.assertEqual(self.post(to_frame="image", origin=2, regions=[reg]).status_code, 422)
        self.assertEqual(self.post(to_frame="nowhere", regions=[reg]).status_code, 422)
        self.assertEqual(self.post(to_frame="icrs", regions=[reg]).status_code, 422)   # no dataset
        self.assertEqual(self.post(to_frame="image", dataset_id=self.ids["sky"], unit="deg",
                                   regions=[reg]).status_code, 422)
        self.assertEqual(self.post(to_frame="icrs", dataset_id=self.ids["sky"], unit="parsec",
                                   regions=[reg]).status_code, 422)


if __name__ == "__main__":
    unittest.main()
