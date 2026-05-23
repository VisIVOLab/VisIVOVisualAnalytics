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
| `meta` | `GET /v1/health`, `GET /v1/sessions`, `GET /v1/sessions/{session_id}/datasets` | startup health check, session stats, per-session dataset enumeration (used by cross-dataset tools like Spectral Stacking) |
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
| `samp` | `POST /v1/samp/send`/`receive`/`connect`/`files/register`/`import-url`/`upload-file`/`send-fits`/`send-catalogue`; `GET /v1/samp/pending`/`inbox`/`status`/`files/{token}` | SAMP messaging + file sharing; **no auth dependency** so the local SAMP hub can reach it directly |
| `spectral` | `POST /v1/spectral/linewidth`, `/linewidth/binary`, `/baseline/{sid}/{did}`, `/stack`, `/stack/binary`; `GET /v1/spectral/linewidth/{ds_id}` | per-pixel FWHM + EW maps (S-02), polynomial / median baseline subtraction (S-03), spectral stacking (S-04) |

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
| `BackendSampSendResult` | SAMP send / send-fits / send-catalogue |
| `BackendSampImportResult` | SAMP import-url / upload-file |
| `BackendCatalogueSubset` | `requestCatalogueSubset()` (legacy bulk fetch) |
| `BackendSessionDatasetsResult` / `BackendSessionDatasetEntry` | `listSessionDatasets()` — enumerate cubes / images currently open in a backend session (drives cross-dataset selection UIs) |

---

## Async Patterns

All viewers follow the same pattern:

1. The UI thread captures needed state and launches a `QtConcurrent::run` worker
2. The worker uses its own `BackendClient` instance (no shared state)
3. A `QFutureWatcher` fires `finished()` on the UI thread
4. The UI thread reads the result struct and applies it (scene rebuild, LUT update, etc.)
5. Controls are disabled during the operation and re-enabled on completion

### Active async paths

| Viewer | Watcher(s) | Triggered by |
|--------|------------|--------------|
| `vtkWindowCube` | `remotePreviewWatcher`, `remoteHighResCubeWatcher`, per-slice + `isosurfaceWatcher`; tools delegated to `MomentMapController`, `NoiseController`, `PvController` | open, slice change, threshold change, moment/PV requests |
| `vtkWindowImage` | `layerLoadWatcher`, `remoteImageWatcher`, `remoteFullImageWatcher` | local "Add layer", remote image preview, remote full-res upgrade |
| `vtkWindowCatalogue3D` | `filterWatcher`, `m_loadMoreWatcher`, `m_cosmologyWatcher` | Apply filter, Load more, cosmology model change |
| `vtkWindowVbt` | `filterWatcher`, `m_loadMoreWatcher` | Apply filter, Load more |

The cube preview→full-res path uses worker-side sanitization and a deferred-Render
pattern to keep the UI thread reactive during the swap — see
`docs/async-patterns.md` (sections *Worker-side data preparation* and
*Deferred render after data swap*) for the details.

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
- `DiagnosticsWindow::showSingleton()` returns the one process-wide window, lazily
  created on first call. Used by `MainWindow::openDiagnosticsWindow()`, the *Command
  Palette* entry "Open Diagnostics Window", and the replicated *View → Diagnostics*
  action in child windows (`vtkWindowCube`, `vtkWindowImage`). All entry points share
  the same model — no duplicate panels.

### Command Palette (⌘K)

`CommandPalette` (`src/gui/`) is a search-driven action launcher owned by
`MainWindow`. `Ctrl+K` is registered as an `ApplicationShortcut` so it fires from
any window. Child windows also expose an explicit *View → Command Palette…* entry
that routes through `QMetaObject::invokeMethod(mainWindow, "openCommandPalette",
QueuedConnection)` — `MainWindow::openCommandPalette` is declared `Q_INVOKABLE` so
child windows do not need to link against `MainWindow` to trigger it.

---

## Cube viewer (`vtkWindowCube`) — interactive extras

In addition to the volume / isosurface / slice / moment / PV / noise pipelines,
the cube viewer exposes a set of configurable interaction extras driven from
the *View* menu and mirrored in the sidebar's *Cube Extras* page. All toggles
use `QAction` as the single source of truth; menu and sidebar widgets sync
bidirectionally via signal-blocker guarded `connect` pairs.

| Feature | QAction / group | Menu | Sidebar widget |
|---------|-----------------|------|----------------|
| Cutting plane visibility | `actionShowCuttingPlane` | View → Show Cutting Plane | "Show cutting plane" checkbox |
| Cutting plane opacity (0–100%) | `cuttingPlaneOpacityGroup` (25 / 50 / 75 / 100 / 0%) | View → Cutting Plane Opacity → … | continuous slider |
| 3D WCS axes (vtkCubeAxesActor) | `actionShow3dWcsAxes` | View → Show 3D WCS Axes | "Show 3D WCS axes" checkbox |
| Slice animation | `actionPlaySlices` + `animationFpsGroup` + `animationModeGroup` | View → Play Slices / Animation Speed / Animation Mode | "▶ Play" button + FPS spin + Mode combo |
| 3D pick on plane click | `actionPickSpectrum3d` | View → Pick Spectrum on Plane Click | "Pick spectrum on plane click" checkbox |

Implementation notes:

- **Cutting plane is a textured `vtkActor`.** The plane source feeds a
  `vtkPolyDataMapper`; a `vtkTexture` is attached to the actor with input
  `sliceColors->GetOutputPort()` (the same `vtkImageMapToColors` pipeline
  that drives the 2D slice view). Slice tile updates propagate to the 3D
  plane automatically via the VTK pipeline. The texture is wired in the
  constructor *after* `setupSliceRenderer()` so that `sliceColors` has a
  valid input by the time the first 3D Render runs; otherwise the volume
  scene would not draw at all.
- **Animation timer** is a `QTimer` member; `setSliceAnimationActive(true)`
  starts it with `1000 / fps` interval, `advanceSliceAnimation()` drives
  `spinSlice` according to mode (Loop / Bounce / Stop at End). The timer
  is stopped in `closeEvent()`.
- **3D spectral pick** installs a `vtkCallbackCommand` on the cube
  interactor's `LeftButtonReleaseEvent` (priority 1.0). On pick:
  `vtkPropPicker` resolves the prop under the cursor; if it is
  `remoteCuttingPlaneActor`, the world XY is mapped to voxel indices and
  fed into the existing probe pipeline (`updateProbePlot()` →
  `ProfileWidget`). `probeModeActive` is flipped on directly (skipping
  `setProbeModeActive` so the 2D cursor and region actions are left
  untouched). The observer is removed in `closeEvent()` and when the
  toggle is unchecked.
- **3D WCS axes** wraps a `vtkCubeAxesActor` with titles from
  `remoteAxisTitle(0..2)` and ranges from `remoteVoxelToWcs` on the cube
  bounds; `applyCubeOpenResult()` refreshes the actor whenever the cube
  bounds change (preview → full-res, ROI switch).

### Optional: VR (OpenXR) offload

The cube viewer can hand off the current `vtkVolume` (with its live LUT /
opacity TF / threshold) to a head-mounted display via a second OpenXR-backed
render window — same actor, same mapper, so every desktop-side parameter
change is reflected in the headset on the next frame without any IPC.

Build is **opt-in** and **does not affect the default macOS / Linux / Windows
build**. Three states:

| `cmake -DVISIVO_ENABLE_VR=…` | VTK has `RenderingOpenXR`? | Result |
|------------------------------|----------------------------|--------|
| `OFF` (default)              | irrelevant                 | Identical to today: *Tools → Open in VR* still appears in the menu but is disabled with an explanatory tooltip. |
| `ON`                         | no                         | CMake prints a `WARNING`, falls back to a no-VR build. Menu entry stays disabled. |
| `ON`                         | yes                        | `VISIVO_HAS_VR=1` defined; `VTK::RenderingOpenXR` linked. *Tools → Open in VR* is enabled when an OpenXR runtime + HMD are detected at runtime. |

Implementation notes:

- [`src/gui/CubeVRController.{h,cpp}`](../src/gui/CubeVRController.h) — PIMPL
  controller. The header is always compilable; OpenXR includes live behind
  `#ifdef VISIVO_HAS_VR` in the `.cpp` only.
- `CubeVRController::isCompiledIn()` — compile-time flag.
- `CubeVRController::isRuntimeAvailable()` — lazy runtime probe (`vtkOpenXRRenderWindow::Initialize()`), result cached for the session.
- `CubeVRController::open(renderer, volume)` — shares the desktop volume actor with a second `vtkOpenXRRenderWindow`. Currently blocks the UI thread for the duration of the session (matches the user mental model "I'm in VR until I take the headset off"); a future iteration can move this to a `QThread` with a mutex on the shared mapper.
- macOS is not a supported VR target (Apple removed SteamVR support in 2020; no Vision Pro / OpenXR runtime). The build flag is honoured anyway — it just falls into the "no runtime" branch.

To enable end-to-end on Windows / Linux:

1. Rebuild VTK with `-DVTK_MODULE_ENABLE_VTK_RenderingOpenXR=YES`. Requires the Khronos OpenXR loader headers (Linux: `libopenxr-loader1-dev` / equivalent; Windows: Khronos OpenXR SDK).
2. Install an OpenXR runtime (SteamVR, Oculus, WMR, Monado, …) — pick the one bundled with your HMD vendor.
3. Configure VisIVO with `cmake -DVISIVO_ENABLE_VR=ON …`.
4. Launch, open a cube, *Tools → Open in VR*.

### QAction creation ordering

QActions referenced by the sidebar's *Cube Extras* page must be created
*before* `setupSidebar()` is called — otherwise the sidebar dereferences
null pointers and the window segfaults. In the constructor:

```
ui->setupUi(this);
setupViewerToolbar();
// 1. Pre-create QActions + QActionGroups used by sidebar
actionShowCuttingPlane = new QAction(…); …
cuttingPlaneOpacityGroup = new QActionGroup(this); …
// 2. Build sidebar (binds widgets to the pre-created actions)
setupSidebar();
// 3. Later: attach tooltips, populate menus, connect signal handlers
```

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

## Tool dialogs (uniform modality)

All "tool" dialogs launched from menus or the Tools sidebar are now opened
**non-modal** (`QDialog::show()`), so the user can keep interacting with the
viewers, panels, and other tools while a long compute is running. The list
covers `LinewidthDialog`, `BaselineDialog`, `StackDialog`, `SourceFindDialog`,
`LUTCustomizerDialog` (2D + 3D), the FITS header viewer, and the moment
description popup. Long-running operations show their own scoped progress
indicator inside the dialog (a `QProgressDialog` window-modal to the dialog
itself, not to the application) so the "busy" state is still clear.

`StackDialog` is built around an explicit `DatasetEntry` list:

- The caller (`MainWindow` or `vtkWindowCube`) calls
  `BackendClient::listSessionDatasets(sessionId)` in a worker, gets the full
  set of cubes open in the backend session, and passes them with the
  `currentDatasetId` of the calling window.
- The dialog pre-checks the current cube, hides non-cube entries, and
  disables (with tooltip) cubes whose `(width × height × depth)` does not
  match the reference shape — they cannot be stacked together.
- Item label = basename of the FITS file; the true `dataset_id` lives in
  `Qt::UserRole`.

---

## Heavy-task throttle (backend pool sharing)

The single `ProcessPoolExecutor` in the backend (size `VISIVO_WORKERS`,
default 4) is shared by every endpoint. A global `asyncio.Semaphore`
(`_HEAVY_SEM`) in `backend/app/dependencies.py` caps how many long-running
"heavy" invocations can hold a pool slot at once, leaving the rest free for
interactive requests:

- Helpers: `_run`, `_run_with_limit` (interactive); `_run_heavy`,
  `_run_heavy_with_limit` (heavy, semaphore-gated).
- Default heavy capacity: `max(1, VISIVO_WORKERS - 1)`.
- Classification: moment / isosurface / PV / spectral (linewidth, baseline,
  stack) → heavy; preview / slice / subvolume / noise / image → interactive.

Net effect: a slice scroll, ROI subvolume, or probe issued while a moment /
linewidth / stack is running is processed by the free worker instead of
queueing behind the long job. The chunk dispatch inside the linewidth
orchestrator also goes through `_run_heavy`, so its parallelism is bounded
by the semaphore rather than by the pool size.

Tunables: `VISIVO_HEAVY_SLOTS`, `VISIVO_LINEWIDTH_CHUNKS`,
`VISIVO_LINEWIDTH_SNR` (see `docs/async-patterns.md`).

---

## Unit Tests (`tests/`)

QTest-based headless suite; no VTK, no Qt::Widgets, no live backend.

| File | Tests | What it covers |
|------|-------|----------------|
| `test_backendclient.cpp` | 14 | `parseMomentResultObject`, `parsePvResultObject`, `parseNoiseResultObject` |
| `test_catalogue_parser.cpp` | 24 | `detectedRedshiftField`, `detectedDistanceField`, `comovingDistanceMpc`, `entryDistanceMpc` |

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
| `VISIVO_HEAVY_SLOTS` | `max(1, VISIVO_WORKERS-1)` | Max concurrent heavy tasks (moment / isosurface / pv / spectral). Leaves `WORKERS - HEAVY_SLOTS` pool slots reserved for interactive requests so the GUI stays responsive while compute is running. |
| `VISIVO_LINEWIDTH_CHUNKS` | `VISIVO_WORKERS` | Row-chunks the linewidth orchestrator fans out per request. Already gated by `VISIVO_HEAVY_SLOTS`, so usually no need to lower this. |
| `VISIVO_LINEWIDTH_SNR` | `3.0` | Per-pixel SNR cutoff for skipping background pixels before the Gaussian fit (`0` disables skip). |
| `VISIVO_PRODUCT_CACHE_ENTRIES` | `32` | LRU capacity of the in-process product cache (shared by moment, isosurface, pv, linewidth results). |

### Client build flags (CMake)

| Flag | Default | Effect |
|------|---------|--------|
| `VISIVO_ENABLE_VR` | `OFF` | Enable the optional VR (OpenXR) cube viewer offload. Requires a VTK built with `-DVTK_MODULE_ENABLE_VTK_RenderingOpenXR=YES`; otherwise CMake falls back to a no-VR build with a warning. macOS is not a supported VR target (no OpenXR runtime available); the flag is accepted but the *Tools → Open in VR* action stays disabled at runtime. See the *"Optional: VR (OpenXR) offload"* subsection above for the full enablement procedure. |
