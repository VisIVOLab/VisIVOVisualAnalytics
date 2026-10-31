# Products

Derived products: moment maps, PV and moment FITS files, isosurfaces, and the
2-D image products (pixel arithmetic, mosaics, figures, quality metrics). The
written files land in the Workspace Exports directory (see
[Exports](exports.md)).

## Moments, PV and isosurfaces

### `POST /v1/products/moment`
Synchronous moment map computation (small datasets / fast moments).
```json
{
  "dataset_id": "...", "moment_order": 1,
  "channel_start": 10, "channel_end": 50,
  "mask_enabled": true, "threshold_value": 0.0, "threshold_auto": true
}
```
- `mask_enabled` (default `false` at the API; the desktop client defaults it to `true`) — only voxels above the threshold contribute.
- `threshold_auto` (default `false`) — when set with `mask_enabled`, the backend derives the threshold from the data as `median + 3·σ` (σ from the MAD), ignoring `threshold_value`. The estimate is strided in all three axes so it stays out-of-core on large cubes.
- `threshold_value` — the manual threshold, used only when `mask_enabled` and not `threshold_auto`.
- `moment2_variance` (default `false`) — for order 2, return the variance σ² instead of the dispersion σ. Ignored for other orders.

Response: `valid`, `width`, `height`, `scalar_type`, `range_min`, `range_max`, `spectral_axis_type`, `spectral_axis_unit`, `moment_unit`, `bunit`, `threshold_used` (the resolved threshold, `null` when no mask was applied), `wcs_status`, `wcs_warning_message`, base64 `data`.

**Physical-range guard (M1/M2).** Regardless of masking, the backend blanks M1 pixels outside the sampled spectral axis `[vmin, vmax]` and M2 pixels outside `[0, span²]`. These can only arise from mixed-sign (noise) weights and would otherwise produce non-physical (e.g. super-luminal) values; the guard never removes a legitimate positive-weight voxel.

Supported `order` values and their definitions:

| Order | Name | Formula | Unit |
|-------|------|---------|------|
| 0 | Integrated intensity | Σ(I·dv) | BUNIT·spectral |
| 1 | Intensity-weighted coordinate | Σ(I·v·dv)/M0 | spectral |
| 2 | Velocity dispersion σ (default) or intensity-weighted variance | √(Σ(I·(v−M1)²·dv)/M0), or without the root when `moment2_variance` is `true` | spectral (σ), spectral² (variance) |
| 3 | Skewness | μ₃/σ³ | dimensionless |
| 4 | Excess kurtosis | μ₄/σ⁴ − 3 | dimensionless |
| 5 | Standardised 5th moment | μ₅/σ⁵ | dimensionless |
| 6 | RMS | √(Σ(I²·dv)/Σ(dv)) | BUNIT |
| 8 | Maximum value | max(I) | BUNIT |
| 10 | Minimum value | min(I) | BUNIT |

For orders 3–5, `spectral_delta` (channel spacing) is used as the integration weight. NaN/blanked voxels are excluded. Variance must be positive; pixels where σ=0 or M0=0 are set to NaN.

### `POST /v1/products/pv_fits`
Compute the PV diagram along a path and save it as a 2-D FITS file in the
Workspace Exports.
```json
{ "dataset_id": "...", "vertices": [[12.5, 40.0], [130.0, 90.0]], "width_pixels": 3,
  "output_basename": "ngc3137_pv.fits" }
```
Axis 1 is `OFFSET` (arcsec, 0 at the first vertex, `CDELT1` = step); axis 2 is
the cube's own spectral axis WCS (CTYPE/CUNIT/CRPIX/CRVAL, CDELT×PC or CD,
`PVn_m`/`PSn_m`). `BUNIT`, beam, `RESTFRQ`/`RESTWAV`, `SPECSYS` are copied;
`PVWIDTH`, `PVSTEP` and the vertices (`HISTORY`, pixel and sky) are recorded.
An existing file is never overwritten (the name is suffixed). Response:
`valid`, `error`, `output_path`, `workspace_filename`, `width` (samples),
`height` (channels), `range_min`, `range_max`, `bunit`.

### `POST /v1/products/isosurface`
```json
{ "dataset_id": "...", "threshold": 0.05 }
```
Returns base64-encoded mesh data + bounds.

### `POST /v1/products/moment_fits`
Compute a moment and save it as a 2-D FITS file in the Workspace Exports. The
request is the `/v1/products/moment` request plus `output_basename`:

```json
{ "dataset_id": "...", "moment_order": 0, "channel_start": 10, "channel_end": 50,
  "mask_enabled": true, "threshold_value": 0.0, "threshold_auto": true,
  "moment2_variance": false, "output_basename": "" }
```

An empty `output_basename` becomes `<source stem>_m<order>.fits`; an existing
name gets a numeric suffix. The file carries the celestial WCS and a `BUNIT`
derived from the cube. Response: `valid`, `error`, `output_path`,
`workspace_filename`, `width`, `height`, `range_min`, `range_max`, `bunit`,
`moment_unit`. A non-cube dataset is 422; a compute failure is `valid: false`.
Runs in the heavy tier (see [Worker pool](../backend-api.md#worker-pool-and-heavy-task-throttle)).

## Advanced 2-D products

Four routes over 2-D images (the first plane of a cube when a cube is given;
degenerate axes are squeezed). Failures come back as `valid: false` with the
reason; an unknown dataset is 404.

### `POST /v1/products/map_math`
Pixel-wise arithmetic on one or two maps, written to a FITS file in the
Workspace.

```json
{ "dataset_id": "ds_a", "op": "spectral_index", "dataset_id_b": "ds_b",
  "freq_a": 1.4e9, "freq_b": 3.0e9, "output_basename": "" }
```

| `op` | Needs | Result |
|---|---|---|
| `log10`, `abs`, `square` | map A | applied to A |
| `add`, `subtract`, `multiply` | A and B | A + B, A − B, A × B |
| `divide`, `ratio` | A and B | A / B |
| `spectral_index` | A, B, `freq_a`, `freq_b` (Hz, positive) | α = log10(A/B) / log10(ν_a/ν_b) |

Map B is reprojected onto A's grid (`reproject_interp`) when the shapes
differ. The output keeps A's header; `BUNIT` is cleared for `divide`, `ratio`
and `spectral_index`, and a `HISTORY` card names the operation. The default
file name is `<A stem>_<op>.fits`. Response (`ProductFitsResponse`):
`valid`, `error`, `output_path`, `workspace_filename`, `width`, `height`,
`range_min`, `range_max`, `info` (`{op}`).

### `POST /v1/products/mosaic`
Reproject two or more images onto a common optimal celestial WCS and co-add
them.

```json
{ "dataset_ids": ["ds_1", "ds_2", "ds_3"], "weight_mode": "noise",
  "output_basename": "mosaic.fits" }
```

`weight_mode: "noise"` (default) weights each input by 1/σ², σ being its
MAD-based robust standard deviation; `"uniform"` weights them equally. Fewer
than two datasets is 422. The response has the same shape as `map_math`, with
`info` = `{n_inputs, weight_mode, coverage_fraction}` (fraction of output
pixels covered by at least one input).

### `POST /v1/products/publication_figure`
Render an image as a figure file with a WCS sky grid and a colour bar labelled
with `BUNIT`.

```json
{ "dataset_id": "...", "cmap": "inferno", "stretch": "asinh", "percent": 99.5,
  "title": "", "add_grid": true, "add_colorbar": true, "dpi": 200,
  "output_basename": "field.pdf" }
```

`stretch` is `linear`, `log`, `sqrt`, `asinh` or `power` (astropy
`simple_norm`, clipped at `percent`). The format follows the extension of
`output_basename` (`.png`, `.pdf`, `.svg`; anything else gets `.png`
appended); the default name is `<stem>_figure.png`. Without a celestial WCS the
axes are in pixels. Response: `valid`, `error`, `output_path`,
`workspace_filename`, `width`, `height` (image pixels), `has_wcs`.

### `POST /v1/products/image_quality`
Quality and artefact metrics for an image. Request: `{dataset_id}`.
Response: `{valid, error, metrics}` with

| Metric | Meaning |
|---|---|
| `total_pixels`, `blanked_fraction` | Pixel count, fraction that is NaN or infinite |
| `zero_fraction`, `negative_fraction` | Among finite pixels |
| `min`, `max`, `mean`, `median`, `robust_std` | `robust_std` = 1.4826 × MAD |
| `dynamic_range` | `max / robust_std` |
| `negative_bowl` | `min < −8 × robust_std` |
| `striping_metric` | Ratio of the larger to the smaller variance of row means and column means |
| `flags` | Any of `high_blanked_fraction` (> 0.2), `negative_bowl`, `striping` (metric > 5), `low_dynamic_range` (< 5), `all_blanked` |
