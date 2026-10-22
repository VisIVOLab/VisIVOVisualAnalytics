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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from datasets import parse_floats  # noqa: E402

import matplotlib
matplotlib.use("Agg")
import matplotlib.image as mpimg
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch

# Stage order, the panel letter, and the words under each panel. The labels are
# what the reader follows; keep them verbs, not feature names.
STAGES = [
    ("01_open",     "a", "open the cube"),
    ("02_explore",  "b", "explore the channels"),
    ("03_lasso",    "c", "one double-click"),
    ("04_isolated", "d", "isolated cube"),
    ("05_moment0",  "e", "moment 0"),
    ("06_model",    "f", "tilted-ring model"),
    ("07_residual", "g", "residual"),
    ("08_export",   "h", "export"),
]



def parse_crop(spec: str | None):
    """`--crop l,t,r,b` as fractions of the image, or None."""
    if not spec:
        return None
    try:
        l, t, r, b = parse_floats(spec, 4, "--crop", "0.18,0.15,0.79,0.96", lo=0.0, hi=1.0)
    except argparse.ArgumentTypeError as exc:
        sys.exit(f"error: {exc}")
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
    # reader gets. The slot aspect follows the first panel there is (a 16:10
    # window capture or a portrait render of the disc), so a portrait panel is
    # not squeezed into a landscape slot with white at both sides.
    width_in = 17.5 / 2.54
    aspect = 0.72
    for stem, _, _ in STAGES:
        first = load(args.src, stem, crop, overrides)
        if first is not None:
            aspect = first.shape[0] / first.shape[1]
            break
    # The slot width is what is left after the margins and the gaps between
    # columns (matplotlib's wspace is a fraction of one slot), and the figure
    # height follows from it: a panel drawn at the slot size then prints at
    # the slot size, with its type at the point size it was set in.
    # Nothing sits under the panels: the call that regenerates the selection
    # is on panel (h), and the caption says what the arrows are.
    left, right, top, bottom, wspace, hspace = 0.012, 0.988, 0.97, 0.03, 0.06, 0.22
    slot_w = width_in * (right - left) / (ncol + (ncol - 1) * wspace)
    slot_h = slot_w * aspect
    height_in = (nrow * slot_h + (nrow - 1) * hspace * slot_h) / (top - bottom)

    fig = plt.figure(figsize=(width_in, height_in))
    gs = fig.add_gridspec(nrow, ncol, wspace=wspace, hspace=hspace,
                          left=left, right=right, top=top, bottom=bottom)

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
        ax.set_title(f"({letter}) {label}", fontsize=8.5, pad=2.5)

        # Arrow to the next panel, inside the row only: an arrow that wrapped
        # from the end of a row to the start of the next would point the wrong
        # way across the page.
        # (a)→(b) is navigation — a change of view that persists nothing — so it
        # is dashed, unlike the transitions that leave a product behind.
        if i < len(STAGES) - 1 and (i % ncol) != ncol - 1:
            navigation = i == 0
            fig.add_artist(FancyArrowPatch(
                (ax.get_position().x1 + 0.004, ax.get_position().y0 + 0.5 * ax.get_position().height),
                (ax.get_position().x1 + 0.016, ax.get_position().y0 + 0.5 * ax.get_position().height),
                transform=fig.transFigure, arrowstyle="-|>", mutation_scale=7,
                linewidth=0.7, color="#999999" if navigation else "#444444",
                linestyle=(0, (2.0, 1.5)) if navigation else "solid"))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=args.dpi, bbox_inches="tight", facecolor="white")
    print(f"Wrote {args.out}  ({ncol}x{nrow})")
    if missing:
        print("still missing:", ", ".join(missing))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
