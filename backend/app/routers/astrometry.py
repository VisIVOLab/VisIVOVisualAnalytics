"""Astrometry endpoints — catalogue cross-match against the image footprint.

Restored after a router refactor accidentally dropped these routes while the
desktop client (BackendClient::requestCrossmatch) kept calling them. The
compute (``app.compute.worker_crossmatch``) was never removed.
"""
import asyncio
import logging

from fastapi import APIRouter, Depends, HTTPException, Query, Response

from ..auth import verify_token
from ..dependencies import _require_dataset
from ..sessions import Session, get_session

router = APIRouter(tags=["astrometry"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)


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
