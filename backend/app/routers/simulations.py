"""Simulation snapshots — AREPO inspection and uniform-cube extraction, and the
Cartesian view of a PLUTO run on curvilinear or stretched cells.

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

* ``POST /v1/simulations/pluto/cartesian/plan`` and ``/cartesian`` (issue #527) —
  the grid a polar, cylindrical, spherical or stretched PLUTO output would be
  resampled onto, and the resampling itself: a FITS cube per variable, opened
  like any other (``app/compute/pluto_cartesian.py``).

* ``POST /v1/simulations/amr/plan``, ``/amr/uniform`` and ``/amr/level-map``
  (issue #550) — a uniform cube of one field of an AMR output (RAMSES) at a
  chosen level over a box, the plan that says its cost first, and the
  refinement level of the cells a slice cuts (``app/compute/amr_views.py``).

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


# ── PLUTO: the Cartesian view of a curvilinear or stretched grid (#527) ──────

class PlutoCartesianPlanRequest(BaseModel):
    #: A PLUTO variable reference, a data file or the run's ``.out``.
    path: str
    #: Voxels per output axis (FITS order: x, y[, z] or x, z); None: as fine as
    #: the finest length cell, coarsened to ``max_voxels``.
    shape: list[int] | None = None
    max_voxels: int | None = None
    #: A meridional (axisymmetric) image also drawn for negative x.
    mirror: bool = False


class PlutoCartesianRequest(PlutoCartesianPlanRequest):
    #: Variables to resample: None, the one ``path`` names; ``["*"]``, every
    #: cell variable of the output on disk.
    fields: list[str] | None = None


def _pluto_cartesian_error(exc: Exception, path: str) -> HTTPException:
    if isinstance(exc, FileNotFoundError):
        return HTTPException(404, f"No such file: {path}")
    if isinstance(exc, ValueError):   # PlutoCartesianError, an unreadable grid, a wrong size
        return HTTPException(422, str(exc))
    return HTTPException(500, str(exc))


def _pluto_reference(path: str) -> str:
    """The variable reference of a data file, a reference or a ``.out``."""
    from ..pluto_source import pluto_dataset_path  # noqa: PLC0415
    from ..volume_source import field_data_file  # noqa: PLC0415
    base = field_data_file(path)
    ref = pluto_dataset_path(str(_normalize_path(base)) + path[len(base):])
    if ref is None:
        raise HTTPException(422, f"{path} is not a PLUTO output with its grid.out beside it.")
    return ref


@router.post("/v1/simulations/pluto/cartesian/plan", tags=["simulations"])
async def pluto_cartesian_plan(request: PlutoCartesianPlanRequest, _: None = _auth):
    """What the Cartesian view of this PLUTO output would be: the mode (cube,
    plane, meridional), its axes, box, voxel counts and size. Reads grid.out
    only. 422 for a run with no such view (1-D, unknown geometry) or a request
    that cannot be met."""
    from .. import pluto_source as ps  # noqa: PLC0415
    from ..compute.pluto_cartesian import DEFAULT_MAX_VOXELS, plan_cartesian  # noqa: PLC0415

    ref = await asyncio.to_thread(_pluto_reference, request.path)
    try:
        var = await asyncio.to_thread(ps.resolve, ref)
        plan = plan_cartesian(var.grid, request.shape, request.max_voxels or DEFAULT_MAX_VOXELS,
                              request.mirror)
    except Exception as exc:  # noqa: BLE001
        raise _pluto_cartesian_error(exc, request.path) from exc
    return {"valid": True, "error": "", "path": ref, "plan": plan.as_dict(),
            "index_reason": var.grid.index_reason()}


@router.post("/v1/simulations/pluto/cartesian", tags=["simulations"])
async def pluto_cartesian(request: PlutoCartesianRequest, _: None = _auth):
    """Resample PLUTO variables onto a uniform Cartesian grid, one FITS per
    variable.

    Returns ``{valid, error, plan, outputs: [{field, path, shape}],
    source_geometry}``. The cubes land in the backend's exports directory and
    open through the ordinary dataset route; they keep the source variable's
    physical units and record how they were made in their header.
    """
    from ..compute.pluto_cartesian import DEFAULT_MAX_VOXELS, worker_pluto_cartesian  # noqa: PLC0415

    ref = await asyncio.to_thread(_pluto_reference, request.path)
    out_dir = get_exports_dir() / "pluto-cartesian"
    logger.info("[pluto-cartesian] path=%s fields=%s shape=%s", ref, request.fields, request.shape)
    try:
        # Heavy: it reads the whole source through a memory map, so it goes
        # through the same admission control and wall-clock timeout as AREPO's.
        result = await _run_heavy(
            worker_pluto_cartesian, ref, request.fields, request.shape,
            request.max_voxels or DEFAULT_MAX_VOXELS, request.mirror, str(out_dir))
    except HTTPException:
        raise
    except Exception as exc:  # noqa: BLE001
        logger.warning("[pluto-cartesian] failed path=%s error=%s", ref, exc)
        raise _pluto_cartesian_error(exc, request.path) from exc
    return {"valid": True, "error": "", **result}


# ── AMR outputs: a uniform cube, the level map of a slice (#550) ─────────────

class AmrPlanRequest(BaseModel):
    #: A RAMSES output: its directory, a file of it or a component reference.
    path: str
    #: A hydro field, ``mass`` (the gas mass in a voxel) or ``level``.
    field: str = "density"
    #: The target level (voxel side 2^-level of the box); None: the finest
    #: level the region holds that fits ``max_voxels``.
    level: int | None = Field(None, ge=0)
    #: The region's two corners (None: the whole run), in ``region_unit``:
    #: ``box`` (a fraction of the box) or ``physical`` (the cube's own axes).
    lo: list[float] | None = None
    hi: list[float] | None = None
    region_unit: str = "box"
    max_voxels: int | None = None


class AmrLevelMapRequest(BaseModel):
    path: str
    axis: str = "z"
    coordinate: float
    #: The unit of ``coordinate`` and of the rectangles: ``box`` or ``physical``.
    unit: str = "box"
    lo: list[float] | None = None
    hi: list[float] | None = None
    region_unit: str = "box"
    max_rects: int = Field(200_000, ge=1, le=5_000_000)


def _amr_error(exc: Exception, path: str) -> HTTPException:
    if isinstance(exc, FileNotFoundError):
        return HTTPException(404, f"No such file: {path}")
    if isinstance(exc, ValueError):   # AmrViewError, an unreadable mesh, a wrong request
        return HTTPException(422, str(exc))
    return HTTPException(500, str(exc))


def _amr_hierarchy(path: str):
    """The mesh ``path`` names, every file of it inside the data roots."""
    from ..amr_source import open_hierarchy  # noqa: PLC0415
    from ..dependencies import _within_data_roots  # noqa: PLC0415
    _normalize_path(path.split("::ramses-family=")[0])
    return open_hierarchy(path, _within_data_roots)


@router.post("/v1/simulations/amr/plan", tags=["simulations"])
async def amr_plan(request: AmrPlanRequest, _: None = _auth):
    """What the uniform cube of this field would be: its level, voxels, bytes,
    memory, whether it fits the limit (``max_voxels``, default and hard limit
    those of the PLUTO Cartesian view), the grid's box in box units and in the
    run's, and ``finest_level_in_region``: the level "refine this box" would
    ask for. Reads the mesh's geometry only. 422 for a request that cannot be
    met."""
    from ..compute.amr_views import DEFAULT_MAX_VOXELS, plan_view  # noqa: PLC0415

    try:
        h = await asyncio.to_thread(_amr_hierarchy, request.path)
        _, answer = await asyncio.to_thread(
            plan_view, h, request.field, request.level, request.lo, request.hi,
            request.region_unit, request.max_voxels or DEFAULT_MAX_VOXELS)
    except PermissionError:
        raise
    except Exception as exc:  # noqa: BLE001
        raise _amr_error(exc, request.path) from exc
    return {"valid": True, "error": "", "path": request.path, "code": h.code, "plan": answer}


@router.post("/v1/simulations/amr/uniform", tags=["simulations"])
async def amr_uniform(request: AmrPlanRequest, _: None = _auth):
    """Resample one field of an AMR output onto a uniform grid, a FITS cube in
    the exports directory that opens like any volume: X/Y/Z in the run's
    length unit, values in its unit for the field, the AMR* cards saying how it
    was made. A region and no level refines that box at the finest level it
    holds (if it fits ``max_voxels``).

    Returns ``{valid, error, plan, outputs: [{field, path, shape}],
    finest_level}``; 422 for a request that cannot be met or a cube above the
    limit (see the plan)."""
    from ..compute.amr_views import DEFAULT_MAX_VOXELS, plan_view, worker_amr_uniform  # noqa: PLC0415

    limit = request.max_voxels or DEFAULT_MAX_VOXELS
    out_dir = get_exports_dir() / "amr-uniform"
    try:
        h = await asyncio.to_thread(_amr_hierarchy, request.path)
        # The level the plan settles on is what runs (and an impossible request
        # is refused before a heavy job is taken).
        plan, answer = await asyncio.to_thread(
            plan_view, h, request.field, request.level, request.lo, request.hi,
            request.region_unit, limit)
        if not answer["fits"]:
            raise ValueError(answer["reason"])
        logger.info("[amr-uniform] path=%s field=%s level=%s shape=%s", request.path,
                    request.field, plan.level, plan.shape)
        result = await _run_heavy(
            worker_amr_uniform, request.path, request.field, plan.level, request.lo,
            request.hi, request.region_unit, limit, str(out_dir))
    except (HTTPException, PermissionError):
        raise
    except Exception as exc:  # noqa: BLE001
        logger.warning("[amr-uniform] failed path=%s error=%s", request.path, exc)
        raise _amr_error(exc, request.path) from exc
    return {"valid": True, "error": "", **result}


@router.post("/v1/simulations/amr/level-map", tags=["simulations"])
async def amr_level_map(request: AmrLevelMapRequest, _: None = _auth):
    """The refinement level of the cells a slice cuts: the plane ``axis =
    coordinate`` (box or physical units), the leaf cells it crosses as
    rectangles ``[u0, v0, u1, v1]`` in the other two axes, grouped by level
    (``levels``), with their ``counts`` per level, for the client to draw over
    the slice. A window (``lo``, ``hi``) limits it; ``max_rects`` keeps the
    coarsest levels and says ``truncated``. 422 for a plane outside the run."""
    from ..compute.amr_views import level_map  # noqa: PLC0415

    try:
        h = await asyncio.to_thread(_amr_hierarchy, request.path)
        result = await asyncio.to_thread(
            level_map, h, request.axis, request.coordinate, request.unit, request.lo,
            request.hi, request.region_unit, request.max_rects)
    except PermissionError:
        raise
    except Exception as exc:  # noqa: BLE001
        raise _amr_error(exc, request.path) from exc
    return {"valid": True, "error": "", "path": request.path, **result}
