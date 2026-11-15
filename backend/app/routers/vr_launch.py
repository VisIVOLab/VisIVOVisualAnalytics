"""The headset app's launch routes (issue #255). See ``app/vr_launch.py``.

* ``GET /.well-known/assetlinks.json`` (public): the Digital Asset Links
  statements of the packaged headset app, fetched by the headset's browser
  at every launch of the app;
* ``POST /v1/vr/launch`` (token and session): a one-time ticket for the
  caller's session, and the cube to show, for ONE launch of the app;
* ``POST /v1/vr/launch/exchange`` (public, the ticket is the credential):
  the page trades the ticket for the token, the session and the cube, once;
* ``GET /v1/vr/launch/{launch}`` (token and session, the launch's owner
  only): how the launch went (pending, exchanged, expired) and the headset
  hub client whose hello named it.

The request models are read by the desktop's contract tests: keep one
``name: type`` field per line in each.
"""
from __future__ import annotations

import asyncio
import logging
import time

from fastapi import APIRouter, Depends, HTTPException, Request
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel, Field, ValidationError

from .. import headset_hub as H
from .. import vr_launch as L
from ..auth import accepted_tokens, verify_token
from ..sessions import Session, get_session
from .headset import HeadsetCube
from .vr import _TokenCheckLimiter, token_check_key

router = APIRouter(tags=["vr"])
logger = logging.getLogger("visivo.vr")

#: The page the desktop opens with a ticket (under the backend's path prefix).
APP_PAGE = "/vr/?launch=immersive"


class _ExchangeLimiter(_TokenCheckLimiter):
    """Exchanges per client key and minute, as /v1/vr/info's token checks;
    the global budget counts refused tickets only. A valid exchange spends a
    256-bit ticket and tells a guesser nothing, so a few addresses sending
    junk must not hold every user's launch at 429: a request within its
    key's limit is always tried, and only a miss beyond the global budget is
    answered 429 instead of 404."""

    def allow(self, client: str, now: float | None = None) -> bool:
        now = time.monotonic() if now is None else now
        with self._lock:
            hist = [t for t in self._seen.pop(client, []) if now - t < 60.0]
            ok = len(hist) < self.per_minute
            if ok:
                hist.append(now)
            self._seen[client] = hist
            while len(self._seen) > self.max_clients:
                self._seen.popitem(last=False)
            return ok

    def _prune_all(self, now: float) -> None:
        while self._all and now - self._all[0] >= 60.0:
            self._all.popleft()

    def miss(self, client: str, now: float | None = None) -> int:
        now = time.monotonic() if now is None else now
        with self._lock:
            self._prune_all(now)
            # One past the budget is enough to say "over": the window stays
            # bounded however many addresses send junk.
            if len(self._all) <= self.global_per_minute:
                self._all.append(now)
        return super().miss(client, now)

    def misses_over_budget(self, now: float | None = None) -> bool:
        """Whether this minute's refused tickets, over every key, are beyond
        ``global_per_minute``."""
        now = time.monotonic() if now is None else now
        with self._lock:
            self._prune_all(now)
            return len(self._all) > self.global_per_minute


#: Exchanges per client key and minute, and refused ones in all (the route is public).
EXCHANGES = _ExchangeLimiter(per_minute=20, max_clients=256, global_per_minute=120)
#: An exchange body, at most (a ticket is 43 characters).
MAX_EXCHANGE_BODY = 1024
_NO_STORE = {"Cache-Control": "no-store", "Pragma": "no-cache"}


class LaunchRequest(BaseModel):
    cube: HeadsetCube | None = None


class LaunchResponse(BaseModel):
    ticket: str
    launch: str
    expires_in: float
    page: str


class LaunchExchangeRequest(BaseModel):
    ticket: str = Field(..., min_length=1, max_length=256)


class LaunchStatus(BaseModel):
    launch: str
    state: str
    expires_in: float
    exchanged_at: float | None = None
    client: str | None = None


@router.get("/.well-known/assetlinks.json", include_in_schema=False)
async def vr_assetlinks() -> Response:
    """The statements that let the packaged headset app open this origin
    (app/vr_launch.py: release, development, extra file). 404 when there is
    none, so a backend without the app says nothing."""
    # An unreadable VISIVO_VR_ASSETLINKS_FILE is logged and skipped there.
    statements = await asyncio.to_thread(L.assetlinks_statements)
    if not statements:
        raise HTTPException(status_code=404, detail="Not found")
    return JSONResponse(statements, headers={"Cache-Control": "no-cache"})


@router.post("/v1/vr/launch", response_model=LaunchResponse)
async def vr_launch(
    body: LaunchRequest | None = None,
    token: str = Depends(verify_token),
    session: Session = Depends(get_session),
) -> JSONResponse:
    """A ticket for ONE launch of the headset app in the caller's session,
    with the cube it opens (none: the app's empty room). The desktop puts the
    ticket in the launch address's fragment (``#ticket=``) and follows the
    launch with ``GET /v1/vr/launch/{launch}``. Never logged."""
    issued = L.TICKETS.issue(token, session.session_id, body.cube.record() if body and body.cube else None)
    return JSONResponse({**issued, "page": APP_PAGE}, headers=_NO_STORE)


@router.post("/v1/vr/launch/exchange", include_in_schema=False)
async def vr_launch_exchange(request: Request) -> JSONResponse:
    """The page's exchange of its ticket: ``{"ticket": "…"}`` in a JSON body
    (never the address) gives ``{token, session, launch, cube}`` once. 404 for
    a ticket unknown, used or expired (one answer for all), 429 beyond the
    rate (per address; refused tickets also in all), 415 for a body that is
    not JSON."""
    ctype = request.headers.get("content-type", "").split(";")[0].strip().lower()
    if ctype != "application/json":
        raise HTTPException(status_code=415, detail="Send the ticket as JSON.")
    key = token_check_key(request)
    if not EXCHANGES.allow(key):
        raise HTTPException(status_code=429, detail="Too many ticket exchanges: wait a minute.",
                            headers={"Retry-After": "60"})
    raw = bytearray()
    async for chunk in request.stream():
        raw += chunk
        if len(raw) > MAX_EXCHANGE_BODY:
            raise HTTPException(status_code=413, detail="The body is too large.")
    try:
        body = LaunchExchangeRequest.model_validate_json(bytes(raw) or b"{}")
    except ValidationError:
        raise HTTPException(status_code=422, detail="The body is {\"ticket\": \"…\"}.") from None
    got = L.TICKETS.exchange(body.ticket, accepted_tokens())
    if got is None:
        n = EXCHANGES.miss(key)
        if n in (5, 20, 100):
            logger.warning("[vr] %d refused launch tickets within a minute", n)
        if EXCHANGES.misses_over_budget():
            raise HTTPException(status_code=429, detail="Too many ticket exchanges: wait a minute.",
                                headers={"Retry-After": "60", **_NO_STORE})
        raise HTTPException(status_code=404, detail="Unknown, used or expired ticket: open the cube again on the computer.",
                            headers=_NO_STORE)
    logger.info("[vr] launch %s: ticket exchanged", got["launch"])
    return JSONResponse(got, headers=_NO_STORE)


@router.get("/v1/vr/launch/{launch}", response_model=LaunchStatus)
async def vr_launch_status(
    launch: str,
    token: str = Depends(verify_token),
    session: Session = Depends(get_session),
) -> JSONResponse:
    """How the launch went: ``pending`` (the ticket is waiting), ``exchanged``
    (the page started), ``expired`` (never used). ``client``: the headset hub
    page whose hello named this launch (the page is connected), else null.
    404 for an unknown launch or another caller's."""
    st = L.TICKETS.status(launch, token, session.session_id)
    if st is None:
        raise HTTPException(status_code=404, detail="Unknown launch.")
    client = H.hub_for(session).launch_client(launch)
    return JSONResponse({**st, "client": client}, headers={"Cache-Control": "no-store"})
