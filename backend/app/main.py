"""
main.py – VisIVO Backend v1 (Refactored)

The backend has been modularized into routers and models for better maintainability.
Each domain (files, datasets, catalogue, vbt, products, cube, tasks, image,
cosmology, hips, resolve, meta) lives in its own router module under app/routers/.
"""

from __future__ import annotations

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
)

# ── Logging ───────────────────────────────────────────────────────────────────
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
logger = logging.getLogger("visivo.backend")

# ── FastAPI app ───────────────────────────────────────────────────────────────
app = FastAPI(
    title="VisIVO Backend",
    version="1.0.0",
    description="Astrophysical data visualization backend for ViaLactea Visual Analytics.",
    docs_url="/v1/docs",
    redoc_url="/v1/redoc",
    openapi_url="/v1/openapi.json",
)

_repo_root = Path(__file__).resolve().parents[2]
_samp_static_dir = _repo_root / "icons"
if _samp_static_dir.exists():
    app.mount("/static", StaticFiles(directory=str(_samp_static_dir)), name="static")

# ── Middleware: X-Request-ID (R8) ─────────────────────────────────────────────
@app.middleware("http")
async def attach_request_id(request: Request, call_next):
    request_id = str(uuid.uuid4())
    response: Response = await call_next(request)
    response.headers["X-Request-ID"] = request_id
    return response

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
# SAMP: /v1/samp/files/{token} is public (no auth) for VO tool access;
# all other /v1/samp/* routes use per-route auth declared in the router itself.
app.include_router(samp.router)
