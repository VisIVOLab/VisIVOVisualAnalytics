"""GET /v1/vr/backend_check (issue #187): whether this backend can serve the
desktop's headset, so a stale cluster overlay is visible before the page fails
on a missing route; the details go to the startup log only."""
from __future__ import annotations

import logging

import pytest
from httpx import AsyncClient

from app.routers import vr_backend_check as vc


async def test_the_check_says_what_the_desktop_reads_and_nothing_else(client: AsyncClient,
                                                                       auth_headers: dict):
    r = await client.get("/v1/vr/backend_check", headers=auth_headers)
    assert r.status_code == 200
    body = r.json()
    assert set(body) == {"vr_api", "page", "missing_routes", "stereo"}
    assert body["vr_api"] == vc.VR_API >= 2
    assert set(body["page"]) == {"present", "build"}
    assert body["page"]["present"] and len(body["page"]["build"]) == 12
    # This backend serves every route the page calls (the render ones too
    # when its render router is mounted, as in the tests).
    assert body["missing_routes"] == []
    assert body["stereo"] == {"depth": body["stereo"]["depth"]}
    assert body["stereo"]["depth"] in ("auto", "off")


async def test_the_check_needs_the_token(client: AsyncClient):
    r = await client.get("/v1/vr/backend_check")
    assert r.status_code in (401, 403)


def test_every_route_the_page_calls_is_found(client):
    from app.main import app
    routes = vc.page_routes(app)
    assert set(routes) >= {"ping", "sessions_new", "files_list", "datasets_open",
                           "render_queue_cancel", "render_delete", "render_ws"}
    assert all(routes.values()), [k for k, v in routes.items() if not v]


def test_the_build_changes_with_the_page(tmp_path, monkeypatch):
    (tmp_path / "js").mkdir()
    (tmp_path / "index.html").write_text("<html></html>")
    (tmp_path / "js" / "main.js").write_text("// one")
    monkeypatch.setattr(vc, "_PAGE_DIR", tmp_path)
    vc.page_build.cache_clear()
    try:
        a = vc.page_build()
        (tmp_path / "js" / "main.js").write_text("// two")
        vc.page_build.cache_clear()
        b = vc.page_build()
        assert a["files"] == b["files"] == 2 and a["build"] != b["build"]
        monkeypatch.setattr(vc, "_PAGE_DIR", tmp_path / "absent")
        vc.page_build.cache_clear()
        assert vc.page_build() == {"present": False, "build": None, "files": 0, "newest": None}
    finally:
        vc.page_build.cache_clear()


def test_an_app_without_the_page_routes_logs_them_missing(caplog):
    from fastapi import FastAPI
    bare = FastAPI()

    @bare.post("/v1/cube/reduced_info")
    def _ri():   # pragma: no cover - never called
        return {}

    routes = vc.page_routes(bare)
    assert routes["reduced_info"] and not routes["render_ws"] and not routes["mask_stats_bin"]
    assert "mask_stats_bin" in vc.check(bare)["missing_routes"]
    with caplog.at_level(logging.INFO, logger="visivo.backend"):
        vc.log_startup(bare)
    line = next(r.getMessage() for r in caplog.records if "VR page" in r.getMessage())
    # The details live here, not in the route's answer.
    assert "MISSING" in line and "mask_stats_bin" in line and "render_ws" in line
    assert "backend code newest" in line and str(vc._APP_DIR) in line


@pytest.mark.parametrize("value, mode", [("0", "off"), ("off", "off"), ("", "auto"), ("1", "auto")])
def test_the_stereo_depth_mode_follows_the_environment(monkeypatch, value, mode):
    monkeypatch.setenv("VISIVO_XR_DEPTH", value)
    assert vc.stereo_depth() == mode
