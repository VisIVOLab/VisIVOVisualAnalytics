from fastapi import APIRouter, Depends, HTTPException, Query
from fastapi.responses import FileResponse
from pydantic import BaseModel
from typing import Any
import tempfile
import uuid
import logging
import os
from pathlib import Path
from datetime import datetime, timezone
import base64
import numpy as np
import pandas as pd
from astropy.io import fits
from ..schemas import *
from ..dependencies import *
from ..auth import verify_token
from ..sessions import Session, get_session, REGISTRY
from ..tasks import TASKS
from ..product_cache import PRODUCT_CACHE
from ..fits_dataset import ScientificFitsDataset
import asyncio

router = APIRouter(tags=['files'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

@router.get("/v1/files/list", response_model=FilesListResponse, tags=["files"])
async def list_files(
    path: str = Query(""),
    _: None = _auth,
) -> FilesListResponse:
    try:
        directory = _normalize_path(path)
    except Exception:
        return FilesListResponse(valid=False, error="Invalid path.", current_path="", entries=[])

    if not directory.exists() or not directory.is_dir():
        return FilesListResponse(valid=False, error="Directory not found.", current_path="", entries=[])

    try:
        children = sorted(directory.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower()))
    except OSError as exc:
        return FilesListResponse(valid=False, error=str(exc), current_path=str(directory), entries=[])

    entries: list[FileEntry] = []
    for child in children:
        try:
            st = child.stat()
            size = int(st.st_size)
            mtime = datetime.fromtimestamp(st.st_mtime, tz=timezone.utc).isoformat()
        except OSError:
            size, mtime = 0, ""
        ftype = "directory" if child.is_dir() else "file"
        entries.append(FileEntry(
            name=child.name,
            path=str(child),
            type=ftype,
            size=size,
            modified_time=mtime,
            is_fits=_is_fits(child) if child.is_file() else False,
        ))

    return FilesListResponse(valid=True, error="", current_path=str(directory), entries=entries)

# ── Export-area allowed temp prefixes ────────────────────────────────────────
# /v1/files/download is restricted to artefacts produced by VisIVO's own
# export workers (sub-cube crop, moment-FITS persistence). The path passed
# in must live under one of these prefixes, otherwise the request is
# rejected with 403 — a defensive check against path-traversal and against
# the endpoint accidentally becoming a generic FS read.
_DOWNLOAD_ALLOWED_PREFIXES = (
    Path(tempfile.gettempdir()) / "visivo_subregion",
    Path(tempfile.gettempdir()) / "visivo_moment_fits",
    Path(tempfile.gettempdir()) / "visivo_baseline",   # produced by S-03 too
)


@router.get("/v1/files/download", tags=["files"])
async def download_export(
    path: str = Query(..., description="Absolute path of the export artefact to download"),
    _: None = _auth,
) -> FileResponse:
    """Stream a FITS export artefact back to the client so the user can
    write it anywhere on their local filesystem.

    Only files under VisIVO's own export temp directories are accepted —
    the endpoint is NOT a generic backend filesystem reader.
    """
    try:
        candidate = Path(path).resolve(strict=False)
    except Exception as exc:
        raise HTTPException(400, f"Invalid path: {exc}")
    # Resolve allowed prefixes the same way (handles /private/var symlinks
    # on macOS where /tmp is actually /private/tmp). One match is enough.
    allowed = any(
        str(candidate).startswith(str(prefix.resolve(strict=False)) + os.sep)
        or str(candidate) == str(prefix.resolve(strict=False))
        for prefix in _DOWNLOAD_ALLOWED_PREFIXES
    )
    if not allowed:
        raise HTTPException(
            403,
            f"Path '{path}' is outside the allowed export directories.",
        )
    if not candidate.exists() or not candidate.is_file():
        raise HTTPException(404, f"File not found: {candidate}")
    return FileResponse(
        path=str(candidate),
        media_type="application/fits",
        filename=candidate.name,
    )


@router.post("/v1/files/header", response_model=FileHeaderResponse, tags=["files"])
async def file_header(request: FileHeaderRequest, _: None = _auth) -> FileHeaderResponse:
    try:
        path = _normalize_path(request.path)
        if not _is_fits(path):
            raise ValueError("FITS file not found.")
        with fits.open(str(path), memmap=True) as hdul:
            cards = [str(card) for card in hdul[0].header.cards]
    except Exception as exc:
        return FileHeaderResponse(valid=False, error=str(exc), cards=[])
    return FileHeaderResponse(valid=True, error="", cards=cards)
