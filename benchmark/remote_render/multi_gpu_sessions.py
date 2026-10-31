#!/usr/bin/env python3
"""
multi_gpu_sessions.py - frames per second of several server-rendered 3-D views
rotated at once, spread over the node's GPUs or all on one.

Runs ON the backend's node, inside its image, with the backend's code on the
path (``PYTHONPATH=<checkout>/backend``). It opens --sessions render sessions
on one cube, as the backend does for remote views (a process each), lets the
adaptive drag level settle, rotates them all together and reports each
session's frames per second and the GPU ``nvidia-smi`` shows its graphics
context on. No network is involved: this is the node's own capacity.

Usage, on a node with 4 GPUs, inside the backend's image:

    PYTHONPATH=backend python3 multi_gpu_sessions.py \
        --cube /data/cube.fits --sessions 4            # spread (the default)
    ... --sessions 4 --one-gpu                         # all on GPU 0

Options:
    --cube PATH        the cube
    --sessions N       views rotated at once      (default 4)
    --one-gpu          bind every view to the job's first GPU
    --width/--height   frame size                 (default 2540 x 1560)
    --threshold V      volume threshold           (default: the backend's own,
                       the cube's 99th percentile; 0 also means that)
    --settle S         wait before checking the cube has loaded (default 30);
                       rotation starts once no frame has come for 8 s
    --warm S           rotation before measuring  (default 6)
    --seconds S        measured rotation          (default 8)
    --out FILE         also append the run to FILE as a JSON line
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import subprocess
import time


async def _rotate(session, seconds: float) -> float:
    """Rotate for ``seconds``; frames per second over the time actually taken
    (the last frame may end after the deadline)."""
    version, frames = -1, 0
    begin = time.perf_counter()
    while time.perf_counter() - begin < seconds:
        await session.apply_command({"type": "rotate", "d_azimuth": 1.0, "d_elevation": 0.0})
        jpeg, version = await session.render_frame_if_changed(version)
        frames += jpeg is not None
    return frames / (time.perf_counter() - begin)


async def _loaded(session, quiet: float = 8.0, limit: float = 600.0) -> None:
    """Wait until the cube looks loaded: a loading cube yields a new frame
    every few seconds, a loaded one none until it is moved. The session does
    not report the end of a load, hence also --settle."""
    version, last = -1, time.perf_counter()
    end = last + limit
    while time.perf_counter() < end:
        jpeg, version = await session.render_frame_if_changed(version)
        now = time.perf_counter()
        if jpeg is not None:
            last = now
        elif now - last >= quiet:
            return
        await asyncio.sleep(0.25)
    raise SystemExit("a cube did not finish loading within 10 minutes")


def _graphics_gpus() -> dict[int, str]:
    """pid -> GPU index of every graphics context nvidia-smi lists."""
    table = subprocess.run(["nvidia-smi"], capture_output=True, text=True).stdout
    gpus = {}
    for line in table.splitlines():
        p = line.split()
        if len(p) > 5 and p[0] == "|" and p[1].isdigit() and p[4].isdigit() and p[5] == "G":
            gpus[int(p[4])] = p[1]
    return gpus


async def _measure(args) -> list[tuple[str, float]]:
    from app.render_session._process import RenderSessionProcess

    sessions = [RenderSessionProcess(f"bench{i:02d}{os.getpid()}", args.cube)
                for i in range(args.sessions)]
    await asyncio.gather(*(s.initialize() for s in sessions))
    try:
        for s in sessions:
            await s.apply_command({"type": "resize", "width": args.width, "height": args.height})
            if args.threshold:      # 0 or none: the backend's own threshold
                await s.apply_command({"type": "set_threshold", "value": args.threshold})
        await asyncio.sleep(args.settle)
        await asyncio.gather(*(_loaded(s) for s in sessions))
        for s in sessions:
            await s.apply_command({"type": "set_interactive", "value": True})
        await asyncio.gather(*(_rotate(s, args.warm) for s in sessions))
        fps = await asyncio.gather(*(_rotate(s, args.seconds) for s in sessions))
        gpus = _graphics_gpus()
        return [(gpus.get(s._process.pid, "?"), f) for s, f in zip(sessions, fps)]
    finally:
        await asyncio.gather(*(s.close() for s in sessions))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--cube", required=True)
    ap.add_argument("--sessions", type=int, default=4)
    ap.add_argument("--one-gpu", action="store_true")
    ap.add_argument("--width", type=int, default=2540)
    ap.add_argument("--height", type=int, default=1560)
    ap.add_argument("--threshold", type=float)
    ap.add_argument("--settle", type=float, default=30.0)
    ap.add_argument("--warm", type=float, default=6.0)
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--out")
    args = ap.parse_args()
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    if args.one_gpu:
        # The job's first GPU, as the backend would choose it for its first
        # view; the children inherit the index.
        from app.render_session._process import _egl_cuda_ordinals, _pick_egl_device
        index = _pick_egl_device(_egl_cuda_ordinals(), 0)
        if index is None:
            raise SystemExit("no EGL device of this job found")
        os.environ["VTK_EGL_DEVICE_INDEX"] = str(index)
    else:
        os.environ.pop("VTK_EGL_DEVICE_INDEX", None)
    rows = asyncio.run(_measure(args))
    fps = [f for _, f in rows]
    label = "one GPU" if args.one_gpu else "spread"
    print(f"{args.sessions} sessions, {label}, CUDA_VISIBLE_DEVICES="
          f"{os.environ.get('CUDA_VISIBLE_DEVICES')}: "
          + ", ".join(f"GPU {g} {f:.1f}" for g, f in rows)
          + f" FPS | mean {sum(fps) / len(fps):.1f}", flush=True)
    if args.out:
        with open(args.out, "a") as f:     # one line per run
            f.write(json.dumps({"cube": args.cube, "sessions": args.sessions, "placement": label,
                                "width": args.width, "height": args.height,
                                "threshold": args.threshold, "seconds": args.seconds,
                                "cuda_visible_devices": os.environ.get("CUDA_VISIBLE_DEVICES"),
                                "when": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                                "per_session": [{"gpu": g, "fps": round(v, 1)} for g, v in rows],
                                "mean_fps": round(sum(fps) / len(fps), 1)}) + "\n")


if __name__ == "__main__":
    main()
