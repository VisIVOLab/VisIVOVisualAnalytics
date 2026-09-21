"""Tests for the VLKB query composer.

The legacy built these queries in a dialog and ran them through a bundled pyvo
script. The queries are the part worth keeping, so most of these tests pin what
each preset actually asks for — including the bubble query, which the legacy
had a button and an importer for but no query at all.
"""
from __future__ import annotations

import pytest

from app.vlkb_tap import (
    BANDS,
    PRESETS,
    VlkbQueryError,
    build_query,
    parse_csv,
    run_query,
    sync_url,
    tap_base,
)


# ── The box ──────────────────────────────────────────────────────────────────

def test_the_box_is_ordered_for_the_user():
    """The legacy sorted min/max in its setters; a box dragged right-to-left is
    the same box."""
    forward = build_query("band", 10.0, 12.0, -1.0, 1.0, band="70")
    backward = build_query("band", 12.0, 10.0, 1.0, -1.0, band="70")
    assert forward == backward
    assert "glon >= 10.000000" in forward and "glon <= 12.000000" in forward


def test_a_box_of_zero_size_is_refused():
    with pytest.raises(VlkbQueryError, match="zero width or height"):
        build_query("bandmerged", 10.0, 10.0, -1.0, 1.0)


def test_a_latitude_off_the_sky_is_refused():
    with pytest.raises(VlkbQueryError, match="latitude"):
        build_query("bandmerged", 10.0, 12.0, -100.0, 1.0)


def test_a_non_finite_box_is_refused():
    with pytest.raises(VlkbQueryError, match="finite"):
        build_query("bandmerged", float("nan"), 12.0, -1.0, 1.0)


# ── The presets ──────────────────────────────────────────────────────────────

def test_the_band_merged_view_uses_its_own_position_columns():
    """sed_view_final has glonft/glatft, not glon/glat — querying the wrong
    names is a 400 from the service."""
    query = build_query("bandmerged", 10.0, 12.0, -1.0, 1.0)
    assert "compactsources.sed_view_final" in query
    assert "glonft" in query and "glatft" in query


def test_a_per_band_table_is_named_after_its_wavelength():
    assert "compactsources.higal250" in build_query("band", 10.0, 12.0, -1.0, 1.0, band="250")
    assert "compactsources.higal70" in build_query("band", 10.0, 12.0, -1.0, 1.0, band="70um")


def test_an_unknown_band_lists_the_ones_that_exist():
    with pytest.raises(VlkbQueryError, match="70"):
        build_query("band", 10.0, 12.0, -1.0, 1.0, band="3000")


def test_filaments_are_joined_to_their_branches():
    query = build_query("filaments", 10.0, 12.0, -1.0, 1.0)
    assert "filaments.filaments" in query and "filaments.branches" in query
    assert "idfil_mos" in query
    # The box has to be qualified, or the join makes it ambiguous.
    assert "f.glon >=" in query


def test_bubbles_have_a_query_of_their_own():
    """The legacy's bubble mode fell through to the compact-source branch,
    which is why it never produced bubbles."""
    query = build_query("bubbles", 10.0, 12.0, -1.0, 1.0)
    assert "bubbles.bubbles" in query
    assert "contour" in query
    assert "glon_cen" in query
    assert "compactsources" not in query


def test_the_three_dimensional_selection_carries_cartesian_coordinates():
    query = build_query("distances", 10.0, 12.0, -1.0, 1.0)
    assert "compactsources.distances" in query
    assert "AS x" in query and "AS y" in query and "AS z" in query
    assert "radians(glon)" in query


def test_an_unknown_preset_is_refused():
    with pytest.raises(VlkbQueryError, match="Unknown query"):
        build_query("magic", 10.0, 12.0, -1.0, 1.0)


def test_every_advertised_preset_can_be_built():
    for preset in PRESETS:
        query = build_query(preset["id"], 10.0, 12.0, -1.0, 1.0, band=BANDS[0])
        assert query.lower().startswith("select")


# ── The request ──────────────────────────────────────────────────────────────

def test_the_sync_url_carries_the_tap_parameters():
    url = sync_url("https://vlkb.example.org/vlkb", "SELECT * FROM t")
    assert url.startswith("https://vlkb.example.org/vlkb/tap/sync?")
    assert "LANG=ADQL" in url and "FORMAT=csv" in url and "REQUEST=doQuery" in url


def test_a_row_limit_is_passed_as_maxrec():
    assert "MAXREC=50" in sync_url("https://vlkb.example.org/vlkb", "SELECT * FROM t", 50)


def test_only_select_statements_are_forwarded():
    """TAP sync is a read interface; anything else is a mistake at best."""
    with pytest.raises(VlkbQueryError, match="Only SELECT"):
        sync_url("https://vlkb.example.org/vlkb", "DROP TABLE compactsources.higal70")


def test_an_empty_query_is_refused():
    with pytest.raises(VlkbQueryError, match="empty"):
        sync_url("https://vlkb.example.org/vlkb", "   ")


def test_the_service_falls_back_to_the_configured_one(monkeypatch):
    monkeypatch.setenv("VISIVO_VLKB_URL", "https://mirror.example.org/vlkb")
    assert tap_base("") == "https://mirror.example.org/vlkb"
    assert tap_base("https://explicit.example.org") == "https://explicit.example.org"


# ── The answer ───────────────────────────────────────────────────────────────

def test_columns_and_rows_come_out_of_the_csv():
    columns, rows = parse_csv(b"idbubble,glon_cen,glat_cen\n1,10.5,0.1\n2,10.6,0.2\n")
    assert columns == ["idbubble", "glon_cen", "glat_cen"]
    assert rows == [["1", "10.5", "0.1"], ["2", "10.6", "0.2"]]


def test_blank_lines_are_not_rows():
    _, rows = parse_csv(b"a,b\n1,2\n\n")
    assert rows == [["1", "2"]]


def test_an_error_returned_as_a_votable_is_raised_as_its_message():
    """A TAP service answers a bad query with a VOTable even when CSV was
    asked for."""
    payload = (b'<?xml version="1.0"?><VOTABLE><RESOURCE type="results">'
               b'<INFO name="QUERY_STATUS" value="ERROR">Unknown column glox</INFO>'
               b'</RESOURCE></VOTABLE>')
    with pytest.raises(VlkbQueryError, match="Unknown column glox"):
        parse_csv(payload)


def test_an_html_error_page_does_not_reach_the_user_as_markup():
    with pytest.raises(VlkbQueryError, match="did not return a table"):
        parse_csv(b"<html><body>502 Bad Gateway</body></html>")


# ── End to end ───────────────────────────────────────────────────────────────

@pytest.fixture
def _trusted_service(monkeypatch):
    """Trust the fake host, so these tests exercise the query and not DNS.

    Setting the allowlist is also the documented way to point the backend at a
    VLKB on a private network.
    """
    monkeypatch.setenv("VISIVO_VLKB_ALLOWED_HOSTS", "vlkb.example.org")


class _Response:
    def __init__(self, payload):
        self._payload = payload

    def read(self):
        return self._payload

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def test_a_query_saves_every_row_and_previews_some(tmp_path, monkeypatch, _trusted_service):
    payload = b"a,b\n" + b"".join(f"{i},{i * 2}\n".encode() for i in range(50))
    monkeypatch.setattr("app.safe_fetch.safe_urlopen", lambda *a, **k: _Response(payload))
    result = run_query("https://vlkb.example.org/vlkb", "SELECT * FROM t",
                       str(tmp_path / "q.csv"), preview_rows=10)
    assert result["total_rows"] == 50
    assert len(result["rows"]) == 10
    assert result["truncated"] is True
    assert len(open(result["output_path"], encoding="utf-8").read().strip().splitlines()) == 51


def test_an_empty_result_saves_nothing(tmp_path, monkeypatch, _trusted_service):
    monkeypatch.setattr("app.safe_fetch.safe_urlopen", lambda *a, **k: _Response(b"a,b\n"))
    result = run_query("https://vlkb.example.org/vlkb", "SELECT * FROM t",
                       str(tmp_path / "q.csv"))
    assert result["total_rows"] == 0
    assert result["output_path"] == ""
    assert not (tmp_path / "q.csv").exists()


def test_hitting_the_row_limit_is_reported(tmp_path, monkeypatch, _trusted_service):
    payload = b"a\n" + b"".join(f"{i}\n".encode() for i in range(10))
    monkeypatch.setattr("app.safe_fetch.safe_urlopen", lambda *a, **k: _Response(payload))
    result = run_query("https://vlkb.example.org/vlkb", "SELECT * FROM t",
                       str(tmp_path / "q.csv"), max_rows=10)
    assert result["hit_row_limit"] is True


def test_a_private_service_address_is_refused(tmp_path, monkeypatch):
    """The real guard runs here: patching the fetch would remove it."""
    monkeypatch.delenv("VISIVO_VLKB_ALLOWED_HOSTS", raising=False)
    with pytest.raises(VlkbQueryError, match="SSRF"):
        run_query("http://127.0.0.1:9000/vlkb", "SELECT * FROM t", str(tmp_path / "q.csv"))
    assert not (tmp_path / "q.csv").exists()


# ── The routes ───────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_the_presets_are_served(client, auth_headers):
    body = (await client.get("/v1/vlkb/tap/presets", headers=auth_headers)).json()
    assert {p["id"] for p in body["presets"]} >= {"bubbles", "distances", "filaments"}
    assert "250" in body["bands"]


@pytest.mark.asyncio
async def test_the_route_refuses_a_degenerate_box_without_querying(
        client, auth_headers, monkeypatch):
    monkeypatch.setattr("app.safe_fetch.safe_urlopen",
                        lambda *a, **k: pytest.fail("must not fetch"))
    response = await client.post(
        "/v1/vlkb/tap",
        json={"preset": "bubbles", "glon_min": 10.0, "glon_max": 10.0,
              "glat_min": -1.0, "glat_max": 1.0},
        headers=auth_headers)
    body = response.json()
    assert body["valid"] is False
    assert "zero width" in body["error"]
