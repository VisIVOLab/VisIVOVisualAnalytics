# Issue #64 — Suggest SoFiA-2 parameters from the data: rules and validation protocol

Status: design spec, step 1 (radio astronomer), 2026-11-05. Revised 2026-11-05 after the implementation review (rev. notes inline; §2.2 lists them). Worktree read: `/Users/fvitello/Documents/GitHub/VisIVO-next-64` (main 1.0.9, 8303e71). No code edited.

SoFiA-2 reference version: **2.7.0** (`sofiapipeline/sofia2:latest`, the image the validation masks were made with; defaults below are read from its `/app/template_par_file.par` and the binary's help text, not recalled).

---

## 0. Blocking findings (fix before any suggestion work)

### B1. The cube viewer's SoFiA-2 dialog cannot run SoFiA-2 2.7.0 at all
`src/gui/SourceFindDialog.cpp:124-129` sends

```cpp
{ u"scfind.kernelXY"_s, ... },   // SoFiA name is scfind.kernelsXY
{ u"scfind.kernelZ"_s,  ... },   // SoFiA name is scfind.kernelsZ
{ u"merge.enable"_s,    ... },   // no 'merge' section in SoFiA-2 (it is SoFiA-1)
```

SoFiA-2 has `pipeline.pedantic = true` by default and stops on unknown keys. Reproduction (synthetic 64×64×40 cube, scratchpad `sofiatest/`):

```
docker run --rm --platform linux/amd64 --entrypoint sofia -v $S:/w sofiapipeline/sofia2 /w/a.par
WARNING: Unknown parameter: 'scfind.kernelXY'
WARNING: Unknown parameter: 'scfind.kernelZ'
WARNING: Unknown parameter: 'merge.enable'
ERROR: Unknown parameter settings encountered. ... Terminating with error code 7.
```

Expected: the dialog sends SoFiA-2 names, kernel LISTS (`"0, 5, 10"`), and no `merge.*`. Also: SoFiA-2's spatial kernel is a circular Gaussian (help text: "The Gaussian filter is assumed to be circular"), so separate X and Y kernel spins have no SoFiA equivalent; a single kernel value is not the same as SoFiA's default list `0, 3, 6`. The user guide (`docs/user-guide/source-finding.md`, "Running it" table: Kernel X/Y, Kernel Z, Merge sources) documents these non-existent controls. #64's UI replaces this dialog, so it should be fixed as part of #64 or before it.
(Docker trap: the image's ENTRYPOINT is broken shell form; use `--entrypoint sofia` and `--platform linux/amd64` on Apple silicon.)

### B2. The parameter allow-list rejects parameters the suggester needs
`backend/app/compute/finders/sofia2.py:42` allows sections `pipeline, flag, scfind, threshold, linker, reliability, dilation, parameter`. Missing, and needed by the rules below:
- `scaleNoise.*` (noise varying across the cube),
- `filter.*` (`filter.minSNR`, `filter.discardNegative` — the hand-tuned validation runs set `filter.minSNR`),
- `contsub.*` (continuum residuals),
- `input.invert` (absorption) and `input.region` (sub-cube on big cubes). These are in the `input` section; they name no file, so they can be allowed individually while `input.data/noise/weights/gain/primaryBeam/mask` stay refused.
`background.*` (2.7.0 has it) is not needed by v1.

### B3. Two routes, two behaviours
The cube viewer dialog posts to `/v1/sourcefind/{id}` (`backend/app/routers/spectral.py:655`, `worker_sofia` in `backend/app/compute/sofia.py:83`), which applies NO allow-list and no newline check (any `input.*`/`output.*` key reaches the .par). The SKAVA dialog uses `/v1/tasks/sourcefind` with `refused()`. Suggested parameters must travel through one validated path; the engineers should decide whether the cube dialog moves to the task route. (Security aspect for the backend engineer, not a science point.)

### What the app exposes today
| Surface | Parameters | Defaults |
|---|---|---|
| Cube dialog (`SourceFindDialog.cpp:56-84`) | threshold 5.0 σ; Kernel X/Y/Z 3 (single ints); Merge true | broken names, see B1 |
| SKAVA dialog / finder catalogue (`SkavaSourceFindDialog.cpp:31-37`, `finders/sofia2.py:21-29`) | free table: scfind.threshold, kernelsXY, kernelsZ, linker.minSizeXY/Z, reliability.enable/threshold | blank = SoFiA default |
| Copilot `find_sources` (`backend/app/assistant/tools.py:598-622`) | none (SoFiA defaults) | — |

### What the backend already measures (reusable)
- Per-channel 1.4826·MAD in a user box, with zero-as-blank heuristic and `blank_fraction`: `worker_noise_estimate`, `backend/app/compute/cube.py:561`, route `/v1/cube/noise`.
- Negative-side σ on a strided sample, asymmetry guard, `stride_for`: `backend/app/noise_threshold.py:52-124` (issue #93).
- Robust σ of the cube: `kinematic_fit.robust_sigma` (`compute/kinematic_fit.py:113`).
- Beam: `ScientificFitsDataset` BMAJ/BMIN and per-channel BEAMS table (`backend/app/fits_dataset.py:99-173`); beam area in pixels `compute/_common.py:74`.
- Channel → km/s: `kinematic_fit.velocity_axis_kms` (optical from RESTFRQ; velocity axes as given).
- Whole-cube stats incl. finite/blank counts: `/v1/analysis/cube-stats` (`routers/analysis.py:41`).

Not measured anywhere yet (needed): adjacent-channel noise correlation ρ; spatial noise map (negative side, per block); per-channel negative-side σ over the whole field; continuum-residual test; number of independent resolution elements N_res.

---

## 1. Measurements the suggester makes (one strided pass, out-of-core)

All on a strided sample (`stride_for`, ~5 M voxels), negative side only so emission does not bias them (validated below: block MAD on WALLABY reads 1.42× max/min because of the galaxy, negative-side σ reads 0.94-1.06).

| Symbol | Definition | HCG 44 (`hgc44_3axes.fits`) | WALLABY NGC 3137 (CASDA cubelet) |
|---|---|---|---|
| shape | z,y,x | 158×700×700 | 109×280×196 |
| σ | negative-side σ, whole sample | 2.247e-4 Jy/beam | 1.97e-3 Jy/beam |
| b_px | geometric mean beam FWHM / pixel | √(6.63×4.08)=5.2 | 5.0 |
| Ω_b | beam area in pixels (1.1331·BMAJ·BMIN/px²) | 30.7 | 28.5 |
| Δv_rest | rest-frame channel width, km/s (§2.0) | 8.20 (8.244 optical / (1+1536/c); VELO-HEL without VELREF is OPTICAL, as wcslib reads it) | 3.92 (c·Δν/ν at band centre; 3.94 is the optical Δv) |
| ρ | adjacent-channel noise correlation, ρ = 1 − σ_Δ²/(2σ²), σ_Δ = 1.4826·MAD of (ch_{k+1} − ch_k) on the sampled pairs, σ the noise-side σ; clamp to [−1, 1] (rev. 2026-11-05: plain Pearson with a 5σ clip reads 0.068 on WALLABY because of sub-5σ galaxy emission; 0.023 outside the galaxy footprint) | 0.741 (SoFiA log); diff estimator 0.740 | 0.024 (SoFiA log); diff estimator 0.011 |
| L_z | 2 if ρ > 0.3 else 1 (channels per independent element) | 2 | 1 |
| r_chan | P95/P5 of per-channel negative-side σ; F_chan = exp(2·1.645·1.65/√n_ind) is what noise alone gives (n_ind = independent values one estimate reads, P10 over channels) | 1.078 (F 1.044) | 1.076 (F 1.135) |
| r_sky | P95/P5 of negative-side σ (about zero) in blocks of ~4 beams; F_sky as F_chan, with n_ind / L_z | 1.043 (F 1.040) | 1.111 (F 1.104) |
| f_NaN, f_zero | non-finite fraction; exact-zero fraction of finite | 0, 0 | 0.011, 0 |
| N_res | finite voxels / (Ω_b·L_z) | 1.26e6 | 0.21e6 |
| cont | fraction of sampled pixels whose first AND last fifth of the band are both off zero, same sign, > 3 standard errors each (rev. 2026-11-05: 'median ≠ 0' flagged broad lines; a line touching both band edges still reads as continuum) | 0.0023 | 1.8e-5 |

Dataset facts measured here: HCG 44's noise is flat across the field (corner 2.245e-4 vs centre 2.257e-4 Jy/beam; radial rings 2.24-2.27e-4): the cube is not primary-beam corrected. Its channels are strongly correlated (ρ 0.74; SoFiA warns "Significant channel-to-channel noise correlation"), consistent with Hanning smoothing.

---

## 2. Rules (v1, rule-based). Each value carries a reason string.

### 2.0 Channel width in km/s
`Δv_rest = c·|Δν|/ν` for a frequency axis (ν at the band centre), `= |Δv_axis|/(1+v/c)` for an optical-velocity axis, `= |Δv_axis|·ν0/ν`-equivalent for radio velocity. Line widths are physical (rest-frame), so the number of channels a line spans is W_rest/Δv_rest. At z≈0 every convention agrees (HCG 44, WALLABY); at SDC2 redshifts (950-1150 MHz, z 0.24-0.5) optical Δv over-states the rest-frame width by (1+z): 30 kHz is 14.2 km/s optical, 8.6 km/s rest-frame at 1050 MHz. No RESTFRQ/RESTFREQ and a FREQ axis → Δv unknown → spectral rules fall back to SoFiA defaults with that reason. Note SDC2 ldev uses RESTFREQ, not RESTFRQ.

### 2.1 Table

| Parameter (SoFiA-2 2.7.0 default) | Rule | Reason string (template) | HCG 44 → | WALLABY → | Hand-tuned (`validation/sofia_*.par`) |
|---|---|---|---|---|---|
| `scfind.kernelsXY` (0, 3, 6) | `0, round(b_px), round(2·b_px)`; add `round(3·b_px)` if N_res ≥ 1e7 (survey field, extended objects expected); no beam → default | "Beam is {b} px: unsmoothed, 1 and 2 beams match point-like and marginally resolved HI sources (Serra+2015; Westmeier+2021)" | 0, 5, 10 | 0, 5, 10 | 0, 3, 6 |
| `scfind.kernelsZ` (0, 3, 7, 15) | (rev. 2026-11-05: the cap N_ch/4 wins; the reason string names any step it drops) 0 plus the odd integer nearest W/Δv_rest for W ∈ {15, 30, 60, 120} km/s (extragalactic HI ladder), drop values < 3 (a boxcar of 1 is the unsmoothed cube), drop duplicates, cap at N_ch/4; with L_z = 2 the 15 km/s step is dropped when it rounds below 3; Galactic/molecular line ladder W ∈ {2, 5, 10, 20} km/s | "Channels are {Δv} km/s (rest frame{, correlated: ρ={ρ}}); boxcars of ~15-120 km/s match dwarf to massive-spiral HI line widths" | 0, 3, 7, 15 | 0, 3, 7, 15 (31 > 109/4 dropped by the cap) | 0, 3, 7, 15 |
| `scfind.threshold` (5.0) | reliability on (2.3) → 4.0; on and N_res ≥ 1e7 → 3.5; reliability off → 5.0 | "{k}σ: with reliability filtering a lower clip is safe (Serra+2012); without it 5σ keeps chance detections rare in {N_res} resolution elements" | 4.0 | 5.0 | 4.0 |
| `scfind.statistic` (mad), `scfind.fluxRange` (negative) | keep | "Noise from the negative side is not biased by emission" | mad/negative | mad/negative | mad |
| `scfind.replacement` (2.0) | keep | — | 2.0 | 2.0 | 2.0 |
| `scaleNoise.enable` (false) / `.mode` (spectral) | r_sky > max(1.2, F_sky) → enable, mode `local`; else r_chan > max(1.1, F_chan) → enable, mode `spectral`; else off (rev. 2026-11-05: floors F from §1) | "Noise varies by {r}× across the {field/band}: a single σ would over-detect in the noisy part" / "Noise is uniform to {r}: no scaling" | off | off | off (WALLABY par comments: spectral changes nothing, local gives 15 vs 12 sources) |
| `scaleNoise.windowXY` (25), `.windowZ` (15) | local mode only: windowXY = odd ≥ max(25, 5·b_px); windowZ = odd ≥ max(15, 2×largest kernelsZ); gridXY/Z left at default | "Window large compared with a source so the source does not inflate its own noise" | — | — | — |
| `linker.radiusXY` (1), `.radiusZ` (1) | radiusXY = max(1, round(b_px/2.5)); radiusZ = 2 if L_z = 2 or Δv_rest < 5 km/s, else 1 | "Merge fragments closer than ~half a beam / across a correlated channel" | 2 / 2 | 2 / 2 | 2 / 2 |
| `linker.minSizeXY` (5) | clamp(round(b_px), 3, 10) | "A real source is at least one beam across" | 5 | 5 | 5 |
| `linker.minSizeZ` (5) | max(round(W_min / Δv_rest), 2·L_z + 1), W_min = 20 km/s (HI ladder) or 2 km/s (Galactic ladder; added 2026-11-05, NOT validated: no Galactic cube in §3; thermal CO FWHM at 10 K is 0.13 km/s, so cold-core lines can be shorter), capped at N_ch/2 | "Narrowest HI lines are ~20 km/s (thermal FWHM at 8000 K ≈ 19 km/s); and longer than the channel correlation" | 5 (2·2+1; 20/8.24 rounds to 2) | 5 (20/3.94 = 5.1) | 5 |
| `linker.positivity` (false) | false whenever reliability is on (SoFiA refuses the combination) | "Reliability needs the negative detections" | false | false | default |
| `reliability.enable` (false) | N_res ≥ 2e6 → on; 5e5 ≤ N_res < 2e6 → on with caveat; N_res < 5e5 → off; off for absorption, for cont > 1 %, and when r_sky > 1.2 without scaleNoise | "{N_res} resolution elements: expect {enough / too few} negative detections to model the noise (SoFiA warns below its minimum)" | on, caveat | off | on (WALLABY: on, but 13 negatives, same 12 sources as off) |
| `reliability.threshold` (0.9) | keep 0.9 | "SoFiA default; lowering trades purity for completeness" | 0.9 | — | 0.9 |
| `reliability.scaleKernel` (0.4), `.autoKernel` (false) | keep | — | default | — | default |
| `filter.minSNR` (3.0), `filter.discardNegative` (true) | keep 3.0 / true; absorption: discardNegative false is irrelevant because the cube is inverted | "Drops detections whose integrated S/N < 3" | 3 / true | 3 / true | 3 |
| `dilation.enable` (false) | off for "catalogue" goal; on (iterationsXY = round(b_px), iterationsZ = L_z+1, threshold 0.001) for "flux" goal | "S+C masks stop at the clip; dilation adds the faint outskirts for total flux" | off (arm 2: on) | off (arm 2: on) | off |
| `input.invert` (false) | true only when the user picks "absorption" | "Absorption is negative signal: SoFiA searches the inverted cube" | false | false | — |
| `contsub.enable` (false) / `flag.auto` (false) | cont > 1 % of pixels → contsub on, order 0 (1 if the median spectrum has a slope > 3σ over the band); strong discrete residuals → also `flag.auto = true`, `flag.threshold = 5` | "{f} % of pixels carry a continuum offset; residual continuum mimics line emission and breaks reliability symmetry" | off | off | — |
| `input.region` | cube bytes × 2.3 > node memory → propose a sub-cube, never silently | "SoFiA-2 needs ~2.3× the cube's size ({GB}); the node has {GB}" (factor from `finders/sofia2.py:39`) | — | — | — |

Notes on the rules:
- Values in the hand-tuned column are NOT per-cube tuning: both `.par` files are identical by design (comment in `validation/sofia_wallaby.par:1-7`). They are a sensible generic choice by the author. The only genuinely expert, cube-specific reference is the Serra+2013 HCG 44 mask (§3).
- The spatial rule gives 0, 5, 10 where the hand-tuned runs used 0, 3, 6: a real difference, tested by the validation. The linker values match the hand-tuned ones; I derived them knowing those values, so the held-out cubes (§3.3) are what tests them.
- WALLABY gets reliability OFF and threshold 5.0, whereas the hand-tuned run used 4.0 with reliability nominally on (SoFiA: 13 negatives; "returns the same 12 sources as with reliability off", `sofia_wallaby.par:9-12`). So on WALLABY the rule is expected to lose faint sources at 5σ; this is a pre-registered risk, not something to tune away after the fact. Alternative to test as arm 3: reliability off, threshold 4.5.
- The N_res cut-offs for reliability (5e5, 2e6) come from two cubes only: HCG 44 (1.26e6 → 33 negatives, SoFiA warns) and WALLABY (0.21e6 → 13 negatives). The count of negatives also depends on kernels and linker, so N_res is a proxy. SoFiA's exact warning constant was not checked (warns at 13 and 33); read it from the SoFiA-2 source before quoting it.
- Post-run check (cheap, v1): parse SoFiA's log for "Found N positive and M negative sources" and "Pearson coefficient" and show them next to the suggestion ("reliability ran on M negative detections"). This turns the volume heuristic into a measured statement for every run.
- Data types out of scope: 2-D continuum images and Stokes Q/U cubes (polarimetry → RM synthesis). The suggester refuses with a reason. Polarised-intensity cubes are not Gaussian-noise cubes; SoFiA's negative-side noise model does not apply.
- Zero-blanked cubes (f_zero > 1 %): SoFiA counts zeros as data and under-estimates σ; v1 warns ("{f} % exact zeros: convert blanks to NaN before running") and does not suggest a threshold silently.
- Line ladder choice: extragalactic HI when RESTFRQ ≈ 1420.405752 MHz and the band spans > 300 km/s; Galactic ladder for CO/13CO/C18O rest frequencies or Δv < 1 km/s; otherwise ask the user ("expected line width").

### 2.2 Revisions after the implementation review (2026-11-05, before any run)
- §2.0: AIPS `VELO-xxx` is OPTICAL unless VELREF > 256 (wcslib/astropy translate VELO-HEL with VELREF absent, 0, 1 or 2 to VOPT, 258 to VRAD; measured). Bare `VELO`/`VELO-F2V` stays relativistic (Paper III). HCG 44: Δv_rest 8.20 km/s, not 8.29.
- Rest frequency keywords: RESTFRQ, RESTFREQ, and FREQ0 (hgc44_3axes.fits carries the HI rest frequency only as FREQ0 = 1420405758.58, same value as RESTFREQ in hgc44.fits). No rest frequency on a velocity axis → HI ladder, flagged ambiguous (user confirms); on a FREQ axis → Δv unknown → SoFiA defaults.
- ρ estimator from differences (§1); r_chan/r_sky noise floors (§1, §2.1); continuum test on both band edges (§1); kernelsZ cap wins (WALLABY 0, 3, 7, 15); Galactic minSizeZ 2 km/s (unvalidated).
- New warning `channel_offsets`: > 10 % of sampled channels whose median is off zero by > max(0.5σ, 5 standard errors).

---

## 3. Validation protocol (fixed before running)

### 3.0 Pre-registration
- Freeze the rules (commit hash of the suggester, AFTER the §2.2 fixes are in) and this metric list before the first run.
- HCG 44 runs on `hgc44_3axes.fits` (the cube the masks were made on); until FREQ0 is read the line ladder is flagged ambiguous: arm S takes the shown default (HI) unedited and the driver records the flag.
- On WALLABY, S differs from D only in kernelsXY (0, 5, 10 vs 0, 3, 6) and linker.radiusXY/Z (2/2 vs 1/1): kernelsZ, threshold (5σ), reliability (off), minSize are identical. The text says so; any WALLABY difference S vs D is attributable to those two. Any rule change after seeing a result is reported as such and re-tested on a held-out cube (§3.3).
- Every number is produced by the app's code path (paper-shows-the-app rule): suggestion from the backend route the dialog calls; SoFiA via the task route; fluxes and w20 measured with the same app function (`kinematic_fit.source_measures` or whatever the GUI's source table uses) on ALL masks, references included, so measurement code is not a variable.
- SoFiA 2.7.0, same Docker image for all arms. Record full `.par`, log, negative/positive counts.

### 3.1 Arms per cube
- S: suggested parameters as shown to the user (no edits).
- H: hand-tuned run (`validation/sofia_<cube>.par`, existing masks in `validation/sofia_out/`).
- D: SoFiA-2 defaults (what a user without the suggester gets) — the baseline the suggester must beat or match.
- S+dil: S with the "flux goal" dilation (secondary).
- (WALLABY only) S4.5: S with threshold 4.5, reliability off (secondary, §2.1 note).

### 3.2 Cubes, references and metrics

**HCG 44** (`~/Downloads/DemoData/hgc44_3axes.fits`; references: R_pub = Serra+2013 published mask `validation/sofia_out/serra2013_hcg44_mask.fits`; R_sof = `sofia_hgc44_mask.fits`; rows A-E = SoFiA ids 5,7,12,14,8 ↔ published 13,1,8,3,7).
Reading scale measured today on rows A-E (R_pub vs R_sof):

| row | n_pix R_sof / R_pub | Dice | flux R_pub/R_sof | channel extent R_sof / R_pub |
|---|---|---|---|---|
| A | 11463 / 28563 | 0.57 | 1.05 | 42 / 73 |
| B | 8754 / 19256 | 0.62 | 1.11 | 68 / 103 |
| C | 4995 / 14563 | 0.50 | 1.31 | 21 / 35 |
| D | 4738 / 10964 | 0.60 | 1.26 | 37 / 59 |
| E | 4207 / 8396 | 0.66 | 1.32 | 32 / 46 |

(flux = Σ mask × Δv / Ω_b, Δv 8.244 km/s, Ω_b 30.7 px; channel extent is the mask's span, not w20.)

**WALLABY / NGC 3137** (CASDA cubelet `WALLABY_J100903-290239_Hydra_TR2_cube.fits`; references: R_sof = `sofia_wallaby_mask.fits` (12 sources, disc 153 227 vox); release catalogue for the disc only: n_pix 299 562 (count ratio only, the release mask is not distributed), flux 142.2 Jy km/s. Honest failure case: four faint compact sources (500-1200 vox).

Metrics (all fixed now):
1. Per-source Dice(S, R) for each matched reference source; match = connected components with overlap ≥ 50 % of the smaller one.
2. Catalogue completeness of S vs H = fraction of H sources matched in S; reliability of S vs H∪R_pub = fraction of S sources matched in H or R_pub. Unmatched S sources are listed with peak S/N, n_pix, SoFiA `rel`, not just counted.
3. Flux ratio F(S)/F(R) per matched source; w20 difference in km/s from the integrated spectrum.
4. Inversion test (reference-free, also on held-out cubes): run SoFiA with S on the inverted cube (`input.invert = true`, reliability off, same filter); N_false ≈ number of inverted detections passing the filter; purity estimate 1 − N_inv/N_pos.

"Comparable" (pass) on HCG 44, per source A-E, needs ALL of:
- Dice(S, R_pub) ≥ Dice(R_sof, R_pub) − 0.05 AND Dice(S, R_sof) ≥ 0.50 (the lower edge of the inter-reference scale), for ≥ 4 of 5 rows;
- flux F(S) within [F(R_sof)·0.9, F(R_pub)·1.1] for ≥ 4 of 5 rows (the two references span 1.05-1.32);
- w20(S) within 2 channels (16.5 km/s) of w20(R_sof) or w20(R_pub);
- completeness vs H ≥ 0.9 for H sources with SoFiA SNR ≥ 5 (all 18 H sources reported);
- inversion purity of S ≥ that of H, or unmatched S sources ≤ unmatched H sources.

On WALLABY:
- disc (NGC 3137): Dice(S, R_sof) ≥ 0.8, flux within 10 % of 142.2 Jy km/s and of F(R_sof), w20 within 2 channels (7.9 km/s);
- the four faint sources: report completeness and Dice for S, S4.5, H, D; no pass/fail (pre-declared failure case); the text says which threshold loses which source.

Global pass: HCG 44 passes AND WALLABY disc passes AND S is no worse than D on every metric (if S equals D somewhere, say so). If S beats H on one cube and loses on the other, report both; no averaging across cubes.

### 3.3 Held-out cubes (rules not seen while designing)
- Apertif `~/Downloads/DemoData/3D_Ape-DR1_200426041_AP_B021_HI_image_cube0.fits` (661×661×1218, ~2 GB, FREQ-OBS): no reference mask → inversion test, positive/negative counts, visual check of the top 10 by the user. Measure first whether noise rises at the field edge (Apertif single-beam cubes are typically not PB-corrected; not checked).
- SDC2 ldev (optional, on PLEIADI, `/mnt/beegfs/fvitello/demo_data/sky_ldev_v2.fits`, 1286²×6668, 44 GB; truth `sky_ldev_truthcat_v2.txt`, Hartley+2023, MNRAS 523, 1967; not on PLEIADI yet): catalogue-level completeness and reliability vs truth with the SDC2 scoring criteria (use the challenge's scoring code, not a re-implementation; criteria to be read from Hartley+2023). Run S and D (and H = the hand-tuned set, unchanged) on 2-4 sub-cubes via `input.region` (memory 2.3×: a full run needs ~100 GB). Expected rule outputs: r_chan ≈ 1.18 (σ 26.7→31.5 µJy/beam) → scaleNoise spectral; b_px = 7"/2.8" = 2.5 → kernelsXY 0, 3, 5 (rounding to check); Δv_rest ≈ 8.6 km/s → kernelsZ 0, 3, 7, 15; N_res ≫ 1e7 → reliability on, threshold 3.5. Continuum residuals to −1 mJy (measured) → the cont test should fire; check.
- (If available) a synthetic injection: model discs in the inverted HCG 44 or WALLABY cube (noise with its real correlation) at S/N 3-20, as in `validation/lasso_synthetic.py`; completeness vs S/N curve for S and D. Optional.

### 3.4 Output
`validation/sofia_suggest.py` (driver of the app's route) → `validation/out/sofia_suggest_<cube>.{csv,md}` with per-row metrics for every arm, the suggested `.par` and the reason strings, plus a docs section.

---

## 4. What the paper and the docs may claim (depending on outcome)

May claim, if the protocol passes:
- "The app proposes SoFiA-2 parameters from the measured noise, beam, channel width and noise correlation, with a stated reason for each; the user edits them before the run."
- "On HCG 44, masks from the suggested parameters agree with both references within the scatter between the references (Dice X-Y vs 0.50-0.66), fluxes within the 5-32 % span of the two references."
- "On NGC 3137 (WALLABY) the main disc is recovered (Dice X, flux X vs 142.2 Jy km/s); faint compact companions are lost at the suggested 5σ threshold without reliability filtering" (or whatever the measured outcome is).
- Reference-free purity from the inversion test on held-out cubes.

Must not claim:
- "Optimal" or "expert-equivalent" parameters (two cubes, one expert mask).
- That the reliability cut-offs on N_res are general: they come from two cubes; say so.
- Any SDC2 score unless the scoring code of the challenge was used.
- That the hand-tuned `.par` files are per-cube expert tuning: they are one generic setting applied to both cubes.

Docs: rewrite the "Running it" table in `docs/user-guide/source-finding.md` (current controls do not exist in SoFiA-2, B1); add "Suggested parameters" with each rule and its reason; add the post-run reliability line (negatives count). The paper series plan keeps the A&C paper SKA-scoped: this feature fits as a short subsection or the generic paper; decision for Fabio.

---

## 5. References
- Serra P. et al. 2015, MNRAS 448, 1922 (SoFiA).
- Westmeier T. et al. 2021, MNRAS 506, 3962 (SoFiA-2).
- Serra P., Jurek R., Flöer L. 2012, PASA 29, 296 (reliability from negative detections).
- Westmeier T. et al. 2022, PASA 39, e058 (WALLABY pilot data release pipeline; its SoFiA settings should be read and compared with the rules; not checked here).
- Serra P. et al. 2013, MNRAS 428, 370 (HCG 44 HI; the published mask).
- Hartley P. et al. 2023, MNRAS 523, 1967 (SKA SDC2).
- SoFiA-2 2.7.0 built-in defaults and help text (`sofiapipeline/sofia2`, `/app/template_par_file.par`).

## 6. Open questions for Fabio
1. Data type: let the user choose "emission / absorption / continuum-not-subtracted" in the dialog, or infer it (continuum test) and only ask when ambiguous?
2. Goal switch "catalogue vs flux" (dilation on/off) in v1, or dilation always off?
3. Is SDC2 in scope for "done" or optional? It needs the truth catalogue on PLEIADI and the SDC2 scoring code.
4. Fix of B1 (dialog names) and B2/B3 (allow-list, one route): inside #64 or a separate quick issue first? The suggestion cannot be validated through the GUI until B1 is fixed.
5. Pre-registered WALLABY expectation: the rules give reliability off + 5σ there; accept a documented loss of faint sources, or take 4.5σ as the v1 rule for small cubes (decide before running, not after).
6. Permission to compare against the Serra+2013 mask in a public doc page (the mask itself still needs Serra's OK to be redistributed).

---

## 7. rev. 2026-11-05b — revised rules after the HCG 44 FAIL, frozen before any SDC2 data is read

Status: written 2026-11-05 after the §3 campaign (results: `validation/out/sofia_suggest/RESULTS.md`, frozen diff d4bdafe). **These rules were written after seeing HCG 44, WALLABY and Apertif.** On those three cubes they are design data, not validation; the only validation is SDC2 ldev (§7.4). HCG 44 stays reported as it was (FAIL). Decisions by Fabio: revise kernelsXY; validate on data not used to build it; remove the "flux" goal (dilation) from v1.

### 7.1 R2: spatial kernels (replaces the §2.1 kernelsXY row)

b = geometric-mean beam FWHM in pixels (as §1). With rnd(x) = floor(x + 0.5) (round half up, NOT Python's round):

    k_max = rnd(1.12 · b)            # effective resolution sqrt(b² + k_max²) ≤ 1.5 b
    k_mid = rnd(k_max / 2)
    kernelsXY = 0, k_mid, k_max      # k_mid dropped if < 2 or == k_max; k_max < 2 → "0, 3" (SoFiA's smallest default); no beam → "0, 3, 6"

No extra kernel for survey-sized cubes (the N_res ≥ 1e7 term of §2.1 is removed).

Justification (Gaussian algebra; my derivation, no paper claimed for it):
- An unresolved-to-marginally-resolved source of observed FWHM θ_s (beam included) on beam-correlated noise, smoothed by a unit-sum Gaussian of FWHM k: peak ∝ θ_s²/(θ_s² + k²), noise ∝ b/√(b² + k²). S/N gain G(k) = θ_s²·√(b² + k²) / (b·(θ_s² + k²)); the best kernel is k = √(θ_s² − 2b²); smoothing helps only when θ_s > √2·b (intrinsic size > 1 beam). The 0 kernel is the matched filter for point sources (Serra+2015 and Westmeier+2021 describe S+C as a set of kernels matched to the expected source sizes).
- Cost of capping at k_max = 1.12 b, as a fraction of the best gain: θ_s = 2 b → 99 %; 2.45 b → 92 %; 3 b → 83 %; 4 b → 67 %. Benefit: blending scale (effective resolution) 1.5 b instead of 2.24 b (old 2b kernel) or 3.16 b (old 3b kernel).
- HCG 44 showed that the extra sensitivity of a 2-beam kernel to faint extended emission linked NGC 3187 and NGC 3190 (6.6 beams apart: the link is low-surface-brightness emission, not beam blending) and raised the negative count used by reliability (54 vs 33), which removed three SNR 6.8–7.9 sources. A separation- or confusion-based bound was considered and rejected for v1: the separation of sources is not measurable before detection, and source counts are not in the noise statistics the suggester reads.
- Expected weakness, said in advance: R2 loses sensitivity to sources larger than ~3 beams (−17 % to −33 % of the best S/N), i.e. well-resolved low-surface-brightness discs. SDC2 contains many resolved discs at 7" (Hartley+2023), so this is tested.

Descriptive only (NOT validation; these cubes were used to design R2):
- HCG 44 (b 5.20): 0, 3, 6 — identical to H in every key, so S2 = H on this cube (H passes there by construction, Dice 1 vs R_sof).
- WALLABY (b 5.01): 0, 3, 6; with 5σ / reliability off / linker 2,2 it differs from D only in linker radius (not run).
- Apertif: no beam → 0, 3, 6 (unchanged). SDC2 (b = 7"/2.8" = 2.5): R2 gives 0, 2, 3; the old rule gives 0, 2, 5, 8 (Python round(2.5) = 2; N_res ≥ 1e7).

### 7.2 C2: continuum residuals on wide bands (adds to the §2.1 contsub row)

Apertif finding (measured 2026-11-05 with the app's own workers, `worker_spectral_aperture`, `worker_moment`):
- 32 of 46 S detections lie at x 495–540, y 119–138 (RA ≈ 167.87°, Dec ≈ +55.67°), at channels 122–1209.
- Aperture spectrum r = 8 px there: median +3.5e-4 Jy/beam over the WHOLE band (thirds +2.8, +4.0, +3.8 e-4; 1.3× the aperture's channel scatter, ≈ 0.19 σ of a single-pixel channel); control apertures 40 px away and at the field centre: +0.1 to +1.0 e-4 (≈ 0).
- Unmasked full-band M0: the largest positive blob of the field is there (238 px > 5σ, peak 9.5σ, ≈ 40×20 px), no line-like shape.
- After removing the offset, one channel stands out: 1169 at 5.5σ (boxcar 15 ch), = SoFiA id 3 (SNR 24.9, 2896 voxels, cz ≈ 19 100 km/s); all other peaks ≤ 3.3σ.
- Reading: a continuum residual of an extended continuum source (offset across ~10 000 km/s, which no HI line can produce), on which noise peaks cross the 5σ clip after smoothing; one candidate real HI source (id 3) sits on top. Counterpart in NVSS: not known (VizieR not reachable from this machine). The user's visual check is still needed for id 3.
- Why the §1 test misses it: it counts the fraction of sampled pixels with offset at both band ends; the residual covers ~240 px of 437 k (0.05 %), far below the 1 % trigger. Its reason string ("the continuum was already subtracted") over-states what was measured.

Rule C2 (OR with the existing 1 % test): contsub.enable = true, contsub.order = 0, when band_rest_kms ≥ 20 × W_max of the line ladder (HI: 20 × 120 = 2400 km/s; Galactic: 20 × 20 = 400 km/s). Reason string: "The band ({B} km/s) is {B/W_max}× wider than the widest expected line: a per-spectrum constant cannot remove a line, and removes local continuum residuals the global test cannot see." SoFiA defaults for contsub.threshold/shift/padding are kept (emission above 2σ is masked before the fit).
Justification: an order-0 baseline fitted over a band ≥ 20 line widths changes a line's flux by ≤ ~1/20 of its integral even before SoFiA's masking; the risk is low and the gain is local residuals. The 1 % test stays for the cube-wide case (and for order 1 / flag.auto).
Descriptive (design data): HCG 44 band 1296 km/s → off; WALLABY 428 → off; Apertif 10 171 → on.
Validation: SDC2 only (§7.4, arm S2-nocont).

### 7.3 Removed from v1
- Goal "flux" (dilation) — measured no-op (HCG 44 +0 voxels, WALLABY +518 on one 197-voxel source). dilation.enable stays SoFiA's default (false).

### 7.4 SDC2 ldev acceptance criteria (frozen before running)

Data: `/mnt/beegfs/fvitello/demo_data/sky_ldev_v2.fits` (read-only), truth `sky_ldev_truthcat_v2.txt`, official scorer `ska_sdc.Sdc2Scorer` (PyPI `ska-sdc`), Hartley+2023. Four sub-regions fixed in `validation/sdc2/sofia_suggest_sdc2.py` (400×400 px; q1_lo, q3_lo in the lower half of the band, q2_hi, q4_hi in the upper half), set before reading the truth. Truth cut to each region with the cube's WCS.

Arms (all through the app's task route, `parameter.physical = true`):
- S2: the app's suggestion with R2 + C2, no dilation. The engineers implement R2/C2 in `sofia_suggest.py` first; the driver asserts that the route's kernelsXY and contsub equal §7.1/§7.2 computed from the route's own measurements and refuses to run otherwise.
- S1: the same suggestion with kernelsXY replaced by the old formula (0, round(b), round(2b), + round(3b) if N_res ≥ 1e7; Python round) and contsub as the old 1 % test decides.
- H: `validation/sofia_hgc44.par` detection keys, unchanged. D: SoFiA defaults.
- S2-nocont: S2 with contsub.enable = false (only if S2 has it on and S1 does not; else C2 has no separate effect on SDC2 and that is reported).
- Inversion: S2_inv, D_inv (input.invert, reliability off, as §3.2 m.4).

Per region r, with C = n_match/n_truth and R = n_match/n_det from the scorer (a region is scored only if n_truth ≥ 20; the 0.05 tolerance ≈ the binomial s.e. of C at C ≈ 0.3, n ≈ 100):
- P1 (vs defaults): C(S2) ≥ C(D) AND R(S2) ≥ R(D) − 0.05.
- P2 (vs hand-tuned): C(S2) ≥ C(H) − 0.05 AND R(S2) ≥ R(H) − 0.05.
- P3 (revision costs nothing): C(S2) ≥ C(S1) − 0.05 AND R(S2) ≥ R(S1) − 0.05.
- P4 (C2, only if S2-nocont was run): R(S2) ≥ R(S2-nocont) − 0.02 AND C(S2) ≥ C(S2-nocont) − 0.05.
Each Pi passes if it holds in ≥ 3 of the scored regions and at least 3 regions are scored; otherwise "inconclusive". SDC2 PASS = P1, P2, P3 (and P4 when run) all pass. Reported, not criteria: SDC2 score per arm and region, n_det, n_false, inversion purity, the suggestion's measurements and reason strings, run times. Expected (§3.3): scaleNoise spectral, kernelsZ 0, 3, 7, 15, reliability on at 3.5σ; whether the old 1 % continuum test fires is recorded.
No change to R2/C2/criteria after the SDC2 numbers are seen; if SDC2 fails, it is reported as a fail.

## 8. rev. 2026-11-05c — measurement fixes from code review, before any SDC2 data is read

These fix how two inputs are measured or bounded. The rules of §7 are unchanged and no criterion moves.

- **ρ estimator.** σ in ρ = 1 − σ_Δ²/(2σ²) was the σ about zero, which keeps a continuum or baseline level that the channel difference cancels: white noise on a +6σ level read ρ ≈ 0.99 and l_z = 2. Now:
  1. **Signal is left out of both estimates.** A pair is blanked at a pixel where either channel sits more than 3 of that pixel's σ from its level (median and MAD over the sampled channels, so a primary-beam gain does not decide). The pairs on either side along the band go with it, for the line wings. Pixels with fewer than 16 sampled values are kept whole.
  2. **σ comes from differences of the same pixel at channels ≥ 4 apart** (Var = 2σ²(1 − ρ_4)), each value paired with the first finite one far enough along. The σ about zero is used only when there are too few such pairs.
  3. **Both σ and σ_Δ are medians over the sampled channel pairs** of each pair's own robust σ, so the few pairs at the edge of broad emission do not weigh, and both keep the same pixels and weights.

  Five earlier versions were refused in review:
  - per-pixel median subtraction;
  - pooled lagged σ;
  - its minimum with the σ about zero;
  - its minimum with a spatial-difference σ;
  - no signal mask: a field-filling line with a velocity gradient read 0.37.

  The final estimator reads:

  | Simulated noise | ρ |
  |---|---|
  | AR(1) 0.4, 8–192 channels (expected 0.384) | 0.38 |
  | AR(1) 0.35 under a radial primary-beam gain (expected 0.336) | 0.34 |
  | Hanning (0.667) | 0.667 ± 0.03, also with a level of 0.25–6σ and with a 6σ line drifting over the band |
  | White + level | 0.00 |
  | Broad emission over 40–80 % of a 192-channel band | ≈ 0 |
  | Short-band emission (3/8, 6/16, 19/32) | ≈ 0 |
  | Field-filling cloud, 5–10σ, 3-channel line drifting over 40 of 64 channels | ≤ 0.05 |

  Known limit, accepted 2026-11-05 after a sixth review round: on short bands (≲ 64 channels) where a line fills the whole field and drifts over much of the band, the line inflates each pixel's MAD enough to escape the mask, and ρ can read above 0.3 (l_z = 2). Examples:
  - 5σ, σ = 3 channels, drifting over 6–26 of 32 channels: 0.41;
  - 10σ, σ = 6 channels on 32: 0.56.

  Every variant tried has a counterexample in this regime; only a real source mask would remove it. The cost is mild (linker.radiusZ 2, N_res halved). It does not arise on HI survey cubes, nor on SDC2 (6668 channels).

  On the local cubes l_z, the kernels and the linker are unchanged:

  | Cube | ρ before | ρ now | SoFiA log |
  |---|---|---|---|
  | HCG 44 | 0.740 | 0.726 | 0.741 |
  | WALLABY | 0.011–0.013 | 0.020 | 0.024 |
  | Apertif | — | 0.019 | — |

  This matters for SDC2, whose continuum residuals reach −1 mJy.
- **kernelsZ without a channel width.** SoFiA's default set (0, 3, 7, 15) now goes through the same N_ch/4 cap as the ladder, because SoFiA refuses a kernel as long as the cube. An 8-channel cube gets 0; a 32-channel cube gets 0, 3, 7.
