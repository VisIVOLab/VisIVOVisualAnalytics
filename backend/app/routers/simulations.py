"""Simulation snapshots — AREPO inspection and uniform-cube extraction.

An AREPO snapshot is a moving-mesh simulation: an unstructured set of cells that
no viewer can display directly. These routes browse one and resample a window of
it onto a regular grid, which is then an ordinary FITS cube the client opens the
way it opens any other.

Three routes, and the split between them is deliberate:

* ``POST /v1/simulations/arepo/inspect`` — the HDF5 tree, the box size and the
  particle types. Needs only h5py, so browsing works on every deployment.
* ``POST /v1/simulations/arepo/plan`` — what a given level and window *would*
  produce: grid shape, cell size, memory. Also h5py-only, so the client can show
  the cost of a job before anyone starts it.
* ``POST /v1/simulations/arepo/extract`` — the resampling itself, which needs
  ``yt``. 503 with an actionable message when it is not installed.

The legacy client ran the extraction by spawning ``python arepo-extractor.py``
through ``QProcess``, against a Python interpreter path kept in QSettings.
"""
from __future__ import annotations

import asyncio
import logging

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field

from ..auth import verify_token
from ..dependencies import _normalize_path, _run_heavy, get_exports_dir

router = APIRouter(tags=["simulations"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)

DEFAULT_LEVEL = 7
DEFAULT_WINDOW_KPC = [20.0, 20.0, 20.0]
DEFAULT_OFFSET_KPC = [0.0, 0.0, 0.0]
DEFAULT_PARTICLE = "gas"
DEFAULT_FIELDS = ["density"]


class ArepoInspectRequest(BaseModel):
    path: str


class ArepoPlanRequest(BaseModel):
    path: str
    level: int = DEFAULT_LEVEL
    window_kpc: list[float] = Field(default_factory=lambda: list(DEFAULT_WINDOW_KPC))
    offset_kpc: list[float] = Field(default_factory=lambda: list(DEFAULT_OFFSET_KPC))


class ArepoExtractRequest(BaseModel):
    path: str
    level: int = DEFAULT_LEVEL
    particle: str = DEFAULT_PARTICLE
    fields: list[str] = Field(default_factory=lambda: list(DEFAULT_FIELDS))
    window_kpc: list[float] = Field(default_factory=lambda: list(DEFAULT_WINDOW_KPC))
    offset_kpc: list[float] = Field(default_factory=lambda: list(DEFAULT_OFFSET_KPC))


@router.post("/v1/simulations/arepo/inspect", tags=["simulations"])
async def arepo_inspect(request: ArepoInspectRequest, _: None = _auth):
    """Browse an AREPO snapshot: HDF5 tree, box size, particle types and fields.

    Returns ``{valid, error, path, tree, box_size_kpc, unit_length_cm,
    particle_types, yt_available}``. ``tree`` is nested
    ``{name, kind, shape, dtype, attributes, children}``. ``yt_available`` tells
    the client whether the extract route will work here, so it can say so before
    the user fills in a form.
    """
    from ..compute.arepo import ArepoError, inspect_snapshot  # noqa: PLC0415

    path = str(_normalize_path(request.path))
    try:
        result = await asyncio.to_thread(inspect_snapshot, path)
    except ArepoError as exc:
        raise HTTPException(422, str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(404, f"No such file: {request.path}") from exc
    except HTTPException:
        # _run_heavy raises 504 on the wall-clock timeout; the catch-all below
        # would relabel it a 500 and hide what actually happened.
        raise
    except Exception as exc:  # noqa: BLE001
        logger.warning("[arepo] inspect failed path=%s error=%s", path, exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", **result}


@router.post("/v1/simulations/arepo/plan", tags=["simulations"])
async def arepo_plan(request: ArepoPlanRequest, _: None = _auth):
    """Grid a level and window would produce, without extracting anything.

    Returns ``{valid, error, shape, cell_size_pc, cells, bytes_estimate, …}``.
    422 when the request is not physical or the grid would be too large — which
    is the answer the client should show *before* starting a job, rather than
    the legacy's fixed "level 9+ may take minutes" warning that knew nothing
    about the window actually asked for.
    """
    from ..compute.arepo import ArepoError, domain_width_kpc, plan_extraction  # noqa: PLC0415

    path = str(_normalize_path(request.path))
    try:
        domain = await asyncio.to_thread(domain_width_kpc, path)
        plan = plan_extraction(request.level, request.window_kpc, request.offset_kpc, domain)
    except ArepoError as exc:
        raise HTTPException(422, str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(404, f"No such file: {request.path}") from exc
    except HTTPException:
        # _run_heavy raises 504 on the wall-clock timeout; the catch-all below
        # would relabel it a 500 and hide what actually happened.
        raise
    except Exception as exc:  # noqa: BLE001
        logger.warning("[arepo] plan failed path=%s error=%s", path, exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", "domain_width_kpc": list(domain), **plan.as_dict()}


@router.post("/v1/simulations/arepo/extract", tags=["simulations"])
async def arepo_extract(request: ArepoExtractRequest, _: None = _auth):
    """Resample a window of the snapshot onto a uniform grid, one FITS per field.

    Returns ``{valid, error, plan, outputs: [{field, particle, path, bunit,
    shape}], domain_width_kpc}``. The cubes land in the backend's exports
    directory and are opened through the ordinary dataset-open route.

    503 when ``yt`` is not installed, 422 on an unusable request (non-physical
    window, unknown field, grid beyond the cell limit).
    """
    from ..compute.arepo import ArepoError, worker_arepo_extract  # noqa: PLC0415

    path = str(_normalize_path(request.path))
    out_dir = get_exports_dir() / "arepo"
    logger.info("[arepo] extract path=%s level=%d particle=%s fields=%s",
                path, request.level, request.particle, request.fields)
    try:
        # Heavy: a covering grid materialises every cell of the window, which is
        # gigabytes. Going through asyncio.to_thread would let any number of
        # these run at once, outside the admission control and the wall-clock
        # timeout every other long compute on this backend observes.
        result = await _run_heavy(
            worker_arepo_extract, path, request.level, request.particle,
            list(request.fields), list(request.window_kpc), list(request.offset_kpc),
            str(out_dir))
    except ImportError as exc:
        raise HTTPException(503, str(exc)) from exc
    except ArepoError as exc:
        raise HTTPException(422, str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(404, f"No such file: {request.path}") from exc
    except HTTPException:
        # _run_heavy raises 504 on the wall-clock timeout; the catch-all below
        # would relabel it a 500 and hide what actually happened.
        raise
    except Exception as exc:  # noqa: BLE001
        logger.warning("[arepo] extract failed path=%s error=%s", path, exc)
        raise HTTPException(500, str(exc)) from exc
    return {"valid": True, "error": "", **result}
