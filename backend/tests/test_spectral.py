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


# ── S-02: Line-width maps ──────────────────────────────────────────────────────

class TestLinewidthWorker:

    @pytest.fixture(autouse=True)
    def cube_path(self, tmp_path):
        path = str(tmp_path / "lw_cube.fits")
        self.sigma_chan = 3.0
        _write_gaussian_cube(path, nz=32, ny=4, nx=4, sigma_chan=self.sigma_chan)
        self.cube_path = path
        # astropy converts CDELT3=1.0 km/s to SI (m/s) → 1000 m/s per channel
        self.expected_fwhm = 2.3548 * self.sigma_chan * 1000.0  # m/s

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

    def test_baseline_rms_reduced(self):
        """After linear baseline subtraction the RMS should drop significantly."""
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_out.fits")
        result = worker_baseline(self.cube_path, out_path, 0, 31, 1, None)
        assert result["output_path"] == out_path
        with fits.open(out_path) as h:
            residual = h[0].data
        # The linear slope contributes ~0.75 in RMS; after subtraction only noise remains
        assert float(np.nanstd(residual)) < 0.1

    def test_output_file_written(self):
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_out2.fits")
        worker_baseline(self.cube_path, out_path, 0, 31, 1, None)
        assert Path(out_path).exists()

    def test_metadata_returned(self):
        from app.compute import worker_baseline
        out_path = str(self.tmp_path / "baseline_out3.fits")
        result = worker_baseline(self.cube_path, out_path, 0, 31, 1, None)
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
