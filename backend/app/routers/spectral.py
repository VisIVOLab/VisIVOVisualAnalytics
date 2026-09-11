"""Spectral-line analysis endpoints (S-02, S-03, S-04).

Recovered from the pre-refactor monolithic ``main.py``: the router-splitting
refactor (commit 867203ee, "Refactor backend: routers, deps & GPU compute")
extracted every other endpoint group into its own router but left the
``/v1/spectral/*`` block out of the migration. The C++ client still wires the
LinewidthDialog / BaselineDialog / StackDialog actions to these URLs, and the
``worker_*`` functions in ``compute.py`` are unchanged, so re-attaching the
router is enough to make the existing UI work again.

Endpoints:

    POST /v1/spectral/linewidth               JSON FWHM + EW maps (base64)
    POST /v1/spectral/linewidth/binary        binary FWHM + EW frames
    GET  /v1/spectral/linewidth/{dataset_id}  binary single frame (fwhm|ew)
    POST /v1/spectral/baseline/{session_id}/{dataset_id}
                                              polynomial / median baseline
                                              subtraction, registers new dataset
    POST /v1/spectral/stack                   JSON stacked spectrum (base64)
    POST /v1/spectral/stack/binary            binary stacked spectrum frame
"""

from __future__ import annotations

import asyncio
import base64
import logging
import os
import struct
import tempfile
import uuid
from pathlib import Path
from typing import Any

import numpy as np
from fastapi import APIRouter, Depends, HTTPException, Query, Response
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import (_run_heavy, _require_dataset, _run_heavy_with_limit,
                            acquire_task_slot, task_slot_busy_error)
from ..product_cache import PRODUCT_CACHE
from ..sessions import Session, get_session

router = APIRouter(tags=["spectral"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)


# ── Pydantic models ──────────────────────────────────────────────────────────

class LinewidthRequest(BaseModel):
    dataset_id: str
    channel_start: int = 0
    channel_end: int = 0
    mask_enabled: bool = False
    threshold_value: float = 0.0
    method: str | None = None       # None=both (binary); "fwhm" | "ew" selects one
    rest_freq_hz: float | None = None
    # R-07: fast vectorised (moment-dispersion) FWHM instead of a per-pixel
    # Gaussian fit — orders of magnitude faster on dense bright cubes.
    fast: bool = False


class LinewidthResponse(BaseModel):
    valid: bool
    error: str = ""
    width: int = 0
    height: int = 0
    scalar_type: str = ""
    fwhm_unit: str = ""
    ew_unit: str = ""
    bunit: str = ""
    fwhm_range_min: float = 0.0
    fwhm_range_max: float = 0.0
    ew_range_min: float = 0.0
    ew_range_max: float = 0.0
    fwhm_base64: str = ""
    ew_base64: str = ""


class BaselineRequest(BaseModel):
    session_id: str
    dataset_id: str
    channel_start: int = 0
    channel_end: int = 0
    poly_order: int = 1
    method: str = "polynomial"          # "polynomial" | "median"
    # Accept flat list [5,6,7] OR list of ranges [[5,7],[20,21]]
    line_free_channels: list = []


class BaselineResponse(BaseModel):
    valid: bool
    error: str = ""
    new_dataset_id: str = ""
    width: int = 0
    height: int = 0
    depth: int = 0
    range_min: float = 0.0
    range_max: float = 0.0
    poly_order: int = 0
    method: str = ""
    fit_channels: int = 0
    rms_before: float = 0.0
    rms_after: float = 0.0
    output_path: str = ""
    # Non-fatal caveats about a fit that SUCCEEDED (an extrapolated baseline, a
    # residual larger than the input). The client shows these; a field missing
    # HERE is a warning the user never sees, however carefully it is computed.
    warning: str = ""


class StackRequest(BaseModel):
    dataset_ids: list[str]
    method: str = "mean"                # "mean" | "median" | "weighted_mean"
    weight_by: str = "uniform"          # "uniform" | "rms" | "peak"
    weights: list[float] = []           # explicit per-cube weights (optional)


class StackResponse(BaseModel):
    valid: bool
    error: str = ""
    rows: int = 0
    cols: int = 0
    scalar_type: str = ""
    method: str = ""
    n_cubes: int = 0
    range_min: float = 0.0
    range_max: float = 0.0
    data_base64: str = ""


# ── Internal helper ──────────────────────────────────────────────────────────

def _array_to_binary(rows: int, cols: int, arr: np.ndarray) -> bytes:
    """Pack a float32 array into the Visivo binary wire format.

    Header (12 bytes, big-endian): uint32 rows, uint32 cols, uint32 flags=0.
    Body: float32 little-endian, row-major, NaN for masked/invalid pixels.
    Matches the format ``BackendClient::parseBinaryFrame()`` expects.
    """
    header = struct.pack(">III", rows, cols, 0)
    body = np.ascontiguousarray(arr, dtype="<f4").tobytes()
    return header + body


def _linewidth_unit_headers(
    result: dict[str, Any], method: str | None = None
) -> dict[str, str]:
    """HTTP headers carrying the line-width map units.

    The binary line-width endpoints return raw float frames with no in-band
    metadata, so the physical unit (e.g. ``km/s``) is lost unless we surface it
    out-of-band. The JSON variant already returns ``fwhm_unit``/``ew_unit``; here
    the same values ride along as response headers so the client can label the
    map. HTTP header values are latin-1, so units are sanitised to that encoding.

    ``X-Fwhm-Unit`` / ``X-Ew-Unit`` are always set; ``X-Map-Unit`` mirrors the
    single frame the caller asked for (``method`` ``fwhm``/``ew``), so the
    single-map GET/POST paths need read only one header.
    """
    def _safe(unit: str) -> str:
        # HTTP header values are latin-1. Canonicalise the common non-latin-1 unit char (Greek mu
        # / micro sign, e.g. "μm") to ASCII "u" first so a wavelength unit survives readably
        # instead of becoming "?m".
        u = (unit or "").replace("μ", "u").replace("µ", "u")
        return u.encode("latin-1", "replace").decode("latin-1")

    fwhm_u = _safe(str(result.get("fwhm_unit", "")))
    ew_u = _safe(str(result.get("ew_unit", "")))
    headers = {"X-Fwhm-Unit": fwhm_u, "X-Ew-Unit": ew_u}
    # X-Map-Unit is a convenience for the SINGLE-frame variants only — the two-frame default body
    # carries both maps, so an aliased single unit there would be ambiguous.
    m = (method or "").lower()
    if m in ("fwhm", "ew"):
        headers["X-Map-Unit"] = ew_u if m == "ew" else fwhm_u
    return headers


# Default SNR cutoff for skipping background pixels. Tunable via env in case a
# specific dataset needs a different threshold; 3σ is the standard radio-astro
# noise-rejection level and matches the value used in similar VLKB tools.
_LINEWIDTH_SNR_THRESHOLD = float(os.environ.get("VISIVO_LINEWIDTH_SNR", "3.0"))

# Number of row-chunks the linewidth job is split into.
#
# Default = VISIVO_WORKERS (full pool). The dispatch is still safe for
# interactive responsiveness because each chunk goes through ``_run_heavy``,
# which is gated by the global ``_HEAVY_SEM`` semaphore (capped at
# ``VISIVO_HEAVY_SLOTS`` = ``max(1, VISIVO_WORKERS-1)`` by default) — so the
# pool will never be fully saturated by linewidth chunks and slice / probe /
# subvolume requests always have a worker available.
#
# Override with ``VISIVO_LINEWIDTH_CHUNKS=N`` if you want to constrain the
# chunk count independently from the heavy-task semaphore.
# Wall-clock ceiling for a SoFiA-2 run, mirroring VISIVO_HEAVY_TIMEOUT_S in
# spirit: the child process is killed and the caller gets a clear error rather
# than a thread wedged for the lifetime of the server. 0 disables it.
_SOFIA_TIMEOUT_S = float(os.environ.get("VISIVO_SOFIA_TIMEOUT_S", "900")) or None

_LINEWIDTH_PARALLEL_CHUNKS = max(
    1,
    int(os.environ.get(
        "VISIVO_LINEWIDTH_CHUNKS",
        os.environ.get("VISIVO_WORKERS", "4"),
    )),
)


def _combine_linewidth_chunks(chunks: list[dict[str, Any]]) -> dict[str, Any]:
    """Merge per-row-chunk linewidth results into one full-image result.

    Each chunk returned a full-size (ny × nx) float32 map with NaN outside its
    assigned row slice. The merge is a nan-aware "take-first-finite" combine,
    which is equivalent to a vertical concat because chunks don't overlap.
    """
    if not chunks:
        raise RuntimeError("Linewidth orchestrator produced no chunks")
    base = chunks[0]
    nx = int(base["width"])
    ny = int(base["height"])
    fwhm = np.frombuffer(base64.b64decode(base["fwhm_base64"]),
                         dtype=np.float32).reshape(ny, nx).copy()
    ew = np.frombuffer(base64.b64decode(base["ew_base64"]),
                       dtype=np.float32).reshape(ny, nx).copy()
    for c in chunks[1:]:
        f = np.frombuffer(base64.b64decode(c["fwhm_base64"]),
                          dtype=np.float32).reshape(ny, nx)
        e = np.frombuffer(base64.b64decode(c["ew_base64"]),
                          dtype=np.float32).reshape(ny, nx)
        np.copyto(fwhm, f, where=np.isfinite(f))
        np.copyto(ew, e, where=np.isfinite(e))

    finite_fwhm = fwhm[np.isfinite(fwhm)]
    finite_ew = ew[np.isfinite(ew)]
    fwhm_min = float(finite_fwhm.min()) if finite_fwhm.size else 0.0
    fwhm_max = float(finite_fwhm.max()) if finite_fwhm.size else 0.0
    ew_min = float(finite_ew.min()) if finite_ew.size else 0.0
    ew_max = float(finite_ew.max()) if finite_ew.size else 0.0

    body_f = np.ascontiguousarray(fwhm, dtype=np.float32).tobytes()
    body_e = np.ascontiguousarray(ew, dtype=np.float32).tobytes()
    return {
        "width": nx,
        "height": ny,
        "scalar_type": "float32",
        "fwhm_unit": base.get("fwhm_unit", ""),
        "ew_unit": base.get("ew_unit", ""),
        "bunit": base.get("bunit", ""),
        "fwhm_range_min": fwhm_min,
        "fwhm_range_max": fwhm_max,
        "ew_range_min": ew_min,
        "ew_range_max": ew_max,
        "fwhm_base64": base64.b64encode(body_f).decode("ascii"),
        "ew_base64": base64.b64encode(body_e).decode("ascii"),
    }


def _linewidth_cache_key(path: str, channel_start: int, channel_end: int,
                          mask_enabled: bool, threshold_value: float,
                          rest_freq_hz: float | None = None,
                          fast: bool = False) -> tuple[str, str]:
    """Stable cache key for a linewidth computation."""
    return PRODUCT_CACHE.make_key(path, "linewidth", {
        "channel_start": int(channel_start),
        "channel_end": int(channel_end),
        "mask_enabled": bool(mask_enabled),
        "threshold_value": float(threshold_value),
        "snr_threshold": _LINEWIDTH_SNR_THRESHOLD,
        # Part of the key: a different rest frequency yields km/s maps scaled
        # differently (or Hz when absent), so cached frames must not be shared.
        "rest_freq_hz": float(rest_freq_hz) if rest_freq_hz else None,
        # Fast (moment dispersion) vs Gaussian-fit FWHM are different products.
        "fast": bool(fast),
    })


async def _compute_linewidth(
    session: Session,
    path: str,
    channel_start: int,
    channel_end: int,
    mask_enabled: bool,
    threshold_value: float,
    cube_height: int,
    rest_freq_hz: float | None = None,
    fast: bool = False,
) -> dict[str, Any]:
    """Compute (or retrieve cached) linewidth maps for a cube.

    Strategy:
    1. Look up PRODUCT_CACHE; on hit return immediately (sub-millisecond).
    2. Split the spatial Y axis into ``_LINEWIDTH_PARALLEL_CHUNKS`` row
       ranges and dispatch one ``worker_linewidth`` invocation per chunk to
       the existing ``_POOL`` (each call is fully self-contained: opens the
       FITS file, processes its row slice, returns a full-image-shaped map
       with NaN outside its slice).
    3. ``asyncio.gather`` waits for all chunks in parallel; they run on up
       to ``_WORKERS`` cores. Per-session task slots are acquired once for
       the whole job so we still honour the per-session rate limit.
    4. ``_combine_linewidth_chunks`` merges the partial maps into the final
       result, which is then stored in the cache.
    """
    cache_key, param_hash = _linewidth_cache_key(
        path, channel_start, channel_end, mask_enabled, threshold_value, rest_freq_hz, fast
    )
    cached = PRODUCT_CACHE.get(cache_key)
    if cached is not None:
        return dict(cached.payload)

    # The shared bounded wait, not an instant refusal: this endpoint held the
    # only fail-fast path left, so a line-width request was refused within
    # milliseconds while every other compute waited 15 s for a slot — and the
    # client's retry budget (~7.5 s of backoff) could not bridge that either.
    if not await acquire_task_slot(session):
        raise task_slot_busy_error()
    try:
        from ..compute import worker_linewidth  # noqa: PLC0415

        n_chunks = max(1, min(_LINEWIDTH_PARALLEL_CHUNKS, max(1, cube_height)))
        # Even split with remainder distributed to the first chunks.
        base = cube_height // n_chunks
        rem = cube_height % n_chunks
        ranges: list[tuple[int, int]] = []
        cursor = 0
        for i in range(n_chunks):
            length = base + (1 if i < rem else 0)
            if length <= 0:
                continue
            ranges.append((cursor, cursor + length))
            cursor += length

        tasks = [
            # Each chunk is a heavy task; the global heavy-task semaphore
            # ensures at most VISIVO_HEAVY_SLOTS chunks run concurrently,
            # leaving ≥1 pool slot free for interactive requests even if
            # _LINEWIDTH_PARALLEL_CHUNKS == VISIVO_WORKERS.
            _run_heavy(
                worker_linewidth,
                path, int(channel_start), int(channel_end),
                bool(mask_enabled), float(threshold_value),
                _LINEWIDTH_SNR_THRESHOLD, int(y0), int(y1),
                rest_freq_hz, fast,
            )
            for (y0, y1) in ranges
        ]
        logger.info(
            "[linewidth] dispatching %d parallel chunks over %d rows (path=%s)",
            len(tasks), cube_height, path,
        )
        chunks = await asyncio.gather(*tasks)
        result = _combine_linewidth_chunks(chunks)
    finally:
        session.release_task_slot()

    PRODUCT_CACHE.put(
        key=cache_key,
        operation="linewidth",
        dataset_path=path,
        parameter_hash=param_hash,
        payload=result,
        scientific_metadata={
            "fwhm_unit": result.get("fwhm_unit", ""),
            "ew_unit": result.get("ew_unit", ""),
            "bunit": result.get("bunit", ""),
            "snr_threshold": _LINEWIDTH_SNR_THRESHOLD,
        },
        provenance_metadata={
            "parallel_chunks": len(ranges),
        },
    )
    return result


# ── S-02: Per-pixel line-width maps ──────────────────────────────────────────

@router.post("/v1/spectral/linewidth", response_model=LinewidthResponse)
async def spectral_linewidth(
    request: LinewidthRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> LinewidthResponse:
    """Compute per-pixel FWHM (Gaussian fit) and equivalent-width maps.

    Both result arrays are returned as base64-encoded float32 in the JSON
    response. Use ``/v1/spectral/linewidth/binary`` for the raw binary format.

    Performance: results are cached (LRU, ``PRODUCT_CACHE``) per dataset +
    channel range + mask params, and the per-pixel Gauss fit is dispatched
    in parallel row-chunks (one task per worker pool slot).
    """
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "This endpoint requires a spectral cube dataset.")
        result = await _compute_linewidth(
            session, entry["path"],
            request.channel_start, request.channel_end,
            request.mask_enabled, request.threshold_value,
            cube_height=int(entry.get("height") or 0),
            rest_freq_hz=request.rest_freq_hz,
            fast=request.fast,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return LinewidthResponse(valid=False, error=str(exc))
    return LinewidthResponse(valid=True, **result)


@router.post("/v1/spectral/linewidth/binary")
async def spectral_linewidth_binary(
    request: LinewidthRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Return FWHM and EW maps as concatenated binary payloads.

    The response body contains two back-to-back Visivo binary frames:
      Frame 1 — FWHM map  (height × width float32)
      Frame 2 — EW map    (height × width float32)

    When ``request.method`` is ``"fwhm"`` or ``"ew"`` only that frame is
    returned (single-map binary path used by the spec-conformant GET variant).

    Shares cache + parallel orchestrator with the JSON variant.
    """
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "This endpoint requires a spectral cube dataset.")
        result = await _compute_linewidth(
            session, entry["path"],
            request.channel_start, request.channel_end,
            request.mask_enabled, request.threshold_value,
            cube_height=int(entry.get("height") or 0),
            rest_freq_hz=request.rest_freq_hz,
            fast=request.fast,
        )
    except HTTPException:
        raise
    except Exception as exc:
        raise HTTPException(500, str(exc)) from exc

    h, w = result["height"], result["width"]
    fwhm = np.frombuffer(base64.b64decode(result["fwhm_base64"]), dtype=np.float32).reshape(h, w)
    ew   = np.frombuffer(base64.b64decode(result["ew_base64"]),   dtype=np.float32).reshape(h, w)
    method = (request.method or "").lower()
    if method == "ew":
        content = _array_to_binary(h, w, ew)
    elif method == "fwhm":
        content = _array_to_binary(h, w, fwhm)
    else:
        content = _array_to_binary(h, w, fwhm) + _array_to_binary(h, w, ew)
    return Response(
        content=content,
        media_type="application/octet-stream",
        headers=_linewidth_unit_headers(result, method),
    )


@router.get("/v1/spectral/linewidth/{dataset_id}")
async def spectral_linewidth_get(
    dataset_id: str,
    _: None = _auth,
    session: Session = Depends(get_session),
    method: str = Query(default="fwhm", description='"fwhm" or "ew"'),
    rest_freq_hz: float | None = Query(default=None),
    fast: bool = Query(default=False),
    channel_start: int = Query(default=0),
    channel_end: int = Query(default=0),
) -> Response:
    """Spec-conformant single-map GET (feature S-02).

    Returns one binary frame (rows×cols float32) for the requested ``method``.
    Shares cache + parallel orchestrator with the POST variants.
    """
    entry = _require_dataset(session, dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "This endpoint requires a spectral cube dataset.")
    try:
        result = await _compute_linewidth(
            session, entry["path"],
            channel_start, channel_end, False, 0.0,
            cube_height=int(entry.get("height") or 0),
            rest_freq_hz=rest_freq_hz,
            fast=fast,
        )
    except HTTPException:
        raise
    except Exception as exc:
        raise HTTPException(500, str(exc)) from exc

    h, w = result["height"], result["width"]
    if method.lower() == "ew":
        arr = np.frombuffer(base64.b64decode(result["ew_base64"]),
                            dtype=np.float32).reshape(h, w)
    else:
        arr = np.frombuffer(base64.b64decode(result["fwhm_base64"]),
                            dtype=np.float32).reshape(h, w)
    return Response(content=_array_to_binary(h, w, arr),
                    media_type="application/octet-stream",
                    headers=_linewidth_unit_headers(result, method))


# ── S-03: Polynomial / median baseline subtraction ──────────────────────────

@router.post(
    "/v1/spectral/baseline/{session_id}/{dataset_id}",
    response_model=BaselineResponse,
)
async def spectral_baseline(
    session_id: str,
    dataset_id: str,
    request: BaselineRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> BaselineResponse:
    """Fit and subtract a per-pixel polynomial baseline from a spectral cube.

    Registers the baseline-subtracted cube as a new dataset in the session
    and returns its ``new_dataset_id`` so the client can open it without an
    additional round-trip.
    """
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "cube":
            raise HTTPException(422, "This endpoint requires a spectral cube dataset.")

        tmp_dir = Path(tempfile.gettempdir()) / "visivo_baseline"
        tmp_dir.mkdir(parents=True, exist_ok=True)
        output_path = str(tmp_dir / f"{uuid.uuid4().hex}_baseline.fits")

        from ..compute import worker_baseline  # noqa: PLC0415
        result = await _run_heavy_with_limit(
            session,
            worker_baseline,
            entry["path"],
            output_path,
            request.channel_start,
            request.channel_end,
            request.poly_order,
            request.line_free_channels or None,
            request.method,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return BaselineResponse(valid=False, error=str(exc))

    new_id = f"ds_{uuid.uuid4().hex[:12]}"
    logger.info(
        "[BASELINE] registering new dataset  new_id=%s  path=%s  depth=%s  width=%s  height=%s",
        new_id, result["output_path"], result["depth"], result["width"], result["height"],
    )
    session.datasets[new_id] = {
        "path": result["output_path"],
        "kind": "cube",
        "width": result["width"],
        "height": result["height"],
        "depth": result["depth"],
    }
    return BaselineResponse(valid=True, new_dataset_id=new_id, **{
        k: result[k] for k in ("width", "height", "depth", "range_min", "range_max",
                                "poly_order", "method", "fit_channels",
                                "rms_before", "rms_after", "output_path", "warning")
    })


# ── S-04: Spectral stacking ──────────────────────────────────────────────────

@router.post("/v1/spectral/stack", response_model=StackResponse)
async def spectral_stack(
    request: StackRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> StackResponse:
    """Stack N spectral cubes into a single mean spectrum.

    ``dataset_ids`` must reference cube datasets already opened in this session.
    ``method``: ``"mean"`` (default) | ``"median"`` | ``"weighted_mean"``.
    ``weights`` (one per cube) are used only for ``"weighted_mean"``.
    """
    try:
        paths: list[str] = []
        for did in request.dataset_ids:
            e = _require_dataset(session, did)
            if e["kind"] != "cube":
                raise HTTPException(422, f"Dataset '{did}' is not a spectral cube.")
            paths.append(e["path"])
        from ..compute import worker_stack  # noqa: PLC0415
        result = await _run_heavy_with_limit(
            session,
            worker_stack,
            paths,
            request.method,
            request.weights or None,
            request.weight_by,
        )
    except HTTPException:
        raise
    except Exception as exc:
        return StackResponse(valid=False, error=str(exc))
    return StackResponse(valid=True, **result)


@router.post("/v1/spectral/stack/binary")
async def spectral_stack_binary(
    request: StackRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Return the stacked spectrum as a binary frame (rows=1, cols=n_chan).

    Wire format: 12-byte big-endian header + float32 LE body.
    """
    try:
        paths: list[str] = []
        for did in request.dataset_ids:
            e = _require_dataset(session, did)
            if e["kind"] != "cube":
                raise HTTPException(422, f"Dataset '{did}' is not a spectral cube.")
            paths.append(e["path"])
        from ..compute import worker_stack  # noqa: PLC0415
        result = await _run_heavy_with_limit(
            session,
            worker_stack,
            paths,
            request.method,
            request.weights or None,
            request.weight_by,
        )
    except HTTPException:
        raise
    except Exception as exc:
        raise HTTPException(500, str(exc)) from exc

    rows, cols = result["rows"], result["cols"]
    arr = np.frombuffer(base64.b64decode(result["data_base64"]),
                        dtype=np.float32).reshape(rows, cols)
    return Response(
        content=_array_to_binary(rows, cols, arr),
        media_type="application/octet-stream",
    )


# ── S-05: SoFiA-2 source finding ───────────────────────────────────────────────

class SourceFindRequest(BaseModel):
    dataset_id: str = ""
    sofia_params: dict = {}


@router.post("/v1/sourcefind/{dataset_id}", tags=["spectral"])
async def sourcefind(
    dataset_id: str,
    request: SourceFindRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Run SoFiA-2 on a node-local cube. Returns the detection catalogue and the
    path to the binary mask cube (for the desktop's 3D isosurface overlay)."""
    import shutil as _shutil  # noqa: PLC0415
    import tempfile  # noqa: PLC0415

    entry = _require_dataset(session, dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "Source finding requires a spectral cube dataset.")
    from ..compute import resolve_sofia_bin, worker_sofia  # noqa: PLC0415
    if not resolve_sofia_bin():
        raise HTTPException(
            503,
            "SoFiA-2 not found on the server PATH (looked for 'sofia2' and "
            "'sofia'; set VISIVO_SOFIA_BIN to override).")

    out_dir = tempfile.mkdtemp(prefix="sofia_")
    try:
        # A ceiling, which this endpoint had none of: worker_sofia already
        # supports one (and carries the message for it), but the call left it at
        # None, so an abandoned or pathological SoFiA-2 run held a thread for
        # ever while the client gave up after its own timeout (audit).
        return await asyncio.to_thread(
            worker_sofia, entry["path"], out_dir, request.sofia_params,
            _SOFIA_TIMEOUT_S)
    except HTTPException:
        raise
    except Exception as exc:
        logger.warning("[sourcefind] failed dataset_id=%s error=%s", dataset_id, exc)
        return {"valid": False, "error": str(exc), "n_sources": 0, "sources": []}


# ── Spectral probe / aperture (interactive profiler on remote/large cubes) ─────

class SpectralProbeRequest(BaseModel):
    dataset_id: str
    x: float
    y: float


class SpectralApertureRequest(BaseModel):
    dataset_id: str
    x: float
    y: float
    radius: float = 3.0


@router.post("/v1/spectral/probe", tags=["spectral"])
async def spectral_probe(
    request: SpectralProbeRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Extract the spectrum at a single pixel — computed on the node so the
    profiler scales to multi-GB remote cubes."""
    entry = _require_dataset(session, request.dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "Spectral probe requires a spectral cube dataset.")
    from ..compute import worker_spectral_probe  # noqa: PLC0415
    try:
        return await asyncio.to_thread(
            worker_spectral_probe, entry["path"], request.x, request.y)
    except Exception as exc:
        raise HTTPException(400, str(exc)) from exc


@router.post("/v1/spectral/aperture", tags=["spectral"])
async def spectral_aperture(
    request: SpectralApertureRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Mean spectrum over a circular aperture (radius in pixels), node-side."""
    entry = _require_dataset(session, request.dataset_id)
    if entry["kind"] != "cube":
        raise HTTPException(422, "Spectral aperture requires a spectral cube dataset.")
    from ..compute import worker_spectral_aperture  # noqa: PLC0415
    try:
        return await asyncio.to_thread(
            worker_spectral_aperture, entry["path"], request.x, request.y, request.radius)
    except Exception as exc:
        raise HTTPException(400, str(exc)) from exc


class LineFitRequest(BaseModel):
    values: list[float]
    axis: list[float]
    with_baseline: bool = True


@router.post("/v1/spectral/linefit", tags=["spectral"])
async def spectral_linefit(request: LineFitRequest, _: None = _auth):
    """Fit a Gaussian (+ optional linear baseline) to an extracted 1-D spectrum.

    Stateless: the caller supplies the spectrum ``values`` and ``axis`` (e.g.
    from the spectral profiler), so the same fit serves cube probes, aperture
    spectra, or any externally-extracted profile. Returns peak/center/σ/FWHM in
    the axis units, integrated flux, baseline, continuum, equivalent width and
    residual RMS.
    """
    if len(request.values) != len(request.axis):
        raise HTTPException(422, "values and axis must have the same length.")
    from ..compute import worker_line_fit  # noqa: PLC0415
    try:
        result = await asyncio.to_thread(
            worker_line_fit, request.values, request.axis, request.with_baseline)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", **result}
