"""The backend as an SRC node: liveness, status and metrics for whoever routes work to it.

These routes are public by default, so a catalogue (SKAVA) or a monitoring
system can probe the node without holding a user's token; they reveal capacity
figures, never users, datasets or paths. Set ``VISIVO_NODE_STATUS_PUBLIC=0`` to
require the usual ``X-Visivo-Token`` (``/v1/ping`` stays open: it says nothing).
"""
from __future__ import annotations

import os

from fastapi import APIRouter, Depends, Request
from fastapi.responses import PlainTextResponse

from .. import node_status
from ..auth import verify_token
from ..compute import finders
from ..dependencies import get_exports_dir, pool_health
from ..render_manager import get_render_manager
from ..tasks import TASKS

router = APIRouter(tags=["node"])


async def _status_access(request: Request) -> None:
    if os.environ.get("VISIVO_NODE_STATUS_PUBLIC", "1").strip().lower() in ("0", "false", "no"):
        await verify_token(request.headers.get("X-Visivo-Token"))


async def _collect(request: Request) -> dict:
    return await collect_status(request.app.version)


async def collect_status(app_version: str) -> dict:
    """The node status document; also what the node pushes to its catalogue."""
    tasks = TASKS.snapshot(recent_limit=0)
    render = await get_render_manager().snapshot()
    return node_status.collect(
        workers=pool_health()["workers"],
        # The registry's running count includes the pending ones.
        running_tasks=max(0, tasks["running_count"] - tasks["pending_count"]),
        queued_tasks=tasks["pending_count"],
        render_active=render["active_sessions"],
        render_capacity=render["capacity"],
        workspace=get_exports_dir(),
        finders=finders.available(),
        finder_memory=finders.memory_needs(),
        version=os.environ.get("VISIVO_VERSION") or app_version,
    )


@router.get("/v1/ping")
async def ping() -> dict:
    """Liveness, for round-trip probes: constant, cheap, unauthenticated."""
    return {"ok": True}


@router.get("/v1/node/status", dependencies=[Depends(_status_access)])
async def node_status_route(request: Request) -> dict:
    """What the node can take on now: CPU and memory under its limits, tasks, renders, finders."""
    return await _collect(request)


@router.get("/v1/metrics", dependencies=[Depends(_status_access)],
            response_class=PlainTextResponse)
async def metrics(request: Request) -> PlainTextResponse:
    """The node status in the Prometheus text format."""
    return PlainTextResponse(node_status.prometheus(await _collect(request)),
                             media_type="text/plain; version=0.0.4")
