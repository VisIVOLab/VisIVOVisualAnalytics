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
from .products import MomentSaveFitsRequest
import asyncio

router = APIRouter(tags=['tasks'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

@router.post("/v1/tasks/moment", response_model=TaskCreateResponse, tags=["tasks"])
async def create_moment_task(
    request: MomentProductRequest,
    token: str = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "Moment tasks require a cube dataset.")
    except HTTPException:
        raise

    task = TASKS.create("moment", owner_token=token)
    TASKS.update(task.task_id, status="running", progress=0.05)

    async def _runner() -> None:
        ticker = asyncio.create_task(_progress_ticker(task.task_id))
        try:
            result, cache_hit = await _moment_product_payload(entry, request, task.task_id)
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

@router.post("/v1/tasks/decompress", response_model=TaskCreateResponse, tags=["tasks"])
async def create_decompress_task(
    request: DecompressRequest,
    token: str = _auth,
) -> TaskCreateResponse:
    """Decompress a compressed FITS into the workspace cache, as a task.

    ``/v1/datasets/open`` decompresses on its own, in one request; a large
    ``.fits.gz`` takes minutes there, without progress and beyond any client's
    request timeout. This route does the same work as a task the client
    follows (``progress`` is the fraction decompressed) and can cancel
    (``DELETE /v1/tasks/{id}`` stops it within one 16 MB chunk and leaves no
    cache entry). Opening the file afterwards finds the cache entry at once.
    A file that is not compressed, or whose copy is cached already, is
    answered ``status: "completed"`` in this response (``cache_hit`` true for
    a cached copy), with the result in the task; nothing needs following.
    A file that is not compressed has ``compression: ""``.
    The result: ``compression``, ``cache_hit``, ``seconds``, ``source_bytes``
    and ``decompressed_bytes``.
    """
    from ..compressed_fits import (  # noqa: PLC0415
        CompressedFitsError, DecompressionCancelled, cached_copy, compression_of, materialize,
    )
    from ..fits_truncation import FitsTruncatedError  # noqa: PLC0415

    try:
        path = _normalize_path(request.path)
    except PermissionError:
        path = None
    if path is None or not _is_supported_dataset(path):
        # The same answer for a refused path as for a missing one.
        raise HTTPException(status_code=404, detail="File not found.")

    def _result(made) -> dict:
        result = {"valid": True, "error": "", "compression": "", "cache_hit": False,
                  "seconds": 0.0, "source_bytes": path.stat().st_size,
                  "decompressed_bytes": 0}
        if made is not None:
            result.update(compression=made.compression, cache_hit=made.cache_hit,
                          seconds=round(made.seconds, 3), source_bytes=made.source_bytes,
                          decompressed_bytes=made.bytes)
        return result

    def _ready() -> dict | None:
        """The result at once, when there is nothing to decompress: the file
        is not compressed, or its copy is in the cache already."""
        if compression_of(path) is None:
            return _result(None)
        made = cached_copy(path)
        return _result(made) if made is not None else None

    # A file opened before (the copy is cached) or not compressed is answered
    # completed in the create response itself, so the client opens it without
    # following a task: a cached reopen costs what a plain open does.
    try:
        ready = await asyncio.to_thread(_ready)
    except OSError:
        ready = None   # the task says what is wrong with the file
    if ready is not None:
        task = TASKS.create("decompress", owner_token=token)
        TASKS.update(task.task_id, status="completed", progress=1.0, result=ready,
                     cache_hit=ready["cache_hit"])
        return TaskCreateResponse(valid=True, error="", task_id=task.task_id,
                                  status="completed", cache_hit=ready["cache_hit"])

    task = TASKS.create("decompress", owner_token=token)
    task_id = task.task_id
    stop_file = TASKS.stop_file(task_id)
    TASKS.update(task_id, status="running", progress=0.0)

    def _work():
        return materialize(
            path,
            progress=lambda fraction: TASKS.update(task_id, progress=0.99 * fraction),
            should_stop=(lambda: os.path.exists(stop_file)) if stop_file else None)

    async def _runner() -> None:
        work = asyncio.ensure_future(asyncio.to_thread(_work))
        # A cancelled runner leaves the thread to notice the stop file; its
        # outcome is taken here so that it is never reported as unretrieved.
        work.add_done_callback(lambda f: f.cancelled() or f.exception())
        TASKS.update(task_id, work_handle=work)
        try:
            made = await asyncio.shield(work)
            result = _result(made)
            TASKS.update(task_id, status="completed", progress=1.0, result=result,
                         cache_hit=bool(made is not None and made.cache_hit))
        except (asyncio.CancelledError, DecompressionCancelled):
            TASKS.update(task_id, status="cancelled", error="Cancelled by client.")
        except (CompressedFitsError, FitsTruncatedError) as exc:
            TASKS.update(task_id, status="failed", progress=1.0, error=str(exc))
        except Exception as exc:  # noqa: BLE001 — reported through the task
            logger.exception("[decompress] %s failed", path)
            TASKS.update(task_id, status="failed", progress=1.0, error=str(exc))

    TASKS.update(task_id, asyncio_handle=asyncio.create_task(_runner()))
    return TaskCreateResponse(valid=True, error="", task_id=task_id, status="running")


@router.post("/v1/tasks/pv", response_model=TaskCreateResponse, tags=["tasks"])
async def create_pv_task(
    request: CubePvRequest,
    token: str = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "PV tasks require a cube dataset.")
    except HTTPException:
        raise

    task = TASKS.create("pv", owner_token=token)
    TASKS.update(task.task_id, status="running", progress=0.05)

    async def _runner() -> None:
        ticker = asyncio.create_task(_progress_ticker(task.task_id))
        try:
            result, cache_hit = await _pv_product_payload(entry, request, task.task_id)
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


# ── Moment map to FITS as a task ────────────────────────────────────────────
# The task form of /v1/products/moment_fits: same request, same result. On a
# large cube the synchronous route runs into VISIVO_HEAVY_TIMEOUT_S, while a
# task has no time limit, reports progress and can be cancelled: the worker
# polls the stop file between slabs and writes nothing when it stops.

@router.post("/v1/tasks/moment_fits", response_model=TaskCreateResponse, tags=["tasks"])
async def create_moment_fits_task(
    request: MomentSaveFitsRequest,
    token: str = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    from ..compute import worker_moment_save_fits  # noqa: PLC0415

    entry = _require_dataset(session, request.dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "Moment FITS export requires a spectral cube dataset.")
    basename = (request.output_basename.strip()
                or f"{Path(entry['path']).stem}_m{int(request.moment_order)}.fits")
    # Reserved now (an empty placeholder): the file is written minutes later,
    # and two exports started meanwhile must not be handed the same name.
    output_path = str(make_workspace_path(basename, reserve=True))

    def _drop_placeholder() -> None:
        """Give the reserved name back once the worker has ended: a task that
        did not complete leaves no file. The worker may outlive a cancelled
        task (past its last abort check it still writes), so the name stays
        ours, and the file is removed, only when the work is over."""
        def _remove(_f: Any = None) -> None:
            try:
                os.unlink(output_path)
            except OSError:
                pass

        record = TASKS.peek(task.task_id)
        work = getattr(record, "work_handle", None)
        if work is not None and not work.done():
            work.add_done_callback(_remove)
        else:
            _remove()

    task = TASKS.create("moment_fits", owner_token=token)
    TASKS.update(task.task_id, status="running", progress=0.05)

    async def _runner() -> None:
        ticker = asyncio.create_task(_progress_ticker(task.task_id))
        try:
            result = await _run_heavy(
                worker_moment_save_fits,
                entry["path"],
                output_path,
                int(request.moment_order),
                int(request.channel_start),
                int(request.channel_end),
                bool(request.mask_enabled),
                float(request.threshold_value),
                bool(request.threshold_auto),
                bool(request.moment2_variance),
                TASKS.stop_file(task.task_id),
                task_id=task.task_id,
            )
            ticker.cancel()
            # The same fields as the synchronous route's response.
            TASKS.update(
                task.task_id,
                status="completed",
                progress=1.0,
                result={"valid": True, "error": "",
                        "workspace_filename": Path(result["output_path"]).name,
                        **{k: result[k] for k in
                           ("output_path", "width", "height", "range_min", "range_max",
                            "bunit", "moment_unit")}},
            )
        except asyncio.CancelledError:
            ticker.cancel()
            _drop_placeholder()
            TASKS.update(task.task_id, status="cancelled", progress=0.0, error="Cancelled by client.")
        except Exception as exc:
            ticker.cancel()
            _drop_placeholder()
            logger.warning("[moment_fits][task] failed task_id=%s error=%s", task.task_id, exc)
            TASKS.update(task.task_id, status="failed", progress=1.0, error=str(exc))

    handle = asyncio.create_task(_runner())
    TASKS.update(task.task_id, asyncio_handle=handle)
    return TaskCreateResponse(valid=True, error="", task_id=task.task_id, status="running", cache_hit=False)


# ── Source finding as a task ─────────────────────────────────────────────────
# The finders are plugins (app/compute/finders): SoFiA-2 for cubes when it is
# installed, photutils segmentation for images. A finder run is a task so it can
# be polled and cancelled, and its products (label mask, VOTable catalogue) land
# in the workspace under sourcefind/<task_id>/ where the client opens them.

class SourceFindTaskRequest(BaseModel):
    dataset_id: str
    finder: str = "sofia2"
    params: dict[str, Any] = {}


@router.get("/v1/finders", tags=["tasks"])
async def list_finders(token: str = _auth) -> dict:
    """The source finders this node knows, available or not, with their parameters."""
    from ..compute import finders  # noqa: PLC0415
    return {"finders": [f.as_dict() for f in finders.catalogue()]}


@router.post("/v1/sofia2/suggest/{dataset_id}", response_model=Sofia2SuggestResponse, tags=["tasks"])
async def suggest_sofia2_parameters(
    dataset_id: str,
    request: Sofia2SuggestRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Sofia2SuggestResponse:
    """SoFiA-2 parameters suggested from the cube (issue #64), each with its
    reason, from one strided pass over the cube (app/compute/sofia_suggest).

    Answers whether or not SoFiA-2 is installed here: the suggestion is read
    from the data, and the run goes to ``POST /v1/tasks/sourcefind`` with
    ``params``. 422 for a dataset that is not a spectral-line cube of Stokes I
    (an image, a catalogue, a Stokes Q/U cube, a volume without a spectral
    axis), with the reason.
    """
    from ..compute.sofia_suggest import SuggestRefused, suggest  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "SoFiA-2 parameters are suggested for spectral-line cubes; "
                                 f"this dataset is a {entry['kind']}.")
    try:
        result = await _run_heavy_with_limit(session, suggest, entry["path"],
                                             request.data_type, request.line)
    except SuggestRefused as exc:
        raise HTTPException(422, str(exc)) from exc
    return Sofia2SuggestResponse(dataset_id=dataset_id, **result)


@router.post("/v1/tasks/sourcefind", response_model=TaskCreateResponse, tags=["tasks"])
async def create_sourcefind_task(
    request: SourceFindTaskRequest,
    token: str = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    import shutil  # noqa: PLC0415

    from ..compute import finders  # noqa: PLC0415

    entry = _require_dataset(session, request.dataset_id)
    meta = finders.info(request.finder)
    if meta is None:
        raise HTTPException(404, f"Unknown finder {request.finder!r}; this node has {', '.join(finders.names())}.")
    if not meta.available:
        raise HTTPException(503, f"{meta.name} is not available on this node: {meta.reason}")
    if entry["kind"] not in meta.kinds:
        raise HTTPException(422, f"{meta.name} works on {', '.join(meta.kinds)} datasets; "
                                 f"this one is a {entry['kind']}.")
    if request.finder == "sofia2":
        # Refused here, before a task exists: the caller knows SoFiA-2 never
        # started. The run checks again (the synchronous route shares it).
        from ..compute.finders.sofia2 import refused  # noqa: PLC0415

        if why := refused(dict(request.params or {})):
            raise HTTPException(422, why)

    task = TASKS.create("sourcefind", owner_token=token)
    TASKS.update(task.task_id, status="running", progress=0.05)
    out_dir = get_exports_dir() / "sourcefind" / task.task_id

    async def _runner() -> None:
        ticker = asyncio.create_task(_progress_ticker(task.task_id))
        try:
            result = await finders.run(request.finder, entry["path"], entry["kind"], out_dir,
                                       request.params, finders.default_timeout())
            ticker.cancel()
            TASKS.update(task.task_id, status="completed", progress=1.0,
                         result={"valid": True, "error": "", "dataset_id": request.dataset_id, **result})
            record_history(session, operation="sourcefind", dataset_id=request.dataset_id,
                           endpoint="/v1/tasks/sourcefind",
                           params={"finder": request.finder, **request.params})
        except asyncio.CancelledError:
            ticker.cancel()
            shutil.rmtree(out_dir, ignore_errors=True)
            TASKS.update(task.task_id, status="cancelled", progress=0.0, error="Cancelled by client.")
        except finders.FinderError as exc:
            ticker.cancel()
            shutil.rmtree(out_dir, ignore_errors=True)
            TASKS.update(task.task_id, status="failed", progress=1.0, error=str(exc))
        except Exception as exc:  # noqa: BLE001 — reported through the task
            ticker.cancel()
            shutil.rmtree(out_dir, ignore_errors=True)
            logger.exception("[sourcefind] task %s failed", task.task_id)
            TASKS.update(task.task_id, status="failed", progress=1.0, error=str(exc))

    handle = asyncio.create_task(_runner())
    TASKS.update(task.task_id, asyncio_handle=handle)
    return TaskCreateResponse(valid=True, error="", task_id=task.task_id, status="running", cache_hit=False)


def _owned_task(task_id: str, token: str):
    """Fetch a task only if it belongs to ``token``. Missing and not-yours are
    indistinguishable (both None → 404) so task ids can't be probed."""
    import secrets as _sec

    from ..auth import multi_user_mode as _multi
    task = TASKS.peek(task_id)          # no TTL refresh while checking ownership
    if task is None:
        return None
    owner = getattr(task, "owner_token", None)
    if owner is None:
        return None if _multi() else task
    if token is None or not _sec.compare_digest(owner, token):
        return None
    return task


@router.get("/v1/tasks/{task_id}", response_model=TaskStatusResponse, tags=["tasks"])
async def task_status(task_id: str, token: str = _auth) -> TaskStatusResponse:
    task = _owned_task(task_id, token)
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
async def cancel_task(task_id: str, token: str = _auth) -> dict:
    """Request cancellation of a running or pending task.

    Returns 200 with ``{"cancelled": true}`` if the cancellation was accepted,
    or 404 if the task does not exist, or 409 if the task has already finished.
    A cancellation request is best-effort: the task may still complete before
    the asyncio cancellation propagates to the worker future.
    """
    task = _owned_task(task_id, token)
    if task is None:
        raise HTTPException(status_code=404, detail=f"Unknown task_id '{task_id}'.")
    if task.status not in ("pending", "running"):
        raise HTTPException(
            status_code=409,
            detail=f"Task '{task_id}' is already in terminal state '{task.status}'.",
        )
    cancelled = TASKS.cancel(task_id)
    return {"cancelled": cancelled, "task_id": task_id}
