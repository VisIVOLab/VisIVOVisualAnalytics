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


# ── Simple Cone Search ───────────────────────────────────────────────────────

class ConeSearchRequest(BaseModel):
    url: str
    ra: float
    dec: float
    radius: float
    verbosity: int = 2
    max_rows: int = 5000
    #: Workspace Exports filename for the saved CSV (basename only).
    output_basename: str = ""


class ConeSearchResponse(BaseModel):
    valid: bool
    error: str = ""
    #: The request actually sent, so a result can be reproduced outside the app.
    url: str = ""
    columns: list[str] = []
    rows: list[list[Any]] = []
    total_rows: int = 0
    truncated: bool = False
    #: The whole result, saved as CSV, so it can be opened as a catalogue.
    output_path: str = ""
    workspace_filename: str = ""
    #: {"ra": "<column>", "dec": "<column>"} when they could be identified.
    position_columns: dict[str, str] = {}


class ConeSearchServicesResponse(BaseModel):
    services: list[dict[str, str]]


@router.get("/v1/resolve/cone_search/services", response_model=ConeSearchServicesResponse,
            tags=["resolve"])
async def cone_search_services(_: None = _auth) -> ConeSearchServicesResponse:
    """A few known cone-search endpoints, so the field starts with something."""
    from ..cone_search import KNOWN_SERVICES  # noqa: PLC0415
    return ConeSearchServicesResponse(services=[dict(s) for s in KNOWN_SERVICES])


@router.post("/v1/resolve/cone_search", response_model=ConeSearchResponse, tags=["resolve"])
async def cone_search(request: ConeSearchRequest, _: None = _auth) -> ConeSearchResponse:
    """Run a VO Simple Cone Search and save the result as a CSV catalogue."""
    from ..cone_search import ConeSearchError, run_cone_search  # noqa: PLC0415

    basename = request.output_basename.strip() or "cone_search.csv"
    output_path = str(make_workspace_path(basename))
    try:
        result = await _run(
            run_cone_search, request.url, request.ra, request.dec, request.radius,
            int(request.verbosity), output_path, int(request.max_rows))
    except ConeSearchError as exc:
        return ConeSearchResponse(valid=False, error=str(exc))
    except Exception as exc:
        logger.exception("[cone-search] failed url=%s", request.url)
        return ConeSearchResponse(
            valid=False, error="The search could not be completed. See the backend log.")

    workspace_filename = Path(result["output_path"]).name if result["output_path"] else ""
    return ConeSearchResponse(valid=True, workspace_filename=workspace_filename, **result)
