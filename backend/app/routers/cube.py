from fastapi import APIRouter, Depends, HTTPException, Query, Request, Response
from pydantic import BaseModel, Field, model_validator
from typing import Any
import math
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
        # Channel scrubbing must feel instantaneous (<200 ms). Bypass both
        # Dask (overhead ~100-500 ms + queueing behind in-flight slabs/
        # moments) AND the per-session task slot limit (which would 429
        # rapid scroll bursts). Slice work is a 1.7 MB single-plane read
        # — runs comfortably in the local ProcessPoolExecutor.
        result = await _run_interactive(worker_cube_slice, path, request.index)
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
    except ValueError as exc:
        # Bad user input — a ROI wholly outside the cube, or inverted after
        # clamping — is 422, not a server error. The worker raises ValueError for
        # exactly those, and the other binary cube routes already answer this way;
        # this one was returning 500 for "Invalid subvolume ROI."
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=422)
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

# ── Tilted-ring kinematic model (F2) ────────────────────────────────────────
# Build a model PPV cube of a regularly rotating disk from a compact tilted-ring
# parameter set and stream it as raw little-endian float32 (same wire format as
# subvolume_bin). The desktop contours it over the data cube so anomalous gas
# (emission outside the model footprint/velocity field) stands out.

class CubeKinematicRing(BaseModel):
    """One ring of a per-ring (warped) tilted-ring model."""
    r_pix: float = Field(gt=0.0, allow_inf_nan=False)             # ring radius, pixels
    pa_deg: float = Field(allow_inf_nan=False)                    # position angle at this ring
    incl_deg: float = Field(ge=0.0, le=90.0, allow_inf_nan=False) # inclination at this ring
    vrot: float = Field(allow_inf_nan=False)                      # rotation velocity at this ring


class CubeKinematicModelRequest(BaseModel):
    dataset_id: str
    # Finite-only floats (reject NaN/inf at the API boundary).
    x0: float = Field(allow_inf_nan=False)            # disk centre, pixels
    y0: float = Field(allow_inf_nan=False)
    vsys: float = Field(allow_inf_nan=False)          # systemic velocity (spectral unit)
    sigma: float = Field(gt=0.0, allow_inf_nan=False)      # line dispersion (spectral unit)
    # Scalar single-ring parameters — REQUIRED when `rings` is empty (enforced by
    # the validator below), optional/ignored for a per-ring request.
    pa_deg: float | None = Field(default=None, allow_inf_nan=False)       # position angle of major axis
    incl_deg: float | None = Field(default=None, ge=0.0, le=90.0, allow_inf_nan=False)  # 0 face-on … 90 edge-on
    vrot: float | None = Field(default=None, allow_inf_nan=False)         # flat rotation velocity
    rmax_pix: float | None = Field(default=None, gt=0.0, allow_inf_nan=False)  # disk radius, pixels
    # Optional per-ring profile (3DBarolo/TiRiFiC-style warp + rotation curve).
    # When given the model varies PA/incl/Vrot with radius and rmax = the
    # outermost ring; the scalar pa/incl/vrot/rmax are ignored.
    rings: list[CubeKinematicRing] = Field(default_factory=list)

    @model_validator(mode="after")
    def _validate_params(self) -> "CubeKinematicModelRequest":
        if not self.rings:
            missing = [n for n in ("pa_deg", "incl_deg", "vrot", "rmax_pix")
                       if getattr(self, n) is None]
            if missing:
                raise ValueError(
                    "scalar kinematic model requires " + ", ".join(missing)
                    + " (or provide a `rings` per-ring profile)")
        else:
            # Ring radii must be distinct: duplicate radii make the piecewise
            # interpolation order-dependent and impossible for a user to reason
            # about.
            radii = [r.r_pix for r in self.rings]
            if len(set(radii)) != len(radii):
                raise ValueError("ring radii (r_pix) must be distinct")
        return self


@router.post("/v1/cube/kinematic_model_bin", tags=["cube"])
async def cube_kinematic_model_bin(
    request: CubeKinematicModelRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    from ..kinematic_model import kinematic_model_worker, max_voxels, ModelTooLargeError
    try:
        path = _require_cube_path(session, request.dataset_id)
        # Plain tuples (picklable for the pre-forked worker pool). Empty when the
        # request is a scalar single-ring model.
        rings = [(r.r_pix, r.pa_deg, r.incl_deg, r.vrot) for r in request.rings]
        # Scalar params are validated non-None when rings is empty; when rings is
        # given they are ignored, so coalesce None → 0.0 for the positional call.
        result = await _run_heavy_with_limit(
            session,
            kinematic_model_worker,
            path,
            request.x0, request.y0,
            request.pa_deg or 0.0, request.incl_deg or 0.0,
            request.vsys, request.vrot or 0.0,
            request.rmax_pix or 1.0, request.sigma,
            max_voxels(),
            rings,
        )
    except HTTPException:
        raise
    except ModelTooLargeError as exc:
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=413)
    except Exception as exc:
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=500)

    raw_bytes: bytes = result["raw_bytes"]
    headers = {
        "X-Visivo-Width": str(result["width"]),
        "X-Visivo-Height": str(result["height"]),
        "X-Visivo-Depth": str(result["depth"]),
        "X-Visivo-Scalar-Type": "float32",
        "X-Visivo-Peak": repr(float(result["peak"])),
        "X-Visivo-Spectral-Axis-Unit": str(result.get("spectral_axis_unit", "")),
        "X-Visivo-Spectral-Mode": str(result.get("spectral_mode", "")),
        "X-Visivo-Byte-Order": "little",
        "Content-Length": str(len(raw_bytes)),
    }
    return Response(content=raw_bytes, media_type="application/octet-stream",
                    headers=headers)

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


# ── Interactive 3-D masking (blank inside / outside a box) ───────────────────
# Blank the voxels inside or outside a 3-D box, keeping full cube dimensions +
# WCS, and register the masked cube as a new session dataset — the SlicerAstro /
# van der Hulst "3-D selection coupled to analysis" use case: isolate a clump of
# anomalous gas (blank outside) or remove an artefact (blank inside), then
# moment-map / profile / noise-estimate on just the selection.

class CubeMaskRequest(BaseModel):
    dataset_id: str
    x0: int
    x1: int
    y0: int
    y1: int
    z0: int
    z1: int
    mode: str = "blank_outside"   # "blank_outside" (isolate box) | "blank_inside" (remove box)
    output_basename: str = ""

    @model_validator(mode="after")
    def _check_mode(self) -> "CubeMaskRequest":
        if self.mode not in ("blank_inside", "blank_outside"):
            raise ValueError("mode must be 'blank_inside' or 'blank_outside'")
        return self


class CubeMaskResponse(BaseModel):
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


@router.post("/v1/cube/mask", response_model=CubeMaskResponse, tags=["cube"])
async def cube_mask(
    request: CubeMaskRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeMaskResponse:
    """Blank inside/outside a 3-D box and persist the masked cube in the
    Workspace Exports dir, registered as a new session dataset."""
    from ..compute import worker_cube_mask  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "3-D masking requires a spectral cube dataset.")

        src_stem = Path(entry["path"]).stem
        tag = "inside" if request.mode == "blank_inside" else "outside"
        default_basename = (
            f"{src_stem}_mask-{tag}_x{request.x0}-{request.x1}"
            f"_y{request.y0}-{request.y1}_z{request.z0}-{request.z1}.fits"
        )
        basename = request.output_basename.strip() or default_basename
        output_path = str(make_workspace_path(basename))

        # Heavy throttle: masking rewrites a full-size cube (I/O over every voxel
        # for blank_outside), so it is not an interactive/light task.
        result = await _run_heavy_with_limit(
            session,
            worker_cube_mask,
            entry["path"],
            output_path,
            request.x0, request.x1,
            request.y0, request.y1,
            request.z0, request.z1,
            request.mode,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return CubeMaskResponse(valid=False, error=str(exc))

    new_id = f"ds_{uuid.uuid4().hex[:12]}"
    session.datasets[new_id] = {
        "path":   result["output_path"],
        "kind":   "cube",
        "width":  result["width"],
        "height": result["height"],
        "depth":  result["depth"],
    }
    workspace_filename = Path(result["output_path"]).name
    logger.info("[cube_mask] registered new dataset new_id=%s path=%s mode=%s",
                new_id, result["output_path"], request.mode)
    return CubeMaskResponse(
        valid=True, new_dataset_id=new_id, workspace_filename=workspace_filename,
        **{k: result[k] for k in
           ("output_path", "width", "height", "depth", "range_min", "range_max")})


# ── Kinematic Lasso — geodesic 3-D selection from a clicked seed ──────────────
# Click a source in the volume render → the front end ray-casts a seed voxel →
# this grows a kinematics-aware geodesic region (bright voxels cheap to cross,
# faint/NaN = barriers; velocity steps weighted by lambda_v) and returns a binary
# mask cube the viewer contours as a live isosurface preview. Then apply it
# (isolate / remove) via /v1/cube/mask_selection.

class CubeKinematicSelectRequest(BaseModel):
    dataset_id: str
    seed_z: int
    seed_y: int
    seed_x: int
    # Extra seeds for multi-object selection: ⇧-click ADDs another source (union),
    # ⌜-click CARVEs a region out. Each is [z, y, x].
    add_seeds: list[list[int]] = Field(default_factory=list)
    sub_seeds: list[list[int]] = Field(default_factory=list)
    reach: float = Field(default=8.0, gt=0.0, allow_inf_nan=False)     # cost budget
    lambda_v: float = Field(default=1.0, gt=0.0, allow_inf_nan=False)  # velocity weight
    sigma: float = Field(default=0.0, ge=0.0, allow_inf_nan=False)     # 0 → robust auto
    nsigma: float = Field(default=2.5, gt=0.0, allow_inf_nan=False)    # soft threshold (σ units)
    roi_radius: int = Field(default=0, ge=0, le=256)                   # 0 = object-bounded (grab whole connected source); >0 = hard half-box around the seed
    # Interactive-slider budget: when > 0 the caller wants a LEVEL FIELD valid for
    # every integer budget up to this, not a single mask. Capped at 254, the
    # largest level a uint8 field can carry alongside the unreachable sentinel.
    reach_max: float = Field(default=0.0, ge=0.0, le=254.0, allow_inf_nan=False)
    # Smooth-before-threshold, in sigma voxels per axis (velocity, y, x). 0 = off,
    # which is the default and the behaviour every published number was measured
    # with. It exists for ONE shape of object: a large source whose faint envelope
    # a per-voxel cut cannot see. Measured on WALLABY's NGC 3137 it takes the
    # selection from 16% of the SoFiA mask to ~103%; measured on the compact,
    # crowded sources of HCG 44 it is actively harmful — completeness reaches 1.0
    # but Dice falls from 0.84 to 0.59, because the selection bleeds into the
    # neighbours the lasso exists to keep apart. Per-object knob, never a default.
    smooth_v: float = Field(default=0.0, ge=0.0, le=16.0, allow_inf_nan=False)
    smooth_xy: float = Field(default=0.0, ge=0.0, le=16.0, allow_inf_nan=False)

    @model_validator(mode="after")
    def _check_seeds(self) -> "CubeKinematicSelectRequest":
        for s in (*self.add_seeds, *self.sub_seeds):
            if len(s) != 3:
                raise ValueError("each add/sub seed must be [z, y, x]")
        return self


@router.post("/v1/cube/kinematic_levels_bin", tags=["cube"])
async def cube_kinematic_levels_bin(
    request: CubeKinematicSelectRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Geodesic LEVEL FIELDS for the interactive Reach slider.

    One grow up to ``reach_max`` instead of one grow per slider step: the body is
    two uint8 fields (add, then sub) over a shared bounding box, and the client
    selects at any integer budget r with ``add <= r`` minus ``sub <= r``. Validation
    showed the useful reach depends on the source's surface brightness and cannot
    be defaulted from the header, so dragging it has to be cheap.
    """
    from ..compute import worker_cube_kinematic_levels  # noqa: PLC0415
    try:
        path = _require_cube_path(session, request.dataset_id)
        budget = request.reach_max if request.reach_max > 0.0 else request.reach
        seeds = [[request.seed_z, request.seed_y, request.seed_x],
                 *[list(s) for s in request.add_seeds]]
        subs = [list(s) for s in request.sub_seeds]
        result = await _run_heavy_with_limit(
            session,
            worker_cube_kinematic_levels,
            path, seeds, subs, budget,
            request.lambda_v, request.sigma, request.roi_radius, request.nsigma,
            # The interactive path honours smoothing too: it is what the panel's
            # Smoothing control drives (the select route already did).
            ((request.smooth_v, request.smooth_xy, request.smooth_xy)
             if (request.smooth_v > 0.0 or request.smooth_xy > 0.0) else None),
        )
    except HTTPException:
        raise
    except ValueError as exc:
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=422)
    except Exception as exc:
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=500)

    return Response(
        content=result["raw_bytes"],
        media_type="application/octet-stream",
        headers={
            # ROI dims + origin: the payload is TWO fields of this size, add then sub.
            "X-Visivo-Width": str(result["roi_width"]),
            "X-Visivo-Height": str(result["roi_height"]),
            "X-Visivo-Depth": str(result["roi_depth"]),
            "X-Visivo-Origin": f"{result['origin_x']},{result['origin_y']},{result['origin_z']}",
            "X-Visivo-Full-Shape":
                f"{result['full_width']},{result['full_height']},{result['full_depth']}",
            "X-Visivo-Scalar-Type": "uint8",
            "X-Visivo-Components": "2",          # add levels, then sub levels
            "X-Visivo-Range-Max": str(result["reach_max"]),      # highest usable budget
            "X-Visivo-Peak": str(result["unreachable"]),         # sentinel level
        },
    )


@router.post("/v1/cube/kinematic_select_bin", tags=["cube"])
async def cube_kinematic_select_bin(
    request: CubeKinematicSelectRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    from ..compute import worker_cube_kinematic_select  # noqa: PLC0415
    try:
        path = _require_cube_path(session, request.dataset_id)
        seeds = [[request.seed_z, request.seed_y, request.seed_x], *[list(s) for s in request.add_seeds]]
        subs = [list(s) for s in request.sub_seeds]
        result = await _run_heavy_with_limit(
            session,
            worker_cube_kinematic_select,
            path,
            seeds, subs,
            request.reach, request.lambda_v, request.sigma,
            request.roi_radius, request.nsigma,
            request.reach_max if request.reach_max > 0.0 else None,
            ((request.smooth_v, request.smooth_xy, request.smooth_xy)
             if (request.smooth_v > 0.0 or request.smooth_xy > 0.0) else None),
        )
    except HTTPException:
        raise
    except ValueError as exc:
        # Bad user input (seed outside cube / on blank emission, empty seed set) →
        # 422, not a server error.
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=422)
    except Exception as exc:
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=500)

    raw_bytes: bytes = result["raw_bytes"]
    bbox = result["bbox"]
    headers = {
        # ROI dimensions + its origin in cube voxels (the payload is the bounding
        # box of the selection, not the full cube) + the full cube dims.
        "X-Visivo-Width": str(result["roi_width"]),
        "X-Visivo-Height": str(result["roi_height"]),
        "X-Visivo-Depth": str(result["roi_depth"]),
        "X-Visivo-Origin": f"{result['origin_x']},{result['origin_y']},{result['origin_z']}",
        "X-Visivo-Full-Shape": f"{result['full_width']},{result['full_height']},{result['full_depth']}",
        "X-Visivo-Scalar-Type": "float32",
        "X-Visivo-Selected-Count": str(result["count"]),
        "X-Visivo-Bbox": ",".join(str(v) for v in bbox),   # x0,x1,y0,y1,z0,z1
        "X-Visivo-Byte-Order": "little",
        "Content-Length": str(len(raw_bytes)),
    }
    return Response(content=raw_bytes, media_type="application/octet-stream",
                    headers=headers)


class CubeMaskSelectionRequest(CubeKinematicSelectRequest):
    """Apply a Kinematic Lasso selection to the cube (isolate / remove)."""
    mode: str = "blank_outside"   # "blank_outside" (isolate) | "blank_inside" (remove)
    output_basename: str = ""

    @model_validator(mode="after")
    def _check_mode2(self) -> "CubeMaskSelectionRequest":
        if self.mode not in ("blank_inside", "blank_outside"):
            raise ValueError("mode must be 'blank_inside' or 'blank_outside'")
        return self


@router.post("/v1/cube/mask_selection", response_model=CubeMaskResponse, tags=["cube"])
async def cube_mask_selection(
    request: CubeMaskSelectionRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeMaskResponse:
    """Blank inside/outside a Kinematic Lasso selection and register the masked
    cube as a new session dataset (persisted in Workspace Exports)."""
    from ..compute import worker_cube_mask_selection  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "3-D masking requires a spectral cube dataset.")
        src_stem = Path(entry["path"]).stem
        tag = "inside" if request.mode == "blank_inside" else "outside"
        basename = request.output_basename.strip() or f"{src_stem}_lasso-{tag}.fits"
        output_path = str(make_workspace_path(basename))

        seeds = [[request.seed_z, request.seed_y, request.seed_x], *[list(s) for s in request.add_seeds]]
        subs = [list(s) for s in request.sub_seeds]
        result = await _run_heavy_with_limit(
            session,
            worker_cube_mask_selection,
            entry["path"], output_path,
            seeds, subs,
            request.reach, request.lambda_v, request.sigma,
            request.roi_radius, request.nsigma, request.mode,
            request.reach_max if request.reach_max > 0.0 else None,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return CubeMaskResponse(valid=False, error=str(exc))

    new_id = f"ds_{uuid.uuid4().hex[:12]}"
    session.datasets[new_id] = {
        "path":   result["output_path"],
        "kind":   "cube",
        "width":  result["width"],
        "height": result["height"],
        "depth":  result["depth"],
    }
    workspace_filename = Path(result["output_path"]).name
    logger.info("[cube_mask_selection] new dataset new_id=%s mode=%s", new_id, request.mode)
    return CubeMaskResponse(
        valid=True, new_dataset_id=new_id, workspace_filename=workspace_filename,
        **{k: result[k] for k in
           ("output_path", "width", "height", "depth", "range_min", "range_max")})


@router.post("/v1/cube/mask_apply_bin", response_model=CubeMaskResponse, tags=["cube"])
async def cube_mask_apply_bin(
    request: Request,
    dataset_id: str = Query(...),
    origin_x: int = Query(...), origin_y: int = Query(...), origin_z: int = Query(...),
    width: int = Query(..., ge=1), height: int = Query(..., ge=1), depth: int = Query(..., ge=1),
    mode: str = Query("blank_outside"),
    output_basename: str = Query(""),
    # "key=value; key=value" — how the mask was made, written to HISTORY.
    recipe: str = Query("", max_length=1000),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeMaskResponse:
    """Apply an EXPLICIT selection mask (raw little-endian float32 ROI mask in the
    request body, + its origin/dims as query params — the same wire the select
    preview returns) and register the masked cube as a new dataset. Unlike
    /v1/cube/mask_selection this does NOT re-grow from seeds, so a hand-refined mask
    is applied exactly as shown."""
    from ..compute import worker_cube_mask_apply  # noqa: PLC0415
    if mode not in ("blank_inside", "blank_outside"):
        raise HTTPException(422, "mode must be 'blank_inside' or 'blank_outside'")
    mask_bytes = await request.body()
    expected = int(width) * int(height) * int(depth) * 4
    if len(mask_bytes) != expected:
        raise HTTPException(
            422, f"mask body is {len(mask_bytes)} bytes; expected {expected}")
    try:
        entry = _require_dataset(session, dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "3-D masking requires a spectral cube dataset.")
        src_stem = Path(entry["path"]).stem
        tag = "inside" if mode == "blank_inside" else "outside"
        basename = output_basename.strip() or f"{src_stem}_lasso-{tag}.fits"
        output_path = str(make_workspace_path(basename))
        result = await _run_heavy_with_limit(
            session,
            worker_cube_mask_apply,
            entry["path"], output_path, mask_bytes,
            origin_x, origin_y, origin_z, width, height, depth, mode, recipe,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return CubeMaskResponse(valid=False, error=str(exc))

    new_id = f"ds_{uuid.uuid4().hex[:12]}"
    session.datasets[new_id] = {
        "path":   result["output_path"],
        "kind":   "cube",
        "width":  result["width"],
        "height": result["height"],
        "depth":  result["depth"],
    }
    workspace_filename = Path(result["output_path"]).name
    logger.info("[cube_mask_apply_bin] new dataset new_id=%s mode=%s", new_id, mode)
    return CubeMaskResponse(
        valid=True, new_dataset_id=new_id, workspace_filename=workspace_filename,
        **{k: result[k] for k in
           ("output_path", "width", "height", "depth", "range_min", "range_max")})


# ── Kinematic fit of a selection ─────────────────────────────────────────────

@router.post("/v1/cube/kinematic_fit_bin", tags=["cube"])
async def cube_kinematic_fit_bin(
    request: Request,
    dataset_id: str = Query(...),
    origin_x: int = Query(...), origin_y: int = Query(...), origin_z: int = Query(...),
    width: int = Query(..., ge=1), height: int = Query(..., ge=1), depth: int = Query(..., ge=1),
    # The session's noise estimate; 0 = the cube's own 1.4826·MAD.
    sigma: float = Query(0.0, ge=0.0, allow_inf_nan=False),
    min_ring_pix: int = Query(150, ge=10),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> dict:
    """Measure and fit a SELECTION (the lasso's explicit mask in the body, as for
    mask_apply_bin) on the raw cube: moment maps, flux and widths, geometry from
    moment 0, a flat and a per-ring tilted-ring fit, model velocity fields and
    residuals — the comparator's input, so it can be checked, edited and drawn."""
    from ..compute.kinematic_fit import worker_kinematic_fit  # noqa: PLC0415
    from ..kinematic_model import ModelTooLargeError, max_voxels  # noqa: PLC0415
    mask_bytes = await request.body()
    expected = int(width) * int(height) * int(depth) * 4
    if len(mask_bytes) != expected:
        raise HTTPException(422, f"mask body is {len(mask_bytes)} bytes; expected {expected}")
    entry = _require_dataset(session, dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "A kinematic fit requires a spectral cube dataset.")
    try:
        return await _run_heavy_with_limit(
            session, worker_kinematic_fit, entry["path"], mask_bytes,
            origin_x, origin_y, origin_z, width, height, depth,
            float(sigma), int(min_ring_pix), max_voxels())
    except ModelTooLargeError as exc:
        raise HTTPException(413, str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc


@router.post("/v1/cube/kinematic_residual_bin", tags=["cube"])
async def cube_kinematic_residual_bin(
    request: Request,
    dataset_id: str = Query(...),
    origin_x: int = Query(...), origin_y: int = Query(...), origin_z: int = Query(...),
    width: int = Query(..., ge=1), height: int = Query(..., ge=1), depth: int = Query(..., ge=1),
    x0: float = Query(..., allow_inf_nan=False), y0: float = Query(..., allow_inf_nan=False),
    vsys: float = Query(..., allow_inf_nan=False),
    line_sigma: float = Query(..., gt=0.0, allow_inf_nan=False),
    pa: float | None = Query(None, allow_inf_nan=False),
    incl: float | None = Query(None, ge=0.0, le=90.0, allow_inf_nan=False),
    vrot: float | None = Query(None, allow_inf_nan=False),
    rmax: float | None = Query(None, gt=0.0, allow_inf_nan=False),
    # "r:pa:incl:vrot,r:pa:incl:vrot,…" — the ring table as it stands.
    rings: str = Query("", max_length=4000),
    sigma: float = Query(0.0, ge=0.0, allow_inf_nan=False),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> dict:
    """Residual of the model GIVEN by these parameters (fitted or edited) against
    the selection's velocity field — what the comparator reports for the model on
    screen. Same selection wire and noise rule as kinematic_fit_bin."""
    from ..compute.kinematic_fit import worker_kinematic_residual  # noqa: PLC0415
    from ..kinematic_model import ModelTooLargeError, max_voxels  # noqa: PLC0415
    mask_bytes = await request.body()
    expected = int(width) * int(height) * int(depth) * 4
    if len(mask_bytes) != expected:
        raise HTTPException(422, f"mask body is {len(mask_bytes)} bytes; expected {expected}")
    ring_list: list[tuple[float, float, float, float]] = []
    if rings.strip():
        try:
            for item in rings.split(","):
                r, p_, i_, v_ = (float(x) for x in item.split(":"))
                if not all(math.isfinite(x) for x in (r, p_, i_, v_)) or r <= 0 or not 0 <= i_ <= 90:
                    raise ValueError(item)
                ring_list.append((r, p_, i_, v_))
        except ValueError as exc:
            raise HTTPException(422, f"rings must be 'r:pa:incl:vrot,…' (bad item {exc})") from exc
        if len({r[0] for r in ring_list}) != len(ring_list):
            raise HTTPException(422, "ring radii must be distinct")
    elif None in (pa, incl, vrot, rmax):
        raise HTTPException(422, "a single-disc model needs pa, incl, vrot and rmax (or rings)")
    entry = _require_dataset(session, dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "A kinematic residual requires a spectral cube dataset.")
    try:
        return await _run_heavy_with_limit(
            session, worker_kinematic_residual, entry["path"], mask_bytes,
            origin_x, origin_y, origin_z, width, height, depth, float(sigma),
            x0, y0, vsys, line_sigma, pa, incl, vrot, rmax, ring_list or None, max_voxels())
    except ModelTooLargeError as exc:
        raise HTTPException(413, str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc


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
