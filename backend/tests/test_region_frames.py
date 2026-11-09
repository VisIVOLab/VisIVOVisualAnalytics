"""Region IO: sky frames, per-token size units and ds9 unit suffixes.

Regression tests for three faults in ``app.regions`` (commit 2de8b2a): a
region in another sky frame was handed to the image WCS unconverted (a
galactic region on an equatorial image gave [NaN, NaN]); a region's second
size took the first one's unit (``ellipse(10,20,1',30",0)`` gave 30 arcmin
instead of 0.5); ds9 sizes written in degrees (``0.01d``) could not be read
back. All WCSs are synthetic.
"""
from __future__ import annotations

import math
import os
import tempfile
import unittest

import numpy as np

from app import regions as R
from tests.test_regions import _GC_FK5, _TEST_TOKEN, _wcs, _wcs_header


def _axis_pa(center, angle_deg, src, dst):
    """Independent reference for a ds9 angle moved from frame ``src`` to ``dst``:
    step along the axis on the sky in ``src``, transform both ends, measure the
    step's position angle in ``dst``. ds9's angle is counter-clockwise from the
    longitude axis, i.e. PA = angle - 90; an axis is a line, so mod 180."""
    import astropy.units as u
    from astropy.coordinates import SkyCoord
    c = SkyCoord(center[0] * u.deg, center[1] * u.deg, frame=src)
    p = c.directional_offset_by((angle_deg - 90.0) * u.deg, 5 * u.arcsec)
    pa = c.transform_to(dst).position_angle(p.transform_to(dst)).deg
    return (pa + 90.0) % 180.0


def _pixel_axis_angle(center_sky, angle_deg, frame, wcs):
    """Independent reference for a sky angle on the pixel grid: the pixel
    direction of a 5" step along the axis (mod 180)."""
    import astropy.units as u
    from astropy.coordinates import SkyCoord
    c = SkyCoord(center_sky[0] * u.deg, center_sky[1] * u.deg, frame=frame)
    p = c.directional_offset_by((angle_deg - 90.0) * u.deg, 5 * u.arcsec)
    x0, y0 = wcs.world_to_pixel(c)
    x1, y1 = wcs.world_to_pixel(p)
    return math.degrees(math.atan2(float(y1 - y0), float(x1 - x0))) % 180.0


def _mod180_close(tc, a, b, places=4):
    d = (a - b + 90.0) % 180.0 - 90.0
    tc.assertAlmostEqual(d, 0.0, places=places, msg=f"{a} vs {b}")


class TestFrameConversion(unittest.TestCase):
    """Bug 1: regions are moved between sky frames, not handed to the WCS as is."""

    def setUp(self):
        self.gc_wcs = _wcs(crval=_GC_FK5, crpix=(32, 32))

    def test_galactic_region_on_equatorial_image(self):
        # (l, b) = (0, 0) is the image's reference pixel: it used to be [NaN, NaN].
        reg = {"shape": "circle", "frame": "galactic", "center": [0.0, 0.0],
               "radius": 5.0, "unit": "arcsec"}
        img = R.convert_regions([reg], to_frame="image", wcs=self.gc_wcs, origin=1)[0]
        self.assertAlmostEqual(img["center"][0], 32.0, places=6)
        self.assertAlmostEqual(img["center"][1], 32.0, places=6)
        self.assertAlmostEqual(img["radius"], 5.0, places=6)

    def test_every_shape_galactic_to_image(self):
        import astropy.units as u
        from astropy.coordinates import SkyCoord
        ref = [[0.0, 0.0], [0.001, 0.002], [-0.002, 0.001]]
        regs = [
            {"shape": "circle", "frame": "galactic", "center": ref[1], "radius": 2.0, "unit": "arcsec"},
            {"shape": "ellipse", "frame": "galactic", "center": ref[1], "semi_axes": [6.0, 3.0],
             "angle_deg": 20.0, "unit": "arcsec"},
            {"shape": "box", "frame": "galactic", "center": ref[1], "size": [6.0, 3.0],
             "angle_deg": 20.0, "unit": "arcsec"},
            {"shape": "annulus", "frame": "galactic", "center": ref[1], "radii": [2.0, 4.0], "unit": "arcsec"},
            {"shape": "point", "frame": "galactic", "center": ref[1], "unit": "deg"},
            {"shape": "polygon", "frame": "galactic", "points": ref, "unit": "deg"},
        ]
        out = R.convert_regions(regs, to_frame="image", wcs=self.gc_wcs, origin=1)
        want = [self.gc_wcs.world_to_pixel(SkyCoord(lon * u.deg, lat * u.deg, frame="galactic"))
                for lon, lat in ref]
        want = [[float(x) + 1.0, float(y) + 1.0] for x, y in want]   # 1-based
        for r in out[:5]:
            self.assertEqual(r["frame"], "image")
            self.assertAlmostEqual(r["center"][0], want[1][0], places=5)
            self.assertAlmostEqual(r["center"][1], want[1][1], places=5)
        for got, exp in zip(out[5]["points"], want):
            self.assertAlmostEqual(got[0], exp[0], places=5)
            self.assertAlmostEqual(got[1], exp[1], places=5)
        self.assertAlmostEqual(out[1]["semi_axes"][0], 6.0, places=6)
        self.assertAlmostEqual(out[3]["radii"][1], 4.0, places=6)
        # Galactic and equatorial north are 58.6 deg apart here.
        for r in (out[1], out[2]):
            _mod180_close(self, r["angle_deg"],
                          _pixel_axis_angle(ref[1], 20.0, "galactic", self.gc_wcs))

    def test_fk5_to_galactic_moves_coordinates_and_frame(self):
        reg = {"shape": "ellipse", "frame": "fk5", "center": list(_GC_FK5),
               "semi_axes": [10.0, 5.0], "angle_deg": 0.0, "unit": "arcsec"}
        gal = R.convert_regions([reg], to_frame="galactic")[0]   # sky → sky: no WCS
        self.assertEqual(gal["frame"], "galactic")
        lon = (gal["center"][0] + 180.0) % 360.0 - 180.0
        self.assertAlmostEqual(lon, 0.0, places=7)
        self.assertAlmostEqual(gal["center"][1], 0.0, places=7)
        self.assertEqual(gal["semi_axes"], [10.0, 5.0])
        _mod180_close(self, gal["angle_deg"], _axis_pa(_GC_FK5, 0.0, "fk5", "galactic"))
        self.assertGreater(abs(((gal["angle_deg"] + 90) % 180) - 90), 30.0)  # really rotated
        txt = R.export_ds9([gal])
        self.assertIn("\ngalactic\n", txt)
        back = R.convert_regions(R.parse_ds9(txt), to_frame="fk5")[0]
        self.assertAlmostEqual(back["center"][0], _GC_FK5[0], places=8)
        self.assertAlmostEqual(back["center"][1], _GC_FK5[1], places=8)
        _mod180_close(self, back["angle_deg"], 0.0, places=6)

    def test_all_sky_frames_round_trip(self):
        reg = {"shape": "box", "frame": "fk5", "center": [83.822, -5.391],
               "size": [30.0, 10.0], "angle_deg": 15.0, "unit": "arcsec"}
        for name in ("icrs", "fk4", "galactic", "ecliptic"):
            there = R.convert_regions([reg], to_frame=name)[0]
            self.assertEqual(there["frame"], name)
            _mod180_close(self, there["angle_deg"],
                          _axis_pa(reg["center"], 15.0, R.sky_frame("fk5"), R.sky_frame(name)))
            back = R.convert_regions([there], to_frame="fk5")[0]
            self.assertAlmostEqual(back["center"][0], 83.822, places=8, msg=name)
            self.assertAlmostEqual(back["center"][1], -5.391, places=8, msg=name)
            _mod180_close(self, back["angle_deg"], 15.0, places=6)

    def test_angle_on_rotated_and_mirrored_grids(self):
        for kw in ({"rot_deg": 30.0}, {"mirror": True}, {"rot_deg": -50.0, "mirror": True}):
            wcs = _wcs(crval=(202.0, 47.0), **kw)
            reg = {"shape": "ellipse", "frame": "fk5", "center": [202.0, 47.0],
                   "semi_axes": [10.0, 4.0], "angle_deg": 25.0, "unit": "arcsec"}
            img = R.convert_regions([reg], to_frame="image", wcs=wcs, origin=1)[0]
            _mod180_close(self, img["angle_deg"],
                          _pixel_axis_angle([202.0, 47.0], 25.0, "fk5", wcs), places=3)
            back = R.convert_regions([img], to_frame="fk5", wcs=wcs, origin=1)[0]
            _mod180_close(self, back["angle_deg"], 25.0, places=6)
            self.assertAlmostEqual(back["semi_axes"][0], 10.0, places=6)
            self.assertAlmostEqual(back["center"][0], 202.0, places=8)

    def test_angle_on_unequal_scales_and_skew(self):
        wcs = _wcs(crval=(10.0, 20.0))
        wcs.wcs.cdelt = [-2.0 / 3600.0, 1.0 / 3600.0]          # 2:1 pixels
        skew = _wcs(crval=(10.0, 20.0))
        skew.wcs.pc = [[1.0, 0.3], [0.0, 1.0]]                  # sheared grid
        for w in (wcs, skew):
            reg = {"shape": "ellipse", "frame": "fk5", "center": [10.0, 20.0],
                   "semi_axes": [10.0, 5.0], "angle_deg": 45.0, "unit": "arcsec"}
            img = R.convert_regions([reg], to_frame="image", wcs=w, origin=1)[0]
            _mod180_close(self, img["angle_deg"], _pixel_axis_angle([10.0, 20.0], 45.0, "fk5", w), places=3)
            back = R.convert_regions([img], to_frame="fk5", wcs=w, origin=1)[0]
            _mod180_close(self, back["angle_deg"], 45.0, places=3)

    def test_omitted_angle_turns_like_zero(self):
        wcs = _wcs(crval=(10.0, 20.0), rot_deg=30.0)
        base = {"shape": "box", "frame": "fk5", "center": [10.0, 20.0], "size": [10.0, 5.0], "unit": "arcsec"}
        omitted = R.convert_regions([base], to_frame="image", wcs=wcs, origin=1)[0]
        zero = R.convert_regions([{**base, "angle_deg": 0.0}], to_frame="image", wcs=wcs, origin=1)[0]
        self.assertAlmostEqual(omitted["angle_deg"], zero["angle_deg"], places=9)
        _mod180_close(self, zero["angle_deg"], _pixel_axis_angle([10.0, 20.0], 0.0, "fk5", wcs), places=3)

    def test_sip_far_side_is_an_error_not_a_crash(self):
        from astropy.wcs import Sip
        w = _wcs(crval=(10.0, 20.0))
        w.wcs.ctype = ["RA---TAN-SIP", "DEC--TAN-SIP"]
        a = np.zeros((3, 3)); b = np.zeros((3, 3)); a[2, 0] = 1e-5; b[0, 2] = 1e-5
        w.sip = Sip(a, b, None, None, w.wcs.crpix)
        reg = {"shape": "circle", "frame": "fk5", "center": [190.0, -20.0], "radius": 3.0, "unit": "arcsec"}
        with self.assertRaises(R.RegionParseError):
            R.convert_regions([reg], to_frame="image", wcs=w, origin=1)

    def test_standard_grid_keeps_the_angle(self):
        # North up, east left: a sky angle and a pixel angle are the same number.
        reg = {"shape": "box", "frame": "fk5", "center": [202.0, 47.0], "size": [10.0, 4.0],
               "angle_deg": 30.0, "unit": "arcsec"}
        img = R.convert_regions([reg], to_frame="image", wcs=_wcs(), origin=1)[0]
        self.assertAlmostEqual(img["angle_deg"], 30.0, places=4)

    def test_galactic_image_from_fk5_region(self):
        wcs = _wcs(crval=(0.0, 0.0), ctype=("GLON-CAR", "GLAT-CAR"), radesys=None)
        reg = {"shape": "circle", "frame": "fk5", "center": list(_GC_FK5), "radius": 3.0, "unit": "arcsec"}
        img = R.convert_regions([reg], to_frame="image", wcs=wcs, origin=1)[0]
        self.assertAlmostEqual(img["center"][0], 32.0, places=5)
        self.assertAlmostEqual(img["center"][1], 32.0, places=5)
        self.assertEqual(R.native_frame_name(wcs), "galactic")

    def test_off_projection_is_an_error(self):
        reg = {"shape": "circle", "frame": "fk5", "center": [22.0, -47.0], "radius": 3.0, "unit": "arcsec"}
        with self.assertRaises(R.RegionParseError):
            R.convert_regions([reg], to_frame="image", wcs=_wcs(), origin=1)

    def test_pixels_need_a_wcs(self):
        reg = {"shape": "circle", "frame": "image", "center": [3, 3], "radius": 3.0, "unit": "pix"}
        with self.assertRaises(R.RegionParseError):
            R.convert_regions([reg], to_frame="fk5")

    def test_crtf_b1950_is_fk4(self):
        reg = R.parse_crtf("circle[[266.4deg, -28.9deg], 3arcsec] coord=B1950\n")[0]
        self.assertEqual(reg["frame"], "fk4")
        j = R.convert_regions([reg], to_frame="fk5")[0]
        self.assertGreater(abs(j["center"][0] - 266.4), 0.5)   # 50 years of precession

    def test_crtf_global_coord_and_unsupported(self):
        reg = R.parse_crtf("global coord=GALACTIC\ncircle[[0deg, 0deg], 3arcsec]\n")[0]
        self.assertEqual(reg["frame"], "galactic")
        with self.assertRaises(R.RegionParseError):
            R.parse_crtf("circle[[0deg, 0deg], 3arcsec] coord=AZEL\n")

    def test_ds9_galactic_sexagesimal_is_degrees(self):
        reg = R.parse_ds9('galactic\ncircle(1:30:00,-0:30:00,3")\n')[0]
        self.assertAlmostEqual(reg["center"][0], 1.5)
        self.assertAlmostEqual(reg["center"][1], -0.5)

    def test_ds9_unsupported_system_refused(self):
        with self.assertRaises(R.RegionParseError):
            R.parse_ds9('wcs\ncircle(1,2,3")\n')


class TestSizeUnits(unittest.TestCase):
    """Bug 2: every size token carries its own unit."""

    def test_ds9_mixed_units(self):
        e = R.parse_ds9("fk5\nellipse(10,20,1',30\",0)\n")[0]
        self.assertEqual(e["unit"], "arcmin")
        self.assertAlmostEqual(e["semi_axes"][0], 1.0)
        self.assertAlmostEqual(e["semi_axes"][1], 0.5)       # was 30 (arcmin): ×60
        b = R.parse_ds9("fk5\nbox(10,20,1',30\",0)\n")[0]
        self.assertEqual(b["size"], [1.0, 0.5])
        a = R.parse_ds9("fk5\nannulus(10,20,30\",1')\n")[0]
        self.assertEqual(a["unit"], "arcsec")
        self.assertEqual(a["radii"], [30.0, 60.0])
        bare = R.parse_ds9("fk5\nannulus(10,20,0.01,1')\n")[0]   # bare = degrees in a sky frame
        self.assertEqual(bare["unit"], "deg")
        self.assertAlmostEqual(bare["radii"][1], 1.0 / 60.0)

    def test_crtf_mixed_units(self):
        e = R.parse_crtf("ellipse[[10deg, 20deg], [1arcmin, 30arcsec], 0deg] coord=J2000\n")[0]
        self.assertEqual((e["semi_axes"], e["unit"]), ([1.0, 0.5], "arcmin"))
        b = R.parse_crtf("centerbox[[10deg, 20deg], [1arcmin, 30arcsec]] coord=J2000\n")[0]
        self.assertEqual(b["size"], [1.0, 0.5])
        a = R.parse_crtf("annulus[[10deg, 20deg], [30arcsec, 1arcmin]] coord=J2000\n")[0]
        self.assertEqual(a["radii"], [30.0, 60.0])
        m = R.parse_crtf("circle[[10deg, 20deg], 500mas] coord=J2000\n")[0]
        self.assertEqual((m["radius"], m["unit"]), (0.5, "arcsec"))

    def test_mixed_pixels_and_angles_refused(self):
        with self.assertRaises(R.RegionParseError):
            R.parse_ds9("fk5\nellipse(10,20,5i,30\",0)\n")
        with self.assertRaises(R.RegionParseError):
            R.parse_crtf("ellipse[[10pix, 20pix], [5pix, 30arcsec], 0deg]\n")

    def test_angular_size_in_image_frame(self):
        # ds9 lets an image-frame region have an angular size; it becomes pixels
        # through the WCS (2 arcsec/pix here).
        reg = R.parse_ds9('image\ncircle(32,32,10")\n')[0]
        self.assertEqual((reg["frame"], reg["unit"], reg["radius"]), ("image", "arcsec", 10.0))
        img = R.convert_regions([reg], to_frame="image", wcs=_wcs(scale_arcsec=2.0), origin=1)[0]
        self.assertEqual((img["unit"], img["center"]), ("pix", [32.0, 32.0]))
        self.assertAlmostEqual(img["radius"], 5.0)

    def test_pixel_size_in_sky_frame(self):
        reg = R.parse_ds9("fk5\ncircle(202,47,6i)\n")[0]
        self.assertEqual((reg["unit"], reg["radius"]), ("pix", 6.0))
        self.assertIn("circle(202,47,6i)", R.export_ds9([reg]))
        img = R.convert_regions([reg], to_frame="image", wcs=_wcs(scale_arcsec=2.0), origin=1)[0]
        self.assertAlmostEqual(img["radius"], 6.0)


class TestDs9Suffixes(unittest.TestCase):
    """Bug 3: every ds9 unit suffix is read, so ds9 exports come back."""

    def test_all_suffixes(self):
        cases = {"0.01d": (0.01, "deg"), "0.001r": (math.degrees(0.001), "deg"),
                 "4p": (4.0, "pix"), "4i": (4.0, "pix"), "2'": (2.0, "arcmin"),
                 '3"': (3.0, "arcsec"), "2arcmin": (2.0, "arcmin"), "1e-3": (1e-3, "deg")}
        for tok, (val, unit) in cases.items():
            reg = R.parse_ds9(f"fk5\ncircle(10,20,{tok})\n")[0]
            self.assertAlmostEqual(reg["radius"], val, msg=tok)
            self.assertEqual(reg["unit"], unit, tok)

    def test_one_line_frame_form(self):
        regs = R.parse_ds9("fk5;circle(10,20,0.01d) # color=red\ngalactic; point(1,2)\n")
        self.assertEqual([(r["shape"], r["frame"]) for r in regs], [("circle", "fk5"), ("point", "galactic")])
        self.assertEqual(regs[0]["props"], {"color": "red"})

    def test_decimal_positions_with_suffix(self):
        reg = R.parse_ds9('fk5\ncircle(10.5d,-20.25d,3")\n')[0]
        self.assertEqual(reg["center"], [10.5, -20.25])

    def test_export_import_every_shape_frame_and_unit(self):
        shapes = [
            ("circle", {"radius": 1.25}), ("ellipse", {"semi_axes": [2.5, 1.0], "angle_deg": 33.0}),
            ("box", {"size": [3.0, 1.5], "angle_deg": 12.5}), ("annulus", {"radii": [1.0, 2.75]}),
            ("point", {}), ("polygon", {}),
        ]
        frames = {"image": ["pix", "arcsec"], "fk5": ["deg", "arcmin", "arcsec", "pix"],
                  "fk4": ["arcsec"], "icrs": ["arcsec"], "galactic": ["deg"], "ecliptic": ["arcmin"]}
        n = 0
        for fmt in ("ds9", "crtf"):
            for frame, units in frames.items():
                c = [101.123456, 57.5] if frame == "image" else [266.40499623, -28.93617240]
                for unit in units:
                    for shape, extra in shapes:
                        reg = {"shape": shape, "frame": frame, "unit": unit, **extra}
                        if shape == "polygon":
                            reg["points"] = [c, [c[0] + 0.001, c[1]], [c[0], c[1] + 0.002]]
                        else:
                            reg["center"] = c
                        tag = (fmt, frame, unit, shape)
                        back = R.parse_regions(R.export_regions([reg], fmt), fmt)
                        self.assertEqual(len(back), 1, tag)
                        b = back[0]
                        self.assertEqual((b["shape"], b["frame"]), (shape, frame), tag)
                        if shape not in ("point", "polygon"):
                            self.assertEqual(b["unit"], unit, tag)
                        for key in ("center", "points", "radius", "semi_axes", "size", "radii", "angle_deg"):
                            if key in reg:
                                np.testing.assert_allclose(b[key], reg[key], rtol=0, atol=1e-9,
                                                           err_msg=str(tag))
                        n += 1
        self.assertEqual(n, 2 * 10 * 6)


class TestRegionFramesHttp(unittest.TestCase):
    """The routes on a 64×64 FK5 image centred on the Galactic centre."""

    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        from astropy.io import fits
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": _TEST_TOKEN}
        tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
        tmp.close()
        fits.PrimaryHDU(np.ones((64, 64), np.float32),
                        _wcs_header(crval=_GC_FK5, crpix=(32, 32))).writeto(tmp.name, overwrite=True)
        cls.img = tmp.name
        d = cls.client.post("/v1/datasets/open", json={"path": cls.img}, headers=cls._auth).json()
        cls.dataset_id = d["dataset_id"]
        cls.h = dict(cls._auth)
        if d.get("session_id"):
            cls.h["X-Visivo-Session"] = d["session_id"]

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.img)

    def test_parse_galactic_to_image(self):
        r = self.client.post("/v1/regions/parse", headers=self.h, json={
            "text": 'galactic\ncircle(0,0,3")\n', "to_frame": "image", "dataset_id": self.dataset_id})
        self.assertEqual(r.status_code, 200, r.text)
        reg = r.json()["regions"][0]
        self.assertAlmostEqual(reg["center"][0], 32.0, places=5)
        self.assertAlmostEqual(reg["center"][1], 32.0, places=5)

    def test_export_fk5_to_galactic_without_dataset(self):
        r = self.client.post("/v1/regions/export", headers=self.h, json={
            "format": "ds9", "to_frame": "galactic",
            "regions": [{"shape": "circle", "frame": "fk5", "center": list(_GC_FK5),
                         "radius": 3.0, "unit": "arcsec"}]})
        self.assertEqual(r.status_code, 200, r.text)
        reg = R.parse_ds9(r.json()["text"])[0]
        self.assertEqual(reg["frame"], "galactic")
        self.assertAlmostEqual((reg["center"][0] + 180) % 360 - 180, 0.0, places=7)

    def test_export_pixels_to_sky_needs_dataset(self):
        r = self.client.post("/v1/regions/export", headers=self.h, json={
            "format": "ds9", "to_frame": "fk5",
            "regions": [{"shape": "circle", "frame": "image", "center": [32, 32], "radius": 3, "unit": "pix"}]})
        self.assertEqual(r.status_code, 422)

    def test_export_pixels_to_galactic(self):
        r = self.client.post("/v1/regions/export", headers=self.h, json={
            "format": "crtf", "to_frame": "galactic", "dataset_id": self.dataset_id,
            "regions": [{"shape": "circle", "frame": "image", "center": [32, 32], "radius": 3, "unit": "pix"}]})
        self.assertEqual(r.status_code, 200, r.text)
        self.assertIn("coord=GALACTIC", r.json()["text"])
        reg = R.parse_crtf(r.json()["text"])[0]
        self.assertAlmostEqual((reg["center"][0] + 180) % 360 - 180, 0.0, places=7)
        self.assertAlmostEqual(reg["radius"], 3.0, places=6)

    def test_stats_with_galactic_region(self):
        r = self.client.post(f"/v1/regions/stats/{self.dataset_id}", headers=self.h, json={
            "region": {"shape": "circle", "frame": "galactic", "center": [0.0, 0.0],
                       "radius": 4.0, "unit": "arcsec"}})
        self.assertEqual(r.status_code, 200, r.text)
        self.assertGreater(r.json()["valid_count"], 40)        # ~π·4² pixels at 1"/pix

    def test_off_image_projection_is_422(self):
        r = self.client.post("/v1/regions/parse", headers=self.h, json={
            "text": 'fk5\ncircle(86.4,28.9,3")\n', "to_frame": "image", "dataset_id": self.dataset_id})
        self.assertEqual(r.status_code, 422, r.text)


if __name__ == "__main__":
    unittest.main()
