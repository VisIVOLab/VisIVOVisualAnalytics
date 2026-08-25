#!/usr/bin/env python
"""VisIVO backend performance benchmark — for the software paper (weakness #1).

Times the operations the paper claims are interactive on multi-GB cubes, on real
cubes of increasing size, and records PEAK RESIDENT MEMORY per operation to
demonstrate the out-of-core (memmap-streaming) design — RSS stays far below the
cube size for preview / slice / moment.

Each (cube, operation) is run in its own subprocess so peak RSS is isolated.

Operations
  preview  worker_cube_preview  — the downsampled 3-D volume shown first
  slice    worker_cube_slice    — one full-resolution channel map
  moment0  worker_moment(0)     — moment-0 over all channels (streaming path)
  lasso    worker_cube_kinematic_select — one geodesic object grow (seed=peak)

Run:  backend/.venv/bin/python validation/benchmark.py
"""
from __future__ import annotations

import csv
import json
import os
import platform
import resource
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "backend"))

# Out-of-repo demo-data directory holding the benchmark cubes; override for reproducibility.
DD = os.environ.get("VISIVO_DEMODATA", "/Users/fvitello/downloads/DemoData")
CUBES = [
    ("WALLABY",  f"{DD}/WALLABY.fits"),
    ("cubehi-m31", f"{DD}/cubehi-clean-m31.fits"),
    ("HGC44",    f"{DD}/hgc44_3axes.fits"),
    ("fornax",   f"{DD}/fornax.fits"),
    ("3D-Ape",   f"{DD}/3D_Ape-DR1_200426041_AP_B021_HI_image_cube0.fits"),
]
OPS = ["preview", "slice", "moment0", "lasso"]
OUT = Path(__file__).resolve().parent / "out"


def _peak_rss_mb() -> float:
    ru = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return ru / 1e6 if platform.system() == "Darwin" else ru / 1e3  # Darwin=bytes, Linux=KB


def _bright_seed(data):
    import numpy as np
    s = max(1, int(round((data.size / 2_000_000) ** (1 / 3))))
    sub = np.asarray(data[::s, ::s, ::s], dtype=np.float32)
    sub = np.where(np.isfinite(sub), sub, -np.inf)
    zz, yy, xx = np.unravel_index(int(np.argmax(sub)), sub.shape)
    return int(zz * s), int(yy * s), int(xx * s)


def run_one(op: str, path: str) -> dict:
    """Run a single operation; return dims + wall time + peak RSS."""
    from app.fits_dataset import ScientificFitsDataset
    import numpy as np

    ds = ScientificFitsDataset(path)
    hdul, raw = ds.open_memmap()
    data = ds.squeeze_to_3d(raw)
    nz, ny, nx = (int(data.shape[0]), int(data.shape[1]), int(data.shape[2]))
    seed = _bright_seed(data) if op == "lasso" else None
    hdul.close()

    t0 = time.perf_counter()
    if op == "preview":
        from app.compute import worker_cube_preview
        ds_factor = max(1, round(max(nz, ny, nx) / 128))
        worker_cube_preview(path, ds_factor)
    elif op == "slice":
        from app.compute import worker_cube_slice
        worker_cube_slice(path, nz // 2)
    elif op == "moment0":
        from app.compute import worker_moment
        worker_moment(path, 0, 0, nz - 1, False, 0.0)
    elif op == "lasso":
        from app.compute import worker_cube_kinematic_select
        worker_cube_kinematic_select(path, [list(seed)], [], 8.0, 1.0, 0.0, 0, 2.5)
    else:
        raise SystemExit(f"unknown op {op}")
    wall = time.perf_counter() - t0

    return {"nz": nz, "ny": ny, "nx": nx, "voxels": nz * ny * nx,
            "wall_s": round(wall, 3), "peak_rss_mb": round(_peak_rss_mb(), 1)}


def driver() -> None:
    OUT.mkdir(exist_ok=True)
    rows = []
    for label, path in CUBES:
        if not os.path.exists(path):
            print(f"skip {label}: not found"); continue
        size_mb = os.path.getsize(path) / 1e6
        rec = {"cube": label, "size_MB": round(size_mb, 0)}
        for op in OPS:
            env = dict(os.environ)
            if op == "moment0":
                env["VISIVO_MOMENT_STREAM_BYTES"] = "67108864"   # force the out-of-core streaming path
            p = subprocess.run(
                [sys.executable, __file__, "--run", op, path],
                capture_output=True, text=True, env=env)
            try:
                r = json.loads(p.stdout.strip().splitlines()[-1])
            except Exception:
                print(f"  {label}/{op} FAILED: {p.stderr.strip()[-300:]}")
                rec[f"{op}_s"] = rec[f"{op}_rss"] = None
                continue
            rec.setdefault("voxels", r["voxels"])
            rec["dims"] = f'{r["nx"]}×{r["ny"]}×{r["nz"]}'
            rec[f"{op}_s"] = r["wall_s"]
            rec[f"{op}_rss"] = r["peak_rss_mb"]
            print(f"  {label:12s} {op:8s} {r['wall_s']:7.2f}s  RSS {r['peak_rss_mb']:6.0f} MB")
        rows.append(rec)

    with open(OUT / "benchmark.csv", "w", newline="") as f:
        keys = ["cube", "dims", "size_MB", "voxels",
                *[f"{op}_{m}" for op in OPS for m in ("s", "rss")]]
        w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        w.writeheader(); w.writerows(rows)

    md = ["# VisIVO backend performance benchmark\n",
          f"Machine: {platform.platform()}. Times in seconds (wall), peak resident "
          "memory (MB); each op in its own process. Slice/preview extract from the "
          "memmap in ~constant memory regardless of cube size (out-of-core navigation); "
          "moment-0 uses the streaming path (bit-identical to in-memory); lasso memory "
          "is bounded by the seed-centred window, not the cube.\n",
          "| Cube | Dims | Size (MB) | Preview s / RSS | Slice s / RSS | "
          "Moment-0 s / RSS | Lasso s / RSS |",
          "|---|---|---:|---|---|---|---|"]
    for r in rows:
        cell = lambda op: (f'{r.get(f"{op}_s")}/{r.get(f"{op}_rss"):.0f}'
                           if r.get(f"{op}_s") is not None else "—")
        md.append(f'| {r["cube"]} | {r.get("dims","?")} | {r["size_MB"]:.0f} | '
                  f'{cell("preview")} | {cell("slice")} | {cell("moment0")} | '
                  f'{cell("lasso")} |')
    (OUT / "benchmark.md").write_text("\n".join(md) + "\n")
    print("\nWrote", OUT / "benchmark.md")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--run":
        print(json.dumps(run_one(sys.argv[2], sys.argv[3])))
    else:
        driver()
