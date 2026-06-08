"""WebRTC loopback spike — Day 2: NVENC hardware encoder.

Replaces aiortc's default ``libx264`` software encoder with
``h264_nvenc`` so the encode runs on the V100 instead of the CPU.
Same loopback topology as ``spike_h265_webrtc_loopback.py``; the
delta is the encoder swap + telemetry that proves NVENC is actually
in use (encode wall-time, codec name reported on the encoder
context).

Strategy:
  1. Subclass ``aiortc.codecs.h264.H264Encoder`` and override the
     codec selection in ``_create_encoder``. Same API contract as
     the stock encoder so aiortc's RTP packetiser keeps working.
  2. Monkey-patch ``aiortc.codecs.CODECS`` so the negotiator picks
     our subclass instead of the default H264 encoder.
  3. Re-run the same 5-second loopback. Compare timing against the
     Day-1 baseline.

If NVENC is live, we expect:
  * Inter-frame mean drops below the Day-1 14 ms (was libx264 limit)
  * Encode wall-time per frame << 5 ms (vs. ~10 ms for libx264)
  * CPU usage near-zero (encode is on the GPU)

Run inside the SIF:

  apptainer exec --nv backend-egl-h265.sif \
      /opt/conda/bin/python apptainer/spike_h265_webrtc_nvenc.py
"""
from __future__ import annotations

import asyncio
import time
from fractions import Fraction

import numpy as np
from aiortc import RTCPeerConnection, MediaStreamTrack
from av import VideoFrame


# ── NVENC encoder swap ───────────────────────────────────────────────────────
#
# aiortc 1.9 builds its H264 encoder via the module-level helper
# ``aiortc.codecs.h264.create_encoder_context(codec_name, w, h, bitrate)``.
# ``_encode_frame`` calls it first with ``"h264_omx"`` (which fails on
# our SIF — Raspberry Pi-specific), then with ``"libx264"`` (the
# software fallback that the Day-1 spike used). We patch the helper so
# any request for ``"h264_omx"`` or ``"libx264"`` is silently redirected
# to ``"h264_nvenc"`` with the NVENC-correct option set.
#
# Why patch the helper rather than the H264Encoder class: clean. One
# function, one purpose, no class-level state to thread through.


def _install_nvenc_encoder() -> str:
    """Replace aiortc's encoder factory with one that builds h264_nvenc."""
    from aiortc.codecs import h264 as h264_mod
    from aiortc.codecs import vpx as vpx_mod
    import av

    encode_times_ms: list[float] = []
    encode_times_ms_singleton[0] = encode_times_ms

    # Diagnostic: log whenever the VP8 encoder is asked to encode — if
    # we see this firing it means the SDP negotiation picked VP8 (aiortc
    # 1.9's default) and our H264/NVENC path is bypassed entirely.
    vpx_encode_calls = [0]
    original_vpx_encode = vpx_mod.Vp8Encoder.encode

    def _vpx_encode_traced(self, frame, force_keyframe=False):
        vpx_encode_calls[0] += 1
        if vpx_encode_calls[0] == 1:
            print("[vpx-trace] Vp8Encoder.encode() called for the first time —"
                  " SDP picked VP8, not H264. Codec forcing needed.")
        return original_vpx_encode(self, frame, force_keyframe)

    vpx_mod.Vp8Encoder.encode = _vpx_encode_traced
    vpx_encode_calls_singleton[0] = vpx_encode_calls

    original_create = h264_mod.create_encoder_context

    def create_encoder_context_nvenc(
        codec_name: str, width: int, height: int, bitrate: int
    ):
        # codec_name is whatever aiortc asked for — we ignore it and
        # always build an NVENC context. If NVENC isn't actually
        # available the encoder would fail at the .open() call below
        # and we'd fall back to the original factory.
        try:
            codec = av.CodecContext.create("h264_nvenc", "w")
            codec.width = width
            codec.height = height
            codec.pix_fmt = "yuv420p"
            codec.framerate = Fraction(30, 1)
            # aiortc expects the encoder to emit RTP-compatible
            # timestamps at the 90 kHz clock.
            codec.time_base = Fraction(1, 90000)
            codec.bit_rate = bitrate
            codec.gop_size = 30
            # NVENC option keys (SDK 11+):
            #   preset: p1=fastest..p7=slowest. p1 for low-latency.
            #   tune:   ll = low latency, ull = ultra-low latency.
            #   rc:     cbr = constant bitrate (smoother for WebRTC).
            #   delay:  0 = no encoder reorder lookahead, lowest latency.
            try:
                codec.options = {
                    "preset": "p1",
                    "tune": "ll",
                    "rc": "cbr",
                    "delay": "0",
                }
            except Exception:
                pass
            codec.open(strict=False)
            # NVENC doesn't need the buffering path aiortc uses for
            # h264_omx (which packs differently). Return ``False`` for
            # ``codec_buffering`` so packets flow immediately.
            return codec, False
        except Exception as e:
            print(f"[nvenc-patch] WARN: NVENC build failed ({e}); "
                  f"falling back to original {codec_name!r} factory.")
            return original_create(codec_name, width, height, bitrate)

    h264_mod.create_encoder_context = create_encoder_context_nvenc

    # Instrument H264Encoder.encode to measure per-frame wall time.
    original_encode = h264_mod.H264Encoder.encode

    def _encode_timed(self, frame, force_keyframe=False):
        t0 = time.perf_counter()
        result = original_encode(self, frame, force_keyframe)
        encode_times_ms.append((time.perf_counter() - t0) * 1000.0)
        return result

    h264_mod.H264Encoder.encode = _encode_timed
    return "h264_nvenc"


encode_times_ms_singleton: list[list[float]] = [[]]
vpx_encode_calls_singleton: list[list[int]] = [[0]]


def _force_h264_only(pc) -> None:
    """Restrict the video transceiver to H264 codecs only.

    aiortc 1.9 defaults to a VP8/VP9/H264 codec list with VP8 first, so
    the SDP offer/answer dance picks VP8. We rebuild the codec
    preferences list with only the H264 variants so the negotiator has
    no choice — the encoder factory we patched above is now reachable.
    """
    from aiortc.rtcrtpsender import RTCRtpSender

    caps = RTCRtpSender.getCapabilities("video")
    h264_codecs = [
        c for c in caps.codecs
        if c.mimeType.lower() == "video/h264"
    ]
    if not h264_codecs:
        print("[force-h264] no H264 capability available — leaving default")
        return
    for transceiver in pc.getTransceivers():
        if transceiver.kind == "video":
            transceiver.setCodecPreferences(h264_codecs)
            print(f"[force-h264] transceiver {transceiver.mid} restricted to "
                  f"{len(h264_codecs)} H264 codec(s)")


# ── Synthetic frame source (same as Day 1) ───────────────────────────────────


class SyntheticVideoTrack(MediaStreamTrack):
    kind = "video"

    def __init__(self, width: int = 640, height: int = 480, fps: float = 30.0):
        super().__init__()
        self._w, self._h, self._fps = width, height, fps
        self._counter = 0
        self._start_wall = time.perf_counter()

    async def recv(self) -> VideoFrame:
        target_t = self._start_wall + self._counter / self._fps
        delta = target_t - time.perf_counter()
        if delta > 0:
            await asyncio.sleep(delta)

        t = self._counter / self._fps
        r = int(128 + 127 * np.sin(2 * np.pi * t * 0.30))
        g = int(128 + 127 * np.sin(2 * np.pi * t * 0.50))
        b = int(128 + 127 * np.sin(2 * np.pi * t * 0.70))
        arr = np.full((self._h, self._w, 3), (r, g, b), dtype=np.uint8)
        stripe_x = int((self._counter * 5) % self._w)
        arr[:, stripe_x : stripe_x + 10] = 255

        frame = VideoFrame.from_ndarray(arr, format="rgb24")
        frame.pts = self._counter
        frame.time_base = Fraction(1, int(self._fps))
        self._counter += 1
        return frame


# ── Loopback driver ──────────────────────────────────────────────────────────


async def run_spike(duration_s: float = 5.0) -> int:
    print("=" * 64)
    print("WebRTC loopback spike — Day 2: NVENC hardware encoder")
    print("=" * 64)

    codec_name = _install_nvenc_encoder()
    print(f"[setup] aiortc H264Encoder patched to use {codec_name!r}")
    encode_times_ms = encode_times_ms_singleton[0]

    server_pc = RTCPeerConnection()
    client_pc = RTCPeerConnection()

    source = SyntheticVideoTrack()
    server_pc.addTrack(source)
    # Restrict to H264 BEFORE the SDP offer is created — codec
    # preferences only affect newly-generated offers.
    _force_h264_only(server_pc)

    arrivals: list[float] = []
    consume_done = asyncio.Event()
    target_count = int(duration_s * 30.0)

    @client_pc.on("track")
    def on_track(received_track: MediaStreamTrack) -> None:
        print(f"[client] track received: kind={received_track.kind}")

        async def consume() -> None:
            try:
                while not consume_done.is_set():
                    frame = await received_track.recv()
                    arrivals.append(time.perf_counter())
                    if len(arrivals) % 30 == 0:
                        print(f"[client] {len(arrivals)} frames received "
                              f"({frame.width}x{frame.height}, "
                              f"{frame.format.name})")
                    if len(arrivals) >= target_count:
                        consume_done.set()
            except Exception as e:
                print(f"[client] consume() exception: {type(e).__name__}: {e}")
                consume_done.set()

        asyncio.create_task(consume())

    # SDP loopback
    offer = await server_pc.createOffer()
    await server_pc.setLocalDescription(offer)
    await client_pc.setRemoteDescription(server_pc.localDescription)
    answer = await client_pc.createAnswer()
    await client_pc.setLocalDescription(answer)
    await server_pc.setRemoteDescription(client_pc.localDescription)

    print(f"[signalling] PCs: server={server_pc.connectionState}, "
          f"client={client_pc.connectionState}")
    for line in server_pc.localDescription.sdp.splitlines():
        if line.startswith("a=rtpmap:") and ("VP" in line or "H264" in line
                                              or "H265" in line):
            print(f"[signalling] {line}")

    try:
        await asyncio.wait_for(consume_done.wait(), timeout=duration_s + 5.0)
    except asyncio.TimeoutError:
        print(f"[spike] timeout — only {len(arrivals)} frames received")

    await server_pc.close()
    await client_pc.close()

    # ── Stats ───────────────────────────────────────────────────────
    print()
    print("─" * 64)
    print(f"Frames received    : {len(arrivals)} (target {target_count})")
    if len(arrivals) >= 2:
        intervals_ms = np.diff(arrivals) * 1000.0
        print(f"Inter-frame mean   : {intervals_ms.mean():.2f} ms")
        print(f"Inter-frame p50    : {np.percentile(intervals_ms, 50):.2f} ms")
        print(f"Inter-frame p95    : {np.percentile(intervals_ms, 95):.2f} ms")
        print(f"Inter-frame p99    : {np.percentile(intervals_ms, 99):.2f} ms")
        print(f"Effective FPS      : {1000.0 / intervals_ms.mean():.2f}")
    vpx_called = vpx_encode_calls_singleton[0][0]
    if vpx_called > 0:
        print(f"⚠️  VP8 encoder fired {vpx_called} times — H264 path bypassed")

    if encode_times_ms:
        et = np.asarray(encode_times_ms)
        print()
        print(f"Encode samples     : {len(et)} (NVENC h264_nvenc)")
        print(f"Encode wall mean   : {et.mean():.2f} ms")
        print(f"Encode wall p50    : {np.percentile(et, 50):.2f} ms")
        print(f"Encode wall p95    : {np.percentile(et, 95):.2f} ms")
        print(f"Encode wall p99    : {np.percentile(et, 99):.2f} ms")
        verdict_ok = (et.mean() < 5.0 and np.percentile(et, 99) < 15.0)
        print()
        if verdict_ok:
            print("VERDICT: NVENC live — encode well under software floor. "
                  "Proceed to REST signalling.")
            return 0
        else:
            print(f"VERDICT: encode timing suspicious (mean={et.mean():.1f} ms). "
                  f"NVENC may be falling back to software path.")
            return 1
    print("VERDICT: no encode samples collected — patch did not engage.")
    return 2


if __name__ == "__main__":
    import sys
    sys.exit(asyncio.run(run_spike(duration_s=5.0)))
