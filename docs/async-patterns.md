# Async Patterns

This document describes the async execution patterns used across the client GUI viewers.

---

## Core Pattern

All background work follows the same structure:

```
┌─ UI thread ──────────────────────────────────────────────┐
│  1. Capture state (urls, ids, params) into local vars     │
│  2. Disable controls                                      │
│  3. watcher.setFuture(QtConcurrent::run([captures](){…}))│
│  4. Return immediately                                    │
│                                                           │
│  ← QFutureWatcher::finished() fires on UI thread         │
│  5. Read result struct                                    │
│  6. Apply to VTK pipeline / widget state                  │
│  7. Re-enable controls                                    │
└───────────────────────────────────────────────────────────┘

┌─ Worker thread (QtConcurrent thread pool) ────────────────┐
│  BackendClient client(url, token);                        │
│  client.setSessionId(sessionId);                          │
│  const auto result = client.someMethod(…);                │
│  return result;   // POD / Qt value types only            │
└───────────────────────────────────────────────────────────┘
```

**Rules**:
- Workers never touch Qt widgets or VTK objects
- Workers always create their own `BackendClient` instance
- Result structs contain only POD, Qt value types (`QString`, `QByteArray`, `std::vector<…>`)
- UI thread never calls `BackendClient` directly

---

## Watcher Inventory

### `vtkWindowCube`

| Watcher | Type | Triggered by |
|---------|------|--------------|
| `remotePreviewWatcher` | `QFutureWatcher<RemoteCubePreviewResult>` | initial cube open (downsampled preview) |
| `remoteHighResCubeWatcher` | `QFutureWatcher<RemoteCubeSubvolumeResult>` | after preview succeeds, or on Camera-ROI refinement |
| `remoteSliceFetchesInFlight` (set, multiple `QFutureWatcher` instances) | `QFutureWatcher<RemoteCubeSliceResult>` | slice slider change, prefetch of neighbour slices |
| `isosurfaceWatcher` | `QFutureWatcher<AsyncIsosurfaceResult>` | isosurface mode toggled / threshold edited |
| `cubeOpenWatcher` | `QFutureWatcher<CubeOpenStageResult>` | legacy local FITS open path (unused in backend-authoritative mode) |

Long-running tools are delegated to controllers (each owns its own watcher):

| Controller | Watcher(s) | Triggered by |
|------------|------------|--------------|
| `MomentMapController` | moment compute watcher | moment order / channel range change |
| `NoiseController` | noise watcher | noise region applied |
| `PvController` | PV watcher | PV polyline drawn by user |

Open sequence (backend-authoritative mode):
```
open() → remotePreviewWatcher → applyCubeOpenResult(preview)
                              ↘ requestHighResCube()
                                  → remoteHighResCubeWatcher
                                      → applyCubeOpenResult(full-res, preSanitized=true)
```
Both watchers converge on the same `applyCubeOpenResult()` apply seam — the second call
swaps the cube data and reuses the worker-side sanitization (no UI-thread rescan).

### `vtkWindowImage`

| Watcher | Type | Triggered by |
|---------|------|--------------|
| `layerLoadWatcher` | `QFutureWatcher<ImageLayerLoadResult>` | "Add New Layer" / "Add New FITS File" (remote browse) / VLKB `addLayerFromBackendPath` |
| `remoteImageWatcher` | `QFutureWatcher<ImageLayerLoadResult>` | initial remote image open (preview) |
| `remoteFullImageWatcher` | `QFutureWatcher<ImageLayerLoadResult>` | full-resolution upgrade after preview |

All three converge on `applyRemoteMasterLayer()` (for remote image watchers) or the `layerLoadWatcher` finished slot (for layer additions). All layer sources — local layer file dialog, remote `RemoteFileBrowserDialog`, and VLKB inventory — go through `addLayerImage()` → `loadImageLayer()` → `AstroUtils`/libwcs for correct WCS alignment.

Open sequence for remote images:
```
open() → remoteImageWatcher → applyRemoteMasterLayer()  [preview]
                             ↘ remoteFullImageWatcher → applyRemoteMasterLayer()  [full-res]
```

### `vtkWindowCatalogue3D`

| Watcher | Type | Triggered by |
|---------|------|--------------|
| `filterWatcher` | `QFutureWatcher<Catalogue3DFilterTaskResult>` | "Apply" button or initial open |
| `m_loadMoreWatcher` | `QFutureWatcher<Catalogue3DFilterTaskResult>` | "Load more" button |
| `m_reopenWatcher` | `QFutureWatcher<BackendCatalogueInfo>` | reopen with a refreshed session (after backend restart) |
| `m_cosmologyWatcher` | `QFutureWatcher<Catalogue3DCosmologyTaskResult>` | cosmology model combo change |

### `vtkWindowVbt`

| Watcher | Type | Triggered by |
|---------|------|--------------|
| `filterWatcher` | `QFutureWatcher<VbtFilterTaskResult>` | "Apply" button or initial open |
| `m_loadMoreWatcher` | `QFutureWatcher<VbtFilterTaskResult>` | "Load more" button |
| `m_reopenWatcher` | `QFutureWatcher<BackendVbtOpenResult>` | reopen with a refreshed session |

---

## Pagination Detail

Applies to `vtkWindowCatalogue3D` and `vtkWindowVbt`.

### State variables

```cpp
int m_pageSize      = 50000;  // rows per fetch
int m_currentOffset = 0;      // rows already loaded
int m_totalCount    = -1;     // backend-reported total (-1 = unknown)
bool m_loadingMore  = false;  // guard: prevent re-entrant load-more
```

### Filter + first page

```
applyFilter() {
    m_currentOffset = 0; m_totalCount = -1;
    qreq.limit = m_pageSize; qreq.offset = 0;
    filterWatcher.setFuture(run(…));
}

filterWatcher::finished() {
    entries = result.entries;           // catalogue: replace
    subsetResult = result.subsetResult; // VBT: replace
    m_currentOffset = returned rows;
    m_totalCount = result.totalRows;
    btnLoadMore.setVisible(offset < total);
}
```

### Subsequent pages

```
loadMoreEntries() {
    if (m_loadingMore || filterBusy || offset >= total) return;
    m_loadingMore = true;
    qreq.limit = m_pageSize; qreq.offset = m_currentOffset;
    m_loadMoreWatcher.setFuture(run(…));
}

m_loadMoreWatcher::finished() {
    // Catalogue3D: append entries to this->entries
    entries.insert(end, result.entries.begin(), result.entries.end());

    // VBT: append column vectors
    for (auto &[field, vec] : result.subsetResult.columns)
        subsetResult.columns[field].insert(end, vec…);

    m_currentOffset = total loaded rows;
    m_totalCount = result.totalRows;
    m_loadingMore = false;
    btnLoadMore.setVisible(offset < total);
    rebuild scene / point cloud;
}
```

### Guard conditions

| Condition | Effect |
|-----------|--------|
| `filterBusy == true` | `loadMoreEntries()` returns immediately |
| `m_loadingMore == true` | `loadMoreEntries()` returns immediately |
| `m_currentOffset >= m_totalCount` | `loadMoreEntries()` returns immediately |
| `applyFilter()` called | resets `m_currentOffset = 0`, hides `btnLoadMore` |

---

## Task-based async (moment, PV)

For operations that may take minutes, the backend provides a task queue.

```
client.createMomentTask(…) → BackendTaskCreateResult { task_id }

client.waitForTaskCompletion(createResult, timeout, pollInterval)
    → BackendTaskStatusResult { status, result }

// waitForTaskCompletion polls GET /v1/tasks/{id} with exponential back-off
// until status == "done" or timeout expires
```

Used by:
- `RemoteMomentWindow` — exclusively task-based
- `vtkWindowCube` — uses synchronous `requestMoment()` for interactive moments; task path available for large cubes

---

## Apply seams (UI thread)

Each viewer has explicit "apply" methods that are called only from the UI thread after a watcher fires:

| Viewer | Apply method |
|--------|-------------|
| `vtkWindowCube` | `applyCubeOpenResult()` (preview AND full-res, branches on `result.preSanitized`), `applyRemoteSliceResult()`, `applyIsosurfaceResult()`, `applyMomentMapResult()` |
| `vtkWindowImage` | `applyRemoteMasterLayer()` (remote image), `layerLoadWatcher` finished lambda (layers) |
| `vtkWindowCatalogue3D` | inline in watcher lambda: calls `buildScene()`, `buildLabels()`, `refreshCatalogueTable()` |
| `vtkWindowVbt` | inline in watcher lambda: calls `buildPointCloud()`, `updateColorMapping()` |

The loading placeholder (`m_loadingActor`) is hidden inside the apply seam — always on the UI thread, always before `Render()` — so no synchronisation is required.

---

## Worker-side data preparation (cube swap)

For large data results (multi-GB cubes) the UI thread cannot afford to scan the
voxel buffer in the apply seam — that would freeze the app for seconds and
trigger the macOS beach-ball. The pattern is:

1. **Worker does all O(N) data work** before returning the result struct.
   For `fetchRemoteSubvolume()` this means:
   - Decode bytes → `vtkImageData` (raw float32).
   - One pass: scan all voxels to compute visible min/max, mean, rms, and
     count NaN/inf voxels (`sanitizeAndStatsCubeInPlace()`, two-pass: range
     pass + NaN-replace pass; combined to reduce three full scans to two).
   - Replace NaN/inf with a sentinel below min (so VTK opacity TF can hide it).
   - Compute `blankFraction = replaced / total`.
2. **Result struct carries derived stats** so the UI can skip the rescan:
   - `RemoteCubeSubvolumeResult` exposes `cubeRange` (visible-only range),
     `invisibleSentinel`, `cubeMean`, `cubeRms`, `blankFraction`.
   - When forwarded to `applyCubeOpenResult()` the call sets
     `preSanitized=true` on the `CubeOpenStageResult` it constructs.
3. **Apply seam is O(1) with respect to data size**:
   - `if (result.preSanitized) { adopt range + sentinel; }` — no rescan.
   - `currentCubeBlankFraction` is cached as a member; `updateSanityPanel()`
     consumes it directly without touching voxel data.
   - `computeBlankFraction()` itself was rewritten with a raw-pointer fast
     path for VTK_FLOAT single-component scalars (≈100× faster than the
     `GetScalarComponentAsDouble()` triple loop) — it's used only as the
     fallback when the apply seam runs with `preSanitized=false`
     (preview path; cubes are small there).

This pattern made the full-resolution swap go from ~10 s of beach-ball to
sub-50 ms of apply on a 512×512×200 float cube.

---

## Deferred render after data swap

The next pitfall after sanitization was the first `Render()` on the swapped
volume: `vtkGPUVolumeRayCastMapper` performs the texture upload + shader
compile synchronously on the UI thread the first time the volume mapper sees
new data. Doing this inside `applyCubeOpenResult()` produces a noticeable
freeze even when the apply itself is cheap.

The pattern:

1. **Cheap pipeline updates first** (data swap, mapper `Modified()`, UI text
   fields, slider sync) — synchronous, sub-100 ms.
2. **Defer the first Render via `QTimer::singleShot(50, …)`** so the Qt event
   loop pumps one tick before the GPU stall hits. Inside the lambda:
   - `QApplication::processEvents(QEventLoop::ExcludeUserInputEvents)` so the
     status bar ("Loading full resolution…") repaints before the stall.
   - The cube `Render()` (GPU upload happens here, inherently synchronous).
   - The slice `Render()` and the deferred slice fetch.
3. **Avoid stacking implicit renders** in functions invoked from the apply
   seam. `updateRemoteCuttingPlane()` used to call `Render()` internally and
   that triggered the GPU upload *before* the deferred singleShot — the
   internal Render was removed and all three callers (`applyCubeOpenResult`,
   `applyRemoteSliceResult`, `updateRemoteSliceDragFeedback`) now drive their
   own Render explicitly afterwards.
4. **Postpone secondary heavy compute** further out: e.g. an isosurface
   recompute scheduled after a full-res swap waits `QTimer::singleShot(500, …)
   → scheduleIsosurfaceRecompute()` so the GPU upload has settled before the
   iso fetch fires.

---

## Heavy-task throttle (backend-side)

The backend shares a single `ProcessPoolExecutor` of `VISIVO_WORKERS`
processes across **all** endpoints. Without a throttle, a long compute (a
moment map, an isosurface mesh, a linewidth fit, a baseline subtract, a
spectral stack) can occupy every pool slot and starve interactive requests
— even when the GUI itself is fully async, the user experiences a "frozen"
viewer because slice / probe / subvolume calls queue behind the heavy job.

The fix is a single global `asyncio.Semaphore` (`_HEAVY_SEM`) declared in
`backend/app/dependencies.py`, with capacity `VISIVO_HEAVY_SLOTS` =
`max(1, VISIVO_WORKERS - 1)`. Endpoints are explicitly classified:

| Tier | Helper (in `dependencies.py`) | What it adds over `_run` |
|------|-------------------------------|--------------------------|
| Interactive | `_run(fn, *args)` | nothing — pool directly |
| Interactive (per-session rate-limited) | `_run_with_limit(session, fn, *args)` | acquires per-session task slot (HTTP 429 on overflow) |
| Heavy | `_run_heavy(fn, *args)` | `async with _HEAVY_SEM` around `_run` |
| Heavy (per-session rate-limited) | `_run_heavy_with_limit(session, fn, *args)` | both of the above |

Classification:

| Endpoint | Helper | Tier |
|----------|--------|------|
| `POST /v1/cube/preview`           | `_run_with_limit` | interactive |
| `POST /v1/cube/slice`             | `_run_with_limit` | interactive |
| `POST /v1/cube/subvolume`         | `_run_with_limit` | interactive (user-driven; full-res ROI fetches expect snappy response) |
| `POST /v1/cube/noise`             | `_run_with_limit` | interactive |
| `POST /v1/image/full` / `/preview`| `_run_with_limit` | interactive |
| `POST /v1/cube/pv`                | `_run_heavy` (in `_pv_product_payload`) | heavy |
| `POST /v1/products/moment`        | `_run_heavy` (in `_moment_product_payload`) | heavy |
| `POST /v1/products/isosurface`    | `_run_heavy_with_limit` | heavy |
| `POST /v1/spectral/linewidth*`    | per-chunk `_run_heavy` from orchestrator | heavy (parallel chunks) |
| `POST /v1/spectral/baseline/*`    | `_run_heavy_with_limit` | heavy |
| `POST /v1/spectral/stack*`        | `_run_heavy_with_limit` | heavy |

**Guarantee**: with the default settings (`VISIVO_WORKERS=4`,
`VISIVO_HEAVY_SLOTS=3`), at most 3 heavy invocations occupy the pool at any
moment, so at least 1 worker is always available to serve a slice / probe /
subvolume / preview request immediately.

**Linewidth-specific note**: the linewidth orchestrator splits the row axis
into `_LINEWIDTH_PARALLEL_CHUNKS` (default = `VISIVO_WORKERS`) and submits
each chunk via `_run_heavy`. With the semaphore in place this fans out to
`_HEAVY_SLOTS` concurrent chunks; the remaining chunks queue behind the
semaphore, not in the pool itself, so interactive requests still bypass
them at the executor level.

Override via env:

```
VISIVO_WORKERS=8           # total pool size
VISIVO_HEAVY_SLOTS=6       # explicit cap on concurrent heavy tasks
VISIVO_HEAVY_SLOTS=$WORKERS  # disable the throttle (heavy can saturate pool)
VISIVO_LINEWIDTH_CHUNKS=2  # cap chunk fan-out independently
```

---

## Common pitfalls

### Do not capture `this` members by reference in worker lambdas
Capture by value: `const QString url = this->backendUrl;`

```cpp
// Wrong — 'this' may be deleted before worker completes
this->filterWatcher.setFuture(QtConcurrent::run([this]() {
    return BackendClient(this->backendUrl, …); // dangling if window closes
}));

// Correct — capture values
const QString url = this->backendUrl;
this->filterWatcher.setFuture(QtConcurrent::run([url, …]() {
    return BackendClient(url, …);
}));
```

### Do not touch VTK objects from the worker
VTK is not thread-safe. All `vtkNew`, `vtkSmartPointer`, renderer calls, etc. must happen on the UI thread in the apply seam.

### Do not call `QWidget` methods from the worker
Including `setText()`, `setEnabled()`, `update()`.

### The watcher `finished()` signal fires on the UI thread
Because `QFutureWatcher` uses `Qt::AutoConnection` and the receiver (`this`) lives on the UI thread.
