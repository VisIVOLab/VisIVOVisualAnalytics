#!/usr/bin/env python
"""The kinematic lasso on discs whose extent is known.

The comparison with source finders on real cubes (``lasso_vs_sofia.py``) cannot
separate what the algorithm does from what two particular observations are.
This script injects rotating discs built with the application's own
tilted-ring model into Gaussian noise and runs the lasso against a truth that
is known by construction. Three things are varied:

* **signal-to-noise** at the peak voxel (3, 5, 10, 20) and **spatial extent**
  (8, 16, 32 pixels in radius), together: one click at the shipped defaults,
  and the same click at the best of four reach budgets, because the budget is
  a per-object slider and the defaults are a first guess, not a tuning. Every
  number is the median over five clicks (the peak and four random bright
  voxels), with the range, so a result is not an accident of where one click
  landed;
* **separation from a neighbour** of the same brightness, at the same systemic
  velocity (the hard case) and 150 km/s away, at two values of the kinematic
  knob — the case the knob exists for.

Two truths are reported, because a per-voxel selector cannot see what is below
its own threshold: the noiseless disc above 2.5 σ of the injected noise (what
the lasso's threshold can see) and above 1 σ (the faint envelope).

Run:  backend/.venv/bin/python validation/lasso_synthetic.py
→ out/lasso_synthetic.{md,csv,png}
"""
from __future__ import annotations

import csv
import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "backend"))
from app.compute.kinematic_fit import deproject  # noqa: E402
from app.compute.selection import cube_kinematic_select  # noqa: E402
from app.kinematic_model import tilted_ring_model  # noqa: E402

OUT = HERE / "out"
NX = NY = 160
NZ = 64
VEL = np.linspace(800.0, 1300.0, NZ)                 # km/s; 7.9 km/s channels
LINE_SIGMA = 12.0
DISC = dict(pa=40.0, incl=60.0, vsys=1050.0, vrot=120.0)
AMPLITUDE = 1.0                                      # peak of the noiseless cube
SNRS = [3.0, 5.0, 10.0, 20.0]
RADII = [8.0, 16.0, 32.0]
REACHES = [4.0, 8.0, 16.0, 32.0]
N_CLICKS = 5
SEPARATIONS = [1.25, 1.5, 2.0, 3.0]                   # centre distance, in disc radii
NEIGHBOUR_DV = [0.0, 150.0]                           # km/s between the two systemic velocities
NEIGHBOUR_LAMBDAS = [1.0, 3.0]
SEP_SNR, SEP_RADIUS = 10.0, 16.0
# The lasso's shipped defaults, exactly as the client sends them.
DEFAULTS = dict(reach=8.0, lambda_v=1.0, nsigma=2.5)


def disc_cube(x0: float, y0: float, rmax: float, vsys: float) -> np.ndarray:
    """Noiseless disc: the model's unit lines, with an exponential surface
    brightness of scale length rmax so the edge sits at 1/e of the centre."""
    model = tilted_ring_model(NX, NY, VEL, x0, y0, DISC["pa"], DISC["incl"], vsys,
                              DISC["vrot"], rmax, sigma=LINE_SIGMA)
    yy, xx = np.mgrid[0:NY, 0:NX].astype(np.float64)
    _, r = deproject(xx - x0, yy - y0, DISC["pa"], DISC["incl"])
    return (AMPLITUDE * np.exp(-r / rmax)[None, :, :] * model).astype(np.float32)


def metrics(sel: np.ndarray, truth: np.ndarray) -> tuple[float, float, float]:
    inter = int(np.count_nonzero(sel & truth))
    a, b = int(sel.sum()), int(truth.sum())
    dice = 2 * inter / (a + b) if a + b else 0.0
    return dice, (inter / b if b else 0.0), (inter / a if a else 0.0)


def noisy(signal: np.ndarray, snr: float, rng_seed: int) -> tuple[np.ndarray, float]:
    noise = AMPLITUDE / snr
    rng = np.random.default_rng(rng_seed)
    return (signal + rng.normal(0.0, noise, signal.shape)).astype(np.float32), noise


def clicks(signal: np.ndarray, rng_seed: int, n: int = N_CLICKS) -> list[tuple[int, int, int]]:
    """Where a scientist might click: the brightest voxel, then random voxels
    brighter than half the peak — all on the object, none chosen to flatter."""
    peak = tuple(int(v) for v in np.unravel_index(int(np.argmax(signal)), signal.shape))
    bright = np.argwhere(signal > 0.5 * float(signal.max()))
    rng = np.random.default_rng(rng_seed)
    extra = bright[rng.choice(len(bright), size=min(n - 1, len(bright)), replace=False)]
    return [peak] + [tuple(int(v) for v in row) for row in extra]


def fmt_range(values: list[float]) -> str:
    return f"{np.median(values):.2f} [{min(values):.2f}–{max(values):.2f}]"


def main() -> None:
    OUT.mkdir(exist_ok=True)
    rows: list[dict] = []
    md = ["# The kinematic lasso on synthetic discs", "",
          f"Discs built with the application's tilted-ring model ({NX}×{NY}×{NZ} voxels, "
          f"{abs(VEL[1] - VEL[0]):.1f} km/s channels, PA {DISC['pa']:g}°, i {DISC['incl']:g}°, "
          f"V_rot {DISC['vrot']:g} km/s, exponential light of scale length = radius) in Gaussian "
          f"noise. The shipped defaults are reach {DEFAULTS['reach']:g}, λv "
          f"{DEFAULTS['lambda_v']:g}, nσ {DEFAULTS['nsigma']:g}; the noise scale is estimated "
          f"from the data, as the application does. '2.5σ truth' is the noiseless disc above "
          f"2.5 σ of the injected noise, '1σ envelope' above 1 σ. Each cell is the median over "
          f"{N_CLICKS} clicks (the peak and {N_CLICKS - 1} random voxels above half the peak), "
          f"with the range in brackets.", ""]

    # ── S/N × extent, at the defaults and at the best reach ──
    md += ["## Signal-to-noise and extent", "",
           "| S/N | radius (px) | 2.5σ truth (vox) | Dice, defaults | completeness | reliability | "
           "best reach | Dice at best reach | completeness vs 1σ envelope (defaults) |",
           "|---|---|---|---|---|---|---|---|---|"]
    dice_default = np.full((len(RADII), len(SNRS)), np.nan)
    dice_best = np.full((len(RADII), len(SNRS)), np.nan)
    for i, rmax in enumerate(RADII):
        signal = disc_cube(NX / 2, NY / 2, rmax, DISC["vsys"])
        for j, snr in enumerate(SNRS):
            data, noise = noisy(signal, snr, rng_seed=int(10 * rmax + snr))
            truth, envelope = signal > 2.5 * noise, signal > 1.0 * noise
            per_reach: dict[float, list[tuple[float, float, float, float]]] = {r: [] for r in REACHES}
            for seed in clicks(signal, rng_seed=int(7 * rmax + snr)):
                for reach in REACHES:
                    sel = cube_kinematic_select(data, seed, reach=reach,
                                                lambda_v=DEFAULTS["lambda_v"],
                                                nsigma=DEFAULTS["nsigma"])
                    d, c, r_ = metrics(sel, truth)
                    ce = float(np.count_nonzero(sel & envelope) / max(1, envelope.sum()))
                    per_reach[reach].append((d, c, r_, ce))
                    rows.append(dict(experiment="snr_extent", snr=snr, radius=rmax, reach=reach,
                                     lambda_v=DEFAULTS["lambda_v"], seed=str(seed),
                                     separation=np.nan, neighbour_dv=np.nan,
                                     truth_vox=int(truth.sum()), selected_vox=int(sel.sum()),
                                     dice=round(d, 4), completeness=round(c, 4),
                                     reliability=round(r_, 4), completeness_envelope=round(ce, 4),
                                     leak=np.nan))
            at_default = per_reach[DEFAULTS["reach"]]
            medians = {r: float(np.median([t[0] for t in v])) for r, v in per_reach.items()}
            best_reach = max(medians, key=medians.get)
            dice_default[i, j] = medians[DEFAULTS["reach"]]
            dice_best[i, j] = medians[best_reach]
            md.append(f"| {snr:g} | {rmax:g} | {int(truth.sum())} | "
                      f"{fmt_range([t[0] for t in at_default])} | "
                      f"{fmt_range([t[1] for t in at_default])} | "
                      f"{fmt_range([t[2] for t in at_default])} | {best_reach:g} | "
                      f"{fmt_range([t[0] for t in per_reach[best_reach]])} | "
                      f"{fmt_range([t[3] for t in at_default])} |")
    md.append("")

    # ── separation from a neighbour, and the kinematic knob ──
    md += ["## A neighbour of the same brightness", "",
           f"Two discs of radius {SEP_RADIUS:g} px at peak S/N {SEP_SNR:g}, centres separated "
           f"along the apparent minor axis; the click is the left disc's brightest voxel, reach "
           f"{DEFAULTS['reach']:g}. 'Leak' is the fraction of the selection inside the "
           f"neighbour's 2.5σ truth. Below 2 radii the two discs overlap on the sky.", "",
           "| Δv_sys (km/s) | λv | separation (radii) | Dice | completeness | reliability | leak |",
           "|---|---|---|---|---|---|---|"]
    leak = np.full((len(NEIGHBOUR_DV), len(NEIGHBOUR_LAMBDAS), len(SEPARATIONS)), np.nan)
    for i, dv in enumerate(NEIGHBOUR_DV):
        for k, lv in enumerate(NEIGHBOUR_LAMBDAS):
            for j, sep in enumerate(SEPARATIONS):
                d = sep * SEP_RADIUS
                ang = math.radians(DISC["pa"] + 90.0)
                dx, dy = d * math.cos(ang), d * math.sin(ang)
                mine = disc_cube(NX / 2 - dx / 2, NY / 2 - dy / 2, SEP_RADIUS, DISC["vsys"])
                other = disc_cube(NX / 2 + dx / 2, NY / 2 + dy / 2, SEP_RADIUS, DISC["vsys"] + dv)
                data, noise = noisy(mine + other, SEP_SNR, rng_seed=int(100 + 10 * sep + dv))
                seed = tuple(int(v) for v in np.unravel_index(int(np.argmax(mine)), mine.shape))
                sel = cube_kinematic_select(data, seed, reach=DEFAULTS["reach"], lambda_v=lv,
                                            nsigma=DEFAULTS["nsigma"])
                truth, truth_other = mine > 2.5 * noise, other > 2.5 * noise
                dd, cc, rr = metrics(sel, truth)
                lk = float(np.count_nonzero(sel & truth_other) / max(1, sel.sum()))
                leak[i, k, j] = lk
                md.append(f"| {dv:g} | {lv:g} | {sep:g} | {dd:.2f} | {cc:.2f} | {rr:.2f} | {lk:.2f} |")
                rows.append(dict(experiment="neighbour", snr=SEP_SNR, radius=SEP_RADIUS,
                                 reach=DEFAULTS["reach"], lambda_v=lv, seed=str(seed),
                                 separation=sep, neighbour_dv=dv, truth_vox=int(truth.sum()),
                                 selected_vox=int(sel.sum()), dice=round(dd, 4),
                                 completeness=round(cc, 4), reliability=round(rr, 4),
                                 completeness_envelope=np.nan, leak=round(lk, 4)))
    md.append("")

    (OUT / "lasso_synthetic.md").write_text("\n".join(md) + "\n")
    with open(OUT / "lasso_synthetic.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(6.89, 2.7), constrained_layout=True)
    colours = ["#1f77b4", "#2ca02c", "#d62728"]
    for i, (rmax, col) in enumerate(zip(RADII, colours)):
        a1.plot(SNRS, dice_default[i], "-o", ms=4, color=col, label=f"radius {rmax:g} px, defaults")
        a1.plot(SNRS, dice_best[i], "--", color=col, alpha=0.7, label=f"radius {rmax:g} px, best reach")
    a1.set_xscale("log"); a1.set_xticks(SNRS, [f"{s:g}" for s in SNRS])
    a1.set_xlabel("peak signal-to-noise", fontsize=8)
    a1.set_ylabel("Dice vs 2.5σ truth (median of 5 clicks)", fontsize=8)
    a1.set_ylim(0, 1.02); a1.tick_params(labelsize=7); a1.legend(fontsize=6, frameon=False, ncol=2)
    a1.set_title("one click on a disc", fontsize=8.5)
    styles = {0.0: "-", 150.0: "--"}
    for i, dv in enumerate(NEIGHBOUR_DV):
        for k, (lv, col) in enumerate(zip(NEIGHBOUR_LAMBDAS, ["#1f77b4", "#d62728"])):
            a2.plot(SEPARATIONS, leak[i, k], styles[dv], marker="s", ms=4, color=col,
                    label=f"Δv$_\\mathrm{{sys}}$ {dv:g} km s$^{{-1}}$, $\\lambda_v$ {lv:g}")
    a2.axvline(2.0, color="k", ls=":", lw=0.8)
    a2.set_xlabel("centre separation (disc radii)", fontsize=8)
    a2.set_ylabel("fraction of selection in the neighbour", fontsize=8)
    a2.set_ylim(-0.02, 1.02); a2.tick_params(labelsize=7); a2.legend(fontsize=6, frameon=False)
    a2.set_title(f"a neighbour of equal brightness (S/N {SEP_SNR:g})", fontsize=8.5)
    fig.savefig(OUT / "lasso_synthetic.png", dpi=220)
    print("\n".join(md))
    print("written", OUT / "lasso_synthetic.md")


if __name__ == "__main__":
    main()
