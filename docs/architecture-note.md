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
| `cube` | `POST /v1/cube/preview`, `/slice`, `/subvolume`, `/pv`, `/noise`, `/save_subregion` | cube data slices, PV diagram, noise stats; `save_subregion` writes a cropped FITS into the Workspace Exports dir |
| `products` | `POST /v1/products/moment`, `/isosurface`, `/moment_fits` | synchronous moment / isosurface computation; `moment_fits` persists a 2-D moment FITS into the Workspace Exports dir |
| `exports` | `GET /v1/exports/list`, `GET /v1/exports/download?filename=…`, `DELETE /v1/exports/{filename}` | Workspace Exports lifecycle: enumerate, stream-download, remove. All operations are path-traversal-safe; absolute paths and `..` segments are refused with HTTP 400 |
| `tasks` | `POST /v1/tasks/moment`, `/pv`; `GET`/`DELETE /v1/tasks/{id}` | async task queue; client polls for completion |
| `image` | `POST /v1/image/full`, `/preview` | 2-D image export/preview |
| `cosmology` | `POST /v1/cosmology/distance`, `/distance/batch` | redshift → comoving distance (astropy) |
| `hips` | `POST /v1/hips/open`; `GET /v1/hips/surveys`, `/v1/hips/{id}/allsky`, `/tile/…`; `POST /query_tiles`, `/catalogue_overlay` | the CDS survey registry, HiPS sky survey tiles + source overlay |
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
| `BackendHiPSRegistryResult` | `requestHiPSSurveys()` |
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

## The remote-render recommendation, and what it is worth locally

`render_session/_scene.py` feeds the **whole cube** to `vtkGPUVolumeRayCastMapper`
— the same mapper the desktop uses — from a shared-memory buffer. Server-side
rendering is therefore *not* out-of-core, which has a consequence worth writing
down because the UI claimed the opposite twice (banner and Load-Full-Resolution
dialog, both corrected):

| | remote backend | backend on this machine |
|---|---|---|
| Transfer | avoided: frames instead of GB of voxels | none either way (loopback) |
| GPU memory | the *backend's* GPU | the **same** GPU, same volume |
| Extra cost | frame encode/decode | frame encode/decode |
| Real gain | not moving the data | only the **full grid** vs the client's decimated proxy |

A prior question, which the client used to skip entirely: **can this backend do
it?** `server_side_rendering_available` has been in the dataset-open response all
along (`routers/datasets.py` → the EGL probe in `render_session/__init__.py`) and
nothing read it, so the UI offered server-side rendering on backends that refuse
it at `/v1/render/*` — every macOS backend among them, since there is no EGL
there. It is now parsed into `OpenDatasetResult::serverSideRenderingAvailable`,
handed to the window by `setServerSideRenderingAvailable()`, and gates the
*Remote* mode action (disabled, with the reason as its tooltip), the banner's
Switch button, and the Load-Full-Resolution dialog's remote option. It defaults
to **true** so a backend predating the field keeps the old behaviour.

So on a local backend the honest first offer is *Load full resolution*
(`startFullResolutionLoad()`, the tail of View ▸ Load Full Resolution without its
warning dialog), which is what `Cube/local_full_threshold_mb` is holding back;
switching to remote remains second, worth it mainly to keep the cube out of the
viewer process. `backendIsLocal()` decides, from the loopback spellings plus a
comparison against the URL the app calls its local backend (read raw from
`Settings.ini` — the `Settings` constructor launches python with 30-second waits
and must not be on a UI path).

The suggestion's dismissal is persisted per dataset AND backend
(`Cube/remote_recommend_dismissed`, keys `host:port|path`, bounded to 64
entries): hiding the banner in one window meant it came back on every open of
the same cube, and keying on the path alone would have silenced the same path
served by a different backend, which is a different cube.

---

## Timeouts: the client must outlast the server

`BackendClient::requestTimeoutFor()` is a per-endpoint table. The rule, which
the copilot entry documented and nothing else followed, is that the **client
timeout sits above the server's own ceiling**, so the server's answer — a result
or its graceful 504 — wins the race.

Heavy scientific compute goes through the backend's `_run_heavy*`, bounded by
`VISIVO_HEAVY_TIMEOUT_S` (900 s). Those endpoints were falling through to the
generic **120 s**, so a line-width map on a 1.5 GB cube was aborted by the client
after two minutes while the backend went on computing for up to fifteen — still
holding a per-session compute slot, which is then one of the three that answer
429 to the next tool. They now get **960 s**, from the same
`heavyComputePaths()` list that feeds the in-flight counter below.

## Knowing what the app is doing

Three fixes to the same complaint ("the log tells me nothing while a tool runs"):

- **`N running · M queued`** counted only the backend's task REGISTRY, which
  holds the async moment / PV jobs. Every other compute is a synchronous POST and
  appeared nowhere, so the status bar read *0 running* for the whole of a
  line-width map. `BackendClient::heavyRequestsInFlight()` counts the client's
  own heavy requests (RAII-scoped, atomic, since they run on worker threads) and
  the status slot and Inspector rail add it in.
- **The Diagnostics *Operation* column** was filled only by the async task
  poller; *Context* needs a `datasetId`, which every call site passed empty. Both
  are now derived in the one place that knows: `operationTagForPath()` (pure,
  unit-tested — `"/v1/spectral/linewidth/binary"` → `spectral/linewidth`) and the
  request's own body/query.
- **The log was drowned** by polls: `/v1/exports/list`, every 4 s per open Data
  Hub, was the only thing on screen during a multi-minute compute. It joined
  `_isDiagnosticsQuietPath` (which gates only the SUCCESS lines — a failing poll
  is still reported), and a heavy request now announces itself when it **starts**
  ("…started — this can take minutes on a large cube") rather than only on
  completion. The announcement is suppressed on a 429 retry, so the line belongs
  to the attempt the user triggered.

### Closing a tool window while it computes

Every compute dialog had the same `closeEvent`: `event->accept()`, with a comment
noting that the worker finishes on the thread pool and the `finished` handler
discards the result because its `QPointer` to the dialog is null. All accurate,
and entirely silent. What actually happens:

- the computation **keeps running**, on the backend to completion, holding one of
  the session's compute slots, and its result is thrown away;
- it **cannot be cancelled**: these are synchronous POSTs with no cancel route,
  and the backend's process-pool children cannot be killed from Python (the note
  in `dependencies.py`). Nothing leaks as an OS zombie — the work finishes
  unobserved;
- while it runs it is now visible in "N running" and in Diagnostics.

`ToolDialogClose.h` holds that explanation once, and `LinewidthDialog`,
`BaselineDialog`, `StackDialog` and `SourceFindDialog` ask before discarding
("Close and Discard" / "Keep Waiting", the latter the default). The two that had
no `closeEvent` at all also had no busy flag; `setUiBusy()` — the one place that
already knew — now sets one.

The real fix, deferred: hand the request to something that outlives the dialog,
so closing it yields the map instead of wasting the compute. The result already
opens in its own window, so nothing but ownership stands in the way.

### "Is anything happening?" — one progress affordance

The tools had grown three answers. Stack and Source Finding put up a
window-modal `QProgressDialog` (on macOS a sheet out of the title bar, the one
users actually noticed); Line-width and Baseline wrote into their own dialog's
status bar; Compute Moment closed its dialog *first* and then reported into the
**cube window's** status bar, bottom-left, away from where the user had just
been working. Same operation, three places to look.

`ToolProgressSheet.h` is now the only way to make one, and every tool that runs
a backend compute follows the same shape:

> **The settings window stays open across the run, and the sheet goes on IT.**
> The action button starts the work instead of closing the window; the sheet is
> window-modal on that window, so it both reports the work and blocks the
> controls underneath for the duration; when the result lands the same window is
> still there to run again with different parameters.

Line-width, Baseline, Stack and Source Finding already worked that way. Compute
Moment, Estimate Noise, Export Sub-Cube, Mask 3-D Region and Kinematic Model
Overlay did not: they were `QDialog::exec()` prompts that closed on Accept, so
the compute they started had no window of its own and its only sign of life was
a line in the *cube window's* status bar, bottom-left, away from where the user
had just been working (user report). They are persistent windows now.

Two of them also asked **twice**. Export Sub-Cube prompted for bounds, closed,
then prompted for a filename; Mask 3-D Region prompted for bounds, closed, then
asked for the mask mode — and it borrowed the export's dialog verbatim, so it
opened a window titled "Export Sub-Cube as FITS" that described exporting. Both
now have one window with every parameter in it (`CubeBoxToolDialog`, shared by
the two tools that act on a 3-D box, with per-tool title, hint, action verb and
extra field).

The remaining exceptions are **Export Current Channel as 2-D FITS** and **Export
Moment Map as FITS**, which take one filename and nothing else and so have no
settings window to host a sheet. They are not always fast, and the note that
used to sit here (0.01–0.03 s, therefore never a sheet) generalised one
measurement on a small cube: `worker_moment_save_fits` RE-COMPUTES the moment,
auto-thresholding included, rather than serialising the map already on screen,
so on a large cube, a queued backend or slow storage it can pass 400 ms easily.
They are simply not converted yet. **Export Movie** keeps its own determinate progress dialog:
it is the one operation that can actually report per-frame progress, and a real
percentage beats the shared spinner. **Channel Maps** opens a window that *is*
the result rather than computing into one.

Three properties of the sheet are deliberate:

- **Indeterminate.** No backend compute reports per-chunk progress yet, and a
  fabricated percentage is worse than an honest spinner. (The missing piece is
  unchanged: line-width dispatches N chunks and reports none of them, which
  needs a channel back to the dialog, not just a log line.)
- **No Cancel button.** The compute keeps running on the backend whether or not
  the client watches, so a Cancel that merely hid the sheet would be a lie.
  Note this does not make the sheet un-dismissable: Escape still closes it
  (measured), and the settings windows converted below do not route their own
  close through `confirmAbandonRunningCompute`. Dismissing either does not
  cancel anything and does not lose the result — the completion handler still
  runs, clears the status line and shows the outcome, parented to the viewer
  when its dialog is gone.
- **Interactive paths only.** `MomentMapController::dispatch()` is also the
  copilot's entry point, so the sheet is created in `startComputation()` and
  `dispatch()` only closes whatever it finds. An agent-driven compute must not
  put a modal sheet in the user's way. It is closed *after* the staleness check,
  so a superseded result cannot close the sheet of the request that replaced it.

It is armed with `setValue(0)`, **not** `show()`. `show()` puts the sheet up at
once, which defeats `minimumDuration` and flashes a sheet at every fast compute
(measured: visible after 50 ms). `setValue()` arms QProgressDialog's own delayed
timer, and because that timer belongs to the dialog, closing it cancels the
pending show. A hand-rolled `QTimer::singleShot(400, …, show)` does not: measured
in an offscreen Qt harness, it re-showed a sheet the completion handler had
already closed.

---

## Compute admission control (429s, and why they should be rare)

Two independent limits gate scientific compute:

| Limit | Scope | Effect |
|-------|-------|--------|
| `VISIVO_MAX_CONCURRENT_TASKS` (3) | per **session** | `Session.try_acquire_task_slot()`; HTTP 429 when exhausted |
| `VISIVO_HEAVY_SLOTS` (`workers − 1`) | per **process** | `_run_heavy()`'s semaphore; queues rather than refusing, leaving a pool slot for interactive requests |

The interaction between them is the part that bites: `_run_heavy_with_limit()`
takes the **session slot first** and then queues on the heavy semaphore, so a
task that is merely *waiting for a worker* still occupies a slot. On a large cube
— where opening the dataset already launches the full-resolution load and the
whole-cube statistics — three such waiters exhausted the session and one click on
Isosurface came back as "Too many concurrent compute tasks".

Two changes make that path behave like backpressure instead of a failure:

- **The server waits before refusing.** `acquire_task_slot()` polls for up to
  `VISIVO_TASK_SLOT_WAIT_S` (default 15 s, `0` restores fail-fast) — admission
  control is not a quota to enforce to the millisecond — and the 429 detail then
  says how many slots there are and how long it waited.
- **The client treats 429 as "come back".** `BackendClient::performPostRetryingBackpressure()`
  retries with exponential backoff (5 attempts: sleeps of 0.5 / 1 / 2 / 4 s,
  ~7.5 s in total) and only then reports, replacing the server's JSON body with a sentence a user can act on.
  It is used for the heavy endpoints — isosurface, moment, noise, line-width,
  stacking — and, because the wait is a plain sleep, only from the worker threads
  those requests already run on.

Not addressed on purpose: hoisting the heavy-semaphore acquisition out of
`_run_heavy()` so a queued task holds no session slot at all. That would be the
structural fix, but the release logic there is deliberately tied to the task's
real completion (a timed-out ProcessPool job keeps its slot until it actually
ends, so admission control does not under-count), and rearranging it for a
symptom the wait + retry already covers is not worth the risk.

---

## Opening a file from the desktop

A double-clicked FITS file reaches the app in two entirely different ways, which
is why there is a relay rather than a line in `main()`:

- **macOS** sends an Apple Event; Qt turns it into `QEvent::FileOpen` on the
  application object. It is **not** in `argv`, and it can arrive within
  milliseconds of `QApplication` being constructed — long before `MainWindow`
  exists, because `StartupDialog` runs the backend + auth sequence first.
- **Linux / Windows** pass the path in `argv` (`Exec=… %F`, or the registry
  association).

`DesktopOpenRelay` (in `src/main.cpp`) is installed as an application event
filter immediately after `QApplication`, queues both sources, and flushes into
`MainWindow::openDatasetPath()` once the window exists — then keeps forwarding.
Three properties of the flush are load-bearing:

- **Queued, not direct.** It can run from inside the event filter, and opening a
  dataset shows dialogs and spins a nested event loop.
- **One at a time** (`m_busy`, and the next flush from the completion of the
  previous). `doOpenDataPath()` makes synchronous backend calls that spin nested
  loops, so two independent queued opens interleave: the second one's dialog
  suspends the first one's health request until its own timeout fires, and a
  reachable backend is reported unavailable.
- **Never on top of a modal dialog** (`QApplication::activeModalWidget()` → retry
  in 250 ms), which also covers a file arriving during the startup sequence.

Because argv is how Linux and Windows deliver a double-click, a second *launch*
carrying files is not a second app: `forwardToRunningInstance()` sends the paths
over a per-account `QLocalServer` socket and exits, so the file opens in the
window the user is looking at instead of booting a second instance with its own
backend session. A launch with no files always starts its own instance — two
VisIVOs against two backends stays possible.

The handover is small but every part of it is there for a reason (all found in
review):

- **A QDataStream-serialised `QStringList`, not newline-separated text.** A Unix
  filename may contain a newline or end in a space, and `readyRead` delivers
  arbitrary fragments — half a path, or half a UTF-8 character. The receiver
  reads inside a stream transaction, so a partial frame stays in the buffer.
- **Two phases, and both acknowledgements matter.** The primary writes `RDY` on
  connection and the sender writes nothing before seeing it — so a primary that
  is not reading receives nothing at all, and the sender's local fallback cannot
  duplicate a late delivery. After committing the frame the primary writes
  `DONE`, and the sender waits for *that* before exiting: `RDY` plus a drained
  write buffer only proves the bytes were transported, and a primary dying in
  between would have lost the files silently. On a `DONE` timeout the sender
  opens them itself — a file opening twice is visible and recoverable, a file
  that silently never opens is not.
- **A `QLockFile` in `TempLocation`, not the socket, decides who is primary.**
  Only the lock holder removes a leftover socket and listens, which closes both
  the probe-and-replace race between two simultaneous launches and the Windows
  case where several `QLocalServer`s may listen on one pipe name (so `listen()`
  succeeding proves nothing about ownership). The relay's destructor closes the
  server *before* releasing the lock — members die before `~QObject` deletes its
  children, so the reverse order left a window in which a new primary could take
  the lock and then have its socket unlinked by the old one.
- **A secondary keeps an eye on the seat.** A bare second instance polls the lock
  every 5 s and takes over when the primary exits, so the surviving window
  receives the next double-click instead of a third instance being started.
- **The name is a SHA-256 digest of the home path.** Not `$USER` (unset in a
  Finder/launchd-started macOS app, and `$USERNAME` is not unique across Windows
  accounts) and not `qHash` (its algorithm is not stable across Qt versions);
  truncating the digest keeps the name inside the ~104-byte Unix-socket path
  limit.

Two loss paths are accepted rather than engineered away, and are worth knowing:
quitting at the startup dialog discards files queued for that instance (the user
asked not to run it), and a primary that dies after accepting a frame but before
opening it loses those files — the sender has already been told `DONE`.

The wire protocol was checked cross-process with a throwaway two-process harness:
the `RDY`/`DONE` handshake, a frame split into two fragments (one partial frame
held in the buffer, then committed), and paths containing an interior space, an
embedded newline and a trailing space, all round-tripping byte-identical.

Packaging carries the other half:

| File | What it declares |
|------|------------------|
| `deploy/macos/Info.plist.in` | `CFBundleDocumentTypes` + an imported `org.fits.image` UTI (extensions fits/fit/fts/fz, MIME image/fits). Set as `MACOSX_BUNDLE_INFO_PLIST`; Qt's generated template has nowhere to put this. `LSHandlerRank` is **Alternate** — VisIVO must not take FITS away from DS9 / CARTA. |
| `deploy/linux/visivo-visual-analytics.desktop` | `MimeType=image/fits;application/fits;` and `Exec=… %F` (paths, not URLs). |
| `deploy/linux/visivo-fits-mime.xml` | `image/fits` with globs and the `SIMPLE  =                    T` magic, for desktops whose shared-mime-info lacks it. |

`MainWindow::openDatasetPath()` is the only public entry point for this: it
validates that the path is a readable file (the backend would otherwise answer
with a generic failure), raises the window — the request comes from outside, so
the app may not be in front — and hands over to `doOpenDataPath()`. Note the
consequence of opening **by path through the backend**: a remote backend must be
able to see that path, and the Data Hub's drag-and-drop (which stages the file)
is the route that always works.

---

## Session Data tree (`SessionDataTree`) — selection focuses, it never mounts

The tree is the table of contents for the dataset's views (`view3d`, `slice2d`)
and its registered products. Its contract, worth stating because breaking it is
invisible until someone loses a pane:

| Gesture | Signal | Effect |
|---------|--------|--------|
| select (any button) | `productSelected(productId)` | Inspector ▸ Properties / Provenance follow. **Nothing is mounted.** |
| select (left button or keyboard) | `rowSelected(viewTag, productId)` | `focusSessionViewIfVisible()` makes the pane ALREADY showing that row active. Mounts nothing, moves nothing; a row on no pane is left alone |
| double-click | `rowActivated(viewTag, productId)` | `activateSessionView()` mounts the row (free pane → grow), or raises the product's own window when it has no pane renderer |
| right-click | `rowContextMenuRequested(viewTag, productId, pos)` | the viewer builds "Show in ▸ Pane N" (and "Bring window to front" for a window-only product) |
| pane-badge click | `paneBadgeActivated(pane)` | activates the pane that already shows the row |

`currentChanged` used to emit **both** `productSelected` and `rowActivated`, so
selection *was* activation. Two consequences: clicking a row to read its
provenance replaced whatever the active pane was showing, and — because Qt makes
the pressed row current on any button — a right-click mounted the product before
its own "Show in ▸" menu opened, which made that menu unreachable in practice.
Selection is now inspection plus focus; mounting stays explicit.

The focus half came later, from a user report that is worth recording because it
shows why "passive" was too passive. The left dock's DISPLAY section is titled
after, and acts on, the **active pane**. Selecting *2D Slice* in the tree while
the 3-D pane stayed active left the header reading `DISPLAY - 3D VIEW`, so the
colour map changed there went to the volume — the tree said one thing and the
dock did another. `rowSelected` closes that gap with the weakest action that
does: follow the pane that already shows the row.

Two guards keep it from becoming the old behaviour again:

- `m_selectingProgrammatically` (RAII, `QScopedValueRollback`) marks every
  selection the CODE makes — `selectProduct()`, the pane→tree reverse sync
  `selectForView()`, the dataset root, and `removeRow()`. The last one is not
  theoretical: removing the current row makes a neighbour current, and without
  the guard deleting a product moved the focus to whatever took its place.
- `focusSessionViewIfVisible()` prefers an already-active matching pane before
  searching, so a product duplicated into panes 1 and 3 does not drag the focus
  from 3 to 1.

The **right-click is not an exception**, and the first attempt at this made it
one — reasoning from the warning below that a right-click must leave the panes
alone. It must leave them *unmounted*; it cannot leave them unfocused. Qt makes
the pressed row current whatever the button, that highlight outlives the menu,
and a highlighted row whose pane is not the active one is precisely the split
`rowSelected` exists to close. The old damage came from selection MOUNTING,
which acted before "Show in ▸" could open; that menu is built per visible pane
and never reads `m_activePane`, so focus cannot disturb it.

What it deliberately does NOT do: mount, grow the layout, or run the
explicit-activation side effects (`restoreProductOrigin`,
`revealParametersTab`). Selecting a row that is on no pane leaves the panes as
they are — the alternative is evicting what you are looking at because you
clicked a row to read its parameters, which is the mistake this whole section
is about.

Two things follow from the change and must not be re-broken:

- `onActivated()` handles the intrinsic **view** rows as well as products. They
  used to arrive in a pane only through the selection side effect, so
  double-click did nothing for them.
- The `m_suppressActivate` flag existed solely to stop a *programmatic*
  selection from mounting a pane. Making selection passive retired it; adding
  the focus half brought the same idea back as
  `m_selectingProgrammatically`, for the same reason. If anything is ever put
  back on `currentChanged`, it has to respect both that flag and the
  right-click one.

### Menus across the application

An audit of every window's menubar (static: the .ui trees plus every runtime
`addAction`, cross-referenced against the `connect`s) found **no dead entries**
anywhere — every action has a handler, a default-action button or a state read.
It did find these:

- **The Data Hub opened the same things twice.** File held *Open File...*, *Open
  VBT...* and *Open 3D Catalogue (CSV)...*; Data held *Open Remote Dataset…*,
  *Open VBT…* and *Open 3D Catalogue (CSV)…* — the same three handlers under two
  sets of labels, so nothing told the user that "Open File" and "Open Remote
  Dataset" were one action. File now keeps a single **Open Dataset…** (the
  conventional home) plus Exit; Data keeps what File has no convention for (3-D
  catalogue, VBT, velocity field, HiPS).
- **The file types were the user's problem.** What remained after that tidying
  was still three open actions — dataset, 3-D catalogue, VBT — and two of them
  accepted `.fits`. So the first thing the application asked was a question the
  user often could not answer: *is the FITS in your hand an image, a cube, or a
  table?* The split was a picture of the backend's three open routes
  (`/v1/datasets/open`, `/v1/catalogue/open`, `/v1/vbt/open`), not of anyone's
  work. The Data Hub now leads with a single **Open…**: the file is chosen
  first, `POST /v1/files/classify` says what it is from its own bytes (magic
  number, extension, at most one FITS header — nothing is opened or converted),
  and the matching viewer opens. The user is asked only when the *content* is
  ambiguous — a FITS table is a source catalogue or an overlay, and nothing in
  the file distinguishes them — and told, rather than asked, when nothing here
  opens the file (a `.fits.gz`: no open route accepts a compressed file, so
  offering the three viewers would only move the failure one dialog later). The
  distinction the old menu buried — a *file* versus a *service* — is now the one
  the panel makes: **Open…**, then an **Archives** group for HiPS and VLKB.
  `openAnything()` / `dispatchOpenForPath()` in `MainWindow_Open.cpp` hold the
  dispatch; the named catalogue and VBT entries stay in the Data menu as the
  escape hatch when the user would rather be explicit.
- **"Last 5 jobs" was always empty.** The Data Hub's activity panel renders
  `recent_tasks` from `/v1/health`, which comes from the task registry — and
  only two endpoints in the whole backend create a task (`/v1/tasks/moment`,
  `/v1/tasks/pv`). Everything else is synchronous and registered nothing, so a
  panel promising "run a tool to see history" showed nothing no matter what the
  user ran. Teaching thirty routers to register a task would have been thirty
  chances to forget, so the history is taken where every request already
  passes: an HTTP middleware in `app/main.py` records each compute request when
  its *response finishes* — the real duration, including a streamed body — into
  a bounded deque that `TaskRegistry.snapshot()` merges with the real tasks. Two
  details make it honest rather than merely populated: most routes report
  failure as HTTP 200 with a `{"valid": false}` envelope, so the middleware
  reads the head of a JSON body rather than trusting the status code; and what
  the user calls a job is work they asked for and waited on, so browsing, image
  tiles, session bookkeeping, channel scrubbing and job polling are excluded —
  one slice per mouse move would otherwise erase the five jobs that mattered.
- **The last five legacy features, and what they became.** *Filter FITS*,
  the header modifier, hips2fits, cone search and the VLKB query composer were
  the remaining gaps against ViaLactea Visual Analytics 1.7.4. Three of them ran
  outside the application in the legacy — cone search and the VLKB composer
  shelled out to bundled Python scripts through whichever interpreter
  *setting.ini* named, so both stopped working whenever that path went stale —
  and all five are backend routes now (`app/compute/imutils.py`,
  `app/fits_header_edit.py`, `app/hips2fits.py`, `app/cone_search.py`,
  `app/vlkb_tap.py`). That is not tidiness: it is what makes them testable
  without a display, safe to point at a user-supplied URL (one SSRF guard, not
  three), and correctable without releasing a client — the legacy compiled the
  HiPS survey list and the VLKB schema into two dialogs.

  Porting them turned up four defects worth naming, because each is the kind
  that produces a plausible wrong answer rather than an error:
  the smoothing filter spread every blank pixel over its kernel footprint and
  left `BMAJ` describing a resolution the map no longer had; the regrid averaged
  `Jy/pixel` blocks, which loses flux by the square of the factor; the header
  modifier reported success whether or not the values typed produced a readable
  WCS; and the bubble mode — a button, a rubber-band selector and an importer —
  had no query behind it at all, so it ran the compact-source query and drew
  sources. The first three are fixed in the arithmetic (and pinned by tests);
  the fourth is now a query.
- **The image viewer had the cube's old disease.** Its Tools menu was one flat
  run of **34** entries while Inspector ▸ Analysis listed **11**, hand-picked —
  so two thirds of the window's tools existed in one surface only. It now has an
  `imageTools()` table exactly like `cubeTools()`, feeding both surfaces, and
  `regroupImageToolsMenu()` groups the menu into Spectral / Regions / Catalogue /
  Contours / Measure / Statistics / Annotations / Polarisation / Products /
  Export. Six actions were locals and had to be promoted to members to be
  listed at all — being a local is *why* they existed only in the menu.
- **The Data Hub's View had one entry** (Diagnostics) while the cube and the 3-D
  catalogue list Diagnostics *and* the Command Palette. The palette was there,
  as a bare `Ctrl+K` window shortcut with no menu entry: a shortcut nobody can
  see is a feature only its author knows about. It is in View now.
- **`RemoteMomentWindow` is unreachable** — nothing instantiates it. Its menus
  are therefore not part of the application's surface; left in place, noted here.

```{admonition} Moment 2 was labelled as its own opposite
:class: warning
The moment dialog offered "Moment 2: Variance – NOT velocity dispersion" and the
provenance string said the same. The backend funnels every order-2 map through
one function that returns **σ = sqrt(variance)** by default (matching CASA
`immoments`, CARTA and SoFiA), and the client never sets the `moment2_variance`
flag that would keep the raw variance — so the label said the opposite of what
the user was handed, and contradicted the copilot's own description of M2. The
label was accurate when it was written and went stale when the backend changed
(R-03); both call sites now read "Velocity dispersion σ".
```

### The cube window's menus, and what each one answers

Four menus had grown by accretion: entries were added where the code that
created them happened to run, so their order was construction order and their
membership was history. Each now answers one question, and `regroupViewMenu()`
imposes the order the same way `regroupToolsMenu()` already did for Tools
(rebuild, never `QMenu::clear()` — see there for why).

| Menu | Answers | Notes |
|---|---|---|
| **Camera** | where am I looking from | The six viewpoints keep their existing icons and gain `Ctrl+1..6` (not bare digits: the window is full of spin boxes and a channel slider). *Use Camera ROI* moved here from the bottom of View — it is about what the camera frames. |
| **Edit** | what can I take back | It held exactly one entry, *Edit LUT*: a menu that existed to hold a menu item. It now leads with **Undo Copilot Display Changes** (⌘Z), which was reversible all along but reachable only from the assistant panel, and is enabled only while a snapshot exists. *Edit LUT* is *Edit Color Map…*, matching the "Color map" control it opens. |
| **View** | what is on screen and how is it drawn | Was 27 entries in no order. Now four groups, separated: **Workspace** (both dock toggles, Focus mode, Reset Layout), **Display** (3-D rendering + blend, cutting plane + opacity, 2-D panel, the two sky overlays), **Animation** (play, speed, mode), **Data** (Load/Crop Full Resolution, absent on a small cube), then the developer entries (Diagnostics, Command Palette) last. |
| **WCS** | which sky coordinates, in what format | Frame (Galactic / FK5 / ecliptic) and format (sexagesimal / decimal), both exclusive. Meaningless without celestial axes, so the menu disables — and because a disabled menu-BAR entry shows no tooltip, the reason goes in its title: "WCS (no celestial axes)". |

Two entries were moved out of View rather than reordered. *Pick Spectrum on
Plane Click* is a **tool** — it was in View and in the INTERACTION dock but in
neither of the two places a user browses tools, so it is in `cubeTools()` under
Spectral now, where the gate can say whether the 3-D view it needs is even on
screen. And *Show WCS Axes* / *Show 3D WCS Axes* are different overlays in
different views; adjacent in the new Display group they read as one entry
duplicated, so they are **Sky Grid on the 2-D Panes** and **WCS Box in the 3-D
View**, both disabled with a reason when the cube has no celestial axes.

View offered *Inspector Panel* — the right-hand dock — and said nothing about
the left-hand **Session Data Panel**, although `WorkspaceChrome` could always
collapse either (user report). A window with two docks and a menu entry for one
of them makes the other look like furniture. They are a pair now, both tracking
the chrome's state so Focus mode moves their check marks too.

Menu action tooltips are now visible on every menu in this window, not just
Tools. That was why the reasons written next to the Camera and Edit actions had
never appeared.

### Six numbers are not a picture

Export Sub-Cube and Mask 3-D Region are configured by six spin boxes and, until
now, nothing showed what those numbers select. The amber overlay that Estimate
Noise draws for its sampling region — a rectangle on the 2-D slice and a
translucent box inside the volume — was already there, wired to one tool. Both
box tools now draw it **as the user types** (`valueChanged`, not
`editingFinished`: the point is to watch the box move while deciding), through
`CubeBoxToolDialog::onBoundsChanged`.

That makes three non-modal windows sharing one overlay, so it needed an owner:
`claimRegionPreview()` records who last drew it and `clearRegionPreviewIfOwner()`
clears it only on that owner's behalf. Without it, closing any one of the three
wiped the box another was still showing.

### A disabled tool has to say why

The gate writes a reason for every tool it turns off ("open a 2D image viewer
first - the slice overlays there as contours"), and puts it in the action's
tooltip. That worked in the Tools menu and nowhere else: **Qt does not deliver
`QEvent::ToolTip` to a disabled widget**, so in Inspector ▸ Analysis — the list
people actually browse — a greyed entry was mute, and its being greyed read as a
bug rather than a precondition (user report: "why isn't Send Slice to Image
Viewer clickable?").

`InspectorPanel::addTool` now wraps each tool button in a container that stays
enabled and mirrors the action's tooltip, kept in step through
`QAction::changed`. Mouse events over a disabled child go to its parent, so the
reason appears on hover whether the tool is available or not. Any new surface
that greys a tool out inherits the same obligation.

That report had a second half worth separating: the tool was also **misnamed**.
"Send Slice to Image Viewer" reads as "open this slice in an image viewer", so
needing a viewer already open looked like the bug. It is "Overlay Slice on an
Open Image" now, and its tooltip says outright that it does not open a viewer of
its own. A precondition the name contradicts will keep being reported as a
defect however well the gate explains it.

### The image viewer's left panel

An audit of the panel (user question: "does this layout work?") found that more
than a third of its height belonged to two `QTableWidget`s, **Inventory** and
**Objects**, which were defined in the `.ui`, referenced by exactly one styling
loop, and **never populated by any code**. Each declared `minimumHeight 120` and
`verstretch 1`, so together they claimed 240 px plus all the spare space — which
is why the panel reads as mostly empty and Session Data is squeezed into a strip
at the top.

The job they were presumably meant for is **already done elsewhere**:
`ensureCatalogueDock()` builds a real source table with a `CatalogueTableModel`
and two-way selection sync (click a row, the source highlights on the image, and
vice versa), and shows itself when a catalogue loads. So both tables were
removed rather than wired — a second list would have competed with the working
one.

What the dock did lack was a way back: it shows itself when the overlay loads and
hides when it is cleared, but nothing could reopen it once closed, so the source
list was gone for the rest of the session. Its `toggleViewAction` is in the image
viewer's View menu now, as "Catalogue Sources" — the 3-D catalogue window already
listed its own dock that way.

Two smaller things in the same panel:

- **"LAYER SETTINGS" now names the layer it edits.** The header sat above the
  list whose current row it acts on, which reads as settings for the panel. With
  one layer that is harmless; this window is built for many (I/Q/U/V plus P, PA,
  P/I, α, RM), and there you could change a colour map without being told which
  layer you changed.
- **Sources / Filaments are gated on celestial WCS.** They query the Hi-GAL /
  VLKB catalogues over the image footprint and used to be enabled always,
  explaining the precondition only *after* the click, in a warning box. Same rule
  as the cube's tool gate: say why before, not after.

### The image viewer's Session Data

The 2-D viewer installed the session dock and minted an owner scope, and then
registered nothing at all: its tree listed the dataset and stayed empty for the
rest of the session, however many maps, statistics and figures its tools
produced. It has the cube's contract now, with one difference forced by the
window itself — it shows a single view, so a derived MAP is a *layer*, not a
pane. That gives three kinds of row (`vtkWindowImage_Products.cpp`):

| Kind | Produced by | Opening the row |
|---|---|---|
| **layer** | the polarisation family: P, PA, P/I, debiased P, spectral index, Faraday RM | selects that layer |
| **file** | Publication Figure, Export to Workspace as FITS | opens the file; the menu copies its path |
| **measurement** | region statistics, Image Quality | replays the result window from its record |

The measurements go through the same record mechanism the cube's noise estimate
uses, so the reopened window *is* the original rather than a reconstruction of
it. Region analysis in particular used to build its sections straight into a
`WA_DeleteOnClose` dialog: closing it meant drawing the region again.

Publication Figure and Image Quality are computed by `MainWindow` (they were
Data-Hub tools until the menu work), so they now take a completion callback and
hand their result back to the window that asked, which records it. The host does
the work; the product belongs to the viewer.

```{admonition} Export to Workspace as FITS copies the SOURCE
:class: note
It calls `copy_dataset` with the window's master `remoteDatasetId`, so it exports
the image the window was opened on — **not** the derived layers computed in it.
The product's Provenance says so, because the tool's name does not.
```

### Where a tool lives: scope, not subject matter

A tool belongs to whatever knows *which data it acts on*. That gives exactly two
homes, and the deciding question is how many datasets the operation takes:

| The operation takes | Lives in | Because |
|---|---|---|
| the data in one view | that viewer's Tools menu + Inspector ▸ Analysis | the tool gate already knows what is on screen and whether it is visible |
| several of the session's datasets, producing a new one | the Data Hub's **Combine** menu | there is no single owning window; the Data Hub is the session surface |

The Data Hub used to carry a **Science** menu with nine entries, and it failed
both tests. Three of them (Line-Width Map, Baseline Subtraction, Stack Cubes to
a Spectrum) duplicated cube-viewer tools. Source Finding (SoFiA-2) was a cube
tool reachable *only* from there. Publication Figure and Image Quality take a
single 2-D map. All six resolved their target through `m_sciDatasetId` — "the
last dataset opened", a sticky global with no relation to the focused window —
so with two cubes open they acted on the wrong one, and the three entries that
were clickable before any dataset existed could only answer "Open a dataset
first" (user report). They now live in the viewers, and each viewer emits a
request that the host answers with **that window's** backend context, captured
when the window was created (the pattern `sourceFinderRequested` already used
for the copilot's `show_sources`).

What is left on the Data Hub is Pixel Math / Spectral Index and Mosaic, which
genuinely combine datasets. "Science" was the wrong name for them too:
everything in this application is science, and what those two share is scope, so
the menu is called **Combine**. Its entries are enabled from what the session
holds, not from a current dataset (`refreshCombineMenuState`).

A corollary worth keeping: the command palette lives on the Data Hub, so it
lists Combine's entries only. Triggering a per-view tool from there would mean
triggering it against no particular window, which is the problem the move fixed.

### What a tool must leave behind

Every tool that produces a *result the user could want later* registers a
product. `renderer` decides how the row behaves, and the three cases are all
legitimate:

| `renderer` | Row means | Tools |
|---|---|---|
| `moment2d` / `image2d` / `spectrum1d` / `pvdiagram` | mountable in a pane | Moment, Line-Width, PV, Extract/Pin Spectrum, region spectra, Stack |
| *(empty)* | a **measurement**: the numbers live in Provenance, and the row re-opens them as a result window | Estimate Noise, region 2-D statistics |
| *(empty, with `window`)* | its own window, raised on activation | Channel Maps |
| *(empty, with an `Output path` param)* | a file, reopenable via the row menu | Baseline-subtracted cube |

The rule exists because a tool whose only output is a `WA_DeleteOnClose` result
dialog silently loses it: Estimate Noise reported σ — the number that then sets
the moment threshold, the isosurface level and the mask cut-off — in a floating
panel and nowhere else, so closing the panel meant re-running the tool. The
region tools had already been moved off that pattern (their statistics are
folded into the product's Provenance) and noise now follows, keyed by
region+channel range so a re-run of the same selection updates its row while a
different selection adds one.

**Every row must do something when you open it.** A measurement has no file and
no pane, so `reopenProductFromFile` puts its result window back on screen.
Without it the noise row was inert: selecting it filled Provenance, but
double-clicking did nothing and the popup the user had closed was gone for good
(user report). The row menu says so explicitly ("Show the Numbers Again"),
because an action reachable only by double-click is an action nobody finds.

**Reopening restores everything the run put up, not just the main output.** A
region analysis on a cube produces a spectrum *and* its statistics: the spectrum
mounts in a pane, the statistics go to Provenance, and registration reveals that
tab (§4.1.4). Reopening the row only re-mounted the pane, so the numbers were
simply missing the second time (user report). Row **activation** now reveals
Provenance too, whenever the product carries parameters — matching what
registration does. Only on activation: passive selection must not yank the
Inspector off the Analysis list.

The same audit found two products registering **no parameters at all**, which
makes two runs of the same tool indistinguishable in the tree:

- **PV diagram** recorded neither its path nor its width. It now carries both,
  plus a `pvpath` origin, so opening the row redraws the cut on the slice. A PV
  path is its own origin kind rather than a "polygon": it is open and has a
  width, and restoring one as a region would close it into a shape that was
  never drawn.
- **Channel maps** recorded nothing. It now carries the channel range, stride,
  column count, colour map and whether the scale is shared, and its label names
  the range so two runs read apart in the tree.

**A product says what it measured; it must also say where.** "Box region stats,
mean 1204.7" does not tell you which box, and a pinned spectrum does not tell you
which line of sight — with three regions drawn and two spectra pinned, the rows
become indistinguishable within minutes (user report). So a product carries its
`origin` alongside its numbers: the drawn region (shape, anchor, current vertex
or polygon points, annulus inner radius) or the probed voxel, encoded by
`ProductOrigin.h`.

Opening the row **redraws it**. In the image viewer that means the region overlay
comes back; in the cube it also re-places the probe crosshair and goes to the
channel the spectrum was taken at, so the plot and the slice agree about the
pixel. It runs on every row activation, before whatever else the row does — a
pane-mounted spectrum restores its region *and* mounts, a measurement restores
its region *and* replays its numbers — and is a no-op for products with no
geometry (a moment map is computed over the whole field; there is nothing to
draw). The human phrasing of the same thing goes into Provenance as **Measured
on**, so the row reads even without clicking.

Like the result record, the encoding lives in a pure header with tests over it
(`tests/test_product_origin.cpp`). It carries the shape as a STRING rather than
either window's `RegionMode` value, so a product registered by one build still
restores in another if that enum ever gains a member, and a malformed record
restores **nothing** rather than half a geometry — an overlay that silently lands
at (0, 0) would be worse than no overlay at all.

**The grid it was drawn on travels with it.** Region geometry is stored in
*displayed-voxel* indices, and in the image viewer that index space is coarser
before the preview → full-resolution swap (spacing 2 on a decimated LOD). Replaying
raw indices across a swap put the overlay at half position and half size — the
numbers right, the box somewhere else, which is the LOD-coordinate bug class this
code has hit before. So `withVoxelFrame()` stamps the mapping
`world = pos + voxel · scale` at record time (`vtkWindowImage::currentVoxelFrame()`,
the single place that mapping is written, shared with `refreshRegionOverlay()`),
and `remapOriginVoxel()` converts into whatever grid is displayed at restore —
including the annulus inner radius, which is a length in the same grid. Products
recorded before the stamp, and grids that did not change, come back untouched.
The cube viewer needs no stamp: every 2-D image it shows (slice, moment map) is
served at unit spacing with origin 0.

`m_productLayer` maps a product to the layer it created, but a row index is not
an identity — the layer list is drag-reorderable — so the layer's **name** is
recorded too and resolved first, with the row as fallback.

**In the cube, geometry is not enough either.** The same pixels mean different
things on channel 10, on channel 50 and on a moment map, and the window shows one
of them at a time. So a cube region also stamps the *surface* it was measured on
(`withMeasuredSurface`): `"slice"` plus the channel, or `"moment"` plus the
moment product's id. Restoring navigates back to that channel (stopping the scrub
debounce first, or a pending scrub would move the data out from under the overlay
we just restored) and, when the recorded surface is not the one displayed at all,
says so in the same status line rather than in a message the caller immediately
overwrites. The channel comes from `m_displayedSliceIndex`, set where a slice is
actually applied — the spin box also shows channels that are still being fetched,
so measuring during a fetch would have stamped the channel the user asked for
instead of the one the numbers came from.

That navigation exposed an older fault in the fetch helper: asking again for a
channel whose fetch was already in flight deduped without adopting it, so neither
result was accepted (the new request's id was not current, the running one's
index no longer matched). `remoteSliceFetchesInFlight` now maps the cache key to
the **watcher**, and the second ask *promotes* that fetch: a fresh id (ids come
from their own monotonic counter, since `currentRemoteSliceRequestId` names the
wanted result and can move backwards), the new caller's linked-broadcast origin,
and `isPrefetch` cleared — a prefetch's result is cached but never displayed and
never reports its errors, so adopting one without promoting it would have shown
nothing. A slice error now also survives an unrelated stale result finishing
after it (`m_sliceErrorOnStatusBar`, cleared wherever the message itself is).

The moment generation is per product (`m_momentProductGeneration`), not global:
switching between two cached moment maps must not mark either as recomputed, and
a measurement whose map is no longer loaded says exactly that.

A region that yields valid statistics but **no spectrum** — on a moment map, or
outside the loaded block — used to exist only in a status line that scrolled away.
It now registers a `region` stats product like the image viewer's, with no result
window, so its Session Data row replays the numbers instead of raising the cube.

**Reopening replays a record; it does not re-derive a layout.** The first cut
rebuilt the window from the product's provenance parameters, and the result only
resembled the original: one flat list where there had been a *Sigma* and a *MAD*
section, keys reading "σ mean" instead of "Mean", rows in alphabetical order
(max, mean, median, min), and the product's label in place of the tool's title
(user report, with both windows side by side). Two renderings of the same numbers
will always drift.

So a result window is described ONCE, as a plain `QVariantMap`
(`{title, subtitle, sections:[{title, rows}]}`), and that single description is
both shown (`ToolResultDialog::applyRecord`) and stored on the product
(`AnalysisProduct::resultView`). Reopening replays it through the same
method, so the two windows cannot differ. `params` stays flat and unchanged for
the Provenance panel, which renders key/value pairs and would choke on a nested
structure. The generic params-derived layout survives only as a fallback, for a
product that never showed a result window.

The shape itself lives in `ToolResultRecord.h` — pure QtCore, no widget — with
`tests/test_tool_result_record.cpp` over it, because it is the part that can be
wrong **without failing to compile**, and it was:

```{admonition} QList::append(const QList<T> &) concatenates
:class: warning
The encoder appended each row as a bare `QVariantList`, so
`rows.append(QVariantList{label, value})` chose the CONCATENATING overload and
flattened every row into two loose strings. The decoder then called `toList()`
on a string, got nothing back, and built each section with no rows. The window
came up with a title, a subtitle and an **empty body** (user report, with a
screenshot of the Region Analysis window). It compiled, it ran, and nothing
anywhere said so. The fix is one explicit `QVariant(...)`; the test asserts the
ENCODED structure directly rather than only the round trip, since a round trip
through a matching bug on both sides would pass.
```

The alternative — keeping measurements OUT of Session Data because they render
nothing — is the option this design rejects. σ is the most reused number in the
session (moment thresholds, isosurface levels, mask cut-offs); dropping the row
would put it back where it started, alive only inside a window the user is about
to close.

The **exports** are the deliberate exception: Export Sub-Cube / Channel 2-D /
Moment FITS / Mask 3-D Region write real files and belong to *Workspace
Exports*, which is durable and already lists them, so they do not also appear
as products.

---

## Cube viewer — tool availability and where results land

Two rules govern the ~30 cube tools. Both were per-pane heuristics scattered
through `vtkWindowCube`; both are now single, stated mechanisms.

### `CubeToolGate` — a tool binds to a VIEW, not to the focused pane

`src/gui/CubeToolGate.h` is a pure, Qt-free decision table
(`evaluate(Inputs) -> Decision`), unit-tested in `tests/test_cube_tool_gate.cpp`,
applied by `vtkWindowCube::applyMomentToolGate()` to the `QAction`s that the
Tools menu and Inspector ▸ Analysis are both built from:

- **view gates** — the seven slice tools need `Slice2D` mounted in a visible
  pane, the two 3-D tools need `View3D`. Gating on the *active* pane instead
  (the first implementation) forced the user to keep switching: select the
  spectrum pane to reach its controls and the slice tools greyed out.
- **state gates** — Pin Spectrum needs a probed spectrum (`m_lastProbeValues`,
  written by `updateProbePlot()`, which both the 2-D probe and the 3-D pick
  feed), Export Region a region, Overlay Slice on an Open Image an open
  `vtkWindowImage`, Link Views a link target (>1 pane or >1 cube window).
- a disabled action always carries the **reason** as its tooltip/status tip, and
  `m_toolBaseTooltips` restores its own text when the gate lifts.

Consequences encoded in the code (each was a real defect):

- a **reason** un-arms a checkable tool, a **baseline** denial only greys it out
  — PV drops its baseline the moment an extraction starts, and un-arming there
  ran `clearPv()` under the controller;
- the gate un-arms actions whose `toggled()` handlers mutate region/probe state
  and call back, so `m_inToolGate` guards re-entry and the **state** inputs are
  re-read *after* the un-arming pass;
- a maximize hides pane frames without unassigning views: keeping an **already
  armed** tool alive uses `viewAssigned()`, while arming a new one still
  requires real visibility;
- every tool `toggled` re-runs the gate.

Input routing follows the same rule: `paneIsArmedTarget(slot)` makes the armed
pane keep mouse events (and its crosshair) even when it is not
`m_activePane` — honoured in `eventFilter`'s idle-pane swallow,
`refreshPaneInteractors()` and `refreshPaneCursors()` — and
`refreshPaneActiveStyles()` paints its amber border and arms the <kbd>Esc</kbd>
shortcut (`disarmArmedTools()` first, maximize-restore second).

### `acquirePaneForProduct()` — free, then grow, then borrow

Placing a result "if a pane happens to be free" meant that in a full 2×2 layout
a tool answered with a Session Data row and a status line, which reads as the
tool doing nothing. One helper now owns placement:

1. a **free** visible pane (skipping the active one for a live preview, whose
   active pane is the slice being hovered — `avoidActivePane`);
2. else **grow** the layout `1 → 2 → 4` (`assignViews()` auto-fills only the
   cube's intrinsic views, so a grown pane normally comes up free);
3. else, for a **transient** result only (`mayBorrow`), borrow the least
   relevant pane — never `Slice2D`, never the active one, never one already
   lent — preferring a duplicate of a view that stays on screen, then a derived
   product, then a primary view. `m_borrowedPanes` records the displaced
   `PaneView` **keyed by the borrower's product id** (a 4→1 collapse swaps views
   between slots, so a slot key goes stale), and `releaseBorrowedPane()` hands it
   back when the tool ends.

Permanent products (pinned/kept spectra, region spectra) may grow but never
borrow. The live probe spectrum is the transient case: `startLiveSpectrum()`
borrows, `stopLiveSpectrum()` releases and — when the probe was **frozen** on a
deliberately clicked pixel and not already saved (`m_lastProbeSpectrumPinned`)
— promotes the curve to a permanent SPEC product before dropping the live one.

### Spectral-axis context for the line overlay

`rest_freq_hz` (header `RESTFRQ`/`RESTFREQ`) travels
`fits_dataset.geometry_metadata()` → `OpenDatasetResponse` →
`OpenDatasetResult::restFreqHz` → `vtkWindowCube::setRestFrequency()` →
`pushSpectralAxisContext()` → each `ProfileWidget`. Together with the axis
`CTYPE` and unit it is what lets `spectral::axisValueForFrequency()` place a
rest-frame line list on a velocity axis in the axis's own convention
(`SpectralUnitConvert.h`, tested).

---

## Cube viewer (`vtkWindowCube`) — interactive extras

In addition to the volume / isosurface / slice / moment / PV / noise pipelines,
the cube viewer exposes a set of configurable interaction extras driven from
the *View* menu and mirrored in the sidebar. The dedicated "Cube Extras" tab
was removed; its widgets were redistributed into the *3-D View Settings* and
*2-D View Settings* pages where they semantically belong. All toggles still
use a checkable `QAction` as the single source of truth — sidebar buttons
bind to it via `QToolButton::setDefaultAction()`, which gives Qt bidirectional
state sync for free (no manual `connect` pairs needed for the boolean toggles).

| Feature | QAction / group | Menu | Sidebar widget |
|---------|-----------------|------|----------------|
| Cutting plane visibility | `actionShowCuttingPlane` | View → Show Cutting Plane | *3-D View Settings* → **CUTTING PLANE** → "Show cutting plane" toggle button (full-width, `setDefaultAction`) |
| Cutting plane opacity (0–100%) | `cuttingPlaneOpacityGroup` (25 / 50 / 75 / 100 / 0% presets) | View → Cutting Plane Opacity → … | *3-D View Settings* → **CUTTING PLANE** → continuous slider with read-only QLineEdit value below (matches RENDERING THRESHOLD layout) |
| 3D WCS axes (`vtkCubeAxesActor`) | `actionShow3dWcsAxes` | View → WCS Box in the 3-D View | *3-D View Settings* → **3D REFERENCE** → "Show 3D WCS axes" toggle button |
| Slice animation | `actionPlaySlices` + `animationFpsGroup` + `animationModeGroup` | View → Play Slices / Animation Speed / Animation Mode | *2-D View Settings* → **SLICE ANIMATION** → "▶ Play" button + FPS combo (preset 2 / 5 / 10 / 15 / 30, **no free-form entry**) + Mode combo (Loop / Bounce / Stop at End) |
| 3D pick on plane click | `actionPickSpectrum3d` | View → Pick Spectrum on Plane Click | *3-D View Settings* → **3D INTERACTION** → "Pick spectrum on plane click" toggle button |
| Slice contours overlay | `ui->checkContours` (canonical state holder, hidden) | — | *2-D View Settings* → **CONTOURS** → "Show Contours" toggle button (drives the hidden checkbox; existing `checkStateChanged` handler stays untouched) + Level / Lower / Upper line edits |
| 2-D view mode | menu actions `actionSlice` / `actionMomentMap` | View → Slice / Moment Map | *2-D View Settings* → inline `SegmentedToggle` ("Slice | Moment Map") at the top of the page, same widget family as RENDERING MODE / VOLUME RENDERING |

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

### Beam indicator ellipse (2-D slice view)

The beam indicator is a filled ellipse rendered in the bottom-left corner
of the 2-D slice renderer. It visualises the synthesised beam reported by
the FITS header keywords `BMAJ`, `BMIN`, and `BPA`.

Implementation notes:

- **Backend**: `geometry_metadata()` in `backend/app/fits_dataset.py` reads
  `BMAJ`, `BMIN`, and `BPA` from the primary header and returns them (in
  degrees) as `beam_major`, `beam_minor`, `beam_pa`. The
  `OpenDatasetResponse` schema (`backend/app/schemas.py`) carries them as
  optional floats; the client receives them via `BackendOpenDatasetResult`
  fields `beamMajorDeg`, `beamMinorDeg`, `beamPaDeg`.
- **Frontend**: `vtkWindowCube::setBeamInfo()` builds an ellipse from
  parametric points (cos/sin sampled at ~64 steps), converts angular sizes
  to pixels via `|CDELT1|`, and creates a filled `vtkActor2D` with white
  colour and semi-transparent opacity. The actor is added to the 2-D slice
  renderer at a fixed position in the bottom-left corner (viewport-relative
  coordinates).
- If `BMAJ` or `BMIN` are absent (i.e. the optional fields are unset), the
  ellipse actor is not created or is set invisible — no fallback drawing
  occurs.

### Spectral smoothing (ProfileWidget)

The `ProfileWidget` spectrum header exposes a **Smooth:** `QComboBox` that
applies a 1-D convolution kernel to the displayed profile.

Implementation notes:

- Kernels available: None, Hanning `[0.25, 0.5, 0.25]`, Boxcar 3/5/7,
  Gaussian σ=1, Gaussian σ=2. The kernel array is applied via a
  `convolve1D` helper function.
- The convolution is **NaN-safe**: NaN samples are excluded from the
  weighted sum and the normalisation factor is adjusted to compensate, so
  NaN values do not propagate into neighbouring channels.
- The stats bar (N, Min, Max, Mean, RMS, ∫) is recomputed on the
  **smoothed** data vector, giving the user immediate quantitative feedback
  on the effect of the kernel.
- Smoothing is active during **live probe hover**: every
  `updateProbePlot()` call re-applies the selected kernel before plotting,
  so changing kernels while hovering is responsive.
- CSV export always writes the **raw** (unsmoothed) data to preserve
  scientific provenance.

### Line identification overlay (ProfileWidget)

**Load Lines…** and **Clear Lines** buttons in the `ProfileWidget` header
allow the user to overlay expected spectral-line positions.

Implementation notes:

- The CSV parser accepts comma- or tab-separated files with two columns
  (`frequency`, `label`). Lines beginning with `#` are skipped as
  comments. No header row is required.
- Each loaded line is rendered as a `QCPItemLine` (vertical, dashed, amber
  pen) spanning the full Y range of the plot. The label is rendered as a
  `QCPItemText` positioned at the line's X coordinate with a viewport-ratio
  Y coordinate (fixed fraction of the plot height, e.g. 0.85) so that
  labels stay readable regardless of zoom level. Labels are rotated 90°.
- **Clear Lines** removes all `QCPItemLine` + `QCPItemText` items that
  belong to the line-ID overlay set and replots.
- No unit conversion is performed: frequencies in the file must match the
  plot's current X-axis unit.

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

QActions referenced by the sidebar's redistributed *Extras* sections
(Cutting Plane / 3D Reference / 3D Interaction / Slice Animation) must be
created *before* `setupSidebar()` is called — otherwise the sidebar
dereferences null pointers and the window segfaults. In the constructor:

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

## Workspace Exports (persistent FITS artefacts)

Cube viewer "Export … as FITS" actions deposit their products in a
**persistent workspace directory** on the backend host, not in temp.
Files survive across sessions and are browsable / re-openable from the
client without the user having to remember a path.

**Config & layout**

- Directory: ``$VISIVO_EXPORTS_DIR`` env var, default
  ``~/.visivo/exports/``. Created on backend startup
  (``app.dependencies._EXPORTS_DIR``).
- Flat namespace — no per-session subdirs. Filename collisions are
  resolved by ``make_workspace_path()`` with a numeric suffix:
  ``cube.fits`` → ``cube_1.fits`` → ``cube_2.fits`` → …
- Empty / dot-only / traversal-prefixed basenames fall back to
  ``export.fits`` so the workspace can never be escaped.

**Backend producers**

| Endpoint | Writer | Produces |
|----------|--------|----------|
| `POST /v1/cube/save_subregion`    | `worker_cube_save_subregion` | Cropped 3-D FITS (WCS preserved, CRPIX shifted). Also registered as a new session dataset so the client can immediately open it without re-uploading. |
| `POST /v1/cube/save_channel_2d`   | `worker_cube_save_channel_2d` | A **single channel** of a cube written as a standalone 2-D FITS (NAXIS=2, spectral axis dropped via `WCS.celestial`). Header retains `BUNIT`, `OBJECT`, telescope keys and adds `SPECVAL`/`SPECTYPE`/`SPECUNIT` recording the source channel's spectral coordinate so downstream client tools (Stokes / spectral index / Faraday RM) can prompt the user with the right frequency. Result is registered as a new `image` dataset. |
| `POST /v1/datasets/open` *(extended)* | `_fits_metadata` → `is_dynamic_spectrum` → `convert_hdf5_to_sidecar_fits` / `convert_psrfits_to_sidecar_fits` | Promotes the response `kind` to `'dynspec'` for: (1) plain FITS whose active axes carry a time-like + frequency-like `CTYPE` pair, (2) HDF5 files (`.h5/.hdf5/.hdf/.bfdata`) — LOFAR `SUB_ARRAY_POINTING/BEAM/STOKES` hierarchy plus a generic 2-D-float fall-back, (3) **PSRFITS** files (any FITS with a `SUBINT` binary-table HDU). For (2) and (3) a sidecar FITS is generated next to the source (`<source>.dynspec.fits`) and the rest of the pipeline reads it like an ordinary 2-D FITS. PSRFITS sidecars pick Stokes I and phase-average folded data; HDF5 sidecars try LOFAR layout first. Sidecars are mtime-cached. |
| `POST /v1/dynspec/dedisperse` | `worker_dynspec_dedisperse` | Incoherent dedispersion at a user-supplied DM (pc cm⁻³). Computes Δt(ν) = 4148.808 · DM · (1/ν² − 1/ν_ref²) per channel; shifts each row by the nearest sample. Output is a 2-D dynspec FITS in the workspace; cards `DM` and `DMREFMHZ` record the parameters used. Registered as a new `dynspec` session dataset. |
| `POST /v1/dynspec/pulse_profile` | `worker_dynspec_pulse_profile` | Folds the frequency-averaged time series at a trial period into N bins (default 64). Optional in-memory dedispersion (`dm_pc_cm3`) and band selection (`freq_start_mhz`/`freq_end_mhz`). Returns phase, profile, counts arrays plus σ_MAD and peak SNR. No file written — payload is JSON. Client renders the profile in a floating QCustomPlot window. |
| `POST /v1/dynspec/rfi_mask` | `worker_dynspec_rfi_mask` | σ-clipped flagging mask: pixels deviating from each channel's (or each time sample's) robust median by more than `n_sigma × 1.4826 × MAD` are marked `1`, others `0`. NaNs are always flagged. Output is a uint8 FITS in the workspace with the source's WCS preserved so it overlays correctly. Registered as `dynspec` so the client can load it as a layer. |
| `POST /v1/products/moment_fits`   | `worker_moment_save_fits`    | 2-D moment FITS (celestial WCS + BUNIT). Not registered as a session dataset (it's an image, not a cube). |

Both accept an optional `output_basename`; empty defaults to a
synthesised name (e.g. ``<src>_subcube_x..y..z..fits``,
``<src>_m{order}.fits``).

**Lifecycle endpoints**

| Endpoint | Behaviour |
|----------|-----------|
| `GET    /v1/exports/list`     | Enumerate every regular file in the workspace, sorted newest-first. Returns `filename`, `size`, `modified_time` (ISO-8601 UTC), `absolute_path`. |
| `GET    /v1/exports/download?filename=…` | Stream the artefact back as `application/fits` for local Save As. **Client-side gotcha:** `+` characters in the filename must be percent-encoded as `%2B` before being put into the query string — Qt's `QUrlQuery::addQueryItem` leaves `+` raw (RFC 3986 permits it as a sub-delim), but FastAPI / Starlette decode the query string using the `application/x-www-form-urlencoded` convention where `+` means a space. `BackendClient::downloadExport()` bypasses `QUrlQuery` and uses `QUrl::toPercentEncoding(filename)` + `setQuery(QString)` to keep the round-trip intact (matters for fields like `G000.0+8.5_…`). |
| `DELETE /v1/exports/{filename}` | Remove the artefact. Idempotent: returns `valid=false` on a missing file rather than 404. |

Every lifecycle endpoint resolves the filename via
`resolve_workspace_path()`, which refuses any path containing a separator
or `..` segment.

**Client surface (`DataHubWidget`)**

The Data Hub's *Workspace Exports* panel (`buildWorkspaceExportsPanel()`)
lists the workspace with per-row buttons:

- **Open** — emits `openWorkspaceExportRequested(absolutePath)` which
  MainWindow forwards to `doOpenDataPath()` (the same flow used for
  every other dataset open).
- **Download…** — `QFileDialog::getSaveFileName` + `BackendClient::downloadExport`
  running in `QtConcurrent`.
- **Delete** — confirm + `BackendClient::deleteExport` + refresh.

The panel auto-refreshes on every backend health tick
(`DataHubWidget::refreshStatus()`); cube viewers also emit
`workspaceExportsChanged()` after a successful export so the panel
updates within frames of the operation instead of on the next 4s tick.

---

### Contour levels follow the data, once

`vtkWindowCube`'s contour filter gets its levels in `setupSliceRenderer()`, which
runs in the constructor - i.e. while `lowerBound`/`upperBound` are still the
placeholder `0..1` installed next to the placeholder cube image. The real range
arrives later, in the async preview handler, and used to update only the volume
rendering: the contour levels and the Lower/Upper fields kept the placeholder, so
on a cube whose values live in, say, `[-0.005, 0.08]` Jy/beam every level sat
outside the data and *Show Contours* toggled an empty actor - the toggle looked
dead.

`refreshContourBoundsFromData()` is called right after the preview sets the real
bounds. It writes the two fields (signal-blocked, 9 significant digits so the
values survive the round trip through text, which is what `updateContours()`
reads) and regenerates the levels. It runs **once**: typing in either bound also
sets `m_contourBoundsInitialised`, so a user's own numbers are never overwritten
by a preview that lands afterwards. `updateContours()` now refuses an inverted or
empty range with a status-bar line, and updates the mapper's scalar range, which
was also still built from `0..1`.

The image viewer's levels were never stale in the same way (`setupContourPipeline()`
reads the master layer's real scalar range and syncs the fields, and the preview ->
full swap marks the pipeline dirty so the first toggle rebuilds it) - but its
Level / Lower / Upper fields were **unreachable**. They were built in
`setupSidebar()`'s Contours card on `pageTools`, and that whole page is parked
hidden in `m_orphanToolsPage` because the Inspector's Analysis tab superseded it.
The Analysis tab can only list actions, so all the user had was a toggle.

`buildContourControlsBlock()` puts a CONTOURS block in the left dock next to
LAYER SETTINGS and CATALOGUE, where the cube viewer keeps its own contour card.
It **re-homes** the widgets `setupSidebar()` already built and wired instead of
building a second set (two Level spin boxes writing the same `m_contourLevel`
would disagree the moment either was used), and adds a "Show contours" checkbox
two-way bound to the `Show Contours` action, so the menu, the Analysis tab and
the dock all show one state. `clearAllContours()` now goes through that action
too, instead of leaving both controls reading "on".

Bounds ownership there works the other way round from the cube: they follow the
image's scalar range on every rebuild until the user types one, after which
`m_contourBoundsUserSet` keeps them. `commitContourBounds()` refuses an
unusable pair instead of storing it - the members feed `GenerateValues()` on the
next rebuild, so a pair that `updateContours()` had rejected would otherwise come
back by the side door.

## Channel Maps (`ChannelMapsWindow`)

Displays an N × M grid of 2-D channel slices with a shared colour scale,
rendered with **QCustomPlot** (`QCPColorMap`) instead of VTK to avoid the
macOS OpenGL context limit (~16 simultaneous contexts — a 64-cell grid
would crash). Each cell is a lightweight raster widget.

| Component | File | Role |
|-----------|------|------|
| Config dialog | `ChannelMapsDialog.{h,cpp}` | Start/End/Stride/Columns/LUT picker (gradient preview icons via `CubeUiAssembler::buildLutPreview`) |
| Mosaic window | `ChannelMapsWindow.{h,cpp}` | Non-modal `QMainWindow` with a `QScrollArea` → `QGridLayout` of QCPColorMap cells |
| LUT mapping | `lutToGradient()` (local to `ChannelMapsWindow.cpp`) | Samples a `vtkLookupTable` at 256 points → `QCPColorGradient` with matching colour stops |

**Data flow**: one `BackendClient::requestSubvolume(did, 0, W-1, 0, H-1, z0, z1)`
call (extended with `range_min/max`, `spectral_axis_type/unit`, `bunit`)
fetches the full z-slab. Client-side slicing extracts each stride-selected
plane and populates the `QCPColorMap::data()` cells. The shared data
range (`rangeMin..rangeMax` from the subvolume response) is applied to
every colour map so the LUT is directly comparable across panels.

**Double-click → enlarged view**: opens a `QDialog` with a single
full-size `QCPColorMap` + `QCPColorScale` (colour bar). The gradient is
passed by value from the mosaic (not copied from the source cell, because
`QCPColorMap::setColorScale()` resets the gradient to the scale's
default — the gradient must be set *after* the colour-scale link).
Drag + zoom are enabled for detail inspection.

**PNG export**: iterates over cells, `resize()` + `replot()` each to
the export geometry (400 × 340 @ 2× DPR) before `toPixmap()` so
`QCPTextElement` title labels ("CH 28") lay out at the export width
rather than the on-screen widget size. Composited into a single `QImage`
with a file-name + range title bar.

---

## Image viewer (`vtkWindowImage`) — recent additions

Several features were ported from (or inspired by) the cube viewer:

| Feature | Implementation | Notes |
|---------|---------------|-------|
| **Beam indicator** | `setBeamInfo()` — same parametric ellipse as the cube viewer, added to the 2-D renderer. `BMAJ`/`BMIN` come from `OpenDatasetResponse` (wired in `MainWindow`). | Hidden when beam keywords are absent. |
| **Region ExclusiveOptional** | `QActionGroup::ExclusionPolicy::ExclusiveOptional` on the four region-shape actions. | Same fix as the cube viewer — prevents multiple shapes checked simultaneously. |
| **WCS SegmentedToggle** | Coordinate format (`Sexagesimal \| Decimal`) and coordinate frame (`Galactic \| FK5 \| Ecliptic`) replaced from individual `QToolButton`s to `SegmentedToggle` pills. | Toggle writes into the original `QAction`s via `trigger()` for backward compat. |
| **Linear / Log scale** | Old `QRadioButton` pair replaced with a `SegmentedToggle` (`Linear \| Log`). The hidden radios remain the canonical state holder; the toggle syncs into them. | Same pattern as cutting-plane Show Contours in the cube sidebar. |
| **Contour overlay** | `vtkFlyingEdges2D` pipeline (same as the cube's slice contours) connected to the master layer. Level / Lower / Upper controls in the sidebar. External FITS contours via `vtkContourFilter` + `vtkFITSReader`. | `setupContourPipeline()` is re-called in `applyRemoteMasterLayer()` so it connects to the real image data, not the placeholder. |
| **FITS export → Workspace** | `POST /v1/exports/copy_dataset` copies the source FITS into the Workspace Exports dir. `BackendClient::copyDatasetToWorkspace()` on the client side. | Simpler than the cube's crop/moment flow — just a file copy with collision-safe basename. |
| **Measurement tools** | `MeasurementMode::Ruler` draws a dashed line between two clicked points and shows pixel + angular distance (Haversine on WCS coords). `MeasurementMode::Angle` uses three points to compute and display the angle at the vertex. VTK actors: `m_measLine1/2`, `m_measLabelActor`. | ExclusiveOptional QActionGroup; deactivates probe/region on enter. |
| **Pixel histogram** | `showHistogramPanel()` opens a QCustomPlot bar chart (256 bins). Two `QCPItemLine` cursors (red low / green high) are draggable; on mouse-release the LUT table range is updated live. | Samples with stride for large images; separate floating window (`m_histogramWindow`). Same logic is mirrored on the cube viewer side as `vtkWindowCube::showSliceHistogramPanel()` — it operates on `remoteSliceDisplaySource`'s current 2-D slice output and re-renders via `sliceWin->Render()` after `lutSlice->SetTableRange()`. |
| **Menu ↔ sidebar coherence** | Every menu action that surfaces a user-facing tool is promoted to a `QPointer<QAction>` member so it can be reused as the `setDefaultAction()` of a sidebar `QToolButton`. Image viewer adds an **I/O** card (Export to Workspace) and extends the **Contours** card with Load External + Clear All. Cube viewer appends an **Export & Inspection** card to the assembler-built Tools page covering Export Sub-Cube / Export Channel 2-D / Export Moment Map / Pixel Histogram / Overlay Slice on an Open Image. | The assembler-built cube Tools page exposes its outer `QVBoxLayout`; the cube window inserts the extra card before the trailing stretch so the layout stays visually balanced. |
| **SKAVA discovery integration** | New tab in `DataHubWidget` powered by `SkavaSearchPanel` (`src/gui/`) + `SkavaClient` (`src/skava/`). The panel queries `GET /discovery/search` on the configured SKAVA base URL with ObsCore-style filters (POS=CIRCLE, BAND in metres, TIME in MJD, COLLECTION, DPTYPE, obs_id) and presents results in a sortable table with replica info, best-node latency and a provenance panel (DOI / PID / citation / license). On **Open**, the panel resolves `GET /datalink/{obs_id}` to extract `primary_access.access_url`, then forwards it to `MainWindow::doOpenDataFromUrl()`. | Refactored `doOpenDataPath` into `openViewerFromOpenedDataset(path, opened, client)` so the same viewer-creation logic serves both local-path opens and SKAVA URL opens. |
| **Open-from-URL backend** | New endpoint `POST /v1/datasets/open_url` (`backend/app/routers/datasets.py`). Takes `{url, obs_id?, bearer_token?}`, downloads the remote FITS / HDF5 to `<exports>/skava_cache/<basename>.<sha256>.<ext>` via streamed `urllib`, classifies it via the standard `_fits_metadata` (so dynspec / PSRFITS / HDF5 sidecar paths kick in automatically), registers it as a session dataset and returns the same `OpenDatasetResponse` shape as `/v1/datasets/open`. | Idempotent: a cache hit (by URL hash) skips the download. `bearer_token` is forwarded as `Authorization` on the storage-node request when the access endpoint requires auth. |
| **Dynamic-spectrum / beamformed mode** | `vtkWindowImage::setDynamicSpectrumMode(true)` is called by `MainWindow::doOpenDataPath()` whenever the backend reports `kind='dynspec'`. The flag suppresses every celestial-WCS-dependent piece of UI: catalogue overlay, beam indicator, Stokes Analysis card, WCS frame switch, sanity panel's RA/Dec checks. Axis titles fall through `remoteOverlayAxisTitle()` to a fixed *Time* / *Frequency* pair. The **Time-Series Tools** sidebar card surfaces three workflow buttons (dedispersion, pulse-profile folding, RFI σ-clip) backed by the `/v1/dynspec/*` endpoints. Sidebar cards to hide/show are tracked via `m_dynspecHiddenCards` (populated at `setupSidebar()` build time) so the toggle is fully reversible. | The HDF5 / PSRFITS sidecar approach means preview/full image readers, region stats, contour pipeline, etc. needed **zero** changes — the dynspec dataset is just an ordinary 2-D FITS with `CTYPE='TIME'/'FREQ'`. The pulse-profile dialog is a stand-alone QCustomPlot QWidget (window), not a sidebar widget, so it can coexist with the main viewer. |
| **Preview → full-res swap (no freeze)** | `vtkWindowImage::applyRemoteMasterLayer()` mirrors the cube viewer's anti-stall pattern from `applyCubeOpenResult`: the cheap data swap (master-layer update, NaN colour) runs synchronously, then the **expensive parts** are deferred into a `QTimer::singleShot(50, …)` that first calls `QApplication::processEvents(ExcludeUserInputEvents)` to flush pending paint / status events before the GPU upload blocks the thread. Three additional optimisations brought the apparent freeze on a 7500×7500 image from ~26 s to ~2 s: **(1)** `computeBlankFraction()` is now stride-sampled to ~250 k samples instead of scanning all 56 M pixels via the virtual `GetScalarComponentAsDouble`. **(2)** `updateSanityPanel()` and `updateDataStatePanel()` (which call `computeBlankFraction`) are moved into the deferred block. **(3)** `setupContourPipeline()` — which runs `vtkFlyingEdges2D` with 15 levels over the full image (10–20 s on 56 M pixels) — is no longer called eagerly on the swap. Instead a `m_contourPipelineDirty` flag is set, and the rebuild happens lazily inside `setContoursVisible(true)` the first time the user actually asks for contours. | Visible contours stay valid: when `m_contoursVisible` is true at swap time, the pipeline is still rebuilt eagerly in the deferred block. The dirty flag also flips back to true on every subsequent swap so the rebuild always uses fresh data. |
| **Annotations** | Text (`vtkTextActor`) and arrow overlays placed interactively (crosshair click). Arrows use `buildArrowGeometry()` (shaft + fixed-size 8 px arrowhead, capped at 30 % of shaft). Arrow placement shows a live preview via `m_annotPreviewActor` updated in `mouseCallback()`. Save/Load via native `QFileDialog`. | `AnnotPlaceMode` state machine: `None → Text` (1 click) or `None → ArrowTip → ArrowLabel` (2 clicks). Right-click cancels. |
| **Blink / Compare** | `m_blinkTimer` toggles visibility of layers 0 and 1 at `m_blinkIntervalMs` (50–1000 ms slider). Requires ≥ 2 layers; `toggleBlink(false)` restores all layers visible. | `getLayerActor(int)` pass-through added to `LayerListModel`. |
| **Contour Phase 2 (cube → image)** | `vtkWindowCube` emits `contourDataReady(label, vtkImageData*)` via *Tools → Overlay Slice on an Open Image*. `MainWindow` relays to the first open `vtkWindowImage` via `overlayExternalContourData()`, which runs `vtkFlyingEdges2D` on the received data and adds the result as an external contour layer. | No WCS reprojection — assumes shared pixel grid (same dataset). |
| **Stokes analysis** | `StokesRole` enum + `QHash<int, StokesRole>` map associates layer indices with their I/Q/U/V/derived role. `visivo::pickStokesCompanion()` (`src/gui/StokesCompanion.h`, unit-tested) matches sibling files against common naming patterns (`StokesI` → `Q/U/V`, `_I.fits` → `_Q.fits`, ASKAP/MeerKAT `_PB` variants, `.fits.gz`). See "Loading the Stokes companions" below for where the listing comes from. Derived maps `P = √(Q²+U²)`, `PA = ½·atan2(U,Q)`, `P/I` are computed in-place on `vtkImageData` via `computeStokesP/PA/Fractional()` and added as layers via `addDerivedLayer()`. | Master layer NaN colour set to transparent for radio mosaic convention. |
| **Debiased P** | `computeStokesPdebiased()` estimates per-channel σ as `½·(σ_MAD(Q) + σ_MAD(U))` over all valid pixels (not robust to bright spatial features, intentional — meant as a quick field-average), then `P_deb = √(max(0, Q²+U²−σ²))`. Result tagged `StokesRole::DerivedPdb`. | Label includes the σ value used for traceability. |
| **Spectral index** | `computeSpectralIndex()` opens a `QDialog` (combos + line edits) to pick two layers and their frequencies, then computes `α = log(S_B/S_A)/log(ν_B/ν_A)` per pixel. Display LUT clamped to [−2.5, +1.5] (synchrotron + thermal range). | Both layers must share extent — explicit check, no resampling. |
| **Faraday RM** | `computeFaradayRM()` opens a `QTableWidget`-based dialog where the user adds ≥ 3 (Q layer, U layer, ν GHz) triplets. Per pixel: PA_i = ½·atan2(U_i, Q_i), λ_i² = (c/ν_i)², then closed-form linear fit slope = `Σ(λ_i²−λ̄²)(PA_i−PA̅) / Σ(λ_i²−λ̄²)²`. Result clamped to ±500 rad m⁻² for display. | **No PA unwrapping** — valid only in the low-RM regime. Documented limitation. |
| **Polarization vector overlay** | `rebuildPolarizationVectors()` samples Q and U on a configurable grid, computes per-pixel PA and P, filters by MAD-σ × SNR threshold, draws line segments in `m_polVecActor` (`vtkPolyData` + `vtkLineSource` family). Sidebar controls: grid step, SNR threshold, length scale. | Pixel-grid orientation assumes N-up E-left (standard FITS); rotated WCS would need a correction matrix. |
| **Radio region stats** | `RegionStatistics` extended with `sum` and `madSigma` (`1.4826 × MAD(values − median)`). `analyzeCurrentRegion()` adds a **Radio (beam-aware)** section computing `Ω_beam_px = π·BMAJ·BMIN/(4·ln 2)` (from `m_beamMajorDeg/MinorDeg` stored in `setBeamInfo()`) and the integrated flux `Jy = sum / Ω_beam_px`. When any Stokes role layers are loaded, a **Stokes (per-layer)** section re-runs stats on each role's data and reports the appropriate scalar (Jy / degrees / %). | Median-MAD σ replaces std-dev for fields with bright sources. |


### Loading the Stokes companions

*Tools -> Load Stokes Q/U/V Companions* used to be three problems in a trench coat,
all of which showed up as "the popup keeps coming back":

1. **Where it looked.** It matched candidate names with `QFile::exists()`, i.e. on
   the *client's* disk. Every dataset in this application is opened through the
   backend, which may be on another machine, so the auto-detection found nothing
   there and the user was dropped straight into the manual path.
   `listStokesSearchDirectory()` now asks the backend (`BackendClient::listFiles`,
   the same call the file browser makes) for the directory holding the Stokes I
   file, and only falls back to a local `QDir` listing when no backend URL is
   configured. The name patterns themselves moved into the pure, unit-tested
   `visivo::stokesCompanionCandidates()` / `pickStokesCompanion()`.

2. **Which chooser.** The manual step opened a local `QFileDialog`, the only one
   left in a dataset path. `File -> Add New Layer` (`addLayerFromBrowser()`) already uses
   `RemoteFileBrowserDialog`; the companion loader now does too, so the paths it
   hands the loader are paths the backend knows.

3. **What Cancel means, and how many popups there are.** The loop asked for Q,
   then U, then V, with `continue` on an empty result: cancelling one chooser
   simply opened the next, which reads exactly like a popup that reopens. Now a
   single summary dialog first states what was already loaded, what was found
   automatically and what is missing, and says how many choosers will follow and
   in which order. Each chooser's title reads *Locate Stokes U  (2 of 3)*, and
   Cancel abandons the rest of the sequence.

One more bug was hiding behind those: the found companions were loaded with three
back-to-back `addLayerImage()` calls, but that method drives a **single**
`QFutureWatcher`, so each `setFuture()` discarded the previous load and only the
last layer ever arrived. Loads are queued in `m_pendingStokesLoads` and started
one at a time from the layer-load completion slot; a failure clears the queue and
says so rather than leaving a silently partial Stokes set.

### Additional layers from a remote backend

The master image already opens remotely: its pixels come from the image
endpoints and never touch this filesystem. An additional layer could not, and
the reason was never pixel transport — `loadImageLayer()` aligns a layer to the
master with `AstroUtils`, and `AstroUtils` could only build a WCS from a **file**.
That is a property of the constructor, not of the alignment: the WCS lives in the
header, and `/v1/files/header` hands the header over as 80-character cards.

So `AstroUtils` gained a constructor over header cards (`wcsinit()` on the
reconstructed header string; everything after it was already read from that
string with `hget*`), and a remote layer now loads the way the master does —
pixels from `/v1/image/preview`, which returns the plane unchanged when it fits
and a decimated one when it does not, WCS from the two headers. Nothing is
copied to this machine, and the alignment is the *same arithmetic* as the local
path; the numbers it works on just arrive over HTTP.

Two mappings compose in the result: the layer's own decimation (displayed pixel →
layer source pixel, from `ImageLod::previewMapping`) and the alignment (layer
source pixel → master pixel). So the origin is the master pixel of the layer's
first **displayed** pixel — source `(scale−1)/2`, not source `(0,0)`, which on a
decimated layer is a different place — and the rotation slope is measured over
100 displayed pixels so a decimated layer measures the same arc.

`addLayerImage()` picks the path: if either the layer or the master is not
readable here and a backend is configured, the load goes through the backend.
Same watcher, same `ImageLayerLoadResult`, so every caller — Add New Layer, the
VLKB cutouts, the Stokes companions — is unaffected. `ImageLayerSet::addLayer`
takes the result's geometry as given (it does **not** re-run
`registerLayerToMaster`), so the local path is untouched: with no decimation the
new arithmetic reduces to exactly the old expressions.

The cards → header-string conversion is its own pure header
(`src/FitsHeaderString.h`, unit-tested): libwcs reads a header as one flat buffer
of fixed 80-column cards terminated by END, and each of those three rules fails
*silently* when broken — a short card shifts every keyword after it, a missing
END makes the parser run off the end of the buffer. The libwcs parse itself is
only exercised at runtime: the test target links neither libwcs nor cfitsio.

One libwcs trap worth knowing: `hlength`/`gethlength` keep the header length in a
**file-static**, set by whoever last read a header, and every `hget*` / `wcsinit`
call reads it back. Each constructor sets it for its own header before parsing,
or the parse runs with another header's length — reading past the end of the
buffer, or stopping before its keywords. That state is also *global*, so two
`AstroUtils` built at the same time (the layer loader runs on a worker thread
while the GUI thread can build its own) would each parse with the other's
length: construction takes a process-wide mutex. Every call into libwcs's header
parser in this codebase is inside those two constructors, so that is the whole
surface; the per-object `pix2wcs`/`wcs2pix` afterwards touch only their own
`WorldCoor`.

### The companion listing does not block the window

`BackendClient` is synchronous. The Stokes companion search ran `listFiles()` on
the GUI thread, inside a nested event loop, which froze the window against a slow
backend and let the user close it out from under the suspended call. The listing
now runs through a `QFutureWatcher` and the interactive half resumes in
`onStokesListingReady()`. The file choosers are still modal (like every other
chooser here), so `closeEvent` still refuses to close while they are up, and the
callback re-checks `isBusy()` before queueing — discovery no longer blocks other
imports, so one can have started while the listing was out. `isBusy()` counts
staging and listing for the same reason: they both end in a layer load, and the
window has exactly one layer watcher.

---

### One place for a tool's parameters

A tool's settings used to live wherever they had been built: the image viewer's
contour Level/Lower/Upper in a block that sat in the left dock whether or not
contours were on; the cube's in a `.ui` page (`pageContours`) inside the hidden
sidebar container, which means **they could not be typed at all** — built, wired,
unreachable, which is also why the cube's contour-bounds bug stayed invisible for
so long; and the compute tools' in their own dialogs.

The Inspector's third tab is now **Parameters**, with two captioned halves:

- the **active tool's** live settings, mounted by the tool when it opens
  (`InspectorPanel::setToolSettings`) and removed when it closes;
- **PROVENANCE**: the recorded parameters of the product selected in Session
  Data, which is what that tab always showed, with its Export-recipe button.

Mounting the settings is not enough: the tool also **reveals** the tab
(`WorkspaceChrome::revealParametersTab()`, which expands the Inspector if it is
collapsed and moves the rail with it). Settings that appear in a tab the user is
not looking at are the same as no settings — the failure this whole change was
meant to fix. The helper is named rather than the bare index 2 the two existing
call sites used.

Same question, one place: *which numbers produced what I am looking at*. Merging
rather than adding a fifth tab also avoids a trap — the tabs are addressed by
INDEX (`revealInspectorTab(2)`, and the collapsed rail's entry list), so a tab
that appears and disappears would silently renumber the others.

The widget is **re-homed**, not rebuilt: the window owns it, keeps it alive and
hands the same instance over, so there is never a second Level spin box writing
the same state. The rule, stated the way a *user* can read it rather than in terms of where the
code happens to keep things: a tool either **changes what you are looking at**
(no product, no run button - its settings are the state of the view, and they
live in this tab while it is on) or it **makes a new thing** (configure, run,
a row in Session Data). The app already has the convention that says which is
which, and it is the one every desktop uses: the **ellipsis**. `Compute Faraday
RM…` asks first; `Show Contours` acts. Four labels were lying about that and were
corrected - `Load Catalogue Overlay…`, `Save Annotations…`, `Load Annotations…` and
`Extract PV Diagram…` (it asks for the path width before you can draw) gained the
ellipsis they had earned, `Pixel Histogram`, `Image Quality / Artifacts` and
`Overlay Slice on an Open Image` lost one they had not (they open
a panel or run straight away; they ask nothing). Whichever route a product came
by, its parameters end up in the same tab afterwards.

In the tab so far: **contours**, the **polarisation-vector overlay** (grid
step, SNR threshold, length scale - which were also stranded on the hidden Tools
page) and the **Kinematic Lasso**. The lasso had kept its own `QDockWidget`
docked to the right of the cube window, under the Inspector — close enough to
read as part of the Analysis tab while belonging to nothing, and with no obvious
way to dismiss it. It now mounts the same body through `setToolSettings`, and
un-checking the tool closes it: the selection, the 2-D refinement and the
controls go together, because a green isosurface left on screen with no Apply or
Clear beside it is a state with no exit. The pane-layout gate un-arms tools whose
view has left the screen — that path **suspends** the lasso instead, or a
temporary switch to one pane would silently discard a selection the user spent
minutes refining. One tool's settings are mounted at a time and the window keeps the list of
tools that are on, most recent last: turning one off hands the section back to
whichever is still on rather than blanking it (contours on, vectors on, vectors
off used to leave the contours running with no way to change their levels).
Unmounted blocks are **parked** under the panel, not orphaned - `setParent(nullptr)`
does not delete a widget, it only takes it out of every ownership tree, so
nothing would ever have freed them. **Region needs no section**: its only
"parameter" is the annulus inner radius, and that is a per-use prompt with a live
preview during the drag, not standing state - its real parameters are the drawn
geometry, which is provenance. Tools that already own a persistent dialog — Moment Map Settings,
Export Sub-Cube, the noise region — keep it: the dialog hosts their progress
sheet, and duplicating their parameters in the tab would be two sources of truth.
For those the tab is the read-only half, after the fact.

### Region bounds: a circle is a centre and a rim, not two corners

Both viewers scan a bounding box and ask the hit-tester about each pixel inside
it. `regionBounds2D` built that box from `anchor` and `current` for every shape
except a polygon — but for a **circle and an annulus** those two points are the
centre and a point on the rim, so the box was one quadrant and the statistics
covered a quarter of the shape, in whichever direction the user happened to drag.
The boxes now come from `CubeRegionGeom::boxBounds` / `circleBounds` (radius
rounded out, so a rim pixel is never truncated away) / `polygonBounds`, in the
pure header with the hit-testers, with tests that assert every pixel the
hit-tester accepts is inside the bounds.

There were **three** copies of that box. The third (`vtkWindowCube.cpp`, which
Export Sub-Cube and Mask 3-D Region default their extents from) had the same bug:
exporting a sub-cube from a circle region gave a box around a quadrant of it.
All three now dispatch to the shared helpers.

### One mapping, written where the sampling is known

Every overlay, coordinate readout and WCS label in the image viewer reads the
displayed image's own VTK `origin`/`spacing` — world coordinates in this window
*are* full-resolution source pixels. So those two numbers, set once at load time,
decide whether all of them are on the right pixel. They have to mirror how the
**backend** sampled, and each loader was deriving them for itself:

| path | backend sampling | mapping |
|------|------------------|---------|
| preview (`_build_preview`) | `source = floor((i + 0.5) · full/preview)` | `scale = full/preview`, `origin = (scale − 1)/2` |
| tiles (`worker_image_tile`) | `data[y0:y_stop:step, x0:x_stop:step]`, `step = 2^level` | `scale = step`, `origin = tile offset` |

`ImageLod::previewMapping()` / `assemblyMapping()` hold those, next to the tile
contract they mirror, with a test that replays the backend sampler and asserts
every displayed pixel lands within **half a source pixel** of the pixel the
backend actually put there (half a *cell* would be satisfied by leaving the
origin at zero — the bug itself).

Three things were wrong before, in increasing order of seriousness:

- The preview and the coarse whole-image assembly used an endpoint-aligned
  `(full−1)/(displayed−1)` with origin 0, pinning the first and last displayed
  pixels to source `0` and `full−1`, which is where neither sampler put them.
- `buildPreviewImage()` — the client's *second* downsample, for rendering a very
  large assembly — uses the same `floor((i+0.5)·factor)` sampler but copied the
  input origin unchanged, so what was drawn sat half a cell off what the probe,
  the region statistics and the catalogue overlay (which read the underlying
  image) reported.
- Worst: the preview path never set `result.spacing`/`result.origin`, and
  `ImageLayer::applyLoadResult` re-applies those **over** whatever the image
  carries — so the preview's decimation was discarded entirely and every
  coordinate on a preview was off by the whole decimation factor, not by a half
  cell. The loader's own `SetSpacing` had been dead code on that path.

With the origin no longer always zero, the probe's `worldCoord − origin` turned
out to be subtracting it a second time; the world position already *is* the
full-resolution index.

The same class of bug surfaced once more in the cube viewer, in the **amber ROI
preview box** that Export Sub-Cube, Mask 3-D Region and Estimate Noise all draw
while you type their bounds. The six numbers are full-resolution voxel indices —
the spin boxes are built from the dataset's real dimensions and the backend reads
them the same way — but they were fed straight to `vtkCubeSource::SetBounds`,
under a comment asserting that spacing (1,1,1) and origin (0,0,0) make pixel and
world coordinates identical. True at full resolution; false on the decimated
preview, which is what the viewer shows until you press *Load full resolution*.
On a 1046×952×398 cube previewed at 349×318×133 the box came out **three times**
the size of the cube's own outline, floating outside it. `setNoiseRegionPreview`
now maps index → display world with the display image's own geometry,
`(ddim−1)/(full−1) × spacing`, and skips the mapping when a high-resolution ROI
is loaded, because that display already carries full-res coordinates (its origin
is the ROI corner, its spacing 1) — the same distinction
`updateLassoDisplayPlacement` makes. The 2-D rectangle beside it needs no
mapping: the backend serves the slice at full resolution. A box drawn before a
preview↔full swap is re-placed afterwards
(`refreshRegionPreviewAfterDisplaySwap`), next to the lasso's equivalent.

The lesson keeps repeating, so it is worth stating once: **an overlay's numbers
and the display's coordinates are two different spaces whenever an LOD is in
play**, and every new overlay has to say which one it is in.

### Extract Spectrum, in the one 2-D case where it means something

*Extract Spectrum* sat permanently disabled in the image viewer with "only
available for cube views". True of a sky image — there is no third axis to run
along — but not of a **dynamic spectrum**, which is a time × frequency plane:
collapsing the time axis is a spectrum, and it is the first thing anyone looks at
on beamformed data. So the action is enabled in dynspec mode and does exactly
that, over the drawn region's bounding box when there is one.

Two things it has to be honest about. Frequencies come from `remoteVoxelToWcs`,
which speaks **full-resolution** indices, so the displayed row goes through
`currentVoxelFrame()` first — a decimated or viewport-tiled display would
otherwise label every channel with another channel's frequency. And the scope
says "the whole observation" only when the display really holds it; on a partial
display it names the time samples it actually collapsed. A fully flagged channel
is NaN (a gap in the plot), not zero.

The curve is kept in `m_extractedSpectra` so the Session Data row replots it
rather than showing only the summary — the point of a spectrum is its shape.

### Cross-match leaves a row

`onCrossmatchReady()` registers a product with the catalogue, the search radius
and the match count, including when the match count is **zero**: "nothing within
60 arcsec of this field" is an answer, and it used to vanish with the status
message. The request parameters are stashed when the query is launched, since the
finished handler is generic (it just projects a parse result).

## VLKB archive path (query → inventory → cutout → layer)

Three hops, and each used to build its own idea of where the user was pointing.

**One POS string.** `visivo::circlePosString` / `rangePosString`
(`src/gui/VlkbPosString.h`, unit-tested) is the only place a POS is built.
Before that there were three: the panel's preview, `VlkbQueryService::search*`
and the SODA URL, and two of them normalised a negative galactic longitude by
adding **180** instead of 360 — so a query at the Galactic centre searched the
far side of the Galaxy, and after the first fix the preview and the search
disagreed with each other. `VlkbQueryService::searchPos()` takes the string the
caller already showed the user, so preview, search and cutout cannot drift.

Latitude is clamped rather than wrapped (past the pole is a typo, not a
position); a box straddling l = 0 is emitted as `l₁ > l₂`, the RANGE convention
for a wrap, because normalising the two edges independently selects the 356° of
sky the user did *not* ask for; and a 360°-wide box becomes `0 360` rather than
the zero-width interval two independently normalised edges would give.

**The response is checked, not assumed.** `/v1/vlkb/fetch_cutout` validates the
downloaded bytes before calling the file a FITS (see
[Backend API · VLKB](backend-api#vlkb)). Without that, a service error saved as
`.fits` surfaced as *"No SIMPLE card found"* from the FITS reader — an error
about the file's syntax standing in for the archive's own explanation of what
had gone wrong, which was inside that very file.

**One layer loader, therefore a queue.** `vtkWindowImage` has a single layer
load in flight (`layerLoadWatcher`), and `addLayerFromBackendPath` used to
*drop* a request that arrived while it was busy. Selecting six datasets in the
inventory fires six at once, so they are queued instead
(`m_pendingLayerLoads`, drained by `pumpLayerQueue()` from every path that stops
being busy — the layer loader, the preview and full-resolution image loads, the
Stokes listing).

The queue is shared with the Stokes companion sequence, and the two have
different failure semantics, so each entry carries `partOfSet`: a failed Stokes
companion abandons the rest of **its set** (half a set is worse than none, and
the user cannot see which half they got), while six independent cutouts carry on
— one failed download is no reason to drop five good ones. The flag of the
running load is taken and cleared *before* the error dialog, whose event loop
would otherwise let a new cutout inherit it.

**Lifetimes.** The inventory window has `WA_DeleteOnClose` while the download
watcher belongs to `MainWindow`, so the tree is held through a
`QPointer` — including across `openVlkbImageLayer`, whose backend calls spin an
event loop of their own.

### The batch path (`app/vlkb_mcutout.py` + `McutoutJobDialog`)

Twenty cutouts as twenty `sync` requests is twenty chances to lose one; the
archive's own answer is a UWS job, which the legacy client drove from the
desktop (`MCutoutSummary`, VLVA 1.7.4). Here the protocol is on the backend and
the client owns the table and the polling — and the results are **unpacked**
rather than handed over as a `.tar.gz`, which is where the legacy stopped.

Three things carry the security weight, and each was found by review rather than
by design:

- **The job id is a path as well as a URL.** It comes back from the client on
  every later call and is interpolated into both, so `validate_job_id` refuses
  anything that is not a plain identifier — `.` and `..` included — and the
  route's own model refuses it again with a 422 before the filesystem is
  touched.
- **A redirect carries the token.** `urllib` copies `Authorization` into the
  redirected request, so a service answering 302 with a host of its choosing is
  handed the user's VLKB bearer. Every call goes through one `_open()`, which
  refuses redirects (`_RefuseRedirect`) and refuses to send a token over plain
  HTTP. The two calls whose answer *is* a 3xx — the submit and the RUN command —
  use `_CaptureRedirect`, which reads the `Location` without following it.
  `submit_job` built its own request and observed neither rule until it was
  routed through the same place.
- **An archive is not its download size.** The byte cap on the transfer says
  nothing about what it unpacks to, so the extraction is incremental and counts
  members and extracted bytes; `filter="data"` does not impose quotas. A member
  that resolves outside the target, a symlink, a device or a *sparse* member is
  refused — for a sparse member `tarfile` does not verify that the map agrees
  with the declared size, which is the bound the whole loop relies on.

A submitted job is never dropped on the floor: if the submit succeeds and the
first phase read fails, the route returns the id with `phase: "UNKNOWN"` rather
than a 502, because the job is already running on the archive and a retry would
submit a second one. Each fetch unpacks into a staging directory of its own,
removed if anything fails — a half-written archive is not a result.

---

## SED fitting (`backend/app/compute/sed.py` + `SedWindow`)

Ported in September 2026 from the legacy `ViaLacteaVisualAnalytics`, whose
engines (`Utils/sedfitgrid_engine_thin|thick_vialactea.py`, D. Elia 2012–2015,
Python port S. Mordini 2021) were driven from `sedvisualizerplot.cpp` through
`QProcess` + `eval()` on positional argv strings, against a `python.exe` path
held in `QSettings`.

**Where each piece lives, and why there.**

| Piece | Where | Why |
|-------|-------|-----|
| The physics | `backend/app/compute/sed.py` | Pure NumPy, no I/O, no Qt. A fit is reproducible from the API, and testable without a GUI or a display. The legacy result depended on whatever Python the user happened to have. |
| The HTTP surface | `backend/app/routers/sed.py` | `/v1/sed/fit`, `/v1/sed/models`, `/v1/sed/defaults`. The VLKB model service's underscore-joined positional query and its SSRF guard live here, not in the client. |
| Building the SED from a catalogue | `src/gui/SedBuilder.h` | Header-only and free of widgets, because this is where a SED goes wrong *quietly*: a misread column gives a fit that runs and is wrong. Unit-tested against both catalogue shapes. |
| Presentation | `src/gui/SedWindow.{h,cpp}` | QCustomPlot + tabs. Owns no physics; the fits run in `QtConcurrent` because `BackendClient` is synchronous. |
| Transport | `src/app/BackendClient_Sed.cpp` | Same result-struct pattern as the other domains. |
| Entry point | `vtkWindowImage::showCatalogueSourceSed()` | Context menu on the catalogue table row. The window emits `fitProduced`; the *viewer* turns that into a Session Data product, because the viewer owns the dataset key, the window scope and the backend session — `SedWindow` knows none of them. |

**Two catalogue shapes, one builder.** `SedBuilder::sourceGroupIndices()` expands
a clicked row into the whole band-merged family (rows sharing `groupKey`);
`buildSed()` then reads one point per band-tagged entry, or — for an untagged
row — reads *across* its flux columns. Column names are matched by explicit
candidate lists and a regex that converts unit suffixes (`S_1.2mm` → 1200 µm,
not 1.2), and deliberately does **not** match `err_flux250`, `flux250_err`,
`background250` or `flag70`, each of which would otherwise add a phantom point.

**Angular size is not the drawn shape.** `CatalogueOverlayEntry::sizeArcsec` is
separate from `radiusX`/`radiusY` on purpose: the radii are *pixels* for every
Image-frame entry (the VLKB ellipse, a ds9 image region), and the optically
thick fit needs arcsec. Using the radii scaled the solid angle — and therefore
the mass — by the pixel scale squared. The VLKB path fills `sizeArcsec` from the
FWHM columns before they are filtered out of `extra` as geometry.

**What was deliberately changed from the legacy engines** (do not "restore"
these; the legacy behaviour is the bug in each case):

- The stop rule *"χ² improved by less than 1 %"* is gone. It halts the zoom
  while the grid is still coarse: on a four-parameter thick fit it returned
  χ² ≈ 6 where the true model scores 0. Convergence is now about **resolution**
  (`_fully_resolved`), with a two-strike guard for a refinement that finds
  nothing better.
- Refinement is **multi-start** (`_seed_indices`, up to 8 separated minima of the
  first grid). Zooming from the single best coarse cell converges neatly into the
  wrong basin; the extra seeds are what find the optimum at all.
- The best model is kept **globally** across iterations, so a refinement that
  lands on a shifted grid can lower the resolution but never the quality.
- Uncertainties are a **profile likelihood**: `_profile_bounds` scans each axis
  outwards from the best fit with the nuisance parameters re-minimised
  (`_profile_chi2`), bracketing and bisecting the Δχ² = 1 crossing
  (`_threshold_crossing`). Reading the interval off the refined grid measures the
  zoom, not the data — a 0.1 % error bar came back as "mass known exactly". The
  threshold is anchored on the profile's own value at the optimum, so a finite
  nuisance sampling cannot start the search outside its own bracket.
- `dof = N_detections − N_free_parameters`, returned **as it is** even when ≤ 0,
  and `chi2_reduced` is then `null`. Clamping it to 1 let an under-determined fit
  win a thin-versus-thick comparison on a number that meant nothing. The client
  carries that as NaN (`std::isnan`), never as 0.
- Upper limits are excluded from the χ² and **veto** models brighter than the
  limit, instead of the legacy sign test that indexed a NumPy array with
  `list.index`.

**Branches, and the identity problem they create.** A band-merged source can
have several counterparts in one band. `SedBuilder` keeps them (sorted by
wavelength, then flux; the fainter ones flagged `alternative` and unticked)
rather than deduplicating, and `SedWindow` can *collapse* them — summing fluxes,
errors in quadrature, refusing any group that mixes a detection with an upper
limit, because that sum is not a measurement.

Collapsing renumbers the rows, which breaks anything that identifies a point by
its row: an export aligned by row index put one band's model next to another
band's photometry. `m_rowBranches` maps each displayed row to the pristine-list
branches behind it, `FitProvenance::sentBranches` records the same for the
request that ran, and the two are matched as *sets*. Edits propagate down to the
branches through the same map — but only the field the user actually changed,
compared against `m_displayedBaseline`: a collapsed row's `use` is the OR of its
branches, so copying it back wholesale turned "one branch in, one out" into
"both in" on the next expand.

**The plot is the input.** `QCustomPlot`'s own selection *is* the set of bands
in the fit: `syncPlotSelection()` writes the `use` flags into it,
`onPlotSelectionChanged()` reads them back, and a selection decorator draws the
difference (filled/bright versus hollow/dim). The read-back defers its
`replot()` through `QTimer::singleShot(0, this, …)` — the slot runs inside
QCustomPlot's mouse handling, and `replot()` destroys the very plottable whose
selection QCP has just changed.

**Provenance, client side.** `SedWindow::FitProvenance` snapshots the request —
the rows sent, the photometry sent, distance, κ, λ_ref, size, colour-correction
flag, model, and an input revision counter — at launch. Export and the Session
Data product read *that*, not the widgets: the user is free to change the ticks
and spin boxes afterwards, and reading them later wrote a provenance that never
happened and mis-aligned `fitted_flux_jy` with the bands. Editing after a fit
marks the results stale; editing *during* one is caught by comparing the
revision at completion.

**Known limits, stated rather than hidden.** L_bol integrates the sampled
5–2000 µm range only (a lower bound). The thick model's mass is derived, so its
range is the spread over the corners of three separately profiled intervals, not
a Δχ² = 1 interval — the result says so in its own `warnings`. Distance
uncertainty is not propagated anywhere.

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
- **Interaction**: hover highlight (yellow wireframe sphere), click-select (red wireframe), Inspector ▸ Properties info panel, table view dock
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
- **Layout**: the shared chrome — command bar, Session Data (display controls), Inspector (Properties / Analysis with the filters / Parameters / Copilot), status rail
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

## One window shape for every viewer

The image and cube viewers had the command bar, the Session Data dock, the
Inspector and the status rail. The 3-D catalogue and the two VBT windows had a
`SidebarPanel` — a control rail of their own design, which existed nowhere else
— and no bar, no rail, no Inspector. Moving between a cube and a catalogue meant
learning the window again, and the catalogue said nothing about which file it was
showing or whether the backend was still answering.

They are the same shape now. `WorkspaceChrome::installCommandBar()` is the whole
"make this window a viewer" step in one call (it was three hand-written copies),
and each window then mounts its own pages in the two docks:

| page | where it goes |
| --- | --- |
| view / rendering controls | Session Data, under the dataset row (design #1d: what is drawn lives beside what it is drawn from) |
| dataset and selection info | Inspector ▸ Properties |
| filters | Inspector ▸ Analysis — filtering is an analysis of the data, not a view setting |
| copilot | Inspector's own tab (`installCopilot`), not a page of its own |

The kind tag in the command bar is clickable in every viewer and answers the
same question — *what is this file* — with what that window can say: the FITS
header, the catalogue's columns and which of them are its coordinates, the VBT's
fields and geometry.

### The column is 272 px, so the controls have to fit it

The left column is a fixed `kLeftDockWidth`, and its scroll area had the
horizontal bar **always off**. Content whose minimum width exceeded the column
was therefore cut with nothing to say so — and the minimum is not the layout's
to give: a `QComboBox` asks for its longest entry ("Supergalactic (SGL, SGB)"), a
`QDoubleSpinBox` with 12 decimals for "0.383494066596" and its buttons. The
pages built for a 320 px rail overflowed a 272 px dock (user report, with a
screenshot).

`addSessionLayers()` now normalises what it mounts: combos elide
(`AdjustToMinimumContentsLengthWithIcon`, 4 characters), spin boxes and line
edits get a 70 px minimum, all of them `Expanding` horizontally, and every
`QFormLayout` in the page switches to `AllNonFixedFieldsGrow` — macOS defaults to
`FieldsStayAtSizeHint`, so relaxing the minimum alone left 100 px combos reading
"Comput" beside an empty half-column. A width the caller FIXED (minimum ==
maximum) is left alone, and a spin box's internal editor is not touched: it is
sized by its owner. The scroll bar is `AsNeeded` underneath all of it, so nothing
can be silently clipped again.

### Anything the application can open, from anywhere

Two entry points decided for themselves what was openable, and both decided
"FITS":

* `openDatasetPath()` — the desktop, the command line, a file association — went
  straight to the FITS/HDF5 route, so a double-clicked IPAC table, `.speck`, CSV,
  VOTable or VisIVO Binary Table was answered with *"Unsupported file type.
  Expected FITS (.fits, .fit) or HDF5"*, although all of them open from the
  application's own Open… dialog. It goes through `dispatchOpenForPath()` now,
  the same classify-first decision, with the backend health checked there so a
  dead backend is reported as such instead of surfacing as "could not identify
  this file".
* `RemoteFileBrowserDialog` enabled its Open button only for a row the listing
  had marked as FITS whenever no caller had set an extension filter — which is
  exactly the unified Open…, whose whole point is that the user does not decide
  the kind first. A VBT could be seen in the list and not opened: the button
  simply stayed grey (user report). Any file is selectable there now; the
  classifier answers afterwards, including "I know what this is and nothing here
  opens it". A caller's extension filter still means exactly what it says — and
  the VBT one accepts either half of the pair, since the classifier resolves a
  `.bin` to the `.head` beside it.

---

## Tool dialogs — two families, and which is which

There are **two** kinds, and the difference is deliberate (this section used to
claim every tool dialog was non-modal, which was never true of half of them):

**Compute dialogs — non-modal** (`QDialog::show()`), because they own a job that
can run for minutes and the user must keep working meanwhile:
`LinewidthDialog`, `BaselineDialog`, `StackDialog`, `SourceFindDialog`,
`ChannelMapsDialog`, `LUTCustomizerDialog` (2D + 3D), the FITS header viewer and
the moment description popup. They use `VisivoTheme::makePrimaryButton` /
`makeSecondaryButton`, and their primary carries the **verb** — *Compute*,
*Stack*, *Generate* — with *Close* beside it. Long operations show a progress
indicator scoped to the dialog (window-modal to it, not to the application).

**Parameter pickers.** These answer one question and return: the tilted-ring
model, Export Movie, Spectral Lines, Color Maps are still modal (`exec()`),
built inline where they are used and still using a bare `QDialogButtonBox`; two
of them relabel the primary (*Export…*, *Overlay*) and the rest read *OK*.
**Known incoherence**, not yet unified: they should take the theme's
primary/secondary buttons and a verb label like the family above.

The pickers that *launch a computation* are **no longer modal** — Moment Map
Settings (`MomentMapController`, `dlg->show()`), the noise region
(`NoiseRegionDialog`, kept and re-raised by `NoiseController`) and Export
Sub-Cube / Mask 3-D Region (`CubeBoxToolDialog`) stay open and host the progress
sheet themselves, so the modal sheet appears on the dialog the user is looking at
rather than on the cube window behind it. That is the rule for every tool that
computes: the dialog stays, the sheet goes on it.

Window titles are Title Case throughout ("Stack Cubes to a Spectrum", "Color Maps",
"Spectral Lines" were the three that were not, and are now).

**Punctuation in user-visible strings: no em-dashes.** Every label, tooltip,
status line, banner and message body uses a comma, a parenthesis or a plain
hyphen instead; 265 strings across 54 files were converted, plus the backend's
user-facing error text. The dash was doing three separate jobs and each takes
its own replacement, which is why this is not a blind substitution:

| Job | Example | Becomes |
|---|---|---|
| "no value yet" placeholder | `"Backend capacity: —"`, `"RA — Dec —"` | `-` |
| self-contained trailing aside | `"…freely — the copilot remembers."` | parentheses |
| apposition | `"VisIVO Binary Table — points / volumes"` | comma |
| anything else, incl. a clause running into the next concatenated fragment | `"…held back — "` | ` - ` |

Two traps a mechanical pass falls into: a dash at the end of a *fragment* of a
concatenated literal must keep its trailing space (otherwise two words glue
together across the join), and a trailing aside cannot become a parenthesis when
the sentence continues in the next fragment (the closing bracket lands
mid-sentence). Docstrings and code comments keep their dashes: they document the
code to developers and are not labels. The copilot's system prompt carries the
rule too, since what the assistant writes is itself user-visible text.

Parameters are remembered per dataset through `AnalysisParamStore` by the moment,
noise, kinematic-model and spectral-lines dialogs. Channel Maps, Export Movie,
Export Sub-Cube, Line-Width, Baseline and Stack still forget — the same known
gap, and the reason the spectral-lines one was fixed first (it asked for the
systemic velocity again on every use).

`StackDialog` is built around an explicit `DatasetEntry` list:

- The caller (`MainWindow` or `vtkWindowCube`) calls
  `BackendClient::listSessionDatasets(sessionId)` in a worker, gets the full
  set of cubes open in the backend session, and passes them with the
  `currentDatasetId` of the calling window.
- The caller collapses entries that resolve to the same FILE. A path can be
  registered in one session more than once (the baseline route registers its
  output, and opening that output from the product row registers it again under
  a second id), and the cube then appeared twice in the list, identical and
  indistinguishable. Stacking a cube with itself is not a thing anyone wants.
- The dialog pre-checks the current cube, hides non-cube entries, and
  disables (with tooltip) cubes whose `(width × height × depth)` does not
  match the reference shape — they cannot be stacked together.
- The selection counter is computed once at the end of the constructor. It used
  to update only on `itemChanged`, which is connected *after* the population, so
  the dialog opened reading "Selected: 0 cubes" with a box visibly ticked and
  the Stack button disabled.
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

QTest-based headless suite; no VTK, no Qt::Widgets, no live backend. Counts are
as QTest reports them, so each class includes its own init/cleanup.

| File | Tests | What it covers |
|------|-------|----------------|
| `test_backendclient.cpp` | 34 | `parseMomentResultObject`, `parsePvResultObject`, `parseNoiseResultObject`, open-response parsing |
| `test_catalogue_parser.cpp` | 35 | `detectedRedshiftField`, `detectedDistanceField`, `comovingDistanceMpc`, `entryDistanceMpc` |
| `test_backend_routing.cpp` | 13 | `skava::BackendRouting` endpoint selection |
| `test_image_lod.cpp` | 24 | tile pyramid / level-of-detail arithmetic |
| `test_analysis_param_store.cpp` | 8 | per-dataset dialog parameter persistence |
| `test_backend_contract.cpp` | 4 | shared header manifest ↔ client constants |
| `test_remote_slice_cache.cpp` | 11 | slice cache keying + eviction |
| `test_linked_window_registry.cpp` | 8 | linked-views registry lifetime |
| `test_spectral_unit_convert.cpp` | 25 | unit parsing/conversion **and** rest-frame → axis placement (`axisKindFromCtype`, `observedFrequencyHz`, `axisValueForFrequency` for VOPT/VRAD/VELO/FREQ) |
| `test_cube_region_geom.cpp` | 20 | pure ROI hit-testers (box / circle / polygon / annulus) |
| `test_cube_tool_gate.cpp` | 11 | `CubeToolGate` — which tools may act, view gates vs state gates |
| `test_spectral_axis_infer.cpp` | 16 | spectral-axis descriptor inference from CTYPE/CUNIT |
| `test_kinematic_level_field.cpp` | 19 | kinematic-lasso level field |
| `test_ray_voxel_pick.cpp` | 18 | 3-D ray → voxel picking |
| `test_tool_result_record.cpp` | 8 | reopening a tool result exactly as it was |
| `test_product_origin.cpp` | 15 | where a measurement was taken (region / voxel) |
| `test_catalogue_csv.cpp` | 11 | CSV quoting, multi-line records, truncated files |
| `test_fits_header_string.cpp` | 6 | header cards → 80-column FITS header string |
| `test_stokes_companion.cpp` | 9 | Stokes Q/U/V companion filename matching |
| `test_vlkb_pos_string.cpp` | 11 | VLKB POS strings: galactic longitude wrapping, latitude clamping, the box across l = 0 and the full-circle box |
| `test_sed_builder.cpp` | 20 | `visivo::SedBuilder` — building a SED from catalogue rows (band-merged groups, flat rows with several flux columns, column-name and unit rules, angular size) |

Build (standalone — this is what CI runs; no VTK, no Qt::Widgets):
```
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests -V
```
or as part of the main project with `-DBUILD_TESTING=ON`.

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
| `VISIVO_MOMENT_THREADS` | `0` (auto) | Number of threads for M0/M6/M10 moment chunking. `0` = `min(4, cpu_count)`. Positive values set the count explicitly. |
| `VISIVO_MOMENT_STREAM_BYTES` | `2147483648` (2 GiB) | Materialised-subset size above which M0/M1/M2 stream in spectral slabs (out-of-core) instead of loading the whole channel range. |
| `VISIVO_MOMENT_SLAB_CHANNELS` | `64` | Channels per slab in the streaming moment path. |
| `VISIVO_DASK_MODE` | `auto` | Moment compute backend: `auto` (distributed if a scheduler is set, else local threaded Dask for large jobs), `off`, `local` (always local threaded Dask), `distributed` (require a cluster). |
| `VISIVO_DASK_MIN_BYTES` | `536870912` (512 MiB) | In `auto` mode, minimum materialised-subset size before a moment is routed to the local threaded Dask path. |
| `VISIVO_DASK_SCHEDULER` | *(unset)* | Address of a `dask.distributed` scheduler; when set, moments fan out across the cluster. |
| `VISIVO_WORKERS` | `min(4, cpu_count)` | Number of ProcessPoolExecutor workers for CPU-bound FITS operations. |
| `VISIVO_HEAVY_SLOTS` | `max(1, VISIVO_WORKERS-1)` | Max concurrent heavy tasks (moment / isosurface / pv / spectral). Leaves `WORKERS - HEAVY_SLOTS` pool slots reserved for interactive requests so the GUI stays responsive while compute is running. |
| `VISIVO_LINEWIDTH_CHUNKS` | `VISIVO_WORKERS` | Row-chunks the linewidth orchestrator fans out per request. Already gated by `VISIVO_HEAVY_SLOTS`, so usually no need to lower this. |
| `VISIVO_LINEWIDTH_SNR` | `3.0` | Per-pixel SNR cutoff for skipping background pixels before the Gaussian fit (`0` disables skip). |
| `VISIVO_PRODUCT_CACHE_ENTRIES` | `32` | LRU capacity of the in-process product cache (shared by moment, isosurface, pv, linewidth results). |

### Client build flags (CMake)

| Flag | Default | Effect |
|------|---------|--------|
| `VISIVO_ENABLE_VR` | `OFF` | Enable the optional VR (OpenXR) cube viewer offload. Requires a VTK built with `-DVTK_MODULE_ENABLE_VTK_RenderingOpenXR=YES`; otherwise CMake falls back to a no-VR build with a warning. macOS is not a supported VR target (no OpenXR runtime available); the flag is accepted but the *Tools → Open in VR* action stays disabled at runtime. See the *"Optional: VR (OpenXR) offload"* subsection above for the full enablement procedure. |
