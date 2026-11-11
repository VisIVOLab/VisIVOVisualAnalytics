"""Metrics of the VR page's refinement (issue #170), from the images and raw
numbers that ``vr_refinement.mjs`` saved in ``validation/out/vr_refinement/``.

Quality. At one still pose of HCG 44: the page's own rendering of the
reduced cube, the same with the backend's frame blended in (as shown, at two
settings), against the backend's reference frame of the full cube (full
canvas size, JPEG 100, rays sampled every quarter voxel). PSNR and SSIM on the
pixels of the emission (any of the images off the background, grown by 8
pixels) and on the whole frame. Two binnings: the page's default for HCG 44
(3 x 3 x 1) and 2 x 2 x 1 (the cube unbinned is over the page's 64 Mi-voxel
cap); part of what is left between the page and the reference is the
difference between two renderers (ray step, interpolation, jitter), not the
binning.

Reprojection. A frame for pose A drawn at pose B by the page (with depth, or
as a plane through the cube's centre), against the backend's own frame for B
at the same settings; "none" is A's frame shown unchanged.

Times. "Stop to refined" from turning the view at 90 deg/s for 0.6 s; the
local frame time with and without the composite; the latencies the page
measured.

    python validation/vr_refinement.py
"""
from __future__ import annotations

import csv
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage
from skimage.metrics import structural_similarity

HERE = Path(__file__).resolve().parent
OUT = HERE / "out" / "vr_refinement"
BG = np.array([11, 13, 18], dtype=float)    # the page's desktop clear colour, 0.043 0.051 0.07


def load(name: str, size: tuple[int, int] | None = None) -> np.ndarray:
    im = Image.open(OUT / name).convert("RGB")
    if size is not None and im.size != size:
        im = im.resize(size, Image.BOX)
    return np.asarray(im, dtype=float)


def footprint(*images: np.ndarray, grow: int = 8) -> np.ndarray:
    lit = np.zeros(images[0].shape[:2], bool)
    for im in images:
        lit |= (np.abs(im - BG) > 6).any(axis=2)
    return ndimage.binary_dilation(lit, iterations=grow)


def psnr(a: np.ndarray, b: np.ndarray, mask: np.ndarray | None = None) -> float:
    d = (a - b) ** 2
    mse = d[mask].mean() if mask is not None else d.mean()
    return float("inf") if mse == 0 else float(10 * np.log10(255.0 ** 2 / mse))


def ssim(a: np.ndarray, b: np.ndarray, mask: np.ndarray | None = None) -> float:
    _, full = structural_similarity(a, b, channel_axis=2, data_range=255, full=True)
    full = full.mean(axis=2)
    return float(full[mask].mean() if mask is not None else full.mean())


def stats(v: list[float]) -> str:
    v = [x for x in v if x is not None]
    if not v:
        return "n/a"
    return f"{np.median(v):.0f} ms median, {np.percentile(v, 95):.0f} p95 (n={len(v)})"


def main() -> int:
    raw = json.loads((OUT / "raw.json").read_text())
    rows, lines = [], ["# Refinement in the VR page: quality and times (issue #170)", "",
                       f"Cube `{Path(raw['cube']).name}` (HCG 44), headless Chrome on {raw['gpu']}, "
                       f"canvas {raw['canvas'][0]} x about {raw['canvas'][1]} px, local backend on the same Mac.", ""]
    lines += ["## Quality at a still pose", "",
              "Against the backend's reference (full cube, full canvas size, JPEG 100, quarter-voxel rays). "
              "Emission: the pixels of the emission, grown by 8 px.", "",
              "| cube on the page | image | PSNR emission (dB) | SSIM emission | PSNR frame (dB) | SSIM frame |",
              "|---|---|---|---|---|---|"]
    for label, run in raw["runs"].items():
        if "error" in run:
            lines.append(f"| {label} | not run: {run['error']} | | | | |")
            continue
        ref = load(f"{label}_reference.png")
        size = (ref.shape[1], ref.shape[0])
        imgs = {"page alone (reduced cube)": load(f"{label}_local.png", size)}
        for f in sorted(OUT.glob(f"{label}_refined_s*_q*.png")):
            s, q = f.stem.split("_")[-2:]
            imgs[f"refined, {s[1:]} x size, JPEG {q[1:]}"] = load(f.name, size)
        mask = footprint(ref, *imgs.values())
        binning = run["status"].split("\n")[0]
        for name, im in imgs.items():
            r = [label, name, psnr(im, ref, mask), ssim(im, ref, mask), psnr(im, ref), ssim(im, ref)]
            rows.append(["quality", *r])
            lines.append(f"| {binning} | {name} | {r[2]:.2f} | {r[3]:.4f} | {r[4]:.2f} | {r[5]:.4f} |")
    lines.append("")

    lines += ["## Reprojection", "",
              "A frame for pose A drawn by the page at pose B, against the backend's frame for B "
              "(full size, JPEG 95: the page's defaults); PSNR (dB) on the emission. "
              "*Orbit*: the eye turned about the cube's centre at 1 m (translation 1.7 cm per degree); "
              "*turn*: the head turned in place.", "",
              "| cube | motion | none (A as is) | plane through the centre | depth (first hit) | page alone at B |",
              "|---|---|---|---|---|---|"]
    for label, run in raw["runs"].items():
        for m in run.get("reprojection", []):
            fresh = load(f"{m['tag']}_fresh.png")
            size = (fresh.shape[1], fresh.shape[0])
            a = load(f"{label}_rep_A.png", size)
            plane, depth, local = (load(f"{m['tag']}_{k}.png", size) for k in ("plane", "depth", "local"))
            mask = footprint(fresh, a, plane, depth)
            vals = [psnr(x, fresh, mask) for x in (a, plane, depth, local)]
            rows.append(["reprojection", label, f"{m['kind']} {m['deg']} deg", *vals])
            lines.append(f"| {label} | {m['kind']} {m['deg']} deg | " + " | ".join(f"{v:.2f}" for v in vals) + " |")
    lines.append("")

    lines += ["## Times", ""]
    for label, run in raw["runs"].items():
        if "sweeps" not in run:
            continue
        sw = run["sweeps"]
        s = run["summary"]
        lines += [f"**{label}** ({run['status'].splitlines()[0]})", "",
                  f"- Camera stopped to refined frame first shown: {stats([x.get('firstSinceStop') for x in sw])}; "
                  f"fully faded in (120 ms fade): {stats([x.get('fullSinceStop') for x in sw])}; "
                  f"timeouts {sum(1 for x in sw if x.get('timeout'))}.",
                  f"- The same from the moment the page decided the view had stopped (what the panel shows): "
                  f"{stats([x['first'] for x in sw])} first shown, {stats([x['full'] for x in sw])} fully in.",
                  f"- Pose sent to frame decoded: {s['latency']['p50']:.0f} ms median, {s['latency']['p95']:.0f} p95; "
                  f"to frame received {s['network']['p50']:.0f} ms; JPEG and depth decode {s['decode']['p50']:.1f} ms; "
                  f"texture upload {s['upload']['p50']:.2f} ms median, {s['upload']['p95']:.2f} p95.",
                  f"- Last frame: {s['lastKB']:.0f} KB; server {json.dumps(s['lastServer'])}.",
                  f"- Poses sent {s['sent']}, frames received {s['received']}, dropped as stale {s['dropped']}, "
                  f"merged by the backend {s['skipped']}."]
        for mode, runs in run["cost"].items():
            gpu = [r["gpu"]["mean"] for r in runs if r["gpu"]]
            cpu = [r["cpu"]["mean"] for r in runs if r["cpu"]]
            name = "with the refined frame composited" if mode == "auto" else "page alone"
            lines.append(f"- Local frame, {name}: GPU {np.mean(gpu):.3f} ms, CPU {np.mean(cpu):.3f} ms "
                         f"(mean of {len(runs)} runs of 3 s).")
            rows.append(["cost", label, mode, float(np.mean(gpu)) if gpu else None, float(np.mean(cpu)) if cpu else None])
        lines.append("")
    (HERE / "out" / "vr_refinement.md").write_text("\n".join(lines) + "\n")
    with open(HERE / "out" / "vr_refinement.csv", "w", newline="") as f:
        csv.writer(f).writerows([["kind", "cube", "what", "v1", "v2", "v3", "v4"], *rows])
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
