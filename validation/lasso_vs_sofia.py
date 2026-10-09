#!/usr/bin/env python
"""Kinematic Lasso vs SoFiA-2 — scientific validation on a real HI cube.

Compares the VisIVO Kinematic-Lasso geodesic selection against a SoFiA-2 source
mask on the same cube. For each of the main SoFiA-detected galaxies we seed the
lasso on the source's peak voxel, grow the selection with default parameters,
and measure:

  - spatial agreement: Dice, IoU, completeness (∩/SoFiA), reliability (∩/lasso)
  - integrated HI flux (Jy·km/s) — lasso vs SoFiA, their ratio, and the
    noise-equivalent contrast of their difference (R-18d)
  - the total (spatially-integrated) spectrum per channel, overlaid

Outputs a metrics table (markdown + CSV) and per-source figures under out/.

Run:  backend/.venv/bin/python validation/lasso_vs_sofia.py [--dataset NAME] [MASK]
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

sys.path.insert(0, str(Path(__file__).resolve().parents[0].parent / "backend"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from app.compute.selection import cube_kinematic_select  # noqa: E402
from datasets import OUT, add_dataset_arg, resolve  # noqa: E402

N_SOURCES = 5                 # compare the N largest SoFiA sources
REACH, LAMBDA_V, NSIGMA = 8.0, 1.0, 2.5   # lasso defaults (as a user would click)


def robust_sigma(data: np.ndarray, source_free: np.ndarray | None = None) -> float:
    """Noise scale (1.4826·MAD), estimated from EMISSION-FREE voxels.

    Including the sources biases the MAD high; the effect measured here is small
    (0.05 % on HGC 44, 1.7 % on WALLABY) but excluding them costs nothing and
    removes an avoidable objection.
    """
    sample = data if source_free is None else data[source_free]
    finite = np.asarray(sample[np.isfinite(sample)], dtype=np.float64)
    if finite.size == 0:
        return 0.0
    if finite.size > 5_000_000:
        finite = finite[:: finite.size // 5_000_000]
    return float(1.4826 * np.median(np.abs(finite - np.median(finite))))


def stencil_flux_sigma(data: np.ndarray, source_free: np.ndarray,
                       weights: np.ndarray, rng: np.random.Generator,
                       n_trials: int = 200, min_trials: int = 30
                       ) -> tuple[float, int]:
    """Uncertainty of a weighted voxel sum, measured rather than modelled.

    An analytic ``rms·√(N·F/B)`` needs three assumptions that do not hold here:
    that the mask is many beams across (the faint WALLABY masks are a few beam
    elements), that one cube-wide spectral factor describes an irregular 3-D
    mask, and — for a DIFFERENCE of two overlapping masks — that an unsigned voxel
    count captures a covariance double sum over signed weights. It does not: the
    two disagreeing sets sit on a shared boundary, so their noise partly cancels.

    So instead: take the stencil itself (``weights`` = +1 / −1 per voxel, e.g.
    ``1_lasso − 1_sofia``), drop it at source-free positions, and take the scatter
    of the resulting weighted sums. This is the standard empty-aperture error used
    in radio work, and it avoids the analytic covariance model entirely — at the
    price of assuming the accepted off-source placements represent the noise and
    covariance at the source's own position and channel range.

    Placements must be **fully** source-free. An earlier version accepted ≥98 %
    and then summed every finite voxel, so up to 2 % of the contributing voxels
    were real emission — enough to inflate the scatter by ~8 % on the largest
    WALLABY stencil, well above the ~5 % Monte-Carlo error of the estimate itself.

    Returns ``(sigma_in_data_units, n_accepted_trials)``; sigma is NaN when fewer
    than ``min_trials`` fully clean placements exist — which is the real answer for
    a source spanning most of the cube, not something to paper over.
    """
    zz, yy, xx = np.nonzero(weights)
    if zz.size == 0:
        return 0.0, 0
    w = weights[zz, yy, xx].astype(np.float64)
    # Stencil coordinates relative to its own corner.
    z0, y0, x0 = zz.min(), yy.min(), xx.min()
    dz, dy, dx = zz - z0, yy - y0, xx - x0
    ez, ey, ex = int(dz.max()) + 1, int(dy.max()) + 1, int(dx.max()) + 1
    nz, ny, nx = data.shape
    if ez > nz or ey > ny or ex > nx:
        return float("nan"), 0

    sums: list[float] = []
    for _ in range(n_trials * 8):          # oversample: most draws hit a source
        if len(sums) >= n_trials:
            break
        oz = int(rng.integers(0, nz - ez + 1))
        oy = int(rng.integers(0, ny - ey + 1))
        ox = int(rng.integers(0, nx - ex + 1))
        zi, yi, xi = dz + oz, dy + oy, dx + ox
        # source_free already requires finite; ALL contributing voxels must be in
        # it, since the sum below uses every one of them.
        if not source_free[zi, yi, xi].all():
            continue
        sums.append(float(np.dot(w, data[zi, yi, xi])))
    if len(sums) < min_trials:
        return float("nan"), len(sums)
    return float(np.std(sums, ddof=1)), len(sums)


def spectral_correlation_factor(data: np.ndarray, mask: np.ndarray, max_lag: int = 5) -> float:
    """Effective-sample-size penalty for channel-to-channel noise correlation,
    ``F = 1 + 2·Σ_k ρ_k`` over positive lags (stationarity makes negative lags the
    same, hence the factor 2).

    Used ONLY for the analytic cross-check column, not for the reported error. Two
    reasons it is not good enough on its own: a finite sum needs lag weights
    ``(1 − k/N)`` and an irregular 3-D mask needs the mask's own channel overlap,
    neither of which one cube-wide scalar can carry. The truncation below (ρ ≤ 0.02
    or lag 5) is also a judgement call — on HGC 44 the raw ρ_k sits near 0.04–0.05
    out to lag 20, which a per-pixel baseline removes past lag 3, so the estimator
    definition moves the answer. The empirical stencil scatter sidesteps all of it.
    """
    empty = (mask == 0) & np.isfinite(data)
    total = 1.0
    for lag in range(1, max_lag + 1):
        both = empty[:-lag] & empty[lag:]
        if int(both.sum()) < 1000:
            break
        rho = float(np.corrcoef(data[:-lag][both], data[lag:][both])[0, 1])
        if not np.isfinite(rho) or rho <= 0.02:
            break
        total += 2.0 * rho
    return max(total, 1.0)


def beam_area_px(hd) -> float:
    """Gaussian beam solid angle in pixels: (π/4ln2)·BMAJ·BMIN / (|CD1|·|CD2|)."""
    bmaj, bmin = float(hd["BMAJ"]), float(hd["BMIN"])
    cd1, cd2 = abs(float(hd["CDELT1"])), abs(float(hd["CDELT2"]))
    return (np.pi / (4.0 * np.log(2.0))) * bmaj * bmin / (cd1 * cd2)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    add_dataset_arg(ap)
    ap.add_argument("mask", nargs="?", help="override the reference SoFiA mask "
                                            "(e.g. a differently-parameterised run)")
    args = ap.parse_args()
    # resolve() fails early with an actionable message rather than a cryptic astropy
    # error: the paper artifact must be runnable by a co-author who has only cloned.
    ds = resolve(args.dataset)
    CUBE = ds.cube
    MASK = args.mask or str(ds.mask_path)
    if args.mask and not Path(MASK).is_file():
        sys.exit(f"error: SoFiA mask not found: {MASK}")

    OUT.mkdir(exist_ok=True)
    hd = fits.getheader(CUBE)
    data = np.squeeze(fits.getdata(CUBE)).astype(np.float32)
    sofia = np.squeeze(fits.getdata(MASK)).astype(np.int32)
    if data.shape != sofia.shape:
        sys.exit(f"error: cube {data.shape} and mask {sofia.shape} differ in shape")

    barea = beam_area_px(hd)
    dv = abs(float(hd["CDELT3"])) / 1000.0            # m/s → km/s per channel
    # Jy·km/s over a boolean voxel mask.
    flux = lambda msk: float(data[msk].sum()) * dv / barea
    # R-18(d) / R-09: correlated-noise uncertainty on an integrated flux. Voxels
    # inside a beam are not independent, so N voxels carry only N/beam_area
    # independent samples: σ_F = rms·√(N/B)·Δv. Same formula the backend uses for
    # aperture photometry, so a number in the paper and a number in the app mean
    # the same thing.
    # Noise domain for both the rms and the empirical stencil scatter: outside a
    # DILATED SoFiA mask, so faint wings below SoFiA's own threshold are excluded
    # from "empty sky" too, and finite.
    from scipy import ndimage  # noqa: PLC0415
    source_free = (~ndimage.binary_dilation(sofia > 0, iterations=2)) & np.isfinite(data)
    sigma_rms = robust_sigma(data, source_free)
    rng = np.random.default_rng(20260904)
    # Uncertainty of a weighted voxel sum, in Jy·km/s.
    def stencil_err(weights: np.ndarray) -> tuple[float, int]:
        sd, n = stencil_flux_sigma(data, source_free, weights, rng)
        return (sd * dv / barea if np.isfinite(sd) else float("nan")), n

    # Kept alongside as a cross-check, NOT as the reported number: the analytic
    # rms·√(N·F/B)·Δv assumes a mask many beams across and one cube-wide spectral
    # factor. Printing both makes it visible when those assumptions break.
    f_chan = spectral_correlation_factor(data, sofia)
    analytic_err = lambda n: sigma_rms * np.sqrt(max(int(n), 0) * f_chan / barea) * dv

    labels, counts = np.unique(sofia[sofia > 0], return_counts=True)
    if labels.size == 0:
        sys.exit("error: the SoFiA mask has no labelled sources (all zero).")
    order = np.argsort(counts)[::-1][:N_SOURCES]

    rows = []
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    for rank, i in enumerate(order, 1):
        lab = int(labels[i])
        src = sofia == lab
        zz, yy, xx = np.where(src)
        src_vals = data[zz, yy, xx]
        if not np.isfinite(src_vals).any():
            # A SoFiA source whose every voxel is NaN has no valid peak to seed on — skip it
            # (seeding a NaN voxel would just be rejected by the lasso).
            print(f"[{rank}] SoFiA #{lab}: skipped — no finite voxel in the mask.")
            continue
        # Peak voxel = the click. nanargmax so a NaN inside the SoFiA mask can't win the peak.
        k = int(np.nanargmax(src_vals))
        seed = (int(zz[k]), int(yy[k]), int(xx[k]))

        lasso = cube_kinematic_select(
            data, seed, reach=REACH, lambda_v=LAMBDA_V, sigma=None, nsigma=NSIGMA)

        inter = int(np.count_nonzero(src & lasso))
        a, b = int(lasso.sum()), int(src.sum())
        dice = 2 * inter / (a + b) if (a + b) else 0.0
        iou = inter / (a + b - inter) if (a + b - inter) else 0.0
        completeness = inter / b if b else 0.0            # fraction of SoFiA recovered
        reliability = inter / a if a else 0.0             # fraction of lasso that is real
        fl, fs = flux(lasso), flux(src)
        # Each mask's own error, and — crucially — the error of the DIFFERENCE from
        # the SIGNED stencil (+1 where only the lasso selects, −1 where only SoFiA
        # does). The intersection cancels in the value, but its noise contribution
        # is not simply removed from the variance: the two disagreeing sets lie on
        # a shared boundary, so the signed sum partly cancels their noise as well.
        # Measuring the signed stencil is what makes that automatic.
        efs, n_s = stencil_err(src.astype(np.int8))
        efl, n_l = stencil_err(lasso.astype(np.int8))
        signed = lasso.astype(np.int8) - src.astype(np.int8)
        n_symdiff = int(np.count_nonzero(signed))
        d_flux = fl - fs
        e_diff, n_d = stencil_err(signed)
        nsig = abs(d_flux) / e_diff if (np.isfinite(e_diff) and e_diff > 0) else float("nan")
        # The model the empirical number replaces, kept for comparison only.
        a_diff = analytic_err(n_symdiff)
        rows.append(dict(
            rank=rank, sofia_id=lab, sofia_vox=b, lasso_vox=a, inter=inter,
            dice=dice, iou=iou, completeness=completeness, reliability=reliability,
            flux_sofia=fs, flux_sofia_err=efs, flux_lasso=fl, flux_lasso_err=efl,
            flux_ratio=(fl / fs if fs else float("nan")),
            symdiff_vox=n_symdiff, flux_diff=d_flux, flux_diff_err=e_diff,
            flux_diff_nsigma=nsig, rms=sigma_rms, spectral_corr_factor=f_chan,
            flux_diff_err_analytic=a_diff, trials_sofia=n_s, trials_lasso=n_l,
            trials_diff=n_d,
            seed_zyx=seed))
        ratio_str = f"{fl / fs:.3f}" if fs else "n/a"   # guard a zero SoFiA flux
        # A stencil too large to place on empty sky has NO measurable scatter in
        # this cube; print that rather than a number.
        fmt = lambda v, p=2: ("n/a" if not np.isfinite(v) else f"{v:.{p}f}")
        print(f"[{rank}] SoFiA #{lab}: Dice={dice:.3f} IoU={iou:.3f} "
              f"compl={completeness:.3f} relia={reliability:.3f} "
              f"Fsofia={fs:.2f}±{fmt(efs)} Flasso={fl:.2f}±{fmt(efl)} "
              f"ratio={ratio_str} Δ={d_flux:+.2f}±{fmt(e_diff)} ({fmt(nsig, 1)}σ) "
              f"[analytic σ_Δ {a_diff:.2f}, {n_d} clean placements]")

        # ── figures: moment-0 with both mask contours + total-spectrum overlay ──
        z0, z1 = min(zz.min(), np.where(lasso.any((1, 2)))[0].min()), \
                 max(zz.max(), np.where(lasso.any((1, 2)))[0].max())
        y0, y1, x0, x1 = yy.min(), yy.max(), xx.min(), xx.max()
        pad = 20
        y0, y1 = max(0, y0 - pad), min(data.shape[1], y1 + pad)
        x0, x1 = max(0, x0 - pad), min(data.shape[2], x1 + pad)
        mom0 = data[z0:z1 + 1, y0:y1 + 1, x0:x1 + 1].sum(0)

        fig, ax = plt.subplots(1, 2, figsize=(11, 4.4))
        ax[0].imshow(mom0, origin="lower", cmap="gray_r")
        ax[0].contour(src[z0:z1 + 1, y0:y1 + 1, x0:x1 + 1].any(0), levels=[0.5],
                      colors="tab:blue", linewidths=1.2)
        ax[0].contour(lasso[z0:z1 + 1, y0:y1 + 1, x0:x1 + 1].any(0), levels=[0.5],
                      colors="tab:green", linewidths=1.2)
        ax[0].set_title(f"SoFiA #{lab}: moment-0 + masks\n(blue=SoFiA, green=Lasso)")
        ax[0].set_xticks([]); ax[0].set_yticks([])

        spec_s = np.where(src, data, 0.0).sum((1, 2)) * dv / barea
        spec_l = np.where(lasso, data, 0.0).sum((1, 2)) * dv / barea
        zc = np.arange(data.shape[0])
        ax[1].plot(zc, spec_s, color="tab:blue", label="SoFiA")
        ax[1].plot(zc, spec_l, color="tab:green", ls="--", label="Lasso")
        ax[1].set_xlim(max(0, z0 - 5), min(data.shape[0], z1 + 5))
        ax[1].set_xlabel("channel"); ax[1].set_ylabel("flux (Jy·km/s)")
        ax[1].set_title("total spectrum"); ax[1].legend()
        fig.tight_layout()
        fig.savefig(ds.out(f"source_{lab:02d}", "png"), dpi=130)
        plt.close(fig)

    # ── metrics table ──
    if not rows:
        sys.exit("error: no SoFiA source could be seeded (all masks empty/NaN).")
    with open(ds.out("metrics", "csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)

    md = [f"# Kinematic Lasso vs SoFiA-2 — {ds.title} ({ds.instrument})",
          f"\n{ds.note}.\n",
          f"\nCube `{Path(CUBE).name}` {data.shape}, beam {barea:.1f} px, "
          f"Δv {dv:.2f} km/s, rms {sigma_rms:.4g} (1.4826·MAD), "
          f"spectral correlation factor F = {f_chan:.2f} (analytic column only). "
          f"Lasso defaults reach={REACH}, λv={LAMBDA_V}, nσ={NSIGMA}. "
          f"Seed = each SoFiA source's peak voxel.\n",
          "\nFlux uncertainties are estimated empirically rather than modelled: the "
          "mask's own stencil "
          "(and, for Δ, the SIGNED stencil +1 lasso / −1 SoFiA) is dropped at "
          "positions that are FULLY outside a dilated SoFiA mask, and σ is the "
          "scatter of the resulting weighted sums. This avoids the analytic "
          "covariance model, at the price of assuming those off-source placements "
          "represent the noise and covariance at the source's own position and "
          "channel range. `σ_Δ analytic` is the rms·√(N·F/B)·Δv model shown for "
          "comparison only: wherever it can be checked it overestimates σ_Δ (the "
          "disagreeing voxels share a mask boundary, so the signed sum cancels much "
          "of their noise), and on the one stencil that spans the field it cannot "
          "be checked at all.\n",
          "\n`n_σ` is a fixed-mask, noise-equivalent contrast, NOT a p-value: both "
          "masks were selected on the same noisy data they are integrated over, so "
          "boundary voxels carry selection bias that this does not model.\n",
          "| SoFiA id | SoFiA vox | Lasso vox | Dice | IoU | Compl. | Relia. | "
          "F_SoFiA | F_Lasso | ratio | Δflux | σ_Δ | σ_Δ analytic | n_σ | clean Δ placements |",
          "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    def cell(v: float, prec: int = 2) -> str:
        """An unmeasurable σ prints as a dash, never as a number."""
        return "—" if not np.isfinite(v) else f"{v:.{prec}f}"

    for r in rows:
        md.append(f"| {r['sofia_id']} | {r['sofia_vox']} | {r['lasso_vox']} | "
                  f"{r['dice']:.3f} | {r['iou']:.3f} | {r['completeness']:.3f} | "
                  f"{r['reliability']:.3f} | "
                  f"{r['flux_sofia']:.2f} ± {cell(r['flux_sofia_err'])} | "
                  f"{r['flux_lasso']:.2f} ± {cell(r['flux_lasso_err'])} | "
                  f"{r['flux_ratio']:.3f} | {r['flux_diff']:+.2f} | "
                  f"{cell(r['flux_diff_err'])} | {r['flux_diff_err_analytic']:.2f} | "
                  f"{cell(r['flux_diff_nsigma'], 1)} | {r['trials_diff']} |")
    if any(not np.isfinite(r["flux_diff_err"]) for r in rows):
        md.append("\nA dash means the stencil is too large to place anywhere fully "
                  "source-free in this cube, so its empty-sky scatter is not "
                  "measurable here — reported as such rather than substituted with "
                  "the analytic model, which the other rows show overestimates σ_Δ "
                  "by up to a factor of two and is untested in exactly this regime.")
    d = np.array([r["dice"] for r in rows]); fr = np.array([r["flux_ratio"] for r in rows])
    ns = np.array([r["flux_diff_nsigma"] for r in rows], dtype=float)
    n_measured = int(np.isfinite(ns).sum())
    # nanmedian: a source with zero SoFiA flux contributes a NaN ratio (guarded above).
    md.append(f"\n**Median Dice {np.median(d):.3f}, median flux ratio "
              f"{np.nanmedian(fr):.3f}, median |Δflux| {np.nanmedian(ns):.1f} "
              f"noise-equivalent σ over the {n_measured}/{len(rows)} sources whose "
              f"scatter is measurable.**")
    ds.out("metrics", "md").write_text("\n".join(md) + "\n")
    print("\nWrote", ds.out("metrics", "md"), "and per-source figures.")


if __name__ == "__main__":
    main()
