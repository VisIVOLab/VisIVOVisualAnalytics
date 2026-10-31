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
  "rest_freq_hz": null
}
```
`method`: `null` = both maps (JSON path); `"fwhm"` or `"ew"` selects one (binary path).
`rest_freq_hz`: rest frequency in Hz for velocity-axis conversion (FWHM only).

Response: `valid`, `width`, `height`, `scalar_type`, `fwhm_unit`, `ew_unit`, `bunit`, `fwhm_range_min/max`, `ew_range_min/max`, `fwhm_base64`, `ew_base64`.

### `POST /v1/spectral/linewidth/binary`
Same request. Returns a binary payload: with `method` `"fwhm"` or `"ew"` one frame, otherwise two concatenated frames (FWHM then EW). Each frame is a 12-byte big-endian header of three `uint32` values (rows, columns, flags = 0) followed by little-endian `float32` pixels in row-major order, NaN for masked pixels. Use `BackendClient::parseBinaryFrame()` to decode.

### `GET /v1/spectral/linewidth/{dataset_id}`
Retrieve a previously computed linewidth result by dataset ID.

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

Response: `valid`, `new_dataset_id` (registered in session — use it directly for moment computation), `width`, `height`, `depth`, `range_min/max`, `poly_order`, `method`, `fit_channels`, `rms_before`, `rms_after`, `output_path`.

---

### `POST /v1/spectral/stack`
Stack N open cubes into a single combined spectrum map.

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
`weights`: explicit per-cube weights (used only for `"weighted_mean"`).

Response: `valid`, `rows`, `cols`, `scalar_type`, `method`, `n_cubes`, `range_min/max`, `data_base64`.

### `POST /v1/spectral/stack/binary`
Same request. Returns a single binary frame (same format as `/linewidth/binary`).

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
not a 3-D cube, is 400; a non-cube dataset entry is 422.

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

Response: `valid`, `error`, `n_sources`, `sources`, `mask_path`,
`catalog_path` (empty when SoFiA did not write them). A non-cube dataset is
422; no SoFiA-2 executable is 503; a SoFiA failure is `valid: false` with the
message. A run is limited to `VISIVO_SOFIA_TIMEOUT_S` (default 900 s). The
task-based route [`/v1/tasks/sourcefind`](tasks-finders.md#post-v1taskssourcefind)
can be polled and cancelled and also offers the photutils finder.
