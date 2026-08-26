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

router = APIRouter(tags=['tasks'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

@router.post("/v1/tasks/moment", response_model=TaskCreateResponse, tags=["tasks"])
async def create_moment_task(
    request: MomentProductRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "Moment tasks require a cube dataset.")
    except HTTPException:
        raise

    task = TASKS.create("moment")
    TASKS.update(task.task_id, status="running", progress=0.05)

    async def _runner() -> None:
        ticker = asyncio.create_task(_progress_ticker(task.task_id))
        try:
            result, cache_hit = await _moment_product_payload(entry, request)
            ticker.cancel()
            TASKS.update(
                task.task_id,
                status="completed",
                progress=1.0,
                result={"valid": True, "error": "", **result},
                cache_hit=cache_hit,
            )
            record_history(  # E-06 — full params so a provenance replay reproduces
                session, operation="moment", dataset_id=request.dataset_id,
                endpoint="/v1/tasks/moment",
                params={"moment_order": request.moment_order,
                        "channel_start": request.channel_start,
                        "channel_end": request.channel_end,
                        "mask_enabled": request.mask_enabled,
                        "threshold_value": request.threshold_value,
                        "threshold_auto": getattr(request, "threshold_auto", False),
                        "moment2_variance": getattr(request, "moment2_variance", False)})
        except asyncio.CancelledError:
            ticker.cancel()
            TASKS.update(task.task_id, status="cancelled", progress=0.0, error="Cancelled by client.")
        except Exception as exc:  # pragma: no cover - exercised at runtime
            ticker.cancel()
            TASKS.update(task.task_id, status="failed", progress=1.0, error=str(exc))

    handle = asyncio.create_task(_runner())
    TASKS.update(task.task_id, asyncio_handle=handle)
    return TaskCreateResponse(valid=True, error="", task_id=task.task_id, status="running", cache_hit=False)

@router.post("/v1/tasks/pv", response_model=TaskCreateResponse, tags=["tasks"])
async def create_pv_task(
    request: CubePvRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "PV tasks require a cube dataset.")
    except HTTPException:
        raise

    task = TASKS.create("pv")
    TASKS.update(task.task_id, status="running", progress=0.05)

    async def _runner() -> None:
        ticker = asyncio.create_task(_progress_ticker(task.task_id))
        try:
            result, cache_hit = await _pv_product_payload(entry, request)
            ticker.cancel()
            TASKS.update(
                task.task_id,
                status="completed",
                progress=1.0,
                result={"valid": True, "error": "", **result},
                cache_hit=cache_hit,
            )
        except asyncio.CancelledError:
            ticker.cancel()
            TASKS.update(task.task_id, status="cancelled", progress=0.0, error="Cancelled by client.")
        except Exception as exc:  # pragma: no cover - exercised at runtime
            ticker.cancel()
            TASKS.update(task.task_id, status="failed", progress=1.0, error=str(exc))

    handle = asyncio.create_task(_runner())
    TASKS.update(task.task_id, asyncio_handle=handle)
    return TaskCreateResponse(valid=True, error="", task_id=task.task_id, status="running", cache_hit=False)

@router.get("/v1/tasks/{task_id}", response_model=TaskStatusResponse, tags=["tasks"])
async def task_status(task_id: str, _: None = _auth) -> TaskStatusResponse:
    task = TASKS.get(task_id)
    if task is None:
        raise HTTPException(status_code=404, detail=f"Unknown task_id '{task_id}'.")
    return TaskStatusResponse(
        valid=True,
        error=task.error,
        task_id=task.task_id,
        operation=task.operation,
        status=task.status,
        created_at=task.created_at,
        updated_at=task.updated_at,
        progress=task.progress,
        cache_hit=task.cache_hit,
        result=task.result,
    )

@router.delete("/v1/tasks/{task_id}", tags=["tasks"])
async def cancel_task(task_id: str, _: None = _auth) -> dict:
    """Request cancellation of a running or pending task.

    Returns 200 with ``{"cancelled": true}`` if the cancellation was accepted,
    or 404 if the task does not exist, or 409 if the task has already finished.
    A cancellation request is best-effort: the task may still complete before
    the asyncio cancellation propagates to the worker future.
    """
    task = TASKS.get(task_id)
    if task is None:
        raise HTTPException(status_code=404, detail=f"Unknown task_id '{task_id}'.")
    if task.status not in ("pending", "running"):
        raise HTTPException(
            status_code=409,
            detail=f"Task '{task_id}' is already in terminal state '{task.status}'.",
        )
    cancelled = TASKS.cancel(task_id)
    return {"cancelled": cancelled, "task_id": task_id}
