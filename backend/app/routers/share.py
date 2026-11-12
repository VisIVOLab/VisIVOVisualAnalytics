"""Selections shared between the clients of a session (issue #188).

The VR headset's page and the desktop's cube viewer, in the same backend
session, publish the selection they make here and wait for the other's with a
long poll (see ``app/selection_hub.py`` for what is kept and its bounds):

* ``POST /v1/share/selection``: publish a Kinematic Lasso's mask (the body, as
  ``kinematic_select_bin`` returns it: little-endian float32 0/1 over the box
  at the origin) or a box (``kind=box``, no body). The backend measures the
  selection on the full cube (``mask_stats_bin``'s statistics) and keeps it,
  replacing the cube's previous one;
* ``GET /v1/share/selection``: the latest selection's description, its
  statistics and the creator's record, once its ``seq`` is above ``after``,
  waiting up to ``wait`` seconds (204 when none came);
* ``GET /v1/share/selection/mask``: its mask, with the select route's headers;
* ``DELETE /v1/share/selection``: forget it.

The token is the usual ``X-Visivo-Token`` header, the session the usual
``X-Visivo-Session`` one: only the clients of the session see its selections.
"""
from __future__ import annotations

import asyncio
import json
import math
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Query, Request, Response

from ..auth import verify_token
from ..dependencies import _require_dataset, _run_heavy_with_limit
from ..selection_hub import (
    INSTANCE, MAX_META_CHARS, MAX_SHARE_VOXELS, MAX_WAIT_SECONDS, MAX_WAITERS_PER_SESSION,
    PUBLISH_BUDGET, SharedSelection, hub_for, mask_bytes, next_seq, pack_mask_async,
)
from ..sessions import Session, get_session

router = APIRouter()
_auth = Depends(verify_token)


def _cube_entry(session: Session, dataset_id: str) -> dict[str, Any]:
    entry = _require_dataset(session, dataset_id)
    if entry.get("kind") != "cube":
        raise HTTPException(422, "Shared selections are of spectral cubes.")
    return entry


def _key(entry: dict[str, Any]) -> str:
    # The cube's file: the headset may hold the desktop's cube under another
    # dataset id (it opened the path again, #172), and still the same cube.
    return str(entry.get("path", ""))


def _hub(session: Session):
    """The session's hub, without the selections of cubes it no longer holds
    open (the anonymous session's datasets are flushed, not the session)."""
    hub = hub_for(session)
    hub.prune({_key(e) for e in session.datasets.values() if e.get("kind") == "cube"})
    return hub


def _finite_float(text: str) -> float:
    """``parse_float`` for the meta record: a number too large for a float
    (``1e999``) parses to inf without ever reaching ``parse_constant``, and an
    inf kept in the record made every later read of the selection fail
    (``allow_nan=False``)."""
    value = float(text)
    if not math.isfinite(value):
        raise ValueError(f"{text} is out of range of a JSON number")
    return value


def _no_constants(name: str):
    raise ValueError(f"{name} is not a number JSON allows")


def parse_meta(meta: str) -> dict[str, Any]:
    """The creator's record: a JSON object of finite numbers, else 422."""
    try:
        meta_obj = json.loads(meta, parse_constant=_no_constants, parse_float=_finite_float)
    except (ValueError, RecursionError) as exc:
        raise HTTPException(422, f"meta is not JSON: {exc}") from exc
    if not isinstance(meta_obj, dict):
        raise HTTPException(422, "meta must be a JSON object")
    return meta_obj


def _release_after(work: asyncio.Future | None, permit: int) -> None:
    """Give ``permit`` back to the publish budget now, or once ``work`` (the
    packing thread, which holds the body) is done, whatever became of the
    request that started it."""
    if work is None or work.done():
        if work is not None and not work.cancelled():
            work.exception()            # retrieved: the request saw it, or is gone
        PUBLISH_BUDGET.release(permit)
        return

    def done(fut: asyncio.Future) -> None:
        if not fut.cancelled():
            fut.exception()
        PUBLISH_BUDGET.release(permit)
    work.add_done_callback(done)


def _full_shape(entry: dict[str, Any]) -> tuple[int, int, int]:
    return int(entry.get("width", 0)), int(entry.get("height", 0)), int(entry.get("depth", 0))


@router.post("/v1/share/selection", tags=["share"])
async def share_publish(
    request: Request,
    dataset_id: str = Query(...),
    kind: str = Query("lasso", pattern="^(lasso|box)$"),
    source: str = Query(..., pattern="^(headset|desktop)$"),
    client: str = Query(..., min_length=1, max_length=64),
    origin_x: int = Query(..., ge=0), origin_y: int = Query(..., ge=0),
    origin_z: int = Query(..., ge=0),
    width: int = Query(..., ge=1), height: int = Query(..., ge=1), depth: int = Query(..., ge=1),
    # The creator's record (JSON object): the grow request under the API's
    # names, the Provenance labels it shows; the receiver attaches it.
    meta: str = Query("{}", max_length=MAX_META_CHARS),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> dict:
    from ..compute import worker_cube_packed_mask_stats  # noqa: PLC0415
    # The order of selections is the order their publishes ARRIVE: the seq is
    # taken now, before the body is read and the selection measured, so a slow
    # earlier publish cannot replace a later one (the hub keeps the higher seq).
    seq = next_seq()
    n = int(width) * int(height) * int(depth)
    if n > MAX_SHARE_VOXELS:
        raise HTTPException(422, f"The selection's box holds {n} voxels, over the "
                                 f"{MAX_SHARE_VOXELS}-voxel limit of a shared selection.")
    meta_obj = parse_meta(meta)
    box_range = [[origin_x, origin_x + width - 1], [origin_y, origin_y + height - 1],
                 [origin_z, origin_z + depth - 1]]
    if kind == "box" and "box" in meta_obj and meta_obj["box"] != box_range:
        # The box IS its origin and dims; a record saying otherwise would be
        # replayed as another box.
        raise HTTPException(422, f"meta.box {meta_obj['box']} is not the box published {box_range}")
    entry = _cube_entry(session, dataset_id)
    full = _full_shape(entry)
    if (origin_x + width > full[0] or origin_y + height > full[1] or origin_z + depth > full[2]):
        raise HTTPException(422, "The selection's box falls outside the cube.")
    expected = 0 if kind == "box" else n * 4
    declared = request.headers.get("content-length")
    if declared is not None and declared.isdigit() and int(declared) != expected:
        raise HTTPException(422, f"mask body is {declared} bytes; expected {expected}")
    # The body's bytes are taken from the publish budget BEFORE it is read and
    # held until nothing holds the body any more (it is packed): publishes
    # waiting for a compute slot would otherwise each buffer up to 64 MiB,
    # unbounded in number.
    permit = await PUBLISH_BUDGET.acquire(expected)
    packing: asyncio.Future | None = None
    keep = -1          # what stays taken once packed: the packed bits
    try:
        chunks, got = [], 0
        async for chunk in request.stream():
            got += len(chunk)
            if got > expected:
                raise HTTPException(422, f"mask body exceeds the expected {expected} bytes")
            chunks.append(chunk)
        if got != expected:
            raise HTTPException(422, f"mask body is {got} bytes; expected {expected}")
        body = b"".join(chunks)
        del chunks
        if kind == "box":
            packed, count = None, n
        else:
            # Off the event loop, in a thread that a cancelled request cannot
            # stop: the bytes go back to the budget when the thread is done
            # with the body, not when this request is (see finally).
            packing = asyncio.ensure_future(pack_mask_async(body, n))
            del body
            packed, count = await asyncio.shield(packing)
        keep = min(permit, len(packed or b""))
    finally:
        if keep >= 0:
            PUBLISH_BUDGET.release(permit - keep)    # the body is gone, its bits stay
        else:
            _release_after(packing, permit)
    # Measured here, on the full cube, so both sides show the same numbers.
    # From the packed bits (n / 8 bytes), not the body: a measurement outlives
    # a request that times out or is cancelled (_run_heavy), holding its
    # arguments, and only its compute slot bounds it then. While it waits for
    # a slot, its bits stay counted in the budget.
    try:
        if count == 0:
            raise HTTPException(422, "The selection is empty.")
        stats = await _run_heavy_with_limit(
            session, worker_cube_packed_mask_stats, entry["path"], packed,
            origin_x, origin_y, origin_z, width, height, depth)
        stats = {"valid": True, "error": "", **stats}
    except HTTPException:
        raise
    except ValueError as exc:
        stats = {"valid": False, "error": str(exc)}
    finally:
        PUBLISH_BUDGET.release(keep)
    sel = SharedSelection(
        seq=seq, kind=kind, source=source, client=client,
        dataset_key=_key(entry), dataset_id=dataset_id,
        origin=(origin_x, origin_y, origin_z), dims=(width, height, depth),
        full_shape=full, count=count, packed=packed, stats=stats, meta=meta_obj)
    hub = _hub(session)
    refused = hub.publish(sel)
    if refused == "cleared":
        # The cube's selection was cleared after this publish arrived.
        raise HTTPException(409, "cleared: the selection was cleared after this publish arrived")
    if refused:
        # A publish that arrived later is already the cube's selection.
        latest = hub.get(sel.dataset_key)
        raise HTTPException(409, f"stale: a later selection (seq {latest.seq if latest else '?'}) "
                                 f"replaced this one while it was measured")
    return {"valid": True, "seq": sel.seq, "instance": INSTANCE, "count": count, "stats": stats}


@router.get("/v1/share/selection", tags=["share"])
async def share_latest(
    request: Request,
    dataset_id: str = Query(...),
    after: int = Query(0, ge=0),
    wait: float = Query(0.0, ge=0.0, le=MAX_WAIT_SECONDS, allow_inf_nan=False),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    entry = _cube_entry(session, dataset_id)
    hub = _hub(session)
    if wait > 0 and hub.waiters >= MAX_WAITERS_PER_SESSION:
        raise HTTPException(429, "Too many clients are waiting for this session's selections.")
    hub.waiters += 1
    try:
        sel = await hub.wait_newer(_key(entry), after, wait if wait > 0 else 0.0,
                                   disconnected=request.is_disconnected)
    finally:
        hub.waiters -= 1
    if sel is None:
        return Response(status_code=204)
    return Response(content=json.dumps(sel.info(), separators=(",", ":"), allow_nan=False),
                    media_type="application/json")


@router.get("/v1/share/selection/mask", tags=["share"])
async def share_mask(
    dataset_id: str = Query(...),
    seq: int = Query(..., ge=1),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    entry = _cube_entry(session, dataset_id)
    sel = _hub(session).get(_key(entry))
    if sel is None or sel.seq != seq:
        raise HTTPException(404, "That selection is no longer the latest (or has expired).")
    raw = await mask_bytes(sel)   # bounded, off the event loop, the latest kept
    ox, oy, oz = sel.origin
    w, h, d = sel.dims
    bbox = (sel.stats or {}).get("bbox") or [ox, ox + w - 1, oy, oy + h - 1, oz, oz + d - 1]
    return Response(content=raw, media_type="application/octet-stream", headers={
        # The select route's headers (kinematic_select_bin), so a client reads
        # a shared mask as it reads its own grow.
        "X-Visivo-Width": str(w), "X-Visivo-Height": str(h), "X-Visivo-Depth": str(d),
        "X-Visivo-Origin": f"{ox},{oy},{oz}",
        "X-Visivo-Full-Shape": ",".join(str(v) for v in sel.full_shape),
        "X-Visivo-Scalar-Type": "float32",
        "X-Visivo-Selected-Count": str(sel.count),
        "X-Visivo-Bbox": ",".join(str(int(v)) for v in bbox),
        "X-Visivo-Byte-Order": "little",
        "X-Visivo-Selection-Seq": str(sel.seq),
        "X-Visivo-Selection-Kind": sel.kind,
        "Content-Length": str(len(raw)),
    })


@router.delete("/v1/share/selection", tags=["share"])
async def share_clear(
    dataset_id: str = Query(...),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> dict:
    entry = _cube_entry(session, dataset_id)
    return {"cleared": _hub(session).clear(_key(entry))}
