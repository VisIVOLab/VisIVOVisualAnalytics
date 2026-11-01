"""Filesystem confinement for the path-based endpoints.

Multi-token auth made the backend shareable, which turned the file browser from
a convenience into an exposure: without confinement any authenticated token can
enumerate the server's filesystem and open any FITS on it. ``data_roots()`` is
the boundary; these tests pin its three modes and the hidden-path rule.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from app.dependencies import _normalize_path, data_roots


@pytest.fixture
def clean_env(monkeypatch):
    """Neither confinement nor multi-user mode, unless a test asks for it."""
    monkeypatch.delenv("VISIVO_DATA_ROOTS", raising=False)
    monkeypatch.delenv("VISIVO_TOKENS", raising=False)
    return monkeypatch


# ── single user: the backend acts for the one person running it ──────────────

def test_single_user_is_unconfined(clean_env):
    assert data_roots() == []
    # Browsing the server's own filesystem is the point of a remote backend.
    assert _normalize_path("/etc") == Path("/etc").resolve()


def test_single_user_allows_hidden_paths(clean_env):
    home = Path.home()
    assert _normalize_path(str(home / ".config")) == (home / ".config").resolve()


# ── explicit roots: enforced in either mode ──────────────────────────────────

def test_explicit_root_allows_inside(clean_env, tmp_path):
    clean_env.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    nested = tmp_path / "survey" / "cube.fits"
    assert _normalize_path(str(nested)) == nested.resolve()


def test_explicit_root_allows_the_root_itself(clean_env, tmp_path):
    clean_env.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    assert _normalize_path(str(tmp_path)) == tmp_path.resolve()


def test_explicit_root_refuses_outside(clean_env, tmp_path):
    clean_env.setenv("VISIVO_DATA_ROOTS", str(tmp_path / "data"))
    (tmp_path / "data").mkdir()
    with pytest.raises(PermissionError):
        _normalize_path("/etc/passwd")


def test_explicit_root_refuses_traversal(clean_env, tmp_path):
    root = tmp_path / "data"
    root.mkdir()
    clean_env.setenv("VISIVO_DATA_ROOTS", str(root))
    # resolve() collapses the "..", so the escape is caught by the root check
    # rather than by any string matching.
    with pytest.raises(PermissionError):
        _normalize_path(str(root / ".." / "secret.fits"))


def test_explicit_root_refuses_sibling_with_shared_prefix(clean_env, tmp_path):
    """``/data`` must not authorise ``/data-private``."""
    root = tmp_path / "data"
    root.mkdir()
    (tmp_path / "data-private").mkdir()
    clean_env.setenv("VISIVO_DATA_ROOTS", str(root))
    with pytest.raises(PermissionError):
        _normalize_path(str(tmp_path / "data-private" / "x.fits"))


def test_multiple_roots(clean_env, tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    a.mkdir(), b.mkdir()
    clean_env.setenv("VISIVO_DATA_ROOTS", os.pathsep.join([str(a), str(b)]))
    assert _normalize_path(str(b / "cube.fits")) == (b / "cube.fits").resolve()
    with pytest.raises(PermissionError):
        _normalize_path(str(tmp_path / "c" / "cube.fits"))


# ── multi-user without explicit roots: fail closed ───────────────────────────

def test_multi_user_without_roots_denies_everything(clean_env):
    """No implicit default. A home-directory fallback would silently share
    whatever the service account happens to keep there."""
    clean_env.setenv("VISIVO_TOKENS", "alice-token-aaaa,bob-token-bbbb")
    assert data_roots() is None
    for path in ("/etc", str(Path.home()), str(Path.home() / "survey.fits")):
        with pytest.raises(PermissionError):
            _normalize_path(path)


def test_multi_user_hides_credential_paths(clean_env, tmp_path):
    clean_env.setenv("VISIVO_TOKENS", "alice-token-aaaa,bob-token-bbbb")
    clean_env.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    # Credentials are the reason for the rule: neither is scientific data.
    for hidden in (".visivo_token", ".ssh/id_rsa"):
        with pytest.raises(PermissionError):
            _normalize_path(str(tmp_path / hidden))


def test_multi_user_hidden_rule_applies_under_explicit_roots(clean_env, tmp_path):
    clean_env.setenv("VISIVO_TOKENS", "alice-token-aaaa,bob-token-bbbb")
    clean_env.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    assert _normalize_path(str(tmp_path / "cube.fits")) == (tmp_path / "cube.fits").resolve()
    with pytest.raises(PermissionError):
        _normalize_path(str(tmp_path / ".hidden" / "cube.fits"))


def test_roots_are_reread_per_call(clean_env, tmp_path):
    """A deployment can change the roots without restarting the backend."""
    assert data_roots() == []
    clean_env.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    assert data_roots() == [tmp_path.resolve()]
    clean_env.delenv("VISIVO_DATA_ROOTS")
    assert data_roots() == []


# ── the endpoint surface, not just the helper ────────────────────────────────

@pytest.mark.asyncio
async def test_files_list_refusal_is_not_enumerable(client, auth_headers, monkeypatch, tmp_path):
    """A denied directory must be indistinguishable from a missing one."""
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    denied = await client.get("/v1/files/list", params={"path": "/etc"},
                              headers=auth_headers)
    missing = await client.get("/v1/files/list",
                               params={"path": str(tmp_path / "nope")},
                               headers=auth_headers)
    assert denied.status_code == 200 and missing.status_code == 200
    assert denied.json()["valid"] is False and missing.json()["valid"] is False
    # Same shape, no path echoed back, so probing tells the caller nothing.
    assert denied.json()["current_path"] == ""


@pytest.mark.asyncio
async def test_files_list_without_a_path_opens_the_first_root(client, auth_headers, monkeypatch,
                                                              tmp_path):
    """A browser opening (no path) on a backend whose home is outside its roots
    (a cluster job: data root and workspace on /scratch) lands in the first root,
    the data root, instead of on "Directory not found"."""
    data = tmp_path / "data"
    exports = tmp_path / "ws" / "exports"
    data.mkdir()
    exports.mkdir(parents=True)
    (data / "cube.fits").write_bytes(b"")
    monkeypatch.setenv("VISIVO_DATA_ROOTS", os.pathsep.join([str(data), str(exports)]))
    monkeypatch.setenv("HOME", str(tmp_path / "home-outside"))
    resp = await client.get("/v1/files/list", params={"path": ""}, headers=auth_headers)
    body = resp.json()
    assert body["valid"] is True
    assert body["current_path"] == str(data.resolve())
    assert [e["name"] for e in body["entries"]] == ["cube.fits"]
    # An explicit path outside the roots is still refused.
    denied = await client.get("/v1/files/list", params={"path": str(tmp_path)}, headers=auth_headers)
    assert denied.json()["valid"] is False


def test_empty_path_keeps_an_allowed_home(clean_env, tmp_path, monkeypatch):
    home = tmp_path / "home"
    home.mkdir()
    monkeypatch.setenv("HOME", str(home))
    clean_env.setenv("VISIVO_DATA_ROOTS", os.pathsep.join([str(tmp_path / "other"), str(tmp_path)]))
    assert _normalize_path("") == home


@pytest.mark.asyncio
async def test_file_header_refuses_outside_roots(client, auth_headers, monkeypatch,
                                                 tmp_path, synthetic_fits_image):
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    resp = await client.post("/v1/files/header", json={"path": str(synthetic_fits_image)},
                             headers=auth_headers)
    assert resp.status_code == 200
    body = resp.json()
    assert body["valid"] is False and body["cards"] == []


@pytest.mark.asyncio
async def test_files_list_filters_hidden_children(client, auth_headers, monkeypatch, tmp_path):
    """Refusing a direct request for ~/.ssh is not enough — listing the parent
    must not hand out the name either."""
    (tmp_path / "survey.fits").write_bytes(b"")
    (tmp_path / ".secrets").mkdir()
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    monkeypatch.setenv("VISIVO_TOKENS", "alice-token-aaaa,bob-token-bbbb")

    resp = await client.get("/v1/files/list", params={"path": str(tmp_path)},
                            headers=auth_headers)
    names = [e["name"] for e in resp.json()["entries"]]
    assert "survey.fits" in names and ".secrets" not in names


@pytest.mark.asyncio
async def test_files_list_keeps_hidden_children_for_single_user(client, auth_headers,
                                                                monkeypatch, tmp_path):
    (tmp_path / ".config").mkdir()
    monkeypatch.delenv("VISIVO_TOKENS", raising=False)
    monkeypatch.delenv("VISIVO_DATA_ROOTS", raising=False)
    resp = await client.get("/v1/files/list", params={"path": str(tmp_path)},
                            headers=auth_headers)
    assert ".config" in [e["name"] for e in resp.json()["entries"]]


@pytest.mark.asyncio
async def test_velocity_open_is_confined(client, auth_headers, monkeypatch, tmp_path):
    """/v1/velocity/open stores the path in session state, so it must be
    confined before it is opened, not just before it is read."""
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    resp = await client.post("/v1/velocity/open",
                             json={"path": "/etc/hosts", "box_mpc": 100.0},
                             headers=auth_headers)
    assert resp.status_code == 200
    body = resp.json()
    assert body["valid"] is False
    # Neutral message: it must not say the path exists but is out of bounds.
    assert "not found" in body["error"].lower()
    assert "root" not in body["error"].lower()


@pytest.mark.asyncio
async def test_uncaught_refusal_is_a_uniform_404(client, auth_headers, monkeypatch, tmp_path):
    """Routes that do not build their own envelope must still answer 404 —
    never 403 (which would confirm the path exists) and never 500."""
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    resp = await client.post("/v1/datasets/open", json={"path": "/etc/hosts"},
                             headers=auth_headers)
    assert resp.status_code == 404
    assert resp.json()["error_code"] == "path_not_found"
