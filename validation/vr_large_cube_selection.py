"""Selections on a large cube stay bounded in memory (part of epic #166).

Against a running backend, on the synthetic cube of vr_large_cube_make.py: the
Kinematic Lasso from a seed on one of its discs (kinematic_select_bin), the
statistics of the mask it returns (mask_stats_bin), and of a box, as the VR
page sends them, with the time of each call and the peak resident memory of
the backend and its worker processes while it runs (sampled every 20 ms).

    python validation/vr_large_cube_selection.py --url http://localhost:8781 \
        --token-file FILE --cube /scratch/large_cube.fits --backend-pid PID
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vr_large_cube_make import discs  # noqa: E402


def tree_rss_mb(root: int) -> float:
    out = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,rss="], capture_output=True, text=True).stdout
    rows = [tuple(int(v) for v in line.split()) for line in out.splitlines() if line.strip()]
    children: dict[int, list[int]] = {}
    rss = {}
    for pid, ppid, kb in rows:
        children.setdefault(ppid, []).append(pid)
        rss[pid] = kb
    total, stack = 0, [root]
    while stack:
        p = stack.pop()
        total += rss.get(p, 0)
        stack.extend(children.get(p, []))
    return total / 1024.0


class Peak:
    def __init__(self, pid):
        self.pid, self.peak, self.run = pid, 0.0, True
        self.base = tree_rss_mb(pid)
        self.t = threading.Thread(target=self._loop, daemon=True)
        self.t.start()

    def _loop(self):
        while self.run:
            self.peak = max(self.peak, tree_rss_mb(self.pid))
            time.sleep(0.02)

    def stop(self):
        self.run = False
        self.t.join()
        return self.base, self.peak


def post(url, token, session, path, body, content_type="application/json"):
    headers = {"X-Visivo-Token": token, "Content-Type": content_type}
    if session:
        headers["X-Visivo-Session"] = session
    data = body if isinstance(body, bytes) else json.dumps(body).encode()
    req = urllib.request.Request(url + path, data=data, headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=600) as r:
        return {k.lower(): v for k, v in r.headers.items()}, r.read()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://localhost:8781")
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--cube", required=True)
    ap.add_argument("--backend-pid", type=int, required=True)
    a = ap.parse_args()
    token = Path(a.token_file).read_text().strip()
    _, body = post(a.url, token, "", "/v1/sessions/new", {})
    session = json.loads(body)["session_id"]
    _, body = post(a.url, token, session, "/v1/datasets/open", {"path": a.cube})
    opened = json.loads(body)
    dataset = opened["dataset_id"]
    nx, ny, nz = 2400, 2400, 520
    g = discs(nx, ny, nz, np.random.default_rng(166))[0]
    seed = {"seed_x": int(g["x"]), "seed_y": int(g["y"]), "seed_z": int(round(g["z"]))}
    out = {"cube": a.cube, "seed": seed}

    peak = Peak(a.backend_pid)
    t0 = time.perf_counter()
    h, mask = post(a.url, token, session, "/v1/cube/kinematic_select_bin", {"dataset_id": dataset, **seed})
    t_lasso = time.perf_counter() - t0
    base, top = peak.stop()
    dims = [int(h["x-visivo-width"]), int(h["x-visivo-height"]), int(h["x-visivo-depth"])]
    origin = [int(v) for v in h["x-visivo-origin"].split(",")]
    out["lasso"] = {"s": round(t_lasso, 3), "voxels": int(h["x-visivo-selected-count"]),
                    "bbox": dims, "rss_before_mb": round(base), "rss_peak_mb": round(top)}

    q = (f"?dataset_id={dataset}&origin_x={origin[0]}&origin_y={origin[1]}&origin_z={origin[2]}"
         f"&width={dims[0]}&height={dims[1]}&depth={dims[2]}")
    peak = Peak(a.backend_pid)
    t0 = time.perf_counter()
    _, stats = post(a.url, token, session, "/v1/cube/mask_stats_bin" + q, mask, "application/octet-stream")
    t_stats = time.perf_counter() - t0
    base, top = peak.stop()
    s = json.loads(stats)
    out["mask_stats"] = {"s": round(t_stats, 3), "voxels": s.get("selected_voxels"),
                         "flux_jy_kms": s.get("flux_jy_kms"), "rss_before_mb": round(base),
                         "rss_peak_mb": round(top)}

    # A box of 400 x 400 x 400 (64 M voxels, the route's limit is 64 Mi).
    bx = {"origin_x": 1000, "origin_y": 1000, "origin_z": 60, "width": 400, "height": 400,
          "depth": 400, "box": "true"}
    q = "?" + "&".join(f"{k}={v}" for k, v in {"dataset_id": dataset, **bx}.items())
    peak = Peak(a.backend_pid)
    t0 = time.perf_counter()
    _, stats = post(a.url, token, session, "/v1/cube/mask_stats_bin" + q, b"", "application/octet-stream")
    t_box = time.perf_counter() - t0
    base, top = peak.stop()
    out["box_stats"] = {"s": round(t_box, 3), "voxels": json.loads(stats).get("selected_voxels"),
                        "rss_before_mb": round(base), "rss_peak_mb": round(top)}
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
