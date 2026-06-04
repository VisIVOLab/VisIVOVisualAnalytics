"""Dynamic-spectrum / beamformed analysis routes.

Three POST endpoints layered on top of :mod:`dynspec_tools`:

* ``/v1/dynspec/dedisperse``    — incoherent dedispersion at a given DM,
                                  writes a new 2-D FITS into the Workspace
                                  Exports dir and registers a new session
                                  dataset.
* ``/v1/dynspec/pulse_profile`` — returns the folded phase profile as a
                                  JSON payload (no file written; the
                                  client renders it in a small dialog).
* ``/v1/dynspec/rfi_mask``      — σ-clipped flagging mask written as a
                                  uint8 FITS in the workspace.
"""
from __future__ import annotations

from pathlib import Path
import logging
import uuid

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import _require_dataset, _run_with_limit, make_workspace_path
from ..sessions import Session, get_session

router = APIRouter(tags=["dynspec"])

logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)


# ── Incoherent dedispersion ─────────────────────────────────────────────────

class DynspecDedisperseRequest(BaseModel):
    dataset_id: str
    dm_pc_cm3: float
    ref_freq_mhz: float = 0.0  # 0 → use top of band
    output_basename: str = ""


class DynspecDedisperseResponse(BaseModel):
    valid: bool
    error: str = ""
    new_dataset_id: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    width: int = 0
    height: int = 0
    range_min: float = 0.0
    range_max: float = 0.0
    dm_pc_cm3: float = 0.0
    ref_freq_mhz: float = 0.0
    max_shift_samples: int = 0


@router.post("/v1/dynspec/dedisperse",
             response_model=DynspecDedisperseResponse, tags=["dynspec"])
async def dynspec_dedisperse(
    request: DynspecDedisperseRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> DynspecDedisperseResponse:
    from ..dynspec_tools import worker_dynspec_dedisperse  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "dynspec":
            raise HTTPException(
                422, "Dedispersion is only available for dynamic-spectrum / "
                     "beamformed datasets (kind='dynspec').")

        src_stem = Path(entry["path"]).stem
        # Avoid double ".dynspec" prefix when the source is already a sidecar.
        if src_stem.endswith(".dynspec"):
            src_stem = src_stem[:-len(".dynspec")]
        default_basename = (
            f"{src_stem}_DM{request.dm_pc_cm3:.3f}.fits".replace(" ", ""))
        basename = request.output_basename.strip() or default_basename
        output_path = str(make_workspace_path(basename))

        result = await _run_with_limit(
            session,
            worker_dynspec_dedisperse,
            entry["path"],
            output_path,
            float(request.dm_pc_cm3),
            float(request.ref_freq_mhz),
        )
    except HTTPException:
        raise
    except Exception as exc:
        return DynspecDedisperseResponse(valid=False, error=str(exc))

    new_id = f"ds_{uuid.uuid4().hex[:12]}"
    session.datasets[new_id] = {
        "path":   result["output_path"],
        "kind":   "dynspec",
        "width":  result["width"],
        "height": result["height"],
        "depth":  1,
    }
    workspace_filename = Path(result["output_path"]).name
    logger.info(
        "[dynspec/dedisperse] registered new_id=%s DM=%s shape=%sx%s",
        new_id, request.dm_pc_cm3, result["width"], result["height"],
    )
    return DynspecDedisperseResponse(
        valid=True, new_dataset_id=new_id,
        workspace_filename=workspace_filename,
        **{k: result[k] for k in ("output_path", "width", "height",
                                   "range_min", "range_max",
                                   "dm_pc_cm3", "ref_freq_mhz",
                                   "max_shift_samples")})


# ── Pulse profile (folded phase profile) ────────────────────────────────────

class DynspecPulseProfileRequest(BaseModel):
    dataset_id: str
    period_s: float
    n_bins: int = 64
    dm_pc_cm3: float = 0.0       # 0 → skip dedispersion (data already dedispersed)
    freq_start_mhz: float = 0.0  # 0 → use full band
    freq_end_mhz: float = 0.0    # 0 → use full band


class DynspecPulseProfileResponse(BaseModel):
    valid: bool
    error: str = ""
    phase: list[float] = []
    profile: list[float] = []
    counts: list[int] = []
    peak: float = 0.0
    sigma_mad: float = 0.0
    snr_peak: float = 0.0
    period_s: float = 0.0
    dm_pc_cm3: float = 0.0
    n_bins: int = 0
    n_samples: int = 0


@router.post("/v1/dynspec/pulse_profile",
             response_model=DynspecPulseProfileResponse, tags=["dynspec"])
async def dynspec_pulse_profile(
    request: DynspecPulseProfileRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> DynspecPulseProfileResponse:
    from ..dynspec_tools import worker_dynspec_pulse_profile  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "dynspec":
            raise HTTPException(
                422, "Pulse profile is only available for dynamic-spectrum / "
                     "beamformed datasets (kind='dynspec').")
        result = await _run_with_limit(
            session,
            worker_dynspec_pulse_profile,
            entry["path"],
            float(request.period_s),
            int(request.n_bins),
            float(request.dm_pc_cm3),
            float(request.freq_start_mhz),
            float(request.freq_end_mhz),
        )
    except HTTPException:
        raise
    except Exception as exc:
        return DynspecPulseProfileResponse(valid=False, error=str(exc))

    return DynspecPulseProfileResponse(valid=True, **result)


# ── RFI mask ────────────────────────────────────────────────────────────────

class DynspecRfiMaskRequest(BaseModel):
    dataset_id: str
    n_sigma: float = 5.0
    axis: str = "channel"  # 'channel' (clip across time) or 'time' (clip across freq)
    output_basename: str = ""


class DynspecRfiMaskResponse(BaseModel):
    valid: bool
    error: str = ""
    new_dataset_id: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    width: int = 0
    height: int = 0
    flagged_fraction: float = 0.0
    n_sigma: float = 0.0
    axis: str = ""


@router.post("/v1/dynspec/rfi_mask",
             response_model=DynspecRfiMaskResponse, tags=["dynspec"])
async def dynspec_rfi_mask(
    request: DynspecRfiMaskRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> DynspecRfiMaskResponse:
    from ..dynspec_tools import worker_dynspec_rfi_mask  # noqa: PLC0415
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "dynspec":
            raise HTTPException(
                422, "RFI mask is only available for dynamic-spectrum / "
                     "beamformed datasets (kind='dynspec').")

        src_stem = Path(entry["path"]).stem
        if src_stem.endswith(".dynspec"):
            src_stem = src_stem[:-len(".dynspec")]
        default_basename = (
            f"{src_stem}_rfimask_{request.axis}_{request.n_sigma:.1f}sigma.fits"
        ).replace(" ", "")
        basename = request.output_basename.strip() or default_basename
        output_path = str(make_workspace_path(basename))

        result = await _run_with_limit(
            session,
            worker_dynspec_rfi_mask,
            entry["path"],
            output_path,
            float(request.n_sigma),
            str(request.axis),
        )
    except HTTPException:
        raise
    except Exception as exc:
        return DynspecRfiMaskResponse(valid=False, error=str(exc))

    new_id = f"ds_{uuid.uuid4().hex[:12]}"
    # The mask is itself a 2-D file with a TIME/FREQ axis pair → register as
    # 'dynspec' so the client can open it as a layer over the parent.
    session.datasets[new_id] = {
        "path":   result["output_path"],
        "kind":   "dynspec",
        "width":  result["width"],
        "height": result["height"],
        "depth":  1,
    }
    workspace_filename = Path(result["output_path"]).name
    logger.info(
        "[dynspec/rfi_mask] flagged_fraction=%.4f axis=%s n_sigma=%s",
        result["flagged_fraction"], result["axis"], result["n_sigma"],
    )
    return DynspecRfiMaskResponse(
        valid=True, new_dataset_id=new_id,
        workspace_filename=workspace_filename,
        **{k: result[k] for k in ("output_path", "width", "height",
                                   "flagged_fraction", "n_sigma", "axis")})
