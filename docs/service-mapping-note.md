# Service Mapping Note

## Purpose
This note documents the current service boundaries, their implementation status, and where the client/backend split is clean vs. where gaps remain.

---

## `BackendClient`

### Status
**Implemented and stable.**

### Contract
- Synchronous REST client; all methods block until the HTTP response arrives
- Called exclusively from `QtConcurrent::run` worker threads
- One instance per async operation (no shared state between concurrent calls)

### Covered operations (by tag)
| Tag | Methods |
|-----|---------|
| meta | `health()`, `listSessionDatasets()` — enumerate datasets currently open in a session (drives the StackDialog cube selector and any other cross-dataset tool) |
| files | `listFiles()`, `fileHeader()` |
| datasets | `openDataset()` |
| catalogue | `openCatalogue()`, `requestCatalogueSubset()`, `queryTabularCatalogue()` |
| vbt | `openVbt()`, `requestVbtSubset()`, `queryTabularVbt()` |
| cube | `requestPreview()`, `requestSlice()`, `requestSubvolume()`, `requestPv()`, `requestNoise()` |
| products | `requestMoment()`, `requestIsosurface()` |
| tasks | `createMomentTask()`, `createPvTask()`, `requestTaskStatus()`, `waitForTaskCompletion()` |
| image | `requestImagePreview()`, `requestImage()` |
| cosmology | `requestCosmologyDistanceBatch()` |
| hips | `openHiPS()`, `requestHiPSAllsky()`, `requestHiPSTile()`, `requestHiPSTilesForView()`, `requestHiPSCatalogueOverlay()` |
| resolve | `resolveTarget()` |
| samp | `requestSampStatus()`, send / receive / inbox / files-register / import-url / upload-file / send-fits / send-catalogue (delegated to `SAMPClient` in `src/app/`) |
| spectral | `requestLinewidthBinary()`, `requestBaselineSubtract()`, `requestSpectralStackBinary()` (S-02 / S-03 / S-04) |

### Token resolution
1. Explicit `setToken()` / `Settings`
2. `~/.visivo_token` written by the backend at startup

### Session management
`openDataset()` stores `session_id`; echoed as `X-Visivo-Session` in all subsequent calls for that dataset. `openCatalogue()` and `openVbt()` return their own session IDs; viewers call `client.setSessionId()` accordingly.

### Pool-sharing contract (server-side)
The backend's single ProcessPoolExecutor is shared across all endpoints, but
its slots are reserved between interactive and heavy tiers so a long compute
never starves UI-driven calls:

- Interactive helpers (`_run`, `_run_with_limit`): preview / slice /
  subvolume / probe / noise / image.
- Heavy helpers (`_run_heavy`, `_run_heavy_with_limit`): moment / isosurface
  / pv / spectral linewidth / baseline / stack — gated by a global
  `asyncio.Semaphore` of size `max(1, VISIVO_WORKERS - 1)`.

This is transparent to `BackendClient`: it just makes synchronous HTTP
calls; the throttle lives entirely on the FastAPI side. See
`docs/async-patterns.md` for the full classification table.

---

## `DatasetOpenService`

### Status
**Local implementation, stable.**

### Contract
- `DatasetOpenRequest { filepath }`
- `DatasetOpenInfo` — dataset classification (image vs. cube, dimensions)
- `DatasetOpenService::inspect(const DatasetOpenRequest &)`

### Current local implementation
- `visivo_shared_core`; no Qt::Widgets, no VTK
- Input: local desktop FITS file path
- Uses `AstroUtils` to classify and inspect the file

### Future remote mapping
- Replace `filepath` with a backend `datasetId` or remote resource reference
- Return the same `DatasetOpenInfo` shape; the GUI must not depend on the inspection being local
- Natural bridge: call `BackendClient::openDataset()` and map the response into `DatasetOpenInfo`

### Gap before clean split
- `DatasetOpenRequest` still carries a local file path; no dataset-reference abstraction exists yet

---

## `ImageLayerImportService`

### Status
**Local implementation, stable. Remote layer path now also uses `loadImageLayer` directly.**

### Contract
- `ImageLayerImportRequest { baseDatasetPath, layerFilepath }`
- `ImageLayerImportResult`
- `ImageLayerImportService::inspect(const ImageLayerImportRequest &)`

### Current local implementation
- `visivo_shared_core`
- Validates WCS overlap and pixel-type compatibility between base dataset and candidate layer
- Uses `AstroUtils` on local FITS files

### Remote layer path (current)
"Add New FITS File" in `vtkWindowImage` no longer goes through `ImageLayerImportService`. It opens a `RemoteFileBrowserDialog`, gets a backend-side path, and calls `addLayerImage(path)` directly. The layer is then loaded by `loadImageLayer()` / `AstroUtils` / libwcs — the same pipeline used by the existing "Add New Layer" action and VLKB layers.

`ImageLayerImportService` is therefore used only for the legacy local-file path; all new remote/VLKB layer loading bypasses it.

### Future remote mapping
- Replace path-based request with dataset/layer reference identifiers
- Validation result shape stays the same
- Backend equivalent: validate via dataset metadata already known to the server

### Gap before clean split
- Request still carries two local file paths; no stable reference identity model

---

## `MomentProcessingService`

### Status
**Backend-authoritative facade, stable.**

### Contract
- `MomentRequest` — carries `datasetId`, moment order, channel range, RMS
- `MomentResult` — carries image payload, scientific metadata, WCS status
- `MomentProcessingService::computeMoment(const MomentRequest &)`

### Current implementation
- `visivo_shared_vtk`; wraps `BackendClient::requestMoment()` or `waitForTaskCompletion()`
- Authoritative computation always runs on the backend
- Result is a portable image payload + metadata; the client renders it locally

### Task-based path
For long-running moments, the client uses `createMomentTask()` + `waitForTaskCompletion()` via `BackendClient`. `RemoteMomentWindow` uses this path. `vtkWindowCube` uses the synchronous `requestMoment()` path.

### Gap before clean split
- Desktop-local FITS files must be manually opened on the backend first (via `DataHub` / `RemoteFileBrowserDialog`) before moment computation is possible
- No automated upload/staging pipeline for local files; this remains a manual step

---

## Tabular query / pagination

### Status
**Implemented and stable.**

### Contract (shared by catalogue and VBT)
```
BackendTabularQueryRequest {
    datasetId, limit, offset, filters[], sortField, sortDirection
}
BackendTabularQueryResponse {
    valid, error, totalRows, returnedRows, rows (catalogue), columns (VBT)
}
```

### Pagination state (per viewer)
| Field | Default | Reset on |
|-------|---------|----------|
| `m_pageSize` | 50 000 | never |
| `m_currentOffset` | 0 | `applyFilter()` |
| `m_totalCount` | -1 | `applyFilter()` |
| `m_loadingMore` | false | after watcher fires |

Filters are re-applied verbatim on each page (same `filters[]` vector, different `offset`).

---

## Cosmology distance service

### Status
**Implemented and stable.**

### Contract
- Single: `POST /v1/cosmology/distance` — one redshift → distance_Mpc
- Batch: `POST /v1/cosmology/distance/batch` — `redshifts[]` + `model` → `distances_Mpc[]`
- Client: `BackendClient::requestCosmologyDistanceBatch(redshifts, model)`
- Result: `BackendCosmologyBatchResult { valid, error, model, distancesMpc[] }`

### Client-side integration
- `vtkWindowCatalogue3D` uses the batch endpoint for Planck15/13/WMAP9 models (async via `m_cosmologyWatcher`)
- Planck18 uses the local `Catalogue3DParser::detail::comovingDistanceMpc()` integration; no network call
- Distances are stored as per-entry override (`Catalogue3DEntry::distanceMpc`); `entryDistanceMpc()` applies the priority chain transparently

---

## HiPS service

### Status
**Implemented and stable.**

### Contract
- `openHiPS(url)` → `BackendHiPSSurveyInfo`
- Tile fetch: `requestHiPSAllsky()`, `requestHiPSTile()`
- View-aware tile set: `requestHiPSTilesForView()`
- Catalogue overlay: `requestHiPSCatalogueOverlay()`
- Name resolution: `resolveTarget()` → `BackendTargetResolveResult`

### Client-side integration
`HiPSWindow` / `HiPSViewportWidget` manage the tile cache and compositing entirely client-side. The backend is responsible only for computing which tiles are needed and serving the tile bytes.

---

## Auth services

### Backend token (simple bearer)
Managed by `BackendClient`. Token is read from `~/.visivo_token` or set explicitly from `SettingsDialog`. **No service boundary gap** — token handling is complete.

### VLKB OIDC
Managed by `AuthWrapper` + `OIDCAuthorizationCodeFlow`. Flow: browser-based PKCE → `localhost` redirect → `HttpServerReplyHandler`. Custom `visivo://` scheme handled by `VisIVOUrlSchemeHandler`.

Gap: tokens are in-process only; no keychain integration.

---

## Diagnostics

### Status
**Implemented and stable.**

### Contract
```cpp
DiagnosticsManager::instance()->publish(
    level, category, source, message,
    datasetId, sessionId, operationTag
);
```

All backend-calling code publishes structured entries before and after operations. `DiagnosticsWindow::showSingleton()` exposes a process-wide window, reachable from:
- *View → Diagnostics* in `MainWindow`
- *View → Diagnostics* in child windows (`vtkWindowCube`, `vtkWindowImage`)
- *Command Palette → "Open Diagnostics Window"* (⌘K from any window)

No external sink (file, remote log collector) is implemented yet.

---

## Summary

| Service | Local impl | Remote impl | Gap |
|---------|-----------|-------------|-----|
| `BackendLauncher` | ✓ (process mgmt) | — | none |
| `StartupDialog` | ✓ (startup UX) | — | none |
| `DatasetOpenService` | ✓ | — | local path; no dataset-ref model |
| `ImageLayerImportService` | ✓ (legacy local) | ✓ (remote browse → loadImageLayer) | service not used for remote path |
| `MomentProcessingService` | — | ✓ (backend-auth) | no local staging pipeline |
| `BackendClient` (all endpoints) | — | ✓ | none |
| Tabular query + pagination | — | ✓ | none |
| Cosmology distance | local (Planck18) | ✓ (batch) | none |
| HiPS tiles | — | ✓ | none |
| Auth (bearer token) | ✓ | — | none |
| Auth (OIDC VLKB) | ✓ | — | no keychain |
| Diagnostics (in-process) | ✓ | — | no external sink |
| Loading placeholder | ✓ (all 4 viewers) | — | none |
