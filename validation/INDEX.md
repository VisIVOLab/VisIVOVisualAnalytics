# VisIVO — paper validation & benchmark artifacts

Reproducible material backing the software paper. Each item is a self-contained
script (run with `backend/.venv/bin/python`) that writes figures/tables to `out/`.
Heavy/regenerable outputs are git-ignored; reproduce via the commands below.

| Paper target | Artifact | Script | Output |
|---|---|---|---|
| **Table 2** (responsiveness, 1/8/30 GB) | [../benchmark/RESULTS.md](../benchmark/RESULTS.md) | `../benchmark/benchmark_visivo.py` (+ `make_synthetic_cube.py`) | `../benchmark/results_cold/table2_rows.tex` |
| Benchmark (out-of-core, compute-side) | [BENCHMARK.md](BENCHMARK.md) | `benchmark.py` | `out/benchmark.{md,csv}` |
| **§5 lasso validation** vs SoFiA-2 | [README.md](README.md) | `lasso_vs_sofia.py` (+ `sofia_hgc44.par` via Docker) | `out/metrics.{md,csv}`, `out/source_NN.png` |
| §5 lasso faint-source recovery (λv/nσ) | [README.md](README.md#interactive-recovery-of-faint-sources) | `lasso_sensitivity.py` | `out/sensitivity.png` |
| **Figure 3** panels b/c/e/f (lasso on real HI galaxy) | — | `figure3_lasso_hgc44.py` | `out/figure3_lasso.png` |
| **Figure 7** (cosmic flows: streamlines + basins) | [velocity_field.md](velocity_field.md) | `velocity_field_demo.py` | `out/velocity_field.png` |

## Headline numbers

- **Performance:** every op ≤ 1.9 s on cubes up to 2.1 GB; a full-res channel in
  ~0.2 s / ~150 MB **independent of cube size** (out-of-core navigation); moment-0
  streaming is bit-identical to in-memory yet holds no full-cube copy.
- **Lasso vs SoFiA (HGC 44):** one click, default params → Dice > 0.83,
  completeness > 0.91, integrated flux within 8 % on the main galaxies; the total
  spectrum tracks SoFiA's double-horn profile. Faint sources: completeness
  0.59 → 0.97 by lowering nσ (tunable, not a limit).
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
