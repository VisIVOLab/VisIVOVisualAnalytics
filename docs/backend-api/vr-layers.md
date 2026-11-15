# Points, catalogues and flows for the headset

These routes give the [VR page](../user-guide/vr/index.md) the data types
other than a cube
([#190](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/190)): a
point table sampled for a headset's GPU, catalogue sources placed in a cube's
pixels, and a velocity field's streamlines and arrows. Every computation is
here; the page only draws. They take the token header and the session header
like every `/v1` route. The point session's stereo frames for the headset are
described in [Server-side rendering](render.md) (`xr_pose`).

## `POST /v1/points/info`

`{"dataset_id"}`: a VBT point table (`/v1/vbt/open`) or a catalogue with
sky or Cartesian positions (`/v1/catalogue/open`). Answers `valid`,
`num_rows`, `fields` (the numeric columns), `axes` (the default X, Y, Z),
`sky` (positions computed from RA/Dec, `@x`, `@y`, `@z`), `format`:
`vbt` for a VBT point table (a simulation's particles, which the VR page opens
in its ray-traced style, issue #264) or `catalogue`, and `units` (issue #259:
`{column: unit}` as a catalogue's file declares them, VOTable, FITS or IPAC;
`{}` for a VBT, whose header carries none). 422 for a dataset that is not a
table of points.

## `POST /v1/points/series`

`{"dataset_id"}` of a table of points (issue #259): the snapshots beside its
file, the files in its folder whose names differ from it only in their number
(the last run of digits in the name up to its first dot: `snap_010.bin.head`
steps through `snap_002`, `snap_010`, `snap_1000`, and an extension's digits
never count). Answers `valid`, `paths` (absolute, by number), `numbers`,
`index` (of this file) and `count`; a series of one when the name has no
number or the folder cannot be listed. Of a longer series, the 10 000 members
nearest this one by number. A member the backend's data roots refuse (a
symbolic link resolving outside them, a hidden name) is left out. Only names
are read (`app/snapshot_series.py`); there is no time-series data model: each
snapshot is opened as any table (`/v1/vbt/open`, `/v1/catalogue/open`).

`sky` in the request (optional, issue #260) places a sky catalogue as the
desktop window does: `{"frame": "fk5" | "galactic" | "supergalactic",
"distance_mode": "comoving" | "redshift_space", "cosmology": "Planck18" |
"Planck15" | "Planck13" | "WMAP9"}`; what it leaves out is the default (fk5,
comoving, Planck18). Redshift space is c z / H₀ with the model's H₀. A custom
cosmology (`"Custom"`) or an unknown name answers 422. The rules are those of
the desktop (`app/catalogue_positions.py`): a distance column > 0 wins under
every model, then the model's comoving distance for a redshift > 0; a source
with neither has no position (issue #265) and is in no sample. A catalogue
with no redshift column and no distance column at all is placed on the
celestial sphere (radius 300 Mpc, not a distance). For a sky catalogue the
answer adds `sky_settings`: `frame`, `distance_mode`, `cosmology`, the counts
`placed`, `unplaced` (no distance) and `uncertain` (placed from
0 < cz < 1000 km/s), and `sky_sphere` (true for positions only).

RA/Dec (and GLON/GLAT in `catalogue_pixels`) are read in the unit the table
declares: a VOTable or FITS column unit, an IPAC header unit, or a unit in the
column name (`RA (h)`). Hours of angle, radians, arcseconds and so on are
converted to degrees, in any case (FITS `DEG`, `RAD`, `HOURS`); a column with
no unit is in degrees. A declared unit that is not an angle (`km/s`) is
refused with 422.

## `POST /v1/points/sample_bin`

An even sample of the table within a budget.

| Field | Default | Meaning |
|---|---|---|
| `dataset_id` | | the table |
| `x`, `y`, `z` | the table's | position columns |
| `color_field` | none | the colour column (none: one colour) |
| `color_log` | false | code the colour column as log10 (it starts at the smallest positive value); `null`: the backend chooses (log for a positive column over more than three decades), and says which in `X-Visivo-Color-Log` |
| `color_min`, `color_max` | the sample's range | the values codes 0 and 65535 stand for |
| `max_points` | 2 000 000 | rows sent at most (1 to 10 000 000) |
| `filters` | `[]` | numeric filters `{field, op, value}`, as the point session's; or a box `{box: {fields: [x, y, z], min: [...], max: [...]}, op: "inside" \| "outside"}` (issue #259: the rows inside it, bounds included, or all but those; the point session's `set_filters` takes the same) |
| `size_field` | none | a column of sizes (a smoothing length, data units): one more code per row after the colours, the log10 of the size over `X-Visivo-Size-Range` (issue #259) |
| `sky` | none | a sky catalogue's placement, as for `/v1/points/info` |

The filters run over the whole table first; the rows are then those the
point render session draws when it samples (evenly spaced over the rows that
pass), without a non-finite position, sent in a low-discrepancy order (van der
Corput: the bit-reversed index): a prefix of length 2^k is exactly every
(n / 2^k)-th row, and any prefix of length m leaves no gap wider than 2 n / m
rows. A page that must draw fewer rows to keep its frame rate draws the first
ones. A catalogue is built from the dataframe the session already holds, not
read again: the table built from it is kept in the dataset's entry, and goes
when the dataset is closed. A malformed filter or a table that cannot be read
answers 422. At most two samples are made at a time (a request waits for its turn before
it takes a worker thread; a catalogue's table is built under its own dataset's
lock); one of 10 M rows with a
colour column peaks at about 0.41 GB of working memory (41 bytes a row) and
takes 0.9 s here.

The body is little-endian `uint16`: the positions, three per row, quantised
over the sample's bounding box (`X-Visivo-Bounds`: code 0 is the minimum, 65535
the maximum of each axis; bounds and codes are worked from float64 offsets, so
coordinates far from 0, such as 1e9, 1e9 + 1 and 1e9 + 2, stay distinct;
the bounds are within float32 rounding of the true extremes; a row whose
position is not finite or lies beyond float32's range, 3.4e38, on any axis is
left out, as the remote point session leaves it out; the session reads its
positions the same way, issue #206, so its stereo frames land on these points), then one colour code per row when a colour column
is given (code 0 also for a non-finite value; the column is read in float64,
so values far from 0 keep distinct colours). Headers: `X-Visivo-Count`,
`X-Visivo-Total-Rows`, `X-Visivo-Step` (table rows per sampled row),
`X-Visivo-Bounds` (`xmin,xmax,ymin,ymax,zmin,zmax`), `X-Visivo-Axes`,
`X-Visivo-Scalar-Type: uint16`, and with a colour column
`X-Visivo-Color-Field`, `X-Visivo-Color-Range`, `X-Visivo-Color-Log` (`0`/`1`)
and `X-Visivo-Data-Range` (the column's range over the sample). A sky
catalogue adds `X-Visivo-Sky` (`frame,distance_mode,cosmology`),
`X-Visivo-Unplaced` and `X-Visivo-Uncertain` (the counts of `sky_settings`).
`X-Visivo-Placed-Rows` is the number of rows the sample was drawn from: a sky
catalogue's rows without a position are never sampled, so the budget goes to
rows that can be drawn, and `X-Visivo-Step` and the page's splat size count
these (the point session's own sample and splats do the same);
`X-Visivo-Total-Rows` is still every row of the table.
The headset hub's `show` command and the app launch's `cube`
(`/v1/vr/headset/command`, `/v1/vr/launch`) take the same placement as
`sky`; the desktop's link and a pairing code carry it as `sky_frame`,
`distance_mode` and `cosmology`. With a size
column, one more `uint16` per row after the colours: code c is the size
10^(log10 lo + c / 65535 (log10 hi − log10 lo)) over `X-Visivo-Size-Range`
(`lo,hi`, the smallest and largest positive finite size; a size that is not
positive and finite gets the median, so no row is dropped for its size), and
`X-Visivo-Size-Field`. A size column with no positive value sends no sizes
(uniform splats) and says why in `X-Visivo-Size-Note`, instead of failing.

## `POST /v1/cube/catalogue_pixels`

Catalogue sources placed in a cube's (or image's) pixels with the file's full
WCS (astropy: CAR, TAN, SIN, a CD or PC matrix), in its own celestial frame;
a source catalogued in another frame (RA/Dec on a Galactic cube) is converted
first.

| Field | Default | Meaning |
|---|---|---|
| `dataset_id` | | the cube |
| `source` | `catalogue` | `catalogue` (one open in the session), `vlkb_sources`, `vlkb_filaments`, `crossmatch` |
| `catalogue_id` | | for `catalogue` |
| `label_field` | a name column | labels |
| `spectral_field` | `freq`, `v_rad`, `v_opt`, `v_app`, `vlsr`… when present | places a source in its channel |
| `spectral_unit` | the column's own: a VOTable, FITS or IPAC unit, or one in the name (`vlsr (km/s)`); else Hz for a frequency, the axis's unit for `v_app`, km/s for the others | |
| `spectral_convention` | `radio`, `optical` or `relativistic`; else the name's (`v_rad` radio, `v_opt`/`cz` optical, `vlsr` radio), the axis's own for `v_app` and for a column of no stated convention (`velocity`, `vel`, with a warning) | |
| `bands` | `["ft"]` | VLKB compact sources: the bands drawn (`ft` = band-merged) |
| `service_url` | the backend's VLKB | |
| `crossmatch_catalogue`, `radius_arcsec` | `simbad`, 60 | for `crossmatch` |
| `max_sources` | 2000 | |

The VLKB is queried over the cube's Galactic footprint with the desktop's own
ADQL (`compactsources.sed_view_final`, `filaments.filaments`) and margin (10 %
of each side plus 0.01°); a box across l = 0 is two longitude ranges
(`glon BETWEEN 359.4 AND 360 OR glon BETWEEN 0 AND 0.6`). At most 20 000 rows
come back (TAP MAXREC; a warning says when the limit was hit); the answer goes
through a temporary file, removed after, and the projection runs off the
event loop. A compact
source is the ellipse of its FWHMs built in pixels around its projected centre
(the semi-major axis along +x, turned counter-clockwise by the catalogue's PA,
sized with the mean pixel scale), each band on its own centroid or the
band-merged one; a filament is its contour, each vertex projected.

A spectral position is read in the cube's own spectral reference frame (no
change between barycentric and LSR): a frequency on any axis, a velocity of
the axis's own convention as it is (`v_app`, which SoFiA writes in the axis's
type), one of another convention through the rest frequency (refused without
`RESTFRQ`, and the source is then placed on the sky only, with a warning).
Sources the sky places on the cube but whose channel is outside it are left
out, counted in `n_outside`, and a warning says how many: a wide-band survey
over a narrow-band cube keeps the channels of the sources it shares. Only when
none falls in the channels and at least 95 % fall outside, and the column
declares no unit (no `spectral_unit`, no unit in the file or in its name), is
a reading a thousand times larger or smaller tried (m/s against km/s): when it
brings most of them in it is applied, with a warning naming it. A convention
is never changed: one given in `spectral_convention` or stated by the name may
rightly put a catalogue outside a narrow band. Otherwise every source is
placed on the sky only, and a warning says so. A warning also says
when the cube's `SPECSYS` is not barycentric or heliocentric, when the
catalogue declares another frame (a FITS table's `SPECSYS`), and when an LSR
column (`vlsr`) meets a barycentric cube: no frame change is applied.

Answer (JSON): `valid`, `name`, `source`, `frame` (`galactic` or `icrs`:
the cube's), `source_frame` (the frame of the sources' `lon`/`lat`: the
catalogue's own, `galactic` for the VLKB), `shape` (`[nx, ny, nz]`),
`pixel_scale_arcsec`, `n_input`, `n_sources`, `n_polylines`, `n_outside`,
`n_unplaced`, `spectral` (`used`, `field`, `note`), `warnings`, `sources`
(`x`, `y`, `z` or null, 0-based pixels; `label`, `band`, `lon`, `lat`,
`ellipse` as `[[x, y], …]` or null; `row`, the catalogue's row, for a
catalogue of the session) and `polylines` (`label`, `points`). A VLKB failure
answers `valid: false` with the message.

## `POST /v1/catalogue/classified_votable`

A catalogue classified in the headset (issue #279) written as a VOTable in the
Workspace Exports, for the desktop's catalogue table and for saving
(`app/catalogue_classified.py`).

| Field | Default | Meaning |
|---|---|---|
| `dataset_id` | | the cube the catalogue is placed over |
| `catalogue_id` | `""` | a catalogue of the session: all its columns and rows are kept |
| `source_frame` | `icrs` | the frame of the sources' `lon`/`lat` (`icrs` or `galactic`) |
| `flag_set` | `[]` | the flags offered (at most 8; written in the table's metadata) |
| `flag_column` | `vr_flag` | the flag column's name (`^[A-Za-z_][A-Za-z0-9_]{0,31}$`; refused when the catalogue has it) |
| `sources` | `[]` | at most 20 000: `origin` (`catalogue`, `overlay`, `user`), `row` (for `catalogue`), `label`, `lon`, `lat`, `frame` (overrides `source_frame`), `x`, `y`, `z` (0-based pixels), `velocity_kms`, `flag`, `note` |
| `output_basename` | `<catalogue>_vr_classified` | |

With a catalogue, the table is every original column and row in the file's
order, then `<flag_column>` ('' when not classified), `vr_note`, `vr_origin`,
`vr_x`, `vr_y`, `vr_z` (NaN when not placed; `vr_z` NaN for a source on the
sky only) and `vr_velocity_kms` (the cube's velocity at `vr_z`). The page
sends only the classified rows; the others keep empty classification columns.
The sources found by eye (`origin: user`) are appended: their original
columns are null except the name column and the sky columns, filled only when
the source's frame is the catalogue's. A row out of the catalogue is 422.
Without a catalogue (VLKB sources, a cross-match) the table is the sources
sent: `name`, `ra`/`dec` (or `glon`/`glat` for `source_frame: galactic`), then
the same classification columns. Answer: `valid`, `output_path`,
`workspace_filename`, `n_rows`, `n_flagged`, `n_user`, `columns`.

## `POST /v1/cube/mask_layer_bin`

A label mask over a cube for the headset's mask layers (issue #282,
`app/mask_layer.py`). Body (JSON): `dataset_id` (the cube), and
`mask_dataset_id` (a mask opened in this session) or `mask_path` (a file on
the backend). The mask must be on the cube's grid: the same size and axis
type on each of the viewer's axes, and the same pixel-to-world transform
(sample pixels of the mask taken to the world and back through the cube's WCS
land within 0.001 pixel: CRPIX, CRVAL, CDELT, a CD or PC matrix and units,
which WCSLIB normalises, are all compared; a degenerate axis of either is
ignored); otherwise 422 naming what differs. An integer mask, or a float one
holding whole numbers (every plane is checked); a negative value, a fraction
or an ID above 2³¹ − 1 is 422. At most 16 Mi
voxels in sources and 65 535 sources. A mask file that cannot be read is 404.

Answer (`application/octet-stream`): a uint32 little-endian length, that many
bytes of JSON (padded with spaces to a multiple of 4), then the voxels as
uint32 little-endian linear indices of the full grid, `(z * ny + y) * nx + x`,
grouped by source (ascending ID) and sorted within each. The JSON: `shape`
(`[nx, ny, nz]`), `sources` (`id`, `count`, `offset` into the indices,
`bbox` `[x0, x1, y0, y1, z0, z1]`, `centroid` `[x, y, z]`, geometric, 0-based
pixels), `max_id`, `n_voxels`, `mask_path`, `mask_name`. Read one channel at a
time.

## `POST /v1/cube/mask_layer_save`

The headset's refined mask written as FITS in the Workspace Exports. Body
(`application/octet-stream`, at most 65 MiB): a uint32 little-endian length,
that many bytes of JSON, padding to a multiple of 4, then each edited source's
sorted voxel indices (uint32) in the order of `edits`. The JSON:
`dataset_id`, `mask_dataset_id` or `mask_path`, `edits` (`[{id, count, new}]`:
each source's whole new voxel list), `removed` (IDs emptied, at most 65 535,
each 1..2³¹ − 1; anything else is 422), `history`
(lines for the header, at most 20), `output_basename` (default
`<mask without _mask>_vr_mask`).

The original mask is read one slab of channels at a time and written out
sequentially, each edited and removed source's voxels cleared in the same pass
(one pass whatever the number of IDs); then each edited source is set to its
list through a memory map of the output. The file is built beside its
reserved name and moved over it in one step. HCG 44's mask (700 × 700 × 158)
saves in 0.2 s, with about 350 MB of the original's pages mapped. A voxel of another
source claimed by an edit, or an index outside the grid, is 422 (the headset
never takes one: Add and Replace claim free voxels). The file keeps the
original header (`BITPIX` 32), adds HISTORY cards (`source N edited`, `added`,
`removed`), and has the original's shape. Answer: `valid`, `output_path`,
`workspace_filename`, `n_sources`, `max_id`, `n_voxels`, `edited`, `removed`.

## `POST /v1/velocity/lines_bin`

The streamlines and arrows of a velocity field (`/v1/velocity/open`), traced
on the display grid `/v1/velocity/grid_bin` serves (its spacing and origin)
with the desktop viewer's filters and settings: seeds from `vtkPointSource` in
a ball at the grid's centre of radius 0.3 × its longest side, Runge-Kutta 4-5
in both directions, at most 2 × the longest side and 2000 steps; arrows at
grid points chosen by `vtkMaskPoints` (uniform spatial sampling). The seeds
and the arrows' points come from a random sequence started at `seed`, so the
same request gives the same lines.

| Field | Default | Meaning |
|---|---|---|
| `dataset_id` | | the field |
| `box_mpc` | omitted | the box the lines are traced with (Mpc), as the desktop shows it: omitted, the one the field was opened with; `0`, unit cells (the same rule as `_effective_box` of the basin and density routes) |
| `max_dim` | 128 | the display grid (as `grid_bin`) |
| `n_seeds` | 200 | 1 to 5000 |
| `max_arrows` | 50 000 | 1 to 500 000 |
| `seed` | 1 | |
| `lines`, `arrows` | true | either may be left out |

The body is little-endian: the streamlines' points (n × 3 `float32`, field
units), their speed (n `float32`), the start of each line in those points
(lines + 1 `uint32`), the arrows' positions (m × 3 `float32`) and vectors
(m × 3 `float32`). Headers: `X-Visivo-Line-Points`, `X-Visivo-Lines`,
`X-Visivo-Arrows`, the grid's `X-Visivo-Width`/`Height`/`Depth`,
`X-Visivo-Full-Shape`, `X-Visivo-Downsample-Factor` (and
`X-Visivo-Downsample-Factors` when the grid route reports per-axis factors),
`X-Visivo-Spacing`, `X-Visivo-Origin`, `X-Visivo-Bounds`,
`X-Visivo-Glyph-Cell` (the smallest cell, for the arrows' scale
3 × cell / speed max), `X-Visivo-Full-Speed-Min`/`Max`, `X-Visivo-Seeds`,
`X-Visivo-Seed`, `X-Visivo-Box-Mpc` (the box the lines were traced with, 0 for
grid units), `X-Visivo-Unit`, `X-Visivo-Frame`. 404 (`text/plain`) for a
dataset that is not a velocity field in the session.
