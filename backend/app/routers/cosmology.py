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
