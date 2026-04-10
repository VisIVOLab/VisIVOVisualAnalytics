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
| `previewWatcher` | `QFutureWatcher<CubeOpenPreviewTask::Result>` | initial cube open |
| `fullResWatcher` | `QFutureWatcher<…>` | after preview succeeds |
| `momentWatcher` | `QFutureWatcher<MomentMapComputeTask::Result>` | moment order / channel range change |
| `pvWatcher` | `QFutureWatcher<…>` | PV polyline drawn by user |

Open sequence:
```
open() → previewWatcher → applyPreview()
                        ↘ fullResWatcher → applyFullRes()
```
If preview fails → synchronous full open fallback.

### `vtkWindowImage`

| Watcher | Type | Triggered by |
|---------|------|--------------|
| `layerWatcher` | `QFutureWatcher<ImageLayerLoadTask::Result>` | "Add layer" action |

### `vtkWindowCatalogue3D`

| Watcher | Type | Triggered by |
|---------|------|--------------|
| `filterWatcher` | `QFutureWatcher<Catalogue3DFilterTaskResult>` | "Apply" button or initial open |
| `m_loadMoreWatcher` | `QFutureWatcher<Catalogue3DFilterTaskResult>` | "Load more" button |
| `m_cosmologyWatcher` | `QFutureWatcher<Catalogue3DCosmologyTaskResult>` | cosmology model combo change |

### `vtkWindowVbt`

| Watcher | Type | Triggered by |
|---------|------|--------------|
| `filterWatcher` | `QFutureWatcher<VbtFilterTaskResult>` | "Apply" button or initial open |
| `m_loadMoreWatcher` | `QFutureWatcher<VbtFilterTaskResult>` | "Load more" button |

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
| `vtkWindowCube` | `applyPreview()`, `applyFullRes()`, `applyMomentMapResult()` |
| `vtkWindowImage` | `applyLoadedLayer()` |
| `vtkWindowCatalogue3D` | inline in watcher lambda: calls `buildScene()`, `buildLabels()`, `refreshCatalogueTable()` |
| `vtkWindowVbt` | inline in watcher lambda: calls `buildPointCloud()`, `updateColorMapping()` |

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
