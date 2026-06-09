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


# ── Trickle-ICE session registry ──────────────────────────────────────────────
#
# A session's WebRTCSession is created by the /sdp/offer endpoint and
# kept alive by the PeerConnection's reference to its tracks; once the
# PC closes, our @on('connectionstatechange') handler removes it from
# the map. Holding the reference here also lets the /sdp/ice endpoint
# look up the right PC to push trickled candidates onto.

_active_sessions: dict[str, "WebRTCSession"] = {}


def get_active_webrtc_session(session_id: str) -> "WebRTCSession | None":
    """Look up the WebRTCSession created by the last /sdp/offer for
    ``session_id``. Returns ``None`` once the PC has closed."""
    return _active_sessions.get(session_id)


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

    # Lift aiortc's internal rate cap. Default is MAX_BITRATE = 3_000_000
    # (≈ 3 Mbps), set for a "video-call over consumer broadband" target.
    # For scientific visualisation over a campus link / SSH tunnel the
    # bottleneck isn't bandwidth — it's V100 NVENC headroom, which is
    # nowhere near saturated at 15 Mbps for 1080p. Without this bump,
    # raising the client's SDP b=AS to 15000 had no effect because
    # aiortc's BWE clamped requested bitrate back down to 3 Mbps before
    # handing it to the encoder.
    for const_name, target in (
        ("MAX_BITRATE", 20_000_000),
        ("DEFAULT_BITRATE", 12_000_000),
        ("MIN_BITRATE", 2_000_000),
    ):
        if hasattr(h264_mod, const_name):
            try:
                setattr(h264_mod, const_name, target)
            except Exception as e:
                logger.warning("[webrtc] could not raise %s: %s",
                               const_name, e)

    original_create = h264_mod.create_encoder_context

    class _NvencCodecProxy:
        """Forwards every codec attribute except ``bit_rate``, which is
        clamped to the last-known-good value.

        Why: certain NVENC rate-control modes zero the internal C-level
        ``bit_rate`` field on the AVCodecContext (in particular CQ mode,
        but we've seen it intermittently in VBR too). PyAV's property
        getter maps ``0`` to ``None`` (see PyAV source —
        ``self.ptr.bit_rate or None``). aiortc's H264Encoder then runs
        ``abs(self.target_bitrate - self.codec.bit_rate)`` for its rate
        adaptation and crashes with ``TypeError: unsupported operand
        type(s) for -: 'int' and 'NoneType'``. The proxy traps reads of
        ``bit_rate`` and returns the integer we last wrote, so aiortc's
        arithmetic always sees a real number even when NVENC hides its
        internal rate from us.
        """
        def __init__(self, codec, initial_bitrate):
            # Bypass our own ``__setattr__`` for the two state fields —
            # otherwise the assignments would recurse trying to delegate
            # them to the wrapped codec.
            object.__setattr__(self, "_codec", codec)
            object.__setattr__(self, "_bitrate_shadow", int(initial_bitrate))

        def __getattr__(self, name):
            # __getattr__ runs only when normal lookup misses; our
            # ``bit_rate`` property below shadows the wrapped codec's.
            return getattr(self._codec, name)

        def __setattr__(self, name, value):
            if name == "bit_rate":
                if value is not None and int(value) > 0:
                    object.__setattr__(self, "_bitrate_shadow", int(value))
                try:
                    self._codec.bit_rate = value
                except Exception:
                    pass
                return
            setattr(self._codec, name, value)

        @property
        def bit_rate(self):
            inner = self._codec.bit_rate
            if inner is None or inner <= 0:
                return self._bitrate_shadow
            return int(inner)

    def create_encoder_context_nvenc(codec_name, width, height, bitrate):
        # Round to the next 100 kbps to give NVENC a stable target —
        # aiortc passes in BWE-driven values that jitter by tens of
        # bps and NVENC dislikes that on CBR.
        bitrate = max(bitrate, 12_000_000)
        try:
            codec = av.CodecContext.create("h264_nvenc", "w")
            codec.width = width
            codec.height = height
            codec.pix_fmt = "yuv420p"
            codec.framerate = Fraction(60, 1)
            codec.time_base = Fraction(1, 90000)
            codec.bit_rate = bitrate
            # 2 s GOP @ 60 FPS — long enough that I-frames don't dominate
            # the bitrate budget and crush quality between them, short
            # enough that a dropped frame recovers within ~2 s instead
            # of stalling for the whole sequence. Game streamers use
            # 1-4 s GOPs; we sit in the middle.
            codec.gop_size = 120
            try:
                # ``p4`` ≈ medium-quality NVENC preset (p1=fastest /
                # ugliest, p7=slowest / prettiest). On V100 at 1080p60
                # p4 encodes well below 16 ms per frame, leaving room
                # for VTK render + RTP packetisation in the same budget.
                # ``tune=ull`` keeps look-ahead disabled (mandatory for
                # interactive latency) but no longer pegs preset to p1.
                # ``rc=vbr`` with explicit ``b`` + ``maxrate`` lets
                # NVENC overshoot for hard-to-encode bursts (e.g. a
                # fly-through revealing new detail) up to 1.5x the
                # target. DO NOT use ``cq``: in quality-targeted CQ
                # mode NVENC zeroes ``bit_rate`` internally, PyAV
                # surfaces that as ``None``, and aiortc's BWE then
                # crashes on ``target_bitrate - None``.
                # ``spatial_aq`` + ``aq_strength`` adapt quantisation
                # per region, which is exactly what makes nebula
                # gradients look clean instead of blocky.
                maxrate = int(bitrate * 1.5)
                codec.options = {
                    "preset": "p4",
                    "tune": "ull",
                    "rc": "vbr",
                    "b": str(bitrate),
                    "maxrate": str(maxrate),
                    "bufsize": str(bitrate),
                    "spatial_aq": "1",
                    "aq_strength": "8",
                    "rc-lookahead": "0",
                    "b_ref_mode": "disabled",
                    "delay": "0",
                    "zerolatency": "1",
                }
            except Exception:
                pass
            codec.open(strict=False)
            logger.info(
                "[webrtc] NVENC encoder open: %dx%d @ %.1f Mbps (preset=p4, "
                "tune=ull, vbr, gop=%d)", width, height,
                bitrate / 1_000_000, codec.gop_size,
            )
            return _NvencCodecProxy(codec, bitrate), False
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

    def __init__(self, render_session: RenderSession, fps: float = 60.0):
        # 60 FPS upper bound — V100 NVENC + tunnel can sustain it
        # comfortably for a 1080p cube (encode ~3 ms, network ~5 ms).
        # The actual delivered rate is capped by either the encoder
        # backlog or whichever pipeline stage is slowest; this is
        # just the pacing ceiling.
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
        slept_ms = 0.0
        if delta > 0:
            slept_ms = delta * 1000.0
            await asyncio.sleep(delta)

        render_start = time.perf_counter()
        rgb, render_ms = await self._session.render_rgb_frame()
        render_total_ms = (time.perf_counter() - render_start) * 1000.0
        h, w, _ = rgb.shape
        wrap_start = time.perf_counter()
        frame = VideoFrame.from_ndarray(rgb, format="rgb24")
        frame.pts = self._counter
        frame.time_base = Fraction(1, int(self._fps))
        wrap_ms = (time.perf_counter() - wrap_start) * 1000.0

        # Print one diagnostic line per second. ``render_ms`` is
        # the VTK render+readback alone; ``render_total_ms`` adds the
        # executor hop and the GIL-released wait. ``wrap_ms`` is the
        # numpy → av.VideoFrame copy. Encode is downstream of recv()
        # and isn't reachable from here; aiortc owns it.
        if self._counter % int(max(1, self._fps)) == 0:
            logger.info(
                "[webrtc-track] frame=%d %dx%d vtk_render=%.1fms "
                "render_total=%.1fms videoframe_wrap=%.1fms paced_sleep=%.1fms",
                self._counter, w, h, render_ms,
                render_total_ms, wrap_ms, slept_ms,
            )

        # Scene-change IDR injection. After certain apply_commands
        # (set_lut, set_blend_mode, set_cutting_plane) the rendered
        # content changes drastically — the next P-frame can't
        # reference enough of the previous frame and the client
        # decoder spits ``concealing N DC/AC/MV errors``. Marking the
        # frame as an I-frame tells NVENC to emit a full IDR, costing
        # a few extra kilobits but eliminating the corruption.
        # ``gop_size=120`` at 60 FPS = a fresh IDR every 2 s without
        # this hint, which is far too long for an interactive viewer
        # where the user toggles state frequently.
        if self._session.consume_idr_flag():
            try:
                frame.pict_type = "I"
            except Exception:
                pass
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
        # Register ourselves so /sdp/ice can find this PC by
        # session_id when the client trickle-posts candidates.
        _active_sessions[self._render.session_id] = self

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

    async def add_ice_candidate(
        self,
        candidate: str,
        sdp_mid: str | None = None,
        sdp_mline_index: int | None = None,
    ) -> None:
        """Push a trickled ICE candidate onto our PeerConnection.

        aiortc's ``addIceCandidate`` takes a parsed
        ``aiortc.RTCIceCandidate`` object, not the raw ``a=candidate:…``
        string the client posts. We parse it here. Out-of-spec
        candidate strings raise — those bubble up to the router and
        become 400s.
        """
        if self._pc is None:
            raise RuntimeError("PeerConnection is closed.")
        from aiortc import RTCIceCandidate
        from aiortc.sdp import candidate_from_sdp  # private but stable since 0.9

        # ``candidate`` may arrive with the leading "candidate:" prefix
        # (RFC 5245 style) or without (libdatachannel emits the bare
        # value). Normalise.
        bare = candidate
        if bare.startswith("candidate:"):
            bare = bare[len("candidate:") :]

        ice = candidate_from_sdp(bare)
        ice.sdpMid = sdp_mid
        ice.sdpMLineIndex = sdp_mline_index
        await self._pc.addIceCandidate(ice)

    async def close(self) -> None:
        # Drop ourselves from the global registry first so /sdp/ice
        # stops accepting candidates for this PC.
        _active_sessions.pop(self._render.session_id, None)
        if self._pc is not None:
            try:
                await self._pc.close()
            except Exception:
                pass
            self._pc = None
        self._commands_dc = None
