from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel
from typing import Any
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
    # A refused path and a missing one answer identically: probing the browser
    # must not reveal which directories exist outside the allowed roots.
    _not_found = FilesListResponse(valid=False, error="Directory not found.",
                                   current_path="", entries=[])
    try:
        directory = _normalize_path(path)
    except Exception:
        return _not_found

    if not directory.exists() or not directory.is_dir():
        return _not_found

    try:
        children = sorted(directory.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower()))
    except OSError as exc:
        return FilesListResponse(valid=False, error=str(exc), current_path=str(directory), entries=[])

    # Rejecting a direct request for ~/.ssh is not enough — listing its parent
    # would still hand out the name. Filter the children too.
    if hide_hidden_entries():
        children = [c for c in children if not c.name.startswith(".")]

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

@router.post("/v1/files/header", response_model=FileHeaderResponse, tags=["files"])
async def file_header(request: FileHeaderRequest, _: None = _auth) -> FileHeaderResponse:
    try:
        path = _normalize_path(request.path)
        if not _is_fits(path):
            raise ValueError("FITS file not found.")
        with fits.open(str(path), memmap=True) as hdul:
            cards = [str(card) for card in hdul[0].header.cards]
    except PermissionError:
        # Same answer as a genuinely absent file: echoing the refusal reason
        # would tell the caller that the path exists but is out of bounds.
        return FileHeaderResponse(valid=False, error="FITS file not found.", cards=[])
    except Exception as exc:
        return FileHeaderResponse(valid=False, error=str(exc), cards=[])
    return FileHeaderResponse(valid=True, error="", cards=cards)


@router.post("/v1/files/classify", response_model=FileClassifyResponse, tags=["files"])
async def classify_file(request: FileClassifyRequest, _: None = _auth) -> FileClassifyResponse:
    """What is this file? — so the client can offer one *Open…* instead of three.

    The answer is derived from the file itself (magic bytes, extension, at most
    one FITS header); nothing is opened, converted or registered. ``opener``
    names the route that should receive the path, and ``confident`` is False
    when the client should ask the user rather than pick.
    """
    from ..classify import classify_path  # noqa: PLC0415

    try:
        path = _normalize_path(request.path)
    except Exception:
        # Same answer for refused and missing: the classifier must not become a
        # way to enumerate what lies outside the allowed roots.
        return FileClassifyResponse(valid=False, error="File not found.")
    if not path.exists():
        return FileClassifyResponse(valid=False, error="File not found.")

    try:
        verdict = classify_path(path)
    except OSError as exc:
        return FileClassifyResponse(valid=False, error=str(exc))

    logger.info("[classify] %s → %s (%s)", path, verdict["kind"], verdict["opener"])
    return FileClassifyResponse(valid=True, error="", path=str(path), **verdict)
