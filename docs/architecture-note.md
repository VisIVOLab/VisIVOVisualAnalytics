# Architecture Note

## Purpose
This note summarises the current architecture of VisIVO Visual Analytics after the full client/backend integration, async viewer additions, authentication layer, and diagnostic infrastructure.

It describes what is already true in the codebase today, which boundaries are now stable, and what remains deferred.

---

## High-Level Topology

```
┌─────────────────────────────────────────────────┐
│              Qt/VTK Desktop Client               │
│                                                  │
│  main() ──► StartupDialog                        │
│               │  BackendLauncher (process mgmt)  │
│               ▼                                  │
│  MainWindow ──► DataHubWidget                    │
│      │                                           │
│      ├──► vtkWindowCube        (local/remote)    │
│      ├──► vtkWindowImage       (local/remote)    │
│      ├──► vtkWindowCatalogue3D (remote-only)     │
│      ├──► vtkWindowVbt         (remote-only)     │
│      ├──► vtkWindowVbtVolume   (remote-only)     │
│      ├──► HiPSWindow           (remote-only)     │
│      └──► RemoteMomentWindow   (remote-only)     │
│                                                  │
│  BackendClient ── Qt Network ──► FastAPI backend │
│  AuthWrapper   ── OIDC/PKCE ──► identity server  │
│  DiagnosticsManager (singleton, in-process log)  │
└─────────────────────────────────────────────────┘
```

`MainWindow` is created only after `StartupDialog` accepts — i.e., only after the backend is reachable and a token is available (or explicitly skipped). The backend process is owned by `BackendLauncher` in `main()` and outlives all windows.

---

## Build Modules

### `visivo_shared_core`
Local non-UI application/core logic (`src/app/`, `src/AstroUtils`).

Contents:
- `DatasetOpenRequest`, `DatasetOpenService`, `DatasetOpenTypes`
- `ImageLayerImportRequest`, `ImageLayerImportService`, `ImageLayerImportResult`
- `AstroUtils`
- `BackendClient` — synchronous REST client (Qt Network, no Qt::Widgets)
- `DiagnosticsManager` — singleton structured event log

Does not depend on Qt::Widgets, VTK, or libwcs.

### `visivo_shared_vtk`
VTK/runtime support (`src/vtk/`).

Contents:
- `ImageLayerSet`, `ImageLayer`
- `MomentProcessingService`, `MomentMapComputeTask`
- `CubeOpenPreviewTask`, `ImageLayerLoadTask`
- `vtkFITSReader`, `vtkFITSWriter`
- `vtkLegendScaleActorWCS`, `vtkInteractorStyleProfile`
- `ColorMaps`

Does not depend on Qt::Widgets.

### `visivo_shared_core` — startup / app layer (`src/app/`)

In addition to the original `BackendClient` and `DiagnosticsManager`:

- `BackendLauncher` — manages the backend process lifecycle:
  1. health-checks the configured URL
  2. optionally starts `python -m uvicorn app.main:app` if not reachable
  3. polls the health endpoint until ready (or times out)
  4. emits `alreadyRunning`, `backendStarted`, or `failed` signals
  5. captures all backend stdout/stderr into `capturedLogs()`
  6. resolves the Python interpreter via a six-level priority chain (see README)

### Client GUI (`src/gui/`, `src/auth/`, `src/`)
Qt widget layer and UI orchestration.

Contents:
- `StartupDialog` — pre-`MainWindow` dialog; drives Backend → Auth → Ready sequence
- `MainWindow` — top-level shell, menus, action routing
- `DataHubWidget` — landing page; health check, file browser, session start
- `vtkWindowCube` — FITS cube viewer (local preview + remote full, moment, PV, noise)
- `vtkWindowImage` — FITS image viewer (local + remote multi-layer); "Add New FITS File" uses `RemoteFileBrowserDialog`
- `vtkWindowCatalogue3D` — 3-D catalogue viewer (remote CSV/backend)
- `vtkWindowVbt` — VBT point-cloud viewer (remote)
- `vtkWindowVbtVolume` — VBT volume viewer (remote)
- `HiPSWindow` / `HiPSViewportWidget` — HiPS sky viewer with overlay
- `RemoteMomentWindow` — standalone moment map viewer (async task-based)
- `RemoteFileBrowserDialog` — file-picker backed by `/v1/files/list`; reused for image layers
- `NoiseRegionDialog` — region selector for noise computation
- `ProfileWidget`, `PvDiagramWidget` — profile/PV plot widgets
- `DiagnosticsWindow` — live table view of `DiagnosticsManager` entries
- `AuthWrapper`, `OIDCAuthorizationCodeFlow` — OIDC PKCE auth
- `LUTCustomizerDialog`, `SettingsDialog`, `AboutDialog`
- `Catalogue3DParser`, `Catalogue3DTableModel`, `CatalogueTableModel`
- `VisivoTheme` — application-wide style sheet

### Third-party bundled
- `libwcs` — WCS coordinate conversion (C, statically linked)
- `qcustomplot` — 1-D profile and PV plots

---

## Backend (FastAPI, Python)

The backend process (`backend/app/main.py`) runs as a local or remote server.
The client talks to it exclusively through `BackendClient` (REST/HTTP, bearer-token auth).

### Endpoint groups

| Tag | Endpoints | Notes |
|-----|-----------|-------|
| `meta` | `GET /v1/health`, `GET /v1/sessions` | startup health check, session enumeration |
| `files` | `GET /v1/files/list`, `POST /v1/files/header` | backend-side filesystem browser |
| `datasets` | `POST /v1/datasets/open` | open FITS/dataset, returns `datasetId` + WCS metadata |
| `catalogue` | `POST /v1/catalogue/open`, `/subset`, `/query` | CSV catalogue; paginated query via `limit`/`offset` |
| `vbt` | `POST /v1/vbt/open`, `/subset`, `/query` | VBT point table; paginated query |
| `cube` | `POST /v1/cube/preview`, `/slice`, `/subvolume`, `/pv`, `/noise` | cube data slices, PV diagram, noise stats |
| `products` | `POST /v1/products/moment`, `/isosurface` | synchronous moment/isosurface computation |
| `tasks` | `POST /v1/tasks/moment`, `/pv`; `GET`/`DELETE /v1/tasks/{id}` | async task queue; client polls for completion |
| `image` | `POST /v1/image/full`, `/preview` | 2-D image export/preview |
| `cosmology` | `POST /v1/cosmology/distance`, `/distance/batch` | redshift → comoving distance (astropy) |
| `hips` | `POST /v1/hips/open`; `GET /v1/hips/{id}/allsky`, `/tile/…`; `POST /query_tiles`, `/catalogue_overlay` | HiPS sky survey tiles + source overlay |
| `resolve` | `POST /v1/resolve/target` | astronomical name → sky coordinates |

### Backend session model
- `POST /v1/datasets/open` returns a `session_id`; the client echoes it as `X-Visivo-Session` in all subsequent requests for that dataset
- Sessions are tracked server-side; `GET /v1/sessions` lists active sessions
- Auth: every request carries `X-Visivo-Token` (static bearer token); separate OIDC flow covers the VLKB identity service

---

## `BackendClient`

Single synchronous REST client (`src/app/BackendClient.h/cpp`).

Key design decisions:
- all methods are **blocking** — called from `QtConcurrent::run` worker threads, never from the UI thread
- session management: `setSessionId()` / `sessionId()`; echoed automatically as `X-Visivo-Session`
- token resolution order: (1) explicit `setToken()`, (2) `~/.visivo_token` file written by the backend at startup
- static parse helpers exposed for unit testing: `parseMomentResultObject`, `parsePvResultObject`, `parseNoiseResultObject`
- timeout: per-request via `requestTimeoutFor()`; long-running endpoints (subvolume, full-res cube) get extended timeouts

Result structs (all in `BackendClient.h`):

| Struct | Produced by |
|--------|-------------|
| `BackendHealthResult` | `health()` |
| `BackendListFilesResult` | `listFiles()` |
| `BackendFileHeaderResult` | `fileHeader()` |
| `BackendOpenDatasetResult` | `openDataset()` |
| `BackendMomentResult` | `requestMoment()` / task polling |
| `BackendCubePreviewResult` | `requestPreview()` |
| `BackendCubeSliceResult` | `requestSlice()` |
| `BackendCubeSubvolumeResult` | `requestSubvolume()` |
| `BackendCubePvResult` | `requestPv()` |
| `BackendCubeNoiseResult` | `requestNoise()` |
| `BackendCatalogueInfo` | `openCatalogue()` |
| `BackendTabularQueryResponse` | `queryTabularCatalogue()`, `queryTabularVbt()` |
| `BackendVbtOpenResult` | `openVbt()` |
| `BackendVbtSubsetResult` | `requestVbtSubset()` |
| `BackendImageResult` | `requestImagePreview()`, `requestImage()` |
| `BackendIsosurfaceResult` | `requestIsosurface()` |
| `BackendHiPSSurveyInfo` | `openHiPS()` |
| `BackendHiPSViewResponse` | `requestHiPSTilesForView()` |
| `BackendTargetResolveResult` | `resolveTarget()` |
| `BackendCosmologyBatchResult` | `requestCosmologyDistanceBatch()` |
| `BackendHiPSCatalogueOverlayResult` | `requestHiPSCatalogueOverlay()` |
| `BackendTaskCreateResult` | `createMomentTask()`, `createPvTask()` |
| `BackendTaskStatusResult` | `requestTaskStatus()`, `waitForTaskCompletion()` |

---

## Async Patterns

All viewers follow the same pattern:

1. The UI thread captures needed state and launches a `QtConcurrent::run` worker
2. The worker uses its own `BackendClient` instance (no shared state)
3. A `QFutureWatcher` fires `finished()` on the UI thread
4. The UI thread reads the result struct and applies it (scene rebuild, LUT update, etc.)
5. Controls are disabled during the operation and re-enabled on completion

### Active async paths

| Viewer | Watcher | Triggered by |
|--------|---------|--------------|
| `vtkWindowCube` | `previewWatcher`, `fullResWatcher`, `momentWatcher`, `pvWatcher` | open, moment order change, PV line draw |
| `vtkWindowImage` | `layerWatcher` | "Add layer" action |
| `vtkWindowCatalogue3D` | `filterWatcher`, `m_loadMoreWatcher`, `m_cosmologyWatcher` | Apply filter, Load more, cosmology model change |
| `vtkWindowVbt` | `filterWatcher`, `m_loadMoreWatcher` | Apply filter, Load more |

### Pagination (large datasets)

Both `vtkWindowCatalogue3D` and `vtkWindowVbt` support incremental pagination for datasets larger than the default page size (50 000 rows):

- `m_pageSize = 50000`, `m_currentOffset`, `m_totalCount`
- `applyFilter()` always resets offset to 0 and fetches the first page
- A "Load more (N remaining)" button appears in the filter panel when `offset < totalCount`
- Clicking it triggers `loadMoreEntries()` via `m_loadMoreWatcher`
- Catalogue3D: new entries are **appended** to `this->entries`; the full VTK scene is rebuilt
- VBT: new column vectors are **appended** to `this->subsetResult.columns`; the point cloud is rebuilt

### Task-based async (moment, PV)

For long-running operations the backend supports a task queue:
- `createMomentTask()` / `createPvTask()` → returns a `task_id`
- `waitForTaskCompletion()` polls `GET /v1/tasks/{id}` with exponential back-off
- `RemoteMomentWindow` uses this path exclusively

---

## Authentication

### Backend token
Static bearer token, resolved by `BackendClient` from:
1. Explicit `setToken()` call (from Settings)
2. `~/.visivo_token` file written by the backend process at startup

### VLKB OIDC
`AuthWrapper` wraps `OIDCAuthorizationCodeFlow` (PKCE) for the VLKB identity service.
- `AuthWrapper::grant(AuthService::VLKB)` launches the browser-based flow
- `HttpServerReplyHandler` captures the redirect callback on `localhost`
- `VisIVOUrlSchemeHandler` handles the custom `visivo://` URL scheme on macOS
- Tokens are stored in-process; `logout()` clears them

---

## Diagnostics

`DiagnosticsManager` (singleton, `src/app/`) is an in-process structured event log.

- `publish(level, category, source, message, datasetId, sessionId, operationTag)` — called from any thread
- Categories: `Scientific`, `Client`, `Backend`, `Task`, `WCS`, `Remote`, `Rendering`, `Performance`
- `DiagnosticsModel` exposes the log as a `QAbstractTableModel` for `DiagnosticsWindow`
- `DiagnosticsWindow` is accessible from the `View` menu at runtime

---

## Catalogue 3D viewer (`vtkWindowCatalogue3D`)

Renders a remote CSV catalogue as a 3-D point cloud in Cartesian (RA/Dec/distance) space.

Key capabilities:
- **Coordinate frame**: FK5 J2000 (default) or Galactic (l, b) via `applyFrameToEntries()`; uses `wcscon()` from libwcs
- **Distance resolution** (per entry, priority order):
  1. `entry.distanceMpc` override (set by cosmology selector)
  2. catalogue `distance` / `dist` / `dMpc` field
  3. cosmological integration from redshift (`z`, `REDSHIFT`, `Zspec`, …)
  4. hardcoded fallback 300 Mpc
- **Cosmology model selector**: Planck18 (local integration), Planck15/13/WMAP9 (async batch via `/v1/cosmology/distance/batch`)
- **Geometry modes**: Ellipsoid, Sphere, Point, Cross (vtkGlyph3D)
- **Size modes**: Fixed, Major axis, LLS, Flux
- **Interaction**: hover highlight (yellow wireframe sphere), click-select (red wireframe), sidebar info panel, table view dock
- **Morphology LUT**: deterministic colour per morphology class; unknown classes cycle the palette
- **Pagination**: 50 000 row pages; "Load more" button

---

## Loading placeholder

All viewers show a centred `vtkTextActor` message while data is being fetched:

| Viewer | Text | Disappears when |
|--------|------|-----------------|
| `vtkWindowCube` | "Loading…" (3D) / "Loading…" (2D slice) | `applyPreview()` / `applyRemoteSliceResult()` |
| `vtkWindowImage` | "Loading image…" | `applyRemoteMasterLayer()` or full-res failure |
| `vtkWindowVbt` | "Loading…" | end of `buildPointCloud()` |
| `vtkWindowVbtVolume` | "Loading…" | end of `applySubsetResult()` |

Style: font 16 pt, colour `(0.72, 0.84, 0.91)` (`#B8D6E8`), centered, NormalizedViewport `(0.5, 0.5)`, no bold, no shadow.

---

## VBT viewer (`vtkWindowVbt`)

Renders a remote VBT (VisIVO Binary Table) dataset as a 3-D point cloud.

Key capabilities:
- **Render modes**: Plain (vtkPolyDataMapper) and Gaussian splat (vtkPointGaussianMapper)
- **Color mapping**: any scalar field; configurable colour map and range
- **Sidebar**: Display (render mode, colour, range), Filters, Metadata pages
- **Pagination**: 50 000 row pages; "Load more" appends column vectors

---

## HiPS viewer (`HiPSWindow`)

Interactive all-sky survey browser.

Key capabilities:
- Fetches AllSky mosaic via `requestHiPSAllsky()`; individual tiles via `requestHiPSTile()`
- `requestHiPSTilesForView()` requests the backend to compute which tiles cover the current viewport
- Astronomical name resolution via `resolveTarget()` → `POST /v1/resolve/target`
- Catalogue overlay via `requestHiPSCatalogueOverlay()` → `POST /v1/hips/catalogue_overlay`
- `HiPSViewportWidget` owns the tile compositing and paint logic

---

## Unit Tests (`tests/`)

QTest-based headless suite; no VTK, no Qt::Widgets, no live backend.

| File | Tests | What it covers |
|------|-------|----------------|
| `test_backendclient.cpp` | 16 | `parseMomentResultObject`, `parsePvResultObject`, `parseNoiseResultObject` |
| `test_catalogue_parser.cpp` | 26 | `detectedRedshiftField`, `detectedDistanceField`, `comovingDistanceMpc`, `entryDistanceMpc` |

Build:
```
cmake -B build -DBUILD_TESTING=ON
cmake --build build --target VisIVOTests
ctest --test-dir build -V
```

Static library `visivo_test_support` (BackendClient + DiagnosticsManager) is shared across test executables without pulling in Qt::Widgets or VTK.

---

## Stable Boundaries Today

| Boundary | Status |
|----------|--------|
| `BackendLauncher` | stable; all backend process management goes through here |
| `StartupDialog` | stable; Backend → Auth → Ready sequence before main window |
| `DatasetOpenService` | local, explicit request/result |
| `ImageLayerImportService` | local, explicit request/result |
| `MomentProcessingService` | backend-authoritative facade |
| `BackendClient` | stable; all backend I/O goes through here |
| `DiagnosticsManager` | stable singleton; all structured logging goes through here |
| Async worker pattern | stable; each viewer owns its watchers explicitly |
| Pagination state | stable; uniform across catalogue and VBT viewers |
| Loading placeholder | stable; all four viewers use the same `vtkTextActor` pattern |
| Layer alignment pipeline | stable; all layer sources (manual, remote, VLKB) route through `loadImageLayer` |

---

## Non-Goals / Deferred

- No dataset upload/staging for desktop-local files (moment computation requires backend-visible dataset)
- No remote rendering
- No general async/job framework (each use case defines its own watcher)
- No cancellation or progress reporting for in-flight requests
- No generic service interfaces (DI, plugin system)
- No large `vtkWindowCube` decomposition

---

## Architecture Decisions

### 1. All backend I/O through a single synchronous client called off-thread
`BackendClient` is synchronous and safe to call from `QtConcurrent::run`. No async Qt Network code, no callback spaghetti. Each worker creates its own `BackendClient` instance.

### 2. UI thread only reads results, never calls the backend directly
The UI thread is responsible only for applying result structs to VTK pipelines and widget state. It never blocks on network I/O.

### 3. Pagination is stateful per-window, reset on filter change
`m_currentOffset` / `m_totalCount` are window-local. `applyFilter()` always resets them. The "Load more" button is the only way to advance the offset.

### 4. Cosmology distances are transparent to rendering code
`Catalogue3DEntry::distanceMpc` (0 = auto) is the override seam. `entryDistanceMpc()` applies the priority chain. Rendering code calls only `entryDistanceMpc()` and is unaware of which source was used.

### 5. Build modularisation follows dependency direction
`visivo_shared_core` → no Qt::Widgets, no VTK.
`visivo_shared_vtk` → no Qt::Widgets.
GUI executable → can use all.
Test suite → links only `visivo_test_support` (core subset).

---

## Performance Tuning

### Backend environment variables

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_MOMENT_THREADS` | `0` (auto) | Number of threads for M0/M6/M10 moment chunking. `0` = `min(4, cpu_count)`. Positive values set the count explicitly. M1, M2, M8 are sequential (normalisation requires the full axis). |
| `VISIVO_DASK_THRESHOLD_BYTES` | `4294967296` (4 GiB) | Cube byte size above which the Dask out-of-core path is selected (when Dask is installed). |
| `VISIVO_WORKERS` | `min(4, cpu_count)` | Number of ProcessPoolExecutor workers for CPU-bound FITS operations. |
