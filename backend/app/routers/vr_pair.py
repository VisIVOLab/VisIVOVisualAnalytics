"""Pairing codes for a headset (issue #212, contract ``visivo-pair/1``).

The desktop's *Open on Headset* asks for a code here; the user types it on
the headset's VR page, which then reaches this backend over WebRTC through
the pairing service (``app/vr_pair_host.py``) instead of plain HTTP. The
desktop lists pending codes and connected headsets, and can revoke a code or
disconnect a headset.

409 with a ``reason`` when pairing cannot be used: ``not-configured``
(``VISIVO_VR_PAIR_URL`` unset, or this backend has no WebRTC support) or
``unreachable`` (the pairing service does not answer).
"""
from __future__ import annotations

import re
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Request
from fastapi.responses import JSONResponse
from pydantic import BaseModel, ConfigDict, Field, model_validator

from ..auth import multi_user_mode, verify_token
from .. import vr_pair_host

router = APIRouter(tags=["vr"])

#: Extra page parameters a code may carry (the ``#…`` fragment's keys beyond
#: session, dataset and path: ``kind``, ``box`` today, others later).
MAX_EXTRA_PARAMS = 16
MAX_PARAM_CHARS = 4096
_PARAM_NAME = re.compile(r"^[a-z][a-z0-9_]{0,31}$")


class PairRequest(BaseModel):
    """What the code opens: the page's fragment parameters, never a token."""

    model_config = ConfigDict(extra="allow")

    session: str = Field(..., max_length=256)
    dataset: str = Field("", max_length=256)
    path: str = Field("", max_length=MAX_PARAM_CHARS)

    @model_validator(mode="after")
    def _extras(self) -> "PairRequest":
        extra = self.model_extra or {}
        if "token" in extra:
            raise ValueError("a pairing never carries the token")
        if len(extra) > MAX_EXTRA_PARAMS:
            raise ValueError(f"at most {MAX_EXTRA_PARAMS} extra parameters")
        for name, value in extra.items():
            if not _PARAM_NAME.match(name):
                raise ValueError(f"bad parameter name {name!r}")
            if value is not None and not isinstance(value, (str, int, float, bool)):
                raise ValueError(f"{name}: a parameter is a string, a number or a boolean")
            if isinstance(value, str) and len(value) > MAX_PARAM_CHARS:
                raise ValueError(f"{name}: too long")
        return self

    def params(self) -> dict[str, Any]:
        out: dict[str, Any] = {"session": self.session, "dataset": self.dataset, "path": self.path}
        out.update({k: v for k, v in (self.model_extra or {}).items() if v is not None})
        return out


def _unavailable(exc: vr_pair_host.PairingUnavailable) -> JSONResponse:
    return JSONResponse(status_code=409, content={"detail": exc.detail, "reason": exc.reason})


def _owner(token: str) -> str | None:
    """Whose codes and headsets a caller sees: with per-user tokens, its own;
    on a single-user backend, all."""
    return token if multi_user_mode() else None


@router.post("/v1/vr/pair")
async def create_pairing(body: PairRequest, request: Request, token: str = Depends(verify_token)):
    """A pairing code for the headset: ``{code, expires_at, page_url}``,
    ``expires_at`` in Unix seconds, ``page_url`` the public loader with
    ``#pair=<code>`` (null when ``VISIVO_VR_PAGE_URL`` is unset)."""
    try:
        host = vr_pair_host.get_host()
        return await host.create(token, body.params(), request.app)
    except vr_pair_host.PairingUnavailable as exc:
        return _unavailable(exc)


@router.get("/v1/vr/pair")
async def list_pairings(token: str = Depends(verify_token)) -> dict:
    """``{enabled, pending:[{code, expires_at}], connected:[{id, since,
    dataset, transport}]}``; ``transport`` is ``webrtc-direct`` or
    ``webrtc-relay`` (through TURN), ``since`` in Unix seconds."""
    try:
        vr_pair_host.get_host()
        enabled = True
    except vr_pair_host.PairingUnavailable:
        enabled = False
    host = vr_pair_host.existing_host()
    listed = host.listing(_owner(token)) if host is not None else {"pending": [], "connected": []}
    return {"enabled": enabled, **listed}


@router.delete("/v1/vr/pair/{code_or_id}")
async def remove_pairing(code_or_id: str, token: str = Depends(verify_token)) -> dict:
    """Revokes a pending code, or disconnects a headset (by its id): it is
    told ``bye`` and its resume code stops working."""
    host = vr_pair_host.existing_host()
    done = await host.remove(code_or_id, _owner(token)) if host is not None else None
    if done is None:
        raise HTTPException(status_code=404, detail="No such pairing code or headset.")
    return {"result": done}
