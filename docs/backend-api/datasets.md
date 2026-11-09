# Datasets, catalogues and VBT

Opening data: FITS and HDF5 datasets (from a backend path, a URL or a SKAVA
access URL), node-side cutouts, CSV catalogues and VBT tables.

## Datasets

### `POST /v1/datasets/open`
Open a FITS file; registers a session.
```json
{ "path": "/data/cube.fits" }
```
`path` may also name a VBT volume (its data file or its `.head`), opened as a
cube of its first field. Any other file type is answered with `valid: false`
("Unsupported file type. Expected FITS (.fits, .fit) or HDF5 (.h5, .hdf5,
.bfdata)."); a gzip-compressed `.fits.gz` is not accepted here.
Response includes `dataset_id`, `session_id`, `kind` (`image`|`cube`), dimensions, WCS metadata (`wcs_status`, `wcs_warning_message`, `wcs_sanitized_axes`, `spacing`, `origin`, `ctype[]`, `cunit[]`, `crval[]`, `crpix[]`, `cdelt[]`), the spectral-axis summary (`spectral_axis_type` = the spectral `CTYPE`, `spectral_axis_unit`), beam (`beam_major`, `beam_minor`, `beam_pa`), `bunit`, and:

| Field | Type | Meaning |
|-------|------|---------|
| `rest_freq_hz` | `float \| null` | Header `RESTFRQ`, else `RESTFREQ`, in Hz. `null` when the header carries neither, or carries a non-positive value — a client must be able to tell "no rest frequency" from one it would divide by. Needed to place a rest-frame spectral-line list on a velocity axis, and to convert a frequency axis to velocity. |

The other response fields: `valid`, `error`, `active_axes`,
`degenerate_axes_summary`, `file_size_bytes`, `full_res_threshold_bytes` and
`recommended_load_mode` (`auto_full` or `preview_only`: a file at or above the
threshold should open as a preview first), and
`server_side_rendering_available` (this backend can host a
[render session](render.md) for the cube). For a multi-field source (a VBT
volume) `field_name` is the field opened and `fields` lists every field as
`{name, path}`, `path` being what `/v1/datasets/open` takes to open that field. The same response is returned by the
three routes below.

The open routes take the session from `X-Visivo-Session`; the reply carries the
`session_id` the client must echo from then on.

**Truncated FITS files.** Before a FITS file is opened, its size on disk is
compared with what its headers declare: every header and every HDU's data with
its padding, from the start of the file up to and including the image HDU that
is read. A shorter file is refused on all three open routes with **HTTP 422**
and the reason as `detail`, rather than opening and failing at the first read
with numpy's "buffer is too small for requested array":

```json
{ "detail": "The file Eridanus.fits is truncated: 697.9 GB on disk, 961.4 GB declared (5644 of 7776 planes present)." }
```

The parenthesis says how much is there: planes of a cube (every axis beyond
the second counts, degenerate ones included), rows of a 2-D image, or which HDU
is incomplete when that cannot be counted (a tile-compressed image, a table, an
HDU before the image). A file cut inside a header says so instead: "The file
x.fits is truncated: 1.0 kB on disk, ending inside the header of HDU 0." Sizes
are decimal (kB, MB, GB, TB); when both round to the same figure they are given
in bytes.

The check reads header blocks and `os.stat` only, never the data, so it costs
the same on a 1 TB cube. A tile-compressed image is sized as the binary table
it is stored in (rows plus heap, `PCOUNT`), and random groups and `GCOUNT` are
followed as the FITS standard has them. Not refused: bytes after the declared
end; a file cut only inside the last 2880-byte padding block (all the data is
there, and astropy reads it); a truncated HDU after the one being read. A
gzip-compressed FITS (`.fits.gz`) is checked once decompressed (below); a
truncated gzip stream is refused there. All other open failures still answer
200 with `valid: false`.

**Compressed FITS.** `.fits.gz` (`.fit.gz`, `.fts.gz`) and tile-compressed
files (fpack, `.fz`, or any FITS whose image HDU is a `ZIMAGE` binary table)
open like their uncompressed versions. Neither can be memory-mapped, so the
first open decompresses the file once into the workspace cache
(`VISIVO_DECOMPRESS_CACHE_DIR`, see [Configuration](../configuration.md)) as an
ordinary FITS file, and every reader (previews, slices, moments, spectra, the
brick pyramid, render sessions, SODA) reads that copy. Tile-compressed images
become image HDUs (an fpack primary, `ZSIMPLE = T`, becomes the primary again,
as funpack does); other HDUs are copied byte for byte. The copy keeps the
source's name without the compression suffix (`cube.fits.gz` → `cube.fits`), so
products are named as from the uncompressed file. Opening the same file again
finds the copy (keyed by path, size and modification time). The dataset keeps
the original path: the session entry has it as `source_path`, and provenance
(`/v1/reproducibility/provenance`, product provenance) names it, never the
cache. Lossy fpack quantisation of floats (`fpack` default, `-q 4`) is
already in the file; the copy holds exactly what cfitsio/astropy decompress.

A large `.fits.gz` takes a while to decompress (about 5 s for 2 GB on an
M-series laptop, warm disk): `/v1/datasets/open` does it in one request, off
the event loop; [`POST /v1/tasks/decompress`](tasks-finders.md) does it as a
task with progress and cancellation, after which the open is a cache hit. The
desktop client uses the task, and for a cached copy the task's create response
already says `completed`, so a reopen costs what a plain open does. The
headers of a `.fits.gz` (for the classifier and the size estimate) are read
off the start of the gzip stream, without decompressing the rest of it. Refusals, as `valid: false` with the reason: a truncated
gzip stream ("x.fits.gz is truncated: the gzip stream ends before its end
marker …"), a damaged gzip or tile-compressed file, a gzip that does not hold a
FITS file, a cache disk without room, a cache whose limit cannot be met; a
gzip of a truncated FITS is refused with HTTP 422 like any truncated FITS. If
the cached copy is deleted while the dataset is open, the next request on it
answers **503** with `Retry-After` while the copy is made again.

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
and the reason. A truncated download is refused with 422 (see above) and
removed from the cache, so opening the URL again downloads it again.

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
roots. Failures are answered with `valid: false`, except a truncated FITS
file: 422, as above.

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
access URL, unusable cutout parameters), 415 (not a supported FITS dataset),
422 (a truncated FITS file, with the message of the open routes).
This route needs the token but no session.

---

## Catalogue

### `POST /v1/catalogue/open`
Open a catalogue file on the backend.
```json
{ "path": "/data/catalogue.csv", "format": "csv" }
```
`format` is `csv` (default), `votable`, `fits`, `ipac`, `speck` or `vbt`.
`paths` (optional) opens several files of the same format, with identical
columns, as one catalogue; `path` is then only a label.
Returns `dataset_id`, `session_id`, `num_rows`, the field list with types
(`fields`) and the detected sky-coordinate columns (`coordinate_fields`).
A missing file or one that does not parse is answered with `valid: false`.

### `POST /v1/catalogue/subset`
Legacy: return up to `max_rows` (default 100000) rows without filtering.

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
  "sort": { "field": "flux", "direction": "desc" }
}
```
Response: `total_rows`, `returned_rows`, `rows[]` (key-value maps), and
`row_index[]`: for each returned row, its 0-based position in the catalogue as
opened, before any filter, sort or sample (empty if the table's index cannot
say). SAMP row messages use these numbers.

`"even_sample": true` (with `"phase"`, default 0): when more rows match than
`limit`, one in every `step` of them from the `phase`-th, instead of a page;
`offset` then does not apply and the response gives `step`
(⌈`total_rows` / `limit`⌉). The phases 0 … `step` − 1 together are every
matching row once. The desktop loads a point table this way: a simulation's
table is stored by region of space, so its first rows are a few blocks of it,
while an even sample covers it all.

Supported filter operators: `<`, `<=`, `>`, `>=`, `=`, `!=` (numeric on a numeric
column, string comparison otherwise), `contains` (case-insensitive substring)
and `in` (comma-separated values). An unknown field or operator is answered
with `valid: false`.

---

## VBT

### `POST /v1/vbt/open`
Open a VBT file.
Returns `dataset_id`, `session_id`, `kind` (`point`|`volume`), `fields[]`, `num_rows`, `scalar_type`.

### `POST /v1/vbt/subset`
Legacy bulk fetch (no filter, `max_rows` cap).

### `POST /v1/vbt/query`
Same request shape as `/v1/catalogue/query`.
Response: `total_rows`, `returned_rows`, `fields[]`, `columns` (base64-encoded float64 column vectors), `num_rows`.
