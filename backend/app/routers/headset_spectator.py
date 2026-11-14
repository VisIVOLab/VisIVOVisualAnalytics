"""The spectator view's routes (issue #281): the desktop watches what a
headset page of its session shows.

See ``app/headset_spectator.py`` for the lease, the bounds and what is kept.

Desktop side:

* ``POST /v1/vr/headset/spectator``: start (or retune) or stop watching a
  page; the page gets a ``watch`` command through its command poll;
* ``GET /v1/vr/headset/spectator/frame``: the latest frame (a JPEG) above
  ``after``, waiting up to ``wait`` seconds; each read renews the watch;
* ``POST /v1/vr/headset/spectator/snapshot``: the latest frame saved as a PNG
  in the Workspace Exports.

Page side:

* ``POST /v1/vr/headset/spectator/frame``: a frame (``image/jpeg``), answered
  with whether the page is still watched (not watched: it stops capturing).

Authenticated as the hub (``X-Visivo-Token``, the pairing bridge adds it) in
the hub's session (``X-Visivo-Session``). The request models are read by the
desktop's contract tests: keep one ``name: type`` field per line in each.
"""
from __future__ import annotations

import asyncio
import re
import time
from contextlib import suppress
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException, Query, Request, Response
from pydantic import BaseModel, Field

from .. import headset_hub as H
from .. import headset_spectator as S
from .. import response_headers as RH
from ..auth import accepted_tokens, verify_token
from ..dependencies import make_workspace_path
from ..sessions import Session, get_session
from .headset import (
    _CLIENT_PATTERN,
    _MIN_TOKEN_SCAN,
    _check_waiters,
    _documented_body,
    _known_page,
    _read_body,
    _refuse_token,
)

router = APIRouter(tags=["vr"])
_auth = Depends(verify_token)


class SpectatorRequest(BaseModel):
    client: str | None = Field(None, pattern=_CLIENT_PATTERN)
    on: bool
    fps: int = Field(S.DEFAULT_FPS, ge=1, le=S.MAX_FPS)
    width: int = Field(S.DEFAULT_WIDTH, ge=S.MIN_WIDTH, le=S.MAX_WIDTH)


class SpectatorResponse(BaseModel):
    client: str
    on: bool
    fps: int
    width: int
    lease_s: float
    instance: str


class SpectatorFrameAck(BaseModel):
    watched: bool
    seq: int
    stored: bool
    fps: int
    width: int


class SpectatorSnapshotRequest(BaseModel):
    client: str = Field(..., pattern=_CLIENT_PATTERN)
    basename: str = Field("", max_length=120)


class SpectatorSnapshotResponse(BaseModel):
    valid: bool
    workspace_filename: str
    output_path: str
    width: int
    height: int
    seq: int


def _present_page(hub: H.HeadsetHub, client: str | None) -> H.Page:
    """``client``'s page, or the newest page present; 404 when it is not connected."""
    present = hub.present_pages()
    page = present[0] if client is None and present else next(
        (p for p in present if p.client == client), None)
    if page is None:
        raise HTTPException(404, "No headset page is connected in this session"
                                 + (f" as '{client}'." if client else "."))
    return page


@router.post("/v1/vr/headset/spectator", response_model=SpectatorResponse,
             openapi_extra=_documented_body(SpectatorRequest))
async def spectator_watch(
    request: Request,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> SpectatorResponse:
    """Start, retune or stop watching a headset page (``client``'s, or the
    newest page's when omitted): the page gets a ``watch`` command (on, with
    ``fps`` and ``width``, or off) through its command poll. Watching lasts
    ``lease_s`` seconds after the start and after every frame read. 404 when
    that page is not connected."""
    body = await _read_body(request, SpectatorRequest)
    _refuse_token(body.client)
    hub = H.hub_for(session)
    page = _present_page(hub, body.client)
    feed = S.start(hub, page, body.fps, body.width) if body.on else S.stop(hub, page)
    return SpectatorResponse(client=page.client, on=feed.watching, fps=feed.fps, width=feed.width,
                             lease_s=S.LEASE_S, instance=H.INSTANCE)


@router.get("/v1/vr/headset/spectator/frame",
            responses={200: {"content": {"image/jpeg": {}},
                             "description": "The latest frame; its numbers in X-Visivo-Frame-* headers."},
                       204: {"description": "No newer frame within `wait`."}})
async def spectator_frame_read(
    request: Request,
    client: str = Query(..., pattern=_CLIENT_PATTERN),
    after: int = Query(0, ge=0),
    wait: float = Query(0.0, ge=0.0, le=S.MAX_WAIT_S, allow_inf_nan=False),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """The latest frame of ``client`` above ``after`` (a JPEG), waiting up to
    ``wait`` seconds (at most 5) for one; 204 when there is none in time.
    Headers: ``X-Visivo-Frame-Seq`` (the next ``after``), ``-Width``,
    ``-Height``, ``-Eye`` (left, right or mono), ``-Age-Ms`` (since the backend
    got it), ``-Cost-Ms`` (the headset's capture cost per frame, when it said),
    ``X-Visivo-Instance``. Renews the watch. 404 for an unknown page, 409 when
    it is not watched (start watching first)."""
    hub = H.hub_for(session)
    page = _known_page(hub, client)
    feed = S.feed_of(page)
    if not feed.watching:
        raise HTTPException(409, "This headset page is not watched: start with POST /v1/vr/headset/spectator.")
    after = S.cursor(feed, after)
    if wait > 0:
        _check_waiters(hub.frame_waiters)
    hub.frame_waiters += 1
    try:
        frame = await S.wait_frame(hub, page, after, wait, disconnected=request.is_disconnected)
    finally:
        hub.frame_waiters -= 1
    headers = {RH.X_INSTANCE: H.INSTANCE, "Cache-Control": "no-store"}
    if frame is None:
        return Response(status_code=204, headers=headers)
    headers.update({
        RH.X_FRAME_SEQ: str(frame.seq),
        RH.X_FRAME_WIDTH: str(frame.width),
        RH.X_FRAME_HEIGHT: str(frame.height),
        RH.X_FRAME_EYE: frame.eye,
        RH.X_FRAME_AGE_MS: str(max(0, round((time.monotonic() - frame.received) * 1000))),
    })
    if frame.cost_ms is not None:
        headers[RH.X_FRAME_COST_MS] = f"{frame.cost_ms:.3f}"
    return Response(content=frame.jpeg, media_type="image/jpeg", headers=headers)


async def _read_frame_body(request: Request) -> bytes:
    """The raw body, at most MAX_FRAME_BYTES (413 beyond, declared or read)."""
    declared = request.headers.get("content-length")
    if declared is not None and declared.isdigit() and int(declared) > S.MAX_FRAME_BYTES:
        raise HTTPException(413, f"A frame is at most {S.MAX_FRAME_BYTES} bytes.")
    buf = bytearray()
    async for chunk in request.stream():
        buf += chunk
        if len(buf) > S.MAX_FRAME_BYTES:
            raise HTTPException(413, f"A frame is at most {S.MAX_FRAME_BYTES} bytes.")
    return bytes(buf)


@router.post("/v1/vr/headset/spectator/frame", response_model=SpectatorFrameAck,
             openapi_extra={"requestBody": {"required": True, "content": {
                 "image/jpeg": {"schema": {"type": "string", "format": "binary"}}}}})
async def spectator_frame_post(
    request: Request,
    client: str = Query(..., pattern=_CLIENT_PATTERN),
    eye: str = Query("left", pattern=r"^(left|right|mono)$"),
    cost_ms: float | None = Query(None, ge=0.0, le=1000.0, allow_inf_nan=False),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> SpectatorFrameAck:
    """The page's frame: a JPEG (``Content-Type: image/jpeg``) of at most
    512 KiB and 1920 pixels a side; ``eye`` it shows (left, right, mono);
    ``cost_ms`` what capturing cost the headset per frame. Answered with
    ``watched`` (false: nobody watches, the frame is not kept, stop
    capturing), ``stored`` (false: it came sooner than 30 frames a second
    allow, and was dropped) and the ``fps`` and ``width`` asked for. 404 for
    a client that did not say hello, 413 too large, 415 not a JPEG."""
    hub = H.hub_for(session)
    page = _known_page(hub, client)
    ctype = request.headers.get("content-type", "").split(";")[0].strip().lower()
    if ctype != "image/jpeg":
        raise HTTPException(415, "A frame is image/jpeg.")
    data = await _read_frame_body(request)
    size = S.jpeg_size(data)
    if size is None:
        raise HTTPException(415, "The body is not a JPEG with a frame header.")
    w, h = size
    if w > S.MAX_FRAME_SIDE or h > S.MAX_FRAME_SIDE:
        raise HTTPException(413, f"A frame is at most {S.MAX_FRAME_SIDE} pixels a side; this one is {w}×{h}.")
    tokens = [t.encode() for t in accepted_tokens() if t and len(t) >= _MIN_TOKEN_SCAN]
    if any(t in data for t in tokens):
        raise HTTPException(422, "The frame carries the backend token; it is never relayed.")
    page.seen()
    feed = S.feed_of(page)
    if not feed.live():
        return SpectatorFrameAck(watched=False, seq=feed.seq, stored=False, fps=feed.fps, width=feed.width)
    frame = S.add_frame(feed, data, w, h, eye, cost_ms)
    return SpectatorFrameAck(watched=True, seq=feed.seq, stored=frame is not None,
                             fps=feed.fps, width=feed.width)


def _snapshot_name(label: str, basename: str) -> str:
    """The PNG's file name: the one asked for, else headset_<label>_<time>.png."""
    name = basename.strip()
    if not name:
        tag = re.sub(r"[^A-Za-z0-9]+", "_", label).strip("_").lower() or "view"
        name = f"headset_{tag}_{time.strftime('%Y%m%d-%H%M%S')}"
    if not name.lower().endswith(".png"):
        name += ".png"
    return name


def _write_png(jpeg: bytes, path: str) -> tuple[int, int]:
    import io  # noqa: PLC0415

    from PIL import Image  # noqa: PLC0415

    with Image.open(io.BytesIO(jpeg)) as im:
        rgb = im.convert("RGB")
        rgb.save(path, format="PNG")
        return rgb.size


@router.post("/v1/vr/headset/spectator/snapshot", response_model=SpectatorSnapshotResponse,
             openapi_extra=_documented_body(SpectatorSnapshotRequest))
async def spectator_snapshot(
    request: Request,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> SpectatorSnapshotResponse:
    """The latest frame of ``client`` saved as a PNG in the Workspace Exports
    (``basename``, else ``headset_<label>_<time>.png``; a name taken gets a
    suffix). 404 for an unknown page, 409 when it has no frame (not watched,
    or none came yet)."""
    body = await _read_body(request, SpectatorSnapshotRequest)
    _refuse_token(body.client, body.basename)
    hub = H.hub_for(session)
    page = _known_page(hub, body.client)
    feed = S.feed_of(page)
    frame = feed.frame if feed.watching else None
    if frame is None:
        raise HTTPException(409, "No frame of this headset page to save: watch it first.")
    out = make_workspace_path(_snapshot_name(page.label, body.basename), reserve=True)
    try:
        width, height = await asyncio.to_thread(_write_png, frame.jpeg, str(out))
    except Exception as exc:
        with suppress(OSError):
            Path(out).unlink()
        raise HTTPException(500, f"The snapshot was not saved: {exc}") from exc
    return SpectatorSnapshotResponse(valid=True, workspace_filename=Path(out).name, output_path=str(out),
                                     width=width, height=height, seq=frame.seq)
