"""
test_hips_registry.py – the CDS HiPS registry behind the survey picker.

Typing a survey URL by hand was the only way into the HiPS viewer, which meant
you had to know one already. These cover the parsing and the on-disk cache, and
they are kept out of test_hips_tile_query.py because none of this needs healpy
and that module skips without it.

Run with:
    cd backend
    pytest tests/test_hips_registry.py -v
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

import pytest


def _record(**overrides) -> dict:
    record = {
        "ID": "CDS/P/DSS2/color",
        "obs_title": "DSS2 colour",
        "hips_service_url": "https://alasky.example/DSS2",
        "client_category": "Image/Optical/DSS",
        "obs_regime": "Optical",
        "hips_order": "9",
        "hips_frame": "equatorial",
        "hips_tile_format": "jpeg fits",
        "dataproduct_type": "image",
    }
    record.update(overrides)
    return record


# ── Parsing ───────────────────────────────────────────────────────────────────

def test_a_record_without_a_service_url_is_dropped():
    from app.dependencies import _parse_hips_registry

    payload = json.dumps([_record(), _record(ID="no-url", hips_service_url="")]).encode()
    surveys = _parse_hips_registry(payload)
    assert [s["id"] for s in surveys] == ["CDS/P/DSS2/color"]


def test_a_latin_1_byte_in_a_title_does_not_lose_the_whole_list():
    """The MOCServer emits a latin-1 micro sign; a strict decode throws away
    every one of the 1500 records over one byte."""
    from app.dependencies import _parse_hips_registry

    payload = (b'[{"ID":"x","obs_title":"850 \xb5m","hips_service_url":"https://e.example/x",'
               b'"dataproduct_type":"image"}]')
    surveys = _parse_hips_registry(payload)
    assert len(surveys) == 1
    assert surveys[0]["title"].startswith("850 ")


def test_an_unparseable_order_is_zero_rather_than_an_exception():
    from app.dependencies import _parse_hips_registry

    payload = json.dumps([_record(hips_order="not a number")]).encode()
    assert _parse_hips_registry(payload)[0]["max_order"] == 0


def test_the_kind_distinguishes_imagery_from_catalogues_and_cubes():
    from app.dependencies import _parse_hips_registry

    payload = json.dumps([
        _record(ID="img", dataproduct_type="image"),
        _record(ID="cat", dataproduct_type="catalog"),
        _record(ID="cube", dataproduct_type="cube"),
        _record(ID="none", dataproduct_type=""),
    ]).encode()
    kinds = {s["id"]: s["kind"] for s in _parse_hips_registry(payload)}
    assert kinds == {"img": "image", "cat": "catalog", "cube": "cube", "none": "unknown"}


def test_a_document_that_is_not_a_list_is_refused_with_a_reason():
    from app.dependencies import _parse_hips_registry

    with pytest.raises(ValueError, match="not a list"):
        _parse_hips_registry(b'{"error": "nope"}')


# ── The on-disk cache ─────────────────────────────────────────────────────────

@pytest.fixture
def registry_cache(monkeypatch, tmp_path: Path):
    import app.dependencies as m

    monkeypatch.setattr(m, "_HIPS_CACHE_DIR", tmp_path)
    return tmp_path / m._hips_registry_cache_path().name


def test_a_fresh_cache_is_used_without_fetching(monkeypatch, registry_cache):
    import app.dependencies as m

    registry_cache.parent.mkdir(parents=True, exist_ok=True)
    registry_cache.write_bytes(json.dumps([_record(ID="cached")]).encode())

    def _must_not_fetch(_url):
        raise AssertionError("a fresh cache must not be refetched")

    monkeypatch.setattr(m, "_hips_fetch_sync", _must_not_fetch)
    assert [s["id"] for s in m._load_hips_registry_sync()] == ["cached"]


def test_refresh_refetches_even_with_a_fresh_cache(monkeypatch, registry_cache):
    import app.dependencies as m

    registry_cache.parent.mkdir(parents=True, exist_ok=True)
    registry_cache.write_bytes(json.dumps([_record(ID="cached")]).encode())
    monkeypatch.setattr(m, "_hips_fetch_sync",
                        lambda _url: json.dumps([_record(ID="fetched")]).encode())

    assert [s["id"] for s in m._load_hips_registry_sync(refresh=True)] == ["fetched"]
    # and the new copy replaced the old one
    assert b"fetched" in registry_cache.read_bytes()


def test_a_stale_cache_is_refetched(monkeypatch, registry_cache):
    import app.dependencies as m

    registry_cache.parent.mkdir(parents=True, exist_ok=True)
    registry_cache.write_bytes(json.dumps([_record(ID="cached")]).encode())
    old = time.time() - m._HIPS_REGISTRY_TTL_SECONDS - 60
    os.utime(registry_cache, (old, old))

    monkeypatch.setattr(m, "_hips_fetch_sync",
                        lambda _url: json.dumps([_record(ID="fetched")]).encode())
    assert [s["id"] for s in m._load_hips_registry_sync()] == ["fetched"]


def test_a_damaged_cache_falls_back_to_the_network(monkeypatch, registry_cache):
    import app.dependencies as m

    registry_cache.parent.mkdir(parents=True, exist_ok=True)
    registry_cache.write_bytes(b"{ this is not json")
    monkeypatch.setattr(m, "_hips_fetch_sync",
                        lambda _url: json.dumps([_record(ID="fetched")]).encode())
    assert [s["id"] for s in m._load_hips_registry_sync()] == ["fetched"]


@pytest.mark.parametrize("method", ["stat", "read_bytes"])
def test_an_unreadable_cache_falls_back_to_the_network(monkeypatch, registry_cache, method):
    """A cache we cannot read is a cache miss, not a failed request — the
    permission error used to escape and fail the route with CDS available.

    Only the cache file itself is made unreadable: patching Path.stat outright
    also breaks the mkdir on the way to writing the new copy, which would make
    this pass by exercising the wrong path.
    """
    import app.dependencies as m

    registry_cache.parent.mkdir(parents=True, exist_ok=True)
    registry_cache.write_bytes(json.dumps([_record(ID="cached")]).encode())

    original = getattr(Path, method)

    def _explode(self, *args, **kwargs):
        if self == registry_cache:
            raise PermissionError("no")
        return original(self, *args, **kwargs)

    monkeypatch.setattr(Path, method, _explode)
    monkeypatch.setattr(m, "_hips_fetch_sync",
                        lambda _url: json.dumps([_record(ID="fetched")]).encode())
    assert [s["id"] for s in m._load_hips_registry_sync()] == ["fetched"]


def test_a_cache_dated_in_the_future_is_refetched(monkeypatch, registry_cache):
    """A clock that jumped would otherwise pin a stale list until it caught up."""
    import app.dependencies as m

    registry_cache.parent.mkdir(parents=True, exist_ok=True)
    registry_cache.write_bytes(json.dumps([_record(ID="cached")]).encode())
    ahead = time.time() + 86400
    os.utime(registry_cache, (ahead, ahead))

    monkeypatch.setattr(m, "_hips_fetch_sync",
                        lambda _url: json.dumps([_record(ID="fetched")]).encode())
    assert [s["id"] for s in m._load_hips_registry_sync()] == ["fetched"]


def test_a_failed_rename_leaves_no_fragment_and_keeps_the_old_cache(monkeypatch,
                                                                    registry_cache):
    """The point of the temporary file is that a failed write cannot destroy
    the last good copy — nor litter the cache directory with fragments."""
    import app.dependencies as m

    registry_cache.parent.mkdir(parents=True, exist_ok=True)
    registry_cache.write_bytes(json.dumps([_record(ID="cached")]).encode())

    monkeypatch.setattr(m, "_hips_fetch_sync",
                        lambda _url: json.dumps([_record(ID="fetched")]).encode())
    monkeypatch.setattr(m.os, "replace",
                        lambda *_a, **_k: (_ for _ in ()).throw(OSError("nope")))

    # The fetch still answers…
    assert [s["id"] for s in m._load_hips_registry_sync(refresh=True)] == ["fetched"]
    # …the previous cache is intact…
    assert b"cached" in registry_cache.read_bytes()
    # …and nothing else was left in the directory.
    assert [p.name for p in registry_cache.parent.iterdir()] == [registry_cache.name]


def test_an_unwritable_cache_still_returns_the_surveys(monkeypatch, tmp_path):
    """Not being able to save the list only costs us the next fetch."""
    import app.dependencies as m

    # A regular file where the cache directory should be: mkdir then fails
    # with NotADirectoryError, which is the ordinary shape of "cannot write".
    blocker = tmp_path / "blocker"
    blocker.write_text("not a directory")
    monkeypatch.setattr(m, "_HIPS_CACHE_DIR", blocker / "hips")
    monkeypatch.setattr(m, "_hips_fetch_sync",
                        lambda _url: json.dumps([_record(ID="fetched")]).encode())
    assert [s["id"] for s in m._load_hips_registry_sync()] == ["fetched"]


def test_the_cache_file_is_named_after_the_query(monkeypatch, tmp_path):
    """Changing the fields we ask CDS for must not go on serving a cached
    document that does not have them."""
    import app.dependencies as m

    monkeypatch.setattr(m, "_HIPS_CACHE_DIR", tmp_path)
    first = m._hips_registry_cache_path()
    monkeypatch.setattr(m, "_HIPS_REGISTRY_URL", m._HIPS_REGISTRY_URL + ",extra_field")
    assert m._hips_registry_cache_path() != first


# ── The route ─────────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_surveys_route_filters_by_kind(client, auth_headers, monkeypatch):
    import app.routers.hips as hips_router

    records = [
        {"id": "a", "title": "an image", "url": "https://example/a", "category": "Image",
         "regime": "Optical", "frame": "equatorial", "tile_formats": ["jpeg"],
         "kind": "image", "max_order": 9},
        {"id": "b", "title": "a catalogue", "url": "https://example/b", "category": "",
         "regime": "", "frame": "", "tile_formats": [], "kind": "catalog", "max_order": 5},
    ]
    monkeypatch.setattr(hips_router, "_load_hips_registry_sync", lambda refresh: records)

    body = (await client.get("/v1/hips/surveys", headers=auth_headers)).json()
    assert body["valid"] is True
    assert [s["id"] for s in body["surveys"]] == ["a"]

    body = (await client.get("/v1/hips/surveys?kind=", headers=auth_headers)).json()
    assert [s["id"] for s in body["surveys"]] == ["a", "b"]


@pytest.mark.asyncio
async def test_surveys_route_reports_an_unreachable_registry(client, auth_headers, monkeypatch):
    import app.routers.hips as hips_router

    def _explode(_refresh):
        raise OSError("the registry is down")

    monkeypatch.setattr(hips_router, "_load_hips_registry_sync", _explode)
    body = (await client.get("/v1/hips/surveys", headers=auth_headers)).json()
    assert body["valid"] is False
    assert "registry is down" in body["error"]




@pytest.mark.asyncio
async def test_opening_a_planetary_surface_survey_is_refused(client, auth_headers, monkeypatch):
    """The registry carries maps of Mars, the Moon and Io. Treating their frame
    as equatorial, as this used to, drew a planet's terrain onto the sky at
    coordinates that mean nothing. The picker hides them; a pasted URL must be
    refused as well."""
    import app.routers.hips as hips_router

    async def _properties(_url):
        return (b"obs_title = Mars OpenPlanetary Basemap\n"
                b"hips_frame = mars\n"
                b"hips_order = 8\n"
                b"hips_tile_format = png\n")

    monkeypatch.setattr(hips_router, "_hips_fetch", _properties)
    body = (await client.post("/v1/hips/open",
                              json={"url": "https://alasky.example/Mars"},
                              headers=auth_headers)).json()
    assert body["valid"] is False
    assert "mars" in body["error"].lower()


@pytest.mark.asyncio
async def test_opening_a_galactic_survey_is_accepted(client, auth_headers, monkeypatch):
    import app.routers.hips as hips_router

    async def _properties(_url):
        return (b"obs_title = A galactic survey\n"
                b"hips_frame = galactic\n"
                b"hips_order = 6\n"
                b"hips_tile_format = png\n")

    monkeypatch.setattr(hips_router, "_hips_fetch", _properties)
    body = (await client.post("/v1/hips/open",
                              json={"url": "https://alasky.example/gal"},
                              headers=auth_headers)).json()
    assert body["valid"] is True, body["error"]
    assert body["frame"] == "galactic"
