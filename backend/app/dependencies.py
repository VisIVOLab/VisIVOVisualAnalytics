import asyncio
import contextlib
import asyncio as _asyncio
import base64
import csv
import hashlib
import functools
import logging
import math as _math
import os
import struct
import sys as _sys
import tempfile
import threading
import time
import urllib.error as _urllib_error
import urllib.request as _urllib_request
import uuid
from concurrent.futures import ProcessPoolExecutor
from concurrent.futures.process import BrokenProcessPool
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
from .auth import verify_token, multi_user_mode
from .fits_dataset import ScientificFitsDataset
from .product_cache import PRODUCT_CACHE
from .sessions import REGISTRY, Session, get_session
from .tasks import TASKS
from .schemas import *
logger = logging.getLogger('visivo.backend')

# Ensure the backend package root (the parent of ``app/``) is on sys.path with an
# ABSOLUTE path. The ProcessPoolExecutor below uses the 'spawn' start method, and
# spawn child processes reconstruct sys.path from the PARENT's sys.path at submit
# time. When the backend is launched from a working directory other than
# ``backend/`` (e.g. the desktop app spawns it, or uvicorn is run from the repo
# root), the parent's only reference to ``app`` may be the empty '' (cwd) entry,
# which resolves to the WRONG directory in the child — so the spawn worker raised
# ``ModuleNotFoundError: No module named 'app'`` when unpickling a worker
# function. Inserting the absolute root here (parent import time, before the pool
# snapshots sys.path) fixes moment/isosurface/etc. tasks regardless of launch cwd.
_BACKEND_ROOT = str(_Path(__file__).resolve().parents[1])
if _BACKEND_ROOT not in _sys.path:
    _sys.path.insert(0, _BACKEND_ROOT)

_WORKERS = int(os.environ.get('VISIVO_WORKERS', '4'))
# Use 'spawn' instead of the default 'fork' so worker processes start
# with a fresh Python interpreter rather than inheriting the parent's
# state. This is REQUIRED when the workers use CuPy / CUDA: CUDA
# contexts cannot cross a fork boundary, and child processes inherit
# a half-initialised state that surfaces as
# ``cudaErrorInitializationError`` on first cupy call. The cost is
# slightly higher worker-startup latency (~50 ms each, once); since
# the pool is process-lifetime, this is amortised away.
#
# The behaviour can be forced with VISIVO_MP_START_METHOD=fork
# (e.g. on CUDA-less builds where the fork penalty isn't worth it).
import multiprocessing as _mp
_MP_METHOD = os.environ.get('VISIVO_MP_START_METHOD', 'spawn')
try:
    _MP_CONTEXT = _mp.get_context(_MP_METHOD)
except ValueError:
    logger.warning(
        "VISIVO_MP_START_METHOD=%r not available; falling back to default fork",
        _MP_METHOD,
    )
    _MP_CONTEXT = _mp.get_context()

# ── GPU round-robin assignment ────────────────────────────────────────────────
# When several CUDA-capable GPUs are visible (e.g. SLURM allocates the whole
# node with --gres gpu:4), we pin each worker to a single device via
# CUDA_VISIBLE_DEVICES BEFORE the worker imports cupy. Otherwise cupy would
# default to device 0 for every worker and 3 GPUs would sit idle.
#
# Detection order:
#   1. CUDA_VISIBLE_DEVICES inherited from the job environment (the
#      respect-SLURM path). Gives us the list of physical IDs allocated to
#      this process.
#   2. Fallback: ask nvidia-smi -L for the host's GPU list (useful when the
#      backend is run outside SLURM).
#   3. None: the pool runs CPU-only; no pinning happens.
#
# Opt out with VISIVO_GPU_ROUND_ROBIN=0 (e.g. when debugging a single GPU
# from multiple workers, or when the user explicitly wants device-0 default).
def _detect_visible_gpus() -> list[str]:
    raw = os.environ.get('CUDA_VISIBLE_DEVICES', '')
    parsed = [g.strip() for g in raw.split(',') if g.strip()]
    if parsed:
        return parsed
    try:
        import subprocess  # noqa: PLC0415
        out = subprocess.check_output(
            ['nvidia-smi', '-L'], text=True, timeout=5,
        )
        return [str(i) for i, line in enumerate(out.splitlines())
                if line.strip().startswith('GPU')]
    except Exception:
        return []


_GPU_ROUND_ROBIN = os.environ.get('VISIVO_GPU_ROUND_ROBIN', '1') == '1'
_VISIBLE_GPUS = _detect_visible_gpus() if _GPU_ROUND_ROBIN else []
_GPU_COUNTER = _MP_CONTEXT.Value('i', 0) if _VISIBLE_GPUS else None
if _VISIBLE_GPUS:
    logger.info(
        "[startup] GPU round-robin enabled: %d worker(s) over GPU(s) %s",
        _WORKERS, ','.join(_VISIBLE_GPUS),
    )


def _worker_init(counter, visible_gpus) -> None:  # pragma: no cover
    """Run once at worker startup. Pin CUDA device + silence VTK noise.

    Pinning happens BEFORE cupy is imported (compute.py is lazy-loaded
    per-task), so each worker sees exactly one GPU and ``cupy.zeros(1)``
    binds to it. With 4 workers and 4 visible GPUs the assignment is
    1:1; with N workers and K<N GPUs it's round-robin (N/K workers per
    GPU).
    """
    import logging as _l
    import os as _os
    _l.getLogger("vtkmodules").setLevel(_l.ERROR)
    if counter is None or not visible_gpus:
        return
    with counter.get_lock():
        idx = counter.value
        counter.value += 1
    pinned = visible_gpus[idx % len(visible_gpus)]
    _os.environ['CUDA_VISIBLE_DEVICES'] = pinned
    # Print rather than log: logging.basicConfig may not have run in
    # the spawn worker yet. Goes to the SLURM job log.
    print(  # noqa: T201
        f"[VisIVO/worker] pid={_os.getpid()} pinned to GPU {pinned} "
        f"(slot {idx} of {len(visible_gpus)})",
        flush=True,
    )


def _new_pool() -> ProcessPoolExecutor:
    return ProcessPoolExecutor(
        max_workers=_WORKERS,
        mp_context=_MP_CONTEXT,
        initializer=_worker_init,
        initargs=(_GPU_COUNTER, _VISIBLE_GPUS),
    )


_POOL = _new_pool()
# Guards the replacement of a broken pool: several requests usually fail
# together, and only the first of them should build the replacement.
_POOL_LOCK = threading.Lock()
# Counts how many times the pool had to be rebuilt, so /health can say so.
_POOL_RESTARTS = 0


def shutdown_pool(timeout_s: float = 3.0) -> None:
    """Stop the worker pool and make sure its children are gone.

    A ProcessPoolExecutor's workers are separate processes: they outlive the
    backend unless it shuts the pool down, and then they outlive the desktop
    client too. Measured on this machine after a day of development: 317 stray
    interpreters holding 16.8 GB.

    Waiting politely is not enough. ``cancel_futures`` drops work that has not
    started, but a task already RUNNING cannot be cancelled — and the client
    gives the backend three seconds before killing it, so a quit during a moment
    map would leave exactly the orphans this exists to prevent. So: stop
    accepting, wait briefly, then terminate whatever is still alive and reap it.

    Deliberately bounded. A shutdown path that can block forever is worse than
    one that leaves a process behind, because it hangs the quit as well.
    """
    import os, signal, time as _time  # noqa: PLC0415 - only needed on this path

    with _POOL_LOCK:
        pool = _POOL
    # Snapshot the children before shutdown(): the executor clears the map as it
    # joins them, and we need the pids to check up on the stragglers.
    children = [p for p in getattr(pool, "_processes", {}).values() if p is not None]
    pool.shutdown(wait=False, cancel_futures=True)

    deadline = _time.monotonic() + max(0.0, timeout_s)
    while _time.monotonic() < deadline:
        if not any(p.is_alive() for p in children):
            break
        _time.sleep(0.05)

    survivors = [p for p in children if p.is_alive()]
    for p in survivors:
        # SIGTERM first: a worker between tasks exits on it cleanly.
        with contextlib.suppress(Exception):
            p.terminate()
    for p in survivors:
        with contextlib.suppress(Exception):
            p.join(0.5)
    hard = [p for p in survivors if p.is_alive()]
    for p in hard:
        # Still there — it is inside a long native call (a VTK pass, a large
        # numpy reduction) that no signal will interrupt politely.
        with contextlib.suppress(Exception):
            os.kill(p.pid, signal.SIGKILL)
            p.join(0.5)
    if children:
        logger.info(
            "[shutdown] worker pool stopped (%d worker(s), %d terminated, %d killed)",
            len(children), len(survivors) - len(hard), len(hard))
    else:
        logger.info("[shutdown] worker pool stopped (no workers had been started)")


def pool_health() -> dict[str, Any]:
    """How the local worker pool is doing, for ``/v1/health``."""
    return {"workers": _WORKERS, "restarts": _POOL_RESTARTS}


def _replace_broken_pool(pool: ProcessPoolExecutor, fn) -> None:
    """Swap in a fresh pool, unless somebody already did it for this one."""
    global _POOL, _POOL_RESTARTS
    with _POOL_LOCK:
        if _POOL is not pool:
            return                      # another caller got there first
        logger.error(
            "[pool] worker pool is broken (a worker process died) around %s; replacing it",
            getattr(fn, "__name__", fn),
        )
        _POOL_RESTARTS += 1
        _POOL = _new_pool()
    try:
        pool.shutdown(wait=False)
    except Exception:                   # noqa: BLE001 - already broken; never mask the caller's error
        logger.debug("[pool] shutdown of the broken pool failed", exc_info=True)


async def _run_in_pool(fn, *args) -> Any:
    """Submit to the local process pool, surviving a worker that dies.

    A ProcessPoolExecutor whose worker is killed (OOM, a segfaulting native
    library, an operator killing the process group) is **permanently broken**:
    every later submit raises, and since this pool is a module global that means
    every slice, preview and probe fails for the lifetime of the backend while
    ``/health`` keeps answering instantly and the client keeps reporting
    "backend ok". Observed exactly that way — a wedged backend whose only
    symptom was "[Errno 32] Broken pipe" in the status bar and a 2-D view that
    never loaded.

    So a broken pool is always replaced. Whether *this* call may be retried
    turns on one question — did its work ever start? — and there is a reliable
    signal for it, which "somebody else replaced the pool first" is not:

    * ``submit()`` **rejected** the task (``run_in_executor`` calls it
      synchronously): the pool was already broken, our work never ran, another
      request's task is what killed the worker. Safe to retry on the new pool.
    * the **future** failed while we awaited it: our task had been accepted, and
      nothing here can tell whether it was the one that killed the worker or an
      innocent bystander that was merely queued behind it. Resubmitting a killer
      would take the replacement pool down too, so this request fails — and the
      next one finds a healthy pool.
    """
    loop = _asyncio.get_running_loop()
    for attempt in (0, 1):
        pool = _POOL
        try:
            future = loop.run_in_executor(pool, fn, *args)
        except BrokenProcessPool:
            _replace_broken_pool(pool, fn)
            if attempt == 0:
                continue                # never ran → retry once on the fresh pool
            raise
        try:
            return await future
        except BrokenProcessPool:
            _replace_broken_pool(pool, fn)
            raise
    raise AssertionError("unreachable: the loop either returns or raises")


_DASK_CLIENT = None
_DASK_SCHEDULER = os.environ.get('VISIVO_DASK_SCHEDULER', '')
# Dask is now ON by default (auto). Modes: 'auto' → use a distributed scheduler
# when VISIVO_DASK_SCHEDULER is set, otherwise fall back to a local *threaded*
# dask.array scheduler for large out-of-core jobs; 'off' disables dask;
# 'local' forces local threaded dask; 'distributed' requires a scheduler.
_DASK_MODE = os.environ.get('VISIVO_DASK_MODE', 'auto').strip().lower()
# Minimum materialised-subset size before the local threaded dask path is worth
# its overhead. Small cubes stay on the fast in-process worker path.
_DASK_MIN_BYTES = int(os.environ.get('VISIVO_DASK_MIN_BYTES', str(512 * 1024 ** 2)))
# Orders the chunked dask.array path (_moment_map_dask) can compute.
_DASK_MOMENT_ORDERS = frozenset({0, 1, 2, 3, 4, 5, 6, 8, 10})
_DASK_AVAILABLE: "bool | None" = None
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
# Timestamp of the last cleanup run — throttles _maybe_cleanup_hips_cache.
_last_hips_cache_cleanup_ts: float = 0.0


def _iter_hips_cache_files():
    """Yield regular files under the HiPS cache dir, recursively.

    Skips directories and symlinks; tolerant of files vanishing mid-walk.
    """
    root = _HIPS_CACHE_DIR
    if not root.exists():
        return
    for path in root.rglob("*"):
        try:
            if path.is_file() and not path.is_symlink():
                yield path
        except OSError:
            continue


def _purge_expired_hips_cache(now: float | None = None) -> int:
    """Delete cache files older than the TTL. Returns the number removed.

    Tolerant of concurrent deletion (a file vanishing between listing and
    stat/unlink is simply skipped, not an error)."""
    if now is None:
        now = time.time()
    ttl = _HIPS_CACHE_TTL_SECONDS
    removed = 0
    for path in list(_iter_hips_cache_files()):
        try:
            if now - path.stat().st_mtime > ttl:
                path.unlink()
                removed += 1
        except FileNotFoundError:
            continue
        except OSError:
            continue
    return removed


def _enforce_hips_cache_size_limit() -> int:
    """Evict least-recently-used files until the cache is under the byte limit.

    LRU is approximated by mtime (refreshed on access via os.utime upstream).
    Returns the number of files removed."""
    entries: list[tuple[float, int, "_Path"]] = []
    total = 0
    for path in list(_iter_hips_cache_files()):
        try:
            st = path.stat()
        except (FileNotFoundError, OSError):
            continue
        entries.append((st.st_mtime, st.st_size, path))
        total += st.st_size

    limit = _HIPS_CACHE_MAX_BYTES
    if total <= limit:
        return 0

    entries.sort(key=lambda e: e[0])  # oldest mtime first (evict first)
    removed = 0
    for _mtime, size, path in entries:
        if total <= limit:
            break
        try:
            path.unlink()
            total -= size
            removed += 1
        except FileNotFoundError:
            total -= size
        except OSError:
            continue
    return removed


def _maybe_cleanup_hips_cache() -> None:
    """Run TTL purge + size enforcement, throttled to once per interval.

    Safe to call on every cache write — it is a cheap no-op between runs."""
    global _last_hips_cache_cleanup_ts
    now = time.time()
    if now - _last_hips_cache_cleanup_ts < _HIPS_CACHE_CLEANUP_INTERVAL_SECONDS:
        return
    _last_hips_cache_cleanup_ts = now
    _purge_expired_hips_cache(now=now)
    _enforce_hips_cache_size_limit()

# ── Workspace Exports directory ────────────────────────────────────────────────
# Persistent landing zone for FITS artefacts produced by VisIVO's "export"
# workers (sub-cube crops, moment-map FITS). Unlike tempdir outputs, files
# here survive across sessions and are browsable / re-openable from the
# client's Workspace Exports panel. Files are flat-namespaced; basename
# collisions are resolved by `make_workspace_path()` with a numeric suffix.
_EXPORTS_DIR = _Path(
    os.environ.get(
        "VISIVO_EXPORTS_DIR",
        str(_Path.home() / ".visivo" / "exports"),
    )
)
_EXPORTS_DIR.mkdir(parents=True, exist_ok=True)


def get_exports_dir() -> _Path:
    """Return the (created) workspace exports directory."""
    return _EXPORTS_DIR


def make_workspace_path(basename: str, reserve: bool = False) -> _Path:
    """Resolve a user-provided basename into a safe workspace path,
    auto-suffixing on collision (``cube.fits`` → ``cube_1.fits`` → …).

    With ``reserve=True`` the name is claimed atomically (an empty file is
    created), so two concurrent writers cannot be handed the same path. It is
    opt-in because a caller that decides not to write after all — a catalogue
    search that returns no rows — would otherwise leave a phantom artefact in
    the workspace listing. Callers that reserve are responsible for removing
    the placeholder if they fail.

    The basename is sanitised: any directory separators are stripped so
    callers cannot break out of the workspace; an empty / dot-only name
    falls back to ``export.fits``.
    """
    raw = (basename or "").strip()
    # Collapse path traversal: take only the final component, strip leading
    # dots (no hidden files), and refuse the empty string.
    safe = _Path(raw).name.lstrip(".")
    if not safe:
        safe = "export.fits"
    # If no extension, default to .fits (the only artefact type for now).
    if "." not in safe:
        safe = safe + ".fits"
    def _usable(path: _Path) -> bool:
        """Free, and still inside the workspace once symlinks are followed.

        ``exists()`` is False for a DANGLING symlink, so a link left in the
        exports directory pointing at a path outside it would be handed back as
        "free" and the writer would then create the target through it. Nobody
        reaches this without write access to the exports directory, but the
        check is one line and the failure is silent.
        """
        if path.exists() or path.is_symlink():
            return False
        try:
            resolved = path.resolve()
        except OSError:
            return False
        root = _EXPORTS_DIR.resolve()
        return resolved == root / resolved.name and root in resolved.parents

    def _reserve(path: _Path) -> bool:
        """Claim the name, atomically when asked to.

        Checking that a name is free and then returning it is a race: two
        exports started together both see it free, both are handed the same
        path, and the second one to finish overwrites the first — with both
        registered dataset ids pointing at one file. O_EXCL makes the check and
        the claim one operation.
        """
        if not _usable(path):
            return False
        if not reserve:
            return True
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        except OSError:
            return False
        os.close(fd)
        return True

    candidate = _EXPORTS_DIR / safe
    if _reserve(candidate):
        return candidate
    stem = candidate.stem
    suffix = candidate.suffix
    for n in range(1, 10000):
        alt = _EXPORTS_DIR / f"{stem}_{n}{suffix}"
        if _reserve(alt):
            return alt
    # Pathological: 10k+ collisions on the same basename. Caller likely has a
    # bug; fail loud rather than overwrite.
    raise RuntimeError(
        f"Too many basename collisions for '{safe}' in {_EXPORTS_DIR}"
    )


def resolve_workspace_path(filename: str) -> _Path:
    """Resolve a workspace-relative filename to its absolute path,
    refusing any input that tries to escape ``_EXPORTS_DIR``.

    Raises ``ValueError`` on traversal attempts; the file does NOT need
    to exist (callers can use the returned path to check existence or
    construct new files).
    """
    raw = (filename or "").strip()
    if not raw:
        raise ValueError("Empty filename.")
    safe = _Path(raw).name
    if safe != raw or safe in (".", ".."):
        raise ValueError(f"Invalid workspace filename: {filename!r}")
    return _EXPORTS_DIR / safe


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
    "_WORKERS", "_POOL", "pool_health", "shutdown_pool", "_DASK_CLIENT", "_DASK_SCHEDULER", "_VTK_AVAILABLE",
    "_auth",
    "_FITS_SUFFIXES",
    "_COSMOLOGY_MODELS",
    "_HIPS_CACHE_DIR", "_HIPS_CACHE_MAX_BYTES", "_HIPS_CACHE_TTL_SECONDS",
    "_HIPS_CACHE_CLEANUP_INTERVAL_SECONDS", "_last_hips_cache_cleanup_ts",
    "_iter_hips_cache_files", "_purge_expired_hips_cache",
    "_enforce_hips_cache_size_limit", "_maybe_cleanup_hips_cache",
    "_EXPORTS_DIR", "get_exports_dir", "make_workspace_path",
    "resolve_workspace_path",
    "_HEALPY_AVAILABLE",
    "_RA_CANDIDATES", "_DEC_CANDIDATES",
    "_default_worker_count", "_pool_initializer", "_error",
    "_apply_tabular_query",
    "data_roots", "hide_hidden_entries", "_normalize_path", "_parse_vbt_header", "_is_fits", "_is_supported_dataset",
    "_normalize_catalogue_field", "_catalogue_field_type",
    "_catalogue_metadata_from_dataframe", "_catalogue_csv_metadata",
    "_catalogue_load_dataframe", "_catalogue_metadata",
    "_get_dask_client", "_run", "_run_io", "_run_interactive", "_run_with_limit",
    "acquire_task_slot", "task_slot_busy_error",
    "_run_heavy", "_run_heavy_with_limit",
    "_moment_product_payload", "_pv_product_payload",
    "_require_dataset", "_require_cube_path", "_fits_metadata", "record_history",
    "assert_safe_fetch_url",
    "_progress_ticker",
    "_load_cosmology_models",
    "_hips_fetch_sync", "_hips_fetch", "_parse_hips_properties",
    "_load_hips_registry_sync", "_parse_hips_registry", "_HIPS_SKY_FRAMES",
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


def _pool_initializer(*_legacy_args) -> None:  # pragma: no cover
    """Backward-compat shim.

    The real worker initializer is :func:`_worker_init` defined above the
    ``_POOL`` construction in this module. This stub is retained because the
    star-import allow-list (``__all__``) exports the name; renaming it would
    silently drop references in routers that ``from ..dependencies import *``.
    Calling it directly is a no-op.
    """
    return None


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


def data_roots() -> list[Path] | None:
    """Directories the path-based endpoints may touch.

    Three outcomes, and the distinction between the last two matters:

    * a non-empty list — confined to exactly those roots. Set
      ``VISIVO_DATA_ROOTS`` (os.pathsep-separated) and this holds in both modes.
    * ``[]`` — unconfined. Only when a single token is accepted: the backend then
      acts on behalf of the one person running it, and browsing their own
      filesystem is the point of a remote backend.
    * ``None`` — DENY. Multi-user mode with no roots configured. Failing closed
      is deliberate: the tokens belong to different people, and a default of, say,
      the service account's home would silently share whatever happens to live
      there. The operator must state what is shareable.

    Re-read per call so a deployment can change it without a restart, matching
    ``accepted_tokens()``.
    """
    raw = os.environ.get("VISIVO_DATA_ROOTS", "").strip()
    roots = [Path(part).expanduser().resolve()
             for part in raw.split(os.pathsep) if part.strip()]
    if roots:
        return roots
    return None if multi_user_mode() else []


def hide_hidden_entries() -> bool:
    """True when dot-entries must be invisible: they hold credentials
    (``~/.visivo_token``, ``~/.ssh``) and are never scientific data."""
    return multi_user_mode()


def _normalize_path(raw: str) -> Path:
    """Resolve a client-supplied path, confined to :func:`data_roots`.

    Raises ``PermissionError`` for anything outside them. Callers wrap this in
    the ``valid=False`` envelope, which keeps the refusal non-enumerable: a
    denied path and a missing one look the same to the client.
    """
    roots = data_roots()
    if roots is None:
        raise PermissionError(
            "Path-based access is disabled: a shared backend (VISIVO_TOKENS) must "
            "declare VISIVO_DATA_ROOTS before any filesystem path is served.")
    path = Path.home() if not raw else Path(raw).expanduser().resolve()
    if roots and not any(path == root or root in path.parents for root in roots):
        raise PermissionError("Path is outside the backend's allowed data roots.")
    if hide_hidden_entries() and any(
            part.startswith(".") and part not in (".", "..") for part in path.parts):
        raise PermissionError("Hidden paths are not accessible.")
    return path


def _parse_vbt_header(head_path: Path) -> dict:
    """Parse a VisIVO Binary Table .head file and return metadata dict."""
    with open(head_path, "r") as fh:
        lines = [ln.strip() for ln in fh.readlines() if ln.strip()]
    if len(lines) < 4:
        raise ValueError(f"VBT header too short: expected at least 4 lines, got {len(lines)}")

    scalar_str = lines[0].lower()
    if scalar_str in ("f", "float"):
        scalar_type = "float32"
        np_itemcode = "f4"
    elif scalar_str in ("d", "double"):
        scalar_type = "float64"
        np_itemcode = "f8"
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

    # The byte order was parsed and then thrown away: the numpy code was fixed
    # to "<" by the scalar type above, so EVERY VBT was read as little-endian
    # and a big-endian one came back as garbage — NaNs and values at the
    # float32 limit, with nothing to say why. The header line exists precisely
    # to be obeyed.
    np_endian_code = ("<" if file_endian == "little" else ">") + np_itemcode

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


def _is_supported_dataset(path: Path) -> bool:
    """Accept FITS and HDF5 beamformed / dynamic-spectrum files."""
    from .hdf5_dynspec import is_hdf5_path  # noqa: PLC0415 (lazy h5py import)
    return path.is_file() and (
            path.suffix.lower() in _FITS_SUFFIXES
            or is_hdf5_path(path))


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

    def _first(aliases: tuple[str, ...]) -> str:
        for a in aliases:
            if a in normalized_to_actual:
                return normalized_to_actual[a]
        return ""

    ra_field = _first(("RA", "RADEG", "RAJ2000", "RAJ2000OBS", "ALPHA", "RAICRS"))
    dec_field = _first(("DEC", "DE", "DECDEG", "DEJ2000", "DECJ2000", "DELTA", "DECICRS"))
    # Cartesian (3-D) catalogues — e.g. Partiview .speck or supergalactic XYZ
    # reconstructions — carry positions directly instead of RA/DEC.
    x_field = _first(("X", "SGX", "XMPC", "POSX", "PX"))
    y_field = _first(("Y", "SGY", "YMPC", "POSY", "PY"))
    z_field = _first(("Z", "SGZ", "ZMPC", "POSZ", "PZ"))

    # Galactic catalogues carry l/b instead. Every VLKB table is one of these —
    # compact sources (glon/glat), the band-merged view (glonft/glatft), bubbles
    # (glon_cen/glat_cen) — and refusing them meant a query could be run, saved,
    # and then not opened, which is where the chain used to end.
    glon_field = _first(("GLON", "GLONFT", "GLONCEN", "L", "LII", "LONGITUDE", "GLONDEG"))
    glat_field = _first(("GLAT", "GLATFT", "GLATCEN", "B", "BII", "LATITUDE", "GLATDEG"))

    if ra_field and dec_field:
        system = "sky"
    elif glon_field and glat_field:
        system = "galactic"
    elif x_field and y_field and z_field:
        system = "cartesian"
    else:
        raise ValueError(
            "Catalogue requires RA/DEC, Galactic l/b, or X/Y/Z columns.")

    return {
        "kind": "catalogue",
        "format": fmt,
        "num_rows": int(len(frame.index)),
        "fields": fields,
        "coordinate_fields": {
            "ra": ra_field, "dec": dec_field,
            # A galactic catalogue reports its own columns here: the viewer is
            # told the FRAME rather than handed l/b relabelled as RA/Dec, which
            # would put every source in the wrong place by up to 90 degrees.
            "glon": glon_field, "glat": glat_field,
            "x": x_field, "y": y_field, "z": z_field,
            "system": system,
        },
    }


def _catalogue_csv_metadata(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle)
        headers = next(reader, [])
    if not headers:
        raise ValueError("CSV catalogue is empty.")
    frame = pd.read_csv(path)
    return _catalogue_metadata_from_dataframe(frame, "csv")


def _read_speck(path: Path) -> pd.DataFrame:
    """Parse a Partiview ``.speck`` point file → DataFrame.

    Columns are X, Y, Z (Cartesian, typically Mpc) followed by the attributes
    declared by ``datavar <index> <name>`` header lines. Data rows are lines
    whose first token is numeric (``x y z attr0 attr1 …``, optional trailing
    text label/comment); all other directive lines are ignored.
    """
    idx_names: dict[int, str] = {}
    data_rows: list[list[str]] = []
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            toks = line.split()
            if toks[0].lower() == "datavar" and len(toks) >= 3:
                try:
                    idx_names[int(toks[1])] = toks[2]
                except ValueError:
                    pass
                continue
            try:
                float(toks[0])           # data lines start with a number (the X coord)
            except ValueError:
                continue                 # any other directive (texture, mesh, …)
            data_rows.append(toks)
    ndv = (max(idx_names) + 1) if idx_names else 0
    cols = ["X", "Y", "Z"] + [idx_names.get(i, f"var{i}") for i in range(ndv)]
    rows: list[list[float]] = []
    for toks in data_rows:
        vals: list[float] = []
        for tok in toks[: 3 + ndv]:
            try:
                vals.append(float(tok))
            except ValueError:
                vals.append(float("nan"))
        vals.extend([float("nan")] * (len(cols) - len(vals)))
        rows.append(vals)
    if not rows:
        raise ValueError("No data points found in .speck file.")
    return pd.DataFrame(rows, columns=cols)


def _read_ipac(path: Path) -> pd.DataFrame:
    """Read an IPAC ASCII table (``.tbl``).

    Format: ``\\``-prefixed comment/keyword lines, then ``|``-delimited header
    rows (column names / types / units) followed by whitespace-aligned data —
    e.g. cosmological-snapshot subtables with ``x y z`` (Mpc) + ``vx vy vz`` +
    attribute columns. astropy parses it natively; keeping the column names lets
    the X/Y/Z detection treat it as a Cartesian catalogue.
    """
    from astropy.io import ascii as astropy_ascii  # noqa: PLC0415

    return astropy_ascii.read(str(path), format="ipac").to_pandas()


def _catalogue_load_dataframe(path: Path, fmt: str) -> pd.DataFrame:
    fmt_lower = fmt.strip().lower()
    if fmt_lower == "csv":
        return pd.read_csv(path)
    if fmt_lower in {"votable", "vot", "xml"}:
        return Table.read(path, format="votable").to_pandas()
    if fmt_lower in {"fits", "fit", "fits-table"}:
        return Table.read(path, format="fits").to_pandas()
    if fmt_lower == "speck":
        return _read_speck(path)
    if fmt_lower in {"ipac", "tbl"}:
        return _read_ipac(path)
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


# ── Sync Dask client for dask.array distributed compute ──────────────────────
# The async client above is convenient for ``await client.submit(fn).result()``
# task-level dispatch (one whole worker_moment per worker). For the
# **chunked** path — ``dask.array.from_array(memmap).compute()`` — we need
# a SYNCHRONOUS Client because the call originates from a thread spun up
# by ``loop.run_in_executor`` and must block until the distributed graph
# completes. We keep a second connection because mixing async and sync
# client APIs on the same instance is fragile and the cost of a second
# scheduler connection is negligible.
_DASK_SYNC_CLIENT = None


def _get_dask_client_sync():
    """Lazy-init a sync ``distributed.Client`` for chunked dask.array work.

    Returns ``None`` if Dask is unavailable / not configured / failed.
    Safe to call from any thread; the first call initialises the connection.
    """
    global _DASK_SYNC_CLIENT
    if _DASK_SYNC_CLIENT is None and _DASK_SCHEDULER:
        try:
            from dask.distributed import Client
            _DASK_SYNC_CLIENT = Client(_DASK_SCHEDULER)
            logger.info(
                "Connected sync Dask client at %s for distributed dask.array work",
                _DASK_SCHEDULER,
            )
        except ImportError:
            _DASK_SYNC_CLIENT = False
        except Exception as e:
            logger.warning(
                "Failed to create sync Dask client at %s: %s. "
                "Chunked moment will fall back to in-process threads.",
                _DASK_SCHEDULER, e,
            )
            _DASK_SYNC_CLIENT = False
    return _DASK_SYNC_CLIENT if _DASK_SYNC_CLIENT else None


def _dask_array_available() -> bool:
    """True if ``dask.array`` can be imported (cached). Needed before routing a
    job to the local threaded dask path, which uses dask.array directly."""
    global _DASK_AVAILABLE
    if _DASK_AVAILABLE is None:
        try:
            import dask.array  # noqa: F401  (probe only)
            _DASK_AVAILABLE = True
        except Exception:
            _DASK_AVAILABLE = False
    return _DASK_AVAILABLE


def _estimate_moment_subset_bytes(dataset, request) -> int:
    """Cheap upper-bound on the float32 bytes a moment over the requested
    channel range would materialise (NAXIS1·NAXIS2·nchannels·4), from the
    header alone — no data read."""
    try:
        nx = int(dataset.header.get("NAXIS1", 0) or 0)
        ny = int(dataset.header.get("NAXIS2", 0) or 0)
        nch_total = int(dataset.header.get("NAXIS3", 0) or 0)
        cs = int(request.channel_start)
        ce = int(request.channel_end)
        nch = (ce - cs + 1) if ce >= cs else nch_total
        if nch <= 0:
            nch = nch_total or 1
        return max(0, nx) * max(0, ny) * max(1, nch) * 4
    except Exception:
        return 0


def _select_moment_backend(order: int, subset_bytes: int):
    """Choose the moment compute backend under VISIVO_DASK_MODE.

    Returns ``(use_dask, dist_client)``:
      * ``(True, client)``  → chunked dask.array over a **distributed** cluster
      * ``(True, None)``    → chunked dask.array over the **local threaded**
                              scheduler (out-of-core, parallel across cores)
      * ``(False, None)``   → the in-process ``worker_moment`` path

    'auto' (default) prefers a configured distributed scheduler, else uses the
    local threaded path once the job is large enough to amortise dask overhead.
    """
    if _DASK_MODE == "off" or order not in _DASK_MOMENT_ORDERS:
        return (False, None)
    # 'local' forces the local threaded scheduler even if a cluster is
    # configured — it must win before we ever consult the distributed client.
    if _DASK_MODE == "local":
        return (True, None) if _dask_array_available() else (False, None)
    sync_client = _get_dask_client_sync()  # non-None only when a scheduler is set
    if sync_client is not None:
        return (True, sync_client)
    if _DASK_MODE == "distributed":
        return (False, None)  # a cluster was required but none is reachable
    if not _dask_array_available():
        return (False, None)
    if subset_bytes >= _DASK_MIN_BYTES:
        return (True, None)  # auto: local threaded dask once big enough
    return (False, None)


async def _run_io(fn, *args) -> Any:
    """Run a blocking **I/O-bound** call in a thread, not in the process pool.

    The pool behind :func:`_run` exists to keep CPU work off the event loop, and
    its slots are the backend's compute capacity. A remote fetch is not compute:
    it is a socket waiting, for as long as the far end takes — up to the whole
    timeout. Parking that in a worker process spends a compute slot on doing
    nothing, and a handful of slow archives can starve the interactive requests
    (slice, preview, probe) that the pool is sized for.

    A thread is the right place for it: the GIL is released while the socket
    waits, nothing is pickled, and the function still runs in this process —
    which also means a test can patch what it calls.
    """
    loop = _asyncio.get_running_loop()
    return await loop.run_in_executor(None, fn, *args)


async def _run(fn, *args) -> Any:
    """Run a CPU-bound function via Dask (if configured) or local ProcessPoolExecutor.

    Use this for INTERACTIVE / short tasks (slice, preview, probe, noise stats,
    subvolume, cutting-plane updates). Heavy / long tasks (moment, isosurface,
    spectral linewidth/baseline/stack) should go through ``_run_heavy`` instead
    so they share the pool fairly with interactive requests.
    """
    client = await _get_dask_client()
    if client:
        future = client.submit(fn, *args)
        return await future
    else:
        return await _run_in_pool(fn, *args)


async def _run_interactive(fn, *args) -> Any:
    """Run a small, latency-sensitive task **always** in the local pool.

    Unlike :func:`_run`, this never routes through Dask. Reason: Dask
    submit/return overhead for tiny tasks (~100-500 ms baseline) plus
    queueing behind in-flight bulk jobs (subvolume slabs, moment chunks)
    can balloon a 200 ms slice fetch to 20 s+ of perceived latency on
    a busy cluster.

    Use this for **interactive** endpoints where the operator's mental
    model is "as fast as a local read":
      * /v1/cube/slice — channel scrubbing
      * /v1/image/preview — single-channel display
      * (potentially) probe, ROI stats during cursor drag
    """
    return await _run_in_pool(fn, *args)


# ── Heavy-task throttle ──────────────────────────────────────────────────────
#
# A global asyncio Semaphore gates how many "heavy" tasks (multi-second
# scientific compute) can occupy worker pool slots simultaneously. We cap it
# at ``VISIVO_WORKERS - 1`` so there is always at least one pool slot free for
# interactive requests (cube slice scroll, ROI subvolume, probe, …) — the GUI
# stays responsive even while a long compute is running.
#
# Set ``VISIVO_HEAVY_SLOTS=N`` to override (e.g. =VISIVO_WORKERS for maximum
# throughput when interactive responsiveness is not needed; =1 to serialise
# heavy tasks completely).
_HEAVY_SLOTS = max(
    1,
    int(os.environ.get(
        "VISIVO_HEAVY_SLOTS",
        str(max(1, _WORKERS - 1)),
    )),
)
_HEAVY_SEM: asyncio.Semaphore | None = None

# E-03: wall-clock ceiling on a single heavy compute. A pathological or
# abandoned request otherwise wedges a worker (and its heavy-semaphore slot)
# indefinitely, and with only VISIVO_HEAVY_SLOTS slots a few such tasks starve
# the pool for everyone. On timeout the awaiting coroutine is cancelled and the
# slot released, so the server stays responsive; note the underlying
# ProcessPool subprocess may run to completion (Python cannot cancel it), so the
# ceiling should be generous. Set VISIVO_HEAVY_TIMEOUT_S=0 to disable.
_HEAVY_TIMEOUT_S = float(os.environ.get("VISIVO_HEAVY_TIMEOUT_S", "900"))


def _heavy_semaphore() -> asyncio.Semaphore:
    """Lazy-init the semaphore so it binds to the running event loop."""
    global _HEAVY_SEM
    if _HEAVY_SEM is None:
        _HEAVY_SEM = asyncio.Semaphore(_HEAVY_SLOTS)
    return _HEAVY_SEM


async def _run_heavy(fn, *args) -> Any:
    """Like ``_run()``, but throttled by the heavy-task semaphore and bounded by
    a wall-clock timeout (E-03).

    Use for long-running scientific compute (moment maps, isosurface, spectral
    linewidth / baseline / stack). Guarantees at least one pool slot remains
    free for interactive requests dispatched via plain ``_run()``. A task that
    exceeds ``VISIVO_HEAVY_TIMEOUT_S`` raises HTTP 504 instead of hanging.
    """
    sem = _heavy_semaphore()
    if not (_HEAVY_TIMEOUT_S and _HEAVY_TIMEOUT_S > 0):
        async with sem:
            return await _run(fn, *args)

    # Timeout path. We acquire/release the semaphore MANUALLY (not via `async
    # with`) so that on a timeout the slot is held until the underlying work
    # ACTUALLY finishes — the ProcessPool subprocess keeps running (Python can't
    # cancel it), and releasing the slot early would let admission control
    # under-count real worker occupancy and admit unbounded queued heavy work
    # after repeated 504s. Exactly one release happens per task.
    await sem.acquire()
    task = asyncio.ensure_future(_run(fn, *args))
    _released = False

    def _release_once(_fut: Any = None) -> None:
        nonlocal _released
        if not _released:
            _released = True
            sem.release()

    try:
        result = await asyncio.wait_for(asyncio.shield(task), timeout=_HEAVY_TIMEOUT_S)
        _release_once()
        return result
    except asyncio.TimeoutError:
        # Keep the slot until the shielded task really settles (accurate
        # admission control); the client gets 504 now.
        task.add_done_callback(_release_once)
        fname = getattr(fn, "__name__", str(fn))
        logger.warning("[heavy] timeout after %.0fs in %s", _HEAVY_TIMEOUT_S, fname)
        raise HTTPException(
            status_code=status.HTTP_504_GATEWAY_TIMEOUT,
            detail=(
                f"Compute exceeded the {_HEAVY_TIMEOUT_S:.0f}s time limit. "
                "Try a smaller channel range / region, or raise "
                "VISIVO_HEAVY_TIMEOUT_S on the server."
            ),
        ) from None
    except BaseException:
        # The task itself errored/was cancelled before the timeout — release now
        # (or when it settles) so the slot is never leaked.
        if task.done():
            _release_once()
        else:
            task.add_done_callback(_release_once)
        raise


# How long a request waits for a per-session compute slot before giving up with
# 429. The slot limit is admission control, not a quota to be enforced to the
# millisecond: a user who clicks Isosurface while the cube's own stats pass is
# still running should have it accepted, not refused. Note that a heavy task
# QUEUEING on the heavy-task semaphore also holds a session slot, so on a big
# cube the slots can all be held by tasks that are not computing yet — refusing
# instantly turned that into a hard "too many requests" on a single click.
# Set VISIVO_TASK_SLOT_WAIT_S=0 for the old fail-fast behaviour.
_TASK_SLOT_WAIT_S = float(os.environ.get("VISIVO_TASK_SLOT_WAIT_S", "15"))
_MAX_CONCURRENT_TASKS_HINT = int(os.environ.get("VISIVO_MAX_CONCURRENT_TASKS", "3"))


async def acquire_task_slot(session: Session) -> bool:
    """Take a per-session compute slot, waiting up to ``VISIVO_TASK_SLOT_WAIT_S``.

    Polling rather than a condition variable: ``Session`` guards its counter with
    a threading lock (it is touched from worker threads too), and awaiting here
    must not block the event loop.
    """
    if session.try_acquire_task_slot():
        return True
    if _TASK_SLOT_WAIT_S <= 0:
        return False
    deadline = time.monotonic() + _TASK_SLOT_WAIT_S
    while time.monotonic() < deadline:
        await asyncio.sleep(0.2)
        if session.try_acquire_task_slot():
            return True
    return False


def task_slot_busy_error() -> HTTPException:
    """The 429 to raise when no slot came free. Says what was tried, so the
    client can report backpressure rather than a bare 'too many requests'."""
    return HTTPException(
        status_code=status.HTTP_429_TOO_MANY_REQUESTS,
        detail=(
            f"All {_MAX_CONCURRENT_TASKS_HINT} compute slots for this session are busy "
            f"(waited {_TASK_SLOT_WAIT_S:.0f}s). Let a running computation finish, then retry."
        ),
    )


async def _run_with_limit(session: Session, fn, *args) -> Any:
    """Like _run(), but enforces per-session concurrent task limit (HTTP 429 on overflow)."""
    if not await acquire_task_slot(session):
        raise task_slot_busy_error()
    try:
        return await _run(fn, *args)
    finally:
        session.release_task_slot()


async def _run_heavy_with_limit(session: Session, fn, *args) -> Any:
    """Like ``_run_with_limit``, but throttled by the heavy-task semaphore.

    Combines the per-session rate limit (HTTP 429 on overflow) with the global
    heavy-task throttle that leaves at least one pool slot free for interactive
    requests. Use for moment / isosurface / spectral compute endpoints.
    """
    if not await acquire_task_slot(session):
        raise task_slot_busy_error()
    try:
        return await _run_heavy(fn, *args)
    finally:
        session.release_task_slot()


async def _moment_product_payload(entry: dict[str, Any], request: MomentProductRequest) -> tuple[dict[str, Any], bool]:
    from .compute import _moment_map_dask, worker_moment

    dataset = ScientificFitsDataset(entry["path"])
    cache_params = {
        "moment_order": request.moment_order,
        "channel_start": request.channel_start,
        "channel_end": request.channel_end,
        "mask_enabled": request.mask_enabled,
        "threshold_value": request.threshold_value,
        "threshold_auto": request.threshold_auto,
        # R-03: dispersion (default) vs variance changes the order-2 output.
        "moment2_variance": bool(getattr(request, "moment2_variance", False)),
    }
    cache_key, param_hash = PRODUCT_CACHE.make_key(dataset.path, "moment", cache_params)
    cached = PRODUCT_CACHE.get(cache_key)
    if cached is not None:
        return dict(cached.payload), True

    # Backend selection (VISIVO_DASK_MODE, default 'auto'): route large / high-
    # order moments through the chunked dask.array path so they compute out-of-
    # core and in parallel. A configured distributed cluster fans the work out
    # across nodes; otherwise a local threaded dask scheduler parallelises across
    # cores. Small cubes stay on the fast in-process worker_moment path.
    subset_bytes = _estimate_moment_subset_bytes(dataset, request)
    use_dask, dist_client = _select_moment_backend(request.moment_order, subset_bytes)
    if use_dask and dist_client is not None:
        logger.info(
            "[moment] backend=dask-distributed order=%s subset_bytes=%s",
            request.moment_order, subset_bytes,
        )
        loop = asyncio.get_event_loop()
        result = await loop.run_in_executor(
            None,
            _moment_map_dask,
            dataset.path,
            request.moment_order,
            request.channel_start,
            request.channel_end,
            request.threshold_value,
            request.mask_enabled,
            64,                     # chunk_z
            dist_client,            # → distributed cluster
            request.threshold_auto,
            bool(getattr(request, "moment2_variance", False)),
        )
    elif use_dask:
        # Local threaded dask.array — CPU-heavy. Run it DIRECTLY on the local
        # process pool under the heavy semaphore, NOT via _run_heavy/_run: the
        # latter would re-dispatch to an async Dask cluster if one happens to be
        # reachable, turning "local threaded" into a single remote task. Pinning
        # to _POOL keeps it a local subprocess with scheduler="threads" inside.
        logger.info(
            "[moment] backend=dask-local-threaded order=%s subset_bytes=%s",
            request.moment_order, subset_bytes,
        )
        async with _heavy_semaphore():
            result = await _run_in_pool(
                _moment_map_dask,
                dataset.path,
                request.moment_order,
                request.channel_start,
                request.channel_end,
                request.threshold_value,
                request.mask_enabled,
                64,       # chunk_z
                None,     # dist_client → local threaded scheduler
                request.threshold_auto,
                bool(getattr(request, "moment2_variance", False)),
            )
    else:
        # Heavy task — throttled so other interactive requests stay responsive.
        result = await _run_heavy(
            worker_moment,
            dataset.path,
            request.moment_order,
            request.channel_start,
            request.channel_end,
            request.mask_enabled,
            request.threshold_value,
            request.threshold_auto,
            bool(getattr(request, "moment2_variance", False)),
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

    # Heavy task — throttled so cube interaction (slice scroll, ROI) stays
    # responsive while a PV diagram is being computed.
    result = await _run_heavy(
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


def assert_safe_fetch_url(
    raw_url: str,
    *,
    allowlist_env: str,
    block_private: bool = True,
) -> None:
    """Validate a client-supplied URL before the server fetches it (anti-SSRF).

    Always requires an http/https scheme and a host. Then, in order:
      * If ``$allowlist_env`` is set (comma-separated hosts), only those hosts
        are allowed; everything else is rejected.
      * Else if ``block_private`` is True, any address that resolves to a
        private / loopback / link-local / reserved / multicast / unspecified
        target is rejected (the SSRF guard).
      * Else (no allowlist, ``block_private`` False) the URL is accepted — used
        where internal/private fetch targets are legitimate (e.g. the SKAVA
        data-mesh, where federated nodes may sit on private networks), so
        hardening stays opt-in and can't break those deployments.

    Raises ``ValueError`` with a client-safe message when the URL is unsafe.
    """
    import ipaddress  # noqa: PLC0415
    import socket  # noqa: PLC0415
    from urllib.parse import urlparse  # noqa: PLC0415

    parsed = urlparse((raw_url or "").strip())
    if parsed.scheme not in ("http", "https"):
        raise ValueError("Only http:// or https:// URLs are allowed.")
    host = parsed.hostname
    if not host:
        raise ValueError("URL has no host component.")

    allow_raw = os.environ.get(allowlist_env, "").strip()
    if allow_raw:
        allowed = {h.strip().lower() for h in allow_raw.split(",") if h.strip()}
        if host.lower() not in allowed:
            raise ValueError(f"Host '{host}' is not in {allowlist_env}.")
        return
    if not block_private:
        return

    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    try:
        infos = socket.getaddrinfo(host, port, proto=socket.IPPROTO_TCP)
    except socket.gaierror as exc:
        raise ValueError(f"Cannot resolve host '{host}': {exc}") from exc
    for info in infos:
        ip = ipaddress.ip_address(info[4][0])
        if (ip.is_private or ip.is_loopback or ip.is_link_local
                or ip.is_reserved or ip.is_multicast or ip.is_unspecified):
            raise ValueError(
                f"Refusing to fetch from non-public address {ip} (SSRF guard). "
                f"Set {allowlist_env} to trust a specific host."
            )


def record_history(
    session: Session,
    *,
    operation: str = "",
    dataset_id: str = "",
    params: dict[str, Any] | None = None,
    result_dataset_id: str = "",
    endpoint: str = "",
    timestamp: str = "",
) -> None:
    """Append one provenance entry to the session history (append-only).

    Compute endpoints call this to auto-record what they did, so the PROV-JSON
    graph and the snapshot list reflect real work. A missing timestamp is
    filled with the current UTC ISO-8601 time. Never raises — provenance must
    never break the actual compute path.
    """
    try:
        session.history.append({
            "timestamp": timestamp or datetime.now(timezone.utc).isoformat(),
            "operation": operation,
            "endpoint": endpoint,
            "dataset_id": dataset_id,
            "result_dataset_id": result_dataset_id,
            "params": params or {},
        })
    except Exception:  # noqa: BLE001 - provenance is best-effort
        logger.debug("[history] failed to record entry op=%s", operation, exc_info=True)


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
    """Return (kind, geometry) for a FITS or HDF5 dataset.

    For HDF5 inputs (LOFAR BFData or any file with a 2-D float dataset) we
    transparently convert the first dynamic-spectrum plane to a sidecar
    FITS next to the source and reflect ``path`` to point at the sidecar
    so the rest of the pipeline (preview / full image readers, region
    stats, …) reuses the standard FITS code path.
    """
    from .hdf5_dynspec import (  # noqa: PLC0415 (lazy h5py import)
        convert_hdf5_to_sidecar_fits, is_hdf5_path,
    )
    from .psrfits import (  # noqa: PLC0415
        convert_psrfits_to_sidecar_fits, is_psrfits,
    )
    if is_hdf5_path(path):
        sidecar = convert_hdf5_to_sidecar_fits(path)
        dataset = ScientificFitsDataset(sidecar)
        geometry = dataset.geometry_metadata()
        # Force kind to 'dynspec' even if the sidecar header somehow looked
        # like a normal 2-D image — the source file is beamformed by
        # construction.
        geometry["kind"] = "dynspec"
        # Reflect the sidecar path so downstream readers see a real FITS.
        geometry["resolved_path"] = str(sidecar)
        return "dynspec", geometry
    # PSRFITS: multi-HDU FITS with a SUBINT binary-table extension. We
    # convert to a 2-D dynspec sidecar (Stokes I, phase-averaged for folded
    # data) so the rest of the pipeline treats it like an HDF5 / FITS dynspec.
    if is_psrfits(path):
        sidecar = convert_psrfits_to_sidecar_fits(path)
        dataset = ScientificFitsDataset(sidecar)
        geometry = dataset.geometry_metadata()
        geometry["kind"] = "dynspec"
        geometry["resolved_path"] = str(sidecar)
        return "dynspec", geometry
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


# ── The CDS HiPS registry ────────────────────────────────────────────────────
#
# Typing a survey URL by hand was the only way into the viewer, which meant you
# had to know one. The MOCServer publishes every registered HiPS, so the client
# can offer the list instead.

_HIPS_REGISTRY_URL = (
    "https://alasky.cds.unistra.fr/MocServer/query"
    "?hips_service_url=*&get=record&fmt=json"
    "&fields=ID,obs_title,hips_service_url,client_category,obs_regime,"
    "hips_order,hips_frame,hips_tile_format,dataproduct_type"
)
# The registry changes rarely and the list is a third of a megabyte, so it is
# kept on disk and only refetched when stale or when the client asks.
_HIPS_REGISTRY_TTL_SECONDS = 7 * 24 * 3600


def _hips_registry_cache_path() -> _Path:
    # Named after the query, so changing the fields we ask for invalidates the
    # cache instead of silently serving a document without them.
    digest = hashlib.sha256(_HIPS_REGISTRY_URL.encode("utf-8")).hexdigest()[:12]
    return _HIPS_CACHE_DIR / f"registry_{digest}.json"


def _parse_hips_registry(payload: bytes) -> list[dict]:
    """Records the viewer can use, out of the MOCServer's JSON.

    The response is not valid UTF-8 — titles carry a latin-1 micro sign — so it
    is decoded leniently rather than refused.
    """
    import json as _json  # noqa: PLC0415

    try:
        records = _json.loads(payload.decode("utf-8", errors="replace"))
    except Exception as exc:  # noqa: BLE001 — a remote document, not our code
        raise ValueError(f"The HiPS registry did not parse: {exc}") from exc
    if not isinstance(records, list):
        raise ValueError("The HiPS registry was not a list of records.")

    surveys: list[dict] = []
    for record in records:
        if not isinstance(record, dict):
            continue
        url = str(record.get("hips_service_url") or "").strip()
        if not url:
            continue
        try:
            max_order = int(str(record.get("hips_order", "")).strip() or 0)
        except ValueError:
            max_order = 0
        # Most of the registry is imagery, but it also lists HiPS catalogues
        # and cubes, which this viewer cannot draw as tiles. Say which is which
        # rather than offering all 1500 and letting a third of them fail.
        kind = str(record.get("dataproduct_type") or "").strip().lower() or "unknown"
        surveys.append({
            "id": str(record.get("ID") or "").strip(),
            "title": str(record.get("obs_title") or record.get("ID") or "").strip(),
            "url": url,
            "category": str(record.get("client_category") or "").strip(),
            "regime": str(record.get("obs_regime") or "").strip(),
            "frame": str(record.get("hips_frame") or "").strip(),
            "tile_formats": str(record.get("hips_tile_format") or "").split(),
            "kind": kind,
            "max_order": max_order,
        })
    surveys.sort(key=lambda s: (s["category"].lower(), s["title"].lower()))
    return surveys


def _load_hips_registry_sync(refresh: bool = False) -> list[dict]:
    """The registry, from disk when it is fresh enough and from CDS otherwise."""
    cache = _hips_registry_cache_path()
    if not refresh:
        try:
            # Everything about reading the cache is best-effort: an unreadable
            # file, one that vanishes between the stat and the read, or one
            # that no longer parses is a cache miss, not a failed request.
            age = time.time() - cache.stat().st_mtime
            if 0.0 <= age < _HIPS_REGISTRY_TTL_SECONDS:
                return _parse_hips_registry(cache.read_bytes())
        except (OSError, ValueError):
            pass

    payload = _hips_fetch_sync(_HIPS_REGISTRY_URL)
    surveys = _parse_hips_registry(payload)
    temporary: _Path | None = None
    try:
        # Written through a temporary file in the same directory and renamed
        # into place, so a write that stops halfway cannot leave a truncated
        # document where the last good copy was, and two of them at once cannot
        # interleave. This is about partial writes, not power loss: a
        # disposable cache does not warrant fsync.
        cache.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=cache.parent, delete=False) as handle:
            temporary = _Path(handle.name)
            handle.write(payload)
        os.replace(temporary, cache)
        temporary = None
    except OSError:
        pass                  # an unwritable cache only costs us the next fetch
    finally:
        if temporary is not None:
            # The rename never happened; do not leave the fragment behind.
            try:
                temporary.unlink()
            except OSError:
                pass
    return surveys


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


# The sky frames a HiPS can be tiled in. Anything else in the registry is a
# planetary body, not a sky, and has no place in a celestial viewer.
_HIPS_SKY_FRAMES = ("equatorial", "galactic", "ecliptic")

_ASTROPY_FRAMES = {
    "galactic": "galactic",
    "ecliptic": "barycentricmeanecliptic",
}


def _hips_frame_to_icrs(frame: str, lon_deg, lat_deg):
    """Longitudes/latitudes in a survey's own frame → ICRS RA/Dec."""
    target = _ASTROPY_FRAMES.get((frame or "").lower())
    if target is None:
        return np.asarray(lon_deg, dtype=float), np.asarray(lat_deg, dtype=float)
    from astropy.coordinates import SkyCoord  # noqa: PLC0415
    import astropy.units as u  # noqa: PLC0415

    coords = SkyCoord(np.asarray(lon_deg, dtype=float) * u.deg,
                      np.asarray(lat_deg, dtype=float) * u.deg,
                      frame=target).icrs
    return coords.ra.deg, coords.dec.deg


def _icrs_to_hips_frame(frame: str, ra_deg: float, dec_deg: float) -> tuple[float, float]:
    """ICRS RA/Dec → the longitude/latitude a survey's own tiles are indexed by."""
    target = _ASTROPY_FRAMES.get((frame or "").lower())
    if target is None:
        return float(ra_deg), float(dec_deg)
    from astropy.coordinates import SkyCoord  # noqa: PLC0415
    import astropy.units as u  # noqa: PLC0415

    coords = SkyCoord(float(ra_deg) * u.deg, float(dec_deg) * u.deg,
                      frame="icrs").transform_to(target)
    return float(coords.spherical.lon.deg), float(coords.spherical.lat.deg)


def _get_tiles_for_fov(
    ra_deg: float, dec_deg: float, fov_deg: float, order: int, frame: str = "equatorial",
    radius_deg: float = 0.0,
) -> list[dict]:
    """HEALPix NESTED tiles covering the given FOV, in ICRS.  Requires healpy.

    A HiPS is tiled in its own frame, and rather more than half the registry is
    galactic. The viewport asks in ICRS and draws in ICRS, so the centre is
    converted into the survey's frame to choose the tiles, and every position
    that comes back is converted out of it again — without which a galactic
    survey landed its tiles wherever those numbers happened to fall on an
    equatorial sky, tens of degrees from the right place.
    """
    nside = _hp.order2nside(order)
    lon_deg, lat_deg = _icrs_to_hips_frame(frame, ra_deg, dec_deg)
    # Centre unit vector, in the survey's own frame
    theta = _math.radians(90.0 - lat_deg)
    phi   = _math.radians(lon_deg)
    vec   = _hp.ang2vec(theta, phi)
    # The radius the client measured for its own viewport when it sent one:
    # only it knows the aspect ratio and the projection, and a 10 degree field
    # in a 600x1200 window reaches 11 degrees at the corner while half the
    # field of view with a sqrt(2) margin gives 7.1. The old estimate remains
    # the fallback for a caller that says nothing.
    radius_rad = _math.radians(radius_deg if radius_deg and radius_deg > 0
                               else fov_deg / 2.0 * 1.42)
    if radius_rad >= _math.pi:
        # A disc that large is the whole sphere, and query_disc will not take a
        # radius above pi. The all-sky projections ask for exactly this.
        pixels = np.arange(12 * nside * nside, dtype=np.int64)
    else:
        pixels = _hp.query_disc(nside, vec, radius_rad, nest=True, inclusive=True)

    pixels = [int(p) for p in pixels]
    if not pixels:
        return []

    # Gather every position first, so a frame conversion is one vectorised call
    # rather than five per tile.
    lons: list[float] = []
    lats: list[float] = []
    for p in pixels:
        theta_c, phi_c = _hp.pix2ang(nside, p, nest=True)
        lons.append(_math.degrees(phi_c))
        lats.append(90.0 - _math.degrees(theta_c))

        corners = _hp.boundaries(nside, p, step=1, nest=True)  # shape (3,4)
        for i in range(4):
            lons.append(_math.degrees(_math.atan2(corners[1, i], corners[0, i])))
            lats.append(90.0 - _math.degrees(_math.acos(
                max(-1.0, min(1.0, corners[2, i])))))

    ra_all, dec_all = _hips_frame_to_icrs(frame, lons, lats)

    result = []
    for index, p in enumerate(pixels):
        base = index * 5
        result.append({
            "pix": p,
            "order": order,
            "ra_center": float(ra_all[base]),
            "dec_center": float(dec_all[base]),
            "corners_ra": [float(ra_all[base + 1 + i]) for i in range(4)],
            "corners_dec": [float(dec_all[base + 1 + i]) for i in range(4)],
        })
    return result


def _normalize_target_name(name: str) -> str:
    """Case- and whitespace-insensitive key so 'M31', 'm31' and '  M31  '
    (and 'NGC 224' vs '  ngc  224 ') share one cache entry."""
    return " ".join((name or "").split()).lower()


@functools.lru_cache(maxsize=128)
def _resolve_target_cached(norm_name: str) -> dict:
    """Actual Sesame resolution, LRU-cached by normalized name. Failures are
    cached too (so a bad/timed-out lookup is not retried on every call). The
    same dict object is returned on a cache hit."""
    try:
        from astropy.coordinates import SkyCoord  # type: ignore
        coord = SkyCoord.from_name(norm_name)
        return {
            "valid": True, "error": "",
            "ra_deg": float(coord.ra.deg), "dec_deg": float(coord.dec.deg),
            "resolved_name": norm_name, "source": "sesame",
        }
    except Exception as exc:
        return {
            "valid": False, "error": str(exc),
            "ra_deg": 0.0, "dec_deg": 0.0, "resolved_name": "", "source": "",
        }


def _resolve_target_sync(name: str) -> dict:
    return _resolve_target_cached(_normalize_target_name(name))


def clear_resolver_cache() -> None:
    """Invalidate all cached name→coordinate resolutions."""
    _resolve_target_cached.cache_clear()


@functools.lru_cache(maxsize=32)
def _load_overlay_csv(path: str) -> pd.DataFrame:
    """Read and parse a catalogue CSV, caching up to 32 files (LRU eviction)."""
    return pd.read_csv(path)


def _catalogue_overlay_sync(
    path: str, ra_center: float, dec_center: float, fov_deg: float,
    ra_field: str, dec_field: str, max_sources: int, radius_deg: float = 0.0,
) -> dict:
    try:
        # Client-supplied path: confine it like every other filesystem read
        # (data_roots), or a shared backend would serve any readable CSV.
        df = _load_overlay_csv(str(_normalize_path(path)))
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

    # Angular separation, not a bounding box in right ascension. The box was
    # described as conservative and is not: it widens RA by the cosine of the
    # *centre's* declination, so a view centred at +85 degrees with a 20 degree
    # field threw away a source 6 degrees from the centre because its right
    # ascension was on the other side of the pole. Near a pole every RA is in
    # view, and no interval in RA says so.
    # The radius the client measured, when it sent one: only it knows the
    # viewport's aspect ratio and projection, and a field of view across the
    # width says nothing about how far the corners reach. 0.6 x fov is the
    # fallback for a caller that does not say.
    radius = float(radius_deg) if radius_deg and radius_deg > 0 else fov_deg * 0.6
    radius = min(180.0, max(0.0, radius))

    lam  = np.radians(ra_col.to_numpy(dtype=float))
    phi  = np.radians(dec_col.to_numpy(dtype=float))
    lam0 = _math.radians(ra_center)
    phi0 = _math.radians(dec_center)
    with np.errstate(invalid="ignore"):
        cos_sep = (_math.sin(phi0) * np.sin(phi)
                   + _math.cos(phi0) * np.cos(phi) * np.cos(lam - lam0))
        # Rounding can put the cosine a hair outside [-1, 1], which at a radius
        # of 180 degrees dropped the antipode of the view centre from a
        # whole-sky selection.
        cos_sep = np.clip(cos_sep, -1.0, 1.0)
        if radius >= 180.0:
            keep = np.isfinite(cos_sep)
        else:
            keep = cos_sep >= _math.cos(_math.radians(radius))
        # A row with an unreadable position gives NaN, which compares false and
        # is dropped, exactly as it was before.
        mask = pd.Series(keep, index=df.index)

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
