"""Workspace Exports — persistent FITS artefacts produced by VisIVO.

The "Workspace" is a single per-host directory (``VISIVO_EXPORTS_DIR``,
default ``~/.visivo/exports/``) where the cube export workers
(`save_subregion`, `moment_fits`) deposit their products. Unlike the
old tempfile-based outputs, files here survive across sessions and are
browsable from the client.

This router exposes the three lifecycle operations the client needs:

  * ``GET    /v1/exports/list``     — enumerate the workspace
  * ``GET    /v1/exports/download`` — stream one artefact for local save
  * ``DELETE /v1/exports/{name}``   — remove an artefact (irreversible)

Path safety: every operation uses :func:`resolve_workspace_path`, which
refuses any input that tries to escape the workspace via traversal
sequences (``../``) or absolute paths.
"""
from __future__ import annotations

import asyncio
import logging
from contextlib import suppress
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Query
from fastapi.responses import FileResponse
from pydantic import BaseModel

from ..auth import require_operator, verify_token
from ..sessions import Session, get_session
from ..dependencies import (
    _require_dataset,
    get_exports_dir,
    make_workspace_path,
    resolve_workspace_path,
)

router = APIRouter(tags=["exports"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)
# The Workspace Exports directory is one shared folder on the server; with
# per-user tokens configured only the operator may list/fetch/delete it.
_operator = Depends(require_operator)


class WorkspaceExportEntry(BaseModel):
    """One entry in the workspace listing.

    Fields:
        filename: basename relative to the workspace dir.
        size: file size in bytes.
        modified_time: ISO-8601 UTC timestamp.
        absolute_path: on-disk absolute path (mostly diagnostic).
    """

    filename: str
    size: int
    modified_time: str
    absolute_path: str


class ExportsListResponse(BaseModel):
    valid: bool
    error: str = ""
    workspace_dir: str = ""
    entries: list[WorkspaceExportEntry] = []


class ExportsDeleteResponse(BaseModel):
    valid: bool
    error: str = ""
    deleted_filename: str = ""


@router.get("/v1/exports/list",
            response_model=ExportsListResponse, tags=["exports"])
async def exports_list(_: str = _operator) -> ExportsListResponse:
    """Enumerate every regular file currently in the Workspace Exports
    directory, sorted by mtime (most-recent first)."""
    try:
        workspace = get_exports_dir()
        entries: list[WorkspaceExportEntry] = []
        for child in workspace.iterdir():
            try:
                if not child.is_file():
                    continue
                st = child.stat()
                entries.append(WorkspaceExportEntry(
                    filename=child.name,
                    size=int(st.st_size),
                    modified_time=datetime.fromtimestamp(
                        st.st_mtime, tz=timezone.utc).isoformat(),
                    absolute_path=str(child),
                ))
            except OSError as exc:
                logger.warning("[exports] stat() failed for %s: %s", child, exc)
                continue
        entries.sort(key=lambda e: e.modified_time, reverse=True)
        return ExportsListResponse(
            valid=True, workspace_dir=str(workspace), entries=entries,
        )
    except Exception as exc:
        return ExportsListResponse(valid=False, error=str(exc))


@router.get("/v1/exports/download", tags=["exports"])
async def exports_download(
    filename: str = Query(..., description="Workspace-relative filename"),
    _: str = _operator,
) -> FileResponse:
    """Stream one workspace artefact back to the client so the user can
    persist it anywhere on their local filesystem."""
    try:
        target = resolve_workspace_path(filename)
    except ValueError as exc:
        raise HTTPException(400, str(exc))
    if not target.exists() or not target.is_file():
        raise HTTPException(404, f"File not found in workspace: {filename}")
    return FileResponse(
        path=str(target),
        media_type="application/fits",
        filename=target.name,
    )


class ExportsCopyRequest(BaseModel):
    """Copy an opened dataset FITS file into the Workspace Exports."""

    dataset_id: str
    output_basename: str = ""


class ExportsCopyResponse(BaseModel):
    valid: bool
    error: str = ""
    workspace_filename: str = ""
    output_path: str = ""


@router.post("/v1/exports/copy_dataset",
             response_model=ExportsCopyResponse, tags=["exports"])
async def exports_copy_dataset(
    request: ExportsCopyRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ExportsCopyResponse:
    """Copy the source FITS file of an opened dataset into the Workspace
    Exports directory (useful for 2-D images that don't have a dedicated
    crop/moment export path)."""
    import shutil

    # Resolve the dataset in the CALLER'S session only — scanning every session
    # would both leak other users' files and depend on a registry API that does
    # not exist (the previous REGISTRY.values() call raised AttributeError).
    entry = session.datasets.get(request.dataset_id)
    src_path = entry.get("path") if entry else None
    if not src_path:
        return ExportsCopyResponse(valid=False,
                                   error=f"Dataset '{request.dataset_id}' not found.")

    src = Path(src_path)
    if not src.exists():
        return ExportsCopyResponse(valid=False,
                                   error=f"Source file not found: {src}")
    try:
        basename = request.output_basename.strip() or src.name
        from ..dependencies import make_workspace_path
        dest = make_workspace_path(basename)
        shutil.copy2(str(src), str(dest))
        logger.info("[exports] copied dataset %s → %s", src, dest)
    except Exception as exc:
        return ExportsCopyResponse(valid=False, error=str(exc))
    return ExportsCopyResponse(
        valid=True,
        workspace_filename=dest.name,
        output_path=str(dest),
    )


class SaveMap2dRequest(BaseModel):
    """A 2-D map computed by the client, to be kept as a FITS in the workspace."""

    #: The dataset the map derives from — the source of its celestial WCS.
    dataset_id: str
    #: Suggested file name; a numeric suffix is added on collision.
    output_basename: str = ""
    width: int
    height: int
    #: Row-major float32 little-endian pixels, base64-encoded.
    pixels_base64: str
    #: Physical unit of the map, which is rarely the source cube's.
    bunit: str = ""
    #: Provenance, written as HISTORY cards: what was computed, from what.
    history: list[str] = []
    #: (x0, y0) pixel origin when the map covers a sub-region of the source.
    region_origin: list[int] | None = None


@router.post("/v1/exports/save_map_2d", tags=["exports"])
async def exports_save_map_2d(
    request: SaveMap2dRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
):
    """Persist a client-computed 2-D map as a standalone FITS in the workspace.

    The generic export path. ``/v1/cube/save_channel_2d`` and the moment
    exporter re-derive their pixels from a recipe, which only works for
    products the service can recompute; everything else the viewer produces —
    polarised intensity, position angle, Faraday depth, line-width — is
    finished pixels, and this is how it leaves the application.

    The header is built here from the source dataset, so every export gets the
    same celestial WCS treatment rather than a client-side reimplementation.
    """
    from ..compute import worker_save_map_2d  # noqa: PLC0415

    entry = _require_dataset(session, request.dataset_id)
    origin = None
    if request.region_origin is not None:
        if len(request.region_origin) != 2:
            raise HTTPException(422, "region_origin must be [x0, y0] in pixels.")
        origin = (int(request.region_origin[0]), int(request.region_origin[1]))

    basename = request.output_basename.strip() or f"{Path(entry['path']).stem}_map.fits"
    if not basename.lower().endswith((".fits", ".fit", ".fts")):
        basename += ".fits"
    # Reserved: two exports started together must not be handed one path.
    output_path = str(make_workspace_path(basename, reserve=True))

    try:
        result = await asyncio.to_thread(
            worker_save_map_2d, entry["path"], output_path, request.pixels_base64,
            request.width, request.height, request.bunit, request.history, origin)
    except Exception as exc:
        # make_workspace_path() reserved the name with an empty file; a failed
        # export must not leave it in the workspace listing.
        with suppress(OSError):
            Path(output_path).unlink()
        if isinstance(exc, HTTPException):
            raise
        if isinstance(exc, ValueError):
            raise HTTPException(422, str(exc)) from exc
        logger.warning("[save_map_2d] failed dataset_id=%s error=%s", request.dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc

    # Registered like any other export, so the map can be reopened in the image
    # viewer without a round trip through the filesystem.
    new_id = f"ds_{uuid.uuid4().hex[:12]}"
    session.datasets[new_id] = {
        "path": result["output_path"], "kind": "image",
        "width": result["width"], "height": result["height"], "depth": 1,
    }
    logger.info("[save_map_2d] registered new dataset  new_id=%s  path=%s",
                new_id, result["output_path"])
    return {
        "valid": True, "error": "",
        "new_dataset_id": new_id,
        "workspace_filename": Path(result["output_path"]).name,
        **{k: result[k] for k in ("output_path", "width", "height", "range_min", "range_max")},
    }


@router.delete("/v1/exports/{filename}",
               response_model=ExportsDeleteResponse, tags=["exports"])
async def exports_delete(
    filename: str,
    _: str = _operator,
) -> ExportsDeleteResponse:
    """Remove one workspace artefact. Idempotent: returns 200 with
    ``valid=false`` if the file does not exist (no exception)."""
    try:
        target = resolve_workspace_path(filename)
    except ValueError as exc:
        return ExportsDeleteResponse(valid=False, error=str(exc))
    if not target.exists():
        return ExportsDeleteResponse(
            valid=False, error=f"File not found: {filename}",
        )
    try:
        target.unlink()
    except OSError as exc:
        return ExportsDeleteResponse(valid=False, error=str(exc))
    logger.info("[exports] deleted %s", target)
    return ExportsDeleteResponse(valid=True, deleted_filename=target.name)
