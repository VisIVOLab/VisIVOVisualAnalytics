"""Server-side rendering endpoints (Phase 1 + Phase 2 wired together).

Endpoints
---------

* ``POST   /v1/render/create_session`` — start a session for a dataset
* ``DELETE /v1/render/{session_id}``   — explicit teardown
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
import json
import logging
from typing import Annotated, Any

from fastapi import APIRouter, Depends, HTTPException, WebSocket, WebSocketDisconnect
from pydantic import BaseModel

from ..auth import verify_token
from ..render_manager import get_render_manager
from ..render_session import server_side_rendering_available
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


class CreateRenderSessionRequest(BaseModel):
    dataset_id: str


class CreateRenderSessionResponse(BaseModel):
    valid: bool
    error: str = ""
    session_id: str = ""
    initial_frame_jpeg_b64: str = ""
    width: int = 0
    height: int = 0


@router.post("/create_session", response_model=CreateRenderSessionResponse)
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
    if entry.get("kind") != "cube":
        raise HTTPException(422, "Server-side rendering currently supports cube datasets only.")

    manager = get_render_manager()
    await manager.start()   # idempotent — reaper coroutine
    try:
        sess = await manager.create_session(dataset_path=str(entry["path"]),
                                            owner_token=token)
    except RuntimeError as e:
        # Capacity reached.
        raise HTTPException(503, str(e))

    # Render an initial frame so the client has something to show
    # immediately after the WS connects (avoids a black widget while
    # the user finds the mouse).
    import base64
    initial = await sess.render_frame()
    return CreateRenderSessionResponse(
        valid=True,
        session_id=sess.session_id,
        initial_frame_jpeg_b64=base64.b64encode(initial).decode("ascii"),
        width=sess.view.width,
        height=sess.view.height,
    )


@router.delete("/{session_id}")
async def close_session(session_id: str, token: str = _auth) -> dict[str, Any]:
    if await _owned_render_session(session_id, token) is None:
        raise HTTPException(404, "Unknown render session_id.")
    closed = await get_render_manager().close_session(session_id)
    if not closed:
        raise HTTPException(404, "Unknown render session_id.")
    return {"closed": True}


@router.get("/diagnostics")
async def diagnostics(token: str = _auth) -> dict[str, Any]:
    """Snapshot of the CALLER'S active render sessions — ops dashboards + tests.

    Session ids and dataset paths are per-user data, so the listing is filtered to
    the sessions this token owns (unowned ones stay visible only in single-token
    mode, where there is nothing to isolate)."""
    from ..auth import multi_user_mode as _multi
    snap = await get_render_manager().snapshot_for_owner(token, _multi())
    # Surface which transports this backend can serve so the client
    # picks the best path: ``jpeg-ws`` is always on; ``h265-webrtc``
    # only when the optional Stage 6 H.265 build was applied.
    try:
        from ..webrtc_session import webrtc_available
        transports = ["jpeg-ws", "jpeg-ws-push"]
        if webrtc_available():
            transports.append("h265-webrtc")
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


class _PushState:
    """Flow control of one push stream: frames sent but not yet acknowledged."""

    def __init__(self) -> None:
        self.window = 3
        self.max_fps = 30.0
        self.in_flight = 0
        self.acked = asyncio.Event()

    def configure(self, command: dict) -> None:
        try:
            self.window = max(1, min(8, int(command.get("window", self.window))))
            self.max_fps = max(1.0, min(60.0, float(command.get("max_fps", self.max_fps))))
        except (TypeError, ValueError):
            pass   # keep the defaults

    def ack(self) -> None:
        # A frame answered under request_frame before the stream started is
        # acknowledged too: never count below zero.
        self.in_flight = max(0, self.in_flight - 1)
        self.acked.set()


async def _push_frames(websocket: WebSocket, sess, send_lock: asyncio.Lock,
                       push: _PushState) -> None:
    """Send a frame whenever the view changes, within the ack window and the rate cap."""
    loop = asyncio.get_running_loop()
    last_sent = 0.0
    sent_version = -1   # the scene version of the last frame sent; -1: none yet
    try:
        while True:
            if push.in_flight >= push.window:
                push.acked.clear()
                if push.in_flight >= push.window:
                    await push.acked.wait()
                continue
            try:
                await asyncio.wait_for(sess.changed.wait(), timeout=_PUSH_POLL_S)
            except asyncio.TimeoutError:
                pass
            # Cleared before rendering: a change made during the render sets
            # it again (and the dirty flag), so it is sent next.
            sess.changed.clear()
            delay = last_sent + 1.0 / push.max_fps - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)
            jpeg, sent_version = await sess.render_frame_if_changed(sent_version)
            if jpeg is None:
                continue
            # The credit is taken before the send: the client can acknowledge
            # the frame while send_bytes() is still waiting on the transport,
            # and an ack counted before its frame would be lost for good.
            push.in_flight += 1
            async with send_lock:
                await websocket.send_bytes(jpeg)
            last_sent = loop.time()
    except asyncio.CancelledError:
        raise
    except Exception:
        # A failed render or send. The receive loop would go on taking
        # commands with nothing coming back, so end the connection: the
        # client sees it close and can reconnect.
        logger.warning("[render-ws] push stream failed, closing", exc_info=True)
        try:
            await websocket.close(code=1011, reason="render failed")
        except Exception:
            pass


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
    from ..auth import accepted_tokens as _accepted_tokens
    import secrets as _secrets
    supplied = token_qp or token_hd
    matched: str | None = None
    if supplied:
        # Every per-user token is accepted (not just the primary one); the loop
        # does not short-circuit, so a match position isn't timing-observable.
        for _accepted in _accepted_tokens():
            if _secrets.compare_digest(supplied, _accepted):
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

    await websocket.accept()
    logger.info("[render-ws] %s connected", session_id[:8] + "…")

    # Inner loop. Pattern: receive a JSON command; if it's request_frame,
    # render and send a JPEG; otherwise apply and continue (the next
    # request_frame picks up the accumulated state). A client that sends
    # ``stream`` switches to push: the server renders and sends a frame
    # whenever the scene changes, at most ``window`` frames ahead of the
    # client's ``frame_ack``s, so a round trip no longer bounds the frame rate.
    send_lock = asyncio.Lock()   # the push task and this loop both send
    push = _PushState()
    push_task: asyncio.Task | None = None
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
                async with send_lock:
                    await websocket.send_bytes(jpeg)
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
            elif ctype == "frame_ack":
                push.ack()
            elif ctype == "keepalive":
                # Touches stats.last_command_at via apply_command's bookkeeping.
                await sess.apply_command({"type": "keepalive"})
            elif ctype == "close":
                break
            else:
                await sess.apply_command(command)
    except WebSocketDisconnect:
        logger.info("[render-ws] %s disconnected", session_id[:8] + "…")
    except Exception:
        logger.exception("[render-ws] %s error, closing", session_id[:8] + "…")
        try:
            await websocket.close(code=1011, reason="server error")
        except RuntimeError:
            pass
    finally:
        if push_task is not None:
            push_task.cancel()
            # Its outcome (cancelled, or failed and already logged) is
            # collected without hiding a cancellation of this handler.
            await asyncio.gather(push_task, return_exceptions=True)
        # Disconnect does NOT auto-evict the session — the user might
        # reconnect quickly (network blip, page reload). TTL eviction
        # handles real abandonment.
        pass
