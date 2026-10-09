"""Reproducibility & provenance endpoints (Domain R).

Restored after a router refactor dropped the whole subsystem. The CARTA-script
export (former R-01) was intentionally retired and is NOT reintroduced.

Provided:
  * GET  /v1/reproducibility/snapshots          — list the session op-history
  * POST /v1/reproducibility/snapshots          — append a history entry
  * GET  /v1/reproducibility/provenance         — W3C PROV-JSON of the session
  * POST /v1/reproducibility/snapshot           — HMAC-signed named snapshot
  * GET  /v1/reproducibility/snapshot/{id}      — retrieve + verify a snapshot

History is also appended automatically by compute endpoints via
``record_history`` (see app.dependencies), so the provenance graph reflects the
real work done in a session, not only manually-recorded steps.
"""
import hashlib
import hmac
import json
import logging
import os
import secrets
import uuid
from datetime import datetime, timezone
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import record_history
from ..sessions import Session, get_session

router = APIRouter(tags=["reproducibility"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)

# Process-wide secret used to sign snapshots. A deployment can pin it via the
# environment so signatures survive restarts; otherwise a fresh random secret
# is generated per process (snapshots are session-scoped and in-memory anyway).
_SNAPSHOT_SECRET = (
    os.environ.get("VISIVO_SNAPSHOT_SECRET")
    or os.environ.get("ADMIN_SESSION_SECRET")
    or secrets.token_hex(32)
).encode("utf-8")

# Prefix URI for the visivo PROV namespace.
_PROV_PREFIX = "https://visivo.oact.inaf.it/prov#"


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _sign(state: dict[str, Any]) -> str:
    canonical = json.dumps(state, sort_keys=True, default=str).encode("utf-8")
    return hmac.new(_SNAPSHOT_SECRET, canonical, hashlib.sha256).hexdigest()


# ── R-03: operation history (snapshots list/record) ──────────────────────────


class SnapshotRecordRequest(BaseModel):
    operation: str = ""
    dataset_id: str = ""
    result_dataset_id: str = ""
    params: dict[str, Any] = {}
    timestamp: str = ""


@router.get("/v1/reproducibility/snapshots", tags=["reproducibility"])
async def list_snapshots(_: None = _auth, session: Session = Depends(get_session)):
    """Return the append-only operation history for the session (oldest first)."""
    return {"valid": True, "error": "", "n_entries": len(session.history), "history": session.history}


@router.post("/v1/reproducibility/snapshots", tags=["reproducibility"])
async def record_snapshot(
    request: SnapshotRecordRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Append a manual history entry. A missing timestamp is auto-filled."""
    record_history(
        session,
        operation=request.operation,
        dataset_id=request.dataset_id,
        params=request.params,
        result_dataset_id=request.result_dataset_id,
        endpoint="",
        timestamp=request.timestamp or _now_iso(),
    )
    return {"valid": True, "error": "", "n_entries": len(session.history), "history": session.history}


# ── R-02: PROV-JSON provenance graph ─────────────────────────────────────────


@router.get("/v1/reproducibility/provenance", tags=["reproducibility"])
async def provenance(_: None = _auth, session: Session = Depends(get_session)):
    """Emit a W3C PROV-JSON document describing the session's datasets + ops.

    Datasets become ``entity`` nodes (``visivo:{id}`` with a ``visivo:path``),
    each history step becomes an ``activity``; ``used`` links an activity to its
    input dataset, ``wasGeneratedBy`` links a produced dataset to its activity,
    and ``wasDerivedFrom`` records the derived→source dataset relationship.
    """
    entity: dict[str, Any] = {}
    for ds_id, entry in session.datasets.items():
        entity[f"visivo:{ds_id}"] = {
            "visivo:path": entry.get("path", ""),
            "visivo:kind": entry.get("kind", ""),
        }

    activity: dict[str, Any] = {}
    used: dict[str, Any] = {}
    was_generated_by: dict[str, Any] = {}
    was_derived_from: dict[str, Any] = {}

    for i, step in enumerate(session.history):
        act_id = f"visivo:activity{i}"
        activity[act_id] = {
            "prov:startTime": step.get("timestamp", ""),
            "visivo:operation": step.get("operation", "") or step.get("endpoint", ""),
            "visivo:params": step.get("params", {}),
        }
        src = step.get("dataset_id", "")
        dst = step.get("result_dataset_id", "")
        if src:
            used[f"visivo:used{i}"] = {
                "prov:activity": act_id,
                "prov:entity": f"visivo:{src}",
            }
        if dst:
            was_generated_by[f"visivo:gen{i}"] = {
                "prov:entity": f"visivo:{dst}",
                "prov:activity": act_id,
            }
            if src:
                was_derived_from[f"visivo:der{i}"] = {
                    "prov:generatedEntity": f"visivo:{dst}",
                    "prov:usedEntity": f"visivo:{src}",
                }

    prov = {
        "prefix": {"visivo": _PROV_PREFIX, "prov": "http://www.w3.org/ns/prov#"},
        "entity": entity,
        "activity": activity,
        "used": used,
        "wasGeneratedBy": was_generated_by,
        "wasDerivedFrom": was_derived_from,
    }
    return {"valid": True, "error": "", "prov": prov}


# ── R-03 enhanced: HMAC-signed named snapshots ───────────────────────────────


class NamedSnapshotRequest(BaseModel):
    label: str = ""


def _snapshot_state(session: Session, label: str) -> dict[str, Any]:
    return {
        "label": label,
        "created_at": _now_iso(),
        "datasets": {
            ds_id: {"path": e.get("path", ""), "kind": e.get("kind", "")}
            for ds_id, e in session.datasets.items()
        },
        "history": list(session.history),
    }


@router.post("/v1/reproducibility/snapshot", tags=["reproducibility"])
async def create_named_snapshot(
    request: NamedSnapshotRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Freeze the current session state into an HMAC-signed, UUID-keyed snapshot."""
    snap_id = str(uuid.uuid4())
    state = _snapshot_state(session, request.label)
    signature = _sign(state)
    session.snapshots[snap_id] = {"state": state, "hmac": signature, "label": request.label}
    return {"valid": True, "error": "", "snapshot_id": snap_id, "hmac": signature, "state": state}


@router.get("/v1/reproducibility/snapshot/{snapshot_id}", tags=["reproducibility"])
async def get_named_snapshot(
    snapshot_id: str,
    provided_hmac: str = Query(default=""),
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Retrieve a snapshot. If ``provided_hmac`` is supplied it must match (else 403)."""
    record = session.snapshots.get(snapshot_id)
    if record is None:
        raise HTTPException(404, f"Unknown snapshot_id '{snapshot_id}'.")
    if provided_hmac and not hmac.compare_digest(provided_hmac, record["hmac"]):
        raise HTTPException(403, "Snapshot HMAC verification failed, state may be tampered.")
    return {
        "valid": True,
        "error": "",
        "snapshot_id": snapshot_id,
        "hmac": record["hmac"],
        "state": record["state"],
    }
