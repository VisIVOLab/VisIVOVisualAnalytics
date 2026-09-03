#!/usr/bin/env python
"""Kinematic Lasso vs SoFiA-2 — scientific validation on a real HI cube.

Compares the VisIVO Kinematic-Lasso geodesic selection against a SoFiA-2 source
mask on the same cube. For each of the main SoFiA-detected galaxies we seed the
lasso on the source's peak voxel, grow the selection with default parameters,
and measure:

  - spatial agreement: Dice, IoU, completeness (∩/SoFiA), reliability (∩/lasso)
  - integrated HI flux (Jy·km/s) — lasso vs SoFiA, and their ratio
  - the total (spatially-integrated) spectrum per channel, overlaid

Outputs a metrics table (markdown + CSV) and per-source figures under out/.

Run:  backend/.venv/bin/python validation/lasso_vs_sofia.py [--dataset NAME] [MASK]
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
from app.compute.selection import cube_kinematic_select  # noqa: E402
from datasets import OUT, add_dataset_arg, resolve  # noqa: E402

N_SOURCES = 5                 # compare the N largest SoFiA sources
REACH, LAMBDA_V, NSIGMA = 8.0, 1.0, 2.5   # lasso defaults (as a user would click)


def beam_area_px(hd) -> float:
    """Gaussian beam solid angle in pixels: (π/4ln2)·BMAJ·BMIN / (|CD1|·|CD2|)."""
    bmaj, bmin = float(hd["BMAJ"]), float(hd["BMIN"])
    cd1, cd2 = abs(float(hd["CDELT1"])), abs(float(hd["CDELT2"]))
    return (np.pi / (4.0 * np.log(2.0))) * bmaj * bmin / (cd1 * cd2)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    add_dataset_arg(ap)
    ap.add_argument("mask", nargs="?", help="override the reference SoFiA mask "
                                            "(e.g. a differently-parameterised run)")
    args = ap.parse_args()
    # resolve() fails early with an actionable message rather than a cryptic astropy
    # error: the paper artifact must be runnable by a co-author who has only cloned.
    ds = resolve(args.dataset)
    CUBE = ds.cube
    MASK = args.mask or str(ds.mask_path)
    if args.mask and not Path(MASK).is_file():
        sys.exit(f"error: SoFiA mask not found: {MASK}")

    OUT.mkdir(exist_ok=True)
    hd = fits.getheader(CUBE)
    data = np.squeeze(fits.getdata(CUBE)).astype(np.float32)
    sofia = np.squeeze(fits.getdata(MASK)).astype(np.int32)
    if data.shape != sofia.shape:
        sys.exit(f"error: cube {data.shape} and mask {sofia.shape} differ in shape")

    barea = beam_area_px(hd)
    dv = abs(float(hd["CDELT3"])) / 1000.0            # m/s → km/s per channel
    # Jy·km/s over a boolean voxel mask.
    flux = lambda msk: float(data[msk].sum()) * dv / barea

    labels, counts = np.unique(sofia[sofia > 0], return_counts=True)
    if labels.size == 0:
        sys.exit("error: the SoFiA mask has no labelled sources (all zero).")
    order = np.argsort(counts)[::-1][:N_SOURCES]

    rows = []
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    for rank, i in enumerate(order, 1):
        lab = int(labels[i])
        src = sofia == lab
        zz, yy, xx = np.where(src)
        src_vals = data[zz, yy, xx]
        if not np.isfinite(src_vals).any():
            # A SoFiA source whose every voxel is NaN has no valid peak to seed on — skip it
            # (seeding a NaN voxel would just be rejected by the lasso).
            print(f"[{rank}] SoFiA #{lab}: skipped — no finite voxel in the mask.")
            continue
        # Peak voxel = the click. nanargmax so a NaN inside the SoFiA mask can't win the peak.
        k = int(np.nanargmax(src_vals))
        seed = (int(zz[k]), int(yy[k]), int(xx[k]))

        lasso = cube_kinematic_select(
            data, seed, reach=REACH, lambda_v=LAMBDA_V, sigma=None, nsigma=NSIGMA)

        inter = int(np.count_nonzero(src & lasso))
        a, b = int(lasso.sum()), int(src.sum())
        dice = 2 * inter / (a + b) if (a + b) else 0.0
        iou = inter / (a + b - inter) if (a + b - inter) else 0.0
        completeness = inter / b if b else 0.0            # fraction of SoFiA recovered
        reliability = inter / a if a else 0.0             # fraction of lasso that is real
        fl, fs = flux(lasso), flux(src)
        rows.append(dict(
            rank=rank, sofia_id=lab, sofia_vox=b, lasso_vox=a, inter=inter,
            dice=dice, iou=iou, completeness=completeness, reliability=reliability,
            flux_sofia=fs, flux_lasso=fl, flux_ratio=(fl / fs if fs else float("nan")),
            seed_zyx=seed))
        ratio_str = f"{fl / fs:.3f}" if fs else "n/a"   # guard a zero SoFiA flux
        print(f"[{rank}] SoFiA #{lab}: Dice={dice:.3f} IoU={iou:.3f} "
              f"compl={completeness:.3f} relia={reliability:.3f} "
              f"Fsofia={fs:.2f} Flasso={fl:.2f} ratio={ratio_str}")

        # ── figures: moment-0 with both mask contours + total-spectrum overlay ──
        z0, z1 = min(zz.min(), np.where(lasso.any((1, 2)))[0].min()), \
                 max(zz.max(), np.where(lasso.any((1, 2)))[0].max())
        y0, y1, x0, x1 = yy.min(), yy.max(), xx.min(), xx.max()
        pad = 20
        y0, y1 = max(0, y0 - pad), min(data.shape[1], y1 + pad)
        x0, x1 = max(0, x0 - pad), min(data.shape[2], x1 + pad)
        mom0 = data[z0:z1 + 1, y0:y1 + 1, x0:x1 + 1].sum(0)

        fig, ax = plt.subplots(1, 2, figsize=(11, 4.4))
        ax[0].imshow(mom0, origin="lower", cmap="gray_r")
        ax[0].contour(src[z0:z1 + 1, y0:y1 + 1, x0:x1 + 1].any(0), levels=[0.5],
                      colors="tab:blue", linewidths=1.2)
        ax[0].contour(lasso[z0:z1 + 1, y0:y1 + 1, x0:x1 + 1].any(0), levels=[0.5],
                      colors="tab:green", linewidths=1.2)
        ax[0].set_title(f"SoFiA #{lab}: moment-0 + masks\n(blue=SoFiA, green=Lasso)")
        ax[0].set_xticks([]); ax[0].set_yticks([])

        spec_s = np.where(src, data, 0.0).sum((1, 2)) * dv / barea
        spec_l = np.where(lasso, data, 0.0).sum((1, 2)) * dv / barea
        zc = np.arange(data.shape[0])
        ax[1].plot(zc, spec_s, color="tab:blue", label="SoFiA")
        ax[1].plot(zc, spec_l, color="tab:green", ls="--", label="Lasso")
        ax[1].set_xlim(max(0, z0 - 5), min(data.shape[0], z1 + 5))
        ax[1].set_xlabel("channel"); ax[1].set_ylabel("flux (Jy·km/s)")
        ax[1].set_title("total spectrum"); ax[1].legend()
        fig.tight_layout()
        fig.savefig(ds.out(f"source_{lab:02d}", "png"), dpi=130)
        plt.close(fig)

    # ── metrics table ──
    if not rows:
        sys.exit("error: no SoFiA source could be seeded (all masks empty/NaN).")
    with open(ds.out("metrics", "csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)

    md = [f"# Kinematic Lasso vs SoFiA-2 — {ds.title} ({ds.instrument})",
          f"\n{ds.note}.\n",
          f"\nCube `{Path(CUBE).name}` {data.shape}, beam {barea:.1f} px, "
          f"Δv {dv:.2f} km/s. Lasso defaults reach={REACH}, λv={LAMBDA_V}, nσ={NSIGMA}. "
          f"Seed = each SoFiA source's peak voxel.\n",
          "| SoFiA id | SoFiA vox | Lasso vox | Dice | IoU | Compl. | Relia. | "
          "F_SoFiA | F_Lasso | ratio |",
          "|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        md.append(f"| {r['sofia_id']} | {r['sofia_vox']} | {r['lasso_vox']} | "
                  f"{r['dice']:.3f} | {r['iou']:.3f} | {r['completeness']:.3f} | "
                  f"{r['reliability']:.3f} | {r['flux_sofia']:.2f} | "
                  f"{r['flux_lasso']:.2f} | {r['flux_ratio']:.3f} |")
    d = np.array([r["dice"] for r in rows]); fr = np.array([r["flux_ratio"] for r in rows])
    # nanmedian: a source with zero SoFiA flux contributes a NaN ratio (guarded above).
    md.append(f"\n**Median Dice {np.median(d):.3f}, median flux ratio {np.nanmedian(fr):.3f}.**")
    ds.out("metrics", "md").write_text("\n".join(md) + "\n")
    print("\nWrote", ds.out("metrics", "md"), "and per-source figures.")


if __name__ == "__main__":
    main()
