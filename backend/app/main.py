"""
main.py – VisIVO Backend v1

Changes vs original (next-server branch):
  R1  Auth         – every route requires X-Visivo-Token (see auth.py)
  R2  Concurrency  – CPU-bound work runs in a ProcessPoolExecutor so the
                     ASGI event-loop is never blocked
  R3  Sessions     – datasets are stored per-session; the client must echo
                     the X-Visivo-Session header returned in /v1/datasets/open
  R4  Lazy I/O     – worker functions open FITS with memmap=True and read
                     only the required pixels (see compute.py)
  R8  API quality  – all routes prefixed /v1/, standardised error schema,
                     X-Request-ID header on every response

The legacy un-versioned paths (/health, /files/*, …) are intentionally
removed to enforce /v1/ from the start. The Qt client must be updated to
prepend /v1/ to every request URL (Settings dialog → backend URL field).
"""

from __future__ import annotations

import asyncio
import base64
import csv
import functools
import logging
import os
import struct
import threading
import uuid
from concurrent.futures import ProcessPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd
from astropy.io import fits
from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request, Response, status
from fastapi.responses import JSONResponse
from pydantic import BaseModel

from .auth import verify_token
from .fits_dataset import ScientificFitsDataset
from .product_cache import PRODUCT_CACHE
from .sessions import REGISTRY, Session, get_session
from .tasks import TASKS

# ── Logging ───────────────────────────────────────────────────────────────────

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
logger = logging.getLogger("visivo.backend")

# ── Process pool (R2) ─────────────────────────────────────────────────────────

def _default_worker_count() -> int:
    """
    Keep the backend conservative by default.

    On shared HPC/login nodes an unbounded cpu_count()-sized pool is too aggressive for
    data-heavy FITS workloads. Users can still override via VISIVO_WORKERS.
    """
    return max(1, min(os.cpu_count() or 4, 4))


_WORKERS = int(os.environ.get("VISIVO_WORKERS", str(_default_worker_count())))


def _pool_initializer() -> None:  # pragma: no cover
    """Silence VTK startup noise in worker processes."""
    import logging as _l
    _l.getLogger("vtkmodules").setLevel(_l.ERROR)


_POOL = ProcessPoolExecutor(max_workers=_WORKERS, initializer=_pool_initializer)

# ── FastAPI app ───────────────────────────────────────────────────────────────

app = FastAPI(
    title="VisIVO Backend",
    version="1.0.0",
    description="Astrophysical data visualization backend for ViaLactea Visual Analytics.",
    docs_url="/v1/docs",
    redoc_url="/v1/redoc",
    openapi_url="/v1/openapi.json",
)

_FITS_SUFFIXES = {".fits", ".fit", ".fts"}


# ── VTK availability check (R startup) ───────────────────────────────────────

_VTK_AVAILABLE = False
try:
    import vtk as _vtk  # noqa: F401
    _VTK_AVAILABLE = True
except ImportError:
    logger.warning(
        "[startup] VTK Python bindings not available – "
        "/v1/products/isosurface will return 503."
    )

# ── Middleware: X-Request-ID (R8) ─────────────────────────────────────────────


@app.middleware("http")
async def attach_request_id(request: Request, call_next):
    request_id = str(uuid.uuid4())
    response: Response = await call_next(request)
    response.headers["X-Request-ID"] = request_id
    return response


# ── Standard error schema (R8) ────────────────────────────────────────────────


class ErrorDetail(BaseModel):
    error: str
    detail: str
    request_id: str = ""


def _error(detail: str, status_code: int = 400, error: str = "BadRequest") -> JSONResponse:
    return JSONResponse(
        status_code=status_code,
        content=ErrorDetail(error=error, detail=detail).model_dump(),
    )


# ── Pydantic models ───────────────────────────────────────────────────────────


class FileEntry(BaseModel):
    name: str
    path: str
    type: str
    size: int = 0
    modified_time: str = ""
    is_fits: bool = False


class FilesListResponse(BaseModel):
    valid: bool
    error: str
    current_path: str = ""
    entries: list[FileEntry]


class FileHeaderRequest(BaseModel):
    path: str


class FileHeaderResponse(BaseModel):
    valid: bool
    error: str
    cards: list[str]


class OpenDatasetRequest(BaseModel):
    path: str


class OpenDatasetResponse(BaseModel):
    valid: bool
    error: str
    dataset_id: str = ""
    session_id: str = ""           # R3: client must echo as X-Visivo-Session
    kind: str = ""
    active_axes: int = 0
    width: int = 0
    height: int = 0
    depth: int = 0
    degenerate_axes_summary: str = ""
    wcs_status: str = "ok"
    wcs_warning_message: str = ""
    wcs_sanitized_axes: list[int] = []
    spectral_axis_type: str = ""
    spectral_axis_unit: str = ""
    spacing: list[float] = [1.0, 1.0, 1.0]
    origin: list[float] = [0.0, 0.0, 0.0]
    ctype: list[str] = ["", "", ""]
    cunit: list[str] = ["", "", ""]
    crval: list[float] = [0.0, 0.0, 0.0]
    crpix: list[float] = [1.0, 1.0, 1.0]
    cdelt: list[float] = [1.0, 1.0, 1.0]


class MomentProductRequest(BaseModel):
    dataset_id: str
    moment_order: int
    channel_start: int = 0
    channel_end: int = 0
    mask_enabled: bool = False
    threshold_value: float = 0.0


class MomentProductResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    spectral_axis_type: str = ""
    spectral_axis_unit: str = ""
    moment_unit: str = ""
    bunit: str = ""
    data_base64: str = ""


class CubePreviewRequest(BaseModel):
    dataset_id: str
    downsample: int = 1


class CubePreviewResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    depth: int = 0
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    data_base64: str = ""


class CubeSliceRequest(BaseModel):
    dataset_id: str
    axis: str
    index: int


class CubeSliceResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    compression: str = ""
    data_base64: str = ""


class CubeSubvolumeRequest(BaseModel):
    dataset_id: str
    x0: int
    x1: int
    y0: int
    y1: int
    z0: int
    z1: int


class CubeSubvolumeResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    depth: int = 0
    scalar_type: str = ""
    data_base64: str = ""


class CubePvRequest(BaseModel):
    dataset_id: str
    vertices: list[list[int]]
    width_pixels: int = 1


class CubePvResponse(BaseModel):
    valid: bool
    error: str
    num_samples: int = 0
    depth: int = 0
    scalar_type: str = ""
    compression: str = ""
    positions_base64: str = ""
    positions_arcsec_base64: str = ""
    pixel_scale_arcsec_per_pixel: float = 0.0
    spatial_unit: str = "pixel"
    data_base64: str = ""
    computed_on: str = ""
    width_pixels: int = 1
    vertex_count: int = 0
    total_length: float = 0.0
    valid_samples: int = 0
    spectral_axis_type: str = ""
    spectral_axis_unit: str = ""
    bunit: str = ""
    beam_major: float | None = None
    beam_minor: float | None = None
    beam_pa: float | None = None


class TaskCreateResponse(BaseModel):
    valid: bool
    error: str
    task_id: str = ""
    status: str = ""
    cache_hit: bool = False


class TaskStatusResponse(BaseModel):
    valid: bool
    error: str
    task_id: str = ""
    operation: str = ""
    status: str = ""
    created_at: str = ""
    updated_at: str = ""
    progress: float = 0.0
    cache_hit: bool = False
    result: dict[str, Any] | None = None


class ImageFullRequest(BaseModel):
    dataset_id: str


class ImagePreviewRequest(BaseModel):
    dataset_id: str
    max_longest_side: int = 1536


class ImageFullResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    full_width: int = 0
    full_height: int = 0
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    is_preview: bool = False
    preview_scale_factor: float = 1.0
    compression: str = ""
    data_base64: str = ""


class CubeNoiseRequest(BaseModel):
    dataset_id: str
    x0: int
    x1: int
    y0: int
    y1: int
    channel_start: int = 0
    channel_end: int = 0


class CubeNoiseResponse(BaseModel):
    valid: bool
    error: str
    channel_start: int = 0
    channel_end: int = 0
    num_channels: int = 0
    mad: list[float] = []
    sigma: list[float] = []
    region: dict = {}


class CatalogueOpenRequest(BaseModel):
    path: str
    format: str = "csv"


class CatalogueFieldInfo(BaseModel):
    name: str
    type: str


class CatalogueCoordinateFields(BaseModel):
    ra: str = ""
    dec: str = ""


class CatalogueOpenResponse(BaseModel):
    valid: bool
    error: str
    dataset_id: str = ""
    session_id: str = ""
    num_rows: int = 0
    fields: list[CatalogueFieldInfo] = []
    coordinate_fields: CatalogueCoordinateFields = CatalogueCoordinateFields()


class CatalogueSubsetRequest(BaseModel):
    dataset_id: str
    max_rows: int = 100000


class CatalogueSubsetResponse(BaseModel):
    valid: bool
    error: str
    rows: list[dict[str, Any]] = []


# ── Tabular query models (shared for catalogue + VBT point) ──────────────────

class TabularFilter(BaseModel):
    field: str
    op: str    # "=", "!=", ">", ">=", "<", "<=", "contains", "in"
    value: str


class TabularSortSpec(BaseModel):
    field: str
    direction: str = "asc"   # "asc" or "desc"


class TabularQueryRequest(BaseModel):
    dataset_id: str
    limit: int = 100000
    offset: int = 0
    filters: list[TabularFilter] = []
    sort: TabularSortSpec | None = None


class TabularQueryResponse(BaseModel):
    valid: bool
    error: str
    total_rows: int = 0
    returned_rows: int = 0
    # catalogue: row-oriented dicts
    rows: list[dict[str, Any]] = []
    # VBT point: column-oriented base64 float64-LE
    num_rows: int = 0
    fields: list[str] = []
    columns: dict[str, str] = {}


# ── VBT models ────────────────────────────────────────────────────────────────

class VbtOpenRequest(BaseModel):
    path: str
    format: str = ""


class VbtFieldInfo(BaseModel):
    name: str
    type: str  # "float32" or "float64"


class VbtOpenResponse(BaseModel):
    valid: bool
    error: str
    dataset_id: str = ""
    session_id: str = ""
    kind: str = ""          # "point" or "volume"
    fields: list[VbtFieldInfo] = []
    num_rows: int = 0
    scalar_type: str = ""
    dimensions: list[int] = []    # [nx, ny, nz] for volume
    spacing: list[float] = []     # [sx, sy, sz] for volume


class VbtSubsetRequest(BaseModel):
    dataset_id: str
    max_rows: int = 0        # 0 → all rows
    field_name: str = ""     # volume: which field to return; empty → first field


class VbtSubsetResponse(BaseModel):
    valid: bool
    error: str
    kind: str = ""
    num_rows: int = 0
    fields: list[str] = []
    # point data: {field_name: base64-encoded float64-LE column}
    columns: dict[str, str] = {}
    # volume data: single-field block
    field_name: str = ""
    block: str = ""         # base64-encoded float32-LE voxel block
    dimensions: list[int] = []
    spacing: list[float] = []


class IsosurfaceProductRequest(BaseModel):
    dataset_id: str
    threshold: float


class IsosurfaceProductResponse(BaseModel):
    valid: bool
    error: str
    compression: str = ""
    points_base64: str = ""
    polys_base64: str = ""
    num_points: int = 0
    num_polys: int = 0


# ── Internal helpers ──────────────────────────────────────────────────────────


def _apply_tabular_query(
    df: "pd.DataFrame",
    filters: list[TabularFilter],
    sort: TabularSortSpec | None,
    limit: int,
    offset: int,
) -> tuple["pd.DataFrame", int]:
    """Apply filters, sort, and limit/offset to a DataFrame.

    Returns ``(result_df, total_rows_before_pagination)``.
    Raises ``ValueError`` for unknown fields or operators.
    """
    _NUMERIC_OPS = {"=", "!=", ">", ">=", "<", "<="}
    for f in filters:
        if f.field not in df.columns:
            raise ValueError(f"Unknown field: {f.field!r}")
        col = df[f.field]
        is_numeric = pd.api.types.is_numeric_dtype(col)
        if f.op in _NUMERIC_OPS:
            if is_numeric:
                try:
                    num_val = float(f.value)
                except ValueError:
                    raise ValueError(
                        f"Cannot convert {f.value!r} to number for field {f.field!r}"
                    )
                if f.op == "=":
                    df = df[col == num_val]
                elif f.op == "!=":
                    df = df[col != num_val]
                elif f.op == ">":
                    df = df[col > num_val]
                elif f.op == ">=":
                    df = df[col >= num_val]
                elif f.op == "<":
                    df = df[col < num_val]
                elif f.op == "<=":
                    df = df[col <= num_val]
            else:
                s = col.astype(str)
                if f.op == "=":
                    df = df[s == f.value]
                elif f.op == "!=":
                    df = df[s != f.value]
                elif f.op == ">":
                    df = df[s > f.value]
                elif f.op == ">=":
                    df = df[s >= f.value]
                elif f.op == "<":
                    df = df[s < f.value]
                elif f.op == "<=":
                    df = df[s <= f.value]
        elif f.op == "contains":
            df = df[col.astype(str).str.contains(f.value, case=False, na=False)]
        elif f.op == "in":
            raw_vals = [v.strip() for v in f.value.split(",")]
            if is_numeric:
                try:
                    num_vals = [float(v) for v in raw_vals]
                    df = df[col.isin(num_vals)]
                except ValueError:
                    df = df[col.astype(str).isin(raw_vals)]
            else:
                df = df[col.astype(str).isin(raw_vals)]
        else:
            raise ValueError(f"Unknown filter operator: {f.op!r}")

    if sort and sort.field in df.columns:
        df = df.sort_values(
            by=sort.field, ascending=(sort.direction.lower() != "desc"), na_position="last"
        )

    total = len(df)
    if offset:
        df = df.iloc[offset:]
    if limit > 0:
        df = df.iloc[:limit]
    return df, total


def _normalize_path(raw: str) -> Path:
    if not raw:
        return Path.home()
    return Path(raw).expanduser().resolve()


def _parse_vbt_header(head_path: Path) -> dict:
    """Parse a VisIVO Binary Table .head file and return metadata dict."""
    with open(head_path, "r") as fh:
        lines = [ln.strip() for ln in fh.readlines() if ln.strip()]
    if len(lines) < 4:
        raise ValueError(f"VBT header too short: expected at least 4 lines, got {len(lines)}")

    scalar_str = lines[0].lower()
    if scalar_str in ("f", "float"):
        scalar_type = "float32"
        np_endian_code = "<f4"
    elif scalar_str in ("d", "double"):
        scalar_type = "float64"
        np_endian_code = "<f8"
    else:
        raise ValueError(f"Unknown VBT scalar type: '{lines[0]}'")

    try:
        field_count = int(lines[1])
    except ValueError:
        raise ValueError(f"Invalid field count: '{lines[1]}'")
    if field_count <= 0:
        raise ValueError(f"Field count must be positive, got {field_count}")

    tokens = lines[2].split()
    if len(tokens) == 1:
        kind = "point"
        num_rows = int(tokens[0])
        dimensions: list[int] | None = None
        spacing: list[float] | None = None
    elif len(tokens) == 7:
        kind = "volume"
        num_rows = int(tokens[0])
        nx, ny, nz = int(tokens[1]), int(tokens[2]), int(tokens[3])
        sx, sy, sz = float(tokens[4]), float(tokens[5]), float(tokens[6])
        if num_rows != nx * ny * nz:
            raise ValueError(
                f"VBT voxel count mismatch: header says {num_rows}, dims give {nx * ny * nz}"
            )
        dimensions = [nx, ny, nz]
        spacing = [sx, sy, sz]
    else:
        raise ValueError(f"Unexpected token count on row-spec line: {len(tokens)}")

    endian_str = lines[3].lower()
    if endian_str in ("l", "little"):
        file_endian = "little"
    elif endian_str in ("b", "big"):
        file_endian = "big"
    else:
        raise ValueError(f"Unknown VBT endian: '{lines[3]}'")

    if len(lines) < 4 + field_count:
        raise ValueError(
            f"VBT header has {len(lines)} lines but expected at least {4 + field_count}"
        )
    field_names = lines[4 : 4 + field_count]

    return {
        "kind": kind,
        "scalar_type": scalar_type,
        "np_endian_code": np_endian_code,  # numpy dtype string including endian mark
        "file_endian": file_endian,
        "field_count": field_count,
        "num_rows": num_rows,
        "dimensions": dimensions,
        "spacing": spacing,
        "field_names": field_names,
    }


def _is_fits(path: Path) -> bool:
    return path.is_file() and path.suffix.lower() in _FITS_SUFFIXES


def _normalize_catalogue_field(raw: str) -> str:
    return "".join(ch.upper() for ch in raw if ch.isalnum())


def _catalogue_csv_metadata(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle)
        headers = next(reader, [])
    if not headers:
        raise ValueError("CSV catalogue is empty.")

    frame = pd.read_csv(path, nrows=256)
    fields: list[dict[str, str]] = []
    normalized_to_actual: dict[str, str] = {}
    for column in frame.columns:
        name = str(column)
        normalized_to_actual[_normalize_catalogue_field(name)] = name
        dtype = frame[column].dtype
        if pd.api.types.is_bool_dtype(dtype):
            field_type = "bool"
        elif pd.api.types.is_integer_dtype(dtype):
            field_type = "int"
        elif pd.api.types.is_float_dtype(dtype):
            field_type = "float"
        else:
            field_type = "string"
        fields.append({"name": name, "type": field_type})

    ra_field = ""
    dec_field = ""
    for alias in ("RA", "RADEG", "RAJ2000", "RAJ2000OBS", "ALPHA", "RAICRS"):
        if alias in normalized_to_actual:
            ra_field = normalized_to_actual[alias]
            break
    for alias in ("DEC", "DE", "DECDEG", "DEJ2000", "DECJ2000", "DELTA", "DECICRS"):
        if alias in normalized_to_actual:
            dec_field = normalized_to_actual[alias]
            break
    if not ra_field or not dec_field:
        raise ValueError("CSV catalogue requires RA and DEC columns.")

    row_count = max(0, sum(1 for _ in path.open("r", encoding="utf-8")) - 1)
    return {
        "kind": "catalogue",
        "format": "csv",
        "num_rows": row_count,
        "fields": fields,
        "coordinate_fields": {"ra": ra_field, "dec": dec_field},
    }


async def _run(fn, *args) -> Any:
    """Run a CPU-bound function in the process pool without blocking the loop."""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(_POOL, fn, *args)




async def _run_with_limit(session: Session, fn, *args) -> Any:
    """Like _run(), but enforces per-session concurrent task limit (HTTP 429 on overflow)."""
    if not session.try_acquire_task_slot():
        raise HTTPException(
            status_code=status.HTTP_429_TOO_MANY_REQUESTS,
            detail=(
                "Too many concurrent compute tasks for this session. "
                "Wait for a running task to finish before submitting another."
            ),
        )
    try:
        return await _run(fn, *args)
    finally:
        session.release_task_slot()


async def _moment_product_payload(entry: dict[str, Any], request: MomentProductRequest) -> tuple[dict[str, Any], bool]:
    from .compute import worker_moment

    dataset = ScientificFitsDataset(entry["path"])
    cache_params = {
        "moment_order": request.moment_order,
        "channel_start": request.channel_start,
        "channel_end": request.channel_end,
        "mask_enabled": request.mask_enabled,
        "threshold_value": request.threshold_value,
    }
    cache_key, param_hash = PRODUCT_CACHE.make_key(dataset.path, "moment", cache_params)
    cached = PRODUCT_CACHE.get(cache_key)
    if cached is not None:
        return dict(cached.payload), True

    result = await _run(
        worker_moment,
        dataset.path,
        request.moment_order,
        request.channel_start,
        request.channel_end,
        request.mask_enabled,
        request.threshold_value,
    )
    PRODUCT_CACHE.put(
        key=cache_key,
        operation="moment",
        dataset_path=dataset.path,
        parameter_hash=param_hash,
        payload=dict(result),
        scientific_metadata={
            "spectral_axis_type": result.get("spectral_axis_type", ""),
            "spectral_axis_unit": result.get("spectral_axis_unit", ""),
            "moment_unit": result.get("moment_unit", ""),
            "bunit": result.get("bunit", ""),
        },
        provenance_metadata=dataset.provenance_context(),
    )
    return result, False


async def _pv_product_payload(entry: dict[str, Any], request: CubePvRequest) -> tuple[dict[str, Any], bool]:
    from .compute import worker_pv

    dataset = ScientificFitsDataset(entry["path"])
    cache_params = {
        "vertices": request.vertices,
        "width_pixels": max(1, int(request.width_pixels)),
    }
    cache_key, param_hash = PRODUCT_CACHE.make_key(dataset.path, "pv", cache_params)
    cached = PRODUCT_CACHE.get(cache_key)
    if cached is not None:
        return dict(cached.payload), True

    result = await _run(
        worker_pv,
        dataset.path,
        request.vertices,
        max(1, int(request.width_pixels)),
    )
    PRODUCT_CACHE.put(
        key=cache_key,
        operation="pv",
        dataset_path=dataset.path,
        parameter_hash=param_hash,
        payload=dict(result),
        scientific_metadata={
            "spectral_axis_type": result.get("spectral_axis_type", ""),
            "spectral_axis_unit": result.get("spectral_axis_unit", ""),
            "bunit": result.get("bunit", ""),
            "beam_major": result.get("beam_major"),
            "beam_minor": result.get("beam_minor"),
            "beam_pa": result.get("beam_pa"),
        },
        provenance_metadata=dataset.provenance_context(),
    )
    return result, False


def _require_dataset(session: Session, dataset_id: str) -> dict[str, Any]:
    entry = session.datasets.get(dataset_id)
    if not entry:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Unknown dataset_id '{dataset_id}'. Open the dataset first via /v1/datasets/open.",
        )
    return entry


def _require_cube_path(session: Session, dataset_id: str) -> str:
    entry = _require_dataset(session, dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="This endpoint requires a spectral cube dataset.",
        )
    return entry["path"]


# ── FITS metadata (runs in main process – no heavy I/O) ───────────────────────


def _fits_metadata(path: Path) -> tuple[str, dict[str, Any]]:
    dataset = ScientificFitsDataset(path)
    geometry = dataset.geometry_metadata()
    return str(geometry["kind"]), geometry


# ── Routes ────────────────────────────────────────────────────────────────────
# All routes require authentication (global dependency).
# Heavy routes additionally use `_run()` to avoid blocking the event-loop.

_auth = Depends(verify_token)


@app.get("/v1/health", tags=["meta"])
async def health(_: None = _auth) -> dict:
    cache_stats = PRODUCT_CACHE.stats()
    task_stats = TASKS.stats()
    return {
        "ok": True,
        "workers": _WORKERS,
        "active_sessions": REGISTRY.stats()["active_sessions"],
        "product_cache_entries": cache_stats["entries"],
        "product_cache_capacity": cache_stats["max_entries"],
        "task_registry_entries": task_stats["entries"],
        "task_ttl_enabled": task_stats["ttl_enabled"],
        "task_ttl_seconds": task_stats["ttl_seconds"],
        "timestamp": datetime.now(tz=timezone.utc).isoformat(),
    }


@app.get("/v1/sessions", tags=["meta"])
async def session_stats(_: None = _auth) -> dict:
    """Expose session registry statistics (admin endpoint)."""
    return REGISTRY.stats()


@app.get("/v1/files/list", response_model=FilesListResponse, tags=["files"])
async def list_files(
    path: str = Query(""),
    _: None = _auth,
) -> FilesListResponse:
    try:
        directory = _normalize_path(path)
    except Exception:
        return FilesListResponse(valid=False, error="Invalid path.", current_path="", entries=[])

    if not directory.exists() or not directory.is_dir():
        return FilesListResponse(valid=False, error="Directory not found.", current_path="", entries=[])

    try:
        children = sorted(directory.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower()))
    except OSError as exc:
        return FilesListResponse(valid=False, error=str(exc), current_path=str(directory), entries=[])

    entries: list[FileEntry] = []
    for child in children:
        try:
            st = child.stat()
            size = int(st.st_size)
            mtime = datetime.fromtimestamp(st.st_mtime, tz=timezone.utc).isoformat()
        except OSError:
            size, mtime = 0, ""
        ftype = "directory" if child.is_dir() else "file"
        entries.append(FileEntry(
            name=child.name,
            path=str(child),
            type=ftype,
            size=size,
            modified_time=mtime,
            is_fits=_is_fits(child) if child.is_file() else False,
        ))

    return FilesListResponse(valid=True, error="", current_path=str(directory), entries=entries)


@app.post("/v1/files/header", response_model=FileHeaderResponse, tags=["files"])
async def file_header(request: FileHeaderRequest, _: None = _auth) -> FileHeaderResponse:
    try:
        path = _normalize_path(request.path)
        if not _is_fits(path):
            raise ValueError("FITS file not found.")
        with fits.open(str(path), memmap=True) as hdul:
            cards = [str(card) for card in hdul[0].header.cards]
    except Exception as exc:
        return FileHeaderResponse(valid=False, error=str(exc), cards=[])
    return FileHeaderResponse(valid=True, error="", cards=cards)


@app.post("/v1/datasets/open", response_model=OpenDatasetResponse, tags=["datasets"])
async def open_dataset(
    request: OpenDatasetRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> OpenDatasetResponse:
    path = _normalize_path(request.path)
    if not _is_fits(path):
        return OpenDatasetResponse(valid=False, error="FITS file not found.")
    try:
        kind, geometry = _fits_metadata(path)
    except Exception as exc:
        return OpenDatasetResponse(valid=False, error=str(exc))

    dataset_id = f"ds_{uuid.uuid4().hex[:12]}"
    session.datasets[dataset_id] = {"path": str(path), "kind": kind, **geometry}
    logger.info(
        "[open] session=%s dataset_id=%s kind=%s path=%s",
        session.session_id, dataset_id, kind, path,
    )
    return OpenDatasetResponse(
        valid=True,
        error="",
        dataset_id=dataset_id,
        session_id=session.session_id,
        kind=kind,
        active_axes=int(geometry.get("active_axes", 0)),
        width=geometry["width"],
        height=geometry["height"],
        depth=geometry["depth"],
        degenerate_axes_summary=str(geometry.get("degenerate_axes_summary", "")),
        wcs_status=str(geometry.get("wcs_status", "ok")),
        wcs_warning_message=str(geometry.get("wcs_warning_message", "")),
        wcs_sanitized_axes=list(geometry.get("wcs_sanitized_axes", [])),
        spectral_axis_type=str(geometry.get("spectral_axis_type", "")),
        spectral_axis_unit=str(geometry.get("spectral_axis_unit", "")),
        spacing=geometry["spacing"],
        origin=geometry["origin"],
        ctype=geometry["ctype"],
        cunit=geometry["cunit"],
        crval=geometry["crval"],
        crpix=geometry["crpix"],
        cdelt=geometry["cdelt"],
    )


@app.post("/v1/catalogue/open", response_model=CatalogueOpenResponse, tags=["catalogue"])
async def open_catalogue(
    request: CatalogueOpenRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CatalogueOpenResponse:
    path = _normalize_path(request.path)
    if request.format.lower() != "csv":
        return CatalogueOpenResponse(valid=False, error="Only CSV catalogue format is supported.")
    if not path.exists() or not path.is_file():
        return CatalogueOpenResponse(valid=False, error="Catalogue file not found.")
    try:
        metadata = _catalogue_csv_metadata(path)
    except Exception as exc:
        return CatalogueOpenResponse(valid=False, error=str(exc))

    # Load the full CSV once at open time so subsequent queries read from memory.
    try:
        dataframe = pd.read_csv(path)
    except Exception as exc:
        return CatalogueOpenResponse(valid=False, error=f"Failed to load CSV: {exc}")

    dataset_id = f"cat_{uuid.uuid4().hex[:12]}"
    session.datasets[dataset_id] = {"path": str(path), "dataframe": dataframe, **metadata}
    logger.info(
        "[catalogue-open] session=%s dataset_id=%s rows=%d cols=%d path=%s",
        session.session_id,
        dataset_id,
        len(dataframe),
        len(dataframe.columns),
        path,
    )
    return CatalogueOpenResponse(
        valid=True,
        error="",
        dataset_id=dataset_id,
        session_id=session.session_id,
        num_rows=int(metadata["num_rows"]),
        fields=[CatalogueFieldInfo(**field) for field in metadata["fields"]],
        coordinate_fields=CatalogueCoordinateFields(**metadata["coordinate_fields"]),
    )


@app.post("/v1/catalogue/subset", response_model=CatalogueSubsetResponse, tags=["catalogue"])
async def catalogue_subset(
    request: CatalogueSubsetRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CatalogueSubsetResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "catalogue":
            raise HTTPException(422, "This endpoint requires a catalogue dataset.")
        df: pd.DataFrame = entry["dataframe"]
        max_rows = max(1, int(request.max_rows))
        frame = df.iloc[:max_rows]
        rows = frame.replace({np.nan: None}).to_dict(orient="records")
    except HTTPException:
        raise
    except Exception as exc:
        return CatalogueSubsetResponse(valid=False, error=str(exc))
    return CatalogueSubsetResponse(valid=True, error="", rows=rows)


@app.post("/v1/catalogue/query", response_model=TabularQueryResponse, tags=["catalogue"])
async def catalogue_query(
    request: TabularQueryRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> TabularQueryResponse:
    """Query a catalogue with optional filters, sort, and pagination."""
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "catalogue":
            raise HTTPException(422, "This endpoint requires a catalogue dataset.")
        # Use the DataFrame cached at open time – no disk I/O.
        # _apply_tabular_query operates on a copy so the original is never mutated.
        df, total = _apply_tabular_query(
            entry["dataframe"], request.filters, request.sort, request.limit, request.offset
        )
        rows = df.replace({np.nan: None}).to_dict(orient="records")
    except HTTPException:
        raise
    except Exception as exc:
        return TabularQueryResponse(valid=False, error=str(exc))
    return TabularQueryResponse(
        valid=True,
        error="",
        total_rows=total,
        returned_rows=len(rows),
        rows=rows,
    )


# ── VBT endpoints ─────────────────────────────────────────────────────────────


@app.post("/v1/vbt/open", response_model=VbtOpenResponse, tags=["vbt"])
async def vbt_open(
    request: VbtOpenRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> VbtOpenResponse:
    """Register a VisIVO Binary Table (.head + binary) with the backend.

    Returns field schema and geometry metadata; the client receives a
    dataset_id to use for subsequent /v1/vbt/subset calls.
    """
    head_path = _normalize_path(request.path)
    if not head_path.exists() or not head_path.is_file():
        return VbtOpenResponse(valid=False, error="VBT header file not found.")
    if not head_path.suffix == ".head" and not str(head_path).endswith(".bin.head"):
        pass  # accept any extension — validation happens on parse
    bin_path = Path(str(head_path)[: -len(".head")] if str(head_path).endswith(".head") else str(head_path))
    if not bin_path.exists():
        return VbtOpenResponse(valid=False, error=f"VBT binary file not found: {bin_path.name}")

    try:
        header = _parse_vbt_header(head_path)
    except Exception as exc:
        return VbtOpenResponse(valid=False, error=str(exc))

    # Memory-map the binary file once at open time.  np.memmap opens a
    # read-only virtual mapping – no actual data is loaded until accessed,
    # and the OS page cache handles repeated access efficiently.
    field_count: int = header["field_count"]
    num_rows: int = header["num_rows"]
    np_dtype = np.dtype(header["np_endian_code"])
    expected_scalars = field_count * num_rows
    actual_scalars = bin_path.stat().st_size // np_dtype.itemsize
    if actual_scalars != expected_scalars:
        return VbtOpenResponse(
            valid=False,
            error=(
                f"Binary size mismatch: header says {expected_scalars} scalars "
                f"({field_count} fields × {num_rows} rows), "
                f"file contains {actual_scalars} scalars."
            ),
        )
    try:
        # Shape (field_count, num_rows): field-major layout matches VBT spec.
        memmap = np.memmap(bin_path, dtype=np_dtype, mode="r", shape=(field_count, num_rows))
    except Exception as exc:
        return VbtOpenResponse(valid=False, error=f"Failed to memory-map VBT binary: {exc}")

    dataset_id = f"vbt_{uuid.uuid4().hex[:12]}"
    session.datasets[dataset_id] = {
        "kind": "vbt",
        "head_path": str(head_path),
        "bin_path": str(bin_path),
        "header": header,
        "memmap": memmap,  # shape (field_count, num_rows), read-only
    }
    logger.info(
        "[vbt-open] session=%s dataset_id=%s kind=%s fields=%d rows=%d path=%s (memmap)",
        session.session_id,
        dataset_id,
        header["kind"],
        header["field_count"],
        header["num_rows"],
        head_path,
    )

    fields = [VbtFieldInfo(name=n, type=header["scalar_type"]) for n in header["field_names"]]
    return VbtOpenResponse(
        valid=True,
        error="",
        dataset_id=dataset_id,
        session_id=session.session_id,
        kind=header["kind"],
        fields=fields,
        num_rows=header["num_rows"],
        scalar_type=header["scalar_type"],
        dimensions=header["dimensions"] or [],
        spacing=header["spacing"] or [],
    )


@app.post("/v1/vbt/subset", response_model=VbtSubsetResponse, tags=["vbt"])
async def vbt_subset(
    request: VbtSubsetRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> VbtSubsetResponse:
    """Return a subset of VBT data encoded as base64 binary columns.

    For **point** datasets all fields are returned up to *max_rows* rows.
    Each column is a base64-encoded float64-LE byte array.

    For **volume** datasets a single field block is returned as a
    base64-encoded float32-LE byte array of length nx*ny*nz.
    """
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "vbt":
            raise HTTPException(422, "This endpoint requires a VBT dataset.")
    except HTTPException:
        raise

    header = entry["header"]
    field_names: list[str] = header["field_names"]
    num_rows: int = header["num_rows"]
    # Use the pre-opened memmap – no disk I/O beyond what the OS pages in.
    arr: np.memmap = entry["memmap"]  # shape (field_count, num_rows)

    if header["kind"] == "point":
        max_rows = request.max_rows if request.max_rows > 0 else num_rows
        max_rows = min(max_rows, num_rows)
        columns: dict[str, str] = {}
        for i, fname in enumerate(field_names):
            # np.array() copies the memmap slice so the wire bytes are a plain ndarray.
            col = np.array(arr[i, :max_rows], dtype="<f8")
            columns[fname] = base64.b64encode(col.tobytes()).decode("ascii")
        return VbtSubsetResponse(
            valid=True,
            error="",
            kind="point",
            num_rows=max_rows,
            fields=field_names,
            columns=columns,
        )
    else:  # volume
        field_name = request.field_name if request.field_name else field_names[0]
        if field_name not in field_names:
            return VbtSubsetResponse(
                valid=False, error=f"Field '{field_name}' not found in VBT dataset."
            )
        field_idx = field_names.index(field_name)
        block = np.array(arr[field_idx], dtype="<f4")  # float32-LE on the wire
        return VbtSubsetResponse(
            valid=True,
            error="",
            kind="volume",
            num_rows=num_rows,
            fields=field_names,
            field_name=field_name,
            block=base64.b64encode(block.tobytes()).decode("ascii"),
            dimensions=header["dimensions"],
            spacing=header["spacing"],
        )


@app.post("/v1/vbt/query", response_model=TabularQueryResponse, tags=["vbt"])
async def vbt_query(
    request: TabularQueryRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> TabularQueryResponse:
    """Query a VBT point dataset with optional filters, sort, and pagination.

    Returns column-oriented float64-LE base64 data, same wire format as
    /v1/vbt/subset but with full filter/sort/pagination support.
    Only ``kind='point'`` datasets are supported.
    """
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "vbt":
            raise HTTPException(422, "This endpoint requires a VBT dataset.")
    except HTTPException:
        raise

    header = entry["header"]
    if header["kind"] != "point":
        return TabularQueryResponse(
            valid=False, error="vbt/query only supports point datasets; use vbt/subset for volumes."
        )

    field_names: list[str] = header["field_names"]
    # Use the pre-opened memmap – shape (field_count, num_rows).
    arr: np.memmap = entry["memmap"]

    # Determine which columns are referenced by filters/sort so we only copy
    # those slices from the memmap rather than materialising the entire table.
    filter_fields: set[str] = {f.field for f in request.filters}
    sort_field: str | None = request.sort.field if request.sort else None
    needed: set[str] = filter_fields | ({sort_field} if sort_field else set())
    # If no filters/sort are active we still need all columns for the response,
    # so just build them all lazily.
    if not needed:
        needed = set(field_names)

    try:
        # Build DataFrame only from needed columns; copy out of memmap.
        df = pd.DataFrame(
            {
                name: np.array(arr[i], dtype=float)
                for i, name in enumerate(field_names)
                if name in needed
            }
        )
        df, total = _apply_tabular_query(
            df, request.filters, request.sort, request.limit, request.offset
        )
    except Exception as exc:
        return TabularQueryResponse(valid=False, error=str(exc))

    # After filtering we know the surviving row indices.  For columns that were
    # not loaded into the DataFrame (non-filter, non-sort columns when filters
    # were active), read only the filtered rows from the memmap now.
    row_index = df.index  # integer positions in the original array
    columns: dict[str, str] = {}
    for i, name in enumerate(field_names):
        if name in df.columns:
            col = df[name].to_numpy().astype("<f8")
        else:
            # Read only the rows that survived filtering.
            col = np.array(arr[i][row_index], dtype="<f8")
        columns[name] = base64.b64encode(col.tobytes()).decode("ascii")

    return TabularQueryResponse(
        valid=True,
        error="",
        total_rows=total,
        returned_rows=len(df),
        num_rows=len(df),
        fields=field_names,
        columns=columns,
    )


@app.post("/v1/products/moment", response_model=MomentProductResponse, tags=["products"])
async def moment_product(
    request: MomentProductRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> MomentProductResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "This endpoint requires a spectral cube dataset.")
        result, _ = await _moment_product_payload(entry, request)
    except HTTPException:
        raise
    except Exception as exc:
        return MomentProductResponse(valid=False, error=str(exc))
    return MomentProductResponse(valid=True, error="", **result)


@app.post("/v1/cube/noise", response_model=CubeNoiseResponse, tags=["cube"])
async def cube_noise(
    request: CubeNoiseRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeNoiseResponse:
    """Estimate per-channel noise (MAD and Gaussian sigma) in an emission-free region."""
    from .compute import worker_noise_estimate
    try:
        path = _require_cube_path(session, request.dataset_id)
        result = await _run_with_limit(
            session,
            worker_noise_estimate,
            path,
            request.x0, request.x1,
            request.y0, request.y1,
            request.channel_start,
            request.channel_end,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return CubeNoiseResponse(valid=False, error=str(exc))
    return CubeNoiseResponse(valid=True, error="", **result)


@app.post("/v1/products/isosurface", response_model=IsosurfaceProductResponse, tags=["products"])
async def isosurface_product(
    request: IsosurfaceProductRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> IsosurfaceProductResponse:
    if not _VTK_AVAILABLE:
        return IsosurfaceProductResponse(
            valid=False,
            error="VTK Python bindings are not installed on this server. Isosurface is unavailable.",
        )
    from .compute import worker_isosurface
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "Isosurface requires a cube dataset.")
        iso_cache_params = {"threshold": float(request.threshold)}
        cache_key, param_hash = PRODUCT_CACHE.make_key(entry["path"], "isosurface", iso_cache_params)
        cached = PRODUCT_CACHE.get(cache_key)
        if cached is not None:
            logger.debug("[isosurface] cache hit dataset_id=%s threshold=%s", request.dataset_id, request.threshold)
            return IsosurfaceProductResponse(valid=True, error="", **cached.payload)
        result = await _run_with_limit(
            session,
            worker_isosurface,
            entry["path"],
            int(entry["width"]),
            int(entry["height"]),
            int(entry["depth"]),
            float(request.threshold),
        )
        dataset = ScientificFitsDataset(entry["path"])
        PRODUCT_CACHE.put(
            key=cache_key,
            operation="isosurface",
            dataset_path=entry["path"],
            parameter_hash=param_hash,
            payload=dict(result),
            scientific_metadata={"threshold": float(request.threshold)},
            provenance_metadata=dataset.provenance_context(),
        )
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[isosurface] failed dataset_id=%s error=%s", request.dataset_id, exc)
        return IsosurfaceProductResponse(valid=False, error=str(exc))
    return IsosurfaceProductResponse(valid=True, error="", **result)


@app.post("/v1/cube/preview", response_model=CubePreviewResponse, tags=["cube"])
async def cube_preview(
    request: CubePreviewRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubePreviewResponse:
    from .compute import worker_cube_preview
    try:
        path = _require_cube_path(session, request.dataset_id)
        result = await _run_with_limit(session, worker_cube_preview, path, request.downsample)
    except HTTPException:
        raise
    except Exception as exc:
        return CubePreviewResponse(valid=False, error=str(exc))
    return CubePreviewResponse(valid=True, error="", **result)


@app.post("/v1/cube/slice", response_model=CubeSliceResponse, tags=["cube"])
async def cube_slice(
    request: CubeSliceRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeSliceResponse:
    from .compute import worker_cube_slice
    try:
        path = _require_cube_path(session, request.dataset_id)
        if request.axis.lower() != "z":
            raise HTTPException(422, "Only axis='z' is currently supported.")
        result = await _run_with_limit(session, worker_cube_slice, path, request.index)
    except HTTPException:
        raise
    except Exception as exc:
        return CubeSliceResponse(valid=False, error=str(exc))
    return CubeSliceResponse(valid=True, error="", **result)


@app.post("/v1/cube/subvolume", response_model=CubeSubvolumeResponse, tags=["cube"])
async def cube_subvolume(
    request: CubeSubvolumeRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubeSubvolumeResponse:
    from .compute import worker_cube_subvolume
    try:
        path = _require_cube_path(session, request.dataset_id)
        result = await _run_with_limit(
            session,
            worker_cube_subvolume,
            path,
            request.x0, request.x1,
            request.y0, request.y1,
            request.z0, request.z1,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return CubeSubvolumeResponse(valid=False, error=str(exc))
    return CubeSubvolumeResponse(valid=True, error="", **result)


@app.post("/v1/cube/pv", response_model=CubePvResponse, tags=["cube"])
async def cube_pv(
    request: CubePvRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CubePvResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "This endpoint requires a spectral cube dataset.")
        result, _ = await _pv_product_payload(entry, request)
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[pv] failed dataset_id=%s error=%s", request.dataset_id, exc)
        return CubePvResponse(valid=False, error=str(exc))
    return CubePvResponse(valid=True, error="", **result)


async def _progress_ticker(task_id: str, start: float = 0.05, end: float = 0.90, interval_s: float = 5.0) -> None:
    """
    Slowly increment task progress from *start* to *end* at *interval_s* second intervals.

    This gives the polling client visible forward progress during long-running
    ProcessPoolExecutor operations where the worker cannot report back intermediate
    state. The ticker is cancelled by the _runner coroutine as soon as the real
    result arrives.

    With the defaults (start=0.05, end=0.90, interval=5s, step≈4.25%):
      - A 30s computation reaches ~45% before completing.
      - A 120s computation reaches ~90% (cap) after 100s, then jumps to 100%.
    """
    step = max(0.01, (end - start) / 20.0)  # ≈20 ticks from start to end
    progress = start
    try:
        while progress < end:
            await asyncio.sleep(interval_s)
            progress = min(end, progress + step)
            record = TASKS.get(task_id)
            if record is None or record.status != "running":
                return
            TASKS.update(task_id, progress=progress)
    except asyncio.CancelledError:
        pass


@app.post("/v1/tasks/moment", response_model=TaskCreateResponse, tags=["tasks"])
async def create_moment_task(
    request: MomentProductRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "Moment tasks require a cube dataset.")
    except HTTPException:
        raise

    task = TASKS.create("moment")
    TASKS.update(task.task_id, status="running", progress=0.05)

    async def _runner() -> None:
        ticker = asyncio.create_task(_progress_ticker(task.task_id))
        try:
            result, cache_hit = await _moment_product_payload(entry, request)
            ticker.cancel()
            TASKS.update(
                task.task_id,
                status="completed",
                progress=1.0,
                result={"valid": True, "error": "", **result},
                cache_hit=cache_hit,
            )
        except asyncio.CancelledError:
            ticker.cancel()
            TASKS.update(task.task_id, status="cancelled", progress=0.0, error="Cancelled by client.")
        except Exception as exc:  # pragma: no cover - exercised at runtime
            ticker.cancel()
            TASKS.update(task.task_id, status="failed", progress=1.0, error=str(exc))

    handle = asyncio.create_task(_runner())
    TASKS.update(task.task_id, asyncio_handle=handle)
    return TaskCreateResponse(valid=True, error="", task_id=task.task_id, status="running", cache_hit=False)


@app.post("/v1/tasks/pv", response_model=TaskCreateResponse, tags=["tasks"])
async def create_pv_task(
    request: CubePvRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "PV tasks require a cube dataset.")
    except HTTPException:
        raise

    task = TASKS.create("pv")
    TASKS.update(task.task_id, status="running", progress=0.05)

    async def _runner() -> None:
        ticker = asyncio.create_task(_progress_ticker(task.task_id))
        try:
            result, cache_hit = await _pv_product_payload(entry, request)
            ticker.cancel()
            TASKS.update(
                task.task_id,
                status="completed",
                progress=1.0,
                result={"valid": True, "error": "", **result},
                cache_hit=cache_hit,
            )
        except asyncio.CancelledError:
            ticker.cancel()
            TASKS.update(task.task_id, status="cancelled", progress=0.0, error="Cancelled by client.")
        except Exception as exc:  # pragma: no cover - exercised at runtime
            ticker.cancel()
            TASKS.update(task.task_id, status="failed", progress=1.0, error=str(exc))

    handle = asyncio.create_task(_runner())
    TASKS.update(task.task_id, asyncio_handle=handle)
    return TaskCreateResponse(valid=True, error="", task_id=task.task_id, status="running", cache_hit=False)


@app.get("/v1/tasks/{task_id}", response_model=TaskStatusResponse, tags=["tasks"])
async def task_status(task_id: str, _: None = _auth) -> TaskStatusResponse:
    task = TASKS.get(task_id)
    if task is None:
        raise HTTPException(status_code=404, detail=f"Unknown task_id '{task_id}'.")
    return TaskStatusResponse(
        valid=True,
        error=task.error,
        task_id=task.task_id,
        operation=task.operation,
        status=task.status,
        created_at=task.created_at,
        updated_at=task.updated_at,
        progress=task.progress,
        cache_hit=task.cache_hit,
        result=task.result,
    )


@app.delete("/v1/tasks/{task_id}", tags=["tasks"])
async def cancel_task(task_id: str, _: None = _auth) -> dict:
    """Request cancellation of a running or pending task.

    Returns 200 with ``{"cancelled": true}`` if the cancellation was accepted,
    or 404 if the task does not exist, or 409 if the task has already finished.
    A cancellation request is best-effort: the task may still complete before
    the asyncio cancellation propagates to the worker future.
    """
    task = TASKS.get(task_id)
    if task is None:
        raise HTTPException(status_code=404, detail=f"Unknown task_id '{task_id}'.")
    if task.status not in ("pending", "running"):
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=f"Task '{task_id}' is already in terminal state '{task.status}'.",
        )
    cancelled = TASKS.cancel(task_id)
    return {"cancelled": cancelled, "task_id": task_id}


@app.post("/v1/image/full", response_model=ImageFullResponse, tags=["image"])
async def image_full(
    request: ImageFullRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ImageFullResponse:
    from .compute import worker_image_full
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "image":
            return ImageFullResponse(valid=False, error="Image endpoint requires an image dataset.")
        result = await _run_with_limit(session, worker_image_full, entry["path"])
    except HTTPException:
        raise
    except Exception as exc:
        return ImageFullResponse(valid=False, error=str(exc))
    return ImageFullResponse(valid=True, error="", **result)


@app.post("/v1/image/preview", response_model=ImageFullResponse, tags=["image"])
async def image_preview(
    request: ImagePreviewRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ImageFullResponse:
    from .compute import worker_image_preview
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "image":
            raise HTTPException(422, "Image preview endpoint requires an image dataset.")
        result = await _run_with_limit(session, worker_image_preview, entry["path"], request.max_longest_side)
    except HTTPException:
        raise
    except Exception as exc:
        return ImageFullResponse(valid=False, error=str(exc))
    return ImageFullResponse(valid=True, error="", **result)


# ── Cosmology ─────────────────────────────────────────────────────────────────

_COSMOLOGY_MODELS: dict[str, Any] = {}


def _load_cosmology_models() -> dict[str, Any]:
    """Lazy-load astropy cosmology presets; returns an empty dict if astropy is absent."""
    if _COSMOLOGY_MODELS:
        return _COSMOLOGY_MODELS
    try:
        import astropy.cosmology as _ac
        for _name in ("Planck18", "Planck15", "Planck13", "WMAP9"):
            if hasattr(_ac, _name):
                _COSMOLOGY_MODELS[_name] = getattr(_ac, _name)
    except ImportError:
        logger.warning("[startup] astropy not available – /v1/cosmology/distance will return 503.")
    return _COSMOLOGY_MODELS


_load_cosmology_models()


class CosmologyDistanceRequest(BaseModel):
    z: float
    model: str = "Planck18"


class CosmologyDistanceResponse(BaseModel):
    valid: bool
    error: str
    z: float = 0.0
    model: str = ""
    comoving_Mpc: float = 0.0
    luminosity_Mpc: float = 0.0
    proper_motion_kpc_per_arcsec: float = 0.0


@app.post("/v1/cosmology/distance", response_model=CosmologyDistanceResponse, tags=["cosmology"])
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


class CosmologyBatchRequest(BaseModel):
    redshifts: list[float]
    model: str = "Planck18"


class CosmologyBatchResponse(BaseModel):
    valid: bool
    error: str
    model: str = ""
    distances_Mpc: list[float] = []


@app.post("/v1/cosmology/distance/batch", response_model=CosmologyBatchResponse, tags=["cosmology"])
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


# ── HiPS (Hierarchical Progressive Survey) ────────────────────────────────────
# Backend-authoritative: the backend fetches tiles from remote HiPS servers,
# caches them on disk, and serves them to the Qt client.  The client never
# makes direct HTTP requests to external hosts.

import asyncio as _asyncio
import math as _math
import urllib.request as _urllib_request
import urllib.error as _urllib_error
from pathlib import Path as _Path

_HIPS_CACHE_DIR = _Path(
    os.environ.get("VISIVO_HIPS_CACHE",
                   str(_Path.home() / ".cache" / "visivo" / "hips"))
)
_HIPS_CACHE_DIR.mkdir(parents=True, exist_ok=True)

# Optional: healpy for HEALPix pixel queries
_HEALPY_AVAILABLE = False
try:
    import healpy as _hp  # type: ignore
    _HEALPY_AVAILABLE = True
except ImportError:
    logger.warning("[startup] healpy not installed – /v1/hips/*/query_tiles will be unavailable.")


# ── HiPS Pydantic models ──────────────────────────────────────────────────────

class HiPSOpenRequest(BaseModel):
    url: str


class HiPSSurveyResponse(BaseModel):
    valid: bool
    error: str = ""
    survey_id: str = ""
    session_id: str = ""
    base_url: str = ""
    title: str = ""
    frame: str = "equatorial"   # "equatorial" | "galactic" | "ecliptic"
    max_order: int = 9
    min_order: int = 0
    tile_width: int = 512
    tile_formats: list[str] = []  # e.g. ["png", "jpeg"]


class HiPSTileInfo(BaseModel):
    pix: int
    order: int
    ra_center: float    # degrees
    dec_center: float   # degrees
    corners_ra: list[float]   # 4 values, degrees
    corners_dec: list[float]  # 4 values, degrees


class HiPSViewRequest(BaseModel):
    survey_id: str
    ra_center: float
    dec_center: float
    fov_deg: float
    order: int


class HiPSViewResponse(BaseModel):
    valid: bool
    error: str = ""
    tiles: list[HiPSTileInfo] = []


# ── HiPS helpers ──────────────────────────────────────────────────────────────

def _hips_fetch_sync(url: str) -> bytes:
    """Synchronous remote fetch.  Called in a thread-pool executor."""
    req = _urllib_request.Request(
        url, headers={"User-Agent": "VisIVO-Backend/1.0 (HiPS proxy)"}
    )
    with _urllib_request.urlopen(req, timeout=30) as resp:
        return resp.read()


async def _hips_fetch(url: str) -> bytes:
    """Fetch *url* without blocking the FastAPI event loop."""
    loop = _asyncio.get_event_loop()
    return await loop.run_in_executor(None, _hips_fetch_sync, url)


def _parse_hips_properties(text: str) -> dict[str, str]:
    """Parse a Java-style `.properties` file as used by the HiPS standard."""
    props: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("!"):
            continue
        if "=" in line:
            key, _, value = line.partition("=")
            props[key.strip()] = value.strip()
    return props


def _best_tile_formats(props: dict[str, str]) -> list[str]:
    """Return tile formats ordered by preference (PNG → JPEG → FITS)."""
    raw = props.get("hips_tile_format", "jpeg")
    found = [f.strip().lower() for f in raw.replace(",", " ").split()]
    preference = ["png", "jpeg", "jpg", "fits"]
    return sorted(set(found), key=lambda f: preference.index(f) if f in preference else 99)


def _tile_url(base_url: str, order: int, pix: int, fmt: str) -> str:
    dir_num = (pix // 10000) * 10000
    ext = "jpg" if fmt in ("jpeg", "jpg") else fmt
    return f"{base_url}/Norder{order}/Dir{dir_num}/Npix{pix}.{ext}"


def _allsky_url(base_url: str, order: int, fmt: str) -> str:
    ext = "jpg" if fmt in ("jpeg", "jpg") else fmt
    return f"{base_url}/Norder{order}/Allsky.{ext}"


def _tile_cache_path(survey_id: str, order: int, pix: int, fmt: str) -> _Path:
    return _HIPS_CACHE_DIR / survey_id / str(order) / f"{pix}.{fmt}"


def _allsky_cache_path(survey_id: str, order: int, fmt: str) -> _Path:
    return _HIPS_CACHE_DIR / survey_id / f"allsky_{order}.{fmt}"


def _get_tiles_for_fov(
    ra_deg: float, dec_deg: float, fov_deg: float, order: int
) -> list[dict]:
    """Return HEALPix NESTED tiles covering the given FOV.  Requires healpy."""
    nside = _hp.order2nside(order)
    # Centre unit vector
    theta = _math.radians(90.0 - dec_deg)
    phi   = _math.radians(ra_deg)
    vec   = _hp.ang2vec(theta, phi)
    # Disc radius with √2 margin so corner tiles are included
    radius_rad = _math.radians(fov_deg / 2.0 * 1.42)
    pixels = _hp.query_disc(nside, vec, radius_rad, nest=True, inclusive=True)

    result = []
    for pix in pixels:
        p = int(pix)
        theta_c, phi_c = _hp.pix2ang(nside, p, nest=True)
        ra_c  = float(_math.degrees(phi_c))
        dec_c = float(90.0 - _math.degrees(theta_c))

        # 4 corners in (theta, phi) then convert to (ra, dec)
        corners = _hp.boundaries(nside, p, step=1, nest=True)  # shape (3,4)
        c_ra  = [float(_math.degrees(_math.atan2(corners[1, i], corners[0, i]))) for i in range(4)]
        c_dec = [float(90.0 - _math.degrees(_math.acos(
            max(-1.0, min(1.0, corners[2, i]))))) for i in range(4)]

        result.append({
            "pix": p,
            "order": order,
            "ra_center": ra_c,
            "dec_center": dec_c,
            "corners_ra": c_ra,
            "corners_dec": c_dec,
        })
    return result


# ── HiPS endpoints ─────────────────────────────────────────────────────────────

@app.post("/v1/hips/open", response_model=HiPSSurveyResponse, tags=["hips"])
async def hips_open(
    request: HiPSOpenRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> HiPSSurveyResponse:
    """Register a remote HiPS survey URL.  Fetches the `properties` file and
    caches it.  Returns a survey_id for subsequent tile/allsky requests."""
    base_url = request.url.rstrip("/")
    props_url = f"{base_url}/properties"

    try:
        props_bytes = await _hips_fetch(props_url)
    except _urllib_error.HTTPError as exc:
        return HiPSSurveyResponse(valid=False,
                                  error=f"Cannot fetch HiPS properties ({exc.code}): {props_url}")
    except Exception as exc:
        return HiPSSurveyResponse(valid=False,
                                  error=f"Cannot fetch HiPS properties: {exc}")

    try:
        props = _parse_hips_properties(props_bytes.decode("utf-8", errors="replace"))
    except Exception as exc:
        return HiPSSurveyResponse(valid=False,
                                  error=f"Cannot parse HiPS properties: {exc}")

    title = (props.get("obs_title")
             or props.get("obs_collection")
             or props.get("creator_did")
             or "Unknown Survey")
    frame_raw = props.get("hips_frame", "equatorial").lower()
    if "galactic" in frame_raw:
        frame = "galactic"
    elif "ecliptic" in frame_raw:
        frame = "ecliptic"
    else:
        frame = "equatorial"

    try:
        max_order = int(props.get("hips_order", "9"))
        min_order = int(props.get("hips_order_min", "0"))
        tile_width = int(props.get("hips_tile_width", "512"))
    except ValueError:
        max_order, min_order, tile_width = 9, 0, 512

    formats = _best_tile_formats(props)

    survey_id = f"hips_{uuid.uuid4().hex[:12]}"
    session.datasets[survey_id] = {
        "kind": "hips",
        "base_url": base_url,
        "title": title,
        "frame": frame,
        "max_order": max_order,
        "min_order": min_order,
        "tile_width": tile_width,
        "tile_formats": formats,
        "properties": props,
    }
    logger.info("[hips-open] session=%s survey_id=%s title=%s max_order=%d frame=%s",
                session.session_id, survey_id, title, max_order, frame)

    return HiPSSurveyResponse(
        valid=True, error="",
        survey_id=survey_id, session_id=session.session_id,
        base_url=base_url, title=title, frame=frame,
        max_order=max_order, min_order=min_order,
        tile_width=tile_width, tile_formats=formats,
    )


@app.get("/v1/hips/{survey_id}/allsky", tags=["hips"])
async def hips_allsky(
    survey_id: str,
    order: int = 3,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Return the AllSky mosaic image for the given order.
    The image is fetched once from the remote survey and cached on disk."""
    try:
        entry = _require_dataset(session, survey_id)
        if entry["kind"] != "hips":
            raise HTTPException(422, "Not a HiPS survey.")
    except HTTPException:
        raise

    base_url: str = entry["base_url"]
    formats: list[str] = entry["tile_formats"]
    fmt = formats[0] if formats else "png"

    cache_path = _allsky_cache_path(survey_id, order, fmt)
    if cache_path.exists():
        data = cache_path.read_bytes()
    else:
        # Try each format until one works
        data = b""
        last_err: Exception | None = None
        for candidate_fmt in (formats or ["png", "jpeg"]):
            url = _allsky_url(base_url, order, candidate_fmt)
            try:
                data = await _hips_fetch(url)
                fmt = candidate_fmt
                break
            except Exception as exc:
                last_err = exc
        if not data:
            raise HTTPException(404, f"AllSky not available at order {order}: {last_err}")

        cache_path.parent.mkdir(parents=True, exist_ok=True)
        cache_path.write_bytes(data)

    mt = ("image/jpeg" if fmt in ("jpeg", "jpg")
          else "image/fits" if fmt == "fits"
          else "image/png")
    return Response(content=data, media_type=mt)


@app.get("/v1/hips/{survey_id}/tile/{order}/{pix}", tags=["hips"])
async def hips_tile(
    survey_id: str,
    order: int,
    pix: int,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Return a single HiPS tile image (PNG/JPEG).  Caches tiles on disk."""
    try:
        entry = _require_dataset(session, survey_id)
        if entry["kind"] != "hips":
            raise HTTPException(422, "Not a HiPS survey.")
    except HTTPException:
        raise

    formats: list[str] = entry["tile_formats"]
    base_url: str = entry["base_url"]
    fmt = formats[0] if formats else "png"

    cache_path = _tile_cache_path(survey_id, order, pix, fmt)
    if cache_path.exists():
        data = cache_path.read_bytes()
    else:
        data = b""
        last_err: Exception | None = None
        for candidate_fmt in (formats or ["png", "jpeg"]):
            url = _tile_url(base_url, order, pix, candidate_fmt)
            try:
                data = await _hips_fetch(url)
                fmt = candidate_fmt
                break
            except Exception as exc:
                last_err = exc
        if not data:
            raise HTTPException(404, f"Tile order={order} pix={pix} not available: {last_err}")

        cache_path.parent.mkdir(parents=True, exist_ok=True)
        cache_path.write_bytes(data)

    mt = ("image/jpeg" if fmt in ("jpeg", "jpg")
          else "image/fits" if fmt == "fits"
          else "image/png")
    return Response(content=data, media_type=mt)


@app.post("/v1/hips/{survey_id}/query_tiles",
          response_model=HiPSViewResponse, tags=["hips"])
async def hips_query_tiles(
    survey_id: str,
    request: HiPSViewRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> HiPSViewResponse:
    """Return the set of HEALPix tiles that cover the given viewport.
    Requires healpy.  Falls back to an empty list with an error if unavailable."""
    try:
        entry = _require_dataset(session, survey_id)
        if entry["kind"] != "hips":
            raise HTTPException(422, "Not a HiPS survey.")
    except HTTPException:
        raise

    if not _HEALPY_AVAILABLE:
        return HiPSViewResponse(
            valid=False,
            error="healpy is not installed on the backend – tile queries unavailable. "
                  "Install it with: pip install healpy",
        )

    max_order: int = entry["max_order"]
    order = max(0, min(request.order, max_order))
    try:
        loop = _asyncio.get_event_loop()
        tiles_raw = await loop.run_in_executor(
            None, _get_tiles_for_fov,
            request.ra_center, request.dec_center, request.fov_deg, order,
        )
    except Exception as exc:
        return HiPSViewResponse(valid=False, error=f"HEALPix query failed: {exc}")

    return HiPSViewResponse(
        valid=True,
        tiles=[HiPSTileInfo(**t) for t in tiles_raw],
    )


# ── Target name resolver ──────────────────────────────────────────────────────
# Uses astropy's Sesame resolver which queries SIMBAD/NED/VizieR in sequence.
# No external HTTP is made by the Qt client directly.

class TargetResolveRequest(BaseModel):
    name: str


class TargetResolveResponse(BaseModel):
    valid: bool
    error: str = ""
    ra_deg: float = 0.0
    dec_deg: float = 0.0
    resolved_name: str = ""
    source: str = ""


def _resolve_target_sync(name: str) -> dict:
    try:
        from astropy.coordinates import SkyCoord  # type: ignore
        coord = SkyCoord.from_name(name)
        return {
            "valid": True, "error": "",
            "ra_deg": float(coord.ra.deg), "dec_deg": float(coord.dec.deg),
            "resolved_name": name, "source": "sesame",
        }
    except Exception as exc:
        return {
            "valid": False, "error": str(exc),
            "ra_deg": 0.0, "dec_deg": 0.0, "resolved_name": "", "source": "",
        }


@app.post("/v1/resolve/target", response_model=TargetResolveResponse, tags=["resolve"])
async def resolve_target(
    request: TargetResolveRequest,
    _: None = _auth,
) -> TargetResolveResponse:
    """Resolve an object name to RA/Dec via the Sesame name resolver (SIMBAD/NED/VizieR).
    Requires astropy; returns an error if astropy is not installed."""
    name = request.name.strip()
    if not name:
        return TargetResolveResponse(valid=False, error="Empty target name.")
    loop = _asyncio.get_event_loop()
    d = await loop.run_in_executor(None, _resolve_target_sync, name)
    return TargetResolveResponse(**d)


# ── HiPS catalogue overlay ────────────────────────────────────────────────────
# Reads a CSV from the backend filesystem and returns RA/Dec pairs within the
# current viewport bounding box.  No session required — path-based lookup.

class CatalogueOverlayRequest(BaseModel):
    catalogue_path: str      # backend-side absolute path to a CSV file
    ra_center: float
    dec_center: float
    fov_deg: float
    ra_field: str = ""       # auto-detected if empty
    dec_field: str = ""
    max_sources: int = 5000  # spatial sub-sample cap


class CatalogueOverlaySource(BaseModel):
    ra: float
    dec: float


class CatalogueOverlayResponse(BaseModel):
    valid: bool
    error: str = ""
    sources: list[CatalogueOverlaySource] = []
    total_in_view: int = 0
    truncated: bool = False


_RA_CANDIDATES  = ["ra", "RA", "Ra", "RAJ2000", "ra_deg", "RAdeg", "ALPHA_J2000"]
_DEC_CANDIDATES = ["dec", "Dec", "DEC", "DEJ2000", "dec_deg", "DECdeg", "DELTA_J2000"]

@functools.lru_cache(maxsize=32)
def _load_overlay_csv(path: str) -> pd.DataFrame:
    """Read and parse a catalogue CSV, caching up to 32 files (LRU eviction)."""
    return pd.read_csv(path)


def clear_overlay_cache() -> None:
    """Invalidate the overlay CSV cache (call on dataset reload or file change)."""
    _load_overlay_csv.cache_clear()


def _catalogue_overlay_sync(
    path: str, ra_center: float, dec_center: float, fov_deg: float,
    ra_field: str, dec_field: str, max_sources: int,
) -> dict:
    try:
        df = _load_overlay_csv(path)
    except Exception as exc:
        return {"valid": False, "error": f"Cannot read CSV '{path}': {exc}",
                "sources": [], "total_in_view": 0, "truncated": False}

    # Auto-detect RA/Dec column names
    if not ra_field:
        ra_field = next((c for c in _RA_CANDIDATES  if c in df.columns), "")
    if not dec_field:
        dec_field = next((c for c in _DEC_CANDIDATES if c in df.columns), "")

    if not ra_field or not dec_field:
        return {"valid": False,
                "error": f"Cannot detect RA/Dec columns. Available: {list(df.columns)[:20]}",
                "sources": [], "total_in_view": 0, "truncated": False}

    ra_col  = pd.to_numeric(df[ra_field],  errors="coerce")
    dec_col = pd.to_numeric(df[dec_field], errors="coerce")

    # Conservative bounding-box filter
    half_fov  = fov_deg * 0.6
    cos_dec   = max(0.02, abs(float(np.cos(np.radians(dec_center)))))
    ra_margin = min(180.0, half_fov / cos_dec)

    ra_min, ra_max   = ra_center - ra_margin, ra_center + ra_margin
    dec_min, dec_max = dec_center - half_fov, dec_center + half_fov

    if ra_min < 0:
        mask = ((ra_col >= ra_min + 360) | (ra_col <= ra_max)) & \
               (dec_col >= dec_min) & (dec_col <= dec_max)
    elif ra_max > 360:
        mask = ((ra_col >= ra_min) | (ra_col <= ra_max - 360)) & \
               (dec_col >= dec_min) & (dec_col <= dec_max)
    else:
        mask = (ra_col >= ra_min) & (ra_col <= ra_max) & \
               (dec_col >= dec_min) & (dec_col <= dec_max)

    filtered  = df[mask]
    total     = int(len(filtered))
    truncated = total > max_sources
    if truncated:
        # Spatial sub-sampling: every N-th row to keep uniform coverage
        step     = max(1, total // max_sources)
        filtered = filtered.iloc[::step].head(max_sources)

    sources = []
    for r, d in zip(filtered[ra_field], filtered[dec_field]):
        try:
            fr, fd = float(r), float(d)
            if np.isfinite(fr) and np.isfinite(fd):
                sources.append({"ra": fr, "dec": fd})
        except (ValueError, TypeError):
            pass

    return {"valid": True, "error": "", "sources": sources,
            "total_in_view": total, "truncated": truncated}


@app.post("/v1/hips/catalogue_overlay",
          response_model=CatalogueOverlayResponse, tags=["hips"])
async def hips_catalogue_overlay(
    request: CatalogueOverlayRequest,
    _: None = _auth,
) -> CatalogueOverlayResponse:
    """Return catalogue sources (RA/Dec) visible in the given HiPS viewport.
    Reads a CSV file at `catalogue_path` on the backend filesystem."""
    loop = _asyncio.get_event_loop()
    d = await loop.run_in_executor(
        None, _catalogue_overlay_sync,
        request.catalogue_path, request.ra_center, request.dec_center,
        request.fov_deg, request.ra_field, request.dec_field, request.max_sources,
    )
    if not d["valid"]:
        return CatalogueOverlayResponse(valid=False, error=d["error"])
    return CatalogueOverlayResponse(
        valid=True,
        sources=[CatalogueOverlaySource(**s) for s in d["sources"]],
        total_in_view=d["total_in_view"],
        truncated=d["truncated"],
    )
