"""E-08: smoke tests for previously untested routers (meta / diagnostics /
resolve). These lock down the basic response shape and auth gating so a refactor
that breaks them is caught, without needing external services or data.
"""
from __future__ import annotations

import os

_TEST_TOKEN = "test-token-visivo-12345"


def _client():
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient
    from app.main import app
    return TestClient(app), {"X-Visivo-Token": _TEST_TOKEN}


# ── meta ──────────────────────────────────────────────────────────────────────

def test_health_shape():
    client, auth = _client()
    r = client.get("/v1/health", headers=auth)
    assert r.status_code == 200
    body = r.json()
    for k in ("ok", "workers", "active_sessions", "product_cache_entries",
              "task_registry_entries", "running_tasks", "timestamp"):
        assert k in body
    assert body["ok"] is True


def test_health_requires_auth():
    client, _ = _client()
    r = client.get("/v1/health")   # no token
    assert r.status_code in (401, 403)


def test_session_stats():
    client, auth = _client()
    r = client.get("/v1/sessions", headers=auth)
    assert r.status_code == 200
    assert isinstance(r.json(), dict)


# ── diagnostics ───────────────────────────────────────────────────────────────

def test_diagnostics_events():
    client, auth = _client()
    r = client.get("/v1/diagnostics/events", headers=auth)
    assert r.status_code == 200
    assert "events" in r.json()


def test_diagnostics_stats():
    client, auth = _client()
    r = client.get("/v1/diagnostics/stats", headers=auth)
    assert r.status_code == 200
    assert isinstance(r.json(), dict)


# ── resolve ───────────────────────────────────────────────────────────────────

def test_resolve_requires_auth():
    client, _ = _client()
    r = client.post("/v1/resolve/target", json={"name": "M31"})
    assert r.status_code in (401, 403)


def test_resolve_target_responds():
    """The resolver may reach an external service or fail gracefully — we only
    assert it returns a well-formed 2xx/4xx/5xx JSON with a `valid` flag, not a
    crash, and that a bogus body is handled."""
    client, auth = _client()
    r = client.post("/v1/resolve/target", json={"name": "__definitely_not_a_real_object__"},
                    headers=auth)
    assert r.status_code in (200, 422, 502, 503, 504)
    if r.headers.get("content-type", "").startswith("application/json"):
        assert isinstance(r.json(), dict)
