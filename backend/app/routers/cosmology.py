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

router = APIRouter(tags=['cosmology'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

class AngularPowerRequest(BaseModel):
    channel: int = 0
    n_bins: int = 64


@router.post("/v1/cosmology/angular_power/{dataset_id}", tags=["cosmology"])
async def cosmology_angular_power(
    dataset_id: str,
    request: AngularPowerRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Azimuthally-averaged 2-D angular power spectrum of an image plane.

    Restored after a router refactor dropped the route; the compute
    (``worker_angular_power_spectrum``) was never removed. Returns
    ``{valid, channel, width, height, n_bins, k_bins, ell, cl, cl_err, power,
    k_unit, power_unit}`` (``power`` is the legacy alias for ``cl``).
    """
    from ..compute import worker_angular_power_spectrum  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    try:
        result = await asyncio.to_thread(
            worker_angular_power_spectrum, entry["path"], request.channel, request.n_bins)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[angular_power] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", **result}


@router.get("/v1/cosmology/powerspectrum/{dataset_id}", tags=["cosmology"])
async def cosmology_powerspectrum(
    dataset_id: str,
    channel: int = Query(default=0),
    n_bins: int = Query(default=64),
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Spec-conformant GET alias of ``/v1/cosmology/angular_power`` (same worker)."""
    from ..compute import worker_angular_power_spectrum  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    try:
        result = await asyncio.to_thread(
            worker_angular_power_spectrum, entry["path"], channel, n_bins)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[powerspectrum] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", **result}


@router.get("/v1/cosmology/distances", tags=["cosmology"])
async def cosmology_distances(
    z: float = Query(...),
    H0: float | None = Query(default=None),
    Om0: float | None = Query(default=None),
    Ode0: float | None = Query(default=None),
    _: None = _auth,
):
    """Full set of cosmological distances/times for a redshift, spec-conformant.

    Optional ``H0`` / ``Om0`` / ``Ode0`` select a custom (LambdaCDM) cosmology;
    omitted parameters default to Planck18. Returns comoving, luminosity and
    angular-diameter distances (Mpc) plus lookback time and age of the universe
    (Gyr). Unphysical inputs are rejected with 422.
    """
    if not (0.0 <= z <= 20.0):
        raise HTTPException(422, "Redshift z must be in [0, 20].")
    if H0 is not None and not (40.0 <= H0 <= 120.0):
        raise HTTPException(422, "H0 must be in [40, 120] km/s/Mpc.")
    if Om0 is not None and not (0.0 <= Om0 <= 1.5):
        raise HTTPException(422, "Om0 must be in [0, 1.5].")
    if Ode0 is not None and not (0.0 <= Ode0 <= 1.5):
        raise HTTPException(422, "Ode0 must be in [0, 1.5].")
    # Reject closed cosmologies (total density > 1): this endpoint targets flat
    # or open models, and it guards against accidental Om0+Ode0 over-specification.
    if Om0 is not None and Ode0 is not None and (Om0 + Ode0) > 1.0 + 1e-6:
        raise HTTPException(422, "Om0 + Ode0 must not exceed 1 (closed cosmologies unsupported).")

    try:
        import astropy.units as u  # noqa: PLC0415
        from astropy.cosmology import LambdaCDM, Planck18  # noqa: PLC0415
    except ImportError:
        raise HTTPException(503, "astropy is not installed on this server.") from None

    if H0 is None and Om0 is None and Ode0 is None:
        cosmo = Planck18
    else:
        cosmo = LambdaCDM(
            H0=(H0 if H0 is not None else Planck18.H0.value),
            Om0=(Om0 if Om0 is not None else Planck18.Om0),
            Ode0=(Ode0 if Ode0 is not None else Planck18.Ode0),
        )
    try:
        return {
            "valid": True,
            "error": "",
            "z": z,
            "comoving_Mpc": float(cosmo.comoving_distance(z).to(u.Mpc).value),
            "luminosity_Mpc": float(cosmo.luminosity_distance(z).to(u.Mpc).value),
            "angular_diameter_Mpc": float(cosmo.angular_diameter_distance(z).to(u.Mpc).value),
            "lookback_time_Gyr": float(cosmo.lookback_time(z).to(u.Gyr).value),
            "age_Gyr": float(cosmo.age(z).to(u.Gyr).value),
        }
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(422, str(exc)) from exc


@router.post("/v1/cosmology/distance", response_model=CosmologyDistanceResponse, tags=["cosmology"])
async def cosmology_distance(
    request: CosmologyDistanceRequest,
    _: None = _auth,
) -> CosmologyDistanceResponse:
    """Compute comoving distance, luminosity distance, and proper-motion scale for a given redshift.

    The ``model`` parameter selects the background cosmology; supported values are
    ``Planck18`` (default), ``Planck15``, ``Planck13``, and ``WMAP9``.
    """
    models = _load_cosmology_models()
    if not models:
        return CosmologyDistanceResponse(
            valid=False, error="astropy is not installed on this server. Cosmology endpoint unavailable."
        )
    if request.model not in models:
        return CosmologyDistanceResponse(
            valid=False,
            error=f"Unknown cosmology model '{request.model}'. Available: {sorted(models.keys())}",
        )
    z = float(request.z)
    if z < 0.0:
        return CosmologyDistanceResponse(valid=False, error="Redshift z must be >= 0.")
    try:
        cosmo = models[request.model]
        comoving_mpc = float(cosmo.comoving_distance(z).to("Mpc").value)
        luminosity_mpc = float(cosmo.luminosity_distance(z).to("Mpc").value)
        # kpc_proper_per_arcmin → divide by 60 to get kpc/arcsec
        kpc_per_arcsec = float(cosmo.kpc_proper_per_arcmin(z).value / 60.0) if z > 0.0 else 0.0
    except Exception as exc:
        return CosmologyDistanceResponse(valid=False, error=str(exc))
    return CosmologyDistanceResponse(
        valid=True,
        error="",
        z=z,
        model=request.model,
        comoving_Mpc=comoving_mpc,
        luminosity_Mpc=luminosity_mpc,
        proper_motion_kpc_per_arcsec=kpc_per_arcsec,
    )

@router.post("/v1/cosmology/distance/batch", response_model=CosmologyBatchResponse, tags=["cosmology"])
async def cosmology_distance_batch(
    request: CosmologyBatchRequest,
    _: None = _auth,
) -> CosmologyBatchResponse:
    """Compute comoving distances (Mpc) for a list of redshifts in one call.

    Accepts up to 100 000 redshift values.  Returns an array of comoving distances
    in the same order.  Negative or non-finite values are replaced with 0.0.
    """
    models = _load_cosmology_models()
    if not models:
        return CosmologyBatchResponse(
            valid=False,
            error="astropy is not installed on this server. Cosmology endpoint unavailable.",
        )
    if request.model not in models:
        return CosmologyBatchResponse(
            valid=False,
            error=f"Unknown cosmology model '{request.model}'. Available: {sorted(models.keys())}",
        )
    if len(request.redshifts) > 100_000:
        return CosmologyBatchResponse(
            valid=False, error="Too many redshifts: limit is 100 000 per request."
        )
    try:
        import numpy as _np
        cosmo = models[request.model]
        zs = _np.array(request.redshifts, dtype=float)
        distances = _np.zeros_like(zs)
        valid_mask = _np.isfinite(zs) & (zs >= 0.0)
        if valid_mask.any():
            distances[valid_mask] = cosmo.comoving_distance(zs[valid_mask]).to("Mpc").value
        return CosmologyBatchResponse(
            valid=True,
            error="",
            model=request.model,
            distances_Mpc=distances.tolist(),
        )
    except Exception as exc:
        return CosmologyBatchResponse(valid=False, error=str(exc))
