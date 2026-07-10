# VisIVO — paper validation & benchmark artifacts

Reproducible material backing the software paper. Each item is a self-contained
script (run with `backend/.venv/bin/python`) that writes figures/tables to `out/`.
Heavy/regenerable outputs are git-ignored; reproduce via the commands below.

| Weakness addressed | Artifact | Script | Output |
|---|---|---|---|
| #1 Performance (out-of-core, interactive on multi-GB) | [BENCHMARK.md](BENCHMARK.md) | `benchmark.py` | `out/benchmark.{md,csv}` |
| #2 Lasso scientifically validated vs SoFiA-2 | [README.md](README.md) | `lasso_vs_sofia.py` (+ `sofia_hgc44.par` via Docker) | `out/metrics.{md,csv}`, `out/source_NN.png` |
| #2/#3 Lasso interactive-parameter recovery | [README.md](README.md#interactive-recovery-of-faint-sources) | `lasso_sensitivity.py` | `out/sensitivity.png` |
| #3 Velocity-field use case (F1, cosmic flows) | [velocity_field.md](velocity_field.md) | `velocity_field_demo.py` | `out/velocity_field.png` |

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

## Still manuscript-dependent

Other use cases (multi-cube compare, tilted-ring model, dynspec) need the draft to
fix framing/datasets. The tilted-ring model *overlays but does not fit* — keep that
scope explicit in the paper.
