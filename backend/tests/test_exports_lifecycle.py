"""End-to-end tests for the Workspace Exports lifecycle:

  * helper safety  (make_workspace_path / resolve_workspace_path)
  * GET    /v1/exports/list
  * GET    /v1/exports/download
  * DELETE /v1/exports/{filename}

The workspace dir is monkey-patched to a per-test tmp_path so the tests
never touch the user's real ``~/.visivo/exports/``.
"""
from __future__ import annotations

from pathlib import Path

import pytest
import pytest_asyncio
from httpx import AsyncClient


# ── Fixture: redirect the workspace to a tmpdir for every test ──────────────

@pytest_asyncio.fixture
async def isolated_workspace(monkeypatch, tmp_path) -> Path:
    """Point _EXPORTS_DIR and the helper accessors at a tmpdir so tests
    operate on a clean, empty workspace."""
    from app import dependencies
    workspace = tmp_path / "exports"
    workspace.mkdir()
    monkeypatch.setattr(dependencies, "_EXPORTS_DIR", workspace)
    # Some callers import the accessor directly — patch it too so they
    # return the same redirected directory regardless of import order.
    monkeypatch.setattr(dependencies, "get_exports_dir", lambda: workspace)
    yield workspace


# ── make_workspace_path: collision handling + sanitisation ──────────────────

class TestMakeWorkspacePath:
    def test_basic_basename(self, isolated_workspace):
        from app.dependencies import make_workspace_path
        p = make_workspace_path("cube.fits")
        assert p == isolated_workspace / "cube.fits"

    def test_collision_appends_numeric_suffix(self, isolated_workspace):
        from app.dependencies import make_workspace_path
        (isolated_workspace / "cube.fits").touch()
        p = make_workspace_path("cube.fits")
        assert p.name == "cube_1.fits"
        p.touch()
        assert make_workspace_path("cube.fits").name == "cube_2.fits"

    def test_no_extension_defaults_to_fits(self, isolated_workspace):
        from app.dependencies import make_workspace_path
        assert make_workspace_path("noext").name == "noext.fits"

    def test_path_traversal_stripped(self, isolated_workspace):
        """Slashes / leading dots in the basename must NOT escape the
        workspace; the result must always live under it."""
        from app.dependencies import make_workspace_path
        p = make_workspace_path("../outside.fits")
        assert p.parent == isolated_workspace
        assert p.name == "outside.fits"
        p2 = make_workspace_path("subdir/inside.fits")
        assert p2.parent == isolated_workspace
        assert p2.name == "inside.fits"

    def test_empty_basename_falls_back(self, isolated_workspace):
        from app.dependencies import make_workspace_path
        assert make_workspace_path("").name == "export.fits"
        assert make_workspace_path("   ").name == "export.fits"


# ── resolve_workspace_path: refuses traversal ───────────────────────────────

class TestResolveWorkspacePath:
    def test_simple_basename(self, isolated_workspace):
        from app.dependencies import resolve_workspace_path
        assert resolve_workspace_path("a.fits") == isolated_workspace / "a.fits"

    def test_traversal_rejected(self, isolated_workspace):
        from app.dependencies import resolve_workspace_path
        with pytest.raises(ValueError):
            resolve_workspace_path("../../etc/passwd")
        with pytest.raises(ValueError):
            resolve_workspace_path("sub/a.fits")
        with pytest.raises(ValueError):
            resolve_workspace_path(".")
        with pytest.raises(ValueError):
            resolve_workspace_path("")


# ── /v1/exports/list ────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_exports_list_empty(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    resp = await client.get("/v1/exports/list", headers=auth_headers)
    assert resp.status_code == 200
    body = resp.json()
    assert body["valid"] is True
    assert body["entries"] == []
    assert body["workspace_dir"] == str(isolated_workspace)


@pytest.mark.asyncio
async def test_exports_list_populated_and_sorted(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    """Listing reports each regular file with size + mtime, sorted
    newest-first."""
    import os
    import time
    a = isolated_workspace / "older.fits"; a.write_bytes(b"AAAA")
    time.sleep(0.05)
    b = isolated_workspace / "newer.fits"; b.write_bytes(b"BBBBBB")
    # Touch with explicit mtimes so the ordering check is deterministic
    os.utime(a, (a.stat().st_atime, 1_000_000_000))
    os.utime(b, (b.stat().st_atime, 2_000_000_000))

    resp = await client.get("/v1/exports/list", headers=auth_headers)
    assert resp.status_code == 200
    entries = resp.json()["entries"]
    names = [e["filename"] for e in entries]
    assert names == ["newer.fits", "older.fits"]
    assert entries[0]["size"] == 6
    assert entries[1]["size"] == 4


# ── /v1/exports/download ────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_exports_download_streams_bytes(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    f = isolated_workspace / "moment.fits"
    payload = b"FITS PAYLOAD CONTENT"
    f.write_bytes(payload)
    resp = await client.get(
        "/v1/exports/download",
        params={"filename": "moment.fits"},
        headers=auth_headers,
    )
    assert resp.status_code == 200
    assert resp.content == payload
    assert resp.headers["content-type"] == "application/fits"


@pytest.mark.asyncio
async def test_exports_download_traversal_rejected(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    """A relative-path / absolute-path filename must NOT be honoured."""
    resp = await client.get(
        "/v1/exports/download",
        params={"filename": "../sensitive.fits"},
        headers=auth_headers,
    )
    assert resp.status_code == 400


@pytest.mark.asyncio
async def test_exports_download_missing_returns_404(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    resp = await client.get(
        "/v1/exports/download",
        params={"filename": "doesnotexist.fits"},
        headers=auth_headers,
    )
    assert resp.status_code == 404


# ── DELETE /v1/exports/{name} ───────────────────────────────────────────────

@pytest.mark.asyncio
async def test_exports_delete_removes_file(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    f = isolated_workspace / "todelete.fits"
    f.write_bytes(b"x")
    assert f.exists()
    resp = await client.delete(
        "/v1/exports/todelete.fits", headers=auth_headers,
    )
    assert resp.status_code == 200
    body = resp.json()
    assert body["valid"] is True
    assert body["deleted_filename"] == "todelete.fits"
    assert not f.exists()


@pytest.mark.asyncio
async def test_exports_delete_idempotent_on_missing(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    """Deleting a non-existent file returns a 200 with valid=false (no
    HTTP exception) so the client can keep its UI flow consistent."""
    resp = await client.delete(
        "/v1/exports/never_existed.fits", headers=auth_headers,
    )
    assert resp.status_code == 200
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_exports_delete_traversal_rejected(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    """Path-segment filenames are sanitised → traversal cannot happen."""
    resp = await client.delete(
        "/v1/exports/..%2Fdangerous.fits", headers=auth_headers,
    )
    # FastAPI may URL-decode and route this differently; either 400 (our
    # validator refuses) or 404 (file genuinely missing) is acceptable —
    # the absolute requirement is that nothing OUTSIDE the workspace gets
    # touched.
    assert resp.status_code in (400, 404)
