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

router = APIRouter(tags=['image'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

@router.post("/v1/image/full", response_model=ImageFullResponse, tags=["image"])
async def image_full(
    request: ImageFullRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ImageFullResponse:
    from ..compute import worker_image_full
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "image":
            return ImageFullResponse(valid=False, error="Image endpoint requires an image dataset.")
        result = await _run_with_limit(session, worker_image_full, entry["path"])
    except HTTPException:
        raise
    except Exception as exc:
        return ImageFullResponse(valid=False, error=str(exc))
    return ImageFullResponse(valid=True, error="", **result)

@router.post("/v1/image/preview", response_model=ImageFullResponse, tags=["image"])
async def image_preview(
    request: ImagePreviewRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ImageFullResponse:
    from ..compute import worker_image_preview
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "image":
            raise HTTPException(422, "Image preview endpoint requires an image dataset.")
        result = await _run_with_limit(session, worker_image_preview, entry["path"], request.max_longest_side)
    except HTTPException:
        raise
    except Exception as exc:
        return ImageFullResponse(valid=False, error=str(exc))
    return ImageFullResponse(valid=True, error="", **result)


@router.post("/v1/image/tile", response_model=ImageTileResponse, tags=["image"])
async def image_tile(
    request: ImageTileRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ImageTileResponse:
    """Serve one out-of-core pyramid tile of a 2-D image mosaic.

    Unlike ``/v1/image/full`` (which loads the whole plane), this reads only the
    decimated tile region from the memmap, so multi-GB mosaics can be panned /
    zoomed without materialising the full image. Level 0 is full resolution;
    level ``L`` decimates by ``2**L``. Out-of-range tiles → 422.
    """
    from ..compute import worker_image_tile
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "image":
            raise HTTPException(422, "Image tile endpoint requires an image dataset.")
        result = await _run_with_limit(
            session, worker_image_tile, entry["path"],
            request.level, request.tile_x, request.tile_y, request.tile_size,
        )
    except HTTPException:
        raise
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:
        return ImageTileResponse(valid=False, error=str(exc))
    return ImageTileResponse(valid=True, error="", **result)
