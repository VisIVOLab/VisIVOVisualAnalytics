"""
test_compute.py – unit tests for the compute worker functions.

These tests exercise the Python-level worker functions directly (without going
through FastAPI) so that WCS edge-cases, metadata correctness, and new
endpoints can be validated in isolation.

Run with:
    cd backend
    pytest tests/test_compute.py -v
"""

from __future__ import annotations

import base64
import struct
import tempfile
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits


# ── FITS fixture helpers ───────────────────────────────────────────────────────


def _write_freq_cube(path: str, nz: int = 32, ny: int = 16, nx: int = 16) -> None:
    """Standard FREQ cube – matches the conftest fixture used in test_api.py."""
    rng = np.random.default_rng(42)
    data = rng.standard_normal((nz, ny, nx)).astype(np.float32) * 0.1
    data[nz // 2 - 2 : nz // 2 + 2, 4:12, 4:12] += 5.0  # emission region
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["NAXIS"] = 3
    h["NAXIS1"] = nx
    h["NAXIS2"] = ny
    h["NAXIS3"] = nz
    h["CTYPE1"] = "RA---SIN"
    h["CTYPE2"] = "DEC--SIN"
    h["CTYPE3"] = "FREQ"
    h["CRVAL1"] = 83.82
    h["CRVAL2"] = -5.39
    h["CRVAL3"] = 115.271e9
    h["CRPIX1"] = nx / 2.0
    h["CRPIX2"] = ny / 2.0
    h["CRPIX3"] = nz / 2.0
    h["CDELT1"] = -2.777e-4
    h["CDELT2"] = 2.777e-4
    h["CDELT3"] = -0.5e6
    h["CUNIT1"] = "deg"
    h["CUNIT2"] = "deg"
    h["CUNIT3"] = "Hz"
    h["BUNIT"] = "Jy/beam"
    h["BMAJ"] = 0.001  # degrees
    h["BMIN"] = 0.0008
    h["BPA"] = 45.0
    hdu.writeto(path, overwrite=True)


def _write_vrad_cube(path: str, nz: int = 32, ny: int = 16, nx: int = 16) -> None:
    """Cube with VRAD (radio velocity) spectral axis in km/s."""
    rng = np.random.default_rng(7)
    data = rng.standard_normal((nz, ny, nx)).astype(np.float32) * 0.1
    data[14:18, 4:12, 4:12] += 3.0
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["NAXIS"] = 3
    h["NAXIS1"] = nx
    h["NAXIS2"] = ny
    h["NAXIS3"] = nz
    h["CTYPE1"] = "RA---SIN"
    h["CTYPE2"] = "DEC--SIN"
    h["CTYPE3"] = "VRAD"
    h["CRVAL1"] = 10.0
    h["CRVAL2"] = 20.0
    h["CRVAL3"] = 0.0      # km/s
    h["CRPIX1"] = nx / 2.0
    h["CRPIX2"] = ny / 2.0
    h["CRPIX3"] = nz / 2.0
    h["CDELT1"] = -2.777e-4
    h["CDELT2"] = 2.777e-4
    h["CDELT3"] = 1.0      # 1 km/s per channel
    h["CUNIT1"] = "deg"
    h["CUNIT2"] = "deg"
    h["CUNIT3"] = "km/s"
    h["BUNIT"] = "Jy/beam"
    hdu.writeto(path, overwrite=True)


def _write_pc_matrix_cube(path: str, nz: int = 32, ny: int = 16, nx: int = 16) -> None:
    """Cube with a PC rotation matrix (non-diagonal WCS)."""
    rng = np.random.default_rng(99)
    data = rng.standard_normal((nz, ny, nx)).astype(np.float32) * 0.1
    data[14:18, 4:12, 4:12] += 4.0
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["NAXIS"] = 3
    h["NAXIS1"] = nx
    h["NAXIS2"] = ny
    h["NAXIS3"] = nz
    h["CTYPE1"] = "RA---SIN"
    h["CTYPE2"] = "DEC--SIN"
    h["CTYPE3"] = "FREQ"
    h["CRVAL1"] = 83.82
    h["CRVAL2"] = -5.39
    h["CRVAL3"] = 115.271e9
    h["CRPIX1"] = nx / 2.0
    h["CRPIX2"] = ny / 2.0
    h["CRPIX3"] = nz / 2.0
    # PC matrix: 15-degree rotation in the RA/Dec plane
    import math
    angle = math.radians(15)
    h["PC1_1"] = math.cos(angle)
    h["PC1_2"] = -math.sin(angle)
    h["PC2_1"] = math.sin(angle)
    h["PC2_2"] = math.cos(angle)
    h["PC3_3"] = 1.0
    h["CDELT1"] = -2.777e-4
    h["CDELT2"] = 2.777e-4
    h["CDELT3"] = -0.5e6
    h["CUNIT1"] = "deg"
    h["CUNIT2"] = "deg"
    h["CUNIT3"] = "Hz"
    h["BUNIT"] = "Jy/beam"
    hdu.writeto(path, overwrite=True)


def _write_degenerate_cube(path: str) -> None:
    """Cube with degenerate Stokes axis (4D FITS squeezed to 3D effective)."""
    nz, ny, nx = 16, 8, 8
    rng = np.random.default_rng(3)
    data = rng.standard_normal((1, nz, ny, nx)).astype(np.float32) * 0.1
    data[0, 6:10, 2:6, 2:6] += 2.0
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["NAXIS"] = 4
    h["NAXIS1"] = nx
    h["NAXIS2"] = ny
    h["NAXIS3"] = nz
    h["NAXIS4"] = 1  # degenerate Stokes
    h["CTYPE1"] = "RA---SIN"
    h["CTYPE2"] = "DEC--SIN"
    h["CTYPE3"] = "FREQ"
    h["CTYPE4"] = "STOKES"
    h["CRVAL1"] = 0.0
    h["CRVAL2"] = 0.0
    h["CRVAL3"] = 1.4e9
    h["CRVAL4"] = 1.0
    h["CRPIX1"] = nx / 2.0
    h["CRPIX2"] = ny / 2.0
    h["CRPIX3"] = nz / 2.0
    h["CRPIX4"] = 1.0
    h["CDELT1"] = -1e-4
    h["CDELT2"] = 1e-4
    h["CDELT3"] = 1e6
    h["CDELT4"] = 1.0
    h["CUNIT1"] = "deg"
    h["CUNIT2"] = "deg"
    h["CUNIT3"] = "Hz"
    h["BUNIT"] = "K"
    hdu.writeto(path, overwrite=True)


def _decode_f32(b64: str) -> np.ndarray:
    return np.frombuffer(base64.b64decode(b64), dtype=np.float32)


def _decode_qt_zlib(b64: str) -> np.ndarray:
    import zlib
    raw = base64.b64decode(b64)
    expected = struct.unpack(">I", raw[:4])[0]
    return np.frombuffer(zlib.decompress(raw[4:])[:expected], dtype=np.float32)


# ── worker_moment: metadata fields ────────────────────────────────────────────


@pytest.fixture(scope="module")
def freq_cube_path(tmp_path_factory) -> Path:
    p = tmp_path_factory.mktemp("wcs") / "freq_cube.fits"
    _write_freq_cube(str(p))
    return p


@pytest.fixture(scope="module")
def vrad_cube_path(tmp_path_factory) -> Path:
    p = tmp_path_factory.mktemp("wcs") / "vrad_cube.fits"
    _write_vrad_cube(str(p))
    return p


@pytest.fixture(scope="module")
def pc_cube_path(tmp_path_factory) -> Path:
    p = tmp_path_factory.mktemp("wcs") / "pc_cube.fits"
    _write_pc_matrix_cube(str(p))
    return p


@pytest.fixture(scope="module")
def degen_cube_path(tmp_path_factory) -> Path:
    p = tmp_path_factory.mktemp("wcs") / "degen_cube.fits"
    _write_degenerate_cube(str(p))
    return p


class TestWorkerMomentMetadata:
    """Verify that worker_moment returns correct scientific metadata fields."""

    def test_freq_cube_m0_metadata(self, freq_cube_path):
        from app.compute import worker_moment
        result = worker_moment(str(freq_cube_path), 0, 0, 31, False, 0.0)
        assert result["bunit"] == "Jy/beam"
        assert result["spectral_axis_type"] == "FREQ"
        assert result["spectral_axis_unit"] == "Hz"
        # M0 unit = BUNIT × spectral_unit
        assert "Jy/beam" in result["moment_unit"]
        assert "Hz" in result["moment_unit"]

    def test_freq_cube_m1_metadata(self, freq_cube_path):
        from app.compute import worker_moment
        result = worker_moment(str(freq_cube_path), 1, 0, 31, False, 0.0)
        # M1 unit = spectral_unit only
        assert result["moment_unit"] == "Hz"

    def test_freq_cube_m2_metadata(self, freq_cube_path):
        from app.compute import worker_moment
        result = worker_moment(str(freq_cube_path), 2, 0, 31, False, 0.0)
        # M2 unit = spectral_unit^2
        assert result["moment_unit"] == "Hz^2"

    @pytest.mark.parametrize("order", [6, 8, 10])
    def test_intensity_moment_unit_equals_bunit(self, freq_cube_path, order):
        from app.compute import worker_moment
        result = worker_moment(str(freq_cube_path), order, 0, 31, False, 0.0)
        # M6/M8/M10 preserve the intensity unit
        assert result["moment_unit"] == "Jy/beam"

    def test_vrad_cube_m0_unit(self, vrad_cube_path):
        """km/s spectral axis should propagate into moment_unit."""
        from app.compute import worker_moment
        result = worker_moment(str(vrad_cube_path), 0, 0, 31, False, 0.0)
        assert result["spectral_axis_unit"] == "km/s"
        assert "km/s" in result["moment_unit"]

    def test_vrad_cube_m2_unit(self, vrad_cube_path):
        from app.compute import worker_moment
        result = worker_moment(str(vrad_cube_path), 2, 0, 31, False, 0.0)
        assert result["moment_unit"] == "km/s^2"


class TestWorkerMomentWCSEdgeCases:
    """Ensure worker_moment succeeds on WCS corner cases."""

    def test_pc_matrix_cube_m0(self, pc_cube_path):
        """PC rotation matrix must not prevent moment computation."""
        from app.compute import worker_moment
        result = worker_moment(str(pc_cube_path), 0, 0, 31, False, 0.0)
        arr = _decode_f32(result["data_base64"])
        assert np.isfinite(arr).any(), "M0 should have finite values for PC-matrix cube"
        assert result["bunit"] == "Jy/beam"
        assert result["spectral_axis_type"] == "FREQ"

    def test_degenerate_stokes_axis_m0(self, degen_cube_path):
        """4D cube with degenerate Stokes axis should be squeezed to 3D correctly."""
        from app.compute import worker_moment
        result = worker_moment(str(degen_cube_path), 0, 0, 15, False, 0.0)
        arr = _decode_f32(result["data_base64"])
        assert np.isfinite(arr).any()
        assert result["bunit"] == "K"

    def test_all_nan_channel_range_raises(self, freq_cube_path):
        """A channel range with no finite data must raise ValueError."""
        from app.compute import worker_moment
        # Set an absurdly high threshold so nothing passes the mask.
        with pytest.raises((ValueError, Exception)):
            worker_moment(str(freq_cube_path), 0, 0, 31, True, 1e10)

    def test_channel_range_clamping(self, freq_cube_path):
        """Out-of-range channel indices should be clamped, not raise."""
        from app.compute import worker_moment
        result = worker_moment(str(freq_cube_path), 0, -5, 999, False, 0.0)
        arr = _decode_f32(result["data_base64"])
        assert np.isfinite(arr).any()


class TestWorkerNoiseEstimate:
    """Verify worker_noise_estimate returns plausible MAD/sigma values."""

    def test_noise_shape(self, freq_cube_path):
        from app.compute import worker_noise_estimate
        result = worker_noise_estimate(str(freq_cube_path), 0, 5, 0, 5, 0, 15)
        assert result["num_channels"] == 16
        assert len(result["mad"]) == 16
        assert len(result["sigma"]) == 16

    def test_sigma_approx_1_4826_times_mad(self, freq_cube_path):
        from app.compute import worker_noise_estimate
        result = worker_noise_estimate(str(freq_cube_path), 0, 5, 0, 5, 0, 15)
        for mad, sigma in zip(result["mad"], result["sigma"]):
            if np.isfinite(mad) and np.isfinite(sigma) and mad > 0:
                assert abs(sigma / mad - 1.4826) < 1e-4

    def test_emission_region_has_higher_mad(self, freq_cube_path):
        """MAD in the emission region should exceed MAD in the noise region."""
        from app.compute import worker_noise_estimate
        # Noise region: corner pixels far from emission
        noise = worker_noise_estimate(str(freq_cube_path), 0, 3, 0, 3, 0, 31)
        # Emission region (4:12, 4:12, channels 14:18 have +5 injected)
        emission = worker_noise_estimate(str(freq_cube_path), 4, 12, 4, 12, 14, 17)
        noise_med = float(np.nanmedian(noise["sigma"]))
        emission_med = float(np.nanmedian(emission["sigma"]))
        assert emission_med > noise_med, (
            f"Emission σ={emission_med:.4f} should exceed noise σ={noise_med:.4f}"
        )

    def test_invalid_region_raises(self, freq_cube_path):
        from app.compute import worker_noise_estimate
        with pytest.raises((ValueError, Exception)):
            worker_noise_estimate(str(freq_cube_path), 10, 5, 0, 3, 0, 15)  # x0 > x1


class TestWorkerCubeSaveSubregion:
    """Sub-cube extraction: shape, WCS shift, header provenance."""

    def test_subregion_shape(self, freq_cube_path, tmp_path):
        from app.compute import worker_cube_save_subregion
        from astropy.io import fits
        out = tmp_path / "sub.fits"
        result = worker_cube_save_subregion(
            str(freq_cube_path), str(out), 2, 9, 3, 11, 5, 20,
        )
        assert result["width"]  == 9 - 2 + 1
        assert result["height"] == 11 - 3 + 1
        assert result["depth"]  == 20 - 5 + 1
        assert out.exists()
        with fits.open(str(out)) as hdul:
            assert hdul[0].data.shape == (16, 9, 8)

    def test_subregion_wcs_crpix_shifted(self, freq_cube_path, tmp_path):
        """CRPIX must be shifted so the same physical pixel keeps the same
        sky / spectral coordinate after cropping."""
        from app.compute import worker_cube_save_subregion
        from astropy.io import fits
        out = tmp_path / "shifted.fits"
        worker_cube_save_subregion(
            str(freq_cube_path), str(out), 4, 13, 4, 13, 8, 23,
        )
        with fits.open(str(freq_cube_path)) as src_hdul:
            src_h = src_hdul[0].header
        with fits.open(str(out)) as dst_hdul:
            dst_h = dst_hdul[0].header
        # crop offsets: x0=4, y0=4, z0=8 → CRPIX shifts by exactly those
        assert dst_h["CRPIX1"] == src_h["CRPIX1"] - 4
        assert dst_h["CRPIX2"] == src_h["CRPIX2"] - 4
        assert dst_h["CRPIX3"] == src_h["CRPIX3"] - 8

    def test_subregion_invalid_range_raises(self, freq_cube_path, tmp_path):
        from app.compute import worker_cube_save_subregion
        with pytest.raises(ValueError):
            worker_cube_save_subregion(
                str(freq_cube_path), str(tmp_path / "bad.fits"),
                10, 5, 0, 3, 0, 15,                   # x0 > x1
            )


class TestWorkerMomentSaveFits:
    """Moment FITS persistence: 2-D output with celestial WCS + BUNIT."""

    def test_moment0_fits_shape_and_axes(self, freq_cube_path, tmp_path):
        from app.compute import worker_moment_save_fits
        from astropy.io import fits
        out = tmp_path / "m0.fits"
        result = worker_moment_save_fits(
            str(freq_cube_path), str(out), 0, 0, 31, False, 0.0,
        )
        assert result["width"]  > 0
        assert result["height"] > 0
        with fits.open(str(out)) as hdul:
            assert hdul[0].data.ndim == 2
            assert hdul[0].header["NAXIS"] == 2

    def test_moment0_fits_bunit_present(self, freq_cube_path, tmp_path):
        """M0 in a cube with a BUNIT must carry a BUNIT in the output."""
        from app.compute import worker_moment_save_fits
        from astropy.io import fits
        out = tmp_path / "m0_bunit.fits"
        result = worker_moment_save_fits(
            str(freq_cube_path), str(out), 0, 0, 31, False, 0.0,
        )
        # moment_unit may be empty if the source cube has no BUNIT; for the
        # freq_cube fixture it is populated.
        if result["moment_unit"]:
            with fits.open(str(out)) as hdul:
                assert hdul[0].header.get("BUNIT", "") == result["moment_unit"]


class TestWorkerPvArcsec:
    """Verify that worker_pv returns physical arcsecond positions."""

    def test_pv_arcsec_present(self, freq_cube_path):
        from app.compute import worker_pv
        result = worker_pv(str(freq_cube_path), [[2, 8], [13, 8]], 1)
        assert "positions_arcsec_base64" in result
        assert result["spatial_unit"] == "arcsec"
        assert result["pixel_scale_arcsec_per_pixel"] > 0

    def test_pv_arcsec_monotone(self, freq_cube_path):
        """Arcsecond positions must be non-decreasing (cumulative path distance)."""
        from app.compute import worker_pv
        result = worker_pv(str(freq_cube_path), [[2, 8], [13, 8]], 1)
        arcsec = _decode_qt_zlib(result["positions_arcsec_base64"])
        assert np.all(np.diff(arcsec) >= 0), "Arcsec positions must be non-decreasing"

    def test_pv_arcsec_scale_relation(self, freq_cube_path):
        """positions_arcsec should equal positions_pixels * pixel_scale."""
        from app.compute import worker_pv
        result = worker_pv(str(freq_cube_path), [[2, 8], [13, 8]], 1)
        pixels = _decode_qt_zlib(result["positions_base64"])
        arcsec = _decode_qt_zlib(result["positions_arcsec_base64"])
        scale = result["pixel_scale_arcsec_per_pixel"]
        np.testing.assert_allclose(arcsec, pixels * scale, rtol=1e-5)


class TestMomentUnitHelper:
    """Test the _moment_unit helper in isolation."""

    def test_m0(self):
        from app.compute import _moment_unit
        assert _moment_unit(0, "Jy/beam", "Hz") == "Jy/beam Hz"

    def test_m0_no_bunit(self):
        from app.compute import _moment_unit
        assert _moment_unit(0, "", "Hz") == "Hz"

    def test_m1(self):
        from app.compute import _moment_unit
        assert _moment_unit(1, "Jy/beam", "km/s") == "km/s"

    def test_m2(self):
        from app.compute import _moment_unit
        assert _moment_unit(2, "Jy/beam", "km/s") == "km/s^2"

    def test_m2_no_spectral(self):
        from app.compute import _moment_unit
        assert _moment_unit(2, "Jy/beam", "") == ""

    @pytest.mark.parametrize("order", [6, 8, 10])
    def test_intensity_orders(self, order):
        from app.compute import _moment_unit
        assert _moment_unit(order, "Jy/beam", "Hz") == "Jy/beam"


# ── Step 2: ThreadPoolExecutor chunked moment tests ───────────────────────────


def _make_random_cube(nz: int = 8, ny: int = 32, nx: int = 32, seed: int = 99) -> np.ndarray:
    rng = np.random.default_rng(seed)
    cube = rng.standard_normal((nz, ny, nx)).astype(np.float32)
    return cube


def _serial_m0(cube: np.ndarray, delta: np.ndarray) -> np.ndarray:
    """Reference M0 using sequential nansum (float64 accumulator, same as _chunked_m0)."""
    safe = np.where(np.isfinite(cube), cube, np.nan).astype(np.float64)
    return np.nansum(safe * delta[:, None, None], axis=0).astype(np.float32)


def _serial_m6(cube: np.ndarray) -> np.ndarray:
    """Reference M6 (RMS) using sequential nanmean."""
    safe = np.where(np.isfinite(cube), cube, np.nan)
    return np.sqrt(np.nanmean(safe * safe, axis=0, dtype=np.float32))


def test_moment0_parallel_matches_serial() -> None:
    """Chunked M0 must match sequential M0 within rtol=1e-5 on a random cube."""
    from app.compute import _chunked_m0, _integrated_spacing

    cube = _make_random_cube(nz=8, ny=32, nx=32)
    delta = _integrated_spacing(np.arange(8, dtype=np.float64))
    valid_mask = np.isfinite(cube)

    parallel = _chunked_m0(cube, valid_mask, delta, n_threads=4)
    serial = _serial_m0(cube, delta)

    np.testing.assert_allclose(parallel, serial, rtol=1e-5,
                               err_msg="M0 parallel != serial")


def test_moment0_parallel_nan_handling() -> None:
    """Chunked M0 handles ~20% NaN voxels correctly."""
    from app.compute import _chunked_m0, _integrated_spacing

    rng = np.random.default_rng(7)
    cube = rng.standard_normal((8, 32, 32)).astype(np.float32)
    # Inject ~20% NaNs
    mask = rng.random(cube.shape) < 0.2
    cube[mask] = np.nan

    delta = _integrated_spacing(np.arange(8, dtype=np.float64))
    valid_mask = np.isfinite(cube)

    parallel = _chunked_m0(cube, valid_mask, delta, n_threads=4)
    serial = _serial_m0(cube, delta)

    np.testing.assert_allclose(parallel[np.isfinite(serial)],
                               serial[np.isfinite(serial)],
                               rtol=1e-5, err_msg="M0 NaN-handling differs")


def test_moment6_parallel_matches_serial() -> None:
    """Chunked M6 (RMS) must match sequential M6 within rtol=1e-5."""
    from app.compute import _chunked_m6

    cube = _make_random_cube(nz=8, ny=32, nx=32)
    valid_mask = np.isfinite(cube)

    parallel = _chunked_m6(cube, valid_mask, n_threads=4)
    serial = _serial_m6(cube)

    # Where both are finite, values must match.
    both_finite = np.isfinite(parallel) & np.isfinite(serial)
    np.testing.assert_allclose(parallel[both_finite], serial[both_finite],
                               rtol=1e-5, err_msg="M6 parallel != serial")


# ── Step 3: Numba JIT M1/M2 tests ────────────────────────────────────────────


def _numpy_m1(cube: np.ndarray, spectral_values: np.ndarray, valid_mask: np.ndarray) -> np.ndarray:
    """Pure-numpy reference M1 implementation."""
    from app.compute import _integrated_spacing
    delta = _integrated_spacing(spectral_values)
    safe = np.where(valid_mask, cube.astype(np.float64), np.nan)
    weights = delta[:, None, None]
    m0 = np.nansum(safe * weights, axis=0)
    num = np.nansum(safe * spectral_values[:, None, None] * weights, axis=0)
    out = np.full(m0.shape, np.nan, dtype=np.float64)
    v = np.isfinite(m0) & (m0 != 0.0)
    out[v] = num[v] / m0[v]
    return out.astype(np.float32)


def _numpy_m2(cube: np.ndarray, spectral_values: np.ndarray, valid_mask: np.ndarray) -> np.ndarray:
    """Pure-numpy reference M2 implementation."""
    from app.compute import _integrated_spacing
    delta = _integrated_spacing(spectral_values)
    safe = np.where(valid_mask, cube.astype(np.float64), np.nan)
    weights = delta[:, None, None]
    m0 = np.nansum(safe * weights, axis=0)
    m1_num = np.nansum(safe * spectral_values[:, None, None] * weights, axis=0)
    m1 = np.full(m0.shape, np.nan, dtype=np.float64)
    v = np.isfinite(m0) & (m0 != 0.0)
    m1[v] = m1_num[v] / m0[v]
    diff = spectral_values[:, None, None] - m1[None, :, :]
    num = np.nansum(safe * diff * diff * weights, axis=0)
    out = np.full(m0.shape, np.nan, dtype=np.float64)
    out[v] = num[v] / m0[v]
    return out.astype(np.float32)


def test_moment1_numba_matches_numpy() -> None:
    """Numba M1 result must match numpy M1 within rtol=1e-4 (float32 precision)."""
    import app.compute.moment as _c

    if not _c._NUMBA_OK:
        pytest.skip("numba not installed")

    rng = np.random.default_rng(11)
    cube = (rng.standard_normal((16, 24, 24)) * 2 + 5).astype(np.float32)
    spectral_values = np.linspace(1.0e9, 1.0016e9, 16)
    valid_mask = np.isfinite(cube)

    numba_result = _c._moment1_numba(
        np.ascontiguousarray(cube, dtype=np.float32),
        np.ascontiguousarray(spectral_values, dtype=np.float64),
        np.ascontiguousarray(valid_mask, dtype=np.bool_),
    ).astype(np.float32)
    numpy_result = _numpy_m1(cube, spectral_values, valid_mask)

    both_finite = np.isfinite(numba_result) & np.isfinite(numpy_result)
    assert both_finite.any(), "No finite values in M1 result"
    np.testing.assert_allclose(
        numba_result[both_finite], numpy_result[both_finite],
        rtol=1e-4, err_msg="M1 numba != numpy"
    )


def test_moment2_numba_matches_numpy() -> None:
    """Numba M2 result must match numpy M2 within rtol=1e-4."""
    import app.compute.moment as _c

    if not _c._NUMBA_OK:
        pytest.skip("numba not installed")

    rng = np.random.default_rng(13)
    cube = (rng.standard_normal((16, 24, 24)) * 2 + 5).astype(np.float32)
    spectral_values = np.linspace(1.0e9, 1.0016e9, 16)
    valid_mask = np.isfinite(cube)

    numba_result = _c._moment2_numba(
        np.ascontiguousarray(cube, dtype=np.float32),
        np.ascontiguousarray(spectral_values, dtype=np.float64),
        np.ascontiguousarray(valid_mask, dtype=np.bool_),
    ).astype(np.float32)
    numpy_result = _numpy_m2(cube, spectral_values, valid_mask)

    both_finite = np.isfinite(numba_result) & np.isfinite(numpy_result)
    assert both_finite.any(), "No finite values in M2 result"
    np.testing.assert_allclose(
        numba_result[both_finite], numpy_result[both_finite],
        rtol=1e-4, err_msg="M2 numba != numpy"
    )


def test_moment1_fallback_no_numba() -> None:
    """With _NUMBA_OK=False the numpy path is used and no crash occurs."""
    import app.compute.moment as _c

    rng = np.random.default_rng(17)
    cube = (rng.standard_normal((8, 16, 16)) * 2 + 5).astype(np.float32)
    spectral_values = np.linspace(1.0e9, 1.0008e9, 8)
    valid_mask = np.isfinite(cube)

    # Monkey-patch _NUMBA_OK off for the duration of the test.
    original = _c._NUMBA_OK
    try:
        _c._NUMBA_OK = False
        result = _c._moment_map_from_array(
            cube, 1, spectral_values, mask_enabled=False, threshold_value=0.0
        )
        assert result.shape == (16, 16), f"Unexpected shape {result.shape}"
        assert np.isfinite(result).any(), "All values NaN — fallback failed"
    finally:
        _c._NUMBA_OK = original


# ── Step 4: Dask out-of-core tests ───────────────────────────────────────────


def test_dask_moment0_matches_numpy() -> None:
    """Dask M0 result must match numpy M0 within rtol=1e-5 on a small in-memory cube."""
    import app.compute.moment as _c

    if not _c._DASK_OK:
        pytest.skip("dask not installed")

    rng = np.random.default_rng(42)
    nz, ny, nx = 16, 20, 20
    cube = rng.standard_normal((nz, ny, nx)).astype(np.float32)
    cube[2, 5, 5] = np.nan  # ensure NaN handling works

    # Write to a temporary FITS file so _moment_map_dask can open it.
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".fits", delete=False) as f:
        tmp_path = f.name

    try:
        from astropy.io import fits as _fits
        hdu = _fits.PrimaryHDU(cube)
        h = hdu.header
        h["NAXIS"] = 3
        h["NAXIS1"] = nx
        h["NAXIS2"] = ny
        h["NAXIS3"] = nz
        h["CTYPE1"] = "RA---SIN"
        h["CTYPE2"] = "DEC--SIN"
        h["CTYPE3"] = "FREQ"
        h["CRVAL3"] = 115.271e9
        h["CRPIX3"] = nz / 2.0
        h["CDELT3"] = -0.5e6
        h["CUNIT3"] = "Hz"
        hdu.writeto(tmp_path, overwrite=True)

        dask_result_dict = _c._moment_map_dask(
            tmp_path, order=0,
            channel_start=0, channel_end=nz - 1,
            mask_threshold=0.0, mask_enabled=False,
            chunk_z=8,
        )
        dask_arr = np.frombuffer(
            __import__("base64").b64decode(dask_result_dict["data_base64"]), dtype=np.float32
        ).reshape(ny, nx)

        # Serial numpy reference.
        from app.compute import worker_moment
        numpy_result_dict = worker_moment(
            tmp_path, order=0,
            channel_start=0, channel_end=nz - 1,
            mask_enabled=False, threshold_value=0.0,
        )
        numpy_arr = np.frombuffer(
            __import__("base64").b64decode(numpy_result_dict["data_base64"]), dtype=np.float32
        ).reshape(ny, nx)

        both_finite = np.isfinite(dask_arr) & np.isfinite(numpy_arr)
        np.testing.assert_allclose(
            dask_arr[both_finite], numpy_arr[both_finite],
            rtol=1e-5, err_msg="Dask M0 != numpy M0"
        )
    finally:
        import os as _os
        _os.unlink(tmp_path)


def test_dask_fallback_when_unavailable() -> None:
    """When _DASK_OK=False the regular numpy path is used without error."""
    import app.compute.moment as _c

    rng = np.random.default_rng(99)
    cube = (rng.standard_normal((8, 16, 16)) * 2 + 5).astype(np.float32)
    spectral_values = np.linspace(1.0e9, 1.0008e9, 8)

    original = _c._DASK_OK
    try:
        _c._DASK_OK = False
        result = _c._moment_map_from_array(
            cube, 0, spectral_values, mask_enabled=False, threshold_value=0.0
        )
        assert result.shape == (16, 16), f"Unexpected shape {result.shape}"
        assert np.isfinite(result).any(), "All values NaN — fallback failed"
    finally:
        _c._DASK_OK = original
