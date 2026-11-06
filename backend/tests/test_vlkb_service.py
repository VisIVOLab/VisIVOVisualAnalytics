"""One VLKB service URL for the TAP queries and the multi-cutout batch (#76).

The query composer used to fall back to a built-in address and the batch to
none, so with ``VISIVO_VLKB_URL`` unset a search worked and the cutouts of what
it found answered 503. Both now resolve the URL through app/vlkb_service.py;
these tests hold them to it, through the helpers and through the routes.
"""
from __future__ import annotations

import os

import pytest

from app.vlkb_mcutout import resolve_service_url
from app.vlkb_service import DEFAULT_VLKB_URL, vlkb_service_url
from app.vlkb_tap import sync_url, tap_base

_TEST_TOKEN = "test-token-visivo-12345"


def _client():
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient

    from app.main import app
    return TestClient(app), {"X-Visivo-Token": _TEST_TOKEN}


# ── the rule ─────────────────────────────────────────────────────────────────

def test_unset_means_the_public_vlkb_for_both(monkeypatch):
    monkeypatch.delenv("VISIVO_VLKB_URL", raising=False)
    assert vlkb_service_url("") == DEFAULT_VLKB_URL
    assert tap_base("") == resolve_service_url("") == DEFAULT_VLKB_URL


def test_the_environment_is_used_by_both(monkeypatch):
    monkeypatch.setenv("VISIVO_VLKB_URL", "  https://mirror.example.org/vlkb/  ")
    assert tap_base("") == resolve_service_url("") == "https://mirror.example.org/vlkb"


def test_the_request_wins_for_both(monkeypatch):
    monkeypatch.setenv("VISIVO_VLKB_URL", "https://mirror.example.org/vlkb")
    explicit = "https://explicit.example.org/a/../vlkb/"
    assert tap_base(explicit) == resolve_service_url(explicit) \
        == "https://explicit.example.org/vlkb"


@pytest.mark.parametrize("blank", ["", "   "])
def test_a_blank_environment_is_unset(monkeypatch, blank):
    monkeypatch.setenv("VISIVO_VLKB_URL", blank)
    assert tap_base("") == resolve_service_url("") == DEFAULT_VLKB_URL


def test_the_default_is_the_service_root_the_endpoints_hang_from(monkeypatch):
    # The TAP and SODA endpoints live directly under the root, as the desktop's
    # own VLKB/url setting assumes; a "/vlkb" suffix would 404 on both.
    monkeypatch.delenv("VISIVO_VLKB_URL", raising=False)
    assert sync_url("", "SELECT 1").startswith("https://vlkb.ia2.inaf.it/tap/sync?")


# ── through the routes ───────────────────────────────────────────────────────

def test_with_nothing_configured_a_query_and_a_batch_reach_the_same_service(monkeypatch):
    """The 'done when' of #76, end to end: same URL, and no 503 for the batch."""
    monkeypatch.delenv("VISIVO_VLKB_URL", raising=False)
    client, auth = _client()
    seen: dict[str, str] = {}

    # TAP: catch the URL at the guarded open, before any connection.
    import app.safe_fetch as safe_fetch

    def _no_open(url, **_kw):
        seen["tap"] = url
        raise ValueError("stopped by the test")

    monkeypatch.setattr(safe_fetch, "safe_urlopen", _no_open)
    r = client.post("/v1/vlkb/tap", json={"query": "SELECT 1"}, headers=auth)
    assert r.status_code == 200 and r.json()["valid"] is False

    # Batch: the SSRF guard resolves DNS, so it is stubbed; the job calls are too.
    import app.routers.vlkb as vlkb_router
    import app.vlkb_mcutout as mcutout

    monkeypatch.setattr(vlkb_router, "assert_safe_fetch_url", lambda *_a, **_kw: None)

    def _submit(base, items, bearer):
        seen["mcutout"] = base
        return "job-1"

    monkeypatch.setattr(mcutout, "submit_job", _submit)
    monkeypatch.setattr(mcutout, "job_phase", lambda *_a, **_kw: "QUEUED")
    r = client.post("/v1/vlkb/mcutout/submit",
                    json={"items": [{"id": "ivo://a/1", "pos": "CIRCLE 40 0 0.3"}]},
                    headers=auth)
    assert r.status_code == 200, r.text
    assert r.json()["job_id"] == "job-1"

    assert seen["tap"].startswith(seen["mcutout"] + "/tap/sync?")
    assert seen["mcutout"] == DEFAULT_VLKB_URL


def test_the_presets_report_the_service_a_batch_would_use(monkeypatch):
    monkeypatch.setenv("VISIVO_VLKB_URL", "https://mirror.example.org/vlkb")
    client, auth = _client()
    r = client.get("/v1/vlkb/tap/presets", headers=auth)
    assert r.status_code == 200
    assert r.json()["service_url"] == resolve_service_url("")
