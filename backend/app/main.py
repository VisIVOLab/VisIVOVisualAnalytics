"""
main.py – VisIVO Backend v1 (Refactored)

The backend has been modularized into routers and models for better maintainability.
Each domain (files, datasets, catalogue, vbt, products, cube, tasks, image,
cosmology, hips, resolve, meta) lives in its own router module under app/routers/.
"""

from __future__ import annotations

import contextlib
import json
import logging
import time
import uuid
from pathlib import Path
from fastapi import FastAPI, Depends, Request, Response
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

from .auth import verify_token
from .tasks import TASKS
from .routers import (
    meta,
    files,
    datasets,
    catalogue,
    vbt,
    products,
    cube,
    tasks,
    image,
    cosmology,
    hips,
    resolve,
    samp,
    spectral,
    diagnostics,
    exports,
    dynspec,
    astrometry,
    polarisation,
    binary_products,
    photometry,
    analysis,
    reproducibility,
    vlkb,
    regions,
    velocity,
    assistant,
    sed,
    simulations,
)
from .diagnostics_buffer import install_logging_handler

# ── Logging ───────────────────────────────────────────────────────────────────
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
logger = logging.getLogger("visivo.backend")

# Mirror every emitted log record into the diagnostics ring buffer so the
# desktop client can pull them via /v1/diagnostics/events and surface them
# alongside its own client-side events. Idempotent.
install_logging_handler()

# ── Lifespan (E-09: replaces the deprecated @app.on_event("shutdown")) ────────
# Set to True after the server-side-rendering router mounts, so the EGL contexts
# are released on shutdown only when they were actually created.
_render_mounted = False


@contextlib.asynccontextmanager
async def _lifespan(_app: "FastAPI"):
    # Multi-user mode fails closed on filesystem access. Say so at boot rather
    # than letting the operator discover it as an unexplained 404 per request.
    from .dependencies import data_roots  # noqa: PLC0415 – avoids an import cycle
    if data_roots() is None:
        logging.getLogger("visivo.backend").warning(
            "[startup] VISIVO_TOKENS is set but VISIVO_DATA_ROOTS is not: "
            "path-based endpoints (file browser, dataset open, overlays) are "
            "DISABLED. Declare the directories this backend may share.")
    yield
    if _render_mounted:
        # Release all open EGL contexts cleanly on uvicorn shutdown. Without this
        # the EGL display handle is leaked and the next backend start may fail to
        # acquire it on the same GPU.
        try:
            from .render_manager import get_render_manager  # noqa: PLC0415
            await get_render_manager().stop()
        except Exception as exc:  # noqa: BLE001
            logging.getLogger("visivo.backend").warning(
                "[shutdown] render manager stop failed: %s", exc)


# ── FastAPI app ───────────────────────────────────────────────────────────────
app = FastAPI(
    title="VisIVO Backend",
    version="1.0.0",
    description="Astrophysical data visualization backend for ViaLactea Visual Analytics.",
    docs_url="/v1/docs",
    redoc_url="/v1/redoc",
    openapi_url="/v1/openapi.json",
    lifespan=_lifespan,
)

_repo_root = Path(__file__).resolve().parents[2]
_samp_static_dir = _repo_root / "icons"
if _samp_static_dir.exists():
    app.mount("/static", StaticFiles(directory=str(_samp_static_dir)), name="static")

# ── Middleware: X-Request-ID (R8) ─────────────────────────────────────────────
@app.middleware("http")
async def attach_request_id(request: Request, call_next):
    request_id = str(uuid.uuid4())
    # Stash on request.state so the global exception handler can echo the same
    # id it logs the traceback under (E-05).
    request.state.request_id = request_id
    response: Response = await call_next(request)
    response.headers["X-Request-ID"] = request_id
    return response


# ── Middleware: what the backend actually did ─────────────────────────────────
# The Data Hub shows "Last 5 jobs", and it was always empty: only
# ``/v1/tasks/moment`` and ``/v1/tasks/pv`` create a TaskRecord, and every other
# computation the backend performs is synchronous and left no trace. Rather than
# teach thirty routers to register a task, the history is taken where every
# request already passes — here.
#
# What counts as a job is "work the user asked for and waited on", so the
# exclusions below are the requests that are not that: browsing, session
# bookkeeping, interactive scrubbing (a channel slice per mouse move), polling,
# and the binary re-fetch of a product that was just computed and recorded.

_HISTORY_SKIP_PREFIXES = (
    "/v1/files/",          # browsing, classifying, header peeks
    "/v1/sessions",        # session lifecycle
    "/v1/tasks/",          # already recorded by the task registry itself
    "/v1/render/",         # WebRTC signalling and per-frame interaction
    "/v1/image/",          # tiles and previews: one per pan / zoom step
    "/v1/samp/",           # interoperability messaging
    "/v1/diagnostics",
    "/v1/assistant/config",
    "/v1/assistant/stream",  # a chat stream, not a job (and /query is recorded)
)

_HISTORY_SKIP_PATHS = frozenset({
    "/v1/cube/slice",          # channel scrubbing
    "/v1/cube/preview",
    "/v1/spectral/probe",      # spectrum under the cursor
    "/v1/vlkb/mcutout/phase",  # job polling, every 2 s
    "/v1/cosmology/distance",
    "/v1/cosmology/distance/batch",
})


def _history_operation_name(request: Request, path: str) -> str | None:
    """The name to file this request under, or None to leave it out."""
    if request.method not in ("POST", "PUT", "PATCH"):
        return None
    if not path.startswith("/v1/"):
        return None
    if path.startswith(_HISTORY_SKIP_PREFIXES) or path in _HISTORY_SKIP_PATHS:
        return None
    if path.endswith("/result/binary"):
        # The product itself was recorded when it was computed; this is only
        # the client collecting the bytes. Other `/binary` routes (spectral
        # linewidth, stack, skeleton, RM synthesis) do the computation, so the
        # exclusion is on `/result/binary` and not on the suffix alone.
        return None
    # Prefer the route template, so twenty moment maps on twenty datasets are
    # twenty "products/moment" rather than twenty distinct-looking operations.
    route = request.scope.get("route")
    template = getattr(route, "path", path) or path
    return template[len("/v1/"):].strip("/") or None


#: How much of a JSON response to keep while it streams past, to read the
#: ``valid``/``error`` envelope out of it. The field is first in every one.
_ENVELOPE_PEEK_BYTES = 4096


def _envelope_failed(body: bytes) -> bool:
    """Whether a captured response body says ``valid: false``.

    Most routes here report failure as HTTP 200 with ``{"valid": false, …}``,
    so the status code alone would file every failed open as a finished job.
    """
    if not body.lstrip().startswith(b"{"):
        return False
    try:
        return json.loads(body).get("valid") is False
    except (ValueError, AttributeError):
        pass
    # Truncated at the peek limit. `valid` is the first field of every one of
    # these envelopes, so only the head is examined: searching the whole prefix
    # would read a nested {"valid": false} — one failed row of a successful
    # batch — as the response itself having failed.
    head = body.lstrip()[1:40].replace(b'"valid": false', b'"valid":false')
    return head.lstrip().startswith(b'"valid":false')


@app.middleware("http")
async def record_operation_history(request: Request, call_next):
    started = time.perf_counter()
    try:
        response: Response = await call_next(request)
    except Exception:
        operation = _history_operation_name(request, request.url.path)
        if operation:
            TASKS.record_operation(operation, status="failed",
                                   duration_seconds=time.perf_counter() - started)
        raise

    operation = _history_operation_name(request, request.url.path)
    if not operation:
        return response

    # Record when the response has actually been produced, not when its headers
    # were sent: a streamed response is still being computed at this point, and
    # its duration would be the startup alone.
    #
    # Known limit: if the client disconnects mid-response, or the body generator
    # itself raises, Starlette has already ended our view of the iterator, so
    # the operation is filed as finished. The history is a record of what the
    # backend was asked to do, not an audit log.
    is_json = response.headers.get("content-type", "").startswith("application/json")
    upstream = response.body_iterator

    async def _watched():
        captured = bytearray()
        try:
            async for chunk in upstream:
                if is_json and len(captured) < _ENVELOPE_PEEK_BYTES:
                    captured.extend(chunk[: _ENVELOPE_PEEK_BYTES - len(captured)])
                yield chunk
        except Exception:
            TASKS.record_operation(operation, status="failed",
                                   duration_seconds=time.perf_counter() - started)
            raise
        failed = response.status_code >= 400 or (is_json and _envelope_failed(bytes(captured)))
        TASKS.record_operation(operation, status="failed" if failed else "done",
                               duration_seconds=time.perf_counter() - started)

    response.body_iterator = _watched()
    return response


# ── Global exception handler (E-05) ───────────────────────────────────────────
# Any exception that escapes a route handler is logged server-side WITH its
# traceback and returned to the client as a compact, non-leaking JSON body
# ``{error, error_code, request_id}``. Without this, FastAPI's default 500 (or a
# handler that echoes str(exc)/a traceback) exposes filesystem paths and internals
# to the client. HTTPException is intentionally NOT caught here — FastAPI's own
# handler renders those (they carry deliberate, safe messages).
# A client-supplied path outside VISIVO_DATA_ROOTS raises PermissionError from
# _normalize_path(). Routes that build a ``valid=False`` envelope catch it
# themselves; this handler covers the rest so a refusal is a uniform 404 rather
# than a 500. 404 (not 403) on purpose: a denied path and an absent one must be
# indistinguishable, or the error code alone would confirm the path exists.
@app.exception_handler(PermissionError)
async def _permission_denied_handler(request: Request, exc: PermissionError) -> JSONResponse:
    logger.warning("[path-denied] method=%s path=%s: %s",
                   request.method, request.url.path, exc)
    return JSONResponse(
        status_code=404,
        content={"error": "Path not found.", "error_code": "path_not_found"},
    )


@app.exception_handler(Exception)
async def _unhandled_exception_handler(request: Request, exc: Exception) -> JSONResponse:
    request_id = getattr(request.state, "request_id", None) or str(uuid.uuid4())
    logger.exception(
        "[unhandled] request_id=%s method=%s path=%s: %s",
        request_id, request.method, request.url.path, exc,
    )
    return JSONResponse(
        status_code=500,
        content={
            "error": "Internal server error.",
            "error_code": "internal_error",
            "request_id": request_id,
        },
        headers={"X-Request-ID": request_id},
    )

# ── Routers ───────────────────────────────────────────────────────────────────
_auth = Depends(verify_token)
app.include_router(meta.router, dependencies=[_auth])
app.include_router(files.router, dependencies=[_auth])
app.include_router(datasets.router, dependencies=[_auth])
app.include_router(catalogue.router, dependencies=[_auth])
app.include_router(vbt.router, dependencies=[_auth])
app.include_router(products.router, dependencies=[_auth])
app.include_router(cube.router, dependencies=[_auth])
app.include_router(tasks.router, dependencies=[_auth])
app.include_router(image.router, dependencies=[_auth])
app.include_router(cosmology.router, dependencies=[_auth])
app.include_router(hips.router, dependencies=[_auth])
app.include_router(resolve.router, dependencies=[_auth])
app.include_router(spectral.router, dependencies=[_auth])
app.include_router(diagnostics.router, dependencies=[_auth])
app.include_router(exports.router, dependencies=[_auth])
app.include_router(dynspec.router, dependencies=[_auth])
app.include_router(astrometry.router, dependencies=[_auth])
app.include_router(polarisation.router, dependencies=[_auth])
app.include_router(binary_products.router, dependencies=[_auth])
app.include_router(photometry.router, dependencies=[_auth])
app.include_router(analysis.router, dependencies=[_auth])
app.include_router(reproducibility.router, dependencies=[_auth])
app.include_router(vlkb.router, dependencies=[_auth])
app.include_router(regions.router, dependencies=[_auth])
app.include_router(velocity.router, dependencies=[_auth])
app.include_router(assistant.router, dependencies=[_auth])
app.include_router(sed.router, dependencies=[_auth])
app.include_router(simulations.router, dependencies=[_auth])
# SAMP: /v1/samp/files/{token} is public (no auth) for VO tool access;
# all other /v1/samp/* routes use per-route auth declared in the router itself.
app.include_router(samp.router)

# Server-side rendering — Phase 1+2. Imported lazily so a backend
# without the EGL build can still serve the rest of the API; on import
# failure the router simply doesn't mount and the dataset open response
# returns ``server_side_rendering_available=False``.
try:
    from .routers import render as render_routes  # noqa: PLC0415
    # NB: do NOT pass ``dependencies=[_auth]`` here. The render router
    # exposes a WebSocket endpoint (``/v1/render/{session_id}/ws``) and
    # FastAPI ``APIKeyHeader`` security objects need an HTTP ``Request``
    # to evaluate — they raise
    #   TypeError: APIKeyHeader.__call__() missing 1 required positional
    #              argument: 'request'
    # on a WebSocket connection. Auth is enforced per-route:
    #   * HTTP routes carry ``_: None = _auth`` in their signature
    #     (see routers/render.py).
    #   * The WS handler validates the token from ``?token=`` (or the
    #     X-Visivo-Token header when a proxy forwards it) and closes
    #     with policy code 1008 on mismatch.
    app.include_router(render_routes.router)
    # Enable the lifespan shutdown hook now that the render router is mounted.
    _render_mounted = True
except ImportError as _ssr_import_err:
    logging.getLogger("visivo.backend").info(
        "[startup] Server-side rendering router not mounted: %s "
        "(this is normal on backends built without EGL).",
        _ssr_import_err,
    )
