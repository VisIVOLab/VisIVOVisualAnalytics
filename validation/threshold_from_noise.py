#!/usr/bin/env python
"""Default 3-D threshold from the noise (issue #93), against reference masks.

For each validation cube, the thresholds of several rules and what each lets
through: the voxels above it outside the reference source mask (noise drawn
as fog), the purity of what is drawn (the fraction of the voxels above it
inside the mask) and the share of the source flux inside the mask that stays
above it. The rules:

  - the 99th percentile, the backend's default before #93;
  - median + 3σ and median + 4σ, σ from the negative side;
  - the app's rule (``app.noise_threshold.default_threshold``), called the way
    the render session calls it: on a stride sample of about 5 M voxels, for
    the number of voxels of the whole cube.

HCG 44 is measured against both of its references (our SoFiA-2 run and the
published Serra et al. 2013 mask), WALLABY against its SoFiA-2 mask. Cubes
without a mask (``--extra PATH``) report only the voxels above each threshold
and their mirror below the median, which on pure noise are equal.

Run:  backend/.venv/bin/python validation/threshold_from_noise.py [--extra CUBE ...]
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

sys.path.insert(0, str(Path(__file__).resolve().parents[0].parent / "backend"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from app.noise_threshold import default_threshold, stride_for  # noqa: E402
from datasets import DATASETS, HERE, OUT  # noqa: E402

REFERENCES = [
    ("hgc44", "SoFiA-2", DATASETS["hgc44"].mask_path),
    ("hgc44", "Serra+2013", HERE / "sofia_out" / "serra2013_hcg44_mask.fits"),
    ("wallaby", "SoFiA-2", DATASETS["wallaby"].mask_path),
]


def load(path) -> np.ndarray:
    with fits.open(path, memmap=True) as hdul:
        hdu = next(h for h in hdul if h.data is not None and h.data.ndim >= 3)
        data = hdu.data
        while data.ndim > 3:
            data = data[0]
        return np.asarray(data, dtype=np.float32)


def rules(cube: np.ndarray) -> dict[str, float]:
    finite = cube[np.isfinite(cube)]
    med = float(np.median(finite))
    sigma = 1.4826 * float(np.median(med - finite[finite < med]))
    st = stride_for(cube.size)
    return {
        "p99 (before #93)": float(np.percentile(finite, 99.0)),
        "median + 3σ": med + 3.0 * sigma,
        "median + 4σ": med + 4.0 * sigma,
        "~100 noise voxels (#93)": default_threshold(cube[::st, ::st, ::st], cube.size),
    }, med, sigma


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--extra", nargs="*", default=[],
                    help="cubes without a reference mask, reported by counts only")
    args = ap.parse_args()

    rows = []
    cubes: dict[str, np.ndarray] = {}
    for name, ref, mask_path in REFERENCES:
        if name not in cubes:
            path = DATASETS[name].cube
            if not Path(path).exists():
                sys.exit(f"{path} not found: set {DATASETS[name].env}")
            cubes[name] = load(path)
        cube = cubes[name]
        finite = np.isfinite(cube)
        mask = (load(mask_path) > 0) & finite
        flux = float(cube[mask & (cube > 0)].sum())
        thresholds, med, sigma = rules(cube)
        for rule, t in thresholds.items():
            above = finite & (cube > t)
            inside = int((above & mask).sum())
            outside = int((above & ~mask).sum())
            rows.append({
                "cube": DATASETS[name].title, "reference": ref, "rule": rule,
                "threshold": t, "k": (t - med) / sigma, "noise_voxels": outside,
                "purity": inside / max(inside + outside, 1),
                "flux_kept": float(cube[above & mask].sum()) / flux,
            })
    for path in args.extra:
        cube = load(path)
        finite = np.isfinite(cube)
        thresholds, med, sigma = rules(cube)
        for rule, t in thresholds.items():
            rows.append({
                "cube": Path(path).name, "reference": "none", "rule": rule,
                "threshold": t, "k": (t - med) / sigma,
                "above": int((finite & (cube > t)).sum()),
                "mirror_below": int((finite & (cube < 2 * med - t)).sum()),
            })

    OUT.mkdir(exist_ok=True)
    fields = ["cube", "reference", "rule", "threshold", "k", "noise_voxels", "purity",
              "flux_kept", "above", "mirror_below"]
    with open(OUT / "threshold_from_noise.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    lines = ["# Default 3-D threshold from the noise (issue #93)", "",
             "| Cube | Reference | Rule | Threshold | k (σ) | Noise voxels | Purity | Flux kept |",
             "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        if r["reference"] == "none":
            lines.append(f"| {r['cube']} | none | {r['rule']} | {r['threshold']:.3g} | {r['k']:.2f} "
                         f"| {r['above']:,} above, {r['mirror_below']:,} mirrored below | – | – |")
        else:
            lines.append(f"| {r['cube']} | {r['reference']} | {r['rule']} | {r['threshold']:.3g} "
                         f"| {r['k']:.2f} | {r['noise_voxels']:,} | {r['purity']:.2f} "
                         f"| {100 * r['flux_kept']:.1f} % |")
    (OUT / "threshold_from_noise.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
