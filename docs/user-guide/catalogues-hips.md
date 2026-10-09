# Catalogues, HiPS, and SAMP

VisIVO is not just a viewer for one cube at a time — it's also a workbench
for putting cubes / images in context: overlay source catalogues, browse
all-sky HiPS surveys at any zoom level, and exchange data with TOPCAT /
Aladin / DS9 via SAMP.

## 3-D catalogue viewer

A dedicated 3-D scatter viewer (`vtkWindowCatalogue3D`) renders large
catalogues — millions of entries — as glyphs in Cartesian (RA / Dec /
distance) space.

Opens from **File → Open 3-D Catalogue…** (CSV or VOTable on the backend
filesystem) or from the Command Palette (⌘K → "open 3d catalogue").

### Coordinate frame & distance

The viewer needs both a sky frame and a distance for each entry:

```{list-table}
:header-rows: 1
:widths: 28 72

* - Frame combo
  - FK5 / J2000 (default) or Galactic (l, b). Conversion is done with
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
  - Ellipsoid, Sphere, Point, Cross
  - Ellipsoid for axis-aware sources; Point for very dense fields where
    glyph overhead matters.
* - Size
  - Fixed, Major axis, LLS, Flux
  - "Major axis" scales each glyph by the catalogue's morphology size;
    "Flux" by intensity (good for highlighting brightest objects).
* - Color
  - Morphology class (deterministic palette) or any scalar field
  - Click the *Color mode* card and pick a scalar to drive the colour
    map; the colour bar updates live.
```

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
entries under *Tools → Catalogue* (in the cube viewer) — three checkable
state toggles plus one one-shot loader:

```{list-table}
:header-rows: 1
:widths: 28 16 56

* - Tool
  - Type
  - What it does
* - **Load Catalogue Overlay**
  - One-shot
  - Open a file picker (or paste a backend path) for a CSV / VOTable.
    The backend parses it (`/v1/catalogue/open`), returns the source
    list, and the client renders the glyphs immediately on the slice.
    Each call replaces the currently-loaded overlay.
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

### Querying the VLKB directly

In the image viewer you do not need a file at all. Two actions — *Tools →
**Overlay VLKB Compact Sources*** and *Tools → **Overlay VLKB Filaments*** (also
in the Inspector's tool list) — query the VLKB over the footprint of the image,
or over a rectangle you drag on it, and overlay the result:

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
* - *Tools → Export Catalogue…*
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
The CSV reader is quote-aware and reads **records**, not lines: a quoted
comma does not shift the columns, and a value — or a header — that spans
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
3. **Target resolution** — type a name ("M31", "NGC 1068") in *Target* and
   press **Go**; the backend resolves it through Sesame
   (`/v1/resolve/target`) and centres the view on it.
4. **Grid** overlays an RA/Dec graticule whose spacing adapts to the field. It
   follows whichever projection is selected, so on an all-sky map the meridians
   curve in to the poles rather than being switched off.
5. **Cat** overlays a catalogue, from either of two sources:

   - **a VO cone search over the field on screen** — pick a service (VizieR,
     NED, or any cone-search URL you paste), and the search is centred on the
     view with a radius of half the field. The result is saved as a CSV in the
     Workspace and overlaid, using the position columns the backend identifies.
     A wide field starts at 5° and says so, rather than quietly searching less
     than you can see; what a given service will actually answer varies.
   - **a CSV/TSV already on the backend filesystem**, chosen with the remote
     file browser.

   Either way the backend returns only the rows inside the visible field
   (`/v1/hips/catalogue_overlay`) and the count is shown in the status bar.
   "Visible" is measured, not guessed: the viewer sends the angular radius that
   covers its own viewport — corners included, which a field of view quoted
   across the width does not tell you — and the backend selects by separation
   from the centre rather than by a range of right ascension, which says
   nothing near a pole. Resizing the window refetches, because a taller window
   sees further.
6. **Overlay** opens a second survey on top of the first. The slider sets
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
those, reach for Aladin over SAMP — VisIVO can hand it the current position
(see [SAMP](#samp)).
```

## SAMP

Send and receive data with TOPCAT, Aladin, DS9, and any other VO-aware
tool on your machine. The desktop client talks to the bundled
**SAMP bridge** in the backend (router `/v1/samp/*`, exposed without
auth so the local SAMP hub can reach it).

Capabilities:

- **Send a FITS** — share the currently open dataset to all subscribed
  applications (`/v1/samp/send-fits`).
- **Send a catalogue** — broadcast a VOTable, e.g. a region selection from
  the catalogue overlay (`/v1/samp/send-catalogue`).
- **Import URL / Upload file** — bring an external resource into your
  session, e.g. a TOPCAT-prepared catalogue
  (`/v1/samp/import-url`, `/upload-file`).
- **Receive** — incoming messages from peers are queued in
  `/v1/samp/inbox`; the client surfaces them as notifications and offers
  to open the payload.

You can monitor the hub state from *View → SAMP Status* in the main
window or via the Command Palette.

## See also

- [VLKB archive](vlkb-archive) — the other route through the same archive:
  finding and cutting out the *images and cubes* over a region, rather than
  overlaying a source catalogue on one you already have.
- [SED fitting](sed-fitting) — turning a band-merged source's fluxes into a
  mass, a dust temperature and a luminosity.
- Developer reference: [`/v1/catalogue/*`](../backend-api#catalogue),
  [`/v1/hips/*`](../backend-api#hips), [`/v1/samp/*`](../backend-api#samp).
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

*Tools → VLKB Catalogue Query…* in the image viewer runs the VLKB catalogue
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
