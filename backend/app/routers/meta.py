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
from ..sessions import Session, get_session, session_for_owner, REGISTRY
from ..tasks import TASKS
from ..product_cache import PRODUCT_CACHE
from ..fits_dataset import ScientificFitsDataset
import asyncio

router = APIRouter(tags=['meta'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)
_configured_workers = int(os.environ.get("VISIVO_WORKERS", "4"))

@router.get("/v1/health", tags=["meta"])
async def health(token: str = _auth) -> dict:
    from ..auth import is_admin_token
    from ..dependencies import pool_health
    cache_stats = PRODUCT_CACHE.stats()
    task_stats = TASKS.stats()
    task_snapshot = TASKS.snapshot(recent_limit=5)
    return {
        "ok": True,
        "workers": _configured_workers,
        # A pool that had to be rebuilt means requests failed with the worker
        # that died. "ok" alone was reassuring a client whose every slice
        # request was failing.
        "worker_pool_restarts": pool_health()["restarts"],
        "active_sessions": REGISTRY.stats()["active_sessions"],
        "product_cache_entries": cache_stats["entries"],
        "product_cache_capacity": cache_stats["max_entries"],
        "task_registry_entries": task_stats["entries"],
        "task_ttl_enabled": task_stats["ttl_enabled"],
        "task_ttl_seconds": task_stats["ttl_seconds"],
        # Live activity for the desktop client's System Insights panel.
        "running_tasks": task_snapshot["running_count"],
        # The recent-task list names other users' operations, so it is only
        # detailed for the operator once per-user tokens are configured.
        "recent_tasks": task_snapshot["recent_tasks"] if is_admin_token(token) else [],
        "timestamp": datetime.now(tz=timezone.utc).isoformat(),
    }

@router.post("/v1/sessions/new", tags=["meta"])
async def session_new(token: str = _auth) -> dict:
    """Mint a fresh session owned by the caller and return its id.

    Clients should call this once and send the id as ``X-Visivo-Session`` on
    every subsequent request. A server-minted id is what makes ids fully
    non-enumerable: with VISIVO_STRICT_SESSIONS on (the default once per-user
    tokens exist) the backend refuses to adopt ids a client invented, so an
    unknown id and another user's id are indistinguishable (both 404).
    """
    from ..sessions import SessionQuotaExceeded
    try:
        session = REGISTRY.create(owner_token=token)
    except SessionQuotaExceeded as exc:
        raise HTTPException(status_code=429, detail=str(exc)) from exc
    return {"valid": True, "session_id": session.session_id}


@router.get("/v1/sessions", tags=["meta"])
async def session_stats(token: str = _auth) -> dict:
    """Expose session registry statistics (admin endpoint).

    Server-wide, so once per-user tokens are configured only the operator (the
    primary token) may read it; with a single token everyone is the operator.
    """
    from ..auth import is_admin_token
    if not is_admin_token(token):
        raise HTTPException(status_code=403, detail="Operator token required.")
    return REGISTRY.stats()


@router.get("/v1/sessions/{session_id}/datasets", tags=["meta"])
async def session_datasets(session_id: str, token: str = _auth) -> dict:
    """Enumerate the datasets currently open in a backend session.

    Used by the desktop client to populate selection lists for cross-dataset
    tools (e.g. Spectral Stacking, where the user picks ≥2 cubes opened in
    the same session). Returns minimal metadata so the client can filter by
    kind / shape without opening each dataset.

    Returns ``404`` if the session does not exist (e.g. backend restarted).
    """
    # Owner-checked: a session belonging to ANOTHER token must not be
    # enumerable (it exposes dataset ids and backend paths). Missing and
    # not-yours are reported identically so existence can't be probed.
    session = session_for_owner(session_id, token)
    if session is None:
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found.")
    datasets = []
    for ds_id, entry in session.datasets.items():
        datasets.append({
            "dataset_id": ds_id,
            "kind": entry.get("kind", ""),
            "path": entry.get("path", ""),
            "width": int(entry.get("width", 0) or 0),
            "height": int(entry.get("height", 0) or 0),
            "depth": int(entry.get("depth", 0) or 0),
        })
    return {"session_id": session_id, "datasets": datasets}
