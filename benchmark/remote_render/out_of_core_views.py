#!/usr/bin/env python3
"""
out_of_core_views.py - the server-rendered 3-D view of a cube drawn from its
brick pyramid (out of core), compared with the same views drawn in core.

Runs ON the backend's node, inside its image, with the backend's code on the
path (``PYTHONPATH=<checkout>/backend``), like ``multi_gpu_sessions.py``. For
each mode it opens a render session on the cube, as the backend does for a
remote view, and draws five views: home, zoomed in, from the side, MIP, MIP
zoomed in. For each it saves the frame and the time the view took to stop
changing (out of core, blocks arrive over several frames). It then measures
the frame rate while the view is dragged and reads the render process's
memory from /proc. With both modes it also compares the frames.

``--mode ooc`` lowers VISIVO_RENDER_OUT_OF_CORE_BYTES so that any cube is
drawn out of core; ``--mode incore`` raises it so that none is. A cube larger
than the GPU's memory can only be drawn with ``--mode ooc``. Both modes render
on the job's first GPU.

In core, a large cube loads in the background after the session opens and the
session does not report the end of the load: the script waits --settle
seconds, then until no frame has come for 8 s, as multi_gpu_sessions.py does.
Out of core the session is ready when it opens.

Usage, inside the backend's image:

    PYTHONPATH=backend VISIVO_PYRAMID_DIR=/scratch/pyr python3 out_of_core_views.py \
        --cube /data/cube.fits --out results/ooc_cube --mode both

Options:
    --cube PATH        the cube
    --out DIR          frames, differences and summary.json go here
    --mode M           incore, ooc or both     (default both)
    --width/--height   frame size               (default 1280 x 800)
    --quiet S          a view has stopped changing after S s without a new
                       frame                    (default 4)
    --settle S         in core, wait before checking the cube has loaded
                       (default 30)
    --seconds S        measured drag            (default 4)
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import time

VIEWS = [
    ("home", []),
    ("zoom", [{"type": "zoom", "factor": 3.0}]),
    ("side", [{"type": "reset"}, {"type": "rotate", "d_azimuth": 70.0, "d_elevation": 25.0}]),
    ("mip", [{"type": "reset"}, {"type": "set_blend_mode", "mode": 1}]),
    ("mip_zoom", [{"type": "zoom", "factor": 3.0}]),
]


async def _settle(session, quiet: float, limit: float = 1200.0):
    """The last frame once none has come for ``quiet`` seconds, the time until
    that last frame, and how many frames came."""
    version, frame, frames = -1, None, 0
    start = last = time.perf_counter()
    while time.perf_counter() - start < limit:
        jpeg, version = await session.render_frame_if_changed(version)
        now = time.perf_counter()
        if jpeg is not None:
            frame, last, frames = jpeg, now, frames + 1
        elif now - last >= quiet:
            return frame, last - start, frames
        await asyncio.sleep(0.05)
    raise SystemExit("a view did not stop changing within 20 minutes")


async def _drag(session, seconds: float) -> float:
    await session.apply_command({"type": "set_interactive", "value": True})
    version, frames = -1, 0
    start = time.perf_counter()
    while time.perf_counter() - start < seconds:
        await session.apply_command({"type": "rotate", "d_azimuth": 1.0, "d_elevation": 0.0})
        jpeg, version = await session.render_frame_if_changed(version)
        frames += jpeg is not None
    fps = frames / (time.perf_counter() - start)
    await session.apply_command({"type": "set_interactive", "value": False})
    return fps


async def _run(args, mode: str) -> dict:
    # Read by the render process when it starts.
    os.environ["VISIVO_RENDER_OUT_OF_CORE_BYTES"] = str(1 if mode == "ooc" else 1 << 50)
    from app.render_session._process import RenderSessionProcess

    session = RenderSessionProcess(f"ooc{mode}{os.getpid()}", args.cube)
    start = time.perf_counter()
    await session.initialize()
    result = {"open_s": round(time.perf_counter() - start, 2), "views": {}}
    try:
        await session.apply_command({"type": "jpeg_quality", "quality": 95})
        await session.apply_command({"type": "resize", "width": args.width, "height": args.height})
        if mode == "incore":
            await asyncio.sleep(args.settle)
            await _settle(session, max(args.quiet, 8.0))
            result["loaded_s"] = round(time.perf_counter() - start, 1)
        for name, commands in VIEWS:
            for command in commands:
                await session.apply_command(command)
            frame, took, frames = await _settle(session, args.quiet)
            with open(os.path.join(args.out, f"{mode}_{name}.jpg"), "wb") as f:
                f.write(frame)
            result["views"][name] = {"settle_s": round(took, 2), "frames": frames}
            print(mode, name, result["views"][name], flush=True)
        await session.apply_command({"type": "set_blend_mode", "mode": 0})
        await session.apply_command({"type": "reset"})
        await _settle(session, args.quiet)
        result["drag_fps"] = round(await _drag(session, args.seconds), 1)
        with open(f"/proc/{session._process.pid}/status") as f:
            result["render_process_kB"] = {
                key: int(value.split()[0]) for key, value in
                (line.split(":", 1) for line in f if line.startswith(("VmHWM", "VmRSS", "RssAnon", "RssFile")))}
        print(mode, "drag fps", result["drag_fps"], result["render_process_kB"], flush=True)
    finally:
        await session.close()
    return result


def _compare(out: str) -> dict:
    """Per view: mean and 99th percentile of the absolute difference of the
    frames (0-255, averaged over RGB), and the fraction of pixels above 32."""
    import numpy as np
    from PIL import Image

    rows = {}
    for name, _ in VIEWS:
        a, b = (np.asarray(Image.open(os.path.join(out, f"{m}_{name}.jpg")).convert("RGB"), dtype=float)
                for m in ("incore", "ooc"))
        d = np.abs(a - b).mean(axis=2)
        rows[name] = {"mean_abs": round(float(d.mean()), 2), "p99": round(float(np.percentile(d, 99)), 1),
                      "frac_gt_32": round(float((d > 32).mean()), 4)}
        Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(os.path.join(out, f"diff_{name}.png"))
    return rows


async def _main(args) -> dict:
    summary = {"cube": args.cube, "width": args.width, "height": args.height,
               "egl_device": os.environ["VTK_EGL_DEVICE_INDEX"],
               "cuda_visible_devices": os.environ.get("CUDA_VISIBLE_DEVICES")}
    for mode in ("incore", "ooc"):
        if args.mode in (mode, "both"):
            summary[mode] = await _run(args, mode)
    if args.mode == "both":
        summary["difference"] = _compare(args.out)
    return summary


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--cube", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--mode", choices=("incore", "ooc", "both"), default="both")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=800)
    ap.add_argument("--quiet", type=float, default=4.0)
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--settle", type=float, default=30.0)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    # Both modes on the job's first GPU, as the backend would choose it for its
    # first view; the render processes inherit the index.
    from app.render_session._process import _egl_cuda_ordinals, _pick_egl_device
    index = _pick_egl_device(_egl_cuda_ordinals(), 0)
    if index is None:
        raise SystemExit("no EGL device of this job found")
    os.environ["VTK_EGL_DEVICE_INDEX"] = str(index)
    summary = asyncio.run(_main(args))
    with open(os.path.join(args.out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":     # render processes are spawned: they import this module
    main()
