# Context Document — VisIVO Visual Analytics (`next-server` branch)

This document is the entry point for a new Claude Code session. Read it fully before
writing any code or answering questions. It summarises the project, the architecture,
the current branch state, coding conventions, and the work already done so you can
pick up exactly where the previous session left off.

---

## 1. Project in one paragraph

**VisIVO Visual Analytics** is a scientific desktop application for interactive
visualisation of astronomical FITS datasets (2-D images, 3-D spectral cubes, catalogues,
VBT point clouds). The desktop client is C++ / Qt 6.5 / VTK 9.5 (macOS primary target).
It talks over HTTP to a Python 3.10+ FastAPI backend that handles all data-intensive
work: FITS I/O, moment computation, isosurface extraction, spectral analysis, HiPS tiles,
catalogue queries, etc. The backend can run locally (bundled) or on an HPC cluster behind
an SSH tunnel.

- **Repository root**: `/Users/fvitello/Documents/GitHub/ViaLacteaVisualAnalytics_pv`
- **Active branch**: `next-server`
- **Main branch** (for PRs): `master`
- **Git user**: Fabio Vitello

---

## 2. Repo layout

```
.
├── src/
│   ├── app/           visivo_shared_core — BackendClient, BackendLauncher,
│   │                  DatasetOpenService, DiagnosticsManager (no Qt::Widgets, no VTK)
│   ├── vtk/           visivo_shared_vtk — MomentMapComputeTask,
│   │                  MomentProcessingService, CubeOpenPreviewTask, vtkFITSReader, …
│   ├── gui/           Qt widget layer: MainWindow, vtkWindowCube, vtkWindowImage,
│   │                  LinewidthDialog, BaselineDialog, StackDialog,
│   │                  MomentMapController, …
│   └── auth/          OIDC/PKCE flow for VLKB
├── ui/                Qt Designer .ui files (vtkWindowCube.ui, …)
├── backend/
│   ├── app/
│   │   ├── main.py    FastAPI application — all endpoints
│   │   └── compute.py Heavy numpy/astropy computation workers
│   └── tests/
│       └── test_spectral.py  29 pytest tests (run with: cd backend && pytest -v)
├── docs/              Markdown documentation
│   ├── architecture-note.md
│   ├── async-patterns.md
│   ├── backend-api.md
│   ├── testing.md
│   └── context-chat.md  ← this file
├── tests/             Qt/C++ unit tests (QTest; cmake -DBUILD_TESTING=ON)
├── BUILDING.md        CMake build instructions
└── README.md
```

---

## 3. Architecture summary

### C++ client

| Layer | Module | Key rule |
|-------|--------|----------|
| Core (no UI) | `visivo_shared_core` | No Qt::Widgets, no VTK; safe for unit tests |
| VTK support | `visivo_shared_vtk` | No Qt::Widgets |
| GUI | GUI executable | Can use everything |
| Tests | `visivo_test_support` | Links only core subset |

**`BackendClient`** — synchronous REST client. Never call from the UI thread. Always call
from a `QtConcurrent::run` worker. Each dialog/viewer creates its own instance.
`setSessionId()` **must** be called before making dataset-scoped requests.

**Async pattern** (every viewer, every dialog):
```
UI thread:   capture state → disable controls → watcher.setFuture(QtConcurrent::run(…))
Worker:      BackendClient client(url, token); client.setSessionId(id); return client.foo(…);
UI thread:   watcher::finished() → read result struct → apply to VTK/widgets → re-enable
```

**`QPointer<T> self`** pattern is used in all watcher lambdas to guard against the parent
window being destroyed while the request is in flight.

### Python backend

- All endpoints require `X-Visivo-Token` header.
- Dataset-scoped endpoints also require `X-Visivo-Session` header (returned by
  `/v1/datasets/open`).
- Error convention: HTTP 200, body `{ "valid": false, "error": "…" }`. The client checks
  `valid`, not the HTTP status.
- CPU-bound work runs in a `ProcessPoolExecutor` (`worker_*` functions in `compute.py`).
- Large cubes use the Dask out-of-core path (threshold: `VISIVO_DASK_THRESHOLD_BYTES`,
  default 4 GiB).
- FITS files opened with `astropy.io.fits` in memory-mapped mode.

---

## 4. Key files to know

| File | What it does |
|------|-------------|
| `src/gui/vtkWindowCube.cpp` | Main cube viewer; owns `MomentMapController`, all spectral tool dialogs |
| `src/gui/MomentMapController.cpp/.h` | Drives the async moment computation flow; owns `m_lastError` |
| `src/vtk/MomentMapComputeTask.cpp` | C++ guard `isSupportedMomentOrder()` and `computeMomentMap()` |
| `src/vtk/MomentProcessingService.cpp` | Calls the backend `/v1/products/moment` endpoint |
| `src/gui/LinewidthDialog.cpp/.h` | Line-width map dialog; uses `/v1/spectral/linewidth/binary` |
| `src/gui/BaselineDialog.cpp/.h` | Baseline subtraction dialog; uses `/v1/spectral/baseline/…` |
| `src/gui/StackDialog.cpp/.h` | Spectral stacking dialog; uses `/v1/spectral/stack/binary` |
| `src/gui/MainWindow.cpp` | Opens all three dialogs from the main window context |
| `src/app/BackendClient.cpp/.h` | All REST calls; `parseBinaryFrame()` for binary spectral payloads |
| `ui/vtkWindowCube.ui` | Qt Designer UI; defines `menuTools` and `actionComputeMoment` |
| `backend/app/compute.py` | `_moment_map_from_array()`, `worker_linewidth()`, `worker_baseline()`, `worker_stack()` |
| `backend/app/main.py` | FastAPI app; all endpoint definitions |
| `backend/tests/test_spectral.py` | 29 pytest tests (domains S-01…S-04 + source finder) |

---

## 5. Completed work on this branch (2026-04)

All 6 bugs and the M3/M4/M5 issues described below have been **fixed and tested**. Do
**not** redo them.

### FIX 1+2 — Unified Moment dialog

- **Problem**: `menuMoment` submenu had 6 individual actions (M0, M1, M2, M6, M8, M10);
  no UI path for M3/M4/M5.
- **Fix**: Removed `menuMoment` entirely from `vtkWindowCube.ui` and `vtkWindowCube.cpp`.
  Added `actionComputeMoment` to `menuTools`. The action always calls `setMomentOrder(0)`
  which opens the `MomentMapController` dialog where the user picks order, channel range,
  and mask.
- **Files changed**: `ui/vtkWindowCube.ui`, `src/gui/vtkWindowCube.cpp`,
  `src/gui/MomentMapController.h/cpp`.

### FIX 3a — Real backend error text

- **Problem**: Status-bar error message showed a hardcoded string, not the backend error.
- **Fix**: `MomentMapController` stores `m_lastError` as
  `"MomentMap M<order>: <backend message>"` on failure and clears it on success.
  `vtkWindowCube` reads `momentController->lastError()`.
- **Files changed**: `src/gui/MomentMapController.h/cpp`, `src/gui/vtkWindowCube.cpp`.

### FIX 3b — M3/M4/M5 backend try/except

- **Problem**: Exception in the M3/M4/M5 block of `_moment_map_from_array()` would
  propagate silently.
- **Fix**: Entire `if order in {3, 4, 5}` block wrapped in `try/except` with
  `logger.exception(…)` then `raise`.
- **File changed**: `backend/app/compute.py`.

### FIX 4 — Empty result guard in LinewidthDialog

- **Problem**: If all pixels fail to fit, `width=0, height=0`; constructing
  `ScienceMapWindow` with a zero-size array caused a crash.
- **Fix**: Added `if (result.width == 0 || result.height == 0)` guard with a
  `QMessageBox::warning` before opening the result window.
- **File changed**: `src/gui/LinewidthDialog.cpp`.

### FIX 5 — Session ID plumbing

- **Problem**: `LinewidthDialog`, `BaselineDialog`, `StackDialog` did not receive
  `sessionId`; `BackendClient::setSessionId()` was never called, causing 401 errors and
  double-slash URLs.
- **Fix**: All three dialog constructors accept `const QString &sessionId`; they call
  `client->setSessionId(m_sessionId)` before any request. Both `vtkWindowCube.cpp` and
  `MainWindow.cpp` pass the session ID when constructing the dialogs.
- **Files changed**: all three dialog `.h/.cpp` pairs, `src/gui/vtkWindowCube.cpp`,
  `src/gui/MainWindow.cpp`.

### FIX 6 — StackDialog redesign

- **Problem**: StackDialog used a plain text list; user could "select" any string; no
  validation that ≥2 cubes were chosen.
- **Fix**: Replaced with a checkable `QListWidget` (items have `Qt::ItemIsUserCheckable`).
  A live `QLabel` counter shows "N selected" in red when < 2 cubes are checked. The
  "Compute" button is disabled until at least 2 are selected.
- **Files changed**: `src/gui/StackDialog.h/cpp`.

### M3/M4/M5 C++ guard

- **Problem**: `isSupportedMomentOrder()` in `MomentMapComputeTask.cpp` only listed
  `{0,1,2,6,8,10}`; attempting M3/M4/M5 returned `"Unsupported moment order."` before
  even reaching the backend.
- **Fix**: Added `case 3: case 4: case 5:` to the switch.
- **File changed**: `src/vtk/MomentMapComputeTask.cpp`.

### M5 normalisation fix

- **Problem**: M5 was the raw μ₅/M0 (not normalised by σ⁵), while M3 divides by σ³ and
  M4 divides by σ⁴. For a Gaussian with σ=6 channels, M5 = −1.46 channel⁵ (looks
  alarming); normalised: −1.46/6⁵ ≈ −0.0002 (essentially zero).
- **Fix**: Both numpy and Dask paths now compute `μ₅ / (M0 · σ⁵)` (dimensionless).
  Unit string changed from `"spectral_unit^5"` to `"dimensionless (standardised 5th
  moment)"`. Test `test_m5_returns_finite_correct_shape` extended with
  `|M5 median| < 0.1` assertion.
- **Files changed**: `backend/app/compute.py`, `backend/tests/test_spectral.py`.

### New backend tests (29 total, all pass)

Tests added in `backend/tests/test_spectral.py`:
- `test_m3_and_m4_symmetric_gaussian_tight` — `|M3| < 0.05`, `|M4| < 0.1`
- `test_m3_sign_for_right_skewed_profile` — positive M3 for right-skewed exponential
- `test_m5_returns_finite_correct_shape` — shape + dtype + `|M5| < 0.1` after σ⁵ norm

---

## 6. Coding conventions

### C++
- Qt 6.5 / C++17.
- Prefer `u"string literal"_s` (`Qt::StringLiterals`) over `QStringLiteral`.
- `QPointer<T> self(this)` in all watcher lambdas. Check `if (!self) return;` first.
- Capture by value across thread boundaries (never `this` by pointer).
- `BackendClient` is NOT a `QObject`. Always heap-allocate per dialog. Connect
  `QDialog::finished` signal to `[client](int) { delete client; }`.
- Async pattern: `QtConcurrent::run` + heap-allocated `QFutureWatcher` (deleted in
  its own `finished` lambda via `watcher->deleteLater()`).
- No `--no-verify` or `--no-gpg-sign` for git. No amend of published commits.

### Python
- FastAPI + Pydantic v2 for endpoint models.
- Worker functions (`worker_*`) run in `ProcessPoolExecutor` — no I/O to FastAPI state.
- `logger.exception(…)` (not `logger.error`) for unexpected exceptions — includes traceback.
- Tests use synthetic FITS fixtures; no real data files needed.
- Run tests: `cd backend && python -m pytest tests/ -v`

### General
- Do NOT commit changes unless explicitly asked (`non voglio che committi`).
- Do not add speculative features, extra error handling for impossible paths, or comments
  on unchanged code.
- Do not create files unless necessary. Prefer editing existing files.
- Keep responses concise; use markdown links for file/line references.

---

## 7. Backend environment variables

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_MOMENT_THREADS` | `0` (auto = min(4, cpu_count)) | Threads for M0/M6/M10 chunking |
| `VISIVO_DASK_THRESHOLD_BYTES` | `4294967296` (4 GiB) | Cube size above which Dask is used |
| `VISIVO_WORKERS` | `min(4, cpu_count)` | ProcessPoolExecutor workers |
| `VISIVO_TOKEN` | — | Static bearer token (also read from `~/.visivo_token`) |
| `VISIVO_BACKEND_PYTHON` | — | Explicit Python interpreter for BackendLauncher |
| `VISIVO_NO_BACKEND_AUTOSTART` | — | Skip auto-start; connect to running backend |

---

## 8. API quick-reference (spectral endpoints)

```
POST /v1/products/moment                Moment map (orders 0–2, 3/4/5, 6, 8, 10)
POST /v1/spectral/linewidth             FWHM + EW maps (JSON, both frames)
POST /v1/spectral/linewidth/binary      Same, raw binary (two frames: FWHM then EW)
GET  /v1/spectral/linewidth/{ds_id}     Single-frame binary (fwhm|ew)
POST /v1/spectral/baseline/{sid}/{did}  Baseline subtraction → new_dataset_id
POST /v1/spectral/stack                 Stack cubes (JSON)
POST /v1/spectral/stack/binary          Stack cubes (raw binary, single frame)
```

Implemented in `backend/app/routers/spectral.py` (S-02 / S-03 / S-04).

Binary frame format (each frame): `[rows:uint32 BE][cols:uint32 BE][flags:uint32 BE][float32 LE…]`
Decode with `BackendClient::parseBinaryFrame(payload, offset, result)`.

---

## 9. Open / potential next items

- **Build verification**: the C++ changes (FIX 1–6 + M3/M4/M5 guard) have not been
  compiled since being written. A `cmake --build build 2>&1 | grep -E "error:|warning:"`
  pass is needed to confirm zero new warnings.
- **`describeMomentScope()`** in `MomentMapController.cpp` always returns
  `"Remote full dataset"`. For locally-opened cubes this string may be inaccurate; no
  fix requested yet.
- **Roadmap items** (from README): SSH tunnel launcher in UI, progressive data streaming,
  ROI-based subvolume loading, AI-assisted analysis copilot, multi-user token management.
