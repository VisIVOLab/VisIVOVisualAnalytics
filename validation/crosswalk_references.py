#!/usr/bin/env python
"""Which published component holds each SoFiA-2 source — computed, not asserted.

The paper's Table 2 pairs rows A-E (published-mask components of Serra et al.
2013) with our SoFiA-2 sources. That pairing is the one thing a change in the
labelling order, or in the SoFiA-2 run, would silently move, so it is produced
here from the two labelled masks: for every SoFiA-2 source, the published
component with the largest overlap and the fraction of the source inside it.

Run:  backend/.venv/bin/python validation/crosswalk_references.py
      (reads sofia_out/sofia_hgc44_mask.fits and sofia_out/serra2013_hcg44_mask.fits,
       writes out/crosswalk_serra2013_sofia2.md)
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

HERE = Path(__file__).resolve().parent
SOFIA = HERE / "sofia_out" / "sofia_hgc44_mask.fits"
PUBLISHED = HERE / "sofia_out" / "serra2013_hcg44_mask.fits"
OUT = HERE / "out" / "crosswalk_serra2013_sofia2.md"
TOP = 5   # the sources in the paper's table, largest first

for p in (SOFIA, PUBLISHED):
    if not p.exists():
        sys.exit(f"missing {p} (see README: SoFiA-2 run / label_published_mask.py)")
sofia = np.squeeze(fits.getdata(SOFIA)).astype(np.int32)
pub = np.squeeze(fits.getdata(PUBLISHED)).astype(np.int32)
if sofia.shape != pub.shape:
    sys.exit(f"mask shapes differ: {sofia.shape} vs {pub.shape}")

sys.path.insert(0, str(HERE))
from datasets import rank_sources  # noqa: E402

# rows A-E are the five largest published components, as in the paper's table;
# the SoFiA-2 ranking is the one lasso_vs_sofia.py uses for its own rows
p_labels, p_counts, p_order = rank_sources(pub, TOP)
s_labels, s_counts, s_order = rank_sources(sofia)
s_rank = {int(s_labels[i]): r for r, i in enumerate(s_order, 1)}
rows = []
for letter, i in zip("ABCDE", p_order):
    comp = int(p_labels[i])
    inside = sofia[pub == comp]
    hits = np.bincount(inside[inside > 0], minlength=int(s_labels.max()) + 1)
    sid = int(np.argmax(hits)) if hits.any() else 0
    n_src = int(s_counts[s_labels == sid][0]) if sid else 0
    rows.append((letter, comp, int(p_counts[i]), sid, s_rank.get(sid, 0), n_src,
                 float(hits[sid]) / n_src if sid else 0.0))

md = ["# Crosswalk: published (Serra et al. 2013) components → SoFiA-2 sources — HCG 44",
      "", "Rows A-E are the five largest published components. For each, the SoFiA-2 source",
      "with most voxels inside it, that source's size rank among all SoFiA-2 sources, and",
      "the fraction of the source inside the component. Table 2 of the paper pairs each",
      "row with the SoFiA-2 source of the same rank, which holds only while the rank",
      "column reads 1-5.", "",
      "| row | published component | component vox | SoFiA-2 source | SoFiA-2 size rank | source vox | fraction inside |",
      "|---|---|---|---|---|---|---|"]
for letter, comp, n_comp, sid, rank, n_src, frac in rows:
    md.append(f"| {letter} | {comp} | {n_comp:,} | {sid} | {rank} | {n_src:,} | {frac:.3f} |")
if any(rank > TOP or rank == 0 for *_, rank, _, _ in [(r[0], r[1], r[2], r[3], r[4], r[5], r[6]) for r in rows]):
    md.append("\n**WARNING**: a row's SoFiA-2 source is not among the five largest; the lasso table's rows and these rows would not describe the same objects.")
OUT.parent.mkdir(exist_ok=True)
OUT.write_text("\n".join(md) + "\n")
print("\n".join(md[7:]))
print("written", OUT)
