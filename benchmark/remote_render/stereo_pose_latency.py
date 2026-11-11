#!/usr/bin/env python3
"""
stereo_pose_latency.py - latency and rate of the headset's stereo frames
(issue #169): synthetic head poses at a headset's refresh rate, over the
render WebSocket, as the VR page sends them.

Opens a cube on a running backend, creates a render session for it, starts
the push stream and sends ``xr_pose`` messages at --rates Hz (a head turning
slowly from side to side, the cube 0.75 m in front, as the VR page places
it), acknowledging each stereo frame as it arrives. For each rate it reports:

* the frames received per second, and the share of poses that got a frame
  (poses that arrive while a frame renders are merged: the newest wins);
* pose -> frame received, on the client's clock (the pose's ``t_client``);
* pose received -> frame handed to the socket, on the server's clock, and its
  parts (waiting for the renderer, both eyes, depth, JPEG, back to the
  backend, the ack window and rate cap);
* the frame's JPEG and depth sizes.

Run it where the headset's page would run so that the network is part of the
first latency (on the Mac itself it is loopback). The server's timings are on
the backend's clock only and do not depend on where this runs.

Usage (backend reachable at --base-url, its token in a file):

    backend/.venv/bin/python benchmark/remote_render/stereo_pose_latency.py \\
        --path /data/cube.fits --base-url http://127.0.0.1:18900 \\
        --token-file ~/.visivo_token --rates 72,90

Options:
    --path PATH         the cube, as the BACKEND sees it (within its data roots)
    --base-url URL      backend base URL          (default http://127.0.0.1:8000)
    --token TOKEN       X-Visivo-Token            (default: read --token-file)
    --token-file FILE   file holding the token    (default ~/.visivo_token)
    --rates LIST        pose rates in Hz          (default 72,90)
    --seconds S         measured time per rate    (default 10)
    --eye W[xH]         one eye's frame size      (default 1024x1024)
    --depth-scale N     depth image reduction, 0 = no depth   (default 4)
    --quality Q         JPEG quality              (default 80)
    --window N          stereo frames ahead of acks          (default 2)
    --threshold V       volume threshold          (default: the backend's own)
    --settle S          wait before checking that the cube has loaded (default 20)
    --out FILE          also write the results as JSON

Needs the ``websockets`` package (in the backend's environment).
"""
from __future__ import annotations

import argparse
import asyncio
import json
import math
import statistics
import struct
import time
import urllib.request
from pathlib import Path

_PREFIX = struct.Struct("<4sI")


def _call(base: str, token: str, method: str, path: str, body: dict | None = None,
          session: str = "") -> dict:
    headers = {"Content-Type": "application/json", "X-Visivo-Token": token}
    if session:
        headers["X-Visivo-Session"] = session
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data, headers, method=method)
    with urllib.request.urlopen(req, timeout=600) as resp:
        return json.load(resp)


def _col(m) -> list[float]:
    """Column-major, as WebXR sends a 4x4 matrix (given as rows)."""
    return [m[r][c] for c in range(4) for r in range(4)]


def _mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def _yaw(angle: float, x: float = 0.0, y: float = 0.0, z: float = 0.0):
    c, s = math.cos(angle), math.sin(angle)
    return [[c, 0, s, x], [0, 1, 0, y], [-s, 0, c, z], [0, 0, 0, 1]]


def _rigid_inverse(m):
    r = [[m[j][i] for j in range(3)] for i in range(3)]
    t = [-sum(r[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [r[0] + [t[0]], r[1] + [t[1]], r[2] + [t[2]], [0, 0, 0, 1]]


# The cube 0.75 m in front at eye height minus 0.12 m, 0.6 m on its longest
# side: the VR page's placeInFront; the eyes 64 mm apart, 1.6 m up.
_MODEL = _col([[0.6, 0, 0, 0], [0, 0.6, 0, 1.48], [0, 0, 0.6, -0.75], [0, 0, 0, 1]])
_FOV = {"left": -0.82, "right": 0.82, "up": 0.82, "down": -0.82, "near": 0.1, "far": 1000.0}


def pose_message(pose_id: int, t: float, width: int, height: int, depth_scale: int,
                 quality: int) -> dict:
    """A head turning +-12 degrees at 0.4 Hz and swaying 3 cm."""
    head = _yaw(math.radians(12.0) * math.sin(2 * math.pi * 0.4 * t),
                0.03 * math.sin(2 * math.pi * 0.25 * t), 1.6, 0.0)
    views = []
    for eye, dx in (("left", -0.032), ("right", 0.032)):
        eye_to_world = _mul(head, _yaw(0.0, dx, 0.0, 0.0))
        views.append({"eye": eye, "view": _col(_rigid_inverse(eye_to_world)), "fov": _FOV,
                      "width": width, "height": height})
    return {"type": "xr_pose", "pose_id": pose_id, "t_client": time.time(),
            "model": _MODEL, "views": views, "quality": quality,
            "depth": {"scale": depth_scale} if depth_scale else False}


def _header(message: bytes) -> dict | None:
    magic, n = _PREFIX.unpack_from(message)
    if magic != b"VXR1":
        return None
    return json.loads(message[_PREFIX.size:_PREFIX.size + n])


class _Frames:
    def __init__(self, ws) -> None:
        self.ws = ws
        self.got: list[tuple[float, dict, int]] = []    # (arrival, header, bytes)
        self.errors: list[str] = []

    async def run(self) -> None:
        async for msg in self.ws:
            if isinstance(msg, str):
                reply = json.loads(msg)
                if reply.get("type") == "xr_error":
                    self.errors.append(reply.get("error", ""))
                continue
            header = _header(msg)
            await self.ws.send('{"type":"frame_ack"}')
            if header is not None:
                self.got.append((time.time(), header, len(msg)))


def _pct(values: list[float]) -> dict:
    if not values:
        return {}
    v = sorted(values)
    pick = lambda q: v[min(len(v) - 1, int(q * len(v)))]   # noqa: E731
    return {"p50": round(pick(0.5), 2), "p90": round(pick(0.9), 2), "p99": round(pick(0.99), 2),
            "max": round(v[-1], 2), "mean": round(statistics.mean(v), 2)}


async def _run_rate(ws, frames: _Frames, rate: float, args, first_id: int) -> tuple[dict, int]:
    period = 1.0 / rate
    begin = time.perf_counter()
    start_wall = time.time()
    sent, pose_id = 0, first_id
    while time.perf_counter() - begin < args.seconds:
        target = begin + sent * period
        delay = target - time.perf_counter()
        if delay > 0:
            await asyncio.sleep(delay)
        await ws.send(json.dumps(pose_message(pose_id, time.perf_counter() - begin,
                                              args.eye_w, args.eye_h, args.depth_scale,
                                              args.quality)))
        sent += 1
        pose_id += 1
    end_wall = time.time()
    await asyncio.sleep(0.5)          # the last frames come in
    got = [(t, h, n) for t, h, n in frames.got
           if first_id <= h["pose_id"] < pose_id and t <= end_wall + 0.5]
    window = [g for g in got if g[0] <= end_wall]
    client = [1000 * (t - h["t_client"]) for t, h, _ in got if h.get("t_client")]
    part = lambda k: [h["timing_ms"][k] for _, h, _ in got if k in h.get("timing_ms", {})]  # noqa: E731
    depth_kb = [h["depth"]["length"] / 1024 for _, h, _ in got if h.get("depth")]
    result = {
        "rate_hz": rate, "poses_sent": sent, "poses_per_s": round(sent / (end_wall - start_wall), 1),
        "frames": len(got), "frames_per_s": round(len(window) / (end_wall - start_wall), 1),
        "poses_with_a_frame": round(len(got) / sent, 3) if sent else 0.0,
        "client_pose_to_frame_ms": _pct(client),
        "server_pose_to_sent_ms": _pct(part("total")),
        "server_parts_ms": {k: _pct(part(k)) for k in
                            ("wait", "render", "depth", "encode", "handoff", "queue")},
        "frame_kb": _pct([n / 1024 for _, _, n in got]),
        "depth_kb": _pct(depth_kb),
    }
    return result, pose_id


async def _measure(args, token: str) -> list[dict]:
    import websockets

    opened = _call(args.base_url, token, "POST", "/v1/datasets/open", {"path": args.path})
    if not opened.get("valid"):
        raise SystemExit(f"open refused: {opened.get('error')}")
    while True:
        created = _call(args.base_url, token, "POST", "/v1/render/create_session",
                        {"dataset_id": opened["dataset_id"]}, opened.get("session_id", ""))
        if not created.get("preparing"):
            break
        await asyncio.sleep(5)
    sid = created["session_id"]
    ws_url = args.base_url.replace("http", "ws", 1) + f"/v1/render/{sid}/ws?token={token}"
    results = []
    try:
        async with websockets.connect(ws_url, max_size=None) as ws:
            if args.threshold:
                await ws.send(json.dumps({"type": "set_threshold", "value": args.threshold}))
            await ws.send(json.dumps({"type": "stream", "window": args.window,
                                      "max_fps": max(args.rates)}))
            reply = json.loads(await ws.recv())
            if reply.get("stream") != "on":
                raise SystemExit(f"the backend did not start a push stream: {reply}")
            frames = _Frames(ws)
            receiver = asyncio.create_task(frames.run())
            # One pose, then wait for the cube to load (a loading cube keeps
            # pushing frames as its chunks arrive).
            await ws.send(json.dumps(pose_message(0, 0.0, args.eye_w, args.eye_h,
                                                  args.depth_scale, args.quality)))
            await asyncio.sleep(args.settle)
            last = len(frames.got)
            while True:
                await asyncio.sleep(4.0)
                if len(frames.got) == last:
                    break
                last = len(frames.got)
            if frames.errors:
                raise SystemExit(f"the backend refused the pose: {frames.errors[-1]}")
            if not frames.got:
                raise SystemExit("no stereo frame came back (a point session, or an old backend)")
            next_id = 1
            for rate in args.rates:
                # A short warm-up at this rate, not measured.
                _, next_id = await _run_rate(ws, frames, rate,
                                             argparse.Namespace(**{**vars(args), "seconds": 1.5}),
                                             next_id)
                r, next_id = await _run_rate(ws, frames, rate, args, next_id)
                results.append(r)
                print(f"{rate:g} Hz: {r['frames_per_s']} frames/s, "
                      f"{100 * r['poses_with_a_frame']:.0f}% of poses; pose->frame "
                      f"p50 {r['client_pose_to_frame_ms'].get('p50')} ms, "
                      f"p90 {r['client_pose_to_frame_ms'].get('p90')} ms (client); "
                      f"server p50 {r['server_pose_to_sent_ms'].get('p50')} ms; "
                      f"render p50 {r['server_parts_ms']['render'].get('p50')} ms, "
                      f"depth {r['server_parts_ms']['depth'].get('p50')} ms, "
                      f"JPEG {r['server_parts_ms']['encode'].get('p50')} ms; "
                      f"{r['frame_kb'].get('p50')} kB/frame", flush=True)
            await ws.send(json.dumps({"type": "xr_stop"}))
            receiver.cancel()
    finally:
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
    ap.add_argument("--rates", default="72,90")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--eye", default="1024x1024")
    ap.add_argument("--depth-scale", type=int, default=4)
    ap.add_argument("--quality", type=int, default=80)
    ap.add_argument("--window", type=int, default=2)
    ap.add_argument("--threshold", type=float)
    ap.add_argument("--settle", type=float, default=20.0)
    ap.add_argument("--out")
    args = ap.parse_args()
    args.rates = [float(r) for r in args.rates.split(",") if r]
    w, _, h = args.eye.partition("x")
    args.eye_w, args.eye_h = int(w), int(h or w)
    token = args.token or Path(args.token_file).expanduser().read_text().strip()
    results = asyncio.run(_measure(args, token))
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(
            {"base_url": args.base_url, "path": args.path, "eye": [args.eye_w, args.eye_h],
             "depth_scale": args.depth_scale, "quality": args.quality, "window": args.window,
             "seconds": args.seconds, "when": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
             "rates": results}, indent=2))


if __name__ == "__main__":
    main()
