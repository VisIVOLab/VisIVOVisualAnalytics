# Kinematic Lasso — scientific validation vs SoFiA-2

Reproducible comparison of the VisIVO **Kinematic Lasso** geodesic selection
against a **SoFiA-2** source mask on **two** real HI cubes — Hickson Compact
Group 44 and a WALLABY (ASKAP) field — for the software paper. The lasso is **assisted segmentation**, not blind
detection: given a seed *on* a source (here SoFiA's own peak voxel), one click
grows a selection, and we quantify how well that selection agrees with the SoFiA
mask (spatial overlap + integrated flux + total spectrum). This measures
segmentation agreement given a correct seed — NOT detection completeness or
reliability over a survey, which only a blind finder like SoFiA provides.

## Data

Two cubes, deliberately unlike each other — one cube with five sources cannot
separate a property of the method from a property of the observation (R-18):

| `--dataset` | file | shape | frame | Δv | beam | why this one |
|---|---|---|---|---|---|---|
| `hgc44` (default) | `hgc44_3axes.fits` | 158 × 700 × 700 | VELO-HEL | 8.24 km/s | 6.6 px | compact group: several galaxies of similar brightness in one field |
| `wallaby` | `WALLABY.fits` | 109 × 280 × 196 | RA---SIN / VOPT | 3.94 km/s | 5.0 px | different instrument (ASKAP), projection and spectral convention; one large velocity-gradient source plus faint compact ones |

Both are run through SoFiA-2 with the **same** detection parameters. That is the
point: if the reference finder were tuned per cube, the agreement we report would
partly be a property of the tuning.

The cubes are **not shipped in the repo** (large). Point the scripts at them with
environment variables so they run on any machine (they default to the author's
local paths and exit naming the missing variable):

```bash
export VISIVO_VALIDATION_CUBE=/path/to/hgc44_3axes.fits     # --dataset hgc44
export VISIVO_VALIDATION_CUBE_2=/path/to/WALLABY.fits       # --dataset wallaby
export VISIVO_DEMODATA=/path/to/DemoData        # benchmark.py (multiple cubes)
export VISIVO_VALIDATION_FIELD=/path/to/CF4_velocity.fits   # velocity_field_demo.py
```

Every lasso script takes `--dataset NAME`; outputs for the non-default cube are
suffixed (`metrics_wallaby.md`, …) so both sets coexist in `out/`.

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

# ...and the second cube, same parameters:
docker run --rm --platform linux/amd64 --entrypoint sofia \
  -v "$(dirname "$VISIVO_VALIDATION_CUBE_2")":/data \
  -v "$PWD/validation/sofia_out:/out" \
  sofiapipeline/sofia2 /out/sofia_wallaby.par
# → validation/sofia_out/sofia_wallaby_mask.fits (12 sources)
```

On the WALLABY cube SoFiA warns that only 13 negative detections are available
for the reliability kernel; it completes anyway and returns the same 12 sources
as with reliability disabled, so that filter is not doing the work there —
`filter.minSNR` is. Noise scaling was tried and left **off**: `spectral` mode
leaves the count at 12 with near-identical sizes, `local` mode gives 15 sources
and larger masks, and neither splits the large source #1 — which turned out to be
a genuine extended object rather than linked noise (its flux-weighted centroid
moves monotonically across 86 channels at peak SNR 10–32), so there was nothing
to fix. Enabling it would only make the reference mask differ from the HGC 44
run for no diagnosed reason.

This reproducible run matches the pre-existing `hgc44-run5_mask.fits` closely
(top source sizes 11463/8754/4995 vs 11420/8847/5085), i.e. the comparison does
not depend on the exact SoFiA parameters.

## 2. Run the comparison

```bash
backend/.venv/bin/python validation/lasso_vs_sofia.py                    # HGC 44
backend/.venv/bin/python validation/lasso_vs_sofia.py --dataset wallaby  # ASKAP
# optional: pass a different mask, e.g. the pre-existing run5 mask
backend/.venv/bin/python validation/lasso_vs_sofia.py \
  "$VISIVO_DEMODATA/hgc44-run5_mask.fits"
```

For each of the 5 largest SoFiA sources it seeds the lasso on the source's peak
voxel (= the click), grows with defaults (reach 8, λv 1, nσ 2.5), and writes
`out/metrics{,_wallaby}.{md,csv}` + per-source figures (moment-0 with both mask
contours + total-spectrum overlay). The `.md`/`.csv` tables are committed; the
figures and the SoFiA masks are regenerable and git-ignored.

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
scope for an assisted tool. The peak-seed concern is addressed head-on by the
seed-robustness analysis below.

## Second cube (WALLABY / ASKAP) — where the method holds and where it does not

```bash
backend/.venv/bin/python validation/lasso_vs_sofia.py --dataset wallaby
```

Same defaults (reach 8, λv 1, nσ 2.5), same peak-voxel seed, `out/metrics_wallaby.md`:

| SoFiA id | SoFiA vox | Dice | Completeness | Reliability | F_Lasso/F_SoFiA |
|---|---|---|---|---|---|
| #1 (large, velocity gradient) | 153 227 | 0.614 | 0.445 | **0.990** | 0.869 |
| #10 | 1 189 | 0.171 | 0.095 | 0.831 | 0.226 |
| #11 | 1 117 | 0.053 | 0.030 | 0.214 | 0.307 |
| #12 | 977 | 0.117 | 0.066 | 0.529 | 0.264 |
| #4 | 539 | 0.063 | 0.033 | 0.562 | 0.111 |

These numbers are much weaker than HGC 44's and they are reported as they came
out. Two distinct causes, and only one of them is a limitation of the method:

**1. `reach` is a voxel budget, not a physical scale.** The lasso grows a
geodesic ball of cost `reach` around the seed, so a single click cannot cover a
structure much longer than that budget. WALLABY #1 spans ~200 voxels; HGC 44's
galaxies are compact. Raising the control — which is what a user does
interactively — recovers most of it, with a well-defined optimum
(`out/sensitivity_wallaby.md`):

| reach | Dice | completeness | reliability |
|---|---|---|---|
| 4 | 0.545 | 0.375 | 0.998 |
| 8 *(default)* | 0.614 | 0.445 | 0.990 |
| 16 | 0.707 | 0.560 | 0.958 |
| **32** | **0.764** | 0.724 | 0.807 |
| 64 | 0.596 | 0.810 | 0.472 |

So on the largest source of each cube the method transfers across instrument,
projection and spectral convention: best Dice **0.86** (HGC 44) and **0.76**
(WALLABY). What does *not* transfer is the default value of `reach` — it should
be read as a user-facing scale control, and any figure should state the value
used. Making it default to a beam/channel-derived scale is the obvious follow-up.

**2. Sources detectable only after multi-scale smoothing are out of scope.**
WALLABY #4/#10/#11/#12 are ~10³-voxel sources that SoFiA finds by smoothing with
kernels up to 15 channels; individually their voxels sit below the lasso's nσ
contour. A voxel-level geodesic cannot follow sub-threshold emission, and no
setting of the controls fixes it. On #12 the best Dice over either sweep is 0.295
(nσ = 1.5), and it is reached by giving up as much reliability as it gains:
nσ 2.5 → 1.5 lifts completeness 0.066 → 0.277 while reliability falls
0.529 → 0.314. This is a
real boundary of the technique, not a tuning failure, and it is the honest
statement of the tool's regime: **the lasso is assisted segmentation of
structure that is visible at the voxel level; for smoothing-limited detections a
blind multi-scale finder like SoFiA remains necessary.**

Seed robustness degrades on this cube in the same way and for the same reason
(`out/seed_robustness_wallaby.md`): interior-click Dice CV is 0.36–0.51 against
HGC 44's 0.00–0.18, because a click in the faint outskirts of a 200-voxel-long
source reaches a different amount of it. Click-invariance is a property of
compact, well-connected sources, not of the method in general.

## Completeness–reliability tradeoff

`lasso_sensitivity.py` sweeps the two interactive controls on a bright and a
faint SoFiA source and reports BOTH completeness (∩/SoFiA) and reliability
(∩/lasso) at every nσ and reach (`out/sensitivity.png`), so a completeness gain
bought by flooding into noise (reliability collapse) is visible, not hidden:

```bash
backend/.venv/bin/python validation/lasso_sensitivity.py [--dataset wallaby]
```

It writes `out/sensitivity{,_wallaby}.{md,csv}` as well as the figure, including
a *best-Dice per source and axis* table — so a number quoted in the paper is
traceable to a committed artifact rather than eyeballed off a curve.

The faint source, only 59 % complete at the default nσ = 2.5, rises toward the
bright source's completeness as nσ is lowered to 1.5 or *reach* is raised — **but
that completeness is bought with reliability**: lowering nσ floods the selection
into noise, so ∩/lasso (reliability) falls at the same time. The plot shows both
curves; the honest reading is a *tunable completeness–reliability tradeoff*, not
a free recovery. nσ = 2.5 is the default because it balances the two; a user who
needs more completeness on a faint source can trade reliability for it knowingly.
(Report the completeness AND reliability at the chosen setting in any figure.)

## Seed (click) robustness

The vs-SoFiA table seeds each lasso on the SoFiA source's *peak* voxel, so a
sceptic could argue the agreement is an artifact of clicking exactly where SoFiA
is most confident. `lasso_seed_robustness.py` perturbs the click and reports how
much the metrics move, in two separately-reported strata:

- **Interior** (the plausible *correct* click — peak + spatial jitter + random
  bright interior voxels): drives the headline.
- **Stress** (a harder test — low-intensity faint interior voxels + a one-voxel
  dilation ring just outside the source): reported apart.

```bash
backend/.venv/bin/python validation/lasso_seed_robustness.py
```

For the bright, well-connected sources the interior result is essentially
**click-invariant** (Dice std ≈ 0): the object-bounded grow takes the connected
component *containing the seed*, which is the same component from any interior
voxel, so the selection does not depend on hitting the peak. The two faint
sources show genuine placement sensitivity (interior Dice std up to ≈ 0.11, a
marginal interior click can drop Dice to ≈ 0.38), so for weakly-detected objects
the result depends more on where the user clicks — reported honestly, not hidden.
The stress stratum degrades sharply, as expected for a faint / off-source click.
Output: `out/seed_robustness{,_wallaby}.{md,csv,png}`. Run on both cubes it says
something sharper than either alone: click-invariance holds for compact,
well-connected sources (HGC 44 interior Dice CV 0.00–0.18) and degrades on a
large, low-surface-brightness one (WALLABY CV 0.36–0.51). It is a property of the
source, not a blanket property of the method — which is why the second cube was
needed to state it at all.

