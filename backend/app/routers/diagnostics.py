"""Diagnostics streaming endpoint.

Exposes the in-process ring buffer (``DIAGNOSTICS_BUFFER``) so the desktop
client can mirror backend log entries in its own Diagnostics window.

A simple long-poll model: the client polls every few seconds with a
``since=<last_id>`` watermark; the endpoint returns all entries with ID
strictly greater than the watermark plus the new watermark to use on the
next call.
"""

from __future__ import annotations

from fastapi import APIRouter, Depends, Query, HTTPException
from pydantic import BaseModel

from ..auth import verify_token
from ..diagnostics_buffer import DIAGNOSTICS_BUFFER

router = APIRouter(tags=["diagnostics"])
_auth = Depends(verify_token)


class DiagnosticsEvent(BaseModel):
    id: int
    ts: float                 # UNIX epoch seconds (float, ms resolution)
    level: str                # "debug" | "info" | "warning" | "error"
    category: str             # Scientific / Client / Backend / Task / WCS / Remote / Rendering / Performance
    source: str               # logger name or component
    message: str
    dataset_id: str = ""
    session_id: str = ""
    operation_tag: str = ""


class DiagnosticsEventsResponse(BaseModel):
    valid: bool
    error: str = ""
    last_id: int                            # highest ID currently in the buffer
    events: list[DiagnosticsEvent] = []     # entries with id > since (oldest first)


@router.get("/v1/diagnostics/events", response_model=DiagnosticsEventsResponse,
            tags=["diagnostics"])
async def diagnostics_events(
    token: str = _auth,
    since: int = Query(default=0, ge=0,
                       description="Watermark, return entries with id > since."),
    limit: int = Query(default=500, ge=1, le=2000,
                       description="Max entries per response."),
) -> DiagnosticsEventsResponse:
    """Return diagnostics entries newer than ``since`` (in insertion order).

    The buffer is server-wide (it mixes every user's activity), so once per-user
    tokens are configured only the operator (primary token) may read it.
    """
    from ..auth import is_admin_token
    if not is_admin_token(token):
        raise HTTPException(status_code=403, detail="Operator token required.")
    items = DIAGNOSTICS_BUFFER.snapshot_since(since=int(since), limit=int(limit))
    return DiagnosticsEventsResponse(
        valid=True,
        last_id=DIAGNOSTICS_BUFFER.latest_id(),
        events=[DiagnosticsEvent(**e) for e in items],
    )


@router.get("/v1/diagnostics/stats", tags=["diagnostics"])
async def diagnostics_stats(_: None = _auth) -> dict:
    """Lightweight stats for monitoring (entry count, capacity, last id)."""
    return DIAGNOSTICS_BUFFER.stats()
