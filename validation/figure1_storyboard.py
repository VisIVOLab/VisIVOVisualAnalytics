#!/usr/bin/env python3
"""Compose Figure 1 of the paper: the workflow storyboard.

The manuscript asks for eight panels "in one row". On the journal's page that
does not survive arithmetic: elsarticle's 5p two-column layout gives a
figure* about 17.5 cm wide, so eight panels in a row are 2.1 cm each — a
screenshot of a 3-D viewer at 2.1 cm is a smudge, and the figure that is
supposed to carry the paper would carry nothing.

So the storyboard is laid out as TWO rows of four, which keeps the left-to-right
reading order and the arrows between stages while giving each panel ~4.2 cm.
Pass --row to get the single row anyway and see for yourself.

Usage:

    python3 validation/figure1_storyboard.py --src figures/fig1 \\
                                             --out figures/figure1_storyboard.png

`--src` holds the eight captures, named so they sort into stage order:

    01_open.png  02_explore.png  03_lasso.png   04_isolated.png
    05_moment0.png  06_model.png  07_residual.png  08_export.png

Any missing file is drawn as a labelled empty slot, so the layout can be
reviewed before all eight exist.
"""
from __future__ import annotations

import argparse
import pathlib
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.image as mpimg
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch

# Stage order, the panel letter, and the words under each panel. The labels are
# what the reader follows; keep them verbs, not feature names.
STAGES = [
    ("01_open",     "a", "open the cube"),
    ("02_explore",  "b", "explore in 3-D"),
    ("03_lasso",    "c", "one double-click"),
    ("04_isolated", "d", "isolated cube"),
    ("05_moment0",  "e", "moment 0"),
    ("06_model",    "f", "tilted-ring model"),
    ("07_residual", "g", "residual"),
    ("08_export",   "h", "export"),
]

# The call that regenerates the derived dataset. mask_apply_bin, NOT
# mask_selection: the claim in the caption is that what you SAW is what is
# reproduced, and mask_selection re-grows from the seeds instead of applying the
# displayed mask.
REST_INSET = (
    "double-click  →  POST /v1/cube/kinematic_select_bin  (seed z,y,x · reach · $\\lambda_v$)\n"
    "apply         →  POST /v1/cube/mask_apply_bin?mode=blank_outside  →  *_lasso-outside.fits"
)


def parse_crop(spec: str | None):
    """`--crop l,t,r,b` as fractions of the image, or None."""
    if not spec:
        return None
    parts = [float(x) for x in spec.split(",")]
    if len(parts) != 4 or not all(0.0 <= v <= 1.0 for v in parts):
        sys.exit("error: --crop wants four fractions 0..1, e.g. 0.18,0.15,0.79,0.96")
    l, t, r, b = parts
    if l >= r or t >= b:
        sys.exit("error: --crop needs left<right and top<bottom")
    return (l, t, r, b)


def load(src: pathlib.Path, stem: str, crop=None, overrides=None):
    """Read a capture, cropping to the viewer pane.

    Capturing the whole window is the repeatable gesture (macOS ⇧⌘4 then Space),
    but a whole window reduced to 4 cm is unreadable — the chrome eats the
    picture. Cropping to the pane afterwards keeps the capture easy and the
    figure legible, and doing it here rather than by hand keeps the eight panels
    consistent. A per-panel override exists because one or two panels (the
    export dialog, a product card) ARE the chrome.
    """
    for ext in (".png", ".jpg", ".jpeg"):
        p = src / f"{stem}{ext}"
        if not p.is_file():
            continue
        img = mpimg.imread(p)
        box = (overrides or {}).get(stem, crop)
        if box:
            h, w = img.shape[0], img.shape[1]
            l, t, r, b = box
            img = img[int(t * h):int(b * h), int(l * w):int(r * w)]
        return img
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="figures/fig1", type=pathlib.Path)
    ap.add_argument("--out", default="figures/figure1_storyboard.png", type=pathlib.Path)
    ap.add_argument("--row", action="store_true",
                    help="single row of eight (shows why two rows are better)")
    ap.add_argument("--dpi", type=int, default=300)
    ap.add_argument("--crop", default="0.18,0.15,0.79,0.96", metavar="L,T,R,B",
                    help="crop every capture to this fraction of the window — the "
                         "default is the viewer pane of a maximised window on a "
                         "16:10 display. Pass --crop '' to disable.")
    ap.add_argument("--crop-panel", action="append", default=[], metavar="STEM=L,T,R,B",
                    help="override the crop for one panel, e.g. "
                         "--crop-panel 08_export=0,0,1,1 to keep the whole window")
    args = ap.parse_args()

    crop = parse_crop(args.crop)
    overrides = {}
    for spec in args.crop_panel:
        stem, _, box = spec.partition("=")
        overrides[stem] = parse_crop(box)

    if not args.src.is_dir():
        sys.exit(f"error: --src {args.src} does not exist; put the eight captures there")

    ncol = 8 if args.row else 4
    nrow = 1 if args.row else 2
    # Figure width = the journal's figure* width, so what you see is what the
    # reader gets. Panel aspect assumes a 16:10 window capture.
    width_in = 17.5 / 2.54
    panel_w = width_in / ncol
    height_in = nrow * panel_w * 0.72 + 0.85   # + room for the REST inset

    fig = plt.figure(figsize=(width_in, height_in))
    gs = fig.add_gridspec(nrow, ncol, wspace=0.06, hspace=0.30,
                          left=0.012, right=0.988, top=0.97, bottom=0.20)

    missing = []
    for i, (stem, letter, label) in enumerate(STAGES):
        ax = fig.add_subplot(gs[i // ncol, i % ncol])
        img = load(args.src, stem, crop, overrides)
        if img is None:
            missing.append(stem)
            ax.set_facecolor("#f0f0f0")
            ax.text(0.5, 0.5, f"{stem}\nmissing", ha="center", va="center",
                    fontsize=5.5, color="#999999", transform=ax.transAxes)
        else:
            ax.imshow(img)
        ax.set_xticks([]); ax.set_yticks([])
        for s in ax.spines.values():
            s.set_linewidth(0.4); s.set_color("#666666")
        ax.set_title(f"({letter}) {label}", fontsize=6.2, pad=2.5)

        # Arrow to the next panel, inside the row only: an arrow that wrapped
        # from the end of a row to the start of the next would point the wrong
        # way across the page.
        if i < len(STAGES) - 1 and (i % ncol) != ncol - 1:
            fig.add_artist(FancyArrowPatch(
                (ax.get_position().x1 + 0.004, ax.get_position().y0 + 0.5 * ax.get_position().height),
                (ax.get_position().x1 + 0.016, ax.get_position().y0 + 0.5 * ax.get_position().height),
                transform=fig.transFigure, arrowstyle="-|>", mutation_scale=7,
                linewidth=0.7, color="#444444"))

    fig.text(0.5, 0.085, REST_INSET, ha="center", va="center", fontsize=6.0,
             family="monospace", linespacing=1.6,
             bbox=dict(boxstyle="round,pad=0.45", facecolor="#f7f7f7",
                       edgecolor="#bbbbbb", linewidth=0.5))
    fig.text(0.5, 0.018,
             "every arrow is a state transition; every panel a persistent product",
             ha="center", fontsize=5.8, style="italic", color="#555555")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=args.dpi, bbox_inches="tight", facecolor="white")
    print(f"Wrote {args.out}  ({ncol}x{nrow})")
    if missing:
        print("still missing:", ", ".join(missing))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
