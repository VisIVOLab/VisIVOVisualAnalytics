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

router = APIRouter(tags=['hips'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

@router.post("/v1/hips/open", response_model=HiPSSurveyResponse, tags=["hips"])
async def hips_open(
    request: HiPSOpenRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> HiPSSurveyResponse:
    """Register a remote HiPS survey URL.  Fetches the `properties` file and
    caches it.  Returns a survey_id for subsequent tile/allsky requests."""
    base_url = request.url.rstrip("/")
    props_url = f"{base_url}/properties"

    try:
        props_bytes = await _hips_fetch(props_url)
    except _urllib_error.HTTPError as exc:
        return HiPSSurveyResponse(valid=False,
                                  error=f"Cannot fetch HiPS properties ({exc.code}): {props_url}")
    except Exception as exc:
        return HiPSSurveyResponse(valid=False,
                                  error=f"Cannot fetch HiPS properties: {exc}")

    try:
        props = _parse_hips_properties(props_bytes.decode("utf-8", errors="replace"))
    except Exception as exc:
        return HiPSSurveyResponse(valid=False,
                                  error=f"Cannot parse HiPS properties: {exc}")

    title = (props.get("obs_title")
             or props.get("obs_collection")
             or props.get("creator_did")
             or "Unknown Survey")
    frame_raw = props.get("hips_frame", "equatorial").lower()
    if "galactic" in frame_raw:
        frame = "galactic"
    elif "ecliptic" in frame_raw:
        frame = "ecliptic"
    else:
        frame = "equatorial"

    try:
        max_order = int(props.get("hips_order", "9"))
        min_order = int(props.get("hips_order_min", "0"))
        tile_width = int(props.get("hips_tile_width", "512"))
    except ValueError:
        max_order, min_order, tile_width = 9, 0, 512

    formats = _best_tile_formats(props)

    survey_id = f"hips_{uuid.uuid4().hex[:12]}"
    session.datasets[survey_id] = {
        "kind": "hips",
        "base_url": base_url,
        "title": title,
        "frame": frame,
        "max_order": max_order,
        "min_order": min_order,
        "tile_width": tile_width,
        "tile_formats": formats,
        "properties": props,
    }
    logger.info("[hips-open] session=%s survey_id=%s title=%s max_order=%d frame=%s",
                session.session_id, survey_id, title, max_order, frame)

    return HiPSSurveyResponse(
        valid=True, error="",
        survey_id=survey_id, session_id=session.session_id,
        base_url=base_url, title=title, frame=frame,
        max_order=max_order, min_order=min_order,
        tile_width=tile_width, tile_formats=formats,
    )

@router.get("/v1/hips/{survey_id}/allsky", tags=["hips"])
async def hips_allsky(
    survey_id: str,
    order: int = 3,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Return the AllSky mosaic image for the given order.
    The image is fetched once from the remote survey and cached on disk."""
    try:
        entry = _require_dataset(session, survey_id)
        if entry["kind"] != "hips":
            raise HTTPException(422, "Not a HiPS survey.")
    except HTTPException:
        raise

    base_url: str = entry["base_url"]
    formats: list[str] = entry["tile_formats"]
    fmt = formats[0] if formats else "png"

    cache_path = _allsky_cache_path(survey_id, order, fmt)
    if cache_path.exists():
        data = cache_path.read_bytes()
    else:
        # Try each format until one works
        data = b""
        last_err: Exception | None = None
        for candidate_fmt in (formats or ["png", "jpeg"]):
            url = _allsky_url(base_url, order, candidate_fmt)
            try:
                data = await _hips_fetch(url)
                fmt = candidate_fmt
                break
            except Exception as exc:
                last_err = exc
        if not data:
            raise HTTPException(404, f"AllSky not available at order {order}: {last_err}")

        cache_path.parent.mkdir(parents=True, exist_ok=True)
        cache_path.write_bytes(data)
        _maybe_cleanup_hips_cache()  # throttled TTL purge + size eviction

    mt = ("image/jpeg" if fmt in ("jpeg", "jpg")
          else "image/fits" if fmt == "fits"
          else "image/png")
    return Response(content=data, media_type=mt)

@router.get("/v1/hips/{survey_id}/tile/{order}/{pix}", tags=["hips"])
async def hips_tile(
    survey_id: str,
    order: int,
    pix: int,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Return a single HiPS tile image (PNG/JPEG).  Caches tiles on disk."""
    try:
        entry = _require_dataset(session, survey_id)
        if entry["kind"] != "hips":
            raise HTTPException(422, "Not a HiPS survey.")
    except HTTPException:
        raise

    formats: list[str] = entry["tile_formats"]
    base_url: str = entry["base_url"]
    fmt = formats[0] if formats else "png"

    cache_path = _tile_cache_path(survey_id, order, pix, fmt)
    if cache_path.exists():
        data = cache_path.read_bytes()
    else:
        data = b""
        last_err: Exception | None = None
        for candidate_fmt in (formats or ["png", "jpeg"]):
            url = _tile_url(base_url, order, pix, candidate_fmt)
            try:
                data = await _hips_fetch(url)
                fmt = candidate_fmt
                break
            except Exception as exc:
                last_err = exc
        if not data:
            raise HTTPException(404, f"Tile order={order} pix={pix} not available: {last_err}")

        cache_path.parent.mkdir(parents=True, exist_ok=True)
        cache_path.write_bytes(data)
        _maybe_cleanup_hips_cache()  # throttled TTL purge + size eviction

    mt = ("image/jpeg" if fmt in ("jpeg", "jpg")
          else "image/fits" if fmt == "fits"
          else "image/png")
    return Response(content=data, media_type=mt)

@router.post("/v1/hips/{survey_id}/query_tiles",
          response_model=HiPSViewResponse, tags=["hips"])
async def hips_query_tiles(
    survey_id: str,
    request: HiPSViewRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> HiPSViewResponse:
    """Return the set of HEALPix tiles that cover the given viewport.
    Requires healpy.  Falls back to an empty list with an error if unavailable."""
    try:
        entry = _require_dataset(session, survey_id)
        if entry["kind"] != "hips":
            raise HTTPException(422, "Not a HiPS survey.")
    except HTTPException:
        raise

    if not _HEALPY_AVAILABLE:
        return HiPSViewResponse(
            valid=False,
            error="healpy is not installed on the backend – tile queries unavailable. "
                  "Install it with: pip install healpy",
        )

    max_order: int = entry["max_order"]
    order = max(0, min(request.order, max_order))
    try:
        loop = _asyncio.get_event_loop()
        tiles_raw = await loop.run_in_executor(
            None, _get_tiles_for_fov,
            request.ra_center, request.dec_center, request.fov_deg, order,
        )
    except Exception as exc:
        return HiPSViewResponse(valid=False, error=f"HEALPix query failed: {exc}")

    return HiPSViewResponse(
        valid=True,
        tiles=[HiPSTileInfo(**t) for t in tiles_raw],
    )

@router.post("/v1/hips/catalogue_overlay",
          response_model=CatalogueOverlayResponse, tags=["hips"])
async def hips_catalogue_overlay(
    request: CatalogueOverlayRequest,
    _: None = _auth,
) -> CatalogueOverlayResponse:
    """Return catalogue sources (RA/Dec) visible in the given HiPS viewport.
    Reads a CSV file at `catalogue_path` on the backend filesystem."""
    loop = _asyncio.get_event_loop()
    d = await loop.run_in_executor(
        None, _catalogue_overlay_sync,
        request.catalogue_path, request.ra_center, request.dec_center,
        request.fov_deg, request.ra_field, request.dec_field, request.max_sources,
    )
    if not d["valid"]:
        return CatalogueOverlayResponse(valid=False, error=d["error"])
    return CatalogueOverlayResponse(
        valid=True,
        sources=[CatalogueOverlaySource(**s) for s in d["sources"]],
        total_in_view=d["total_in_view"],
        truncated=d["truncated"],
    )


# ── hips2fits: a FITS cut out of any HiPS survey ─────────────────────────────

class Hips2FitsRequest(BaseModel):
    hips: str
    width: int = 512
    height: int = 512
    ra: float = 0.0
    dec: float = 0.0
    fov: float = 1.0
    projection: str = "TAN"
    coordsys: str = "icrs"
    rotation_angle: float = 0.0
    format: str = "fits"
    #: Percentile cuts and stretch — only meaningful for jpg/png.
    min_cut: float = 0.5
    max_cut: float = 99.5
    stretch: str = "linear"
    output_basename: str = ""


class Hips2FitsResponse(BaseModel):
    valid: bool
    error: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    bytes: int = 0
    is_fits: bool = False
    #: The full request URL, so what was asked for is reproducible outside the app.
    url: str = ""


class Hips2FitsSurveysResponse(BaseModel):
    surveys: list[str]
    projections: list[str]
    stretches: list[str]
    endpoint: str


@router.get("/v1/hips/hips2fits/surveys", response_model=Hips2FitsSurveysResponse, tags=["hips"])
async def hips2fits_surveys(_: None = _auth) -> Hips2FitsSurveysResponse:
    """The starting list of surveys and the accepted enumerations.

    The legacy carried these as a QStringList inside the dialog, so correcting a
    survey id meant releasing a client.
    """
    from .. import hips2fits  # noqa: PLC0415
    return Hips2FitsSurveysResponse(
        surveys=hips2fits.known_surveys(),
        projections=list(hips2fits.PROJECTIONS),
        stretches=list(hips2fits.STRETCHES),
        endpoint=hips2fits.endpoint(),
    )


@router.post("/v1/hips/hips2fits", response_model=Hips2FitsResponse, tags=["hips"])
async def hips2fits_cutout(request: Hips2FitsRequest, _: None = _auth) -> Hips2FitsResponse:
    """Cut a FITS (or a picture) out of a HiPS survey, into the workspace."""
    from .. import hips2fits  # noqa: PLC0415

    try:
        params = hips2fits.build_query(
            request.hips, request.width, request.height, request.ra, request.dec,
            request.fov, request.projection, request.coordsys, request.rotation_angle,
            request.format, request.min_cut, request.max_cut, request.stretch)
    except ValueError as exc:
        # A parameter the service would have refused: say so without the round trip.
        return Hips2FitsResponse(valid=False, error=str(exc))

    basename = request.output_basename.strip() or hips2fits.default_basename(
        request.hips, request.format)
    output_path = str(make_workspace_path(basename))
    try:
        # A network fetch: a thread, not a compute worker (see _run_io).
        result = await _run_io(hips2fits.fetch_cutout, params, output_path)
    except ValueError as exc:
        return Hips2FitsResponse(valid=False, error=str(exc))
    except Exception as exc:
        # An unforeseen failure: logged in full, reported without the
        # server-side detail (a path, a mount point) that str(exc) carries.
        logger.exception("[hips2fits] failed hips=%s", request.hips)
        return Hips2FitsResponse(
            valid=False, error="The cutout could not be completed. See the backend log.")

    return Hips2FitsResponse(
        valid=True, workspace_filename=Path(result["output_path"]).name, **result)
