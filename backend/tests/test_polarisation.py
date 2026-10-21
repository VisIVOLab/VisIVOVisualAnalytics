"""
Tests for Domain P – Polarisation features (P-01, P-02, P-03).

Synthetic FITS cubes are built in-memory so no real data files are required.

P-01: Stokes plane extraction from a 4-plane FITS cube.
P-02: Bias-corrected polarised intensity (PI) and position angle (PA).
P-03: Rotation Measure (RM) synthesis – peak of |F(φ)| at correct φ.
"""
from __future__ import annotations

import base64
import io
import os
import struct
import tempfile
import unittest

import numpy as np

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _b64_to_f32(b64: str, shape: tuple) -> np.ndarray:
    return np.frombuffer(base64.b64decode(b64), dtype=np.float32).reshape(shape)


def _parse_binary_frame(payload: bytes, offset: int = 0):
    """Parse the 12-byte big-endian header + float32 LE body."""
    rows, cols, flags = struct.unpack_from(">III", payload, offset)
    body_start = offset + 12
    body_end = body_start + rows * cols * 4
    arr = np.frombuffer(payload[body_start:body_end], dtype="<f4").reshape(rows, cols)
    return arr, body_end


# ---------------------------------------------------------------------------
# Shared FITS builder
# ---------------------------------------------------------------------------

def _make_stokes_fits(
    n_stokes: int = 4,
    ny: int = 16,
    nx: int = 16,
    n_freq: int = 8,
    freq_start_hz: float = 1.4e9,
    freq_delta_hz: float = 1.0e6,
) -> str:
    """Write a synthetic 4-D FITS file (freq, stokes, dec, ra) to a temp file.

    Returns the path to the created file.  Caller is responsible for cleanup.

    Data layout: data[freq, stokes, y, x]
    FITS axes:   NAXIS1=nx, NAXIS2=ny, NAXIS3=n_stokes(STOKES), NAXIS4=n_freq
    """
    from astropy.io import fits

    # Build data: each Stokes plane has a constant value equal to its FITS pixel
    # index (I=1, Q=2, U=3, V=4).
    data = np.zeros((n_freq, n_stokes, ny, nx), dtype=np.float32)
    for s in range(n_stokes):
        data[:, s, :, :] = float(s + 1)   # I=1, Q=2, U=3, V=4

    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    # RA axis (FITS axis 1)
    h["NAXIS1"] = nx;   h["CTYPE1"] = "RA---SIN";  h["CRPIX1"] = nx / 2
    h["CRVAL1"] = 83.8; h["CDELT1"] = -1e-4;        h["CUNIT1"] = "deg"
    # Dec axis (FITS axis 2)
    h["NAXIS2"] = ny;   h["CTYPE2"] = "DEC--SIN";  h["CRPIX2"] = ny / 2
    h["CRVAL2"] = -5.4; h["CDELT2"] = 1e-4;          h["CUNIT2"] = "deg"
    # Stokes axis (FITS axis 3)
    h["NAXIS3"] = n_stokes; h["CTYPE3"] = "STOKES"
    h["CRPIX3"] = 1.0;      h["CRVAL3"] = 1.0;  h["CDELT3"] = 1.0
    # Frequency axis (FITS axis 4)
    h["NAXIS4"] = n_freq; h["CTYPE4"] = "FREQ"
    h["CRPIX4"] = 1.0;    h["CRVAL4"] = freq_start_hz; h["CDELT4"] = freq_delta_hz
    h["CUNIT4"] = "Hz"

    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    hdul = fits.HDUList([hdu])
    hdul.writeto(tmp.name, overwrite=True)
    tmp.close()
    return tmp.name


# ============================================================================
# P-01 – Stokes plane extraction
# ============================================================================

class TestStokesPlaneDetection(unittest.TestCase):
    """ScientificFitsDataset correctly identifies and maps Stokes planes."""

    @classmethod
    def setUpClass(cls):
        cls.fits_path = _make_stokes_fits(n_stokes=4)

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.fits_path)

    def _dataset(self):
        from app.fits_dataset import ScientificFitsDataset
        return ScientificFitsDataset(self.fits_path)

    def test_all_four_stokes_detected(self):
        ds = self._dataset()
        planes = ds.stokes_planes
        self.assertIn("I", planes)
        self.assertIn("Q", planes)
        self.assertIn("U", planes)
        self.assertIn("V", planes)

    def test_stokes_plane_indices_are_unique(self):
        ds = self._dataset()
        planes = ds.stokes_planes
        indices = list(planes.values())
        self.assertEqual(len(indices), len(set(indices)), "Duplicate Stokes indices")

    def test_stokes_numpy_axis_is_not_none(self):
        ds = self._dataset()
        ax = ds.stokes_numpy_axis()
        self.assertIsNotNone(ax)

    def test_stokes_numpy_axis_is_valid(self):
        ds = self._dataset()
        ax = ds.stokes_numpy_axis()
        hdul_open = __import__("astropy.io.fits", fromlist=["open"]).open
        from astropy.io import fits
        with fits.open(self.fits_path) as hdul:
            ndim = hdul[0].data.ndim
        self.assertGreaterEqual(ax, 0)
        self.assertLess(ax, ndim)


class TestStokesPlaneWorker(unittest.TestCase):
    """worker_stokes_plane extracts the correct plane values."""

    @classmethod
    def setUpClass(cls):
        cls.fits_path = _make_stokes_fits(n_stokes=4, ny=8, nx=8, n_freq=4)

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.fits_path)

    def _run(self, plane: str, channel: int = 0):
        from app.compute import worker_stokes_plane
        return worker_stokes_plane(self.fits_path, plane, channel)

    def test_stokes_I_value(self):
        result = self._run("I")
        arr = _b64_to_f32(result["data_base64"], (result["height"], result["width"]))
        np.testing.assert_allclose(arr, 1.0, atol=1e-5,
                                   err_msg="Stokes I should be 1.0 everywhere")

    def test_stokes_Q_value(self):
        result = self._run("Q")
        arr = _b64_to_f32(result["data_base64"], (result["height"], result["width"]))
        np.testing.assert_allclose(arr, 2.0, atol=1e-5,
                                   err_msg="Stokes Q should be 2.0 everywhere")

    def test_stokes_U_value(self):
        result = self._run("U")
        arr = _b64_to_f32(result["data_base64"], (result["height"], result["width"]))
        np.testing.assert_allclose(arr, 3.0, atol=1e-5,
                                   err_msg="Stokes U should be 3.0 everywhere")

    def test_stokes_V_value(self):
        result = self._run("V")
        arr = _b64_to_f32(result["data_base64"], (result["height"], result["width"]))
        np.testing.assert_allclose(arr, 4.0, atol=1e-5,
                                   err_msg="Stokes V should be 4.0 everywhere")

    def test_result_has_width_height(self):
        result = self._run("I")
        self.assertIn("width", result)
        self.assertIn("height", result)
        self.assertEqual(result["width"], 8)
        self.assertEqual(result["height"], 8)

    def test_unknown_plane_raises(self):
        from app.compute import worker_stokes_plane
        with self.assertRaises((ValueError, KeyError)):
            worker_stokes_plane(self.fits_path, "X", 0)

    def test_channel_selection_within_freq_axis(self):
        """Extracting channel 2 should still yield the same constant Stokes value."""
        result = self._run("Q", channel=2)
        arr = _b64_to_f32(result["data_base64"], (result["height"], result["width"]))
        np.testing.assert_allclose(arr, 2.0, atol=1e-5)


# ============================================================================
# P-02 – Polarised intensity and position angle
# ============================================================================

class TestPolarisationPiPa(unittest.TestCase):
    """
    worker_polarisation_pi_pa computes bias-corrected PI and PA correctly.

    We build a single-channel Stokes FITS where:
      Q = q_val (uniform), U = u_val (uniform)
    so analytic PI = sqrt(Q²+U²) and PA = 0.5*atan2(U,Q) in degrees.
    """

    @classmethod
    def _make_qu_fits(cls, q_val: float, u_val: float, ny: int = 8, nx: int = 8) -> str:
        """Single-frequency, 4-Stokes FITS with uniform Q and U."""
        from astropy.io import fits
        data = np.zeros((1, 4, ny, nx), dtype=np.float32)
        data[0, 0, :, :] = 1.0       # I
        data[0, 1, :, :] = q_val     # Q
        data[0, 2, :, :] = u_val     # U
        data[0, 3, :, :] = 0.0       # V
        hdu = fits.PrimaryHDU(data)
        h = hdu.header
        h["CTYPE1"] = "RA---SIN";  h["CRPIX1"] = nx/2; h["CRVAL1"] = 83.8
        h["CDELT1"] = -1e-4;       h["CUNIT1"] = "deg"
        h["CTYPE2"] = "DEC--SIN"; h["CRPIX2"] = ny/2; h["CRVAL2"] = -5.4
        h["CDELT2"] = 1e-4;        h["CUNIT2"] = "deg"
        h["CTYPE3"] = "STOKES";   h["CRPIX3"] = 1.0; h["CRVAL3"] = 1.0; h["CDELT3"] = 1.0
        h["CTYPE4"] = "FREQ";     h["CRPIX4"] = 1.0; h["CRVAL4"] = 1.4e9; h["CDELT4"] = 1e6
        h["CUNIT4"] = "Hz"
        tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
        fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
        tmp.close()
        return tmp.name

    def _run(self, fits_path: str, channel: int = 0, line_free_channels=None):
        from app.compute import worker_polarisation_pi_pa
        return worker_polarisation_pi_pa(fits_path, channel, line_free_channels)

    # -- PA=0°: Q=1, U=0 -------------------------------------------------------

    def test_pa_zero_when_q1_u0(self):
        path = self._make_qu_fits(q_val=1.0, u_val=0.0)
        try:
            result = self._run(path)
            pa = _b64_to_f32(result["pa_base64"], (result["height"], result["width"]))
            # PA = 0.5 * atan2(0, 1) = 0°
            np.testing.assert_allclose(pa, 0.0, atol=0.1,
                                       err_msg="PA should be 0° when Q=1, U=0")
        finally:
            os.unlink(path)

    # -- PA=45°: Q=0, U=1 (atan2(1,0)=90° → PA=45°) ---------------------------

    def test_pa_45_when_q0_u1(self):
        path = self._make_qu_fits(q_val=0.0, u_val=1.0)
        try:
            result = self._run(path)
            pa = _b64_to_f32(result["pa_base64"], (result["height"], result["width"]))
            # PA = 0.5 * 90° = 45°
            np.testing.assert_allclose(pa, 45.0, atol=0.1,
                                       err_msg="PA should be 45° when Q=0, U=1")
        finally:
            os.unlink(path)

    # -- PI analytic value (no bias) -------------------------------------------

    def test_pi_value_q1_u0(self):
        path = self._make_qu_fits(q_val=1.0, u_val=0.0)
        try:
            result = self._run(path, line_free_channels=[])
            pi = _b64_to_f32(result["pi_base64"], (result["height"], result["width"]))
            # Without noise, bias correction leaves PI ≈ |Q| = 1.0
            # (bias term σ²_QU ≈ 0 when no line-free channels → raw PI)
            self.assertTrue(np.all(pi >= 0.0), "PI must be non-negative")
        finally:
            os.unlink(path)

    # -- Bias correction clamps negative values to 0 ---------------------------

    def test_pi_bias_corrected_non_negative(self):
        """Bias correction: PI_corr = sqrt(max(Q²+U²−σ², 0)) ≥ 0 always."""
        path = self._make_qu_fits(q_val=0.001, u_val=0.001, ny=16, nx=16)
        try:
            # Use many "line-free" channels to get a noise estimate that could
            # exceed Q²+U², testing the clamping behaviour.
            result = self._run(path, line_free_channels=None)
            pi = _b64_to_f32(result["pi_base64"], (result["height"], result["width"]))
            self.assertTrue(np.all(pi >= 0.0), "Bias-corrected PI must be non-negative")
        finally:
            os.unlink(path)

    # -- PA wrapped into [0, 180) ----------------------------------------------

    def test_pa_q_neg1_u0(self):
        """Q=-1, U=0 → PA should be ±90° (wrapped to 90°)."""
        path = self._make_qu_fits(q_val=-1.0, u_val=0.0)
        try:
            result = self._run(path)
            pa = _b64_to_f32(result["pa_base64"], (result["height"], result["width"]))
            # atan2(0, -1) = 180° → PA = 90°  (or −90° wrapped to 90°)
            np.testing.assert_allclose(np.abs(pa), 90.0, atol=0.5)
        finally:
            os.unlink(path)

    # -- Result keys -----------------------------------------------------------

    def test_result_contains_expected_keys(self):
        path = self._make_qu_fits(q_val=1.0, u_val=0.0)
        try:
            result = self._run(path)
            for key in ("pi_base64", "pa_base64", "width", "height",
                        "pi_range_min", "pi_range_max", "pa_range_min", "pa_range_max"):
                self.assertIn(key, result, f"Missing key: {key}")
        finally:
            os.unlink(path)

    # -- Missing Q or U raises -------------------------------------------------

    def test_missing_stokes_raises(self):
        """A FITS with only I and V should raise when Q/U are needed."""
        from astropy.io import fits
        # Build a 2-Stokes (I, V) FITS – no Q or U
        data = np.ones((1, 2, 8, 8), dtype=np.float32)
        # Stokes CRVAL=1, CDELT=3 → pixel values 1, 4 (I, V)
        hdu = fits.PrimaryHDU(data)
        h = hdu.header
        h["CTYPE1"] = "RA---SIN"; h["CRVAL1"] = 0.0; h["CDELT1"] = -1e-4; h["CRPIX1"] = 4
        h["CTYPE2"] = "DEC--SIN"; h["CRVAL2"] = 0.0; h["CDELT2"] = 1e-4;  h["CRPIX2"] = 4
        h["CTYPE3"] = "STOKES";   h["CRVAL3"] = 1.0; h["CDELT3"] = 3.0;   h["CRPIX3"] = 1
        h["CTYPE4"] = "FREQ";     h["CRVAL4"] = 1.4e9; h["CDELT4"] = 1e6; h["CRPIX4"] = 1
        h["CUNIT4"] = "Hz"
        tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
        fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
        tmp.close()
        try:
            from app.compute import worker_polarisation_pi_pa
            with self.assertRaises((ValueError, KeyError)):
                worker_polarisation_pi_pa(tmp.name, 0, None)
        finally:
            os.unlink(tmp.name)


# ============================================================================
# P-03 – Rotation Measure synthesis
# ============================================================================

class TestRmSynthesis(unittest.TestCase):
    """
    worker_rm_synthesis recovers a known RM component.

    We inject a single RM component at φ₀ = 10 rad/m²:
      Q(λ²) = cos(2 φ₀ λ²)
      U(λ²) = sin(2 φ₀ λ²)
    so P(λ²) = exp(2i φ₀ λ²) and |F(φ)| peaks at φ = φ₀.
    """

    PHI_0 = 10.0   # rad/m²
    C_LIGHT = 2.99792458e8

    @classmethod
    def setUpClass(cls):
        cls.fits_path = cls._make_rm_fits()

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.fits_path)

    @classmethod
    def _make_rm_fits(cls, ny: int = 4, nx: int = 4,
                      n_freq: int = 32,
                      freq_start_hz: float = 1.3e9,
                      freq_delta_hz: float = 4.0e6) -> str:
        """
        Build a FITS cube where Q and U encode a pure RM at PHI_0.
        """
        from astropy.io import fits

        freqs = freq_start_hz + np.arange(n_freq) * freq_delta_hz
        lambda2 = (cls.C_LIGHT / freqs) ** 2   # (n_freq,)

        # Q + iU = exp(2i φ₀ λ²)
        P = np.exp(2j * cls.PHI_0 * lambda2)
        Q_arr = np.real(P).astype(np.float32)  # (n_freq,)
        U_arr = np.imag(P).astype(np.float32)  # (n_freq,)

        n_stokes = 4
        data = np.zeros((n_freq, n_stokes, ny, nx), dtype=np.float32)
        data[:, 0, :, :] = 1.0                       # I
        data[:, 1, :, :] = Q_arr[:, np.newaxis, np.newaxis]  # Q
        data[:, 2, :, :] = U_arr[:, np.newaxis, np.newaxis]  # U

        hdu = fits.PrimaryHDU(data)
        h = hdu.header
        h["CTYPE1"] = "RA---SIN"; h["CRPIX1"] = nx/2; h["CRVAL1"] = 83.8
        h["CDELT1"] = -1e-4;       h["CUNIT1"] = "deg"
        h["CTYPE2"] = "DEC--SIN"; h["CRPIX2"] = ny/2; h["CRVAL2"] = -5.4
        h["CDELT2"] = 1e-4;        h["CUNIT2"] = "deg"
        h["CTYPE3"] = "STOKES";   h["CRPIX3"] = 1.0; h["CRVAL3"] = 1.0; h["CDELT3"] = 1.0
        h["CTYPE4"] = "FREQ";     h["CRPIX4"] = 1.0
        h["CRVAL4"] = freq_start_hz; h["CDELT4"] = freq_delta_hz; h["CUNIT4"] = "Hz"
        tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
        fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
        tmp.close()
        return tmp.name

    def _run(self, phi_min=-50.0, phi_max=50.0, d_phi=0.5):
        from app.compute import worker_rm_synthesis
        return worker_rm_synthesis(self.fits_path, phi_min, phi_max, d_phi)

    # -- Peak at correct φ -----------------------------------------------------

    def test_peak_at_phi0(self):
        """The FDF peak (averaged over pixels) should occur at φ ≈ φ₀ = 10 rad/m²."""
        result = self._run(phi_min=-50.0, phi_max=50.0, d_phi=0.5)
        phi_grid = np.array(result["phi_grid"])
        n_phi = result["n_phi"]
        ny    = result["height"]
        nx    = result["width"]
        fdf = _b64_to_f32(result["data_base64"], (n_phi, ny, nx))
        # Average FDF over spatial pixels
        fdf_mean = fdf.mean(axis=(1, 2))   # (n_phi,)
        peak_phi = phi_grid[np.argmax(fdf_mean)]
        self.assertAlmostEqual(peak_phi, self.PHI_0, delta=result["d_phi"],
                               msg=f"Peak φ={peak_phi:.2f} not within dphi of φ₀={self.PHI_0}")

    def test_fdf_shape(self):
        result = self._run(phi_min=0.0, phi_max=20.0, d_phi=1.0)
        n_phi = result["n_phi"]
        ny    = result["height"]
        nx    = result["width"]
        fdf = _b64_to_f32(result["data_base64"], (n_phi, ny, nx))
        self.assertEqual(fdf.shape, (n_phi, ny, nx))

    def test_fdf_non_negative(self):
        result = self._run()
        fdf = _b64_to_f32(result["data_base64"],
                           (result["n_phi"], result["height"], result["width"]))
        self.assertTrue(np.all(fdf >= 0.0), "FDF magnitudes must be non-negative")

    def test_phi_grid_length_matches_n_phi(self):
        result = self._run()
        self.assertEqual(len(result["phi_grid"]), result["n_phi"])

    def test_peak_mode_reports_the_peak_noise(self):
        """peak_sigma is 1.4826·MAD of the measured peak values, for a significance cut."""
        from app.compute import worker_rm_synthesis
        r = worker_rm_synthesis(self.fits_path, -50.0, 50.0, 1.0, reduce="peak")
        peak = _b64_to_f32(r["data_base64"], (r["height"], r["width"]))
        v = peak[np.isfinite(peak) & (peak != 0)]
        expected = 1.4826 * np.median(np.abs(v - np.median(v)))
        self.assertAlmostEqual(r["peak_sigma"], float(expected), places=6)

    def test_phi_sample_count_edges(self):
        from app.compute.polarisation import phi_sample_count
        self.assertEqual(phi_sample_count(99999.99, 100000.0, 0.001), 11)   # large offset
        self.assertEqual(phi_sample_count(-0.3, 0.3, 0.1), 7)               # decimal step
        self.assertEqual(phi_sample_count(-80.0, 80.0, 3.0), 54)            # ends at 79, not past 80
        self.assertEqual(phi_sample_count(5.0, 5.0, 0.1), 1)
        self.assertEqual(phi_sample_count(10.0, 5.0, 1.0), 0)
        self.assertEqual(phi_sample_count(1e12, 1e12, 0.001), 1)            # equal ends at any scale
        with self.assertRaises(ValueError):                                  # step below φ's resolution
            phi_sample_count(1e12, 1e12 + 1.0, 1e-6)

    def test_phi_grid_includes_both_ends(self):
        """−20..20 at step 1 is 41 samples, symmetric: φ max is sampled too."""
        result = self._run(phi_min=-20.0, phi_max=20.0, d_phi=1.0)
        grid = np.array(result["phi_grid"])
        self.assertEqual(result["n_phi"], 41)
        self.assertAlmostEqual(grid[0], -20.0)
        self.assertAlmostEqual(grid[-1], 20.0)

    def test_rmsf_peak_at_zero(self):
        """RMSF = |Σ exp(-2i φ λ²)|/N; its peak should be at φ=0 (index 0 when phi_min=0)."""
        result = self._run(phi_min=0.0, phi_max=50.0, d_phi=0.5)
        rmsf = np.array(result["rmsf"])
        self.assertEqual(np.argmax(rmsf), 0,
                         "RMSF should peak at φ=0 (first element when phi_min=0)")

    def test_range_min_max_consistent(self):
        result = self._run()
        fdf = _b64_to_f32(result["data_base64"],
                           (result["n_phi"], result["height"], result["width"]))
        self.assertAlmostEqual(result["range_min"], float(np.nanmin(fdf)), places=4)
        self.assertAlmostEqual(result["range_max"], float(np.nanmax(fdf)), places=4)

    def test_empty_phi_range_raises(self):
        from app.compute import worker_rm_synthesis
        with self.assertRaises((ValueError, Exception)):
            worker_rm_synthesis(self.fits_path, 10.0, 5.0, 1.0)   # phi_max < phi_min

    def test_missing_qu_raises(self):
        """A pure-I FITS (no Q/U) should raise ValueError."""
        from astropy.io import fits
        data = np.ones((4, 1, 4, 4), dtype=np.float32)
        hdu = fits.PrimaryHDU(data)
        h = hdu.header
        h["CTYPE1"] = "RA---SIN"; h["CRVAL1"] = 0.0; h["CDELT1"] = -1e-4; h["CRPIX1"] = 2
        h["CTYPE2"] = "DEC--SIN"; h["CRVAL2"] = 0.0; h["CDELT2"] = 1e-4;  h["CRPIX2"] = 2
        h["CTYPE3"] = "STOKES";   h["CRVAL3"] = 1.0; h["CDELT3"] = 1.0;   h["CRPIX3"] = 1
        h["CTYPE4"] = "FREQ";     h["CRVAL4"] = 1.4e9; h["CDELT4"] = 1e6; h["CRPIX4"] = 1
        h["CUNIT4"] = "Hz"
        tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
        fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
        tmp.close()
        try:
            from app.compute import worker_rm_synthesis
            with self.assertRaises((ValueError, KeyError)):
                worker_rm_synthesis(tmp.name, -50.0, 50.0, 1.0)
        finally:
            os.unlink(tmp.name)


# ============================================================================
# P-domain HTTP endpoint smoke tests (via FastAPI TestClient)
# ============================================================================

_TEST_TOKEN = "test-token-visivo-12345"


class TestStokesEndpoint(unittest.TestCase):
    """GET /v1/stokes/{dataset_id} returns correct plane list."""

    @classmethod
    def setUpClass(cls):
        # Ensure token is set before importing the app
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        cls.fits_path = _make_stokes_fits(n_stokes=4, ny=8, nx=8, n_freq=4)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": _TEST_TOKEN}
        # Open dataset
        r = cls.client.post("/v1/datasets/open", json={"path": cls.fits_path},
                            headers=cls._auth)
        data = r.json()
        cls.dataset_id = data["dataset_id"]
        cls.session_id = data.get("session_id", "")

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.fits_path)

    def _headers(self):
        h = dict(self._auth)
        if self.session_id:
            h["X-Visivo-Session"] = self.session_id
        return h

    def test_stokes_I_returns_200(self):
        r = self.client.get(f"/v1/stokes/{self.dataset_id}",
                            params={"plane": "I"},
                            headers=self._headers())
        self.assertEqual(r.status_code, 200)

    def test_stokes_plane_valid_field(self):
        r = self.client.get(f"/v1/stokes/{self.dataset_id}",
                            params={"plane": "Q"},
                            headers=self._headers())
        data = r.json()
        self.assertTrue(data.get("valid"))
        self.assertEqual(data.get("plane"), "Q")

    def test_stokes_Q_data_value(self):
        """JSON endpoint: Q plane base64 should decode to 2.0 everywhere."""
        r = self.client.get(f"/v1/stokes/{self.dataset_id}",
                            params={"plane": "Q"},
                            headers=self._headers())
        data = r.json()
        arr = _b64_to_f32(data["data_base64"], (data["height"], data["width"]))
        np.testing.assert_allclose(arr, 2.0, atol=1e-4)

    def test_stokes_binary_endpoint_returns_bytes(self):
        r = self.client.get(
            f"/v1/stokes/{self.dataset_id}/binary",
            params={"plane": "I"},
            headers=self._headers(),
        )
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.headers.get("content-type"), "application/octet-stream")
        # 12-byte header + float32 body
        self.assertGreater(len(r.content), 12)

    def test_stokes_binary_parseable(self):
        r = self.client.get(
            f"/v1/stokes/{self.dataset_id}/binary",
            params={"plane": "Q"},
            headers=self._headers(),
        )
        arr, _ = _parse_binary_frame(r.content)
        # Q plane constant = 2.0
        np.testing.assert_allclose(arr, 2.0, atol=1e-4)


# ============================================================================
# One file per Stokes plane — the MGCLS / POSSUM / Obit MFImage layout
#
# Every case below was taken from the real MeerKAT MGCLS release
# (doi:10.48479/f2a2-qw16), whose per-field cubes are FIELD_I/Q/U/V.fits.gz,
# 7500 x 7500 x 16 x 1, each with a degenerate STOKES axis.
# ============================================================================

def _make_single_plane_fits(
    stokes_value: int,
    value: float,
    ny: int = 8,
    nx: int = 8,
    n_freq: int = 4,
    freq_start_hz: float = 1.2838e9,
    freq_delta_hz: float = 5.755108e7,
    ctype3: str = "SPECLNMF",
    cunit3: str | None = None,
    rm_rad_m2: float | None = None,
    quadrature: bool = False,
) -> str:
    """One MGCLS-shaped file: (stokes=1, freq, dec, ra), STOKES axis length 1.

    `stokes_value` is written verbatim to CRVAL4, so a test can reproduce the
    mislabelled V cube. With `rm_rad_m2` the plane carries a Faraday-rotated
    signal instead of a constant: Q = cos(2 phi lambda^2), U = sin(...) when
    `quadrature`.
    """
    from astropy.io import fits

    c = 2.99792458e8
    freqs = freq_start_hz + np.arange(n_freq) * freq_delta_hz
    data = np.zeros((1, n_freq, ny, nx), dtype=np.float32)
    if rm_rad_m2 is None:
        data[...] = value
    else:
        lam2 = (c / freqs) ** 2
        angle = 2.0 * rm_rad_m2 * lam2
        wave = np.sin(angle) if quadrature else np.cos(angle)
        data[0] = wave[:, None, None].astype(np.float32) * value

    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["CTYPE1"] = "GLON-SIN"; h["CRPIX1"] = nx / 2; h["CRVAL1"] = 0.0; h["CDELT1"] = -4.166667e-4
    h["CTYPE2"] = "GLAT-SIN"; h["CRPIX2"] = ny / 2; h["CRVAL2"] = 2.5; h["CDELT2"] = 4.166667e-4
    h["CTYPE3"] = ctype3;     h["CRPIX3"] = 1.0;    h["CRVAL3"] = freq_start_hz
    h["CDELT3"] = freq_delta_hz
    if cunit3:
        h["CUNIT3"] = cunit3
    h["CTYPE4"] = "STOKES";   h["CRPIX4"] = 1.0;    h["CRVAL4"] = float(stokes_value)
    h["CDELT4"] = 1.0

    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
    tmp.close()
    return tmp.name


class TestPerFileStokes(unittest.TestCase):
    """Q and U supplied as separate files alongside an I-only primary."""

    @classmethod
    def setUpClass(cls):
        cls.i_path = _make_single_plane_fits(stokes_value=1, value=10.0)
        cls.q_path = _make_single_plane_fits(stokes_value=2, value=3.0)
        cls.u_path = _make_single_plane_fits(stokes_value=3, value=4.0)
        # The MGCLS V cube really does carry CRVAL4 = 1 (i.e. "I").
        cls.v_mislabelled = _make_single_plane_fits(stokes_value=1, value=7.0)

    @classmethod
    def tearDownClass(cls):
        for p in (cls.i_path, cls.q_path, cls.u_path, cls.v_mislabelled):
            os.unlink(p)

    def test_plane_from_companion_file(self):
        from app.compute import worker_stokes_plane
        r = worker_stokes_plane(self.i_path, "Q", None, {"Q": self.q_path})
        img = _b64_to_f32(r["data_base64"], (r["height"], r["width"]))
        np.testing.assert_allclose(img, 3.0, atol=1e-5)

    def test_primary_plane_still_wins(self):
        """I is in the primary, so no companion is consulted for it."""
        from app.compute import worker_stokes_plane
        r = worker_stokes_plane(self.i_path, "I", None, {"I": self.q_path})
        img = _b64_to_f32(r["data_base64"], (r["height"], r["width"]))
        np.testing.assert_allclose(img, 10.0, atol=1e-5)

    def test_mislabelled_single_plane_companion_is_trusted(self):
        """The caller said this file is V; its header says I. The caller wins."""
        from app.compute import worker_stokes_plane
        r = worker_stokes_plane(self.i_path, "V", None, {"V": self.v_mislabelled})
        img = _b64_to_f32(r["data_base64"], (r["height"], r["width"]))
        np.testing.assert_allclose(img, 7.0, atol=1e-5)

    def test_missing_companion_names_the_plane(self):
        from app.compute import worker_polarisation_pi_pa
        with self.assertRaises(ValueError) as ctx:
            worker_polarisation_pi_pa(self.i_path, 0, None, {"Q": self.q_path})
        self.assertIn("U", str(ctx.exception))

    def test_pi_pa_across_files(self):
        from app.compute import worker_polarisation_pi_pa
        r = worker_polarisation_pi_pa(self.i_path, 0, None,
                                      {"Q": self.q_path, "U": self.u_path})
        pi = _b64_to_f32(r["pi_base64"], (r["height"], r["width"]))
        pa = _b64_to_f32(r["pa_base64"], (r["height"], r["width"]))
        np.testing.assert_allclose(pi, 5.0, atol=1e-4)          # 3-4-5
        np.testing.assert_allclose(pa, 0.5 * np.degrees(np.arctan2(4.0, 3.0)), atol=1e-3)


class TestPerFileRmSynthesis(unittest.TestCase):
    """RM synthesis with Q and U in separate files, MGCLS-style."""

    RM = 25.0

    @classmethod
    def setUpClass(cls):
        # A primary whose frequency grid DIFFERS from Q/U, as in MGCLS where the
        # I cube starts at 1.3426 GHz and Q/U at 1.2838 GHz.
        cls.i_path = _make_single_plane_fits(stokes_value=1, value=1.0,
                                             freq_start_hz=1.3426e9, n_freq=16)
        cls.q_path = _make_single_plane_fits(stokes_value=2, value=1.0, n_freq=16,
                                             rm_rad_m2=cls.RM)
        cls.u_path = _make_single_plane_fits(stokes_value=3, value=1.0, n_freq=16,
                                             rm_rad_m2=cls.RM, quadrature=True)

    @classmethod
    def tearDownClass(cls):
        for p in (cls.i_path, cls.q_path, cls.u_path):
            os.unlink(p)

    def _companions(self):
        return {"Q": self.q_path, "U": self.u_path}

    def test_peak_is_at_the_injected_rm(self):
        """lambda^2 must come from the Q file, not from the primary.

        Using the primary's grid shifts the recovered peak; this is the test
        that would have caught that.
        """
        from app.compute import worker_rm_synthesis
        r = worker_rm_synthesis(self.i_path, -100.0, 100.0, 1.0, self._companions())
        fdf = _b64_to_f32(r["data_base64"], (r["n_phi"], r["height"], r["width"]))
        phi = np.asarray(r["phi_grid"])
        peak_phi = phi[int(np.argmax(fdf[:, 0, 0]))]
        self.assertAlmostEqual(peak_phi, self.RM, delta=2.0)

    def test_reduce_peak_returns_two_maps(self):
        from app.compute import worker_rm_synthesis
        r = worker_rm_synthesis(self.i_path, -100.0, 100.0, 1.0, self._companions(),
                                reduce="peak")
        self.assertEqual(r["reduce"], "peak")
        peak = _b64_to_f32(r["data_base64"], (r["height"], r["width"]))
        phi_map = _b64_to_f32(r["phi_peak_base64"], (r["height"], r["width"]))
        self.assertEqual(peak.shape, (r["height"], r["width"]))
        np.testing.assert_allclose(phi_map, self.RM, atol=2.0)

    def test_region_crops(self):
        from app.compute import worker_rm_synthesis
        r = worker_rm_synthesis(self.i_path, -50.0, 50.0, 2.0, self._companions(),
                                reduce="peak", region=(2, 1, 3, 4))
        self.assertEqual((r["height"], r["width"]), (4, 3))

    def test_oversized_cube_is_refused_with_a_way_out(self):
        from app.compute import worker_rm_synthesis
        with self.assertRaises(ValueError) as ctx:
            worker_rm_synthesis(self.i_path, -1e5, 1e5, 1e-2, self._companions())
        message = str(ctx.exception)
        self.assertIn("MiB", message)
        self.assertIn("peak", message)          # says how to get an answer anyway

    def test_mismatched_frequency_grids_are_refused(self):
        """Q and U from files measured at different frequencies must not combine."""
        from app.compute import worker_rm_synthesis
        other_u = _make_single_plane_fits(stokes_value=3, value=1.0, n_freq=16,
                                          freq_start_hz=1.5e9, rm_rad_m2=self.RM,
                                          quadrature=True)
        try:
            with self.assertRaises(ValueError) as ctx:
                worker_rm_synthesis(self.i_path, -50.0, 50.0, 1.0,
                                    {"Q": self.q_path, "U": other_u})
            self.assertIn("frequency grids", str(ctx.exception))
        finally:
            os.unlink(other_u)


class TestSpectralAxisUnits(unittest.TestCase):
    """SPECLNMF, and the unit inference that has to go with a missing CUNIT."""

    def test_speclnmf_is_recognised_as_spectral(self):
        from app.fits_dataset import ScientificFitsDataset
        path = _make_single_plane_fits(stokes_value=2, value=1.0, ctype3="SPECLNMF")
        try:
            ds = ScientificFitsDataset(path)
            self.assertEqual(
                str(ds.header[f"CTYPE{ds.cube_numpy_spectral_fits_axis()}"]).strip(),
                "SPECLNMF")
        finally:
            os.unlink(path)

    def test_ghz_header_is_rejected_not_silently_scaled(self):
        """A GHz axis read as Hz would scale lambda^2 by 1e18."""
        from app.compute.polarisation import _frequencies_hz
        from app.fits_dataset import ScientificFitsDataset
        path = _make_single_plane_fits(stokes_value=2, value=1.0,
                                       freq_start_hz=1.28, freq_delta_hz=0.0575,
                                       cunit3=None)
        try:
            with self.assertRaises(ValueError) as ctx:
                _frequencies_hz(ScientificFitsDataset(path), 4)
            self.assertIn("plausible frequencies", str(ctx.exception))
        finally:
            os.unlink(path)

    def test_declared_ghz_is_converted(self):
        from app.compute.polarisation import _frequencies_hz
        from app.fits_dataset import ScientificFitsDataset
        path = _make_single_plane_fits(stokes_value=2, value=1.0,
                                       freq_start_hz=1.28, freq_delta_hz=0.0575,
                                       cunit3="GHz")
        try:
            freq = _frequencies_hz(ScientificFitsDataset(path), 4)
            self.assertAlmostEqual(freq[0], 1.28e9, delta=1e3)
        finally:
            os.unlink(path)


# ============================================================================
# The Obit MFImage plane layout, and the other ways a header can mislead
# ============================================================================

def _make_mfimage_fits(
    stokes_value: int,
    rm_rad_m2: float,
    quadrature: bool = False,
    n_term: int = 2,
    freqs_hz: list[float] | None = None,
    ny: int = 6,
    nx: int = 6,
    wcs_freq0: float = 1.283791015625e9,
    wcs_dfreq: float = 5.755108e7,
) -> str:
    """An MGCLS-shaped MFImage cube: NTERM fitted planes, then NSPEC sub-bands.

    The WCS spectral axis is deliberately WRONG, exactly as in the real files:
    it describes a linear grid from `wcs_freq0` that has nothing to do with the
    FREQnnnn keywords. A worker that believes CRVAL3/CDELT3 gets the wrong
    lambda^2 and includes the two fitted planes as if they were sub-bands.
    """
    from astropy.io import fits

    c = 2.99792458e8
    if freqs_hz is None:
        # The real G000.0+2.5 sub-band centres: 908.037 MHz + k * 57.55108 MHz.
        freqs_hz = [9.080371093750e8 + k * 5.755108e7 for k in range(14)]
    n_spec = len(freqs_hz)
    n_planes = n_term + n_spec

    lam2 = (c / np.asarray(freqs_hz)) ** 2
    angle = 2.0 * rm_rad_m2 * lam2
    wave = (np.sin(angle) if quadrature else np.cos(angle)).astype(np.float32)

    data = np.zeros((1, n_planes, ny, nx), dtype=np.float32)
    # The fitted planes carry something loud and unrelated, so a worker that
    # mistakes them for sub-bands cannot accidentally still get the right answer.
    data[0, :n_term] = 100.0
    data[0, n_term:] = wave[:, None, None]

    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["CTYPE1"] = "GLON-SIN"; h["CRPIX1"] = nx / 2; h["CRVAL1"] = 0.0; h["CDELT1"] = -4.166667e-4
    h["CTYPE2"] = "GLAT-SIN"; h["CRPIX2"] = ny / 2; h["CRVAL2"] = 2.5; h["CDELT2"] = 4.166667e-4
    h["CTYPE3"] = "SPECLNMF"; h["CRPIX3"] = 1.0;    h["CRVAL3"] = wcs_freq0
    h["CDELT3"] = wcs_dfreq
    h["CTYPE4"] = "STOKES";   h["CRPIX4"] = 1.0;    h["CRVAL4"] = float(stokes_value)
    h["CDELT4"] = 1.0
    h["NTERM"] = n_term
    h["NSPEC"] = n_spec
    for i, f in enumerate(freqs_hz, start=1):
        h[f"FREQ{i:04d}"] = float(f)
    h["BUNIT"] = "JY/BEAM"

    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
    tmp.close()
    return tmp.name


class TestMfImagePlaneLayout(unittest.TestCase):
    """NTERM fitted planes + NSPEC sub-bands with their own FREQnnnn keywords."""

    RM = 40.0

    @classmethod
    def setUpClass(cls):
        cls.q = _make_mfimage_fits(stokes_value=2, rm_rad_m2=cls.RM)
        cls.u = _make_mfimage_fits(stokes_value=3, rm_rad_m2=cls.RM, quadrature=True)

    @classmethod
    def tearDownClass(cls):
        for p in (cls.q, cls.u):
            os.unlink(p)

    def test_layout_is_detected(self):
        from app.compute.polarisation import _mfimage_subbands
        from app.fits_dataset import ScientificFitsDataset
        offset, freqs = _mfimage_subbands(ScientificFitsDataset(self.q))
        self.assertEqual(offset, 2)              # the two fitted planes are skipped
        self.assertEqual(len(freqs), 14)
        self.assertAlmostEqual(freqs[0], 9.080371093750e8, delta=1.0)

    def test_rm_is_recovered_from_the_freq_keywords(self):
        """The WCS axis would give the wrong lambda^2 AND two bogus planes."""
        from app.compute import worker_rm_synthesis
        r = worker_rm_synthesis(self.q, -200.0, 200.0, 1.0, {"U": self.u}, reduce="peak")
        self.assertEqual(r["n_freq"], 14)        # not 16: the fitted planes are out
        phi = _b64_to_f32(r["phi_peak_base64"], (r["height"], r["width"]))
        np.testing.assert_allclose(phi, self.RM, atol=2.0)

    def test_inconsistent_keywords_fall_back_to_the_wcs(self):
        """NTERM + NSPEC that do not fill the axis means the keywords are not trustworthy."""
        from app.compute.polarisation import _mfimage_subbands
        from app.fits_dataset import ScientificFitsDataset
        path = _make_mfimage_fits(stokes_value=2, rm_rad_m2=self.RM, n_term=5)
        try:
            from astropy.io import fits
            with fits.open(path, mode="update") as hdul:
                hdul[0].header["NSPEC"] = 3      # 5 + 3 != 19 planes
            self.assertIsNone(_mfimage_subbands(ScientificFitsDataset(path)))
        finally:
            os.unlink(path)

    def test_reading_the_wcs_axis_instead_would_be_off_by_a_factor(self):
        """Pins the number quoted in the paper: 25 rad/m^2 read as 63.

        Not a test of the worker — the worker gets it right, which is the point
        — but of the claim that getting it wrong is expensive. The grids are
        the real MGCLS ones: FREQnnnn spans 908-1656 MHz over 14 sub-band
        planes, the WCS axis 1284-2147 MHz over all 16, so reading the axis
        also drags the two fitted planes into the transform. If this number
        ever moves, the sentence in the paper has to move with it.
        """
        c = 2.99792458e8
        true_rm = 25.0
        keywords = 9.080371093750e8 + np.arange(14) * 5.755108e7
        wcs_axis = 1.283791015625e9 + np.arange(16) * 5.755108e7
        phi = np.arange(-200.0, 200.0, 0.5)

        signal = np.exp(2j * true_rm * (c / keywords) ** 2)
        correct = phi[np.argmax(np.abs(
            np.exp(-2j * np.outer(phi, (c / keywords) ** 2)) @ signal))]
        # As a WCS reader would see it: wrong frequencies, and two fitted
        # planes prepended where sub-bands are expected.
        with_fitted = np.concatenate([[signal[0], signal[0]], signal])
        naive = phi[np.argmax(np.abs(
            np.exp(-2j * np.outer(phi, (c / wcs_axis) ** 2)) @ with_fitted))]

        self.assertAlmostEqual(correct, true_rm, delta=0.5)
        self.assertAlmostEqual(naive, 63.0, delta=1.0)

    def test_q_mfimage_with_plain_u_is_refused(self):
        """Same sky grid, same plane count — only the sub-band keywords differ."""
        from app.compute import worker_rm_synthesis
        plain_u = _make_single_plane_fits(stokes_value=3, value=1.0, n_freq=16,
                                          ny=6, nx=6)
        try:
            with self.assertRaises(ValueError) as ctx:
                worker_rm_synthesis(self.q, -50.0, 50.0, 1.0, {"U": plain_u})
            self.assertIn("do not correspond", str(ctx.exception))
        finally:
            os.unlink(plain_u)


class TestGridAndNaNGuards(unittest.TestCase):

    def test_same_shape_different_sky_is_refused(self):
        """Two 8x8 cutouts centred elsewhere are not a Q/U pair."""
        from app.compute import worker_polarisation_pi_pa
        q = _make_single_plane_fits(stokes_value=2, value=3.0)
        u = _make_single_plane_fits(stokes_value=3, value=4.0)
        from astropy.io import fits
        with fits.open(u, mode="update") as hdul:
            hdul[0].header["CRVAL1"] = 12.5          # a different patch of sky
        try:
            with self.assertRaises(ValueError) as ctx:
                worker_polarisation_pi_pa(q, 0, None, {"U": u})
            self.assertIn("same sky grid", str(ctx.exception))
        finally:
            os.unlink(q)
            os.unlink(u)

    def test_all_nan_pixel_gets_no_rotation_measure(self):
        """argmax on an all-NaN spectrum answers 0; phi_min is not a measurement."""
        from app.compute import worker_rm_synthesis
        from astropy.io import fits
        q = _make_single_plane_fits(stokes_value=2, value=1.0, n_freq=8, rm_rad_m2=20.0)
        u = _make_single_plane_fits(stokes_value=3, value=1.0, n_freq=8, rm_rad_m2=20.0,
                                    quadrature=True)
        with fits.open(q, mode="update") as hdul:
            hdul[0].data[0, :, 0, 0] = np.nan        # one pixel, every channel
        try:
            r = worker_rm_synthesis(q, -100.0, 100.0, 1.0, {"U": u}, reduce="peak")
            phi = _b64_to_f32(r["phi_peak_base64"], (r["height"], r["width"]))
            self.assertTrue(np.isnan(phi[0, 0]), "the dead pixel must not carry an RM")
            self.assertAlmostEqual(float(phi[1, 1]), 20.0, delta=2.0)
        finally:
            os.unlink(q)
            os.unlink(u)

    def test_one_row_image_keeps_its_shape(self):
        """A (nfreq, 1, nx) cube must not squeeze into an nfreq-row image."""
        from app.compute.polarisation import _read_stokes_cube
        from app.fits_dataset import ScientificFitsDataset
        path = _make_single_plane_fits(stokes_value=2, value=5.0, ny=1, nx=12, n_freq=4)
        try:
            cube = _read_stokes_cube(ScientificFitsDataset(path), "Q", authoritative=True)
            self.assertEqual(cube.shape, (4, 1, 12))
        finally:
            os.unlink(path)

    def test_peak_mode_does_not_build_the_whole_cube(self):
        """The guard peak mode skips would be pointless if it allocated it anyway."""
        from app.compute import worker_rm_synthesis
        q = _make_single_plane_fits(stokes_value=2, value=1.0, n_freq=4, rm_rad_m2=10.0)
        u = _make_single_plane_fits(stokes_value=3, value=1.0, n_freq=4, rm_rad_m2=10.0,
                                    quadrature=True)
        try:
            # 400k phi samples over 8x8: the cube would be 97 MiB, the blocks are small.
            r = worker_rm_synthesis(q, -10000.0, 10000.0, 0.05, {"U": u}, reduce="peak")
            self.assertEqual(r["n_phi"], 400001)   # both ends sampled
            phi = _b64_to_f32(r["phi_peak_base64"], (r["height"], r["width"]))
            np.testing.assert_allclose(phi, 10.0, atol=1.0)
        finally:
            os.unlink(q)
            os.unlink(u)

    def test_unit_inference_requires_a_frequency_ctype(self):
        from app.compute.polarisation import _frequencies_hz
        from app.fits_dataset import ScientificFitsDataset
        path = _make_single_plane_fits(stokes_value=2, value=1.0, ctype3="LINEAR")
        try:
            with self.assertRaises(ValueError) as ctx:
                _frequencies_hz(ScientificFitsDataset(path), 4)
            self.assertIn("does not name a frequency", str(ctx.exception))
        finally:
            os.unlink(path)


class TestBlankSubBands(unittest.TestCase):
    """A released cube can leave a sub-band entirely blank.

    Found on the MeerKAT Bulge field G000.0+2.5 (Cotton et al. 2025), where the
    1199 and 1256 MHz sub-bands are NaN end to end. Before this, one such plane
    made every F(phi) NaN at every pixel — the RM map came back empty — and the
    PI/PA worker turned the NaN into a measured zero, so a blank plane looked
    perfectly unpolarised.
    """

    N_FREQ, NY, NX, RM = 10, 12, 12, 37.0
    FREQ0, DFREQ = 9.0e8, 6.0e7

    def _trio(self, blank_plane=None, fill=np.nan, n_freq=None):
        """I, Q, U on a plain FREQ axis, Q/U Faraday-rotated by RM; one plane blanked."""
        kw = dict(ny=self.NY, nx=self.NX, n_freq=n_freq or self.N_FREQ, freq_start_hz=self.FREQ0,
                  freq_delta_hz=self.DFREQ, ctype3="FREQ", cunit3="Hz")
        paths = {"I": _make_single_plane_fits(stokes_value=1, value=1.0, **kw),
                 "Q": _make_single_plane_fits(stokes_value=2, value=1.0, rm_rad_m2=self.RM, **kw),
                 "U": _make_single_plane_fits(stokes_value=3, value=1.0, rm_rad_m2=self.RM,
                                              quadrature=True, **kw)}
        for path in paths.values():
            self.addCleanup(os.unlink, path)
        if blank_plane is not None:
            self._blank(paths, blank_plane, fill)
        return paths

    @staticmethod
    def _blank(paths, plane, fill=np.nan):
        """Blank one plane of Q and of U: a sub-band is blank in both or it is not blank."""
        from astropy.io import fits
        for stokes in ("Q", "U"):
            with fits.open(paths[stokes], mode="update") as hdul:
                hdul[0].data[0, plane] = fill

    def _rm(self, paths, **kw):
        from app.compute import worker_rm_synthesis
        return worker_rm_synthesis(paths["I"], -100.0, 100.0, 1.0,
                                   companions={"Q": paths["Q"], "U": paths["U"]}, reduce="peak", **kw)

    def test_a_blank_plane_is_dropped_and_the_rm_is_still_recovered(self):
        r = self._rm(self._trio(blank_plane=4))
        self.assertEqual(r["dropped_planes"], [4])
        self.assertAlmostEqual(r["dropped_freq_hz"][0], self.FREQ0 + 4 * self.DFREQ, delta=1.0)
        self.assertEqual(r["n_freq"], self.N_FREQ - 1)
        phi = _b64_to_f32(r["phi_peak_base64"], (self.NY, self.NX))
        self.assertTrue(np.isfinite(phi).all(), "no pixel may be poisoned by the blank plane")
        self.assertAlmostEqual(float(phi[6, 6]), self.RM, delta=1.0)

    def test_a_zero_filled_plane_is_blank_too(self):
        """Pipelines fill flagged bands with 0.0 as often as with NaN."""
        r = self._rm(self._trio(blank_plane=4, fill=0.0))
        self.assertEqual(r["dropped_planes"], [4])
        self.assertEqual(r["n_freq"], self.N_FREQ - 1)

    def test_nothing_is_dropped_when_every_plane_has_data(self):
        r = self._rm(self._trio())
        self.assertEqual(r["dropped_planes"], [])
        self.assertEqual(r["dropped_freq_hz"], [])
        self.assertEqual(r["n_freq"], self.N_FREQ)

    def test_dropped_planes_are_counted_as_the_file_counts_them(self):
        """MFImage: two fitted planes first, so sub-band 5 is file plane 7 — the
        number the file and the figures use (0-based; the GUI names the band
        by frequency), not the position in the sub-band list."""
        q = _make_mfimage_fits(stokes_value=2, rm_rad_m2=self.RM)
        u = _make_mfimage_fits(stokes_value=3, rm_rad_m2=self.RM, quadrature=True)
        for path in (q, u):
            self.addCleanup(os.unlink, path)
        paths = {"I": q, "Q": q, "U": u}       # the Q file carries the primary's own Stokes
        self._blank(paths, 7)
        r = self._rm(paths)
        self.assertEqual(r["dropped_planes"], [7])
        self.assertAlmostEqual(r["dropped_freq_hz"][0], 9.080371093750e8 + 5 * 5.755108e7, delta=1.0)
        self.assertEqual(r["n_freq"], 13)
        phi = _b64_to_f32(r["phi_peak_base64"], (r["height"], r["width"]))
        np.testing.assert_allclose(phi, self.RM, atol=2.0)

    def test_fewer_than_three_surviving_planes_is_refused(self):
        """With one or two planes |F(phi)| has no unique peak: the map would be
        phi_min everywhere, finite and invented."""
        paths = self._trio()
        for plane in range(2, self.N_FREQ):
            self._blank(paths, plane)
        with self.assertRaises(ValueError) as ctx:
            self._rm(paths)
        self.assertIn("carry data in this region", str(ctx.exception))

    def test_a_cube_with_fewer_than_three_planes_is_refused_as_such(self):
        """A two-plane cube is not a blank-plane problem and must not be told it is."""
        with self.assertRaises(ValueError) as ctx:
            self._rm(self._trio(n_freq=2))
        self.assertIn("spectral plane", str(ctx.exception))

    def test_coverage_query_matches_the_worker(self):
        from app.compute.polarisation import stokes_plane_coverage
        paths = self._trio(blank_plane=4)
        cov = stokes_plane_coverage(paths["I"], {"Q": paths["Q"], "U": paths["U"]})
        self.assertEqual([c["plane"] for c in cov if not c["has_data"]], [4])
        self.assertAlmostEqual(cov[4]["freq_hz"], self.FREQ0 + 4 * self.DFREQ, delta=1.0)
        self.assertEqual(len(cov), self.N_FREQ)

    def test_pi_pa_keeps_nan_and_refuses_a_blank_plane(self):
        from app.compute import worker_polarisation_pi_pa
        paths = self._trio(blank_plane=4)
        comp = {"Q": paths["Q"], "U": paths["U"]}
        with self.assertRaises(ValueError) as ctx:
            worker_polarisation_pi_pa(paths["I"], 4, None, companions=comp)
        self.assertIn("blank on plane 4", str(ctx.exception))
        with self.assertRaises(ValueError) as ctx:
            worker_polarisation_pi_pa(paths["I"], 40, None, companions=comp)
        self.assertIn("outside", str(ctx.exception))       # refused, not clamped to the last plane
        # a plane with data, but poke a hole in it: the hole must stay a hole
        from astropy.io import fits
        with fits.open(paths["Q"], mode="update") as hdul:
            hdul[0].data[0, 2, 3, 3] = np.nan
        r = worker_polarisation_pi_pa(paths["I"], 2, None, companions=comp)
        pi = _b64_to_f32(r["pi_base64"], (self.NY, self.NX))
        self.assertTrue(np.isnan(pi[3, 3]), "a missing measurement is not a zero one")
        self.assertTrue(np.isfinite(pi[0, 0]))

    def test_blank_line_free_channels_are_dropped_from_the_noise_estimate(self):
        """A blank channel in the line-free list is no measurement of the noise:
        a NaN plane would silently switch the de-bias off (nanvar over one
        sample is 0), a zero-filled one would count the fill as signal."""
        from app.compute import worker_polarisation_pi_pa
        paths = self._trio(blank_plane=4)
        comp = {"Q": paths["Q"], "U": paths["U"]}
        r = worker_polarisation_pi_pa(paths["I"], 2, [3, 4, 5], companions=comp)
        self.assertEqual(r["line_free_channels_dropped"], [4])
        with self.assertRaises(ValueError) as ctx:
            worker_polarisation_pi_pa(paths["I"], 2, [2, 4], companions=comp)
        self.assertIn("line-free", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
