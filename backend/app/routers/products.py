from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel, Field
from typing import Any, Literal
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
    # Waits briefly for a slot rather than refusing on the spot — see
    # dependencies.acquire_task_slot for why an instant 429 turned a single
    # click into a hard failure on a large cube.
    if not await acquire_task_slot(session):
        raise task_slot_busy_error()
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
            "spectral_quantity": request.spectral_quantity,
            "spectral_frame": request.spectral_frame,
            "rest_freq_hz": request.rest_freq_hz,
        },
    )
    return MomentProductResponse(valid=True, error="", **result)


# ── Persist a moment map to a standalone 2-D FITS file ─────────────────────
# Same input parameters as /v1/products/moment so the client can mirror the
# moment it is currently visualising. The output FITS is NOT registered as
# a session dataset (it's a 2-D image, not a cube) — the client receives
# the on-disk path and may re-open it via the Data Hub or share it.

class MomentSaveFitsRequest(SpectralChoice):
    dataset_id: str
    moment_order: int
    channel_start: int = 0
    channel_end: int = 0
    mask_enabled: bool = False
    threshold_value: float = 0.0
    threshold_auto: bool = False
    # R-03: order-2 default is dispersion σ (km/s); True keeps variance σ².
    moment2_variance: bool = False
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
    from the cube). Collisions are resolved with a numeric suffix.

    The map is computed as ``/v1/products/moment`` computes it for the same
    request (issue #119): same backend, thresholds, memory bound and product
    cache, so the file holds the map the viewer shows."""
    entry = _require_dataset(session, request.dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "Moment FITS export requires a spectral cube dataset.")
    # One of the session's heavy slots, as for /v1/products/moment.
    if not await acquire_task_slot(session):
        raise task_slot_busy_error()
    try:
        src_stem = Path(entry["path"]).stem
        default_basename = f"{src_stem}_m{int(request.moment_order)}.fits"
        basename = request.output_basename.strip() or default_basename
        output_path = str(make_workspace_path(basename))
        result = await _moment_fits_export(entry, request, output_path)
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[moment_fits] failed dataset_id=%s error=%s",
                       request.dataset_id, exc)
        return MomentSaveFitsResponse(valid=False, error=str(exc))
    finally:
        session.release_task_slot()
    workspace_filename = Path(result["output_path"]).name
    return MomentSaveFitsResponse(
        valid=True, workspace_filename=workspace_filename,
        **{k: result[k] for k in
           ("output_path", "width", "height", "range_min", "range_max",
            "bunit", "moment_unit")})

# ── Persist a PV diagram to a 2-D FITS file ────────────────────────────────
# Same path parameters as /v1/cube/pv. Axis 1 = offset along the path
# (arcsec), axis 2 = the cube's spectral axis; written to Workspace Exports.

class PvSaveFitsRequest(SpectralRangeChoice):
    dataset_id: str
    vertices: list[list[float]]
    width_pixels: int = 1
    output_basename: str = ""


class PvSaveFitsResponse(BaseModel):
    valid: bool
    error: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    width: int = 0
    height: int = 0
    range_min: float = 0.0
    range_max: float = 0.0
    bunit: str = ""


@router.post("/v1/products/pv_fits", response_model=PvSaveFitsResponse, tags=["products"])
async def pv_save_fits(
    request: PvSaveFitsRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> PvSaveFitsResponse:
    """Compute the PV diagram along the given path and save it in the
    **Workspace Exports** dir as a 2-D FITS image with WCS."""
    from ..compute import worker_pv_save_fits  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "PV FITS export requires a spectral cube dataset.")
        basename = request.output_basename.strip() or f"{Path(entry['path']).stem}_pv.fits"
        output_path = str(make_workspace_path(basename))
        result = await _run_heavy_with_limit(
            session,
            worker_pv_save_fits,
            entry["path"],
            output_path,
            [list(v) for v in request.vertices],
            max(1, int(request.width_pixels)),
            None,
            request.spectral_choice(),
            request.spectral_range(),
        )
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[pv_fits] failed dataset_id=%s error=%s", request.dataset_id, exc)
        return PvSaveFitsResponse(valid=False, error=str(exc))
    return PvSaveFitsResponse(
        valid=True, workspace_filename=Path(result["output_path"]).name,
        **{k: result[k] for k in ("output_path", "width", "height", "range_min", "range_max", "bunit")})

# ── Persist a spectrum to a small table ────────────────────────────────────
# The spectrum of /v1/spectral/probe (radius 0) or /v1/spectral/aperture
# (radius > 0), as a CSV or FITS table in Workspace Exports, so a spectrum
# made in the headset can be opened on the desktop (issue #236).

class SpectrumTableRequest(BaseModel):
    dataset_id: str
    x: float
    y: float
    radius: float = Field(0.0, ge=0.0, le=4096.0)
    format: Literal["csv", "fits"] = "csv"
    output_basename: str = ""


class SpectrumTableResponse(BaseModel):
    valid: bool
    error: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    n_channels: int = 0
    spectral_unit: str = ""
    spectral_type: str = ""
    bunit: str = ""
    x: float = 0.0
    y: float = 0.0
    radius: float = 0.0
    n_pix: int = 0
    range_min: float = 0.0
    range_max: float = 0.0


_TABLE_SUFFIX = {"csv": ".csv", "fits": ".fits"}


@router.post("/v1/products/spectrum_table", response_model=SpectrumTableResponse, tags=["products"])
async def spectrum_save_table(
    request: SpectrumTableRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> SpectrumTableResponse:
    """Extract the spectrum at pixel (x, y) (``radius`` 0, as
    /v1/spectral/probe) or the mean over a circular aperture (as
    /v1/spectral/aperture), and save it in the **Workspace Exports** dir as a
    ``csv`` (the desktop's spectrum CSV) or ``fits`` (binary table). The
    file's suffix follows ``format``; collisions get a numeric suffix."""
    from ..compute import worker_spectrum_table  # noqa: PLC0415
    from ..dependencies import _run, _run_heavy  # noqa: PLC0415
    if not (math.isfinite(request.x) and math.isfinite(request.y)):
        raise HTTPException(422, "x and y must be finite pixel coordinates.")
    entry = _require_dataset(session, request.dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "A spectrum table requires a spectral cube dataset.")
    suffix = _TABLE_SUFFIX[request.format]
    stem = Path(request.output_basename.strip()).stem if request.output_basename.strip() else (
        f"{Path(entry['path']).stem}_spectrum_x{int(round(request.x))}_y{int(round(request.y))}")
    try:
        # Reserved (an empty file made atomically), so two saves of the same
        # name at once get two names; the worker writes over its placeholder.
        output_path = make_workspace_path(stem + suffix, reserve=True)
    except Exception as exc:
        return SpectrumTableResponse(valid=False, error=str(exc))
    # Who removes the placeholder on a failure: this request while the work
    # was never handed to the pool; once it was, the work itself, when the
    # worker fails. _run_heavy shields the work from this request's
    # cancellation and 504, so the name stays reserved while a worker may
    # still write it.
    dispatched = False

    async def _written(fn, *args):
        try:
            return await _run(fn, *args)
        except BaseException:
            output_path.unlink(missing_ok=True)
            raise

    def _dispatch(fn, *args):
        nonlocal dispatched
        dispatched = True              # synchronously, as the work is created
        return _written(fn, *args)

    try:
        if not await acquire_task_slot(session):
            raise task_slot_busy_error()
        try:
            result = await _run_heavy(
                worker_spectrum_table, entry["path"], str(output_path),
                float(request.x), float(request.y), float(request.radius), request.format,
                dispatch=_dispatch)
        finally:
            session.release_task_slot()
    except BaseException as exc:
        if not dispatched:
            output_path.unlink(missing_ok=True)
        if isinstance(exc, HTTPException) or not isinstance(exc, Exception):
            raise
        logger.warning("[spectrum_table] failed dataset_id=%s error=%s", request.dataset_id, exc)
        return SpectrumTableResponse(valid=False, error=str(exc))
    return SpectrumTableResponse(valid=True, workspace_filename=Path(result["output_path"]).name, **result)


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
        levels = request.resolved_levels()
        # "budget": a surface cached before it was bounded (ISO_MAX_POLYS), or
        # before an answer carried one surface per level (2), is not served again.
        iso_cache_params = {"levels": levels, "budget": 2}
        cache_key, param_hash = PRODUCT_CACHE.make_key(entry["path"], "isosurface", iso_cache_params)
        cached = PRODUCT_CACHE.get(cache_key)
        if cached is not None:
            logger.debug("[isosurface] cache hit dataset_id=%s levels=%s", request.dataset_id, levels)
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
            levels,
        )
        dataset = ScientificFitsDataset(entry["path"])
        PRODUCT_CACHE.put(
            key=cache_key,
            operation="isosurface",
            dataset_path=entry["path"],
            parameter_hash=param_hash,
            payload=dict(result),
            scientific_metadata={
                "threshold": levels[0] if len(levels) == 1 else None,
                "levels": levels,
                "spacing": request.spacing if request.n_levels is not None and not request.levels else None,
                "skipped_levels": result.get("skipped_levels", []),
                "stride": result.get("stride", []),
            },
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
        info={"op": result["op"], "reprojected": result["reprojected"],
              "overlap_fraction": result["overlap_fraction"],
              "warning": result["warning"]})


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


# ── LTE analysis of CO isotopologues (issue #416, item 4) ────────────────────
# Optical depth from the 13CO/C18O ratio, T_ex, LTE column densities, N(H2)
# and mass, as maps and per region. Physics in app/lte_co.py, data handling
# and provenance in app/lte_products.py.

class LteMeasurements(BaseModel):
    """The integrals of one region measured elsewhere (e.g. Gaussian fits of
    its mean spectra), in K km/s on the scale ``scale``, with 1-sigma errors."""
    w_13co: float
    sigma_13co: float = Field(ge=0)
    w_c18o: float
    sigma_c18o: float = Field(ge=0)
    scale: Literal["ta*", "tmb"] = "ta*"
    #: Peak temperatures (K, same scale) for T_ex: 12CO taken as optically thick,
    #: or 13CO with the optical depth at the peak from the ratio of the peaks.
    peak_13co: float | None = None
    peak_c18o: float | None = None
    peak_12co: float | None = None
    #: For R_GC from a distance, and for the mass.
    glon_deg: float | None = None
    glat_deg: float | None = None
    n_pixels: int | None = Field(None, ge=1)
    pixel_arcsec: float | None = Field(None, gt=0)


class LteCoRequest(BaseModel):
    transition: Literal["1-0", "3-2"]
    dataset_id_13co: str = ""
    dataset_id_c18o: str = ""
    dataset_id_12co: str = ""
    measurements: LteMeasurements | None = None
    #: The line's velocity window (km/s) when a dataset is a cube.
    v_min_kms: float | None = None
    v_max_kms: float | None = None
    #: Noise: K per channel for a cube, K km/s for an integrated map; measured
    #: from the data when absent.
    rms_13co: float | None = Field(None, gt=0)
    rms_c18o: float | None = Field(None, gt=0)
    snr_min: float = Field(3.0, ge=0)
    #: Main-beam efficiencies; from the header when it names one, else required.
    eta_mb_13co: float | None = None
    eta_mb_c18o: float | None = None
    eta_mb_12co: float | None = None
    #: X(13CO)/X(C18O); else from the galactocentric gradients at R_GC.
    x_ratio: float | None = None
    r_gc_kpc: float | None = Field(None, ge=0)
    distance_kpc: float | None = Field(None, gt=0)
    carbon_gradient: Literal["milam2005", "wilson_rood1994"] = "milam2005"
    #: Fixed T_ex (K); else from 12CO, else from the 13CO peak and its optical depth.
    t_ex_k: float | None = None
    h2_from: Literal["c18o", "13co"] = "c18o"
    x12co: float = Field(8.5e-5, gt=0)
    abundance_c18o: float | None = Field(None, gt=0)
    abundance_13co: float | None = Field(None, gt=0)
    mu_h2: float = Field(2.8, gt=0)
    #: Canonical image-frame regions (0-based pixels) on the 13CO grid.
    regions: list[dict[str, Any]] = Field(default_factory=list)
    write_maps: bool = True
    output_basename: str = ""


class LteMapResult(BaseModel):
    quantity: str
    output_path: str
    workspace_filename: str
    bunit: str
    range_min: float
    range_max: float


class LteCoResponse(BaseModel):
    valid: bool
    error: str = ""
    width: int = 0
    height: int = 0
    assumptions: dict[str, Any] = Field(default_factory=dict)
    warnings: list[str] = Field(default_factory=list)
    regions: list[dict[str, Any]] = Field(default_factory=list)
    maps: list[LteMapResult] = Field(default_factory=list)
    pixel_flags: dict[str, int] = Field(default_factory=dict)


@router.post("/v1/products/lte_co", response_model=LteCoResponse, tags=["products"])
async def lte_co(
    request: LteCoRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> LteCoResponse:
    """LTE analysis of 13CO and C18O (optionally 12CO): optical depth from the
    line ratio, T_ex, opacity-corrected column densities, N(H2) and mass, per
    region and as FITS maps whose headers record every assumption."""
    from ..lte_products import MAP_QUANTITIES, _remove_files, run_lte_job  # noqa: PLC0415
    paths: dict[str, str | None] = {"13co": None, "c18o": None, "12co": None}
    kinds: dict[str, str] = {}
    if request.measurements is None:
        for key, ds_id in (("13co", request.dataset_id_13co), ("c18o", request.dataset_id_c18o),
                           ("12co", request.dataset_id_12co)):
            if not ds_id:
                continue
            entry = _require_dataset(session, ds_id)
            if entry["kind"] not in ("cube", "image"):
                raise HTTPException(422, f"The {key} dataset must be a cube or a 2-D map.")
            paths[key], kinds[key] = entry["path"], entry["kind"]
        if not paths["13co"] or not paths["c18o"]:
            raise HTTPException(422, "Give dataset_id_13co and dataset_id_c18o, or measurements.")
    if not await acquire_task_slot(session):
        raise task_slot_busy_error()
    reserved: list[Path] = []
    try:
        outputs: dict[str, str] = {}
        if request.measurements is None and request.write_maps:
            stem = request.output_basename.strip() or f"{_fits_stem(paths['13co'])}_lte"
            stem = _fits_stem(stem)
            for q in MAP_QUANTITIES:
                p = make_workspace_path(f"{stem}_{q}.fits", reserve=True)
                reserved.append(p)
                outputs[q] = str(p)
    except Exception as exc:
        session.release_task_slot()
        _remove_files([str(p) for p in reserved])
        return LteCoResponse(valid=False, error=str(exc))
    try:
        # The job owns its files from here: run in a thread, it finishes (and
        # cleans up) even when the request is cancelled.
        result = await asyncio.to_thread(
            run_lte_job, request.model_dump(mode="json"),
            path13=paths["13co"], path18=paths["c18o"], path12=paths["12co"],
            kinds=kinds, output_paths=outputs)
    except Exception as exc:
        logger.warning("[lte_co] failed error=%s", exc)
        return LteCoResponse(valid=False, error=str(exc))
    finally:
        session.release_task_slot()
    maps = [LteMapResult(workspace_filename=Path(m["output_path"]).name, **m)
            for m in result["maps"]]
    return LteCoResponse(valid=True, width=result["width"], height=result["height"],
                         assumptions=result["assumptions"], warnings=result["warnings"],
                         regions=result["regions"], maps=maps,
                         pixel_flags=result["pixel_flags"])


def _fits_stem(name: str) -> str:
    """The name without .fits, .fit or .fits.gz (``Path.stem`` keeps ".fits" of ".fits.gz")."""
    base = Path(name).name
    for ext in (".fits.gz", ".fits", ".fit", ".fts"):
        if base.lower().endswith(ext):
            return base[: -len(ext)]
    return base
