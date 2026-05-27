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

router = APIRouter(tags=['products'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

@router.post("/v1/products/moment", response_model=MomentProductResponse, tags=["products"])
async def moment_product(
    request: MomentProductRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> MomentProductResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "This endpoint requires a spectral cube dataset.")
        result, _ = await _moment_product_payload(entry, request)
    except HTTPException:
        raise
    except Exception as exc:
        return MomentProductResponse(valid=False, error=str(exc))
    return MomentProductResponse(valid=True, error="", **result)


# ── Persist a moment map to a standalone 2-D FITS file ─────────────────────
# Same input parameters as /v1/products/moment so the client can mirror the
# moment it is currently visualising. The output FITS is NOT registered as
# a session dataset (it's a 2-D image, not a cube) — the client receives
# the on-disk path and may re-open it via the Data Hub or share it.

class MomentSaveFitsRequest(BaseModel):
    dataset_id: str
    moment_order: int
    channel_start: int = 0
    channel_end: int = 0
    mask_enabled: bool = False
    threshold_value: float = 0.0
    # Workspace Exports filename (basename only). Empty → synthesised
    # from source name + moment order.
    output_basename: str = ""


class MomentSaveFitsResponse(BaseModel):
    valid: bool
    error: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    width: int = 0
    height: int = 0
    range_min: float = 0.0
    range_max: float = 0.0
    bunit: str = ""
    moment_unit: str = ""


@router.post("/v1/products/moment_fits",
             response_model=MomentSaveFitsResponse, tags=["products"])
async def moment_save_fits(
    request: MomentSaveFitsRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> MomentSaveFitsResponse:
    """Compute the requested moment and persist it in the **Workspace
    Exports** dir as a 2-D FITS file (celestial WCS + ``BUNIT`` derived
    from the cube). Collisions are resolved with a numeric suffix."""
    from ..compute import worker_moment_save_fits  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "Moment FITS export requires a spectral cube dataset.")

        src_stem = Path(entry["path"]).stem
        default_basename = f"{src_stem}_m{int(request.moment_order)}.fits"
        basename = request.output_basename.strip() or default_basename
        output_path = str(make_workspace_path(basename))

        result = await _run_heavy_with_limit(
            session,
            worker_moment_save_fits,
            entry["path"],
            output_path,
            int(request.moment_order),
            int(request.channel_start),
            int(request.channel_end),
            bool(request.mask_enabled),
            float(request.threshold_value),
        )
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[moment_fits] failed dataset_id=%s error=%s",
                       request.dataset_id, exc)
        return MomentSaveFitsResponse(valid=False, error=str(exc))
    workspace_filename = Path(result["output_path"]).name
    return MomentSaveFitsResponse(
        valid=True, workspace_filename=workspace_filename,
        **{k: result[k] for k in
           ("output_path", "width", "height", "range_min", "range_max",
            "bunit", "moment_unit")})

@router.post("/v1/products/isosurface", response_model=IsosurfaceProductResponse, tags=["products"])
async def isosurface_product(
    request: IsosurfaceProductRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> IsosurfaceProductResponse:
    if not _VTK_AVAILABLE:
        return IsosurfaceProductResponse(
            valid=False,
            error="VTK Python bindings are not installed on this server. Isosurface is unavailable.",
        )
    from ..compute import worker_isosurface
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "Isosurface requires a cube dataset.")
        iso_cache_params = {"threshold": float(request.threshold)}
        cache_key, param_hash = PRODUCT_CACHE.make_key(entry["path"], "isosurface", iso_cache_params)
        cached = PRODUCT_CACHE.get(cache_key)
        if cached is not None:
            logger.debug("[isosurface] cache hit dataset_id=%s threshold=%s", request.dataset_id, request.threshold)
            return IsosurfaceProductResponse(valid=True, error="", **cached.payload)
        # Heavy task — gated by the heavy-task semaphore so it leaves at
        # least one worker pool slot free for interactive slice / probe /
        # subvolume requests.
        result = await _run_heavy_with_limit(
            session,
            worker_isosurface,
            entry["path"],
            int(entry["width"]),
            int(entry["height"]),
            int(entry["depth"]),
            float(request.threshold),
        )
        dataset = ScientificFitsDataset(entry["path"])
        PRODUCT_CACHE.put(
            key=cache_key,
            operation="isosurface",
            dataset_path=entry["path"],
            parameter_hash=param_hash,
            payload=dict(result),
            scientific_metadata={"threshold": float(request.threshold)},
            provenance_metadata=dataset.provenance_context(),
        )
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[isosurface] failed dataset_id=%s error=%s", request.dataset_id, exc)
        return IsosurfaceProductResponse(valid=False, error=str(exc))
    return IsosurfaceProductResponse(valid=True, error="", **result)
