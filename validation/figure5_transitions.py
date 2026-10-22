#!/usr/bin/env python
"""Figure 5 of the paper: the HI session as a chain of persistent objects.

Seven nodes, each a thumbnail of the actual product of the NGC 3137 session
(``usecase_ngc3137.run()``) over a product card with its real parameters and
the API endpoint that performs, and can replay, the transition. The endpoint
paths are read from the service's router at build time, so the figure cannot
drift from the API. Laid out as one chain folded in two rows (left to right,
then right to left) at the journal's two-column text width, so that the card
text prints at 6.4 pt instead of the 3.5 pt a single 14-inch row would give.

Run:  backend/.venv/bin/python validation/figure5_transitions.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from usecase_ngc3137 import run, SEED, SMOOTH, REACH, LAMBDA_V  # noqa: E402

ROUTERS = HERE.parent / "backend" / "app" / "routers"
def endpoint(pattern: str) -> str:
    """First route in the service whose path matches `pattern`."""
    for f in sorted(ROUTERS.glob("*.py")):
        for m in re.finditer(r'@router\.(post|get)\("(/v1/[^"]+)"', f.read_text()):
            if re.search(pattern, m.group(2)):
                return f"{m.group(1).upper()} {m.group(2)}"
    return "?"

R = run(verbose=False)
vel, data, iso, m0, m1 = R["vel"], R["data"], R["isolated"], R["m0"], R["m1"]
mask, warp, vw = R["mask"], R["model_warp"], R["vmod_warp"]
csys = int(np.argmin(np.abs(vel - R["vsys_fit"])))
crec = int(np.argmin(np.abs(vel - (R["vsys_fit"] + 0.6 * R["vrot_sini"]))))
ys, xs = np.where(np.isfinite(m1))
crop = (slice(max(0, ys.min() - 10), ys.max() + 11), slice(max(0, xs.min() - 10), xs.max() + 11))
vmax = np.nanpercentile(data[:, crop[0], crop[1]], 99.7)
nz, ny, nx = data.shape

def show_channel(ax, cube, c, contour=None, color="#d62728", cmap="Greys"):
    ax.imshow(np.ma.masked_invalid(cube[c][crop]), origin="lower", cmap=cmap, vmin=0, vmax=vmax,
              interpolation="nearest")
    if contour is not None:
        ax.contour(contour[c][crop].astype(float), levels=[0.5], colors=[color], linewidths=1.0)

def ep(pattern: str) -> str:
    """The endpoint on two lines, split at the last slash, so it fits a card."""
    route = endpoint(pattern)
    head, _, name = route.rpartition("/")
    return f"{head}/\n{name}"

resid_map = m1 - vw          # signed, as the viewer's "Residual v − v_model (rings)"
cards = [   # every line at most 25 monospace characters: that is what a card holds at 6.4 pt
    ("Cube", lambda ax: show_channel(ax, data, crec),
     f"WALLABY J100903-290239\n{nx}×{ny}×{nz} voxels\n{ep(r'datasets/open$')}"),
    ("Mask", lambda ax: show_channel(ax, data, crec, mask),
     f"seed {SEED}\nλv={LAMBDA_V:g}, reach={REACH:g}\nsmoothed {tuple(int(v) for v in SMOOTH)}\n→ {R['n_sel']:,} vox\n(default click: {R['n_default']:,})\n{ep(r'kinematic_select_bin')}"),
    ("Isolated cube", lambda ax: show_channel(ax, iso, crec),
     f"NaN outside the mask,\nas displayed; WCS and\ndimensions kept\n{ep(r'mask_apply_bin')}"),
    ("Moments", lambda ax: ax.imshow(np.ma.masked_invalid(m0[crop]), origin="lower", cmap="magma", interpolation="nearest"),
     f"M0: {R['flux']:.1f} Jy km/s\nM1 over {R['n_velocity_field']} px > 3σ\n{ep(r'kinematic_fit_bin')}"),
    ("Model", lambda ax: show_channel(ax, iso, crec, warp, color="#1f77b4"),
     f"PA {R['pa_kin']:.0f}°, i {R['incl']:.0f}°, V_rot {R['vrot']:.0f}\n{len(R['rings'])} rings, fitted\n{ep(r'kinematic_model_bin')}"),
    ("Residual", lambda ax: ax.imshow(np.ma.masked_invalid(resid_map[crop]), origin="lower", cmap="RdBu_r",
                                      vmin=-60, vmax=60, interpolation="nearest"),
     f"v_data − v_model\n|median| {R['warp_median']:.1f} km/s\n(flat {np.median(R['resid_flat']):.1f})\n{ep(r'kinematic_residual_bin')}"),
    ("Export", lambda ax: show_channel(ax, iso, csys, warp, color="#1f77b4"),
     f"channel scan,\nmodel vs data\nmovie + FITS products\n{ep(r'exports')}"),
]
assert all(len(line) <= 25 for _, _, t in cards for line in t.split("\n")), "a card line is too long for the column"

# one chain folded in two: cards 0-3 left to right on the top row, 4-6 right to
# left on the bottom row, so every arrow joins neighbours and the fold is one
# short downward arrow under "Moments"
NCOL = 4
cells = [(0, 0), (0, 1), (0, 2), (0, 3), (2, 3), (2, 2), (2, 1)]
fig = plt.figure(figsize=(6.89, 5.9))
# The text rows hold up to six card lines (the mask card names both the
# smoothed and the default selection), so they get more than half an image.
gs = fig.add_gridspec(4, NCOL, height_ratios=[1.0, 0.62, 1.0, 0.62], hspace=0.06, wspace=0.06,
                      left=0.01, right=0.99, top=0.955, bottom=0.01)
img_axes, txt_axes = [], []
for (r, c), (title, draw, text) in zip(cells, cards):
    ax = fig.add_subplot(gs[r, c]); draw(ax)
    ax.set_title(title, fontsize=9, fontweight="bold", pad=3); ax.set_xticks([]); ax.set_yticks([])
    tx = fig.add_subplot(gs[r + 1, c]); tx.axis("off")
    tx.text(0.5, 1.0, text, ha="center", va="top", fontsize=6.4, family="monospace", linespacing=1.25,
            bbox=dict(boxstyle="round,pad=0.4", fc="#f4f4f4", ec="#999999", lw=0.6))
    img_axes.append(ax); txt_axes.append(tx)
lg = fig.add_subplot(gs[2:4, 0]); lg.axis("off")
lg.text(0.5, 0.55, "each card is a product the service\nkeeps, with its provenance;\n"
        "the endpoint under it performs\nthe transition and replays it",
        ha="center", va="center", fontsize=7.2, style="italic", color="#333333")

def join(a: int, b: int, down: bool = False) -> None:
    """Arrow from card a to card b, in figure coordinates."""
    pa, pb = img_axes[a].get_position(), img_axes[b].get_position()
    if down:   # from the bottom of a's text card to just above b's title
        x = 0.5 * (pa.x0 + pa.x1)
        p, q = (x, txt_axes[a].get_position().y0 + 0.03), (x, pb.y1 + 0.04)
    else:
        y = 0.5 * (pa.y0 + pa.y1)
        if pb.x0 > pa.x0:
            p, q = (pa.x1 + 0.004, y), (pb.x0 - 0.004, y)
        else:
            p, q = (pa.x0 - 0.004, y), (pb.x1 + 0.004, y)
    fig.patches.append(FancyArrowPatch(p, q, transform=fig.transFigure, arrowstyle="-|>",
                                       mutation_scale=10, color="#444444", lw=1.0))

for k in range(len(cards) - 1):
    join(k, k + 1, down=(k == 3))
out = HERE / "out" / "figure5_transitions.png"
fig.savefig(out, dpi=220); print("written", out)
