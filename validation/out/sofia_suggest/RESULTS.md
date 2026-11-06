# Issue #64: SoFiA-2 suggested parameters, validation results (local part)

Run 2026-11-05 under the protocol in the #64 spec, §3 (rev. 2026-11-05). The criteria were frozen before the first run and have not been changed. Driver: `validation/sofia_suggest.py` (stages `run`, `measure`, `criteria`). Raw outputs: `validation/out/sofia_suggest/<cube>/<arm>/` holds `sofia.par`, `sofia.log`, `sofia_out_cat.xml` and `result.json` for each run, plus `suggest_{catalogue,flux}.json`, `measures.json` and `criteria.json`. The mask cubes (about 300 MB each on HCG 44) were left in the backend's exports directory. They are not in the repository; their paths are in each `result.json`.

## Frozen state

| item | value |
|---|---|
| base commit | 8303e71 (main, Release 1.0.9) |
| worktree diff, tracked + untracked (`(git diff HEAD; cat untracked) \| shasum`) | d4bdafe2b75a8f4849bbd65f798e6f7b8393c683 |
| `git stash create` (tracked files only) | 61ff2b9d903010f3c8d8fe7017478318f0d47110 |
| `backend/app/compute/sofia_suggest.py` sha1 | 9cf8505eab4c57d2deeb8b3013db0bc1a224b3bc |
| `backend/app/compute/finders/sofia2.py` sha1 | 17c3245ccebb7e7bbc9b93c7597ad6cae24a4b21 |
| `backend/app/compute/kinematic_fit.py` sha1 | f2c989ff4f0fe5e2c9914bc1efcc3ad76508e28b |
| SoFiA-2 | 2.7.0, `sofiapipeline/sofia2:latest` (15ca83d8079f), via `VISIVO_SOFIA_BIN` docker wrapper |

## How every number was produced (paper-shows-the-app)

- Suggestions: `POST /v1/sofia2/suggest/{id}` on a backend started from the worktree (port 18764). The arms use the `params` the route returned, unedited.
- Runs: `POST /v1/tasks/sourcefind`. The app's finder plugin wrote every .par.
- Flux and w20 on every mask, including the references: `app.compute.kinematic_fit.source_measures`, with `cube_velocity_axis` and the beam area loaded as the kinematic-fit route loads them. The reference rows reproduce the spec's reading scale exactly: Dice 0.572/0.625/0.500/0.602/0.660 and flux R_pub/R_sof 1.05/1.11/1.31/1.26/1.32.
- H run through the app reproduces the stored reference masks voxel for voxel. On HCG 44 and WALLABY, Dice(H, R_sof) = 1.000 for every source. The WALLABY H run used the CASDA FREQ cubelet; R_sof was made on the VOPT-relabelled copy, which holds the same pixel data.

Interpretations fixed in the driver before the measurements were read:
- (a) A reference source matches the union of all arm sources that overlap it by at least 50 % of the smaller of the two.
- (b) Inversion purity is computed like for like: 1 − N_inv/N_pos, where N_pos is the positive run with the same parameters and reliability off, as in the inverted run.
- (c) "Unmatched H" is defined like "unmatched S": H sources matched neither in S nor in R_pub.
- (d) The w20 criterion has no row count in the spec, so all 5 rows are required.

One addition after the runs, which is a fact and not tuning: SoFiA-2 exit code 8 is "No sources left after filtering" (verified on HCG 44 D_inv by running the same .par directly), so it is counted as N_inv = 0. The "merged" counts are also post hoc and are supplementary only.

## Suggested parameters (as shown to the user)

| | HCG 44 (`hgc44_3axes.fits`) | WALLABY NGC 3137 (CASDA cubelet) | Apertif (held out) |
|---|---|---|---|
| measured | σ 2.246e-4 Jy/beam, b_px 5.20, ρ 0.740, Δv_rest 8.20 km/s, N_res 1.26e6, r_chan 1.08, r_sky 1.04 | σ 1.97e-3, b_px 5.01, ρ 0.013, Δv 3.92, N_res 2.1e5, r_chan 1.08, r_sky 1.11 | σ 1.85e-3, **no BMAJ/BMIN in header**, ρ 0.019, Δv 8.35, r_chan 1.18 (floor 1.04), data type "emission, ambiguous (low)" |
| kernelsXY | 0, 5, 10 | 0, 5, 10 | 0, 3, 6 (no beam → default) |
| kernelsZ | 0, 3, 7, 15 | 0, 3, 7, 15 | 0, 3, 7, 15 |
| threshold / reliability | 4.0 / on (caveat) | 5.0 / off | 5.0 / off (no beam → N_res unknown) |
| scaleNoise | off | off | on, spectral |
| linker radius / minSize | 2,2 / 5,5 | 2,2 / 5,5 | 1,1 / 5,3 |
| time of the suggest call | 0.43 s | 2.4 s | 0.93 s |

Every parameter in which S differs from H on HCG 44 is a default SoFiA already uses, except kernelsXY (0, 5, 10 vs 0, 3, 6). **So every HCG 44 difference between S and H comes from kernelsXY.** On WALLABY, S differs from H in kernelsXY, threshold (5 vs 4) and reliability (off vs on).

## HCG 44: per-criterion results (arm S)

| row | Dice(S,R_pub) vs Dice(R_sof,R_pub)−0.05 | Dice(S,R_sof) ≥ 0.50 | c1 | F(S) Jy km/s in [0.9 F_sof, 1.1 F_pub] | c2 | w20(S) − w20(R_sof / R_pub), km/s | c3 (≤ 2 ch = 16.49) |
|---|---|---|---|---|---|---|---|
| A NGC 3187 | 0.579 vs 0.522 | **0.482** | fail | **9.31** in [5.22, 6.70] | fail | +362.7 / +362.7 | fail |
| B NGC 3190 | **0.522** vs 0.575 | **0.390** | fail | **9.31** in [2.59, 3.51] | fail | +148.4 / +131.9 | fail |
| C | 0.761 vs 0.450 | 0.506 | pass | **1.409** in [0.871, 1.391] | fail | +8.2 / 0.0 | pass |
| D | 0.804 vs 0.552 | 0.638 | pass | 1.097 in [0.769, 1.189] | pass | 0.0 / −8.2 | pass |
| E | 0.781 vs 0.610 | 0.583 | pass | **0.825** in [0.503, 0.812] | fail | +24.7 / 0.0 | pass |

| criterion | needed | S | result |
|---|---|---|---|
| c1 Dice | ≥ 4/5 rows | 3/5 | **FAIL** |
| c2 flux | ≥ 4/5 rows | 1/5 | **FAIL** |
| c3 w20 | 5/5 rows | 3/5 | **FAIL** |
| c4 completeness vs H (16 H sources with SNR ≥ 5) | ≥ 0.9 | 0.8125 (13/16; missed H 4, 6, 10) | **FAIL** |
| c5 inversion purity S ≥ H, or unmatched S ≤ unmatched H | either | purity 0.50 (28/56) vs H 0.62 (18/47); unmatched 1 vs 1 | pass (on the second clause) |
| **HCG 44** | all | | **FAIL** |

Why it fails (diagnosed, not tuned):
- **Merging.** The 10-px (2-beam) spatial kernel links NGC 3187 and NGC 3190 (rows A and B) into one source of 36 127 voxels, flux 9.31 Jy km/s and w20 610 km/s. It also links H sources 13, 15 and 17 into one. Counting only one-to-one matches, completeness is 8/16 (supplementary number).
- **Reliability.** H 4, 6 and 10 are detected by S before the reliability filter (S_roff: SNR 6.9, 7.9, 6.8). The filter then removes them. With the larger kernel, the reliability filter ran on 54 negatives / 75 positives, against 33/53 in H.
- **Larger masks.** On rows C, D and E, the S masks agree with the published mask better than our SoFiA-2 reference does: Dice 0.76–0.80 vs 0.50–0.66. Their fluxes on C and E exceed 1.1 × F(R_pub) by 1.3 % and 1.6 %. This is an observation, not a pass.

Comparison of S with D (SoFiA defaults) on HCG 44:
- S is better on Dice vs R_pub (all 5 rows), on c1 (3 rows vs 0) and on completeness (0.81 vs 0.69).
- S is worse on Dice vs R_sof (all 5 rows), on c2 (1 row vs 2), c3 (3 rows vs 4), reliability vs H∪R_pub (0.92 vs 1.00) and inversion purity (0.50 vs 1.00; D_inv has 0 detections).
- **"S no worse than D on every metric": FAIL.**

## WALLABY NGC 3137

| criterion | S | D | H | S4.5 |
|---|---|---|---|---|
| disc Dice vs R_sof ≥ 0.8 | **0.873 pass** | 0.887 | 1.000 | 0.834 |
| disc flux (Jy km/s) within 10 % of 142.2 and of F(R_sof) 133.8 | **135.6 pass** (−4.6 % / +1.3 %) | 129.8 (−8.7 % / −3.0 %) | 133.8 (−5.9 % / 0) | 137.0 (−3.7 % / +2.4 %) |
| disc w20 within 2 ch (7.87 km/s) of R_sof 259.9 | **259.9 pass** (0.0; −4.0 vs release 263.9) | 263.8 (+3.9) | 259.9 | 259.9 |
| disc n_pix (release 299 562; R_sof 153 227) | 195 023 | 122 050 | 153 227 | 212 667 |
| inversion purity | 1.0 (0/5) | 1.0 (0/1) | 1.0 (0/12) | 1.0 (0/10) |
| **WALLABY disc** | **PASS** | pass | pass | pass |

Faint sources (pre-declared failure case, no pass/fail). Each cell is the Dice vs R_sof; "–" means not detected.

| R_sof id (voxels) | S (5σ) | S4.5 | H (4σ + rel) | D (5σ, 0,3,6) |
|---|---|---|---|---|
| 10 (1189) | 0.75 | 0.68 | 1 | – |
| 11 (1117) | 0.63 | 0.40 | 1 | – |
| 12 (977) | 0.72 | 0.62 | 1 | – |
| 4 (539) | – | 0.65 | 1 | – |
| 5 (397) | 0.66 | 0.80 | 1 | – |
| 2, 8, 9 (462, 183, 112) | – | 0.57, 0.78, 0.71 | 1 | – |
| 3, 6, 7 (343, 319, 198) | – (6 is swallowed by the disc) | – (6, 7 swallowed by the disc) | 1 | – |
| completeness vs R_sof | 6/12 (5 distinct) | 11/12 (9 distinct) | 12/12 | 1/12 |

At 5σ without reliability, S loses source 4 (539 voxels) of the four faint compact sources, plus the six smallest sources. At 4.5σ, only 3 of the 12 sources are lost: 3 is missed, and 6 and 7 are swallowed by the disc. S4.5 also adds one source (859 voxels, SNR_max 5.7) that is not in R_sof. SoFiA defaults keep the disc only. Comparison of S with D on WALLABY: S has lower Dice (0.873 vs 0.887), better flux and much higher completeness. Elsewhere they are equal.

The release mask (CASDA cubelet-42, `WALLABY_J100903-290239_Hydra_TR2_mask.fits`) could not be downloaded: the CASDA datalink answers HTTP 401 anonymously and needs an OPAL login. The comparison with the release therefore uses only the catalogue values (n_pix 299 562; f_sum 673 807.5 Jy Hz = 142.2 Jy km/s with c/ν0, 143.3 with the optical c·ν0/ν²; w20 1.2455 MHz = 263.9 km/s). Once the mask is downloaded, `measure` can score against it with one more entry in `CUBES["wallaby"]["refs"]`.

## Global verdict

**FAIL.** HCG 44 fails 4 of 5 criteria and S is worse than D on several metrics. The WALLABY disc passes. No rule was changed. A change to kernelsXY (the only cause on HCG 44) would be a rule change after seeing results, and §3.0 says it must be re-tested on held-out cubes (Apertif, SDC2).

## Held-out: Apertif (3D_Ape-DR1_200426041, 661×661×1218, FREQ-OBS)

- **Header.** It has no BMAJ/BMIN (SoFiA-2 warns too). The suggester falls back to defaults for kernelsXY, linker and reliability, and says why (`no_beam` warning). The data type is "emission, ambiguous (low)": only 6 voxels > +5σ in the sample. The suggester enables scaleNoise spectral because r_chan = 1.18 against a floor of 1.04.
- **Noise vs radius** (negative-side σ, 24 channels): 1.852 → 1.865 mJy/beam from the centre to the corners (ratio 1.007). **The noise is flat across the field, so the cube is not primary-beam corrected.** Per-channel σ has isolated jumps: channel 10 is 3.59 mJy/beam and channel 800 is 3.00, against about 1.8–2.0 elsewhere.
- **Inversion test:**

| arm | N_pos | N_inv | purity |
|---|---|---|---|
| S | 46 | 26 | 0.43 |
| D | 116 | 109 | 0.06 |

  S is far purer than D on this held-out cube. The likely reason is scaleNoise spectral: 92 of D's 116 detections, and 102 of D_inv's 109, lie in channels z < 150.
- **What S finds is not mostly galaxies. The user's visual check is required.** 32 of the 46 S detections lie within 30 px of one position (x≈525, y≈128; RA 167.87°, Dec +55.67°) and are spread over channels 105–1181. That pattern is typical of a continuum residual, not of HI. The continuum test does not fire, because only a few pixels are affected (cont_fraction 5.7e-5 against the 1 % rule). This has not been checked against a continuum image ("not known"). Another 14 S detections are at z < 150, and 24 of the 26 inverted detections are there too.
- **Top 10 S detections for the visual check** (SoFiA id, x, y, z, SNR):

  | id | x | y | z | SNR |
  |---|---|---|---|---|
  | 3 | 517 | 125 | 1170 | 24.9 |
  | 35 | 494 | 133 | 105 | 20.0 |
  | 39 | 67 | 233 | 88 | 15.0 |
  | 36 | 386 | 651 | 109 | 14.8 |
  | 9 | 525 | 127 | 1040 | 14.7 |
  | 2 | 528 | 136 | 1182 | 13.0 |
  | 32 | 254 | 241 | 278 | 12.6 |
  | 42 | 255 | 244 | 10 | 11.9 |
  | 7 | 537 | 121 | 1053 | 11.8 |
  | 16 | 522 | 130 | 761 | 11.7 |

  Six of the ten are at the x≈525, y≈128 position; ids 32 and 42 are one position (x≈255, y≈242) at z 278 and z 10. Full rows are in `apertif/measures.json`.

## Findings for the engineers (not fixed here)

1. **`backend/app/compute/finders/sofia2.py`, `run()` (the `if code != 0` branch, the line with `err[:500]`).**
   - SoFiA-2 exit 8 ("No sources left after filtering") becomes a failed task, and the output directory is deleted. Expected: a completed task with `n_sources: 0`, the log kept and the summary given. Zero detections is a valid result, and the inversion test depends on it.
   - The error keeps the first 500 characters of stderr, which on HCG 44 are only SoFiA's CELLSCAL warning box, so the cause is hidden. It should keep the `ERROR:` line.
   - Repro: open `hgc44_3axes.fits`, then `POST /v1/tasks/sourcefind {"params": {"input.invert": "true", "reliability.enable": "false"}}`. The task fails and the error text is the warning box only.
2. **The "flux" goal (dilation) does almost nothing on both cubes.**
   - HCG 44 S+dil adds 0 voxels (95 849 = 95 849).
   - WALLABY S+dil adds 518 voxels to one 197-voxel source, and the disc is unchanged.
   - SoFiA-2 runs both dilation passes and stops, because the flux increment is below `dilation.threshold` 0.001. The masks from the 0, 5, 10 kernels already reach past the emission.
   - The reason string ("dilation adds the faint outskirts for total flux") is not supported on these cubes.
3. Apertif DR1 cubes carry no beam keywords. The suggester handles this correctly, but N_res, and so the reliability decision, cannot be computed. A user-supplied beam field would be needed. That is a product decision, not a bug.

## What the docs and paper may claim now (spec §4, given the FAIL)

May claim:
- "The app proposes SoFiA-2 parameters from the measured noise, beam, channel width and noise correlation, with a stated reason for each; the user edits them before the run."
- "On NGC 3137 (WALLABY) the suggested parameters recover the main disc: Dice 0.87 against our SoFiA-2 reference, flux 135.6 vs 142.2 Jy km/s in the release catalogue (−4.6 %), w20 4.0 km/s (one channel) from the release value. At the suggested 5σ without reliability, the faint companion of 539 voxels and the six smallest sources are lost; 4.5σ recovers all but three."
- "On a held-out Apertif cube, the inversion test gives a purity of 0.43 for the suggested parameters against 0.06 for SoFiA's defaults, driven by the spectral noise scaling", stated together with the caveat that most of the positive detections cluster at one position (not yet inspected).
- "With the suggestion, SoFiA's log line 'reliability ran on N negatives' is shown after the run" (a measured statement for each run).

Must not claim:
- That the suggested masks agree with the HCG 44 references within the inter-reference scatter. They do not: c1 3/5, c2 1/5, c3 3/5.
- That the suggestion is "comparable to hand-tuned" or "better than defaults" in general. On HCG 44, S is worse than D on Dice vs R_sof, flux rows, w20 rows, reliability and purity.
- "Optimal" or "expert-equivalent", the generality of the N_res cut-offs, or any SDC2 number (not run).
- That the flux-goal dilation recovers extra flux (finding 2).

## SDC2 ldev: setup as prepared on 2026-11-05 (run 2026-11-05/06; results in "SDC2 ldev: result" below)

Files:
- `validation/sdc2/sofia_suggest_sdc2.py` (`run` / `score`). It uses the same client as the local driver.
- `validation/sdc2/run_sdc2_pleiadi.sbatch`. **Not submitted.**

What it does:
- It asks for one suggestion on the whole 44 GB cube. Arms S, D and H (the HCG 44 .par detection keys, unchanged) then run through the task route on four 400×400 px sub-cubes, two in the low-frequency half of the band and two in the high half (`input.region`, about 2.1 GB each). The regions are fixed in the script before any look at the truth catalogue.
- Each catalogue is converted to the SDC2 submission columns and scored with the challenge's own scorer (`ska_sdc.Sdc2Scorer`, PyPI `ska-sdc`) against the truth catalogue cut to each sub-cube with the cube's WCS.
- It reports n_match, n_false, completeness, reliability and score.

To do before launching:
1. PLEIADI GPU nodes are POWER9 (ppc64le) and the `sofiapipeline/sofia2` image is amd64. SoFiA-2 2.7.0 must be built from source (gcc + OpenMP + WCSLIB) into `/scratch/fvitello/skava-e2e/issue64/sofia2/`.
2. Put `sky_ldev_truthcat_v2.txt` from the SDC2 data release in `…/issue64/truth/`. It is not on PLEIADI, and I have not verified the download link.
3. `pip install --target …/issue64/pylib ska-sdc`, and check the `Sdc2Scorer.from_txt(...).run()` API and result fields (`value`, `n_match`, `n_false`) against the installed version.
4. Rsync the worktree to `…/issue64/VisIVO-next-64`.
5. Check `ls -ldn /scratch` on the node (the shared mount was lost on 2026-11-02).
6. `sbatch validation/sdc2/run_sdc2_pleiadi.sbatch`.

Points to check before quoting a score:
- The conversions in the submission: hi_size = 2 × ell_maj × pixel scale, inclination from the axis ratio with q0 = 0.2, and PA = SoFiA kin_pa, which points to the approaching side on frequency cubes. These need checking against Hartley+2023 §2, because they affect the score more than the match count.
- Expected rule outputs (spec §3.3): scaleNoise spectral (r_chan ≈ 1.18), kernelsXY 0, 3, 5, kernelsZ 0, 3, 7, 15, reliability on at 3.5σ, and the continuum test should fire (residuals to −1 mJy).

## Addendum, same day: decisions after the FAIL (spec §7, rev. 2026-11-05b)

Fabio decided three things:
- revise kernelsXY;
- remove the "flux" goal (dilation) from v1;
- validate the revised rules only on SDC2 ldev.

HCG 44 stays reported as above. The frozen rules and criteria are in spec §7: R2 (kernelsXY), C2 (wide-band contsub) and the SDC2 criteria P1–P4. On HCG 44, WALLABY and Apertif the revised rules give the following. These are descriptive notes, not validation, because these cubes were used to write the rules.

| cube | R2 kernelsXY | C2 contsub |
|---|---|---|
| HCG 44 | 0, 3, 6 | off |
| WALLABY | 0, 3, 6 | off |
| Apertif | 0, 3, 6 (no beam) | on |

On HCG 44, S2 equals H in every key.

**Apertif cluster at x≈525, y≈128.** Measured with the app's `worker_spectral_aperture` and `worker_moment`. Script: `scratchpad/v64/ap/` (`ap_cluster.py`, `m0.py`, `line.py`).
- **Continuum residual.** An aperture of r = 8 px has a median of +3.5e-4 Jy/beam over the whole band (each third positive). Control apertures read ≈ 0.
- **The largest M0 blob of the field.** In the unmasked full-band M0, this position holds the largest positive blob of the whole field: 238 px above 5σ, peak 9.5σ, no line shape.
- **One possible real HI source on top: SoFiA id 3.** After removing the offset, only channel 1169 stands out, at 5.5σ (boxcar of 15 channels). That is SoFiA id 3 (SNR 24.9, cz ≈ 19 100 km/s).
- **The test does not catch it.** The global continuum test misses it (0.05 % of pixels against the 1 % trigger). C2 would switch on contsub order 0 on this cube.
- **Not validated.** C2 is validated only on SDC2 (arm S2nocont, criterion P4).

**SDC2 driver updated** (`validation/sdc2/`).
- Arms: S2, S1 (old rule), H, D, S2nocont, S2_inv and D_inv.
- It refuses to run until `sofia_suggest.py` implements §7.
- After scoring it writes `sdc2_criteria.json` with P1–P4.
- Not submitted.

## SDC2 ldev: result (job 1074, PLEIADI-GPU r33c1s03, 2026-11-05/06)

**Verdict under the frozen §7.4 criteria: SDC2 PASS.** P1, P2 and P3 hold in 4 of 4 scored regions. P4 holds in 3 of 4: it fails on q3_lo, which still meets the 3-of-4 rule. No rule, criterion or region was changed. Read this verdict together with the caveats below. Absolute completeness is low for every arm, and the comparisons rest on small numbers of sources.

### What ran
- **Data.** `/mnt/beegfs/fvitello/demo_data/sky_ldev_v2.fits`, read in place: 1286×1286×6668, 44 110 059 840 B. The size matches the official listing, whose md5 is `d1588196b47c6d7fcbe5da99631bfa77`. The md5 was not recomputed on PLEIADI.
- **Truth.** `sky_ldev_truthcat_v2.txt`, 11 091 sources, sha256 `dd5ac6c28d95401995cb9a87afffa4ba625669c391009ea843422a6fff4e9fab`, md5 `18f80ff94f63bb27b79c05811da39cde`. It comes from the SDC2 data page (sdc2.skao.int/sdc2-challenge/data, development dataset), through the Dropbox folder `scl/fo/e847a6pnjtqk7xmxz6flt`, file link `.../AIo4631BbKfwhW2NGhvtbkw/sky_ldev_truthcat_v2.txt?rlkey=84kkeaw021ajh7t9lqur2n7p8&dl=1`.
- **Scorer.** `ska_sdc.Sdc2Scorer` from ska-sdc 2.0.0 (PyPI). It ran in a conda-forge env: scikit-learn 1.9.1, astropy 7.2.0, pandas 2.2.3, numpy 1.26.4, setuptools 80.10.2. The backend image has no scikit-learn, PyPI has no ppc64le wheel for it, and ska-sdc imports `pkg_resources`.
- **SoFiA-2.** 2.7.0 (2026-04-15), built from the GitLab v2.7.0 source inside `backend-conda-gpu.sif`: gcc 13, OpenMP, WCSLIB 8.10 linked statically. Binary sha256 `dd317675…f14e`. This is not the amd64 Docker image used for HCG 44, WALLABY and Apertif, because the Docker image does not run on ppc64le. The official image builds the same source with `make OMP=true` against Ubuntu's WCSLIB 8.2.
- **Code that ran** (sha1, identical locally and on the cluster):

  | file | sha1 |
  |---|---|
  | `sofia_suggest.py` | 72ee9356… |
  | `finders/sofia2.py` | 0a835b25… |
  | `sofia_suggest_sdc2.py` | 8d70e2c4… |

  The backend runs the worktree code in the image, started by `validation/sdc2/run_sdc2_pleiadi.sbatch`.
- **Outputs.** In `validation/out/sofia_suggest/sdc2/`:
  - `suggest_catalogue.json`, `arms.json`, `sdc2_scores.json`, `sdc2_criteria.json`;
  - the job log; `backend.log`, with the token line removed;
  - per region and arm: `result.json`, `sofia.par`, `sofia_out_cat.xml`, `submission.txt`, the tail of `sofia.log`, and `truth_region.txt`.

  The masks stay on PLEIADI in `/scratch/fvitello/skava-e2e/issue64/exports/`.

### Driver changes made before any SDC2 result was seen
These were made on 2026-11-05, after reading the installed scorer's code and before the job was submitted. No SDC2 number existed when they were made.
1. **`Sdc2Scorer.from_txt` skiprows, 1 → 0.** ska-sdc 2.0.0 reads the first unskipped line as the column names. With 1, the scorer refused every file (InvalidCatalogueException in a self-test), so the prepared driver could not have scored anything.
2. **w20 conversion in the submission: `c·f0·w20[Hz]/ν²` instead of `c·w20[Hz]/ν`.** This is the inverse of the scorer's own conversion (`ScaleAndCalculateLargestSize`: `spectral_size = w20/(c·f0/ν²)`). The old form was smaller by (1+z) ≈ 1.24–1.5 in that convention. The scorer uses the submission's w20 to set its candidate search radius, so this affects matching. It applies identically to every arm, and it changes a measurement adapter, not a rule or a criterion. Accepted by Fabio Vitello on 2026-11-06. The verdict above uses it.
3. A zero-detection run scores as zero instead of crashing.
4. Inversion counts are written to `sdc2_scores.json`.

Unchanged and still approximate: hi_size = 2 × `ell_maj` × pixel, where `ell_maj` is SoFiA's 2σ semi-axis; inclination from the axis ratio with q0 = 0.2; PA = `kin_pa`. These enter the per-source score and the multi-dimensional match error, not the candidate search radius.

### The suggestion on the real cube
`POST /v1/sofia2/suggest` on the whole 44 GB cube took 9.9 s: one strided pass over 3.93 M sampled voxels, 96 channel pairs. The driver checked that kernelsXY and contsub equal §7.1/§7.2 computed from the route's own measurements.

| measurement | value | §3.3 / §7 expectation |
|---|---|---|
| σ (negative side) | 2.884e-5 Jy/beam | |
| b_px (7" / 2.8") | 2.50000006 | 2.5 |
| Ω_b | 7.08 px | |
| Δv_rest | 8.565 km/s (c·Δν/ν at band centre) | ≈ 8.6 |
| band | 57 114 km/s | |
| ρ | −0.0086 | |
| l_z | 1 | |
| r_chan | 1.154 (noise floor 1.039) → scaleNoise spectral | ≈ 1.18, spectral |
| r_sky | 1.017 (floor 1.023) → no local scaling | |
| N_res | 1.56e9 | ≫ 1e7 |
| cont_fraction | 4.9e-5 → the old 1 % test does **not** fire | "should fire": it did not |
| tails | 159 voxels > +5σ, 165 < −5σ (1.1 expected from noise) | |

Suggested parameters (S2): kernelsXY **0, 2, 3**; kernelsZ 0, 3, 7, 15; threshold 3.5, reliability on (0.9); linker radiusXY 1, radiusZ 1, minSizeXY 3, minSizeZ 3; filter minSNR 3; **contsub on, order 0, from C2** (the band is ≥ 20 × 120 km/s).

Things to note about the suggestion:
- **The data type is "emission, ambiguous, low confidence"**, because both tails carry signal. The protocol takes the default (emission) unedited; in the GUI the user would be asked to choose.
- **The suggestion's own `input.region` (114–1170 in x and y, all channels)** comes from the memory warning: SoFiA needs ~102 GB and the job had 64 GB. The driver replaces it with each fixed region box.
- **S1 is 0, 3, 5, 8, not the 0, 2, 5, 8 §7.1 predicted.** The measured b_px is 2.50000006, so Python's `round` gives 3. S1 follows the frozen formula on the measured value.
- **SoFiA-2 warns "Spatial kernel sizes of < 3 cannot be accurately modelled"** on every run with the 2 px kernel (S2, S2nocont, S2_inv).

### Scores per region and arm
n_match, n_false and the SDC2 score come from the scorer. n_det is the number of rows the driver submitted. C = n_match/n_truth and R = n_match/n_det are computed by the driver; this R is the one the frozen criteria use. Every region has n_truth ≥ 20, so all four are scored.

| region | arm | n_truth | n_det | n_match | n_false | C | R | SDC2 score |
|---|---|---|---|---|---|---|---|---|
| q1_lo | S2 | 482 | 9 | 9 | 0 | 0.019 | 1.000 | 6.3 |
| q1_lo | S1 | 482 | 2 | 2 | 0 | 0.004 | 1.000 | 1.1 |
| q1_lo | H | 482 | 9 | 8 | 1 | 0.017 | 0.889 | 4.3 |
| q1_lo | D | 482 | 351 | 4 | 345 | 0.008 | 0.011 | −342.3 |
| q1_lo | S2nocont | 482 | 8 | 8 | 0 | 0.017 | 1.000 | 5.6 |
| q2_hi | S2 | 424 | 41 | 37 | 4 | 0.087 | 0.902 | 22.0 |
| q2_hi | S1 | 424 | 24 | 20 | 4 | 0.047 | 0.833 | 8.2 |
| q2_hi | H | 424 | 46 | 37 | 8 | 0.087 | 0.804 | 17.6 |
| q2_hi | D | 424 | 37 | 31 | 5 | 0.073 | 0.838 | 15.7 |
| q2_hi | S2nocont | 424 | 46 | 38 | 8 | 0.090 | 0.826 | 19.0 |
| q3_lo | S2 | 600 | 20 | 19 | 1 | 0.032 | 0.950 | 12.2 |
| q3_lo | S1 | 600 | 10 | 10 | 0 | 0.017 | 1.000 | 5.7 |
| q3_lo | H | 600 | 15 | 15 | 0 | 0.025 | 1.000 | 10.5 |
| q3_lo | D | 600 | 332 | 9 | 315 | 0.015 | 0.027 | −308.7 |
| q3_lo | S2nocont | 600 | 20 | 20 | 0 | 0.033 | 1.000 | 13.9 |
| q4_hi | S2 | 481 | 54 | 47 | 6 | 0.098 | 0.870 | 28.0 |
| q4_hi | S1 | 481 | 32 | 29 | 3 | 0.060 | 0.906 | 15.0 |
| q4_hi | H | 481 | 49 | 43 | 6 | 0.089 | 0.878 | 24.4 |
| q4_hi | D | 481 | 50 | 41 | 9 | 0.085 | 0.820 | 19.8 |
| q4_hi | S2nocont | 481 | 55 | 48 | 7 | 0.100 | 0.873 | 27.7 |

n_match + n_false is sometimes smaller than n_det. The scorer drops rows with a NaN value before it counts detections; here these are the sources whose SoFiA `kin_pa` is NaN. The driver's R divides by every row written, so these sources count against R. The difference is 0–8 rows per run; q3_lo/D has the 8.

Inversion runs (input.invert = true, **reliability off**, the rest of each arm's parameters unchanged):

| region | S2_inv | D_inv | D (positive), n_false from the scorer |
|---|---|---|---|
| q1_lo | 118 | 309 | 345 |
| q2_hi | 147 | 1 | 5 |
| q3_lo | 116 | 312 | 315 |
| q4_hi | 147 | 0 (SoFiA exit 8) | 9 |

### Criteria per region (§7.4)

| | q1_lo | q2_hi | q3_lo | q4_hi | result |
|---|---|---|---|---|---|
| P1: S2 vs D | pass | pass | pass | pass | **pass** |
| P2: S2 vs H | pass | pass | pass | pass | **pass** |
| P3: S2 vs S1 | pass | pass | pass | pass | **pass** |
| P4: S2 vs S2nocont | pass | pass | fail (R 0.950 vs 1.000; tolerance 0.02) | pass | **pass** (3/4) |

### Reading the result (caveats that bound any claim)
- **Absolute completeness is low for every arm: 0.4–10 %.** The truth catalogue is deep: 424–600 sources per 400×400 px half-band region, and most of them were not recovered by any tested setting. A likely cause is that most are too faint for a 3.5–5σ search, but this is a hypothesis: their S/N was not measured here. S2's best region recovers 47 of 481 sources. The result compares settings near SoFiA's detection limit. It does not show that the suggestion finds most of the sources.
- **The tolerances are as large as the values.** §7.4 sized the 0.05 tolerance for C ≈ 0.3 and n ≈ 100; the measured C is 0.004–0.10. Without any tolerance:
  - **C:** S2 ≥ D, H and S1 in every region (S2 = H on q2_hi).
  - **R:** S2 is below H on q3_lo (0.95 vs 1.00) and q4_hi (0.870 vs 0.878), and below S1 on q3_lo (0.95 vs 1.00) and q4_hi (0.870 vs 0.906). Those P2/P3 passes on R rest on the tolerance. The differences are 1–2 sources.
- **Low-frequency regions: the defaults fail badly.** D gives 351 and 332 detections with 345 and 315 false, so a negative SDC2 score (−342, −309). SoFiA warns there that the kurtosis deviates from 3 (non-Gaussian noise); D has neither noise scaling nor reliability. In the high-frequency regions D is a reasonable baseline (R 0.82–0.84, C 0.073–0.085), and S2 beats it more modestly: on q2_hi / q4_hi, C +0.014 / +0.012 and R +0.065 / +0.050. P1 is won mostly in the low-frequency half.
- **The inversion runs do not give S2's purity.**
  - S2_inv differs from S2 in two ways: the cube is inverted, and reliability is off (§3.2 m.4). S2 has reliability on.
  - S2_inv's 116–147 detections per region are what S2's clip, linker and minSNR filter pass on the inverted cube without a reliability step. They exceed S2's positive count (9–54).
  - The positive S2 arm with reliability off was not run, so these counts cannot isolate how much the reliability filter contributes. 1 − N_inv/N_pos would be negative; it is not a purity and is not reported as one.
  - SoFiA's own log for S2 shows ~7 400–7 900 positive and about as many negative detections entering the reliability step.
  - For D and D_inv (both without reliability), the inversion count is close to the scorer's false count in the low-frequency regions: 309 vs 345 and 312 vs 315. In the high-frequency regions both counts are small (1 vs 5, 0 vs 9). Those small counts do not show whether the inversion count tracks the false count there.
  - S2's purity against the truth is the scorer-based R: 0.87–1.00.
- **R2 kernels.** S2 vs S1 (the P3 comparison) changes two things at once: S1 has the old kernels (0, 3, 5, 8) **and** contsub off. It measures the combined revision.
  - S2 vs S1: matches 9 vs 2, 37 vs 20, 19 vs 10, 47 vs 29; R difference −0.05 to +0.07.
  - The kernel change alone is S2nocont vs S1, since both have contsub off: matches 8 vs 2, 38 vs 20, 20 vs 10, 48 vs 29; R difference 0, −0.007, 0, −0.034.
  - So the revised kernels roughly double the matches at about the same reliability.
  - With the larger kernels, SoFiA's reliability step started from ~20 000 positive and ~20 000 negative detections per region instead of ~7 600, and kept fewer sources. This is consistent with the §7.1 argument against large kernels.
  - S1 here is 0, 3, 5, 8: rounding of b_px gave 3 instead of 2.
- **C2 (S2 vs S2nocont).** Continuum subtraction gives small, mixed changes, with no consistent improvement across regions:

  | region | matches S2 / S2nocont | false S2 / S2nocont |
  |---|---|---|
  | q1_lo | 9 / 8 | 0 / 0 |
  | q2_hi | 37 / 38 | 4 / 8 |
  | q3_lo | 19 / 20 | 1 / 0 |
  | q4_hi | 47 / 48 | 6 / 7 |

  - Matches change by one source in every region.
  - R rises on q2_hi (0.826 → 0.902, 4 fewer false) and falls on q3_lo (1.00 → 0.95, one extra false), which is P4's single failure.
  - Whether these regions contain continuum residuals was not checked spatially.
  - C2's motivation (a compact continuum residual, as in Apertif) is not specifically tested by SDC2.
- **S2 vs H.** S2 matches as many or more sources than the hand-tuned HCG 44 set in every region, and has higher R on q1_lo and q2_hi. H is not tuned for SDC2 (0, 3, 6 kernels at 4σ), so this is a comparison with a generic hand choice, not with an expert's SDC2 setting.

### Run times
Suggestion 9.9 s. SoFiA per task (400×400×3334 voxels, 32 cores, POWER9; the driver polls every 30 s, so ±30 s):

| arm | time (s) |
|---|---|
| S2 | 300 |
| S2nocont, S2_inv | 270 |
| S1 | 420–450 |
| H, D, D_inv | 210 |

Run stage 7602 s; whole job 7611 s (2 h 07 min).

### What this allows the docs and paper to say
May say:
- On four held-out SDC2 ldev sub-cubes, scored with the challenge's own scorer, the suggested parameters matched at least as many truth sources as SoFiA's defaults, the generic hand-tuned set and the previous kernel rule in every region (completeness 1.9–9.8 %; reliability 0.87–1.00, as matches over submitted rows).
- With the defaults, the low-frequency regions return 330–350 detections, almost all false.

Must say with it:
- that completeness is low in absolute terms for every setting;
- that the advantage over the hand-tuned set and the old rule rests on tens of sources;
- that the wide-band continuum rule (C2) gave only small, mixed changes (±1 match, false detections −4 to +1 per region).

Must not say:
- "high completeness", "optimal" or "better than an expert";
- any inversion "purity" for S2.
