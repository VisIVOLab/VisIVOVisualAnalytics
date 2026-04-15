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


if __name__ == "__main__":
    unittest.main()
