"""Server-side WebRTC transport for VisIVO SSR — Phase 3.

Wraps an ``aiortc.RTCPeerConnection`` around an existing
``RenderSession`` so the same VTK render thread can feed either:

  * the JPEG-over-WebSocket transport (legacy / fallback), via
    :meth:`RenderSession.render_frame`
  * the H.264-over-WebRTC transport (this module), via
    :meth:`RenderSession.render_rgb_frame` → NVENC encode → RTP

Selecting per-session:

  * The router accepts either ``WS /v1/render/{id}/ws`` (JPEG) OR
    ``POST /v1/render/{id}/sdp/offer`` (WebRTC). A session can have
    at most one active transport at a time; switching tears down the
    other.
  * A backend without NVENC (e.g. the default ``backend-egl.sif``
    without Stage 6) refuses the WebRTC route at module-import time
    and the JPEG path stays the only option — clients fall back
    automatically via the capability negotiation.

NVENC integration:

  * At module import we monkey-patch
    ``aiortc.codecs.h264.create_encoder_context`` to build a
    ``h264_nvenc`` encoder instead of the default ``libx264``.
  * The patch is best-effort: if NVENC can't open (no GPU, wrong
    driver, etc.) the factory falls back to the upstream behaviour
    and we silently end up with software H.264. The Stage 6
    smoke-test in the SIF guarantees NVENC is available where it
    matters.

Camera command bridge:

  * Each peer connection gets a single ``RTCDataChannel`` called
    "commands". JSON frames flowing in and out match the existing
    WebSocket protocol byte-for-byte, so the desktop client can
    share its serialisation code with the JPEG path.
"""
from __future__ import annotations

import asyncio
import json
import logging
import time
from fractions import Fraction
from typing import Any

from aiortc import (
    MediaStreamTrack,
    RTCDataChannel,
    RTCPeerConnection,
    RTCSessionDescription,
)
from av import VideoFrame

from .render_session import RenderSession

logger = logging.getLogger("visivo.render.webrtc")


# ── NVENC patch (applied once at import) ─────────────────────────────────────


_NVENC_INSTALLED = False


def _install_nvenc_encoder() -> bool:
    """Patch aiortc's H.264 encoder factory to use ``h264_nvenc``.

    Returns True if the patch was applied successfully, False if NVENC
    isn't available (in which case aiortc keeps using its libx264
    default — the WebRTC transport still works, just with CPU encode).
    """
    global _NVENC_INSTALLED
    if _NVENC_INSTALLED:
        return True
    try:
        from aiortc.codecs import h264 as h264_mod
        import av
    except ImportError as e:
        logger.info("[webrtc] aiortc / PyAV unavailable (%s); "
                    "WebRTC transport disabled.", e)
        return False

    original_create = h264_mod.create_encoder_context

    def create_encoder_context_nvenc(codec_name, width, height, bitrate):
        try:
            codec = av.CodecContext.create("h264_nvenc", "w")
            codec.width = width
            codec.height = height
            codec.pix_fmt = "yuv420p"
            codec.framerate = Fraction(30, 1)
            codec.time_base = Fraction(1, 90000)
            codec.bit_rate = bitrate
            codec.gop_size = 30
            try:
                codec.options = {
                    "preset": "p1", "tune": "ll", "rc": "cbr", "delay": "0",
                }
            except Exception:
                pass
            codec.open(strict=False)
            return codec, False
        except Exception as e:
            logger.warning(
                "[webrtc] NVENC encoder build failed (%s); falling back "
                "to %r software encoder.", e, codec_name,
            )
            return original_create(codec_name, width, height, bitrate)

    h264_mod.create_encoder_context = create_encoder_context_nvenc
    _NVENC_INSTALLED = True
    logger.info("[webrtc] aiortc H.264 encoder patched to use h264_nvenc.")
    return True


def webrtc_available() -> bool:
    """Backend feature probe — used by the router to advertise capabilities."""
    return _install_nvenc_encoder()


def _force_h264_on(pc: RTCPeerConnection) -> None:
    """Restrict video transceivers to H.264 — aiortc's default offer
    leads with VP8, which would bypass our NVENC patch entirely."""
    try:
        from aiortc.rtcrtpsender import RTCRtpSender
        caps = RTCRtpSender.getCapabilities("video")
        h264 = [c for c in caps.codecs if c.mimeType.lower() == "video/h264"]
        if not h264:
            return
        for tr in pc.getTransceivers():
            if tr.kind == "video":
                tr.setCodecPreferences(h264)
    except Exception as e:  # pragma: no cover
        logger.warning("[webrtc] codec forcing failed: %s", e)


# ── VTK-backed MediaStreamTrack ──────────────────────────────────────────────


class VtkFrameTrack(MediaStreamTrack):
    """Pulls RGB frames from a :class:`RenderSession` at the requested FPS.

    Pacing is server-side: the track produces one frame per
    ``1/fps`` second of wall-clock. aiortc's RTP layer then encodes
    via NVENC and sends. If the encoder can't keep up we lose
    frames (which is fine for an interactive viewer — a stale
    frame is worse than a dropped one).
    """
    kind = "video"

    def __init__(self, render_session: RenderSession, fps: float = 30.0):
        super().__init__()
        self._session = render_session
        self._fps = fps
        self._counter = 0
        self._start_wall: float | None = None

    async def recv(self) -> VideoFrame:
        if self._start_wall is None:
            self._start_wall = time.perf_counter()

        # Target FPS pacing.
        target_t = self._start_wall + self._counter / self._fps
        delta = target_t - time.perf_counter()
        if delta > 0:
            await asyncio.sleep(delta)

        rgb, _render_ms = await self._session.render_rgb_frame()
        h, w, _ = rgb.shape
        frame = VideoFrame.from_ndarray(rgb, format="rgb24")
        frame.pts = self._counter
        frame.time_base = Fraction(1, int(self._fps))
        self._counter += 1
        return frame


# ── WebRTC session — one per peer ─────────────────────────────────────────────


class WebRTCSession:
    """Aggregates the PeerConnection + media track + command channel.

    The lifetime is tied to a single offer/answer dance. On
    disconnect (PeerConnection state transitions to ``closed`` or
    ``failed``) the session releases its reference to the underlying
    ``RenderSession`` — but does NOT close the RenderSession itself,
    which may still be serving the WebSocket transport in parallel.
    """

    def __init__(self, render_session: RenderSession):
        self._render = render_session
        self._pc: RTCPeerConnection | None = None
        self._commands_dc: RTCDataChannel | None = None

    async def create_answer(self, offer_sdp: str, offer_type: str) -> dict:
        # Make sure NVENC is plumbed in.
        webrtc_available()

        pc = RTCPeerConnection()
        self._pc = pc

        @pc.on("connectionstatechange")
        async def _on_state() -> None:
            logger.info("[webrtc] %s PC state -> %s",
                        self._render.session_id, pc.connectionState)
            if pc.connectionState in ("closed", "failed"):
                await self.close()

        @pc.on("datachannel")
        def _on_dc(channel: RTCDataChannel) -> None:
            logger.info("[webrtc] %s data channel %r open",
                        self._render.session_id, channel.label)
            if channel.label == "commands":
                self._commands_dc = channel

                @channel.on("message")
                def _on_message(message) -> None:
                    asyncio.ensure_future(self._handle_command(message))

        pc.addTrack(VtkFrameTrack(self._render))
        _force_h264_on(pc)

        await pc.setRemoteDescription(
            RTCSessionDescription(sdp=offer_sdp, type=offer_type)
        )
        answer = await pc.createAnswer()
        await pc.setLocalDescription(answer)
        return {
            "sdp": pc.localDescription.sdp,
            "type": pc.localDescription.type,
        }

    async def _handle_command(self, message: Any) -> None:
        """Forward a DataChannel message to the underlying RenderSession.

        Same protocol as the WebSocket transport: a JSON object with
        a ``type`` key. ``request_frame`` is a no-op here because the
        media track pushes frames continuously at the configured FPS.
        """
        if isinstance(message, (bytes, bytearray)):
            try:
                message = message.decode("utf-8")
            except UnicodeDecodeError:
                logger.warning("[webrtc] non-UTF-8 binary command ignored")
                return
        try:
            cmd = json.loads(message)
        except json.JSONDecodeError:
            logger.warning("[webrtc] invalid JSON command: %r", message[:80])
            return
        ctype = cmd.get("type", "")
        if ctype == "request_frame":
            # WebRTC transports continuous video; ignore.
            return
        await self._render.apply_command(cmd)

    async def close(self) -> None:
        if self._pc is not None:
            try:
                await self._pc.close()
            except Exception:
                pass
            self._pc = None
        self._commands_dc = None
