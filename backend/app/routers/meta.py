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

router = APIRouter(tags=['meta'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)
_configured_workers = int(os.environ.get("VISIVO_WORKERS", "4"))

@router.get("/v1/health", tags=["meta"])
async def health(_: None = _auth) -> dict:
    cache_stats = PRODUCT_CACHE.stats()
    task_stats = TASKS.stats()
    return {
        "ok": True,
        "workers": _configured_workers,
        "active_sessions": REGISTRY.stats()["active_sessions"],
        "product_cache_entries": cache_stats["entries"],
        "product_cache_capacity": cache_stats["max_entries"],
        "task_registry_entries": task_stats["entries"],
        "task_ttl_enabled": task_stats["ttl_enabled"],
        "task_ttl_seconds": task_stats["ttl_seconds"],
        "timestamp": datetime.now(tz=timezone.utc).isoformat(),
    }

@router.get("/v1/sessions", tags=["meta"])
async def session_stats(_: None = _auth) -> dict:
    """Expose session registry statistics (admin endpoint)."""
    return REGISTRY.stats()
