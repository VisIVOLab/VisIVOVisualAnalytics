# Catalogue 3D Viewer

Reference for `vtkWindowCatalogue3D` (`src/gui/vtkWindowCatalogue3D.h/cpp`).

> Ready-made test data for every feature below: see
> [Demo datasets](demo-datasets) (`catalogue_sky.csv`, `catalogue_cartesian.csv`,
> `catalogue.speck`, the `.tbl` snapshot).

---

## Point-cloud rendering (performance)

The source cloud is rendered with **`vtkGlyph3DMapper`** (GPU-instanced glyphs):
the mapper takes the point cloud (`sourcesPolyData`) and a glyph source
(`sphereSource` / `crossSource`) and replicates the glyph per point on the GPU —
no per-point geometry is materialised on the CPU, so large catalogues stay
interactive. Per-axis size comes from the `ScaleVector` 3-component point vectors
(`SetScaleModeToScaleByVectorComponents` + `OrientOff` → ellipsoids); colour from
the active point scalars through the morphology / value LUT. *Point* geometry
mode uses a plain points actor instead.

**Picking is independent of the render pipeline** — `pickNearestSource()` does a
CPU nearest-source search projecting each catalogue entry to display coordinates,
and hover/selection use separate highlight-sphere actors. So the glyph mapper can
be changed freely without affecting picking/selection — and picking stays exact
regardless of the render budget (which only thins the GPU-fed cloud, not the
entry list). Rather than a `vtkCoordinate` transform per entry (millions of
virtual calls per click), it fetches the camera's composite world→clip matrix
**once** and projects each point inline (matrix·point → perspective divide →
NDC→viewport pixels), keeping the same nearest-within-14px screen test. This is
~10–50× faster (verified to match `vtkCoordinate` to ≤1px — its int rounding —
against VTK 9.6.2); points with clip-w ≤ 0 (at/behind the camera) are skipped.

### Render budget (GPU point LOD)

Rotating millions of *real* glyphs (sphere/arrow geometry) is GPU-heavy, so the
mappers are fed through a **`vtkMaskPoints` decimator** when the catalogue exceeds
the **Render budget** (Visualization ▸ encoding card; 500k / 1M / 2M / 5M /
Unlimited, default 1M). `applyRenderBudget()` sets the decimator to a
**deterministic stride** (`OnRatio = ⌈N/budget⌉`, `RandomMode` **off**) and
routes the glyph + points mappers to its output; under budget (or *Unlimited*)
the mappers read `sourcesPolyData` directly with no extra pass.

Stride — not `vtkMaskPoints` RandomMode — is used deliberately: every random
distribution (RANDOM_SAMPLING, RANDOMIZED_ID_STRIDES, SPATIALLY_STRATIFIED)
**reseeds on each `Update()`**, so it re-picks a *different* subset every time a
colour/size/position edit bumps the input's MTime — the cloud would visibly
shimmer (verified against VTK 9.6.2). Stride is stable across re-executions and,
for ID-ordered catalogues, samples all spatial regions proportionally so density
still reads correctly. Colour/scale edits (`morphScalars->Modified()` etc.)
propagate through the decimator to the subsample.

**Only the GPU-fed polydata is thinned** — `pickNearestSource()`, region stats
and `ResetCamera` framing all use the complete entry list. When decimation is
active the Filters status shows *"Render budget: drawing ≤N of M points (LOD) —
picking and stats use all"*. (Out-of-core streaming — octree / k-d tiling for
catalogues too large for RAM — remains future work.)

### Chunked scene build (responsiveness for millions of points)

`buildScene()` runs on the GUI thread (it touches VTK objects, which aren't
thread-safe). For catalogues above `kAsyncBuildThreshold` (250 000 entries) it
switches to a **chunked** build so the window never shows as *"not responding"*:

- The glyph + points actors are hidden and a wait cursor is set, so no stray
  repaint flashes a half-built cloud.
- Every O(N) loop (positions, vertices, velocity vectors, colour and size
  normalisation) calls `pumpSceneBuild(i)`, which every `kBuildPumpChunk`
  (200 000) points updates a *"Building scene… X / Y points"* status and pumps
  the event loop with `QApplication::processEvents(QEventLoop::ExcludeUserInputEvents)`.
  `ExcludeUserInputEvents` lets paints/status through but **not** clicks, combo
  changes, picking or window-close — so no slot can re-enter `buildScene`, mutate
  the half-built arrays, or delete the window mid-build.
- `m_sceneBuilding` guards the re-entrant entry points (`applyFilter`,
  `loadMoreEntries`, `loadAllEntries`) and `closeEvent` ignores the close while a
  build is in progress (the window is `WA_DeleteOnClose`; closing mid-build would
  use-after-free its members).
- A cosmology-watcher result that arrives mid-build (`onCosmologyFinished`) is
  **deferred**: it sets `m_cosmologyFinishDeferred` and returns instead of
  mutating entries / rebuilding re-entrantly. At the end of the build (with
  `m_sceneBuilding` already cleared) a single `QTimer::singleShot(0, …,
  onCosmologyFinished)` applies it once — queued, so the follow-up rebuild is not
  stack-recursive. `m_cosmologyBusy` stays true across the deferral so the
  pending `QFuture` result isn't replaced before it's consumed.

This keeps the *Load all* of a ~3.9M-point snapshot responsive during the
multi-second build. (Chunking addresses build-time UI responsiveness; the GPU
render cost is handled separately by the *Render budget* below, and per-entry RAM
by the compact row store.)

### Per-entry memory (compact row store)

`Catalogue3DEntry` stores its row **once**, as a `QVariantList fields` in
`schema.headers` order. Index access (velocity, table model, details text) is
direct; name access (`fieldString`/`fieldDouble`) resolves the column via
`Catalogue3DSchema::indexOf()`, an O(1) lazily-built (build-once) header→index
`QHash`. This replaced the previous layout, where every row was duplicated as a
`QMap<QString,QVariant>` (column-name keys repeated per row) **and** a parallel
`QStringList`, plus two dead per-row RA/Dec name strings. On a ~3.9M-row snapshot
that redundancy cost gigabytes; the single `QVariant` store also keeps full
numeric precision (the old `QStringList` had stringified values to ~6 significant
digits). This is the data-model half of F5 (RAM); the render/LOD half is still
pending.

---

## Opening a catalogue

`MainWindow::openCatalogue3D()`:
1. Health-check the backend via `BackendClient::health()`
2. Open `RemoteFileBrowserDialog` filtered to `.csv`, `.vot`/`.votable`/`.xml`, `.fits`/`.fit`, `.speck`, `.tbl`
3. Construct `vtkWindowCatalogue3D(filepath, backendUrl, backendToken)`
4. The viewer calls `BackendClient::openCatalogue()` synchronously in its constructor, then triggers the first `applyFilter()` call

Supported formats (backend `_catalogue_load_dataframe`): **CSV**, **VOTable**
(`.vot`/`.votable`/`.xml`), **FITS table**, **Partiview `.speck`** (3-D points),
and **IPAC ASCII table `.tbl`** (`format` "ipac"/"tbl"; e.g. cosmological-snapshot
subtables with `x y z` Mpc + `vx vy vz` + attribute columns → Cartesian). The
format is inferred from the file suffix (`catalogueFormatForPath`). All formats
can also be relayed over SAMP (`/v1/samp/send-catalogue` converts non-native
formats to VOTable).

### Paging — Load more / Load all

The viewer loads sources in pages of 50 000 (`m_pageSize`); the **Filters** tab
shows a *"Loaded X of Y sources"* status and a **Load more…** button (appends the
next page) plus a **Load all** button (one query for every remaining row, no
paging). For large catalogues *Load all* asks for confirmation (>500 000) because
it materialises the whole point cloud at once — the GPU glyph mapper renders
millions fine and per-point labels are capped, but the scene build runs on the
GUI thread and can briefly freeze it. Both buttons hide once everything is
loaded.

### Multi-file load

When you pick a catalogue file that has same-extension siblings in its folder,
the viewer offers to load **all of them as one combined catalogue** (e.g. the 27
spatial subtables of a cosmological snapshot). The desktop sends the full path
list; `/v1/catalogue/open` accepts `paths: [...]`, loads each (same format) and
`pd.concat`s them — **requiring identical columns** (a mismatch is rejected,
naming the offending file, so a silent NaN union can't corrupt the data). The
group is preserved across session-expiry reconnect, so a reopen rebuilds the
same combined catalogue.

---

## Colour mapping

The **Color** combo offers *Morphology color*, any numeric raw column
(`raw:<name>`), and — when the catalogue carries velocity components `vx/vy/vz`
(detected by `detectVelocityColumns()`) — a derived **Speed |v|** option
(`builtin:speed`) that colours each source by its peculiar-velocity magnitude
√(vx²+vy²+vz²). Velocity columns are re-detected from `rawHeaders` whenever the
column set changes (before any scene/colour rebuild), so the speed mapping never
uses stale indices. The chosen colour scale (Linear/Log/Sqrt/…) applies via the
value LUT. The colour scalar array is `float`, so continuous mappings
(speed, raw columns) keep full precision.

## Velocity arrows

When the catalogue carries `vx/vy/vz`, the **Shape** combo gains **Arrows
(velocity)**: each source becomes a `vtkArrowSource` glyph **oriented** by its
velocity vector and **scaled by speed** (the fastest arrow ≈ 6 % of the data
diagonal) — a peculiar-velocity / cosmic-flow view of the point cloud. It uses
the same GPU-instanced `vtkGlyph3DMapper`, orienting/scaling by a named
`VelocityVector` point array (built in `buildScene`) while the other shape modes
keep `ScaleVector` for ellipsoid sizing. Colour still follows the Color combo, so
*Speed |v|* colours the arrows by magnitude too. (If a catalogue without velocity
is loaded, the mode falls back to *Ellipsoid*.)

---

## Coordinate systems: sky vs Cartesian

A catalogue is opened in one of two coordinate systems, auto-detected by the
backend (`_catalogue_metadata_from_dataframe`, returned as
`coordinate_fields.system`):

- **Sky** (`system = "sky"`) — has RA/DEC columns; positions are built from
  RA/Dec + a distance/redshift (see *Distance resolution* and *Coordinate
  frames*). The default for CSV/VOTable/FITS sky catalogues.
- **Cartesian** (`system = "cartesian"`) — has X/Y/Z columns
  (aliases `X`/`SGX`/`XMPC`/`POSX`/`PX`, etc.; e.g. Partiview `.speck` or
  supergalactic-Mpc reconstructions). Positions are the X/Y/Z fields directly.

For Cartesian catalogues the parser (`Catalogue3DParser::parseBackendSubset`,
`schema.isCartesian()`) sets `sceneX/Y/Z` straight from the X/Y/Z columns, so the
default `computed:x/y/z` axis mapping renders them with no manual remap. The
RA/Dec machinery is bypassed: `applyFrameToEntries()`, `triggerCosmologyUpdate()`
and `onCosmologyFinished()` early-return, `detectedRedshiftField()` returns empty,
and `entryDistanceMpc()` reports the radial distance √(x²+y²+z²) from the origin.
This lets a galaxy point cloud in supergalactic Mpc be shown in the same frame as
a velocity field (see the velocity-field viewer).

---

## Distance resolution priority

For each entry, `Catalogue3DParser::detail::entryDistanceMpc(entry, schema)` applies in order:

1. `entry.distanceMpc > 0` — cosmology-selector override (set by `onCosmologyFinished()`)
2. Catalogue `distance` / `dist` / `dMpc` field — positive values only
3. Redshift field (`z`, `REDSHIFT`, `ZSPEC`, `ZMEAN`, `ZPHOT`, …) → `comovingDistanceMpc(z)` — if result > 0
4. Hardcoded 300 Mpc fallback

`comovingDistanceMpc(z)` uses a simple Riemann integral with Planck18 constants (H₀ = 67.74, Ωm = 0.3089, ΩΛ = 0.6911). It returns 0 for z ≤ 0.

---

## Coordinate frames

| Frame | Enum | Description |
|-------|------|-------------|
| FK5 / J2000 | `CoordFrame::FK5_J2000` | Parser output; no conversion needed |
| Galactic (l, b) | `CoordFrame::Galactic` | `wcscon(J2000→GAL)` via libwcs |

`applyFrameToEntries()` recomputes `sceneX/Y/Z` for all entries when the frame combo changes or after cosmology update.

---

## Cosmology model selector

| Model | Implementation | Network call |
|-------|---------------|--------------|
| Planck18 | local `comovingDistanceMpc()` | none |
| Planck15 | backend `/v1/cosmology/distance/batch` | async |
| Planck13 | backend `/v1/cosmology/distance/batch` | async |
| WMAP9 | backend `/v1/cosmology/distance/batch` | async |

On model change:
- Planck18 → clears all `entry.distanceMpc` overrides, recomputes FK5 Cartesian locally, calls `applyFrameToEntries()`
- Others → `m_cosmologyWatcher` launches batch request; `onCosmologyFinished()` applies results

---

## Geometry modes

| Mode | VTK source | Description |
|------|-----------|-------------|
| Ellipsoid | `vtkSphereSource` (non-uniform scale) | scaled by major/minor axes |
| Sphere | `vtkSphereSource` (uniform scale) | single radius |
| Point | `vtkCubeSource` (tiny cube) | for very dense datasets |
| Cross | `vtkAppendPolyData` of line pairs | wireframe cross |

---

## Size modes

| Mode | Scale source |
|------|-------------|
| Fixed | constant `defaultGlyphRadius` |
| Major axis | `entry.fields["major"]` |
| LLS | `entry.fields["lls"]` |
| Flux | `entry.fields["flux"]` (normalised) |

---

## Redshift field detection

`Catalogue3DParser::detail::detectedRedshiftField(schema)` matches schema headers (case-insensitive, stripped of non-alphanumeric chars) against an alias list in priority order:

`Z` > `REDSHIFT` > `ZSPEC` > `ZMEAN` > `ZPHOT`

Compound names like `flux_z` normalise to `FLUXZ` and do not match `Z`.

---

## Interaction

| Event | Effect |
|-------|--------|
| Mouse move | `pickNearestSource()` within 14 px → yellow wireframe sphere (throttled to ~30 ms; hover cleared while dragging) |
| Left click (no drag) | select source → red wireframe sphere + info panel |
| Left click + drag | camera trackball rotate (VTK), hover suppressed |
| Click selected source again | deselect |
| `syncTableSelection()` | table row → camera center on source |

Hover highlighting runs the accelerated `pickNearestSource()` on mouse motion,
leading-edge throttled (`m_hoverThrottle`, ~30 ms) so a flood of move events
can't saturate the pick on a multi-million-point catalogue; `setHoveredSource()`
no-ops when the nearest source is unchanged, so a still cursor triggers no
re-renders.

---

## Sidebar pages

The sidebar uses a rail + stacked-widget layout with three pages:

1. **Visualization** — geometry / size mode + Axis Mapping card (X/Y/Z, size, colour field assignment) + scene controls (frame, cosmology, axes, shells)
2. **Source Info** — selected source metadata (from `ui->pageInfo`)
3. **Filters** — multi-filter list + Apply + Load more

The full catalogue table view is a separate `QDockWidget`, not a sidebar page.

---

## Pagination

Page size: 50 000 rows.
"Load more (N remaining)" button appears in the Filter sidebar page when more rows are available.
Each load-more fetch uses the same active filters and increments the offset.
After each append the full VTK scene is rebuilt (points, glyphs, labels).
