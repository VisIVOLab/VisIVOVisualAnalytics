# Backend API Reference

Reference for the FastAPI backend (`backend/app/main.py`).
All endpoints require `X-Visivo-Token` header (bearer token).
Dataset-scoped endpoints also require `X-Visivo-Session` (returned by `open` calls).

---

## Auth

```
X-Visivo-Token: <token>          # every request
X-Visivo-Session: <session_id>  # dataset-scoped requests
```

Token written to `~/.visivo_token` at backend startup; read by `BackendClient::readTokenFile()`.

---

## Meta

### `GET /v1/health`
Backend liveness and capacity.
```json
{
  "ok": true,
  "workers": 4,
  "active_sessions": 1,
  "product_cache_entries": 3,
  "product_cache_capacity": 64,
  "task_registry_entries": 0,
  "task_ttl_enabled": true,
  "task_ttl_seconds": 3600
}
```

### `GET /v1/sessions`
Aggregate session-registry statistics.

### `GET /v1/sessions/{session_id}/datasets`
Enumerate the datasets currently open in a backend session. Used by
cross-dataset tools (Spectral Stacking, etc.) to populate selection lists in
the desktop client. Returns `404` if the session does not exist.

```json
{
  "session_id": "anon-abc",
  "datasets": [
    { "dataset_id": "ds_f558b5d90c55", "kind": "cube",
      "path": "/data/cubehi-clean-m31.fits",
      "width": 512, "height": 512, "depth": 200 },
    { "dataset_id": "ds_…", "kind": "cube", "path": "…",
      "width": 512, "height": 512, "depth": 200 }
  ]
}
```

---

## Files

### `GET /v1/files/list?path=<dir>`
List backend-side directory. Returns `FileEntry[]` with `name`, `path`, `type`, `size`, `modified_time`, `is_fits`.

### `POST /v1/files/header`
```json
{ "path": "/data/cube.fits" }
```
Returns FITS header cards as `string[]`.

---

## Datasets

### `POST /v1/datasets/open`
Open a FITS file; registers a session.
```json
{ "path": "/data/cube.fits" }
```
Response includes `dataset_id`, `session_id`, `kind` (`image`|`cube`), dimensions, WCS metadata (`wcs_status`, `wcs_warning_message`, `wcs_sanitized_axes`, `spacing`, `origin`, `ctype[]`, `cunit[]`, `crval[]`, `crpix[]`, `cdelt[]`), the spectral-axis summary (`spectral_axis_type` = the spectral `CTYPE`, `spectral_axis_unit`), beam (`beam_major`, `beam_minor`, `beam_pa`), `bunit`, and:

| Field | Type | Meaning |
|-------|------|---------|
| `rest_freq_hz` | `float \| null` | Header `RESTFRQ`, else `RESTFREQ`, in Hz. `null` when the header carries neither, or carries a non-positive value — a client must be able to tell "no rest frequency" from one it would divide by. Needed to place a rest-frame spectral-line list on a velocity axis, and to convert a frequency axis to velocity. |

---

## Catalogue

### `POST /v1/catalogue/open`
Open a remote CSV file.
```json
{ "path": "/data/catalogue.csv", "format": "csv" }
```
Returns `dataset_id`, `session_id`, field list with types.

### `POST /v1/catalogue/subset`
Legacy: return up to `max_rows` rows without filtering.

### `POST /v1/catalogue/query`
Paginated, filtered query.
```json
{
  "dataset_id": "...",
  "limit": 50000,
  "offset": 0,
  "filters": [
    { "field": "flux", "op": ">", "value": "0.1" }
  ],
  "sort_field": "flux",
  "sort_direction": "desc"
}
```
Response: `total_rows`, `returned_rows`, `rows[]` (key-value maps).

Supported filter operators: `<`, `<=`, `>`, `>=`, `=`, `!=`, `contains`, `startswith`, `endswith`.

---

## VBT

### `POST /v1/vbt/open`
Open a VBT file.
Returns `dataset_id`, `session_id`, `kind` (`point`|`volume`), `fields[]`, `num_rows`, `scalar_type`.

### `POST /v1/vbt/subset`
Legacy bulk fetch (no filter, `max_rows` cap).

### `POST /v1/vbt/query`
Same request shape as `/v1/catalogue/query`.
Response: `total_rows`, `returned_rows`, `field_names[]`, `columns` (base64-encoded float64 column vectors), `num_rows`.

---

## Cube

### `POST /v1/cube/preview`
Downsampled cube for fast initial display.
```json
{ "dataset_id": "...", "downsample": 4 }
```
Returns `scalar_type`, `width`, `height`, `depth`, `range_min`, `range_max` + base64 `data`.

### `POST /v1/cube/slice`
Single 2-D slice.
```json
{ "dataset_id": "...", "axis": "z", "index": 42 }
```

### `POST /v1/cube/subvolume`
Spatial + spectral sub-region.
```json
{ "dataset_id": "...", "x0": 0, "x1": 255, "y0": 0, "y1": 255, "channel_start": 10, "channel_end": 50 }
```

### `POST /v1/cube/pv`
Position-velocity diagram along a polyline.
```json
{
  "dataset_id": "...",
  "points_ra_dec": [[ra1, dec1], [ra2, dec2]],
  "width_pixels": 3
}
```
Returns `num_samples`, `depth`, `scalar_type`, `positions_arcsec_base64`, `total_length`, `valid_samples`, `pixel_scale_arcsec_per_pixel`, `spatial_unit`, `spectral_axis_type`, `spectral_axis_unit`, `bunit`, `beam_major`, `beam_minor`, `beam_pa`.

### `POST /v1/cube/noise`
Noise statistics (MAD and sigma) over a spatial region and channel range.
```json
{ "dataset_id": "...", "x0": 0, "x1": 127, "y0": 0, "y1": 127, "channel_start": 0, "channel_end": 63 }
```
Returns `channel_start`, `channel_end`, `num_channels`, `mad[]`, `sigma[]`, `region`.

---

## Products

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

Response: `valid`, `width`, `height`, `scalar_type`, `range_min`, `range_max`, `spectral_axis_type`, `spectral_axis_unit`, `moment_unit`, `bunit`, `threshold_used` (the resolved threshold, `null` when no mask was applied), `wcs_status`, `wcs_warning_message`, base64 `data`.

**Physical-range guard (M1/M2).** Regardless of masking, the backend blanks M1 pixels outside the sampled spectral axis `[vmin, vmax]` and M2 pixels outside `[0, span²]`. These can only arise from mixed-sign (noise) weights and would otherwise produce non-physical (e.g. super-luminal) values; the guard never removes a legitimate positive-weight voxel.

Supported `order` values and their definitions:

| Order | Name | Formula | Unit |
|-------|------|---------|------|
| 0 | Integrated intensity | Σ(I·dv) | BUNIT·spectral |
| 1 | Intensity-weighted coordinate | Σ(I·v·dv)/M0 | spectral |
| 2 | Intensity-weighted variance | Σ(I·(v−M1)²·dv)/M0 | spectral² |
| 3 | Skewness | μ₃/σ³ | dimensionless |
| 4 | Excess kurtosis | μ₄/σ⁴ − 3 | dimensionless |
| 5 | Standardised 5th moment | μ₅/σ⁵ | dimensionless |
| 6 | RMS | √(Σ(I²·dv)/Σ(dv)) | BUNIT |
| 8 | Maximum value | max(I) | BUNIT |
| 10 | Minimum value | min(I) | BUNIT |

For orders 3–5, `spectral_delta` (channel spacing) is used as the integration weight. NaN/blanked voxels are excluded. Variance must be positive; pixels where σ=0 or M0=0 are set to NaN.

### `POST /v1/products/isosurface`
```json
{ "dataset_id": "...", "threshold": 0.05 }
```
Returns base64-encoded mesh data + bounds.

---

## Astrometry

### `GET /v1/astrometry/crossmatch/{dataset_id}`
Cross-match the image footprint against an external catalogue via `astroquery`.

Query params: `catalogue` (`simbad` | `2MASS` | `NVSS` | `FIRST`, default `simbad`), `radius_arcsec` (default 60), `max_sources` (1–5000, default 500).

Response: `{ valid, error, n_sources, sources, truncated, centre_ra_deg, centre_dec_deg }`, where each source is `{ ra_deg, dec_deg, name, catalogue_id, separation_arcsec }`. The `X-Truncated` header mirrors `truncated`. Sources are cropped to the **valid (finite) image pixels** — those projecting onto the blank/NaN border of a non-rectangular footprint are dropped. When the field exceeds `max_sources`, the retained set is a **uniform spatial sub-sample** across the footprint (queried against a generous upstream row cap) rather than only the sources nearest the image centre, so coverage stays representative. Catalogue column names are resolved case-insensitively (robust across `astroquery` versions), and unparseable rows are skipped rather than failing the whole query. Errors map to HTTP: missing `astroquery` → 503, unknown catalogue → 422.

### `POST /v1/astrometry/reproject/{dataset_id}`
```json
{ "target_wcs": { "...": "FITS WCS header cards" }, "target_shape": [ny, nx], "order": 1 }
```
Reprojects an image plane onto a target WCS/grid, writes a new FITS to the session workspace, and registers it. Response: `{ valid, new_dataset_id, width, height, range_min, range_max, output_path, order }`. Missing `reproject` package → 503.

---

## Tasks (async)

For long-running operations the backend queues a task; the client polls for completion.

### `POST /v1/tasks/moment`
### `POST /v1/tasks/pv`
Same request body as the synchronous equivalents. Returns `{ "task_id": "uuid" }`.

### `GET /v1/tasks/{task_id}`
```json
{ "task_id": "...", "status": "pending"|"running"|"done"|"error", "result": { … } }
```
`result` is populated when `status == "done"`.

### `DELETE /v1/tasks/{task_id}`
Cancel and remove.

`BackendClient::waitForTaskCompletion()` wraps the polling loop with exponential back-off.

---

## Image

### `POST /v1/image/preview`
```json
{ "dataset_id": "...", "max_longest_side": 1024 }
```
Returns a downsampled float32 image (base64) + `width`, `height`, `full_width`, `full_height`, `range_min`, `range_max`, `is_preview`, `preview_scale_factor`, `bunit`. **Out-of-core**: reads only the decimated pixels from the memmap (via `np.ix_`), so a multi-GB mosaic is never fully loaded just to preview it.

### `POST /v1/image/full`
Same, no size cap. **Loads the whole plane into RAM** — use `/v1/image/tile` (or `/v1/image/preview`) for large mosaics.

### `POST /v1/image/tile`
Out-of-core pyramid tile of a 2-D image mosaic — reads only the requested tile
region from the memmap, so multi-GB mosaics can be panned / zoomed without
materialising the full plane (unlike `full` / `preview`).
```json
{ "dataset_id": "...", "level": 0, "tile_x": 0, "tile_y": 0, "tile_size": 256 }
```
- `level` — pyramid level: `0` = full resolution, `L` decimates by `2**L` (nearest-neighbour). Must be `0..num_levels-1`.
- `tile_x` / `tile_y` — tile indices at that level; each tile covers `tile_size` downsampled pixels per axis.
- `tile_size` — output tile edge in pixels (1–4096, default 256).

Response: `width` / `height` (actual tile dims, smaller at edges), `full_width` / `full_height`, `level`, `num_levels`, `tile_x`, `tile_y`, `tile_size`, `range_min` / `range_max`, `compression`, base64 `data`. Out-of-range tile or `level ≥ num_levels` → 422; non-image dataset → 422.

---

## Cosmology

### `POST /v1/cosmology/distance`
Single redshift.
```json
{ "redshift": 0.5, "model": "Planck18" }
```

### `POST /v1/cosmology/distance/batch`
```json
{ "redshifts": [0.1, 0.5, 1.0], "model": "Planck15" }
```
Returns `distances_Mpc[]` in the same order. Supported models: `Planck18`, `Planck15`, `Planck13`, `WMAP9`.

---

## HiPS

### `POST /v1/hips/open`
```json
{ "url": "http://alasky.u-strasbg.fr/DSS/DSS2Merged" }
```
Returns `survey_id`, `order_min`, `order_max`, `frame`, `tile_format`, `ra_center`, `dec_center`, `fov`.

### `GET /v1/hips/{survey_id}/allsky?order=<N>`
AllSky mosaic PNG/JPEG bytes.

### `GET /v1/hips/{survey_id}/tile/{order}/{pix}`
Single HiPS tile bytes.

### `POST /v1/hips/{survey_id}/query_tiles`
Given a viewport (RA/Dec center + FOV), return tile pixel indices at the appropriate order.

### `POST /v1/hips/catalogue_overlay`
Given a HiPS survey viewport, return catalogue sources within the field as `BackendHiPSCatalogueSource[]`.

---

## Spectral

Implemented in `backend/app/routers/spectral.py`. All endpoints require
`X-Visivo-Token` and `X-Visivo-Session`. Three feature blocks:
S-02 (linewidth maps), S-03 (baseline subtraction), S-04 (spectral stacking).

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
Same request. Returns a binary payload with two concatenated frames (FWHM then EW), each prefixed with a 12-byte header (width int32, height int32, n_bytes int32). Use `BackendClient::parseBinaryFrame()` to decode.

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

## SAMP

The SAMP router (`backend/app/routers/samp.py`) is included **without auth dependencies**
(no `X-Visivo-Token`) so a SAMP hub running on the user's machine can reach it directly.

### Messaging

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/v1/samp/send` | Send a SAMP message to one / all peers (legacy generic send). |
| `POST` | `/v1/samp/receive` | Inbound delivery hook used by the bundled hub bridge. |
| `GET`  | `/v1/samp/pending` | Pull pending out-bound messages enqueued by the client. |
| `GET`  | `/v1/samp/inbox` | Pull messages addressed to the running session. |

### Status / hub lifecycle

| Method | Path | Purpose |
|--------|------|---------|
| `GET`  | `/v1/samp/status` | Current hub connection state. |
| `POST` | `/v1/samp/hub-status` | Register the local hub status (heartbeat). |
| `POST` | `/v1/samp/connect` | Bring the session up against the active hub. |

### File transfer / registration

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/v1/samp/files/register` | Register a backend-side FITS file as a SAMP-shareable token. |
| `POST` | `/v1/samp/import-url` | Import a remote URL into the session. |
| `POST` | `/v1/samp/upload-file` | Upload a local FITS to the backend for sharing. |
| `GET`  | `/v1/samp/files/{token}` | Serve a previously registered file by token. |
| `POST` | `/v1/samp/send-fits` | Broadcast a FITS to peers via SAMP. |
| `POST` | `/v1/samp/send-catalogue` | Broadcast a catalogue (votable) to peers. |

---

## Resolve

### `POST /v1/resolve/target`
```json
{ "name": "M87" }
```
Returns `ra_deg`, `dec_deg`, `resolved_name`.

---

## VLKB

### `POST /v1/vlkb/fetch_cutout`
Download a cutout from a *remote* SODA service to the backend's filesystem.
```json
{ "soda_url": "https://…/soda/sync?ID=…&POS=CIRCLE%2040%200%200.3&POSSYS=GALACTIC",
  "vlkb_bearer": "" }
```
Returns `{valid, error, path}` — `path` is on the **backend**, and is what the
dataset-open route is then given.

The URL comes from the client, so it is checked against SSRF before anything is
fetched (`VISIVO_VLKB_ALLOWED_HOSTS`; private, loopback and link-local targets
are refused unless a host is explicitly trusted).

**What came back is checked before it is called a FITS file.** A SODA service
answers an error with HTTP 200 and a VOTable or an HTML page, and mislabels its
Content-Type in both directions — the observed failure carried
`Content-Type: application/fits` on a plain-text error. So the check is on the
bytes:

| First bytes | Reported as |
|-------------|-------------|
| `SIMPLE` | a FITS file; the path is returned |
| `\x1f\x8b` | gzip — ask for it uncompressed |
| `ustar` at offset 257 | a tar archive — this ID needs the multi-cutout endpoint |
| anything else | the service's own message, extracted from the body |

The message is extracted from a VOTable `<INFO value="ERROR">`, an HTML
`<title>`/`<h1>`/`<p>`, or plain text, and is returned as **plain text**: markup
is stripped, because it is written by a remote service and ends up in a dialog.
A response that is not FITS is deleted rather than kept, and a *cached* response
is re-checked for the same reason — a bad file saved before this check would
otherwise be handed back for ever, and re-requesting the same URL is exactly
what a user does after a failure.

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_VLKB_MAX_BYTES` | 8 GiB | Cap on a single cutout download |
| `VISIVO_VLKB_ALLOWED_HOSTS` | *(unset)* | Hosts that skip the private-address check |

---

## SED

Greybody fits of a compact source's photometry, and fits against the VLKB grid
of pre-computed theoretical models. Wavelengths are µm and fluxes Jy throughout
(the units of the Hi-GAL band-merged catalogue the SEDs are built from).

The fit lives here rather than in the desktop client on purpose: the legacy
ViaLactea client shelled out to `python sedfit_main.py` with `eval()`-ed argv
strings, so the result depended on the user's own Python. See
[SED fitting](user-guide/sed-fitting) for the science and the workflow.

### `GET /v1/sed/defaults`
The fit ranges and constants the server suggests, so the client does not carry a
second copy of them.
Returns `mass_range`, `temp_range`, `beta_range`, `lambda0_range`,
`scale_range` (each `[min, max, steps]`), `kappa_ref`, `lambda_ref_um`,
`distance_pc`, `models_service_url`.

### `POST /v1/sed/fit`
Fit a modified blackbody. `model` is `thin`, `thick`, or `both`.
```json
{
  "model": "both",
  "wavelengths_um": [70, 160, 250, 350, 500, 870],
  "fluxes_jy":      [12.5, 40.1, 38.0, 22.4, 11.9, 2.1],
  "errors_jy":      [1.2, 4.0, 3.8, 2.2, 1.2, 0.3],
  "upper_limits":   [false, false, false, false, false, true],
  "distance_pc": 1500,
  "kappa_ref": 0.1,
  "lambda_ref_um": 300,
  "colour_correction": true,
  "size_arcsec": 30.0,
  "mass_range":    [1, 5000, 30],
  "temp_range":    [5, 50, 30],
  "beta_range":    [2, 2, 1],
  "lambda0_range": [10, 300, 30],
  "scale_range":   [1, 1, 1],
  "source_label": "HIGALBM000.0000+00.0000"
}
```

Every range is `[min, max, steps]`; **one step holds that parameter fixed at its
minimum**. Ranges left out fall back to the values `/v1/sed/defaults` reports.
An all-zero `errors_jy` should be omitted rather than sent — it means "no
errors", not "zero uncertainty".

Response: `{valid, error, fits[], best}`. `best` names the model with the lower
reduced χ² (`both` runs two fits; a thick fit that cannot run — no source size —
is skipped rather than failing the request). Each entry of `fits` carries:

| Field | Meaning |
|-------|---------|
| `model` | `thin` or `thick` |
| `parameters` | `mass_msun`, `temperature_k`, `beta`, and for thick `lambda0_um`, `size_arcsec`, `scale`; plus `luminosity_lsun` and `l_over_m` |
| `uncertainties` | Half-width of the Δχ² = 1 profile interval, per parameter |
| `parameter_bounds` | The interval itself, `[lo, hi]` |
| `unbounded` | Parameters whose interval ran off the end of the searched range — their uncertainty is a **lower bound** |
| `at_boundary` | Parameters whose best value sits on the edge of the range |
| `chi2`, `dof` | dof is `N_detections − N_free_parameters` and **can be ≤ 0** |
| `chi2_reduced` | `null` when `dof ≤ 0`: an under-determined fit has no reduced χ². Do not read a missing value as 0 |
| `model_wavelength_um`, `model_flux_jy` | The fitted SED, sampled 5–2000 µm |
| `band_wavelength_um`, `fitted_flux_jy` | The model at the input bands, **in the order they were sent** |
| `luminosity_lsun` | Integrated over the sampled range only, so a lower bound on L_bol |
| `bands_used`, `bands_upper_limit` | How many bands entered the χ², how many vetoed models |
| `warnings` | Free text the caller should surface: unbounded intervals, missing errors, dof ≤ 0, refinement that did not settle |

Upper limits are excluded from the χ² and **veto** any model brighter than the
limit. 422 on an unusable SED: fewer than two detections, a non-positive
distance / opacity / reference wavelength, a non-physical range, or a grid too
large to evaluate.

### `POST /v1/sed/models`
Fit against the VLKB grid of theoretical models. Proxies the VLKB `searchd`
service, whose query is a single underscore-joined positional string.
```json
{
  "wavelengths_um": [70, 160, 250, 350, 500],
  "fluxes_jy": [12.5, 40.1, 38.0, 22.4, 11.9],
  "errors_jy": [1.2, 4.0, 3.8, 2.2, 1.2],
  "flags": [1, 1, 1, 1, 0],
  "distance_pc": 1500,
  "prefilter": 0.0,
  "weight_mid_ir": 1.0, "weight_far_ir": 1.0, "weight_submm": 1.0,
  "delta_chi2": 1.0,
  "service_url": "",
  "max_models": 200
}
```
`service_url` defaults to `VISIVO_VLKB_SEDFIT_URL`. Response:
`{valid, error, columns[], models[], best, total_models, skipped_models,
truncated}` — models sorted by χ², each carrying the service's own physical
columns plus `model_wavelength_um` / `model_flux_jy`. Rows with no usable χ²
are dropped and counted in `skipped_models`; non-finite numbers anywhere in a
row become `null` rather than breaking serialisation.

The URL is validated against SSRF **on every hop**, not only the first: a public
service is otherwise free to answer `302` with a loopback or link-local address.
503 when no service is configured, 400 on a refused URL, 502 when the service
fails or answers something that is not a model table.

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_VLKB_SEDFIT_URL` | *(unset)* | Base URL of the VLKB SED-fit service |
| `VISIVO_VLKB_ALLOWED_HOSTS` | *(unset)* | Comma-separated host allowlist; allowlisted hosts skip the private-address check |
| `VISIVO_SED_MODELS_TIMEOUT` | 300 s | Wall-clock deadline for the whole model-fit request |
| `VISIVO_SED_MODELS_MAX_BYTES` | 64 MiB | Cap on the service's response |
| `VISIVO_SED_MAX_MODELS` | 2000 | Server-side cap on `max_models` |

---

## Simulations

AREPO snapshots: browse one, cost an extraction, run it. See
[Simulation snapshots](user-guide/simulations).

### `POST /v1/simulations/arepo/inspect`
`{path}` → `{valid, error, path, tree, box_size_kpc, unit_length_cm,
particle_types, tree_truncated, yt_available}`. `tree` is nested
`{name, kind, shape, dtype, attributes, children}`. Needs only h5py, so it works
on a backend that cannot run an extraction — `yt_available` says which that is.
External links are named, never followed; the walk is bounded in depth and node
count and reports `tree_truncated`.

### `POST /v1/simulations/arepo/plan`
`{path, level, window_kpc, offset_kpc}` → the grid the extraction *would*
produce: `{shape, cell_size_pc, window_kpc, extent_kpc, offset_kpc,
domain_width_kpc, level, base_dimensions, cells, bytes_estimate}`.

A covering-grid cell is `domain / (base · 2^level)` — the level fixes the
resolution and the window only says how many cells to take — so `extent_kpc`
(what will be extracted) differs from `window_kpc` (what was asked for) by the
rounding to whole cells. 422 on a non-physical window, a window larger than the
box, or a grid past the cell limit.

### `POST /v1/simulations/arepo/extract`
`{path, level, particle, fields, window_kpc, offset_kpc}` →
`{valid, error, plan, outputs: [{field, particle, path, bunit, shape}],
domain_width_kpc}`. One FITS per field in a directory of its own, so re-running
with a different window cannot overwrite an earlier result.

Each cube carries a linear WCS in parsec centred on the window, `BUNIT` from
yt's units, and the provenance of the extraction (`AREPOSRC`, `AREPOLVL`,
`AREPOPRT`, `AREPOFLD`, `AREPOW*` extracted extent, `AREPOR*` requested window,
`AREPOO*` offset).

503 when `yt` is not installed; 422 for a cosmological snapshot (comoving
coordinates are not handled), a non-scalar field, or an unusable grid. The route
goes through the heavy-task throttle, so several extractions cannot saturate the
backend.

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_AREPO_MAX_CELLS` | 600,000,000 | Largest grid an extraction may materialise |
| `VISIVO_AREPO_MAX_LEVEL` | 14 | Highest refinement level accepted |

---

## Error convention
All endpoints return `{ "valid": false, "error": "<message>" }` on failure.
HTTP status is typically 200 even for logical errors; the client checks `valid`.

---

## Worker pool & heavy-task throttle

CPU-bound work runs in a single process pool of `VISIVO_WORKERS` (default 4)
workers. To keep the GUI responsive while a long compute is running, endpoints
are classified into two tiers:

| Tier | Helper | Examples |
|------|--------|----------|
| **Interactive** | `_run` / `_run_with_limit` | `/v1/cube/preview`, `/slice`, `/subvolume`, `/noise`, `/image/*` |
| **Heavy** | `_run_heavy` / `_run_heavy_with_limit` | `/v1/products/moment`, `/products/isosurface`, `/v1/cube/pv`, `/v1/spectral/*` |

Heavy invocations are gated by a global `asyncio.Semaphore` (`_HEAVY_SEM`)
with capacity `VISIVO_HEAVY_SLOTS` — defaulting to `max(1, VISIVO_WORKERS-1)`
— so the pool is never fully saturated by long jobs and an interactive
request (slice scroll, ROI subvolume, probe) always finds a free worker.

Tunables:

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_WORKERS` | 4 | Total ProcessPoolExecutor workers |
| `VISIVO_HEAVY_SLOTS` | `max(1, WORKERS-1)` | Max concurrent heavy tasks (rest of the pool stays available for interactive requests) |
| `VISIVO_LINEWIDTH_CHUNKS` | `VISIVO_WORKERS` | Row-chunks the linewidth compute is split into (already throttled by `_HEAVY_SEM`) |
| `VISIVO_LINEWIDTH_SNR` | 3.0 | SNR cutoff for skipping background pixels in linewidth fit (0 disables skip) |
| `VISIVO_MOMENT_STREAM_BYTES` | 2 GiB | Materialised-subset size above which M0/M1/M2 stream in spectral slabs (out-of-core) instead of loading the whole range |
| `VISIVO_MOMENT_SLAB_CHANNELS` | 64 | Channels per slab in the streaming moment path |
| `VISIVO_DASK_MODE` | `auto` | Moment compute backend: `auto` (distributed if a scheduler is set, else local threaded Dask for large jobs), `off` (plain worker), `local` (always local threaded Dask), `distributed` (require a cluster) |
| `VISIVO_DASK_MIN_BYTES` | 512 MB | In `auto`, minimum materialised-subset size before a moment is routed to the local threaded Dask path |
| `VISIVO_DASK_SCHEDULER` | *(unset)* | Address of a `dask.distributed` scheduler; when set, moments fan out across the cluster |
