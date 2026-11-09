"""Region interchange endpoints — import/export CRTF (CASA) and DS9 regions.

Interop, not a new analysis capability: regions drawn in CARTA/CASA/ds9 can be
loaded into VisIVO and vice-versa. Parsing/serialising lives in ``app.regions``;
this router adds the HTTP surface and the optional frame conversion that turns
a sky-frame CARTA region into the pixel coordinates the desktop overlay draws
in (and back), or moves it to another sky frame.

  * POST /v1/regions/parse   {format?, text, dataset_id?, to_frame?}
  * POST /v1/regions/export  {format, regions, dataset_id?, to_frame?}
"""
import logging
from typing import Any

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel

from .. import regions as R
from ..auth import verify_token
from ..dependencies import _require_dataset
from ..sessions import Session, get_session

router = APIRouter(tags=["regions"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)


class RegionParseRequest(BaseModel):
    text: str
    format: str = "auto"          # auto | crtf | ds9
    dataset_id: str = ""          # needed only when to_frame is set
    to_frame: str = ""            # "" = keep native; else "image" | world frame


class RegionExportRequest(BaseModel):
    regions: list[dict[str, Any]]
    format: str                   # crtf | ds9
    dataset_id: str = ""
    to_frame: str = ""


class RegionStatsRequest(BaseModel):
    region: dict[str, Any]        # canonical region dict
    channel: int | None = None    # for cubes; defaults to 0


def _celestial_wcs(path: str):
    """The dataset's 2-D celestial WCS, or 422 when it has none."""
    from astropy.io import fits  # noqa: PLC0415,F401
    from astropy.wcs import WCS  # noqa: PLC0415

    from ..fits_dataset import image_hdu_index  # noqa: PLC0415

    with _vs_open_fits(path) as hdul:
        header = hdul[image_hdu_index(hdul)].header
    wcs = WCS(header)
    if not wcs.has_celestial:
        raise HTTPException(422, "Dataset has no celestial WCS; cannot convert region frames.")
    return wcs.celestial


def _convert(regions, to_frame, wcs, origin):
    try:
        return R.convert_regions(regions, to_frame=to_frame, wcs=wcs, origin=origin)
    except (R.RegionParseError, ValueError) as exc:
        raise HTTPException(422, str(exc)) from exc


def _maybe_convert(regions, to_frame, session, dataset_id):
    """Reframe regions for parse/export (1-based pixels, as in region files).

    A sky-to-sky conversion needs no dataset; anything touching pixels, or a
    ``to_frame`` that names no sky frame (meaning "the dataset's own"), does.
    """
    if not to_frame:
        return regions
    wcs = None
    if dataset_id:
        entry = _require_dataset(session, dataset_id)
        wcs = _celestial_wcs(entry["path"])
    if to_frame != "image" and to_frame not in R._WORLD_FRAMES:
        if wcs is None:
            raise HTTPException(422, "to_frame requires dataset_id for the WCS conversion.")
        try:
            to_frame = R.native_frame_name(wcs)
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
    return _convert(regions, to_frame, wcs, origin=1)


@router.post("/v1/regions/parse", tags=["regions"])
async def parse_regions(
    request: RegionParseRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Parse CRTF/DS9 text into canonical region dicts (optionally reframed)."""
    try:
        fmt = R.detect_format(request.text) if request.format == "auto" else request.format
        regions = R.parse_regions(request.text, request.format)
    except R.RegionParseError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[regions/parse] failed: %s", exc)
        raise HTTPException(500, str(exc)) from exc
    regions = _maybe_convert(regions, request.to_frame, session, request.dataset_id)
    return {"valid": True, "error": "", "format": fmt, "n_regions": len(regions), "regions": regions}


@router.post("/v1/regions/export", tags=["regions"])
async def export_regions(
    request: RegionExportRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Serialize canonical region dicts to CRTF or DS9 text (optionally reframed)."""
    regions = _maybe_convert(request.regions, request.to_frame, session, request.dataset_id)
    try:
        text = R.export_regions(regions, request.format)
    except R.RegionParseError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[regions/export] failed: %s", exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", "format": request.format, "text": text}


@router.post("/v1/regions/stats/{dataset_id}", tags=["regions"])
async def region_stats(
    dataset_id: str,
    request: RegionStatsRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Pixel statistics (mean/rms/sum/min/max/median/flux) inside a region.

    The region may be supplied in the ``image`` (0-based pixel) frame or in a
    world frame — world regions are converted to pixels via the dataset WCS
    before the statistics are computed.
    """
    entry = _require_dataset(session, dataset_id)
    region = request.region
    if region.get("frame", "image") != "image" or region.get("unit", "pix") != "pix":
        region = _convert([region], "image", _celestial_wcs(entry["path"]), origin=0)[0]
    from ..compute import worker_region_stats  # noqa: PLC0415
    import asyncio  # noqa: PLC0415
    try:
        result = await asyncio.to_thread(worker_region_stats, entry["path"], region, request.channel)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[regions/stats] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", **result}


def _vs_open_fits(path, **kwargs):
    # A FITS file, or a field of a VBT volume read in place (issue #83).
    from app.volume_source import open_fits  # noqa: PLC0415
    return open_fits(path, **kwargs)
