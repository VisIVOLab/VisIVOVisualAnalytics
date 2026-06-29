"""routers/velocity.py — REST endpoints for the cosmic-flow velocity field (F1).

A velocity field is a 3-component vector grid (not a scalar cube), so it gets its
own open + binary-grid endpoints. The desktop fetches the raw vector grid and
runs vtkStreamTracer / vtkGlyph3D locally (interactive rendering).
"""
import logging
import uuid

from fastapi import APIRouter, Depends, Response
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import _run
from ..sessions import Session, get_session
from ..velocity_field import velocity_grid_worker, velocity_summary_worker

router = APIRouter(tags=["velocity"])
logger = logging.getLogger("visivo.velocity.router")
_auth = Depends(verify_token)


class VelocityOpenRequest(BaseModel):
    path: str
    box_mpc: float | None = None  # physical box size in Mpc; None → unit spacing


class VelocityOpenResponse(BaseModel):
    valid: bool
    error: str = ""
    dataset_id: str = ""
    session_id: str = ""
    summary: dict | None = None


class VelocityGridRequest(BaseModel):
    dataset_id: str


@router.post("/v1/velocity/open", response_model=VelocityOpenResponse, tags=["velocity"])
async def velocity_open(
    request: VelocityOpenRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> VelocityOpenResponse:
    try:
        summary = await _run(velocity_summary_worker, request.path, request.box_mpc)
    except Exception as exc:
        return VelocityOpenResponse(valid=False, error=str(exc))
    dataset_id = f"vf_{uuid.uuid4().hex[:12]}"
    session.datasets[dataset_id] = {
        "path": request.path,
        "kind": "velocity",
        "box_mpc": request.box_mpc,
    }
    logger.info("[velocity] session=%s dataset_id=%s path=%s",
                session.session_id, dataset_id, request.path)
    return VelocityOpenResponse(
        valid=True, dataset_id=dataset_id, session_id=session.session_id, summary=summary
    )


@router.post("/v1/velocity/grid_bin", tags=["velocity"])
async def velocity_grid_bin(
    request: VelocityGridRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    entry = session.datasets.get(request.dataset_id)
    if not entry or entry.get("kind") != "velocity":
        return Response(content=b"unknown velocity dataset",
                        media_type="text/plain", status_code=404)
    try:
        payload = await _run(velocity_grid_worker, entry["path"], entry.get("box_mpc"))
    except Exception as exc:
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=500)

    raw: bytes = payload["raw_bytes"]
    headers = {
        "X-Visivo-Width": str(payload["nx"]),
        "X-Visivo-Height": str(payload["ny"]),
        "X-Visivo-Depth": str(payload["nz"]),
        "X-Visivo-Components": "3",
        "X-Visivo-Spacing": ",".join(repr(float(s)) for s in payload["spacing"]),
        "X-Visivo-Origin": ",".join(repr(float(o)) for o in payload["origin"]),
        "X-Visivo-Speed-Min": repr(float(payload["speed_min"])),
        "X-Visivo-Speed-Max": repr(float(payload["speed_max"])),
        "X-Visivo-Unit": str(payload["unit"]),
        "X-Visivo-Frame": str(payload["frame"]),
        "X-Visivo-Byte-Order": "little",
        "Content-Length": str(len(raw)),
    }
    return Response(content=raw, media_type="application/octet-stream", headers=headers)
