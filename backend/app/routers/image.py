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
from ..image_pyramid import tile_source

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
        # Interactive path (not _run_with_limit): the desktop assembles a mosaic
        # as a burst of parallel tile POSTs, so counting each against the
        # per-session task quota (VISIVO_MAX_CONCURRENT_TASKS, default 3) would
        # 429 most of the burst. Tile reads are bounded strided memmap reads
        # (I/O-bound, not heavy compute), so they run on the interactive pool —
        # same rationale as the binary subvolume endpoint.
        # A large image is served from its averaged pyramid once built; the
        # first tile starts the build, and meanwhile tiles come from the file
        # (issue #85).
        source = await asyncio.to_thread(tile_source, str(entry["path"]))
        # From the pyramid, in this machine's pool: it lies in this backend's
        # pyramid directory, which Dask workers elsewhere may not see.
        run = _run_interactive if source.pyramid else _run
        result = await run(
            worker_image_tile, entry["path"],
            request.level, request.tile_x, request.tile_y, request.tile_size,
            source.pyramid,
        )
    except HTTPException:
        raise
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:
        return ImageTileResponse(valid=False, error=str(exc))
    return ImageTileResponse(valid=True, error="", pyramid=source.state,
                             pyramid_progress=round(source.progress, 3), **result)


# ── Filter FITS: smooth and/or regrid a file into the workspace ──────────────
# The legacy's *Filter FITS* dialog, moved to the backend so the arithmetic is
# the same for every client and can be tested without a display. What it does
# differently is in app/compute/imutils.py: blanks stay blank, the beam grows
# with the kernel, and a per-pixel unit is summed rather than averaged.

class FilterFitsRequest(BaseModel):
    """Either action may be omitted, but not both."""
    path: str
    #: Gaussian FWHM in pixels; 0 = no smoothing.
    smooth_fwhm_pixels: float = 0.0
    #: Integer shrink factor; 1 = no regrid.
    regrid_factor: int = 1
    #: "auto" reads BUNIT: per-pixel units are summed, surface brightness
    #: averaged. "mean"/"sum" force it.
    combine: str = "auto"
    #: Workspace Exports filename (basename only). Empty → derived from the input.
    output_basename: str = ""


class FilterFitsResponse(BaseModel):
    valid: bool
    error: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    width: int = 0
    height: int = 0
    depth: int = 1
    range_min: float = 0.0
    range_max: float = 0.0
    bunit: str = ""
    combine: str = ""
    smooth_fwhm_pixels: float = 0.0
    smooth_fwhm_arcsec: float = 0.0
    regrid_factor: int = 1
    #: 1.0 unless the beam grew under a per-beam unit, in which case the pixels
    #: were scaled with it so the integrated flux is unchanged.
    flux_scale: float = 1.0
    #: The unit is per beam, the map was smoothed, and the header carries no
    #: beam — so the values could not be rescaled and are no longer comparable.
    beam_missing: bool = False


def _filtered_basename(source: Path, smooth: float, factor: int) -> str:
    """Name the output after what was done to it, as the legacy did — but only
    for the steps that were actually taken (it appended `_smooth` whether or not
    the box was ticked, because it asked `isCheckable()` instead of
    `isChecked()`)."""
    stem = source.stem
    if smooth > 0:
        stem += "_smooth"
    if factor > 1:
        stem += f"_regrid{factor}"
    return f"{stem}.fits"


@router.post("/v1/image/filter", response_model=FilterFitsResponse, tags=["image"])
async def filter_fits(
    request: FilterFitsRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> FilterFitsResponse:
    """Smooth and/or shrink a FITS file, writing a new one in the workspace."""
    from ..compute.imutils import worker_filter_fits  # noqa: PLC0415
    try:
        source = _normalize_path(request.path)
        if not source.is_file():
            return FilterFitsResponse(valid=False, error="File not found.")
        basename = request.output_basename.strip() or _filtered_basename(
            source, float(request.smooth_fwhm_pixels), int(request.regrid_factor))
        output_path = str(make_workspace_path(basename))
        result = await _run_heavy_with_limit(
            session,
            worker_filter_fits,
            str(source),
            output_path,
            float(request.smooth_fwhm_pixels),
            int(request.regrid_factor),
            str(request.combine),
        )
    except HTTPException:
        raise
    except PermissionError:
        # Same answer as a missing file: the refusal must not confirm the path.
        return FilterFitsResponse(valid=False, error="File not found.")
    except ValueError as exc:
        # Deliberate refusals from the worker — "nothing to do", a factor that
        # leaves nothing of the image — are written for the user.
        return FilterFitsResponse(valid=False, error=str(exc))
    except Exception:
        logger.exception("[filter] failed path=%s", request.path)
        return FilterFitsResponse(
            valid=False, error="The filter could not be completed. See the backend log.")
    return FilterFitsResponse(
        valid=True, workspace_filename=Path(result["output_path"]).name, **result)
