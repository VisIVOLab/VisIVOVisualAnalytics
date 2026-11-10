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
# NaN/inf → None, recursively (numpy scalars included); one helper, not a copy.
from ..compute.kinematic_fit import _json_safe
import asyncio

router = APIRouter(tags=['cosmology'])

# Far above any observed redshift (the CMB is z ≈ 1100). A larger z is refused:
# astropy's integrals return 0 Mpc there (z = 1e308 gave every distance 0.0).
Z_MAX = 1.0e4

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

def _finite_or_none(value: Any) -> float | None:
    """A number JSON can carry, or None (JSON null) for inf / NaN.

    Starlette encodes with ``allow_nan=False``, so one infinite quantity —
    the age of a de Sitter universe, which has no Big Bang — used to turn the
    whole answer into an HTTP 500. Null says "no finite value" for that field
    alone and keeps every other quantity of the answer.
    """
    if value is None:
        return None
    number = float(value)
    return number if np.isfinite(number) else None


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
    ``{valid, channel, width, height, n_bins, k_bins, ell, cl, cl_err, n_modes,
    mode_coupling, power,
    k_unit, power_unit}`` (``power`` is the legacy alias for ``cl``).

    ``cl_err`` is the uncertainty of THIS windowed relative estimator, not of a
    calibrated cosmological Cℓ, and is null for a bin with fewer than 3 modes,
    where no standard error is estimable. Judge a bin by its EFFECTIVE mode count,
    ``n_modes / mode_coupling**2`` — with the Hann window's 1.94, eight raw modes
    are only about two effective ones.
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
    return {"valid": True, "error": "", **_json_safe(result)}


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
    return {"valid": True, "error": "", **_json_safe(result)}


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

    A quantity that is not finite for the chosen model is answered as null and
    the rest of the answer is kept: the age of a de Sitter universe
    (``Om0=0, Ode0=1``) is infinite, since it has no Big Bang.
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
            "comoving_Mpc": _finite_or_none(cosmo.comoving_distance(z).to(u.Mpc).value),
            "luminosity_Mpc": _finite_or_none(cosmo.luminosity_distance(z).to(u.Mpc).value),
            "angular_diameter_Mpc": _finite_or_none(
                cosmo.angular_diameter_distance(z).to(u.Mpc).value),
            "lookback_time_Gyr": _finite_or_none(cosmo.lookback_time(z).to(u.Gyr).value),
            "age_Gyr": _finite_or_none(cosmo.age(z).to(u.Gyr).value),
        }
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(422, str(exc)) from exc


@router.post("/v1/cosmology/distance", response_model=CosmologyDistanceResponse, tags=["cosmology"])
async def cosmology_distance(
    request: CosmologyDistanceRequest,
    _: None = _auth,
) -> CosmologyDistanceResponse:
    """Comoving and luminosity distance and the proper transverse scale at a redshift.

    The ``model`` parameter selects the background cosmology; supported values are
    ``Planck18`` (default), ``Planck15``, ``Planck13``, and ``WMAP9``.

    ``scale_kpc_per_arcsec`` is the proper transverse scale (kpc of proper
    length per arcsecond, D_A per arcsecond). ``proper_motion_kpc_per_arcsec``
    carries the same number under its old, misleading name — it is not a proper
    motion — and is kept as a deprecated alias for one release (#129). A
    quantity that is not finite is answered as null.
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
    # `not z >= 0` also catches NaN, which JSON bodies can carry as a literal.
    if not (np.isfinite(z) and 0.0 <= z <= Z_MAX):
        return CosmologyDistanceResponse(
            valid=False, error=f"Redshift z must be finite, >= 0 and at most {Z_MAX:g}.")
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
        comoving_Mpc=_finite_or_none(comoving_mpc),
        luminosity_Mpc=_finite_or_none(luminosity_mpc),
        scale_kpc_per_arcsec=_finite_or_none(kpc_per_arcsec),
        proper_motion_kpc_per_arcsec=_finite_or_none(kpc_per_arcsec),
    )

@router.post("/v1/cosmology/distance/batch", response_model=CosmologyBatchResponse, tags=["cosmology"])
async def cosmology_distance_batch(
    request: CosmologyBatchRequest,
    _: None = _auth,
) -> CosmologyBatchResponse:
    """Compute comoving distances (Mpc) for a list of redshifts in one call.

    Accepts up to 100 000 redshift values.  Returns an array of comoving distances
    in the same order.  A negative or non-finite entry yields **null** at its
    position rather than a number: bulk input is tolerated (one bad row does not
    fail the request) but never silently turned into a distance of 0 Mpc, which
    would be indistinguishable from a genuine z = 0.
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
        valid_mask = _np.isfinite(zs) & (zs >= 0.0) & (zs <= Z_MAX)
        if valid_mask.any():
            distances[valid_mask] = cosmo.comoving_distance(zs[valid_mask]).to("Mpc").value
        # NULL for an invalid entry, not 0.0 — which is what the response model has
        # always documented and what the single-redshift endpoint does (it answers
        # "Redshift z must be >= 0"). Substituting 0.0 made an unphysical input
        # indistinguishable from a genuine z = 0 at the observer's own position.
        out: list[float | None] = [
            (_finite_or_none(d) if ok else None)
            for d, ok in zip(distances.tolist(), valid_mask.tolist())
        ]
        return CosmologyBatchResponse(
            valid=True,
            error="",
            model=request.model,
            distances_Mpc=out,
        )
    except Exception as exc:
        return CosmologyBatchResponse(valid=False, error=str(exc))
