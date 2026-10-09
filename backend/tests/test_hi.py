"""R-19: HI column density + mass from moment-0."""
from __future__ import annotations

import numpy as np
import pytest
from astropy.io import fits

from app.compute import worker_hi_products


def _write_hi_cube(path, nz=24, ny=12, nx=12, dv_kms=5.0, bmaj_deg=3e-3, bmin_deg=2e-3):
    """A Jy/beam cube with a velocity axis and a bright Gaussian source."""
    rng = np.random.default_rng(0)
    chans = np.arange(nz, dtype=np.float32)
    prof = np.exp(-0.5 * ((chans - nz / 2) / 3.0) ** 2)
    data = (prof[:, None, None] * 0.0).astype(np.float32) + \
        rng.standard_normal((nz, ny, nx)).astype(np.float32) * 0.001
    data[:, 5:8, 5:8] += (prof[:, None, None] * 2.0).astype(np.float32)
    h = fits.Header()
    h["NAXIS"] = 3; h["NAXIS1"] = nx; h["NAXIS2"] = ny; h["NAXIS3"] = nz
    h["CTYPE1"] = "RA---SIN"; h["CTYPE2"] = "DEC--SIN"; h["CTYPE3"] = "VRAD"
    h["CRVAL1"] = 10.0; h["CRVAL2"] = 20.0; h["CRVAL3"] = 0.0
    h["CRPIX1"] = 6.0; h["CRPIX2"] = 6.0; h["CRPIX3"] = nz / 2.0
    h["CDELT1"] = -1e-3; h["CDELT2"] = 1e-3; h["CDELT3"] = dv_kms
    h["CUNIT1"] = "deg"; h["CUNIT2"] = "deg"; h["CUNIT3"] = "km/s"
    h["BUNIT"] = "Jy/beam"; h["BMAJ"] = bmaj_deg; h["BMIN"] = bmin_deg; h["BPA"] = 0.0
    fits.PrimaryHDU(data, header=h).writeto(path, overwrite=True)


def test_hi_column_density_positive(tmp_path):
    path = str(tmp_path / "hi.fits")
    _write_hi_cube(path)
    out = worker_hi_products(path, 0, 23)
    assert out["nhi_unit"] == "cm^-2"
    assert out["nhi_max"] > 0
    assert out["integrated_flux_jy_kms"] > 0
    assert out["hi_mass_msun"] is None  # no distance given


def test_is_per_beam_bunit_spellings():
    from app.compute._common import is_per_beam_bunit
    for good in ("Jy/beam", "Jy beam-1", "Jy/Beam-1", "JY BEAM-1", "Jy beam^-1",
                 "Jy/beam km/s"):
        assert is_per_beam_bunit(good), good
    # mJy/uJy per-beam must NOT match: the constants assume Jy, so treating
    # milli/micro-Jy as Jy would be 1000×/1e6× wrong.
    for bad in ("K", "Jy", "Jy/pixel", "", "mJy/beam", "uJy/beam"):
        assert not is_per_beam_bunit(bad), bad


def test_hi_bunit_jy_beam_dash1_accepted(tmp_path):
    """R-review: 'Jy beam-1' (FITS spelling) must be accepted, not rejected."""
    path = str(tmp_path / "hi_dash.fits")
    _write_hi_cube(path)
    with fits.open(path, mode="update") as hdul:
        hdul[0].header["BUNIT"] = "Jy beam-1"
    out = worker_hi_products(path, 0, 23)   # must not raise
    assert out["nhi_max"] > 0


def test_hi_mask_off_warns_whole_field(tmp_path):
    """R-review: with no mask the integral is whole-field → a warning + pixel count."""
    path = str(tmp_path / "hi_mask.fits")
    _write_hi_cube(path)
    off = worker_hi_products(path, 0, 23, distance_mpc=10.0, mask_enabled=False)
    assert off["mask_enabled"] is False
    assert any("whole-field" in w.lower() for w in off["warnings"])
    assert off["n_pixels_summed"] > 0
    on = worker_hi_products(path, 0, 23, distance_mpc=10.0,
                            mask_enabled=True, threshold_auto=True)
    assert on["mask_enabled"] is True
    assert on["warnings"] == []
    # Masking restricts the integral to fewer pixels.
    assert on["n_pixels_summed"] <= off["n_pixels_summed"]


def test_hi_mass_scales_with_distance_squared(tmp_path):
    path = str(tmp_path / "hi2.fits")
    _write_hi_cube(path)
    a = worker_hi_products(path, 0, 23, distance_mpc=10.0)
    b = worker_hi_products(path, 0, 23, distance_mpc=20.0)
    assert a["hi_mass_msun"] > 0
    # 2x distance → 4x mass (M_HI ∝ D²).
    assert b["hi_mass_msun"] == pytest.approx(4.0 * a["hi_mass_msun"], rel=1e-6)
    assert b["hi_mass_log10_msun"] == pytest.approx(np.log10(b["hi_mass_msun"]))


def test_hi_requires_jy_beam(tmp_path):
    path = str(tmp_path / "hi_k.fits")
    _write_hi_cube(path)
    # Rewrite BUNIT to K → must be rejected.
    with fits.open(path, mode="update") as hdul:
        hdul[0].header["BUNIT"] = "K"
    with pytest.raises(ValueError, match="Jy/beam"):
        worker_hi_products(path, 0, 23)


def test_hi_requires_beam(tmp_path):
    path = str(tmp_path / "hi_nobeam.fits")
    _write_hi_cube(path)
    with fits.open(path, mode="update") as hdul:
        del hdul[0].header["BMAJ"]
    with pytest.raises(ValueError, match="beam"):
        worker_hi_products(path, 0, 23)


def test_hi_defaults_to_full_depth(tmp_path):
    """Empty channel range → whole spectral axis, effective range reported."""
    path = str(tmp_path / "hi_fd.fits")
    _write_hi_cube(path, nz=24)
    out = worker_hi_products(path, 0, 0)   # unset range
    assert out["channel_start"] == 0
    assert out["channel_end"] == 23
    assert out["n_channels_in_cube"] == 24


def test_hi_rejects_per_channel_beam(tmp_path):
    """A BEAMS table (varying beam) can't be integrated with one beam area."""
    path = str(tmp_path / "hi_multibeam.fits")
    _write_hi_cube(path, nz=6)
    # Append a CASA BEAMS table so the beam varies per channel.
    with fits.open(path) as hdul:
        primary = hdul[0].copy()
    nz = 6
    cols = fits.ColDefs([
        fits.Column(name="BMAJ", format="E", unit="arcsec",
                    array=np.linspace(3.0, 4.0, nz).astype(np.float32)),
        fits.Column(name="BMIN", format="E", unit="arcsec",
                    array=np.linspace(2.0, 3.0, nz).astype(np.float32)),
        fits.Column(name="BPA", format="E", unit="deg", array=np.zeros(nz, np.float32)),
        fits.Column(name="CHAN", format="J", array=np.arange(nz, dtype=np.int32)),
    ])
    beams = fits.BinTableHDU.from_columns(cols)
    beams.header["EXTNAME"] = "BEAMS"
    fits.HDUList([primary, beams]).writeto(path, overwrite=True)
    with pytest.raises(ValueError, match="per-channel|common"):
        worker_hi_products(path, 0, 5)
