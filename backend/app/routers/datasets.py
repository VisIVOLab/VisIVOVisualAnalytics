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

router = APIRouter(tags=['datasets'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

def _build_open_response(dataset_id: str, session_id: str, kind: str,
                          geometry: dict[str, Any]) -> OpenDatasetResponse:
    """Shared response-builder for /open and /open_url so the OpenDatasetResponse
    schema stays in one place."""
    return OpenDatasetResponse(
        valid=True,
        error="",
        dataset_id=dataset_id,
        session_id=session_id,
        kind=kind,
        active_axes=int(geometry.get("active_axes", 0)),
        width=geometry["width"],
        height=geometry["height"],
        depth=geometry["depth"],
        degenerate_axes_summary=str(geometry.get("degenerate_axes_summary", "")),
        wcs_status=str(geometry.get("wcs_status", "ok")),
        wcs_warning_message=str(geometry.get("wcs_warning_message", "")),
        wcs_sanitized_axes=list(geometry.get("wcs_sanitized_axes", [])),
        spectral_axis_type=str(geometry.get("spectral_axis_type", "")),
        spectral_axis_unit=str(geometry.get("spectral_axis_unit", "")),
        spacing=geometry["spacing"],
        origin=geometry["origin"],
        ctype=geometry["ctype"],
        cunit=geometry["cunit"],
        crval=geometry["crval"],
        crpix=geometry["crpix"],
        cdelt=geometry["cdelt"],
        beam_major=geometry.get("beam_major"),
        beam_minor=geometry.get("beam_minor"),
        beam_pa=geometry.get("beam_pa"),
    )


@router.post("/v1/datasets/open", response_model=OpenDatasetResponse, tags=["datasets"])
async def open_dataset(
    request: OpenDatasetRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> OpenDatasetResponse:
    path = _normalize_path(request.path)
    if not _is_supported_dataset(path):
        return OpenDatasetResponse(
            valid=False,
            error="Unsupported file type. Expected FITS (.fits, .fit) or "
                  "HDF5 (.h5, .hdf5, .bfdata).")
    try:
        kind, geometry = _fits_metadata(path)
    except Exception as exc:
        return OpenDatasetResponse(valid=False, error=str(exc))

    dataset_id = f"ds_{uuid.uuid4().hex[:12]}"
    # For HDF5 inputs, geometry["resolved_path"] points at the sidecar FITS
    # produced by _fits_metadata; pixel-data readers downstream operate on
    # FITS, so we register the resolved path with the session.
    registered_path = str(geometry.get("resolved_path") or path)
    session.datasets[dataset_id] = {"path": registered_path, "kind": kind, **geometry}
    logger.info(
        "[open] session=%s dataset_id=%s kind=%s path=%s",
        session.session_id, dataset_id, kind, path,
    )
    return _build_open_response(dataset_id, session.session_id, kind, geometry)


# ── Open from URL (SKAVA discovery → access) ────────────────────────────────

class OpenDatasetUrlRequest(BaseModel):
    url: str            # http:// or https:// pointing at a FITS / HDF5 file
    obs_id: str = ""    # SKAVA obs_id (used as filename hint), optional
    bearer_token: str = ""  # forwarded as Authorization to the remote node


@router.post("/v1/datasets/open_url",
             response_model=OpenDatasetResponse, tags=["datasets"])
async def open_dataset_from_url(
    request: OpenDatasetUrlRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> OpenDatasetResponse:
    """Download a FITS / HDF5 file from an HTTP(S) URL and open it as a
    session dataset.

    Pipeline:
      1. Cache by URL hash under ``<workspace_exports>/skava_cache/`` so
         the same access_url resolved twice does not re-download.
      2. Stream the bytes to the cache (chunked) — works for multi-GB files
         without buffering everything in memory.
      3. Delegate to the same metadata extraction (`_fits_metadata`) used
         by /v1/datasets/open so HDF5 / PSRFITS sidecar conversion and the
         dynspec classifier kick in automatically.

    Used by the desktop SKAVA discovery flow: after the user picks a
    dataset, the client calls `/datalink/{obs_id}` on SKAVA, extracts
    `primary_access.access_url`, then POSTs that URL here. Returns the same
    `OpenDatasetResponse` as the local-path open, so the rest of the
    desktop flow is unchanged."""
    import hashlib  # noqa: PLC0415

    raw_url = (request.url or "").strip()
    if not (raw_url.startswith("http://") or raw_url.startswith("https://")):
        return OpenDatasetResponse(
            valid=False,
            error="open_url requires an http:// or https:// URL.")

    # Cache layout: <exports>/skava_cache/<sha256>.<ext>
    cache_dir = get_exports_dir() / "skava_cache"
    cache_dir.mkdir(parents=True, exist_ok=True)
    url_hash = hashlib.sha256(raw_url.encode("utf-8")).hexdigest()[:16]
    # Best-effort filename: prefer the SKAVA obs_id when supplied (gives
    # informative `Recent Datasets` entries), otherwise fall back to the
    # URL basename.
    if request.obs_id:
        basename_hint = f"{request.obs_id}__{url_hash}"
    else:
        from urllib.parse import urlparse, unquote, parse_qs  # noqa: PLC0415
        parsed = urlparse(raw_url)
        # Workspace-exports-style URLs put the filename in a query param.
        qs = parse_qs(parsed.query)
        if "filename" in qs and qs["filename"]:
            basename_hint = f"{unquote(qs['filename'][0])}.{url_hash}"
        else:
            basename_hint = (Path(unquote(parsed.path)).name or "remote.fits")
            basename_hint = f"{basename_hint}.{url_hash}"

    # Preserve a sensible extension so downstream classifiers (FITS vs
    # HDF5 sidecar logic) trigger correctly on the cached filename.
    lower = raw_url.lower()
    if any(lower.endswith(suffix) for suffix in (".h5", ".hdf5", ".bfdata")):
        ext = Path(raw_url).suffix.lower()
    elif lower.endswith(".fits.gz"):
        ext = ".fits.gz"
    elif lower.endswith(".fits") or lower.endswith(".fit"):
        ext = ".fits"
    else:
        ext = ".fits"  # safe default; FITS reader will reject if not actually FITS
    cached_path = cache_dir / f"{basename_hint}{ext}"

    if not cached_path.exists():
        logger.info(
            "[open_url] downloading url=%s -> cache=%s", raw_url, cached_path,
        )
        try:
            req = _urllib_request.Request(raw_url, method="GET")
            if request.bearer_token:
                req.add_header("Authorization", f"Bearer {request.bearer_token}")
            with _urllib_request.urlopen(req, timeout=600) as resp, \
                 open(cached_path, "wb") as out:
                while True:
                    chunk = resp.read(8 * 1024 * 1024)  # 8 MiB
                    if not chunk:
                        break
                    out.write(chunk)
        except _urllib_error.HTTPError as exc:
            return OpenDatasetResponse(
                valid=False,
                error=f"Remote returned HTTP {exc.code}: {exc.reason}")
        except Exception as exc:
            # Clean up partial file so retries succeed.
            try:
                cached_path.unlink()
            except FileNotFoundError:
                pass
            return OpenDatasetResponse(
                valid=False, error=f"Download failed: {exc}")
    else:
        logger.info("[open_url] cache hit url=%s -> %s", raw_url, cached_path)

    # Delegate to the standard FITS / HDF5 / PSRFITS classifier.
    try:
        kind, geometry = _fits_metadata(cached_path)
    except Exception as exc:
        return OpenDatasetResponse(
            valid=False,
            error=f"Cached file could not be classified: {exc}")

    dataset_id = f"ds_{uuid.uuid4().hex[:12]}"
    registered_path = str(geometry.get("resolved_path") or cached_path)
    session.datasets[dataset_id] = {
        "path": registered_path, "kind": kind, **geometry,
        "source_url": raw_url,
    }
    logger.info(
        "[open_url] session=%s dataset_id=%s kind=%s cache=%s",
        session.session_id, dataset_id, kind, cached_path,
    )
    return _build_open_response(dataset_id, session.session_id, kind, geometry)
