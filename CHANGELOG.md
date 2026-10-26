# Changelog

Development history for VisIVO Visual Analytics.

These notes were kept in the README until the README became a front door for
people who want to *use* the software rather than follow its development. They
are preserved here unedited; the branch names they refer to are historical.

## 1.0.4 (2026-10-26)

- **Left dock no longer clipped.** The cube viewer's sidebar sized itself to
  the widest page of its *Display* section, hidden ones included, so the 2-D
  page (266 px) pushed the 3-D controls past the 272 px dock and Volume,
  MinIP and the threshold field were cut on the right. Only the page shown
  counts now (content 234 px).
- **Channel strip.** The slider takes the width of the strip; the spectral
  readout takes only what its value needs (compacting to GHz/MHz in a narrow
  pane and widening back), instead of half the strip as blank space. The
  symbol follows the axis: ν for a frequency, λ for a wavelength, v for a
  velocity.
- **2-D layer list does not scroll sideways.** Layer cards are as wide as the
  list and follow it when the dock is resized or a scrollbar appears.
- **Docs publish through the `VisIVO-doc` mirror again.** The sync workflow
  and `scripts/sync-docs.sh` are back, now keyed to this repository: a push
  that touches `docs/` copies the tree into `VisIVOLab/VisIVO-doc`, which is
  what the Read the Docs project builds (`VISIVO_DOC_DEPLOY_KEY` secret
  required here).

## 1.0.3 (2026-10-22)

- **Recipe replay steps are typed.** *Export recipe as JSON…* now writes each
  replay call with its `request` as the service takes it, under the API's
  field names with numbers as numbers and seeds as `[z, y, x]` (`seed_z`,
  `reach`, `lambda_v`, `reach_max`, …; the apply's `mode`; the fit's `sigma`
  and `min_ring_pix`), plus a `body` line naming what travels as bytes — the
  float32 mask the grow returns, posted to the apply with the origin and
  dimensions of that response — so a script posts the record back instead of
  parsing the Provenance labels. The request recorded is the one the mask on
  screen was grown with, kept beside the mask, not the sliders' values at the
  time of the apply (a keyboard nudge of λv invalidates the field without
  regrowing it). The schema name stays `visivo-recipe/1`; the display
  `parameters` are unchanged. A C++ test reads the fields of the service's
  request model out of the router and holds the recipe to them.
- **The lasso says when its domain was cut.** A source larger than the grow's
  graph budget (four million nodes) is cut to a seed-centred box, and the
  selection is exact within that box only. The service now reports it
  (`X-Visivo-Trim-Box`, the largest box half-width over the seeds), the
  completion message says so, and the product's Provenance (*Domain trimmed*)
  and the cube's FITS `HISTORY` (`domain_trimmed=`) record it; nothing is
  added when no grow was cut.
- **Validation harness.** `usecase_ngc3137.py` reports how the default
  selection splits between the 2.5σ component and the sub-threshold shell;
  the interface figure adds enlarged crops (`figure_interface.py` lays panels
  out in rows); Figure 1's storyboard drops the API strip, keeps its panel
  notes on two lines and draws the navigation step (a)→(b) dashed, apart from
  the transitions that leave a product; the client's complete recipe export
  quoted in the paper ships as `validation/out/recipe_ngc3137_lasso.json`;
  the RM-synthesis test that pins the 25 → 63 rad m⁻² number states its
  conventions (per-plane frequencies, λ² = (c/ν)², sign, grid maximum).
- **Docs build from this repository.** Read the Docs builds from the
  `docs/` tree here, so the `VisIVO-doc` mirror and its sync workflow are gone.

## 1.0.2 (2026-10-21)

- **Public repository: `VisIVOLab/VisIVOVisualAnalytics`.** Documentation,
  issue links, the paper and the build/HPC examples point there; the SLURM
  tunnel scripts take `--repo-dir` (or `VISIVO_REMOTE_REPO`) for a clone with
  another name on the frontend.
- **Full-resolution load: a bounded transient, and no JSON fallback.** Loading
  a cube at full resolution held it three times over while it arrived (the
  four Z-slabs, a stitched copy, the image) before the GPU copy; the slabs are
  now copied straight into the image and released one by one. Worse, a slab
  that failed for any reason — a timeout, a connection the backend dropped
  under load — made the client re-request the whole volume as base64 JSON and
  hold it as the reply, its copy, the parsed document and the decoded bytes,
  while the backend kept serving the aborted slabs (the "5 running" of a
  45 GB client on a laptop). That fallback now exists only for a backend
  without the binary route (404); a failed slab fails the load and says so.
  Before fetching, the client compares three times the region with the
  machine's physical memory and refuses, with the numbers, when it does not
  fit.
- **Windows build fixed.** Three lambdas in the kinematic-model dialog used
  the C++20 capture `[=, this]`, which MSVC rejects in C++17 (clang only
  warns); the release workflow's Windows job stopped there. The sources were
  checked with clang's C++20-extension warnings turned into errors.
- **Multi-extension FITS cubes open.** An ESO phase-3 or APEX product keeps
  the primary HDU empty (`NAXIS = 0`) and the cube in an image extension
  (`DATA_EXT`, with the variance in `STAT_EXT`). The classifier called it
  "unknown" and the opener read an image of shape `()`. Every reader now takes
  the first image HDU with a real array (`image_hdu_index`): the opener, the
  classifier, the cube reader, server-side rendering, moment and PV headers,
  region WCS, the velocity-field and dynamic-spectrum loaders, and the header
  viewer, which lists the extension's header followed by the descriptive cards
  only the primary carries; header edits go to the HDU that owns the card.
- **Validation harness: the methods on discs of known truth.**
  `validation/lasso_synthetic.py` runs the lasso on discs built with the
  application's tilted-ring model (S/N, extent, a neighbour along the minor
  axis); `validation/kinematic_fit_synthetic.py` recovers a flat and a warped
  disc with the comparator's fit; `validation/lasso_sensitivity.py --grid`
  maps Dice over the (λv, reach) plane. Figure 4 of the paper gains the two
  residual maps; Figure 1's panels are drawn at the size they print at.
- **Exported recipe is a record, not a list.** *Export recipe as JSON…* now
  writes the product (id, kind, label, the service routes that replay it, in
  order: the grow from the parameters, then the apply — and a `replay_caveat`
  when the parameters alone do not suffice: a hand-edited mask, a fit that
  needs its selection), the
  dataset (name, path, backend ids), the parameters, the geometry it was taken
  from, the application version and the export time, under
  `"schema": "visivo-recipe/1"`. Before, it was the label→value map of the
  Provenance tab alone.

- **Large IPAC catalogues open in seconds.** Opening the 2.3-million-row
  `NKATZ_2.3M.tbl` used to take minutes and more than 18 GB, and the whole
  backend stopped answering meanwhile (the parse ran on its event loop, so
  `/v1/health` timed out). All-numeric IPAC tables are now read by pandas' C
  parser (0.6 s, 165 MB), giving the same frame as astropy, whose reader is
  still used for tables with string columns or declared null markers; the
  catalogue open runs in a worker thread.
- **Multi-file catalogue offer, narrowed.** Picking a `.tbl` used to offer every
  same-extension file in the folder as one catalogue, so a folder of unrelated
  tables ended in "Column mismatch". Only pieces whose names differ by a
  trailing partition index (`…_1_1_1.tbl`) are offered now, and the question
  lists them.
- **Region spectrum across a blank channel.** A channel that is entirely NaN
  in the cube (channel 91 of the WALLABY Hydra cube) showed as a deep negative
  spike in the box/circle/polygon mean spectrum, because the sanitised cube
  stores blanks as a value below the data minimum. Blanks are now skipped and
  the channel plots as a gap.
- **Windows build instructions rewritten** from the release workflow: Qt 6.10.1
  for MSVC 2022 with its add-ons, zlib → cfitsio → VTK built from source (vcpkg's
  VTK has no Qt support), `-DVISIVO_ENABLE_WEBRTC=OFF` as in the release, the
  DLLs the executable needs, and the backend environment. The docs no longer
  imply that only macOS and Linux build. When run from the build folder on
  Windows, the backend's `requirements.txt` is now copied too, so the first-run
  environment bootstrap works there.

- **PV diagram: fixed a mirrored spectral axis.** On a cube whose spectral axis
  decreases (`CDELT3 < 0`, most HI cubes) the diagram was drawn mirrored against
  its own axis, so emission appeared at the wrong velocity. Channels are now
  placed by their spectral value, taken from the backend; non-linear axes
  (e.g. `VOPT-F2W`) are placed correctly too. PV diagrams of such cubes made with
  earlier versions should be redone. Reported by P. Serra.
- **PV path, editable and live** (requested by P. Serra, in the manner of
  kpvslice): the finished path stays on the image and can be edited (drag a
  vertex, move it, Shift-drag to rotate, double-click to add a vertex,
  right-click to remove one); the diagram is recomputed as you edit. Width,
  *Smooth curve* and *New path* are controls of the diagram; the width dialog is
  gone. A colour bar shows `BUNIT`.
- **PV as FITS:** *Save as FITS…* (`POST /v1/products/pv_fits`): offset axis in
  arcsec and the cube's own spectral WCS, beam, `BUNIT`, path in `HISTORY`.
- **PV sampling:** sub-pixel vertices; a uniform step along the path measured on
  the sky (non-square and skewed pixels); NaN-aware bilinear interpolation;
  only the pixels around the path are read. Cubes whose spectral axis is not the
  last one are refused instead of being cut along a spatial axis.
- **Draw on a moment map:** arming PV, a region or the probe with a moment pane
  active draws on the map (it keeps its title and colours); the pane that showed
  the 2-D Slice keeps showing the channels, with the channel bar and the PV path.
  The PV path is drawn on every map pane of the field. *2D Slice* in a pane's
  menu restores the layout.
- **Zoom while drawing:** wheel zoom and middle-button pan stay available while a
  PV path or a region is being drawn.
- **Product panes showed native RA/Dec under a Galactic title:** their axis
  ticks now use the same frame conversion as the 2-D Slice.
- **Channel bar on the 2-D Slice:** the channel scrubber was only on the 3-D view,
  so a layout without it (slice + moment + PV) had no way to change channel; the
  2-D Slice now has its own, kept in step with the other.
- **One colour range for every channel:** *Same range on every channel* and
  *Cube range* for the 2-D Slice; a range chosen by hand is no longer undone by
  the next channel.
- **Fixed a crash on quit** when a PV diagram or a probe/profile plot was open.
- **PV diagram placement:** a first diagram with every pane taken grows the layout
  instead of being parked out of sight; live updates refresh it in place without
  moving the focus, the selection or the Inspector away from the path.
- **PV on full-Stokes cubes** (RA/DEC/FREQ/STOKES) works again (plane 0, as
  before); the FITS `PVSTEP` card is in arcsec; HISTORY carries one vertex per card.
- **Narrow panes (4-pane layout):** margins shrink with the pane, so a map keeps
  room; crowded axis labels are thinned and the axis title gives way; the channel
  bar keeps its slider and shows a long frequency as GHz/MHz. Overlay labels no
  longer reappear on every redraw.
- **Buttons inside a pane work on the first click** (they used to only activate
  the pane).
- **Readout and DISPLAY on a drawn-on map:** the cursor readout names the map
  instead of a channel; with the PV diagram active, the DISPLAY section says where
  its controls are instead of showing colour controls that do not apply.
- **Status bar:** "queued" counts jobs that are actually waiting (it counted every
  finished job still in the backend's task registry).
- **Kinematic Lasso, threshold and smoothing in the panel:** *Threshold* (1.0–5.0σ)
  and *Smoothing* (Off / Light / Medium / Strong, smooth-before-threshold) sit next
  to Reach and Velocity coupling; the field computation now honours the smoothing.
- **Lasso results are products:** *Isolate* / *Remove* add a LASSO entry to Session
  Data with how it was made (seed, reach, velocity coupling, threshold, smoothing,
  selected voxels, file); the new cube's FITS `HISTORY` carries the same recipe.
- **Lasso pick:** a double-click selects the emission under the cursor even with
  the cutting plane, a kinematic-model overlay or a catalogue in front of it (they
  used to stop the pick at their own position); isolines mode picks the surface.
- **Kinematic Model controls in the Inspector:** level, opacity, representation,
  colour and visibility of the model overlay are in Inspector › Parameters, like
  every other live tool, instead of a dock that overflowed the window. Tools on
  together stack: switching one off brings back the one still on.
- **Inspector › Parameters scrolls:** a tool's controls and a long recipe no longer
  squeeze each other; long values (file names) wrap, and copying them gives the
  plain text.
- **Axes above the channel bar:** the 2-D Slice and the Channels pane keep room
  for the channel bar below the x axis, whose labels and title it used to cover.
- **Polarimetry colour maps:** the polarisation-angle and Faraday-depth maps are
  drawn with the diverging CoolWarm map they ask for (a misspelt name left them
  sequential, with a header swatch that did not match).
- **RM synthesis samples φ max:** the φ grid includes φ max when the step reaches
  it (−80…80 at 1 is 161 samples; it stopped at +79). An axis tick at the origin reads `0.000`, not
  `-0.000`.
- **Pick Spectrum on Plane Click works:** a click on the cutting plane, the volume
  or the isosurface extracts the spectrum there. It never fired in the 3-D view
  (the camera style holds the mouse between press and release, and the pick was
  not listening that way). A press that moves is a rotation, not a pick.
- **Spectrum CSV provenance:** the file names the dataset, the pixel and its WCS
  position (to 10⁻⁵ degree in decimal format) of the spectrum actually shown;
  it said `pixel: (0, 0)` with no dataset or position. A probed spectrum's
  intensity column carries `BUNIT`, and the default file name the real pixel.
- **Spectrum statistics** include the median and σ(MAD), next to mean and RMS.
- **Channel maps keep square pixels:** a 196 × 280 cube was stretched into
  landscape panels (and in the enlarged view).
- **Export Movie at full size:** a channel scan started with only the 3-D view on
  screen came out 200 × 60 pixels; the needed view is now shown for the export
  (or the user is asked to show it), and movies default to the Movies folder
  rather than the program's working directory.
- **Channels are numbered from 1 everywhere:** Channel Maps, Estimate Noise and
  the Z range of Export Sub-Cube took 0-based channels while the channel bar,
  Compute Moment, Line-width and Export Movie counted from 1, and Channel Maps'
  own provenance printed the other numbering (30–80 became "31..81"). File names,
  status messages and polarimetry labels follow.
- **σ from Estimate Noise is used by the other tools:** the moment mask offers
  3σ of it (chosen by default), the line-width mask is set to 3σ of it, and the
  Kinematic Lasso counts its threshold in units of it; each records σ and the
  region it came from. Without an estimate nothing changes. The median reported
  for an even number of channels is now the true median.
- **Line-width map provenance** lists method, channels, mask and rest frequency
  (it had only the map type); the rest frequency is prefilled from `RESTFRQ`.
- The noise dialog's pixel count no longer uses the system's thousands
  separator ("54.880" read as a decimal).
- **Kinematic fit in the app:** *Fit from the lasso selection* in the Kinematic
  Model dialog (`POST /v1/cube/kinematic_fit_bin`) measures the selection on the
  raw cube (flux, w20/w50, V_sys, an S/N-cut velocity field), reads the geometry
  from moment 0, fits a flat and a per-ring disc to the velocity field, and fills
  in the parameters and the ring table. Velocity field, model and residual maps,
  the integrated spectrum and the source's measures become products with their
  provenance; *Save rings as JSON…* exports the rings. The validation script of
  the paper's use case now calls these same functions.
- **One velocity convention for model and fit:** a frequency axis is converted to
  optical velocity (cz), as HI catalogues quote it; the model used the radio
  convention (≈ 4 km/s apart at 1100 km/s).
- Residual and Faraday-depth maps are shown on a range symmetric about zero, so
  the diverging colour map's neutral colour is zero.
- **Residual of the model on screen:** after *Overlay*, with a lasso selection
  present, the residual v − v_model of the model actually in the fields (fitted or
  edited rings) is computed (`POST /v1/cube/kinematic_residual_bin`), reported in
  the dialog and added as a product — dropping or changing a ring is judged on the
  next overlay.
- **Scrolling no longer edits values:** a wheel over a spin box, combo box or
  slider inside a scrolling page (the Inspector, a tool window) scrolls the page;
  a control takes the wheel only once clicked. A fitted V_sys was changed from
  1120 to 1081 that way.
- **Velocity fields open from *Open…*:** a FITS with a length-3 component axis
  is recognised and opened in the velocity-field viewer; it opened as a blank 2-D
  image.
- **RM synthesis: Faraday depth above 6σ.** The service reports the noise of the
  peak |F| map, and the viewer adds *Faraday depth (peak |F| > 6σ)*, the map to
  read rotation measures from.
- **Lasso panel: per-channel profile and extent** of the selection, and the
  voxel count is recounted after a 2-D hand refinement (provenance and the FITS
  recipe recorded the count from before the edit).

## 1.0.1 (2026-10-16)

- **Polarimetry: blank sub-bands.** A release can flag a sub-band out entirely
  (two of the fourteen of the MeerKAT Bulge field G000.0+2.5 are NaN end to
  end). RM synthesis now leaves such planes out instead of returning an empty
  map, reports them (`dropped_planes`, `dropped_freq_hz`; the product records
  *Sub-bands dropped* by frequency) and refuses fewer than three usable
  sub-bands. PI/PA keeps NaN where there is no data instead of a measured zero,
  refuses a blank plane or an out-of-range channel, and drops blank line-free
  channels from the noise estimate. The Faraday transform is a BLAS matrix
  product (about 20× faster on a 900² cutout).
- **Kinematic lasso: optional smooth-before-threshold** (`smooth_v`, `smooth_xy`
  on the select request; off by default).
- **Remote backends:** a remote URL is never auto-started locally, and the local
  token is never sent to it.
- **Containers:** SoFiA-2 installed robustly in the Apptainer images
  (`ENABLE_SOFIA=1`); local artifacts excluded from the image staging.
- **Docs and CI:** documentation refreshed; release workflow triggers on plain
  numeric tags; docs sync on the right branches.

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
