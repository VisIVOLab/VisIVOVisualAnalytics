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
| `wallaby` | `WALLABY_J100903-290239_Hydra_TR2_cube.fits` (CASDA, cubelet-41) | 109 × 280 × 196 | RA---SIN / FREQ | 3.94 km/s | 5.0 px | different instrument (ASKAP), projection and spectral convention; one large velocity-gradient source plus faint compact ones |

Both are run through SoFiA-2 with the **same** detection parameters. That is the
point: if the reference finder were tuned per cube, the agreement we report would
partly be a property of the tuning.

The cubes are **not shipped in the repo** (large). Point the scripts at them with
environment variables so they run on any machine (they default to the author's
local paths and exit naming the missing variable):

```bash
export VISIVO_VALIDATION_CUBE=/path/to/hgc44_3axes.fits     # --dataset hgc44
export VISIVO_VALIDATION_CUBE_2=/path/to/WALLABY_J100903-290239_Hydra_TR2_cube.fits   # --dataset wallaby (CASDA)
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

### The second reference: the published mask

The SoFiA-2 run above is *our* reference. The paper's primary reference is the
mask the published science of HCG 44 was done with (Serra et al. 2013, provided
by the authors; distributed with the paper's supplementary material next to the
cube). It is binary, so label it once into the SoFiA-style id mask the
comparison reads, then pass it as the positional mask override; `--smooth Z,Y,X`
switches on the lasso's smooth-before-threshold option:

```bash
backend/.venv/bin/python validation/label_published_mask.py \
  /path/to/hcg44_cube_3.0_binmask.fits validation/sofia_out/serra2013_hcg44_mask.fits
backend/.venv/bin/python validation/lasso_vs_sofia.py validation/sofia_out/serra2013_hcg44_mask.fits
backend/.venv/bin/python validation/lasso_vs_sofia.py validation/sofia_out/serra2013_hcg44_mask.fits --smooth 1,3,3
# → out/metrics_ref-serra2013_hcg44{,_smooth1-3-3}.{md,csv}
```

Labelling is 6-connected in raster order (scipy default), so component ids are
deterministic. The pairing the paper's rows A–E rest on — published components
13, 1, 8, 3, 7 hold SoFiA-2 sources 5, 7, 12, 14, 8, each to ≥ 98 % of its
voxels — is computed, not asserted: `crosswalk_references.py` writes
`out/crosswalk_serra2013_sofia2.md` from the two masks. The published mask is 2–3× more
inclusive than our SoFiA-2 run (their mutual Dice is 0.50–0.66), which is the
scale any lasso-vs-reference number has to be read against.

## Result (vs the reproducible mask)

| SoFiA id | Dice | IoU | Completeness | Reliability | F_Lasso/F_SoFiA | Δflux (Jy·km/s) | noise-equiv. n_σ |
|---|---|---|---|---|---|---|---|
| bright #1 | 0.844 | 0.731 | 0.909 | 0.788 | 1.046 | +0.27 ± 0.02 | 15.1 |
| bright #2 | 0.826 | 0.704 | 0.917 | 0.752 | 1.088 | +0.25 ± 0.02 | 15.4 |
| #3 | 0.737 | 0.584 | 0.772 | 0.705 | 1.120 | +0.12 ± 0.02 | 7.0 |
| faint #4 | 0.660 | 0.492 | 0.587 | 0.753 | 0.790 | −0.18 ± 0.02 | 8.7 |
| faint #5 | 0.687 | 0.523 | 0.604 | 0.797 | 0.938 | −0.03 ± 0.02 | 1.9 |

**Headline (segmentation given a correct seed):** on the two main HI galaxies,
one click with default parameters overlaps > 91 % of the SoFiA mask
(completeness; Dice > 0.83) at reliability ≈ 0.75, with integrated flux within
8 % (a *segmentation* difference, not consistency within errors — see below), and
the total spectrum tracks SoFiA's double-horn profile channel-by-channel.
Completeness drops for the faintest sources because the lasso's nσ = 2.5 contour
is more conservative than SoFiA's multi-scale smooth+clip — the interactive
reach / nσ controls trade completeness against reliability (below).

### What the flux uncertainty says — and it is not what you would hope

The uncertainty is **empirically estimated**, not modelled. The mask's own stencil — and, for
the comparison, the SIGNED stencil (+1 where only the lasso selects, −1 where only
SoFiA does) — is dropped at up to 200 positions lying **fully** outside a dilated
SoFiA mask, and σ is the scatter of the resulting weighted sums. It is the standard
empty-aperture error, and it assumes those off-source placements represent the
noise and covariance at the source's own position and channel range. The mask is
dilated by 2 voxels, roughly one beam sigma on both cubes; varying that between 0
and 5 moves the measurable σ_Δ negligibly.

That was not the first approach. The obvious analytic form,
σ_F = rms·√(N·F/B)·Δv with a beam-correlation term and a measured
spectral-correlation penalty F = 1 + 2Σρ_k, needs three assumptions that do not
survive contact with these data:

- **the mask is many beams across** — the faint WALLABY masks are 32–159 voxels
  against a 28.5-pixel beam, i.e. a handful of beam elements;
- **one cube-wide spectral factor describes an irregular 3-D mask** — a finite sum
  needs lag weights (1 − k/N) and the mask's own channel overlap, and the measured
  ρ_k itself depends on the estimator (raw ρ sits near 0.04–0.05 out to lag 20 on
  HGC 44, which a per-pixel baseline removes past lag 3);
- **an unsigned voxel count describes the variance of a DIFFERENCE** — it does
  not. The intersection cancels in the *value*, but the variance is a covariance
  double sum over signed weights, and the two disagreeing sets sit on a shared
  boundary where their noise partly cancels too.

The `σ_Δ analytic` column shows how badly that matters. For every stencil whose
scatter can actually be measured, the model overestimates σ_Δ — by ~1.6–2.3×
on HGC 44 and ~1.3× on WALLABY — because it counts the disagreeing voxels as
independent contributors when in fact they lie on a shared mask boundary, where
the signed sum cancels much of their noise. The one stencil where the model could
have been optimistic instead, WALLABY #1, is exactly the one that cannot be
checked empirically. So the model is not "conservative": it is unvalidated in the
regime where it would matter most, which is why it is shown for comparison and
never used as the reported number. Ratios are quoted coarsely on purpose — with
200 placements the measured σ itself carries a ~5 % Monte-Carlo error.

**Result: HGC 44 differences are 1.9–15.4 noise-equivalent σ; the four measurable
WALLABY sources are 4.8–6.3.** So "integrated flux within 8 %" is true about the
magnitude and misleading about the cause: the residual is not measurement scatter
but a real difference in which voxels each method includes. Quote it as a
*segmentation* agreement.

**One source has no measurable uncertainty at all.** WALLABY #1 spans most of the
field, so there is nowhere in that cube to place its stencil fully on empty sky —
zero clean placements out of 1600 draws. Its flux difference (−17.6 Jy·km/s, a
13 % shortfall) is reported without a σ rather than backed by the analytic model,
which the other rows show overestimates σ_Δ by up to a factor of two — and which
is untested in exactly this regime.
Placements must be **fully** source-free for exactly this reason: an earlier
version accepted ≥ 98 % and then summed every finite voxel, letting real emission
into the "empty sky" sum and inflating the scatter by ~8 % on that same stencil —
larger than the ~5 % Monte-Carlo error of the estimate itself.

**What `n_σ` is not.** It is a fixed-mask, noise-only contrast — the scatter you
would get from dropping *these* stencils on empty sky. It is **not a p-value for
segmentation disagreement**, because both masks were selected using the same noisy
data they are then integrated over, so their boundary voxels carry a selection
(Eddington-type) bias this does not model. A formal test would need injected
sources or masks derived from independent data. The honest sentence is: *these
realized segmentations differ in measured flux by many noise-equivalent sigmas.*

The rms is estimated from emission-free voxels only, and local rms around each
source agrees with the global estimate within 2 % on both cubes.

**Caveats (paper honesty):** the seed is SoFiA's own peak voxel, so this is
segmentation agreement, not detection; five sources per cube. A blind-detection
claim is out of scope for an assisted tool. The peak-seed concern is addressed
head-on by the seed-robustness analysis below.

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
(WALLABY). What does *not* transfer is the default value of `reach`.

**`reach` cannot be defaulted from the header, and that is not a gap to close.**
The obvious follow-up — derive it from the beam and the channel width — was
tested and does not work: the two cubes have near-identical beams (6.6 vs 5.0 px)
while their best reach differs eightfold, and within a single cube the optimum
varies 4 → 16 between sources of similar extent. The reason is in the cost
function: a voxel costs `exp(nσ − I/σ)`, so traversing bright material is nearly
free and traversing near-threshold material costs ≈ 1 per step. The useful budget
therefore tracks the source's **surface brightness relative to the noise**, which
is neither in the header nor known before the click.

Two self-contained automatic rules were measured against the ground-truth optimum
over all 10 sources of both cubes, and both are *worse* than simply leaving the
default alone (mean Dice lost, lower is better):

| rule | mean Dice lost | worst |
|---|---|---|
| growth-curve knee (Kneedle on log N vs log reach) | 0.301 | 0.607 |
| stop when the added shell falls below nσ | 0.190 | 0.438 |
| **fixed default reach = 8** | **0.116** | 0.318 |

Retuning the fixed default does not help either: the per-cube optima are 8
(HGC 44, mean loss 0.022) and 32 (WALLABY, 0.018), and the global compromise of
16 is worse than each cube's own choice for that cube (0.078 and 0.111). One
number cannot serve both.

The honest conclusion is that `reach` is irreducibly an **interactive** control —
which is the premise of the tool, not an embarrassment: a human looking at the
rendering supplies exactly the surface-brightness judgement the header lacks.
What follows is a UI obligation rather than an algorithmic one, and it is now
implemented: `POST /v1/cube/kinematic_levels_bin` returns the geodesic **level
field** once per click, and the Reach slider thresholds it locally, so the
surface follows the drag with no backend call per step. The threshold is exact
rather than an approximation — levels are `ceil(distance)`, and for integer `r`,
`ceil(d) ≤ r ⟺ d ≤ r` — so what the slider shows is the same voxel set a grow at
that budget returns. One field also costs *less* than one old single-budget grow
(0.30 s vs 0.44 s on HGC 44), because it is bounded by the object instead of
allocating a cube-sized mask. The slider now runs to 64, since the old cap of 40
could not express the budgets this cube needs.

Published figures should still state the reach used.

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

## Figures of the paper

Every figure is rendered from the products, not screenshotted, and each script
prints the numbers it draws. All are sized for the journal's 17.5 cm text width.

| figure | script | needs |
|---|---|---|
| 1 storyboard | `figure1_panels_ngc3137.py`, then `figure1_storyboard.py --src validation/out/fig1 --out validation/out/figure1_storyboard.png --crop 0,0,1,1` | `VISIVO_VALIDATION_CUBE_2` |
| 2 architecture | `figure2_architecture.py` | nothing (a drawing) |
| 3 lasso on HCG 44 | `figure3_lasso_hgc44.py` | `VISIVO_VALIDATION_CUBE`, SoFiA mask |
| 4 tilted-ring model | `figure4_model_ngc3137.py` | `VISIVO_VALIDATION_CUBE_2` |
| 5 chain of products | `figure5_transitions.py` | `VISIVO_VALIDATION_CUBE_2`, the service's routers |
| 6 polarimetry | `figure6_polarimetry.py` (below) | the MeerKAT Bulge I/Q/U cubes of G000.0+2.5 and `Mosaic_Pol_Cat.csv` from doi:10.48479/f2a2-qw16 |
| 7 velocity field | `velocity_field_demo.py` | `VISIVO_VALIDATION_FIELD` |
| interface, Data Hub | `figure_interface.py` on the window captures in `shots/` (`screencapture -l` of the client's main window: lasso armed, lasso selected; nothing retouched) | the client, 1512×900 window, Retina |

Figures 4 and 5 call `usecase_ngc3137.run()`, so every number on them is the
one in `out/usecase_ngc3137.md`. Figure 6 is the 900-px cutout holding three
catalogued polarised sources; the script runs the service's own workers and
prints the catalogue-vs-ours rotation measures:

```bash
backend/.venv/bin/python validation/figure6_polarimetry.py \
  --dir /path/to/meerkat_bulge --field G000.0+2.5 \
  --centre "17h36m16.510s -27d49m09.54s" --size 900 --catalogue /path/to/Mosaic_Pol_Cat.csv
```
