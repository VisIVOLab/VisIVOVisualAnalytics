# Points, catalogues and flows for the headset

These routes give the [VR page](../user-guide/vr-headset.md) the data types
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
`num_rows`, `fields` (the numeric columns), `axes` (the default X, Y, Z) and
`sky` (positions computed from RA/Dec, `@x`, `@y`, `@z`). 422 for a dataset
that is not a table of points.

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
| `filters` | `[]` | numeric filters `{field, op, value}`, as the point session's |

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
the maximum of each axis), then one colour code per row when a colour column
is given (code 0 also for a non-finite value). Headers: `X-Visivo-Count`,
`X-Visivo-Total-Rows`, `X-Visivo-Step` (table rows per sampled row),
`X-Visivo-Bounds` (`xmin,xmax,ymin,ymax,zmin,zmax`), `X-Visivo-Axes`,
`X-Visivo-Scalar-Type: uint16`, and with a colour column
`X-Visivo-Color-Field`, `X-Visivo-Color-Range`, `X-Visivo-Color-Log` (`0`/`1`)
and `X-Visivo-Data-Range` (the column's range over the sample).

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

Answer (JSON): `valid`, `name`, `source`, `frame` (`galactic` or `icrs`),
`shape` (`[nx, ny, nz]`), `pixel_scale_arcsec`, `n_input`, `n_sources`,
`n_polylines`, `n_outside`, `n_unplaced`, `spectral` (`used`, `field`,
`note`), `warnings`, `sources` (`x`, `y`, `z` or null, 0-based pixels; `label`,
`band`, `lon`, `lat`, `ellipse` as `[[x, y], …]` or null) and `polylines`
(`label`, `points`). A VLKB failure answers `valid: false` with the message.

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
