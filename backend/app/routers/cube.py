from fastapi import APIRouter, Depends, HTTPException, Query, Response
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


# ── Binary-streaming variant ──────────────────────────────────────────────
# Same semantics as /v1/cube/subvolume but the voxel buffer is streamed as
# raw little-endian float32 bytes (Content-Type: application/octet-stream),
# with metadata in custom X-Visivo-* response headers. Eliminates the
# base64 + zlib encoding pipeline that empirically dominates wall-time on
# multi-GB payloads (~80 s on a 1.6 GB SDC2 subcube vs ~5-10 s here).
@router.post("/v1/cube/subvolume_bin", tags=["cube"])
async def cube_subvolume_bin(
    request: CubeSubvolumeRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    from ..compute import worker_cube_subvolume_raw
    try:
        path = _require_cube_path(session, request.dataset_id)
        # Bypass the per-session task-slot limit (VISIVO_MAX_CONCURRENT_TASKS,
        # default 3). The binary endpoint is designed to be called as a
        # burst of N parallel POSTs by the desktop client for client-side
        # transport parallelism (4 slabs of one logical ROI in flight at
        # once). Counting each slab against the per-session quota would
        # cause the 4th POST to receive HTTP 429, the client to fall back
        # to the slow legacy JSON path, and the parallelism win to be
        # lost. The slab reads are I/O-bound on BeeGFS, not compute-heavy,
        # so unbounded concurrency here is safe.
        result = await _run(
            worker_cube_subvolume_raw,
            path,
            request.x0, request.x1,
            request.y0, request.y1,
            request.z0, request.z1,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return Response(
            content=str(exc).encode("utf-8"),
            media_type="text/plain",
            status_code=500,
        )

    raw_bytes: bytes = result["raw_bytes"]
    headers = {
        "X-Visivo-Width": str(result["width"]),
        "X-Visivo-Height": str(result["height"]),
        "X-Visivo-Depth": str(result["depth"]),
        "X-Visivo-Scalar-Type": str(result["scalar_type"]),
        "X-Visivo-Range-Min": repr(float(result["range_min"])),
        "X-Visivo-Range-Max": repr(float(result["range_max"])),
        "X-Visivo-Spectral-Axis-Type": str(result.get("spectral_axis_type", "")),
        "X-Visivo-Spectral-Axis-Unit": str(result.get("spectral_axis_unit", "")),
        "X-Visivo-Bunit": str(result.get("bunit", "")),
        "X-Visivo-Byte-Order": "little",
        "Content-Length": str(len(raw_bytes)),
    }
    return Response(
        content=raw_bytes,
        media_type="application/octet-stream",
        headers=headers,
    )

# ── Sub-cube extraction (crop ROI → new FITS dataset) ───────────────────────
# Inputs are inclusive 0-indexed pixel/channel ranges (same convention as
# CubeSubvolumeRequest). The output is written to a tempfile and registered
# as a new dataset in the current session so the client can immediately
# open it without an extra round-trip — same pattern as the Baseline
# Subtraction endpoint.

class CubeSaveSubregionRequest(BaseModel):
    dataset_id: str
    x0: int
    x1: int
    y0: int
    y1: int
    z0: int
    z1: int
    # Persistent Workspace Exports filename (basename only — no slashes).
    # If empty, a default is synthesised from source name + crop range.
    output_basename: str = ""


class CubeSaveSubregionResponse(BaseModel):
    valid: bool
    error: str = ""
    new_dataset_id: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    width: int = 0
    height: int = 0
    depth: int = 0
    range_min: float = 0.0
    range_max: float = 0.0


@router.post("/v1/cube/save_subregion",
             response_model=CubeSaveSubregionResponse, tags=["cube"])
async def cube_save_subregion(
    request: CubeSaveSubregionRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeSaveSubregionResponse:
    """Crop the source cube to ``[z0..z1, y0..y1, x0..x1]`` and persist it
    in the **Workspace Exports** dir (default ``~/.visivo/exports/``).

    Filename collisions are resolved by suffixing ``_1``, ``_2``, … so the
    user never silently overwrites a previous export."""
    from ..compute import worker_cube_save_subregion  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "Sub-cube export requires a spectral cube dataset.")

        # Default basename: <src_stem>_subcube_x<a>-<b>_y<a>-<b>_z<a>-<b>.fits
        # — encodes the crop in the filename so the user can tell apart
        # exports from the same parent cube at a glance.
        src_stem = Path(entry["path"]).stem
        default_basename = (
            f"{src_stem}_subcube_x{request.x0}-{request.x1}"
            f"_y{request.y0}-{request.y1}_z{request.z0}-{request.z1}.fits"
        )
        basename = request.output_basename.strip() or default_basename
        output_path = str(make_workspace_path(basename))

        result = await _run_with_limit(
            session,
            worker_cube_save_subregion,
            entry["path"],
            output_path,
            request.x0, request.x1,
            request.y0, request.y1,
            request.z0, request.z1,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return CubeSaveSubregionResponse(valid=False, error=str(exc))

    new_id = f"ds_{uuid.uuid4().hex[:12]}"
    session.datasets[new_id] = {
        "path":   result["output_path"],
        "kind":   "cube",
        "width":  result["width"],
        "height": result["height"],
        "depth":  result["depth"],
    }
    workspace_filename = Path(result["output_path"]).name
    logger.info(
        "[save_subregion] registered new dataset  new_id=%s  path=%s  "
        "shape=(d=%s, h=%s, w=%s)",
        new_id, result["output_path"],
        result["depth"], result["height"], result["width"],
    )
    return CubeSaveSubregionResponse(
        valid=True, new_dataset_id=new_id,
        workspace_filename=workspace_filename,
        **{k: result[k] for k in
           ("output_path", "width", "height", "depth", "range_min", "range_max")})


# ── Single-channel 2-D FITS export ───────────────────────────────────────────

class CubeSaveChannel2dRequest(BaseModel):
    dataset_id: str
    channel: int  # 0-indexed
    output_basename: str = ""


class CubeSaveChannel2dResponse(BaseModel):
    valid: bool
    error: str = ""
    new_dataset_id: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    width: int = 0
    height: int = 0
    channel: int = 0
    range_min: float = 0.0
    range_max: float = 0.0


@router.post("/v1/cube/save_channel_2d",
             response_model=CubeSaveChannel2dResponse, tags=["cube"])
async def cube_save_channel_2d(
    request: CubeSaveChannel2dRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeSaveChannel2dResponse:
    """Extract a single channel of a cube as a **standalone 2-D FITS image**.

    Unlike ``/v1/cube/save_subregion`` with a 1-channel z-range — which writes
    a degenerate 3-D cube (NAXIS=3 with depth=1) — this endpoint writes a
    proper 2-D file (NAXIS=2) so the image viewer opens it directly without
    the spectral axis stub. Used by the Stokes / spectral-index workflow:
    extract single-channel Q, U, I, V at the same frequency, then compute
    the derived maps client-side."""
    from ..compute import worker_cube_save_channel_2d  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "Channel export requires a spectral cube dataset.")

        src_stem = Path(entry["path"]).stem
        default_basename = f"{src_stem}_ch{request.channel:04d}.fits"
        basename = request.output_basename.strip() or default_basename
        output_path = str(make_workspace_path(basename))

        result = await _run_with_limit(
            session,
            worker_cube_save_channel_2d,
            entry["path"],
            output_path,
            request.channel,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return CubeSaveChannel2dResponse(valid=False, error=str(exc))

    new_id = f"ds_{uuid.uuid4().hex[:12]}"
    session.datasets[new_id] = {
        "path":   result["output_path"],
        "kind":   "image",
        "width":  result["width"],
        "height": result["height"],
        "depth":  1,
    }
    workspace_filename = Path(result["output_path"]).name
    logger.info(
        "[save_channel_2d] registered new dataset  new_id=%s  path=%s  "
        "shape=(h=%s, w=%s)  channel=%s",
        new_id, result["output_path"],
        result["height"], result["width"], result["channel"],
    )
    return CubeSaveChannel2dResponse(
        valid=True, new_dataset_id=new_id,
        workspace_filename=workspace_filename,
        **{k: result[k] for k in
           ("output_path", "width", "height", "channel", "range_min", "range_max")})


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
