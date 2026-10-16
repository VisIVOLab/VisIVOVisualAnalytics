#!/usr/bin/env python
"""The eight panels of Figure 1, rendered from the products of the NGC 3137
session rather than captured from the screen, so that each panel shows the
actual object the corresponding stage produced. figure1_storyboard.py then
lays them out.

Run:  backend/.venv/bin/python validation/figure1_panels_ngc3137.py
      backend/.venv/bin/python validation/figure1_storyboard.py \
          --src validation/out/fig1 --out validation/out/figure1_storyboard.png
"""
from __future__ import annotations

import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from usecase_ngc3137 import run, SEED, SMOOTH, REACH, LAMBDA_V  # noqa: E402

R = run(verbose=False)
vel, data, iso, m0, m1 = R["vel"], R["data"], R["isolated"], R["m0"], R["m1"]
mask, warp, vw = R["mask"], R["model_warp"], R["vmod_warp"]
OUT = HERE / "out" / "fig1"; OUT.mkdir(parents=True, exist_ok=True)
csys = int(np.argmin(np.abs(vel - R["vsys_fit"])))
capp = int(np.argmin(np.abs(vel - (R["vsys_fit"] - 0.6 * R["vrot_sini"]))))
crec = int(np.argmin(np.abs(vel - (R["vsys_fit"] + 0.6 * R["vrot_sini"]))))
ys, xs = np.where(np.isfinite(m1))
crop = (slice(max(0, ys.min() - 12), ys.max() + 13), slice(max(0, xs.min() - 12), xs.max() + 13))
vmax = np.nanpercentile(data[:, crop[0], crop[1]], 99.7)

def panel(name, draw, note):
    fig, ax = plt.subplots(figsize=(3.2, 3.6))
    draw(ax); ax.set_xticks([]); ax.set_yticks([])
    ax.text(0.03, 0.02, note, transform=ax.transAxes, fontsize=8, va="bottom", ha="left",
            bbox=dict(fc="white", ec="none", alpha=0.85, pad=1.5))
    fig.savefig(OUT / f"{name}.png", dpi=200, bbox_inches="tight"); plt.close(fig)

def channel(ax, cube, c, cmap="Greys", contour=None, color="#d62728", ls="-"):
    ax.imshow(np.ma.masked_invalid(cube[c][crop]), origin="lower", cmap=cmap, vmin=0, vmax=vmax,
              interpolation="nearest")
    if contour is not None:
        ax.contour(contour[c][crop].astype(float), levels=[0.5], colors=[color], linewidths=1.3,
                   linestyles=ls)

panel("01_open", lambda ax: channel(ax, data, csys), f"channel {csys}, {vel[csys]:.0f} km/s")
def explore(ax):
    ax.imshow(np.ma.masked_invalid(data[crec][crop]), origin="lower", cmap="Greys", vmin=0, vmax=vmax,
              interpolation="nearest")
    ax.plot([SEED[2] - crop[1].start], [SEED[1] - crop[0].start], marker="+", ms=14, mew=2, color="#d62728")
panel("02_explore", explore, f"channel {crec}, {vel[crec]:.0f} km/s; the click")
panel("03_lasso", lambda ax: channel(ax, data, crec, contour=mask),
      f"lasso: {R['n_sel']:,} voxels")
panel("04_isolated", lambda ax: channel(ax, iso, crec), "isolated: NaN outside the mask")
panel("05_moment0", lambda ax: ax.imshow(np.ma.masked_invalid(m0[crop]), origin="lower", cmap="magma",
                                        interpolation="nearest"), f"$M_0$: {R['flux']:.1f} Jy km/s")
panel("06_model", lambda ax: channel(ax, iso, crec, contour=warp, color="#1f77b4"),
      f"tilted ring, {len(R['rings'])} rings, {vel[crec]:.0f} km/s")
panel("07_residual", lambda ax: ax.imshow(np.ma.masked_invalid(np.abs(m1 - vw)[crop]), origin="lower",
                                         cmap="viridis", vmin=0, vmax=60, interpolation="nearest"),
      f"|v$_{{data}}$ − v$_{{model}}$|, median {R['warp_median']:.1f} km/s")
def export(ax):
    ax.axis("off")
    txt = ("masked cube  →  FITS\n$M_0$, $M_1$ maps  →  FITS\nrings  →  JSON\nchannel scan  →  movie\n\n"
           f"replay:\nPOST /v1/cube/kinematic_select_bin\n  seed {SEED}\n  reach {REACH:g}, λv {LAMBDA_V:g}\n"
           f"  smooth {tuple(int(v) for v in SMOOTH)}")
    ax.text(0.5, 0.5, txt, ha="center", va="center", fontsize=7.6, family="monospace",
            bbox=dict(boxstyle="round,pad=0.6", fc="#f4f4f4", ec="#888888"))
panel("08_export", export, "")
print("panels written to", OUT)
