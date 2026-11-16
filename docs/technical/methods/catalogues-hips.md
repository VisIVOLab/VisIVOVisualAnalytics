# Catalogues, HiPS, and SAMP: methods and implementation

User reference: [Catalogues, HiPS, and SAMP](../../user/reference/catalogues-hips.md).
The 3-D catalogue viewer's internals (distance rules, `cz` precision,
cosmology integration) are in [Catalogue 3-D viewer](../design/catalogue3d-viewer.md);
the backend's delimited-text reader is in [Datasets](../backend-api/datasets.md).

## Cosmology models of the 3-D catalogue viewer

- Planck18 is integrated in the client (`H₀ = 67.66`, `Ωm = 0.3111`, flat
  `ΩΛ = 1 − Ωm`), so it needs no network call. It agrees with astropy's
  Planck18 to better than 0.1 % out to z = 20, pinned by
  `tests/test_catalogue_cosmology.cpp`: the two have to match, or switching
  the selector away from the default and back would move every source.
- Planck15, Planck13, WMAP9 and Custom are computed by the backend
  (`POST /v1/cosmology/distance/batch`, asynchronous); the viewer re-projects
  the cloud when the results arrive.

On a remote backend that renders server-side, positions of a sky catalogue are
computed there from the same RA/Dec, distance or redshift columns and with the
frame, cosmology and distance convention chosen in the window. Positions far
from 0 keep their separations on the backend: a table at a large offset
(1e9, 1e9 + 1, 1e9 + 2) is drawn as three points, with the box and camera in
the table's own units.

## Overlay file formats

The overlay file is read by what it holds, not only by its name, with the rule
the backend applies to delimited text:

- A `.reg` or `.crtf` file is read as regions.
- A file whose first lines (up to ten, `#` comment lines and blank lines
  aside) split into the same number of fields, two or more, with a comma, a
  tab or a semicolon (tried in that order) is read as a table with that
  separator, whatever its extension. It needs `RA` and `DEC` (or `DEC_DEG`)
  columns in degrees; a `LABEL`, `ID` or `NAME` column names the sources, and
  every other column becomes a field of the source. A field that starts with
  `"` is quoted and may contain any separator, a line break, or `""` for a
  quote; a `"` anywhere else is an ordinary character (`3"`). Lines may end in
  `\n`, `\r\n` or a lone `\r`.
  - The header is the first line that does not start with `#`, unless the `#`
    line right above it names as many columns as it has (none of them a
    number) and that first line holds numbers: then `#ID,RA,DEC` or
    `# ID, RA, DEC` over `1,10,20` is the header, and `1,10,20` the first
    source. A title such as `# Sources, v2, J2000` above a real header stays a
    comment.
  - Below the header, a `#` line is a comment unless it has as many fields as
    the table: then it is a source (one named `#S5`). A `#` line inside a
    quoted field that spans lines is part of that field.
  - A semicolon table whose sampled rows use a decimal comma (`10,5`) in at
    least two columns, with no decimal point among them, is a decimal-comma
    table. In it, a column whose every value is a number (`10,5`, `11` or
    `20.5`) is read as numbers, in the extra fields too, so a SED fit reads
    its fluxes; a column with any text in it stays as written.
  - A comma file with uneven rows (short rows, a trailing comma on each row, a
    single column) is still read as comma-separated when some column holds
    numbers (`NaN` and `inf` count, as in the backend).
  - The one exception is a file whose every one of those lines is a DS9
    region statement: bare `fk5;point(10.1,20.2)` lines split on their commas
    like a two-column table, and are read as regions.
- A file that is not a table is read as DS9 regions when it looks like one:
  ds9's `# Region file format` comment, a `global` line, a coordinate system
  such as `fk5` on its own line, or a shape written as `circle(…)`. A column
  called `global_id`, `Box (arcsec)` or `Line (GHz)` does not make a table a
  region file.
- A `.csv` or `.tsv` file that is neither is refused with a message saying
  why. A file with any other name falls back to the DS9 reader, and its message
  says it is not a table either.

The table reader is quote-aware and reads records, not lines. A truncated last
row is skipped and counted, and the count is reported separately from the
"loaded" message. Sources are projected through the image WCS with `wcscon()`
for the frame conversion.

## VLKB catalogue overlays

*Overlay VLKB Compact Sources* queries `compactsources.sed_view_final` (the
Hi-GAL band-merged catalogue); *Overlay VLKB Filaments* queries
`filaments.filaments`. The query is `SELECT *` and the client reads the row by
name from the response header: the per-band fluxes are the reason the
catalogue exists, naming the columns would need a `TAP_SCHEMA` round trip, and
one of them (`distance`) is an ADQL reserved word that makes the service answer
400 unless quoted.

## HiPS viewer

- The survey list comes from the CDS MOCServer. HiPS catalogues, cubes and
  planetary maps are left out: the viewer cannot draw the first two as tiles,
  and the last have no sky position.
- The backend reads the survey's `properties` file (orders, tile format,
  native frame). Over half the registry is galactic: the requested position is
  converted into the survey's frame to choose the tiles, and every tile comes
  back in ICRS.
- Only the tiles the viewport needs are requested
  (`/v1/hips/{id}/query_tiles`). While a new order is in flight, each missing
  tile is drawn from the coarsest level already cached, and the AllSky mosaic
  fills a deep zoom immediately.
- Projections: `TAN` diverges towards a hemisphere and stops at 120°; `SIN` is
  exactly one hemisphere. `MOL` is equal-area and so not conformal: at the
  centre it stretches by 11 % vertically and squeezes by 10 % horizontally;
  its field of view is their geometric mean, so switching to it preserves the
  area on screen. `MER` stops at ±85°.
- The catalogue overlay (`/v1/hips/catalogue_overlay`) selects by angular
  separation from the centre, with the radius that covers the viewport
  including its corners, not by a range of right ascension (meaningless near a
  pole). Resizing the window refetches.
- Target names are resolved through Sesame (`/v1/resolve/target`).

## Archive queries

- hips2fits: every parameter is checked before the request leaves (an empty
  survey, a field of view of zero or a cutout over 30 Mpx is refused). For
  FITS output the percentile cuts and stretch are not sent.
- Cone search: `RA`, `DEC`, `SR` and `VERB` are merged into whatever query
  the service URL already carries.

## SAMP

- Before preparing a file, the client asks the hub whether any running tool
  takes that kind of data, and stops with *No running tool takes …* if none
  does. A table that is not a VOTable is converted to a binary VOTable
  (BINARY2) and sent with a `.vot` name. Preparing 2.3 million rows takes about
  40 s.
- Received files are fetched by the client and handed to the backend, so a
  remote backend works too (the hub's `http://127.0.0.1:…` addresses only
  mean something locally). The backend's copy is temporary, so received data
  is not added to *Recent Datasets*; a local file the sender names by path is.
- Rows are matched by their place in the original table (SAMP counts from 0,
  the interface from 1), so filters, sorts and the even sample loaded for a
  large table do not break the link. A catalogue combined from several files is
  not linked.
- All `/v1/samp/*` routes need the operator token except
  `GET /v1/samp/files/{token}`, which the other tools use; each file is
  reachable only by its random token, for one hour (`VISIVO_SAMP_FILE_TTL`).
  The client's own SAMP endpoint listens on loopback only and refuses calls
  without the hub's key. See [SAMP API](../backend-api/samp.md).
