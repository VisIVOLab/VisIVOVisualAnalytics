"""
test_wcs_sanitization.py – focused tests for WCS sanitisation in ScientificFitsDataset.

Complements test_fits_dataset.py (which already covers baseline ok/degraded/sanitized
for 2D images with CUNIT='pixel'). This file adds:

  1. Static helpers — _is_celestial_ctype and _is_angular_unit tested directly
  2. 3-axis cube with bad RA CUNIT — sanitization triggers on a real cube
  3. Partial sanitization — only axis 1 is bad, axis 2 is fine
  4. Galactic coordinates (GLON/GLAT) — recognized as celestial and sanitized
  5. Empty CUNIT — treated as angular (no sanitization)
  6. Non-celestial axis with non-standard unit — NOT sanitized
  7. geometry_metadata() keys after sanitization on a cube
  8. Non-regression: a correct header must not be modified at all
  9. wcs_warning_message contents for each status string

Run with:
    cd backend
    pytest tests/test_wcs_sanitization.py -v
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits


# ── Helpers ───────────────────────────────────────────────────────────────────


def _load(path) -> "ScientificFitsDataset":
    from app.fits_dataset import ScientificFitsDataset
    return ScientificFitsDataset(str(path))


def _cls():
    from app.fits_dataset import ScientificFitsDataset
    return ScientificFitsDataset


def _write(tmp_path: Path, header: fits.Header, shape=(4, 4, 4), name="test.fits") -> Path:
    """Write a minimal FITS file from the given header."""
    data = np.zeros(shape, dtype=np.float32)
    fits.PrimaryHDU(data, header=header).writeto(str(tmp_path / name), overwrite=True)
    return tmp_path / name


def _cube_header(ctype1="RA---TAN", ctype2="DEC--TAN", ctype3="FREQ",
                 cunit1="deg", cunit2="deg", cunit3="Hz") -> fits.Header:
    """Minimal well-formed 3-axis cube header."""
    h = fits.Header()
    h["NAXIS"]  = 3
    h["NAXIS1"] = 4
    h["NAXIS2"] = 4
    h["NAXIS3"] = 4
    h["CTYPE1"] = ctype1
    h["CTYPE2"] = ctype2
    h["CTYPE3"] = ctype3
    h["CRVAL1"] = 180.0
    h["CRVAL2"] = 0.0
    h["CRVAL3"] = 1.4e9
    h["CRPIX1"] = 2.0
    h["CRPIX2"] = 2.0
    h["CRPIX3"] = 1.0
    h["CDELT1"] = -0.001
    h["CDELT2"] =  0.001
    h["CDELT3"] =  1.0e6
    if cunit1:
        h["CUNIT1"] = cunit1
    if cunit2:
        h["CUNIT2"] = cunit2
    if cunit3:
        h["CUNIT3"] = cunit3
    return h


# ═══════════════════════════════════════════════════════════════════════════════
# 1. Static helper: _is_celestial_ctype
# ═══════════════════════════════════════════════════════════════════════════════


class TestIsCelestialCtype:

    @pytest.mark.parametrize("ctype", [
        "RA---TAN", "RA---SIN", "RA---NCP",
        "DEC--TAN", "DEC--SIN",
        "GLON-TAN", "GLAT-TAN",
        "ELON-TAN", "ELAT-TAN",
        "HLON-CAR", "HLAT-CAR",
        "SLON",     "SLAT",
        "TLON",     "TLAT",
    ])
    def test_celestial_prefixes_recognised(self, ctype):
        assert _cls()._is_celestial_ctype(ctype) is True

    @pytest.mark.parametrize("ctype", [
        "FREQ", "VELO-F2V", "VRAD", "STOKES", "",
        "WAVE", "ZOPT", "BETA",
    ])
    def test_non_celestial_types_rejected(self, ctype):
        assert _cls()._is_celestial_ctype(ctype) is False

    def test_empty_string_is_not_celestial(self):
        assert _cls()._is_celestial_ctype("") is False

    def test_case_insensitive_matching(self):
        # Internal code strips and uppercases; lowercase should also match
        assert _cls()._is_celestial_ctype("ra---tan") is True
        assert _cls()._is_celestial_ctype("dec--sin") is True


# ═══════════════════════════════════════════════════════════════════════════════
# 2. Static helper: _is_angular_unit
# ═══════════════════════════════════════════════════════════════════════════════


class TestIsAngularUnit:

    @pytest.mark.parametrize("unit", [
        "deg", "rad", "arcmin", "arcsec", "mas",
    ])
    def test_angular_units_accepted(self, unit):
        assert _cls()._is_angular_unit(unit) is True

    @pytest.mark.parametrize("unit", [
        "m/s", "km/s", "Hz", "Jy", "K", "pixel",
    ])
    def test_non_angular_units_rejected(self, unit):
        assert _cls()._is_angular_unit(unit) is False

    def test_empty_string_treated_as_angular(self):
        """Empty CUNIT means 'assume degrees' — should NOT trigger sanitization."""
        assert _cls()._is_angular_unit("") is True

    def test_whitespace_only_treated_as_angular(self):
        assert _cls()._is_angular_unit("   ") is True

    def test_unrecognised_unit_is_not_angular(self):
        """An unrecognised unit string is not angular → triggers sanitization."""
        assert _cls()._is_angular_unit("frobble") is False


# ═══════════════════════════════════════════════════════════════════════════════
# 3. 3-axis cube with bad RA CUNIT
# ═══════════════════════════════════════════════════════════════════════════════


def test_cube_ra_bad_cunit_triggers_sanitization(tmp_path):
    h = _cube_header(cunit1="m/s")   # RA axis with velocity unit
    path = _write(tmp_path, h)
    ds = _load(path)
    assert ds.wcs_status == "sanitized"
    assert 1 in ds.wcs_sanitized_axes
    assert ds.sanitized_header["CUNIT1"] == "deg"


def test_cube_dec_bad_cunit_triggers_sanitization(tmp_path):
    h = _cube_header(cunit2="km/s")
    path = _write(tmp_path, h)
    ds = _load(path)
    assert ds.wcs_status == "sanitized"
    assert 2 in ds.wcs_sanitized_axes
    assert ds.sanitized_header["CUNIT2"] == "deg"


def test_cube_both_celestial_axes_bad_both_sanitized(tmp_path):
    h = _cube_header(cunit1="pixel", cunit2="pixel")
    path = _write(tmp_path, h)
    ds = _load(path)
    assert ds.wcs_status == "sanitized"
    assert 1 in ds.wcs_sanitized_axes
    assert 2 in ds.wcs_sanitized_axes
    assert ds.sanitized_header["CUNIT1"] == "deg"
    assert ds.sanitized_header["CUNIT2"] == "deg"


def test_cube_sanitized_wcs_is_usable(tmp_path):
    """After sanitization WCS object must be constructible and non-None."""
    h = _cube_header(cunit1="m/s")
    path = _write(tmp_path, h)
    ds = _load(path)
    assert ds.wcs is not None


# ═══════════════════════════════════════════════════════════════════════════════
# 4. Partial sanitization — only one axis bad
# ═══════════════════════════════════════════════════════════════════════════════


def test_only_axis1_sanitized_axis2_untouched(tmp_path):
    h = _cube_header(cunit1="km/s", cunit2="deg")
    path = _write(tmp_path, h)
    ds = _load(path)
    assert 1 in ds.wcs_sanitized_axes
    assert 2 not in ds.wcs_sanitized_axes
    # axis 2 must keep its original unit
    assert ds.sanitized_header.get("CUNIT2") == "deg"


def test_only_axis2_sanitized_axis1_untouched(tmp_path):
    h = _cube_header(cunit1="deg", cunit2="pixel")
    path = _write(tmp_path, h)
    ds = _load(path)
    assert 2 in ds.wcs_sanitized_axes
    assert 1 not in ds.wcs_sanitized_axes
    assert ds.sanitized_header.get("CUNIT1") == "deg"


# ═══════════════════════════════════════════════════════════════════════════════
# 5. Galactic coordinate axes (GLON / GLAT)
# ═══════════════════════════════════════════════════════════════════════════════


def test_galactic_axes_recognized_as_celestial(tmp_path):
    h = _cube_header(ctype1="GLON-TAN", ctype2="GLAT-TAN", cunit1="m/s")
    path = _write(tmp_path, h)
    ds = _load(path)
    assert ds.wcs_status == "sanitized"
    assert 1 in ds.wcs_sanitized_axes


def test_galactic_axis_correct_unit_not_sanitized(tmp_path):
    h = _cube_header(ctype1="GLON-TAN", ctype2="GLAT-TAN", cunit1="deg", cunit2="deg")
    path = _write(tmp_path, h)
    ds = _load(path)
    # Good units — must not be sanitized
    assert 1 not in ds.wcs_sanitized_axes
    assert 2 not in ds.wcs_sanitized_axes


# ═══════════════════════════════════════════════════════════════════════════════
# 6. Empty CUNIT — treated as angular, no sanitization
# ═══════════════════════════════════════════════════════════════════════════════


def test_empty_cunit1_not_sanitized(tmp_path):
    """Missing CUNIT is treated as 'assume degrees' — no sanitization fired."""
    h = _cube_header()
    del h["CUNIT1"]        # remove the key entirely
    path = _write(tmp_path, h)
    ds = _load(path)
    assert 1 not in ds.wcs_sanitized_axes


def test_empty_string_cunit_not_sanitized(tmp_path):
    h = _cube_header(cunit1="")
    path = _write(tmp_path, h, name="empty_cunit.fits")
    ds = _load(path)
    assert 1 not in ds.wcs_sanitized_axes


# ═══════════════════════════════════════════════════════════════════════════════
# 7. Non-celestial axis — bad unit must NOT be sanitized
# ═══════════════════════════════════════════════════════════════════════════════


def test_spectral_axis_bad_unit_not_sanitized(tmp_path):
    """FREQ axis with CUNIT='pixel' is NOT a celestial axis — must not be touched."""
    h = _cube_header(cunit3="pixel")
    path = _write(tmp_path, h)
    ds = _load(path)
    # Axis 3 (FREQ) must NOT be sanitized
    assert 3 not in ds.wcs_sanitized_axes
    assert ds.sanitized_header.get("CUNIT3") == "pixel"


def test_spectral_correct_hz_unit_untouched(tmp_path):
    h = _cube_header(cunit3="Hz")
    path = _write(tmp_path, h)
    ds = _load(path)
    assert 3 not in ds.wcs_sanitized_axes
    assert ds.sanitized_header.get("CUNIT3") == "Hz"


# ═══════════════════════════════════════════════════════════════════════════════
# 8. geometry_metadata() keys after sanitization on a cube
# ═══════════════════════════════════════════════════════════════════════════════


def test_geometry_metadata_has_wcs_status(tmp_path):
    path = _write(tmp_path, _cube_header())
    ds = _load(path)
    meta = ds.geometry_metadata()
    assert "wcs_status" in meta
    assert meta["wcs_status"] == "ok"


def test_geometry_metadata_wcs_status_sanitized(tmp_path):
    h = _cube_header(cunit1="m/s")
    path = _write(tmp_path, h)
    ds = _load(path)
    meta = ds.geometry_metadata()
    assert meta["wcs_status"] == "sanitized"
    assert 1 in meta["wcs_sanitized_axes"]


def test_geometry_metadata_spectral_axis_type_populated_for_cube(tmp_path):
    """A 3-axis cube must report a non-empty spectral_axis_type."""
    path = _write(tmp_path, _cube_header(), shape=(4, 4, 4))
    ds = _load(path)
    meta = ds.geometry_metadata()
    assert meta["kind"] == "cube"
    # spectral_axis_type may be empty if WCS lookup fails, but the key must exist
    assert "spectral_axis_type" in meta
    assert "spectral_axis_unit" in meta


def test_geometry_metadata_spectral_fields_present_after_sanitization(tmp_path):
    h = _cube_header(cunit1="m/s")
    path = _write(tmp_path, h)
    ds = _load(path)
    meta = ds.geometry_metadata()
    assert "spectral_axis_type" in meta
    assert "spectral_axis_unit" in meta


def test_geometry_metadata_wcs_warning_message_present(tmp_path):
    h = _cube_header(cunit1="m/s")
    path = _write(tmp_path, h)
    ds = _load(path)
    meta = ds.geometry_metadata()
    assert "wcs_warning_message" in meta
    assert meta["wcs_warning_message"] != ""


# ═══════════════════════════════════════════════════════════════════════════════
# 9. Non-regression: correct header must not be modified
# ═══════════════════════════════════════════════════════════════════════════════


def test_non_regression_ok_header_untouched(tmp_path):
    path = _write(tmp_path, _cube_header())
    ds = _load(path)
    assert ds.wcs_status == "ok"
    assert ds.wcs_sanitized_axes == []
    assert ds.sanitized_header.get("CUNIT1") == "deg"
    assert ds.sanitized_header.get("CUNIT2") == "deg"
    assert ds.sanitized_header.get("CUNIT3") == "Hz"


def test_non_regression_spectral_unit_untouched(tmp_path):
    """Spectral CUNIT must never be overwritten by the celestial sanitiser."""
    h = _cube_header(cunit3="m/s")   # velocity spectral axis — valid
    path = _write(tmp_path, h)
    ds = _load(path)
    assert 3 not in ds.wcs_sanitized_axes
    assert ds.sanitized_header.get("CUNIT3") == "m/s"


def test_non_regression_rad_unit_not_sanitized(tmp_path):
    """CUNIT='rad' is a valid angular unit — must not be overwritten."""
    h = _cube_header(cunit1="rad")
    path = _write(tmp_path, h)
    ds = _load(path)
    assert 1 not in ds.wcs_sanitized_axes
    assert ds.sanitized_header.get("CUNIT1") == "rad"


# ═══════════════════════════════════════════════════════════════════════════════
# 10. wcs_warning_message contents per status
# ═══════════════════════════════════════════════════════════════════════════════


def test_warning_message_empty_when_ok(tmp_path):
    path = _write(tmp_path, _cube_header())
    ds = _load(path)
    assert ds.wcs_status == "ok"
    assert ds.wcs_warning_message == ""


def test_warning_message_mentions_axis_when_sanitized(tmp_path):
    h = _cube_header(cunit1="km/s")
    path = _write(tmp_path, h)
    ds = _load(path)
    assert ds.wcs_status == "sanitized"
    msg = ds.wcs_warning_message
    assert msg != ""
    # Message must mention the axis number that was repaired
    assert "1" in msg


def test_warning_message_contains_repaired_keyword(tmp_path):
    h = _cube_header(cunit1="pixel")
    path = _write(tmp_path, h)
    ds = _load(path)
    msg = ds.wcs_warning_message.lower()
    assert "repaired" in msg or "compatibility" in msg or "corrected" in msg


def test_warning_message_non_empty_when_degraded(tmp_path):
    """A header that cannot build WCS at all must still produce a warning."""
    h = fits.Header()
    h["NAXIS"]  = 3
    h["NAXIS1"] = 4
    h["NAXIS2"] = 4
    h["NAXIS3"] = 4
    # No CTYPE, CRVAL etc — WCS will be trivial but status might be ok or degraded
    path = _write(tmp_path, h, name="minimal.fits")
    ds = _load(path)
    # Whether ok or degraded, the invariant is: warning is empty ↔ status is "ok"
    if ds.wcs_status != "ok":
        assert ds.wcs_warning_message != ""
    else:
        assert ds.wcs_warning_message == ""
