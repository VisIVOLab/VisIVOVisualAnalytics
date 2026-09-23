"""
test_hips_tile_query.py – regression tests for the HiPS viewport routes.

`/v1/hips/{survey_id}/query_tiles` and `/v1/hips/catalogue_overlay` are what
make the HiPS viewer show anything at all, and neither had a test. Both shipped
broken: the router refactor left them calling `_asyncio`, a name the star-import
from `dependencies` does not re-export, so every call died on a NameError that
the surrounding `except Exception` reported as a plausible-looking
"HEALPix query failed" / invalid response. The viewer drew an empty sky.

These tests exercise the routes through the ASGI app with no network: the
survey is registered directly in the session registry, so only the HEALPix
maths and the response wiring are under test.

Run with:
    cd backend
    pytest tests/test_hips_tile_query.py -v
"""

from __future__ import annotations

import math

import pytest

from app.dependencies import _HEALPY_AVAILABLE

pytestmark = pytest.mark.skipif(
    not _HEALPY_AVAILABLE, reason="healpy is not installed in this environment"
)


def _register_survey() -> tuple[str, dict]:
    """Put a HiPS survey straight into the registry, no `properties` fetch.

    Registered on the anonymous session, which is what a request carrying no
    X-Visivo-Session header resolves to — the same path the desktop client
    takes when it has not adopted a session id.
    """
    from app.sessions import REGISTRY

    session = REGISTRY.get_or_create_anon()
    survey_id = "hips_testsurvey01"
    session.datasets[survey_id] = {
        "kind": "hips",
        "base_url": "https://example.invalid/DSS/DSSColor",
        "title": "Test Survey",
        "frame": "equatorial",
        "max_order": 9,
        "min_order": 0,
        "tile_width": 512,
        "tile_formats": ["jpeg"],
        "properties": {},
    }
    return survey_id, {}


@pytest.mark.asyncio
async def test_query_tiles_returns_a_covering_tile_set(client, auth_headers):
    survey_id, session_headers = _register_survey()

    response = await client.post(
        f"/v1/hips/{survey_id}/query_tiles",
        json={
            "survey_id": survey_id,
            "ra_center": 10.6847,   # M31
            "dec_center": 41.269,
            "fov_deg": 1.0,
            "order": 7,
        },
        headers={**auth_headers, **session_headers},
    )
    assert response.status_code == 200
    body = response.json()

    # The failure this guards against reported valid=False with a message that
    # read like a healpy problem, so assert on the error text too.
    assert body["valid"] is True, body["error"]
    assert body["error"] == ""
    assert body["tiles"], "a 1° field must be covered by at least one tile"

    for tile in body["tiles"]:
        assert tile["order"] == 7
        assert len(tile["corners_ra"]) == 4
        assert len(tile["corners_dec"]) == 4
        assert -90.0 <= tile["dec_center"] <= 90.0

    # Order 7 tiles are ~0.7° across, so a 1° field needs a handful of them and
    # nothing like the whole sphere.
    assert 1 <= len(body["tiles"]) <= 64


@pytest.mark.asyncio
async def test_query_tiles_honours_an_explicit_radius(client, auth_headers):
    """A field of view is quoted across the width. A tall window, or a curved
    projection, reaches further than half of it, and the tiles for the corners
    were simply never asked for."""
    survey_id, session_headers = _register_survey()

    async def query(**extra):
        body = {"survey_id": survey_id, "ra_center": 0.0, "dec_center": 0.0,
                "fov_deg": 10.0, "order": 5}
        body.update(extra)
        return (await client.post(f"/v1/hips/{survey_id}/query_tiles", json=body,
                                  headers={**auth_headers, **session_headers})).json()

    narrow = await query()
    wide = await query(radius_deg=11.05)      # the corner of a 600x1200 viewport
    assert narrow["valid"] and wide["valid"]
    assert len(wide["tiles"]) > len(narrow["tiles"])
    assert set(t["pix"] for t in narrow["tiles"]) < set(t["pix"] for t in wide["tiles"])


@pytest.mark.asyncio
async def test_a_galactic_survey_returns_tiles_around_the_requested_sky_position():
    """A HiPS is tiled in its own frame, and more than half the CDS registry is
    galactic. Reading those tile coordinates as RA/Dec put a galactic survey's
    imagery roughly a hundred degrees from where it belongs."""
    from app.dependencies import _get_tiles_for_fov

    ra, dec = 10.6847, 41.269          # M31: galactic (121.17, -21.57)
    tiles = _get_tiles_for_fov(ra, dec, 1.0, 8, "galactic")
    assert tiles

    def separation(ra1, dec1, ra2, dec2):
        r1, d1, r2, d2 = map(math.radians, (ra1, dec1, ra2, dec2))
        return math.degrees(math.acos(max(-1.0, min(1.0,
            math.sin(d1) * math.sin(d2)
            + math.cos(d1) * math.cos(d2) * math.cos(r1 - r2)))))

    # Every tile is near the requested position, in ICRS — the disc is queried
    # with a sqrt(2) margin, so a degree or so beyond the field is expected.
    worst = max(separation(t["ra_center"], t["dec_center"], ra, dec) for t in tiles)
    assert worst < 2.0, f"a tile came back {worst:.1f} deg from the requested position"

    # And the frame actually made a difference, so this is not passing because
    # the conversion silently did nothing.
    equatorial = _get_tiles_for_fov(ra, dec, 1.0, 8, "equatorial")
    assert {t["pix"] for t in tiles} != {t["pix"] for t in equatorial}


@pytest.mark.asyncio
async def test_query_tiles_clamps_the_order_to_the_survey_maximum(client, auth_headers):
    survey_id, session_headers = _register_survey()

    response = await client.post(
        f"/v1/hips/{survey_id}/query_tiles",
        json={
            "survey_id": survey_id,
            "ra_center": 0.0,
            "dec_center": 0.0,
            "fov_deg": 0.05,
            "order": 25,          # far beyond the survey's max_order of 9
        },
        headers={**auth_headers, **session_headers},
    )
    body = response.json()
    assert body["valid"] is True, body["error"]
    assert all(t["order"] == 9 for t in body["tiles"])


@pytest.mark.asyncio
async def test_query_tiles_returns_the_whole_sphere_for_an_all_sky_field(client, auth_headers):
    """The all-sky projections ask for a 360-degree field.

    The covering disc is then wider than the sphere, and healpy's query_disc
    refuses a radius above pi — so this used to come back empty and the
    whole-sky view had nothing to draw.
    """
    survey_id, session_headers = _register_survey()

    response = await client.post(
        f"/v1/hips/{survey_id}/query_tiles",
        json={
            "survey_id": survey_id,
            "ra_center": 180.0,
            "dec_center": 0.0,
            "fov_deg": 360.0,
            "order": 3,
        },
        headers={**auth_headers, **session_headers},
    )
    body = response.json()
    assert body["valid"] is True, body["error"]
    # Order 3 tiles the whole sphere in exactly 12 * 4**3 of them, once each.
    assert len(body["tiles"]) == 768
    assert len({t["pix"] for t in body["tiles"]}) == 768


@pytest.mark.asyncio
async def test_catalogue_overlay_reports_a_missing_file_rather_than_crashing(
        client, auth_headers, tmp_path):
    """The overlay route ran the same undefined-name path as query_tiles."""
    response = await client.post(
        "/v1/hips/catalogue_overlay",
        json={
            "catalogue_path": str(tmp_path / "does_not_exist.csv"),
            "ra_center": 0.0,
            "dec_center": 0.0,
            "fov_deg": 1.0,
        },
        headers=auth_headers,
    )
    assert response.status_code == 200
    body = response.json()
    assert body["valid"] is False
    # A NameError would surface as "_asyncio is not defined"; the honest answer
    # is about the file.
    assert "_asyncio" not in body["error"]


@pytest.mark.asyncio
async def test_catalogue_overlay_keeps_a_source_across_the_pole(client, auth_headers, tmp_path):
    """Near a pole every right ascension is in view, and no interval in RA says
    so. The old filter widened RA by the cosine of the *centre's* declination,
    which threw away a source six degrees away because its RA was on the other
    side of the pole."""
    csv_path = tmp_path / "polar.csv"
    csv_path.write_text("ra,dec,name\n"
                        "180.0,89.0,across_the_pole\n"
                        "0.0,85.0,at_the_centre\n"
                        "270.0,10.0,far_away\n")

    response = await client.post(
        "/v1/hips/catalogue_overlay",
        json={"catalogue_path": str(csv_path),
              "ra_center": 0.0, "dec_center": 85.0, "fov_deg": 20.0},
        headers=auth_headers,
    )
    body = response.json()
    assert body["valid"] is True, body["error"]
    assert body["total_in_view"] == 2
    assert sorted(round(s["dec"]) for s in body["sources"]) == [85, 89]


@pytest.mark.asyncio
async def test_catalogue_overlay_honours_an_explicit_radius(client, auth_headers, tmp_path):
    """A field of view measured across the width says nothing about how far the
    corners reach, so a source visible in a corner was dropped. The client
    measures the radius that covers its own viewport and sends it."""
    csv_path = tmp_path / "corner.csv"
    csv_path.write_text("ra,dec,name\n0.0,0.0,centre\n9.0,9.0,corner\n40.0,0.0,outside\n")

    async def overlay(**extra):
        body = {"catalogue_path": str(csv_path),
                "ra_center": 0.0, "dec_center": 0.0, "fov_deg": 20.0}
        body.update(extra)
        return (await client.post("/v1/hips/catalogue_overlay", json=body,
                                  headers=auth_headers)).json()

    # 0.6 x 20 = 12 degrees: the corner source is 12.7 degrees out and is missed.
    assert (await overlay())["total_in_view"] == 1
    # The measured radius of a square viewport reaches it.
    body = await overlay(radius_deg=14.5)
    assert body["total_in_view"] == 2
    assert {s["name"] if "name" in s else round(s["ra"]) for s in body["sources"]} == {0, 9}


@pytest.mark.asyncio
async def test_catalogue_overlay_over_the_whole_sky_keeps_the_antipode(
        client, auth_headers, tmp_path):
    """At a radius of 180 degrees rounding could put the cosine a hair below
    -1, which dropped the point opposite the view centre."""
    csv_path = tmp_path / "antipode.csv"
    csv_path.write_text("ra,dec\n0.0,-88.30000000000004\n180.0,88.30000000000004\n")

    body = (await client.post(
        "/v1/hips/catalogue_overlay",
        json={"catalogue_path": str(csv_path), "ra_center": 0.0,
              "dec_center": -88.30000000000004, "fov_deg": 360.0, "radius_deg": 180.0},
        headers=auth_headers)).json()
    assert body["valid"] is True, body["error"]
    assert body["total_in_view"] == 2


@pytest.mark.asyncio
async def test_catalogue_overlay_returns_sources_inside_the_field(
        client, auth_headers, tmp_path):
    csv_path = tmp_path / "sources.csv"
    csv_path.write_text(
        "ra,dec,name\n"
        "10.0,20.0,inside\n"
        "10.1,20.1,inside_too\n"
        "200.0,-60.0,far_away\n"
    )

    response = await client.post(
        "/v1/hips/catalogue_overlay",
        json={
            "catalogue_path": str(csv_path),
            "ra_center": 10.0,
            "dec_center": 20.0,
            "fov_deg": 1.0,
        },
        headers=auth_headers,
    )
    body = response.json()
    assert body["valid"] is True, body["error"]
    assert body["total_in_view"] == 2
    assert {round(s["ra"], 1) for s in body["sources"]} == {10.0, 10.1}
