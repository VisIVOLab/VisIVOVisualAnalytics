from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel, Field
from typing import Any, Literal
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
from ..compressed_fits import compression_of, gzip_image_headers
from ..file_preview import (DEFAULT_MAX_BYTES, DEFAULT_MAX_LINES, clamp_bounds,
                            preview_cards, preview_path)
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

# The header route and the file browser's preview show the same cards.
_preview_cards = preview_cards


@router.post("/v1/files/header", response_model=FileHeaderResponse, tags=["files"])
async def file_header(request: FileHeaderRequest, _: None = _auth) -> FileHeaderResponse:
    from ..volume_source import open_fits, split_ref, vbt_dataset_path  # noqa: PLC0415

    try:
        # A field of a VBT volume (issue #83): the header it is read with.
        base, field = split_ref(request.path)
        path = _normalize_path(base)
        ref = vbt_dataset_path(path, field) if field is not None else None
        if ref is None and not _is_fits(path):
            raise ValueError("FITS file not found.")
        # A .fits.gz: the headers off the start of the gzip stream (#57).
        # astropy would decompress the whole stream to skip the image's data,
        # 4.5 s for a 2 GB cube, and the file browser waits for this preview.
        hdus = None
        if ref is None and compression_of(path) == "gzip":
            hdus = await asyncio.to_thread(gzip_image_headers, path)
        if hdus is not None:
            cards = _preview_cards(hdus)
        else:
            with open_fits(ref or str(path), memmap=True) as hdul:
                cards = _preview_cards(hdul)
    except PermissionError:
        # Same answer as a genuinely absent file: echoing the refusal reason
        # would tell the caller that the path exists but is out of bounds.
        return FileHeaderResponse(valid=False, error="FITS file not found.", cards=[])
    except Exception as exc:
        return FileHeaderResponse(valid=False, error=str(exc), cards=[])
    return FileHeaderResponse(valid=True, error="", cards=cards)


class FilePreviewRequest(BaseModel):
    path: str = Field(min_length=1)
    #: Bytes of the file's content read, and of text returned; capped at 65536.
    max_bytes: int = Field(DEFAULT_MAX_BYTES, ge=1)
    #: Lines of text returned; capped at 400.
    max_lines: int = Field(DEFAULT_MAX_LINES, ge=1)


class FilePreviewResponse(BaseModel):
    path: str
    size_bytes: int
    kind: Literal["fits", "text", "vbt", "hdf5", "compressed", "binary", "directory"]
    #: A short label: "FITS image 700×700×158", "Comma-separated table, 5 columns".
    format: str
    text: str
    #: True when the file holds more than ``text`` shows.
    truncated: bool


def _may_read(path: Path) -> bool:
    try:
        _normalize_path(str(path))
    except PermissionError:
        return False
    return True


@router.post("/v1/files/preview", response_model=FilePreviewResponse, tags=["files"])
async def preview_file(request: FilePreviewRequest, _: None = _auth) -> FilePreviewResponse:
    """What the file browser shows for any file (issue #159): a FITS header,
    the first lines of a text file, a VBT's ``.head``, an HDF5 tree, or a type
    and a hex dump. Only the start of the file is read (``app.file_preview``),
    so it is as quick through a remote backend as on this machine.

    Unlike the header route this answers with HTTP status codes: 403 for a
    path outside the allowed roots (whether or not it exists, so the refusal
    names nothing), 404 for a missing one, 422 for bad bounds.
    """
    if "\x00" in request.path:
        raise HTTPException(status_code=422, detail="The path contains a null byte.")
    try:
        path = _normalize_path(request.path)
    except PermissionError:
        raise HTTPException(status_code=403, detail="This path is not accessible.")
    except ValueError:
        raise HTTPException(status_code=422, detail="This path is not valid.")
    if not path.exists():
        raise HTTPException(status_code=404, detail="File not found.")
    max_bytes, max_lines = clamp_bounds(request.max_bytes, request.max_lines)
    try:
        result = await asyncio.to_thread(
            preview_path, path, max_bytes, max_lines,
            allow=_may_read, hide_hidden=hide_hidden_entries())
    except PermissionError:
        raise HTTPException(status_code=403, detail="The file cannot be read.")
    except FileNotFoundError:
        raise HTTPException(status_code=404, detail="File not found.")
    except OSError as exc:
        logger.warning("[preview] %s: %s", path, exc)
        raise HTTPException(status_code=500, detail=f"The file cannot be read: {exc.strerror or exc}")
    return FilePreviewResponse(path=request.path, **result)


@router.post("/v1/files/classify", response_model=FileClassifyResponse, tags=["files"])
async def classify_file(request: FileClassifyRequest, _: None = _auth) -> FileClassifyResponse:
    """What is this file? — so the client can offer one *Open…* instead of three.

    The answer is derived from the file itself (magic bytes, extension, at most
    one FITS header); nothing is opened, converted or registered. ``opener``
    names the route that should receive the path, and ``confident`` is False
    when the client should ask the user rather than pick.
    """
    from ..classify import classify_path  # noqa: PLC0415
    from ..volume_source import split_ref, vbt_dataset_path  # noqa: PLC0415

    # A field of a VBT volume (a Recent entry, a Session Data row) opens as the
    # cube it was (issue #83).
    base, field = split_ref(request.path)
    if field is not None:
        try:
            ok = vbt_dataset_path(_normalize_path(base), field) is not None
        except Exception:
            ok = False
        if not ok:
            return FileClassifyResponse(valid=False, error="File not found.")
        return FileClassifyResponse(valid=True, error="", path=request.path, kind="cube",
                                    opener="dataset", confident=True,
                                    detail="A field of a VisIVO Binary Table volume.")
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
