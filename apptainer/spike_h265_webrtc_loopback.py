"""WebRTC loopback spike — server + client in one process.

Validates the bottom half of the Phase 3 SSR pipeline INSIDE the
``backend-egl-h265.sif`` container:

  * aiortc 1.9 can negotiate a video stream end-to-end
  * frames pushed by a ``MediaStreamTrack`` arrive at the receiver
  * the per-frame wall-clock latency budget allows ~30 FPS

A synthetic video source emits 480x640 RGB frames at 30 FPS for 5
seconds. A loopback peer connection consumes them. We measure:

  * total frames received
  * inter-frame interval distribution (jitter)
  * effective FPS
  * codec actually negotiated (sanity check)

If this spike succeeds, the next step is to:

  1. Add NVENC by subclassing aiortc's H264Encoder
  2. Replace the synthetic source with the VTK frame producer
  3. Move signalling onto the FastAPI router (instead of in-process)

Run inside the SIF:

  apptainer exec --nv backend-egl-h265.sif \
      /opt/conda/bin/python apptainer/spike_h265_webrtc_loopback.py
"""
from __future__ import annotations

import asyncio
import time
from fractions import Fraction

import numpy as np
from aiortc import RTCPeerConnection, MediaStreamTrack
from av import VideoFrame


# ── Synthetic frame source ───────────────────────────────────────────────────


class SyntheticVideoTrack(MediaStreamTrack):
    """30 FPS RGB frame source with a moving stripe + colour shift.

    Inheriting from ``MediaStreamTrack`` means aiortc's RTP plumbing
    calls ``recv()`` whenever it's ready to encode a new frame; we
    pace ourselves with ``asyncio.sleep`` so the rate stays ~30 FPS
    instead of running as fast as encode allows.
    """

    kind = "video"

    def __init__(self, width: int = 640, height: int = 480, fps: float = 30.0):
        super().__init__()
        self._w, self._h, self._fps = width, height, fps
        self._counter = 0
        self._start_wall = time.perf_counter()

    async def recv(self) -> VideoFrame:
        # Pace ourselves to the requested FPS — without this aiortc
        # spins as fast as encode allows, producing a useless burst.
        target_t = self._start_wall + self._counter / self._fps
        delta = target_t - time.perf_counter()
        if delta > 0:
            await asyncio.sleep(delta)

        # Build a uniform RGB colour that shifts over time + a moving
        # vertical white stripe so we can SEE motion in the decoder.
        t = self._counter / self._fps
        r = int(128 + 127 * np.sin(2 * np.pi * t * 0.30))
        g = int(128 + 127 * np.sin(2 * np.pi * t * 0.50))
        b = int(128 + 127 * np.sin(2 * np.pi * t * 0.70))
        arr = np.full((self._h, self._w, 3), (r, g, b), dtype=np.uint8)
        stripe_x = int((self._counter * 5) % self._w)
        arr[:, stripe_x : stripe_x + 10] = 255

        frame = VideoFrame.from_ndarray(arr, format="rgb24")
        # PTS is in time_base units; (counter, 1/fps) → counter * (1/fps) sec.
        frame.pts = self._counter
        frame.time_base = Fraction(1, int(self._fps))

        self._counter += 1
        return frame


# ── Loopback wiring ──────────────────────────────────────────────────────────


async def run_spike(duration_s: float = 5.0) -> int:
    """Spin up server + client, run for ``duration_s`` seconds, print stats."""
    print("=" * 64)
    print("WebRTC loopback spike — aiortc 1.9 inside the SIF")
    print("=" * 64)

    server_pc = RTCPeerConnection()
    client_pc = RTCPeerConnection()

    # Server side: produce frames.
    source = SyntheticVideoTrack()
    server_pc.addTrack(source)

    # Client side: collect received frames + their arrival timestamps.
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
                    # Print occasional progress.
                    if len(arrivals) % 30 == 0:
                        print(f"[client] {len(arrivals)} frames received "
                              f"(size {frame.width}x{frame.height}, "
                              f"format {frame.format.name})")
                    if len(arrivals) >= target_count:
                        consume_done.set()
            except Exception as e:
                print(f"[client] consume() exception: {type(e).__name__}: {e}")
                consume_done.set()

        asyncio.create_task(consume())

    # Signalling — for a loopback we just pass SDP between the two PCs
    # directly. In the real backend this is a REST round-trip.
    offer = await server_pc.createOffer()
    await server_pc.setLocalDescription(offer)
    await client_pc.setRemoteDescription(server_pc.localDescription)
    answer = await client_pc.createAnswer()
    await client_pc.setLocalDescription(answer)
    await server_pc.setRemoteDescription(client_pc.localDescription)

    print(f"[signalling] SDP negotiated: server={server_pc.connectionState}, "
          f"client={client_pc.connectionState}")
    print(f"[signalling] local SDP type: {server_pc.localDescription.type}")
    # Surface the negotiated codec — a sanity check that we're getting
    # the encoder we expect (defaults to H264/VP8 in aiortc 1.9).
    for line in server_pc.localDescription.sdp.splitlines():
        if line.startswith("a=rtpmap:") and ("VP" in line or "H264" in line
                                              or "H265" in line):
            print(f"[signalling] {line}")

    # Run until we have enough frames OR timeout, whichever first.
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
        # Quality verdict
        if intervals_ms.mean() < 40 and np.percentile(intervals_ms, 95) < 80:
            print()
            print("VERDICT: pipeline healthy — proceed to NVENC + REST signalling.")
            return 0
        else:
            print()
            print("VERDICT: jitter/throughput suspicious — investigate before NVENC swap.")
            return 1
    print()
    print("VERDICT: fewer than 2 frames received — pipeline FAIL.")
    return 2


if __name__ == "__main__":
    import sys
    sys.exit(asyncio.run(run_spike(duration_s=5.0)))
