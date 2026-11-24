"""Region interchange endpoints — import/export CRTF (CASA) and DS9 regions.

Interop, not a new analysis capability: regions drawn in CARTA/CASA/ds9 can be
loaded into VisIVO and vice-versa. Parsing/serialising lives in ``app.regions``;
this router adds the HTTP surface and the optional frame conversion that turns
a sky-frame CARTA region into the pixel coordinates the desktop overlay draws
in (and back), or moves it to another sky frame.

  * POST /v1/regions/parse   {format?, text, dataset_id?, to_frame?}
  * POST /v1/regions/export  {format, regions, dataset_id?, to_frame?}
  * POST /v1/regions/convert {regions, to_frame, dataset_id?, origin?, unit?}
  * POST /v1/regions/stats/{dataset_id}    {region, channel?}
  * POST /v1/regions/profile/{dataset_id}  {region (a line), channel?, width?}
  * POST /v1/regions/noise/{dataset_id}    {regions?, channel?, clip_sigma?}
"""
import asyncio
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


class RegionConvertRequest(BaseModel):
    regions: list[dict[str, Any]] = []
    to_frame: str                 # "image" | a sky frame | "native" (the dataset's own)
    dataset_id: str = ""          # needed whenever pixels are involved
    origin: int = 1               # pixel base of image-frame regions, in and out
    unit: str = ""                # sizes of the result; "" = pix in image, else as converted


class RegionStatsRequest(BaseModel):
    region: dict[str, Any]        # canonical region dict
    channel: int | None = None    # for cubes; defaults to 0


class RegionProfileRequest(BaseModel):
    region: dict[str, Any]        # a canonical ``line`` region (two or more points)
    channel: int | None = None    # for cubes; defaults to 0
    width: int = 1                # sampling width across the line, pixels


class ImageNoiseRequest(BaseModel):
    regions: list[dict[str, Any]] = []   # background regions; none = the whole plane, clipped
    channel: int | None = None           # for cubes; defaults to 0
    clip_sigma: float = 3.0              # the clip of the automatic estimate, in σ


#: Diagnostics returned by one parse; a file of garbage would otherwise echo
#: every line. ``n_skipped`` always counts them all.
_MAX_DIAGNOSTICS = 1000


def _dataset_header(path: str):
    """The header of the dataset's image HDU (a FITS file, or a VBT field)."""
    from ..fits_dataset import image_hdu_index  # noqa: PLC0415

    with _vs_open_fits(path) as hdul:
        return hdul[image_hdu_index(hdul)].header.copy()


def _celestial_from_header(header):
    """The 2-D celestial WCS of a header (an image or a cube's sky axes), or
    None. A 3-D header with SIP terms makes astropy refuse the full WCS; its
    first two axes are read then."""
    from astropy.wcs import WCS  # noqa: PLC0415

    try:
        wcs = WCS(header)
    except Exception:  # noqa: BLE001
        try:
            wcs = WCS(header, naxis=2)
        except Exception:  # noqa: BLE001
            return None
    return wcs.celestial if wcs.has_celestial else None


def _celestial_wcs(path: str):
    """The dataset's 2-D celestial WCS, or 422 when it has none."""
    wcs = _celestial_from_header(_dataset_header(path))
    if wcs is None:
        raise HTTPException(422, "Dataset has no celestial WCS; cannot convert region frames.")
    return wcs


def _celestial_wcs_or_none(path: str):
    """The dataset's 2-D celestial WCS, or None when it has none (or no FITS
    header to read one from: a catalogue or a VBT field)."""
    try:
        return _celestial_wcs(path)
    except Exception:  # noqa: BLE001
        return None


def _convert(regions, to_frame, wcs, origin):
    try:
        return R.convert_regions(regions, to_frame=to_frame, wcs=wcs, origin=origin)
    except (R.RegionParseError, ValueError) as exc:
        raise HTTPException(422, str(exc)) from exc


def _resolve_to_frame(to_frame: str, wcs) -> str:
    """``image``, a sky frame, or (any other value) the dataset's own sky frame."""
    if to_frame == "image" or to_frame in R._WORLD_FRAMES:
        return to_frame
    if wcs is None:
        raise HTTPException(422, "to_frame requires a dataset_id with a celestial WCS.")
    try:
        return R.native_frame_name(wcs)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc


def _maybe_convert(regions, to_frame, session, dataset_id):
    """Reframe regions for export (1-based pixels, as in region files).

    A sky-to-sky conversion needs no dataset; anything touching pixels, or a
    ``to_frame`` that names no sky frame (meaning "the dataset's own"), does.
    """
    if not to_frame:
        return regions
    wcs = None
    if dataset_id:
        entry = _require_dataset(session, dataset_id)
        wcs = _celestial_wcs(entry["path"])
    return _convert(regions, _resolve_to_frame(to_frame, wcs), wcs, origin=1)


@router.post("/v1/regions/parse", tags=["regions"])
async def parse_regions(
    request: RegionParseRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Parse CRTF/DS9 text into canonical region dicts (optionally reframed).

    Every statement that does not become a region comes back in
    ``diagnostics`` with its line and the reason; the regions that did are
    returned either way.
    """
    if request.format.lower() not in ("auto", "crtf", "ds9"):
        raise HTTPException(422, f"Unknown region format '{request.format}'.")
    path = _require_dataset(session, request.dataset_id)["path"] if request.dataset_id else ""

    def work():
        # Off the event loop: reading the header and converting a file of
        # thousands of regions both take time.
        wcs = physical = None
        no_wcs_reason = ""
        if path:
            header = _dataset_header(path)
            physical = R.PhysicalTransform.from_header(header)
            wcs = _celestial_from_header(header)
            if wcs is None:
                no_wcs_reason = "the dataset has no celestial WCS, so a sky region cannot be placed on it"
        to_frame = _resolve_to_frame(request.to_frame, wcs) if request.to_frame else ""
        return R.parse_report(request.text, request.format, to_frame=to_frame, wcs=wcs, origin=1,
                              physical=physical, no_wcs_reason=no_wcs_reason)

    try:
        rep = await asyncio.to_thread(work)
    except HTTPException:
        raise
    except R.RegionParseError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[regions/parse] failed: %s", exc)
        raise HTTPException(500, str(exc)) from exc
    diags = rep["diagnostics"]
    # Nothing read but something reported: say why in ``error`` too, for a
    # client that shows only that.
    valid = bool(rep["regions"]) or not diags
    error = "" if valid else (f"No region could be read. Line {diags[0]['line']}: {diags[0]['reason']}"
                              + (f" (and {len(diags) - 1} more)" if len(diags) > 1 else ""))
    return {"valid": valid, "error": error, "format": rep["format"],
            "n_regions": len(rep["regions"]), "n_skipped": len(diags),
            "diagnostics": diags[:_MAX_DIAGNOSTICS],
            "diagnostics_truncated": len(diags) > _MAX_DIAGNOSTICS,
            "regions": rep["regions"]}


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


@router.post("/v1/regions/convert", tags=["regions"])
async def convert_regions(
    request: RegionConvertRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Convert canonical regions between frames with the dataset's WCS.

    What a region properties editor needs: the region the viewer holds (image
    frame, ``origin``-based pixels) shown in the frame the user picked, and
    the numbers typed in that frame brought back to pixels. With no regions
    it only reports the dataset's sky frame (``native_frame``), so a client
    can tell which frames to offer: an empty ``native_frame`` means the
    dataset has no celestial WCS and only pixels make sense.
    """
    if request.origin not in (0, 1):
        raise HTTPException(422, "origin must be 0 or 1.")
    unit = request.unit
    if unit and unit != "pix" and unit not in R._ANGLE_TO_DEG:
        raise HTTPException(422, f"Unknown size unit '{unit}'.")
    to_frame = request.to_frame
    if to_frame not in ("image", "native") and to_frame not in R._WORLD_FRAMES:
        raise HTTPException(422, f"Unknown region frame '{to_frame}'.")
    wcs = None
    native = ""
    if request.dataset_id:
        entry = _require_dataset(session, request.dataset_id)
        wcs = _celestial_wcs_or_none(entry["path"])
        if wcs is not None:
            try:
                native = R.native_frame_name(wcs)
            except ValueError:   # a celestial WCS astropy cannot name a frame for
                wcs = None
    if to_frame == "native":
        if not native:
            raise HTTPException(422, "Dataset has no celestial WCS; cannot convert region frames.")
        to_frame = native
    if to_frame == "image" and unit and unit != "pix":
        raise HTTPException(422, "Image-frame regions are converted to pixel sizes.")
    if not request.regions:
        return {"valid": True, "error": "", "frame": to_frame, "native_frame": native, "regions": []}
    def _needs_wcs(r: dict[str, Any]) -> bool:
        src = r.get("frame", "image")
        if (src == "image") != (to_frame == "image"):
            return True   # pixels <-> sky
        # pixel sizes on the sky, or angular sizes on pixels
        return src == "image" and r.get("unit", "pix") != "pix"

    if wcs is None and any(_needs_wcs(r) for r in request.regions):
        if not request.dataset_id:
            raise HTTPException(422, "Converting between pixels and sky needs dataset_id.")
        raise HTTPException(422, "Dataset has no celestial WCS; cannot convert region frames.")
    try:
        regions = _convert(request.regions, to_frame, wcs, origin=request.origin)
    except (KeyError, IndexError, TypeError) as exc:   # a malformed region dict
        raise HTTPException(422, f"Malformed region: {exc!r}") from exc
    if unit and to_frame != "image":
        # On the sky, convert_regions has already measured sizes along each
        # region's own axes in an angle unit: only that unit changes here.
        try:
            for r in regions:
                R.rescale_angle_units(r, unit)
        except (R.RegionParseError, ValueError) as exc:
            raise HTTPException(422, str(exc)) from exc
    return {"valid": True, "error": "", "frame": to_frame, "native_frame": native, "regions": regions}


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
    try:
        result = await asyncio.to_thread(worker_region_stats, entry["path"], region, request.channel)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[regions/stats] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc
    result.update(_peak_world(entry["path"], result.get("peak_pixel")))
    return {"valid": True, "error": "", **result}


def _peak_world(path: str, pixel) -> dict[str, Any]:
    """``peak_world``: the sky position of the peak pixel (0-based ``[x, y]``)
    in the dataset's own frame, degrees; None without a celestial WCS."""
    out: dict[str, Any] = {"peak_world": None}
    wcs = _celestial_wcs_or_none(path) if pixel else None
    if wcs is None:
        return out
    try:
        lon, lat = (float(v) for v in wcs.pixel_to_world_values(pixel[0], pixel[1]))
        frame = R.native_frame_name(wcs)
    except (ValueError, TypeError):
        return out
    if lon == lon and lat == lat:
        out["peak_world"] = {"lon": lon % 360.0, "lat": lat, "frame": frame}
    return out


@router.post("/v1/regions/profile/{dataset_id}", tags=["regions"])
async def region_profile(
    dataset_id: str,
    request: RegionProfileRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """The profile along a ``line`` region: the value at samples one pixel
    apart (bilinear, ``width`` pixels wide) against the offset from the start
    in arcsec (pixels when the dataset has no celestial WCS). Works on an image
    or on one channel of a cube, read in small windows along the line."""
    entry = _require_dataset(session, dataset_id)
    region = request.region
    if str(region.get("shape", "")).lower() != "line":
        raise HTTPException(422, "A profile needs a line region.")
    wcs = _celestial_wcs_or_none(entry["path"])
    if region.get("frame", "image") != "image" or region.get("unit", "pix") != "pix":
        region = _convert([region], "image", _celestial_wcs(entry["path"]), origin=0)[0]
    from ..fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    from ..line_profile import line_profile  # noqa: PLC0415

    def run():
        dataset = ScientificFitsDataset(entry["path"])
        from ..brick_pyramid import source_shape  # noqa: PLC0415
        nz = source_shape(entry["path"], dataset.hdu_index)[0]
        ch = 0 if request.channel is None else max(0, min(int(request.channel), nz - 1))
        points = [(float(x), float(y)) for x, y in region.get("points", ())]
        out = line_profile(entry["path"], dataset.hdu_index, ch, points, request.width, wcs)
        out.update({"channel": ch, "bunit": dataset.intensity_unit})
        return out

    try:
        result = await asyncio.to_thread(run)
    except (ValueError, TypeError, KeyError, IndexError) as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[regions/profile] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", **result}


@router.post("/v1/regions/noise/{dataset_id}", tags=["regions"])
async def image_noise(
    dataset_id: str,
    request: ImageNoiseRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """The noise of an image (or one channel of a cube) over its source-free
    pixels: RMS √⟨x²⟩ and σ(MAD), averaged over the background ``regions``
    given (each region's own values listed) or, with none, sigma-clipped over
    the whole plane in blocks. A Jy/beam image also gets the noise per pixel
    and the beam area in pixels."""
    entry = _require_dataset(session, dataset_id)
    wcs = None
    regions = []
    for r in request.regions:
        if r.get("frame", "image") != "image" or r.get("unit", "pix") != "pix":
            wcs = wcs or _celestial_wcs(entry["path"])
            r = _convert([r], "image", wcs, origin=0)[0]
        regions.append(r)
    from ..compute import worker_image_noise  # noqa: PLC0415
    try:
        result = await asyncio.to_thread(
            worker_image_noise, entry["path"], regions, request.channel, request.clip_sigma)
    except (ValueError, KeyError, TypeError, IndexError) as exc:   # incl. a malformed region
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[regions/noise] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", **result}


def _vs_open_fits(path, **kwargs):
    # A FITS file, or a field of a VBT volume read in place (issue #83).
    from app.volume_source import open_fits  # noqa: PLC0415
    return open_fits(path, **kwargs)
