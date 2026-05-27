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

import logging
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Query
from fastapi.responses import FileResponse
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import (
    get_exports_dir,
    resolve_workspace_path,
)

router = APIRouter(tags=["exports"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)


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
async def exports_list(_: None = _auth) -> ExportsListResponse:
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
    _: None = _auth,
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


@router.delete("/v1/exports/{filename}",
               response_model=ExportsDeleteResponse, tags=["exports"])
async def exports_delete(
    filename: str,
    _: None = _auth,
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
