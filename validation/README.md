# Kinematic Lasso — scientific validation vs SoFiA-2

Reproducible comparison of the VisIVO **Kinematic Lasso** geodesic selection
against a **SoFiA-2** source mask on a real HI cube (Hickson Compact Group 44),
for the software paper. The lasso is **assisted segmentation**, not blind
detection: given a seed *on* a source (here SoFiA's own peak voxel), one click
grows a selection, and we quantify how well that selection agrees with the SoFiA
mask (spatial overlap + integrated flux + total spectrum). This measures
segmentation agreement given a correct seed — NOT detection completeness or
reliability over a survey, which only a blind finder like SoFiA provides.

## Data

`hgc44_3axes.fits` — HI cube, 158 × 700 × 700 (VELO-HEL, Jy/beam), beam
0.0147° × 0.0091°, Δv = 8.24 km/s.

The cube is **not shipped in the repo** (large). Point the scripts at it with an
environment variable so they run on any machine (they default to the author's
local path and exit with a clear message if it is missing):

```bash
export VISIVO_VALIDATION_CUBE=/path/to/hgc44_3axes.fits
export VISIVO_DEMODATA=/path/to/DemoData        # benchmark.py (multiple cubes)
export VISIVO_VALIDATION_FIELD=/path/to/CF4_velocity.fits   # velocity_field_demo.py
```

## 1. Reproduce the SoFiA-2 mask (Docker)

SoFiA-2 v2.7.0 via the official image, parameters in `sofia_hgc44.par`
(smooth+clip at 4σ over kernels 0/3/6/7/15, linker, reliability ≥ 0.9):

```bash
cp validation/sofia_hgc44.par validation/sofia_out/
docker run --rm --platform linux/amd64 --entrypoint sofia \
  -v "$(dirname "$VISIVO_VALIDATION_CUBE")":/data \
  -v "$PWD/validation/sofia_out:/out" \
  sofiapipeline/sofia2 /out/sofia_hgc44.par
# → validation/sofia_out/sofia_hgc44_mask.fits (18 sources)
```

This reproducible run matches the pre-existing `hgc44-run5_mask.fits` closely
(top source sizes 11463/8754/4995 vs 11420/8847/5085), i.e. the comparison does
not depend on the exact SoFiA parameters.

## 2. Run the comparison

```bash
backend/.venv/bin/python validation/lasso_vs_sofia.py
# optional: pass a different mask, e.g. the pre-existing run5 mask
backend/.venv/bin/python validation/lasso_vs_sofia.py \
  "$VISIVO_DEMODATA/hgc44-run5_mask.fits"
```

For each of the 5 largest SoFiA sources it seeds the lasso on the source's peak
voxel (= the click), grows with defaults (reach 8, λv 1, nσ 2.5), and writes
`out/metrics.{md,csv}` + `out/source_NN.png` (moment-0 with both mask contours +
total-spectrum overlay).

## Result (vs the reproducible mask)

| SoFiA id | Dice | IoU | Completeness | Reliability | F_Lasso/F_SoFiA |
|---|---|---|---|---|---|
| bright #1 | 0.844 | 0.731 | 0.909 | 0.788 | 1.046 |
| bright #2 | 0.826 | 0.704 | 0.917 | 0.752 | 1.088 |
| #3 | 0.737 | 0.584 | 0.772 | 0.705 | 1.120 |
| faint #4 | 0.660 | 0.492 | 0.587 | 0.753 | 0.790 |
| faint #5 | 0.687 | 0.523 | 0.604 | 0.797 | 0.938 |

**Headline (segmentation given a correct seed):** on the two main HI galaxies,
one click with default parameters overlaps > 91 % of the SoFiA mask
(completeness; Dice > 0.83) at reliability ≈ 0.75, with integrated flux within
8 %, and the total spectrum tracks SoFiA's double-horn profile channel-by-channel.
Completeness drops for the faintest sources because the lasso's nσ = 2.5 contour
is more conservative than SoFiA's multi-scale smooth+clip — the interactive
reach / nσ controls trade completeness against reliability (below).

**Caveats (paper honesty):** the seed is SoFiA's own peak voxel, so this is
segmentation agreement, not detection; it is one cube and five sources; and the
reported flux carries no uncertainty yet. A blind-detection claim is out of
scope for an assisted tool.

## Completeness–reliability tradeoff

`lasso_sensitivity.py` sweeps the two interactive controls on a bright and a
faint SoFiA source and reports BOTH completeness (∩/SoFiA) and reliability
(∩/lasso) at every nσ and reach (`out/sensitivity.png`), so a completeness gain
bought by flooding into noise (reliability collapse) is visible, not hidden:

```bash
backend/.venv/bin/python validation/lasso_sensitivity.py
```

The faint source, only 59 % complete at the default nσ = 2.5, rises toward the
bright source's completeness as nσ is lowered to 1.5 or *reach* is raised — **but
that completeness is bought with reliability**: lowering nσ floods the selection
into noise, so ∩/lasso (reliability) falls at the same time. The plot shows both
curves; the honest reading is a *tunable completeness–reliability tradeoff*, not
a free recovery. nσ = 2.5 is the default because it balances the two; a user who
needs more completeness on a faint source can trade reliability for it knowingly.
(Report the completeness AND reliability at the chosen setting in any figure.)

