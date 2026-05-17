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
* - Distance source (priority)
  - 1. `entry.distanceMpc` override (set by the cosmology selector)
    2. The catalogue's `distance` / `dist` / `dMpc` field
    3. A redshift field (`z`, `REDSHIFT`, `ZSPEC`, `ZMEAN`, `ZPHOT`)
       integrated through a chosen cosmology
    4. Hard-coded 300 Mpc fallback
```

### Cosmology models

For redshift-derived distances pick one of:

- **Planck18** — local Riemann integration (`H₀ = 67.74`, `Ωm = 0.3089`,
  `ΩΛ = 0.6911`). No network call.
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
- The *Filter* sidebar lets you stack multiple filters and apply them in
  one click — the query is sent to the backend, the result count is shown,
  and the cloud rebuilds with only matching entries.
- Datasets > 50 000 rows are loaded in pages of 50 000; the *Load more
  (N remaining)* button appears in the filter card.

### Interaction

- Hover a glyph → yellow wireframe sphere; the info panel shows all
  catalogue fields for that source.
- Click → red wireframe outline + the source is highlighted in the
  *Catalogue* table dock.
- Click a table row → camera flies to centre on the source.

## Catalogue overlay on cubes / images

In any **cube viewer** or **image viewer** you can overlay a catalogue on
the 2-D slice / moment / image:

1. **Tools → Load Catalogue Overlay** — pick a CSV / VOTable. The backend
   parses it and returns the source list.
2. Sources are drawn as glyphs (with optional labels) on the viewer.
3. The *Catalogue dock* shows a sortable / filterable table:
   - Click a row → centre the view on the source.
   - Hover a glyph → highlight the row.
   - Right-click selection → send via SAMP (see below).

The overlay respects the viewer's WCS frame (J2000 / Galactic / Ecliptic);
each source is projected through `wcscon()` so you see the same point
regardless of the active frame.

## HiPS viewer

Hierarchical Progressive Surveys (HiPS) are pre-tiled multi-resolution
all-sky images served as static files. VisIVO ships with a built-in HiPS
browser:

Open from **File → HiPS Viewer…** or the Command Palette.

Workflow:

1. Paste a HiPS root URL (e.g. `http://alasky.u-strasbg.fr/DSS/DSS2Merged`).
   The viewer asks the backend to discover the survey properties (orders,
   tile format, native frame, FOV).
2. Pan & zoom with the mouse. The viewer requests only the tiles needed
   for the current viewport (`/v1/hips/{id}/query_tiles`); levels of
   detail load on demand.
3. **Catalogue overlay** — the backend can return a Simbad / VizieR /
   custom catalogue restricted to the visible field
   (`/v1/hips/catalogue_overlay`).
4. **Target resolution** — type a name ("M87", "NGC 1068") in the search
   bar; the backend resolves it via Sesame (`/v1/resolve/target`) and
   centres the view.

```{note}
HiPS tile fetching is HTTP only and goes through the backend so that
firewalled / VPN'd HiPS roots still work for the desktop client; you
don't need direct internet access from the GUI.
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

- Developer reference: [`/v1/catalogue/*`](../backend-api#catalogue),
  [`/v1/hips/*`](../backend-api#hips), [`/v1/samp/*`](../backend-api#samp).
- [Catalogue 3-D viewer technical reference](../catalogue3d-viewer) — for
  developers customising the geometry / size cards or adding new
  cosmology models.
