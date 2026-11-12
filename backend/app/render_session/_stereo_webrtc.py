"""Stereo frames for a headset as H.264 video over WebRTC (issue #189, VR epic #166).

Over Wi-Fi the JPEG stereo frames of the render session's WebSocket limit
the refined frame rate and quality. Here the WebSocket that owns a session's
stereo mode (routers/render.py) also negotiates a WebRTC peer of its own: the
two eyes, side by side as on the JPEG path, travel as one H.264 video frame,
while the frame's JSON header and its depth image stay on the WebSocket (a
``VXR1`` message whose colour is ``h264-webrtc`` with no payload).

Pairing a video frame with its pose. The pose id is drawn into the frame: a
stripe of ``STRIPE_ROWS`` rows under the two eyes holds ``STRIPE_CELLS``
black or white cells (a start pattern, the low 32 bits of the pose id, a
CRC-8 of them, an end pattern). The page reads the stripe of every decoded
frame, looks up the header with that id, and crops the stripe off. The
pairing is then exact by construction, whatever the transport does to the
frames (drops, repeats, keyframe resends) and without the RTP timestamps,
whose origin aiortc chooses at random. A stripe that does not decode (start
or end pattern wrong, CRC wrong) makes the frame unusable: it is dropped,
never shown with another frame's pose.

Encoding. The render process encodes each frame where it renders it (YUV
4:2:0 straight from its RGBA, the stripe drawn in the Y plane) and hands
back tens of kilobytes of H.264 instead of megabytes of pixels; the
backend's track gives aiortc these packets as they are, so the encoder, its
rate control and its keyframes are this module's: ``h264_nvenc`` on Linux,
else ``libx264`` (``VISIVO_XR_H264_ENCODER`` names one); an encoder that
holds a frame back (no packet for the first frame, as VideoToolbox does) is
given up for the next one. Constrained baseline, no B-frames, BT.709 limited
range signalled in the stream. A keyframe is sent first, on the browser's
request (PLI/FIR, a lost packet: the session renders the pose again), and
every ``KEYFRAME_EVERY`` frames.

The functions up to the encoder need numpy only (PyAV for the YUV ones), so
the stripe can be tested without aiortc; aiortc is imported when a link is made.
"""
from __future__ import annotations

import asyncio
import collections
import logging
import os
import sys
import time
from fractions import Fraction

import numpy as np

logger = logging.getLogger("visivo.render.xr_rtc")

# ── The pose id stripe ───────────────────────────────────────────────────────

#: Rows of the stripe under the eyes (one H.264 macroblock row); one more when
#: the eye height is odd, so the video's height is even (4:2:0).
STRIPE_ROWS = 16
#: Cells across the frame: start pattern, 32 id bits (most significant
#: first), CRC-8 of the id's four bytes (big-endian), end pattern.
START, END = (1, 1, 0, 1), (0, 1, 0, 0)
ID_BITS, CRC_BITS = 32, 8
STRIPE_CELLS = len(START) + ID_BITS + CRC_BITS + len(END)      # 48
#: Narrowest cell, in pixels: a frame narrower than STRIPE_CELLS * MIN_CELL_PX
#: (each eye under 192 pixels) is not sent as video.
MIN_CELL_PX = 8
ID_MASK = (1 << ID_BITS) - 1


def crc8(data: bytes) -> int:
    """CRC-8, polynomial 0x07, initial value 0 (CRC-8/SMBUS)."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def stripe_bits(pose_id: int) -> list[int]:
    """The 48 cells of the stripe for ``pose_id`` (its low 32 bits)."""
    low = int(pose_id) & ID_MASK
    crc = crc8(low.to_bytes(4, "big"))
    bits = [(low >> (ID_BITS - 1 - i)) & 1 for i in range(ID_BITS)]
    bits += [(crc >> (CRC_BITS - 1 - i)) & 1 for i in range(CRC_BITS)]
    return [*START, *bits, *END]


def stripe_cell(frame_width: int) -> int:
    """Width of one cell for a frame this wide; 0 when it is too narrow."""
    cell = int(frame_width) // STRIPE_CELLS
    return cell if cell >= MIN_CELL_PX else 0


def stripe_rows(eye_height: int) -> int:
    return STRIPE_ROWS + (int(eye_height) & 1)


def stripe_layout(frame_width: int, eye_height: int) -> dict:
    """Where the stripe is, as the frame header gives it to the page."""
    return {"y": int(eye_height), "rows": stripe_rows(eye_height), "cells": STRIPE_CELLS,
            "cell": stripe_cell(frame_width), "id_bits": ID_BITS}


def draw_stripe(pose_id: int, frame_width: int, rows: int = STRIPE_ROWS) -> np.ndarray:
    """The stripe as grey levels (rows, frame_width): 255 for a 1, 0 for a 0,
    black past the last cell."""
    cell = stripe_cell(frame_width)
    if not cell:
        raise ValueError(f"a frame {frame_width} pixels wide is too narrow for the stripe")
    line = np.zeros(frame_width, np.uint8)
    for i, b in enumerate(stripe_bits(pose_id)):
        if b:
            line[i * cell:(i + 1) * cell] = 255
    return np.broadcast_to(line, (rows, frame_width)).copy()


def read_stripe(row: np.ndarray) -> int | None:
    """The pose id (low 32 bits) from one row of grey levels across the
    stripe (the middle row is best), or None when it does not decode. Each
    cell is the mean of its middle half, thresholded at 128."""
    row = np.asarray(row, dtype=np.float32).reshape(-1)
    cell = stripe_cell(row.size)
    if not cell:
        return None
    lo, hi = cell // 4, cell - cell // 4
    bits = [int(row[i * cell + lo:i * cell + hi].mean() >= 128.0) for i in range(STRIPE_CELLS)]
    n0 = len(START)
    if tuple(bits[:n0]) != START or tuple(bits[-len(END):]) != END:
        return None
    value = 0
    for b in bits[n0:n0 + ID_BITS]:
        value = (value << 1) | b
    crc = 0
    for b in bits[n0 + ID_BITS:n0 + ID_BITS + CRC_BITS]:
        crc = (crc << 1) | b
    return value if crc == crc8(value.to_bytes(4, "big")) else None


def video_image(picture, pose_id: int):
    """The video frame: the two eyes, rows top first, with the stripe under
    them. ``picture`` is RGB (h, 2w, 3), or YUV 4:2:0 as three planes (Y of
    h x 2w, U and V of h/2 x w, h even); the result is the same kind."""
    if not isinstance(picture, tuple):
        h, width = picture.shape[:2]
        stripe = draw_stripe(pose_id, width, stripe_rows(h))
        out = np.empty((h + stripe.shape[0], width, 3), np.uint8)
        out[:h] = picture
        out[h:] = stripe[:, :, None]
        return out
    y, u, v = picture
    h, width = y.shape
    rows = stripe_rows(h)                     # 16: h is even here
    # Limited range: a white cell is Y 235, a black one Y 16; no colour.
    stripe = (16 + draw_stripe(pose_id, width, rows).astype(np.uint16) * 219 // 255).astype(np.uint8)
    planes = [np.concatenate([y, stripe])]
    for c in (u, v):
        planes.append(np.concatenate([c, np.full((rows // 2, c.shape[1]), 128, np.uint8)]))
    return tuple(planes)


def yuv_video_frame(planes):
    """A PyAV yuv420p frame from three planes (rows top first)."""
    import av
    y = planes[0]
    frame = av.VideoFrame(y.shape[1], y.shape[0], "yuv420p")
    for dst, src in zip(frame.planes, planes):
        view = np.asarray(memoryview(dst)).reshape(dst.height, dst.line_size)
        view[:, :dst.width] = src
    return frame


def rgba_to_yuv420(rgba: np.ndarray):
    """The render's RGBA (h, w, 4, rows bottom first) as YUV 4:2:0 planes,
    BT.709 limited range, rows top first: half the bytes of RGB to hand from
    the render process to the backend, and no conversion left for the
    encoder. None when PyAV is missing or a dimension is odd."""
    h, w = rgba.shape[:2]
    if h % 2 or w % 2:
        return None
    try:
        import av
    except ImportError:
        return None
    frame = av.VideoFrame.from_ndarray(np.ascontiguousarray(rgba), format="rgba")
    frame = frame.reformat(format="yuv420p", dst_colorspace="ITU709", dst_color_range="MPEG")
    return tuple(np.asarray(memoryview(p)).reshape(p.height, p.line_size)[::-1, :p.width].copy()
                 for p in frame.planes)


# ── The encoder ──────────────────────────────────────────────────────────────

#: A keyframe at least this often (frames): a decoder that lost one recovers.
KEYFRAME_EVERY = 120
#: The RTP clock of video.
VIDEO_CLOCK = 90000
#: Defaults of a link: constant quality (x264's CRF scale; lower is better),
#: and the bit rate the encoder may not exceed, in kbit/s.
DEFAULT_CRF = 14
DEFAULT_MAX_KBPS = 40000
MIN_CRF, MAX_CRF = 10, 40
MIN_KBPS, MAX_KBPS = 1000, 200000


def encoder_candidates() -> list[str]:
    """The encoders tried, in order."""
    named = os.environ.get("VISIVO_XR_H264_ENCODER", "").strip()
    if named:
        return [named]
    # VideoToolbox holds a frame back (its output is one frame late), which
    # would keep the newest refined frame in the encoder until the next one.
    if sys.platform.startswith("linux"):
        return ["h264_nvenc", "libx264"]
    return ["libx264"]


class H264Encoder:
    """Frames (h, w, 3 RGB) in, H.264 Annex B packets out, one per frame."""

    def __init__(self, crf: int = DEFAULT_CRF, max_kbps: int = DEFAULT_MAX_KBPS,
                 candidates: list[str] | None = None,
                 options: dict[str, str] | None = None) -> None:
        self.crf, self.max_kbps = int(crf), int(max_kbps)
        self.options = dict(options or {})      # added to (or overriding) the encoder's
        self.candidates = candidates or encoder_candidates()
        self.name: str | None = None
        self._codec = None
        self._size: tuple[int, int] | None = None
        self._since_key = 0
        self.last_keyframe = False

    def _open(self, name: str, width: int, height: int):
        import av
        codec = av.CodecContext.create(name, "w")
        codec.width, codec.height = width, height
        codec.pix_fmt = "yuv420p"
        codec.time_base = Fraction(1, VIDEO_CLOCK)
        codec.framerate = Fraction(72, 1)
        codec.gop_size = 100000          # keyframes are asked for (see encode)
        codec.max_b_frames = 0
        # BT.709, limited range, as the frames are converted.
        for attr, value in (("color_primaries", 1), ("color_trc", 1), ("colorspace", 1),
                            ("color_range", 1)):
            try:
                setattr(codec, attr, value)
            except Exception:   # noqa: BLE001 - an older PyAV: unsignalled
                pass
        rate = 1000 * self.max_kbps
        if name == "libx264":
            # Without a global header libx264 repeats SPS/PPS before every
            # keyframe (Annex B), which a decoder joining late needs.
            codec.options = {"preset": "superfast", "tune": "zerolatency", "profile": "baseline",
                             "crf": str(self.crf), "maxrate": str(rate), "bufsize": str(rate)}
        elif name == "h264_videotoolbox":
            codec.bit_rate = rate // 2
            codec.options = {"realtime": "1", "profile": "constrained_baseline",
                             "allow_sw": "1", "maxrate": str(rate)}
        elif name == "h264_nvenc":
            codec.bit_rate = rate // 2
            codec.options = {"preset": "p4", "tune": "ull", "rc": "vbr", "profile": "baseline",
                             "maxrate": str(rate), "bufsize": str(rate), "zerolatency": "1",
                             "delay": "0", "rc-lookahead": "0", "forced-idr": "1"}
        if self.options:
            codec.options = {**codec.options, **self.options}
        codec.open(strict=False)
        return codec

    def encode(self, image, pts: int, keyframe: bool = False) -> list:
        """The packets of one frame (normally one): ``image`` RGB (h, w, 3) or
        YUV 4:2:0 planes (video_image). ``pts`` in 1/90000 s."""
        import av
        if isinstance(image, tuple):           # already YUV 4:2:0 planes (video_image)
            height, width = image[0].shape
            frame = yuv_video_frame(image)
        else:
            height, width = image.shape[:2]
            frame = av.VideoFrame.from_ndarray(np.ascontiguousarray(image), format="rgb24")
            try:
                frame = frame.reformat(format="yuv420p", dst_colorspace="ITU709",
                                       dst_color_range="MPEG")
            except TypeError:   # an older PyAV
                frame = frame.reformat(format="yuv420p")
        frame.pts = int(pts)
        frame.time_base = Fraction(1, VIDEO_CLOCK)
        if self._codec is not None and self._size == (width, height):
            key = keyframe or self._since_key >= KEYFRAME_EVERY
            frame.pict_type = av.video.frame.PictureType.I if key else av.video.frame.PictureType.NONE
            packets = list(self._codec.encode(frame))
        else:
            # A new size: the first encoder that opens and answers this frame
            # at once (one that holds frames back is no use here). Every
            # candidate is tried again, the one in use first: an encoder may
            # take one size and refuse another (NVENC above 4096 pixels).
            self._codec, key, packets, errors = None, True, [], []
            names = ([self.name] if self.name else []) + [n for n in self.candidates if n != self.name]
            for name in names:
                try:
                    codec = self._open(name, width, height)
                    frame.pict_type = av.video.frame.PictureType.I
                    packets = list(codec.encode(frame))
                except Exception as exc:   # noqa: BLE001 - try the next one
                    errors.append(f"{name}: {exc}")
                    continue
                if not packets:
                    errors.append(f"{name}: holds frames back")
                    continue
                self._codec, self._size = codec, (width, height)
                if name != self.name:
                    logger.info("[xr-rtc] H.264 encoder %s, %dx%d", name, width, height)
                self.name = name
                break
            if self._codec is None:
                self._size = None
                raise RuntimeError("no H.264 encoder: " + "; ".join(errors))
        self._since_key = 0 if key else self._since_key + 1
        self.last_keyframe = bool(key)          # this frame is a keyframe
        for p in packets:
            p.time_base = Fraction(1, VIDEO_CLOCK)
            if p.pts is None:
                p.pts = int(pts)
        return packets




# ── In the render process ────────────────────────────────────────────────────
#
# The frames are encoded where they are rendered: the render process keeps the
# encoder of the connection's link and hands back the H.264 bytes of each
# frame (tens of kB) instead of its pixels (megabytes through the pipe, and
# an encode on the backend's event-loop process). The backend tells it with
# commands, in order with the poses: ``xr_video`` (a link is up: its settings,
# a fresh encoder, a keyframe first; ``"off": true``: no link, JPEG frames),
# ``xr_video_keyframe`` (the browser lost a picture: the next frame, rendered
# again at once since every command marks the scene, is a keyframe).


class ChildVideo:
    """The encoder of a stereo session's video link, in the render process."""

    def __init__(self, crf: int = DEFAULT_CRF, max_kbps: int = DEFAULT_MAX_KBPS) -> None:
        self.encoder = H264Encoder(crf, max_kbps)
        self.keyframe = True
        self.frames = 0


#: Largest video frame side, in pixels (both eyes side by side, stripe
#: included): what H.264 decoders (a headset's included) and NVENC take.
#: Larger eyes go as JPEG.
MAX_VIDEO_SIDE = 4096


def apply_video_command(session, command: dict) -> None:
    """``xr_video`` / ``xr_video_keyframe`` on the render thread."""
    if command.get("type") == "xr_video":
        if command.get("off"):
            session._xr_video = None
        else:
            crf, kbps = _offer_settings(command)
            session._xr_video = ChildVideo(crf, kbps)
    elif command.get("type") == "xr_video_keyframe":
        video = getattr(session, "_xr_video", None)
        if video is not None:
            video.keyframe = True


def encode_eyes(session, rgba: np.ndarray, pose_id: int):
    """The two eyes (RGBA, rows bottom first) as one H.264 frame with the pose
    id stripe: ``(bytes, color header fields)``; or None when the session has
    no link, the eyes are too narrow or too large, PyAV is missing, or the
    encoder failed (the frame is then a JPEG). A failure also turns the
    session's video off and is kept in ``session._xr_video_error`` for the
    frame's header, so the backend can tell the page and close the link."""
    video = getattr(session, "_xr_video", None)
    if video is None or not stripe_cell(rgba.shape[1]):
        return None
    h, width = rgba.shape[:2]
    if width > MAX_VIDEO_SIDE or h + STRIPE_ROWS + 1 > MAX_VIDEO_SIDE:
        return None
    try:
        if h % 2:
            # An odd eye height: one black row under the eyes (the first of a
            # 17-row stripe, as stripe_layout says), so the frame's height is even.
            rgba = np.concatenate([np.zeros_like(rgba[:1]), rgba])
        yuv = rgba_to_yuv420(rgba)
        if yuv is None:
            return None
        key, video.keyframe = video.keyframe, False
        packets = video.encoder.encode(video_image(yuv, pose_id), video.frames, key)
        video.frames += 1
        data = b"".join(bytes(p) for p in packets)
        key = video.encoder.last_keyframe
    except Exception as exc:   # noqa: BLE001 - the frame goes as JPEG instead
        logger.warning("[xr-rtc] H.264 encoding failed, back to JPEG: %s", exc)
        session._xr_video = None
        session._xr_video_error = f"H.264 encoding failed: {type(exc).__name__}: {exc}"
        return None
    return data, {"keyframe": bool(key), "encoder": video.encoder.name}


def take_video_error(session) -> str | None:
    """The encoder's failure since the last call, once."""
    error = getattr(session, "_xr_video_error", None)
    session._xr_video_error = None
    return error



# ── The offer ────────────────────────────────────────────────────────────────

#: Largest SDP offer accepted, in bytes (a browser's single video offer is a
#: few kilobytes).
MAX_OFFER_BYTES = 64 * 1024


def _usable_candidate(address: str) -> bool:
    """A remote candidate worth checking: an IP address that is not loopback,
    multicast or unspecified (private, link-local, CGNAT/Tailscale 100.64/10
    and public addresses are all kept: a headset on a network with public
    IPv6 or IPv4 addresses must still connect), or a browser's mDNS name
    (``<uuid>.local``). A best-effort filter against offers that would make
    the backend send ICE checks to its own loopback or to multicast groups,
    not a firewall."""
    import ipaddress
    if address.lower().endswith(".local"):
        return True
    try:
        ip = ipaddress.ip_address(address.split("%")[0])
    except ValueError:
        return False
    return not (ip.is_loopback or ip.is_multicast or ip.is_unspecified)


def check_offer(sdp) -> tuple[str | None, str | None]:
    """``(sdp, None)``: the offer as it may be answered, its loopback,
    multicast and unspecified remote candidates taken out
    (_usable_candidate); ``(None, why)`` when it is refused: not a string,
    larger than MAX_OFFER_BYTES, or not exactly one video section that only
    receives (what the page offers)."""
    if not isinstance(sdp, str) or not sdp.strip():
        return None, "xr_rtc_offer needs an sdp."
    if len(sdp.encode("utf-8", "replace")) > MAX_OFFER_BYTES:
        return None, f"The offer is larger than {MAX_OFFER_BYTES // 1024} KB."
    lines = sdp.replace("\r\n", "\n").split("\n")
    media = [ln for ln in lines if ln.startswith("m=")]
    if len(media) != 1 or not media[0].startswith("m=video "):
        return None, "The offer must have exactly one video section."
    section = lines[lines.index(media[0]):]
    directions = {ln[2:] for ln in section if ln in ("a=recvonly", "a=sendrecv", "a=sendonly",
                                                    "a=inactive")}
    if directions != {"recvonly"}:
        return None, "The offer's video section must only receive (a=recvonly)."
    kept = []
    for ln in lines:
        if ln.startswith("a=candidate:"):
            parts = ln.split()
            if len(parts) < 6 or not _usable_candidate(parts[4]):
                continue
        kept.append(ln)
    return "\r\n".join(kept), None


# ── The track and the link (aiortc), in the backend ──────────────────────────

#: H.264 frames the track holds for aiortc at most; beyond (the network or
#: aiortc cannot keep up) they are all dropped and a keyframe is asked for.
MAX_QUEUED_FRAMES = 4

_TRACK_CLASS = None


def _track_class():
    """``StereoVideoTrack``, defined on first use so that importing this
    module does not need aiortc."""
    global _TRACK_CLASS
    if _TRACK_CLASS is not None:
        return _TRACK_CLASS
    import av
    from aiortc import MediaStreamTrack
    from aiortc.mediastreams import MediaStreamError

    class StereoVideoTrack(MediaStreamTrack):
        """Hands aiortc the H.264 frames the render process encoded, in order
        (each references the one before), stamped with the wall clock: the
        browser's jitter buffer sees them as far apart as they were made.

        The reference chain is kept whole: once a frame is missing (dropped
        with a link that was not connected, or a full queue) every frame up
        to the next keyframe is dropped too, and ``on_gap()`` asks for that
        keyframe at once."""

        kind = "video"

        def __init__(self, on_gap=lambda: None) -> None:
            super().__init__()
            self._queue: collections.deque = collections.deque()
            self._wake = asyncio.Event()
            self._t0 = time.monotonic()
            self._pts = -1
            self.need_keyframe = True          # nothing sent yet
            self.on_gap = on_gap
            self.stats = {"frames": 0, "keyframes": 0, "bytes": 0, "dropped": 0, "overflows": 0}

        def gap(self) -> None:
            """A frame will not be sent: wait for, and ask for, a keyframe."""
            self.need_keyframe = True
            self.on_gap()

        def push(self, data: bytes, keyframe: bool) -> bool:
            """Queues a frame; False when it is dropped (see the class)."""
            if self.need_keyframe and not keyframe:
                # Still waiting for a keyframe (the one asked for may itself
                # have been dropped): ask again (the link rate-limits it).
                self.stats["dropped"] += 1
                self.on_gap()
                return False
            if len(self._queue) >= MAX_QUEUED_FRAMES:
                self.stats["overflows"] += 1
                self.stats["dropped"] += len(self._queue)
                self._queue.clear()
                if not keyframe:
                    self.stats["dropped"] += 1
                    self.gap()
                    return False
            self.need_keyframe = False
            self._queue.append(data)
            self.stats["frames"] += 1
            self.stats["keyframes"] += int(keyframe)
            self.stats["bytes"] += len(data)
            self._wake.set()
            return True

        def stop(self) -> None:
            super().stop()
            self._wake.set()

        async def recv(self):
            while not self._queue:
                if self.readyState != "live":
                    raise MediaStreamError
                self._wake.clear()
                await self._wake.wait()
            data = self._queue.popleft()
            self._pts = max(self._pts + 1, int((time.monotonic() - self._t0) * VIDEO_CLOCK))
            packet = av.Packet(data)
            packet.pts = self._pts
            packet.time_base = Fraction(1, VIDEO_CLOCK)
            return packet

    _TRACK_CLASS = StereoVideoTrack
    return StereoVideoTrack


def rtc_available() -> str | None:
    """None when this backend can make a link, else why not."""
    try:
        import aiortc  # noqa: F401
        import av  # noqa: F401
    except ImportError:
        return "WebRTC transport not built in this backend."
    return None


def _offer_settings(command: dict) -> tuple[int, int]:
    def bounded(key, default, lo, hi):
        v = command.get(key, default)
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            return default
        return int(max(lo, min(hi, v)))
    return (bounded("crf", DEFAULT_CRF, MIN_CRF, MAX_CRF),
            bounded("max_kbps", DEFAULT_MAX_KBPS, MIN_KBPS, MAX_KBPS))


#: The browser's keyframe requests (PLI, FIR) are passed to the render
#: process at most this often: a browser repeats them while it waits. A
#: keyframe the backend needs itself (a frame not sent) is asked at once.
KEYFRAME_REQUEST_S = 0.25


class StereoRtcLink:
    """The WebRTC peer of one stereo WebSocket: a receive-only H.264 track
    for the browser. Not one of the session's mono peers (webrtc_session.py):
    it carries only the stereo frames of the connection that made it.
    ``send_command(cmd)``: a command to the session (``xr_video``,
    ``xr_video_keyframe``); it may return an awaitable, which ``close``
    awaits (so that its ``xr_video`` off reaches the session before anything
    sent after the close)."""

    def __init__(self, session_id: str, crf: int = DEFAULT_CRF,
                 max_kbps: int = DEFAULT_MAX_KBPS, send_command=None) -> None:
        self.session_id = session_id
        self.crf, self.max_kbps = int(crf), int(max_kbps)
        self.send_command = send_command or (lambda cmd: None)
        self.pc = None
        self.track = None
        self.frames = 0
        self.closed = False
        self._closing: asyncio.Future | None = None   # the one close, shared (see close)
        self._last_keyframe_request = -KEYFRAME_REQUEST_S
        self._retry = None                     # a keyframe request held back by the rate limit
        self.clock = time.monotonic

    @classmethod
    def from_offer_command(cls, session_id: str, command: dict, send_command=None):
        crf, kbps = _offer_settings(command)
        return cls(session_id, crf, kbps, send_command)

    def video_command(self) -> dict:
        """The session command that turns its encoder on with this link's settings."""
        return {"type": "xr_video", "crf": self.crf, "max_kbps": self.max_kbps}

    def request_keyframe(self) -> None:
        """A keyframe asked for: the browser's (PLI, FIR), or the track's (a
        frame not sent). Passed to the session at most every 0.25 s, since
        each one renders the pose again. One asked for within that time is
        sent at its end if the track is still waiting for a keyframe (the one
        before may itself have been dropped, and a still view pushes no frame
        that would ask again)."""
        if self.closed:
            return
        now = self.clock()
        wait = self._last_keyframe_request + KEYFRAME_REQUEST_S - now
        if wait <= 0:
            self._last_keyframe_request = now
            self.send_command({"type": "xr_video_keyframe"})
            return
        if self._retry is None:
            try:
                loop = asyncio.get_running_loop()
            except RuntimeError:
                return
            self._retry = loop.call_later(wait, self._retry_keyframe)

    def _retry_keyframe(self) -> None:
        self._retry = None
        if self.track is not None and getattr(self.track, "need_keyframe", False):
            self.request_keyframe()

    #: The track's request after a frame not sent: the same, rate-limited.
    force_keyframe = request_keyframe

    def _on_connected(self) -> None:
        """The peer has just connected. The keyframe rendered for this link
        may have been dropped before (no track yet, or not connected); a still
        view pushes no frame that would meet the gate and ask again, so ask
        now if the track is still waiting for one."""
        if self.track is not None and getattr(self.track, "need_keyframe", False):
            self.request_keyframe()

    async def answer(self, sdp: str, sdp_type: str = "offer") -> dict:
        from aiortc import RTCConfiguration, RTCPeerConnection, RTCSessionDescription
        from aiortc import rtcrtpsender
        from ..webrtc_session import _force_h264_on
        sdp, why = check_offer(sdp)
        if why is not None:
            raise ValueError(why)
        # A keyframe of a large frame is a few hundred RTP packets: keep
        # enough of them to answer the browser's NACKs (aiortc keeps 128).
        if getattr(rtcrtpsender, "RTP_HISTORY_SIZE", 0) < 4096:
            rtcrtpsender.RTP_HISTORY_SIZE = 4096
        # Host candidates only: the headset is on the Mac's network (aiortc
        # would otherwise ask a public STUN server, seconds on a LAN without one).
        pc = RTCPeerConnection(RTCConfiguration(iceServers=[]))
        self.pc = pc
        self.track = _track_class()(self.force_keyframe)
        sender = pc.addTrack(self.track)
        # The browser's keyframe requests (PLI, FIR) reach aiortc's encoder
        # only for raw frames; these frames are encoded by the render process.
        original = sender._send_keyframe

        def keyframe() -> None:
            original()
            self.request_keyframe()
        sender._send_keyframe = keyframe
        _force_h264_on(pc)

        @pc.on("connectionstatechange")
        async def _state() -> None:
            logger.info("[xr-rtc] %s peer %s", self.session_id[:8] + "…", pc.connectionState)
            if pc.connectionState in ("failed", "closed"):
                await self.close()
            elif pc.connectionState == "connected":
                self._on_connected()

        await pc.setRemoteDescription(RTCSessionDescription(sdp=sdp, type=sdp_type))
        answer = await pc.createAnswer()
        await pc.setLocalDescription(answer)
        return {"sdp": pc.localDescription.sdp, "sdp_type": pc.localDescription.type}

    def _sending(self) -> bool:
        """The negotiated video section sends this link's track."""
        for t in self.pc.getTransceivers():
            if t.sender is not None and t.sender.track is self.track and \
                    getattr(t, "currentDirection", None) in ("sendonly", "sendrecv"):
                return True
        return False

    @property
    def ready(self) -> bool:
        """Connected, and sending the track: frames go as video from now on."""
        return (not self.closed and self.pc is not None and self.track is not None
                and self.pc.connectionState == "connected" and self._sending())

    def frame_message(self, frame, ready: float | None, sent: float | None) -> bytes | None:
        """Hands an ``h264`` frame's video to the track and returns the
        WebSocket message (header and depth, no colour) for it; None when the
        frame is not sent: the link is closed (the session has been told to
        make JPEGs again), not connected at the moment, or the track drops it
        to keep the reference chain whole (a keyframe is then asked for)."""
        from ._stereo import XRFrame
        if self.closed:
            return None
        if self.track is None:
            # Before the answer made the track: the keyframe is lost; the
            # track starts waiting for one, asked for once connected.
            return None
        c = frame.header["color"]
        keyframe = bool(c.get("keyframe"))
        if not self.ready:
            # Not connected at the moment: no keyframe asked for now (it would
            # be re-rendered and dropped again, in a loop); the first frame
            # pushed once connected meets the gate, which asks for one.
            self.track.need_keyframe = True
            return None
        if not self.track.push(frame.color, keyframe):
            return None
        width, height = int(c["width"]), int(c["height"])
        self.frames += 1
        header = dict(frame.header)
        header["color"] = {"format": "h264-webrtc", "width": width, "height": height,
                           "stripe": stripe_layout(width, height)}
        header["video"] = {"frame": self.frames, "id": int(frame.header["pose_id"]) & ID_MASK,
                           "keyframe": keyframe, "bytes": len(frame.color),
                           "encoder": c.get("encoder")}
        return XRFrame(header, b"", frame.depth).to_bytes(ready=ready, sent=sent)

    def stats(self) -> dict:
        return dict(self.track.stats) if self.track is not None else {}

    async def close(self) -> None:
        """Close the link; every caller waits for the same close to finish.

        Two callers are usual (the peer failing, the connection or a new offer
        replacing the link). One that returned while the other was still
        awaiting the encoder's off let a replacement link turn the encoder on
        before this one turned it off. The close runs as one task, shielded,
        so a caller cancelled meanwhile does not cut it short either."""
        await asyncio.shield(self.close_task())

    def close_task(self) -> asyncio.Future:
        """The link's one close, started on the first call (see :meth:`close`)."""
        if self._closing is None:
            self.closed = True
            self._closing = asyncio.ensure_future(self._close())
        return self._closing

    async def _close(self) -> None:
        if self._retry is not None:
            self._retry.cancel()
            self._retry = None
        pc, self.pc = self.pc, None
        if self.track is not None:
            self.track.stop()
        try:
            # The render process back to JPEG frames (rendered again at once),
            # before anything sent after this close.
            pending = self.send_command({"type": "xr_video", "off": True})
            if pending is not None and hasattr(pending, "__await__"):
                await pending
        except Exception:   # noqa: BLE001 - the session may be gone
            logger.debug("[xr-rtc] xr_video off failed", exc_info=True)
        finally:
            if pc is not None:
                try:
                    await pc.close()
                except Exception:   # noqa: BLE001 - already closed
                    pass
