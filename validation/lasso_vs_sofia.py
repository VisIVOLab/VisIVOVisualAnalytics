#!/usr/bin/env python
"""Kinematic Lasso vs SoFiA-2 — scientific validation on a real HI cube.

Compares the VisIVO Kinematic-Lasso geodesic selection against a SoFiA-2 source
mask on the same cube (Hickson Compact Group 44 HI observation). For each of the
main SoFiA-detected galaxies we seed the lasso on the source's peak voxel, grow
the selection with default parameters, and measure:

  - spatial agreement: Dice, IoU, completeness (∩/SoFiA), reliability (∩/lasso)
  - integrated HI flux (Jy·km/s) — lasso vs SoFiA, and their ratio
  - the total (spatially-integrated) spectrum per channel, overlaid

Outputs a metrics table (markdown + CSV) and per-source figures under out/.

Run:  backend/.venv/bin/python validation/lasso_vs_sofia.py
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

sys.path.insert(0, str(Path(__file__).resolve().parents[0].parent / "backend"))
from app.compute.selection import cube_kinematic_select  # noqa: E402

CUBE = "/Users/fvitello/downloads/DemoData/hgc44_3axes.fits"
# Reproducible SoFiA-2 mask from validation/sofia_hgc44.par (see README). Override
# with argv[1] to compare against another mask (e.g. the pre-existing run5 mask).
MASK = str(Path(__file__).resolve().parent / "sofia_out" / "sofia_hgc44_mask.fits")
if len(sys.argv) > 1:
    MASK = sys.argv[1]
OUT = Path(__file__).resolve().parent / "out"
N_SOURCES = 5                 # compare the N largest SoFiA sources
REACH, LAMBDA_V, NSIGMA = 8.0, 1.0, 2.5   # lasso defaults (as a user would click)


def beam_area_px(hd) -> float:
    """Gaussian beam solid angle in pixels: (π/4ln2)·BMAJ·BMIN / (|CD1|·|CD2|)."""
    bmaj, bmin = float(hd["BMAJ"]), float(hd["BMIN"])
    cd1, cd2 = abs(float(hd["CDELT1"])), abs(float(hd["CDELT2"]))
    return (np.pi / (4.0 * np.log(2.0))) * bmaj * bmin / (cd1 * cd2)


def main() -> None:
    OUT.mkdir(exist_ok=True)
    hd = fits.getheader(CUBE)
    data = np.squeeze(fits.getdata(CUBE)).astype(np.float32)
    sofia = np.squeeze(fits.getdata(MASK)).astype(np.int32)
    assert data.shape == sofia.shape, (data.shape, sofia.shape)

    barea = beam_area_px(hd)
    dv = abs(float(hd["CDELT3"])) / 1000.0            # m/s → km/s per channel
    # Jy·km/s over a boolean voxel mask.
    flux = lambda msk: float(data[msk].sum()) * dv / barea

    labels, counts = np.unique(sofia[sofia > 0], return_counts=True)
    order = np.argsort(counts)[::-1][:N_SOURCES]

    rows = []
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    for rank, i in enumerate(order, 1):
        lab = int(labels[i])
        src = sofia == lab
        zz, yy, xx = np.where(src)
        k = int(np.argmax(data[zz, yy, xx]))
        seed = (int(zz[k]), int(yy[k]), int(xx[k]))       # peak voxel = the click

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
        print(f"[{rank}] SoFiA #{lab}: Dice={dice:.3f} IoU={iou:.3f} "
              f"compl={completeness:.3f} relia={reliability:.3f} "
              f"Fsofia={fs:.2f} Flasso={fl:.2f} ratio={fl/fs:.3f}")

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
        fig.savefig(OUT / f"source_{lab:02d}.png", dpi=130)
        plt.close(fig)

    # ── metrics table ──
    with open(OUT / "metrics.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)

    md = ["# Kinematic Lasso vs SoFiA-2 — HGC 44 HI cube",
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
    md.append(f"\n**Median Dice {np.median(d):.3f}, median flux ratio {np.median(fr):.3f}.**")
    (OUT / "metrics.md").write_text("\n".join(md) + "\n")
    print("\nWrote", OUT / "metrics.md", "and per-source figures.")


if __name__ == "__main__":
    main()
