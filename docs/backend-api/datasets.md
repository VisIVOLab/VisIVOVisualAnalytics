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
cube of its first field. A compressed FITS (`.fits.gz`, `.fz`) is accepted
(see *Compressed FITS* below). Any other file type is answered with
`valid: false` ("Unsupported file type. Expected FITS (.fits, .fit, also
compressed: .fits.gz, .fz) or HDF5 (.h5, .hdf5, .bfdata).").
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

**VBT byte order (issue #165).** For a VBT volume the request may carry
`byte_order` (`"little"` or `"big"`), and the response carries `byte_order`
(the order the values are read in), `byte_order_declared` (the `.head`'s) and
`byte_order_warning`; see [VBT byte order](#vbt-byte-order) below.

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

### `POST /v1/datasets/pixel_to_sky`

The ICRS position of a pixel of an open image or cube, from the file's full
WCS; used for SAMP `coord.pointAt.sky`. See [SAMP](samp.md), "Pixel positions".

### `DELETE /v1/datasets/{dataset_id}`
Close a dataset of the session (any kind: image, cube, catalogue, VBT); what it
holds (a catalogue's table, a VBT's memory map) is dropped. The desktop sends
it when the last window showing a 3-D catalogue closes. Answers
`{"valid": true, "error": "", "closed": true}`, or `closed: false` when the
session has no such dataset (already closed or expired), which is not an error.

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
removed from the cache, so opening the URL again downloads it again; so is a
damaged gzip or tile-compressed download (`valid: false`).

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
`csv` is any delimited text table: the separator (comma, tab or semicolon) is
detected as `/v1/files/classify` detects it, on the same first 8 KB — the
first one that splits the first lines (up to ten, `#` comments and blank lines
left out) into the same number of fields, two or more. Quoted fields may hold
any separator. With N the field count of the first line not starting with `#`:

- the header is that line, unless the `#` line just above it (blank lines
  allowed between), with the `#` and the spaces around it removed, splits into
  N non-empty names none of which is a number while that first line holds a
  number: then the `#` line is the header (`#id,ra,dec` over `1,10,20`);
- below the header, a line starting with `#` is a comment, and skipped, only
  when it does not split into N fields; one that does is a row (a source named
  `#S5`). A `#` inside a quoted field spanning lines is part of the field;
- a semicolon table is read with decimal commas when at least two columns of
  its first rows are entirely numbers written that way (`12,5`) and no number
  in them has a decimal point; a single column such as `1,2` is left as text.

A file no separator makes consistent is read as a plain comma-separated file
was always read (short rows padded, a trailing comma, a single column) when
that gives a number somewhere; otherwise it answers `valid: false` with an
error saying it is not a delimited text table. A header with no rows opens as
an empty catalogue.
`paths` (optional) opens several files of the same format, with identical
columns, as one catalogue; `path` is then only a label.
Returns `dataset_id`, `session_id`, `num_rows`, the field list with types
(`fields`), the detected coordinate columns (`coordinate_fields`: `ra`,
`dec`, `glon`, `glat`, `x`, `y`, `z` and `system`, one of `sky`, `galactic`,
`cartesian`) and `server_side_rendering_available` (a VBT point table on a
backend that renders server-side).
A missing file or one that does not parse is answered with `valid: false`.
With `format: "vbt"` the request may carry `byte_order` and the response
carries `byte_order`, `byte_order_declared` and `byte_order_warning`
([VBT byte order](#vbt-byte-order)); `byte_order` with any other format is
answered with `valid: false`.

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
`limit` defaults to 100000 and `offset` to 0; `sort.direction` is `asc`
(default) or `desc`.
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
Returns `dataset_id`, `session_id`, `kind` (`point`|`volume`), `fields[]`, `num_rows`, `scalar_type`, and for a volume `dimensions` (`[nx, ny, nz]`) and `spacing`.
Takes `byte_order` and returns `byte_order`, `byte_order_declared` and
`byte_order_warning` as below.

### VBT byte order

Some converters write a VBT's data in one byte order and its `.head` in the
other (ChaNGa/Tipsy snapshots are big-endian; tables converted from them were
found declaring `little`). Read as declared, every value is absurd. The three
open routes that take a VBT (`/v1/catalogue/open` with `format: "vbt"`,
`/v1/datasets/open` on a VBT volume field, `/v1/vbt/open`) check it before
answering:

- Up to 16 chunks of 128 values per field are read, spread from the first row
  to the last (at most 256 reads and 64 fields in all; a few milliseconds even
  on a 51-million-point table). Tables of fewer than 64 rows are not checked.
- Both byte orders are scored on the same bytes: the share of the non-NaN
  values that are infinite, subnormal, or more than 20 decades from the
  field's median magnitude. No absolute magnitude counts, so cgs values
  (masses of 1e33 g, densities of 1e-24 g/cm³) are as plausible as Mpc
  positions, and a density spanning 30 decades is plausible too. NaN is left
  out (a masked field is full of it; read the other way those NaNs are
  subnormals, which count there). A field that scores 0 both ways (constant,
  all zero) gives no evidence and is left out.
- The two orders are compared with each other: the declared one is flagged
  when its mean score exceeds the other's by at least 0.2 and the other's is
  at most a quarter of it. A table read in the wrong order scores about
  0.45–1.0, one read in the right order 0.

The warning is

```json
{"declared": "little", "suggested": "big", "confidence": "overwhelming",
 "evidence": {"implausible_declared": 0.62, "implausible_suggested": 0.0,
              "sampled_values": 18432, "informative_fields": 7,
              "fields": [{"name": "MASS", "implausible_declared": 0.0,
                          "implausible_suggested": 0.0,
                          "first_declared": "-1.273e+24",
                          "first_suggested": "2.45e-05"}]}}
```

`confidence` is `overwhelming` when at least 40% of the sampled values are
absurd as declared, at most one in a hundred in the other order, over two or
more fields; `strong` otherwise. `fields` lists at most twelve fields. The
warning describes the FILE (its header against its values): it is given
whenever `byte_order` is left out, also when the file is already read in the
other order, so that a client can ask again on a fresh open.

`byte_order` in a request reads the file in that order from then on, for
every reader of that file (catalogue queries, point and ray-traced renders,
volume fields, cube tools, pyramids, moments, previews), in every process of
the backend; the file is never rewritten. Asking for the declared order drops
the choice; leaving `byte_order` out keeps whatever the backend already reads
the file in.

- **Where it is kept.** The byte order is a property of the header and its
  data, so the choice is per backend installation (and per store: backends
  sharing a workspace share it), not per session. It is kept in
  `$VISIVO_VBT_BYTE_ORDERS`, by default `.vbt-byte-orders.json` in the
  workspace (`$VISIVO_EXPORTS_DIR`), which every backend process finds from
  its environment. An entry is keyed by the real path of the `.head` the
  header parser reads (the one beside the path given, so a symlinked data
  file with its own `.head` works) and holds the order, the declared order,
  the size, modification time and inode of the `.head` and its data, when it
  was set and by which session. Correcting the `.head` or rewriting the data
  retires it; a rewrite that keeps size, modification time and inode
  (`touch -r` after an in-place edit) does not. An entry whose files cannot
  be stat'ed for the moment (an offline mount) is kept. Asking for an order
  that cannot be kept (the header or its data cannot be read) is answered
  with `valid: false`.
- **Writing.** Atomically (temporary file, fsync, rename), under an exclusive
  lock: `flock`, `msvcrt.locking` on Windows, a lock directory with a 30 s
  stale timeout where the file system refuses `flock` (Lustre without it,
  some SMB/NFS mounts); failing all of them the write goes ahead unlocked
  and says so in the log. Every change is logged (`[vbt-byte-order]`).
- **Open datasets keep their order.** The store decides how a file is read
  when it is OPENED. An open dataset carries that order in its path
  (`…::vbt-order=big`), so every reader of it, in any process, a task
  running while the choice changes, a cached product, a render session,
  reads that order whatever the store says later, also when another backend
  sharing the store changes it.
- **Datasets and render sessions already open.** When an open changes the
  order, every dataset open on that file in another order, in any session of
  this backend, is closed, and so are this backend's render sessions drawing
  it in the other order (logged): one `dataset_id` never serves two sets of
  values. A request on such a dataset is answered as for any unknown dataset;
  a client finds its render session gone, as after an expiry, and opens
  again. Another backend sharing the store keeps its open datasets in the
  order they were opened in.
- **Caches.** Pyramids and cached products of a file read against its header
  are kept apart from those of the declared order.
- **Provenance.** The dataset's entry in `/v1/reproducibility/provenance`
  records `visivo:byte_order` and `visivo:byte_order_declared`.

### `POST /v1/vbt/subset`
Legacy bulk fetch (no filter, `max_rows` cap).

### `POST /v1/vbt/query`
Same request shape as `/v1/catalogue/query`.
Response: `total_rows`, `returned_rows`, `fields[]`, `columns` (base64-encoded float64 column vectors), `num_rows`.
