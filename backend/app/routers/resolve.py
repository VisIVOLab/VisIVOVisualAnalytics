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

router = APIRouter(tags=['resolve'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

@router.post("/v1/resolve/target", response_model=TargetResolveResponse, tags=["resolve"])
async def resolve_target(
    request: TargetResolveRequest,
    _: None = _auth,
) -> TargetResolveResponse:
    """Resolve an object name to RA/Dec via the Sesame name resolver (SIMBAD/NED/VizieR).
    Requires astropy; returns an error if astropy is not installed."""
    name = request.name.strip()
    if not name:
        return TargetResolveResponse(valid=False, error="Empty target name.")
    loop = asyncio.get_running_loop()
    d = await loop.run_in_executor(None, _resolve_target_sync, name)
    return TargetResolveResponse(**d)
