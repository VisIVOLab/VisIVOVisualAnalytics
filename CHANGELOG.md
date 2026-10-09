# Changelog

Development history for VisIVO Visual Analytics.

These notes were kept in the README until the README became a front door for
people who want to *use* the software rather than follow its development. They
are preserved here unedited; the branch names they refer to are historical.

**next-server branch — spectral analysis improvements (2026-04)**

- **Moment orders 3, 4, 5 fully supported** — M3 (skewness = μ₃/σ³), M4 (excess kurtosis = μ₄/σ⁴ − 3), and M5 (standardised 5th moment = μ₅/σ⁵) are now correctly computed in both the numpy and Dask paths. All three are dimensionless and normalised consistently with one another. The C++ `isSupportedMomentOrder()` guard was extended to include these orders (previously returned "Unsupported moment order" at runtime).
- **Unified moment dialog** — the Moment submenu (six individual order actions) is replaced by a single **Tools → Compute Moment…** action that always opens a configuration dialog. The dialog lets the user select any supported order, channel range, and optional threshold mask in one step.
- **Surface real backend error text** — `MomentMapController` stores the backend error message with the `"MomentMap M<order>: …"` prefix and exposes it via `lastError()`. The status-bar deferred-error path in `vtkWindowCube` now shows the actual backend message instead of a generic string.
- **Line-width map guard** — `LinewidthDialog` validates `width > 0 && height > 0` before constructing the `ScienceMapWindow`, preventing a crash on all-NaN output (when all pixels fail to fit).
- **Session ID plumbing** — `LinewidthDialog`, `BaselineDialog`, and `StackDialog` all accept a `sessionId` parameter and call `client->setSessionId()` before making any request, fixing the double-slash URL and 401 errors that occurred when session ID was not forwarded.
- **StackDialog redesign** — replaced the plain list with a checkable `QListWidget`. A live counter label shows "N selected" in red when fewer than 2 cubes are chosen; the Compute button is disabled until at least 2 are selected.
- **Backend spectral endpoints** — `/v1/spectral/linewidth`, `/v1/spectral/linewidth/binary`, `/v1/spectral/baseline/{session_id}/{dataset_id}`, `/v1/spectral/stack`, `/v1/spectral/stack/binary` added. These support per-pixel Gaussian FWHM and equivalent-width fitting, polynomial/median baseline subtraction, and mean/median/weighted-mean cube stacking. Source-finder integration via `/v1/spectral/sourcefind` (SoFiA-2 backend).
- **next-server branch**

- **Guided startup flow** — a `StartupDialog` shown before `MainWindow` drives backend health-check, optional auto-start, and token setup. `BackendLauncher` manages the backend process lifetime and captures all stdout/stderr for the log panel.
- **Smart Python resolution** — `BackendLauncher` walks a six-level priority chain (`VISIVO_BACKEND_PYTHON` → active venv → project `.venv` → bundled Python → Settings Python → PATH fallback) to find the right interpreter without manual configuration.
- **Remote file browsing for image layers** — "File → Add New FITS File" in the image viewer now opens the backend-side `RemoteFileBrowserDialog` instead of a local file picker. The selected path is loaded through the same WCS-aware pipeline used by all other layer sources.
- **Unified VLKB layer alignment** — VLKB survey layers added from the inventory tree route through `addLayerImage()` → `loadImageLayer()` → `AstroUtils` / libwcs, the same pipeline as the standard "Add New Layer" path. All layer sources are now aligned consistently regardless of how they were added.
- **Loading placeholder in all viewers** — `vtkWindowImage`, `vtkWindowVbt`, and `vtkWindowVbtVolume` now display a centred "Loading…" message (font 16 pt, colour `#B8D6E8`) while data is being fetched, matching the existing pattern in `vtkWindowCube`.
- **Dockable workspace** — the 3D cube view and the 2D slice/detail view are now independent `QDockWidget` panels. Each panel can be floated, resized, or closed independently. A "Reset Layout" action in the View menu restores the default side-by-side arrangement.
- **3D View Settings tab** — the former "Threshold" sidebar tab is redesigned. Rendering mode (Isosurface / Volume), volume blend mode, threshold slider, and color map selection are organised into collapsible cards. Color Map and Surface Color cards are shown only for the active rendering mode.
- **Volume rendering blend modes** — Composite ray-casting, MIP, and MinIP are selectable both from the sidebar and from the View → Volume Rendering submenu. The sidebar and menu stay in sync bidirectionally.
- **Color map scope** — the LUT combo in the 3D View Settings tab now updates only the volume color transfer function. Slice and moment LUTs are managed independently and are not affected.
- **Non-blocking async controllers** — `NoiseController`, `MomentMapController`, and `PvController` dispatch computation via `QtConcurrent::run` with per-request heap-allocated `QFutureWatcher`. The main thread never blocks waiting for results.
- **WCS axes overlay** — toggled from View → Show WCS Axes.
- **Backend LRU cache** — the overlay CSV catalogue cache is bounded (`functools.lru_cache(maxsize=32)`) instead of growing without limit.
- **Plot background theme** — Settings → UI → Plot background lets the user choose between a dark (default) and light plot background. The preference is applied to the PV Diagram window and all spectrum/profile plot windows when they are opened. The setting is persisted in `Settings.ini` under `Plots/theme`.
- **PNG export for plots** — the PV Diagram window and the spectrum/profile plot windows each have a "Save as PNG…" button in the footer bar. Clicking it opens a file dialog and saves the current plot via `QCustomPlot::savePng()`, suitable for inclusion in papers and presentations.
- **Non-modal analysis results** — the Noise Estimation and Region Analysis result dialogs (`ToolResultDialog`) are now non-modal. The result window stays open while the user continues to interact with the cube viewer, and closes itself when dismissed.
