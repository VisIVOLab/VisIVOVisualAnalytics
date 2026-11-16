"""Isosurfaces of a cube's field for the headset and for export (issue #370;
the extraction is app/isosurface.py, the API docs/backend-api/isosurface.md).

``POST /v1/isosurface/extract`` starts a task (``/v1/tasks/{id}`` gives its
progress and, once completed, the surfaces' descriptions, each with a
``mesh_id``); levels already made are not made again, and a request whose
levels are all made answers a completed task at once. ``GET
/v1/isosurface/mesh/{mesh_id}`` is the compact mesh the headset draws, and
``.../export`` the same surface as PLY or binary glTF, sent back or saved in
the workspace's exports.
"""
from __future__ import annotations

import asyncio
import contextlib
import logging
import re
import threading
from typing import Literal

from fastapi import APIRouter, Depends, HTTPException, Query, Response
from pydantic import BaseModel, Field, field_validator

from .. import isosurface as ISO
from ..auth import verify_token
from ..dependencies import _require_cube_path, get_exports_dir
from ..schemas import TaskCreateResponse
from ..sessions import Session, get_session
from ..tasks import TASKS

router = APIRouter(tags=["isosurface"])
logger = logging.getLogger("visivo.isosurface")
_auth = Depends(verify_token)


class IsosurfaceRequest(BaseModel):
    dataset_id: str
    #: Another field of the dataset's source (a PLUTO variable or a VBT field,
    #: by its name in reduced_info's ``fields``); empty: the dataset's own.
    field: str = ""
    #: The levels, in the field's units (above zero on a log scale).
    levels: list[float] = Field(min_length=1, max_length=ISO.MAX_LEVELS)
    #: The space the field is contoured in: ``log`` contours log10 of the
    #: values at log10 of each level; ``auto`` is the reduced cube's rule.
    scale: Literal["auto", "linear", "log"] = "auto"
    #: The level of detail: -1 automatic (the finest within ``max_voxels``),
    #: 0 the cube itself, ``l`` the cube halved ``l`` times (its brick
    #: pyramid's level when one is ready, else every 2^l-th voxel).
    lod: int = Field(default=-1, ge=-1, le=16)
    max_voxels: int = Field(default=ISO.DEFAULT_MAX_VOXELS, ge=8 ** 3, le=ISO.HARD_MAX_VOXELS)
    #: Each surface decimated to at most this many triangles.
    max_triangles: int = Field(default=ISO.DEFAULT_MAX_TRIANGLES, ge=ISO.MIN_TRIANGLES,
                               le=ISO.MAX_TRIANGLES)

    @field_validator("levels")
    @classmethod
    def _finite(cls, v: list[float]) -> list[float]:
        import math  # noqa: PLC0415
        if any(not math.isfinite(x) for x in v):
            raise ValueError("Every level must be a finite number.")
        if len(set(v)) != len(v):
            raise ValueError("A level is given twice.")
        return v


def _field_path(path: str, name: str) -> str:
    if not name:
        return path
    from ..volume_source import field_listing  # noqa: PLC0415
    listed, _ = field_listing(path)
    for f in listed:
        if str(f["name"]) == name:
            return str(f["path"])
    names = ", ".join(str(f["name"]) for f in listed) or "none"
    raise HTTPException(422, f"No field {name!r} beside this cube: its fields are {names}.")


def _plan(path: str, request: IsosurfaceRequest) -> dict:
    """What the request resolves to before any surface is made: the field's
    path, its pyramid, the scale, the levels' cache keys."""
    from ..preview_pyramid import ready_pyramid  # noqa: PLC0415
    from ..product_cache import file_identity  # noqa: PLC0415
    pyramid = ready_pyramid(path)
    scale = ISO.resolve_scale(path, request.scale, pyramid)
    for v in request.levels:
        ISO.transform_level(v, scale)   # a level with no logarithm is refused here
    lod = f"auto:{request.max_voxels}" if request.lod < 0 else str(request.lod)
    identity = file_identity(path)
    lod += "|pyramid" if pyramid else "|cube"
    keys = [ISO.MeshCache.key(identity, path, v, scale, lod, request.max_triangles) for v in request.levels]
    return {"pyramid": pyramid, "scale": scale, "keys": keys}


def _result(scale: str, made: list[ISO.Cached], hits: int) -> dict:
    meshes = [{**c.describe, "mesh_id": c.mesh_id} for c in made]
    return {"valid": True, "error": "", "scale": scale, "meshes": meshes, "cache_hits": hits,
            "triangles": sum(m["triangles"] for m in meshes)}


@router.post("/v1/isosurface/extract", response_model=TaskCreateResponse)
async def isosurface_extract(request: IsosurfaceRequest, token: str = _auth,
                             session: Session = Depends(get_session)) -> TaskCreateResponse:
    """Surfaces of the dataset's field at ``levels``, as a task: its result
    (``/v1/tasks/{id}``) lists one mesh a level, in the request's order, each
    with ``mesh_id``, ``triangles``, ``vertices``, ``raw_triangles`` (before
    decimation), ``lod``, ``grid``, ``source`` and timings. Completed at once
    (``cache_hit``) when every level is made already."""
    path = _field_path(_require_cube_path(session, request.dataset_id), request.field)
    try:
        plan = await asyncio.to_thread(_plan, path, request)
    except HTTPException:
        raise
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    cached = [ISO.CACHE.lookup(k) for k in plan["keys"]]
    task = TASKS.create("isosurface", owner_token=token)
    task_id = task.task_id
    if all(c is not None for c in cached):
        TASKS.update(task_id, status="completed", progress=1.0, cache_hit=True,
                     result=_result(plan["scale"], cached, len(cached)))
        return TaskCreateResponse(valid=True, error="", task_id=task_id, status="completed", cache_hit=True)

    todo = [i for i, c in enumerate(cached) if c is None]
    stop = threading.Event()
    TASKS.update(task_id, status="running", progress=0.0)

    def _work() -> list[ISO.Cached]:
        with ISO.EXTRACT_LOCK:
            if stop.is_set():
                raise ISO.IsosurfaceCancelled("Cancelled by the client.")
            meshes = ISO.extract(
                path, [request.levels[i] for i in todo], scale=plan["scale"], lod=request.lod,
                max_voxels=request.max_voxels, max_triangles=request.max_triangles,
                pyramid_path=plan["pyramid"],
                progress=lambda f, _s: TASKS.update(task_id, progress=round(0.99 * f, 3)),
                should_stop=stop.is_set)
        made = []
        for i, mesh in zip(todo, meshes):
            wire = ISO.encode_mesh(mesh)
            made.append(ISO.CACHE.put(plan["keys"][i], mesh, path, ISO.describe(mesh, len(wire))))
        return made

    async def _runner() -> None:
        work = asyncio.ensure_future(asyncio.to_thread(_work))
        work.add_done_callback(lambda f: f.cancelled() or f.exception())
        TASKS.update(task_id, work_handle=work)
        try:
            made = await asyncio.shield(work)
            out = list(cached)
            for i, c in zip(todo, made):
                out[i] = c
            TASKS.update(task_id, status="completed", progress=1.0,
                         result=_result(plan["scale"], out, len(cached) - len(todo)))
        except (asyncio.CancelledError, ISO.IsosurfaceCancelled):
            stop.set()
            TASKS.update(task_id, status="cancelled", error="Cancelled by client.")
            with contextlib.suppress(BaseException):
                await work
        except ValueError as exc:   # a surface too large at the level asked for, a bad level
            TASKS.update(task_id, status="failed", progress=1.0, error=str(exc))
        except Exception as exc:  # noqa: BLE001 - reported through the task
            logger.exception("[isosurface] %s failed", path)
            TASKS.update(task_id, status="failed", progress=1.0, error=str(exc))

    TASKS.update(task_id, asyncio_handle=asyncio.create_task(_runner()))
    return TaskCreateResponse(valid=True, error="", task_id=task_id, status="running")


def _cached(mesh_id: str) -> ISO.Cached:
    c = ISO.CACHE.get(mesh_id) if re.fullmatch(r"[0-9a-f]{24}", mesh_id) else None
    if c is None:
        raise HTTPException(404, f"No isosurface {mesh_id!r}: extract it again "
                                 "(made surfaces are kept while the cache has room).")
    return c


@router.get("/v1/isosurface/mesh/{mesh_id}")
async def isosurface_mesh(mesh_id: str) -> Response:
    """The surface as the headset draws it (``app.isosurface.encode_mesh``:
    a 40-byte header, 16-bit positions in box coordinates, 8-bit normals,
    32-bit indices)."""
    c = _cached(mesh_id)
    return Response(content=c.wire, media_type="application/octet-stream",
                    headers={"Content-Length": str(len(c.wire)), "Cache-Control": "no-store"})


def _hex_color(text: str) -> tuple[float, float, float, float]:
    m = re.fullmatch(r"#?([0-9a-fA-F]{6})", text or "")
    if not m:
        return (0.8, 0.8, 0.8, 1.0)
    h = m.group(1)
    return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)) + (1.0,)


@router.get("/v1/isosurface/mesh/{mesh_id}/export")
async def isosurface_export(mesh_id: str, format: Literal["ply", "glb"] = "ply",  # noqa: A002
                            save: bool = False, color: str = Query("", max_length=7)) -> Response:
    """The surface as a binary PLY or a binary glTF (``glb``), in the cube's
    world coordinates when its axes are linear lengths (a simulation's X, Y,
    Z), else in full-resolution voxels. ``save``: written in the workspace's
    exports (``isosurfaces/``) and answered with its path instead."""
    c = _cached(mesh_id)

    def make() -> bytes:
        axes = ISO.world_axes(c.path)
        if format == "glb":
            return ISO.to_glb(c.mesh, axes, _hex_color(color))
        return ISO.to_ply(c.mesh, axes, comment=f"source {c.path.split('/')[-1]}")
    body = await asyncio.to_thread(make)
    stem = re.sub(r"[^A-Za-z0-9._=-]+", "_", c.path.split("/")[-1])[:80]
    name = f"{stem}_iso_{c.mesh.level:.6g}_{mesh_id[:8]}.{format}"
    if save:
        out = get_exports_dir() / "isosurfaces"
        out.mkdir(parents=True, exist_ok=True)
        target = out / name
        await asyncio.to_thread(target.write_bytes, body)
        import json  # noqa: PLC0415
        return Response(content=json.dumps({"valid": True, "path": str(target), "bytes": len(body),
                                            "format": format}),
                        media_type="application/json")
    media = "model/gltf-binary" if format == "glb" else "application/octet-stream"
    return Response(content=body, media_type=media,
                    headers={"Content-Disposition": f'attachment; filename="{name}"',
                             "Content-Length": str(len(body))})
