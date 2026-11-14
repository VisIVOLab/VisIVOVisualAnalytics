"""The headset hub's routes (issue #236): which headset pages are open in the
session, what the desktop asks of them, and what they report back.

See ``app/headset_hub.py`` for what is kept, its order and its bounds.

Page side (the headset):

* ``POST /v1/vr/headset/hello``: the page announces itself, at start and after
  any reconnect (also when a route below answers 404 for its client: the
  backend restarted or forgot it);
* ``POST /v1/vr/headset/state``: what it shows, on every change;
* ``GET /v1/vr/headset/commands``: its long poll for the desktop's commands;
  ``after`` acknowledges the commands up to it;
* ``POST /v1/vr/activity``: what was done in the headset;
* ``DELETE /v1/vr/headset``: it closes (best effort).

Desktop side:

* ``GET /v1/vr/headset``: the pages seen in the last 45 s, newest first;
* ``POST /v1/vr/headset/command``: a command for a page (404: none connected);
* ``GET /v1/vr/activity``: its long poll for the pages' activity.

The token is the usual ``X-Visivo-Token`` header (the pairing bridge adds it),
the session the usual ``X-Visivo-Session`` one: only the clients of a session
see each other. No body may carry the token: a request that does is refused,
so nothing stored here, or relayed to the other side, can leak it.

The request models are read by the desktop's contract tests: keep one
``name: type`` field per line in each.
"""
from __future__ import annotations

import time
from typing import Annotated, Any, Literal, TypeVar

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from fastapi.exceptions import RequestValidationError
from pydantic import AfterValidator, BaseModel, Field, ValidationError

from .. import headset_hub as H
from ..auth import accepted_tokens, verify_token
from ..dependencies import get_exports_dir, resolve_workspace_path
from ..sessions import Session, get_session

router = APIRouter(tags=["vr"])
_auth = Depends(verify_token)

#: What a product event's ``payload.product`` may name.
PRODUCT_KINDS = ("moment", "pv", "spectrum", "isolate", "remove", "catalogue", "flow")
#: Tokens shorter than this are not searched for in bodies (a hand-set test
#: token of a letter or two would refuse every request).
_MIN_TOKEN_SCAN = 8

_CLIENT_PATTERN = r"^[A-Za-z0-9_.:-]{1,64}$"
#: A request body of the hub's POST routes, at most (a 16 KiB payload with
#: its fields and escapes). Counted while it is read, before it is parsed:
#: FastAPI would buffer and parse a body of any size.
MAX_BODY_BYTES = 64 * 1024


def _box(value: list[list[int]] | None) -> list[list[int]] | None:
    if value is None:
        return None
    if len(value) != 3 or any(len(r) != 2 or r[0] < 0 or r[1] < r[0] for r in value):
        raise ValueError("box is [[x0, x1], [y0, y1], [z0, z1]]: inclusive voxel ranges, 0 <= lo <= hi")
    return value


#: Inclusive voxel ranges of the full cube, [[x0, x1], [y0, y1], [z0, z1]].
VoxelBox = Annotated[list[list[int]], AfterValidator(_box)]


class HeadsetCube(BaseModel):
    dataset_id: str = Field(..., min_length=1, max_length=128)
    path: str = Field(..., min_length=1, max_length=4096)
    kind: str = Field("cube", min_length=1, max_length=32)
    box: VoxelBox | None = None


class HeadsetHelloRequest(BaseModel):
    client: str = Field(..., pattern=_CLIENT_PATTERN)
    transport: Literal["adb", "pair", "lan"]
    label: str = Field("", max_length=80)


class HeadsetHelloResponse(BaseModel):
    client: str
    server_time: float
    instance: str


class HeadsetStateRequest(BaseModel):
    client: str = Field(..., pattern=_CLIENT_PATTERN)
    in_vr: bool
    cube: HeadsetCube | None = None
    staged_cube: HeadsetCube | None = None


class HeadsetInfo(BaseModel):
    client: str
    transport: str
    label: str
    since: float
    last_seen: float
    in_vr: bool
    cube: HeadsetCube | None = None
    staged_cube: HeadsetCube | None = None


class HeadsetListResponse(BaseModel):
    headsets: list[HeadsetInfo]
    server_time: float
    instance: str


class HeadsetRemoveResponse(BaseModel):
    removed: bool


class HeadsetCommandRequest(BaseModel):
    client: str | None = Field(None, pattern=_CLIENT_PATTERN)
    type: Literal["show"]
    dataset_id: str = Field(..., min_length=1, max_length=128)
    path: str = Field(..., min_length=1, max_length=4096)
    kind: str | None = Field(None, min_length=1, max_length=32)
    box: VoxelBox | None = None


class HeadsetCommandResponse(BaseModel):
    seq: int
    client: str


class HeadsetCommandsResponse(BaseModel):
    commands: list[dict[str, Any]]
    next: int
    instance: str


class ActivityRequest(BaseModel):
    client: str = Field(..., pattern=_CLIENT_PATTERN)
    kind: Literal["look", "clip", "product", "cube"]
    cube_path: str | None = Field(None, max_length=4096)
    summary: str = Field(..., max_length=300)
    payload: dict[str, Any] = Field(default_factory=dict)


class ActivityResponse(BaseModel):
    seq: int


class ActivityEvent(BaseModel):
    seq: int
    client: str
    time: float
    kind: str
    cube_path: str | None = None
    summary: str
    payload: dict[str, Any]


class ActivityListResponse(BaseModel):
    events: list[ActivityEvent]
    next: int
    lost: bool
    instance: str


# ── Helpers ─────────────────────────────────────────────────────────────────

M = TypeVar("M", bound=BaseModel)


async def _read_body(request: Request, model: type[M]) -> M:
    """The request's JSON body as ``model``: 413 once it is over
    MAX_BODY_BYTES (declared or read; over HTTP and through the pairing
    bridge alike), 422 when it does not validate."""
    declared = request.headers.get("content-length")
    if declared is not None and declared.isdigit() and int(declared) > MAX_BODY_BYTES:
        raise HTTPException(413, f"At most {MAX_BODY_BYTES} bytes per request.")
    buf = bytearray()
    async for chunk in request.stream():
        buf += chunk
        if len(buf) > MAX_BODY_BYTES:
            raise HTTPException(413, f"At most {MAX_BODY_BYTES} bytes per request.")
    try:
        return model.model_validate_json(bytes(buf))
    except ValidationError as exc:
        # Without the rejected input: it may not encode (bytes, NaN), and is not echoed.
        raise RequestValidationError(
            exc.errors(include_url=False, include_context=False, include_input=False)) from None


def _inline_refs(node: Any, defs: dict[str, Any]) -> Any:
    if isinstance(node, dict):
        ref = node.get("$ref")
        if isinstance(ref, str) and ref.startswith("#/$defs/"):
            return _inline_refs(defs[ref[len("#/$defs/"):]], defs)
        return {k: _inline_refs(v, defs) for k, v in node.items()}
    if isinstance(node, list):
        return [_inline_refs(v, defs) for v in node]
    return node


def _documented_body(model: type[BaseModel]) -> dict[str, Any]:
    """The OpenAPI request body of a route that reads its body itself."""
    schema = model.model_json_schema()
    defs = schema.pop("$defs", {})
    return {"requestBody": {"required": True, "content": {
        "application/json": {"schema": _inline_refs(schema, defs)}}}}


def _refuse_token(*texts: str | None) -> None:
    """422 when any text carries an accepted token."""
    tokens = [t for t in accepted_tokens() if t and len(t) >= _MIN_TOKEN_SCAN]
    for text in texts:
        if text and any(t in text for t in tokens):
            raise HTTPException(422, "The request carries the backend token; it is never stored "
                                     "nor relayed to another client.")


def _known_page(hub: H.HeadsetHub, client: str) -> H.Page:
    p = hub.page(client)
    if p is None:
        raise HTTPException(404, "Unknown headset client in this session: say hello first "
                                 "(POST /v1/vr/headset/hello).")
    return p


def _cube_dict(cube: HeadsetCube | None) -> dict[str, Any] | None:
    return cube.model_dump() if cube is not None else None


def _product_payload(payload: dict[str, Any]) -> dict[str, Any]:
    """A product event's payload, checked: ``product`` one of PRODUCT_KINDS;
    a ``file`` the backend wrote is a Workspace Exports filename that exists,
    and gets its ``output_path`` (what the desktop opens) added here."""
    product = payload.get("product")
    if product not in PRODUCT_KINDS:
        raise HTTPException(422, f"payload.product must be one of {', '.join(PRODUCT_KINDS)}.")
    if "output_path" in payload:
        raise HTTPException(422, "payload.output_path is the backend's to set: name the file in payload.file.")
    file = payload.get("file")
    if file is None:
        return payload
    if not isinstance(file, str):
        raise HTTPException(422, "payload.file is a Workspace Exports filename.")
    try:
        path = resolve_workspace_path(file)
    except ValueError as exc:
        raise HTTPException(422, f"payload.file: {exc}") from exc
    try:
        # Followed through any symlink, still a file of the exports directory.
        inside = path.is_file() and path.resolve().parent == get_exports_dir().resolve()
    except OSError:
        inside = False
    if not inside:
        raise HTTPException(422, f"payload.file '{file}' is not in the Workspace Exports.")
    return {**payload, "output_path": str(path)}


def _check_waiters(count: int) -> None:
    if count >= H.MAX_WAITERS_PER_SESSION:
        raise HTTPException(429, "Too many clients are waiting on this session's headset hub.")


# ── Presence ────────────────────────────────────────────────────────────────


@router.post("/v1/vr/headset/hello", response_model=HeadsetHelloResponse,
             openapi_extra=_documented_body(HeadsetHelloRequest))
async def headset_hello(
    request: Request,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> HeadsetHelloResponse:
    """A headset page announces itself (at start, after any reconnect, and
    whenever a hub route answers 404 for its client). Its queued commands are
    kept across a reconnect under the same ``client``."""
    body = await _read_body(request, HeadsetHelloRequest)
    _refuse_token(body.client, body.label)
    H.hub_for(session).hello(body.client, body.transport, body.label)
    return HeadsetHelloResponse(client=body.client, server_time=time.time(), instance=H.INSTANCE)


@router.post("/v1/vr/headset/state", response_model=HeadsetHelloResponse,
             openapi_extra=_documented_body(HeadsetStateRequest))
async def headset_state(
    request: Request,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> HeadsetHelloResponse:
    """What the page shows: whether it is in VR, its cube (the box it loaded,
    in voxels of the full cube) and the cube staged to show next."""
    body = await _read_body(request, HeadsetStateRequest)
    _refuse_token(body.client)
    for c in (body.cube, body.staged_cube):
        if c is not None:
            _refuse_token(c.dataset_id, c.path, c.kind)
    hub = H.hub_for(session)
    p = _known_page(hub, body.client)
    p.in_vr, p.cube, p.staged_cube = body.in_vr, _cube_dict(body.cube), _cube_dict(body.staged_cube)
    p.seen()
    hub.notify()
    return HeadsetHelloResponse(client=body.client, server_time=time.time(), instance=H.INSTANCE)


@router.get("/v1/vr/headset", response_model=HeadsetListResponse)
async def headset_list(
    _: None = _auth,
    session: Session = Depends(get_session),
) -> HeadsetListResponse:
    """The headset pages of this session seen in the last 45 s, the newest
    (latest hello) first. ``since`` and ``last_seen`` are Unix seconds of
    the backend's clock, as ``server_time``."""
    pages = H.hub_for(session).present_pages()
    return HeadsetListResponse(headsets=[HeadsetInfo(**p.info()) for p in pages],
                               server_time=time.time(), instance=H.INSTANCE)


@router.delete("/v1/vr/headset", response_model=HeadsetRemoveResponse)
async def headset_remove(
    client: str = Query(..., pattern=_CLIENT_PATTERN),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> HeadsetRemoveResponse:
    """The page closes (best effort): it is no longer listed, its queued
    commands go, and its waiting poll ends."""
    return HeadsetRemoveResponse(removed=H.hub_for(session).remove(client))


# ── Commands ────────────────────────────────────────────────────────────────


@router.post("/v1/vr/headset/command", response_model=HeadsetCommandResponse,
             openapi_extra=_documented_body(HeadsetCommandRequest))
async def headset_command(
    request: Request,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> HeadsetCommandResponse:
    """A command for a headset page: ``client``'s, or the newest page's when
    omitted. 404 when that page is not connected (the desktop then opens one
    the usual way). ``show``: show this cube (``dataset_id`` in this session;
    the page opens ``path`` again when it does not hold that id), optionally
    its ``box``; it replaces an older ``show`` the page has not acknowledged."""
    body = await _read_body(request, HeadsetCommandRequest)
    _refuse_token(body.client, body.dataset_id, body.path, body.kind)
    cmd_body: dict[str, Any] = {"dataset_id": body.dataset_id, "path": body.path}
    if body.kind is not None:
        cmd_body["kind"] = body.kind
    if body.box is not None:
        cmd_body["box"] = body.box
    queued = H.hub_for(session).queue_command(body.client, body.type, cmd_body)
    if queued is None:
        raise HTTPException(404, "No headset page is connected in this session"
                                 + (f" as '{body.client}'." if body.client else "."))
    cmd, target = queued
    return HeadsetCommandResponse(seq=cmd.seq, client=target)


@router.get("/v1/vr/headset/commands", response_model=HeadsetCommandsResponse)
async def headset_commands(
    request: Request,
    client: str = Query(..., pattern=_CLIENT_PATTERN),
    after: int = Query(0, ge=0),
    wait: float = Query(0.0, ge=0.0, le=H.MAX_WAIT_SECONDS, allow_inf_nan=False),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> HeadsetCommandsResponse:
    """The page's long poll: its commands above ``after`` (which acknowledges
    every command up to it), waiting up to ``wait`` seconds (at most 25) for
    one. ``next`` is the ``after`` of the next poll. A newer poll of the same
    page ends this one (empty); so does the backend's shutdown. Polling keeps
    the page listed."""
    hub = H.hub_for(session)
    page = _known_page(hub, client)
    after = H.cursor(after)
    if wait > 0:
        _check_waiters(hub.command_waiters)
    hub.command_waiters += 1
    try:
        cmds = await hub.wait_commands(page, after, wait, disconnected=request.is_disconnected)
    finally:
        hub.command_waiters -= 1
    nxt = max([after] + [c.seq for c in cmds])
    return HeadsetCommandsResponse(commands=[c.info() for c in cmds], next=nxt, instance=H.INSTANCE)


# ── Activity ────────────────────────────────────────────────────────────────


@router.post("/v1/vr/activity", response_model=ActivityResponse,
             openapi_extra=_documented_body(ActivityRequest))
async def activity_post(
    request: Request,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ActivityResponse:
    """What the page did, for the desktop:

    * ``look``: colormap, threshold, colour max, opacity, gamma, render mode,
      stretch; the latest per cube replaces the previous;
    * ``clip``: ``payload.box`` the clip box in voxels of the full cube, or
      null; the latest per cube replaces the previous;
    * ``product``: ``payload.product`` (moment, pv, spectrum, isolate, remove,
      catalogue, flow), and what the desktop needs to open it: ``file`` (a
      Workspace Exports filename; its ``output_path`` is added) or
      ``dataset_id``, the ``request`` under the API's names, the ``recipe``;
    * ``cube``: the page switched cube (``payload.path``, ``payload.binning``).

    The payload is at most 16 KiB of JSON, of finite numbers."""
    body = await _read_body(request, ActivityRequest)
    hub = H.hub_for(session)
    _known_page(hub, body.client).seen()
    payload = _product_payload(body.payload) if body.kind == "product" else body.payload
    try:
        text = H.compact_json(payload)
    except (ValueError, RecursionError) as exc:
        raise HTTPException(422, f"payload is not JSON of finite numbers: {exc}") from exc
    nbytes = len(text.encode())
    if nbytes > H.MAX_PAYLOAD_BYTES:
        raise HTTPException(413, f"payload is {nbytes} bytes; at most {H.MAX_PAYLOAD_BYTES}.")
    _refuse_token(body.client, text, body.summary, body.cube_path)
    ev = hub.add_event(body.client, body.kind, body.cube_path, body.summary, payload, nbytes)
    return ActivityResponse(seq=ev.seq)


@router.get("/v1/vr/activity", response_model=ActivityListResponse)
async def activity_poll(
    request: Request,
    after: int = Query(0, ge=0),
    wait: float = Query(0.0, ge=0.0, le=H.MAX_WAIT_SECONDS, allow_inf_nan=False),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ActivityListResponse:
    """The desktop's long poll: the events above ``after``, oldest first,
    waiting up to ``wait`` seconds (at most 25) for one. ``next`` is the
    ``after`` of the next poll; ``lost`` says events above ``after`` (not 0:
    a first poll has missed nothing) were dropped by the ring's bounds (256
    events, 1 MiB of payloads) before they were read. A ``look`` or ``clip`` replaced by a later one of the same cube
    is not lost: only the latest is kept. The backend's shutdown ends the
    poll (empty)."""
    hub = H.hub_for(session)
    after = H.cursor(after)
    if wait > 0:
        _check_waiters(hub.activity_waiters)
    hub.activity_waiters += 1
    try:
        events = await hub.wait_events(after, wait, disconnected=request.is_disconnected)
    finally:
        hub.activity_waiters -= 1
    nxt = max([after] + [e.seq for e in events])
    return ActivityListResponse(events=[ActivityEvent(**e.info()) for e in events], next=nxt,
                                lost=0 < after < hub.lost_up_to, instance=H.INSTANCE)
