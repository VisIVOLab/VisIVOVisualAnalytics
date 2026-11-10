"""Issue #129 — four fixes from the scientific review of the cosmology routes.

1. a non-finite quantity (the age of a de Sitter universe) is answered as null,
   not as an HTTP 500, and the rest of the answer is kept;
2. ``scale_kpc_per_arcsec`` names the proper transverse scale, with the old
   ``proper_motion_kpc_per_arcsec`` kept as a deprecated alias;
3. skeleton smoothing is normalised over the finite pixels: a blank area does
   not pull its neighbours down and draw a false edge;
4. ``persistence_threshold`` <= 0 is an intensity cut like any other.
"""
from __future__ import annotations

import base64
import json
import os
import tempfile
import unittest

import numpy as np

_TEST_TOKEN = "test-token-visivo-12345"

try:
    import skimage  # noqa: F401
    _HAS_SKIMAGE = True
except ImportError:  # pragma: no cover
    _HAS_SKIMAGE = False


def _client():
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient
    from app.main import app
    return TestClient(app, raise_server_exceptions=True), {"X-Visivo-Token": _TEST_TOKEN}


def _fits(data: np.ndarray) -> str:
    from astropy.io import fits
    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    tmp.close()
    fits.PrimaryHDU(data.astype(np.float32)).writeto(tmp.name, overwrite=True)
    return tmp.name


def _mask(result: dict) -> np.ndarray:
    raw = np.frombuffer(base64.b64decode(result["data_base64"]), dtype=np.float32)
    return raw.reshape(result["height"], result["width"]) > 0


class TestNonFiniteCosmologyAnswers(unittest.TestCase):
    """Point 1: inf/NaN never reaches the JSON encoder."""

    @classmethod
    def setUpClass(cls):
        cls.client, cls.auth = _client()

    def test_de_sitter_age_is_null_and_the_rest_is_kept(self):
        r = self.client.get("/v1/cosmology/distances",
                            params={"z": 1.0, "H0": 70.0, "Om0": 0.0, "Ode0": 1.0},
                            headers=self.auth)
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        self.assertIsNone(d["age_Gyr"])
        # De Sitter in closed form: D_C = (c/H0) z, t_lb = ln(1+z)/H0.
        hubble_mpc = 299792.458 / 70.0
        self.assertAlmostEqual(d["comoving_Mpc"], hubble_mpc * 1.0, places=3)
        self.assertAlmostEqual(d["luminosity_Mpc"], 2.0 * hubble_mpc, places=3)
        self.assertAlmostEqual(d["angular_diameter_Mpc"], 0.5 * hubble_mpc, places=3)
        hubble_gyr = 977.792221 / 70.0
        self.assertAlmostEqual(d["lookback_time_Gyr"], hubble_gyr * np.log(2.0), places=3)

    def test_de_sitter_at_z0_is_null_age_too(self):
        r = self.client.get("/v1/cosmology/distances",
                            params={"z": 0.0, "Om0": 0.0, "Ode0": 1.0}, headers=self.auth)
        self.assertEqual(r.status_code, 200, r.text)
        self.assertIsNone(r.json()["age_Gyr"])
        self.assertEqual(r.json()["comoving_Mpc"], 0.0)

    def test_finite_age_stays_a_number(self):
        r = self.client.get("/v1/cosmology/distances",
                            params={"z": 1.0, "Om0": 0.3, "Ode0": 0.7}, headers=self.auth)
        self.assertIsInstance(r.json()["age_Gyr"], float)

    def test_point_distance_refuses_a_non_finite_redshift(self):
        # NaN and Infinity are literals Python's JSON parser accepts in a body.
        for literal in ("NaN", "Infinity"):
            r = self.client.post("/v1/cosmology/distance",
                                 content='{"z": %s, "model": "Planck18"}' % literal,
                                 headers={**self.auth, "Content-Type": "application/json"})
            self.assertEqual(r.status_code, 200, f"{literal}: {r.text}")
            self.assertFalse(r.json()["valid"])

    def test_redshift_above_the_cap_is_refused_not_zero(self):
        r = self.client.post("/v1/cosmology/distance", json={"z": 1e308, "model": "Planck18"},
                             headers=self.auth)
        self.assertEqual(r.status_code, 200, r.text)
        self.assertFalse(r.json()["valid"])
        r = self.client.post("/v1/cosmology/distance", json={"z": 1100.0, "model": "Planck18"},
                             headers=self.auth)
        self.assertTrue(r.json()["valid"])
        self.assertGreater(r.json()["comoving_Mpc"], 10_000.0)
        r = self.client.post("/v1/cosmology/distance/batch",
                             json={"redshifts": [0.5, 1e308, 2e4], "model": "Planck18"},
                             headers=self.auth)
        body = r.json()
        self.assertTrue(body["valid"])
        self.assertGreater(body["distances_Mpc"][0], 0.0)
        self.assertIsNone(body["distances_Mpc"][1])
        self.assertIsNone(body["distances_Mpc"][2])

    def test_finite_or_none(self):
        from app.routers.cosmology import _finite_or_none
        self.assertIsNone(_finite_or_none(float("inf")))
        self.assertIsNone(_finite_or_none(np.float64("nan")))
        self.assertIsNone(_finite_or_none(None))
        self.assertEqual(_finite_or_none(np.float32(2.5)), 2.5)
        json.dumps([_finite_or_none(v) for v in (np.inf, -np.inf, 1.0)], allow_nan=False)


class TestScaleFieldName(unittest.TestCase):
    """Point 2: the scale has its own name; the old one is a deprecated alias."""

    @classmethod
    def setUpClass(cls):
        cls.client, cls.auth = _client()

    def test_new_and_deprecated_fields_carry_the_same_scale(self):
        r = self.client.post("/v1/cosmology/distance", json={"z": 1.0, "model": "Planck18"},
                             headers=self.auth)
        d = r.json()
        from astropy.cosmology import Planck18
        expected = Planck18.kpc_proper_per_arcmin(1.0).value / 60.0
        self.assertAlmostEqual(d["scale_kpc_per_arcsec"], expected, places=9)
        self.assertEqual(d["proper_motion_kpc_per_arcsec"], d["scale_kpc_per_arcsec"])

    def test_openapi_marks_the_old_name_deprecated(self):
        from app.main import app
        schema = app.openapi()
        props = schema["components"]["schemas"]["CosmologyDistanceResponse"]["properties"]
        self.assertIn("scale_kpc_per_arcsec", props)
        self.assertTrue(props["proper_motion_kpc_per_arcsec"].get("deprecated"))


@unittest.skipUnless(_HAS_SKIMAGE, "scikit-image not installed")
class TestSkeletonSmoothingHasNoFalseEdge(unittest.TestCase):
    """Point 3: a flat map next to a blank area stays flat after smoothing.

    With blanks filled by 0, a sigma = 3 smoothing dragged the band next to the
    blank strip below 4.9 and `void` mode drew a void along that edge. With
    normalised convolution the map is 5.0 everywhere it was observed, so there
    is no void at all.
    """

    def test_no_void_along_the_blank_edge(self):
        from app.compute import worker_skeleton
        data = np.full((80, 80), 5.0, dtype=np.float32)
        data[:, 50:] = np.nan
        path = _fits(data)
        try:
            res = worker_skeleton(path, 0, 4.9, "void", 3.0, None)
            self.assertEqual(res["n_skeleton_pixels"], 0)
        finally:
            os.unlink(path)


@unittest.skipUnless(_HAS_SKIMAGE, "scikit-image not installed")
class TestIntensityCutHonoursZeroAndNegative(unittest.TestCase):
    """Point 4: 0 and negative cuts are applied, not silently dropped."""

    @staticmethod
    def _bar_map() -> np.ndarray:
        # A bar whose left half is negative and right half positive, on a
        # strongly negative background; threshold -5 binarises the whole bar.
        data = np.full((48, 64), -10.0, dtype=np.float32)
        data[22:27, 4:32] = -1.0
        data[22:27, 32:60] = 2.0
        return data

    def setUp(self):
        self.path = _fits(self._bar_map())

    def tearDown(self):
        os.unlink(self.path)

    def _run(self, cut):
        from app.compute import worker_skeleton
        return worker_skeleton(self.path, 0, -5.0, "skeleton", 0.0, cut)

    def test_zero_cut_keeps_only_the_positive_half(self):
        uncut = _mask(self._run(None))
        cut = _mask(self._run(0.0))
        self.assertGreater(uncut[:, :32].sum(), 0)
        self.assertEqual(int(cut[:, :32].sum()), 0, "branch below 0 survived a cut at 0")
        self.assertGreater(cut[:, 32:].sum(), 0)
        self.assertEqual(self._run(0.0)["persistence_threshold"], 0.0)

    def test_negative_cut_is_a_cut(self):
        data = self._bar_map()
        # Below every pixel: nothing dropped. Between the halves' levels: the
        # -1 half survives a -2 cut. Above the bar: everything goes.
        self.assertEqual(self._run(-20.0)["n_skeleton_pixels"], self._run(None)["n_skeleton_pixels"])
        self.assertEqual(self._run(-2.0)["n_skeleton_pixels"], self._run(None)["n_skeleton_pixels"])
        self.assertEqual(self._run(float(data.max()) + 1.0)["n_skeleton_pixels"], 0)


class TestSkeletonRequestValidation(unittest.TestCase):
    """Point 4 at the route: the cut reaches the worker; finite values only."""

    @classmethod
    def setUpClass(cls):
        cls.client, cls.auth = _client()
        cls.path = _fits(TestIntensityCutHonoursZeroAndNegative._bar_map())
        r = cls.client.post("/v1/datasets/open", json={"path": cls.path}, headers=cls.auth)
        body = r.json()
        cls.dataset_id = body["dataset_id"]
        cls.headers = dict(cls.auth)
        if body.get("session_id"):
            cls.headers["X-Visivo-Session"] = body["session_id"]

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.path)

    def _post(self, body: dict | str):
        url = f"/v1/analysis/skeleton/{self.dataset_id}"
        if isinstance(body, str):
            return self.client.post(url, content=body,
                                    headers={**self.headers, "Content-Type": "application/json"})
        return self.client.post(url, json=body, headers=self.headers)

    @unittest.skipUnless(_HAS_SKIMAGE, "scikit-image not installed")
    def test_zero_cut_reaches_the_worker_through_the_route(self):
        uncut = self._post({"threshold": -5.0}).json()
        cut = self._post({"threshold": -5.0, "persistence_threshold": 0.0}).json()
        self.assertIsNone(uncut["persistence_threshold"])
        self.assertEqual(cut["persistence_threshold"], 0.0)
        self.assertLess(cut["n_skeleton_pixels"], uncut["n_skeleton_pixels"])

    def test_non_finite_numbers_are_refused(self):
        for field in ("threshold", "smooth_sigma_pix", "persistence_threshold"):
            for literal in ("NaN", "Infinity", "-Infinity"):
                r = self._post('{"%s": %s}' % (field, literal))
                self.assertEqual(r.status_code, 422, f"{field}={literal}: {r.text}")


if __name__ == "__main__":
    unittest.main()
