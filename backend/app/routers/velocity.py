"""routers/velocity.py — REST endpoints for the cosmic-flow velocity field (F1).

A velocity field is a 3-component vector grid (not a scalar cube), so it gets its
own open + binary-grid endpoints. The desktop fetches the raw vector grid and
runs vtkStreamTracer / vtkGlyph3D locally (interactive rendering).
"""
import logging
import uuid

from fastapi import APIRouter, Depends, Response
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import _run
from ..sessions import Session, get_session
from ..velocity_basins import velocity_basins_worker
from ..velocity_field import (
    velocity_density_worker,
    velocity_difference_worker,
    velocity_grid_worker,
    velocity_subgrid_worker,
    velocity_summary_worker,
)

router = APIRouter(tags=["velocity"])
logger = logging.getLogger("visivo.velocity.router")
_auth = Depends(verify_token)


class VelocityOpenRequest(BaseModel):
    path: str
    box_mpc: float | None = None  # physical box size in Mpc; None → unit spacing


class VelocityOpenResponse(BaseModel):
    valid: bool
    error: str = ""
    dataset_id: str = ""
    session_id: str = ""
    summary: dict | None = None


class VelocityGridRequest(BaseModel):
    dataset_id: str
    # Display-LOD cap: block-average so the largest grid dimension is <= max_dim.
    # 0 = full resolution. Default keeps transfer + client memory bounded for
    # large (256³/512³) fields.
    max_dim: int = 128


class VelocitySubgridRequest(BaseModel):
    dataset_id: str
    x0: int
    x1: int
    y0: int
    y1: int
    z0: int
    z1: int


class VelocityBasinsRequest(BaseModel):
    dataset_id: str
    basin_max_dim: int = 64   # basins are segmented on a grid this size (coarse, like the science)
    direction: int = 1        # +1 = basins of attraction (superclusters); -1 = repulsion (voids)


class VelocityDensityRequest(BaseModel):
    dataset_id: str
    max_dim: int = 0          # 0 = full-res ∇·v density; >0 block-averages first (LOD)


class VelocityDifferenceRequest(BaseModel):
    dataset_id_a: str
    dataset_id_b: str
    max_dim: int = 0          # 0 = full-res |v_a − v_b|; >0 block-averages first (LOD)


@router.post("/v1/velocity/open", response_model=VelocityOpenResponse, tags=["velocity"])
async def velocity_open(
    request: VelocityOpenRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> VelocityOpenResponse:
    try:
        summary = await _run(velocity_summary_worker, request.path, request.box_mpc)
    except Exception as exc:
        return VelocityOpenResponse(valid=False, error=str(exc))
    dataset_id = f"vf_{uuid.uuid4().hex[:12]}"
    session.datasets[dataset_id] = {
        "path": request.path,
        "kind": "velocity",
        "box_mpc": request.box_mpc,
    }
    logger.info("[velocity] session=%s dataset_id=%s path=%s",
                session.session_id, dataset_id, request.path)
    return VelocityOpenResponse(
        valid=True, dataset_id=dataset_id, session_id=session.session_id, summary=summary
    )


@router.post("/v1/velocity/grid_bin", tags=["velocity"])
async def velocity_grid_bin(
    request: VelocityGridRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    entry = session.datasets.get(request.dataset_id)
    if not entry or entry.get("kind") != "velocity":
        return Response(content=b"unknown velocity dataset",
                        media_type="text/plain", status_code=404)
    try:
        payload = await _run(velocity_grid_worker, entry["path"], entry.get("box_mpc"),
                             request.max_dim)
    except Exception as exc:
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=500)

    raw: bytes = payload["raw_bytes"]
    full = payload.get("full_shape", [payload["nz"], payload["ny"], payload["nx"]])
    headers = {
        "X-Visivo-Width": str(payload["nx"]),
        "X-Visivo-Height": str(payload["ny"]),
        "X-Visivo-Depth": str(payload["nz"]),
        "X-Visivo-Full-Shape": ",".join(str(int(s)) for s in full),  # (nz,ny,nx) pre-LOD
        "X-Visivo-Downsample-Factor": str(payload.get("downsample_factor", 1)),
        "X-Visivo-Components": "3",
        "X-Visivo-Spacing": ",".join(repr(float(s)) for s in payload["spacing"]),
        "X-Visivo-Origin": ",".join(repr(float(o)) for o in payload["origin"]),
        "X-Visivo-Speed-Min": repr(float(payload["speed_min"])),
        "X-Visivo-Speed-Max": repr(float(payload["speed_max"])),
        "X-Visivo-Full-Speed-Min": repr(float(payload.get("full_speed_min", payload["speed_min"]))),
        "X-Visivo-Full-Speed-Max": repr(float(payload.get("full_speed_max", payload["speed_max"]))),
        "X-Visivo-Unit": str(payload["unit"]),
        "X-Visivo-Frame": str(payload["frame"]),
        "X-Visivo-Byte-Order": "little",
        "Content-Length": str(len(raw)),
    }
    return Response(content=raw, media_type="application/octet-stream", headers=headers)


@router.post("/v1/velocity/basins_bin", tags=["velocity"])
async def velocity_basins_bin(
    request: VelocityBasinsRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Gravitational-basin (watershed) label volume for a velocity field. Returns
    an int32 label grid (0 = unassigned, 1..N = basins) on a coarse basin grid."""
    entry = session.datasets.get(request.dataset_id)
    if not entry or entry.get("kind") != "velocity":
        return Response(content=b"unknown velocity dataset",
                        media_type="text/plain", status_code=404)
    try:
        payload = await _run(velocity_basins_worker, entry["path"], entry.get("box_mpc"),
                             request.basin_max_dim, request.direction)
    except Exception as exc:
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=500)

    raw: bytes = payload["raw_bytes"]
    headers = {
        "X-Visivo-Width": str(payload["nx"]),
        "X-Visivo-Height": str(payload["ny"]),
        "X-Visivo-Depth": str(payload["nz"]),
        "X-Visivo-Components": "1",
        "X-Visivo-Scalar-Type": "int32",
        "X-Visivo-N-Basins": str(payload["n_basins"]),
        "X-Visivo-Byte-Order": "little",
        "Content-Length": str(len(raw)),
    }
    return Response(content=raw, media_type="application/octet-stream", headers=headers)


@router.post("/v1/velocity/density_bin", tags=["velocity"])
async def velocity_density_bin(
    request: VelocityDensityRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Density-from-divergence (∇·v) scalar grid for a velocity field. Returns a
    little-endian float32 volume (x fastest); ∇·v < 0 = overdensity, > 0 = void.
    Use it for a full-res density on large fields the display LOD can't resolve."""
    entry = session.datasets.get(request.dataset_id)
    if not entry or entry.get("kind") != "velocity":
        return Response(content=b"unknown velocity dataset",
                        media_type="text/plain", status_code=404)
    try:
        payload = await _run(velocity_density_worker, entry["path"], entry.get("box_mpc"),
                             request.max_dim)
    except Exception as exc:
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=500)

    raw: bytes = payload["raw_bytes"]
    headers = {
        "X-Visivo-Width": str(payload["nx"]),
        "X-Visivo-Height": str(payload["ny"]),
        "X-Visivo-Depth": str(payload["nz"]),
        "X-Visivo-Components": "1",
        "X-Visivo-Scalar-Type": "float32",
        "X-Visivo-Spacing": ",".join(repr(float(s)) for s in payload["spacing"]),
        "X-Visivo-Origin": ",".join(repr(float(o)) for o in payload["origin"]),
        "X-Visivo-Div-Min": repr(float(payload["div_min"])),
        "X-Visivo-Div-Max": repr(float(payload["div_max"])),
        "X-Visivo-Unit": str(payload["unit"]),
        "X-Visivo-Byte-Order": "little",
        # Density-calibration honesty (D): the grid is raw ∇·v (uncalibrated); a
        # client turns it into a linear-theory overdensity δ ≈ -∇·v · factor.
        "X-Visivo-Calibrated": "0",
        "Content-Length": str(len(raw)),
    }
    _factor = payload.get("linear_density_factor")
    if _factor is not None:
        headers["X-Visivo-Density-Factor"] = repr(float(_factor))
    return Response(content=raw, media_type="application/octet-stream", headers=headers)


@router.post("/v1/velocity/difference_bin", tags=["velocity"])
async def velocity_difference_bin(
    request: VelocityDifferenceRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Multi-field compare (R-20): |v_a − v_b| scalar grid for two velocity fields
    (e.g. CF3 vs CF4, reconstruction vs simulation). Little-endian float32 volume
    (x fastest). The two fields must share a grid shape (else 422)."""
    ea = session.datasets.get(request.dataset_id_a)
    eb = session.datasets.get(request.dataset_id_b)
    if not ea or ea.get("kind") != "velocity" or not eb or eb.get("kind") != "velocity":
        return Response(content=b"unknown velocity dataset(s)",
                        media_type="text/plain", status_code=404)
    try:
        payload = await _run(velocity_difference_worker, ea["path"], eb["path"],
                             ea.get("box_mpc"), request.max_dim, eb.get("box_mpc"))
    except ValueError as exc:
        return Response(content=str(exc).encode("utf-8"), media_type="text/plain",
                        status_code=422)
    except Exception as exc:  # noqa: BLE001
        return Response(content=str(exc).encode("utf-8"), media_type="text/plain",
                        status_code=500)
    raw: bytes = payload["raw_bytes"]
    headers = {
        "X-Visivo-Width": str(payload["nx"]),
        "X-Visivo-Height": str(payload["ny"]),
        "X-Visivo-Depth": str(payload["nz"]),
        "X-Visivo-Components": "1",
        "X-Visivo-Scalar-Type": "float32",
        "X-Visivo-Spacing": ",".join(repr(float(s)) for s in payload["spacing"]),
        "X-Visivo-Origin": ",".join(repr(float(o)) for o in payload["origin"]),
        # A magnitude difference |v_a − v_b| is a scalar range, not a signed
        # divergence — use the generic Range headers, not the ∇·v Div headers.
        "X-Visivo-Range-Min": repr(float(payload["diff_min"])),
        "X-Visivo-Range-Max": repr(float(payload["diff_max"])),
        "X-Visivo-Unit": str(payload["unit"]),
        "X-Visivo-Byte-Order": "little",
        "Content-Length": str(len(raw)),
    }
    # Surface unit/frame-mismatch (and per-field) warnings so a mixed-unit diff is
    # never silent. HTTP header values are latin-1 single-line: collapse newlines
    # and drop any non-latin-1 char (e.g. an em-dash) so encoding can't fail.
    _warnings = payload.get("warnings", [])
    if _warnings:
        _joined = "; ".join(_warnings).replace("\r", " ").replace("\n", " ")
        headers["X-Visivo-Warnings"] = _joined.encode("latin-1", "replace").decode("latin-1")
    return Response(content=raw, media_type="application/octet-stream", headers=headers)


@router.post("/v1/velocity/subgrid_bin", tags=["velocity"])
async def velocity_subgrid_bin(
    request: VelocitySubgridRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Full-resolution sub-region (ROI on demand) of a velocity field."""
    entry = session.datasets.get(request.dataset_id)
    if not entry or entry.get("kind") != "velocity":
        return Response(content=b"unknown velocity dataset",
                        media_type="text/plain", status_code=404)
    try:
        payload = await _run(velocity_subgrid_worker, entry["path"],
                             request.x0, request.x1, request.y0, request.y1,
                             request.z0, request.z1, entry.get("box_mpc"))
    except Exception as exc:
        return Response(content=str(exc).encode("utf-8"),
                        media_type="text/plain", status_code=500)

    raw: bytes = payload["raw_bytes"]
    headers = {
        "X-Visivo-Width": str(payload["nx"]),
        "X-Visivo-Height": str(payload["ny"]),
        "X-Visivo-Depth": str(payload["nz"]),
        "X-Visivo-Components": "3",
        # Clamped full-res start indices so the client can place the ROI exactly.
        "X-Visivo-Roi-X0": str(payload["x0"]),
        "X-Visivo-Roi-Y0": str(payload["y0"]),
        "X-Visivo-Roi-Z0": str(payload["z0"]),
        "X-Visivo-Full-Shape": ",".join(str(int(s)) for s in payload["full_shape"]),
        "X-Visivo-Speed-Min": repr(float(payload["speed_min"])),
        "X-Visivo-Speed-Max": repr(float(payload["speed_max"])),
        "X-Visivo-Byte-Order": "little",
        "Content-Length": str(len(raw)),
    }
    return Response(content=raw, media_type="application/octet-stream", headers=headers)
