#!/usr/bin/env python
"""Compose the interface figure of the paper from window captures of the client.

The other figures are rendered from the products; this one has to show the
tool itself, so it is made of genuine window captures (``screencapture -l``
on the main window, nothing retouched) stacked at the journal's 17.5 cm text
width. Each panel is ``path[:x0,y0,x1,y1][:label]``: an optional crop in
window pixels and an optional label printed above the panel.

Run:  backend/.venv/bin/python validation/figure_interface.py \\
          shots/a.png:"(a) the cube viewer: 3D volume and 2D channel, linked" \\
          shots/b.png:0,120,1600,760:"(b) one double-click: the lasso on the rendering" \\
          --out validation/out/figure_interface.png
"""
from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.image as mpimg
import matplotlib.pyplot as plt

WIDTH_IN = 17.5 / 2.54


def parse_panel(spec: str):
    parts = spec.split(":")
    path, crop, label = parts[0], None, ""
    for p in parts[1:]:
        if p and p[0].isdigit() and p.count(",") == 3:
            crop = tuple(int(v) for v in p.split(","))
        else:
            label = p
    img = mpimg.imread(path)
    if crop:
        x0, y0, x1, y1 = crop
        img = img[y0:y1, x0:x1]
    return img, label


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("panels", nargs="+", help="path[:x0,y0,x1,y1][:label]")
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parent / "out" / "figure_interface.png")
    ap.add_argument("--dpi", type=int, default=None,
                    help="default: the first panel's native pixels across the 17.5 cm, so nothing is resampled")
    a = ap.parse_args()
    panels = [parse_panel(p) for p in a.panels]
    label_in = 0.22
    heights = [WIDTH_IN * img.shape[0] / img.shape[1] for img, _ in panels]
    fig = plt.figure(figsize=(WIDTH_IN, sum(heights) + label_in * len(panels)))
    y = 1.0
    total = sum(heights) + label_in * len(panels)
    for (img, label), h in zip(panels, heights):
        y -= label_in / total
        if label:
            fig.text(0.0, y + 0.3 * label_in / total, label, fontsize=8, va="bottom", ha="left")
        y -= h / total
        ax = fig.add_axes([0.0, y, 1.0, h / total])
        ax.imshow(img, interpolation="lanczos")
        ax.set_axis_off()
    a.out.parent.mkdir(exist_ok=True)
    dpi = a.dpi or round(panels[0][0].shape[1] / WIDTH_IN)
    fig.savefig(a.out, dpi=dpi, facecolor="white")
    print("written", a.out, "panels:", len(panels))


if __name__ == "__main__":
    main()
