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
cube of its first field, or a PLUTO binary output (see *PLUTO output* below). A compressed FITS (`.fits.gz`, `.fz`) is accepted
(see *Compressed FITS* below). Any other file type is answered with
`valid: false` ("Unsupported file type. Expected FITS (.fits, .fit, also
compressed: .fits.gz, .fz) or HDF5 (.h5, .hdf5, .bfdata).").
Response includes `dataset_id`, `session_id`, `kind` (`image`|`cube`), dimensions, WCS metadata (`wcs_status`, `wcs_warning_message`, `wcs_sanitized_axes`, `spacing`, `origin`, `ctype[]`, `cunit[]`, `crval[]`, `crpix[]`, `cdelt[]`), the spectral-axis summary (`spectral_axis_type` = the spectral `CTYPE`, `spectral_axis_unit`), beam (`beam_major`, `beam_minor`, `beam_pa` in degrees, `beam_per_channel`: true when a `BEAMS` table gives one beam per channel, the beam fields then being the header's, and `beam_source`: `header`, `user` for a beam typed with [`/v1/datasets/beam`](#post-v1datasetsbeam), `none`, or `not_applicable` for data without celestial axes such as VBT fields and raw simulation fields, where no beam can be set), `bunit` (the header's `BUNIT`, possibly empty), `simulation_volume` and `physical_units` (see [`/v1/datasets/units`](#post-v1datasetsunits)), `intensity_unit` and `intensity_unit_note` (the unit of the values when `BUNIT` is missing: `K` when `TEMPSCAL` names a temperature scale, as on JCMT/ACSIS cubes with `TEMPSCAL = 'TA*'`, or when `BTYPE` or `LABEL` names a temperature; empty when nothing says; the note says which keyword gave it), and:

| Field | Type | Meaning |
|-------|------|---------|
| `rest_freq_hz` | `float \| null` | Header `RESTFRQ`, else `RESTFREQ`, in Hz. `null` when the header carries neither, or carries a non-positive value — a client must be able to tell "no rest frequency" from one it would divide by. Needed to place a rest-frame spectral-line list on a velocity axis, and to convert a frequency axis to velocity. |

The other response fields: `valid`, `error`, `active_axes`,
`degenerate_axes_summary`, `file_size_bytes`, `full_res_threshold_bytes` and
`recommended_load_mode` (`auto_full` or `preview_only`: a file at or above the
threshold should open as a preview first), and
`server_side_rendering_available` (this backend can host a
[render session](render.md) for the cube). For a multi-field source (a VBT
volume, a PLUTO output) `field_name` is the field opened and `fields` lists every field as
`{name, path}`, `path` being what `/v1/datasets/open` takes to open that field. The same response is returned by the
three routes below.

**PLUTO output (issue #315).** `path` may name a PLUTO binary output: a data
file (`data.NNNN.dbl` in `single_file` mode, `<var>.NNNN.dbl` or `.flt` in
`multiple_files` mode) with its `dbl.out` / `flt.out` and `grid.out` beside it,
the `.out` file itself (the newest output listed, its first variable whose file
is present), or a variable reference `<data file>::pluto-var=<name>`. The
variable is memory-mapped where it is (`.dbl` float64, `.flt` float32, in the
byte order the `.out` line gives; `byte_order` is refused), shape
(nx3, nx2, nx1) with x1 fastest, and the file's size is checked against the
grid. Nothing is read beyond the text files when it opens: a 32 GB variable
opens in milliseconds, with `recommended_load_mode: "preview_only"`. A 3-D run
is a `cube`, a 2-D run an `image`. `fields` lists the output's variables whose
files are present (a run copied one variable at a time offers only those);
face-centred fields (`Bx1s` …) are not offered. The header it is read with:

| Grid | Axes |
|------|------|
| Cartesian, every direction uniform (cell widths equal to 1e-6) | `CTYPE` `X`/`Y`/`Z`, `CDELT` the cell width, `CRVAL` the first cell's centre at `CRPIX` 1, in code units; `PLUTOGRD = 'UNIFORM'` |
| Non-uniform, or a polar, cylindrical or spherical geometry | `CTYPE` `X`/`Y`/`Z` counting cells (`CDELT` 1, `CRVAL` 1 at the first: `grid.out`'s numbering), `PLUTOGRD = 'INDEX'` and `PLUTOWHY` the reason |

Every header also carries `OBJECT` and `PLUTOVAR` (the variable), `PLUTOTIM`
(the output time), `PLUTOSTP` (the step), `PLUTONUM` (the output number) and
`PLUTOGEO` (the geometry). A pyramid or cached product of a variable is keyed
by its data file, its `grid.out` and what its own `.out` line says, so a running
simulation appending outputs to the `.out` does not invalidate it.

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

### `POST /v1/datasets/beam`

Type the restoring beam of a FITS image or cube whose header has none (no
complete `BMAJ`/`BMIN` and no `BEAMS` table), or forget it.

```json
{ "dataset_id": "...", "major_arcsec": 15.0, "minor_arcsec": 15.0, "pa_deg": 0.0,
  "clear": false }
```

`minor_arcsec` defaults to `major_arcsec` (a circular beam) and `pa_deg` to 0.
The beam is kept with the file, not the session: it is stored in
`.beam-overrides.json` in the Workspace (or `$VISIVO_BEAM_OVERRIDES`), keyed by
the file the user opened and valid while that file is unchanged (size,
modification time, inode). Every later request on the file, in any session
and in every worker process, then sees it as if the header carried
`BMAJ`/`BMIN`/`BPA`: per-beam fluxes, beam areas and the per-beam error terms
use it, and moment maps and PV diagrams saved as FITS carry it with a
`COMMENT` saying it was typed. Reopening the file reports
`beam_source: "user"`. A complete header beam is never overridden; an
incomplete one (only one of `BMAJ`/`BMIN`, or a zero or non-finite value) is,
and `note` says so. Rewriting the file, a header edit included, retires the
typed beam.

Response: `valid`, `error`, `beam_major`, `beam_minor`, `beam_pa` (degrees,
as in the open response), `beam_source` and `note`. `valid: false` when the header
already has a beam (the header's beam is returned), when `major_arcsec` is
missing, when an axis is not positive, the minor axis exceeds the major, the
major exceeds 10 degrees, or the position angle is not finite, and for a
dataset that is not a FITS image or cube or has no celestial axes (`beam_source: "not_applicable"`).

### `DELETE /v1/datasets/{dataset_id}`
Close a dataset of the session (any kind: image, cube, catalogue, VBT); what it
holds (a catalogue's table, a VBT's memory map) is dropped. The desktop sends
it when the last window showing a 3-D catalogue closes. Answers
`{"valid": true, "error": "", "closed": true}`, or `closed: false` when the
session has no such dataset (already closed or expired), which is not an error.

### `POST /v1/datasets/units`

Physical units of a dataset (issue #524), one table for every reader in
`app/physical_units.py`: `{column: {factor, unit[, source]}}`, the factor that
makes the column physical and the unit it is then in. Each reader states its
own columns (a Gadget snapshot through its unit system, `Component.physical()`;
a RAMSES family or output folder from the `unit_l`, `unit_d`, `unit_t` of its
`info_*.txt`: `x`, `y`, `z`, `dx` in kpc, `vx`.. in km/s, `mass` in Msun,
`density` in g/cm³, `pressure` in erg/cm³, proper at the output's `aexp`;
a PLUTO output through the run's code units; a VBT field or a FITS cube has
none of its own), `scales(path)` lays what the user typed over them, and every
route, export and client reads that. What was typed is kept with the file
(`$VISIVO_PHYSICAL_UNITS`, else `.physical-units.json` in the workspace; under
the cross-process lock of the other stores). Data are never rewritten: a viewer
shows `value * factor` in `unit` and asks the data routes for `value_scale`
(see [`/v1/cube/slice`](cube.md)).

A volume has the columns `value` (the variable's values), `x`, `y`, `z` (its
coordinates) and, for PLUTO, `time`; a Gadget snapshot its own (`x`, `mass`,
`hsml`, `vx`, ...). A column that is absent has no known conversion (code
units).

Request (`DatasetUnitsRequest`): `dataset_id`; `unit_density` (g/cm³),
`unit_length` (cm), `unit_velocity` (cm/s), the run's units, shared by every
variable and output of a PLUTO run; `columns`, `{column: {factor, unit}}`, a
typed conversion for any column of the dataset (the coordinates and the time
of a PLUTO run are shared by its variables; a Gadget column is shared by the
snapshot's particle types); `clear` (forget everything typed for this dataset
and run, then apply the rest). A factor must be a positive finite number
(`valid: false`). For a simulation volume or a particle snapshot (`valid:
false` otherwise). After a change on a Gadget dataset the profile and total
routes use the new factors.

Response (`DatasetUnitsResponse`): `valid`, `error`, `physical_units`. The
same object is in the open response of a volume, with `simulation_volume`
(true for a VBT field, a PLUTO variable, or a cube whose three axes are `X`,
`Y`, `Z`): `columns`, `active` (a column was stated by the run or typed),
`source` (`definitions`, `user`, `none`), `origin` (the `definitions.h` it was
read from), `variable`, `unit_density`, `unit_length`, `unit_velocity` (cgs,
null when unknown), `value_quantity` (`density`, `velocity`, `pressure`,
`magnetic`, or empty) and `time_code` (the output's time in code units,
PLUTO). Each column carries `source`: `definitions`, `user`, or empty for a
reader's own table.

Priority: what was typed, then the producer: for PLUTO the `UNIT_*` of the
run's `definitions.h` (or a `pluto.ini` that sets them) in the data directory
or up to two above it; for Gadget the snapshot's unit system (header,
parameter file, `VISIVO_GADGET_UNITS`); then nothing (code units).
Expressions are evaluated with PLUTO's `CONST_*` (`(1.0e-24*CONST_mp)`);
anything else in a definition is ignored. Derived as PLUTO does: time = L/V,
pressure = ρV², magnetic field = V√(4πρ) (Gaussian). Variables are matched by
name (`rho`; `vx1`…, `vr`, `vphi`; `prs`; `Bx1`…; `tr1`… are dimensionless
and stay unconverted); another variable gets a factor only if typed. A grid
shown in cell indices (`PLUTOGRD = INDEX`) has no coordinate columns.

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
`format` is `csv` (default), `votable`, `fits`, `ipac`, `speck`, `vbt`,
`gadget` or `ramses`. A RAMSES output (issue #549) is recognised the same way:
its `output_NNNNN` folder, any file in it (`info_*.txt`, `amr_*.out*`,
`part_*.out*`) or a family reference `<output folder>::ramses-family=<slug>`
(`dm`, `stars`, `sinks`, ..., and `cells`: the leaf cells of the mesh, issue
#550) opens that particle family (dark matter first)
as a table of particles, response `format: "ramses"`
([RAMSES outputs](../design/ramses-outputs.md)). A Gadget snapshot (issue #31) is recognised by its content whatever
format is asked for: a file of it, its `snapdir` folder, or a particle-type
reference `<first file>::gadget-type=<N>` opens that particle type (the first
with particles for a file or a folder) as a table of particles, read in place
([Gadget snapshots](../design/gadget-snapshots.md)). A reference to a type
with no particles answers `valid: false`, and a snapshot opens one type at a
time (not with `paths`).
A generic HDF5 file (issue #552) opens with `format` `hdf5` through a table reference `<file>::hdf5-table=/group[offset:count]` (either separator is accepted), or by the file when it holds one table; with several the answer is `valid: false` listing the references. Columns of a contiguous, uncompressed dataset are views of the file's map; the others are read by rows in whole chunks (the Gadget reader's windows and cache budget), so opening reads no data. It is a component of the particle seam, so sampling, filters, profiles and the ray-traced renders read it as they read a snapshot.
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
backend that renders server-side), `format` (the format read), `particles`
(a simulation's particles read in place, a VBT of points or a snapshot's
particle type: always cartesian, drawn ray-traced by default in VR), `path`
(the path the dataset is known by: a snapshot's particle-type reference),
and for a snapshot `components` (its particle types, `[{name, path,
num_rows}]`), `snapshot` (format, files, time, `scale_factor`, `redshift`,
`box_size`, `omega0`, `omega_lambda`, `hubble_param`, `mass_table`,
`npart_total`, `units`, `blocks`, `skipped_blocks`) and `component` (type,
name, columns, `units` per column as stored, `physical` factor and unit per
column, `derived` columns and how they were computed, `mass_from_table`).
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
(default) or `desc`. Filter operators: `=`, `!=`, `<`, `<=`, `>`, `>=`,
`in` (comma-separated values) and `contains` (case-insensitive), for a loaded
catalogue and a snapshot's particle type alike. On a particle type read out of
core, a sort orders NaN last and equal values by row number ascending in both
directions, so successive pages neither repeat nor skip rows; `offset + limit`
may not exceed 1,000,000.
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

A spatial selection is a filter with `"op": "inside"` or `"outside"` and a
`box` (`{"fields": [x, y, z], "min": [...], "max": [...]}`) or a `sphere`
(`{"fields": [x, y, z], "center": [...], "radius": r}`). Either may carry
`"period": L`, the length of a periodic box: distances then use the nearest
periodic image and a box wraps round the faces. Without it, a cosmological
Gadget snapshot's `BoxSize` is the period of its position columns (`x`, `y`,
`z`, `centre_of_mass_*`) when the run is periodic (a Gadget-4 `Config/PERIODIC`,
`periodic=` in `VISIVO_GADGET_UNITS`, else a comoving run); `"period": 0`
turns wrapping off. A VBT or catalogue
wraps only when the period is given. On a loaded table the selection is one
boolean mask applied with the scalar filters, so a big-endian VBT read in
place works and only the page's rows are copied.

### `POST /v1/catalogue/diagnostics`

Exact histogram and summary moments over the complete filtered population:

```json
{
  "dataset_id": "...",
  "field": "rho",
  "weight_field": "mass",
  "bins": 64,
  "range_min": 1e-12,
  "range_max": 1e-3,
  "filters": [
    { "field": "x", "op": ">=", "value": "-1000" }
  ]
}
```

`weight_field` and the range (both ends or neither) are optional; `bins` is
between 4 and 512. The response contains `selected_rows`, `finite_rows`,
extrema, mean and standard deviation (population, combined chunk by chunk with
Chan's update, so it keeps its digits far from zero), optional `weighted_mean`
and `weight_sum`, `bin_edges`, `counts`, the field and weight units, and
textual weighting and selection provenance. Counts are floating point because
weighted bins contain sums of weights. A loaded table is read a million rows
at a time, only the columns the filters, the field and the weight need.

### `POST /v1/tasks/catalogue_diagnostics`

The same request and computation as a task, for a table large enough that the
two passes take a while: the answer is `{task_id, status: "running"}`. It
takes one of the session's compute slots until the computation ends (HTTP 429
when none comes free), as the other heavy tasks do.
`GET /v1/tasks/{task_id}` gives `progress` (0 to 1 over both passes; one pass
when the range is given) and, once `completed`, `result`, the diagnostics
response above (`valid: false` with the reason for an unknown field or a bad
range). `DELETE /v1/tasks/{task_id}` stops it between two chunks (status
`cancelled`).

For out-of-core particle tables the calculation is a bounded-memory two-pass
scan over the full component. It is independent of the point-render sample and
render budget. Non-finite field values are excluded. A row whose weight is non-finite
or negative weighs zero in the weighted mean, weight sum and histogram, and
stays in the unweighted count, range, mean and standard deviation, which do
not depend on the weight field. An unknown field, invalid range or
population with no finite values returns `valid: false`.

### `POST /v1/catalogue/profile` and `POST /v1/tasks/catalogue_profile`

Radial profile of the selected particles of a catalogue (a Gadget particle
type read out of core, a VBT or another loaded table), in log-spaced shells
around a centre. The task route takes the same body and is polled and
cancelled like `/v1/tasks/catalogue_diagnostics`, reporting progress over the
centre search and the passes.

```json
{
  "dataset_id": "...",
  "filters": [],
  "position_fields": ["x", "y", "z"],
  "velocity_fields": null,
  "mass_field": null,
  "columns": ["temperature", "radial_velocity"],
  "centre": { "mode": "shrinking_sphere", "start": null, "initial_radius": null,
              "shrink_factor": 0.975, "min_particles": 1000, "velocity": null },
  "r_min": null, "r_max": null, "bins": 32,
  "axis": null,
  "period": null,
  "geometry": "spherical", "z_min": null, "z_max": null
}
```

- `velocity_fields`: `null` uses `vx, vy, vz` when present, `[]` none.
  `mass_field`: `null` uses `mass` when present, `""` gives every particle
  weight 1 (densities are then number densities).
- `columns`: numeric columns profiled per shell, plus the derived
  `radial_velocity`.
- `period`: `null` is the dataset's own box (a periodic Gadget snapshot), `0`
  turns wrapping off, a length sets it on the three position fields.
  Distances use the minimum image.
- `centre.mode`:
  - `point` with `point: [x, y, z]`;
  - `target` with `target_dataset_id` and `target_row`, the position columns
    of a row of another open catalogue, such as a SubFind group or subhalo.
    It is taken to be in the particles' coordinate units;
  - `shrinking_sphere`. The nested shrinking spheres of Power et al. (2003),
    mass-weighted. Each sphere is `shrink_factor` times the last and holds
    only the particles of the previous one. The search stops before fewer
    than min(`min_particles`, max(100, 1 % of the selection)) particles
    remain. The default start is the periodic centre of mass (Bai & Breen
    2008), and the default initial radius is the largest distance from it.
    Step 0 is the centre of the start sphere itself. A selection with fewer
    than 100 particles, or a start sphere holding fewer than the minimum,
    is refused. Out of core, the search keeps one bit per row for the
    current sphere's members, so each step reads the table once. Once the
    sphere holds at most 2 million particles, it keeps those in memory,
    with the same result.
- `centre.velocity`: velocities are measured relative to it. They are
  peculiar velocities, without the Hubble flow. The default is the
  mass-weighted mean velocity of the smallest sphere about the centre that
  holds 1000 particles. A small sphere keeps a halo's own motion from being
  diluted by the background of a large selection.
- `axis`: the polar axis of the polar and azimuthal velocity components. The
  default is the angular momentum, about the centre and in the centre's
  frame, of the particles of that same sphere. The table's z is used when the
  angular momentum vanishes. The radial velocity does not depend on it.
- `r_max` defaults to the smallest of the largest distance, half the period,
  and the distance to the nearest boundary of the selected region on the
  position fields. The boundaries counted are `inside` boxes and spheres,
  comparisons such as `x > 10` (half-spaces), and the nearest point of an
  `outside` box or sphere. `r_min` defaults to the larger of the smallest
  positive distance and `r_max`/1000.

- `geometry`: `spherical` (default, shells in the distance from the centre),
  `cylindrical` or `axial`. Both of the last two use an axis through the
  centre: `axis` if given, else the angular momentum of the particles of the
  centre-velocity sphere above (needs velocities), else the table's z. The
  result's `axis` says which. R, the distance from the axis, and z, the
  signed height along it, come from the minimum-image vector from the centre.
  - `cylindrical`: log-spaced annuli in R from `r_min` to `r_max` (`r_max`
    as above; `r_min` defaults to `r_max`/1000). With both `z_min` and `z_max`, only particles with
    `z_min` ≤ z < `z_max` count (a slab); without them the whole selection
    along the axis does.
  - `axial`: linear slabs in z from `z_min` to `z_max` (default ±the
    `r_max` default above). `r_max`, when given, keeps only particles with
    R < `r_max`; `r_min` is refused.
  - `axis` no longer needs velocities for these two.

The response has `profile`, `centre`, `units` and `provenance`.

`profile`:
- `r_lo`, `r_hi`, `r_mid` (geometric) and `r_mean` (mass-weighted);
- `complete`: true while the shell is inside the selected region (as for
  `r_max`) and within half the period. It is never true when the centre
  itself is outside the region;
- `count`, `mass`, `density` (shell mass over shell volume);
- `enclosed_mass`: every particle closer than `r_hi`, those inside `r_min`
  included;
- `inside_r_min`, `outside_r_max`, `selected_rows`, `r_min`, `r_max`,
  `complete_radius`;
- `columns.<name>`: `mean`, `std`, `p16`, `median`, `p84` (mass-weighted),
  `weight`, and `quantile_axis`. The quantiles are read from a 1024-bin
  histogram per shell, refined once inside the quantile's bin, on log10 when
  the shell's values are positive and span more than a factor 100;
- with velocities, `velocity`: `centre_velocity`, `axis`,
  `centre_velocity_from` (the radius and particle count of the central
  sphere, and where the velocity and axis came from),
  `mean_radial`, `mean_azimuthal` (over the particles with finite
  velocities; the others still count in the mass and density), `sigma_r`, `sigma_theta`, `sigma_phi`,
  `sigma_1d` = sqrt((σ_r² + σ_θ² + σ_φ²)/3), each about its shell mean so
  that infall and rotation are not counted as dispersion, `beta` = 1 −
  (σ_θ² + σ_φ²)/(2σ_r²), and `sigma_cartesian_with_streaming`.

Empty shells give `null`.

With `geometry: "cylindrical"` the bins keep the `r_*` names (R) and
`inside_r_min`, `outside_r_max` (now with their mass too), and add:
- `surface_density`: annulus mass over π (r_hi² − r_lo²), the selection
  projected along the axis;
- `density`: annulus mass over that area times `z_max − z_min`, or `null`
  without a slab;
- `enclosed_mass`: every particle with R < `r_hi` (inside the slab, if any);
- `z_range` (`null` without a slab), `outside_z_range` (`count`, `mass`:
  particles outside the slab, in no bin);
- `axis`: `vector`, `from` (`given`, the angular momentum of the central
  particles, or the table's z) and `through` (the centre);
- `complete` is conservative: the farthest corner of the annulus's cylinder
  (R = `r_hi`, height `max(|z_min|, |z_max|)`) must be inside the region the
  selection samples fully, and a cylinder without a slab is complete only
  when nothing bounds the selection.

With `geometry: "axial"` the bins are `z_lo`, `z_hi`, `z_mid`, `z_mean`
(mass-weighted) and `z_min`, `z_max`, with `count`, `mass`, `columns`,
`axis` as above, and:
- `linear_density`: slab mass over `z_hi − z_lo`;
- `density`: slab mass over π `r_max`² (`z_hi − z_lo`), or `null` without
  `r_max` (`r_limit` echoes it);
- `enclosed_mass`: every particle below `z_hi` (within R < `r_max` when given), those below `z_min` included;
- `below_z_min`, `above_z_max` (`count`, `mass`) and `outside_r_limit`
  (particles with R ≥ `r_max`, in no bin);
- `complete`: as for the cylinder, with the slab's largest |z| and `r_max`.

In both, the closure is: every usable particle is below, in a bin, above, or
outside the slab or cylinder. `velocity` holds the cylindrical components
about the axis instead of the spherical ones: `mean_cylindrical_radial`,
`mean_azimuthal`, `mean_vertical`, `sigma_R`, `sigma_phi`, `sigma_z`,
`sigma_1d` = sqrt((σ_R² + σ_φ² + σ_z²)/3) and
`sigma_cartesian_with_streaming`; v_R is 0 on the axis. `units` adds
`height`, `surface_density` and `linear_density`.

`centre`: the mode and the centre. For `shrinking_sphere` it also has
`radius`, `particles_inside`, `steps`, `n_min`, `start`, `initial_radius`,
`shrink_factor`, `selected_particles` and `out_of_core_passes`.

`units`: the units the table states (a Gadget snapshot's code units: comoving
and /h where the run uses them, velocities as v/√a) for `radius`, `mass`,
`density`, `velocity` and each column. `units.physical` gives the reader's
factor and unit to physical kpc, M☉ and km/s, and M☉/kpc³ for the density.

`provenance`: the dataset, the selection, the position and velocity fields,
the weights, the frame (periods and minimum image), the binning, the
definitions of every quantity, and the snapshot's redshift, time, h and box.

### `POST /v1/catalogue/totals` and `POST /v1/tasks/catalogue_totals`

Physical totals of the exact selected population (every row the filters
keep), read out of core in one pass, with the progress and cancellation of
the profile task.

```json
{
  "dataset_id": "...",
  "filters": [],
  "velocity_fields": null,
  "mass_field": null,
  "energy_field": null,
  "position_fields": ["x", "y", "z"]
}
```

`velocity_fields` (`null`: `vx, vy, vz` when present, `[]`: none),
`mass_field` (`null`: `mass` when present, `""`: none) and `energy_field`
(`null`: `u`, the specific internal energy, when present, `""`: none).
`position_fields` only supplies a column to count rows with when none of the
others is read. A field the request names that the table lacks is refused.

`totals`:
- `count`, and `rows`: `selected`, `with_mass` (finite, non-negative mass),
  `with_velocity` (those that also have finite velocities) and
  `with_energy` (finite internal energy ≥ 0). `mass_with_velocity` is the
  mass of the `with_velocity` rows, which the centre-of-mass velocity is
  relative to;
- `quantities.<name>`: `mass`, `centre_of_mass_velocity`, `momentum` (vector),
  `momentum_magnitude`, `kinetic_energy` (0.5 Σ m |v|², in the frame of the
  stored velocities), `kinetic_energy_com_frame` (0.5 Σ m |v − v_com|²),
  `velocity_dispersion_1d` (sqrt of 2 × that over 3M) and `thermal_energy`
  (Σ m u). Each has `value` (physical), `unit` (Msun, km/s, Msun km/s or
  erg), `stored` (`value` and `unit` in the table's own units) and `reason`.

A `value` is `null` when a field it needs has no declared physical factor,
or one in another unit, or when the factors of the three velocity fields
differ. `reason` then says what is missing and `stored` is kept. When the
field itself is missing (no velocities, no mass, no `u`, as for dark matter),
`stored` is `null` too. Velocities are the peculiar velocities the reader
converts to km/s (a cosmological snapshot stores v/√a), so the energies are
those of the peculiar motion in the simulation frame. The centre-of-mass
frame energy is accumulated from per-chunk second moments merged pairwise
(Chan et al. 1983), not as Σ m v² − M v_com², so a large bulk velocity does
not cancel it.

`provenance` has the dataset, the selection, the fields used, the
definitions and the snapshot's redshift, time, h and box.

### `POST /v1/catalogue/phase_diagram` and `POST /v1/tasks/catalogue_phase_diagram`

2-D histogram of two columns of the selected rows, weighted or not; the task
route as above.

```json
{
  "dataset_id": "...",
  "filters": [],
  "x": { "field": "rho", "scale": "log", "bins": 64, "min": null, "max": null },
  "y": { "field": "temperature", "scale": "log", "bins": 64 },
  "weight_field": "mass",
  "centre": null,
  "position_fields": ["x", "y", "z"], "velocity_fields": null, "mass_field": null,
  "period": null
}
```

An axis `field` is a numeric column, or `radius` or `radial_velocity`, which
need a `centre` (as for the profile). With a `centre`, these two names take
the place of table columns with the same name. Without one, a table column
with that name is used.

`min`/`max` give both ends of an axis range, or neither. Without them the
range is the extent of the selection's placeable values.

Bins are [lo, hi), except the last, which holds its upper edge.

`diagram` has:
- `x_edges`, `y_edges`;
- `counts` and `weights`, indexed [x][y];
- `selected_rows` and `selected_weight`, `binned_*`, `outside_*` (outside a
  given range) and `dropped_*` (non-finite values, or values ≤ 0 on a log
  axis);
- `invalid_weight_rows`: rows whose weight is not finite and ≥ 0. They carry
  no weight.

The totals close: `binned + outside + dropped` equals the selected weight,
and `binned + outside + dropped + invalid_weight` equals the selected rows.

`units` gives `x`, `y`, `weight` and `physical`. `centre` and `provenance` are
as for the profile.

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
