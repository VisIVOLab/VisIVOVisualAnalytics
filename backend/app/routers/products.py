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
    entry = _require_dataset(session, request.dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "This endpoint requires a spectral cube dataset.")
    # Per-session backpressure: a moment map is a heavy task, so it consumes a
    # concurrency slot and 429s when the session is already at capacity. This
    # mirrors the spectral linewidth endpoint and lets the desktop client fall
    # back gracefully instead of piling unbounded work on the worker pool.
    if not session.try_acquire_task_slot():
        raise HTTPException(
            status_code=429,
            detail="Too many concurrent compute tasks for this session.",
        )
    try:
        result, _ = await _moment_product_payload(entry, request)
    except HTTPException:
        raise
    except Exception as exc:
        return MomentProductResponse(valid=False, error=str(exc))
    finally:
        session.release_task_slot()
    # Auto-record into the session provenance history (R-01/R-02).
    record_history(
        session,
        operation="moment",
        endpoint="/v1/products/moment",
        dataset_id=request.dataset_id,
        params={
            "moment_order": request.moment_order,
            "channel_start": request.channel_start,
            "channel_end": request.channel_end,
            "mask_enabled": request.mask_enabled,
            "threshold_value": request.threshold_value,
            "threshold_auto": request.threshold_auto,
        },
    )
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
    threshold_auto: bool = False
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
            bool(request.threshold_auto),
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


# ── Advanced 2-D products: pixel math, mosaic, publication figure, QC ───────
# Back SKAVA visualization use cases UC 3.2 / 3.3 / 1.3 / 4.2. The compute
# lives in app/products_advanced.py (pure, unit-tested); these endpoints own
# session/dataset resolution and workspace output paths.


class MapMathRequest(BaseModel):
    dataset_id: str                 # map A
    op: str                         # add|subtract|multiply|divide|ratio|log10|abs|square|spectral_index
    dataset_id_b: str = ""          # map B (binary ops)
    freq_a: float = 0.0             # Hz, for spectral_index
    freq_b: float = 0.0             # Hz, for spectral_index
    output_basename: str = ""


class ProductFitsResponse(BaseModel):
    valid: bool
    error: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    width: int = 0
    height: int = 0
    range_min: float = 0.0
    range_max: float = 0.0
    info: dict = {}


@router.post("/v1/products/map_math", response_model=ProductFitsResponse, tags=["products"])
async def map_math(
    request: MapMathRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ProductFitsResponse:
    """Pixel-wise math on one/two 2-D maps (UC 3.2), incl. spectral index."""
    from ..products_advanced import worker_map_math  # noqa: PLC0415
    try:
        entry_a = _require_dataset(session, request.dataset_id)
        path_b = None
        if request.dataset_id_b:
            path_b = _require_dataset(session, request.dataset_id_b)["path"]
        src_stem = Path(entry_a["path"]).stem
        basename = request.output_basename.strip() or f"{src_stem}_{request.op}.fits"
        output_path = str(make_workspace_path(basename))
        result = worker_map_math(
            output_path,
            path_a=entry_a["path"],
            op=request.op,
            path_b=path_b,
            freq_a=request.freq_a or None,
            freq_b=request.freq_b or None,
        )
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[map_math] failed dataset_id=%s error=%s", request.dataset_id, exc)
        return ProductFitsResponse(valid=False, error=str(exc))
    return ProductFitsResponse(
        valid=True, workspace_filename=Path(result["output_path"]).name,
        output_path=result["output_path"], width=result["width"], height=result["height"],
        range_min=result["range_min"], range_max=result["range_max"],
        info={"op": result["op"]})


class MosaicRequest(BaseModel):
    dataset_ids: list[str]
    weight_mode: str = "noise"      # noise | uniform
    output_basename: str = ""


@router.post("/v1/products/mosaic", response_model=ProductFitsResponse, tags=["products"])
async def mosaic(
    request: MosaicRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ProductFitsResponse:
    """Noise-weighted reproject + co-add of 2-D images (UC 3.3)."""
    from ..products_advanced import worker_mosaic  # noqa: PLC0415
    try:
        if len(request.dataset_ids) < 2:
            raise HTTPException(422, "Mosaicking requires at least two datasets.")
        paths = [_require_dataset(session, did)["path"] for did in request.dataset_ids]
        basename = request.output_basename.strip() or "mosaic.fits"
        output_path = str(make_workspace_path(basename))
        result = worker_mosaic(output_path, paths=paths, weight_mode=request.weight_mode)
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[mosaic] failed n=%s error=%s", len(request.dataset_ids), exc)
        return ProductFitsResponse(valid=False, error=str(exc))
    return ProductFitsResponse(
        valid=True, workspace_filename=Path(result["output_path"]).name,
        output_path=result["output_path"], width=result["width"], height=result["height"],
        range_min=result["range_min"], range_max=result["range_max"],
        info={"n_inputs": result["n_inputs"], "weight_mode": result["weight_mode"],
              "coverage_fraction": result["coverage_fraction"]})


class PublicationFigureRequest(BaseModel):
    dataset_id: str
    cmap: str = "inferno"
    stretch: str = "linear"         # linear | log | sqrt | asinh | power
    percent: float = 99.5
    title: str = ""
    add_grid: bool = True
    add_colorbar: bool = True
    dpi: int = 200
    output_basename: str = ""       # .png/.pdf/.svg; defaults to .png


class PublicationFigureResponse(BaseModel):
    valid: bool
    error: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    width: int = 0
    height: int = 0
    has_wcs: bool = False


@router.post("/v1/products/publication_figure",
             response_model=PublicationFigureResponse, tags=["products"])
async def publication_figure(
    request: PublicationFigureRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> PublicationFigureResponse:
    """Render a publication-quality figure with WCS grid + colorbar (UC 1.3)."""
    from ..products_advanced import worker_publication_figure  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        basename = request.output_basename.strip() or f"{Path(entry['path']).stem}_figure.png"
        if not basename.lower().endswith((".png", ".pdf", ".svg")):
            basename = basename + ".png"
        # make_workspace_path defaults missing extensions to .fits, so pass a
        # name that already carries an image extension (handled above).
        output_path = str(make_workspace_path(basename))
        result = worker_publication_figure(
            output_path, path=entry["path"], cmap=request.cmap, stretch=request.stretch,
            percent=request.percent, title=request.title, add_grid=request.add_grid,
            add_colorbar=request.add_colorbar, dpi=int(request.dpi))
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[publication_figure] failed dataset_id=%s error=%s", request.dataset_id, exc)
        return PublicationFigureResponse(valid=False, error=str(exc))
    return PublicationFigureResponse(
        valid=True, output_path=result["output_path"],
        workspace_filename=Path(result["output_path"]).name,
        width=result["width"], height=result["height"], has_wcs=result["has_wcs"])


class ImageQualityRequest(BaseModel):
    dataset_id: str


class ImageQualityResponse(BaseModel):
    valid: bool
    error: str = ""
    metrics: dict = {}


@router.post("/v1/products/image_quality", response_model=ImageQualityResponse, tags=["products"])
async def image_quality(
    request: ImageQualityRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ImageQualityResponse:
    """Quality / artifact metrics for a 2-D image (UC 4.2)."""
    from ..products_advanced import worker_image_quality  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        metrics = worker_image_quality(entry["path"])
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[image_quality] failed dataset_id=%s error=%s", request.dataset_id, exc)
        return ImageQualityResponse(valid=False, error=str(exc))
    return ImageQualityResponse(valid=True, metrics=metrics)
