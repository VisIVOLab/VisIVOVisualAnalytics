"""
samp_bridge.py – SAMP integration bridge for VisIVO backend.

This module provides a thread-safe singleton (SAMPBridge) that:

  1. Connects to a SAMP Hub running on the local machine (if available) using
     astropy.samp.SAMPIntegratedClient.  The Hub is typically provided by TOPCAT,
     Aladin or a standalone ``samp_hub`` process.

  2. Provides a temporary HTTP file server so that FITS/VOTable result files
     produced by the backend can be shared with other VO tools via URL even when
     the backend runs on a remote HPC node (in that case the Qt client must ensure
     its local machine can reach the backend HTTP port).

  3. Queues inbound SAMP messages received from other tools (forwarded by the Qt
     client via POST /v1/samp/receive) so that async handlers can process them.

Architecture note
-----------------
The SAMP Hub always runs on the user's desktop.  The Qt client is the *primary*
SAMP client that speaks XML-RPC with the Hub.  The backend acts as a *bridge*:

  Desktop                               Server (may be remote)
  ──────────────────────────────────    ──────────────────────────────
  Hub (TOPCAT/Aladin) ←─XML-RPC──► Qt Client ←─HTTP/REST──► Backend
                                         │ (forwarded SAMP msgs)
                                         └── POST /v1/samp/receive
                                             GET  /v1/samp/files/{token}
                                             POST /v1/samp/send

When the backend runs locally the bridge can ALSO connect directly to the Hub
as an additional client (useful for server-side scripts or automated workflows).
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import logging
import os
import shutil
import tempfile
import threading
import time
import uuid
from pathlib import Path
from typing import Any

logger = logging.getLogger("visivo.samp")

# ── MType constants ───────────────────────────────────────────────────────────

MTYPE_IMAGE_LOAD_FITS    = "image.load.fits"
MTYPE_TABLE_LOAD_VOTABLE = "table.load.votable"
MTYPE_TABLE_LOAD_FITS    = "table.load.fits"
MTYPE_COORD_POINT_SKY    = "coord.pointAt.sky"
MTYPE_HUB_SHUTDOWN       = "samp.hub.event.shutdown"
MTYPE_HUB_REGISTER       = "samp.hub.event.register"
MTYPE_HUB_UNREGISTER     = "samp.hub.event.unregister"
MTYPE_VISIVO_DATASET     = "visivo.dataset.open"   # VisIVO-specific

SUPPORTED_SEND_MTYPES = {
    MTYPE_IMAGE_LOAD_FITS,
    MTYPE_TABLE_LOAD_VOTABLE,
    MTYPE_TABLE_LOAD_FITS,
    MTYPE_COORD_POINT_SKY,
    MTYPE_VISIVO_DATASET,
}

SUPPORTED_RECEIVE_MTYPES = {
    MTYPE_IMAGE_LOAD_FITS,
    MTYPE_TABLE_LOAD_VOTABLE,
    MTYPE_TABLE_LOAD_FITS,
    MTYPE_COORD_POINT_SKY,
    MTYPE_HUB_SHUTDOWN,
    MTYPE_HUB_REGISTER,
    MTYPE_HUB_UNREGISTER,
}

# ── Temporary file store ──────────────────────────────────────────────────────

_SAMP_FILES_DIR = Path(tempfile.gettempdir()) / "visivo_samp_files"
_SAMP_FILES_DIR.mkdir(parents=True, exist_ok=True)

_FILE_TTL_SECONDS = int(os.environ.get("VISIVO_SAMP_FILE_TTL", "3600"))


def _cleanup_old_samp_files() -> None:
    """Remove files older than _FILE_TTL_SECONDS from the SAMP temp directory."""
    cutoff = time.time() - _FILE_TTL_SECONDS
    for f in _SAMP_FILES_DIR.iterdir():
        try:
            if f.stat().st_mtime < cutoff:
                f.unlink(missing_ok=True)
        except OSError:
            pass


def store_samp_file(data: bytes, filename: str) -> str:
    """Write *data* to a temporary file and return its access token (UUID4).

    The caller should construct the final URL as:
        ``{backend_base_url}/v1/samp/files/{token}``
    """
    _cleanup_old_samp_files()
    token = uuid.uuid4().hex
    dest = _SAMP_FILES_DIR / f"{token}_{filename}"
    dest.write_bytes(data)
    logger.debug("[samp] stored file token=%s path=%s bytes=%d", token, dest, len(data))
    return token


def store_samp_file_from_path(source_path: str | Path, filename: str | None = None) -> str:
    """Copy an existing file into the SAMP temp store and return its token."""
    _cleanup_old_samp_files()
    source = Path(source_path)
    token = uuid.uuid4().hex
    dest_name = filename or source.name
    dest = _SAMP_FILES_DIR / f"{token}_{dest_name}"
    shutil.copy2(source, dest)
    logger.debug("[samp] stored file token=%s src=%s", token, source)
    return token


def get_samp_file_path(token: str) -> Path | None:
    """Return the Path for *token*, or None if not found / expired."""
    for f in _SAMP_FILES_DIR.iterdir():
        if f.name.startswith(token + "_"):
            return f
    return None


# ── Message inbox (inbound SAMP messages forwarded by Qt client) ──────────────

class SAMPMessage:
    """An inbound SAMP message received by the Qt client and forwarded here."""

    __slots__ = ("msg_id", "mtype", "params", "sender_id", "received_at")

    def __init__(
        self,
        mtype: str,
        params: dict[str, Any],
        sender_id: str = "",
        msg_id: str = "",
    ) -> None:
        self.msg_id = msg_id or uuid.uuid4().hex
        self.mtype = mtype
        self.params = params
        self.sender_id = sender_id
        self.received_at = time.time()

    def to_dict(self) -> dict[str, Any]:
        return {
            "msg_id": self.msg_id,
            "mtype": self.mtype,
            "params": self.params,
            "sender_id": self.sender_id,
            "received_at": self.received_at,
        }


class SAMPInbox:
    """Thread-safe bounded queue for inbound SAMP messages."""

    def __init__(self, max_size: int = 256) -> None:
        self._lock = threading.Lock()
        self._messages: list[SAMPMessage] = []
        self._max_size = max_size

    def put(self, message: SAMPMessage) -> None:
        with self._lock:
            if len(self._messages) >= self._max_size:
                self._messages.pop(0)   # evict oldest
            self._messages.append(message)
        logger.info("[samp] inbox: mtype=%s sender=%s", message.mtype, message.sender_id)

    def drain(self, limit: int = 64) -> list[SAMPMessage]:
        with self._lock:
            batch, self._messages = self._messages[:limit], self._messages[limit:]
        return batch

    def peek(self, limit: int = 32) -> list[SAMPMessage]:
        with self._lock:
            return list(self._messages[:limit])

    def size(self) -> int:
        with self._lock:
            return len(self._messages)


# ── Optional SAMP direct connection ───────────────────────────────────────────
# astropy.samp was deprecated in astropy 8.0 (moved to pyvo). Prefer pyvo.samp,
# falling back to astropy.samp for older environments where pyvo is absent.

_SAMP_AVAILABLE = False
try:
    from pyvo.samp import SAMPIntegratedClient as _SAMPIntegratedClient  # type: ignore
    _SAMP_AVAILABLE = True
except ImportError:
    try:
        from astropy.samp import SAMPIntegratedClient as _SAMPIntegratedClient  # type: ignore
        _SAMP_AVAILABLE = True
    except ImportError:
        pass


class SAMPBridge:
    """Singleton that manages the backend's optional direct SAMP Hub connection.

    When ``VISIVO_SAMP_DIRECT=1`` is set AND a Hub is reachable on the local
    machine, the bridge registers itself as a SAMP client.  This enables
    server-side scripts to send messages directly without going through the Qt
    client (useful for batch pipelines).

    When the backend runs on a remote HPC node the direct connection will not
    work (the Hub is on the user's desktop), but the REST bridge endpoints
    still function correctly.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._client: Any = None          # SAMPIntegratedClient instance or None
        self._connected = False
        self._direct_enabled = os.environ.get("VISIVO_SAMP_DIRECT", "0") == "1"
        self.inbox = SAMPInbox()
        self._pending_send: list[dict[str, Any]] = []  # outbound queue for Qt client
        self._pending_lock = threading.Lock()
        self._qt_hub_status: dict[str, Any] = {}

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    def connect(self) -> bool:
        """Attempt a direct Hub connection. Returns True if connected."""
        if not _SAMP_AVAILABLE:
            logger.warning("[samp] SAMP client (pyvo/astropy) not available; direct mode disabled.")
            return False
        if not self._direct_enabled:
            logger.debug("[samp] direct mode disabled (VISIVO_SAMP_DIRECT != 1).")
            return False
        with self._lock:
            if self._connected:
                return True
            try:
                client = _SAMPIntegratedClient(
                    name="VisIVO Backend",
                    description="VisIVO astrophysical data visualization backend",
                )
                client.connect()
                client.bind_receive_notification("*", self._on_notification)
                client.bind_receive_call("*", self._on_call)
                self._client = client
                self._connected = True
                logger.info("[samp] connected to Hub as '%s'", client.get_public_id())
                return True
            except Exception as exc:
                logger.info("[samp] could not connect to Hub directly: %s", exc)
                self._connected = False
                return False

    def disconnect(self) -> None:
        """Gracefully disconnect from the Hub if connected."""
        with self._lock:
            if self._client and self._connected:
                try:
                    self._client.disconnect()
                    logger.info("[samp] disconnected from Hub.")
                except Exception:
                    pass
            self._client = None
            self._connected = False

    @property
    def is_connected(self) -> bool:
        with self._lock:
            return self._connected

    # ── Inbound (direct mode) ─────────────────────────────────────────────────

    def _on_notification(self, private_key: str, sender_id: str, message: dict) -> None:  # noqa: ARG002
        mtype = message.get("samp.mtype", "")
        params = message.get("samp.params", {})
        self.inbox.put(SAMPMessage(mtype, params, sender_id))

    def _on_call(self, private_key: str, sender_id: str, msg_id: str, message: dict) -> None:  # noqa: ARG002
        mtype = message.get("samp.mtype", "")
        params = message.get("samp.params", {})
        self.inbox.put(SAMPMessage(mtype, params, sender_id, msg_id))
        # Return a basic OK reply (fire-and-forget; caller handles reply via Hub)
        if self._client:
            try:
                self._client.reply(msg_id, {"samp.status": "samp.ok", "samp.result": {}})
            except Exception:
                pass

    # ── Outbound ──────────────────────────────────────────────────────────────

    def send_direct(
        self,
        mtype: str,
        params: dict[str, Any],
        target_id: str | None = None,
    ) -> bool:
        """Send a message via direct Hub connection. Returns True if sent."""
        if not self.is_connected or not self._client:
            return False
        try:
            message = {"samp.mtype": mtype, "samp.params": params}
            if target_id:
                self._client.notify(target_id, message)
            else:
                self._client.notify_all(message)
            logger.info("[samp] sent direct mtype=%s target=%s", mtype, target_id or "ALL")
            return True
        except Exception as exc:
            logger.warning("[samp] send_direct failed: %s", exc)
            return False

    def enqueue_for_qt_client(
        self,
        mtype: str,
        params: dict[str, Any],
        target_id: str | None = None,
    ) -> str:
        """Enqueue a message to be forwarded to the Hub by the Qt client.

        The Qt client polls GET /v1/samp/pending and picks up queued messages.
        Returns a job_id for tracking.
        """
        job_id = uuid.uuid4().hex
        with self._pending_lock:
            if len(self._pending_send) > 512:
                self._pending_send.pop(0)
            self._pending_send.append({
                "job_id": job_id,
                "mtype": mtype,
                "params": params,
                "target_id": target_id,
                "queued_at": time.time(),
            })
        logger.info("[samp] enqueued for Qt client: mtype=%s job_id=%s", mtype, job_id)
        return job_id

    def drain_pending(self) -> list[dict[str, Any]]:
        """Drain the outbound queue (called by Qt client via GET /v1/samp/pending)."""
        with self._pending_lock:
            batch, self._pending_send = list(self._pending_send), []
        return batch

    # ── Hub status (proxied from Qt client reports) ───────────────────────────

    def update_hub_status(self, status: dict[str, Any]) -> None:
        """Called when the Qt client reports its Hub connection status."""
        with self._lock:
            self._qt_hub_status = dict(status)

    def hub_status(self) -> dict[str, Any]:
        with self._lock:
            direct = self._connected
            return {
                "direct_connected": direct,
                "direct_client_id": (
                    self._client.get_public_id() if direct and self._client else None
                ),
                "qt_client_status": dict(self._qt_hub_status),
                "inbox_size": self.inbox.size(),
                "pending_send": len(self._pending_send),
            }


# Module-level singleton.
SAMP: SAMPBridge = SAMPBridge()
