"""Server-side rendering endpoints (Phase 1 + Phase 2 wired together).

Endpoints
---------

* ``POST   /v1/render/create_session`` — start a session for a dataset
  (above the session cap, optionally wait in a queue: 202 with a place)
* ``DELETE /v1/render/queue/{queue_id}`` — give up a queue place
* ``DELETE /v1/render/{session_id}``   — explicit teardown
* ``GET    /v1/render/{session_id}/levels`` — starting threshold and data range
* ``WS     /v1/render/{session_id}/ws`` — bidirectional render channel
* ``GET    /v1/render/diagnostics``    — fleet snapshot for ops

WebSocket protocol
------------------

* Client → server: JSON text frames. Each is one command. Examples:
  ``{"type": "rotate", "d_azimuth": 5.0, "d_elevation": -1.2}``
  ``{"type": "zoom",   "factor": 1.1}``
  ``{"type": "request_frame"}``
  ``{"type": "keepalive"}``
* Server → client: binary frames are JPEG bytes (one frame per message).
  Text frames are diagnostic / error JSON.
* Headset (issue #169): ``xr_pose`` switches the session to stereo frames for
  that pose, ``xr_stop`` back to the mono view. A stereo frame is one binary
  message starting with ``VXR1`` (see ``render_session/_stereo.py``).

Frame production rule
---------------------

We do NOT render a frame for every command. The server reads commands
in a tight loop, mutates :class:`RenderSession.view`, and renders only:

  1. when the client sends ``{"type": "request_frame"}`` (= "I need an
     update on screen NOW") — this is the dominant case during drag
     (the client throttles itself to 30/60 Hz and asks for a frame
     each tick)
  2. when a command arrives AFTER an idle period (configurable) so the
     viewer auto-updates without explicit request

This decouples the client's command rate from the actual render
cadence and lets the server skip frames cleanly when the GPU is the
bottleneck (e.g. a TB-scale cube where each frame takes 60 ms; the
client can drag at 120 Hz, server still renders 16 FPS, the user gets
a steady stream — no backpressure).
"""
from __future__ import annotations

import asyncio
import collections
import json
import logging
import time
from typing import Annotated, Any

from fastapi import APIRouter, Depends, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
from pydantic import BaseModel

from ..auth import verify_token
from ..render_manager import (
    RenderQueued,
    RenderTicketGone,
    get_render_manager,
)
from ..render_session import server_side_rendering_available
from ..render_session._stereo import XRFrame, parse_pose
from ..render_session import _stereo_webrtc as xr_rtc
from ..sessions import Session as VisivoSession, get_session

router = APIRouter(prefix="/v1/render", tags=["render"])
logger = logging.getLogger("visivo.render")

_auth = Depends(verify_token)


async def _owned_render_session(session_id: str, token: str | None):
    """Look up a render session, enforcing that it belongs to ``token``.

    Returns None when it is missing OR owned by a different token, so callers
    answer 404 either way and can't be used to probe another user's session.
    """
    import secrets as _sec

    from ..auth import multi_user_mode as _multi
    sess = await get_render_manager().get_session(session_id)
    if sess is None:
        return None
    owner = getattr(sess, "owner_token", None)
    if owner is None:
        # Unowned (legacy/internal): shareable only on a single-user backend.
        return None if _multi() else sess
    if token is None or not _sec.compare_digest(owner, token):
        return None
    return sess


# ── REST: session lifecycle ──────────────────────────────────────────────────



def point_table_spec(entry: dict) -> str | None:
    """What a point renderer opens for a dataset (app/point_table.py), None
    for one that is not a table of points. Opened by the VBT route, or as a
    catalogue (the 3-D catalogue viewer): a VBT by its header, any other
    catalogue by its files and coordinates."""
    # A VBT carries the byte order it was opened in (issue #165): the render
    # process reads that, whatever the store says by then.
    from ..volume_source import with_order  # noqa: PLC0415
    if entry.get("kind") == "vbt" and entry.get("header", {}).get("kind") == "point":
        head = str(entry["head_path"])
        return with_order(head, entry["byte_order"]) if entry.get("byte_order") else head
    if entry.get("kind") == "catalogue" and entry.get("format") == "vbt":
        path = str(entry["path"])
        return with_order(path, entry["byte_order"]) if entry.get("byte_order") else path
    if entry.get("kind") == "catalogue":
        coords = entry.get("coordinate_fields") or {}
        if coords.get("system") not in ("sky", "cartesian"):
            raise HTTPException(422, "Drawing a catalogue in 3-D needs RA/Dec or X/Y/Z columns.")
        return json.dumps({"paths": entry.get("paths") or [str(entry["path"])],
                           "format": entry.get("format", "csv"),
                           "coordinates": coords})
    return None

class CreateRenderSessionRequest(BaseModel):
    dataset_id: str
    # Above the session cap: wait in the queue (202 with a place) rather than
    # be refused (503). Off by default, so a client that cannot wait keeps the
    # refusal it understands.
    queue: bool = False
    # A place given by an earlier 202: asking again with it reports the new
    # position and keeps the place; the session starts when its turn comes.
    queue_id: str | None = None


class CreateRenderSessionResponse(BaseModel):
    valid: bool
    error: str = ""
    session_id: str = ""
    initial_frame_jpeg_b64: str = ""
    width: int = 0
    height: int = 0
    # The values the session renders with, for the client to show rather than
    # replace with its own (issue #82); see RenderLevels.
    threshold: float | None = None
    data_min: float | None = None
    data_max: float | None = None
    levels_pending: bool = False


class RenderLevels(BaseModel):
    """The threshold a render session draws with and the cube's data range.

    ``pending`` while a large cube is still streaming into the session: the
    values are null until it is shown and may change until it is complete.
    """
    threshold: float | None = None
    data_min: float | None = None
    data_max: float | None = None
    pending: bool = False


def _levels(sess) -> RenderLevels:
    return RenderLevels(**(sess.levels() or {"pending": True}))


async def _pyramid_preparing(path: str, hdu: int | None) -> JSONResponse | None:
    """202 while the cube's pyramid is being built, an error if its build
    failed, None when the session can start (an in-core cube, or the pyramid
    ready)."""
    from ..pyramid_builds import BuildStatus, ensure_pyramid  # noqa: PLC0415
    from ..render_session._ooc import wants_out_of_core  # noqa: PLC0415
    from ..volume_source import volume_shape  # noqa: PLC0415

    def status() -> BuildStatus | None:
        try:
            # As the render session decides (render_session/_scene.py): the
            # HDU it opens and the last three axes of its header shape.
            full, index = volume_shape(path, hdu)
            shape = full[-3:]
        except Exception as exc:  # noqa: BLE001
            return BuildStatus("failed", 0.0, f"the cube could not be read: {exc}")
        if len(shape) < 3 or not wants_out_of_core(shape[0] * shape[1] * shape[2] * 4):
            return None
        try:
            return ensure_pyramid(path, index)
        except Exception as exc:  # noqa: BLE001 - e.g. a pyramid directory that cannot be written
            return BuildStatus("failed", 0.0, f"the build could not start: {exc}")

    built = await asyncio.to_thread(status)
    if built is None or built.state == "ready":
        return None
    if built.state == "failed":
        raise HTTPException(500, f"Preparing the cube for full resolution failed: {built.error}")
    return JSONResponse(status_code=202, content={
        "valid": False, "preparing": True, "progress": round(built.progress, 3),
        "error": "Preparing the cube for full-resolution rendering on the backend.",
    })


@router.post("/create_session", response_model=CreateRenderSessionResponse, responses={
    202: {"description": "The cube's brick pyramid is being built for out-of-core rendering: "
                         "``{valid: false, preparing: true, progress: 0..1, error}``. Ask again "
                         "in a moment; the session starts once it is ready. Or, with ``queue`` "
                         "or ``queue_id``, every slot is busy and the request waits: "
                         "``{valid: false, queued: true, queue_id, position, queue_length, "
                         "active_sessions, capacity, waited_seconds, wait_timeout_seconds, "
                         "poll_after_seconds, expires_after_seconds, error}`` and a "
                         "``Retry-After`` header; ask again with ``queue_id``."},
    410: {"description": "``queue_id`` is no longer in the queue (not asked for within "
                         "``expires_after_seconds``, cancelled, already used, or another "
                         "token's or dataset's)."},
    503: {"description": "Server-side rendering unavailable, or every slot busy and no "
                         "queue place (not asked for, queue off or full, or waited past "
                         "``wait_timeout_seconds``)."}})
async def create_session(
    request: CreateRenderSessionRequest,
    token: str = _auth,
    session: VisivoSession = Depends(get_session),
) -> CreateRenderSessionResponse:
    """Create a server-side render session for an already-opened dataset.

    The dataset must have been registered with ``/v1/datasets/open*`` —
    we look up its path via the VisIVO session, NOT from the request, so
    the client cannot bypass the data-root path jail.
    """
    if not server_side_rendering_available():
        raise HTTPException(503, "Server-side rendering is not available on this backend.")

    entry = session.datasets.get(request.dataset_id)
    if entry is None:
        raise HTTPException(404, f"Unknown dataset_id {request.dataset_id!r}.")
    # A VBT point table (issue #83): drawn by the point session, from the
    # table where it lies.
    point_head = point_table_spec(entry)
    if entry.get("kind") != "cube" and point_head is None:
        raise HTTPException(422, "Server-side rendering supports cubes and VBT point tables.")
    wait = {} if not (request.queue or request.queue_id) else {
        "queue": True, "ticket_id": request.queue_id}
    if point_head is not None:
        return await _start_session(point_head, None, token,
                                    factory="app.render_session.points:PointRenderSession",
                                    wait=wait)
    # A FITS cube, or a field of a VBT volume read in place (its path a field
    # reference, app/volume_source.py, issue #83).
    path, hdu = str(entry["path"]), None

    # A cube drawn out of core needs its brick pyramid. When it is missing it
    # is built in the background (app/pyramid_builds.py) and this answers 202
    # with the fraction done; the client keeps its preview and asks again.
    preparing = await _pyramid_preparing(path, hdu)
    if preparing is not None:
        return preparing

    return await _start_session(path, hdu, token, wait=wait)


def _queued_response(status: dict) -> JSONResponse:
    """202 for a request waiting for a render slot (issue #4)."""
    if status["position"] == 0:
        message = "A rendering slot is free: the session starts on the next request."
    else:
        message = (f"All {status['capacity']} server-side rendering slots are busy: "
                   f"number {status['position']} of {status['queue_length']} in the queue.")
    return JSONResponse(
        status_code=202,
        content={"valid": False, "queued": True, "error": message, **status},
        headers={"Retry-After": str(max(1, round(status["poll_after_seconds"])))})


async def _start_session(path: str, hdu: int | None, token: str,
                         factory: str | None = None,
                         wait: dict | None = None) -> CreateRenderSessionResponse:
    manager = get_render_manager()
    await manager.start()   # idempotent — reaper coroutine
    try:
        # ``factory`` only for point data: a cube's session is the default;
        # ``wait`` only when the client asked to queue.
        sess = await manager.create_session(dataset_path=path, owner_token=token, hdu=hdu,
                                            **({} if factory is None else {"factory": factory}),
                                            **(wait or {}))
    except RenderQueued as queued:
        return _queued_response(queued.status)
    except RenderTicketGone as gone:
        raise HTTPException(410, f"{gone} Ask again without queue_id.")
    except RuntimeError as e:
        # Capacity reached (no queue place), or the session failed to start.
        raise HTTPException(503, str(e))

    # Render an initial frame so the client has something to show
    # immediately after the WS connects (avoids a black widget while
    # the user finds the mouse).
    import base64
    initial = await sess.render_frame()
    levels = _levels(sess)
    return CreateRenderSessionResponse(
        valid=True,
        session_id=sess.session_id,
        initial_frame_jpeg_b64=base64.b64encode(initial).decode("ascii"),
        width=sess.view.width,
        height=sess.view.height,
        threshold=levels.threshold,
        data_min=levels.data_min,
        data_max=levels.data_max,
        levels_pending=levels.pending,
    )


@router.get("/{session_id}/levels", response_model=RenderLevels)
async def session_levels(session_id: str, token: str = _auth) -> RenderLevels:
    """The session's current threshold and data range; asked again by the
    client while ``pending``, until a cube streaming in is complete."""
    sess = await _owned_render_session(session_id, token)
    if sess is None:
        raise HTTPException(404, "Unknown render session_id.")
    # In its own process, the session answers after the commands sent before
    # this request (a colour change), not with the levels of its last frame.
    if hasattr(sess, "refresh_levels"):
        await sess.refresh_levels()
    return _levels(sess)


@router.get("/{session_id}/camera")
async def session_camera(session_id: str, token: str = _auth) -> dict:
    """The camera of a session's view (points or a cube), after the commands
    sent before this request: a ray-traced still of the view on screen is
    rendered from it (app/routers/particle_render.py, volume_render.py)."""
    sess = await _owned_render_session(session_id, token)
    if sess is None:
        raise HTTPException(404, "Unknown render session_id.")
    if hasattr(sess, "refresh_levels"):
        await sess.refresh_levels()
    camera = (sess.levels() or {}).get("camera")
    if not camera:
        raise HTTPException(409, "This render session has no camera to report.")
    return camera


@router.delete("/{session_id}")
async def close_session(session_id: str, token: str = _auth) -> dict[str, Any]:
    if await _owned_render_session(session_id, token) is None:
        raise HTTPException(404, "Unknown render session_id.")
    closed = await get_render_manager().close_session(session_id)
    if not closed:
        raise HTTPException(404, "Unknown render session_id.")
    return {"closed": True}


@router.delete("/queue/{queue_id}")
async def cancel_queued(queue_id: str, token: str = _auth) -> dict[str, Any]:
    """Give up a place in the rendering queue: the client stopped waiting (the
    view closed, the user went back to local rendering). A place not asked
    for within ``expires_after_seconds`` is dropped anyway."""
    if not await get_render_manager().cancel_ticket(queue_id, token):
        raise HTTPException(404, "Unknown queue_id.")
    return {"cancelled": True}


@router.get("/diagnostics")
async def diagnostics(token: str = _auth) -> dict[str, Any]:
    """Snapshot of the CALLER'S active render sessions — ops dashboards + tests.

    Session ids and dataset paths are per-user data, so the listing is filtered to
    the sessions this token owns (unowned ones stay visible only in single-token
    mode, where there is nothing to isolate)."""
    from ..auth import multi_user_mode as _multi
    snap = await get_render_manager().snapshot_for_owner(token, _multi())
    # Surface which transports this backend can serve so the client
    # picks the best path: ``jpeg-ws`` is always on; the WebRTC stream only
    # when the optional WebRTC build was applied. That stream is H.264
    # (webrtc_session.py): ``h264-webrtc`` names it, and ``h265-webrtc`` is
    # kept for the clients that look for the name it was first given.
    try:
        from ..webrtc_session import webrtc_available
        transports = ["jpeg-ws", "jpeg-ws-push"]
        if webrtc_available():
            transports += ["h264-webrtc", "h265-webrtc"]
    except ImportError:
        transports = ["jpeg-ws", "jpeg-ws-push"]
    snap["transports"] = transports
    return snap


# ── WebRTC: SDP offer/answer signalling ─────────────────────────────────────


class _SdpExchange(BaseModel):
    sdp: str
    type: str


class _IceCandidate(BaseModel):
    """One trickled ICE candidate from the client.

    Mirrors the JS WebRTC ``RTCIceCandidateInit`` payload so any
    web/desktop client can post it verbatim. Either of the two
    identification fields (``sdpMid`` or ``sdpMLineIndex``) is enough
    for aiortc to wire the candidate up to the right media section;
    we forward both when the client has them.
    """
    candidate: str
    sdpMid: str | None = None
    sdpMLineIndex: int | None = None


@router.post("/{session_id}/sdp/offer", response_model=_SdpExchange)
async def webrtc_offer(
    session_id: str,
    offer: _SdpExchange,
    token: str = _auth,
) -> _SdpExchange:
    """Establish a WebRTC peer connection for a render session.

    The client posts its SDP offer; we attach a video track that pulls
    RGB frames from the existing :class:`RenderSession` (no JPEG
    round-trip), NVENC-encode them, and return the SDP answer. Camera
    commands flow back over an ``RTCDataChannel`` the client opens
    before posting the offer — same JSON protocol as the WebSocket
    transport so the desktop client can share serialization code.

    Each call creates a fresh PeerConnection. A session can host
    multiple PCs (e.g. a desktop client + a tablet looking at the same
    cube); the underlying RenderSession serialises them with its
    existing render lock.
    """
    try:
        from ..webrtc_session import WebRTCSession, webrtc_available
    except ImportError:
        raise HTTPException(503, "WebRTC transport not built in this backend.")
    if not webrtc_available():
        raise HTTPException(503, "WebRTC transport disabled (no NVENC).")

    sess = await _owned_render_session(session_id, token)
    if sess is None:
        raise HTTPException(404, "Unknown render session_id.")
    if session_id in _stereo_owners:
        # Its frames are a headset's stereo pair (see _stereo_owners).
        raise HTTPException(409, STEREO_BUSY)

    logger.info(
        "[webrtc] /sdp/offer received for session %s (offer type=%s, %d chars)",
        session_id, offer.type, len(offer.sdp or ""),
    )
    try:
        webrtc = WebRTCSession(sess)
        answer = await webrtc.create_answer(offer.sdp, offer.type)
    except Exception as e:
        # Surface the actual reason to the client — without this the
        # caller sees a bare 500 and has to dig through the backend
        # logs to learn what went wrong. The HTTPException detail is
        # not sensitive: it's the same string that ends up in the log.
        import traceback
        tb = traceback.format_exc()
        logger.error("[webrtc] /sdp/offer failed for %s: %s\n%s",
                     session_id, e, tb)
        raise HTTPException(
            500,
            f"WebRTC negotiation failed: {type(e).__name__}: {e}",
        )
    return _SdpExchange(**answer)


# ── WebRTC: trickle ICE ─────────────────────────────────────────────────────


@router.post("/{session_id}/sdp/ice")
async def webrtc_ice(
    session_id: str,
    cand: _IceCandidate,
    token: str = _auth,
) -> dict[str, Any]:
    """Accept a trickled ICE candidate from the client.

    The client posts its local candidates as they're gathered, rather
    than waiting for the gathering phase to finish before sending the
    SDP offer. Cuts first-frame latency from ~30 s (wait for STUN
    timeout) down to ~2 s (host candidates connect immediately, srflx
    are added as background information).

    The PeerConnection is looked up by ``session_id``; if the
    matching :class:`WebRTCSession` instance has been garbage-collected
    (e.g. the offer was rejected) we 410-Gone the candidate rather
    than wait indefinitely.
    """
    try:
        from ..webrtc_session import get_active_webrtc_session
    except ImportError:
        raise HTTPException(503, "WebRTC transport not built in this backend.")

    # Owner-checked: only the token that created the render session may trickle
    # candidates into its PeerConnection.
    if await _owned_render_session(session_id, token) is None:
        raise HTTPException(404, "Unknown render session_id.")

    webrtc = get_active_webrtc_session(session_id)
    if webrtc is None:
        raise HTTPException(410, "No active WebRTC PeerConnection for this session.")

    try:
        await webrtc.add_ice_candidate(
            candidate=cand.candidate,
            sdp_mid=cand.sdpMid,
            sdp_mline_index=cand.sdpMLineIndex,
        )
    except Exception as e:
        logger.warning("[webrtc] /sdp/ice failed for %s: %s", session_id, e)
        raise HTTPException(400, f"Bad ICE candidate: {type(e).__name__}: {e}")
    return {"added": True}


# Module logger — set up beside the router for the log line above.
logger = logging.getLogger("visivo.render")


# ── WebSocket: command / frame channel ───────────────────────────────────────


# How often the push loop looks at a cube still loading in the background when
# no command arrives: its chunks change the picture without anyone asking.
_PUSH_POLL_S = 0.25


# Highest frame rate a push stream may ask for: a headset refreshes at 72, 90
# or 120 Hz (the desktop asks for 30 or 60).
_MAX_PUSH_FPS = 120.0

# WebRTC offers of one connection (issue #189): at most this many in a window.
_MAX_OFFERS, _OFFER_WINDOW_S = 6, 60.0


class _PushState:
    """Flow control of one push stream: frames sent but not yet acknowledged."""

    def __init__(self) -> None:
        self.window = 3
        self.max_fps = 30.0
        self.in_flight = 0
        self.acked = asyncio.Event()
        self.rtc: xr_rtc.StereoRtcLink | None = None   # this connection's stereo video (#189)
        self.offer_times: collections.deque = collections.deque()   # its recent xr_rtc_offers

    def configure(self, command: dict) -> None:
        try:
            self.window = max(1, min(8, int(command.get("window", self.window))))
            self.max_fps = max(1.0, min(_MAX_PUSH_FPS,
                                        float(command.get("max_fps", self.max_fps))))
        except (TypeError, ValueError):
            pass   # keep the defaults

    def ack(self) -> None:
        # A frame answered under request_frame before the stream started is
        # acknowledged too: never count below zero.
        self.in_flight = max(0, self.in_flight - 1)
        self.acked.set()


# Sessions in the headset's stereo mode -> the WebSocket that drives them. The
# mode is the session's (its render process draws every frame in it), so it is
# given to one connection alone: no other WebSocket may attach and no WebRTC
# peer may connect meanwhile, and a pose is refused while another client is
# there. Every other client would otherwise get stereo frames, or a volume
# drawn with the stereo settings, instead of the mono view.
_stereo_owners: dict[str, object] = {}
STEREO_BUSY = "This render session is in use by a headset (stereo frames)."


def _stereo_conflict(session_id: str, connection: object) -> str | None:
    """Why ``connection`` may not drive the session in stereo, or None."""
    from ..render_manager import _webrtc_attached
    owner = _stereo_owners.get(session_id)
    if owner is not None and owner is not connection:
        return STEREO_BUSY
    if get_render_manager().clients(session_id) > 1 or _webrtc_attached(session_id):
        return ("Another client is viewing this render session: stereo frames need a "
                "session of their own.")
    return None


#: Sessions that draw a headset's stereo frames: cubes (issue #169) and point
#: tables (issue #190, render_session/_stereo_points.py).
_STEREO_FACTORIES = ("app.render_session:RenderSession",
                     "app.render_session.points:PointRenderSession")


def _supports_stereo(sess) -> bool:
    """Whether the session draws the headset's stereo frames."""
    factory = getattr(sess, "_factory", None)
    if factory is not None:     # a session in a render process of its own
        return factory in _STEREO_FACTORIES
    return hasattr(sess, "_render_xr_frame_on_thread")


def _supports_stereo_video(sess) -> bool:
    """Stereo JPEGs also work for points; H.264 encoding is cube-only."""
    factory = getattr(sess, "_factory", None)
    if factory is not None:
        return factory == "app.render_session:RenderSession"
    from ..render_session._stereo_points import PointStereoMixin
    return _supports_stereo(sess) and not isinstance(sess, PointStereoMixin)


def _stereo_message(frame: XRFrame, ready: float | None, rtc) -> bytes | None:
    """A stereo frame's WebSocket message. A frame the render process encoded
    for the connection's video link (issue #189) goes to the video track, and
    the message carries its header and depth only; None when it is not sent
    (the link closed, not connected at the moment, or the track keeping its
    reference chain whole: the link has asked for a keyframe, or told the
    session to make JPEGs again)."""
    if frame.header.get("color", {}).get("format") != "h264":
        return frame.to_bytes(ready=ready, sent=time.time())
    return rtc.frame_message(frame, ready, time.time()) if rtc is not None else None


async def _send_frame(websocket: WebSocket, send_lock: asyncio.Lock, frame,
                      ready: float | None = None, push: "_PushState | None" = None) -> bool:
    """Send a frame: JPEG bytes as they are, a stereo frame with the time it
    came back from the renderer and the time it is handed to the socket, or a
    stereo frame's error as text. True when a frame (not an error, nor a
    video frame not sent) went out. A frame whose H.264 encoding failed comes
    as a JPEG with the reason: the page is told (``xr_rtc_error``) and the
    connection's video link closed, before the JPEG goes out."""
    rtc = push.rtc if push is not None else None
    if isinstance(frame, XRFrame):
        video_error = frame.header.pop("video_error", None)
        if video_error and push is not None:
            async with send_lock:
                await websocket.send_text(json.dumps(
                    {"type": "xr_rtc_error", "error": video_error}, separators=(",", ":")))
            await _xr_rtc_close(push)
            rtc = None
        message = None if frame.error is not None else _stereo_message(frame, ready, rtc)
        async with send_lock:
            if frame.error is not None:
                await websocket.send_text(frame.error_message())
                return False
            if message is None:
                return False
            await websocket.send_bytes(message)
        return True
    async with send_lock:
        await websocket.send_bytes(frame)
    return True


async def _window_room(push: _PushState, sess=None) -> None:
    """Wait until fewer than ``window`` frames are unacknowledged, or until
    ``sess`` has ended (the caller then closes the stream)."""
    while push.in_flight >= push.window:
        push.acked.clear()
        if push.in_flight >= push.window:
            try:
                await asyncio.wait_for(push.acked.wait(), timeout=_PUSH_POLL_S)
            except asyncio.TimeoutError:
                pass
            if sess is not None and _session_ended(sess):
                return


#: The close reason when the session itself has ended (its render process
#: exited or was killed, or it was closed): reconnecting to it is refused, the
#: client has to create a new session (docs/backend-api/render.md).
SESSION_ENDED_REASON = "session ended"


def _session_ended(sess) -> bool:
    """Whether ``sess`` takes no more requests: a session in a process of its
    own whose child is gone or which was closed, or an in-process one closed."""
    return bool(getattr(sess, "ended", False) or getattr(sess, "_closed", False))


async def _close_ended(websocket: WebSocket, session_id: str) -> None:
    """The session ended under a connected client: one line, no traceback (the
    manager has logged why), and a close the client can tell from a failure."""
    logger.info("[render-ws] %s: the session ended, closing its stream",
                session_id[:8] + "…")
    try:
        await websocket.close(code=1011, reason=SESSION_ENDED_REASON)
    except Exception:   # noqa: BLE001 - already closed
        pass


async def _push_frames(websocket: WebSocket, sess, send_lock: asyncio.Lock,
                       push: _PushState) -> None:
    """Send a frame whenever the view changes, within the ack window and the rate cap.

    The next frame renders while this one is sent: as soon as a frame comes
    back, the following one is asked for if the view has moved on and the
    window has room for it, so the renderer is not idle while the frame goes
    out (and, when the session renders in a process of its own, while it is
    handed across).
    """
    loop = asyncio.get_running_loop()
    last_sent = 0.0
    version = -1          # the scene version of the newest frame asked for; -1: none yet
    ahead: asyncio.Future | None = None   # the next frame, rendering while one is sent
    try:
        while True:
            # A session that ended (its render process gone) is noticed here
            # within a poll, whether or not the client sends anything.
            if _session_ended(sess):
                await _close_ended(websocket, getattr(sess, "session_id", "?"))
                return
            if ahead is None:
                # Nothing rendered while the window is full: a frame made now
                # would wait for acks and arrive stale.
                await _window_room(push, sess)
                try:
                    await asyncio.wait_for(sess.changed.wait(), timeout=_PUSH_POLL_S)
                except asyncio.TimeoutError:
                    pass
                # Cleared before rendering: a change made during the render
                # sets it again (and moves the version on), so it is sent next.
                sess.changed.clear()
                ahead = asyncio.ensure_future(sess.render_frame_if_changed(version))
            jpeg, version = await ahead
            ready = time.time()
            ahead = None
            if jpeg is None:
                continue
            if isinstance(jpeg, XRFrame) and jpeg.error is not None:
                # Nothing to acknowledge: the client is told why, as text.
                await _send_frame(websocket, send_lock, jpeg)
                continue
            # Room for one more after this frame: start the next one now.
            if sess.changed.is_set() and push.in_flight + 1 < push.window:
                sess.changed.clear()
                ahead = asyncio.ensure_future(sess.render_frame_if_changed(version))
            await _window_room(push, sess)
            if _session_ended(sess):
                continue   # closed at the top of the loop
            delay = last_sent + 1.0 / push.max_fps - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)
            # The credit is taken before the send: the client can acknowledge
            # the frame while send_bytes() is still waiting on the transport,
            # and an ack counted before its frame would be lost for good.
            push.in_flight += 1
            if not await _send_frame(websocket, send_lock, jpeg, ready, push):
                push.ack()       # a video frame dropped with its link: nothing to acknowledge
            last_sent = loop.time()
    except asyncio.CancelledError:
        raise
    except Exception:
        if _session_ended(sess):
            await _close_ended(websocket, getattr(sess, "session_id", "?"))
            return
        # A failed render or send. The receive loop would go on taking
        # commands with nothing coming back, so end the connection: the
        # client sees it close and can reconnect.
        logger.warning("[render-ws] push stream failed, closing", exc_info=True)
        try:
            await websocket.close(code=1011, reason="render failed")
        except Exception:
            pass
    finally:
        if ahead is not None:
            ahead.cancel()
            await asyncio.gather(ahead, return_exceptions=True)


WS_SUBPROTOCOL = "visivo.v1"
_TOKEN_SUBPROTOCOL = "visivo-token."


def _token_from_subprotocols(header: str) -> tuple[str, str | None]:
    """(token, subprotocol to answer) from a Sec-WebSocket-Protocol header:
    the token of a "visivo-token.<token>" entry, and "visivo.v1" when the
    client offered it (None otherwise: no subprotocol in the answer)."""
    offered = [p.strip() for p in header.split(",") if p.strip()]
    token = next((p[len(_TOKEN_SUBPROTOCOL):] for p in offered if p.startswith(_TOKEN_SUBPROTOCOL)), "")
    return token, (WS_SUBPROTOCOL if WS_SUBPROTOCOL in offered else None)


@router.websocket("/{session_id}/ws")
async def render_ws(websocket: WebSocket, session_id: str) -> None:
    """Bidirectional channel: client commands in, JPEG frames out.

    Auth on a WebSocket can't ride on a header set by ``fetch``-style
    code; we accept the token as a query parameter (``?token=…``) and
    fall back to ``X-Visivo-Token`` if the underlying transport
    forwarded it (e.g. an SSH tunnel doesn't, but a reverse-proxy
    might).
    """
    # Manual auth — the verify_token Depends() doesn't apply to WS.
    token_qp = websocket.query_params.get("token", "")
    token_hd = websocket.headers.get("x-visivo-token", "")
    # A browser can also send it as a subprotocol, "visivo-token.<token>" next
    # to "visivo.v1" (the VR page does, so the token is in no access log); the
    # answer names "visivo.v1" only, never the token.
    token_sp, subprotocol = _token_from_subprotocols(websocket.headers.get("sec-websocket-protocol", ""))
    from ..auth import accepted_tokens as _accepted_tokens, token_equal as _token_equal
    supplied = token_qp or token_hd or token_sp
    matched: str | None = None
    if supplied:
        # Every per-user token is accepted (not just the primary one); the loop
        # does not short-circuit, so a match position isn't timing-observable.
        for _accepted in _accepted_tokens():
            if _token_equal(supplied, _accepted):
                matched = _accepted
    if matched is None:
        # Close with policy code 1008 — "policy violation".
        await websocket.close(code=1008, reason="invalid token")
        return

    # Owner-checked: another valid token holding this id must not be able to
    # drive someone else's render stream.
    sess = await _owned_render_session(session_id, matched)
    if sess is None:
        await websocket.close(code=1011, reason="unknown session_id")
        return

    await websocket.accept(subprotocol=subprotocol)
    if session_id in _stereo_owners:
        # The session draws a headset's stereo frames: this client would get
        # them instead of its view (1013: try again later). Checked after the
        # accept and with no await before the attach below, so an xr_pose
        # cannot take the session between the check and the attach; closing
        # after the accept lets the client see the code.
        await websocket.close(code=1013, reason="session in stereo use")
        return
    logger.info("[render-ws] %s connected", session_id[:8] + "…")
    # While a client is connected the session keeps its slot even when
    # requests wait; the last one leaving starts the disconnect grace.
    manager = get_render_manager()
    manager.attach(session_id)

    # Inner loop. Pattern: receive a JSON command; if it's request_frame,
    # render and send a JPEG; otherwise apply and continue (the next
    # request_frame picks up the accumulated state). A client that sends
    # ``stream`` switches to push: the server renders and sends a frame
    # whenever the scene changes, at most ``window`` frames ahead of the
    # client's ``frame_ack``s, so a round trip no longer bounds the frame rate.
    send_lock = asyncio.Lock()   # the push task and this loop both send
    push = _PushState()
    push_task: asyncio.Task | None = None
    stereo = False               # this connection owns the session's stereo mode
    try:
        while True:
            raw = await websocket.receive_text()
            try:
                command = json.loads(raw)
            except json.JSONDecodeError:
                async with send_lock:
                    await websocket.send_text(
                        json.dumps({"error": "invalid JSON"}, separators=(",", ":"))
                    )
                continue
            ctype = command.get("type", "")
            if ctype == "request_frame":
                if push_task is not None:
                    # Streaming: every frame goes through the push loop, so
                    # each one is a frame the client acknowledges. A request
                    # with nothing changed has nothing to send.
                    sess.changed.set()
                    continue
                jpeg = await sess.render_frame()
                await _send_frame(websocket, send_lock, jpeg, time.time(), push)
            elif ctype == "stream":
                if push_task is None:
                    push.configure(command)
                    async with send_lock:
                        await websocket.send_text(json.dumps(
                            {"stream": "on", "window": push.window, "max_fps": push.max_fps},
                            separators=(",", ":")))
                    sess.changed.set()     # the client gets the current view first
                    push_task = asyncio.create_task(
                        _push_frames(websocket, sess, send_lock, push))
                else:
                    # Streaming already: a new ack window or rate cap (the
                    # headset's page widens its window over a slow link,
                    # issue #187). A wider window lets a waiting frame go.
                    push.configure(command)
                    push.acked.set()
                    async with send_lock:
                        await websocket.send_text(json.dumps(
                            {"stream": "on", "window": push.window, "max_fps": push.max_fps},
                            separators=(",", ":")))
            elif ctype == "frame_ack":
                push.ack()
            elif ctype == "xr_pose":
                # Checked here, so a bad pose is answered at once and never
                # reaches the renderer; stamped with its arrival, the start
                # of the frame's server timings.
                received = time.time()
                # The backend sets it, below; a client may only ask for a JPEG.
                as_jpeg = command.pop("frame_color", None) == "jpeg"
                conflict = None if stereo else _stereo_conflict(session_id, websocket)
                if not _supports_stereo(sess):
                    reply = {"type": "xr_error", "pose_id": command.get("pose_id"),
                             "error": "Stereo frames are drawn for cubes and point tables only.",
                             "code": "unsupported"}
                elif conflict is not None:
                    reply = {"type": "xr_error", "pose_id": command.get("pose_id"),
                             "error": conflict}
                else:
                    try:
                        parse_pose(command)
                        reply = None
                    except ValueError as exc:
                        reply = {"type": "xr_error", "pose_id": command.get("pose_id"),
                                 "error": str(exc)}
                if reply is not None:
                    async with send_lock:
                        await websocket.send_text(json.dumps(reply, separators=(",", ":")))
                    continue
                command["t_received"] = received
                if not as_jpeg and push.rtc is not None and push.rtc.ready:
                    # Its eyes go as video, encoded by the render process
                    # (which falls back to a JPEG for eyes too narrow or odd).
                    command["frame_color"] = "h264"
                if not stereo:
                    _stereo_owners[session_id] = websocket
                    stereo = True
                await sess.apply_command(command)
            elif ctype == "xr_rtc_offer":
                await _xr_rtc_offer(websocket, send_lock, session_id, push, command, sess)
            elif ctype == "xr_rtc_stop":
                await _xr_rtc_close(push)
            elif ctype == "keepalive":
                # Touches stats.last_command_at via apply_command's bookkeeping.
                await sess.apply_command({"type": "keepalive"})
            elif ctype == "close":
                break
            elif isinstance(ctype, str) and ctype.startswith("xr_video"):
                # The backend's own commands to the render process (the video
                # link's encoder): never a client's.
                logger.warning("[render-ws] %s: client command %r ignored",
                               session_id[:8] + "…", ctype[:40])
            else:
                await sess.apply_command(command)
    except WebSocketDisconnect:
        logger.info("[render-ws] %s disconnected", session_id[:8] + "…")
    except Exception:
        if _session_ended(sess):
            await _close_ended(websocket, session_id)
        else:
            logger.exception("[render-ws] %s error, closing", session_id[:8] + "…")
            try:
                await websocket.close(code=1011, reason="server error")
            except RuntimeError:
                pass
    finally:
        try:
            if push_task is not None:
                push_task.cancel()
                # Its outcome (cancelled, or failed and already logged) is
                # collected without hiding a cancellation of this handler.
                await asyncio.gather(push_task, return_exceptions=True)
        finally:
            # Even when this handler is cancelled at the wait above.
            try:
                try:
                    await _xr_rtc_close(push)
                finally:
                    if stereo:
                        await _leave_stereo(session_id, sess)
            finally:
                # Disconnect does NOT evict the session — the user might
                # reconnect quickly (network blip, page reload, a switch back
                # from Local). The idle TTL handles real abandonment; while
                # requests wait, so does the disconnect grace (render_manager).
                manager.detach(session_id)


async def _leave_stereo(session_id: str, sess) -> None:
    """The headset's connection is gone: the session goes back to its mono
    view and may be attached again. The xr_stop is shielded: a cancellation
    of the closing handler does not leave the session in stereo."""
    _stereo_owners.pop(session_id, None)
    stop = asyncio.ensure_future(sess.apply_command({"type": "xr_stop"}))
    try:
        await asyncio.shield(stop)
    except asyncio.CancelledError:
        raise                 # the xr_stop goes on; the handler's cancellation does too
    except Exception:   # noqa: BLE001 - the session may be gone already
        logger.debug("[render-ws] xr_stop on leave failed", exc_info=True)


# ── Stereo frames as WebRTC video (issue #189) ───────────────────────────────


async def _xr_rtc_offer(websocket: WebSocket, send_lock: asyncio.Lock, session_id: str,
                        push: _PushState, command: dict, sess) -> None:
    """``xr_rtc_offer``: the page's SDP offer for a video link of this
    connection, answered on the WebSocket (``xr_rtc_answer``, or
    ``xr_rtc_error`` with the reason; the JPEG frames then go on). The link
    is the connection's own, not one of the session's mono WebRTC peers: it
    is refused while another client views the session, as a pose is. The
    session's render process encodes the frames: it is told so first
    (``xr_video``), in order with the poses that follow."""
    async def reply(message: dict) -> None:
        async with send_lock:
            await websocket.send_text(json.dumps(message, separators=(",", ":")))

    now = time.monotonic()
    recent = push.offer_times
    while recent and now - recent[0] > _OFFER_WINDOW_S:
        recent.popleft()
    sdp = command.get("sdp")
    why = (xr_rtc.rtc_available()
           or (None if _supports_stereo_video(sess) else "Stereo WebRTC video is available for cubes only; use JPEG stereo frames for points.")
           or _stereo_conflict(session_id, websocket)
           or (f"Too many offers: at most {_MAX_OFFERS} a minute."
               if len(recent) >= _MAX_OFFERS else None)
           or xr_rtc.check_offer(sdp)[1])
    if why is not None:
        await reply({"type": "xr_rtc_error", "error": why})
        return
    recent.append(now)
    # The link before is closed, its "video off" applied by the session,
    # before the new one's "video on" is sent.
    await _xr_rtc_close(push)

    def to_session(cmd: dict):
        task = asyncio.ensure_future(sess.apply_command(cmd))
        task.add_done_callback(lambda t: t.cancelled() or t.exception())   # never unretrieved
        return task

    link = xr_rtc.StereoRtcLink.from_offer_command(session_id, command, to_session)
    # Held by the connection from now on: closed with it, whatever happens below.
    push.rtc = link
    try:
        await sess.apply_command(link.video_command())
        answer = await link.answer(sdp, "offer")
    except Exception as exc:   # noqa: BLE001 - told to the page, which keeps JPEG
        await _xr_rtc_close(push)
        logger.warning("[render-ws] %s: stereo WebRTC negotiation failed: %s",
                       session_id[:8] + "…", exc)
        await reply({"type": "xr_rtc_error",
                     "error": f"WebRTC negotiation failed: {type(exc).__name__}: {exc}"})
        return
    except BaseException:
        await asyncio.shield(_xr_rtc_close(push))
        raise
    await reply({"type": "xr_rtc_answer", **answer, "crf": link.crf, "max_kbps": link.max_kbps})


async def _xr_rtc_close(push: _PushState) -> None:
    """``xr_rtc_stop``, or the connection closing: the link is closed and the
    stereo frames go as JPEG again."""
    link, push.rtc = push.rtc, None
    if link is not None:
        await link.close()
