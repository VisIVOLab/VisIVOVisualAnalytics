#!/usr/bin/env python
"""Kinematic Lasso — interactive-parameter sensitivity (nσ, reach).

Reports the completeness–RELIABILITY tradeoff of the lasso's two interactive
controls, per source. Lowering nσ (or raising reach) grows the selection toward
the SoFiA mask and raises completeness (∩/SoFiA) — but it also floods into
noise, which LOWERS reliability (precision, ∩/lasso). Both curves are reported
at every nσ and reach so the tradeoff is explicit, not just the metric that
improves (R-18): a completeness gain bought with a reliability collapse is not a
real recovery. Dice is tabulated alongside as the single number that penalises
both failure modes, so "the best setting" is not read off the flattering curve.

The numbers go to out/sensitivity{,_<dataset>}.{md,csv} as well as the figure —
a claim in the paper must be traceable to a committed artifact, not to a curve
someone has to re-measure with a ruler.

Run:  backend/.venv/bin/python validation/lasso_sensitivity.py [--dataset NAME]
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "backend"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from app.compute.selection import cube_kinematic_select  # noqa: E402
from datasets import OUT, add_dataset_arg, rank_sources, resolve  # noqa: E402

NSIGMAS = [1.5, 2.0, 2.5, 3.0, 3.5]
# Wide enough to show an interior optimum, not just the part of the curve that
# rises: on an extended source the best Dice sits well above the default reach,
# and past it the selection floods. A sweep that stopped at the default would
# have hidden both facts.
REACHES = [4.0, 8.0, 16.0, 32.0, 64.0]
DEFAULT_NSIGMA, DEFAULT_REACH = 2.5, 8.0
# The two interactive sliders together: Dice over the (λv, reach) plane, per
# source. The kinematic knob is a per-channel step weight, so its useful range
# is set by how many channels the object spans — the grid covers both sides of
# isotropic (λv = 1) at every reach the one-dimensional sweep uses.
GRID_LAMBDAS = [0.35, 0.5, 1.0, 2.0, 3.0]
GRID_REACHES = [4.0, 8.0, 16.0, 32.0]


def metrics(lasso, src) -> tuple[float, float, float]:
    inter = int(np.count_nonzero(lasso & src))
    a, b = int(lasso.sum()), int(src.sum())
    dice = 2 * inter / (a + b) if a + b else 0.0
    completeness = inter / b if b else 0.0          # ∩ / SoFiA  (recall)
    reliability = inter / a if a else 0.0           # ∩ / lasso  (precision)
    return dice, completeness, reliability


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    add_dataset_arg(ap)
    ap.add_argument("--grid", action="store_true",
                    help="also sweep the (λv, reach) plane and write sensitivity_grid.{png,md,csv}")
    args = ap.parse_args()
    ds = resolve(args.dataset)

    OUT.mkdir(exist_ok=True)
    data = np.squeeze(fits.getdata(ds.cube)).astype(np.float32)
    sofia = np.squeeze(fits.getdata(ds.mask_path)).astype(np.int32)
    labels, counts, order = rank_sources(sofia)
    bright, faint = int(labels[order[0]]), int(labels[order[3]])   # #1 and #4 by size

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(1, 2, figsize=(11, 4.4))

    rows: list[dict] = []
    for lab, color in ((bright, "tab:blue"), (faint, "tab:red")):
        src = sofia == lab
        zz, yy, xx = np.where(src)
        k = int(np.nanargmax(data[zz, yy, xx]))
        seed = (int(zz[k]), int(yy[k]), int(xx[k]))
        kind = "largest" if lab == bright else "4th-largest"
        tag = f"#{lab} ({kind})"

        curves: dict[str, list[float]] = {}
        for axis, values, fixed in (("nsigma", NSIGMAS, DEFAULT_REACH),
                                    ("reach", REACHES, DEFAULT_NSIGMA)):
            d_, c_, r_ = [], [], []
            for v in values:
                kw = ({"nsigma": v, "reach": fixed} if axis == "nsigma"
                      else {"reach": v, "nsigma": fixed})
                m = cube_kinematic_select(data, seed, lambda_v=1.0, **kw)
                dd, cc, rr = metrics(m, src)
                d_.append(dd); c_.append(cc); r_.append(rr)
                rows.append(dict(
                    sofia_id=lab, kind=kind, swept=axis, value=v,
                    nsigma=kw["nsigma"], reach=kw["reach"],
                    dice=round(dd, 4), completeness=round(cc, 4),
                    reliability=round(rr, 4),
                    lasso_vox=int(m.sum()), sofia_vox=int(src.sum())))
            curves[axis] = [d_, c_, r_]

        print(tag,
              "| nσ: compl", [f"{c:.2f}" for c in curves["nsigma"][1]],
              "relia", [f"{r:.2f}" for r in curves["nsigma"][2]],
              "| reach: compl", [f"{c:.2f}" for c in curves["reach"][1]],
              "relia", [f"{r:.2f}" for r in curves["reach"][2]])

        # Solid = completeness (recall), dashed = reliability (precision),
        # dotted = Dice, which is the one that shows where the optimum is.
        for i, (vals, key) in enumerate(((NSIGMAS, "nsigma"), (REACHES, "reach"))):
            d_, c_, r_ = curves[key]
            ax[i].plot(vals, c_, "-o", color=color, label=f"{tag} completeness")
            ax[i].plot(vals, r_, "--s", color=color, alpha=0.7, label=f"{tag} reliability")
            ax[i].plot(vals, d_, ":^", color=color, alpha=0.5, label=f"{tag} Dice")

    ax[0].axvline(DEFAULT_NSIGMA, ls=":", color="gray")
    ax[0].set_xlabel("nσ threshold"); ax[0].set_ylabel("completeness / reliability / Dice")
    ax[0].set_title(f"{ds.title}: completeness↑ vs reliability↓ as nσ drops "
                    f"(reach={DEFAULT_REACH:g})")
    ax[0].set_ylim(0, 1.02); ax[0].invert_xaxis(); ax[0].legend(fontsize=6)
    ax[1].axvline(DEFAULT_REACH, ls=":", color="gray")
    ax[1].set_xscale("log", base=2)
    ax[1].set_xlabel("reach (geodesic cost budget, voxels)")
    ax[1].set_ylabel("completeness / reliability / Dice")
    ax[1].set_title(f"vs reach (nσ={DEFAULT_NSIGMA})")
    ax[1].set_ylim(0, 1.02); ax[1].legend(fontsize=6)
    fig.tight_layout()
    fig.savefig(ds.out("sensitivity", "png"), dpi=130)

    # ── traceable tables ──
    with open(ds.out("sensitivity", "csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)

    md = [f"# Lasso parameter sensitivity — {ds.title} ({ds.instrument})",
          f"\nCompleteness (∩/SoFiA), reliability (∩/lasso) and Dice at every "
          f"setting of the two interactive controls, for the largest and the "
          f"4th-largest SoFiA source. Defaults are nσ={DEFAULT_NSIGMA}, "
          f"reach={DEFAULT_REACH:g}; the other control is held at its default "
          f"while one is swept.\n"]
    for swept, unit in (("nsigma", "nσ"), ("reach", "reach")):
        md += [f"\n## Sweeping {unit}\n",
               f"| source | {unit} | Dice | completeness | reliability | lasso vox |",
               "|---|---|---|---|---|---|"]
        for r in rows:
            if r["swept"] != swept:
                continue
            md.append(f"| #{r['sofia_id']} ({r['kind']}) | {r['value']:g} | "
                      f"{r['dice']:.3f} | {r['completeness']:.3f} | "
                      f"{r['reliability']:.3f} | {r['lasso_vox']} |")
    # State the best-Dice setting explicitly: it is the honest summary, and it is
    # what a reader would otherwise eyeball off the figure.
    md.append("\n**Best Dice per source and axis**\n")
    md.append("| source | axis | best value | Dice | completeness | reliability |")
    md.append("|---|---|---|---|---|---|")
    for lab in sorted({r["sofia_id"] for r in rows}):
        for swept in ("nsigma", "reach"):
            sub = [r for r in rows if r["sofia_id"] == lab and r["swept"] == swept]
            b = max(sub, key=lambda r: r["dice"])
            md.append(f"| #{lab} | {swept} | {b['value']:g} | {b['dice']:.3f} | "
                      f"{b['completeness']:.3f} | {b['reliability']:.3f} |")
    ds.out("sensitivity", "md").write_text("\n".join(md) + "\n")
    print("Wrote", ds.out("sensitivity", "png"), "and", ds.out("sensitivity", "md"))

    if args.grid:
        grid(ds, data, sofia, bright, faint)


def grid(ds, data, sofia, bright: int, faint: int) -> None:
    """Dice over the (λv, reach) plane for the two sources: a heatmap and a table."""
    import matplotlib.pyplot as plt
    rows: list[dict] = []
    fig, axes = plt.subplots(1, 2, figsize=(6.89, 2.9), constrained_layout=True)
    for ax, lab in zip(axes, (bright, faint)):
        src = sofia == lab
        zz, yy, xx = np.where(src)
        k = int(np.nanargmax(data[zz, yy, xx]))
        seed = (int(zz[k]), int(yy[k]), int(xx[k]))
        kind = "largest" if lab == bright else "4th-largest"
        dice = np.full((len(GRID_LAMBDAS), len(GRID_REACHES)), np.nan)
        for i, lv in enumerate(GRID_LAMBDAS):
            for j, reach in enumerate(GRID_REACHES):
                m = cube_kinematic_select(data, seed, reach=reach, lambda_v=lv,
                                          nsigma=DEFAULT_NSIGMA)
                dd, cc, rr = metrics(m, src)
                dice[i, j] = dd
                rows.append(dict(sofia_id=lab, kind=kind, lambda_v=lv, reach=reach,
                                 nsigma=DEFAULT_NSIGMA, dice=round(dd, 4),
                                 completeness=round(cc, 4), reliability=round(rr, 4),
                                 lasso_vox=int(m.sum()), sofia_vox=int(src.sum())))
        im = ax.imshow(dice, origin="lower", cmap="viridis", vmin=0, vmax=1, aspect="auto")
        ax.set_xticks(range(len(GRID_REACHES)), [f"{r:g}" for r in GRID_REACHES])
        ax.set_yticks(range(len(GRID_LAMBDAS)), [f"{l:g}" for l in GRID_LAMBDAS])
        ax.set_xlabel("reach", fontsize=8); ax.set_ylabel("$\\lambda_v$", fontsize=8)
        ax.tick_params(labelsize=7)
        ax.set_title(f"#{lab} ({kind}): Dice vs SoFiA-2", fontsize=8.5)
        for i in range(len(GRID_LAMBDAS)):
            for j in range(len(GRID_REACHES)):
                ax.text(j, i, f"{dice[i, j]:.2f}", ha="center", va="center", fontsize=6.5,
                        color="white" if dice[i, j] < 0.6 else "black")
        # the shipped defaults
        ax.plot(GRID_REACHES.index(DEFAULT_REACH), GRID_LAMBDAS.index(1.0), marker="s",
                mfc="none", mec="#d62728", mew=1.4, ms=16)
    cb = fig.colorbar(im, ax=axes, shrink=0.9, pad=0.02)
    cb.set_label("Dice", fontsize=8); cb.ax.tick_params(labelsize=7)
    fig.savefig(ds.out("sensitivity_grid", "png"), dpi=220)

    with open(ds.out("sensitivity_grid", "csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)
    md = [f"# Dice over (λv, reach) — {ds.title}", "",
          f"nσ = {DEFAULT_NSIGMA}; seed = each source's peak voxel; Dice against the SoFiA-2 mask. "
          f"The square marks the shipped defaults (λv = 1, reach = {DEFAULT_REACH:g}).", ""]
    for lab in (bright, faint):
        sub = [r for r in rows if r["sofia_id"] == lab]
        md += [f"## #{lab} ({sub[0]['kind']})", "",
               "| λv \\ reach | " + " | ".join(f"{r:g}" for r in GRID_REACHES) + " |",
               "|---|" + "---|" * len(GRID_REACHES)]
        for lv in GRID_LAMBDAS:
            cells = [next(r for r in sub if r["lambda_v"] == lv and r["reach"] == rc)["dice"]
                     for rc in GRID_REACHES]
            md.append(f"| {lv:g} | " + " | ".join(f"{c:.3f}" for c in cells) + " |")
        best = max(sub, key=lambda r: r["dice"])
        at_default = next(r for r in sub if r["lambda_v"] == 1.0 and r["reach"] == DEFAULT_REACH)
        md += ["", f"Best Dice {best['dice']:.3f} at λv = {best['lambda_v']:g}, reach = "
               f"{best['reach']:g}; at the defaults {at_default['dice']:.3f} "
               f"(completeness {at_default['completeness']:.2f}, reliability "
               f"{at_default['reliability']:.2f}).", ""]
    ds.out("sensitivity_grid", "md").write_text("\n".join(md) + "\n")
    print("Wrote", ds.out("sensitivity_grid", "png"), "and", ds.out("sensitivity_grid", "md"))


if __name__ == "__main__":
    main()
