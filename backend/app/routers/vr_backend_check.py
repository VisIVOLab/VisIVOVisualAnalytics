"""Whether this backend can serve the desktop's headset (issue #187):
``GET /v1/vr/backend_check`` (with the token).

A backend on a cluster often runs the code of an overlay (``--backend-src``,
a copy of ``backend/app`` bound over the image's) that is older than the
desktop pairing with it. The headset's page then loads, and its newer calls
fail one by one with a bare 404. This route answers the few facts the
desktop's *Open in VR Headset…* checks before pairing: whether the backend
serves the page and which build (a hash of the files under
``app/static/vr``), which routes the page calls it lacks, the headset protocol
level and whether its stereo frames carry depth. The details for an operator
(code directory and dates, every route) go to one line of the startup log.
"""
from __future__ import annotations

import hashlib
import logging
from datetime import datetime, timezone
from functools import lru_cache
from pathlib import Path

from fastapi import APIRouter, Request

router = APIRouter(tags=["vr"])
logger = logging.getLogger("visivo.backend")

#: The level of the headset protocol this backend speaks. 2 (issue #187):
#: frame headers say ``reprojection`` / ``depth_unavailable``, a running
#: stream takes a new ack window, and this route exists.
VR_API = 2

_APP_DIR = Path(__file__).resolve().parents[1]
_PAGE_DIR = _APP_DIR / "static" / "vr"

#: The routes the page calls, by the name the check reports them under.
PAGE_ROUTES = {
    "ping": ("GET", "/v1/ping"),
    "sessions_new": ("POST", "/v1/sessions/new"),
    "files_list": ("GET", "/v1/files/list"),
    "datasets_open": ("POST", "/v1/datasets/open"),
    "reduced_info": ("POST", "/v1/cube/reduced_info"),
    "reduced_bin": ("POST", "/v1/cube/reduced_bin"),
    "subvolume_bin": ("POST", "/v1/cube/subvolume_bin"),
    "kinematic_select_bin": ("POST", "/v1/cube/kinematic_select_bin"),
    "mask_stats_bin": ("POST", "/v1/cube/mask_stats_bin"),
    "spectral_probe": ("POST", "/v1/spectral/probe"),
    "pixel_to_sky": ("POST", "/v1/datasets/pixel_to_sky"),
    "render_create_session": ("POST", "/v1/render/create_session"),
    "render_queue_cancel": ("DELETE", "/v1/render/queue/{queue_id}"),
    "render_delete": ("DELETE", "/v1/render/{session_id}"),
    "render_ws": ("WS", "/v1/render/{session_id}/ws"),
}


def _iso(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat(timespec="seconds")


@lru_cache(maxsize=1)
def page_build() -> dict:
    """The page's build: a short hash over its files' paths and bytes, their
    count and the newest one's time. Read once (the files do not change
    under a running backend)."""
    if not _PAGE_DIR.is_dir():
        return {"present": False, "build": None, "files": 0, "newest": None}
    digest = hashlib.sha256()
    newest, count = 0.0, 0
    for path in sorted(p for p in _PAGE_DIR.rglob("*") if p.is_file()):
        rel = path.relative_to(_PAGE_DIR).as_posix()
        data = path.read_bytes()
        digest.update(rel.encode() + b"\0" + len(data).to_bytes(8, "little") + data)
        newest = max(newest, path.stat().st_mtime)
        count += 1
    return {"present": True, "build": digest.hexdigest()[:12], "files": count,
            "newest": _iso(newest) if count else None}


@lru_cache(maxsize=1)
def code_newest() -> str | None:
    """When the newest Python file of this backend's code was written: a
    copy made weeks ago shows here."""
    times = [p.stat().st_mtime for p in _APP_DIR.rglob("*.py")]
    return _iso(max(times)) if times else None


def _served(routes, prefix: str, have: set, depth: int = 0) -> None:
    for route in routes:
        inner = getattr(route, "original_router", None)
        if inner is not None and depth < 8:
            # FastAPI 0.13x keeps an included router as one entry of its
            # own; older ones (the cluster images) copy its routes in.
            context = getattr(route, "include_context", None)
            _served(inner.routes, prefix + (getattr(context, "prefix", "") or ""), have, depth + 1)
            continue
        path = getattr(route, "path", None)
        if path is None:
            continue
        methods = getattr(route, "methods", None)
        if methods is None:     # a WebSocket route
            have.add(("WS", prefix + path))
        else:
            have.update((m, prefix + path) for m in methods)


def page_routes(app) -> dict[str, bool]:
    """Which of :data:`PAGE_ROUTES` this app serves."""
    have: set = set()
    _served(app.routes, "", have)
    return {name: key in have for name, key in PAGE_ROUTES.items()}


def stereo_depth() -> str:
    """``"auto"`` (read back and checked per frame) or ``"off"``."""
    from ..render_session._stereo import depth_off_on_this_backend  # noqa: PLC0415
    return "off" if depth_off_on_this_backend() else "auto"


def check(app) -> dict:
    """What the desktop reads: no paths, dates or route maps."""
    build = page_build()
    routes = page_routes(app)
    return {
        "vr_api": VR_API,
        "page": {"present": build["present"], "build": build["build"]},
        "missing_routes": sorted(k for k, v in routes.items() if not v),
        "stereo": {"depth": stereo_depth()},
    }


def log_startup(app) -> None:
    """One line at startup with the details, so a stale overlay is visible in
    the job log."""
    page = page_build()
    missing = sorted(k for k, v in page_routes(app).items() if not v)
    logger.info(
        "[startup] VR page %s (%d files, newest %s) at /vr/; backend code newest %s in %s; "
        "VR routes %s; stereo depth %s",
        page["build"] or "MISSING", page["files"], page["newest"], code_newest(), _APP_DIR,
        "all present" if not missing else "MISSING " + ", ".join(missing), stereo_depth())


@router.get("/v1/vr/backend_check")
async def vr_backend_check(request: Request) -> dict:
    """Whether the headset page can work with this backend (issue #187)."""
    return check(request.app)
