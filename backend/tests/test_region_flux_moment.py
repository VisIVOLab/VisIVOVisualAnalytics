"""Issue #401: beam-aware region flux on moment-0 maps.

A Gaussian source the shape of the beam, of peak P Jy/beam in each of n
channels Δv wide, has a flux of P·n·Δv Jy km/s: the pixel sum of a Gaussian of
peak P is P·2πσxσy, which is P·Ω_beam in pixels. The region flux must give it
back, in km/s whatever the cube's velocity unit, and the exported moment map
must carry the beam the image viewer needs to do the same.
"""
from __future__ import annotations

import numpy as np
import pytest
from astropy.io import fits

from app.compute import worker_hi_products, worker_moment_save_fits
from app.compute.hi import luminosity_distance_mpc, region_flux

_FWHM_TO_SIGMA = 1.0 / (2.0 * np.sqrt(2.0 * np.log(2.0)))


def _write_source_cube(path, *, unit="m/s", dv=5000.0, peak=0.5, nz=10, n=64,
                       pedestal=0.0, noise=0.0, ctype="VRAD", bmaj_px=6.0, bmin_px=4.0,
                       cunit=True):
    """A cube with a beam-shaped Gaussian source at the centre, ``peak``
    Jy/beam in every channel, on a constant ``pedestal`` plus white noise."""
    pix_deg = 2.0 / 3600.0
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float64)
    sx, sy = bmin_px * _FWHM_TO_SIGMA, bmaj_px * _FWHM_TO_SIGMA   # BPA = 0: major along y
    c = (n - 1) / 2.0
    plane = peak * np.exp(-0.5 * (((xx - c) / sx) ** 2 + ((yy - c) / sy) ** 2))
    rng = np.random.default_rng(401)
    data = (plane[None] + pedestal + noise * rng.standard_normal((nz, n, n))).astype(np.float32)
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"], h["CTYPE3"] = "RA---SIN", "DEC--SIN", ctype
    h["CRVAL1"], h["CRVAL2"], h["CRVAL3"] = 150.0, 20.0, 1000.0
    h["CRPIX1"] = h["CRPIX2"] = n / 2.0
    h["CRPIX3"] = 1.0
    h["CDELT1"], h["CDELT2"], h["CDELT3"] = -pix_deg, pix_deg, dv
    h["CUNIT1"] = h["CUNIT2"] = "deg"
    if cunit:
        h["CUNIT3"] = unit
    h["BUNIT"] = "Jy/beam"
    h["BMAJ"], h["BMIN"], h["BPA"] = bmaj_px * pix_deg, bmin_px * pix_deg, 0.0
    fits.PrimaryHDU(data, header=h).writeto(path, overwrite=True)
    return c


def _circle(c, r):
    return {"frame": "image", "shape": "circle", "center": [c, c], "radius": r}


def _annulus(c, r1, r2):
    return {"frame": "image", "shape": "annulus", "center": [c, c], "radii": [r1, r2]}


@pytest.mark.parametrize("unit,dv,to_kms", [("m/s", 5000.0, 1e-3), ("km/s", 5.0, 1.0)])
def test_region_flux_is_the_known_flux_in_jy_km_s(tmp_path, unit, dv, to_kms):
    path = str(tmp_path / f"src_{unit.replace('/', '')}.fits")
    c = _write_source_cube(path, unit=unit, dv=dv, peak=0.5, nz=10)
    out = worker_hi_products(path, 0, 9, region=_circle(c, 25))
    expected = 0.5 * 10 * dv * to_kms      # P · n · Δv, in Jy km/s
    assert expected == pytest.approx(25.0)
    reg = out["region"]
    assert reg["beam_area_pix"] == pytest.approx(np.pi * 6.0 * 4.0 / (4 * np.log(2)), rel=1e-9)
    assert reg["flux_jy_kms"] == pytest.approx(expected, rel=1e-4)
    assert reg["error_from"] == "map_noise"


def test_velocity_axis_without_cunit_is_read_in_m_per_s(tmp_path):
    """HCG 44's VELO-HEL axis has no CUNIT3: astropy names it ``m.s**-1``,
    which the HI products used to refuse."""
    path = str(tmp_path / "src_nocunit.fits")
    c = _write_source_cube(path, ctype="VELO-HEL", dv=5000.0, cunit=False)
    out = worker_hi_products(path, 0, 9, region=_circle(c, 25))
    assert out["region"]["flux_jy_kms"] == pytest.approx(25.0, rel=1e-4)


def test_background_region_is_subtracted_and_gives_the_error(tmp_path):
    path = str(tmp_path / "src_bg.fits")
    c = _write_source_cube(path, unit="km/s", dv=5.0, pedestal=0.01, noise=0.002, nz=10)
    out = worker_hi_products(path, 0, 9, distance_mpc=20.0, region=_circle(c, 18),
                             background_region=_annulus(c, 22, 30))
    reg = out["region"]
    assert reg["error_from"] == "background"
    # The pedestal (0.01 Jy/beam × 50 km/s per pixel) is gone, the source stays.
    assert reg["background_mean_jy_beam_kms"] == pytest.approx(0.5, abs=0.02)
    assert reg["flux_jy_kms"] == pytest.approx(25.0, abs=3 * reg["flux_error_jy_kms"] + 0.05)
    # σ_S² = σ_bg² N/Ω + σ_bg² N²/(N_bg Ω): the region's beams and the error of
    # the background mean, with the numbers the response reports.
    sig, n, n_bg, om = (reg["background_sigma_jy_beam_kms"], reg["n_pixels"],
                        reg["background_n_pixels"], reg["beam_area_pix"])
    assert reg["flux_error_jy_kms"] == pytest.approx(
        sig * np.sqrt(n / om + n * n / (n_bg * om)))
    assert reg["warnings"] == []
    assert reg["hi_mass_msun"] == pytest.approx(2.356e5 * 20.0 ** 2 * reg["flux_jy_kms"])
    assert reg["hi_mass_error_msun"] == pytest.approx(
        2.356e5 * 20.0 ** 2 * reg["flux_error_jy_kms"])
    # Without the background the pedestal is in the flux.
    plain = worker_hi_products(path, 0, 9, region=_circle(c, 18))["region"]
    assert plain["flux_jy_kms"] - reg["flux_jy_kms"] == pytest.approx(
        plain["n_pixels"] * reg["background_mean_jy_beam_kms"] / reg["beam_area_pix"])


def test_redshift_gives_the_luminosity_distance(tmp_path):
    path = str(tmp_path / "src_z.fits")
    c = _write_source_cube(path, unit="km/s", dv=5.0)
    out = worker_hi_products(path, 0, 9, redshift=0.005, region=_circle(c, 25))
    assert out["distance_mpc"] == pytest.approx(luminosity_distance_mpc(0.005))
    assert 20.0 < out["distance_mpc"] < 25.0
    # From a redshift: M_HI = 2.356e5 D_L² S / (1 + z) (Meyer et al. 2017).
    assert out["region"]["hi_mass_msun"] == pytest.approx(
        2.356e5 * out["distance_mpc"] ** 2 * out["region"]["flux_jy_kms"] / 1.005)
    assert out["hi_mass_msun"] == pytest.approx(
        2.356e5 * out["distance_mpc"] ** 2 * out["integrated_flux_jy_kms"] / 1.005)
    # A typed distance wins over the redshift, and takes no (1 + z).
    typed = worker_hi_products(path, 0, 9, distance_mpc=10.0, redshift=0.005,
                               region=_circle(c, 25))
    assert typed["distance_mpc"] == 10.0
    assert typed["region"]["hi_mass_msun"] == pytest.approx(
        2.356e5 * 100.0 * typed["region"]["flux_jy_kms"])


def test_region_errors_are_value_errors():
    m0 = np.ones((10, 10))
    with pytest.raises(ValueError, match="image"):
        region_flux(m0, 5.0, {"frame": "fk5", "shape": "circle", "center": [1, 1], "radius": 2})
    with pytest.raises(ValueError, match="Malformed"):
        region_flux(m0, 5.0, {"frame": "image", "shape": "circle", "center": [1, 1]})
    with pytest.raises(ValueError, match="no valid pixel"):
        region_flux(m0, 5.0, _circle(50.0, 2.0))
    with pytest.raises(ValueError, match="no valid pixel"):
        region_flux(m0, 5.0, _circle(5.0, 2.0), background=_circle(50.0, 1.0))


def test_an_unmeasurable_sigma_gives_no_error_and_says_so():
    """A one-pixel background, or a constant map (a clipped moment): no σ."""
    m0 = np.ones((20, 20))
    one = region_flux(m0, 5.0, _circle(5.0, 2.0), background=_circle(15.0, 0.0))
    assert one["background_n_pixels"] == 1
    assert one["flux_error_jy_kms"] is None
    assert any("cannot be measured" in w for w in one["warnings"])
    flat = region_flux(m0, 5.0, _circle(5.0, 2.0))
    assert flat["flux_error_jy_kms"] is None


def test_a_clipped_map_is_flagged():
    """A moment computed with a threshold mask: most of the field is 0 or
    blank, its σ too small and the flux biased; the result says so."""
    rng = np.random.default_rng(1)
    m0 = rng.normal(0.0, 1.0, (40, 40))
    m0[:, :30] = 0.0           # masked voxels summed to zero
    m0[:5, 30:] = np.nan       # or left no value
    out = region_flux(m0, 5.0, _circle(35.0, 3.0), background=_circle(20.0, 8.0))
    assert any("masked or thresholded" in w for w in out["warnings"])
    clean = region_flux(rng.normal(0.0, 1.0, (40, 40)), 5.0, _circle(35.0, 3.0))
    assert not any("masked" in w for w in clean["warnings"])


def test_a_masked_moment_warns(tmp_path):
    path = str(tmp_path / "src_mask.fits")
    c = _write_source_cube(path, unit="km/s", dv=5.0, noise=0.01)
    out = worker_hi_products(path, 0, 9, mask_enabled=True, threshold_auto=True,
                             region=_circle(c, 18))
    assert any("threshold mask" in w for w in out["region"]["warnings"])


def test_moment_fits_carries_the_beam(tmp_path):
    path = str(tmp_path / "src_fits.fits")
    _write_source_cube(path, unit="m/s", dv=5000.0)
    out = str(tmp_path / "m0.fits")
    worker_moment_save_fits(path, out, 0, 0, 9, False, 0.0)
    src = fits.getheader(path)
    h = fits.getheader(out)
    for key in ("BMAJ", "BMIN", "BPA"):
        assert h[key] == pytest.approx(src[key])
    assert "m" in h["BUNIT"].lower() and "beam" in h["BUNIT"].lower()
    # The map's pixel sum over the beam area is the source's flux (Jy m/s here).
    data = fits.getdata(out).astype(np.float64)
    omega = np.pi * h["BMAJ"] * h["BMIN"] / (4 * np.log(2)) / (h["CDELT2"] ** 2)
    assert np.nansum(data) / omega * 1e-3 == pytest.approx(25.0, rel=1e-3)


def test_moment_fits_of_a_per_channel_beam_cube_names_the_beam_used(tmp_path):
    path = str(tmp_path / "src_beams.fits")
    _write_source_cube(path, unit="km/s", dv=5.0, nz=6)
    with fits.open(path) as hdul:
        primary = hdul[0].copy()
    for key in ("BMAJ", "BMIN", "BPA"):
        del primary.header[key]
    nz = 6
    cols = fits.ColDefs([
        fits.Column(name="BMAJ", format="E", unit="arcsec",
                    array=np.linspace(10.0, 15.0, nz).astype(np.float32)),
        fits.Column(name="BMIN", format="E", unit="arcsec",
                    array=np.linspace(8.0, 13.0, nz).astype(np.float32)),
        fits.Column(name="BPA", format="E", unit="deg", array=np.full(nz, 30.0, np.float32)),
        fits.Column(name="CHAN", format="J", array=np.arange(nz, dtype=np.int32)),
    ])
    beams = fits.BinTableHDU.from_columns(cols)
    beams.header["EXTNAME"] = "BEAMS"
    fits.HDUList([primary, beams]).writeto(path, overwrite=True)
    out = str(tmp_path / "m0_beams.fits")
    worker_moment_save_fits(path, out, 0, 2, 4, False, 0.0)
    h = fits.getheader(out)
    assert h["BMAJ"] == pytest.approx(13.0 / 3600.0, rel=1e-6)   # channel 3
    assert h["BMIN"] == pytest.approx(11.0 / 3600.0, rel=1e-6)
    assert h["BPA"] == pytest.approx(30.0)
    assert any("channel 3" in str(c) for c in h["COMMENT"])


def test_moment_fits_without_a_beam_writes_none(tmp_path):
    path = str(tmp_path / "src_nobeam.fits")
    _write_source_cube(path)
    with fits.open(path, mode="update") as hdul:
        for key in ("BMAJ", "BMIN", "BPA"):
            del hdul[0].header[key]
    out = str(tmp_path / "m0_nobeam.fits")
    worker_moment_save_fits(path, out, 0, 0, 9, False, 0.0)
    h = fits.getheader(out)
    assert "BMAJ" not in h and "BMIN" not in h


def test_the_open_geometry_says_when_the_beam_varies(tmp_path):
    """The viewers say a flux uses the header beam on such a cube."""
    from app.fits_dataset import ScientificFitsDataset

    path = str(tmp_path / "src_geom.fits")
    _write_source_cube(path, unit="km/s", dv=5.0, nz=4)
    assert ScientificFitsDataset(path).geometry_metadata()["beam_per_channel"] is False
    with fits.open(path) as hdul:
        primary = hdul[0].copy()
    cols = fits.ColDefs([
        fits.Column(name="BMAJ", format="E", unit="arcsec", array=np.full(4, 12.0, np.float32)),
        fits.Column(name="BMIN", format="E", unit="arcsec", array=np.full(4, 8.0, np.float32)),
        fits.Column(name="CHAN", format="J", array=np.arange(4, dtype=np.int32)),
    ])
    beams = fits.BinTableHDU.from_columns(cols)
    beams.header["EXTNAME"] = "BEAMS"
    fits.HDUList([primary, beams]).writeto(path, overwrite=True)
    assert ScientificFitsDataset(path).geometry_metadata()["beam_per_channel"] is True
