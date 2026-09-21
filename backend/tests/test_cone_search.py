"""Tests for the VO Simple Cone Search.

The legacy shelled out to a bundled pyvo script through whichever Python the
user had configured, then split the output on commas. These tests cover what
replaced it: building a correct SCS URL against services that already carry
parameters, reading the VOTable (including the error it can carry *inside* a
valid document), and saving the result as a catalogue.
"""
from __future__ import annotations

import io

import pytest

from app.cone_search import (
    ConeSearchError,
    build_url,
    parse_votable,
    position_columns,
    run_cone_search,
    write_csv,
)


VOTABLE = """<?xml version="1.0"?>
<VOTABLE version="1.3" xmlns="http://www.ivoa.net/xml/VOTable/v1.3">
 <RESOURCE type="results">
  <TABLE>
   <FIELD name="_RAJ2000" datatype="double" unit="deg"/>
   <FIELD name="_DEJ2000" datatype="double" unit="deg"/>
   <FIELD name="Jmag" datatype="float"/>
   <FIELD name="Name" datatype="char" arraysize="*"/>
   <DATA><TABLEDATA>
    <TR><TD>83.8221</TD><TD>-5.3911</TD><TD>12.3</TD><TD>alpha</TD></TR>
    <TR><TD>83.8300</TD><TD>-5.3800</TD><TD>13.1</TD><TD>beta</TD></TR>
   </TABLEDATA></DATA>
  </TABLE>
 </RESOURCE>
</VOTABLE>
"""

ERROR_VOTABLE = """<?xml version="1.0"?>
<VOTABLE version="1.3" xmlns="http://www.ivoa.net/xml/VOTable/v1.3">
 <INFO name="Error" value="ERROR">Radius must be positive</INFO>
 <RESOURCE type="results"/>
</VOTABLE>
"""


# ── Building the request ─────────────────────────────────────────────────────

def test_the_standard_parameters_are_added():
    url = build_url("https://example.org/scs", 83.82, -5.39, 0.05)
    assert "RA=83.82000000" in url
    assert "DEC=-5.39000000" in url
    assert "SR=0.05000000" in url
    assert "VERB=2" in url


def test_a_service_url_that_already_has_parameters_keeps_them():
    """VizieR cone-search URLs carry their own query; concatenating a '?' makes
    a URL that is not a URL."""
    url = build_url("https://vizier.cds.unistra.fr/viz-bin/conesearch/II/246/out?-out.max=50",
                    10.0, 20.0, 0.1)
    assert url.count("?") == 1
    assert "-out.max=50" in url
    assert "RA=10.00000000" in url


def test_the_standard_parameters_are_not_duplicated():
    """A pasted URL that already has RA/DEC in it must not end up with two."""
    url = build_url("https://example.org/scs?RA=1&DEC=2&SR=3", 10.0, 20.0, 0.1)
    assert url.count("RA=") == 1 and url.count("SR=") == 1
    assert "RA=10.00000000" in url


def test_right_ascension_wraps():
    assert "RA=350.00000000" in build_url("https://example.org/scs", -10.0, 0.0, 0.1)


@pytest.mark.parametrize("bad", ["", "   ", "ftp://example.org/scs", "not a url"])
def test_a_url_that_is_not_a_service_is_refused(bad):
    with pytest.raises(ConeSearchError):
        build_url(bad, 10.0, 20.0, 0.1)


def test_a_declination_off_the_sky_is_refused():
    with pytest.raises(ConeSearchError, match="Dec must be"):
        build_url("https://example.org/scs", 10.0, 100.0, 0.1)


@pytest.mark.parametrize("radius", [0.0, -1.0, 120.0])
def test_a_radius_that_is_not_a_radius_is_refused(radius):
    with pytest.raises(ConeSearchError, match="radius"):
        build_url("https://example.org/scs", 10.0, 20.0, radius)


def test_a_non_numeric_coordinate_is_refused_by_name():
    with pytest.raises(ConeSearchError, match="RA must be a number"):
        build_url("https://example.org/scs", "over there", 20.0, 0.1)


def test_an_unknown_verbosity_is_refused():
    with pytest.raises(ConeSearchError, match="1, 2 or 3"):
        build_url("https://example.org/scs", 10.0, 20.0, 0.1, verbosity=9)


# ── Reading the answer ───────────────────────────────────────────────────────

def test_columns_and_rows_come_out_of_the_votable():
    columns, rows = parse_votable(VOTABLE.encode())
    assert columns == ["_RAJ2000", "_DEJ2000", "Jmag", "Name"]
    assert len(rows) == 2
    assert rows[0][0] == pytest.approx(83.8221)
    assert rows[0][3] == "alpha"


def test_an_error_carried_inside_a_valid_votable_is_raised():
    """A VO service reports failure with HTTP 200 and an INFO element, so an
    empty table is not necessarily an empty sky."""
    with pytest.raises(ConeSearchError, match="Radius must be positive"):
        parse_votable(ERROR_VOTABLE.encode())


def test_something_that_is_not_a_votable_says_so():
    with pytest.raises(ConeSearchError, match="readable VOTable"):
        parse_votable(b"<html><body>gateway timeout</body></html>")


def test_the_position_columns_are_identified():
    assert position_columns(["_RAJ2000", "_DEJ2000", "Jmag"]) == {"ra": "_RAJ2000",
                                                                  "dec": "_DEJ2000"}
    assert position_columns(["RA", "Dec"]) == {"ra": "RA", "dec": "Dec"}
    assert position_columns(["x", "y"]) == {}


def test_the_result_is_saved_as_a_catalogue(tmp_path):
    columns, rows = parse_votable(VOTABLE.encode())
    path = write_csv(columns, rows, str(tmp_path / "cone.csv"))
    lines = open(path, encoding="utf-8").read().strip().splitlines()
    assert lines[0] == "_RAJ2000,_DEJ2000,Jmag,Name"
    assert len(lines) == 3


# ── End to end ───────────────────────────────────────────────────────────────

class _Response:
    def __init__(self, payload):
        self._payload = payload

    def read(self):
        return self._payload

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def test_a_search_returns_rows_and_a_saved_catalogue(tmp_path, monkeypatch):
    monkeypatch.setattr("app.safe_fetch.safe_urlopen",
                        lambda *a, **k: _Response(VOTABLE.encode()))
    result = run_cone_search("https://example.org/scs", 83.82, -5.39, 0.05, 2,
                             str(tmp_path / "cone.csv"))
    assert result["total_rows"] == 2
    assert result["truncated"] is False
    assert result["position_columns"]["ra"] == "_RAJ2000"
    assert (tmp_path / "cone.csv").exists()


def test_the_row_cap_does_not_lose_the_saved_rows(tmp_path, monkeypatch):
    """The cap is for the table on screen; the CSV keeps everything."""
    monkeypatch.setattr("app.safe_fetch.safe_urlopen",
                        lambda *a, **k: _Response(VOTABLE.encode()))
    result = run_cone_search("https://example.org/scs", 83.82, -5.39, 0.05, 2,
                             str(tmp_path / "cone.csv"), max_rows=1)
    assert len(result["rows"]) == 1
    assert result["total_rows"] == 2
    assert result["truncated"] is True
    assert len(open(result["output_path"], encoding="utf-8").read().strip().splitlines()) == 3


def test_a_private_address_is_refused(tmp_path, monkeypatch):
    """The service URL is typed by the user, so it is a fetch target like any
    other. The real guard runs here — patching the fetch would remove it."""
    monkeypatch.delenv("VISIVO_CONE_SEARCH_ALLOWED_HOSTS", raising=False)
    with pytest.raises(ConeSearchError, match="SSRF"):
        run_cone_search("http://127.0.0.1:8000/scs", 10.0, 20.0, 0.1, 2,
                        str(tmp_path / "cone.csv"))
    assert not (tmp_path / "cone.csv").exists()


def test_a_table_stored_elsewhere_is_not_fetched():
    """A VOTable can keep its data in a STREAM with an href, and the parser will
    go and get it — with no guard of ours in the way. A public service could
    point that at 127.0.0.1, or at file:///etc/passwd, and have the contents
    come back as table cells."""
    payload = (b'<?xml version="1.0"?><VOTABLE><RESOURCE><TABLE>'
               b'<FIELD name="x" datatype="double"/>'
               b'<DATA><BINARY><STREAM href="file:///etc/passwd"/></BINARY></DATA>'
               b'</TABLE></RESOURCE></VOTABLE>')
    with pytest.raises(ConeSearchError, match="stored elsewhere"):
        parse_votable(payload)


def test_the_stream_guard_survives_an_encoding():
    """The same document declared UTF-16 is the same ASCII with a zero between
    every character, and a pattern written for UTF-8 slides straight past it."""
    payload = ('<?xml version="1.0" encoding="UTF-16"?><VOTABLE><RESOURCE><TABLE>'
               '<FIELD name="x" datatype="double"/>'
               '<DATA><BINARY><STREAM href="http://127.0.0.1/private"/></BINARY></DATA>'
               '</TABLE></RESOURCE></VOTABLE>').encode("utf-16")
    with pytest.raises(ConeSearchError, match="stored elsewhere"):
        parse_votable(payload)


def test_the_stream_guard_sees_through_a_namespace_prefix():
    """<v:STREAM …> is the same element; a pattern anchored on "<STREAM" is not."""
    payload = ('<?xml version="1.0"?>'
               '<v:VOTABLE xmlns:v="http://www.ivoa.net/xml/VOTable/v1.3">'
               '<v:RESOURCE><v:TABLE><v:FIELD name="x" datatype="double"/>'
               '<v:DATA><v:BINARY><v:STREAM href="http://127.0.0.1/private"/></v:BINARY></v:DATA>'
               '</v:TABLE></v:RESOURCE></v:VOTABLE>').encode()
    with pytest.raises(ConeSearchError, match="stored elsewhere"):
        parse_votable(payload)


def test_an_attribute_containing_a_bracket_does_not_hide_the_stream():
    """`<STREAM rights=">" href="…">` is valid XML, and any pattern written as
    "up to the closing bracket" can be stepped around with it. This is why the
    check parses."""
    payload = ('<?xml version="1.0"?><VOTABLE><RESOURCE><TABLE>'
               '<FIELD name="x" datatype="double"/>'
               '<DATA><BINARY><STREAM rights="&gt;" href="http://127.0.0.1/private"/>'
               '</BINARY></DATA></TABLE></RESOURCE></VOTABLE>').encode()
    with pytest.raises(ConeSearchError, match="stored elsewhere"):
        parse_votable(payload)


def test_a_document_with_a_dtd_is_refused():
    """No VOTable needs one, and entity expansion is a denial of service
    waiting to happen."""
    payload = (b'<?xml version="1.0"?>'
               b'<!DOCTYPE VOTABLE [<!ENTITY a "aaaaaaaaaa">]>'
               b'<VOTABLE><RESOURCE/></VOTABLE>')
    with pytest.raises(ConeSearchError, match="DTD"):
        parse_votable(payload)


def test_inline_binary_data_is_still_read():
    """A STREAM without an href carries its data inline and is not the problem."""
    import base64
    import struct

    encoded = base64.b64encode(struct.pack(">d", 1.5)).decode()
    payload = (f'<?xml version="1.0"?><VOTABLE version="1.3"><RESOURCE><TABLE>'
               f'<FIELD name="x" datatype="double"/>'
               f'<DATA><BINARY><STREAM encoding="base64">{encoded}</STREAM></BINARY></DATA>'
               f'</TABLE></RESOURCE></VOTABLE>').encode()
    columns, rows = parse_votable(payload)
    assert columns == ["x"]
    assert rows[0][0] == pytest.approx(1.5)


def test_a_stream_without_an_href_is_still_read():
    """Inline binary data carries no href and is not the problem."""
    payload = (b'<?xml version="1.0"?><VOTABLE><RESOURCE><TABLE>'
               b'<FIELD name="x" datatype="double"/>'
               b'<DATA><TABLEDATA><TR><TD>1.0</TD></TR></TABLEDATA></DATA>'
               b'</TABLE></RESOURCE></VOTABLE>')
    columns, rows = parse_votable(payload)
    assert columns == ["x"] and len(rows) == 1


# ── The routes ───────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_the_service_list_is_served(client, auth_headers):
    body = (await client.get("/v1/resolve/cone_search/services", headers=auth_headers)).json()
    assert body["services"]
    assert all("url" in service and "name" in service for service in body["services"])


@pytest.mark.asyncio
async def test_the_route_reports_a_bad_radius_without_searching(client, auth_headers, monkeypatch):
    monkeypatch.setattr("app.safe_fetch.safe_urlopen",
                        lambda *a, **k: pytest.fail("must not fetch"))
    response = await client.post(
        "/v1/resolve/cone_search",
        json={"url": "https://example.org/scs", "ra": 1.0, "dec": 2.0, "radius": 0.0},
        headers=auth_headers)
    body = response.json()
    assert body["valid"] is False
    assert "radius" in body["error"]
