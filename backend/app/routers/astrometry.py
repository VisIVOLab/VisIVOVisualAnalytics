"""Astrometry endpoints — catalogue cross-match against the image footprint.

Restored after a router refactor accidentally dropped these routes while the
desktop client (BackendClient::requestCrossmatch) kept calling them. The
compute (``app.compute.worker_crossmatch``) was never removed.
"""
import asyncio
import logging
import uuid
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException, Query, Response
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import _require_dataset, make_workspace_path
from ..sessions import Session, get_session

router = APIRouter(tags=["astrometry"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)


class ReprojectRequest(BaseModel):
    target_wcs: dict
    target_shape: list[int]
    order: int = 1


@router.get("/v1/astrometry/crossmatch/{dataset_id}", tags=["astrometry"])
async def crossmatch(
    response: Response,
    dataset_id: str,
    catalogue: str = Query(default="simbad"),
    radius_arcsec: float = Query(default=60.0, ge=0.0),
    max_sources: int = Query(default=500, ge=1, le=5000),
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Cross-match the image footprint against an external catalogue.

    Catalogues: ``simbad`` (astroquery.simbad) and Vizier ids ``2MASS`` /
    ``NVSS`` / ``FIRST``. Returns ``{valid, n_sources, sources, truncated,
    centre_ra_deg, centre_dec_deg}``.

    Maps the worker's failure modes to HTTP: a missing astroquery install →
    503; an unknown catalogue identifier → 422.
    """
    from ..compute import worker_crossmatch  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    try:
        result = await asyncio.to_thread(
            worker_crossmatch, entry["path"], radius_arcsec, catalogue, max_sources)
        # The worker returns the raw payload; the route adds the standard
        # {valid,error} envelope the desktop client expects, and signals
        # result truncation via the X-Truncated header.
        response.headers["X-Truncated"] = "true" if result.get("truncated") else "false"
        return {"valid": True, "error": "", **result}
    except ImportError as exc:
        raise HTTPException(503, str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[crossmatch] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc


@router.post("/v1/astrometry/reproject/{dataset_id}", tags=["astrometry"])
async def reproject(
    dataset_id: str,
    request: ReprojectRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Reproject an image plane onto a target WCS/grid, registering the result.

    Writes a new FITS to the session workspace and registers it as a new
    dataset; returns ``{valid, new_dataset_id, width, height, range_min,
    range_max, output_path, order}``. A missing ``reproject`` package → 503.
    """
    from ..compute import worker_reproject  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    output_path = str(make_workspace_path(f"{uuid.uuid4().hex[:12]}_reproject.fits"))
    try:
        result = await asyncio.to_thread(
            worker_reproject, entry["path"], output_path,
            request.target_wcs, request.target_shape, request.order)
    except ImportError as exc:
        raise HTTPException(503, str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[reproject] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc

    new_id = f"ds_{uuid.uuid4().hex[:12]}"
    session.datasets[new_id] = {
        "path":   result["output_path"],
        "kind":   "image",
        "width":  result["width"],
        "height": result["height"],
    }
    logger.info("[reproject] registered new dataset new_id=%s path=%s", new_id, result["output_path"])
    return {"valid": True, "error": "", "new_dataset_id": new_id, **result}
