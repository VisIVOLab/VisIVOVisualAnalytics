"""Polarisation endpoints — Stokes plane extraction (I, Q, U, V).

Restored after a router refactor accidentally dropped these routes while the
desktop client (BackendClient::requestStokesPlane) kept calling them. The
compute (``app.compute.worker_stokes_plane``) was never removed.

Two variants share the same worker:
  * ``GET /v1/stokes/{dataset_id}``        — JSON, base64 float32 payload.
  * ``GET /v1/stokes/{dataset_id}/binary`` — raw Visivo binary frame.
"""
import asyncio
import base64
import logging

import numpy as np
from fastapi import APIRouter, Depends, HTTPException, Query, Response
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import _require_dataset
from ..sessions import Session, get_session
from .spectral import _array_to_binary

router = APIRouter(tags=["polarisation"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)


class PiRequest(BaseModel):
    dataset_id: str = ""
    channel: int | None = None
    line_free_channels: list[int] | None = None


class RmSynthesisRequest(BaseModel):
    dataset_id: str = ""
    phi_range_rad_m2: list[float]
    dphi_rad_m2: float


def _b64_to_plane(result: dict, key: str) -> np.ndarray:
    h, w = int(result["height"]), int(result["width"])
    return np.frombuffer(base64.b64decode(result[key]), dtype=np.float32).reshape(h, w)


async def _extract_plane(session: Session, dataset_id: str, plane: str, channel: int | None) -> dict:
    """Shared worker dispatch for the JSON and binary stokes variants.

    Maps the worker's failure modes to HTTP: an unknown/absent Stokes plane →
    422; any other failure → 500.
    """
    from ..compute import worker_stokes_plane  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    try:
        return await asyncio.to_thread(worker_stokes_plane, entry["path"], plane, channel)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except HTTPException:
        raise
    except Exception as exc:  # noqa: BLE001
        logger.warning("[stokes] failed dataset_id=%s plane=%s error=%s", dataset_id, plane, exc)
        raise HTTPException(500, str(exc)) from exc


@router.get("/v1/stokes/{dataset_id}", tags=["polarisation"])
async def stokes_plane(
    dataset_id: str,
    plane: str = Query(default="I"),
    channel: int | None = Query(default=None),
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Extract one Stokes plane as a base64 float32 image.

    Returns ``{valid, plane, channel, width, height, range_min, range_max,
    data_base64}``. ``plane`` must be one of I/Q/U/V and present in the cube.
    """
    result = await _extract_plane(session, dataset_id, plane, channel)
    return {"valid": True, "error": "", **result}


@router.get("/v1/stokes/{dataset_id}/binary", tags=["polarisation"])
async def stokes_plane_binary(
    dataset_id: str,
    plane: str = Query(default="I"),
    channel: int | None = Query(default=None),
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Same Stokes plane as the JSON variant, as a raw Visivo binary frame.

    Body = 12-byte header (rows, cols, flags) + row-major float32 little-endian,
    matching ``BackendClient::parseBinaryFrame()``.
    """
    result = await _extract_plane(session, dataset_id, plane, channel)
    h, w = int(result["height"]), int(result["width"])
    arr = np.frombuffer(base64.b64decode(result["data_base64"]), dtype=np.float32).reshape(h, w)
    return Response(content=_array_to_binary(h, w, arr), media_type="application/octet-stream")


# ── P-02: Polarised intensity / position angle ───────────────────────────────


async def _compute_pi(session: Session, dataset_id: str, request: PiRequest) -> dict:
    from ..compute import worker_polarisation_pi_pa  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    try:
        return await asyncio.to_thread(
            worker_polarisation_pi_pa, entry["path"], request.channel, request.line_free_channels)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except HTTPException:
        raise
    except Exception as exc:  # noqa: BLE001
        logger.warning("[pi] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc


@router.post("/v1/polarisation/pi/{dataset_id}", tags=["polarisation"])
async def polarisation_pi(
    dataset_id: str,
    request: PiRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Bias-corrected polarised intensity (PI) and position angle (PA) metadata.

    Returns shape, channel, the de-bias term ``sigma_qu2`` and the PI/PA value
    ranges. The map pixels themselves come from the ``/result/binary`` variant;
    ``pi_dataset_id``/``pa_dataset_id`` are kept for wire compatibility (the
    desktop client renders from the binary frames and does not consume them).
    """
    result = await _compute_pi(session, dataset_id, request)
    payload = {k: result[k] for k in (
        "width", "height", "channel", "sigma_qu2",
        "pi_range_min", "pi_range_max", "pa_range_min", "pa_range_max")}
    return {"valid": True, "error": "", "pi_dataset_id": "", "pa_dataset_id": "", **payload}


@router.post("/v1/polarisation/pi/result/binary", tags=["polarisation"])
async def polarisation_pi_binary(
    request: PiRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """PI and PA maps as two concatenated Visivo binary frames (PI then PA)."""
    result = await _compute_pi(session, request.dataset_id, request)
    h, w = int(result["height"]), int(result["width"])
    pi = _b64_to_plane(result, "pi_base64")
    pa = _b64_to_plane(result, "pa_base64")
    content = _array_to_binary(h, w, pi) + _array_to_binary(h, w, pa)
    return Response(content=content, media_type="application/octet-stream")


# ── P-03: Rotation Measure synthesis ─────────────────────────────────────────


async def _compute_rm(session: Session, dataset_id: str, request: RmSynthesisRequest) -> dict:
    from ..compute import worker_rm_synthesis  # noqa: PLC0415

    if len(request.phi_range_rad_m2) != 2:
        raise HTTPException(422, "phi_range_rad_m2 must be [phi_min, phi_max].")
    phi_min, phi_max = float(request.phi_range_rad_m2[0]), float(request.phi_range_rad_m2[1])
    entry = _require_dataset(session, dataset_id)
    try:
        return await asyncio.to_thread(
            worker_rm_synthesis, entry["path"], phi_min, phi_max, float(request.dphi_rad_m2))
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except HTTPException:
        raise
    except Exception as exc:  # noqa: BLE001
        logger.warning("[rmsynthesis] failed dataset_id=%s error=%s", dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc


@router.post("/v1/polarisation/rmsynthesis/{dataset_id}", tags=["polarisation"])
async def rm_synthesis(
    dataset_id: str,
    request: RmSynthesisRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """RM-synthesis metadata: φ grid, RMSF and the Faraday dispersion shape."""
    result = await _compute_rm(session, dataset_id, request)
    payload = {k: result[k] for k in (
        "n_phi", "n_freq", "width", "height", "phi_min", "phi_max", "d_phi",
        "phi_grid", "rmsf", "range_min", "range_max")}
    return {"valid": True, "error": "", **payload}


@router.post("/v1/polarisation/rmsynthesis/{dataset_id}/binary", tags=["polarisation"])
async def rm_synthesis_binary(
    dataset_id: str,
    request: RmSynthesisRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Faraday dispersion cube as one binary frame ((n_phi·height) rows × width)."""
    result = await _compute_rm(session, dataset_id, request)
    rows = int(result["n_phi"]) * int(result["height"])
    cols = int(result["width"])
    arr = np.frombuffer(base64.b64decode(result["data_base64"]), dtype=np.float32).reshape(rows, cols)
    return Response(content=_array_to_binary(rows, cols, arr), media_type="application/octet-stream")
