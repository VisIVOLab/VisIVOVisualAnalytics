#!/usr/bin/env python
"""Figure 2 of the paper: the hybrid desktop--service architecture.

A diagram, drawn rather than screenshotted, of the two processes, the boundary
between them, the three places the service can run, and the two stores the
loop touches. Sized for the two-column (17.5 cm) text width of the journal:
the canvas is 100 x 54 units on a 10 x 5.4 inch figure, printed at 0.69 of
its drawn size, so every font is set 1.25x larger than it should read (FS)
and no label ends up under 6 pt on the page. A DejaVu Sans character is
about 0.63 em wide, which is what the line lengths below are budgeted on.

Run:  backend/.venv/bin/python validation/figure2_architecture.py
"""
from __future__ import annotations

from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

HERE = Path(__file__).resolve().parent
FS = 1.25   # font scale: drawn at 10 in, printed at 6.9 in
fig, ax = plt.subplots(figsize=(10.0, 5.4))
ax.set_xlim(0, 100); ax.set_ylim(0, 54); ax.axis("off")

def box(x, y, w, h, title, lines, fc, tfs=9.0, lfs=7.2):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.3,rounding_size=1.0",
                                fc=fc, ec="#333333", lw=1.0))
    ax.text(x + w / 2, y + h - 1.6, title, ha="center", va="top", fontsize=tfs * FS, fontweight="bold")
    for i, l in enumerate(lines):
        ax.text(x + w / 2, y + h - 6.2 - 3.6 * i, l, ha="center", va="top", fontsize=lfs * FS)

def arrow(p, q, style="<|-|>", color="#222222", lw=1.2):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle=style, mutation_scale=10, color=color, lw=lw))

def note(x, y, text, **kw):
    ax.text(x, y, text, ha=kw.pop("ha", "center"), va=kw.pop("va", "top"),
            fontsize=kw.pop("fontsize", 7.0) * FS, color=kw.pop("color", "#444444"), **kw)

# ── the two processes ────────────────────────────────────────────────────
box(1, 31, 32, 21, "Desktop client",
    ["C++17 · Qt 6 · VTK 9", "interaction and GPU volume rendering",
     "3D picking, linked views", "optional OpenXR hand-off"], fc="#e8f1fb")
box(67, 31, 32, 21, "Scientific compute service",
    ["Python · FastAPI · astropy / numpy", "memory-mapped FITS, worker pool",
     "out-of-core kernels, optional GPU", "sessions, provenance, diagnostics"], fc="#fdeeea")
arrow((33.8, 44.5), (66.2, 44.5))
note(50, 46.0, "authenticated, versioned REST API", va="bottom", fontsize=7.6, color="#222222")
note(50, 42.8, "token and session headers")
note(50, 39.6, "on every request")
note(50, 36.4, "WebSocket / WebRTC streams")
note(50, 33.2, "for server-side rendering")

# ── where the service runs ───────────────────────────────────────────────
# box widths follow their longest line (~0.85 unit per character at this size)
for x, w, title, sub in [(54.0, 12.0, "embedded", ["same laptop,", "launched by", "the client"]),
                         (67.0, 15.5, "remote server", ["SSH tunnel or", "reverse proxy"]),
                         (83.5, 15.0, "HPC node", ["Apptainer image", "SLURM scripts"])]:
    box(x, 8, w, 15, title, sub, fc="#fff6e0", tfs=7.4, lfs=7.0)
    xm = x + w / 2
    arrow((xm, 23.4), (max(xm, 70.0), 30.6), style="-|>", color="#666666", lw=1.0)   # lands on the service box
note(76.25, 5.4, "one client, one experience, three places to run", fontsize=7.0, style="italic", color="#222222")

# ── the stores: the workspace is server-side; the archives are reached by the
#    client (OIDC login, sky queries, SAMP) and by the service (cutouts, SAMP bridge)
box(0.5, 8, 25.5, 15, "Workspace", ["persistent products with", "provenance: masks, cubes,",
                                    "maps, models, measurements"], fc="#eef7ea", tfs=8.4, lfs=7.0)
box(27, 8, 26, 15, "Archives and the VO", ["VLKB: OIDC, SODA cutouts",
                                           "SAMP: TOPCAT, Aladin, DS9",
                                           "HiPS, VOTable, cone search"], fc="#eef7ea", tfs=8.4, lfs=7.0)
arrow((13.25, 23.4), (13.25, 30.6), color="#666666", lw=1.0)
arrow((40, 23.4), (31, 30.6), color="#666666", lw=1.0)   # lands on the client box
note(14.5, 27.0, "read, re-open,\nexport", ha="left", va="center")
note(39.4, 26.6, "query, fetch,\nbroadcast", ha="left", va="center")
note(26.75, 5.4, "workspace held by the service; archives reached by both", fontsize=7.0, style="italic", color="#222222")

out = HERE / "out" / "figure2_architecture"
fig.savefig(out.with_suffix(".pdf"), bbox_inches="tight")
fig.savefig(out.with_suffix(".png"), dpi=220, bbox_inches="tight")
print("written", out.with_suffix(".pdf"))
