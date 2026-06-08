"""WebRTC spike — Day 3: REST signalling between server and client.

Splits the loopback into a FastAPI server process + a Python client
process. SDP offer/answer flows over HTTP (POST ``/spike/sdp/offer``),
matching the real-backend integration pattern: the existing
``/v1/render/*`` REST router gains an ``offer`` endpoint that builds
a PeerConnection per client and returns the answer.

The server runs the same NVENC encoder as Day 2 (synthetic
colour-shifting frames at 30 FPS). The client connects, consumes
frames for 5 seconds, prints stats.

Both halves run in the SAME process — the script forks the FastAPI
server into a background thread and the client into the main loop,
so a single ``apptainer exec`` invocation tests the whole chain.

Run:

  apptainer exec --nv backend-egl-h265.sif \
      /opt/conda/bin/python apptainer/spike_h265_webrtc_rest.py
"""
from __future__ import annotations

import asyncio
import contextlib
import logging
import threading
import time
from fractions import Fraction
from typing import Optional

import numpy as np
import uvicorn
from aiohttp import ClientSession
from aiortc import (
    MediaStreamTrack,
    RTCPeerConnection,
    RTCSessionDescription,
)
from av import VideoFrame
from fastapi import FastAPI
from pydantic import BaseModel

# Silence uvicorn's banner — keeps the spike output focused on metrics.
logging.getLogger("uvicorn.access").disabled = True
logging.getLogger("uvicorn.error").setLevel(logging.WARNING)

PORT = 8765
encode_times_ms_singleton: list[list[float]] = [[]]
vpx_encode_calls_singleton: list[list[int]] = [[0]]


# ── NVENC patch + codec forcing (same as Day 2) ──────────────────────────────


def _install_nvenc_encoder() -> str:
    from aiortc.codecs import h264 as h264_mod
    from aiortc.codecs import vpx as vpx_mod
    import av

    encode_times_ms: list[float] = []
    encode_times_ms_singleton[0] = encode_times_ms

    vpx_calls = [0]
    original_vpx_encode = vpx_mod.Vp8Encoder.encode

    def _vpx_encode_traced(self, frame, force_keyframe=False):
        vpx_calls[0] += 1
        return original_vpx_encode(self, frame, force_keyframe)

    vpx_mod.Vp8Encoder.encode = _vpx_encode_traced
    vpx_encode_calls_singleton[0] = vpx_calls

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
            print(f"[nvenc-patch] WARN: NVENC build failed ({e}) "
                  f"— falling back to {codec_name!r}")
            return original_create(codec_name, width, height, bitrate)

    h264_mod.create_encoder_context = create_encoder_context_nvenc

    original_encode = h264_mod.H264Encoder.encode

    def _encode_timed(self, frame, force_keyframe=False):
        t0 = time.perf_counter()
        result = original_encode(self, frame, force_keyframe)
        encode_times_ms.append((time.perf_counter() - t0) * 1000.0)
        return result

    h264_mod.H264Encoder.encode = _encode_timed
    return "h264_nvenc"


def _force_h264_only(pc) -> None:
    from aiortc.rtcrtpsender import RTCRtpSender
    caps = RTCRtpSender.getCapabilities("video")
    h264_codecs = [c for c in caps.codecs
                   if c.mimeType.lower() == "video/h264"]
    if not h264_codecs:
        return
    for tr in pc.getTransceivers():
        if tr.kind == "video":
            tr.setCodecPreferences(h264_codecs)


# ── Synthetic source (same as Day 2) ─────────────────────────────────────────


class SyntheticVideoTrack(MediaStreamTrack):
    kind = "video"

    def __init__(self, width=640, height=480, fps=30.0):
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


# ── FastAPI server (mirrors the real /v1/render/*/sdp/offer endpoint) ────────


class SdpExchange(BaseModel):
    sdp: str
    type: str


def build_server_app() -> FastAPI:
    app = FastAPI()
    # Keep references so PCs aren't GC'd when the request returns.
    pcs: list[RTCPeerConnection] = []
    app.state.pcs = pcs

    @app.post("/spike/sdp/offer", response_model=SdpExchange)
    async def handle_offer(offer: SdpExchange) -> SdpExchange:
        pc = RTCPeerConnection()
        pcs.append(pc)

        @pc.on("connectionstatechange")
        async def on_state_change() -> None:
            if pc.connectionState in ("failed", "closed"):
                if pc in pcs:
                    pcs.remove(pc)

        pc.addTrack(SyntheticVideoTrack())
        _force_h264_only(pc)

        await pc.setRemoteDescription(
            RTCSessionDescription(sdp=offer.sdp, type=offer.type)
        )
        answer = await pc.createAnswer()
        await pc.setLocalDescription(answer)
        return SdpExchange(
            sdp=pc.localDescription.sdp,
            type=pc.localDescription.type,
        )

    @app.on_event("shutdown")
    async def on_shutdown() -> None:
        await asyncio.gather(*(pc.close() for pc in list(pcs)))
        pcs.clear()

    return app


def run_server_in_thread() -> threading.Thread:
    app = build_server_app()
    config = uvicorn.Config(app, host="127.0.0.1", port=PORT, log_level="warning")
    server = uvicorn.Server(config)

    def _run() -> None:
        # uvicorn.Server.run requires its own loop. asyncio.run is fine
        # because we're on a fresh thread.
        asyncio.run(server.serve())

    t = threading.Thread(target=_run, daemon=True, name="spike-uvicorn")
    t.start()
    return t


async def wait_for_server_up(timeout_s: float = 5.0) -> None:
    deadline = time.perf_counter() + timeout_s
    async with ClientSession() as session:
        while time.perf_counter() < deadline:
            try:
                async with session.post(
                    f"http://127.0.0.1:{PORT}/spike/sdp/offer",
                    json={"sdp": "", "type": "offer"},
                    timeout=0.5,
                ):
                    return
            except Exception:
                await asyncio.sleep(0.05)
    raise RuntimeError("uvicorn never came up")


# ── Client ───────────────────────────────────────────────────────────────────


async def run_client(duration_s: float = 5.0) -> int:
    client_pc = RTCPeerConnection()

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

    client_pc.addTransceiver("video", direction="recvonly")
    offer = await client_pc.createOffer()
    await client_pc.setLocalDescription(offer)

    # HTTP roundtrip — this is the real signalling path.
    async with ClientSession() as session:
        async with session.post(
            f"http://127.0.0.1:{PORT}/spike/sdp/offer",
            json={"sdp": client_pc.localDescription.sdp,
                  "type": client_pc.localDescription.type},
            timeout=5.0,
        ) as resp:
            data = await resp.json()
    print(f"[client] received SDP answer (type={data['type']})")
    await client_pc.setRemoteDescription(
        RTCSessionDescription(sdp=data["sdp"], type=data["type"])
    )

    try:
        await asyncio.wait_for(consume_done.wait(), timeout=duration_s + 5.0)
    except asyncio.TimeoutError:
        print(f"[spike] timeout — only {len(arrivals)} frames received")

    await client_pc.close()

    # ── Stats ───────────────────────────────────────────────────────
    print()
    print("─" * 64)
    print(f"Frames received    : {len(arrivals)} (target {target_count})")
    if len(arrivals) >= 2:
        intervals_ms = np.diff(arrivals) * 1000.0
        print(f"Inter-frame mean   : {intervals_ms.mean():.2f} ms")
        print(f"Inter-frame p99    : {np.percentile(intervals_ms, 99):.2f} ms")
        print(f"Effective FPS      : {1000.0 / intervals_ms.mean():.2f}")

    encode_times_ms = encode_times_ms_singleton[0]
    if encode_times_ms:
        et = np.asarray(encode_times_ms)
        print()
        print(f"Encode samples     : {len(et)} (NVENC h264_nvenc)")
        print(f"Encode wall mean   : {et.mean():.2f} ms")
        print(f"Encode wall p99    : {np.percentile(et, 99):.2f} ms")

    vpx_called = vpx_encode_calls_singleton[0][0]
    if vpx_called > 0:
        print(f"⚠️  VP8 fired {vpx_called} times — H264 path bypassed")

    print()
    if encode_times_ms and len(arrivals) >= target_count - 5:
        print("VERDICT: REST signalling end-to-end OK with NVENC. "
              "Ready to integrate into /v1/render router.")
        return 0
    print("VERDICT: pipeline degraded — see stats above.")
    return 1


# ── Main ─────────────────────────────────────────────────────────────────────


async def main() -> int:
    print("=" * 64)
    print("WebRTC spike — Day 3: REST signalling (FastAPI server + client)")
    print("=" * 64)
    codec = _install_nvenc_encoder()
    print(f"[setup] aiortc patched to use {codec!r}; H264 forced on transceivers")

    run_server_in_thread()
    print(f"[setup] uvicorn started on 127.0.0.1:{PORT} (background thread)")
    # Tiny grace period for uvicorn to bind the socket.
    await asyncio.sleep(0.5)

    return await run_client(duration_s=5.0)


if __name__ == "__main__":
    import sys
    sys.exit(asyncio.run(main()))
