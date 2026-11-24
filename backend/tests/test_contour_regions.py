"""Contour lines as ds9 regions (issues #49, #519).

The desktop client turns each contour line into a canonical image-frame region
(a closed line is a polygon, an open one a polyline, 1-based pixels, with the
colour, width, dash and level as properties) and has this module write it, in
a sky frame through the dataset's WCS. The file has to read back as it was
written and, moved to the pixel frame, land on the pixels it came from.
"""
from __future__ import annotations

import unittest

from astropy.wcs import WCS

from app import regions as R


def _polygon():
    return {"shape": "polygon", "frame": "image", "unit": "pix",
            "points": [[10.0, 10.0], [20.5, 10.0], [20.5, 22.25], [10.0, 22.25]],
            "props": {"color": "#ff6633", "width": 1.5, "dash": True,
                      "tag": ["level=-0.0125"]}}


def _polyline():
    return {"shape": "line", "frame": "image", "unit": "pix",
            "points": [[1.0, 1.0], [2.0, 3.0], [4.0, 3.5]],
            "props": {"color": "#00ff00", "width": 1.5, "tag": ["level=2"]}}


def _wcs():
    w = WCS(naxis=2)
    w.wcs.ctype = ["GLON-CAR", "GLAT-CAR"]
    w.wcs.crpix = [50, 50]
    w.wcs.cdelt = [-0.001, 0.001]
    w.wcs.crval = [10.0, 0.5]
    return w


class TestContourRegions(unittest.TestCase):
    def test_style_and_level_survive_a_ds9_round_trip(self):
        text = R.export_regions([_polygon(), _polyline()], "ds9")
        back = R.parse_regions(text, "ds9")
        self.assertEqual([r["shape"] for r in back], ["polygon", "line"])
        props = back[0]["props"]
        self.assertEqual(props["color"], "#ff6633")
        self.assertEqual(props["width"], 1.5)
        self.assertTrue(props["dash"])
        self.assertEqual(props["tag"], ["level=-0.0125"])
        # An open contour line is a ds9 segment, with all its vertices.
        self.assertIn("segment(", text)
        self.assertEqual(len(back[1]["points"]), 3)
        self.assertFalse(back[1]["props"].get("dash", False))

    def test_sky_frames_and_back_to_the_same_pixels(self):
        wcs = _wcs()
        for frame in ("galactic", "fk5"):
            sky = R.convert_regions([_polygon(), _polyline()], to_frame=frame, wcs=wcs, origin=1)
            text = R.export_regions(sky, "ds9")
            self.assertIn(frame + "\n", text)
            # The file's vertices are the contour's in degrees, with its style.
            back = R.parse_regions(text, "ds9")
            self.assertEqual(back[0]["frame"], frame)
            self.assertEqual(back[0]["props"]["color"], "#ff6633")
            pix = R.convert_regions(back, to_frame="image", wcs=wcs, origin=1)
            for want, got in zip(_polygon()["points"], pix[0]["points"]):
                self.assertAlmostEqual(want[0], got[0], places=5)
                self.assertAlmostEqual(want[1], got[1], places=5)
            for want, got in zip(_polyline()["points"], pix[1]["points"]):
                self.assertAlmostEqual(want[0], got[0], places=5)
                self.assertAlmostEqual(want[1], got[1], places=5)


if __name__ == "__main__":
    unittest.main()
