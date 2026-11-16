# VLKB archive

The *VLKB* tab of the main window (beside *Data Hub* and *SKAVA*; also *VLKB
Inventory* in the Command Palette, ⌘K / Ctrl+K) queries the ViaLactea
Knowledge Base. You give it a region of the Galactic plane, it lists every
image and spectral cube that covers it, and it cuts out and opens the pieces
you choose. The archive is the public VLKB at `https://vlkb.ia2.inaf.it`,
changed in *Settings ▸ Data Access ▸ VLKB URL*. Cutouts are downloaded to the
backend, which needs network access.

To overlay the VLKB source catalogues on an image you already have, see
[Catalogues, HiPS, and SAMP](catalogues-hips.md).

## Choosing the region

- *Selection*: *None*, *Point* or *Rectangular* decides what you can draw on
  the all-sky view; drawing fills in the fields. Only the fields are queried.
- *glon*, *glat*: the centre, in galactic degrees.
- *radius*: queries a cone (`CIRCLE l b r`).
- *dl* and *db*: query a box (`RANGE l₁ l₂ b₁ b₂`); dl and db are full widths.
- *Query*: runs the search. It stays disabled, with the reason, until the
  centre and a size are given.

A line under the fields shows the exact region string sent, which is also the
one the cutout uses. A box across l = 0 is sent as a wrapped range (`RANGE 359
3 …`); a box 360° wide is sent as `RANGE 0 360`.

## The inventory

The result lists every dataset that covers the region, grouped by regime
(Radio, Submm / mm, Far-IR, Near-IR / Mid-IR, Spectroscopy, …) and product type,
with a count.

- *Dataset*: survey and band or line (*THOR 1.44 GHz*, *GLIMPSE I 3.6 um*).
- *Coverage*: `full` (highlighted green) when the dataset covers the whole
  region, `partial` when it only overlaps it.
- *Identifier*: the tail of the archive identifier, which tells apart rows
  with the same name.
- Filter box: matches survey, band and line names.
- *Only datasets that fully cover the region*: hides partial coverage.

## Downloading

- Double-click a dataset, or select several (⌘/Ctrl-click, Shift-click) and
  press *Download & Open*. Each dataset is cut to the region by the archive,
  downloaded to the backend and opened: images as layers of the viewer this
  inventory session opened, cubes in a cube viewer. Layers load one after
  another; a failed download does not stop the rest.
- Right-click a dataset: *Open as a Layer* (*Open* for a cube), *Open in a
  New Window* (an image in its own viewer) and *Save File…* (download without
  opening).

## Many datasets at once

*Request as batch (N)…* (two datasets or more) sends the whole selection as one
asynchronous archive job, instead of one request per dataset. The job keeps
running on the archive if you close the window; *Download & Open* stops.

The batch window shows the phase (*QUEUED*, *EXECUTING*, *COMPLETED*) and the
time elapsed, then marks each dataset ready or failed; hover a failed row for
the archive's reason.

- *Abort job*: stops the job on the archive. Closing the window while it runs
  asks whether to *Stop the job* or *Leave it running* (a job left running
  finishes, but its results can no longer be collected from that window).
- *Download*: fetches and unpacks the results on the backend without opening
  them. *Show folder* shows where they are.
- *Open…*: opens the results (downloading first if needed): the images of each
  source as layers of one viewer, each cube in its own window. Above twelve
  files it asks first.
- *Retry report*: reads the job's report again after a network error; the
  results are still on the archive.

## A list of sources

*Data ▸ VLKB Cutouts for a Source Table…* cuts out the same surveys around many
positions.

1. *Open file…*, or drop a file on the window: a table with a header line and
   one source per line, with an identifier and galactic l and b. Tab, comma,
   semicolon and space separators are read; `#` lines are ignored.
2. Choose the *ID*, *Longitude (l)* and *Latitude (b)* columns (guessed from
   names such as `id`, `glon`, `glat`) and the region: *Cone around each
   source* (a radius) or *Box around each source* (half-widths dl × db).
3. Tick the sources and press *Query*. The archive is searched once per
   source; *Stop* ends early and keeps what was found.
4. Read the *Images* and *Cubes* tabs: one row per source with the number of
   datasets found. A survey cell is green when the source has every species and
   transition of that survey found in the table, yellow when it has some, red
   when it has none. Hover a cell for the products.
5. Tick sources and products (*All*, *None* help).
6. *Request N cutouts…* sends, for every ticked source and product, the
   dataset that best covers the source as one batch job, followed in the batch
   window. One job takes at most 500 cutouts.

## When the archive says no

VisIVO Visual Analytics checks that each download is a FITS file, whatever type
the service declares. When it is not, it shows the archive's own message, for
example *The service returned application/fits instead of FITS: UsageError :
overlap computation could not be completed with the given arguments.* A gzip
or tar archive is named as such. A message of this kind describes a server
failure and is worth sending to the archive's operators.

Details: [VLKB routes](../../technical/backend-api/sky-services.md),
[Configuration](../../technical/configuration.md) (`VISIVO_VLKB_URL` for requests
that do not give an archive address).
