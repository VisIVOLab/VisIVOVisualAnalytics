# Catalogues, HiPS, and SAMP

VisIVO is not just a viewer for one cube at a time — it's also a workbench
for putting cubes / images in context: overlay source catalogues, browse
all-sky HiPS surveys at any zoom level, and send images and catalogues to
TOPCAT / Aladin / DS9 via SAMP.

## 3-D catalogue viewer

A dedicated 3-D scatter viewer (`vtkWindowCatalogue3D`) renders large
catalogues — millions of entries — as glyphs in Cartesian (RA / Dec /
distance) space.

Opens from **Data → Open 3D Catalogue (CSV)…** (⌘⇧O; delimited text,
VOTable, FITS, speck or IPAC on the backend filesystem), from **File → Open…**
when the file is a catalogue, or from the Command Palette (⌘K → "open 3d
catalogue"). Delimited text (`.csv`, `.tsv`, `.txt`) may be separated by
commas, tabs or semicolons: the backend reads it with the separator it saw
when it identified the file, so a tab-separated table has the same columns as
its comma version. A quoted field may contain any of the separators. Blank
lines are skipped, and so are `#` comment lines, with two exceptions: a header
written as a comment (`#id,ra,dec` right above the first row of numbers) is
the header, and a `#` line with as many fields as the table is a row (a
source named `#S5`). A semicolon table whose numbers are written with a
decimal comma (`12,5`, as spreadsheets export them in many European locales)
is read as numbers when at least two columns are written that way. A comma
file with uneven rows opens as it always did. A text file that is not a table
is refused with a message saying so. A
**VisIVO Binary Table of points** (a simulation snapshot, a mock catalogue)
opens here too, from any route: its X/Y/Z columns are the positions, read in
place from the binary ([VBT tables](vbt-viewer.md#point-clouds)).

### Coordinate frame & distance

The viewer needs both a sky frame and a distance for each entry:

```{list-table}
:header-rows: 1
:widths: 28 72

* - Frame combo
  - FK5 / J2000 (default), Galactic (l, b) or Supergalactic (SGL, SGB).
    Conversion is done with
    `wcscon()` from libwcs the moment you switch frame.
* - Distance mode
  - **Real-space (comoving)**, the default, or **redshift-space (cz/H₀)**,
    which places every source at the naive Hubble distance from its redshift
    alone. Against a source that also has an independent real-space distance,
    the difference between the two is its line-of-sight peculiar velocity.
* - Distance source (priority, in real-space mode)
  - 1. `entry.distanceMpc` override (set by the cosmology selector)
    2. The catalogue's `distance` / `dist` / `dMpc` field
    3. A redshift field (`z`, `REDSHIFT`, `ZSPEC`, `ZMEAN`, `ZPHOT`)
       integrated through a chosen cosmology
    4. Hard-coded 300 Mpc fallback
```

The **Cosmology** card in the left column says which of these produced the
positions on screen — the model, or the catalogue's own distance column, or
cz/H₀ with the model unused.

### Cosmology models

For redshift-derived distances pick one of:

- **Planck18** — integrated in the client (`H₀ = 67.66`, `Ωm = 0.3111`,
  flat `ΩΛ = 1 − Ωm`), so no network call. It agrees with astropy's Planck18 to
  better than 0.1 % out to z = 20, which `tests/test_catalogue_cosmology.cpp`
  pins — the two have to match, or switching the selector away from the default
  and back would move every source for a numerical reason.
- **Planck15** / **Planck13** / **WMAP9** — computed by the backend
  (`POST /v1/cosmology/distance/batch`, async). The viewer re-projects
  the cloud automatically when results arrive.
- **Custom** — the ΛCDM set in *Tools ▸ Cosmology Calculator* (**Use in 3-D
  Catalogues**), computed by the backend. See
  [Cosmology tools](cosmology.md#the-cosmology-of-a-3-d-catalogue).

```{tip}
For catalogues with explicit redshifts, switching between Planck18 and
WMAP9 can shift comoving distances by several %; useful as a sanity
check before drawing conclusions about clustering or LSS.
```

### Glyph & size modes

```{list-table}
:header-rows: 1
:widths: 22 38 40

* - Setting
  - Options
  - When to use
* - Geometry
  - Ellipsoid, Sphere, Point, Cross, Gaussian splats; and the separate
    *Ray-traced light* check below them
  - Ellipsoid for axis-aware sources; Point for very dense fields where
    glyph overhead matters; Gaussian splats for a simulation's particles —
    emissive, so dense regions add up to bright and sparse filaments stay
    faint but visible (the *Scale* slider sets their size). *Ray-traced
    light* draws a simulation for figures on this machine's GPU, prepared by
    the backend ([Ray-traced particles](raytraced-particles)); the shape is
    kept for when it is cleared.
* - Size
  - Fixed, Major axis, LLS, Flux
  - "Major axis" scales each glyph by the catalogue's morphology size;
    "Flux" by intensity (good for highlighting brightest objects).
* - Color
  - Morphology class (deterministic palette) or any scalar field
  - Click the *Color mode* card and pick a scalar to drive the colour
    map; the colour bar updates live.
* - Colormap / Edit
  - The default rainbow ramp or a named map (Inferno, Viridis, …). *Edit*
    opens the LUT editor of the cube viewer over the histogram of the colour
    column: Min and Max, the scale (Linear, Log, Sqrt, Square, Power) and the
    map
  - Log for quantities spanning decades (a particle mass, a density).
```

A colour bar at the right of the view shows the colours over the column's
range (or the editor's Min..Max) on the scale in use, titled with the column.
It is drawn straight on the view, with no box behind it, as in the image and
cube viewers.

Clicking a source selects it: a ring marks it and the *Source Info* page shows
its row. The ring wraps the glyph of an ellipsoid, sphere or cross, and is a
small ring of a few pixels around a point, a splat or an arrow; it keeps that
size on screen as you zoom. To deselect, press **Esc**, click on empty space,
click the selected source again, or choose *View ▸ Clear Selection*.

### Drawn on the backend

A catalogue larger than what the window loads (a sky catalogue, a cartesian
one or a VBT table), on a **remote** backend that renders server-side (an EGL
node of a cluster), is drawn on the backend: every row, streamed to the window,
while the loaded rows stay here for hovering, picking, the table and
statistics. A sky catalogue's positions are computed there as the window
computes them, from the same RA/Dec, distance or redshift columns and with the
frame, cosmology and distance convention chosen here. *View ▸ Rendering* chooses **Auto** (on the
backend when the table has more rows than were loaded), **Local** or
**Remote**; the status bar says which (*Remote · all N rows*,
*Local · n of N rows*). Shape, colour, colormap, scale, filters and the box
drive both views the same way: Point and Gaussian splats are drawn as such on
the backend, the other shapes as points; colour by morphology or by speed is
drawn in a single colour there; numeric filters apply to the backend's view,
*contains* and the like to the loaded rows. With the **local** backend
everything is drawn here.

Positions far from 0 keep their separations on the backend: a table in
metres or at a large offset (1e9, 1e9 + 1, 1e9 + 2) is drawn as three points,
with the box and the camera in the table's own units, and the headset's
frames land on the points the headset draws.

### Filters & paging

For million-row catalogues the backend exposes a paginated query API:

- Filters are AND-combined, with operators `<`, `≤`, `>`, `≥`, `=`,
  `≠`, `contains`, `startswith`, `endswith`.
- The Inspector's *Analysis* tab lets you stack multiple filters and apply
  them in one click — the query is sent to the backend, the result count is
  shown, and the cloud rebuilds with only matching entries.
- Datasets > 50 000 rows are loaded in pages of 50 000; the *Load more
  (N remaining)* button appears under the filters.

### Interaction

- Hover a glyph → yellow wireframe sphere; the Inspector's *Properties* tab
  shows all catalogue fields for that source.
- Click → red wireframe outline + the source is highlighted in the
  *Catalogue* table dock.
- Click a table row → camera flies to centre on the source.

## Catalogue overlay on cubes / images

In any **cube viewer** or **image viewer** you can overlay a catalogue on
the 2-D slice / moment / image. The whole feature is driven by four menu
entries under *Tools → Catalogue* — two checkable state toggles and two
one-shot actions:

```{list-table}
:header-rows: 1
:widths: 28 16 56

* - Tool
  - Type
  - What it does
* - **Load Catalogue Overlay**
  - One-shot
  - Open a file picker for a delimited text table (`.csv`, `.tsv`,
    `.txt`) with `RA` and `DEC` columns in degrees, or a DS9 region file
    (`.reg`); the image viewer also takes CASA CRTF (`.crtf`). See
    [Overlay file formats](#overlay-file-formats). The glyphs are drawn
    immediately on the slice. Each call replaces the currently-loaded
    overlay.
* - **Show Catalogue Overlay**
  - Toggle
  - Show / hide the glyph layer without unloading the catalogue. Useful
    when you want to compare slice features against the catalogue back
    and forth — much cheaper than re-loading.
* - **Show Catalogue Labels**
  - Toggle
  - Show / hide the per-source labels (object name from the catalogue's
    name column). Labels are crowded on dense fields, so they default
    to *off*. The viewer caps the number of visible labels (≈ 200) to
    keep rendering fluid; when the field has many more sources, only
    the brightest / most central are labelled.
* - **Clear Catalogue Overlay**
  - One-shot
  - Drop the catalogue entirely — glyphs, labels, and the table dock.
    Frees backend memory and resets the *Show…* toggles. Run a *Load*
    again to bring it back.
```

The overlay respects the viewer's WCS frame (J2000 / Galactic / Ecliptic);
each source is projected through `wcscon()` so you see the same point
regardless of the active frame.

### Overlay file formats

The file is read by what it holds, not only by its name, with the same rule
the backend applies to delimited text (see
[3-D catalogue viewer](#3-d-catalogue-viewer)):

- A **`.reg`** file is read as **DS9 regions**: `fk5` / `icrs` points and
  circles, and `image` ellipses.
- A file whose first lines (up to ten, `#` comment lines and blank lines
  aside) split into the same number of fields, two or more, with a **comma**,
  a **tab** or a **semicolon** (tried in that order) is read as a **table**
  with that separator, whatever its extension. It needs `RA` and `DEC` (or
  `DEC_DEG`) columns in degrees; a `LABEL`, `ID` or `NAME` column names the
  sources, and every other column becomes a field of the source. Fields are
  read as the backend reads them: a field that starts with `"` is quoted and
  may contain any of the separators, a line break, or `""` for a quote; a
  `"` anywhere else is an ordinary character (`3"`). Lines may end in
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
    its fluxes; a column with any text in it, such as a note reading `1,2`,
    stays as written.
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
  why, rather than being tried as regions. A file with any other name falls
  back to the DS9 reader, and its message says it is not a table either.

### Querying the VLKB directly

In the image viewer you do not need a file at all. *Tools → Catalogue →
**Overlay VLKB Compact Sources***, ***Overlay VLKB Filaments*** and ***Overlay
VLKB Bubbles*** (also in the Inspector's tool list) query the VLKB over the
footprint of the image, or over a rectangle you drag on it, and overlay the
result (bubbles: see [VLKB Catalogue Query](#vlkb-catalogue-query)):

- **Compact sources** → `compactsources.sed_view_final`, the Hi-GAL band-merged
  catalogue. One query brings back every band; per-band ellipses are centred on
  each band's own peak, and the band toggles let you show one wavelength at a
  time.
- **Filaments** → `filaments.filaments`, drawn as contours.

Both need a celestial WCS: without one there is no sky position to query over,
and the actions are disabled with that explanation in their tooltip.

The query is `SELECT *`: the whole row comes back and the client reads it by
name from the response header. That is deliberate — the fluxes per band are the
reason the catalogue exists, naming the columns explicitly would need a
`TAP_SCHEMA` round trip, and one of them (`distance`) is an ADQL reserved word
that makes the service answer 400 unless quoted.

### The catalogue table

The overlay drives a table dock at the bottom of the window.

- **Names** are the catalogue's own designations, not `Source 1`, `Source 2`.
- **RA / Dec** are shown for every entry that has a sky position — including
  shapes drawn in pixels, which still know where they are on the sky.
- **Every field the catalogue carries** is a column: the fluxes, errors,
  backgrounds and flags next to the position, not just the four numbers needed
  to draw a glyph. Columns appear in the file's own order after the fixed ones.
- **Size / angle** appear when the catalogue gives a shape.

Interaction:

```{list-table}
:header-rows: 1
:widths: 34 66

* - Action
  - Result
* - Click a row
  - Highlights that source in the image (the view does not move, and the table
    does not scroll out from under the pointer).
* - Double-click a row, or double-click a source in the image
  - Zooms to it.
* - Hover a glyph
  - Highlights the matching row.
* - Right-click a row
  - *Fit SED of this source…* and *Zoom to this source*.
* - *Tools → Catalogue → Export Catalogue…* (image viewer)
  - Writes what is on screen to CSV — names, positions in the overlay's own sky
    frame (named in the header rather than implied), pixel coordinates, shapes
    and every extra field.
```

For a **band-merged** catalogue the table is a tree: the band-merged source is
the parent row, and its per-band detections are children under the usual
disclosure arrow. A flat catalogue — a plain CSV, a ds9 region file — has no
children and stays exactly the flat table it always was, with no expander
column.

```{note}
The table reader is quote-aware and reads **records**, not lines: a quoted
separator does not shift the columns, and a value — or a header — that spans
several lines stays one field. A truncated last row is skipped and
counted, and the count is reported rather than folded into the "loaded"
message, so a file that lost rows does not look like one that did not.
```

Right-clicking a source and choosing *Fit SED of this source…* builds its SED
from the whole band-merged family (or, for a flat catalogue, from the row's own
flux columns) and opens the fitting window — see
[SED fitting](sed-fitting).

```{tip}
**Workflow sequence**: *Load* once to bring sources in → toggle *Show
Catalogue Overlay* off and on as you scrub channels to compare emission
against catalogued positions → enable *Show Catalogue Labels* only when
you actually need names (cluttering otherwise) → *Clear* when you switch
to a different catalogue.
```

### Why overlay a catalogue on a cube?

Three common use cases:

1. **Source identification** — overlaying a YSO catalogue on a moment-0
   map tells you which IR-bright pre-main-sequence sources sit inside
   the molecular emission you're integrating, helping disentangle
   star formation context from diffuse cloud structure.
2. **Cross-survey comparison** — overlay a centimetre-continuum source
   list on a HI cube to see whether neutral-hydrogen self-absorption
   features coincide with background continuum positions (classic
   technique for HISA studies).
3. **Pointing / coordinate sanity check** — when you receive a new cube
   from a different observatory, overlaying a well-known catalogue
   (e.g. 2MASS PSCs, Spitzer point sources) is a quick way to spot
   coordinate-system or astrometric offsets before any science analysis.

## HiPS viewer

Hierarchical Progressive Surveys (HiPS) are pre-tiled multi-resolution
all-sky images served as static files. VisIVO ships with a built-in HiPS
browser:

Open from **Data → HiPS Viewer…** or the Command Palette. It opens as a tab
in the main window, not a separate viewer window.

Workflow:

1. Press **Browse…** and pick a survey. The list is every image HiPS
   registered with the CDS MOCServer — around 1400 of them — grouped by the
   category its publisher gave it and filtered as you type: every word has to
   appear somewhere in the row, so `herschel 250` narrows to the six Herschel
   250 µm surveys rather than widening. The list is cached for a week;
   **Refresh** fetches it again.

   You can still paste a HiPS root URL into *Survey* and press **Open** — any
   server works, not only CDS. Either way the backend reads the survey's
   `properties` file (orders, tile format, native frame).

   ```{note}
   Only image surveys of the sky are offered. The registry also lists HiPS
   catalogues and cubes, which this viewer cannot draw as tiles, and maps of
   planetary surfaces — Mars, the Moon, Io — which have no sky position at all.
   ```

   A HiPS is tiled in its own frame, and rather more than half the registry is
   **galactic** rather than equatorial. The backend converts: the position you
   ask for goes into the survey's frame to choose the tiles, and every tile
   comes back in ICRS, so a galactic survey lines up with an equatorial one and
   with the catalogue overlays.
2. Pan with a drag and zoom with the wheel, or use **−** / **+** and **⌂**
   for a reset. The viewer requests only the tiles the current viewport
   needs (`/v1/hips/{id}/query_tiles`); the order in use is shown next to
   the zoom buttons. While the tiles for a new zoom level are in flight the
   view stays filled: each missing tile is drawn from the coarsest level
   already in the cache, so the picture sharpens rather than flashing.

   ```{note}
   Orientation follows the sky convention — RA increases to the **left** —
   so the field matches hips2fits, Aladin and the image viewer's overlays.
   ```

3. **Projection** — the selector next to the order picks how the sphere is
   laid on the screen:

   ```{list-table}
   :header-rows: 1
   :widths: 12 88

   * - Code
     - What it is for
   * - `TAN`
     - Gnomonic, the default. A tangent plane: great straight out to a few
       degrees, and it diverges as the field approaches a hemisphere, so it
       stops at 120°.
   * - `SIN`
     - Orthographic — the sphere as seen from far away. Exactly one
       hemisphere, no more.
   * - `ARC`
     - Zenithal equidistant. Distances from the centre are true, so it is the
       one to use when you care how far something is from where you are
       pointing.
   * - `AIT`
     - Hammer-Aitoff. The whole sky in one ellipse; the usual choice for an
       all-sky figure.
   * - `MOL`
     - Mollweide. Also the whole sky, and equal-area, so relative sky
       coverage is honest. Being equal-area it cannot also be conformal: at
       the centre it stretches by 11% vertically and squeezes by 10%
       horizontally, the two cancelling exactly. The field of view is that
       geometric mean, so switching to it preserves the area on screen rather
       than either width.
   * - `MER`
     - Mercator, cut off at ±85° because the poles are infinitely far away.
   ```

   Zooming out past 90° puts you in a whole-sky view, which is drawn from the
   survey's AllSky mosaic — a single request that covers the entire sphere.
   That same mosaic is what makes a deep zoom fill in immediately rather than
   from nothing.
4. **Target resolution** — type a name ("M31", "NGC 1068") in *Target* and
   press **Go**; the backend resolves it through Sesame
   (`/v1/resolve/target`) and centres the view on it.
5. **Grid** overlays an RA/Dec graticule whose spacing adapts to the field. It
   follows whichever projection is selected, so on an all-sky map the meridians
   curve in to the poles rather than being switched off.
6. **Cat** overlays a catalogue, from either of two sources:

   - **a VO cone search over the field on screen** — pick a service (VizieR,
     NED, or any cone-search URL you paste), and the search is centred on the
     view with a radius of half the field. The result is saved as a CSV in the
     Workspace and overlaid, using the position columns the backend identifies.
     A wide field starts at 5° and says so, rather than quietly searching less
     than you can see; what a given service will actually answer varies.
   - **a delimited text table already on the backend filesystem** (comma,
     tab or semicolon separated, read as the 3-D catalogue viewer reads it),
     chosen with the backend's file browser (it starts on *Delimited text
     tables*, `.csv`, `.tsv` and `.txt`).

   Either way the backend returns only the rows inside the visible field
   (`/v1/hips/catalogue_overlay`) and the count is shown in the status bar.
   "Visible" is measured, not guessed: the viewer sends the angular radius that
   covers its own viewport — corners included, which a field of view quoted
   across the width does not tell you — and the backend selects by separation
   from the centre rather than by a range of right ascension, which says
   nothing near a pole. Resizing the window refetches, because a taller window
   sees further.
7. **Overlay** opens a second survey on top of the first. The slider sets
   its opacity, and **⧸⧸** switches from alpha blending to a side-by-side
   split so you can compare the two halves of the field.

```{note}
HiPS tile fetching is HTTP only and goes through the backend so that
firewalled / VPN'd HiPS roots still work for the desktop client; you
don't need direct internet access from the GUI.
```

```{caution}
Still missing compared with Aladin: no MOC display, no per-survey colour map
or stretch controls, and no FITS-HiPS pixel readout. When you need one of
those, reach for Aladin: VisIVO can send it the current image over
[SAMP](#samp).
```

## SAMP

Send and receive data with TOPCAT, Aladin, DS9 and any other VO tool on your
machine, through a SAMP hub. TOPCAT and Aladin start one when they run; any
hub using the Standard Profile works. The **SAMP** menu is in the Data Hub
window, the main window of the application, and in every image, cube and 3-D
catalogue window, where it acts on that window's data; the Data Hub's actions
are also in the Command Palette (*SAMP: Connect to Hub*, …). The whole
workflow, from connecting to troubleshooting, is in
[Working with TOPCAT, Aladin and DS9](interoperability.md).

**Connecting.** *SAMP → Connect To Hub* registers VisIVO with the hub. VisIVO
then appears in the other tools as a target (TOPCAT's *Send table to…* and
*Send Sky Coordinates*, for example). The first line of the **SAMP** menu says
where things stand: *Not connected*, *Connected as VisIVO [c3], receiving*, or
*Connected as VisIVO [c3], cannot receive* (its tooltip gives the reason). Every
connection, failure and received message is also listed in **Diagnostics**
(*View → Diagnostics*).

If no hub answers when you connect (TOPCAT not started yet, or a hub that was
killed and left its `~/.samp` behind), VisIVO says so once, shows *Not
connected: waiting for a hub*, and keeps trying every few seconds: start TOPCAT
and VisIVO connects by itself. The same happens when the hub disappears while
connected (TOPCAT quit or crashed): VisIVO notices within a few seconds and
reconnects as soon as a hub is running again. *Disconnect From Hub* stops the
waiting. VisIVO does not reconnect after *Disconnect From Hub*, nor when the
hub itself disconnects VisIVO.

**Sending.** From a viewer window, its **SAMP** menu sends that window's image,
cube or catalogue, a selection of a catalogue's rows, or a clicked sky
position, to every tool that takes it or to one tool by name (see
[Send from a viewer](interoperability.md#send-from-a-viewer)). The Data Hub's
SAMP menu sends the last dataset opened:

- *SAMP → Send Current FITS Dataset* broadcasts the last opened image or cube
  (`image.load.fits`). Only image viewers take it: run **Aladin** or **DS9**.
  TOPCAT does not load images, so with TOPCAT alone nothing visible happens.
- *SAMP → Send Current Catalogue* broadcasts the last opened catalogue as a
  VOTable (`table.load.votable`); TOPCAT loads it. A table that is not a
  VOTable already is converted to a binary VOTable (BINARY2), which TOPCAT
  reads several times faster than the text form. Above a million rows VisIVO
  asks first: TOPCAT holds the whole table in memory.

There, "current" is the last dataset opened, not the window in front.

VisIVO first asks the hub whether any running tool takes that kind of data;
if none does, it says so at once (*No running tool takes …*) instead of
preparing the file. Otherwise it asks every tool that takes the data and
reports what each one answered, in the status bar and in Diagnostics, under
the name the tool was given: *SAMP: topcat loaded NKATZ_2.3M.vot* (a table
converted to VOTable is sent with a `.vot` name), or the tool's own error.
Preparing a large file takes a while (about 40 s for 2.3 million rows); VisIVO
and the backend stay usable meanwhile.

**Receiving.** VisIVO accepts:

| From the other tool | What VisIVO does |
|---|---|
| a table (`table.load.votable`, `table.load.fits`) | opens it in the 3-D catalogue viewer |
| an image (`image.load.fits`) | opens it in the image or cube viewer |
| a sky position (`coord.pointAt.sky`) | centres the HiPS viewer on it, opening one (with the last survey you opened, else DSS colour) if needed |
| a row highlight (`table.highlight.row`) | selects that source, as a click would |
| a row selection (`table.select.rowList`) | marks those sources |

VisIVO fetches the file the other tool offers itself and hands it to the
backend, so this works with a remote backend too: the address a hub gives
(`http://127.0.0.1:…`) only means something on your machine. When the other
tool asks for an answer (TOPCAT does), it is told whether the data opened, and
why not when it did not.

A received table or image is shown under the name the sender gave it
(TOPCAT's table name, for example), in the title, the path bar, Session Data
and the Inspector; the copy the backend keeps is a temporary file, whose path is
in the tooltips of the path bar and of the Inspector's *Path* row. Because the
backend deletes that copy after a while, data received this way is not added to
*Recent Datasets*: send it again from the other tool. (A local file the sender
names by path, read where it is, is added.)

**Linking rows with TOPCAT.** A table that went from one application to the
other, in either direction, stays linked:

- clicking a row in TOPCAT selects the same source in VisIVO's 3-D catalogue
  view and its table, and centres the view on it;
- clicking a source in VisIVO's view, or a row in its table, highlights that
  row in TOPCAT: in the table VisIVO received and in every copy VisIVO sent
  back (*Send Catalogue To*), since they hold the same rows, and in a subset
  VisIVO sent (*Send N Selected Rows To*) when it has that row;
- a subset selected in TOPCAT (*Transmit Subset*) is marked in VisIVO with
  orange circles, and the status line says how many rows were marked.

The status line, the row numbers down the side of VisIVO's table and the *Row*
line of Source Info count rows of the original table from 1, as TOPCAT shows
them (SAMP itself counts from 0); a source without a name is called by its row
(*Row 149998*). Rows are matched by their place in the original table, so a filter, a sort or
the even sample VisIVO loads for a large table ("100000 of 3931546 rows") does
not break the link. A row that is not loaded in VisIVO cannot be shown: the
status line says so (filtered out, or not loaded yet: *Load more* or *Load all*
may bring it). The link is kept for a catalogue opened from one file; a
catalogue combined from several files is not linked. The selection and the
orange marks are drawn by the local view: while a large table is drawn on the
backend, choose *View → Rendering → Local* to see them; while it is ray-traced,
choose another *Shape* than *Ray-traced*. A click of your own replaces the
status line about the last highlight.

Rows selected in VisIVO's table can be sent as a new table or selected in
the tools that hold the catalogue
([Send a selection of rows](interoperability.md#send-a-selection-of-rows)).
Spectra are not exchanged yet.

**Security.** All `/v1/samp/*` routes need the backend's operator token, except
`GET /v1/samp/files/{token}`: the other tools fetch the files VisIVO sends from
there, with no token, so that route is open. Each file is reachable only by its
random token, for one hour (`VISIVO_SAMP_FILE_TTL`). VisIVO's own SAMP endpoint
listens on the loopback interface only, and refuses any call that does not
carry the key the hub gave it.

## See also

- [VLKB archive](vlkb-archive) — the other route through the same archive:
  finding and cutting out the *images and cubes* over a region, rather than
  overlaying a source catalogue on one you already have.
- [SED fitting](sed-fitting) — turning a band-merged source's fluxes into a
  mass, a dust temperature and a luminosity.
- Developer reference: [`/v1/catalogue/*`](../backend-api/datasets.md),
  [`/v1/hips/*`](../backend-api/sky-services.md), [`/v1/samp/*`](../backend-api/samp.md).
- [Catalogue 3-D viewer technical reference](../catalogue3d-viewer) — for
  developers customising the geometry / size cards or adding new
  cosmology models.


## Getting data out of the archives

Three entries under **Data** fetch rather than open. Each writes into the
Workspace, so what comes back is a file the rest of the application can use.

### HiPS Cutout (hips2fits)

*Data → HiPS Cutout (hips2fits)…* reprojects any HiPS survey onto a WCS of your
choosing through the CDS service, and opens the FITS it returns. Pick a survey
(the list is a starting point — any id from the CDS registry works), a position,
a field of view and a size.

Every parameter is checked before the request leaves: an empty survey, a field of
view of zero or a cutout over 30 Mpx is refused with the reason rather than sent
for the service to reject. Choosing `png`/`jpg` enables the percentile cuts and
the stretch — they turn numbers into pixels, and a FITS carries the numbers
themselves, so for FITS they are not sent at all.

### Cone Search

*Data → Cone Search…* asks a VO Simple Cone Search service what is around a
position. Choose one of the listed services or paste the URL of another; `RA`,
`DEC`, `SR` and `VERB` are merged into whatever query the URL already carries.

The rows appear in the window, **and the whole result is saved as a CSV** —
**Open as Catalogue** opens it in the [3-D catalogue viewer](catalogues-hips),
where it can be filtered, coloured and overlaid like any other catalogue.

### VLKB Catalogue Query

*Tools → Catalogue → VLKB Catalogue Query…* in the image viewer runs the VLKB catalogue
queries over a Galactic box — prefilled with the footprint of the image you are
looking at:

| Catalogue | What it is |
|-----------|------------|
| Compact sources, band-merged | the SED view: one row per source, fluxes in every band |
| Compact sources, one band | a single Hi-GAL band table |
| Filaments | filament spines with their branches |
| Bubbles | bubble rims |
| 3-D selection | sources with distances, carrying Galactocentric `x`/`y`/`z` |

…or write the ADQL yourself. The result is saved as a CSV and can be opened as a
catalogue; the query actually sent is shown, so a preset can be read, copied and
edited.

For the two contour catalogues there is also a direct overlay: *Tools → Overlay
VLKB Filaments* and *Overlay VLKB Bubbles* draw them on the image over a
rectangle you drag (or the whole footprint, if you click without dragging).
