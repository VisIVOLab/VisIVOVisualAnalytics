"""Analysis endpoints — filament / skeleton extraction from an image plane.

Restored after a router refactor dropped these routes while the desktop client
(BackendClient::requestSkeleton) kept POSTing to ``.../binary``. The compute
(``worker_skeleton``) was never removed.
"""
import asyncio
import base64
import logging

import numpy as np
from fastapi import APIRouter, Depends, HTTPException, Response
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import _require_dataset
from ..sessions import Session, get_session
from .spectral import _array_to_binary

router = APIRouter(tags=["analysis"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)


class SkeletonRequest(BaseModel):
    channel: int = 0
    threshold: float | None = None
    mode: str = "skeleton"
    smooth_sigma_pix: float = 0.0
    persistence_threshold: float | None = None


async def _compute_skeleton(session: Session, dataset_id: str, request: SkeletonRequest) -> dict:
    from ..compute import worker_skeleton  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    try:
        return await asyncio.to_thread(
            worker_skeleton, entry["path"], request.channel, request.threshold,
            request.mode, request.smooth_sigma_pix, request.persistence_threshold)
    except ImportError as exc:
        raise HTTPException(503, str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[skeleton] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc


@router.post("/v1/analysis/skeleton/{dataset_id}", tags=["analysis"])
async def skeleton(
    dataset_id: str,
    request: SkeletonRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Skeleton / filament mask as a base64 float32 image plus endpoint list."""
    result = await _compute_skeleton(session, dataset_id, request)
    return {"valid": True, "error": "", **result}


@router.post("/v1/cosmology/skeleton/{dataset_id}", tags=["analysis"])
async def cosmology_skeleton_alias(
    dataset_id: str,
    request: SkeletonRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Alias of ``/v1/analysis/skeleton`` kept for the cosmology/void grouping."""
    result = await _compute_skeleton(session, dataset_id, request)
    return {"valid": True, "error": "", **result}


@router.post("/v1/analysis/skeleton/{dataset_id}/binary", tags=["analysis"])
async def skeleton_binary(
    dataset_id: str,
    request: SkeletonRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Same skeleton map as a raw Visivo binary frame (height × width float32)."""
    result = await _compute_skeleton(session, dataset_id, request)
    h, w = int(result["height"]), int(result["width"])
    arr = np.frombuffer(base64.b64decode(result["data_base64"]), dtype=np.float32).reshape(h, w)
    return Response(content=_array_to_binary(h, w, arr), media_type="application/octet-stream")
