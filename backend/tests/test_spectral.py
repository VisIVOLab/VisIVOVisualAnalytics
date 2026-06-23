"""
test_spectral.py – unit and integration tests for Domain S (spectral analysis).

Covers:
  S-01  Higher-order moments   (M3 skewness, M4 kurtosis, M5 raw)
  S-02  Line-width maps        (FWHM Gaussian fit, equivalent width)
  S-03  Polynomial baseline    (subtraction, new dataset registration)
  S-04  Spectral stacking      (mean / median / weighted_mean)
  S-05  SoFiA-2 wrapper        (absent sofia2 → 503; subprocess mock)

Run with:
    cd backend
    pytest tests/test_spectral.py -v
"""

from __future__ import annotations

import base64
import struct
import tempfile
from pathlib import Path
from unittest.mock import MagicMock, patch

import numpy as np
import pytest
import pytest_asyncio
from astropy.io import fits


# ── Helpers ────────────────────────────────────────────────────────────────────

def _write_gaussian_cube(path: str, nz: int = 32, ny: int = 8, nx: int = 8,
                          sigma_chan: float = 3.0) -> None:
    """Write a cube whose per-pixel spectrum is a clean Gaussian + noise.

    The Gaussian is centred at channel nz//2, FWHM = 2.3548 * sigma_chan.
    """
    rng = np.random.default_rng(0)
    chans = np.arange(nz, dtype=np.float32)
    center = nz / 2.0
    gauss = np.exp(-0.5 * ((chans - center) / sigma_chan) ** 2)  # (nz,)
    # broadcast to (nz, ny, nx) and add small noise
    data = (gauss[:, None, None] * 10.0
            + rng.standard_normal((nz, ny, nx)).astype(np.float32) * 0.05)
    hdu = fits.PrimaryHDU(data.astype(np.float32))
    h = hdu.header
    h["NAXIS"] = 3; h["NAXIS1"] = nx; h["NAXIS2"] = ny; h["NAXIS3"] = nz
    h["CTYPE3"] = "VRAD"; h["CRVAL3"] = 0.0; h["CRPIX3"] = nz / 2.0
    h["CDELT3"] = 1.0; h["CUNIT3"] = "km/s"
    h["BUNIT"] = "Jy/beam"
    hdu.writeto(path, overwrite=True)


def _write_simple_cube(path: str, data: np.ndarray) -> None:
    """Write a 3-D float32 array as a minimal FITS cube."""
    nz, ny, nx = data.shape
    hdu = fits.PrimaryHDU(data.astype(np.float32))
    h = hdu.header
    h["NAXIS"] = 3; h["NAXIS1"] = nx; h["NAXIS2"] = ny; h["NAXIS3"] = nz
    h["CTYPE3"] = "VRAD"; h["CRVAL3"] = 0.0; h["CRPIX3"] = 1.0
    h["CDELT3"] = 1.0; h["CUNIT3"] = "km/s"
    h["BUNIT"] = "Jy/beam"
    hdu.writeto(path, overwrite=True)


def _decode_f32(b64: str, shape: tuple[int, ...]) -> np.ndarray:
    return np.frombuffer(base64.b64decode(b64), dtype=np.float32).reshape(shape)


def _parse_binary_frame(raw: bytes, offset: int = 0) -> tuple[np.ndarray, int]:
    """Parse one Visivo binary frame starting at byte offset.

    Returns (array, next_offset).
    """
    rows, cols, flags = struct.unpack_from(">III", raw, offset)
    offset += 12
    n = rows * cols
    arr = np.frombuffer(raw, dtype="<f4", count=n, offset=offset).reshape(rows, cols)
    return arr, offset + n * 4


# ── S-01: Higher-order moments ─────────────────────────────────────────────────

class TestHigherOrderMoments:
    """Pure compute-layer tests — no HTTP round-trip."""

    @pytest.fixture(autouse=True)
    def cube_path(self, tmp_path):
        path = str(tmp_path / "moment_cube.fits")
        _write_gaussian_cube(path, nz=32, ny=4, nx=4, sigma_chan=3.0)
        self.cube_path = path

    def _run_moment(self, order: int):
        from app.compute import _moment_map_from_array  # noqa: PLC0415
        from astropy.io import fits as _fits
        with _fits.open(self.cube_path, memmap=True) as hdul:
            data = hdul[0].data
        nz, ny, nx = data.shape
        spectral = np.arange(nz, dtype=np.float64)
        return _moment_map_from_array(data, order, spectral, False, 0.0)

    def test_m3_skewness_near_zero_for_symmetric_gaussian(self):
        """A symmetric Gaussian should have skewness ≈ 0."""
        m3 = self._run_moment(3)
        finite = m3[np.isfinite(m3)]
        assert finite.size > 0
        # Median absolute skewness should be small
        assert float(np.abs(np.nanmedian(m3))) < 0.5

    def test_m4_kurtosis_near_zero_for_gaussian(self):
        """A Gaussian has excess kurtosis = 0 (by definition)."""
        m4 = self._run_moment(4)
        finite = m4[np.isfinite(m4)]
        assert finite.size > 0
        # Should be within ±2 of 0 for the test cube
        assert float(np.abs(np.nanmedian(m4))) < 2.0

    def test_m5_shape_and_type(self):
        """M5 should return a float32 map with the right spatial shape."""
        with fits.open(self.cube_path) as h:
            _, ny, nx = h[0].data.shape
        m5 = self._run_moment(5)
        assert m5.dtype == np.float32
        assert m5.shape == (ny, nx)

    @pytest.mark.parametrize("order", [3, 4, 5])
    def test_higher_order_all_finite_for_clean_cube(self, order):
        """For a noise-free Gaussian cube all spatial pixels should be finite."""
        result = self._run_moment(order)
        # Allow up to 5 % NaN pixels (edge effects)
        nan_frac = float(np.isnan(result).sum()) / result.size
        assert nan_frac < 0.05, f"Order {order}: {nan_frac:.1%} NaN pixels"

    def test_unsupported_order_raises(self):
        from app.compute import _moment_map_from_array
        with fits.open(self.cube_path) as h:
            data = h[0].data
        spectral = np.arange(data.shape[0], dtype=np.float64)
        with pytest.raises(ValueError, match="Unsupported moment order"):
            _moment_map_from_array(data, 99, spectral, False, 0.0)

    # ── New tight-tolerance tests (spec §S-01) ─────────────────────────────────

    def test_m3_and_m4_symmetric_gaussian_tight(self):
        """Noise-free symmetric Gaussian → |M3| < 0.05 and |M4| < 0.1 (excess kurtosis).

        Uses a large noiseless cube so numerical effects from finite-grid
        sampling are the only source of error.
        """
        from app.compute import _moment_map_from_array

        nz, ny, nx = 64, 4, 4
        chans = np.arange(nz, dtype=np.float64)
        center = nz / 2.0
        sigma = 6.0
        profile = np.exp(-0.5 * ((chans - center) / sigma) ** 2).astype(np.float32)
        # Identical Gaussian profile in every spatial pixel — result must be uniform.
        cube = (profile[:, None, None] * np.ones((1, ny, nx), dtype=np.float32))
        spectral = chans.copy()  # channel indices as velocity values

        m3 = _moment_map_from_array(cube, 3, spectral, False, 0.0)
        m4 = _moment_map_from_array(cube, 4, spectral, False, 0.0)

        med_m3 = float(np.abs(np.nanmedian(m3)))
        med_m4 = float(np.abs(np.nanmedian(m4)))
        assert med_m3 < 0.05, f"|M3 median| = {med_m3:.4f}, expected < 0.05"
        assert med_m4 < 0.1,  f"|M4 median| = {med_m4:.4f}, expected < 0.1"

    def test_m3_sign_for_right_skewed_profile(self):
        """Right-skewed spectral profile → M3 > 0 (positive skewness).

        Profile: v * exp(-v / scale) — peaks at v=scale, then decays with a
        long right tail.  Mean > mode → positive third standardised moment.
        """
        from app.compute import _moment_map_from_array

        nz, ny, nx = 64, 4, 4
        chans = np.arange(nz, dtype=np.float64)
        scale = 8.0
        profile = (chans * np.exp(-chans / scale)).astype(np.float32)
        # Keep only positive values (they already are)
        cube = profile[:, None, None] * np.ones((1, ny, nx), dtype=np.float32)
        spectral = chans.copy()

        m3 = _moment_map_from_array(cube, 3, spectral, False, 0.0)
        finite = m3[np.isfinite(m3)]
        assert finite.size > 0, "M3 should have finite pixels"
        assert float(np.nanmedian(m3)) > 0.0, (
            f"Right-skewed profile must give positive M3, got {np.nanmedian(m3):.4f}"
        )

    def test_m5_returns_finite_correct_shape(self):
        """M5 (standardised 5th moment = μ₅/σ⁵) returns finite float32 map.

        For a symmetric Gaussian the value should be near zero (|M5| < 0.1
        after normalisation), and well within ±100 even for a skewed profile.
        """
        from app.compute import _moment_map_from_array

        nz, ny, nx = 32, 4, 4
        chans = np.arange(nz, dtype=np.float64)
        center = nz / 2.0
        sigma = 3.0
        profile = np.exp(-0.5 * ((chans - center) / sigma) ** 2).astype(np.float32)
        cube = profile[:, None, None] * np.ones((1, ny, nx), dtype=np.float32)
        spectral = chans.copy()

        m5 = _moment_map_from_array(cube, 5, spectral, False, 0.0)

        assert m5.dtype == np.float32, f"Expected float32, got {m5.dtype}"
        assert m5.shape == (ny, nx), f"Expected ({ny}, {nx}), got {m5.shape}"
        assert np.isfinite(m5).all(), (
            f"All M5 pixels should be finite; got {np.isnan(m5).sum()} NaN values"
        )
        # Normalised by σ⁵ → dimensionless; symmetric Gaussian → near zero
        assert float(np.abs(np.nanmedian(m5))) < 0.1, (
            f"|M5 median| = {np.nanmedian(m5):.4f}, expected < 0.1 after σ⁵ normalisation"
        )


# ── S-02: Line-width maps ──────────────────────────────────────────────────────

class TestLinewidthWorker:

    @pytest.fixture(autouse=True)
    def cube_path(self, tmp_path):
        path = str(tmp_path / "lw_cube.fits")
        self.sigma_chan = 3.0
        _write_gaussian_cube(path, nz=32, ny=4, nx=4, sigma_chan=self.sigma_chan)
        self.cube_path = path
        # Spectral metadata is reported in the header-native unit (km/s here),
        # with WCS coordinates converted into it. With CDELT3=1.0 km/s the
        # per-channel step is 1 km/s, so FWHM = 2.3548 * sigma_chan km/s.
        self.expected_fwhm = 2.3548 * self.sigma_chan  # km/s

    def test_fwhm_accuracy(self):
        """FWHM recovered by Gaussian fit should match the injected σ within 10 %."""
        from app.compute import worker_linewidth
        result = worker_linewidth(self.cube_path, 0, 31, False, 0.0)
        fwhm = _decode_f32(result["fwhm_base64"], (result["height"], result["width"]))
        finite = fwhm[np.isfinite(fwhm)]
        assert finite.size > 0
        median_fwhm = float(np.median(finite))
        assert abs(median_fwhm - self.expected_fwhm) / self.expected_fwhm < 0.10

    def test_ew_positive_for_emission(self):
        """EW should be positive for an emission cube."""
        from app.compute import worker_linewidth
        result = worker_linewidth(self.cube_path, 0, 31, False, 0.0)
        ew = _decode_f32(result["ew_base64"], (result["height"], result["width"]))
        finite = ew[np.isfinite(ew)]
        assert finite.size > 0
        assert float(np.median(finite)) > 0.0

    def test_result_shape(self):
        """Output maps must have the correct (ny, nx) shape."""
        from app.compute import worker_linewidth
        result = worker_linewidth(self.cube_path, 0, 31, False, 0.0)
        with fits.open(self.cube_path) as h:
            _, ny, nx = h[0].data.shape
        assert result["height"] == ny
        assert result["width"] == nx

    def test_units_returned(self):
        from app.compute import worker_linewidth
        result = worker_linewidth(self.cube_path, 0, 31, False, 0.0)
        # astropy converts km/s → SI (m/s); the unit string reflects that
        assert "s" in result["fwhm_unit"]   # e.g. "m.s**-1" or "km/s"
        assert "s" in result["ew_unit"]


# ── S-03: Polynomial baseline ──────────────────────────────────────────────────

class TestBaselineWorker:

    @pytest.fixture(autouse=True)
    def cube_path(self, tmp_path):
        self.tmp_path = tmp_path
        rng = np.random.default_rng(11)
        nz, ny, nx = 32, 6, 6
        # Flat baseline + gentle linear slope + small noise
        chans = np.arange(nz, dtype=np.float32)
        slope = 0.05 * chans[:, None, None]  # linear baseline
        noise = (rng.standard_normal((nz, ny, nx)) * 0.02).astype(np.float32)
        data = slope + noise
        path = str(tmp_path / "baseline_cube.fits")
        _write_simple_cube(path, data)
        self.cube_path = path

    # Fixture cube: 32 channels, linear ramp baseline + noise, no emission.
    # Line-free sidebands suitable for all fixture-based tests:
    _LF_32 = [[0, 10], [20, 31]]   # two sidebands, no signal region to protect

    def test_baseline_rms_reduced(self):
        """After linear baseline subtraction the RMS should drop significantly."""
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_out.fits")
        result = worker_baseline(self.cube_path, out_path, 0, 31, 1, self._LF_32)
        assert result["output_path"] == out_path
        with fits.open(out_path) as h:
            residual = h[0].data
        # The linear slope contributes ~0.75 in RMS; after subtraction only noise remains
        assert float(np.nanstd(residual)) < 0.1

    def test_output_file_written(self):
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_out2.fits")
        worker_baseline(self.cube_path, out_path, 0, 31, 1, self._LF_32)
        assert Path(out_path).exists()

    def test_metadata_returned(self):
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_out3.fits")
        result = worker_baseline(self.cube_path, out_path, 0, 31, 1, self._LF_32)
        assert result["width"] == 6
        assert result["height"] == 6
        assert result["depth"] == 32
        assert result["poly_order"] == 1

    def test_line_free_channels(self):
        """Fitting on selected channels should still work."""
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_out4.fits")
        line_free = list(range(0, 10)) + list(range(22, 32))
        result = worker_baseline(self.cube_path, out_path, 0, 31, 1, line_free)
        assert result["fit_channels"] == len(line_free)

    # ── Regression tests for the NAXIS3=1 collapse bug ────────────────────────

    def test_full_depth_sentinel_channel_end_zero(self):
        """channel_start=0, channel_end=0 is the 'use all channels' sentinel.

        This is the exact call the Qt client always makes.  Before the fix,
        z1 was clamped to 0 → data[0:1] → NAXIS3=1 in the output.
        """
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_sentinel.fits")
        # Mimic exactly what the Qt client sends (line-free sidebands required)
        result = worker_baseline(self.cube_path, out_path, 0, 0, 1, self._LF_32)
        assert result["depth"] == 32, (
            f"Expected depth=32 (full cube) but got {result['depth']}. "
            "channel_end=0 must be treated as the 'last channel' sentinel."
        )
        with fits.open(out_path) as h:
            assert h[0].data.shape == (32, 6, 6), (
                f"Output shape {h[0].data.shape} != (32, 6, 6). "
                "NAXIS3 must equal the input spectral depth."
            )

    def test_output_shape_equals_input_shape(self):
        """Output cube shape must exactly match the input cube shape.

        Regression for the bug where output NAXIS3 was written as 1.
        """
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_shape.fits")
        nz, ny, nx = 32, 6, 6  # matches the fixture
        result = worker_baseline(self.cube_path, out_path, 0, 0, 1, self._LF_32)
        assert (result["depth"], result["height"], result["width"]) == (nz, ny, nx)
        with fits.open(out_path) as h:
            assert h[0].data.shape == (nz, ny, nx)
            assert h[0].header["NAXIS3"] == nz

    def test_naxis3_never_one_when_input_is_deep(self):
        """A deep cube (109 channels, like WALLABY) must never produce NAXIS3=1."""
        from app.compute import worker_baseline
        # Synthetic cube shaped like the WALLABY example: 196×280×109
        # Use a smaller proxy (109×8×8) to keep the test fast.
        rng = np.random.default_rng(42)
        nz, ny, nx = 109, 8, 8
        chans = np.arange(nz, dtype=np.float32)
        baseline = 0.1 * chans[:, None, None]
        signal = np.zeros((nz, ny, nx), dtype=np.float32)
        signal[50:60] = 1.0  # fake emission line in channels 50-59
        noise = (rng.standard_normal((nz, ny, nx)) * 0.02).astype(np.float32)
        data = baseline + signal + noise
        cube_path = str(self.tmp_path / "wallaby_proxy.fits")
        _write_simple_cube(cube_path, data)

        # Line-free sidebands outside the 50-59 emission window
        lf_109 = [[0, 40], [70, 108]]
        out_path = str(self.tmp_path / "wallaby_baseline.fits")
        # Qt client always sends channel_start=0, channel_end=0
        result = worker_baseline(cube_path, out_path, 0, 0, 1, lf_109)

        assert result["depth"] == nz, (
            f"WALLABY-proxy: expected depth={nz}, got {result['depth']}."
        )
        with fits.open(out_path) as h:
            assert h[0].data.shape[0] == nz, (
                f"NAXIS3={h[0].data.shape[0]} but input had {nz} channels."
            )
            assert h[0].header["NAXIS3"] == nz

    def test_corrected_cube_not_all_zeros(self):
        """After baseline subtraction the corrected cube must retain signal.

        The cube has a linear ramp baseline + noise; after subtraction the
        residuals must not be all zeros and the std must remain above the
        noise floor.
        """
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_nonzero.fits")
        worker_baseline(self.cube_path, out_path, 0, 0, 1, self._LF_32)
        with fits.open(out_path) as h:
            residual = h[0].data
        assert not np.all(residual == 0.0), "Corrected cube is all zeros."
        assert float(np.nanstd(residual)) > 0.0, "Corrected cube has zero variance."

    def test_line_signal_survives_subtraction(self):
        """Real spectral line signal must survive baseline subtraction.

        Build a cube with a known Gaussian emission line on top of a linear
        baseline.  After subtraction, the line peak should be detectable at
        the same channel and the line-free regions should be near-zero.
        """
        from app.compute import worker_baseline
        rng = np.random.default_rng(7)
        nz, ny, nx = 32, 4, 4
        chans = np.arange(nz, dtype=np.float32)

        # Linear baseline
        baseline = 0.5 * chans[:, None, None] * np.ones((nz, ny, nx), dtype=np.float32)
        # Gaussian emission line at channel 16, FWHM ≈ 5 channels
        sigma = 2.0
        line = 5.0 * np.exp(-0.5 * ((chans - 16.0) / sigma) ** 2)
        line = line[:, None, None] * np.ones((nz, ny, nx), dtype=np.float32)
        noise = (rng.standard_normal((nz, ny, nx)) * 0.05).astype(np.float32)
        data = (baseline + line + noise).astype(np.float32)

        cube_path = str(self.tmp_path / "signal_cube.fits")
        _write_simple_cube(cube_path, data)

        # Line-free regions: channels 0-10 and 22-31
        line_free = [[0, 10], [22, 31]]
        out_path = str(self.tmp_path / "signal_baseline.fits")
        result = worker_baseline(cube_path, out_path, 0, 0, 1, line_free)

        assert result["depth"] == nz

        with fits.open(out_path) as h:
            residual = h[0].data  # (nz, ny, nx)

        # Peak of the emission line should survive — mean residual at line
        # center must be significantly above the noise floor.
        line_peak_mean = float(np.mean(residual[15:18, :, :]))
        line_free_rms = float(np.sqrt(np.mean(residual[[0, 1, 2, 29, 30, 31]] ** 2)))
        assert line_peak_mean > 3.0 * line_free_rms, (
            f"Line peak ({line_peak_mean:.3f}) not detectable above "
            f"3σ noise ({3 * line_free_rms:.3f}) after baseline subtraction."
        )

    def test_wcs_spectral_keywords_preserved(self):
        """CTYPE3 / CUNIT3 / CRVAL3 / CDELT3 must be unchanged in the output.

        The fix must not accidentally drop or corrupt the spectral WCS axis.
        """
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_wcs.fits")
        worker_baseline(self.cube_path, out_path, 0, 0, 1, self._LF_32)
        with fits.open(out_path) as h:
            hdr = h[0].header
        assert hdr["CTYPE3"] == "VRAD"
        assert hdr["CUNIT3"] == "km/s"
        assert hdr["CDELT3"] == 1.0
        # CRVAL3 is preserved (baseline subtraction does not shift the axis)
        assert "CRVAL3" in hdr

    def test_sofia_4d_stokes_input_preserves_depth(self):
        """SoFiA outputs 4-D FITS with a degenerate Stokes axis (NAXIS4=1).

        squeeze_to_3d must collapse (1, nz, ny, nx) → (nz, ny, nx) without
        losing the spectral axis.  The output must have NAXIS3=nz, not 1.
        """
        from app.compute import worker_baseline
        rng = np.random.default_rng(99)
        nz, ny, nx = 64, 5, 5
        # Build a 4-D array: shape (1, nz, ny, nx) — Stokes-I only
        chans = np.arange(nz, dtype=np.float32)
        data_3d = (0.1 * chans[:, None, None]
                   + rng.standard_normal((nz, ny, nx)).astype(np.float32) * 0.02)
        data_4d = data_3d[np.newaxis, ...]   # (1, nz, ny, nx)

        # Write a 4-D FITS file the way SoFiA would
        cube_path = str(self.tmp_path / "sofia_4d.fits")
        hdu = fits.PrimaryHDU(data_4d.astype(np.float32))
        h = hdu.header
        h["NAXIS"] = 4
        h["NAXIS1"] = nx; h["NAXIS2"] = ny; h["NAXIS3"] = nz; h["NAXIS4"] = 1
        h["CTYPE3"] = "VRAD"; h["CRVAL3"] = 0.0; h["CRPIX3"] = 1.0
        h["CDELT3"] = 1.0; h["CUNIT3"] = "km/s"
        h["BUNIT"] = "Jy/beam"
        hdu.writeto(cube_path, overwrite=True)

        out_path = str(self.tmp_path / "sofia_baseline.fits")
        # Line-free sidebands for 64-channel cube with no signal
        result = worker_baseline(cube_path, out_path, 0, 0, 1, [[0, 20], [50, 63]])

        assert result["depth"] == nz, (
            f"4-D SoFiA input: expected depth={nz}, got {result['depth']}. "
            "squeeze_to_3d must not collapse the spectral axis."
        )
        with fits.open(out_path) as h:
            assert h[0].data.shape == (nz, ny, nx), (
                f"Output shape {h[0].data.shape} != ({nz}, {ny}, {nx}). "
                "NAXIS3 must equal the input spectral depth after squeezing Stokes."
            )
            assert h[0].header["NAXIS3"] == nz


    # ── Policy and validation regression tests ────────────────────────────────

    def test_missing_line_free_channels_raises(self):
        """Calling worker_baseline with no line-free channels must raise ValueError.

        Policy A: silently fitting on all channels absorbs real signal and is
        scientifically incorrect.  The worker must reject the call, not proceed.
        """
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "should_not_exist.fits")
        with pytest.raises(ValueError, match="[Ll]ine.free channels"):
            worker_baseline(self.cube_path, out_path, 0, 0, 1, None)
        assert not Path(out_path).exists(), "Output file must not be created on validation failure."

    def test_empty_list_line_free_channels_raises(self):
        """An explicit empty list must also be rejected — same as None."""
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "should_not_exist2.fits")
        with pytest.raises(ValueError, match="[Ll]ine.free channels"):
            worker_baseline(self.cube_path, out_path, 0, 0, 1, [])

    def test_poly_order_too_high_raises(self):
        """Polynomial order higher than (fit_channels - 2) must raise ValueError.

        Degree-d polynomial requires at least d+2 data points to be over-
        determined.  Providing only 2 line-free channels with order=2 must fail.
        """
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "poly_order_too_high.fits")
        # Only 2 line-free channels — insufficient for degree-2 (needs ≥ 4)
        with pytest.raises(ValueError, match="[Nn]ot enough"):
            worker_baseline(self.cube_path, out_path, 0, 0, 2, [[0, 1]])

    def test_poly_order_minimum_boundary_passes(self):
        """Exactly deg+2 line-free channels is the minimum accepted for order=1."""
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "poly_order_boundary.fits")
        # order=1 needs ≥ 3 channels; provide exactly 3
        result = worker_baseline(self.cube_path, out_path, 0, 0, 1, [[0, 2]])
        assert result["fit_channels"] == 3

    def test_out_of_range_channels_raise(self):
        """Channels outside [0, nz-1] must produce a ValueError."""
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "oob.fits")
        with pytest.raises(ValueError, match="[Oo]ut of range|[Nn]o valid"):
            # Fixture cube has 32 channels (0-31); 500 is out of range
            worker_baseline(self.cube_path, out_path, 0, 0, 1, [[500, 510]])

    def test_line_survives_with_explicit_line_free_sidebands(self):
        """Canonical scientific test: Gaussian line must survive baseline subtraction.

        Build a cube:
          - linear baseline ramp (slope=0.5 per channel)
          - Gaussian emission at channel 16 (sigma=2), peak amplitude 5
          - small Gaussian noise (σ=0.05)
        Fit baseline using line-free sidebands ch 0-10 and ch 22-31 only.
        Assert:
          1. output shape preserved
          2. line peak is detectable at ≥ 3σ above the line-free rms
          3. rms_after < rms_before (baseline was actually removed)
          4. fit_channels == 21 (= 11 + 10)
        """
        from app.compute import worker_baseline
        rng = np.random.default_rng(7)
        nz, ny, nx = 32, 4, 4
        chans = np.arange(nz, dtype=np.float32)
        slope    = 0.5 * chans[:, None, None] * np.ones((nz, ny, nx), dtype=np.float32)
        sigma    = 2.0
        line     = 5.0 * np.exp(-0.5 * ((chans - 16.0) / sigma) ** 2)
        line     = line[:, None, None] * np.ones((nz, ny, nx), dtype=np.float32)
        noise    = (rng.standard_normal((nz, ny, nx)) * 0.05).astype(np.float32)
        data     = (slope + line + noise).astype(np.float32)

        cube_path = str(self.tmp_path / "line_cube_canonical.fits")
        _write_simple_cube(cube_path, data)

        line_free = [[0, 10], [22, 31]]
        out_path  = str(self.tmp_path / "line_baseline_canonical.fits")
        result    = worker_baseline(cube_path, out_path, 0, 0, 1, line_free)

        # Shape preserved
        assert result["depth"] == nz
        assert result["fit_channels"] == 21   # 11 + 10

        with fits.open(out_path) as h:
            residual = h[0].data  # (nz, ny, nx)

        # Line survived: peak SNR > 3σ
        peak_mean     = float(np.mean(residual[14:19, :, :]))
        linefree_rms  = float(np.sqrt(np.mean(residual[[0, 1, 2, 29, 30, 31]] ** 2)))
        assert peak_mean > 3.0 * linefree_rms, (
            f"Line peak ({peak_mean:.3f}) ≤ 3σ ({3 * linefree_rms:.3f}): "
            "baseline subtraction absorbed the signal."
        )

        # RMS improved
        assert result["rms_after"] < result["rms_before"], (
            "rms_after must be smaller than rms_before after baseline removal."
        )

    def test_rms_after_is_not_zero_for_valid_cube(self):
        """rms_after must never be falsely 0.0 for a valid (non-trivial) cube."""
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "rms_nonzero.fits")
        result = worker_baseline(self.cube_path, out_path, 0, 0, 1, self._LF_32)
        assert result["rms_after"] > 0.0, (
            f"rms_after={result['rms_after']} — should be > 0 for a noisy cube."
        )

    def test_rms_after_is_finite(self):
        """rms_after must be a finite float — nan/inf indicate a broken result."""
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "rms_finite.fits")
        result = worker_baseline(self.cube_path, out_path, 0, 0, 1, self._LF_32)
        assert np.isfinite(result["rms_after"]), (
            f"rms_after={result['rms_after']} is not finite."
        )

    def test_median_method_requires_line_free(self):
        """Median method must also require explicit line-free channels."""
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "median_no_lf.fits")
        with pytest.raises(ValueError, match="[Ll]ine.free channels"):
            worker_baseline(self.cube_path, out_path, 0, 0, 0, None, method="median")

    def test_median_method_with_line_free(self):
        """Median baseline with explicit line-free channels must succeed."""
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "median_lf.fits")
        result = worker_baseline(
            self.cube_path, out_path, 0, 0, 0, self._LF_32, method="median"
        )
        assert result["depth"] == 32
        assert result["rms_after"] > 0.0


# ── S-04: Spectral stacking ────────────────────────────────────────────────────

class TestStackWorker:

    @pytest.fixture(autouse=True)
    def cubes(self, tmp_path):
        self.tmp_path = tmp_path
        nz, ny, nx = 16, 4, 4
        self.paths = []
        self.arrays = []
        for i in range(3):
            rng = np.random.default_rng(i)
            data = rng.standard_normal((nz, ny, nx)).astype(np.float32) + float(i)
            path = str(tmp_path / f"stack_{i}.fits")
            _write_simple_cube(path, data)
            self.paths.append(path)
            self.arrays.append(data)
        self.nz = nz

    def _expected_mean_spectrum(self) -> np.ndarray:
        specs = [np.nanmean(a.reshape(a.shape[0], -1), axis=1) for a in self.arrays]
        return np.mean(np.stack(specs, axis=0), axis=0)

    def test_mean_identity(self):
        """Stack mean should equal the plain mean of collapsed per-cube spectra."""
        from app.compute import worker_stack
        result = worker_stack(self.paths, "mean", None)
        arr = _decode_f32(result["data_base64"], (1, self.nz))[0]
        expected = self._expected_mean_spectrum()
        np.testing.assert_allclose(arr, expected.astype(np.float32), rtol=1e-4)

    def test_median_method(self):
        from app.compute import worker_stack
        result = worker_stack(self.paths, "median", None)
        assert result["method"] == "median"
        arr = _decode_f32(result["data_base64"], (1, self.nz))[0]
        assert arr.shape == (self.nz,)

    def test_weighted_mean(self):
        """Weighted mean with equal weights should equal the simple mean."""
        from app.compute import worker_stack
        weights = [1.0, 1.0, 1.0]
        result_w = worker_stack(self.paths, "weighted_mean", weights)
        result_m = worker_stack(self.paths, "mean", None)
        arr_w = _decode_f32(result_w["data_base64"], (1, self.nz))[0]
        arr_m = _decode_f32(result_m["data_base64"], (1, self.nz))[0]
        np.testing.assert_allclose(arr_w, arr_m, rtol=1e-5)

    def test_shape_rows_one(self):
        """Output must always have rows=1."""
        from app.compute import worker_stack
        result = worker_stack(self.paths, "mean", None)
        assert result["rows"] == 1
        assert result["cols"] == self.nz

    def test_invalid_method_raises(self):
        from app.compute import worker_stack
        with pytest.raises(ValueError, match="Unknown stacking method"):
            worker_stack(self.paths, "foobar", None)

    def test_empty_paths_raises(self):
        from app.compute import worker_stack
        with pytest.raises(ValueError, match="No input paths"):
            worker_stack([], "mean", None)


# ── S-05: SoFiA-2 wrapper (HTTP layer) ────────────────────────────────────────

@pytest.mark.asyncio
class TestSourceFindEndpoint:
    """Tests for /v1/sourcefind/{dataset_id}."""

    async def _open_cube(self, client, headers, cube_path) -> tuple[str, dict]:
        resp = await client.post(
            "/v1/datasets/open",
            json={"path": str(cube_path)},
            headers=headers,
        )
        data = resp.json()
        session_id = data["session_id"]
        full_headers = {**headers, "X-Visivo-Session": session_id}
        return data["dataset_id"], full_headers

    async def test_sofia_absent_returns_503(
        self, client, auth_headers, synthetic_fits_cube
    ):
        """When sofia2 is not on PATH the endpoint must return 503."""
        dataset_id, full_headers = await self._open_cube(
            client, auth_headers, synthetic_fits_cube
        )
        with patch("shutil.which", return_value=None):
            resp = await client.post(
                f"/v1/sourcefind/{dataset_id}",
                json={"dataset_id": dataset_id},
                headers=full_headers,
            )
        assert resp.status_code == 503
        assert "SoFiA" in resp.json()["detail"]

    async def test_sofia_present_success_mock(
        self, client, auth_headers, synthetic_fits_cube, tmp_path
    ):
        """When sofia2 returns exit code 0 and writes a VOTable, parse it."""
        import xml.etree.ElementTree as ET

        # Build a minimal VOTable catalogue
        vot_xml = """<?xml version="1.0" encoding="UTF-8"?>
<VOTABLE version="1.3" xmlns="http://www.ivoa.net/xml/VOTable/v1.3">
 <RESOURCE>
  <TABLE name="sofia_cat">
   <FIELD name="id" datatype="int"/>
   <FIELD name="x" datatype="float"/>
   <DATA><TABLEDATA>
    <TR><TD>1</TD><TD>32.5</TD></TR>
   </TABLEDATA></DATA>
  </TABLE>
 </RESOURCE>
</VOTABLE>"""

        dataset_id, full_headers = await self._open_cube(
            client, auth_headers, synthetic_fits_cube
        )

        def _fake_run(cmd, **kwargs):
            # Write the fake VOTable to the tmpdir in cmd args
            par_path = Path(cmd[1])
            out_dir = None
            for line in par_path.read_text().splitlines():
                if line.startswith("output.directory"):
                    out_dir = line.split("=", 1)[1].strip()
            if out_dir:
                (Path(out_dir) / "sofia_out_cat.xml").write_text(vot_xml)
            proc = MagicMock()
            proc.returncode = 0
            proc.stderr = ""
            return proc

        with patch("shutil.which", return_value="/usr/bin/sofia2"), \
             patch("subprocess.run", side_effect=_fake_run):
            resp = await client.post(
                f"/v1/sourcefind/{dataset_id}",
                json={"dataset_id": dataset_id},
                headers=full_headers,
            )
        assert resp.status_code == 200
        data = resp.json()
        assert data["valid"]
        assert data["n_sources"] == 1

    async def test_non_cube_returns_422(
        self, client, auth_headers, opened_image
    ):
        """Source finding on a 2D image must return 422."""
        resp = await client.post(
            f"/v1/sourcefind/{opened_image['dataset_id']}",
            json={"dataset_id": opened_image["dataset_id"]},
            headers=opened_image["headers"],
        )
        assert resp.status_code == 422


# ── S-02 / S-04 binary endpoint smoke tests ───────────────────────────────────

@pytest.mark.asyncio
class TestSpectralBinaryEndpoints:

    async def _open_cube(self, client, headers, cube_path) -> tuple[str, dict]:
        resp = await client.post(
            "/v1/datasets/open",
            json={"path": str(cube_path)},
            headers=headers,
        )
        data = resp.json()
        session_id = data["session_id"]
        return data["dataset_id"], {**headers, "X-Visivo-Session": session_id}

    async def test_linewidth_binary_two_frames(
        self, client, auth_headers, tmp_path
    ):
        """Binary response must contain exactly two valid frames (FWHM + EW)."""
        cube_path = str(tmp_path / "lw_bin.fits")
        _write_gaussian_cube(cube_path, nz=16, ny=4, nx=4, sigma_chan=2.0)
        dataset_id, full_headers = await self._open_cube(client, auth_headers, cube_path)

        resp = await client.post(
            "/v1/spectral/linewidth/binary",
            json={"dataset_id": dataset_id, "channel_start": 0, "channel_end": 15},
            headers=full_headers,
        )
        assert resp.status_code == 200
        raw = resp.content
        fwhm_arr, off = _parse_binary_frame(raw, 0)
        ew_arr, _ = _parse_binary_frame(raw, off)
        assert fwhm_arr.shape == (4, 4)
        assert ew_arr.shape == (4, 4)

    async def test_stack_binary_single_frame(
        self, client, auth_headers, tmp_path
    ):
        """Stack binary response must be a single frame with rows=1."""
        paths, ids, headers_list = [], [], []
        for i in range(2):
            cube_path = str(tmp_path / f"stk_{i}.fits")
            rng = np.random.default_rng(i)
            data = rng.standard_normal((8, 4, 4)).astype(np.float32)
            _write_simple_cube(cube_path, data)
            resp = await client.post(
                "/v1/datasets/open",
                json={"path": cube_path},
                headers=auth_headers,
            )
            d = resp.json()
            paths.append(cube_path)
            ids.append(d["dataset_id"])
            if i == 0:
                session_id = d["session_id"]

        full_headers = {**auth_headers, "X-Visivo-Session": session_id}
        # Re-open second cube in same session
        resp2 = await client.post(
            "/v1/datasets/open",
            json={"path": paths[1]},
            headers=full_headers,
        )
        ids[1] = resp2.json()["dataset_id"]

        resp = await client.post(
            "/v1/spectral/stack/binary",
            json={"dataset_ids": ids, "method": "mean"},
            headers=full_headers,
        )
        assert resp.status_code == 200
        arr, _ = _parse_binary_frame(resp.content, 0)
        assert arr.shape == (1, 8)
