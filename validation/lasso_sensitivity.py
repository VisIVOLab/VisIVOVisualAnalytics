#!/usr/bin/env python
"""Kinematic Lasso — interactive-parameter sensitivity (nσ, reach).

Reports the completeness–RELIABILITY tradeoff of the lasso's two interactive
controls, per source. Lowering nσ (or raising reach) grows the selection toward
the SoFiA mask and raises completeness (∩/SoFiA) — but it also floods into
noise, which LOWERS reliability (precision, ∩/lasso). Both curves are shown at
every nσ and reach so the tradeoff is explicit, not just the metric that
improves (R-18): a completeness gain bought with a reliability collapse is not a
real recovery. The conservative default (nσ = 2.5) is where the two balance.

Run:  backend/.venv/bin/python validation/lasso_sensitivity.py
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "backend"))
from app.compute.selection import cube_kinematic_select  # noqa: E402

import os  # noqa: E402

# Out-of-repo HI cube; point at it via the env var so the sweep is reproducible anywhere.
CUBE = os.environ.get(
    "VISIVO_VALIDATION_CUBE", "/Users/fvitello/downloads/DemoData/hgc44_3axes.fits")
MASK = str(Path(__file__).resolve().parent / "sofia_out" / "sofia_hgc44_mask.fits")
OUT = Path(__file__).resolve().parent / "out"
NSIGMAS = [1.5, 2.0, 2.5, 3.0, 3.5]
REACHES = [4.0, 6.0, 8.0, 12.0, 16.0]


def metrics(lasso, src):
    inter = int(np.count_nonzero(lasso & src))
    a, b = int(lasso.sum()), int(src.sum())
    dice = 2 * inter / (a + b) if a + b else 0.0
    completeness = inter / b if b else 0.0          # ∩ / SoFiA  (recall)
    reliability = inter / a if a else 0.0           # ∩ / lasso  (precision)
    return dice, completeness, reliability


def main():
    for label, path in (("cube (set $VISIVO_VALIDATION_CUBE)", CUBE), ("SoFiA mask", MASK)):
        if not Path(path).is_file():
            sys.exit(f"error: {label} not found: {path}")
    OUT.mkdir(exist_ok=True)
    data = np.squeeze(fits.getdata(CUBE)).astype(np.float32)
    sofia = np.squeeze(fits.getdata(MASK)).astype(np.int32)
    labels, counts = np.unique(sofia[sofia > 0], return_counts=True)
    order = np.argsort(counts)[::-1]
    bright, faint = int(labels[order[0]]), int(labels[order[3]])   # #1 and #4 by size

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(1, 2, figsize=(11, 4.4))

    for lab, color in ((bright, "tab:blue"), (faint, "tab:red")):
        src = sofia == lab
        zz, yy, xx = np.where(src)
        k = int(np.argmax(data[zz, yy, xx]))
        seed = (int(zz[k]), int(yy[k]), int(xx[k]))
        tag = f"#{lab} ({'bright' if lab == bright else 'faint'})"

        comp_ns, rel_ns = [], []
        for ns in NSIGMAS:
            m = cube_kinematic_select(data, seed, reach=8.0, lambda_v=1.0, nsigma=ns)
            _, c, r = metrics(m, src)
            comp_ns.append(c); rel_ns.append(r)
        comp_re, rel_re = [], []
        for re in REACHES:
            m = cube_kinematic_select(data, seed, reach=re, lambda_v=1.0, nsigma=2.5)
            _, c, r = metrics(m, src)
            comp_re.append(c); rel_re.append(r)
        print(tag,
              "| nσ: completeness", [f"{c:.2f}" for c in comp_ns],
              "reliability", [f"{r:.2f}" for r in rel_ns],
              "| reach: completeness", [f"{c:.2f}" for c in comp_re],
              "reliability", [f"{r:.2f}" for r in rel_re])
        # Solid = completeness (recall), dashed = reliability (precision).
        ax[0].plot(NSIGMAS, comp_ns, "-o", color=color, label=f"{tag} completeness")
        ax[0].plot(NSIGMAS, rel_ns, "--s", color=color, alpha=0.7, label=f"{tag} reliability")
        ax[1].plot(REACHES, comp_re, "-o", color=color, label=f"{tag} completeness")
        ax[1].plot(REACHES, rel_re, "--s", color=color, alpha=0.7, label=f"{tag} reliability")

    ax[0].axvline(2.5, ls=":", color="gray")
    ax[0].set_xlabel("nσ threshold"); ax[0].set_ylabel("completeness / reliability")
    ax[0].set_title("completeness↑ vs reliability↓ as nσ drops (reach=8)")
    ax[0].set_ylim(0, 1.02); ax[0].invert_xaxis(); ax[0].legend(fontsize=7)
    ax[1].axvline(8.0, ls=":", color="gray")
    ax[1].set_xlabel("reach (cost budget)"); ax[1].set_ylabel("completeness / reliability")
    ax[1].set_title("vs reach (nσ=2.5)"); ax[1].set_ylim(0, 1.02); ax[1].legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(OUT / "sensitivity.png", dpi=130)
    print("Wrote", OUT / "sensitivity.png")


if __name__ == "__main__":
    main()
