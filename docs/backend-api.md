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
Response includes `dataset_id`, `session_id`, `kind` (`image`|`cube`), dimensions, WCS metadata (`wcs_status`, `wcs_warning_message`, `wcs_sanitized_axes`, `spacing`, `origin`, `ctype[]`, `cunit[]`, `crval[]`, `crpix[]`, `cdelt[]`).

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
{ "dataset_id": "...", "order": 0, "channel_start": 10, "channel_end": 50, "rms": 0.002 }
```
Response: `valid`, `width`, `height`, `scalar_type`, `range_min`, `range_max`, `spectral_axis_type`, `spectral_axis_unit`, `moment_unit`, `bunit`, `wcs_status`, `wcs_warning_message`, base64 `data`.

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
Returns RGBA PNG bytes (base64) + `width`, `height`, `range_min`, `range_max`, `bunit`.

### `POST /v1/image/full`
Same, no size cap.

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
