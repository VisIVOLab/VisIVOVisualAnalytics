#!/usr/bin/env python
"""Figure 3 (data-driven panels) — the kinematic lasso on a real HI galaxy.

Produces the science panels of the paper's Figure 3 on an HGC 44 galaxy, headless:
  (b) the grown selection outlined on the galaxy's moment-0;
  (c) the kinematic knob λv: low λv follows the rotation further in velocity;
  (e) isolate  — the galaxy on a NaN background (masked moment-0);
  (f) remove   — the residual cube (masked moment-0).
Panels (a) volume rendering with the seed double-click and (d) per-channel polygon
refinement are live GUI screenshots, captured in the app.

Run:  backend/.venv/bin/python validation/figure3_lasso_hgc44.py
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "backend"))
from app.compute.selection import cube_kinematic_select  # noqa: E402

CUBE = "/Users/fvitello/downloads/DemoData/hgc44_3axes.fits"
MASK = str(Path(__file__).resolve().parent / "sofia_out" / "sofia_hgc44_mask.fits")
OUT = Path(__file__).resolve().parent / "out"


def moment0(cube, sub=None):
    d = cube if sub is None else np.where(sub, cube, np.nan)
    return np.nansum(np.where(np.isfinite(d), d, 0.0), axis=0)


def main():
    OUT.mkdir(exist_ok=True)
    data = np.squeeze(fits.getdata(CUBE)).astype(np.float32)
    sofia = np.squeeze(fits.getdata(MASK)).astype(np.int32)
    labels, counts = np.unique(sofia[sofia > 0], return_counts=True)
    lab = int(labels[np.argmax(counts)])                       # the biggest galaxy
    src = sofia == lab
    zz, yy, xx = np.where(src)
    k = int(np.argmax(data[zz, yy, xx]))
    seed = (int(zz[k]), int(yy[k]), int(xx[k]))

    # crop to the galaxy for display
    pad = 25
    y0, y1 = max(0, yy.min() - pad), min(data.shape[1], yy.max() + pad)
    x0, x1 = max(0, xx.min() - pad), min(data.shape[2], xx.max() + pad)
    zc0, zc1 = max(0, zz.min() - 5), min(data.shape[0], zz.max() + 5)
    crop = lambda m: m[y0:y1, x0:x1]

    sel = cube_kinematic_select(data, seed, reach=8.0, lambda_v=1.0, nsigma=2.5)
    lo = cube_kinematic_select(data, seed, reach=8.0, lambda_v=0.35, nsigma=2.5)
    hi = cube_kinematic_select(data, seed, reach=8.0, lambda_v=1.6, nsigma=2.5)
    print(f"galaxy #{lab} seed {seed}: sel={int(sel.sum())} "
          f"loλv={int(lo.sum())} hiλv={int(hi.sum())} vox")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(2, 2, figsize=(10, 8.6))

    # (b) grown selection outlined on moment-0
    m0 = crop(moment0(data[zc0:zc1], None))
    ax[0, 0].imshow(m0, origin="lower", cmap="gray_r")
    ax[0, 0].contour(crop(sel[zc0:zc1].any(0)), [0.5], colors="tab:green", linewidths=1.4)
    ax[0, 0].set_title("(b) grown selection on moment-0")

    # (c) kinematic knob λv: per-channel selected area
    zc = np.arange(data.shape[0])
    area = lambda m: m.sum((1, 2))
    ax[0, 1].plot(zc, area(lo), color="tab:blue", label="low λv = 0.35")
    ax[0, 1].plot(zc, area(sel), color="gray", ls=":", label="λv = 1.0")
    ax[0, 1].plot(zc, area(hi), color="tab:red", label="high λv = 1.6")
    ax[0, 1].set_xlim(zz.min() - 8, zz.max() + 8)
    ax[0, 1].set_xlabel("channel (velocity)"); ax[0, 1].set_ylabel("selected pixels")
    ax[0, 1].set_title("(c) kinematic knob λv: velocity extent"); ax[0, 1].legend()

    # (e) isolate — galaxy on NaN background
    iso = moment0(data[zc0:zc1], sel[zc0:zc1])
    im = ax[1, 0].imshow(crop(iso), origin="lower", cmap="magma")
    ax[1, 0].set_title("(e) isolate → galaxy on blank background")
    fig.colorbar(im, ax=ax[1, 0], fraction=0.046)

    # (f) remove — residual
    res = moment0(data[zc0:zc1], ~sel[zc0:zc1])
    im2 = ax[1, 1].imshow(crop(res), origin="lower", cmap="magma")
    ax[1, 1].set_title("(f) remove → residual")
    fig.colorbar(im2, ax=ax[1, 1], fraction=0.046)
    for a in (ax[0, 0], ax[1, 0], ax[1, 1]):
        a.set_xticks([]); a.set_yticks([])

    fig.suptitle(f"Kinematic lasso on an HGC 44 HI galaxy "
                 f"(one double-click, {int(sel.sum())} voxels)", y=0.995)
    fig.tight_layout(rect=(0, 0, 1, 0.98))
    fig.savefig(OUT / "figure3_lasso.png", dpi=130)
    print("Wrote", OUT / "figure3_lasso.png")


if __name__ == "__main__":
    main()
