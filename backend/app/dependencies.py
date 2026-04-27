import asyncio
import asyncio as _asyncio
import base64
import csv
import functools
import logging
import math as _math
import os
import struct
import threading
import urllib.error as _urllib_error
import urllib.request as _urllib_request
import uuid
from concurrent.futures import ProcessPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
from pathlib import Path as _Path
from typing import Any
import numpy as np
import pandas as pd
from astropy.io import fits
from astropy.table import Table
from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request, Response, status
from fastapi.responses import JSONResponse
from .auth import verify_token
from .fits_dataset import ScientificFitsDataset
from .product_cache import PRODUCT_CACHE
from .sessions import REGISTRY, Session, get_session
from .tasks import TASKS
from .schemas import *
logger = logging.getLogger('visivo.backend')

_WORKERS = int(os.environ.get('VISIVO_WORKERS', '4'))
_POOL = ProcessPoolExecutor(max_workers=_WORKERS)
_DASK_CLIENT = None
_DASK_SCHEDULER = os.environ.get('VISIVO_DASK_SCHEDULER', '')
_VTK_AVAILABLE = False
try:
    import vtk as _vtk
    _VTK_AVAILABLE = True
except ImportError:
    pass
_auth = Depends(verify_token)

# ── Module-level constants used across routers ───────────────────────────────
# These were lost during the refactor that split main.py into routers; the
# routers' `from ..dependencies import *` only picks them up if they are
# defined here AND listed in __all__ below (Python star-imports skip names
# starting with "_" unless explicitly exported).

_FITS_SUFFIXES = {".fits", ".fit", ".fts"}

_COSMOLOGY_MODELS: dict[str, Any] = {}

_HIPS_CACHE_DIR = _Path(
    os.environ.get(
        "VISIVO_HIPS_CACHE",
        str(_Path.home() / ".cache" / "visivo" / "hips"),
    )
)
_HIPS_CACHE_DIR.mkdir(parents=True, exist_ok=True)
_HIPS_CACHE_MAX_BYTES: int = int(
    os.environ.get("VISIVO_HIPS_CACHE_MAX_BYTES", str(2 * 1024 * 1024 * 1024))
)
_HIPS_CACHE_TTL_SECONDS: int = int(
    os.environ.get("VISIVO_HIPS_CACHE_TTL_SECONDS", str(7 * 24 * 3600))
)
_HIPS_CACHE_CLEANUP_INTERVAL_SECONDS: int = int(
    os.environ.get("VISIVO_HIPS_CACHE_CLEANUP_INTERVAL", "60")
)

_HEALPY_AVAILABLE = False
try:
    import healpy as _hp  # type: ignore
    _HEALPY_AVAILABLE = True
except ImportError:
    logger.warning(
        "[startup] healpy not installed – /v1/hips/*/query_tiles will be unavailable."
    )

_RA_CANDIDATES = ["ra", "RA", "Ra", "RAJ2000", "ra_deg", "RAdeg", "ALPHA_J2000"]
_DEC_CANDIDATES = ["dec", "Dec", "DEC", "DEJ2000", "dec_deg", "DECdeg", "DELTA_J2000"]

# Star-import allow-list: Python's `from x import *` skips underscore-prefixed
# names unless they appear in __all__. Routers rely on this module's helpers
# (e.g. _normalize_path, _is_fits) being available, so list them explicitly.
__all__ = [
    "logger",
    "_WORKERS", "_POOL", "_DASK_CLIENT", "_DASK_SCHEDULER", "_VTK_AVAILABLE",
    "_auth",
    "_FITS_SUFFIXES",
    "_COSMOLOGY_MODELS",
    "_HIPS_CACHE_DIR", "_HIPS_CACHE_MAX_BYTES", "_HIPS_CACHE_TTL_SECONDS",
    "_HIPS_CACHE_CLEANUP_INTERVAL_SECONDS",
    "_HEALPY_AVAILABLE",
    "_RA_CANDIDATES", "_DEC_CANDIDATES",
    "_default_worker_count", "_pool_initializer", "_error",
    "_apply_tabular_query",
    "_normalize_path", "_parse_vbt_header", "_is_fits",
    "_normalize_catalogue_field", "_catalogue_field_type",
    "_catalogue_metadata_from_dataframe", "_catalogue_csv_metadata",
    "_catalogue_load_dataframe", "_catalogue_metadata",
    "_get_dask_client", "_run", "_run_with_limit",
    "_moment_product_payload", "_pv_product_payload",
    "_require_dataset", "_require_cube_path", "_fits_metadata",
    "_progress_ticker",
    "_load_cosmology_models",
    "_hips_fetch_sync", "_hips_fetch", "_parse_hips_properties",
    "_best_tile_formats", "_tile_url", "_allsky_url",
    "_tile_cache_path", "_allsky_cache_path", "_get_tiles_for_fov",
    "_resolve_target_sync", "_load_overlay_csv", "_catalogue_overlay_sync",
]

def _default_worker_count() -> int:
    """
    Keep the backend conservative by default.

    On shared HPC/login nodes an unbounded cpu_count()-sized pool is too aggressive for
    data-heavy FITS workloads. Users can still override via VISIVO_WORKERS.
    """
    return max(1, min(os.cpu_count() or 4, 4))


def _pool_initializer() -> None:  # pragma: no cover
    """Silence VTK startup noise in worker processes."""
    import logging as _l
    _l.getLogger("vtkmodules").setLevel(_l.ERROR)


def _error(detail: str, status_code: int = 400, error: str = "BadRequest") -> JSONResponse:
    return JSONResponse(
        status_code=status_code,
        content=ErrorDetail(error=error, detail=detail).model_dump(),
    )


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


def _catalogue_field_type(dtype: Any) -> str:
    if pd.api.types.is_bool_dtype(dtype):
        return "bool"
    if pd.api.types.is_integer_dtype(dtype):
        return "int"
    if pd.api.types.is_float_dtype(dtype):
        return "float"
    return "string"


def _catalogue_metadata_from_dataframe(frame: pd.DataFrame, fmt: str) -> dict[str, Any]:
    fields: list[dict[str, str]] = []
    normalized_to_actual: dict[str, str] = {}
    for column in frame.columns:
        name = str(column)
        normalized_to_actual[_normalize_catalogue_field(name)] = name
        fields.append({"name": name, "type": _catalogue_field_type(frame[column].dtype)})

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
        raise ValueError("Catalogue requires RA and DEC columns.")

    return {
        "kind": "catalogue",
        "format": fmt,
        "num_rows": int(len(frame.index)),
        "fields": fields,
        "coordinate_fields": {"ra": ra_field, "dec": dec_field},
    }


def _catalogue_csv_metadata(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle)
        headers = next(reader, [])
    if not headers:
        raise ValueError("CSV catalogue is empty.")
    frame = pd.read_csv(path)
    return _catalogue_metadata_from_dataframe(frame, "csv")


def _catalogue_load_dataframe(path: Path, fmt: str) -> pd.DataFrame:
    fmt_lower = fmt.strip().lower()
    if fmt_lower == "csv":
        return pd.read_csv(path)
    if fmt_lower in {"votable", "vot", "xml"}:
        return Table.read(path, format="votable").to_pandas()
    if fmt_lower in {"fits", "fit", "fits-table"}:
        return Table.read(path, format="fits").to_pandas()
    raise ValueError(f"Unsupported catalogue format: {fmt}")


def _catalogue_metadata(path: Path, fmt: str) -> tuple[pd.DataFrame, dict[str, Any]]:
    frame = _catalogue_load_dataframe(path, fmt)
    if frame.empty and len(frame.columns) == 0:
        raise ValueError("Catalogue is empty.")
    return frame, _catalogue_metadata_from_dataframe(frame, fmt.strip().lower())


async def _get_dask_client():
    global _DASK_CLIENT
    if _DASK_CLIENT is None and _DASK_SCHEDULER:
        try:
            from dask.distributed import Client
            _DASK_CLIENT = await Client(_DASK_SCHEDULER, asynchronous=True)
            logger.info(f"Connected to Dask cluster at {_DASK_SCHEDULER}")
        except ImportError:
            logger.warning("VISIVO_DASK_SCHEDULER is set but dask is not installed. Falling back to ProcessPoolExecutor.")
            _DASK_CLIENT = False  # False means tried and failed
        except Exception as e:
            logger.warning(f"Failed to connect to Dask cluster: {e}. Falling back to ProcessPoolExecutor.")
            _DASK_CLIENT = False
    return _DASK_CLIENT if _DASK_CLIENT else None


async def _run(fn, *args) -> Any:
    """Run a CPU-bound function via Dask (if configured) or local ProcessPoolExecutor."""
    client = await _get_dask_client()
    if client:
        future = client.submit(fn, *args)
        return await future
    else:
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


def _fits_metadata(path: Path) -> tuple[str, dict[str, Any]]:
    dataset = ScientificFitsDataset(path)
    geometry = dataset.geometry_metadata()
    return str(geometry["kind"]), geometry


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


@functools.lru_cache(maxsize=32)
def _load_overlay_csv(path: str) -> pd.DataFrame:
    """Read and parse a catalogue CSV, caching up to 32 files (LRU eviction)."""
    return pd.read_csv(path)


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
