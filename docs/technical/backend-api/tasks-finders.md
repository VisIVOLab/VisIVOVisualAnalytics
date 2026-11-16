# Tasks and source finders

## Tasks

For long-running operations the backend queues a task; the client polls for completion.

### `POST /v1/tasks/moment` · `POST /v1/tasks/pv`
Same request body as the synchronous equivalents (`/v1/products/moment`,
`/v1/cube/pv`); the dataset must be a cube (422 otherwise). Returns
`{valid, error, task_id, status: "running", cache_hit}`.

A task is not bound by `VISIVO_HEAVY_TIMEOUT_S`, the 900 s ceiling of the
synchronous routes. It runs until it completes or is cancelled, unless the
operator sets `VISIVO_TASK_TIMEOUT_S`. Use the task route for a moment or PV
over a large cube or channel range: on one HPC node an M2 over 450 GB takes
about 15 minutes (917 s on BeeGFS, `benchmark/RESULTS.md`).

### `POST /v1/tasks/moment_fits`
The task form of `POST /v1/products/moment_fits`: it computes a moment map and
writes it to Workspace Exports as a 2-D FITS file, without the synchronous
route's `VISIVO_HEAVY_TIMEOUT_S` limit. Use it for large cubes. The map comes
from the same path as `POST /v1/tasks/moment` (and its product cache), so the
file equals the moment product of the same request bit for bit.

Request: the same body as `/v1/products/moment_fits` (`MomentSaveFitsRequest`).

| field | type | default | meaning |
|---|---|---|---|
| `dataset_id` | string | required | an open cube of this session |
| `moment_order` | int | required | 0, 1, 2, … as for `/v1/products/moment` |
| `channel_start`, `channel_end` | int | 0, 0 | inclusive range, clamped to the cube |
| `mask_enabled` | bool | false | keep only voxels ≥ the threshold |
| `threshold_value` | float | 0.0 | the threshold |
| `threshold_auto` | bool | false | with `mask_enabled`: median + 3σ (MAD) of the range |
| `moment2_variance` | bool | false | order 2: variance instead of dispersion |
| `output_basename` | string | `""` | file name in Workspace Exports; empty gives `<cube stem>_m<order>.fits` |

The output name is reserved when the task starts, as an empty file. The map
is written under a temporary name in a hidden subdirectory of the workspace,
which `GET /v1/exports/list` does not enter. On completion it is renamed onto
the reserved name in one step. So, in the listing, a zero-size entry is an
export not yet complete, and a non-empty one is whole. A failed or cancelled
task removes both files.

Answers at once:

- 200 `{valid: true, error: "", task_id, status: "running", cache_hit: false}`;
- 404 when `dataset_id` is not open in the session;
- 422 when the dataset is not a cube or the body is malformed.

Poll `GET /v1/tasks/{task_id}`; its `operation` is `"moment_fits"`. When
`status` is `completed`, `result` has the fields of the synchronous route's
response (`MomentSaveFitsResponse`):

```json
{ "valid": true, "error": "", "output_path": "/…/exports/cube_m0.fits",
  "workspace_filename": "cube_m0.fits", "width": 1286, "height": 1286,
  "range_min": -0.01, "range_max": 2.3, "bunit": "Jy/beam",
  "moment_unit": "Jy/beam Hz" }
```

A failed computation gives `status: "failed"` with the reason in `error` (no
`result`). `DELETE /v1/tasks/{task_id}` cancels it: the worker stops at the
next slab. A task that fails or is cancelled leaves no file: what was
reserved or written is removed once its worker has ended. The file name is
reserved when the task is created, as an empty placeholder in Workspace
Exports until the map is written. If the name
is already taken, a numeric suffix is added (`cube_m0_1.fits`), so read the
name from `result.workspace_filename`.

### `POST /v1/tasks/decompress`
Decompress a compressed FITS (`.fits.gz`, `.fz`) into the workspace cache, as a
task, before opening it (see [Datasets](datasets.md), *Compressed FITS*).
```json
{ "path": "/data/cube.fits.gz" }
```
`progress` is the fraction decompressed (of the compressed bytes read, so a
real measure, unlike the moment task's ticker). `DELETE /v1/tasks/{task_id}`
stops it within one 16 MB chunk and leaves nothing in the cache.

When there is nothing to decompress, the create response says so itself:
`"status": "completed"`, with `"cache_hit": true` for a copy already in the
cache and `false` for a file that is not compressed. Nothing needs polling, so
a reopen costs what a plain open does. Otherwise the response is
`"status": "running"`.

The result: `compression` (`gzip`, `tile`, or `""` for a file that needs
none), `cache_hit`, `seconds`, `source_bytes` and `decompressed_bytes`. A
missing file, one outside the data roots, or one that is no dataset answers
404. A damaged file fails the task with the reason as its `error`. Afterwards
`/v1/datasets/open` on the same path finds the copy. The desktop client runs
this task before the open, behind a progress sheet with the percentage and
Cancel; `BackendClient::openDataset()` called without a window runs it too,
without a sheet.

### `GET /v1/tasks/{task_id}`
```json
{ "valid": true, "error": "", "task_id": "...", "operation": "moment",
  "status": "completed", "created_at": "…", "updated_at": "…",
  "progress": 1.0, "cache_hit": false, "result": { … } }
```
`status` is `pending`, `running`, `completed`, `failed` or `cancelled`.
`result` holds the same payload as the synchronous route once the task has
`completed`; `error` holds the reason of a failure. A task is visible only to
the token that created it (404 for any other, as for an unknown id).

### `DELETE /v1/tasks/{task_id}`
Request cancellation of a pending or running task: `{cancelled, task_id}`.
404 for an unknown task, 409 when it has already finished. Cancellation is
best-effort: the task may still complete first. A moment or PV worker polls a
stop file between slabs or blocks: the streamed path and the block-wise (dask)
path that the backend uses above 512 MB both stop within one slab or block.

On the desktop, moment maps, moment FITS exports and PV diagrams follow their
task with `visivo::followBackendTask()`. It polls after 250 ms, then backs off by
1.5× per poll up to 3 s, with no wall-clock cap, and sends
`DELETE /v1/tasks/{task_id}` when the user aborts. It falls back to the
synchronous route only when task creation answers 404 or 405 (a backend
without the task route). A PV whose path the user changes while it runs is
cancelled on the backend, and the newest path starts; closing the cube window
cancels the PV under way.

---

## Source finders

Source finders are plugins (`backend/app/compute/finders`) behind one
interface: a finder writes a label mask (FITS on the dataset's grid, 0 for
background, n for the n-th source) and a VOTable catalogue, and returns the
catalogue rows. Two ship with the backend:

| Finder | Works on | Available when |
|---|---|---|
| `sofia2` | `cube` | the SoFiA-2 executable is found (`VISIVO_SOFIA_BIN`, or `sofia2`/`sofia` on `PATH`) |
| `photutils` | `image` | always |

The list of available finders is also published in
[`/v1/node/status`](node-soda.md#get-v1nodestatus).

### `GET /v1/finders`
Every finder this node knows, available or not:

```json
{ "finders": [
  { "name": "sofia2", "kinds": ["cube"],
    "description": "SoFiA-2 smooth-and-clip source finder for spectral-line cubes.",
    "parameters": { "scfind.threshold": "S/N threshold of the smooth-and-clip finder (default 5.0)", "…": "…" },
    "available": false,
    "reason": "SoFiA-2 executable not found (set VISIVO_SOFIA_BIN or put sofia2 on PATH)",
    "memory_factor": 2.3, "memory_floor_bytes": 134217728 } ] }
```

`parameters` maps each parameter a caller may set to a one-line description.
`memory_factor` and `memory_floor_bytes` estimate a run's memory as
`factor × dataset bytes + floor`.

### `POST /v1/tasks/sourcefind`
Run a finder as an asynchronous task.

```json
{ "dataset_id": "...", "finder": "sofia2",
  "params": { "scfind.threshold": 4.0, "linker.minSizeZ": 3 } }
```

`finder` defaults to `sofia2`. For SoFiA-2, `params` keys are SoFiA-2 2.7's
own names (`section.key`), limited to the pipeline, flag, contsub,
scaleNoise, scfind, threshold, linker, reliability, filter, dilation and
parameter sections, plus `input.invert` and `input.region` (six pixel
indices, `x_min, x_max, y_min, y_max, z_min, z_max`). The rest of the input
section, the output section and the keys that name a file (`flag.cube`,
`flag.catalog`, `reliability.catalog`) are refused, so a parameter can never
make the node read a path of the caller's choosing; so are values with a line
break, and names SoFiA-2 2.7 does not know (SoFiA would stop on them with
exit code 7), with the closest real name when there is one:
`scfind.kernelXY` → "did you mean 'scfind.kernelsXY'?". Values are checked
against how SoFiA-2 reads them, because SoFiA itself checks none: a switch is
on only for exactly `true` (so `yes` would silently be off), a number is what
`strtod`/`strtol` make of the text (`abc` would be 0), and an unknown word
falls back without a warning (`scfind.statistic = bogus` would run with the
standard deviation). So switches must be `true` or `false` (any case; written
in lower case), whole numbers and numbers must be just that, lists such as
`scfind.kernelsXY` whole numbers separated by commas, words one of those SoFiA
knows (`scfind.statistic`: `std`, `mad`, `gauss`; `*.fluxRange`: `negative`,
`positive`, `full`; …), and a key sent with no value is refused (leave it out
to keep SoFiA's default). For `photutils` they
are `nsigma` (default 3.0), `npixels` (default 5) and `connectivity` (4 or 8,
default 8).

Returns `{valid, error, task_id, status: "running", cache_hit}`. Refusals
before the task starts: 404 unknown finder, 503 finder not available on this
node, 422 wrong dataset kind or, for SoFiA-2, a refused parameter (the
reason in `detail`; no task is created and SoFiA-2 never starts). A refused
`photutils` parameter fails the task, with the reason in `error`. Poll with `GET /v1/tasks/{task_id}`; on
completion `result` holds `valid`, `error`, `dataset_id`, `finder`,
`n_sources`, `sources` (catalogue rows), `mask_path` and `catalog_path` (under
`<workspace exports>/sourcefind/<task_id>/`), plus `log_path`, `exit_code`
and `summary` for SoFiA-2 and `threshold` and `noise` for photutils. A run is
limited to `VISIVO_FINDER_TIMEOUT_S` (default 3600 s); cancelling the task
stops the finder process and removes its output directory.

SoFiA-2's exit code 8 ("No sources left after filtering") is an empty result,
not a failure: the task completes with `n_sources` 0, `sources` `[]`, empty
`mask_path`/`catalog_path`, `exit_code` 8, and its output directory (log,
.par) kept. The inversion test of a setting relies on it. Any other non-zero
exit fails the task; `error` then carries SoFiA's own `ERROR:` messages (the
head of its output is often a long warning box) followed by the end of its
output, at most 1200 characters.

`summary` is read from SoFiA-2's log (HCG 44 example):

```json
{ "n_positive": 53, "n_negative": 33, "rho": 0.741, "n_after_filter": 18,
  "warnings": ["FITS header keyword of CELLSCAL = '1/F' detected",
               "Significant channel-to-channel noise correlation detected.",
               "Only 33 negative detections found."],
  "errors": [],
  "note": "Reliability ran on 33 negative and 53 positive detections." }
```

`n_positive`/`n_negative` are the detections the reliability filter modelled
(`null` when reliability was off), `rho` SoFiA's Pearson coefficient of
adjacent channels, `n_after_filter` the sources left after reliability and
filtering, `warnings` the first sentence of each SoFiA warning (the text
inside a boxed one), `errors` SoFiA's `ERROR:` messages (on exit 8: "No
sources left after filtering. Terminating pipeline."), `note` a sentence for
the client to show next to the suggestion: the reliability counts, or "No
sources left after filtering.", or empty.

The older synchronous route `POST /v1/sourcefind/{dataset_id}` is described
under [Spectral](spectral.md#post-v1sourcefinddataset_id); it applies the same
parameter checks.

### `POST /v1/sofia2/suggest/{dataset_id}`
SoFiA-2 parameters suggested from the cube, each with the reason it has that
value (issue #64). The backend reads the cube once, on a strided sample of
channel pairs (at most 96 pairs, about 4 M voxels, read from a memory map:
cubes larger than the node's memory are fine; about 1 s on a 2 GB cube on a
laptop), and does not need SoFiA-2 to be installed. The run itself goes to
`POST /v1/tasks/sourcefind` with the returned `params`, once the user has
reviewed them.

Request:

```json
{ "data_type": null, "line": null }
```

Both fields are optional (`{}` is a valid body). Fields not listed are
ignored: a client that still sends the first draft's `goal` is not refused.
The suggestion never includes mask dilation (`dilation.*`): a user who wants
dilated masks for total fluxes sets those keys by hand.

| Field | Values | Meaning |
|---|---|---|
| `data_type` | `null`, `emission`, `absorption`, `continuum` | `null`: deduced. Send it when the previous answer's `data_type.ambiguous` was true and the user chose |
| `line` | `null`, `hi`, `galactic` | the line-width ladder of the spectral kernels; `null`: deduced. Send it when `line.ambiguous` was true |

Response (HCG 44, `hgc44_3axes.fits`; abridged, 18 parameters in all):

```json
{ "valid": true, "error": "", "dataset_id": "…", "sofia_version": "2.7.0",
  "data_type": { "value": "emission", "deduced": true, "confidence": "high", "ambiguous": false,
    "reason": "Signal is positive: 814 sample voxels above +5σ, 1 below −5σ (noise alone: 1.4).",
    "options": ["emission", "absorption", "continuum"] },
  "line": { "value": "hi", "deduced": true, "ambiguous": false, "confidence": "",
    "reason": "HI rest frequency and a 1296 km/s band: extragalactic HI line widths.",
    "options": ["hi", "galactic"] },
  "parameters": [
    { "key": "scfind.kernelsXY", "value": "0, 3, 6",
      "reason": "Beam is 5.2 px. Unsmoothed is the matched filter for point sources; the largest kernel, 6 px (~1.12 beams), keeps the effective resolution near 1.5 beams (√(b² + k²) = 1.53 b here, against 2.24 b for a 2-beam kernel), so nearby sources and faint bridges between them are not blended into one; it gives ≥ 92 % of the best smoothing S/N for sources of observed size up to ~2.5 beams, less for larger ones (83 % at 3, 67 % at 4 beams).",
      "sofia_default": "0, 3, 6", "changed": false },
    { "key": "reliability.enable", "value": "true",
      "reason": "1.26e+06 resolution elements: probably enough negative detections, but SoFiA may warn that there are few; check the count after the run.",
      "sofia_default": "false", "changed": true } ],
  "params": { "input.invert": "false", "contsub.enable": "false", "scaleNoise.enable": "false",
    "scfind.kernelsXY": "0, 3, 6", "scfind.kernelsZ": "0, 3, 7, 15", "reliability.enable": "true",
    "reliability.threshold": "0.9", "linker.positivity": "false", "scfind.threshold": "4.0",
    "scfind.statistic": "mad", "scfind.fluxRange": "negative", "scfind.replacement": "2.0",
    "linker.radiusXY": "2", "linker.minSizeXY": "5", "linker.radiusZ": "2", "linker.minSizeZ": "5",
    "filter.minSNR": "3.0", "filter.discardNegative": "true" },
  "sofia_defaults": { "dilation.enable": "false", "input.region": "", "scfind.threshold": "5.0", "…": "…" },
  "measurements": { "shape": [158, 700, 700], "sigma": 0.0002246, "noise_side": "negative",
    "b_px": 5.202, "omega_b_px": 30.66, "channel_kms": 8.202,
    "channel_note": "optical velocity (AIPS VELO-HEL, no VELREF), Δv/(1 + v/c)",
    "rest_frequency_hz": 1420405758.58, "rho": 0.726, "l_z": 2,
    "r_chan": 1.078, "r_chan_noise": 1.044, "r_sky": 1.043, "r_sky_noise": 1.04,
    "f_nan": 0.0, "f_zero": 0.0, "n_res": 1262000.0, "cont_fraction": 0.002253,
    "channel_offsets": 0.0 },
  "warnings": [ { "code": "reliability_caveat",
    "message": "Few resolution elements for the reliability filter: read the negative-detection count in the run summary." } ] }
```

- `parameters` is ordered for display; every entry has a non-empty `reason`.
  `value` is the text written in the .par (`"0, 5, 10"`, `"true"`, `"4.0"`);
  `sofia_default` is SoFiA-2 2.7's built-in value (`null` when it has none),
  `changed` whether the suggestion differs from it. `sofia_defaults` maps
every key a run accepts to SoFiA-2 2.7's default (`""` when it has none), so
a client can show the default of a key the user adds and refuse a name the
run would refuse. `params` is the same as a
  map, ready for `POST /v1/tasks/sourcefind`; it always passes that route's
  checks. Some keys appear only when they apply: `scaleNoise.mode`/`windowXY`/
  `windowZ`, `contsub.order`, `flag.auto`/`threshold`, `reliability.threshold`,
  `linker.positivity` and `input.region`.
- `data_type` and `line` are deductions. When `ambiguous` is true the client
  asks the user, pre-selecting `value` among `options`, and posts again with
  the choice. `data_type.confidence` is `high`, `medium`, `low` or `user`
  (`line` has no confidence: empty).
- `input.region`, when present, is a proposal: the cube does not fit SoFiA-2
  (about 2.3× its size) in this node's memory, and a `memory` warning says so.
  It is never applied silently; the user reviews it.
- `warnings[].code`: `memory`, `no_beam`, `no_channel_width`,
  `reliability_caveat`, `zero_blanks`, `blank_footprint`, `channel_offsets`.
- `measurements` (`null` where not measurable): `shape` (z, y, x),
  `sample_voxels`, `stride_xy`, `channel_pairs`; `sigma` the noise-side σ in
  `bunit`, read as SoFiA reads it: 1.4826 × the median distance from zero of
  the values on the noise side of zero (`noise_side` is `negative`, or
  `positive` for absorption: the cube SoFiA will search), so bright emission
  does not inflate it (faint emission over most of the field pulls it low,
  for SoFiA as here);
  `sigma_negative`, `sigma_positive` (the same from each side of zero);
  `beam_major_arcsec`, `beam_minor_arcsec`, `pixel_arcsec`, `b_px` (geometric
  mean beam FWHM in pixels), `omega_b_px` (beam area in pixels);
  `channel_kms` (rest-frame channel width: c|Δν|/ν for frequency, c|Δλ|/λ for
  wavelength, |Δv|/(1 − v/c) radio (VRAD, AIPS VELO-xxx with VELREF > 256),
  |Δv|/(1 + v/c) optical (VOPT, FELO, AIPS VELO-xxx with VELREF absent or
  ≤ 256, as wcslib reads it), |Δv|/(1 − v²/c²) relativistic (Paper III
  VELO); a frequency axis without a rest frequency gives `null`),
  `channel_note` (how it was read), `band_kms`, `rest_frequency_hz`
  (RESTFRQ, RESTFREQ or FREQ0), `spectral_ctype`; `rho` (adjacent-channel
  correlation of the noise, 1 − σ_Δ²/(2σ²): σ_Δ the robust σ of the
  difference between the two channels of a sampled pair, σ that of the
  difference between channels at least 4 apart at the same pixel, where the
  noise no longer correlates; both cancel a continuum or baseline level, both
  are medians over the sampled pairs, and both leave out the values more than
  3σ from their pixel's level (and the pairs next to them along the band), so
  a line does not count as noise; the σ about zero only when the band is too
  short. On a short band (under about 64 channels) where a line fills the
  field and drifts over much of the band, `rho` can still read too high), `l_z` (2 when `rho` > 0.3); `r_chan`, `r_sky`
  (P95/P5 of the noise-side σ over channels and over blocks of about four
  beams) with `r_chan_noise`, `r_sky_noise` (what estimator noise alone gives
  on this cube) and `sky_blocks`; `channel_offsets` (fraction of sampled
  channels whose median is off zero); `f_nan`, `f_zero`, `zero_blanked`;
  `n_res` (finite voxels / (beam area × `l_z`)), `finite_voxels`;
  `cont_fraction`, `cont_strong_fraction`, `cont_slope_sigma` (continuum
  test); `tail_high`, `tail_low`, `tail_expected` (sample voxels beyond ±5σ,
  and what noise alone gives per side); `cube_bytes`, `sofia_memory_bytes`,
  `node_memory_bytes`.

Errors: 422 when the dataset is not a spectral-line cube of Stokes I (an
image, a catalogue, a Stokes Q/U/V cube, a volume without a spectral axis
such as a simulation grid or a VBT field, a cube of fewer than 8 channels),
or is one SoFiA-2 2.7 cannot read as it is (in a FITS extension, which
includes tile-compressed cubes; a gzip/bzip2/zip-compressed file; more than
four axes; the spectrum on an axis other than 3), with the reason in
`detail`; 422
for a `data_type` or `line` outside the values above; 429 when the
session's heavy-task slots are busy.

#### The rules

Written for SoFiA-2 2.7.0 by the project's radio astronomer; the spec, with
its validation protocol, is `validation/sofia_suggest_spec.md`. The spatial
kernels (R2) and the wide-band continuum rule (C2) are its revision of
2026-11-05b, written after the HCG 44, WALLABY and Apertif runs: on those
cubes they are design data, and SDC2 is their validation. b is the beam FWHM
in pixels, Δv the rest-frame channel width, N_res the resolution elements,
l_z the channels per independent element, rnd(x) = floor(x + 0.5).

| Parameter | Rule |
|---|---|
| `scfind.kernelsXY` | R2: `0, rnd(k_max/2), k_max` with k_max = rnd(1.12·b) (effective resolution √(b² + k_max²) ≈ 1.5 b); the middle kernel dropped below 2 px or when equal to k_max; k_max < 2: `0, 3`; no beam: `0, 3, 6`. HCG 44 (b 5.2) and WALLABY (5.0): `0, 3, 6`; b 2.5: `0, 2, 3` |
| `scfind.kernelsZ` | 0 and the odd integer nearest W/Δv for W = 15, 30, 60, 120 km/s (HI) or 2, 5, 10, 20 km/s (Galactic), keeping values ≥ 3 and ≤ N_ch/4 |
| `scfind.threshold` | 4.0 with reliability, 3.5 with reliability and N_res ≥ 1e7, 5.0 without |
| `reliability.enable` | on for N_res ≥ 2e6, on with a caveat for 5e5 ≤ N_res < 2e6, off below; off for absorption, continuum residuals, and field noise variation without local noise scaling |
| `scaleNoise.*` | `local` when the noise varies across the field by more than 1.2×, else `spectral` when it varies over the band by more than 1.1×; in both cases also by more than estimator noise alone gives (`r_*_noise`) |
| `linker.radiusXY` / `radiusZ` | max(1, round(b/2.5)) / 2 when channels are correlated or Δv < 5 km/s, else 1 |
| `linker.minSizeXY` / `minSizeZ` | clamp(round(b), 3, 10) / max(round(W_min/Δv), 2·l_z + 1), at most half the channels; W_min 20 km/s (HI) or 2 km/s (Galactic: not validated) |
| `contsub.*`, `flag.auto` | on when more than 1 % of the sampled spectra sit off zero at both ends of the band (order 1 when the offset changes by more than 3σ over the band; `flag.auto` when some sit more than 5σ off), OR (C2) order 0 when the rest-frame band is ≥ 20× the ladder's widest line (HI 2400 km/s, Galactic 400 km/s): a constant per spectrum cannot remove a line there and removes local continuum residuals too small for the 1 % test. HCG 44 (1296 km/s) and WALLABY (428) off; Apertif (10 171) on |
| `input.invert` | true for absorption |
| `dilation.*` | not suggested (v1 leaves masks at the clip); still accepted when set by hand |

The data type is read from the two tails, sample voxels beyond +5σ against
those beyond −5σ (emission, absorption, or ambiguous when both or neither
carry signal), then from the continuum test: a spectrum is off zero when the
medians of the first and last fifth of its sampled channels are both off zero,
with the same sign, by more than 3 standard errors of the pixel's own noise.
Continuum lies under the whole band, a line seldom reaches both of its ends,
so a galaxy filling most of a cubelet's band is not taken for continuum (a
line wider than the band in those pixels would be, and the reason says so).
The HI ladder needs a rest frequency (RESTFRQ, RESTFREQ or FREQ0) ≈
1420.405752 MHz and a band wider than 300 km/s; the Galactic one a
CO/13CO/C18O rest frequency or channels narrower than 1 km/s; otherwise HI
is assumed and `line.ambiguous` is true. The Galactic ladder and its 2 km/s
narrowest line are not validated: the validation cubes are extragalactic HI.

The reasons name what decided a value: a spectral kernel step left out
("the 120 km/s step (31 channels) is dropped: more than a quarter of the 109
channels"), and which condition set `linker.radiusZ` to 2 ("correlated
(ρ=0.74)", "narrow (3.92 < 5 km/s)").
