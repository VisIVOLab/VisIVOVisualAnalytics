"""Region interchange endpoints — import/export CRTF (CASA) and DS9 regions.

Interop, not a new analysis capability: regions drawn in CARTA/CASA/ds9 can be
loaded into VisIVO and vice-versa. Parsing/serialising lives in ``app.regions``;
this router adds the HTTP surface and the optional pixel↔world conversion that
turns a sky-frame CARTA region into the pixel coordinates the desktop overlay
draws in (and back).

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


def _wcs_transforms(path: str, origin: int = 1):
    """Build (sky_to_pix, pix_to_sky, pixel_scale_arcsec, world_frame) from a
    dataset's celestial WCS. ``origin`` is the pixel-coordinate base: 1 for the
    FITS/ds9/CRTF file convention (parse/export), 0 for numpy-indexed analysis
    (region statistics).
    """
    from astropy.io import fits  # noqa: PLC0415
    from astropy.wcs import WCS  # noqa: PLC0415
    from astropy.wcs.utils import proj_plane_pixel_scales  # noqa: PLC0415

    with fits.open(path) as hdul:
        header = hdul[0].header
    wcs = WCS(header)
    if not wcs.has_celestial:
        raise HTTPException(422, "Dataset has no celestial WCS; cannot convert region frames.")
    cel = wcs.celestial
    scales_deg = proj_plane_pixel_scales(cel)
    pixel_scale_arcsec = float(sum(scales_deg) / len(scales_deg) * 3600.0)
    frame_name = "fk5"
    radesys = str(header.get("RADESYS", "")).lower()
    if "icrs" in radesys:
        frame_name = "icrs"
    elif "gal" in str(header.get("CTYPE1", "")).lower():
        frame_name = "galactic"

    def sky_to_pix(ra_deg, dec_deg):
        x, y = cel.all_world2pix(ra_deg, dec_deg, origin)
        return float(x), float(y)

    def pix_to_sky(x, y):
        ra, dec = cel.all_pix2world(x, y, origin)
        return float(ra), float(dec)

    return sky_to_pix, pix_to_sky, pixel_scale_arcsec, frame_name


def _maybe_convert(regions, to_frame, session, dataset_id):
    if not to_frame:
        return regions
    if not dataset_id:
        raise HTTPException(422, "to_frame requires dataset_id for the WCS conversion.")
    entry = _require_dataset(session, dataset_id)
    sky_to_pix, pix_to_sky, scale, world_frame = _wcs_transforms(entry["path"])
    target = "image" if to_frame == "image" else (to_frame if to_frame in R._WORLD_FRAMES else world_frame)
    return R.convert_regions(
        regions, to_frame=target, sky_to_pix=sky_to_pix, pix_to_sky=pix_to_sky,
        pixel_scale_arcsec=scale, world_frame=world_frame,
    )


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
    if region.get("frame", "image") != "image":
        sky_to_pix, pix_to_sky, scale, world_frame = _wcs_transforms(entry["path"], origin=0)
        region = R.convert_regions(
            [region], to_frame="image", sky_to_pix=sky_to_pix, pix_to_sky=pix_to_sky,
            pixel_scale_arcsec=scale, world_frame=world_frame)[0]
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
