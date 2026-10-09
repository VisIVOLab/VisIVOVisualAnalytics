"""Tests for the FITS header editor.

The legacy dialog wrote three keyword groups into a copy and reported success
whether or not the result was readable. These tests are about what replaced
that: any keyword, structural ones refused, values typed as numbers rather than
quoted strings, and an answer about whether the WCS now works.
"""
from __future__ import annotations

import numpy as np
import pytest
from astropy.io import fits

from app.fits_header_edit import (
    HeaderEditError,
    apply_patch,
    coerce_value,
    describe_wcs,
    validate_key,
    worker_patch_header,
)


def _image(path, **cards):
    header = fits.Header()
    for key, value in cards.items():
        header[key] = value
    fits.PrimaryHDU(data=np.zeros((8, 8), dtype=np.float32), header=header).writeto(path)
    return path


def _celestial_cards():
    return {"CRPIX1": 4.0, "CRPIX2": 4.0, "CRVAL1": 10.0, "CRVAL2": -5.0,
            "CDELT1": -0.001, "CDELT2": 0.001,
            "CTYPE1": "RA---TAN", "CTYPE2": "DEC--TAN"}


# ── Keywords ─────────────────────────────────────────────────────────────────

def test_a_keyword_is_upper_cased_and_trimmed():
    assert validate_key("  bunit ") == "BUNIT"


@pytest.mark.parametrize("key", ["SIMPLE", "BITPIX", "NAXIS", "NAXIS1", "naxis3", "END",
                                 "BSCALE", "BZERO", "BLANK"])
def test_structural_keywords_are_refused(key):
    """They describe the bytes on disk; typing over them makes the header lie.

    BSCALE and BZERO are in the same category one level up: editing one does not
    annotate the data, it multiplies every value in it."""
    with pytest.raises(HeaderEditError, match="structure"):
        validate_key(key)


@pytest.mark.parametrize("key", ["", "TOOLONGKEY", "BAD KEY", "BAD;"])
def test_a_malformed_keyword_is_refused(key):
    with pytest.raises(HeaderEditError):
        validate_key(key)


# ── Values ───────────────────────────────────────────────────────────────────

def test_a_numeric_string_becomes_a_number():
    """A quoted '-0.001' in CDELT1 is a string card that WCS parsers ignore —
    exactly the breakage the editor exists to repair."""
    assert coerce_value("-0.001") == pytest.approx(-0.001)
    assert isinstance(coerce_value("3"), int)


def test_quoting_forces_a_string():
    assert coerce_value("'3'") == "3"


def test_booleans_are_recognised():
    assert coerce_value("T") is True
    assert coerce_value("false") is False


def test_text_stays_text():
    assert coerce_value("RA---TAN") == "RA---TAN"
    assert coerce_value("") == ""


# ── Patching ─────────────────────────────────────────────────────────────────

def test_a_change_reports_the_old_value_too():
    header = fits.Header({"BUNIT": "Jy/beam"})
    changes = apply_patch(header, [{"key": "BUNIT", "value": "MJy/sr"}])
    assert header["BUNIT"] == "MJy/sr"
    assert changes == ["BUNIT: 'Jy/beam' → 'MJy/sr'"]


def test_an_added_keyword_says_so():
    header = fits.Header()
    changes = apply_patch(header, [{"key": "BMAJ", "value": "0.002", "comment": "beam"}])
    assert header["BMAJ"] == pytest.approx(0.002)
    assert header.comments["BMAJ"] == "beam"
    assert "added" in changes[0]


def test_a_comment_only_edit_is_applied():
    """The comment is part of the card; comparing values alone drops it."""
    header = fits.Header()
    header["BUNIT"] = ("Jy/beam", "old note")
    changes = apply_patch(header, [{"key": "BUNIT", "value": "Jy/beam",
                                    "comment": "new note"}])
    assert header.comments["BUNIT"] == "new note"
    assert "comment" in changes[0]


def test_a_comment_can_be_cleared():
    """An empty comment means "make it empty"; omitting the field means "leave
    it". Without the distinction a comment can be written but never removed."""
    header = fits.Header()
    header["OBJECT"] = ("M42", "old note")
    apply_patch(header, [{"key": "OBJECT", "value": "M42", "comment": ""}])
    assert header.comments["OBJECT"] == ""


def test_an_omitted_comment_is_left_alone():
    header = fits.Header()
    header["OBJECT"] = ("M42", "keep me")
    apply_patch(header, [{"key": "OBJECT", "value": "NGC 1976"}])
    assert header.comments["OBJECT"] == "keep me"


def test_an_unchanged_value_is_reported_as_unchanged():
    header = fits.Header({"BUNIT": "K"})
    assert apply_patch(header, [{"key": "BUNIT", "value": "K"}]) == ["BUNIT: unchanged ('K')"]


def test_removal():
    header = fits.Header({"EQUINOX": 1950.0})
    changes = apply_patch(header, [], ["EQUINOX"])
    assert "EQUINOX" not in header
    assert "removed" in changes[0]


def test_removing_what_is_not_there_is_not_an_error():
    changes = apply_patch(fits.Header(), [], ["BUNIT"])
    assert "nothing removed" in changes[0]


def test_history_is_appended_not_replaced():
    header = fits.Header()
    apply_patch(header, [{"key": "HISTORY", "value": "one"}])
    apply_patch(header, [{"key": "HISTORY", "value": "two"}])
    assert [str(v) for v in header["HISTORY"]] == ["one", "two"]


# ── What the header now amounts to ───────────────────────────────────────────

def test_a_celestial_header_is_reported_as_celestial():
    verdict = describe_wcs(fits.Header(_celestial_cards()))
    assert verdict["status"] == "celestial"
    assert verdict["celestial"] is True
    assert "RA---TAN" in verdict["message"]


def test_a_header_without_ctype_is_reported_as_degraded():
    verdict = describe_wcs(fits.Header({"CRPIX1": 1.0, "CDELT1": 1.0}))
    assert verdict["status"] == "degraded"
    assert verdict["celestial"] is False


# ── End to end ───────────────────────────────────────────────────────────────

def test_repairing_a_header_reports_that_it_now_works(tmp_path):
    """The legacy's own use case: the CTYPEs are missing and the user types them."""
    source = _image(tmp_path / "broken.fits", CRPIX1=4.0, CRPIX2=4.0,
                    CRVAL1=10.0, CRVAL2=-5.0, CDELT1=-0.001, CDELT2=0.001)
    out = tmp_path / "fixed.fits"
    result = worker_patch_header(str(source), str(out), [
        {"key": "CTYPE1", "value": "RA---TAN"},
        {"key": "CTYPE2", "value": "DEC--TAN"},
    ])
    assert result["wcs_celestial"] is True
    assert result["wcs_status"] == "celestial"
    with fits.open(out) as hdul:
        assert hdul[0].header["CTYPE1"] == "RA---TAN"


def test_a_wrong_repair_says_so_instead_of_claiming_success(tmp_path):
    """"File has been saved!" was the legacy's answer either way."""
    source = _image(tmp_path / "broken.fits", CRPIX1=4.0)
    out = tmp_path / "still_broken.fits"
    result = worker_patch_header(str(source), str(out),
                                 [{"key": "CTYPE1", "value": "NONSENSE"}])
    assert result["wcs_celestial"] is False


def test_the_input_file_is_not_touched(tmp_path):
    source = _image(tmp_path / "in.fits", BUNIT="Jy/beam")
    worker_patch_header(str(source), str(tmp_path / "out.fits"),
                        [{"key": "BUNIT", "value": "K"}])
    with fits.open(source) as hdul:
        assert hdul[0].header["BUNIT"] == "Jy/beam"


def test_extensions_are_carried_into_the_copy(tmp_path):
    path = tmp_path / "multi.fits"
    column = fits.Column(name="X", format="E", array=np.array([1.0, 2.0]))
    fits.HDUList([
        fits.PrimaryHDU(data=np.zeros((4, 4), dtype=np.float32)),
        fits.BinTableHDU.from_columns([column], name="SOURCES"),
    ]).writeto(path)
    out = tmp_path / "multi_out.fits"
    worker_patch_header(str(path), str(out), [{"key": "BUNIT", "value": "K"}])
    with fits.open(out) as hdul:
        assert len(hdul) == 2
        assert hdul[1].name == "SOURCES"
        assert len(hdul[1].data) == 2


def test_an_empty_patch_is_refused(tmp_path):
    source = _image(tmp_path / "in.fits")
    with pytest.raises(HeaderEditError, match="No changes"):
        worker_patch_header(str(source), str(tmp_path / "out.fits"), [])


def test_a_structural_edit_is_refused_end_to_end(tmp_path):
    source = _image(tmp_path / "in.fits")
    with pytest.raises(HeaderEditError, match="structure"):
        worker_patch_header(str(source), str(tmp_path / "out.fits"),
                            [{"key": "NAXIS1", "value": 4}])


# ── The route ────────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_the_route_writes_a_copy_and_reports_the_wcs(client, auth_headers, tmp_path):
    source = _image(tmp_path / "broken.fits", CRPIX1=4.0, CRPIX2=4.0,
                    CRVAL1=10.0, CRVAL2=-5.0, CDELT1=-0.001, CDELT2=0.001)
    response = await client.post(
        "/v1/files/header/patch",
        json={"path": str(source),
              "cards": [{"key": "CTYPE1", "value": "RA---TAN"},
                        {"key": "CTYPE2", "value": "DEC--TAN"}]},
        headers=auth_headers)
    body = response.json()
    assert body["valid"] is True
    assert body["wcs_celestial"] is True
    assert body["workspace_filename"] == "broken_hdr.fits"
    assert len(body["changes"]) == 2


@pytest.mark.asyncio
async def test_the_route_refuses_a_structural_keyword(client, auth_headers, tmp_path):
    source = _image(tmp_path / "in.fits")
    response = await client.post(
        "/v1/files/header/patch",
        json={"path": str(source), "cards": [{"key": "BITPIX", "value": 8}]},
        headers=auth_headers)
    body = response.json()
    assert body["valid"] is False
    assert "structure" in body["error"]


@pytest.mark.asyncio
async def test_the_route_hides_whether_a_path_exists(client, auth_headers, tmp_path):
    response = await client.post(
        "/v1/files/header/patch",
        json={"path": str(tmp_path / "absent.fits"),
              "cards": [{"key": "BUNIT", "value": "K"}]},
        headers=auth_headers)
    assert response.json()["error"] == "File not found."
