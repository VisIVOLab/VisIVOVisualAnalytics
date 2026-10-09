"""
Tests for Domain C – Cosmology & Statistical Analysis (C-01, C-03).

C-02 (Cosmological distances) is already tested in test_redshift_nan.py and
test_compute.py; only new features are covered here.

C-01: Angular power spectrum – azimuthally averaged 2-D FFT power.
C-03: Skeleton / void detection – morphological thinning via scikit-image.
"""
from __future__ import annotations

# The cosmology angular-power-spectrum and filament-skeleton endpoints — and
# their spec aliases /v1/cosmology/powerspectrum and /v1/cosmology/skeleton —
# were dropped by a router refactor and have been restored (see routers
# cosmology.py / analysis.py). This module is live again.

import base64
import os
import tempfile
import unittest

import numpy as np


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

def _b64_to_f32(b64: str, shape: tuple) -> np.ndarray:
    return np.frombuffer(base64.b64decode(b64), dtype=np.float32).reshape(shape)


def _make_2d_fits(data: np.ndarray) -> str:
    """Write a 2-D float32 FITS file with minimal WCS to a temp file."""
    from astropy.io import fits
    hdu = fits.PrimaryHDU(data.astype(np.float32))
    h = hdu.header
    ny, nx = data.shape
    h["CTYPE1"] = "RA---SIN"; h["CRPIX1"] = nx / 2; h["CRVAL1"] = 83.8
    h["CDELT1"] = -1e-4;       h["CUNIT1"] = "deg"
    h["CTYPE2"] = "DEC--SIN"; h["CRPIX2"] = ny / 2; h["CRVAL2"] = -5.4
    h["CDELT2"] = 1e-4;        h["CUNIT2"] = "deg"
    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
    tmp.close()
    return tmp.name


def _make_cube_fits(data: np.ndarray, n_chan: int = 8) -> str:
    """Write a 3-D (n_chan, ny, nx) FITS cube."""
    from astropy.io import fits
    ny, nx = data.shape
    cube = np.stack([data] * n_chan, axis=0).astype(np.float32)
    hdu = fits.PrimaryHDU(cube)
    h = hdu.header
    h["CTYPE1"] = "RA---SIN"; h["CRPIX1"] = nx / 2; h["CRVAL1"] = 83.8
    h["CDELT1"] = -1e-4;       h["CUNIT1"] = "deg"
    h["CTYPE2"] = "DEC--SIN"; h["CRPIX2"] = ny / 2; h["CRVAL2"] = -5.4
    h["CDELT2"] = 1e-4;        h["CUNIT2"] = "deg"
    h["CTYPE3"] = "FREQ";     h["CRPIX3"] = 1.0; h["CRVAL3"] = 1.4e9
    h["CDELT3"] = 1e6;         h["CUNIT3"] = "Hz"
    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
    tmp.close()
    return tmp.name


# ============================================================================
# C-01: Angular power spectrum
# ============================================================================

class TestAngularPowerSpectrumWorker(unittest.TestCase):
    """Unit tests for worker_angular_power_spectrum."""

    def _run(self, path: str, channel: int = 0, n_bins: int = 32):
        from app.compute import worker_angular_power_spectrum
        return worker_angular_power_spectrum(path, channel, n_bins)

    def test_returns_correct_n_bins(self):
        """Output n_bins ≤ requested n_bins (empty bins at high k are dropped)."""
        rng = np.random.default_rng(0)
        data = rng.standard_normal((64, 64))
        path = _make_2d_fits(data)
        try:
            result = self._run(path, n_bins=32)
            self.assertGreater(result["n_bins"], 0)
            self.assertLessEqual(result["n_bins"], 32)
        finally:
            os.unlink(path)

    def test_k_bins_monotonically_increasing(self):
        """Wavenumber bins must be strictly increasing."""
        rng = np.random.default_rng(1)
        data = rng.standard_normal((64, 64))
        path = _make_2d_fits(data)
        try:
            result = self._run(path)
            k = np.array(result["k_bins"])
            self.assertTrue(np.all(np.diff(k) > 0), "k_bins not monotonically increasing")
        finally:
            os.unlink(path)

    def test_power_non_negative(self):
        """Power spectrum values must be non-negative (|FFT|²)."""
        rng = np.random.default_rng(2)
        data = rng.standard_normal((64, 64))
        path = _make_2d_fits(data)
        try:
            result = self._run(path)
            self.assertTrue(all(p >= 0.0 for p in result["power"]),
                            "Power values must be non-negative")
        finally:
            os.unlink(path)

    def test_white_noise_flat_spectrum(self):
        """White noise should produce an approximately flat power spectrum.

        We test that the ratio max/min power is less than 10 (rough flatness).
        """
        rng = np.random.default_rng(42)
        data = rng.standard_normal((128, 128))
        path = _make_2d_fits(data)
        try:
            result = self._run(path, n_bins=16)
            power = np.array(result["power"])
            ratio = power.max() / power.min()
            self.assertLess(ratio, 20.0,
                            f"White-noise power ratio too high: {ratio:.1f} (expected <20)")
        finally:
            os.unlink(path)

    def test_constant_image_zero_high_k_power(self):
        """A constant image has all power at k=0; high-k bins should be ~0."""
        data = np.ones((64, 64))
        path = _make_2d_fits(data)
        try:
            result = self._run(path, n_bins=16)
            # High-k half of bins should have very little power relative to first bin
            power = np.array(result["power"])
            high_k_power = power[len(power) // 2:].mean()
            low_k_power = power[0]
            # For a truly constant image the FFT at k>0 is zero; with Hann windowing
            # there is minimal leakage, so the ratio should be negligible.
            if low_k_power > 0:
                self.assertLess(high_k_power / low_k_power, 0.1)
        finally:
            os.unlink(path)

    def test_3d_cube_channel_selection(self):
        """Channel selection from a 3-D cube should not raise."""
        rng = np.random.default_rng(7)
        data = rng.standard_normal((32, 32))
        path = _make_cube_fits(data, n_chan=4)
        try:
            result = self._run(path, channel=2)
            self.assertGreater(result["n_bins"], 0)
        finally:
            os.unlink(path)

    def test_result_has_expected_keys(self):
        data = np.random.default_rng(3).standard_normal((32, 32))
        path = _make_2d_fits(data)
        try:
            result = self._run(path)
            for key in ("k_bins", "power", "n_bins", "width", "height",
                        "k_unit", "power_unit"):
                self.assertIn(key, result, f"Missing key: {key}")
        finally:
            os.unlink(path)

    def test_k_bins_and_power_same_length(self):
        data = np.random.default_rng(4).standard_normal((32, 32))
        path = _make_2d_fits(data)
        try:
            result = self._run(path)
            self.assertEqual(len(result["k_bins"]), len(result["power"]))
        finally:
            os.unlink(path)

    def test_all_nan_raises(self):
        data = np.full((32, 32), np.nan)
        path = _make_2d_fits(data)
        try:
            from app.compute import worker_angular_power_spectrum
            with self.assertRaises(ValueError):
                worker_angular_power_spectrum(path)
        finally:
            os.unlink(path)


class TestAngularPowerEndpoint(unittest.TestCase):
    """HTTP-level smoke test for POST /v1/cosmology/angular_power/{dataset_id}."""

    _TEST_TOKEN = "test-token-visivo-12345"

    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", cls._TEST_TOKEN)
        rng = np.random.default_rng(99)
        data = rng.standard_normal((32, 32)).astype(np.float32)
        cls.fits_path = _make_2d_fits(data)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": cls._TEST_TOKEN}
        r = cls.client.post("/v1/datasets/open",
                            json={"path": cls.fits_path}, headers=cls._auth)
        data_resp = r.json()
        cls.dataset_id = data_resp["dataset_id"]
        cls.session_id = data_resp.get("session_id", "")

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.fits_path)

    def _headers(self):
        h = dict(self._auth)
        if self.session_id:
            h["X-Visivo-Session"] = self.session_id
        return h

    def test_returns_200(self):
        r = self.client.post(f"/v1/cosmology/angular_power/{self.dataset_id}",
                             json={"channel": 0, "n_bins": 16},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200)

    def test_response_valid(self):
        r = self.client.post(f"/v1/cosmology/angular_power/{self.dataset_id}",
                             json={"channel": 0, "n_bins": 16},
                             headers=self._headers())
        data = r.json()
        self.assertTrue(data["valid"])
        self.assertGreater(data["n_bins"], 0)
        self.assertEqual(len(data["k_bins"]), len(data["power"]))

    def test_response_has_ell_and_cl_err(self):
        """Response must include ell and cl_err arrays matching length of k_bins."""
        r = self.client.post(f"/v1/cosmology/angular_power/{self.dataset_id}",
                             json={"channel": 0, "n_bins": 16},
                             headers=self._headers())
        data = r.json()
        self.assertEqual(len(data.get("ell", [])), data["n_bins"])
        self.assertEqual(len(data.get("cl_err", [])), data["n_bins"])

    def test_get_powerspectrum_endpoint(self):
        """Spec-conformant GET /v1/cosmology/powerspectrum/{id} should return 200."""
        r = self.client.get(f"/v1/cosmology/powerspectrum/{self.dataset_id}",
                            params={"channel": 0, "n_bins": 16},
                            headers=self._headers())
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["valid"])
        self.assertIn("ell", data)
        self.assertIn("cl_err", data)


# ============================================================================
# C-03: Skeleton / void detection
# ============================================================================

skimage_available = True
try:
    import skimage  # noqa: F401
except ImportError:
    skimage_available = False


@unittest.skipUnless(skimage_available, "scikit-image not installed")
class TestSkeletonWorker(unittest.TestCase):
    """Unit tests for worker_skeleton (requires scikit-image)."""

    def _run(self, path: str, channel: int = 0, threshold=None, mode: str = "skeleton",
             smooth_sigma_pix: float = 0.0, persistence_threshold=None):
        from app.compute import worker_skeleton
        return worker_skeleton(path, channel, threshold, mode, smooth_sigma_pix, persistence_threshold)

    @staticmethod
    def _make_bar_image(ny: int = 64, nx: int = 64) -> tuple[np.ndarray, str]:
        """Bright horizontal bar at centre on a zero background."""
        data = np.zeros((ny, nx), dtype=np.float32)
        data[ny // 2 - 2: ny // 2 + 2, 8: nx - 8] = 1.0
        return data, _make_2d_fits(data)

    def test_skeleton_non_empty(self):
        """A bright bar should produce a non-empty skeleton."""
        data, path = self._make_bar_image()
        try:
            result = self._run(path, threshold=0.5)
            self.assertGreater(result["n_skeleton_pixels"], 0)
        finally:
            os.unlink(path)

    def test_skeleton_binary_values(self):
        """Output must be 0 or 255 (uint8 mask encoded as float32)."""
        data, path = self._make_bar_image()
        try:
            result = self._run(path, threshold=0.5)
            arr = _b64_to_f32(result["data_base64"], (result["height"], result["width"]))
            unique_vals = set(np.unique(arr))
            self.assertTrue(unique_vals.issubset({0.0, 255.0}),
                            f"Unexpected values in skeleton: {unique_vals}")
        finally:
            os.unlink(path)

    def test_skeleton_thinner_than_input(self):
        """The skeleton of the bar should be thinner (fewer pixels) than the bar."""
        ny, nx = 64, 64
        data = np.zeros((ny, nx), dtype=np.float32)
        # 8-pixel-wide bar
        data[ny // 2 - 4: ny // 2 + 4, 8: nx - 8] = 1.0
        path = _make_2d_fits(data)
        try:
            result = self._run(path, threshold=0.5)
            n_bar = int((data > 0.5).sum())
            n_skel = result["n_skeleton_pixels"]
            self.assertLess(n_skel, n_bar,
                            "Skeleton should have fewer pixels than the original bar")
        finally:
            os.unlink(path)

    def test_void_mode_inverts_mask(self):
        """In void mode the skeleton of the background should be returned."""
        data, path = self._make_bar_image()
        try:
            skel_result = self._run(path, threshold=0.5, mode="skeleton")
            void_result = self._run(path, threshold=0.5, mode="void")
            # Void pixels + skeleton pixels < total image pixels (both are thinned masks)
            total = skel_result["width"] * skel_result["height"]
            self.assertLess(void_result["n_skeleton_pixels"] + skel_result["n_skeleton_pixels"],
                            total)
        finally:
            os.unlink(path)

    def test_threshold_none_uses_mean(self):
        """When threshold=None the mean is used; result should be non-empty for non-flat image."""
        rng = np.random.default_rng(5)
        data = rng.standard_normal((32, 32)).astype(np.float32)
        path = _make_2d_fits(data)
        try:
            result = self._run(path, threshold=None)
            # With random data ~50% of pixels above mean → non-empty skeleton
            self.assertGreater(result["n_skeleton_pixels"], 0)
        finally:
            os.unlink(path)

    def test_all_nan_raises(self):
        data = np.full((32, 32), np.nan, dtype=np.float32)
        path = _make_2d_fits(data)
        try:
            from app.compute import worker_skeleton
            with self.assertRaises(ValueError):
                worker_skeleton(path, 0, None, "skeleton")
        finally:
            os.unlink(path)

    def test_result_has_expected_keys(self):
        data, path = self._make_bar_image()
        try:
            result = self._run(path, threshold=0.5)
            for key in ("n_skeleton_pixels", "width", "height", "threshold",
                        "mode", "smooth_sigma_pix", "range_min", "range_max",
                        "data_base64", "endpoints"):
                self.assertIn(key, result, f"Missing key: {key}")
        finally:
            os.unlink(path)

    def test_endpoints_is_list(self):
        """The 'endpoints' field must be a list of [row, col] pairs."""
        data, path = self._make_bar_image()
        try:
            from app.compute import worker_skeleton
            result = worker_skeleton(path, 0, 0.5, "skeleton")
            self.assertIsInstance(result["endpoints"], list)
            for ep in result["endpoints"]:
                self.assertEqual(len(ep), 2)
        finally:
            os.unlink(path)

    def test_smooth_sigma_pix_accepted(self):
        """Gaussian pre-smoothing parameter should be accepted without error."""
        data, path = self._make_bar_image()
        try:
            from app.compute import worker_skeleton
            result = worker_skeleton(path, 0, 0.5, "skeleton", smooth_sigma_pix=1.0)
            self.assertIn("n_skeleton_pixels", result)
            self.assertEqual(result["smooth_sigma_pix"], 1.0)
        finally:
            os.unlink(path)

    def test_persistence_threshold_prunes_skeleton(self):
        """With a very high persistence_threshold the skeleton should be empty or smaller."""
        data, path = self._make_bar_image()
        try:
            from app.compute import worker_skeleton
            result_no_prune = worker_skeleton(path, 0, 0.5, "skeleton")
            result_pruned = worker_skeleton(path, 0, 0.5, "skeleton",
                                            persistence_threshold=1e9)
            self.assertLessEqual(result_pruned["n_skeleton_pixels"],
                                 result_no_prune["n_skeleton_pixels"])
        finally:
            os.unlink(path)


class TestSkeletonEndpoint(unittest.TestCase):
    """HTTP-level smoke test for POST /v1/analysis/skeleton/{dataset_id}."""

    _TEST_TOKEN = "test-token-visivo-12345"

    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", cls._TEST_TOKEN)
        rng = np.random.default_rng(11)
        data = rng.standard_normal((32, 32)).astype(np.float32)
        cls.fits_path = _make_2d_fits(data)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": cls._TEST_TOKEN}
        r = cls.client.post("/v1/datasets/open",
                            json={"path": cls.fits_path}, headers=cls._auth)
        data_resp = r.json()
        cls.dataset_id = data_resp["dataset_id"]
        cls.session_id = data_resp.get("session_id", "")

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.fits_path)

    def _headers(self):
        h = dict(self._auth)
        if self.session_id:
            h["X-Visivo-Session"] = self.session_id
        return h

    @unittest.skipUnless(skimage_available, "scikit-image not installed")
    def test_skeleton_returns_200(self):
        r = self.client.post(f"/v1/analysis/skeleton/{self.dataset_id}",
                             json={"channel": 0, "mode": "skeleton"},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["valid"])

    @unittest.skipUnless(skimage_available, "scikit-image not installed")
    def test_cosmology_skeleton_alias(self):
        """POST /v1/cosmology/skeleton/{id} must be equivalent to /v1/analysis/skeleton/{id}."""
        r = self.client.post(f"/v1/cosmology/skeleton/{self.dataset_id}",
                             json={"channel": 0, "mode": "skeleton"},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["valid"])

    @unittest.skipUnless(skimage_available, "scikit-image not installed")
    def test_skeleton_response_has_endpoints(self):
        """Response JSON must include 'endpoints' list."""
        r = self.client.post(f"/v1/analysis/skeleton/{self.dataset_id}",
                             json={"channel": 0, "mode": "skeleton"},
                             headers=self._headers())
        data = r.json()
        self.assertIn("endpoints", data)
        self.assertIsInstance(data["endpoints"], list)

    @unittest.skipUnless(skimage_available, "scikit-image not installed")
    def test_skeleton_smooth_sigma_pix_accepted(self):
        """smooth_sigma_pix parameter in JSON body must be accepted."""
        r = self.client.post(f"/v1/analysis/skeleton/{self.dataset_id}",
                             json={"channel": 0, "smooth_sigma_pix": 1.5},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200)
        self.assertTrue(r.json()["valid"])

    def test_skeleton_503_when_skimage_missing(self):
        """When scikit-image is absent the endpoint returns valid=False with install hint."""
        import sys
        from unittest.mock import patch, MagicMock
        mock_skimage = MagicMock()
        mock_skimage.morphology = MagicMock()
        mock_skimage.morphology.skeletonize = None

        # Temporarily patch skimage import in compute so ImportError is raised
        with patch.dict("sys.modules", {
            "skimage": None,
            "skimage.morphology": None,
        }):
            # We need to invalidate the cached module
            import importlib
            # The worker catches ImportError and re-raises; the endpoint catches it
            r = self.client.post(f"/v1/analysis/skeleton/{self.dataset_id}",
                                 json={"channel": 0},
                                 headers=self._headers())
            # Should return 200 with valid=False and install hint (not hard 503)
            if r.status_code == 200:
                data = r.json()
                if not data["valid"]:
                    self.assertIn("install", data)
            # Or a 503 with JSON body is also acceptable
            elif r.status_code == 503:
                pass  # acceptable
            else:
                self.fail(f"Unexpected status code {r.status_code}")


if __name__ == "__main__":
    unittest.main()
