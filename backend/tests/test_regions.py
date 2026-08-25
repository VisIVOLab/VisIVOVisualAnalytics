"""Tests for CRTF/DS9 region interchange (app.regions + /v1/regions/*)."""
from __future__ import annotations

import os
import tempfile
import unittest

import numpy as np

from app import regions as R

_TEST_TOKEN = "test-token-visivo-12345"


# ── unit: parse / export / round-trip ────────────────────────────────────────


class TestDs9(unittest.TestCase):
    DS9 = """# Region file format: DS9 version 4.1
fk5
circle(202.4,47.2,30")
ellipse(202.4,47.2,30",20",30)
box(202.4,47.2,1',2',0)
polygon(202.4,47.2,202.5,47.3,202.6,47.2)
point(202.4,47.2)
annulus(202.4,47.2,10",20")
image
circle(100,100,15)
box(50,50,20,30,0)
"""

    def test_parse_all_shapes(self):
        regs = R.parse_regions(self.DS9)
        self.assertEqual([r["shape"] for r in regs],
                         ["circle", "ellipse", "box", "polygon", "point", "annulus", "circle", "box"])

    def test_frames_detected(self):
        regs = R.parse_regions(self.DS9)
        self.assertEqual(regs[0]["frame"], "fk5")
        self.assertEqual(regs[-1]["frame"], "image")

    def test_world_circle_radius_unit(self):
        regs = R.parse_regions(self.DS9)
        self.assertAlmostEqual(regs[0]["radius"], 30.0)
        self.assertEqual(regs[0]["unit"], "arcsec")

    def test_image_circle_pixels(self):
        regs = R.parse_regions(self.DS9)
        self.assertEqual(regs[6]["radius"], 15.0)
        self.assertEqual(regs[6]["unit"], "pix")

    def test_round_trip(self):
        regs = R.parse_regions(self.DS9)
        regs2 = R.parse_regions(R.export_ds9(regs))
        self.assertEqual([r["shape"] for r in regs], [r["shape"] for r in regs2])
        self.assertEqual([r["frame"] for r in regs], [r["frame"] for r in regs2])
        self.assertAlmostEqual(regs[0]["radius"], regs2[0]["radius"])


class TestCrtf(unittest.TestCase):
    CRTF = """#CRTF
circle[[202.4deg, 47.2deg], 30arcsec] coord=J2000
circle[[18h12m24s, -23d11m00s], 2.3arcsec] coord=J2000
ellipse[[100pix, 100pix], [30pix, 20pix], 30deg]
box[[10pix, 10pix], [50pix, 40pix]]
centerbox[[202.4deg, 47.2deg], [1arcmin, 2arcmin]] coord=J2000
rotbox[[100pix,100pix],[50pix,40pix], 30deg]
poly[[1pix,1pix],[2pix,2pix],[3pix,1pix]]
annulus[[202.4deg,47.2deg],[10arcsec,20arcsec]] coord=J2000
symbol[[100pix,100pix], .]
"""

    def test_parse_all_shapes(self):
        regs = R.parse_regions(self.CRTF)
        self.assertEqual([r["shape"] for r in regs],
                         ["circle", "circle", "ellipse", "box", "box", "box",
                          "polygon", "annulus", "point"])

    def test_sexagesimal_to_deg(self):
        regs = R.parse_regions(self.CRTF)
        ra, dec = regs[1]["center"]
        self.assertAlmostEqual(ra, (18 + 12 / 60 + 24 / 3600) * 15.0, places=3)
        self.assertAlmostEqual(dec, -(23 + 11 / 60), places=3)

    def test_box_corners_to_center_size(self):
        regs = R.parse_regions(self.CRTF)
        box = regs[3]  # box[[10,10],[50,40]] → center (30,25) size (40,30)
        self.assertEqual(box["center"], [30.0, 25.0])
        self.assertEqual(box["size"], [40.0, 30.0])

    def test_rotbox_angle(self):
        regs = R.parse_regions(self.CRTF)
        self.assertEqual(regs[5]["angle_deg"], 30.0)

    def test_ellipse_angle(self):
        regs = R.parse_regions(self.CRTF)
        self.assertEqual(regs[2]["angle_deg"], 30.0)

    def test_round_trip(self):
        regs = R.parse_regions(self.CRTF)
        regs2 = R.parse_regions(R.export_crtf(regs), "crtf")
        self.assertEqual([r["shape"] for r in regs], [r["shape"] for r in regs2])


class TestCrossFormatAndConvert(unittest.TestCase):
    def test_ds9_to_crtf_to_ds9(self):
        ds9 = "fk5\ncircle(202.4,47.2,30\")\nimage\ncircle(100,100,15)\n"
        regs = R.parse_regions(ds9)
        crtf = R.export_regions(regs, "crtf")
        back = R.parse_regions(crtf, "crtf")
        self.assertEqual([r["shape"] for r in regs], [r["shape"] for r in back])
        self.assertEqual(regs[1]["frame"], "image")

    def test_convert_world_to_image_with_fake_wcs(self):
        # Fake linear WCS: 1 arcsec/pix, ref pixel (50,50) at (200deg, 0deg).
        scale_deg = 1.0 / 3600.0

        def sky_to_pix(ra, dec):
            return (50 + (ra - 200.0) / scale_deg, 50 + (dec - 0.0) / scale_deg)

        def pix_to_sky(x, y):
            return (200.0 + (x - 50) * scale_deg, 0.0 + (y - 50) * scale_deg)

        world = [{"shape": "circle", "frame": "fk5", "center": [200.0, 0.0],
                  "radius": 10.0, "unit": "arcsec", "props": {}}]
        img = R.convert_regions(world, to_frame="image", sky_to_pix=sky_to_pix,
                                pix_to_sky=pix_to_sky, pixel_scale_arcsec=1.0)
        self.assertEqual(img[0]["frame"], "image")
        self.assertEqual(img[0]["unit"], "pix")
        self.assertAlmostEqual(img[0]["center"][0], 50.0, places=3)
        self.assertAlmostEqual(img[0]["radius"], 10.0, places=3)  # 10 arcsec / 1 arcsec/pix


# ── HTTP endpoints ───────────────────────────────────────────────────────────


def _make_wcs_image(nx=64, ny=64) -> str:
    from astropy.io import fits
    data = np.zeros((ny, nx), dtype=np.float32)
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["CTYPE1"] = "RA---TAN"; h["CRPIX1"] = 32; h["CRVAL1"] = 202.0; h["CDELT1"] = -1.0 / 3600.0; h["CUNIT1"] = "deg"
    h["CTYPE2"] = "DEC--TAN"; h["CRPIX2"] = 32; h["CRVAL2"] = 47.0; h["CDELT2"] = 1.0 / 3600.0; h["CUNIT2"] = "deg"
    h["RADESYS"] = "FK5"
    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
    tmp.close()
    return tmp.name


class TestRegionsHttp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": _TEST_TOKEN}
        cls.img = _make_wcs_image()
        r = cls.client.post("/v1/datasets/open", json={"path": cls.img}, headers=cls._auth)
        d = r.json()
        cls.dataset_id = d["dataset_id"]
        cls.session_id = d.get("session_id", "")

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.img)

    def _headers(self):
        h = dict(self._auth)
        if self.session_id:
            h["X-Visivo-Session"] = self.session_id
        return h

    def test_parse_ds9(self):
        r = self.client.post("/v1/regions/parse",
                             json={"text": "image\ncircle(10,20,5)\n"}, headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        self.assertEqual(d["format"], "ds9")
        self.assertEqual(d["n_regions"], 1)
        self.assertEqual(d["regions"][0]["shape"], "circle")

    def test_parse_auto_detects_crtf(self):
        r = self.client.post("/v1/regions/parse",
                             json={"text": "#CRTF\ncircle[[10pix,20pix],5pix]\n"},
                             headers=self._headers())
        self.assertEqual(r.json()["format"], "crtf")

    def test_export_round_trip(self):
        parsed = self.client.post("/v1/regions/parse",
                                  json={"text": "image\nbox(50,50,20,30,0)\n"},
                                  headers=self._headers()).json()
        r = self.client.post("/v1/regions/export",
                             json={"format": "crtf", "regions": parsed["regions"]},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        self.assertIn("centerbox", r.json()["text"])

    def test_parse_with_world_to_image_conversion(self):
        # A world circle at the image reference point should land near pixel (32,32).
        text = "fk5\ncircle(202.0,47.0,3\")\n"
        r = self.client.post("/v1/regions/parse",
                             json={"text": text, "to_frame": "image", "dataset_id": self.dataset_id},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        reg = r.json()["regions"][0]
        self.assertEqual(reg["frame"], "image")
        self.assertAlmostEqual(reg["center"][0], 32.0, delta=0.5)
        self.assertAlmostEqual(reg["center"][1], 32.0, delta=0.5)
        self.assertAlmostEqual(reg["radius"], 3.0, delta=0.1)  # 3" / 1"/pix

    def test_invalid_text_returns_422(self):
        r = self.client.post("/v1/regions/parse",
                             json={"text": "circle[[bad", "format": "crtf"},
                             headers=self._headers())
        self.assertEqual(r.status_code, 422)


def _make_value_image(value=2.0, nx=64, ny=64, beam=False) -> str:
    from astropy.io import fits
    data = np.full((ny, nx), value, dtype=np.float32)
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["CTYPE1"] = "RA---TAN"; h["CRPIX1"] = 32; h["CRVAL1"] = 202.0; h["CDELT1"] = -1.0 / 3600.0; h["CUNIT1"] = "deg"
    h["CTYPE2"] = "DEC--TAN"; h["CRPIX2"] = 32; h["CRVAL2"] = 47.0; h["CDELT2"] = 1.0 / 3600.0; h["CUNIT2"] = "deg"
    h["RADESYS"] = "FK5"
    if beam:
        h["BUNIT"] = "Jy/beam"
        h["BMAJ"] = 5.0 / 3600.0  # 5 arcsec
        h["BMIN"] = 5.0 / 3600.0
        h["BPA"] = 0.0
    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
    tmp.close()
    return tmp.name


class TestRegionStats(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": _TEST_TOKEN}
        cls.img = _make_value_image(value=2.0)
        cls.beam_img = _make_value_image(value=1.0, beam=True)
        cls.ds = cls._open(cls, cls.img)
        cls.beam_ds = cls._open(cls, cls.beam_img)

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.img); os.unlink(cls.beam_img)

    def _open(self, path):
        r = self.client.post("/v1/datasets/open", json={"path": path}, headers=self._auth)
        d = r.json()
        self.session_id = d.get("session_id", "")
        return d["dataset_id"]

    def _headers(self):
        h = dict(self._auth)
        if getattr(self, "session_id", ""):
            h["X-Visivo-Session"] = self.session_id
        return h

    def _stats(self, dataset_id, region):
        return self.client.post(f"/v1/regions/stats/{dataset_id}",
                                json={"region": region}, headers=self._headers())

    def test_circle_mean_and_count(self):
        reg = {"shape": "circle", "frame": "image", "center": [32, 32], "radius": 10, "unit": "pix"}
        r = self._stats(self.ds, reg)
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        self.assertAlmostEqual(d["mean"], 2.0, places=5)
        self.assertAlmostEqual(d["rms"], 0.0, places=5)
        # Pixel count is approximately the disc area π r².
        self.assertAlmostEqual(d["valid_count"], np.pi * 100, delta=0.1 * np.pi * 100)
        self.assertAlmostEqual(d["sum"], 2.0 * d["valid_count"], places=4)

    def test_box_count_exact(self):
        reg = {"shape": "box", "frame": "image", "center": [32, 32], "size": [10, 6],
               "angle_deg": 0.0, "unit": "pix"}
        d = self._stats(self.ds, reg).json()
        # Axis-aligned 10×6 box → 11×7 = 77 pixels (inclusive of the boundary).
        self.assertGreaterEqual(d["valid_count"], 60)
        self.assertEqual(d["mean"], 2.0)

    def test_polygon_stats(self):
        reg = {"shape": "polygon", "frame": "image",
               "points": [[10, 10], [30, 10], [30, 30], [10, 30]], "unit": "pix"}
        d = self._stats(self.ds, reg).json()
        self.assertTrue(d["valid"])
        self.assertGreater(d["valid_count"], 300)  # ~20×20 square
        self.assertEqual(d["mean"], 2.0)

    def test_world_region_converted(self):
        # World circle at the image reference point → stats near pixel centre.
        reg = {"shape": "circle", "frame": "fk5", "center": [202.0, 47.0],
               "radius": 5.0, "unit": "arcsec"}
        d = self._stats(self.ds, reg).json()
        self.assertTrue(d["valid"])
        self.assertAlmostEqual(d["mean"], 2.0, places=5)

    def test_flux_jy_with_beam(self):
        reg = {"shape": "circle", "frame": "image", "center": [32, 32], "radius": 10, "unit": "pix"}
        d = self._stats(self.beam_ds, reg).json()
        self.assertIsNotNone(d["flux_jy"])
        # flux = sum(Jy/beam) / beam_area_pixels; sum == valid_count (image is 1.0).
        self.assertAlmostEqual(d["flux_jy"], d["sum"] / d["beam_area_pix"], places=5)
        self.assertGreater(d["flux_jy"], 0.0)

    def test_empty_region_422(self):
        reg = {"shape": "circle", "frame": "image", "center": [1000, 1000], "radius": 2, "unit": "pix"}
        r = self._stats(self.ds, reg)
        self.assertEqual(r.status_code, 422)


def _make_cube(nz=16, ny=24, nx=24, nan_count=3) -> str:
    from astropy.io import fits
    rng = np.random.default_rng(7)
    data = rng.standard_normal((nz, ny, nx)).astype(np.float32)
    flat = data.reshape(-1)
    flat[:nan_count] = np.nan          # a few blank voxels
    hdr = fits.Header()
    hdr["BUNIT"] = "Jy/beam"
    hdr["CTYPE3"] = "FREQ"
    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    tmp.close()
    fits.PrimaryHDU(data, hdr).writeto(tmp.name, overwrite=True)
    return tmp.name


class TestCubeStats(unittest.TestCase):
    """Out-of-core whole-cube statistics endpoint (/v1/analysis/cube-stats)."""

    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": _TEST_TOKEN}
        cls.cube = _make_cube()
        # keep the reference finite data to compare against
        from astropy.io import fits
        cls.data = fits.getdata(cls.cube).astype(np.float64)
        r = cls.client.post("/v1/datasets/open", json={"path": cls.cube}, headers=cls._auth)
        d = r.json()
        cls.session_id = d.get("session_id", "")
        cls.ds = d["dataset_id"]

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.cube)

    def _headers(self):
        h = dict(self._auth)
        if self.session_id:
            h["X-Visivo-Session"] = self.session_id
        return h

    def test_full_cube_stats_match_numpy(self):
        r = self.client.get(f"/v1/analysis/cube-stats/{self.ds}", headers=self._headers())
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        finite = self.data[np.isfinite(self.data)]
        self.assertEqual(d["valid_count"], int(finite.size))
        self.assertEqual(d["blanked_count"], int(self.data.size - finite.size))
        self.assertAlmostEqual(d["mean"], float(finite.mean()), places=5)
        self.assertAlmostEqual(d["std"], float(finite.std()), places=5)
        self.assertAlmostEqual(d["rms"], float(np.sqrt((finite ** 2).mean())), places=5)
        self.assertAlmostEqual(d["min"], float(finite.min()), places=5)
        self.assertAlmostEqual(d["max"], float(finite.max()), places=5)
        self.assertAlmostEqual(d["sum"], float(finite.sum()), places=3)
        # Small cube → the median sample is the full data, so it's exact.
        self.assertTrue(d["median_exact"])
        self.assertAlmostEqual(d["median"], float(np.median(finite)), places=5)

    def test_cube_stats_cached_on_second_call(self):
        self.client.get(f"/v1/analysis/cube-stats/{self.ds}", headers=self._headers())
        r2 = self.client.get(f"/v1/analysis/cube-stats/{self.ds}", headers=self._headers())
        self.assertTrue(r2.json()["cached"])


if __name__ == "__main__":
    unittest.main()
