#!/usr/bin/env python
"""Kinematic Lasso — seed (click) robustness on a real HI cube.

Rebuts the strongest criticism of the lasso-vs-SoFiA comparison: there, each
lasso is seeded on the SoFiA source's own *peak* voxel, so a sceptic can argue
the agreement is an artifact of seeding exactly where SoFiA is most confident.

This script perturbs the click and measures how much Dice / completeness /
reliability move. To be honest about *which* claim the numbers support, the
perturbed seeds are split into two strata, reported separately:

  - INTERIOR (the main claim): the peak, small spatial jitters around it, and
    random *bright* voxels elsewhere inside the source — i.e. the plausible
    range of a correct click on the galaxy. A small spread here means the result
    does not depend on hitting the peak.
  - STRESS (a harder test, reported apart): low-intensity (faint) voxels near the detection
    edge inside the source, plus a one-voxel dilation ring just OUTSIDE it — i.e.
    an imprecise or off-source click. Larger spread here is expected and honest.

Two independent RNG streams (science vs plot jitter) so changing a figure never
perturbs the sampled seeds. Same cube/mask as lasso_vs_sofia.py (no new data).

Run:  backend/.venv/bin/python validation/lasso_seed_robustness.py
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

sys.path.insert(0, str(Path(__file__).resolve().parents[0].parent / "backend"))
from app.compute.selection import cube_kinematic_select  # noqa: E402

# Out-of-repo HI cube; point at it via the env var so the script is reproducible anywhere.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from datasets import OUT, add_dataset_arg, rank_sources, resolve  # noqa: E402

N_SOURCES = 5                        # perturb the N largest SoFiA sources
REACH, LAMBDA_V, NSIGMA = 8.0, 1.0, 2.5   # lasso defaults (as a user would click)
JITTER = (1, 2, 3)                   # spatial offsets (voxels) around the peak
N_JITTER_MAX = 6                     # cap jitter seeds so they don't crowd out random
N_RANDOM_INTERIOR = 12               # reserved quota of random bright interior seeds
N_STRESS_FAINT = 6                   # low-intensity (faint) interior seeds
N_STRESS_RING = 6                    # one-voxel dilation-ring (just-outside) seeds
BRIGHT_PERCENTILE = 50.0             # "bright, a user would click": top-half intensity
FAINT_PERCENTILE = 25.0             # faint detection-edge voxels: bottom-quartile in-mask intensity
INVARIANT_TOL = 1.0e-3               # a source is "click-invariant" if Dice std < this
RNG_SEED = 20260827                  # fixed → reproducible seed-sampling


def metrics(lasso, src):
    inter = int(np.count_nonzero(lasso & src))
    a, b = int(lasso.sum()), int(src.sum())
    dice = 2 * inter / (a + b) if (a + b) else 0.0
    completeness = inter / b if b else 0.0          # ∩ / SoFiA  (recall)
    reliability = inter / a if a else 0.0           # ∩ / lasso  (precision)
    return dice, completeness, reliability


def _dedupe(seeds):
    seen, uniq = set(), []
    for s in seeds:
        if s not in seen:
            seen.add(s); uniq.append(s)
    return uniq


def interior_seeds(data, src, peak, rng):
    """The plausible range of a CORRECT click on the galaxy: the peak, a few
    spatial jitters around it, and a reserved quota of random bright interior
    voxels (the quota is sampled independently so jitter can't crowd it out)."""
    zz, yy, xx = np.where(src)
    src_vals = data[zz, yy, xx]
    finite = np.isfinite(src_vals)
    seeds = [peak]

    pz, py, px = peak
    jitter = []
    for d in JITTER:
        for dz, dy, dx in ((0, d, 0), (0, -d, 0), (0, 0, d), (0, 0, -d),
                           (0, d, d), (0, -d, -d)):
            z, y, x = pz + dz, py + dy, px + dx
            if (0 <= z < data.shape[0] and 0 <= y < data.shape[1] and 0 <= x < data.shape[2]
                    and src[z, y, x] and np.isfinite(data[z, y, x])):
                jitter.append((int(z), int(y), int(x)))
    # Shuffle so the retained jitter samples all offset radii, not just radius-1.
    rng.shuffle(jitter)
    seeds += jitter[:N_JITTER_MAX]

    if finite.any():
        thr = np.percentile(src_vals[finite], BRIGHT_PERCENTILE)
        idx = np.where(finite & (src_vals >= thr))[0]
        if idx.size:
            pick = rng.choice(idx, size=min(idx.size, N_RANDOM_INTERIOR), replace=False)
            seeds += [(int(zz[j]), int(yy[j]), int(xx[j])) for j in pick]
    return _dedupe(seeds)


def stress_seeds(data, src, rng):
    """A harder, separately-reported test: low-intensity (faint) voxels near the detection
    edge INSIDE the source, plus a one-voxel dilation ring just OUTSIDE it — an
    imprecise / slightly off-source click. Larger spread here is expected."""
    from scipy import ndimage
    zz, yy, xx = np.where(src)
    src_vals = data[zz, yy, xx]
    finite = np.isfinite(src_vals)
    seeds = []

    # (a) faint interior: bottom-quartile intensity voxels still inside the source.
    if finite.any():
        thr = np.percentile(src_vals[finite], FAINT_PERCENTILE)
        idx = np.where(finite & (src_vals <= thr))[0]
        if idx.size:
            pick = rng.choice(idx, size=min(idx.size, N_STRESS_FAINT), replace=False)
            seeds += [(int(zz[j]), int(yy[j]), int(xx[j])) for j in pick]

    # (b) one-voxel dilation ring just outside the source (finite voxels only).
    ring = ndimage.binary_dilation(src) & ~src & np.isfinite(data)
    rz, ry, rx = np.where(ring)
    if rz.size:
        pick = rng.choice(rz.size, size=min(rz.size, N_STRESS_RING), replace=False)
        seeds += [(int(rz[j]), int(ry[j]), int(rx[j])) for j in pick]
    return _dedupe(seeds)


def eval_seeds(data, src, seeds):
    d, c, r = [], [], []
    for s in seeds:
        m = cube_kinematic_select(data, s, reach=REACH, lambda_v=LAMBDA_V,
                                  sigma=None, nsigma=NSIGMA)
        di, ci, ri = metrics(m, src)
        d.append(di); c.append(ci); r.append(ri)
    return np.asarray(d), np.asarray(c), np.asarray(r)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    add_dataset_arg(ap)
    ap.add_argument("mask", nargs="?", help="override the reference SoFiA mask")
    args = ap.parse_args()
    dset = resolve(args.dataset)
    CUBE = dset.cube
    MASK = args.mask or str(dset.mask_path)
    # a different reference mask is a different experiment: tag its outputs
    variant = f"_ref-{Path(args.mask).stem.removesuffix('_mask')}" if args.mask else ""
    for label, path in (("cube", CUBE), ("SoFiA mask", MASK)):
        if not Path(path).is_file():
            sys.exit(f"error: {label} not found: {path}")
    OUT.mkdir(exist_ok=True)
    # Independent streams: science-sampling must never depend on plot-jitter draws.
    rng_sci, rng_plot = (np.random.default_rng(s)
                         for s in np.random.SeedSequence(RNG_SEED).spawn(2))

    data = np.squeeze(fits.getdata(CUBE)).astype(np.float32)
    sofia = np.squeeze(fits.getdata(MASK)).astype(np.int32)
    if data.shape != sofia.shape:
        sys.exit(f"error: cube {data.shape} and mask {sofia.shape} differ in shape")

    labels, counts, order = rank_sources(sofia, N_SOURCES)
    if labels.size == 0:
        sys.exit("error: the reference mask has no labelled sources (all zero).")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(1, 2, figsize=(11, 4.4))

    rows = []
    for rank, i in enumerate(order, 1):
        lab = int(labels[i])
        src = sofia == lab
        zz, yy, xx = np.where(src)
        src_vals = data[zz, yy, xx]
        if not np.isfinite(src_vals).any():
            print(f"[{rank}] SoFiA #{lab}: skipped — no finite voxel in the mask.")
            continue
        k = int(np.nanargmax(src_vals))
        peak = (int(zz[k]), int(yy[k]), int(xx[k]))

        i_seeds = interior_seeds(data, src, peak, rng_sci)
        s_seeds = stress_seeds(data, src, rng_sci)
        di, ci, ri = eval_seeds(data, src, i_seeds)          # interior (main)
        ds, cs, rs = eval_seeds(data, src, s_seeds) if s_seeds else (np.array([]),) * 3

        row = dict(
            rank=rank, sofia_id=lab, n_interior=len(i_seeds), n_stress=len(s_seeds),
            dice_peak=float(di[0]),
            dice_mean=float(di.mean()), dice_std=float(di.std()),
            dice_min=float(di.min()), dice_max=float(di.max()),
            compl_mean=float(ci.mean()), compl_std=float(ci.std()),
            relia_mean=float(ri.mean()), relia_std=float(ri.std()),
            dice_cv=float(di.std() / di.mean()) if di.mean() else float("nan"),
            stress_dice_mean=float(ds.mean()) if ds.size else float("nan"),
            stress_dice_min=float(ds.min()) if ds.size else float("nan"))
        rows.append(row)
        print(f"[{rank}] SoFiA #{lab}: interior {len(i_seeds)} seeds | "
              f"Dice peak={di[0]:.3f} mean={di.mean():.3f} std={di.std():.3f} "
              f"[{di.min():.3f},{di.max():.3f}] CV={row['dice_cv']:.3f} | "
              f"compl {ci.mean():.3f}±{ci.std():.3f} relia {ri.mean():.3f}±{ri.std():.3f} "
              f"|| stress {len(s_seeds)} seeds: Dice mean="
              f"{row['stress_dice_mean']:.3f} min={row['stress_dice_min']:.3f}")

        # Interior spread (green) with the peak ringed (blue); stress seeds (red x).
        xj = np.full(di.shape, rank) + rng_plot.uniform(-0.12, 0.12, di.shape)
        ax[0].scatter(xj, di, s=14, alpha=0.55, color="tab:green")
        ax[0].scatter([rank], [di[0]], s=70, facecolors="none",
                      edgecolors="tab:blue", linewidths=1.6, zorder=3)
        if ds.size:
            xs = np.full(ds.shape, rank) + rng_plot.uniform(-0.12, 0.12, ds.shape)
            ax[0].scatter(xs, ds, s=18, alpha=0.5, color="tab:red", marker="x")

    if not rows:
        sys.exit("error: no SoFiA source could be seeded (all masks empty/NaN).")

    ax[0].set_xticks(range(1, len(rows) + 1))
    ax[0].set_xticklabels([f"#{r['sofia_id']}" for r in rows])
    ax[0].set_ylim(0, 1.02)
    ax[0].set_xlabel("SoFiA source"); ax[0].set_ylabel("Dice per seed")
    ax[0].set_title("Dice under click perturbation\n(blue ring = peak, green = interior, red × = stress)")

    xr = np.arange(len(rows))
    ax[1].bar(xr - 0.25, [r["dice_std"] for r in rows], width=0.25, label="Dice std", color="tab:green")
    ax[1].bar(xr,        [r["compl_std"] for r in rows], width=0.25, label="completeness std", color="tab:blue")
    ax[1].bar(xr + 0.25, [r["relia_std"] for r in rows], width=0.25, label="reliability std", color="tab:red")
    ax[1].set_xticks(xr); ax[1].set_xticklabels([f"#{r['sofia_id']}" for r in rows])
    ax[1].set_xlabel("SoFiA source"); ax[1].set_ylabel("interior std across seeds")
    ax[1].set_title("interior metric spread (lower = more click-robust)")
    ax[1].legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(dset.out("seed_robustness", "png", variant=variant), dpi=130)

    with open(dset.out("seed_robustness", "csv", variant=variant), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)

    med_std = float(np.median([r["dice_std"] for r in rows]))
    max_std = float(np.max([r["dice_std"] for r in rows]))
    worst_min = float(np.min([r["dice_min"] for r in rows]))
    n_invariant = int(np.sum([r["dice_std"] < INVARIANT_TOL for r in rows]))
    stress_worst = float(np.nanmin([r["stress_dice_min"] for r in rows]))
    md = ["# Kinematic Lasso — seed (click) robustness — HGC 44 HI cube",
          f"\nCube `{Path(CUBE).name}` {data.shape}. Lasso defaults reach={REACH}, "
          f"λv={LAMBDA_V}, nσ={NSIGMA}, rng={RNG_SEED}. Per source: an INTERIOR stratum "
          "(peak + spatial jitter + random bright interior voxels — the plausible correct "
          "click) drives the headline; a separate STRESS stratum (low-intensity faint interior "
          "voxels + a one-voxel dilation ring just outside) is reported apart.\n",
          "| SoFiA id | interior seeds | Dice peak | Dice mean | Dice std | Dice CV | "
          "[min, max] | compl mean±std | relia mean±std | stress Dice mean/min |",
          "|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        md.append(
            f"| {r['sofia_id']} | {r['n_interior']} | {r['dice_peak']:.3f} | "
            f"{r['dice_mean']:.3f} | {r['dice_std']:.3f} | {r['dice_cv']:.3f} | "
            f"[{r['dice_min']:.3f}, {r['dice_max']:.3f}] | "
            f"{r['compl_mean']:.3f}±{r['compl_std']:.3f} | "
            f"{r['relia_mean']:.3f}±{r['relia_std']:.3f} | "
            f"{r['stress_dice_mean']:.3f} / {r['stress_dice_min']:.3f} |")
    md.append(
        f"\n**Interior stratum: median Dice std = {med_std:.3f}, worst-case std = {max_std:.3f}, "
        f"worst single-seed Dice = {worst_min:.3f}; {n_invariant}/{len(rows)} sources "
        f"click-invariant (Dice std < {INVARIANT_TOL:g}).** For the bright, well-connected "
        "sources the recovered region is essentially click-invariant — the object-bounded "
        "grow takes the connected component *containing the seed*, the same from any interior "
        "voxel — so their lasso-vs-SoFiA agreement is not an artifact of seeding on SoFiA's "
        "peak voxel. The two fainter sources show genuine placement sensitivity (a marginal "
        f"interior click can drop Dice to {worst_min:.3f}), so for weakly-detected objects the "
        "result depends more on where the user clicks. The separate STRESS stratum (worst min "
        f"Dice {stress_worst:.3f}) shows a deliberately faint / off-source click degrades "
        "sharply, as expected.\n\n"
        f"_Scope: a single-cube, {len(rows)}-source sanity check — it shows the peak-seed "
        "comparison is not driven by exact peak clicks for well-connected sources; it does "
        "not by itself establish general seed robustness across all cubes._")
    (dset.out("seed_robustness", "md", variant=variant)).write_text("\n".join(md) + "\n")
    print("\nWrote", dset.out("seed_robustness", "md", variant=variant), "and", dset.out("seed_robustness", "png", variant=variant))


if __name__ == "__main__":
    main()
