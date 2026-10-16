#!/usr/bin/env python
"""Figure 4 of the paper: tilted-ring model against the data on NGC 3137.

Top row: three channel maps of the isolated cube (approaching, systemic,
receding), with the half-maximum contour of the flat model and of the
warped eight-ring model. Bottom row: the measured velocity field and the two
model velocity fields on one colour scale. Every array comes from
``usecase_ngc3137.run()``, so the figure and the numbers in Section 6.1 are
the same products.

Run:  backend/.venv/bin/python validation/figure4_model_ngc3137.py
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
from usecase_ngc3137 import run  # noqa: E402

R = run(verbose=False)
vel, iso, m1 = R["vel"], R["isolated"], R["m1"]
vsys, vsini = R["vsys_fit"], R["vrot_sini"]
flat, warp = R["model_flat"], R["model_warp"]
vf, vw = R["vmod_flat"], R["vmod_warp"]

def ch(v):            # channel nearest a velocity (the axis is descending)
    return int(np.argmin(np.abs(vel - v)))

# approaching = blueshifted = below vsys; receding = above. The velocity axis of
# this cube runs downward with channel index, which is how the labels got
# swapped once: they are tied to the velocity here, not to the channel.
chans = [ch(vsys - 0.7 * vsini), ch(vsys), ch(vsys + 0.7 * vsini)]
labels = ["approaching", "systemic", "receding"]
# crop to the disc, with margin
ys, xs = np.where(np.isfinite(m1))
y0, y1 = max(0, ys.min() - 12), min(m1.shape[0], ys.max() + 13)
x0, x1 = max(0, xs.min() - 12), min(m1.shape[1], xs.max() + 13)
crop = (slice(y0, y1), slice(x0, x1))
vmax = np.nanpercentile(iso[:, y0:y1, x0:x1], 99.5)

# drawn at the journal's two-column text width (17.5 cm) so the 7-9 pt labels print at size
fig, axes = plt.subplots(2, 3, figsize=(6.89, 4.73), constrained_layout=True)
for ax, c, lab in zip(axes[0], chans, labels):
    img = np.ma.masked_invalid(iso[c][crop])
    ax.imshow(img, origin="lower", cmap="Greys", vmin=0, vmax=vmax, interpolation="nearest")
    ax.contour(flat[c][crop], levels=[0.5], colors=["#1f77b4"], linewidths=1.2)
    ax.contour(warp[c][crop], levels=[0.5], colors=["#d62728"], linewidths=1.2, linestyles="--")
    ax.set_title(f"{lab}: {vel[c]:.0f} km s$^{{-1}}$", fontsize=9)
    ax.set_xticks([]); ax.set_yticks([])
axes[0, 0].plot([], [], color="#1f77b4", label="flat disc")
axes[0, 0].plot([], [], color="#d62728", ls="--", label=f"warped, {len(R['rings'])} rings")
axes[0, 0].legend(loc="lower left", fontsize=7, frameon=True)

vmin_v, vmax_v = vsys - 1.1 * vsini, vsys + 1.1 * vsini
for ax, fld, lab in zip(axes[1], [m1, vf, vw],
                        ["data: moment 1", "flat model", "warped model"]):
    im = ax.imshow(np.ma.masked_invalid(fld[crop]), origin="lower", cmap="RdBu_r",
                   vmin=vmin_v, vmax=vmax_v, interpolation="nearest")
    ax.set_title(lab, fontsize=9); ax.set_xticks([]); ax.set_yticks([])
cb = fig.colorbar(im, ax=axes[1], shrink=0.85, pad=0.01)
cb.set_label("$v_\\mathrm{los}$ (km s$^{-1}$)", fontsize=8); cb.ax.tick_params(labelsize=7)
fig.supxlabel(f"median |v$_\\mathrm{{data}}$ − v$_\\mathrm{{model}}$|: flat {np.median(R['resid_flat']):.1f}, "
              f"warped {np.median(R['resid_warp']):.1f} km s$^{{-1}}$ (one channel is {R['dv']:.1f})", fontsize=8)
out = HERE / "out" / "figure4_model.png"
fig.savefig(out, dpi=220)
print("written", out, "channels", chans)
