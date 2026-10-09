"""End-to-end test client for the VisIVO WebRTC transport.

Connects to a backend instance, opens a render session, exchanges SDP
via REST, receives H.264 frames via the WebRTC video track, optionally
sends camera commands via the DataChannel, prints per-frame stats.

Doesn't depend on Qt — uses ``aiortc`` for the WebRTC path and PIL
to dump occasional decoded frames to disk (so you can eyeball the
render). The desktop client integration in Week 2 reuses the same SDP
exchange + DataChannel protocol; this script is the reference
implementation.

Usage
-----

  # Backend serving the H.265 build, accessible at $URL (e.g. through
  # an SSH tunnel — same pattern as the JPEG transport)
  export VISIVO_BACKEND=http://localhost:8010
  export VISIVO_TOKEN=$(cat ~/.visivo/power9_token)

  # Open a render session for a previously-opened dataset
  python tools/test_webrtc_client.py \
      --dataset-id ds_49844ab461de \
      --duration 10 \
      --dump-every 30 \
      --commands rotate,zoom

Requires ``av`` + ``aiortc`` + ``httpx`` in the local Python env — same
as the SIF build. On the Mac you can install with::

  conda create -n visivo-webrtc python=3.12 -c conda-forge \\
      ffmpeg pkg-config
  conda activate visivo-webrtc
  pip install 'av<12.3' 'aiortc<1.10' httpx pillow numpy
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
from pathlib import Path

import numpy as np
from aiortc import (
    MediaStreamTrack,
    RTCDataChannel,
    RTCPeerConnection,
    RTCSessionDescription,
)

try:
    import httpx
except ImportError:
    print("ERROR: missing httpx — pip install httpx", file=sys.stderr)
    sys.exit(1)
try:
    from PIL import Image
except ImportError:
    print("ERROR: missing pillow — pip install pillow", file=sys.stderr)
    sys.exit(1)


# ── REST helpers ─────────────────────────────────────────────────────────────


async def open_render_session(client: httpx.AsyncClient, base_url: str,
                               token: str, dataset_id: str) -> str:
    """Hit POST /v1/render/create_session and return the session_id."""
    r = await client.post(
        f"{base_url}/v1/render/create_session",
        headers={"X-Visivo-Token": token, "Content-Type": "application/json"},
        json={"dataset_id": dataset_id},
        timeout=30.0,
    )
    r.raise_for_status()
    body = r.json()
    sid = body.get("session_id", "")
    if not sid:
        raise RuntimeError(f"create_session returned no session_id: {body}")
    return sid


async def post_sdp_offer(client: httpx.AsyncClient, base_url: str,
                          token: str, session_id: str,
                          offer_sdp: str, offer_type: str) -> dict:
    """Hit POST /v1/render/<sid>/sdp/offer and return the answer."""
    r = await client.post(
        f"{base_url}/v1/render/{session_id}/sdp/offer",
        headers={"X-Visivo-Token": token, "Content-Type": "application/json"},
        json={"sdp": offer_sdp, "type": offer_type},
        timeout=30.0,
    )
    r.raise_for_status()
    return r.json()


async def close_render_session(client: httpx.AsyncClient, base_url: str,
                                token: str, session_id: str) -> None:
    try:
        await client.delete(
            f"{base_url}/v1/render/{session_id}",
            headers={"X-Visivo-Token": token},
            timeout=10.0,
        )
    except Exception:
        pass


async def fetch_diagnostics(client: httpx.AsyncClient, base_url: str,
                             token: str) -> dict:
    r = await client.get(
        f"{base_url}/v1/render/diagnostics",
        headers={"X-Visivo-Token": token},
        timeout=10.0,
    )
    r.raise_for_status()
    return r.json()


# ── Decode + telemetry ──────────────────────────────────────────────────────


async def consume_track(received_track: MediaStreamTrack,
                         duration_s: float,
                         dump_every: int,
                         dump_dir: Path) -> tuple[list[float], int]:
    """Pull frames from the track until duration_s elapses.

    Returns (arrival_times, frames_decoded). Optionally writes every
    Nth frame to disk as a PNG so you can verify the render visually.
    """
    arrivals: list[float] = []
    decoded = 0
    start = time.perf_counter()
    while time.perf_counter() - start < duration_s:
        frame = await received_track.recv()
        arrivals.append(time.perf_counter())
        decoded += 1
        if dump_every and decoded % dump_every == 0:
            img = frame.to_ndarray(format="rgb24")
            out = dump_dir / f"frame_{decoded:05d}.png"
            Image.fromarray(img).save(out)
            print(f"[client] dumped {out.name} ({img.shape})")
        if decoded % 30 == 0:
            elapsed = time.perf_counter() - start
            print(f"[client] {decoded} frames @ "
                  f"{decoded/elapsed:.1f} FPS instant")
    return arrivals, decoded


# ── Command sender ──────────────────────────────────────────────────────────


async def send_demo_commands(channel: RTCDataChannel,
                              commands: list[str],
                              duration_s: float) -> None:
    """Drive a few demo camera commands over the DataChannel.

    Exercises the bidirectional path that desktop clients will use for
    rotate/zoom/threshold updates. Each command waits for the channel
    to be in ``open`` state and then sends a JSON line.
    """
    # Wait for channel open. aiortc creates it in ``connecting`` until
    # DTLS finishes; we poll briefly.
    deadline = time.perf_counter() + 5.0
    while channel.readyState != "open":
        if time.perf_counter() > deadline:
            print("[client] command channel never opened; skipping commands")
            return
        await asyncio.sleep(0.1)
    print("[client] command channel open — sending demo commands")

    rotate_count = 0
    interval = max(0.1, duration_s / 30)
    while True:
        for cmd_name in commands:
            if cmd_name == "rotate":
                channel.send(json.dumps({
                    "type": "rotate",
                    "d_azimuth": 2.0,
                    "d_elevation": 0.5,
                }))
                rotate_count += 1
            elif cmd_name == "zoom":
                channel.send(json.dumps({
                    "type": "zoom",
                    "factor": 1.02,
                }))
            await asyncio.sleep(interval)


# ── Main ────────────────────────────────────────────────────────────────────


async def amain(args: argparse.Namespace) -> int:
    base_url = args.backend_url.rstrip("/")
    token = args.token

    async with httpx.AsyncClient() as client:
        # 0. Capability probe.
        try:
            diag = await fetch_diagnostics(client, base_url, token)
            transports = diag.get("transports", [])
            print(f"[client] backend transports: {transports}")
            if "h265-webrtc" not in transports:
                print("⚠️  backend does NOT advertise h265-webrtc — "
                      "test will likely fail at /sdp/offer")
        except Exception as e:
            print(f"[client] diagnostics probe failed: {e}")
            return 1

        # 1. Open render session.
        if not args.session_id:
            print(f"[client] opening render session for dataset={args.dataset_id}")
            session_id = await open_render_session(
                client, base_url, token, args.dataset_id,
            )
            print(f"[client] session created: {session_id}")
        else:
            session_id = args.session_id
            print(f"[client] using existing session: {session_id}")

        try:
            # 2. Build the peer connection.
            pc = RTCPeerConnection()
            arrivals: list[float] = []
            consume_done = asyncio.Event()
            received_track_holder: list[MediaStreamTrack | None] = [None]

            @pc.on("track")
            def on_track(track: MediaStreamTrack) -> None:
                print(f"[client] track received: kind={track.kind}")
                received_track_holder[0] = track

            # Open a DataChannel for camera commands BEFORE we create
            # the offer — the backend's @on("datachannel") fires once
            # the channel is announced in the offer/answer.
            commands_channel = pc.createDataChannel("commands")
            pc.addTransceiver("video", direction="recvonly")

            offer = await pc.createOffer()
            await pc.setLocalDescription(offer)

            # 3. REST signalling.
            answer = await post_sdp_offer(
                client, base_url, token, session_id,
                pc.localDescription.sdp, pc.localDescription.type,
            )
            print(f"[client] received SDP answer (type={answer['type']})")
            await pc.setRemoteDescription(
                RTCSessionDescription(sdp=answer["sdp"], type=answer["type"])
            )

            # 4. Wait for the video track to arrive.
            wait_deadline = time.perf_counter() + 5.0
            while received_track_holder[0] is None:
                if time.perf_counter() > wait_deadline:
                    print("[client] track never arrived after 5s")
                    return 2
                await asyncio.sleep(0.05)

            # 5. Concurrent: consume frames + send commands.
            dump_dir = Path(args.dump_dir) if args.dump_dir else Path(".")
            dump_dir.mkdir(parents=True, exist_ok=True)
            consume_task = asyncio.create_task(consume_track(
                received_track_holder[0],
                args.duration,
                args.dump_every,
                dump_dir,
            ))
            cmd_task: asyncio.Task | None = None
            if args.commands:
                cmd_task = asyncio.create_task(send_demo_commands(
                    commands_channel,
                    args.commands.split(","),
                    args.duration,
                ))

            arrivals, decoded = await consume_task
            if cmd_task is not None:
                cmd_task.cancel()
                try:
                    await cmd_task
                except asyncio.CancelledError:
                    pass

            # 6. Stats.
            print()
            print("─" * 64)
            print(f"Frames decoded     : {decoded}")
            if len(arrivals) >= 2:
                intervals_ms = np.diff(arrivals) * 1000.0
                print(f"Inter-frame mean   : {intervals_ms.mean():.2f} ms")
                print(f"Inter-frame p99    : {np.percentile(intervals_ms, 99):.2f} ms")
                print(f"Effective FPS      : {1000.0 / intervals_ms.mean():.2f}")
            return 0

        finally:
            try:
                await pc.close()
            except Exception:
                pass
            if not args.session_id and args.cleanup:
                await close_render_session(client, base_url, token, session_id)
                print(f"[client] session {session_id} closed via REST")


def main() -> int:
    ap = argparse.ArgumentParser(description="VisIVO WebRTC transport test client")
    ap.add_argument("--backend-url",
                    default=os.environ.get("VISIVO_BACKEND",
                                            "http://localhost:8010"),
                    help="Backend base URL (default $VISIVO_BACKEND or localhost:8010)")
    ap.add_argument("--token",
                    default=os.environ.get("VISIVO_TOKEN", ""),
                    help="Bearer token (default $VISIVO_TOKEN)")
    ap.add_argument("--dataset-id", default="",
                    help="Dataset id from /v1/datasets/open (skip if --session-id given)")
    ap.add_argument("--session-id", default="",
                    help="Re-use an existing render session instead of creating one")
    ap.add_argument("--duration", type=float, default=5.0,
                    help="Seconds to consume frames for")
    ap.add_argument("--dump-every", type=int, default=0,
                    help="Dump every Nth decoded frame to PNG (0 = never)")
    ap.add_argument("--dump-dir", default="/tmp/visivo-webrtc-dump",
                    help="Where to write dumped PNG frames")
    ap.add_argument("--commands", default="",
                    help="Comma-separated command list to drive over the "
                         "DataChannel: rotate,zoom")
    ap.add_argument("--cleanup", action="store_true",
                    help="DELETE the session via REST when done")
    args = ap.parse_args()

    if not args.token:
        print("ERROR: provide --token or set VISIVO_TOKEN", file=sys.stderr)
        return 1
    if not args.dataset_id and not args.session_id:
        print("ERROR: provide --dataset-id or --session-id", file=sys.stderr)
        return 1
    return asyncio.run(amain(args))


if __name__ == "__main__":
    sys.exit(main())
