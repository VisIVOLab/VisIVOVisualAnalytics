from fastapi import APIRouter, Depends, HTTPException, Query
from fastapi.responses import StreamingResponse
from pydantic import BaseModel
from typing import Any
import io
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
from ..skava_client import (
    SkavaResolveError,
    access_url_to_local_path,
)
import asyncio

router = APIRouter(tags=['datasets'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

def _probe_server_side_rendering() -> bool:
    """Lazy probe for the SSR capability flag on the open dataset response.

    Wrapped because ``app.render_session`` imports VTK + Pillow at
    module-load time, which we don't want on CPU-only / Web-only
    backends. ``ImportError`` here means the backend was built without
    EGL — perfectly normal, just means no SSR.
    """
    try:
        from ..render_session import server_side_rendering_available
        return bool(server_side_rendering_available())
    except ImportError:
        return False


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
        bunit=str(geometry.get("bunit", "")),
        ctype=geometry["ctype"],
        cunit=geometry["cunit"],
        crval=geometry["crval"],
        crpix=geometry["crpix"],
        cdelt=geometry["cdelt"],
        beam_major=geometry.get("beam_major"),
        beam_minor=geometry.get("beam_minor"),
        beam_pa=geometry.get("beam_pa"),
        file_size_bytes=int(geometry.get("file_size_bytes", 0)),
        full_res_threshold_bytes=int(
            geometry.get("full_res_threshold_bytes", 4 * 1024 ** 3)
        ),
        recommended_load_mode=str(
            geometry.get("recommended_load_mode", "auto_full")
        ),
        # Backend-wide capability flag — the same for every dataset the
        # backend serves (depends on the SIF build, not on the cube).
        # Imported lazily so a backend without the EGL build doesn't
        # have to install the render module.
        server_side_rendering_available=_probe_server_side_rendering(),
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
    # Anti-SSRF validation. Unlike VLKB, open_url legitimately reaches federated
    # SKAVA data-mesh nodes that may sit on private networks, so private-address
    # blocking is OPT-IN (VISIVO_OPENURL_BLOCK_PRIVATE=1) to avoid breaking those
    # deployments; an optional VISIVO_OPENURL_ALLOWED_HOSTS allowlist can pin
    # trusted hosts. With neither set, behaviour is unchanged (http/https only).
    try:
        assert_safe_fetch_url(
            raw_url,
            allowlist_env="VISIVO_OPENURL_ALLOWED_HOSTS",
            block_private=os.environ.get("VISIVO_OPENURL_BLOCK_PRIVATE", "").lower() in ("1", "true", "yes"),
        )
    except ValueError as exc:
        return OpenDatasetResponse(valid=False, error=str(exc))

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


# ── Open from SKAVA discovery (compute-next-to-data) ───────────────────────


class OpenSkavaDatasetRequest(BaseModel):
    """Open a dataset identified by a SKAVA obs_id.

    The desktop client has already queried SKAVA to find this dataset's
    access URL — it passes both fields straight to us, saving the
    backend a redundant round-trip back to SKAVA. obs_id stays in the
    payload for session-side bookkeeping and log correlation, but the
    actual file lookup uses access_url only.

    This is the "compute next to data" entry point: no file transfer
    happens at any layer. The file is opened in place from the
    backend's own filesystem, on the assumption that the backend was
    deployed co-located with its data on the SRC node.

    Path safety: when ``VISIVO_DATA_ROOT`` is set on the backend, the
    file:// access URL must resolve inside that root. The backend
    rejects anything outside.
    """

    obs_id: str
    access_url: str   # the file:// URL the client received from SKAVA


@router.post("/v1/datasets/open_skava",
             response_model=OpenDatasetResponse, tags=["datasets"])
async def open_dataset_from_skava(
    request: OpenSkavaDatasetRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> OpenDatasetResponse:
    """Open a dataset whose access URL the client got from SKAVA.

    Pipeline:
      1. Parse the supplied ``access_url`` — must be ``file://``.
      2. Apply the ``VISIVO_DATA_ROOT`` path jail if configured.
      3. Delegate to the same metadata extraction (`_fits_metadata`) used
         by /v1/datasets/open, so HDF5 / PSRFITS sidecar conversion and
         the dynspec classifier kick in automatically.

    Returns the same OpenDatasetResponse as the local-path open, so
    every downstream endpoint (moments, channel, region, dynspec, …)
    works without modification.

    Configuration (all optional):
      ``VISIVO_DATA_ROOT`` — restrict opens to a single directory subtree.
    """
    obs_id = (request.obs_id or "").strip()
    if not obs_id:
        return OpenDatasetResponse(
            valid=False, error="obs_id is required.")

    try:
        path = access_url_to_local_path(request.access_url)
    except SkavaResolveError as exc:
        return OpenDatasetResponse(valid=False, error=str(exc))

    path = _normalize_path(path)
    if not _is_supported_dataset(path):
        return OpenDatasetResponse(
            valid=False,
            error=f"Resolved file {path!r} is not a supported dataset type "
                   "(expected FITS / HDF5).")

    try:
        kind, geometry = _fits_metadata(path)
    except Exception as exc:
        return OpenDatasetResponse(
            valid=False,
            error=f"Failed to open resolved file {path!r}: {exc}")

    dataset_id = f"ds_{uuid.uuid4().hex[:12]}"
    registered_path = str(geometry.get("resolved_path") or path)
    session.datasets[dataset_id] = {
        "path": registered_path, "kind": kind, **geometry,
        "obs_id": obs_id,
        "skava_access_url": request.access_url,
    }
    logger.info(
        "[open_skava] session=%s dataset_id=%s kind=%s obs_id=%s path=%s",
        session.session_id, dataset_id, kind, obs_id, path,
    )
    return _build_open_response(dataset_id, session.session_id, kind, geometry)


# ── SODA cutout (compute-next-to-data, byte-level subset) ──────────────────


class CutoutRequest(BaseModel):
    """Server-side SODA cutout request.

    Mirrors ``open_skava``: the caller (a desktop client, or SKAVA acting
    as a SODA service on the client's behalf) supplies the obs_id and the
    ``file://`` access_url it already resolved via SKAVA's DataLink. The
    backend opens the file in place and returns only the subset — no full
    file transfer at any layer.

    SODA parameters:
      * ``pos``  — ``CIRCLE <ra> <dec> <radius>`` / ``RANGE ...`` (deg)
      * ``band`` — ``<min> <max>`` in metres (wavelength)
      * ``time`` — accepted for symmetry; not yet a sliceable axis
    """

    obs_id: str
    access_url: str
    pos: str | None = None
    band: str | None = None
    time: str | None = None
    responseformat: str = "application/fits"
    # When true, the cutout is written into the backend's Workspace Exports
    # directory and the response is JSON ``{valid, path, …}`` pointing at the
    # backend-side file — the desktop then opens it in place (compute next to
    # data, same pattern as the VLKB cutout). When false (default), the FITS
    # is streamed back in the response body (used by SKAVA's /soda/execute).
    save_to_workspace: bool = False


@router.post("/v1/datasets/cutout", tags=["datasets"])
async def cutout_dataset(
    request: CutoutRequest,
    _: None = _auth,
):
    """Produce a FITS cutout of a node-local dataset.

    This is the byte-level half of "compute next to data": SKAVA's
    ``/soda/execute`` delegates here so the spatial/spectral subset is
    computed on the SRC node that already holds the file, and only the
    (small) cutout transits the network. The VisIVO desktop's "SODA subset"
    action calls it with ``save_to_workspace=true`` to keep the cutout on the
    backend and open it in place.

    Path safety reuses the ``open_skava`` resolver — ``file://`` only,
    confined to ``VISIVO_DATA_ROOT`` when set.
    """
    from ..soda_cutout import CutoutError, make_cutout  # noqa: PLC0415

    obs_id = (request.obs_id or "").strip()
    if not obs_id:
        raise HTTPException(status_code=400, detail="obs_id is required.")

    try:
        path = access_url_to_local_path(request.access_url)
    except SkavaResolveError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    path = _normalize_path(path)
    if not _is_supported_dataset(path):
        raise HTTPException(
            status_code=415,
            detail=f"Resolved file {str(path)!r} is not a supported FITS dataset.")

    try:
        result = await asyncio.to_thread(
            make_cutout,
            str(path),
            pos=request.pos,
            band=request.band,
            time=request.time,
            filename_hint=obs_id or "dataset",
        )
    except CutoutError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    applied = ",".join(k for k, v in result.applied.items() if v is True)
    logger.info(
        "[cutout] obs_id=%s path=%s applied=%s bytes=%d save_to_workspace=%s",
        obs_id, path, result.applied, len(result.fits_bytes), request.save_to_workspace,
    )

    if request.save_to_workspace:
        # Persist on the backend and return a path the desktop can open in
        # place. A uuid suffix avoids collisions between concurrent subsets
        # of the same dataset.
        out_dir = get_exports_dir() / "soda_cutouts"
        out_dir.mkdir(parents=True, exist_ok=True)
        safe_obs = "".join(c if c.isalnum() or c in "-_." else "_" for c in obs_id)
        out_path = out_dir / f"{safe_obs}_{uuid.uuid4().hex[:8]}_cutout.fits"
        out_path.write_bytes(result.fits_bytes)
        return {
            "valid": True,
            "error": "",
            "obs_id": obs_id,
            "path": str(out_path),
            "applied": applied,
        }

    headers = {
        "Content-Disposition": f'attachment; filename="{result.filename}"',
        "X-Soda-Applied": applied,
    }
    return StreamingResponse(
        io.BytesIO(result.fits_bytes),
        media_type="application/fits",
        headers=headers,
    )
