from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel
from typing import Any
import uuid
import logging
import os
from pathlib import Path
from datetime import datetime, timezone
import base64
import numpy as np
import pandas as pd
from astropy.io import fits
from ..schemas import *
from ..dependencies import *
from ..auth import verify_token
from ..sessions import Session, get_session, REGISTRY
from ..tasks import TASKS
from ..product_cache import PRODUCT_CACHE
from ..fits_dataset import ScientificFitsDataset
import asyncio

router = APIRouter(tags=['cube'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

@router.post("/v1/cube/noise", response_model=CubeNoiseResponse, tags=["cube"])
async def cube_noise(
    request: CubeNoiseRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeNoiseResponse:
    """Estimate per-channel noise (MAD and Gaussian sigma) in an emission-free region."""
    from ..compute import worker_noise_estimate
    try:
        path = _require_cube_path(session, request.dataset_id)
        result = await _run_with_limit(
            session,
            worker_noise_estimate,
            path,
            request.x0, request.x1,
            request.y0, request.y1,
            request.channel_start,
            request.channel_end,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return CubeNoiseResponse(valid=False, error=str(exc))
    return CubeNoiseResponse(valid=True, error="", **result)

@router.post("/v1/cube/preview", response_model=CubePreviewResponse, tags=["cube"])
async def cube_preview(
    request: CubePreviewRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubePreviewResponse:
    from ..compute import worker_cube_preview
    try:
        path = _require_cube_path(session, request.dataset_id)
        result = await _run_with_limit(session, worker_cube_preview, path, request.downsample)
    except HTTPException:
        raise
    except Exception as exc:
        return CubePreviewResponse(valid=False, error=str(exc))
    return CubePreviewResponse(valid=True, error="", **result)

@router.post("/v1/cube/slice", response_model=CubeSliceResponse, tags=["cube"])
async def cube_slice(
    request: CubeSliceRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeSliceResponse:
    from ..compute import worker_cube_slice
    try:
        path = _require_cube_path(session, request.dataset_id)
        if request.axis.lower() != "z":
            raise HTTPException(422, "Only axis='z' is currently supported.")
        result = await _run_with_limit(session, worker_cube_slice, path, request.index)
    except HTTPException:
        raise
    except Exception as exc:
        return CubeSliceResponse(valid=False, error=str(exc))
    return CubeSliceResponse(valid=True, error="", **result)

@router.post("/v1/cube/subvolume", response_model=CubeSubvolumeResponse, tags=["cube"])
async def cube_subvolume(
    request: CubeSubvolumeRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeSubvolumeResponse:
    from ..compute import worker_cube_subvolume
    try:
        path = _require_cube_path(session, request.dataset_id)
        result = await _run_with_limit(
            session,
            worker_cube_subvolume,
            path,
            request.x0, request.x1,
            request.y0, request.y1,
            request.z0, request.z1,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return CubeSubvolumeResponse(valid=False, error=str(exc))
    return CubeSubvolumeResponse(valid=True, error="", **result)

@router.post("/v1/cube/pv", response_model=CubePvResponse, tags=["cube"])
async def cube_pv(
    request: CubePvRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubePvResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "This endpoint requires a spectral cube dataset.")
        result, _ = await _pv_product_payload(entry, request)
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[pv] failed dataset_id=%s error=%s", request.dataset_id, exc)
        return CubePvResponse(valid=False, error=str(exc))
    return CubePvResponse(valid=True, error="", **result)
