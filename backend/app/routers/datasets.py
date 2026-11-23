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
import urllib.error as _urllib_error
import urllib.request as _urllib_request
import pandas as pd
from astropy.io import fits
from ..schemas import *
from ..dependencies import *
from ..auth import verify_token
from ..sessions import Session, get_session, REGISTRY
from ..tasks import TASKS
from ..product_cache import PRODUCT_CACHE
from ..fits_dataset import ScientificFitsDataset
from ..fits_truncation import FitsTruncatedError, check_not_truncated
from ..compressed_fits import CompressedFitsError
from ..skava_client import (
    SkavaResolveError,
    access_url_to_local_path,
)
import asyncio
import functools

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


def _refuse_truncated(exc: FitsTruncatedError, suffix: str = "") -> HTTPException:
    """A truncated FITS (issue #118) is refused with 422 and the message as the
    detail, on every open route: unlike the other open failures, which answer
    200 with ``valid=false``, it is a property of the file that no retry
    fixes, and a 4xx keeps it out of any caller that only checks the status."""
    return HTTPException(status_code=422, detail=str(exc) + suffix)


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
        rest_freq_hz=geometry.get("rest_freq_hz"),
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
        beam_per_channel=bool(geometry.get("beam_per_channel", False)),
        beam_source=str(geometry.get("beam_source", "")),
        intensity_unit=str(geometry.get("intensity_unit", "")),
        intensity_unit_note=str(geometry.get("intensity_unit_note", "")),
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
        field_name=str(geometry.get("field_name", "")),
        fields=[DatasetField(**f) for f in geometry.get("fields", [])],
    )


@router.delete("/v1/datasets/{dataset_id}", tags=["datasets"])
async def close_dataset(
    dataset_id: str,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> dict:
    """Close a dataset of this session (any kind: an image, a cube, a
    catalogue, a VBT): its entry, and what it holds (a catalogue's table, a
    VBT's memory map), are dropped. The client sends it when the last window
    showing the dataset closes. ``closed`` is false when the session had no
    such dataset (already closed, or expired): not an error."""
    entry = session.datasets.pop(dataset_id, None)
    if entry is not None:
        logger.info("[close] session=%s dataset_id=%s kind=%s",
                    session.session_id[:8] + "…", dataset_id, entry.get("kind", ""))
        # A snapshot's decompressed HDF5 pieces go with its last dataset.
        await asyncio.to_thread(session.release_particle_cache, entry)
    return {"valid": True, "error": "", "closed": entry is not None}


@router.post("/v1/datasets/open", response_model=OpenDatasetResponse, tags=["datasets"])
async def open_dataset(
    request: OpenDatasetRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> OpenDatasetResponse:
    from ..pluto_source import pluto_dataset_path  # noqa: PLC0415
    from ..volume_source import field_data_file, split_ref, vbt_dataset_path, with_order  # noqa: PLC0415

    # A field of a VBT volume: the file checked as any path, the field kept.
    base, field = split_ref(request.path)
    path = _normalize_path(base)
    vbt = vbt_dataset_path(path, field)
    # A PLUTO output (issue #315), by a data file, its .out or a variable
    # reference: the variable's reference, read in place. Its byte order is
    # the one PLUTO wrote in the .out.
    pluto = None
    if vbt is None:
        pluto_base = field_data_file(request.path)
        # Off the event loop: grid.out of a 2048^3 run is 6144 lines.
        pluto = await asyncio.to_thread(pluto_dataset_path, str(_normalize_path(pluto_base))
                                        + request.path[len(pluto_base):])
        if pluto is not None:
            if request.byte_order is not None:
                return OpenDatasetResponse(
                    valid=False, error="byte_order does not apply to a PLUTO output: it is "
                                       "read in the byte order its .out file gives.")
            path, field = Path(pluto), None
    orders: dict = {}
    if vbt is not None:
        path = Path(vbt)
        # Before any value is read (issue #165): the byte order asked for,
        # else a check of the one the header declares.
        from ..vbt_byte_order import open_with_byte_order  # noqa: PLC0415
        try:
            orders = await asyncio.to_thread(open_with_byte_order, vbt, request.byte_order,
                                             f"session {session.session_id[:8]}")
        except (OSError, ValueError) as exc:
            return OpenDatasetResponse(valid=False, error=str(exc))
        if orders["changed"]:
            from ..vbt_byte_order import close_render_sessions_read_otherwise  # noqa: PLC0415
            await close_render_sessions_read_otherwise(vbt, orders["byte_order"])
        # The dataset is read in this order for as long as it is open, by every
        # reader and process, whatever the store says later.
        path = Path(with_order(vbt, orders["byte_order"]))
    elif request.byte_order is not None:
        return OpenDatasetResponse(
            valid=False, error=("byte_order applies to a VisIVO Binary Table volume here; "
                                "a VBT table of points opens with /v1/catalogue/open "
                                "(format vbt)."))
    if vbt is None and pluto is None and (field is not None
                                          or not _is_supported_dataset(path)):
        return OpenDatasetResponse(
            valid=False,
            error="Unsupported file type. Expected FITS (.fits, .fit, also "
                  "compressed: .fits.gz, .fz) or HDF5 (.h5, .hdf5, .bfdata).")
    try:
        kind, geometry = await asyncio.to_thread(_fits_metadata, path)
    except FitsTruncatedError as exc:
        raise _refuse_truncated(exc) from exc
    except Exception as exc:
        return OpenDatasetResponse(valid=False, error=str(exc))

    dataset_id = f"ds_{uuid.uuid4().hex[:12]}"
    # For HDF5 inputs, geometry["resolved_path"] points at the sidecar FITS
    # produced by _fits_metadata; pixel-data readers downstream operate on
    # FITS, so we register the resolved path with the session.
    registered_path = str(geometry.get("resolved_path") or path)
    session.datasets[dataset_id] = {"path": registered_path, "kind": kind, **geometry}
    if orders:
        # Provenance: the order the values were read in, and the header's.
        session.datasets[dataset_id]["byte_order"] = orders["byte_order"]
        session.datasets[dataset_id]["byte_order_declared"] = orders["byte_order_declared"]
    logger.info(
        "[open] session=%s dataset_id=%s kind=%s path=%s",
        session.session_id[:8] + "…", dataset_id, kind, path,
    )
    response = _build_open_response(dataset_id, session.session_id, kind, geometry)
    if orders:
        response.byte_order = orders["byte_order"]
        response.byte_order_declared = orders["byte_order_declared"]
        if orders["byte_order_warning"]:
            response.byte_order_warning = VbtByteOrderWarning(**orders["byte_order_warning"])
    return response


# ── Pixel → sky (SAMP coord.pointAt.sky, issue #130) ────────────────────────

class PixelToSkyRequest(BaseModel):
    dataset_id: str
    # 0-based pixel along the viewer's axes (the dataset's non-degenerate
    # axes, as /v1/datasets/open reports them): x, y and, for a cube, the
    # channel.
    x: float
    y: float
    z: float = 0.0


class PixelToSkyResponse(BaseModel):
    valid: bool
    error: str = ""
    ra_deg: float = 0.0        # ICRS
    dec_deg: float = 0.0
    frame: str = ""            # the image's own celestial frame (icrs, fk5, galactic, …)
    lon_deg: float = 0.0       # the position in that frame
    lat_deg: float = 0.0


@functools.lru_cache(maxsize=32)
def _celestial_wcs_for(path: str, mtime_ns: int):
    """(WCS, viewer axis → FITS axis) of a dataset, read once per file version."""
    dataset = ScientificFitsDataset(path)
    return dataset.wcs, dict(dataset.client_axis_to_fits_axis)


def _pixel_to_sky(path: str, x: float, y: float, z: float) -> PixelToSkyResponse:
    from ..fits_dataset import pixel_to_icrs  # noqa: PLC0415

    wcs, axis_map = _celestial_wcs_for(path, Path(path).stat().st_mtime_ns)
    if wcs is None or not wcs.has_celestial:
        return PixelToSkyResponse(valid=False, error="The dataset has no sky coordinates.")
    sky = pixel_to_icrs(wcs, axis_map, x, y, z)
    if sky is None:
        return PixelToSkyResponse(valid=False, error="That pixel is off the projection.")
    return PixelToSkyResponse(valid=True, **sky)


@router.post("/v1/datasets/pixel_to_sky", response_model=PixelToSkyResponse, tags=["datasets"])
async def pixel_to_sky(
    request: PixelToSkyRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> PixelToSkyResponse:
    """The ICRS position of a pixel of an open image or cube, from the file's
    full WCS (projection, rotation, distortions, frame): what the desktop
    sends as SAMP coord.pointAt.sky for a clicked pixel."""
    entry = _require_dataset(session, request.dataset_id)
    if entry.get("kind") not in ("image", "cube"):
        return PixelToSkyResponse(valid=False, error="Only images and cubes have pixels on the sky.")
    try:
        return await asyncio.to_thread(_pixel_to_sky, entry["path"], request.x, request.y,
                                       request.z)
    except Exception as exc:
        return PixelToSkyResponse(valid=False, error=str(exc))


# ── Typed beam for a header without one (issue #416) ────────────────────────

class DatasetBeamRequest(BaseModel):
    dataset_id: str
    major_arcsec: float | None = None
    minor_arcsec: float | None = None   # default: major_arcsec (circular beam)
    pa_deg: float | None = None         # default: 0
    clear: bool = False


class DatasetBeamResponse(BaseModel):
    valid: bool
    error: str = ""
    beam_major: float | None = None     # degrees, as in the open response
    beam_minor: float | None = None
    beam_pa: float | None = None
    beam_source: str = ""
    note: str = ""


def _set_dataset_beam(path: str, request: DatasetBeamRequest, by: str) -> DatasetBeamResponse:
    from .. import beam_override_store  # noqa: PLC0415
    current = ScientificFitsDataset(path)
    if current.beam_source == "not_applicable":
        return DatasetBeamResponse(
            valid=False, error="This dataset has no celestial axes (a simulation field or "
            "other non-sky data): a restoring beam does not apply to it.",
            beam_source="not_applicable")
    if current.beam_source == "header":
        return DatasetBeamResponse(
            valid=False, error="The header already has a beam (BMAJ/BMIN or a BEAMS "
            "table); a typed beam is only used when it has none.",
            beam_major=current.beam.major, beam_minor=current.beam.minor,
            beam_pa=current.beam.pa, beam_source="header")
    if request.clear:
        beam_override_store.set_beam(path, None, by)
    else:
        if request.major_arcsec is None:
            return DatasetBeamResponse(valid=False, error="major_arcsec is required.")
        beam = beam_override_store.validate(
            request.major_arcsec, request.minor_arcsec, request.pa_deg)
        beam_override_store.set_beam(path, beam, by)
    after = ScientificFitsDataset(path)
    note = ""
    if after.beam_header_partial and after.beam_source == "user":
        note = ("The header has an incomplete beam (BMAJ or BMIN missing, zero or "
                "not finite); the typed beam replaces it.")
    return DatasetBeamResponse(valid=True, beam_major=after.beam.major,
                               beam_minor=after.beam.minor, beam_pa=after.beam.pa,
                               beam_source=after.beam_source, note=note)


@router.post("/v1/datasets/beam", response_model=DatasetBeamResponse, tags=["datasets"])
async def set_dataset_beam(
    request: DatasetBeamRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> DatasetBeamResponse:
    """Type the restoring beam of an image or cube whose header has none
    (``major_arcsec``, ``minor_arcsec``, ``pa_deg``), or forget it with
    ``clear``. The beam is kept with the file (``app.beam_override_store``),
    so every later request on it, in any session, uses it for per-beam
    quantities and errors as if the header had BMAJ/BMIN/BPA; reopening the
    file reports it with ``beam_source = "user"``. A complete header beam
    is never overridden; an incomplete one is, and ``note`` says so. The beam
    is tied to the file as it is: rewriting the file (a header edit included)
    retires it."""
    from ..volume_source import is_field_ref  # noqa: PLC0415
    entry = _require_dataset(session, request.dataset_id)
    if entry.get("kind") not in ("image", "cube") or is_field_ref(entry["path"]):
        return DatasetBeamResponse(valid=False,
                                   error="A beam can be set only on a FITS image or cube.")
    try:
        return await asyncio.to_thread(_set_dataset_beam, entry["path"], request,
                                       f"session {session.session_id[:8]}")
    except (ValueError, OSError, FitsTruncatedError) as exc:
        return DatasetBeamResponse(valid=False, error=str(exc))


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
    elif lower.endswith(".fz"):
        ext = ".fits.fz"
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
        kind, geometry = await asyncio.to_thread(_fits_metadata, cached_path)
    except FitsTruncatedError as exc:
        # The cached copy is ours: drop it, so opening the URL again fetches
        # it again rather than finding the same short file in the cache.
        try:
            cached_path.unlink()
        except OSError:
            pass
        raise _refuse_truncated(
            exc, " The downloaded copy was removed; opening the URL again downloads it again.") from exc
    except CompressedFitsError as exc:
        # A damaged gzip or tile-compressed download: drop it too.
        try:
            cached_path.unlink()
        except OSError:
            pass
        return OpenDatasetResponse(
            valid=False,
            error=f"{exc} The downloaded copy was removed; opening the URL again "
                  "downloads it again.")
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
        session.session_id[:8] + "…", dataset_id, kind, cached_path,
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
        kind, geometry = await asyncio.to_thread(_fits_metadata, path)
    except FitsTruncatedError as exc:
        raise _refuse_truncated(exc) from exc
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
        session.session_id[:8] + "…", dataset_id, kind, obs_id, path,
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
        # Headers and os.stat only: a truncated cube is refused before the
        # cutout memory-maps it (issue #118).
        await asyncio.to_thread(check_not_truncated, str(path))
    except FitsTruncatedError as exc:
        raise _refuse_truncated(exc) from exc
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
