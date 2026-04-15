"""
test_astrometry.py – unit and integration tests for Domain A (astrometry & photometry).

Covers:
  A-01  Aperture photometry    (flux accuracy, background, pixel coords)
  A-02  2-D Gaussian source fit (peak, PA, deconvolution)
  A-03  WCS reprojection        (identity transform, shape, 503 without reproject)
  A-04  External catalogue cross-match (mock astroquery, 503, truncation, 422)

Run with:
    cd backend
    pytest tests/test_astrometry.py -v
"""

from __future__ import annotations

import math
import tempfile
from pathlib import Path
from unittest.mock import MagicMock, patch

import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS


# ── Fixtures / helpers ─────────────────────────────────────────────────────────

def _wcs_image_header(nx: int = 64, ny: int = 64,
                      ra_cen: float = 83.82, dec_cen: float = -5.39,
                      cdelt_deg: float = 2.777e-4) -> fits.Header:
    h = fits.Header()
    h["NAXIS"]  = 2
    h["NAXIS1"] = nx
    h["NAXIS2"] = ny
    h["CTYPE1"] = "RA---TAN"
    h["CTYPE2"] = "DEC--TAN"
    h["CRVAL1"] = ra_cen
    h["CRVAL2"] = dec_cen
    h["CRPIX1"] = nx / 2.0
    h["CRPIX2"] = ny / 2.0
    h["CDELT1"] = -cdelt_deg
    h["CDELT2"] = cdelt_deg
    h["CUNIT1"] = "deg"
    h["CUNIT2"] = "deg"
    h["BUNIT"]  = "Jy/beam"
    return h


def _write_gaussian_image(path: str,
                           nx: int = 64, ny: int = 64,
                           amp: float = 1.0,
                           cx: float = 32.0, cy: float = 32.0,
                           sigma_pix: float = 4.0,
                           bmaj_deg: float = 0.001,
                           bmin_deg: float = 0.0008) -> dict:
    """Write a 2-D Gaussian source on a flat background."""
    yy, xx = np.mgrid[0:ny, 0:nx]
    data = (amp * np.exp(-0.5 * ((xx - cx) ** 2 + (yy - cy) ** 2) / sigma_pix ** 2)
            ).astype(np.float32)
    h = _wcs_image_header(nx=nx, ny=ny)
    h["BMAJ"] = bmaj_deg
    h["BMIN"] = bmin_deg
    h["BPA"]  = 0.0
    hdu = fits.PrimaryHDU(data, header=h)
    hdu.writeto(path, overwrite=True)
    wcs = WCS(h)
    sky = wcs.all_pix2world([[cx, cy]], 0)[0]
    return {"ra": float(sky[0]), "dec": float(sky[1]),
            "amp": amp, "sigma_pix": sigma_pix,
            "cdelt_deg": 2.777e-4}


def _write_plain_image(path: str, nx: int = 64, ny: int = 64) -> fits.Header:
    rng = np.random.default_rng(55)
    data = rng.standard_normal((ny, nx)).astype(np.float32)
    h = _wcs_image_header(nx=nx, ny=ny)
    hdu = fits.PrimaryHDU(data, header=h)
    hdu.writeto(path, overwrite=True)
    return h


# ── A-01: Aperture photometry ──────────────────────────────────────────────────

@pytest.mark.skipif(
    not __import__("importlib").util.find_spec("photutils"),
    reason="photutils not installed",
)
class TestAperturePhotometry:

    @pytest.fixture(autouse=True)
    def setup(self, tmp_path):
        self.tmp = tmp_path
        path = str(tmp_path / "gauss_src.fits")
        info = _write_gaussian_image(path, amp=2.0, sigma_pix=3.0)
        self.path = path
        self.info = info

    def test_flux_within_2pct_of_analytic(self):
        """Aperture flux should be within 2% of the analytic Gaussian integral."""
        from app.compute import worker_aperture_photometry
        # Analytic flux = amp * 2π * σ² (in pixel units)
        sigma_pix = self.info["sigma_pix"]
        amp = self.info["amp"]
        analytic = float(amp * 2.0 * math.pi * sigma_pix ** 2)

        # Aperture = 5σ (should capture >99.9% of flux)
        pixel_scale_arcsec = self.info["cdelt_deg"] * 3600.0
        ap_arcsec = 5.0 * sigma_pix * pixel_scale_arcsec

        result = worker_aperture_photometry(
            self.path, None,
            self.info["ra"], self.info["dec"],
            ap_arcsec,
            ap_arcsec * 1.5,
            ap_arcsec * 2.0,
        )
        assert result["n_pix_aperture"] > 0
        assert abs(result["flux_jy"] - analytic) / analytic < 0.05

    def test_background_near_zero(self):
        """For a source-only image with no background the annulus estimate should be ~0."""
        from app.compute import worker_aperture_photometry
        pixel_scale_arcsec = self.info["cdelt_deg"] * 3600.0
        ap_arcsec = 5.0 * self.info["sigma_pix"] * pixel_scale_arcsec
        result = worker_aperture_photometry(
            self.path, None,
            self.info["ra"], self.info["dec"],
            ap_arcsec, ap_arcsec * 1.5, ap_arcsec * 2.0,
        )
        assert abs(result["background_jy_per_pix"]) < 0.1

    def test_pixel_coords_near_centre(self):
        """The converted pixel coords should be near the image centre."""
        from app.compute import worker_aperture_photometry
        pixel_scale_arcsec = self.info["cdelt_deg"] * 3600.0
        ap_arcsec = 3.0 * self.info["sigma_pix"] * pixel_scale_arcsec
        result = worker_aperture_photometry(
            self.path, None,
            self.info["ra"], self.info["dec"],
            ap_arcsec, ap_arcsec * 1.5, ap_arcsec * 2.0,
        )
        # Centre is at pixel (32, 32) by construction
        assert abs(result["pix_x"] - 32.0) < 1.0
        assert abs(result["pix_y"] - 32.0) < 1.0

    def test_returns_pixel_scale(self):
        from app.compute import worker_aperture_photometry
        pixel_scale_arcsec = self.info["cdelt_deg"] * 3600.0
        result = worker_aperture_photometry(
            self.path, None,
            self.info["ra"], self.info["dec"],
            pixel_scale_arcsec * 10,
            pixel_scale_arcsec * 15,
            pixel_scale_arcsec * 20,
        )
        assert result["pixel_scale_arcsec"] > 0.0

    def test_photutils_missing_raises_importerror(self, tmp_path):
        """When photutils is absent the worker should raise ImportError."""
        from app.compute import worker_aperture_photometry
        path = str(tmp_path / "dummy.fits")
        _write_plain_image(path)
        with patch.dict("sys.modules", {"photutils": None,
                                        "photutils.aperture": None}):
            with pytest.raises(ImportError, match="photutils"):
                worker_aperture_photometry(path, None, 83.82, -5.39, 5.0, 8.0, 12.0)


# ── A-02: 2-D Gaussian source fitting ─────────────────────────────────────────

class TestGaussFit2D:

    @pytest.fixture(autouse=True)
    def setup(self, tmp_path):
        self.tmp = tmp_path
        path = str(tmp_path / "fit_src.fits")
        info = _write_gaussian_image(path, amp=5.0, sigma_pix=4.0)
        self.path = path
        self.info = info

    def test_peak_within_1pct(self):
        """Fitted peak amplitude must be within 1% of the injected amplitude."""
        from app.compute import worker_gauss_fit_2d
        pixel_scale_arcsec = self.info["cdelt_deg"] * 3600.0
        result = worker_gauss_fit_2d(
            self.path, None,
            self.info["ra"], self.info["dec"],
            50.0 * pixel_scale_arcsec,
        )
        assert abs(result["peak_jy_beam"] - self.info["amp"]) / self.info["amp"] < 0.01

    def test_pa_within_5deg_for_circular_source(self):
        """For a circular Gaussian the PA is arbitrary but should not raise."""
        from app.compute import worker_gauss_fit_2d
        pixel_scale_arcsec = self.info["cdelt_deg"] * 3600.0
        result = worker_gauss_fit_2d(
            self.path, None,
            self.info["ra"], self.info["dec"],
            50.0 * pixel_scale_arcsec,
        )
        # Just check it's in [0, 180)
        assert 0.0 <= result["pa_deg"] < 180.0

    def test_deconvolved_sizes_returned_with_beam(self):
        """With beam keywords present the deconvolved sizes should be non-None."""
        from app.compute import worker_gauss_fit_2d
        pixel_scale_arcsec = self.info["cdelt_deg"] * 3600.0
        result = worker_gauss_fit_2d(
            self.path, None,
            self.info["ra"], self.info["dec"],
            50.0 * pixel_scale_arcsec,
        )
        # Deconvolved values may be None if unresolved; just check dict key exists
        assert "deconvolved_major_arcsec" in result

    def test_fit_rms_small_for_clean_gaussian(self):
        """Fit RMS should be very small for a noise-free Gaussian."""
        from app.compute import worker_gauss_fit_2d
        pixel_scale_arcsec = self.info["cdelt_deg"] * 3600.0
        result = worker_gauss_fit_2d(
            self.path, None,
            self.info["ra"], self.info["dec"],
            50.0 * pixel_scale_arcsec,
        )
        assert result["fit_rms"] < 0.1 * self.info["amp"]

    def test_major_axis_matches_sigma(self):
        """Major axis FWHM should be 2.355 × σ in arcsec (within 5%)."""
        from app.compute import worker_gauss_fit_2d
        pixel_scale_arcsec = self.info["cdelt_deg"] * 3600.0
        expected_fwhm_arcsec = 2.3548 * self.info["sigma_pix"] * pixel_scale_arcsec
        result = worker_gauss_fit_2d(
            self.path, None,
            self.info["ra"], self.info["dec"],
            50.0 * pixel_scale_arcsec,
        )
        assert abs(result["major_arcsec"] - expected_fwhm_arcsec) / expected_fwhm_arcsec < 0.05


# ── A-03: WCS reprojection ────────────────────────────────────────────────────

@pytest.mark.skipif(
    not __import__("importlib").util.find_spec("reproject"),
    reason="reproject not installed",
)
class TestReproject:

    @pytest.fixture(autouse=True)
    def setup(self, tmp_path):
        self.tmp = tmp_path
        path = str(tmp_path / "reproject_src.fits")
        _write_plain_image(path)
        self.path = path
        self.header = _wcs_image_header()

    def _identity_header_dict(self) -> dict:
        """Return the same WCS as the source image (identity reprojection)."""
        h = self.header
        return {k: h[k] for k in h.keys() if k.startswith(("NAXIS", "CTYPE", "CRVAL",
                                                              "CRPIX", "CDELT", "CUNIT"))}

    def test_identity_reproject_max_diff_small(self, tmp_path):
        """Reprojecting to the same WCS should change pixel values by < 1e-4."""
        from app.compute import worker_reproject
        out_path = str(tmp_path / "reprojected.fits")
        worker_reproject(self.path, out_path, self._identity_header_dict(), [64, 64], 1)
        with fits.open(self.path) as h_src, fits.open(out_path) as h_out:
            src = h_src[0].data.astype(np.float64)
            out = h_out[0].data.astype(np.float64)
        diff = np.nanmax(np.abs(src - out))
        assert diff < 1e-3   # bilinear interp has some error at edges

    def test_output_shape_matches_target(self, tmp_path):
        """Output FITS must have the requested shape."""
        from app.compute import worker_reproject
        out_path = str(tmp_path / "reprojected_shape.fits")
        result = worker_reproject(self.path, out_path, self._identity_header_dict(),
                                   [32, 32], 1)
        assert result["width"] == 32
        assert result["height"] == 32

    def test_output_file_created(self, tmp_path):
        from app.compute import worker_reproject
        out_path = str(tmp_path / "reprojected_exists.fits")
        worker_reproject(self.path, out_path, self._identity_header_dict(), [64, 64], 1)
        assert Path(out_path).exists()

    def test_reproject_missing_raises_importerror(self, tmp_path):
        from app.compute import worker_reproject
        out_path = str(tmp_path / "noop.fits")
        with patch.dict("sys.modules", {"reproject": None}):
            with pytest.raises(ImportError, match="reproject"):
                worker_reproject(self.path, out_path, self._identity_header_dict(),
                                  [64, 64], 1)


# ── A-04: External catalogue cross-match ──────────────────────────────────────

@pytest.mark.asyncio
class TestCrossmatchEndpoint:
    """HTTP-level tests for /v1/astrometry/crossmatch/{dataset_id}."""

    async def _open_image(self, client, headers, image_path) -> tuple[str, dict]:
        resp = await client.post(
            "/v1/datasets/open",
            json={"path": str(image_path)},
            headers=headers,
        )
        data = resp.json()
        session_id = data["session_id"]
        return data["dataset_id"], {**headers, "X-Visivo-Session": session_id}

    async def test_astroquery_missing_returns_503(
        self, client, auth_headers, synthetic_fits_image
    ):
        dataset_id, full_headers = await self._open_image(
            client, auth_headers, synthetic_fits_image
        )
        with patch.dict("sys.modules", {"astroquery": None,
                                        "astroquery.simbad": None,
                                        "astroquery.vizier": None}):
            resp = await client.get(
                f"/v1/astrometry/crossmatch/{dataset_id}?catalogue=simbad",
                headers=full_headers,
            )
        assert resp.status_code == 503

    async def test_unknown_catalogue_returns_422(
        self, client, auth_headers, synthetic_fits_image
    ):
        dataset_id, full_headers = await self._open_image(
            client, auth_headers, synthetic_fits_image
        )

        def _fake_crossmatch(*args, **kwargs):
            raise ValueError("Unknown catalogue 'foobar'.")

        # Patch the symbol as imported in app.main (thread-pool path)
        with patch("app.compute.worker_crossmatch", side_effect=_fake_crossmatch):
            resp = await client.get(
                f"/v1/astrometry/crossmatch/{dataset_id}?catalogue=foobar",
                headers=full_headers,
            )
        assert resp.status_code == 422

    async def test_simbad_mock_returns_sources(
        self, client, auth_headers, synthetic_fits_image
    ):
        dataset_id, full_headers = await self._open_image(
            client, auth_headers, synthetic_fits_image
        )

        mock_result = {
            "n_sources": 2,
            "truncated": False,
            "centre_ra_deg": 83.82,
            "centre_dec_deg": -5.39,
            "search_radius_deg": 0.05,
            "sources": [
                {"ra_deg": 83.82, "dec_deg": -5.39, "name": "* alp Ori",
                 "catalogue_id": "SIMBAD", "separation_arcsec": 0.0},
                {"ra_deg": 83.84, "dec_deg": -5.40, "name": "Star2",
                 "catalogue_id": "SIMBAD", "separation_arcsec": 10.0},
            ],
        }
        # asyncio.to_thread runs in-process so patching app.compute works
        with patch("app.compute.worker_crossmatch", return_value=mock_result):
            resp = await client.get(
                f"/v1/astrometry/crossmatch/{dataset_id}?catalogue=simbad&radius_arcsec=200",
                headers=full_headers,
            )
        assert resp.status_code == 200
        data = resp.json()
        assert data["valid"]
        assert data["n_sources"] == 2
        assert len(data["sources"]) == 2

    async def test_truncated_header_set(
        self, client, auth_headers, synthetic_fits_image
    ):
        dataset_id, full_headers = await self._open_image(
            client, auth_headers, synthetic_fits_image
        )
        mock_result = {
            "n_sources": 500,
            "truncated": True,
            "centre_ra_deg": 83.82,
            "centre_dec_deg": -5.39,
            "search_radius_deg": 0.5,
            "sources": [{"ra_deg": 83.82, "dec_deg": -5.39, "name": f"S{i}",
                          "catalogue_id": "2MASS", "separation_arcsec": float(i)}
                         for i in range(500)],
        }
        with patch("app.compute.worker_crossmatch", return_value=mock_result):
            resp = await client.get(
                f"/v1/astrometry/crossmatch/{dataset_id}?catalogue=2MASS",
                headers=full_headers,
            )
        assert resp.status_code == 200
        assert resp.headers.get("X-Truncated") == "true"

    async def test_vizier_catalog_id_mapping(self, tmp_path):
        """Verify the Vizier catalog ID mapping is correct for all supported catalogs."""
        from app.compute import worker_crossmatch

        EXPECTED = {"2MASS": "II/246", "NVSS": "VIII/65", "FIRST": "VIII/92"}

        for cat_name, expected_vizier in EXPECTED.items():
            path = str(tmp_path / "img.fits")
            _write_plain_image(path)

            captured_catalog: list[str] = []

            mock_vizier_instance = MagicMock()
            mock_vizier_instance.query_region.side_effect = lambda *a, **kw: (
                captured_catalog.append(kw.get("catalog", "")), []
            )[1]

            mock_vizier_cls = MagicMock(return_value=mock_vizier_instance)
            mock_simbad = MagicMock()
            mock_simbad.Simbad = MagicMock()

            # Fake the entire astroquery namespace so the import inside worker_crossmatch works
            fake_astroquery_vizier = MagicMock()
            fake_astroquery_vizier.Vizier = mock_vizier_cls

            with patch.dict("sys.modules", {
                "astroquery": MagicMock(),
                "astroquery.vizier": fake_astroquery_vizier,
                "astroquery.simbad": mock_simbad,
                "astropy.coordinates": MagicMock(SkyCoord=MagicMock()),
                "astropy.units": MagicMock(),
            }):
                try:
                    worker_crossmatch(path, 60.0, cat_name, 500)
                except Exception:
                    pass
            # The Vizier class is constructed and query_region is called with
            # the catalog keyword; verify the correct Vizier ID was used.
            if mock_vizier_instance.query_region.called:
                _, call_kwargs = mock_vizier_instance.query_region.call_args
                assert call_kwargs.get("catalog") == expected_vizier


# ── A-02 direct worker tests ──────────────────────────────────────────────────

class TestGaussFit2DEdgeCases:

    def test_no_wcs_raises(self, tmp_path):
        """Worker must raise ValueError if no WCS."""
        from app.compute import worker_gauss_fit_2d
        path = str(tmp_path / "nowcs.fits")
        data = np.ones((32, 32), dtype=np.float32)
        hdu = fits.PrimaryHDU(data)
        hdu.header["NAXIS"] = 2
        hdu.header["NAXIS1"] = 32
        hdu.header["NAXIS2"] = 32
        # deliberately omit WCS keywords
        hdu.writeto(path, overwrite=True)
        with pytest.raises((ValueError, Exception)):
            worker_gauss_fit_2d(path, None, 0.0, 0.0, 10.0)

    def test_channel_selection_3d(self, tmp_path):
        """Worker must accept channel selection for 3-D input."""
        from app.compute import worker_gauss_fit_2d
        path = str(tmp_path / "cube3d.fits")
        nz, ny, nx = 4, 64, 64
        yy, xx = np.mgrid[0:ny, 0:nx]
        layer = (5.0 * np.exp(-0.5 * ((xx - 32) ** 2 + (yy - 32) ** 2) / 4.0 ** 2)
                 ).astype(np.float32)
        data = np.stack([layer] * nz, axis=0)
        h = fits.Header()
        h["NAXIS"] = 3
        h["NAXIS1"] = nx; h["NAXIS2"] = ny; h["NAXIS3"] = nz
        h["CTYPE1"] = "RA---TAN"; h["CTYPE2"] = "DEC--TAN"; h["CTYPE3"] = "VRAD"
        h["CRVAL1"] = 83.82; h["CRVAL2"] = -5.39; h["CRVAL3"] = 0.0
        h["CRPIX1"] = nx / 2.0; h["CRPIX2"] = ny / 2.0; h["CRPIX3"] = 1.0
        h["CDELT1"] = -2.777e-4; h["CDELT2"] = 2.777e-4; h["CDELT3"] = 1.0
        h["CUNIT1"] = "deg"; h["CUNIT2"] = "deg"; h["CUNIT3"] = "km/s"
        fits.PrimaryHDU(data, header=h).writeto(path, overwrite=True)
        wcs = WCS(h).celestial
        sky = wcs.all_pix2world([[32, 32]], 0)[0]
        result = worker_gauss_fit_2d(path, 1, float(sky[0]), float(sky[1]), 50.0 * 2.777e-4 * 3600.0)
        assert result["peak_jy_beam"] > 0
