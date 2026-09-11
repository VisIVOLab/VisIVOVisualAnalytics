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

Still missing too: per-chunk progress from the backend for the chunked computes
(line-width dispatches N chunks and reports none of them), which needs a channel
back to the dialog, not just a log line.

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

## Session Data tree (`SessionDataTree`) — selection is passive

The tree is the table of contents for the dataset's views (`view3d`, `slice2d`)
and its registered products. Its contract, worth stating because breaking it is
invisible until someone loses a pane:

| Gesture | Signal | Effect |
|---------|--------|--------|
| select (any button) | `productSelected(productId)` | Inspector ▸ Properties / Provenance follow. **Nothing is mounted.** |
| double-click | `rowActivated(viewTag, productId)` | `activateSessionView()` mounts the row (free pane → grow), or raises the product's own window when it has no pane renderer |
| right-click | `rowContextMenuRequested(viewTag, productId, pos)` | the viewer builds "Show in ▸ Pane N" (and "Bring window to front" for a window-only product) |
| pane-badge click | `paneBadgeActivated(pane)` | activates the pane that already shows the row |

`currentChanged` used to emit **both** `productSelected` and `rowActivated`, so
selection *was* activation. Two consequences: clicking a row to read its
provenance replaced whatever the active pane was showing, and — because Qt makes
the pressed row current on any button — a right-click mounted the product before
its own "Show in ▸" menu opened, which made that menu unreachable in practice.
Selection is now inspection only; mounting is explicit.

Two things follow from the change and must not be re-broken:

- `onActivated()` handles the intrinsic **view** rows as well as products. They
  used to arrive in a pane only through the selection side effect, so
  double-click did nothing for them.
- The `m_suppressActivate` flag (with its RAII guards around `selectProduct()` /
  `selectForView()`) existed solely to stop a *programmatic* selection from
  mounting a pane. With selection passive it guards nothing and is gone — if
  activation is ever put back on `currentChanged`, that guard has to come back
  with it, and so does the right-click problem.

### What a tool must leave behind

Every tool that produces a *result the user could want later* registers a
product. `renderer` decides how the row behaves, and the three cases are all
legitimate:

| `renderer` | Row means | Tools |
|---|---|---|
| `moment2d` / `image2d` / `spectrum1d` / `pvdiagram` | mountable in a pane | Moment, Line-Width, PV, Extract/Pin Spectrum, region spectra, Stack |
| *(empty)* | a **measurement**: the numbers live in Provenance | Estimate Noise, region 2-D statistics |
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
  feed), Export Region a region, Send Slice to Image Viewer an open
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
| 3D WCS axes (`vtkCubeAxesActor`) | `actionShow3dWcsAxes` | View → Show 3D WCS Axes | *3-D View Settings* → **3D REFERENCE** → "Show 3D WCS axes" toggle button |
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
| **Menu ↔ sidebar coherence** | Every menu action that surfaces a user-facing tool is promoted to a `QPointer<QAction>` member so it can be reused as the `setDefaultAction()` of a sidebar `QToolButton`. Image viewer adds an **I/O** card (Export to Workspace) and extends the **Contours** card with Load External + Clear All. Cube viewer appends an **Export & Inspection** card to the assembler-built Tools page covering Export Sub-Cube / Export Channel 2-D / Export Moment Map / Pixel Histogram / Send Slice to Image Viewer. | The assembler-built cube Tools page exposes its outer `QVBoxLayout`; the cube window inserts the extra card before the trailing stretch so the layout stays visually balanced. |
| **SKAVA discovery integration** | New tab in `DataHubWidget` powered by `SkavaSearchPanel` (`src/gui/`) + `SkavaClient` (`src/skava/`). The panel queries `GET /discovery/search` on the configured SKAVA base URL with ObsCore-style filters (POS=CIRCLE, BAND in metres, TIME in MJD, COLLECTION, DPTYPE, obs_id) and presents results in a sortable table with replica info, best-node latency and a provenance panel (DOI / PID / citation / license). On **Open**, the panel resolves `GET /datalink/{obs_id}` to extract `primary_access.access_url`, then forwards it to `MainWindow::doOpenDataFromUrl()`. | Refactored `doOpenDataPath` into `openViewerFromOpenedDataset(path, opened, client)` so the same viewer-creation logic serves both local-path opens and SKAVA URL opens. |
| **Open-from-URL backend** | New endpoint `POST /v1/datasets/open_url` (`backend/app/routers/datasets.py`). Takes `{url, obs_id?, bearer_token?}`, downloads the remote FITS / HDF5 to `<exports>/skava_cache/<basename>.<sha256>.<ext>` via streamed `urllib`, classifies it via the standard `_fits_metadata` (so dynspec / PSRFITS / HDF5 sidecar paths kick in automatically), registers it as a session dataset and returns the same `OpenDatasetResponse` shape as `/v1/datasets/open`. | Idempotent: a cache hit (by URL hash) skips the download. `bearer_token` is forwarded as `Authorization` on the storage-node request when the access endpoint requires auth. |
| **Dynamic-spectrum / beamformed mode** | `vtkWindowImage::setDynamicSpectrumMode(true)` is called by `MainWindow::doOpenDataPath()` whenever the backend reports `kind='dynspec'`. The flag suppresses every celestial-WCS-dependent piece of UI: catalogue overlay, beam indicator, Stokes Analysis card, WCS frame switch, sanity panel's RA/Dec checks. Axis titles fall through `remoteOverlayAxisTitle()` to a fixed *Time* / *Frequency* pair. The **Time-Series Tools** sidebar card surfaces three workflow buttons (dedispersion, pulse-profile folding, RFI σ-clip) backed by the `/v1/dynspec/*` endpoints. Sidebar cards to hide/show are tracked via `m_dynspecHiddenCards` (populated at `setupSidebar()` build time) so the toggle is fully reversible. | The HDF5 / PSRFITS sidecar approach means preview/full image readers, region stats, contour pipeline, etc. needed **zero** changes — the dynspec dataset is just an ordinary 2-D FITS with `CTYPE='TIME'/'FREQ'`. The pulse-profile dialog is a stand-alone QCustomPlot QWidget (window), not a sidebar widget, so it can coexist with the main viewer. |
| **Preview → full-res swap (no freeze)** | `vtkWindowImage::applyRemoteMasterLayer()` mirrors the cube viewer's anti-stall pattern from `applyCubeOpenResult`: the cheap data swap (master-layer update, NaN colour) runs synchronously, then the **expensive parts** are deferred into a `QTimer::singleShot(50, …)` that first calls `QApplication::processEvents(ExcludeUserInputEvents)` to flush pending paint / status events before the GPU upload blocks the thread. Three additional optimisations brought the apparent freeze on a 7500×7500 image from ~26 s to ~2 s: **(1)** `computeBlankFraction()` is now stride-sampled to ~250 k samples instead of scanning all 56 M pixels via the virtual `GetScalarComponentAsDouble`. **(2)** `updateSanityPanel()` and `updateDataStatePanel()` (which call `computeBlankFraction`) are moved into the deferred block. **(3)** `setupContourPipeline()` — which runs `vtkFlyingEdges2D` with 15 levels over the full image (10–20 s on 56 M pixels) — is no longer called eagerly on the swap. Instead a `m_contourPipelineDirty` flag is set, and the rebuild happens lazily inside `setContoursVisible(true)` the first time the user actually asks for contours. | Visible contours stay valid: when `m_contoursVisible` is true at swap time, the pipeline is still rebuilt eagerly in the deferred block. The dirty flag also flips back to true on every subsequent swap so the rebuild always uses fresh data. |
| **Annotations** | Text (`vtkTextActor`) and arrow overlays placed interactively (crosshair click). Arrows use `buildArrowGeometry()` (shaft + fixed-size 8 px arrowhead, capped at 30 % of shaft). Arrow placement shows a live preview via `m_annotPreviewActor` updated in `mouseCallback()`. Save/Load via native `QFileDialog`. | `AnnotPlaceMode` state machine: `None → Text` (1 click) or `None → ArrowTip → ArrowLabel` (2 clicks). Right-click cancels. |
| **Blink / Compare** | `m_blinkTimer` toggles visibility of layers 0 and 1 at `m_blinkIntervalMs` (50–1000 ms slider). Requires ≥ 2 layers; `toggleBlink(false)` restores all layers visible. | `getLayerActor(int)` pass-through added to `LayerListModel`. |
| **Contour Phase 2 (cube → image)** | `vtkWindowCube` emits `contourDataReady(label, vtkImageData*)` via *Tools → Send Slice to Image Viewer*. `MainWindow` relays to the first open `vtkWindowImage` via `overlayExternalContourData()`, which runs `vtkFlyingEdges2D` on the received data and adds the result as an external contour layer. | No WCS reprojection — assumes shared pixel grid (same dataset). |
| **Stokes analysis** | `StokesRole` enum + `QHash<int, StokesRole>` map associates layer indices with their I/Q/U/V/derived role. `findStokesCompanionPath()` greps for sibling files matching common naming patterns (`StokesI` → `Q/U/V`, `_I.fits` → `_Q.fits`, ASKAP/MeerKAT `_PB` variants, `.fits.gz`). Derived maps `P = √(Q²+U²)`, `PA = ½·atan2(U,Q)`, `P/I` are computed in-place on `vtkImageData` via `computeStokesP/PA/Fractional()` and added as layers via `addDerivedLayer()`. | Master layer NaN colour set to transparent for radio mosaic convention. |
| **Debiased P** | `computeStokesPdebiased()` estimates per-channel σ as `½·(σ_MAD(Q) + σ_MAD(U))` over all valid pixels (not robust to bright spatial features, intentional — meant as a quick field-average), then `P_deb = √(max(0, Q²+U²−σ²))`. Result tagged `StokesRole::DerivedPdb`. | Label includes the σ value used for traceability. |
| **Spectral index** | `computeSpectralIndex()` opens a `QDialog` (combos + line edits) to pick two layers and their frequencies, then computes `α = log(S_B/S_A)/log(ν_B/ν_A)` per pixel. Display LUT clamped to [−2.5, +1.5] (synchrotron + thermal range). | Both layers must share extent — explicit check, no resampling. |
| **Faraday RM** | `computeFaradayRM()` opens a `QTableWidget`-based dialog where the user adds ≥ 3 (Q layer, U layer, ν GHz) triplets. Per pixel: PA_i = ½·atan2(U_i, Q_i), λ_i² = (c/ν_i)², then closed-form linear fit slope = `Σ(λ_i²−λ̄²)(PA_i−PA̅) / Σ(λ_i²−λ̄²)²`. Result clamped to ±500 rad m⁻² for display. | **No PA unwrapping** — valid only in the low-RM regime. Documented limitation. |
| **Polarization vector overlay** | `rebuildPolarizationVectors()` samples Q and U on a configurable grid, computes per-pixel PA and P, filters by MAD-σ × SNR threshold, draws line segments in `m_polVecActor` (`vtkPolyData` + `vtkLineSource` family). Sidebar controls: grid step, SNR threshold, length scale. | Pixel-grid orientation assumes N-up E-left (standard FITS); rotated WCS would need a correction matrix. |
| **Radio region stats** | `RegionStatistics` extended with `sum` and `madSigma` (`1.4826 × MAD(values − median)`). `analyzeCurrentRegion()` adds a **Radio (beam-aware)** section computing `Ω_beam_px = π·BMAJ·BMIN/(4·ln 2)` (from `m_beamMajorDeg/MinorDeg` stored in `setBeamInfo()`) and the integrated flux `Jy = sum / Ω_beam_px`. When any Stokes role layers are loaded, a **Stokes (per-layer)** section re-runs stats on each role's data and reports the appropriate scalar (Jy / degrees / %). | Median-MAD σ replaces std-dev for fields with bright sources. |

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

**Parameter pickers — modal** (`exec()`), because they answer one question and
return: Moment Map Settings (`MomentMapController`), the noise region
(`NoiseRegionDialog`), the tilted-ring model, Export Movie, Export Sub-Cube,
Spectral Lines, Color Maps. These are built inline where they are used and still
use a bare `QDialogButtonBox`; two of them relabel the primary (*Export…*,
*Overlay*) and the rest read *OK*. **Known incoherence**, not yet unified: they
should take the theme's primary/secondary buttons and a verb label like the
family above.

Window titles are Title Case throughout ("Stack Spectral Cubes", "Color Maps",
"Spectral Lines" were the three that were not, and are now).

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
| `test_backendclient.cpp` | 30 | `parseMomentResultObject`, `parsePvResultObject`, `parseNoiseResultObject`, open-response parsing |
| `test_catalogue_parser.cpp` | 35 | `detectedRedshiftField`, `detectedDistanceField`, `comovingDistanceMpc`, `entryDistanceMpc` |
| `test_backend_routing.cpp` | 13 | `skava::BackendRouting` endpoint selection |
| `test_image_lod.cpp` | 21 | tile pyramid / level-of-detail arithmetic |
| `test_analysis_param_store.cpp` | 8 | per-dataset dialog parameter persistence |
| `test_backend_contract.cpp` | 4 | shared header manifest ↔ client constants |
| `test_remote_slice_cache.cpp` | 11 | slice cache keying + eviction |
| `test_linked_window_registry.cpp` | 8 | linked-views registry lifetime |
| `test_spectral_unit_convert.cpp` | 22 | unit parsing/conversion **and** rest-frame → axis placement (`axisKindFromCtype`, `observedFrequencyHz`, `axisValueForFrequency` for VOPT/VRAD/VELO/FREQ) |
| `test_cube_region_geom.cpp` | 17 | pure ROI hit-testers (box / circle / polygon / annulus) |
| `test_cube_tool_gate.cpp` | 11 | `CubeToolGate` — which tools may act, view gates vs state gates |
| `test_spectral_axis_infer.cpp` | 16 | spectral-axis descriptor inference from CTYPE/CUNIT |
| `test_kinematic_level_field.cpp` | 19 | kinematic-lasso level field |
| `test_ray_voxel_pick.cpp` | 18 | 3-D ray → voxel picking |

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
