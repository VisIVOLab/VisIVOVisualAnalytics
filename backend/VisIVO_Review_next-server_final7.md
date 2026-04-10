# VisIVO Visual Analytics — Multidisciplinary Panel Review
## Branch: `next-server` · Commit: `ad93a9ed` · Date: 2026-04-10

**Panel:** Senior Astronomer · Senior Cosmologist · Senior Software Engineer · Senior HPC Expert

> **This document supersedes all previous review iterations.**
> It is the single authoritative record of the project's current state, covering every commit from the initial architecture refactor through `ad93a9ed`.

---

## 1. Executive Summary

Since the previous review (`61bb5884`) eleven commits have landed. The two remaining Medium-priority issues (unbounded overlay CSV cache, QFutureWatcher destructor blocking on close) have both been resolved. The principal theme of this cycle is rendering UI maturity: the cube viewer has gained dockable 3D/slice panels, a 3D View Settings sidebar page with live colour-map previews, volume blend-mode controls (Composite/MIP/MinIP), a 2D View Settings page with isolated slice and moment LUTs, a moment-card state machine with silent-failure detection, and an Info/Stats page exposing dataset metadata and WCS status. Build documentation (BUILDING.md) has also been added.

The architecture remains backend-authoritative and internally consistent. The primary new concern is that the UI feature additions have grown `vtkWindowCube.cpp` from 6 953 to 8 171 lines (+1 218), partially reversing the gains from the controller extraction cycle. All other open issues are Low severity.

---

## 2. Commits Under Review

| Hash | Summary | Δ Lines |
|------|---------|---------|
| `ac191eaa` | Use per-request QFutureWatcher and m_busy flag | +194 / −171 |
| `2f3eb079` | fix(backend): bound HiPS overlay CSV cache with lru_cache | +15 / −9 |
| `f7160f34` | feat(ui): make cube workspace dockable with resettable 3D and slice panels | +116 / 0 |
| `c469c9d3` | Add 3D View Settings UI and color map previews | +263 / −19 |
| `547a10ac` | Add volume rendering UI and blend modes | +139 / 0 |
| `cd174b38` | Refactor color map / surface color UI and logic | +52 / −49 |
| `6481b8ac` | Create BUILDING.md | +163 / 0 |
| `3f1e82f8` | Update README.md | minor |
| `6585c56d` | Add 2D View Settings sidebar and slice LUT support | +240 / −8 |
| `e6d6c00a` | feat(ui): improve 2D moment mode flow and add dedicated moment LUT | +159 / −11 |
| `80a146a5` | Improve moment computation UI and diagnostics | +157 / −10 |
| `ad93a9ed` | Add Info/Stats page and refine Moment Map UI | +316 / −55 |

---

## 3. Resolved Issues from Previous Review

### M1 — `_OVERLAY_CSV_CACHE` unbounded

Resolved in `2f3eb079`. The global dict replaced with `@functools.lru_cache(maxsize=32)` on a new `_load_overlay_csv(path: str) -> pd.DataFrame` helper. This is the idiomatic Python pattern: the LRU eviction is handled transparently by the standard library, the maxsize=32 cap prevents unbounded growth, and the added `clear_overlay_cache()` function (wrapping `_load_overlay_csv.cache_clear()`) provides an explicit invalidation path for future use. The surrounding `_catalogue_overlay_sync` function is simplified by removing the manual cache lookup/insert logic.

### M2 — `QFutureWatcher` member destructor blocks GUI thread

Resolved in `ac191eaa`. All three controllers (`NoiseController`, `MomentMapController`, `PvController`) have been refactored from a shared `m_watcher` member to a **per-request heap-allocated `QFutureWatcher`**. The pattern used is:

```
m_busy = true;
QPointer<Controller> self(this);
auto *watcher = new QFutureWatcher<Result>();
QObject::connect(watcher, &QFutureWatcher<Result>::finished, watcher,
    [watcher, self, /* captures */]() {
        watcher->deleteLater();
        if (!self) return;          // controller was destroyed; safe no-op
        self->m_busy = false;
        // ... handle result
    });
watcher->setFuture(QtConcurrent::run(...));
```

The `QPointer<Controller>` guards against the controller being destroyed between the lambda capture and the `finished` signal dispatch. The watcher is cleaned up via `deleteLater()` rather than stack unwinding, so there is no destructor blocking. The `m_busy` flag prevents re-entrancy without holding a watcher reference. The `onWatcherFinished` virtual method and `m_watcher` member declarations have been removed from all three headers.

---

## 4. Feature Analysis

### 4.1 Dockable Cube Workspace (`f7160f34`)

The 3D and 2D rendering panels in `vtkWindowCube` are now `QDockWidget` instances (`dockCubeView` / `dockSliceView`). Users can float, resize, close, and rearrange them independently. A "Reset Layout" action in the View menu restores the default side-by-side arrangement. Dock visibility is wired to the View menu via each dock's `toggleViewAction()`. The implementation reparents the existing `ui->vtkCube` and `ui->vtkImage` widgets into the docks at runtime, preserving all existing rendering code paths without changes to the `.ui` file.

**Astronomy note:** Dockable panels are significant for multi-monitor workflows common in observatory control rooms and data-analysis pipelines, where users may want to display the 3D cube on one monitor and the 2D slice/moment viewer on another.

### 4.2 3D View Settings Sidebar (`c469c9d3`, `547a10ac`, `cd174b38`)

A dedicated "3D View Settings" sidebar page replaces the previous combined threshold panel. The page is built by `setupThresholdPage()` and contains three logically separated cards:

**Rendering mode card:** A `QComboBox` (`m_comboRenderMode`) with Full Resolution / Preview / Isosurface / Volume options. Bidirectional sync is maintained between the combo and the corresponding menu actions (`actionFullResolution`, `actionFast`, `actionIsosurface`).

**Surface Color card (isosurface mode):** Shows the cube colour button. Visible only when Isosurface mode is active.

**Colour Map card (volume mode):** A `QComboBox` (`m_comboLut`) populated with available LUT names. A `QLabel` (`m_lutPreviewLabel`) shows a horizontal gradient pixmap generated by `buildLutPreview()`. Selecting a LUT calls `applyColorMapByName()`, which updates only the volume colour transfer function (vtkColorTransferFunction) and triggers a 3D-only re-render. Visible only when Volume mode is active.

**Volume blend modes** (`547a10ac`): Composite, MIP (Maximum Intensity Projection), and MinIP (Minimum Intensity Projection) are exposed as both menu actions (View → Volume Rendering submenu) and a combo in the Volume mode card. `applyVolumeBlendMode()` sets the blend mode on the `vtkGPUVolumeRayCastMapper` reference. The menu is enabled only when Volume mode is active; card visibility is toggled by `updateVolumeModeVisibility()`.

**LUT isolation** (`cd174b38`): The colour map selection for 3D volume, 2D slice, and moment maps is now fully isolated. `applyColorMapByName()` updates only the volume CTF; it no longer touches slice or moment LUTs. This resolves a previous coupling where changing the 3D LUT would unexpectedly alter 2D rendering.

### 4.3 2D View Settings Sidebar (`6585c56d`, `e6d6c00a`)

A "2D View Settings" sidebar page is introduced by `setup2dViewPage()`. It contains a Slice vs Moment mode selector, reparented slice controls (channel slider, spin box, spectral line), and a slice colour-map selector. Bidirectional sync between the 2D mode combo and `actionSlice`/`actionMomentMap` is maintained by `update2dModeVisibility()` and `sync2dModeSidebar()`. The slice dock is renamed from "Slice / Detail" to "2D View".

A dedicated `applyMomentLutByName()` function handles the moment-mode LUT independently of the slice LUT (`applySliceLutByName()`), completing the three-way LUT isolation (slice / moment / 3D volume).

**Empty-view prevention** (`e6d6c00a`): Switching to Moment mode when no moment map has been computed previously no longer produces a black/empty panel. The viewer now detects this state and triggers the moment generation flow, keeping the 2D panel consistent on cancel or failure.

### 4.4 Moment Card State Machine (`80a146a5`)

A four-state machine (`MomentCardState::NoMoment / Computing / Ready / Error`) governs the appearance and content of the moment-map information card. State transitions are managed by `setMomentCardState()`, which updates card text, tooltip, and style (colour-coded border) atomically. `MomentMapController::lastAcceptedConfig()` exposes the configuration accepted by the most recent user interaction, used to build a compact provenance summary ("M0, ch 10–50, mask ON") displayed during the Computing state.

A deferred silent-failure detection is implemented via `QTimer::singleShot(0, ...)`: after the async watcher's `finished` signal fires, a zero-delay timer checks on the next event-loop tick whether the card is still in `Computing` state (indicating the controller went idle without triggering `onCompleted`). If so, the card transitions to `Error` and a diagnostic warning is published. This pattern correctly catches the case where the backend returns a non-fatal error that the controller handles by calling `onRunFinished` but not `onCompleted`.

**Software engineering note:** The deferred check is an elegant solution to the ambiguity in the controller callback protocol. The zero-delay timer relies on the Qt event loop processing the timer event after the watcher signal, which is guaranteed by Qt's single-thread event ordering.

### 4.5 Info/Stats Sidebar Page (`ad93a9ed`)

A new Info/Stats page (`setupInfoStatsPage()` / `updateInfoStatsPage()`) exposes four grouped sections:

- **Dataset Summary:** filename, dimensions (W × H × D), spectral axis type and unit.
- **Data Statistics:** reparented existing statistics frames (min/max/mean/RMS) from their previous location.
- **Coordinate / WCS:** a status badge (`m_infoWcsBadge`) colour-coded green (ok) / amber (sanitized) / red (failed), with the WCS warning message surfaced as a tooltip. This makes the WCS sanitisation that the backend applies visible to the user at a glance.
- **System/Status:** active backend URL, session ID.

`updateInfoStatsPage()` is called after dataset open, after moment completion (to update WCS status from the moment response), and whenever relevant state changes.

**Astronomy note:** The WCS status badge is particularly valuable. Users working with non-standard radio-interferometric headers (e.g., AIPS-generated FITS with VELO CTYPE) will now see immediately whether the backend has silently corrected their header, rather than discovering discrepancies in axis labels later.

### 4.6 BUILDING.md (`6481b8ac`)

A 163-line build guide covers CMake configuration, Qt 6 and VTK 9.5 dependency setup, backend Python environment setup, and test suite execution. This reduces the barrier to entry for new contributors.

---

## 5. Open Issues

### 5.1 High

**H1 — `vtkWindowCube.cpp` has regressed to 8 171 lines (+1 218 from `61bb5884`)**

This is the most significant concern of the current cycle. The controller extraction in the previous cycle reduced the file to 6 953 lines; the eleven UI-feature commits in this cycle have pushed it back to 8 171 lines, 607 lines above the level before the refactoring began (7 564). The additions are all legitimate UI features (settings pages, state machines, colour-map wiring, dock management), but they have been implemented directly in `vtkWindowCube` rather than following the controller-extraction pattern established for `NoiseController`, `MomentMapController`, and `PvController`.

The correct architectural response is to recognise that `vtkWindowCube` now has two distinct responsibilities: (1) VTK scene management and (2) sidebar/panel UI assembly. The UI assembly code (`setupThresholdPage`, `setup2dViewPage`, `setupInfoStatsPage`, `buildLutPreview`, `applyColorMapByName`, `applySliceLutByName`, `applyMomentLutByName`, `setMomentCardState`, `updateInfoStatsPage`, dockable panel setup) could be extracted into a `CubeUiAssembler` or similar class, leaving `vtkWindowCube` responsible only for VTK rendering, WCS overlay, probe, and action orchestration. The `vtkWindowCube.h` header has also grown to 709 lines with many `QLabel*` `QPointer` members for dynamic info fields that belong in the UI assembler.

### 5.2 Low

**L1 — HiPS tile disk cache grows unbounded**
`_HIPS_CACHE_DIR` has no TTL or maximum-size constraint. Add configurable max-age or max-disk-size with LRU eviction.

**L2 — Sesame resolver: no in-process result cache**
`_resolve_target_sync` makes a round-trip to external Sesame on every call. Add `functools.lru_cache(maxsize=256)`.

**L3 — `np.memmap` not explicitly closed on session expiry**
Explicit `entry["memmap"]._mmap.close()` in session cleanup is good practice for correctness on non-CPython runtimes.

**L4 — `z=0.0` sentinel for absent redshift in cosmology batch**
Replace with `z=float("nan")` for semantic clarity; the backend already handles non-finite values correctly.

**L5 — Load-more offset not validated against session expiry**
If a session expires between a filter fetch and a subsequent "Load more" click, the error is surfaced via `QMessageBox::warning` but no "Re-open dataset" recovery action is offered.

**L6 — No Python unit tests for backend endpoints**
`_apply_tabular_query()` (the most complex backend function), `_catalogue_overlay_sync()` (RA wrap-around logic), and the WCS sanitisation path are not covered by automated tests. This represents the largest remaining gap in the test suite.

---

## 6. Architecture Assessment

### 6.1 QFutureWatcher per-request pattern

The migration from member watchers to per-request heap watchers in all three controllers (`ac191eaa`) is the most important correctness fix of this cycle. The `QPointer<Controller>` + `deleteLater()` pattern is the standard Qt idiom for safe async lifecycle management and should be adopted as the canonical pattern for all future async dispatch in the codebase.

### 6.2 LUT isolation

The three-way LUT isolation (slice / moment / 3D volume) achieved across `c469c9d3`, `cd174b38`, `6585c56d`, and `e6d6c00a` removes a previous source of surprising cross-contamination. Each LUT is now independent: `applySliceLutByName()` affects only the 2D slice renderer, `applyMomentLutByName()` affects only the moment overlay, and `applyColorMapByName()` affects only the volume CTF. Re-renders are scoped to the affected panel. This is the correct design.

### 6.3 Moment card state machine

The `MomentCardState` state machine is a good UX pattern that extends naturally to other long-running operations. The silent-failure detection via deferred zero-delay timer is a pragmatic solution to a real observability gap. The `lastAcceptedConfig()` accessor on `MomentMapController` provides the provenance data needed by the UI without exposing mutable state.

### 6.4 File size trajectory (updated)

| File | `af4df122` | `61bb5884` | `ad93a9ed` | Δ (this cycle) |
|------|------------|------------|------------|----------------|
| `vtkWindowCube.cpp` | 7 555 | 6 953 | 8 171 | **+1 218** |
| `vtkWindowCube.h`   | —    | —    | 709  | significant |
| `backend/app/main.py` | 1 687 | 2 320 | 2 320+ | ~stable |

The regression in `vtkWindowCube.cpp` is the clearest signal that the next structural priority should be extracting the sidebar UI assembly into a dedicated class.

---

## 7. Recommended Next Steps (Priority Order)

1. **(H1 — High)** Extract `CubeUiAssembler` (or equivalent) from `vtkWindowCube`: move all sidebar setup functions (`setupThresholdPage`, `setup2dViewPage`, `setupInfoStatsPage`, `buildLutPreview`), LUT application helpers, `setMomentCardState`, and the dockable-panel setup out of `vtkWindowCube`. The `QLabel*` `QPointer` members for info fields belong in this class. Leave `vtkWindowCube` responsible only for VTK scene management and action wiring.

2. **(L6 — Low/Medium)** Add Python pytest coverage for `_apply_tabular_query()` (edge cases: `in` operator, empty filters, `offset` > `total`, string/numeric coercion), `_catalogue_overlay_sync()` (RA wrap-around at 0°/360°), and the WCS sanitisation path in `ScientificFitsDataset`.

3. **(L1 — Low)** Add configurable max-age or max-disk-size for HiPS tile disk cache.

4. **(L2 — Low)** `functools.lru_cache(maxsize=256)` on `_resolve_target_sync`.

5. **(L3 — Low)** Explicit `memmap._mmap.close()` in session cleanup.

6. **(L4 — Low)** Replace `z=0.0` sentinel with `z=float("nan")` in `triggerCosmologyUpdate`.

7. **(L5 — Low)** Detect session-expiry errors in load-more path and offer re-open prompt.

8. **Extend C++ tests** to cover `SidebarPanel::addPage` + `finalize` smoke test, and the `MomentCardState` transitions in `setMomentCardState`.

---

## 8. Summary Scorecard

| Criterion | Score | Notes |
|-----------|-------|-------|
| Backend-authoritative architecture | ✅ Excellent | No external calls from client |
| Async / non-blocking UI | ✅ Excellent | Per-request watcher + QPointer; all paths off UI thread |
| Scientific correctness (WCS, moments, PV, cosmology) | ✅ Good | Unchanged correct implementation; WCS status now visible to user |
| Rendering UX (3D) | ✅ Good | Dockable panels, LUT previews, blend modes, isolated LUTs |
| Rendering UX (2D) | ✅ Good | Dedicated 2D page, isolated slice/moment LUTs, empty-view prevention |
| Moment map UX | ✅ Good | State machine, silent-failure detection, provenance card |
| Dataset discoverability | ✅ Good | Info/Stats page with WCS badge |
| Memory / I/O efficiency | ✅ Good | memmap, DataFrame cache, bounded overlay cache (lru_cache) |
| Code structure | ⚠️ Fair | vtkWindowCube regressed to 8 171 lines; UI assembly needs extraction |
| Documentation | ✅ Good | BUILDING.md added |
| Test coverage | ✅ Good | 36 C++ unit tests; Python tests still absent |
| Security / isolation | ✅ Excellent | Bearer token, session isolation, client never hits CDN |

**Overall:** The application has reached a high level of functional maturity across all its major subsystems. The single most important remaining action is preventing further growth of `vtkWindowCube` by extracting its UI assembly responsibility into a dedicated class.
