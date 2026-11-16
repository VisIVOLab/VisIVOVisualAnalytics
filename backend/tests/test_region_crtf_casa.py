"""CRTF angles and default frame against CASA (issue #151).

The fixtures are written by hand after the CASA documentation, "Region File
Format" (casadocs, Image Analysis ▸ Region Files ▸ Region File Format):
``Allowed Shapes`` (``ellipse[[x, y], [bmaj, bmin], pa]``, ``rotbox[[x, y],
[x_width, y_width], rotang]``), ``Global Definitions`` (``coord``, "Default:
image value") and ``Examples``. The documentation does not say where the
angles are measured from; the expected numbers below were measured with
casatools 6.7.0 (``regionmanager.fromtext`` + ``image.set(region=...)``) on
201 × 201 SIN grids with 1" pixels centred on (150°, 2°): the direction of
the mask's long axis, counter-clockwise from pixel +x, and its pixel count.
CASA's source agrees: ``AnnEllipse`` lays the first semi-axis at ``pa + 90``
from pixel +x ("astronomers measure the position angle relative to north"),
and ``AnnPolygon`` turns a rotated box's sky offsets by ``-pa`` ("pa is
measured from north through east").
"""
from __future__ import annotations

import math
import os
import tempfile
import unittest

import numpy as np

from app import regions as R
from app.region_stats import Region
from tests.test_regions import _TEST_TOKEN


def _header(ctype=("RA---SIN", "DEC--SIN"), mirror=False, rot_deg=0.0):
    """The grids the CASA masks were measured on (``rot_deg`` as a PC matrix)."""
    from astropy.io import fits
    h = fits.Header()
    h["NAXIS"], h["NAXIS1"], h["NAXIS2"] = 2, 201, 201
    h["CTYPE1"], h["CTYPE2"] = ctype
    h["CRVAL1"], h["CRVAL2"] = 150.0, 2.0
    h["CRPIX1"], h["CRPIX2"] = 101.0, 101.0
    h["CDELT1"] = (1.0 if mirror else -1.0) / 3600.0
    h["CDELT2"] = 1.0 / 3600.0
    h["CUNIT1"] = h["CUNIT2"] = "deg"
    if ctype[0].startswith("RA"):
        h["RADESYS"], h["EQUINOX"] = "FK5", 2000.0
    if rot_deg:
        c, s = math.cos(math.radians(rot_deg)), math.sin(math.radians(rot_deg))
        h["PC1_1"], h["PC1_2"], h["PC2_1"], h["PC2_2"] = c, -s, s, c
    return h


def _wcs(**kw):
    from astropy.wcs import WCS
    return WCS(_header(**kw)).celestial


def _mask(line: str, wcs) -> np.ndarray:
    """The pixels VisIVO puts inside a CRTF line on ``wcs`` (numpy [y, x])."""
    rep = R.parse_report("#CRTF\n" + line + "\n", "crtf", to_frame="image", wcs=wcs, origin=1)
    assert rep["regions"], rep["diagnostics"]
    reg = dict(rep["regions"][0])
    reg["center"] = [v - 1.0 for v in reg["center"]]       # 1-based → numpy
    yy, xx = np.mgrid[0:201, 0:201]
    return Region(reg).mask(xx, yy)


def _long_axis(mask: np.ndarray) -> float:
    """Direction of a mask's long axis, counter-clockwise from +x, mod 180."""
    y, x = np.nonzero(mask)
    w, v = np.linalg.eigh(np.cov(np.vstack([x - x.mean(), y - y.mean()])))
    return math.degrees(math.atan2(v[1, 1], v[0, 1])) % 180.0


class TestCrtfAnglesAgainstCasa(unittest.TestCase):
    """Long-axis direction and pixel count of CASA's mask for each line."""

    # line, grid, CASA long axis (deg from +x), CASA pixel count
    CASES = [
        ("ellipse[[100pix, 100pix], [40pix, 10pix], 0deg]", {}, 90.0, 1249),
        ("ellipse[[100pix, 100pix], [40pix, 10pix], 30deg]", {}, 120.0, 1249),
        # The first semi-axis is along pa even when it is the shorter one.
        ("ellipse[[100pix, 100pix], [10pix, 40pix], 30deg]", {}, 30.0, 1249),
        ("ellipse[[150deg, 2deg], [40arcsec, 10arcsec], 0deg], coord=J2000", {}, 90.0, 1249),
        ("ellipse[[150deg, 2deg], [40arcsec, 10arcsec], 30deg], coord=J2000", {}, 120.0, 1249),
        ("rotbox[[100pix, 100pix], [80pix, 20pix], 0deg]", {}, 0.0, 1701),
        ("rotbox[[100pix, 100pix], [80pix, 20pix], 30deg]", {}, 30.0, 1603),
        ("rotbox[[150deg, 2deg], [80arcsec, 20arcsec], 30deg], coord=J2000", {}, 30.0, 1603),
        ("rotbox[[150deg, 2deg], [80arcsec, 20arcsec], 0.5236rad], coord=J2000", {}, 30.0, 1603),
        ("centerbox[[100pix, 100pix], [80pix, 20pix]]", {}, 0.0, 1701),
        # A sky rotbox follows the sky on any grid: east to the right ...
        ("rotbox[[150deg, 2deg], [80arcsec, 20arcsec], 30deg], coord=J2000",
         {"mirror": True}, 150.0, 1603),
        # ... and the sky turned by 25° on the grid (CASA's mask: 5.1°).
        ("rotbox[[150deg, 2deg], [80arcsec, 20arcsec], 30deg], coord=J2000",
         {"rot_deg": 25.0}, 5.0, 1603),
        ("centerbox[[150deg, 2deg], [80arcsec, 20arcsec]], coord=J2000",
         {"rot_deg": 25.0}, 155.0, 1601),
    ]

    def test_long_axis_and_area_match_casa(self):
        for line, grid, casa_axis, casa_n in self.CASES:
            with self.subTest(line=line, grid=grid):
                m = _mask(line, _wcs(**grid))
                d = (_long_axis(m) - casa_axis + 90.0) % 180.0 - 90.0
                self.assertLess(abs(d), 0.5, f"long axis {_long_axis(m):.2f} vs CASA {casa_axis}")
                self.assertLess(abs(int(m.sum()) - casa_n) / casa_n, 0.01)


class TestCrtfAngleReading(unittest.TestCase):
    """What the angles become in the canonical (ds9) model, and back."""

    def test_ellipse_first_semi_axis_along_pa(self):
        e = R.parse_crtf("ellipse[[150deg, 2deg], [40arcsec, 10arcsec], 30deg], coord=J2000\n")[0]
        # ds9 axis at 30° (position angle -60°) carries b2; b1 lies at PA 30°.
        self.assertEqual((e["semi_axes"], e["angle_deg"], e["unit"]), ([10.0, 40.0], 30.0, "arcsec"))

    def test_rotbox_angle_unchanged(self):
        b = R.parse_crtf("rotbox[[100pix, 100pix], [80pix, 20pix], 30deg]\n")[0]
        self.assertEqual((b["shape"], b["center"], b["size"], b["angle_deg"]),
                         ("box", [101.0, 101.0], [80.0, 20.0], 30.0))

    def test_documentation_example_rotbox(self):
        # "Examples": the rotated box for a range of velocities, under the
        # example's global line (B1950_VLA is FK4 B1950 here).
        text = ("#CRTFv0\nglobal coord=B1950_VLA, frame=BARY, corr=[I, Q], color=blue\n"
                "rotbox[[12h01m34.1s, 12d23m33s], [3arcmin, 1arcmin], 12deg], "
                "range=[-1240km/s, 1240km/s]\n")
        b = R.parse_crtf(text)[0]
        self.assertEqual((b["frame"], b["size"], b["unit"], b["angle_deg"]),
                         ("fk4", [3.0, 1.0], "arcmin", 12.0))
        self.assertAlmostEqual(b["center"][0], (12 + 1 / 60 + 34.1 / 3600) * 15.0, places=9)
        self.assertAlmostEqual(b["center"][1], 12 + 23 / 60 + 33 / 3600, places=9)

    def test_round_trip_keeps_casa_numbers(self):
        lines = ["ellipse[[150deg, 2deg], [40arcsec, 10arcsec], 30deg] coord=J2000",
                 "ellipse[[99pix, 99pix], [10pix, 40pix], 30deg]",
                 "rotbox[[150deg, 2deg], [80arcsec, 20arcsec], 12deg] coord=J2000",
                 "rotbox[[99pix, 99pix], [80pix, 20pix], 30deg]"]
        regs = R.parse_crtf("#CRTF\n" + "\n".join(lines) + "\n")
        out = R.export_crtf(regs).splitlines()
        self.assertEqual(out, ["#CRTF"] + lines)
        back = R.parse_crtf("\n".join(out))
        for a, b in zip(regs, back):
            for key in ("center", "semi_axes", "size", "angle_deg", "frame", "unit"):
                self.assertEqual(a.get(key), b.get(key), key)


class TestCrtfDefaultFrame(unittest.TestCase):
    """``coord`` — "Default: image value": a sky region that states no frame
    is in the image's own. CASA's mask of ``circle[[150deg, 2deg], 10arcsec]``
    on the galactic grid is centred on its reference pixel (100, 100, 0-based;
    317 pixels); with ``coord=J2000`` CASA finds no region on that image."""

    def test_no_coord_takes_the_image_frame(self):
        rep = R.parse_report("#CRTF\ncircle[[150deg, 2deg], 10arcsec]\n", "crtf",
                             to_frame="image", wcs=_wcs(ctype=("GLON-SIN", "GLAT-SIN")), origin=1)
        reg = rep["regions"][0]
        np.testing.assert_allclose(reg["center"], [101.0, 101.0], atol=1e-6)
        self.assertAlmostEqual(reg["radius"], 10.0, places=6)

    def test_explicit_j2000_is_not_the_image_frame(self):
        rep = R.parse_report("#CRTF\ncircle[[150deg, 2deg], 10arcsec], coord=J2000\n", "crtf",
                             to_frame="image", wcs=_wcs(ctype=("GLON-SIN", "GLAT-SIN")), origin=1)
        for reg in rep["regions"]:
            self.assertGreater(math.dist(reg["center"], [101.0, 101.0]), 1000.0)

    def test_frame_kept_when_not_converted(self):
        rep = R.parse_report("#CRTF\ncircle[[150deg, 2deg], 10arcsec]\n", "crtf",
                             wcs=_wcs(ctype=("GLON-SIN", "GLAT-SIN")))
        self.assertEqual(rep["regions"][0]["frame"], "galactic")
        rep = R.parse_report("#CRTF\ncircle[[150deg, 2deg], 10arcsec]\n", "crtf", wcs=_wcs())
        self.assertEqual(rep["regions"][0]["frame"], "fk5")

    def test_global_coord_wins_over_the_image(self):
        rep = R.parse_report("#CRTF\nglobal coord=J2000\ncircle[[150deg, 2deg], 10arcsec]\n", "crtf",
                             wcs=_wcs(ctype=("GLON-SIN", "GLAT-SIN")))
        self.assertEqual(rep["regions"][0]["frame"], "fk5")

    def test_without_an_image_j2000(self):
        self.assertEqual(R.parse_crtf("circle[[150deg, 2deg], 10arcsec]\n")[0]["frame"], "fk5")
        self.assertEqual(R.parse_report("circle[[150deg, 2deg], 10arcsec]\n", "crtf")["regions"][0]["frame"],
                         "fk5")
        with self.assertRaises(R.RegionParseError):
            R.parse_crtf("circle[[150deg, 2deg], 10arcsec]\n", default_frame="supergal")

    def test_ds9_unchanged(self):
        # ds9 has its own frame lines; the default applies to CRTF only.
        rep = R.parse_report("fk5\ncircle(150,2,10\")\n", "ds9", wcs=_wcs(ctype=("GLON-SIN", "GLAT-SIN")))
        self.assertEqual(rep["regions"][0]["frame"], "fk5")


class TestCrtfDefaultFrameHttp(unittest.TestCase):
    """``/v1/regions/parse`` with a galactic dataset."""

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
        fits.PrimaryHDU(np.zeros((201, 201), np.float32),
                        _header(ctype=("GLON-SIN", "GLAT-SIN"))).writeto(tmp.name, overwrite=True)
        cls.img = tmp.name
        d = cls.client.post("/v1/datasets/open", json={"path": cls.img}, headers=cls._auth).json()
        cls.dataset_id = d["dataset_id"]
        cls.h = dict(cls._auth)
        if d.get("session_id"):
            cls.h["X-Visivo-Session"] = d["session_id"]

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.img)

    def test_parse_without_coord_on_galactic_image(self):
        text = "#CRTF\ncircle[[150deg, 2deg], 10arcsec]\nellipse[[150deg, 2deg], [40arcsec, 10arcsec], 30deg]\n"
        r = self.client.post("/v1/regions/parse", headers=self.h, json={
            "text": text, "dataset_id": self.dataset_id})
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual([g["frame"] for g in r.json()["regions"]], ["galactic", "galactic"])
        r = self.client.post("/v1/regions/parse", headers=self.h, json={
            "text": text, "dataset_id": self.dataset_id, "to_frame": "image"})
        self.assertEqual(r.status_code, 200, r.text)
        circle, ellipse = r.json()["regions"]
        np.testing.assert_allclose(circle["center"], [101.0, 101.0], atol=1e-6)
        # CASA's mask of this ellipse on the galactic grid: long axis at 120°.
        b1_dir = (ellipse["angle_deg"] + 90.0) % 180.0
        self.assertAlmostEqual((b1_dir - 120.0 + 90.0) % 180.0 - 90.0, 0.0, places=3)

    def test_parse_without_dataset_is_j2000(self):
        r = self.client.post("/v1/regions/parse", headers=self.h, json={
            "text": "#CRTF\ncircle[[150deg, 2deg], 10arcsec]\n"})
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(r.json()["regions"][0]["frame"], "fk5")


if __name__ == "__main__":
    unittest.main()
