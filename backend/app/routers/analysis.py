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


# ── Full-cube statistics (out-of-core; the client's local stats are only the
#    decimated preview) ─────────────────────────────────────────────────────────
# Small in-process cache keyed by (path, size, mtime): the client requests cube
# stats once per open, and a full-res scan of a large cube is not free.
_CUBE_STATS_CACHE: dict[tuple[str, int, int], dict] = {}
_CUBE_STATS_CACHE_MAX = 64


@router.get("/v1/analysis/cube-stats/{dataset_id}", tags=["analysis"])
async def cube_stats(
    dataset_id: str,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> dict:
    """Whole-cube pixel statistics computed on the FULL-resolution data.

    Returns min / max / mean / median / std / rms / sum + finite/blank counts
    (and ``median_exact``). Streamed out-of-core in z-slab chunks, so it is safe
    on cubes far larger than RAM. Cached per file (path + size + mtime).
    """
    import os  # noqa: PLC0415
    from ..compute import worker_cube_stats  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    path = entry["path"]
    try:
        st = os.stat(path)
        key = (path, int(st.st_size), int(st.st_mtime_ns))
    except OSError:
        key = None
    if key is not None and key in _CUBE_STATS_CACHE:
        return {"valid": True, "error": "", "cached": True, **_CUBE_STATS_CACHE[key]}
    try:
        result = await asyncio.to_thread(worker_cube_stats, path)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[cube-stats] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc
    if key is not None:
        if len(_CUBE_STATS_CACHE) >= _CUBE_STATS_CACHE_MAX:
            _CUBE_STATS_CACHE.pop(next(iter(_CUBE_STATS_CACHE)))
        _CUBE_STATS_CACHE[key] = result
    return {"valid": True, "error": "", "cached": False, **result}


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
