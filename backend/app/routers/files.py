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
from ..fits_dataset import ScientificFitsDataset, image_hdu_index
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
            # The image's own header (WCS, BUNIT) — the extension's, on a
            # multi-extension product — followed by the primary's descriptive
            # cards it does not repeat (OBJECT, the ESO hierarchy). Structural
            # cards and duplicates stay out: the header editor needs every
            # keyword once, and edits go to the image HDU.
            index = image_hdu_index(hdul)
            image_header = hdul[index].header
            cards = [str(card) for card in image_header.cards]
            if index != 0:
                skip = {"SIMPLE", "BITPIX", "NAXIS", "EXTEND", "PCOUNT", "GCOUNT",
                        "XTENSION", "EXTNAME", "EXTVER", "END", "COMMENT", "HISTORY", ""}
                for card in hdul[0].header.cards:
                    key = card.keyword
                    if key in skip or key.startswith("NAXIS") or key in image_header:
                        continue
                    cards.append(str(card))
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


# ── Header editing: write a corrected copy into the workspace ────────────────

class HeaderCardEdit(BaseModel):
    key: str
    value: Any = ""
    #: None (omitted) leaves the card's comment alone; "" clears it.
    comment: str | None = None


class FileHeaderPatchRequest(BaseModel):
    path: str
    cards: list[HeaderCardEdit] = []
    remove: list[str] = []
    #: Workspace Exports filename (basename only). Empty → "<stem>_hdr.fits".
    output_basename: str = ""


class FileHeaderPatchResponse(BaseModel):
    valid: bool
    error: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    #: One line per edit, naming the old value as well as the new.
    changes: list[str] = []
    hdu_index: int = 0
    #: "celestial" | "degraded" | "none" | "unknown" — what the edited header
    #: now amounts to, so "did that fix it?" is answered here.
    wcs_status: str = ""
    wcs_message: str = ""
    wcs_celestial: bool = False


@router.post("/v1/files/header/patch", response_model=FileHeaderPatchResponse, tags=["files"])
async def patch_file_header(request: FileHeaderPatchRequest,
                            _: None = _auth) -> FileHeaderPatchResponse:
    """Apply header edits to a COPY of the file, in the Workspace Exports dir.

    The input is never modified. Structural keywords (SIMPLE, BITPIX, NAXISn, …)
    are refused: they describe the bytes on disk, and editing them here would
    make the header disagree with the data that follows it.
    """
    from ..fits_header_edit import HeaderEditError, worker_patch_header  # noqa: PLC0415

    try:
        source = _normalize_path(request.path)
    except Exception:
        return FileHeaderPatchResponse(valid=False, error="File not found.")
    if not source.is_file():
        return FileHeaderPatchResponse(valid=False, error="File not found.")

    basename = request.output_basename.strip() or f"{source.stem}_hdr.fits"
    output_path = str(make_workspace_path(basename))
    try:
        result = await _run(
            worker_patch_header, str(source), output_path,
            [card.model_dump() for card in request.cards], list(request.remove))
    except HeaderEditError as exc:
        return FileHeaderPatchResponse(valid=False, error=str(exc))
    except Exception as exc:
        logger.exception("[header-patch] failed path=%s", request.path)
        return FileHeaderPatchResponse(
            valid=False,
            error="The header could not be written. See the backend log.")

    return FileHeaderPatchResponse(
        valid=True, workspace_filename=Path(result["output_path"]).name, **result)
