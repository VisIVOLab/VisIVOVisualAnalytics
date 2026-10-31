# Dynamic spectra

These routes analyse dynamic spectra and beamformed data: datasets of kind
`dynspec`, a 2-D FITS image with time on `NAXIS1` and frequency on `NAXIS2`
(HDF5 inputs are converted to such a FITS sidecar when opened). Any degenerate
leading axes are dropped. The sampling is read from the header: `CDELT1` is the
time step in seconds and `CRVAL2`, `CRPIX2`, `CDELT2` give the channel
frequencies in Hz.

All three routes return HTTP 404 for an unknown `dataset_id`, 422 when the
dataset is not of kind `dynspec`, and 429 when the session's compute slots stay
busy. Failures inside the computation (including the parameter checks listed
below) return HTTP 200 with `valid: false` and the message in `error`. The two
routes that write files put them in the Workspace Exports directory: the
basename is reduced to its final path component, leading dots are stripped,
`.fits` is appended when there is no extension, and an existing name gets a
`_1`, `_2`, ... suffix instead of being overwritten.

## `POST /v1/dynspec/dedisperse`

Incoherent dedispersion at a given dispersion measure. Each channel is shifted
in time by `round(Δt / dt)` samples, with
Δt = 4.148808 × 10³ × DM × (1/f² - 1/f_ref²) seconds (f in MHz), so that all
channels align at the reference frequency. Shifted samples wrap around to the
other end of the time axis (`np.roll`), they are not padded. The result is
written as a new 2-D FITS with the source header plus `DM`, `DMREFMHZ` and a
`HISTORY` card, and registered as a new `dynspec` dataset.

Request (`DynspecDedisperseRequest`):

| Field | Default | Notes |
|---|---|---|
| `dataset_id` | required | |
| `dm_pc_cm3` | required | dispersion measure, pc cm⁻³ |
| `ref_freq_mhz` | 0.0 | reference frequency; 0 or negative uses the highest channel frequency |
| `output_basename` | `""` | empty gives `<stem>_DM<dm with 3 decimals>.fits` (a `.dynspec` suffix of the source stem is dropped) |

```json
{ "dataset_id": "ds_1a2b3c4d5e6f", "dm_pc_cm3": 26.764 }
```

Response (`DynspecDedisperseResponse`): `valid`, `error`, `new_dataset_id`,
`output_path`, `workspace_filename`, `width` (time samples), `height`
(channels), `range_min`, `range_max`, `dm_pc_cm3`, `ref_freq_mhz` (the value
used), `max_shift_samples` (largest absolute shift applied).

Fails with `valid: false` when the spectrum has at most one channel or one time
sample, when a channel frequency is not positive, or when `CDELT1` is zero.

## `POST /v1/dynspec/pulse_profile`

Folds the frequency-averaged time series at a pulse period and returns the
phase profile as JSON; no file is written. When `dm_pc_cm3` is not zero the
data are first dedispersed in memory, referenced to the highest channel. The
time of each sample comes from the time WCS (`CRVAL1`, `CRPIX1`, `CDELT1`), and
phase is that time modulo the period.

Request (`DynspecPulseProfileRequest`):

| Field | Default | Notes |
|---|---|---|
| `dataset_id` | required | |
| `period_s` | required | pulse period in seconds, `> 0` |
| `n_bins` | 64 | number of phase bins, `>= 4` |
| `dm_pc_cm3` | 0.0 | 0 skips dedispersion (data already dedispersed) |
| `freq_start_mhz`, `freq_end_mhz` | 0.0, 0.0 | when both are positive and `end > start`, only channels inside the band are averaged; otherwise the full band is used |

```json
{ "dataset_id": "ds_1a2b3c4d5e6f", "period_s": 0.7145, "n_bins": 128, "dm_pc_cm3": 26.764 }
```

Response (`DynspecPulseProfileResponse`):

| Field | Content |
|---|---|
| `phase` | bin centres, `(i + 0.5) / n_bins` |
| `profile` | mean intensity per bin; `null` for a bin with no finite samples |
| `counts` | number of samples per bin |
| `peak` | maximum of the profile |
| `sigma_mad` | 1.4826 × MAD of the profile (0 when there are 4 or fewer finite bins) |
| `snr_peak` | `(peak - median) / sigma_mad` (0 when `sigma_mad` is 0) |
| `period_s`, `dm_pc_cm3`, `n_bins` | echo of the request |
| `n_samples` | number of time samples folded |

Fails with `valid: false` for `period_s <= 0`, `n_bins < 4`, a zero `CDELT1`,
a band with no channels, or no finite sample after averaging.

## `POST /v1/dynspec/rfi_mask`

Flags RFI by robust clipping: a pixel is flagged when it deviates from the
median of its row or column by more than `n_sigma × 1.4826 × MAD` (median and
MAD ignore NaN). Rows or columns with zero MAD are not clipped; NaN pixels are
always flagged. The mask is written as a `uint8` FITS (0 good, 1 flagged) with
the source WCS, `BUNIT = 'flag'`, `NSIGMA`, `AXIS` and a `HISTORY` card, and
registered as a new `dynspec` dataset so it can be shown over the parent.

Request (`DynspecRfiMaskRequest`):

| Field | Default | Notes |
|---|---|---|
| `dataset_id` | required | |
| `n_sigma` | 5.0 | `> 0` |
| `axis` | `"channel"` | `"channel"`: statistics per channel, across time; `"time"`: per time sample, across frequency |
| `output_basename` | `""` | empty gives `<stem>_rfimask_<axis>_<n_sigma with 1 decimal>sigma.fits` |

```json
{ "dataset_id": "ds_1a2b3c4d5e6f", "n_sigma": 4.0, "axis": "time" }
```

Response (`DynspecRfiMaskResponse`): `valid`, `error`, `new_dataset_id`,
`output_path`, `workspace_filename`, `width`, `height`, `flagged_fraction`
(fraction of flagged pixels), `n_sigma`, `axis`.

Fails with `valid: false` for `n_sigma <= 0` or an `axis` other than
`channel` or `time`.
