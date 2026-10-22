#!/usr/bin/env python
"""Compose the interface figure of the paper from window captures of the client.

The other figures are rendered from the products; this one has to show the
tool itself, so it is made of genuine window captures (``screencapture -l``
on the main window, nothing retouched) stacked at the journal's 17.5 cm text
width. Each panel is ``path[:x0,y0,x1,y1][:label]``: an optional crop in
window pixels and an optional label printed above the panel. A panel whose
spec starts with ``+`` sits in the same row as the previous one, at the
height that makes the row span the text width: the way to put a zoomed
crop of a control panel beside, or under, the whole window it comes from.

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
    """`path[:x0,y0,x1,y1][:label]` — the label is everything after the path
    (and the optional crop), colons included."""
    path, _, rest = spec.partition(":")
    crop, label = None, rest
    head, sep, tail = rest.partition(":")
    parts = head.split(",")
    if len(parts) == 4 and all(p.strip().lstrip("-").isdigit() for p in parts):
        crop, label = tuple(int(v) for v in parts), tail if sep else ""
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
    # Rows: a leading "+" joins the previous row.
    rows: list[list[tuple]] = []
    for spec in a.panels:
        joined = spec.startswith("+")
        panel = parse_panel(spec[1:] if joined else spec)
        if joined and rows:
            rows[-1].append(panel)
        else:
            rows.append([panel])
    label_in, gap_in = 0.22, 0.08
    # A row's height is set by its panels' aspects: widths w_i = h * (px_w/px_h)_i
    # must sum to the text width, less the gaps between them.
    row_h = []
    for row in rows:
        inv = sum(img.shape[1] / img.shape[0] for img, _ in row)
        row_h.append((WIDTH_IN - gap_in * (len(row) - 1)) / inv)
    total = sum(row_h) + label_in * len(rows)
    fig = plt.figure(figsize=(WIDTH_IN, total))
    y = 1.0
    for row, h in zip(rows, row_h):
        y -= label_in / total
        labels = [lab for _, lab in row if lab]
        if labels:
            fig.text(0.0, y + 0.3 * label_in / total, "   ".join(labels), fontsize=8,
                     va="bottom", ha="left")
        y -= h / total
        x = 0.0
        for img, _ in row:
            w = h * img.shape[1] / img.shape[0]
            ax = fig.add_axes([x / WIDTH_IN, y, w / WIDTH_IN, h / total])
            ax.imshow(img, interpolation="lanczos")
            ax.set_axis_off()
            x += w + gap_in
    a.out.parent.mkdir(exist_ok=True)
    first = rows[0][0][0]
    dpi = a.dpi or first.shape[1] / (row_h[0] * first.shape[1] / first.shape[0])   # the first panel keeps its pixels
    fig.savefig(a.out, dpi=dpi, facecolor="white")
    print("written", a.out, "rows:", [len(r) for r in rows])


if __name__ == "__main__":
    main()
