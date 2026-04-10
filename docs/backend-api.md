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
List active dataset sessions.

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
