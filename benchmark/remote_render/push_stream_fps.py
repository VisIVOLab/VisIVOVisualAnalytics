#!/usr/bin/env python3
"""
push_stream_fps.py - frames per second of a server-rendered 3-D view while it
is rotated, as a client sees them.

Opens a FITS cube on a running backend, creates a render session for it (as
the desktop's "Switch to Remote" does), sets the view the desktop would have
(frame size, colour map, threshold), then rotates it for a few seconds over
the push stream (POST-free: the backend sends a frame per change, the client
acknowledges each one) and reports the frames received per second. Run it
where the desktop would run, so the network in between (an SSH tunnel to a
cluster node, for instance) is part of the measurement.

The first rotation starts at the finest drag level; later ones start where
the adaptive level settled, so --runs 2 or more shows both.

Usage (backend reachable at --base-url, its token in a file):

    python3 push_stream_fps.py --path /data/cube.fits \
        --base-url http://localhost:18900 --token-file ~/.visivo_slurm_token

Options:
    --path PATH        the cube, as the BACKEND sees it (within its data roots)
    --base-url URL     backend base URL         (default http://127.0.0.1:8000)
    --token TOKEN      X-Visivo-Token           (default: read --token-file)
    --token-file FILE  file holding the token   (default ~/.visivo_token)
    --width/--height   frame size in pixels     (default 2540 x 1560, a Retina
                       desktop's 3-D pane)
    --threshold V      volume threshold         (default: the backend's own, the
                       cube's 99th percentile; 0 also means that)
    --window N         frames ahead of acks     (default 8, as the desktop)
    --seconds S        length of each rotation  (default 4)
    --runs N           rotations                (default 2)
    --settle S         wait before checking the cube has loaded (default 30);
                       rotation starts once no frame has come for 8 s. The
                       backend does not report the end of a load: give a
                       cube that loads slowly a longer --settle.
    --out FILE         also write the results as JSON

Needs the ``websockets`` package (in the backend's environment).
"""
from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import time
import urllib.request
from pathlib import Path


def _call(base: str, token: str, method: str, path: str, body: dict | None = None,
          session: str = "") -> dict:
    headers = {"Content-Type": "application/json", "X-Visivo-Token": token}
    if session:
        headers["X-Visivo-Session"] = session
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data, headers, method=method)
    with urllib.request.urlopen(req, timeout=600) as resp:
        return json.load(resp)


def _jpeg_width(data: bytes) -> int:
    """Width from the JPEG's start-of-frame marker; 0 if none is found."""
    i = 2
    while i + 9 <= len(data):
        if data[i] != 0xFF:
            return 0
        marker = data[i + 1]
        if marker in (0xC0, 0xC1, 0xC2):
            return int.from_bytes(data[i + 7:i + 9], "big")
        i += 2 + int.from_bytes(data[i + 2:i + 4], "big")
    return 0


class _Frames:
    """Receives every pushed frame for the whole connection and acknowledges
    it at once, so frames pushed while nothing is being measured (the load,
    the pauses between rotations) are consumed then and never counted."""

    def __init__(self, ws) -> None:
        self.ws = ws
        self.arrivals: list[tuple[float, int, int]] = []    # (time, bytes, width)

    async def run(self) -> None:
        async for msg in self.ws:
            if isinstance(msg, str):
                continue
            self.arrivals.append((time.perf_counter(), len(msg), _jpeg_width(msg)))
            await self.ws.send('{"type":"frame_ack"}')

    async def quiet(self, seconds: float, limit: float) -> bool:
        """Wait until a frame has arrived and none has come since for
        ``seconds``; False if that did not happen within ``limit``. This is
        how the load is waited for: a loading cube pushes a frame every few
        seconds, and the backend does not report the end of the load."""
        end = time.perf_counter() + limit
        while time.perf_counter() < end:
            if self.arrivals and time.perf_counter() - self.arrivals[-1][0] >= seconds:
                return True
            await asyncio.sleep(0.25)
        return False

    async def at_rest(self, after: float, width: int, limit: float) -> bool:
        """Wait for the full-size frame the backend renders when a rotation
        ends (drag frames are half the size): one wider than half ``width``
        arrived after ``after``. False if none came within ``limit``."""
        end = time.perf_counter() + limit
        while time.perf_counter() < end:
            if any(t > after and w > width // 2 for t, _, w in self.arrivals):
                return True
            await asyncio.sleep(0.1)
        return False


async def _rotate(ws, frames: _Frames, seconds: float, width: int) -> dict:
    """Rotate for ``seconds`` with a pointer at 60 Hz; statistics of the drag
    frames (half size) that arrived meanwhile. The clock starts at the first
    of them and stops at the end of the rotation, so a stall at the end
    counts."""
    await ws.send(json.dumps({"type": "set_interactive", "value": True}))
    begin = time.perf_counter()
    end = begin + seconds
    while time.perf_counter() < end:
        await ws.send(json.dumps({"type": "rotate", "d_azimuth": 1.0, "d_elevation": 0.0}))
        await asyncio.sleep(1 / 60)
    end = time.perf_counter()
    await ws.send(json.dumps({"type": "set_interactive", "value": False}))
    got = [(t, n) for t, n, w in frames.arrivals if begin < t <= end and w <= width // 2]
    if len(got) < 2:
        return {"fps": 0.0, "frames": 0, "kb_per_frame": None,
                "gap_median_ms": None, "gap_p90_ms": None, "released_at": end}
    times = [t for t, _ in got]
    gaps = sorted(b - a for a, b in zip(times, times[1:]))
    sizes = [n for _, n in got[1:]]
    return {"fps": round((len(got) - 1) / (end - times[0]), 1),
            "frames": len(got) - 1,
            "kb_per_frame": round(statistics.mean(sizes) / 1024, 1),
            "gap_median_ms": round(1000 * gaps[len(gaps) // 2], 1),
            "gap_p90_ms": round(1000 * gaps[int(len(gaps) * 0.9)], 1),
            "released_at": end}


async def _measure(args, token: str) -> list[dict]:
    import websockets

    opened = _call(args.base_url, token, "POST", "/v1/datasets/open", {"path": args.path})
    if not opened.get("valid"):
        raise SystemExit(f"open refused: {opened.get('error')}")
    # 202 while the backend builds the cube's brick pyramid: ask again.
    while True:
        created = _call(args.base_url, token, "POST", "/v1/render/create_session",
                        {"dataset_id": opened["dataset_id"]}, opened.get("session_id", ""))
        if not created.get("preparing"):
            break
        print(f"preparing the cube on the backend: {100 * created.get('progress', 0):.0f}%",
              flush=True)
        await asyncio.sleep(5)
    sid = created["session_id"]
    ws_url = args.base_url.replace("http", "ws", 1) + f"/v1/render/{sid}/ws?token={token}"
    results = []
    try:
        async with websockets.connect(ws_url, max_size=None) as ws:
            setup = [{"type": "resize", "width": args.width, "height": args.height}]
            if args.threshold:      # 0 or none: the backend's own threshold
                setup.append({"type": "set_threshold", "value": args.threshold})
            for cmd in setup:
                await ws.send(json.dumps(cmd))
            await ws.send(json.dumps({"type": "stream", "window": args.window, "max_fps": 30}))
            reply = json.loads(await ws.recv())
            if reply.get("stream") != "on":
                raise SystemExit(f"the backend did not start a push stream: {reply}")
            frames = _Frames(ws)
            receiver = asyncio.create_task(frames.run())
            await asyncio.sleep(args.settle)
            if not await frames.quiet(8.0, limit=600):
                raise SystemExit("no frame, or still loading, after 10 minutes")
            for i in range(args.runs):
                r = await _rotate(ws, frames, args.seconds, args.width)
                released = r.pop("released_at")
                r["run"] = i + 1
                results.append(r)
                print(f"rotation {i + 1}: {r['fps']} FPS, {r['kb_per_frame']} kB/frame, "
                      f"gap median {r['gap_median_ms']} ms, p90 {r['gap_p90_ms']} ms", flush=True)
                # The full-size frame at rest, before the next rotation.
                if not await frames.at_rest(released, args.width, limit=60):
                    raise SystemExit("no frame at rest within a minute of the rotation")
            receiver.cancel()
    finally:
        # A closed socket leaves the session alive until it expires.
        try:
            _call(args.base_url, token, "DELETE", f"/v1/render/{sid}")
        except OSError:
            pass
    return results


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--path", required=True)
    ap.add_argument("--base-url", default="http://127.0.0.1:8000")
    ap.add_argument("--token")
    ap.add_argument("--token-file", default=str(Path.home() / ".visivo_token"))
    ap.add_argument("--width", type=int, default=2540)
    ap.add_argument("--height", type=int, default=1560)
    ap.add_argument("--threshold", type=float,
                    help="volume threshold; 0 or none: the backend's own (p99 of the cube)")
    ap.add_argument("--window", type=int, default=8)
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--settle", type=float, default=30.0,
                    help="seconds to wait before checking that the cube has loaded")
    ap.add_argument("--out")
    args = ap.parse_args()
    if not 128 <= args.width <= 4096:
        # The backend clamps a view to 64-4096 pixels and halves it while it
        # moves; drag and rest frames are told apart by width only inside this.
        ap.error("--width must be between 128 and 4096")
    token = args.token or Path(args.token_file).expanduser().read_text().strip()
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    results = asyncio.run(_measure(args, token))
    if args.out:
        Path(args.out).write_text(json.dumps(
            {"base_url": args.base_url, "path": args.path, "width": args.width,
             "height": args.height, "threshold": args.threshold, "window": args.window,
             "seconds": args.seconds, "when": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
             "rotations": results}, indent=2))


if __name__ == "__main__":
    main()
