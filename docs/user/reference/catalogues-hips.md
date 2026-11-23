# Catalogues, HiPS, and SAMP

These tools put cubes and images in context: a 3-D viewer for large
catalogues, catalogue overlays on images and cubes, a HiPS browser for
all-sky surveys, archive queries (hips2fits, cone search, VLKB catalogues) and
SAMP exchange with TOPCAT, Aladin and DS9.

For a walk-through, see
[3-D catalogues, HiPS and cross-match](../use-cases/06-catalogues-hips-crossmatch.md).

## 3-D catalogue viewer

Shows a catalogue as glyphs in 3-D. Open it with *Data ▸ Open 3D Catalogue
(CSV)…* (⌘⇧O), with *File ▸ Open…* on a catalogue, or from the Command Palette
(⌘K). It reads delimited text (comma, tab or semicolon; decimal commas are
recognised), VOTable, FITS, speck and IPAC tables on the backend, and VisIVO
Binary Tables of points ([VBT tables](vbt-viewer.md)).

A sky catalogue (RA, Dec) is placed in 3-D from a distance or a redshift; a
table of X, Y, Z columns is drawn in its own units (*AXIS MAPPING* picks the
columns).

### Frame and distance

- *Frame*: FK5 / J2000 (default), Galactic (l, b) or *Supergalactic (SGL,
  SGB)*.
- Distance mode: *Real-space (comoving)* (default) or *Redshift-space (cz/H₀)*,
  the naive Hubble distance with the model's H₀ (Planck18 67.66, WMAP9 69.32,
  or the custom model's). Against an independent distance, the difference is
  the line-of-sight peculiar velocity.
- Distance source in real space, in this order: a `distance`, `dist` or
  `dMpc` column when > 0 (used under every cosmology), then a redshift column
  (`z`, `REDSHIFT`, `ZSPEC`, `ZMEAN`, `ZPHOT`) > 0 converted with the chosen
  cosmology. Otherwise the source has no distance.
- *COSMOLOGY* card: *Model* is *Planck18* (computed in the client), *Planck15*,
  *Planck13*, *WMAP9* (computed on the backend) or *Custom*, the ΛCDM set in
  *Tools ▸ Cosmology Calculator* with *Use in 3-D Catalogues*
  ([Cosmology tools](cosmology.md)). The card says what produced the positions
  on screen.

Sources without a distance (missing, z ≤ 0, or z = 0, which catalogues use for
"not measured") are left out and counted under the Cosmology card. *Tools ▸
Unplaced Sources…* lists them with Name, RA, Dec, cz and the reason. Sources
with 0 < cz < 1000 km/s are placed but flagged as uncertain (peculiar
velocities of about 300 km/s make the distance uncertain by 30 % or more); a
measured distance is not flagged. A catalogue with no redshift and no distance
column at all is drawn on the celestial sphere at a radius of 300 Mpc, which
is not a distance.

Selecting a source shows where its distance came from (*Distance 12.3 Mpc,
from z (comoving, Planck18)*).

### Source representation and visual encoding

- *Shape*: *Ellipsoid*, *Sphere*, *Point*, *Cross*, *Gaussian splats*
  (additive light, for simulation particles; *Scale* sets their size; their
  brightness does not depend on the number of particles; for numbers use a
  projected map), and the
  separate *Ray-traced light* check, which draws a simulation for figures on
  the GPU from data prepared by the backend
  ([Ray-traced particles](raytraced-particles.md)).
- *Size*: *Fixed*, *Major axis* (the catalogue's size), *LLS*, *Flux*.
- *Colour by*: *Morphology color* or any numeric column.
- *Colormap* and *Range and scale…*: the map, and an editor over the column's histogram
  for Min, Max and the scale (Linear, Log, Sqrt, Square, Power). A colour bar
  on the right shows the range and the column.

### Selection and table

- Hover a glyph to see its fields in *Inspector ▸ Properties*.
- Click to select: a ring marks it, the *Source Info* page shows the row and
  the *Catalogue Data* table highlights it. Esc, a click on empty space or
  *View ▸ Clear Selection* deselects.
- Click a table row to centre the camera on the source.
- *Camera*: *Front*, *Back*, *Left*, *Right*, *Top*, *Bottom* (Ctrl+1 to
  Ctrl+6), in the Local, Remote and ray-traced views alike.

### Filters and loading

- *Inspector ▸ Analysis*: *Add Filter* stacks conditions (`<`, `≤`, `>`, `≥`,
  `=`, `≠`, `contains`, `startswith`, `endswith`), combined with AND and
  applied on the backend.
- Tables over 50 000 rows load in pages: *Load more (N remaining)* and *Load
  all*.
- *View ▸ Rendering*: *Auto*, *Local* or *Remote*. On a remote backend that
  renders on its GPU, *Remote* (or *Auto*, when not every row is loaded) draws
  every row there while the loaded rows stay in the window for picking and the
  table; the status bar says which (*Remote · all N rows*, *Local · n of N
  rows*). The backend draws other shapes as points, morphology colour in one
  colour, and applies only numeric filters and 3-D regions. If the stream
  drops (sleep, network, tunnel), the view reconnects by itself and the status
  bar shows the attempts; after about two minutes it falls back to the loaded
  rows.

### 3-D regions and selection diagnostics

In a Cartesian table (VBT points, Gadget snapshots, Cartesian catalogues):

- *Select 3-D region…*, under the filter list, adds a named *Box* or *Sphere*
  with *Inside* or *Outside*, pre-filled from the scene bounds. Names default
  to *Region 1*, *Region 2* …, and a name already in use is refused. Regions
  combine with the other filters, act on the whole table on the backend, and
  last while the window is open.
- *Tools ▸ Selection Diagnostics…*: an exact histogram and summary
  statistics of one quantity, optionally weighted, over every row the
  **applied** filters match (those last sent with *Apply*), not only the
  loaded ones. It follows a new *Apply* while open. A long run shows a
  progress bar; *Cancel* stops waiting. *Save CSV…* keeps the numbers with
  their units and selection, *Save PNG…* the plot. Non-finite values and
  negative weights are left out and counted.

## Catalogue overlay on cubes / images

Overlays a catalogue on the 2-D slice, moment map or image. Needs a celestial
WCS. The entries are under *Tools ▸ Catalogue* in both viewers:

- *Load Catalogue Overlay…*: reads a delimited text table with `RA` and `DEC`
  (or `DEC_DEG`) columns in degrees, and draws the sources. A `.reg` or
  `.crtf` file is read as regions ([Regions, PV diagrams and
  noise](region-pv-noise.md)). Loading replaces the current overlay.
- *Show Catalogue Overlay*: shows or hides the markers without unloading.
- *Show Catalogue Labels*: shows or hides the names (off by default; at most
  about 200 are drawn).
- *Clear Catalogue Overlay*: removes markers, labels and table.

The image viewer adds *Cross-match with Catalogue…*, the VLKB overlays, *VLKB
Catalogue Query…* and *Export Catalogue…* ([Image viewer](image-viewer.md)).
The sources are projected with the image WCS in whatever frame the viewer
shows.

### Overlay file formats

A file is read by its content. One whose first lines split into the same number
of fields with a comma, a tab or a semicolon is a table: `LABEL`, `ID` or
`NAME` names the sources and every other column becomes a field. A file of DS9
region statements, or one that looks like a DS9 region file, is read as
regions. A `.csv` or `.tsv` that is neither is refused with the reason.

### Querying the VLKB directly

In the image viewer, *Tools ▸ Catalogue ▸ Overlay VLKB Compact Sources*,
*Overlay VLKB Filaments* and *Overlay VLKB Bubbles* query the VLKB over the
image footprint, or over a rectangle you drag, and overlay the result:

- Compact sources: the Hi-GAL band-merged catalogue. Every band comes back;
  each band's ellipse is centred on its own peak, and band toggles show one
  wavelength at a time.
- Filaments and bubbles: drawn as contours.

They are disabled, with the reason in the tooltip, on an image without a
celestial WCS. The whole row is returned, so every band's flux is available
for SED fitting.

### The catalogue table

The overlay fills a table under the image: the catalogue's own names, RA and
Dec for every entry with a sky position, size and angle when given, and every
other column of the catalogue in file order. A band-merged catalogue is a tree:
the merged source is the parent row and its per-band detections are children.

- Click a row: highlights the source (the view does not move).
- Double-click a row, or a source in the image: zooms to it.
- Hover a marker: highlights its row.
- Right-click a row: *Fit SED of this source…* ([SED fitting](sed-fitting.md))
  and *Zoom to this source*.
- *Tools ▸ Catalogue ▸ Export Catalogue…* (image viewer): writes what is on
  screen to CSV, with names, positions in the overlay's sky frame (named in the
  header), pixel coordinates, shapes and every extra field.

A truncated last row of a file is skipped and reported.

## HiPS viewer

Browses all-sky HiPS surveys. Open it with *Data ▸ HiPS Viewer…*, from
*Archives* in the Data Hub, or from the Command Palette; it opens as a tab of
the main window. Tiles are fetched through the backend, which needs network
access; the desktop does not.

- *Browse…*: the image surveys registered with CDS (about 1400), grouped by
  category. *Search:* keeps rows that contain every word you type. The list is
  cached for a week; *Refresh* reloads it. Only sky image surveys are listed.
- *Survey:* and *Open*: open any HiPS by its root URL, from any server.
- Drag to pan, wheel or *−* / *+* to zoom, *⌂* to reset. The order in use is
  shown next to the zoom buttons. RA increases to the left.
- *Sky projection*: *TAN* (default, up to a 120° field), *SIN* (one
  hemisphere), *ARC* (true distances from the centre), *AIT* (whole sky),
  *MOL* (whole sky, equal area), *MER* (cut at ±85°). Past 90° the whole sky is
  drawn from the survey's AllSky image.
- *Target:* and *Go*: resolves a name with Sesame and centres on it.
- *Grid*: RA/Dec graticule in the current projection.
- *Cat*: overlays a catalogue on the field, either from a VO cone search
  (VizieR, NED or a pasted URL; centred on the view with a radius of half the
  field, at most 5°, saved as a CSV in the Workspace) or from a delimited text
  table on the backend. Only rows inside the visible field are drawn; the
  count is in the status bar.
- *Overlay:* a second survey on top, with an opacity slider; *⧸⧸* switches to
  a side-by-side split.

Galactic and equatorial surveys line up: every tile is returned in ICRS.
There is no MOC display, per-survey colour map or stretch, or pixel readout of
FITS HiPS; for those, send the image to Aladin over SAMP.

## SAMP

The *SAMP* menu (Data Hub and every image, cube and 3-D catalogue window)
exchanges data with TOPCAT, Aladin, DS9 and other VO tools: *Connect To Hub*,
*Disconnect From Hub*, the Data Hub's *Send Current FITS Dataset* and *Send
Current Catalogue* (the last dataset opened), and each viewer's own *Send … To*
items. Received tables open in the 3-D catalogue viewer, images in the image or
cube viewer, and sky positions centre the HiPS viewer. Every item, the linked
rows with TOPCAT and troubleshooting are on
[Working with TOPCAT, Aladin and DS9](interoperability.md).

## Getting data out of the archives

These entries write their result into the Workspace.

### HiPS cutout (hips2fits)

*Data ▸ HiPS Cutout (hips2fits)…* reprojects a HiPS survey onto a WCS you
choose, through the CDS service, and opens the FITS. Give a survey (any CDS
id), a position, a field of view and a size; cutouts over 30 Mpx are refused.
Percentile cuts and stretch apply only to `png` and `jpg` output.

### Cone search

*Data ▸ Cone Search…* queries a VO Simple Cone Search service (listed, or a
pasted URL) around a position. The rows are shown and saved as a CSV; *Open as
Catalogue* opens them in the 3-D catalogue viewer.

### VLKB Catalogue Query

*Tools ▸ Catalogue ▸ VLKB Catalogue Query…* (image viewer) queries the VLKB
catalogues over a Galactic box prefilled with the image footprint:

| Preset | Content |
|---|---|
| Compact sources, band-merged | one row per source, fluxes in every band |
| Compact sources, one band | a single Hi-GAL band table |
| Filaments | filament spines with their branches |
| Bubbles | bubble rims |
| 3-D selection | sources with distances, with Galactocentric `x`, `y`, `z` |

You can also write the ADQL; the query sent is shown and can be edited. The
result is saved as a CSV and can be opened as a catalogue.

Details: [Catalogues, HiPS, and SAMP: methods and implementation](../../technical/methods/catalogues-hips.md),
[3-D catalogue viewer design](../../technical/design/catalogue3d-viewer.md),
[`/v1/catalogue/*`](../../technical/backend-api/datasets.md),
[`/v1/hips/*`](../../technical/backend-api/sky-services.md),
[`/v1/samp/*`](../../technical/backend-api/samp.md).
