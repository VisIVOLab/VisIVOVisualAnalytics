# Catalogue 3D Viewer

Reference for `vtkWindowCatalogue3D` (`src/gui/vtkWindowCatalogue3D.h/cpp`).

---

## Opening a catalogue

`MainWindow::openCatalogue3D()`:
1. Health-check the backend via `BackendClient::health()`
2. Open `RemoteFileBrowserDialog` filtered to `.csv`
3. Construct `vtkWindowCatalogue3D(filepath, backendUrl, backendToken)`
4. The viewer calls `BackendClient::openCatalogue()` synchronously in its constructor, then triggers the first `applyFilter()` call

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
| Mouse move | `pickNearestSource()` within 14 px → yellow wireframe sphere |
| Left click (no drag) | select source → red wireframe sphere + info panel |
| Left click + drag | camera trackball rotate (VTK) |
| Click selected source again | deselect |
| `syncTableSelection()` | table row → camera center on source |

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
