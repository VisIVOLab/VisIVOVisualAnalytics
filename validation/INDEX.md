# VisIVO — paper validation & benchmark artifacts

Reproducible material backing the software paper. Each item is a self-contained
script (run with `backend/.venv/bin/python`) that writes figures/tables to `out/`.
Heavy/regenerable outputs are git-ignored; reproduce via the commands below.

| Paper target | Artifact | Script | Output |
|---|---|---|---|
| **Table 2** (responsiveness, 1/8/30 GB) | [../benchmark/RESULTS.md](../benchmark/RESULTS.md) | `../benchmark/benchmark_visivo.py` (+ `make_synthetic_cube.py`) | `../benchmark/results_cold/table2_rows.tex` |
| Benchmark (out-of-core, compute-side) | [BENCHMARK.md](BENCHMARK.md) | `benchmark.py` | `out/benchmark.{md,csv}` |
| **§5 lasso validation** vs SoFiA-2 (2 cubes) | [README.md](README.md) | `lasso_vs_sofia.py [--dataset wallaby]` (+ `sofia_{hgc44,wallaby}.par` via Docker) | `out/metrics{,_wallaby}.{md,csv}`, `out/source_NN{,_wallaby}.png` |
| §5 lasso completeness–reliability tradeoff (nσ, reach) | [README.md](README.md#completenessreliability-tradeoff) | `lasso_sensitivity.py [--dataset wallaby]` | `out/sensitivity{,_wallaby}.{md,csv,png}` |
| §5 lasso seed (click) robustness | [README.md](README.md#seed-click-robustness) | `lasso_seed_robustness.py [--dataset wallaby]` | `out/seed_robustness{,_wallaby}.{md,csv,png}` |
| §5 Dice over (λv, reach) — Fig. "lasso grid" | [README.md](README.md#completenessreliability-tradeoff) | `lasso_sensitivity.py --grid [--dataset wallaby]` | `out/sensitivity_grid{,_wallaby}.{md,csv,png}` |
| §5 lasso on synthetic discs (S/N, extent, neighbour) — Fig. "lasso synthetic" | [README.md](README.md#the-method-on-its-own-synthetic-discs) | `lasso_synthetic.py` | `out/lasso_synthetic.{md,csv,png}` |
| §5 comparator's fit on discs of known kinematics | [README.md](README.md#the-method-on-its-own-synthetic-discs) | `kinematic_fit_synthetic.py` | `out/kinematic_fit_synthetic.{md,csv}` |
| Default 3-D threshold from the noise (issue #93) | `out/threshold_from_noise.md` | `threshold_from_noise.py [--extra CUBE ...]` | `out/threshold_from_noise.{md,csv}` |
| **Figure 3** panels b/c/e/f (lasso on real HI galaxy) | — | `figure3_lasso_hgc44.py` | `out/figure3_lasso.png` |
| **Figure 7** (cosmic flows: streamlines + basins) | [velocity_field.md](velocity_field.md) | `velocity_field_demo.py` | `out/velocity_field.png` |

## Headline numbers

- **Performance:** every op ≤ 1.9 s on cubes up to 2.1 GB; a full-res channel in
  ~0.2 s / ~150 MB **independent of cube size** (out-of-core navigation); moment-0
  streaming is bit-identical to in-memory yet holds no full-cube copy.
- **Lasso vs SoFiA — HGC 44:** one click, default params → Dice > 0.83,
  completeness > 0.91 at reliability ≈ 0.75, integrated flux within 8 % on the
  main galaxies; the total spectrum tracks SoFiA's double-horn profile.
- **Flux differences are segmentation, not noise:** uncertainties are estimated empirically —
  the mask's stencil (signed, for the comparison) dropped at up to 200 FULLY
  source-free positions — because the analytic rms·√(N·F/B)·Δv model overestimates
  σ_Δ by ~1.3–2.3× wherever it can be checked (the disagreeing voxels share a mask
  boundary, so the signed sum cancels much of their noise) and cannot be checked at
  all on the one stencil that spans the field.
  Differences are 1.9–15.4 noise-equivalent σ on HGC 44 and 4.8–6.3 on the four
  measurable WALLABY sources; the fifth spans the field, so its stencil cannot be
  placed on empty sky at all and is reported without a σ.
  `n_σ` is a fixed-mask noise contrast, NOT a p-value: both masks were selected on
  the data they are integrated over. Quote as segmentation agreement.
- **Velocity field (CF4):** flow streamlines + 15 gravitational basins of
  attraction, via the real backend pipeline.

## Paper figures — status

- **Figure 3** (six panels on a real HI galaxy): the data-driven panels **(b)**
  grown selection, **(c)** the λv kinematic knob, **(e)** isolate, **(f)** remove
  are produced in `out/figure3_lasso.png` on an HGC 44 galaxy — note the selection
  correctly excludes the companion galaxy in (b) and leaves it in the residual (f).
  Panels **(a)** volume rendering with the seed double-click and **(d)** per-channel
  polygon refinement are live GUI screenshots to capture in the app.
- **Figure 7** (cosmic flows): `out/velocity_field.png` gives the streamlines +
  gravitational basins; the co-registered **galaxy catalogue overlay** still needs
  the CF4 galaxy-position file to add.
- **Figure 1** (eight-panel storyboard) and **Figure 4** (tilted-ring model-vs-data):
  GUI screenshots / a fitted-parameter model — capture/produce in the app once the
  HI use-case dataset in the manuscript is fixed. The tilted-ring model *overlays,
  does not fit* — keep that scope explicit.
- **Table 1** capability comparison and the **CARTA column** of Table 2: need CARTA
  installed / cell-by-cell verification (manuscript editing).
