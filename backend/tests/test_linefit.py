"""Tests for POST /v1/spectral/linefit — 1-D Gaussian line fitting."""
from __future__ import annotations

import os
import unittest

import numpy as np

_TEST_TOKEN = "test-token-visivo-12345"


def _gaussian(x, amp, mu, sigma, base=0.0):
    return base + amp * np.exp(-((x - mu) ** 2) / (2.0 * sigma ** 2))


class TestLineFit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": _TEST_TOKEN}

    def _fit(self, values, axis, with_baseline=True):
        return self.client.post("/v1/spectral/linefit",
                                json={"values": list(values), "axis": list(axis),
                                      "with_baseline": with_baseline},
                                headers=self._auth)

    def test_recovers_emission_gaussian(self):
        x = np.linspace(-50, 50, 200)
        y = _gaussian(x, amp=10.0, mu=5.0, sigma=4.0, base=2.0)
        r = self._fit(y, x)
        self.assertEqual(r.status_code, 200, r.text)
        d = r.json()
        self.assertTrue(d["valid"])
        self.assertAlmostEqual(d["peak"], 10.0, delta=0.05)
        self.assertAlmostEqual(d["center"], 5.0, delta=0.05)
        self.assertAlmostEqual(d["sigma"], 4.0, delta=0.05)
        self.assertAlmostEqual(d["fwhm"], 2.35482 * 4.0, delta=0.1)
        self.assertAlmostEqual(d["continuum"], 2.0, delta=0.05)

    def test_integrated_flux(self):
        x = np.linspace(-50, 50, 400)
        y = _gaussian(x, amp=5.0, mu=0.0, sigma=3.0, base=0.0)
        d = self._fit(y, x, with_baseline=False).json()
        # ∫ amp·exp(...) = amp·σ·√(2π)
        self.assertAlmostEqual(d["integrated_flux"], 5.0 * 3.0 * np.sqrt(2 * np.pi), delta=0.5)

    def test_fit_uncertainties_present(self):
        """R-08: a noisy line fit must return finite, positive 1-sigma errors."""
        rng = np.random.default_rng(7)
        x = np.linspace(-50, 50, 300)
        y = _gaussian(x, amp=8.0, mu=2.0, sigma=5.0, base=1.0) + rng.normal(0, 0.3, x.size)
        d = self._fit(y, x).json()
        self.assertTrue(d["valid"])
        for k in ("peak_err", "center_err", "sigma_err", "fwhm_err",
                  "integrated_flux_err"):
            self.assertIn(k, d)
            self.assertIsNotNone(d[k], k)
            self.assertGreater(d[k], 0.0, k)

    def test_recovers_absorption_line(self):
        x = np.linspace(0, 100, 200)
        y = _gaussian(x, amp=-6.0, mu=40.0, sigma=5.0, base=10.0)
        d = self._fit(y, x).json()
        self.assertTrue(d["valid"])
        self.assertAlmostEqual(d["peak"], -6.0, delta=0.1)
        self.assertAlmostEqual(d["center"], 40.0, delta=0.1)

    def test_low_residual_on_clean_data(self):
        x = np.linspace(-20, 20, 100)
        y = _gaussian(x, amp=8.0, mu=1.0, sigma=2.5, base=1.0)
        d = self._fit(y, x).json()
        self.assertLess(d["rms"], 1e-3)

    def test_length_mismatch_422(self):
        r = self.client.post("/v1/spectral/linefit",
                             json={"values": [1, 2, 3], "axis": [0, 1]},
                             headers=self._auth)
        self.assertEqual(r.status_code, 422)

    def test_too_few_points_422(self):
        r = self._fit([1.0, 2.0], [0.0, 1.0])
        self.assertEqual(r.status_code, 422)


if __name__ == "__main__":
    unittest.main()
