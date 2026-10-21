#!/usr/bin/env python
"""Figure 4 of the paper: tilted-ring model against the data on NGC 3137.

Top row: three channel maps of the isolated cube (approaching, systemic,
receding), with the half-maximum contour of the rings model — the 2-D contour
the overlay draws on the channel map. Middle row: the velocity field and the
two model velocity fields the kinematic fit adds to Session Data, on one colour
scale. Bottom row: the two residual maps v_data − v_model the comparator reports
(the "Residual v − v_model" products), on one signed scale, with the
distribution of |residual| beside them. Every array comes from
``usecase_ngc3137.run()``, which calls the application's own fit and model, so
the figure and the numbers in Section 6.1 are the same products.

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
warp = R["model_warp"]
warp_peak = float(np.nanmax(warp))   # the panel's level is a fraction of the model's peak
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
fig, axes = plt.subplots(3, 3, figsize=(6.89, 7.0), constrained_layout=True)
for ax, c, lab in zip(axes[0], chans, labels):
    img = np.ma.masked_invalid(iso[c][crop])
    ax.imshow(img, origin="lower", cmap="Greys", vmin=0, vmax=vmax, interpolation="nearest")
    # The overlay's 2-D contour on the channel map: one model at a time, at the
    # panel's default level (50 % of peak) — the rings model the dialog's fit gives.
    ax.contour(warp[c][crop], levels=[0.5 * warp_peak], colors=["#d62728"], linewidths=1.2)
    ax.set_title(f"{lab}: {vel[c]:.0f} km s$^{{-1}}$", fontsize=9)
    ax.set_xticks([]); ax.set_yticks([])
axes[0, 0].plot([], [], color="#d62728", label=f"model, {len(R['rings'])} rings")
axes[0, 0].legend(loc="lower left", fontsize=7, frameon=True)

vmin_v, vmax_v = vsys - 1.1 * vsini, vsys + 1.1 * vsini
for ax, fld, lab in zip(axes[1], [m1, vf, vw],
                        ["data: velocity field", "flat disc", f"{len(R['rings'])} rings"]):
    im = ax.imshow(np.ma.masked_invalid(fld[crop]), origin="lower", cmap="RdBu_r",
                   vmin=vmin_v, vmax=vmax_v, interpolation="nearest")
    ax.set_title(lab, fontsize=9); ax.set_xticks([]); ax.set_yticks([])
cb = fig.colorbar(im, ax=axes[1], shrink=0.85, pad=0.01)
cb.set_label("$v_\\mathrm{los}$ (km s$^{-1}$)", fontsize=8); cb.ax.tick_params(labelsize=7)

# Residual maps, signed, as the viewer's "Residual v − v_model" products show
# them (the same pixels the median is taken over: where the velocity field and
# the model are both defined), on one symmetric scale for the two models.
res_flat, res_warp = m1 - vf, m1 - vw
lim = 60.0
for ax, fld, lab, med in zip(axes[2, 1:], [res_flat, res_warp],
                             ["data − flat disc", f"data − {len(R['rings'])} rings"],
                             [np.median(R["resid_flat"]), np.median(R["resid_warp"])]):
    im2 = ax.imshow(np.ma.masked_invalid(fld[crop]), origin="lower", cmap="RdBu_r",
                    vmin=-lim, vmax=lim, interpolation="nearest")
    ax.set_title(f"{lab}\nmedian |Δv| {med:.1f} km s$^{{-1}}$", fontsize=8.5)
    ax.set_xticks([]); ax.set_yticks([])
cb2 = fig.colorbar(im2, ax=axes[2, 1:], shrink=0.85, pad=0.01)
cb2.set_label("$v_\\mathrm{data} - v_\\mathrm{model}$ (km s$^{-1}$)", fontsize=8)
cb2.ax.tick_params(labelsize=7)
# The same residuals as a distribution: how much of the disc each model leaves
# within a channel, and where the tails are.
axh = axes[2, 0]
# Bins cover both samples to their maximum, so the tail is drawn whole.
top = 10.0 * np.ceil(max(np.max(R["resid_flat"]), np.max(R["resid_warp"])) / 10.0)
bins = np.linspace(0, top, int(top / 2.5) + 1)
axh.hist(R["resid_flat"], bins=bins, histtype="step", color="#7f7f7f", lw=1.2,
         label=f"flat disc, p90 {np.percentile(R['resid_flat'], 90):.0f}")
axh.hist(R["resid_warp"], bins=bins, histtype="step", color="#d62728", lw=1.2,
         label=f"{len(R['rings'])} rings, p90 {np.percentile(R['resid_warp'], 90):.0f}")
axh.axvline(R["dv"], color="k", ls=":", lw=0.8, label=f"one channel ({R['dv']:.1f})")
axh.set_xlabel("|v$_\\mathrm{data}$ − v$_\\mathrm{model}$| (km s$^{-1}$)", fontsize=8)
axh.set_ylabel("pixels", fontsize=8); axh.tick_params(labelsize=7)
axh.legend(fontsize=7, frameon=False)
out = HERE / "out" / "figure4_model.png"
fig.savefig(out, dpi=220)
print("written", out, "channels", chans)
