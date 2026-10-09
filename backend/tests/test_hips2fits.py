"""Tests for the hips2fits cutout service.

The legacy form sent whatever was in its fields and let CDS refuse; most of
these are about refusing first, with a message that says what is wrong.
"""
from __future__ import annotations

import json

import pytest

from app import hips2fits


def _query(**overrides):
    params = {"hips": "CDS/P/DSS2/color", "width": 256, "height": 256,
              "ra": 83.82, "dec": -5.39, "fov": 0.5}
    params.update(overrides)
    return hips2fits.build_query(**params)


# ── Building the request ─────────────────────────────────────────────────────

def test_a_valid_request_carries_every_parameter_the_service_needs():
    params = _query()
    assert params["hips"] == "CDS/P/DSS2/color"
    assert params["projection"] == "TAN"
    assert params["coordsys"] == "icrs"
    assert params["format"] == "fits"
    assert float(params["ra"]) == pytest.approx(83.82)


def test_an_empty_survey_is_refused():
    with pytest.raises(ValueError, match="Choose a HiPS survey"):
        _query(hips="   ")


@pytest.mark.parametrize("width,height", [(0, 256), (256, 0), (-5, 5)])
def test_a_degenerate_size_is_refused(width, height):
    with pytest.raises(ValueError, match="at least 1 pixel"):
        _query(width=width, height=height)


def test_an_enormous_cutout_is_refused_before_the_service_spends_an_hour_on_it():
    with pytest.raises(ValueError, match="Mpx"):
        _query(width=8000, height=8000)


def test_a_single_absurd_side_is_refused():
    with pytest.raises(ValueError, match="limited to"):
        _query(width=50000, height=1)


def test_a_declination_off_the_sky_is_refused():
    with pytest.raises(ValueError, match="Dec must be"):
        _query(dec=120.0)


def test_right_ascension_wraps_rather_than_being_refused():
    assert float(_query(ra=-10.0)["ra"]) == pytest.approx(350.0)
    assert float(_query(ra=370.0)["ra"]) == pytest.approx(10.0)


@pytest.mark.parametrize("fov", [0.0, -1.0, 200.0])
def test_a_field_of_view_that_is_not_a_field_is_refused(fov):
    with pytest.raises(ValueError, match="field of view"):
        _query(fov=fov)


def test_a_non_numeric_coordinate_is_refused_by_name():
    with pytest.raises(ValueError, match="RA must be a number"):
        _query(ra="over there")


def test_an_unknown_projection_names_the_ones_that_work():
    with pytest.raises(ValueError, match="TAN"):
        _query(projection="XYZ")


def test_the_projection_is_taken_from_a_descriptive_label():
    """The legacy's combo held "TAN (gnomonic)" and sent its first 3 letters."""
    assert _query(projection="tan (gnomonic)")["projection"] == "TAN"


def test_an_unknown_coordinate_system_is_refused():
    with pytest.raises(ValueError, match="icrs or galactic"):
        _query(coordsys="supergalactic")


# ── Cuts and stretch belong to pictures, not to FITS ─────────────────────────

def test_a_fits_request_carries_no_cuts():
    params = _query(image_format="fits", min_cut=1.0, max_cut=99.0)
    assert "min_cut" not in params and "stretch" not in params


def test_a_picture_request_carries_them_as_percentages():
    params = _query(image_format="png", min_cut=1.0, max_cut=99.0, stretch="sqrt")
    assert params["min_cut"] == "1.0%" and params["max_cut"] == "99.0%"
    assert params["stretch"] == "sqrt"


def test_inverted_cuts_are_refused():
    with pytest.raises(ValueError, match="percentiles"):
        _query(image_format="png", min_cut=99.0, max_cut=1.0)


def test_an_unknown_stretch_is_refused():
    with pytest.raises(ValueError, match="asinh"):
        _query(image_format="png", stretch="magic")


def test_an_unknown_format_is_refused():
    with pytest.raises(ValueError, match="fits, jpg or png"):
        _query(image_format="tiff")


# ── Naming and errors ────────────────────────────────────────────────────────

def test_the_output_is_named_after_the_survey():
    assert hips2fits.default_basename("CDS/P/DSS2/color", "fits") \
        == "hips2fits_CDS_P_DSS2_color.fits"


def test_the_services_own_message_is_preferred():
    payload = json.dumps({"description": "Survey xyz not found"}).encode()
    assert hips2fits.service_error(payload) == "Survey xyz not found"


def test_a_plain_text_error_is_passed_through():
    assert hips2fits.service_error(b"bad fov") == "bad fov"


def test_an_html_error_page_is_not_shown_to_the_user():
    message = hips2fits.service_error(b"<html><body>500</body></html>", 500)
    assert "<html>" not in message
    assert "500" in message


def test_a_fits_is_recognised_by_its_first_card():
    assert hips2fits.looks_like_fits(b"SIMPLE  =                    T")
    assert not hips2fits.looks_like_fits(b"<html>")


def test_the_query_url_is_built_from_the_endpoint():
    url = hips2fits.query_url(_query())
    assert url.startswith(hips2fits.endpoint())
    assert "hips=CDS%2FP%2FDSS2%2Fcolor" in url


# ── Fetching ─────────────────────────────────────────────────────────────────

def test_an_error_body_is_not_saved_under_a_fits_name(tmp_path, monkeypatch):
    """The legacy wrote whatever came back, so a refusal became a .fits file."""
    class _Response:
        def read(self):
            return json.dumps({"description": "Survey xyz not found"}).encode()
        def __enter__(self):
            return self
        def __exit__(self, *args):
            return False

    monkeypatch.setattr("app.safe_fetch.safe_urlopen", lambda *a, **k: _Response())
    out = tmp_path / "cutout.fits"
    with pytest.raises(ValueError, match="Survey xyz not found"):
        hips2fits.fetch_cutout(_query(), str(out))
    assert not out.exists()


def test_a_fits_response_is_written(tmp_path, monkeypatch):
    payload = b"SIMPLE  =                    T" + b" " * 100

    class _Response:
        def read(self):
            return payload
        def __enter__(self):
            return self
        def __exit__(self, *args):
            return False

    monkeypatch.setattr("app.safe_fetch.safe_urlopen", lambda *a, **k: _Response())
    out = tmp_path / "cutout.fits"
    result = hips2fits.fetch_cutout(_query(), str(out))
    assert out.read_bytes() == payload
    assert result["is_fits"] is True
    assert result["bytes"] == len(payload)


def test_an_empty_response_is_refused(tmp_path, monkeypatch):
    class _Response:
        def read(self):
            return b""
        def __enter__(self):
            return self
        def __exit__(self, *args):
            return False

    monkeypatch.setattr("app.safe_fetch.safe_urlopen", lambda *a, **k: _Response())
    with pytest.raises(ValueError, match="empty response"):
        hips2fits.fetch_cutout(_query(), str(tmp_path / "cutout.fits"))


def test_a_picture_response_is_not_checked_for_a_fits_card(tmp_path, monkeypatch):
    payload = b"\x89PNG\r\n\x1a\n" + b"\x00" * 32

    class _Response:
        def read(self):
            return payload
        def __enter__(self):
            return self
        def __exit__(self, *args):
            return False

    monkeypatch.setattr("app.safe_fetch.safe_urlopen", lambda *a, **k: _Response())
    out = tmp_path / "cutout.png"
    result = hips2fits.fetch_cutout(_query(image_format="png"), str(out))
    assert result["is_fits"] is False
    assert out.exists()


# ── The routes ───────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_the_survey_list_is_served(client, auth_headers):
    body = (await client.get("/v1/hips/hips2fits/surveys", headers=auth_headers)).json()
    assert "CDS/P/DSS2/color" in body["surveys"]
    assert "TAN" in body["projections"]
    assert body["endpoint"].startswith("http")


@pytest.mark.asyncio
async def test_the_route_refuses_a_bad_request_without_calling_the_service(
        client, auth_headers, monkeypatch):
    def _explode(*args, **kwargs):
        raise AssertionError("the service must not be called for an invalid request")

    monkeypatch.setattr("app.safe_fetch.safe_urlopen", _explode)
    response = await client.post("/v1/hips/hips2fits",
                                 json={"hips": "CDS/P/DSS2/color", "fov": 0.0},
                                 headers=auth_headers)
    body = response.json()
    assert body["valid"] is False
    assert "field of view" in body["error"]
