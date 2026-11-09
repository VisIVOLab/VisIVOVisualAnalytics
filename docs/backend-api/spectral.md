# Spectral

Implemented in `backend/app/routers/spectral.py`. All endpoints require
`X-Visivo-Token` and, except `/v1/spectral/linefit`, `X-Visivo-Session`. Line-width maps, baseline subtraction, spectral stacking, spectrum extraction,
line fitting and source finding.

## Line width, baseline and stacking

### `POST /v1/spectral/linewidth`
Per-pixel line-width map. Returns both FWHM (Gaussian fit) and equivalent-width maps as base64 float32.

```json
{
  "dataset_id": "...",
  "channel_start": 10,
  "channel_end": 50,
  "mask_enabled": false,
  "threshold_value": 0.0,
  "method": null,
  "rest_freq_hz": null,
  "fast": false
}
```
`dataset_id` is required; the other fields default to 0, `false`, `null`.
`method`: ignored by the JSON route, which always returns both maps; on the binary route `"fwhm"` or `"ew"` selects one.
`rest_freq_hz`: rest frequency in Hz for velocity-axis conversion (FWHM only).
`fast`: a vectorised moment-dispersion FWHM instead of the per-pixel Gaussian fit, much faster on dense bright cubes.

A non-cube dataset is 422; a failed computation is `valid: false` with the reason.

Response: `valid`, `error`, `width`, `height`, `scalar_type`, `fwhm_unit`, `ew_unit`, `bunit`, `fwhm_range_min/max`, `ew_range_min/max`, `fwhm_base64`, `ew_base64`.

### `POST /v1/spectral/linewidth/binary`
Same request. Returns a binary payload: with `method` `"fwhm"` or `"ew"` one frame, otherwise two concatenated frames (FWHM then EW). Each frame is a 12-byte big-endian header of three `uint32` values (rows, columns, flags = 0) followed by little-endian `float32` pixels in row-major order, NaN for masked pixels. Use `BackendClient::parseBinaryFrame()` to decode. The units ride in the headers `X-Fwhm-Unit` and `X-Ew-Unit`, plus `X-Map-Unit` for a single-frame answer. A failed computation is 500.

### `GET /v1/spectral/linewidth/{dataset_id}`
One line-width map as a single binary frame, with the same unit headers. Query: `method` (`"fwhm"`, the default, or `"ew"`), `rest_freq_hz`, `fast`, `channel_start`, `channel_end` (0, 0); no threshold mask. It shares the cache with the POST routes, so a map computed there is not computed again.

---

### `POST /v1/spectral/baseline/{session_id}/{dataset_id}`
Fit and subtract a per-pixel polynomial or median baseline from a spectral cube.

```json
{
  "session_id": "...",
  "dataset_id": "...",
  "channel_start": 0,
  "channel_end": 0,
  "poly_order": 1,
  "method": "polynomial",
  "line_free_channels": [[5, 15], [80, 120]]
}
```
`method`: `"polynomial"` or `"median"`.
`line_free_channels`: flat list `[5, 6, 7]` or list of ranges `[[5, 15], [80, 120]]`.
`session_id` and `dataset_id` are required in the body; the cube is the body's `dataset_id`, looked up in the session of the `X-Visivo-Session` header (the path segments are not used). A non-cube dataset is 422; a failed fit is `valid: false`.

Response: `valid`, `error`, `new_dataset_id` (registered in session — use it directly for moment computation), `width`, `height`, `depth`, `range_min/max`, `poly_order`, `method`, `fit_channels`, `rms_before`, `rms_after`, `output_path`, `warning`.

---

### `POST /v1/spectral/stack`
Stack N open cubes into one spectrum: each cube is averaged over its pixels, and the N mean spectra are combined channel by channel. The cubes must share their spatial shape and channel count.

```json
{
  "dataset_ids": ["...", "..."],
  "method": "mean",
  "weight_by": "uniform",
  "weights": []
}
```
`method`: `"mean"` (default), `"median"`, `"weighted_mean"`.
`weight_by`: `"uniform"`, `"rms"`, `"peak"` (used when `method="weighted_mean"` and `weights` is empty).
`weights`: explicit per-cube weights (used only for `"weighted_mean"`); when given there must be one per cube, finite, non-negative and not all zero, otherwise the response is `valid: false`.

Blank (NaN) channels are left out channel by channel: `"mean"` and `"median"` use the cubes that have a value there, and `"weighted_mean"` divides by the weights of those cubes only, so a channel one cube does not cover is not pulled towards zero. A channel no cube covers (or whose only values weigh zero) is NaN.

Response: `valid`, `error`, `rows` (1), `cols` (channels), `scalar_type`, `method`, `n_cubes`, `range_min/max`, `data_base64`. A dataset that is not a cube is 422; any other failure is `valid: false`.

### `POST /v1/spectral/stack/binary`
Same request. Returns a single binary frame of 1 × channels (same format as `/linewidth/binary`); a failure is 500.

---

## Profiles and line fitting

Spectra are extracted on the node, so the profiler works on multi-GB remote
cubes. The spectral axis is the cube's third axis, converted to world values
through the WCS when it has a spectral axis, otherwise channel numbers with
`spectral_unit: "pix"` and `spectral_type: "CHANNEL"`. Non-finite values come
back as `null`.

### `POST /v1/spectral/probe`
The spectrum at one pixel.

```json
{ "dataset_id": "...", "x": 120.0, "y": 88.0 }
```

`x`, `y` are 0-based pixels, rounded to the nearest integer. Response:
`valid`, `error`, `x`, `y`, `spectral_axis`, `spectral_unit`,
`spectral_type`, `values`. A pixel outside the image, or a dataset that is
not a 3-D cube, is 400; a non-cube dataset entry is 422. The desktop client
reads the live spectrum and Pin Spectrum of a cube opened as a preview from
this route (one request at a time, at the latest pixel), since the decimated
preview does not hold the full-resolution spectra.

### `POST /v1/spectral/aperture`
The mean spectrum over a circular aperture (NaN-aware mean per channel).

```json
{ "dataset_id": "...", "x": 120.0, "y": 88.0, "radius": 3.0 }
```

`radius` is in pixels (default 3). Response: `valid`, `error`, `x`, `y`,
`radius`, `n_pix`, `spectral_axis`, `spectral_unit`, `spectral_type`,
`values`. An aperture that covers no pixel is 400.

### `POST /v1/spectral/linefit`
Fit one Gaussian, plus an optional linear baseline, to a spectrum the caller
supplies. Stateless: no dataset or session is involved, so the same fit
serves probes, aperture spectra and any external profile.

```json
{ "values": [0.01, 0.02, 0.35, 0.81, 0.33, 0.03],
  "axis":   [1500, 1510, 1520, 1530, 1540, 1550],
  "with_baseline": true }
```

At least 4 finite samples are needed; `values` and `axis` of different length,
or a fit that does not converge, is 422. Response, in the axis units:
`peak`, `center`, `sigma`, `fwhm`, `integrated_flux` (amp·σ·√(2π)), each with
an `_err` companion (`null` when undefined), `continuum` (baseline at the line
centre), `equivalent_width` (`null` without a baseline or with zero
continuum), `baseline` (`[a, b]` of `a + b·x`), `rms` of the residuals,
`n_points`, `spectral_correlation_factor` and `spectral_correlation_note`.
The errors are inflated by the measured residual autocorrelation factor
(1.0 for independent channels); the note is non-empty when the factor is
2.25 or more, warning that the cause may be a poor model rather than
correlated channels.

## Source finding (synchronous)

### `POST /v1/sourcefind/{dataset_id}`
Run SoFiA-2 on a cube and wait for the result. Body: `{sofia_params}`
(SoFiA parameters as a dictionary; a `dataset_id` field is accepted and
ignored, the path parameter is used).

The parameters pass the same checks as the task route's (allowed sections,
SoFiA-2 2.7 names, no file paths, no line breaks: see
[`/v1/tasks/sourcefind`](tasks-finders.md#post-v1taskssourcefind)); a refused
set is answered `valid: false` with the reason, before SoFiA-2 starts.

Response: `valid`, `error`, `n_sources`, `sources`, `mask_path`,
`catalog_path` (empty when SoFiA did not write them), `exit_code`, and
`summary` (the reliability counts read from SoFiA's output, as in the task
route). A non-cube dataset is 422; no SoFiA-2 executable is 503. SoFiA's exit
8 ("No sources left after filtering") is `valid: true` with `n_sources` 0;
any other SoFiA failure is `valid: false` with SoFiA's `ERROR:` messages and
the end of its output. A run is limited to `VISIVO_SOFIA_TIMEOUT_S` (default 900 s). The
task-based route [`/v1/tasks/sourcefind`](tasks-finders.md#post-v1taskssourcefind)
can be polled and cancelled and also offers the photutils finder.
