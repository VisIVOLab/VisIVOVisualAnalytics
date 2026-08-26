"""
main.py – VisIVO Backend v1 (Refactored)

The backend has been modularized into routers and models for better maintainability.
Each domain (files, datasets, catalogue, vbt, products, cube, tasks, image,
cosmology, hips, resolve, meta) lives in its own router module under app/routers/.
"""

from __future__ import annotations

import contextlib
import logging
import uuid
from pathlib import Path
from fastapi import FastAPI, Depends, Request, Response
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

from .auth import verify_token
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


# ── Global exception handler (E-05) ───────────────────────────────────────────
# Any exception that escapes a route handler is logged server-side WITH its
# traceback and returned to the client as a compact, non-leaking JSON body
# ``{error, error_code, request_id}``. Without this, FastAPI's default 500 (or a
# handler that echoes str(exc)/a traceback) exposes filesystem paths and internals
# to the client. HTTPException is intentionally NOT caught here — FastAPI's own
# handler renders those (they carry deliberate, safe messages).
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
