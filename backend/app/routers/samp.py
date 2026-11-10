"""
routers/samp.py – REST endpoints for the VisIVO SAMP bridge.

These routes allow the Qt client (and any REST caller) to:
  • POST /v1/samp/send      – Enqueue a SAMP message to be sent by the Qt client
  • POST /v1/samp/receive   – Qt client forwards an inbound SAMP message here
  • GET  /v1/samp/pending   – Qt client polls for outbound messages to forward to Hub
  • POST /v1/samp/pending/ack – Qt client confirms the ones it sent (lease mode)
  • GET  /v1/samp/status    – Connection/inbox status
  • GET  /v1/samp/clients   – Hub client list (reported by Qt client)
  • POST /v1/samp/hub-status – Qt client reports its Hub connection state
  • POST /v1/samp/connect   – Connect/disconnect backend direct bridge (local only)
  • GET  /v1/samp/files/{token} – Serve a temporary file for Hub-based URL sharing
"""

from __future__ import annotations

import asyncio
import logging
import mimetypes
import time
import urllib.parse
from io import BytesIO
from pathlib import Path
from typing import Any

from fastapi import APIRouter, Depends, Header, HTTPException, Path as FPath, Query
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel
import pandas as pd
from astropy.table import Table

from ..auth import require_operator, verify_token
from ..samp_bridge import (
    MTYPE_COORD_POINT_SKY,
    MTYPE_IMAGE_LOAD_FITS,
    MTYPE_TABLE_LOAD_FITS,
    MTYPE_TABLE_LOAD_VOTABLE,
    SAMP,
    SUPPORTED_RECEIVE_MTYPES,
    SUPPORTED_SEND_MTYPES,
    SAMPMessage,
    store_samp_file,
    get_samp_file_path,
    store_samp_file_from_path,
)

logger = logging.getLogger("visivo.samp.router")
router = APIRouter(tags=["samp"])

# ── Pydantic models ───────────────────────────────────────────────────────────


class SAMPSendRequest(BaseModel):
    """Request to send a SAMP message (backend→Hub via Qt client)."""
    mtype: str
    params: dict[str, Any]
    target_id: str | None = None   # None → broadcast to all


class SAMPSendResponse(BaseModel):
    valid: bool
    error: str = ""
    job_id: str = ""
    sent_direct: bool = False      # True if sent via direct astropy connection
    # deliver="return": the message for the caller to send itself (it is then
    # neither queued nor sent here). {"mtype": ..., "params": {...}}
    message: dict[str, Any] | None = None
    rows: int = -1                 # rows of the table sent, when known
    size_bytes: int = 0            # size of the staged file
    subset: bool = False           # send-catalogue: only the row_indices asked for


class SAMPReceiveRequest(BaseModel):
    """Qt client forwarding an inbound Hub message to the backend."""
    mtype: str
    params: dict[str, Any]
    sender_id: str = ""
    msg_id: str = ""


class SAMPReceiveResponse(BaseModel):
    valid: bool
    error: str = ""
    msg_id: str = ""


class SAMPHubStatusReport(BaseModel):
    """Qt client reporting its current Hub connection state."""
    connected: bool
    hub_url: str = ""
    client_id: str = ""
    client_name: str = ""
    registered_clients: list[dict[str, Any]] = []


class SAMPConnectRequest(BaseModel):
    action: str   # "connect" | "disconnect"


class SAMPFitsRequest(BaseModel):
    """Register a local FITS file path for SAMP URL sharing."""
    path: str
    filename: str = ""
    # "queue": send it (direct connection, else the Qt client's outbound queue).
    # "return": hand the message back in the response; the caller sends it,
    # and can then ask the recipients for an answer (a SAMP call).
    deliver: str = "queue"


class SAMPFitsResponse(BaseModel):
    valid: bool
    error: str = ""
    token: str = ""
    url_path: str = ""   # /v1/samp/files/{token} — client appends backend base URL


class SAMPImportUrlRequest(BaseModel):
    url: str
    filename: str = ""


class SAMPUploadFileRequest(BaseModel):
    filename: str
    content_base64: str


class SAMPImportResponse(BaseModel):
    valid: bool
    error: str = ""
    token: str = ""
    backend_path: str = ""
    filename: str = ""


class SAMPCatalogueRequest(BaseModel):
    path: str
    format: str = "csv"
    filename: str = ""
    # The SAMP table-id the client chose for this table. It is sent with the
    # table.load message so that later table.highlight.row / table.select.rowList
    # messages can name the table (SAMP 1.3: by table-id and/or url).
    table_id: str = ""
    deliver: str = "queue"   # as SAMPFitsRequest.deliver
    # Send only these rows (0-based positions in the table as opened, as
    # /v1/catalogue/query reports them in row_index), in this order, as a new
    # VOTable. None: the whole table.
    row_indices: list[int] | None = None


class SAMPPendingAckRequest(BaseModel):
    job_ids: list[str]


# A leased message the client has not acknowledged by then is handed out again.
_PENDING_LEASE_SECONDS = 60.0

# The most rows a subset may name: the list travels as JSON.
_MAX_SUBSET_ROWS = 5_000_000


# ── Endpoints ─────────────────────────────────────────────────────────────────

_auth = Depends(verify_token)
# SAMP is a machine-wide hub shared by every caller; with per-user tokens
# configured only the operator may drive it.
_operator = Depends(require_operator)


@router.post("/v1/samp/send", response_model=SAMPSendResponse, tags=["samp"])
async def samp_send(
    request: SAMPSendRequest,
    _: str = _operator,
) -> SAMPSendResponse:
    """Enqueue a SAMP message for dispatch.

    The message is first attempted via the direct astropy Hub connection (if the
    backend is local and VISIVO_SAMP_DIRECT=1).  If that is unavailable the
    message is placed on the outbound queue and the Qt client will forward it to
    the Hub on its next poll of ``GET /v1/samp/pending``.
    """
    if request.mtype not in SUPPORTED_SEND_MTYPES:
        return SAMPSendResponse(
            valid=False,
            error=f"Unsupported MType '{request.mtype}'. "
                  f"Supported: {sorted(SUPPORTED_SEND_MTYPES)}",
        )

    # Try direct send first (backend local, VISIVO_SAMP_DIRECT=1)
    sent = SAMP.send_direct(request.mtype, request.params, request.target_id)
    if sent:
        return SAMPSendResponse(valid=True, sent_direct=True, job_id="direct")

    # Fall back: enqueue for Qt client
    job_id = SAMP.enqueue_for_qt_client(request.mtype, request.params, request.target_id)
    return SAMPSendResponse(valid=True, job_id=job_id)


@router.post("/v1/samp/receive", response_model=SAMPReceiveResponse, tags=["samp"])
async def samp_receive(
    request: SAMPReceiveRequest,
    _: str = _operator,
) -> SAMPReceiveResponse:
    """Accept an inbound SAMP message forwarded by the Qt client.

    The Qt client calls this endpoint whenever it receives a SAMP notification
    or call from the Hub.  The backend stores the message in its inbox for
    consumption by downstream handlers (e.g., automatically loading a dataset).
    """
    if request.mtype not in SUPPORTED_RECEIVE_MTYPES:
        # Unknown mtype: store anyway (may be useful for logging/debug)
        logger.debug("[samp] received unsupported mtype=%s, storing anyway", request.mtype)

    msg = SAMPMessage(
        mtype=request.mtype,
        params=request.params,
        sender_id=request.sender_id,
        msg_id=request.msg_id,
    )
    SAMP.inbox.put(msg)
    return SAMPReceiveResponse(valid=True, msg_id=msg.msg_id)


@router.get("/v1/samp/pending", tags=["samp"])
async def samp_pending(
    lease: bool = Query(False),
    _: str = _operator,
) -> dict:
    """The outbound SAMP message queue, polled by the desktop client.

    With ``lease=true`` the messages stay queued: they are held for the caller
    for ``lease_seconds`` and removed only by ``POST /v1/samp/pending/ack``, so
    a client that fails between this answer and the send gets them again.
    Without it the queue is drained as it is answered (older clients).
    """
    if lease:
        batch = SAMP.lease_pending(_PENDING_LEASE_SECONDS)
        return {"count": len(batch), "messages": batch, "lease_seconds": _PENDING_LEASE_SECONDS}
    batch = SAMP.drain_pending()
    return {"count": len(batch), "messages": batch}


@router.post("/v1/samp/pending/ack", tags=["samp"])
async def samp_pending_ack(
    request: SAMPPendingAckRequest,
    _: str = _operator,
) -> dict:
    """Remove the leased messages the desktop client has sent (by ``job_id``)."""
    return {"acked": SAMP.ack_pending(request.job_ids)}


@router.get("/v1/samp/inbox", tags=["samp"])
async def samp_inbox(
    limit: int = Query(32, ge=1, le=256),
    drain: bool = Query(False),
    _: str = _operator,
) -> dict:
    """Inspect (or drain) the inbound SAMP message inbox.

    Set ``drain=true`` to consume and remove the returned messages.
    """
    if drain:
        messages = SAMP.inbox.drain(limit)
    else:
        messages = SAMP.inbox.peek(limit)
    return {
        "count": len(messages),
        "inbox_remaining": SAMP.inbox.size(),
        "messages": [m.to_dict() for m in messages],
    }


@router.get("/v1/samp/status", tags=["samp"])
async def samp_status(_: str = _operator) -> dict:
    """Return the current SAMP bridge status."""
    return SAMP.hub_status()


@router.post("/v1/samp/hub-status", tags=["samp"])
async def samp_hub_status_report(
    report: SAMPHubStatusReport,
    _: str = _operator,
) -> dict:
    """Qt client reports its current Hub connection state.

    Call this whenever the Hub connection state changes (connect, disconnect,
    client list update) so that ``GET /v1/samp/status`` reflects reality.
    """
    SAMP.update_hub_status(report.model_dump())
    logger.info(
        "[samp] Qt hub status: connected=%s client_id=%s n_peers=%d",
        report.connected,
        report.client_id,
        len(report.registered_clients),
    )
    return {"ok": True}


@router.post("/v1/samp/connect", tags=["samp"])
async def samp_connect(
    request: SAMPConnectRequest,
    _: str = _operator,
) -> dict:
    """Connect or disconnect the backend's direct astropy Hub connection.

    Only effective when the backend runs on the same machine as the Hub
    and ``VISIVO_SAMP_DIRECT=1`` is set.
    """
    if request.action == "connect":
        ok = SAMP.connect()
        return {"ok": ok, "connected": SAMP.is_connected}
    elif request.action == "disconnect":
        SAMP.disconnect()
        return {"ok": True, "connected": False}
    raise HTTPException(400, f"Unknown action '{request.action}'. Use 'connect' or 'disconnect'.")


@router.post("/v1/samp/files/register", response_model=SAMPFitsResponse, tags=["samp"])
async def samp_register_file(
    request: SAMPFitsRequest,
    _: str = _operator,
) -> SAMPFitsResponse:
    """Register an existing backend file path for SAMP URL sharing.

    Returns a token that can be used to build the SAMP-accessible URL:
        ``{backend_base_url}/v1/samp/files/{token}``

    The file is copied to a temporary store and served for up to
    ``VISIVO_SAMP_FILE_TTL`` seconds (default 1 hour).
    """
    from pathlib import Path as _Path
    src = _Path(request.path)
    if not src.exists() or not src.is_file():
        return SAMPFitsResponse(valid=False, error=f"File not found: {request.path}")
    try:
        fname = request.filename or src.name
        token = store_samp_file_from_path(src, fname)
    except Exception as exc:
        return SAMPFitsResponse(valid=False, error=str(exc))
    url_path = f"/v1/samp/files/{token}"
    return SAMPFitsResponse(valid=True, token=token, url_path=url_path)


@router.post("/v1/samp/import-url", response_model=SAMPImportResponse, tags=["samp"])
async def samp_import_url(
    request: SAMPImportUrlRequest,
    _: str = _operator,
) -> SAMPImportResponse:
    """Download a remote URL into the backend SAMP temp store.

    The URL comes from the caller, so it goes through the guarded fetch: the
    SSRF check on the URL and on every redirect. A SAMP hub's own URLs
    (``http://127.0.0.1:…``) are therefore refused here; the desktop client
    downloads those itself and stages them with ``/v1/samp/upload-file``.
    Trusted hosts can be listed in ``VISIVO_SAMP_ALLOWED_HOSTS``.
    """
    from ..safe_fetch import safe_urlopen  # noqa: PLC0415

    def fetch() -> bytes:
        with safe_urlopen(request.url, allowlist_env="VISIVO_SAMP_ALLOWED_HOSTS",
                          timeout=30) as handle:
            return handle.read()

    try:
        # Off the event loop: a slow archive must not stall every other route.
        data = await asyncio.to_thread(fetch)
        parsed = urllib.parse.urlparse(request.url)
        filename = request.filename or Path(parsed.path).name or "samp-import.bin"
        token = store_samp_file(data, filename)
        path = get_samp_file_path(token)
        if path is None:
            raise RuntimeError("Imported file is unavailable after staging.")
        return SAMPImportResponse(
            valid=True,
            token=token,
            backend_path=str(path),
            filename=filename,
        )
    except Exception as exc:
        return SAMPImportResponse(valid=False, error=str(exc))


@router.post("/v1/samp/upload-file", response_model=SAMPImportResponse, tags=["samp"])
async def samp_upload_file(
    request: SAMPUploadFileRequest,
    _: str = _operator,
) -> SAMPImportResponse:
    """Stage a client-local file on the backend SAMP temp store."""
    try:
        import base64

        payload = base64.b64decode(request.content_base64.encode("ascii"))
        token = store_samp_file(payload, request.filename)
        path = get_samp_file_path(token)
        if path is None:
            raise RuntimeError("Uploaded file is unavailable after staging.")
        return SAMPImportResponse(
            valid=True,
            token=token,
            backend_path=str(path),
            filename=request.filename,
        )
    except Exception as exc:
        return SAMPImportResponse(valid=False, error=str(exc))


@router.get("/v1/samp/files/{token}", tags=["samp"])
async def samp_serve_file(
    token: str = FPath(..., description="File token returned by /v1/samp/files/register"),
) -> FileResponse:
    """Serve a temporary SAMP file by its token.

    This endpoint is intentionally unauthenticated so that TOPCAT, Aladin and
    other VO tools can fetch the file directly using the URL embedded in SAMP
    messages.  Files are auto-cleaned after ``VISIVO_SAMP_FILE_TTL`` seconds.
    """
    path = get_samp_file_path(token)
    if path is None:
        raise HTTPException(404, f"SAMP file token '{token}' not found or expired.")
    media_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
    # Strip the token prefix from the download filename
    orig_name = path.name[len(token) + 1:]
    return FileResponse(
        path=str(path),
        media_type=media_type,
        filename=orig_name,
    )


def _dispatch(mtype: str, params: dict[str, Any], target_id: str | None, deliver: str,
              rows: int = -1, size_bytes: int = 0) -> SAMPSendResponse:
    """Send a staged file's message, or return it for the caller to send."""
    if deliver.strip().lower() == "return":
        return SAMPSendResponse(valid=True, message={"mtype": mtype, "params": params},
                                rows=rows, size_bytes=size_bytes)
    sent = SAMP.send_direct(mtype, params, target_id)
    if sent:
        return SAMPSendResponse(valid=True, sent_direct=True, job_id="direct",
                                rows=rows, size_bytes=size_bytes)
    job_id = SAMP.enqueue_for_qt_client(mtype, params, target_id)
    return SAMPSendResponse(valid=True, job_id=job_id, rows=rows, size_bytes=size_bytes)


def _staged_size(token: str) -> int:
    path = get_samp_file_path(token)
    try:
        return path.stat().st_size if path is not None else 0
    except OSError:
        return 0


# ── Convenience: build a standard image.load.fits send from a dataset path ───

@router.post("/v1/samp/send-fits", response_model=SAMPSendResponse, tags=["samp"])
async def samp_send_fits(
    request: SAMPFitsRequest,
    target_id: str | None = Query(None),
    _: str = _operator,
) -> SAMPSendResponse:
    """Register a FITS file and immediately enqueue an image.load.fits message.

    Convenience wrapper that combines file registration and message dispatch.
    """
    from pathlib import Path as _Path
    src = _Path(request.path)
    if not src.exists() or not src.is_file():
        return SAMPSendResponse(valid=False, error=f"File not found: {request.path}")
    try:
        fname = request.filename or src.name
        # A copy of a large FITS takes seconds: off the event loop, which
        # serves /v1/health and the client's other requests meanwhile.
        token = await asyncio.to_thread(store_samp_file_from_path, src, fname)
    except Exception as exc:
        return SAMPSendResponse(valid=False, error=str(exc))

    # The URL will be resolved by the Qt client using its known backend base URL.
    # We embed a special placeholder that the Qt client expands.
    url_path = f"/v1/samp/files/{token}"
    params = {
        "url": f"__BACKEND_BASE_URL__{url_path}",
        "name": fname,
        "samp.name": fname,
    }
    return _dispatch(MTYPE_IMAGE_LOAD_FITS, params, target_id, request.deliver,
                     size_bytes=_staged_size(token))


def _vbt_as_opened(src: Path, session_id: str | None) -> str:
    """``src`` (a VBT point table) with the byte order the session's dataset
    of it was opened in (issue #165); as it is when the session has none."""
    from ..sessions import REGISTRY  # noqa: PLC0415
    from ..vbt_byte_order_store import data_key  # noqa: PLC0415
    from ..volume_source import with_order  # noqa: PLC0415

    session = REGISTRY.peek(session_id) if session_id else None
    if session is None:
        return str(src)
    key = data_key(src)
    for entry in list(session.datasets.values()):
        if (isinstance(entry, dict) and entry.get("format") == "vbt" and entry.get("byte_order")
                and entry.get("path") and data_key(str(entry["path"])) == key):
            return with_order(src, entry["byte_order"])
    return str(src)


@router.post("/v1/samp/send-catalogue", response_model=SAMPSendResponse, tags=["samp"])
async def samp_send_catalogue(
    request: SAMPCatalogueRequest,
    target_id: str | None = Query(None),
    x_visivo_session: str | None = Header(None),
    _: str = _operator,
) -> SAMPSendResponse:
    """Stage a catalogue and enqueue (or return) a SAMP table.load message.

    Staging reads and converts the whole table, which for millions of rows
    takes tens of seconds: it runs off the event loop. A table converted to
    VOTable is written in the BINARY2 serialization, several times smaller and
    faster to parse than TABLEDATA XML for every SAMP table tool.
    """
    src = Path(request.path)
    if not src.exists() or not src.is_file():
        return SAMPSendResponse(valid=False, error=f"File not found: {request.path}")

    fmt = request.format.strip().lower()
    subset = request.row_indices
    # A VBT is read in the byte order the sender's dataset was opened in
    # (issue #165), so that a row means the values the window shows.
    read_src: str | Path = src
    if fmt == "vbt":
        read_src = _vbt_as_opened(src, x_visivo_session)
    if subset is not None:
        if not subset:
            return SAMPSendResponse(valid=False, error="No rows to send.")
        if len(subset) > _MAX_SUBSET_ROWS:
            return SAMPSendResponse(
                valid=False,
                error=f"Too many rows for a subset ({len(subset)}; at most {_MAX_SUBSET_ROWS}): "
                      f"send the whole table instead.")

    def write_votable(table: Table, filename: str) -> str:
        buffer = BytesIO()
        table.write(buffer, format="votable", tabledata_format="binary2")
        return store_samp_file(buffer.getvalue(), filename)

    def as_votable(dataframe: pd.DataFrame, filename: str) -> str:
        return write_votable(Table.from_pandas(dataframe), filename)

    def stage_subset() -> tuple[str, str, str, int]:
        """The rows asked for, as a new VOTable, read as /v1/catalogue/open
        reads the table, so that a row index means the same row."""
        import numpy as np  # noqa: PLC0415

        rows = np.asarray(subset, dtype=np.int64)
        if fmt in {"votable", "vot", "xml", "fits", "fit", "fits-table"}:
            # Read as a Table, not through pandas: the columns keep their
            # units, UCDs and descriptions. Same row order as to_pandas().
            table = Table.read(src, format="fits" if fmt.startswith("fit") else "votable")
            count = len(table)
        else:
            from ..dependencies import _catalogue_load_dataframe  # noqa: PLC0415

            dataframe = _catalogue_load_dataframe(Path(read_src), fmt)
            table = None
            count = len(dataframe)
        if rows.min() < 0 or rows.max() >= count:
            raise ValueError(f"Row indices must lie in 0..{count - 1}; the table has {count} rows.")
        stem = Path(request.filename).stem if request.filename else src.stem
        filename = f"{stem}.vot"
        if table is not None:
            token = write_votable(table[rows], filename)
        else:
            # A big-endian VBT column cannot be indexed by pandas (issue #165).
            from ..dependencies import _take_rows  # noqa: PLC0415
            token = as_votable(_take_rows(dataframe, rows).reset_index(drop=True), filename)
        return token, filename, MTYPE_TABLE_LOAD_VOTABLE, int(len(rows))

    def stage() -> tuple[str, str, str, int]:
        """(token, filename, mtype, rows); rows is -1 when not read."""
        if subset is not None:
            return stage_subset()
        if fmt == "csv":
            # Any delimited text the catalogue viewer opened: comma, tab or
            # semicolon (issue #155).
            from ..text_table import read_delimited_table  # noqa: PLC0415

            dataframe = read_delimited_table(src)
            filename = request.filename or f"{src.stem}.vot"
            return as_votable(dataframe, filename), filename, MTYPE_TABLE_LOAD_VOTABLE, len(dataframe)
        if fmt in {"votable", "vot", "xml"}:
            filename = request.filename or src.name
            return (store_samp_file_from_path(src, filename), filename,
                    MTYPE_TABLE_LOAD_VOTABLE, -1)
        if fmt in {"fits", "fit", "fits-table"}:
            filename = request.filename or src.name
            return store_samp_file_from_path(src, filename), filename, MTYPE_TABLE_LOAD_FITS, -1
        if fmt in {"ipac", "tbl", "speck"}:
            # No native SAMP mtype for these — load via the catalogue reader and
            # re-emit as VOTable (same path as CSV).
            from ..dependencies import _catalogue_load_dataframe  # noqa: PLC0415

            dataframe = _catalogue_load_dataframe(src, fmt)
            # Force a .vot name — the bytes are VOTable now, not the source format.
            stem = Path(request.filename).stem if request.filename else src.stem
            filename = f"{stem}.vot"
            return as_votable(dataframe, filename), filename, MTYPE_TABLE_LOAD_VOTABLE, len(dataframe)
        raise ValueError(f"Unsupported catalogue format: {request.format}")

    try:
        token, filename, mtype, rows = await asyncio.to_thread(stage)
    except Exception as exc:
        return SAMPSendResponse(valid=False, error=str(exc))

    url_path = f"/v1/samp/files/{token}"
    params = {
        "url": f"__BACKEND_BASE_URL__{url_path}",
        "name": filename,
        "samp.name": filename,
    }
    if request.table_id.strip():
        params["table-id"] = request.table_id.strip()
    response = _dispatch(mtype, params, target_id, request.deliver, rows=rows,
                         size_bytes=_staged_size(token))
    # Says the rows were honoured: a backend without row_indices sends the
    # whole table, and the client must not take that for the subset.
    response.subset = subset is not None
    return response
