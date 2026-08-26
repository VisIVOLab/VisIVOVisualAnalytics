"""Photometry endpoints — aperture photometry and 2-D Gaussian source fitting.

Restored after a router refactor dropped these routes while the desktop client
(BackendClient::requestAperturePhotometry / requestGaussFit) kept POSTing to
them. The compute (``worker_aperture_photometry`` / ``worker_gauss_fit_2d``)
was never removed.
"""
import asyncio
import logging

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import _require_dataset
from ..sessions import Session, get_session

router = APIRouter(tags=["photometry"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)


class AperturePhotometryRequest(BaseModel):
    ra_deg: float
    dec_deg: float
    aperture_arcsec: float
    annulus_inner_arcsec: float = 0.0
    annulus_outer_arcsec: float = 0.0
    channel: int | None = None


class GaussFitRequest(BaseModel):
    ra_deg: float
    dec_deg: float
    box_arcsec: float
    channel: int | None = None


@router.post("/v1/photometry/aperture/{dataset_id}", tags=["photometry"])
async def aperture_photometry(
    dataset_id: str,
    request: AperturePhotometryRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Circular-aperture flux with an optional annulus background subtraction.

    Returns ``{valid, flux_jy, flux_err_jy, flux, flux_err, flux_unit,
    beam_area_pix, background_per_pix, background_rms_per_pix, bunit,
    n_pix_aperture, channel, pix_x, pix_y, pixel_scale_arcsec}``. ``flux_jy`` is
    the beam-corrected physical flux (Jy) and is ``null`` unless the data are
    Jy/beam with a known beam; ``flux`` is always the raw summed brightness in
    ``flux_unit``. Requires ``photutils`` (→503) and celestial WCS (→422).
    """
    from ..compute import worker_aperture_photometry  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    try:
        result = await asyncio.to_thread(
            worker_aperture_photometry, entry["path"], request.channel,
            request.ra_deg, request.dec_deg, request.aperture_arcsec,
            request.annulus_inner_arcsec, request.annulus_outer_arcsec)
    except ImportError as exc:
        raise HTTPException(503, str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[aperture] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", **result}


@router.post("/v1/photometry/gaussfit/{dataset_id}", tags=["photometry"])
async def gauss_fit(
    dataset_id: str,
    request: GaussFitRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Fit a 2-D elliptical Gaussian to a source in a box around (ra, dec).

    Returns peak/integrated flux, fitted + deconvolved FWHM, PA, fit RMS and the
    centre pixel. Bad WCS / too-small box → 422; a failed fit → 500.
    """
    from ..compute import worker_gauss_fit_2d  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    try:
        result = await asyncio.to_thread(
            worker_gauss_fit_2d, entry["path"], request.channel,
            request.ra_deg, request.dec_deg, request.box_arcsec)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[gaussfit] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", **result}
