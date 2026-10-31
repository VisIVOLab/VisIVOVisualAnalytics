# Datasets, catalogues and VBT

Opening data: FITS and HDF5 datasets (from a backend path, a URL or a SKAVA
access URL), node-side cutouts, CSV catalogues and VBT tables.

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

The other response fields: `valid`, `error`, `active_axes`,
`degenerate_axes_summary`, `file_size_bytes`, `full_res_threshold_bytes` and
`recommended_load_mode` (`auto_full` or `preview_only`: a file at or above the
threshold should open as a preview first), and
`server_side_rendering_available` (this backend can host a
[render session](render.md) for the cube). The same response is returned by the
three routes below.

The open routes take the session from `X-Visivo-Session`; the reply carries the
`session_id` the client must echo from then on.

### `POST /v1/datasets/open_url`
Download a FITS or HDF5 file from an `http(s)` URL into the backend's cache and
open it as a session dataset. Used after a SKAVA discovery, when the dataset
lives on another node.

```json
{ "url": "https://node.example.org/data/cube.fits",
  "obs_id": "", "bearer_token": "" }
```

| Field | Meaning |
|---|---|
| `url` | `http://` or `https://` address of the file |
| `obs_id` | SKAVA observation id; used only to name the cached file |
| `bearer_token` | Sent to the remote node as `Authorization: Bearer …` |

The file is cached under `<workspace exports>/skava_cache/`, keyed by a hash
of the URL, so the same URL is downloaded once; it is streamed to disk in
8 MiB chunks with a 600 s timeout. The URL goes through the SSRF check:
when `VISIVO_OPENURL_ALLOWED_HOSTS` is set, only the listed hosts are
accepted; otherwise private addresses are refused only when
`VISIVO_OPENURL_BLOCK_PRIVATE=1` (federated nodes often sit on private
networks). A refused URL, a remote
HTTP error or a file that does not classify is answered with `valid: false`
and the reason.

### `POST /v1/datasets/open_skava`
Open a dataset in place on this node, from the `file://` access URL a SKAVA
DataLink returned ("compute next to data": nothing is copied).

```json
{ "obs_id": "ivo://…", "access_url": "file:///data/survey/cube.fits" }
```

`obs_id` is required and recorded with the dataset. `access_url` must be
`file://` with no host (or `localhost`); an `http(s)` URL is refused with a
pointer to `/v1/datasets/open_url`. When `VISIVO_DATA_ROOT` or
`VISIVO_DATA_ROOTS` is set, the resolved path must lie inside one of those
roots. Failures are answered with `valid: false`.

### `POST /v1/datasets/cutout`
Cut a FITS subset out of a node-local dataset. This is what SKAVA's SODA
service delegates to, and what the desktop's SODA subset action calls.

```json
{ "obs_id": "ivo://…", "access_url": "file:///data/survey/cube.fits",
  "pos": "CIRCLE 83.82 -5.39 0.1", "band": "0.2106 0.2112",
  "time": null, "responseformat": "application/fits",
  "save_to_workspace": true }
```

| Field | Meaning |
|---|---|
| `obs_id`, `access_url` | As for `open_skava`; the same path rules apply |
| `pos` | SODA position: `CIRCLE <ra> <dec> <radius>` or `RANGE …`, degrees |
| `band` | `<min> <max>` wavelength in metres |
| `time` | Accepted, not a sliceable axis |
| `save_to_workspace` | `false` (default): the FITS is the response body. `true`: the cutout is written under `<workspace exports>/soda_cutouts/` and the reply is JSON |

With `save_to_workspace: false` the response is `application/fits` with a
`Content-Disposition` filename and an `X-Soda-Applied` header naming the
constraints that were applied. With `true` it is
`{valid, error, obs_id, path, applied}`, `path` being the backend file to open
with `/v1/datasets/open`. Errors are HTTP: 400 (missing `obs_id`, refused
access URL, unusable cutout parameters), 415 (not a supported FITS dataset).
This route needs the token but no session.

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
