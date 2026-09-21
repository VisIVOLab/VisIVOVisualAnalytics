# `gui_smoke` — driving the real application

A developer tool, not part of `ctest`. It launches the built application on a
real FITS file, clicks through it the way a person would, and checks what the
software actually *did*.

```bash
python3 tools/gui_smoke/gui_smoke.py --list
python3 tools/gui_smoke/gui_smoke.py --image path/to/2d.fits --cube path/to/3d.fits
python3 tools/gui_smoke/gui_smoke.py --image path/to/2d.fits --only ruler,region-readout
```

The catalogue scenarios need no data of their own: a 300-source RA/Dec/redshift
CSV is written into the artefacts directory unless `--catalogue` names one.

The suite needs the application and port 8000 to itself, and it will not stop
either behind your back: with one already running it says so and exits, unless
you pass `--take-over`. What it starts itself, it stops itself. Artefacts go
into a fresh `run-<timestamp>` directory — nothing you point `--artefacts` at is
deleted.

Requirements: macOS with a display, a build in `build/`, and **Accessibility
permission** for whatever runs it (System Settings → Privacy & Security →
Accessibility). CI has neither a display nor that permission, which is why this
is not a test target.

Occasionally a run dies at startup with *"the window never appeared"* while the
application's log shows it running perfectly well: macOS accessibility stops
seeing the process altogether, usually right after a rebuild or under load.
It is not the application — re-run, and if it persists, remove and re-add the
terminal under Accessibility.

## What it asserts on

Not pixels. A scenario fails on a fact:

* **the backend's job history** (`/v1/health` → `recent_tasks`) — the backend
  saying which operations it was asked to perform, whether they finished, and
  whether they failed. This is the strongest oracle available;
* **the workspace** — which files a tool actually wrote into `~/.visivo/exports`;
* **windows and menus**, through macOS accessibility.

Screenshots are written to the artefacts directory (`/tmp/visivo-gui-smoke` by
default) for triage only.

The history endpoint keeps just the last few jobs, so the suite does not read it
directly: `OperationLog` samples it in the background and scenarios ask *that*
what happened since they began. Reading the snapshot straight is how a scenario
ends up passing because the viewer happened to re-bin a cube for its own
display.

The snapshot carries no job id, so identity is reconstructed from the operation
name and the finish time (`now - age_seconds`, with the clock read before the
request so the round trip is not counted into it). Jobs listed **together in one
reply are never merged** — otherwise a failed job and an older successful one of
the same name, half a second apart, collapse into one entry whose status is the
successful one, and the failure a scenario exists to catch disappears. Between
replies, two finishes of the same name within five seconds are taken to be one
job seen twice. What that cannot do without a real job id is stated in the class
docstring rather than papered over.

## Scenarios

| Kind  | Name                   | What it proves |
|-------|------------------------|----------------|
| image | `mode-strip`           | Every interaction mode arms — checked through the viewer's own hint text — and the hint follows a mode armed from the menu |
| image | `region-readout`       | A dragged region produces statistics that hold together: pixels counted, mean inside the extremes, non-zero scatter |
| image | `ruler`                | The ruler arms and stays armed across the two clicks that measure |
| image | `annotation-overlay`   | An annotation is placed and the Overlays block's count goes up |
| image | `histogram`            | The pixel histogram opens |
| image | `image-quality`        | The backend product route runs and the metrics window appears |
| image | `smooth-regrid`        | Smoothing reaches the backend and writes a 2-D FITS into the workspace |
| image | `header-editor`        | The FITS header opens from the toolbar tag |
| image | `publication-figure`   | The figure is rendered by the backend and its path reported back |
| image | `export-workspace`     | Export to Workspace copies the dataset where the Data Hub can find it |
| image | `polarisation-refusal` | A tool whose inputs are missing says so instead of doing nothing |
| image | `circle-region`        | A circle is measured as a circle — and arming it from the menu takes effect whatever mode was armed before |
| image | `menus`                | Every tool group the viewer advertises is still reachable |
| cube  | `moment-map`           | Compute Moment runs as a backend **task** and the task is recorded |
| cube  | `noise`                | Estimate Noise reaches the backend and answers |
| cube  | `cube-header`          | The cube's header opens from its toolbar tag |
| cube  | `cube-histogram`       | The per-slice histogram opens |
| cube  | `channel-maps`         | Channel maps are generated and the window describes real data (panels and a non-empty range) |
| cube  | `cube-smooth`          | A smoothed cube is written with three axes, opened, and lands in the CUBE viewer |
| cube  | `linewidth`            | The line-width map is computed by the backend |
| cube  | `baseline`             | Baseline subtraction runs on the line-free channels it is given and reports the RMS either side |
| cube  | `pv-diagram`           | A PV path clicked on the 2-D pane and finalised with a double click is extracted |
| cube  | `kinematic-lasso`      | A double-click in the volume selects a connected source, and the Inspector reports how many voxels |
| cube  | `kinematic-model`      | The tilted-ring model overlay is built by the backend |
| cube  | `mask-region`          | Masking a 3-D region writes a cube of the same shape |
| cube  | `export-subcube`       | Export Sub-Cube writes a three-axis FITS into the workspace |
| cube  | `export-channel`       | The current channel is written as a 2-D FITS |
| cube  | `sofia`                | Source finding reaches the backend and the answer — catalogue or refusal — is shown |
| catalogue | `catalogue-opens`  | A catalogue handed over by the DESKTOP (command line, Finder, file association) opens — the regression test for everything that is not a FITS |
| catalogue | `catalogue-cosmology` | Choosing another cosmology recomputes the catalogue through the backend |

Not covered here, and why: anything that needs the network (VLKB overlays and
queries, HiPS, cone search, hips2fits, catalogue cross-match against SIMBAD),
anything that needs data this repository has no copy of (Stokes Q/U companions,
so polarisation is only tested through its refusal), file dialogs (Load/Save
annotations, Import/Export region, Load contour), and VR.

## What the platform makes you do

Every one of these was found by a scenario failing for the wrong reason.

* **Accessibility's `click` does nothing** on this application's custom
  widgets, so clicks are real `CGEvent`s (`click.swift`, compiled on first run).
* **A click into a background application is spent activating it.** Every click
  brings the application forward first and refuses to proceed if it cannot.
* **Accessibility reports no windows at all while the application is not
  frontmost** — not an error, an empty list. Every query activates first.
* **A Qt dialog with a parent is a macOS *sheet***: it is attached to its parent
  window and is not in the window list. `dialog_open()` looks for both.
* **Qt's dialog buttons live in an `AXGroup`** (the `QDialogButtonBox`), so
  `button "Apply" of sheet 1` finds nothing. `button_center()` searches the
  groups too, and buttons are always found **by name** — a dialog is centred on
  a window whose size depends on the display, so a hard-coded point misses it
  on any other machine.
* **Close a window by its `AXCloseButton` subrole, never by index**: on the FITS
  header window `button 1` is *Edit…*, so closing it opened the editor and left
  a modal "Do you want to exit?" in front of everything.
* **Closing a window renumbers the rest**, so the tidy-up closes one window per
  pass and looks again. Closing a list of indices in one pass closed the viewer.
* **Never send a keystroke you cannot address.** An early version pressed ⌘W,
  which goes to whatever is frontmost — and started closing windows in other
  applications.
* **The menu bar belongs to the frontmost window**, and the Data Hub and the
  viewers carry different menus while sharing one title, so windows are raised
  by index until the wanted menu appears.
* **A `QMessageBox` is a window with the application's name and an OK button
  that accessibility does not name.** What it does report is `AXModal`, and
  that is the only way to tell it from the viewer underneath. While one is up
  the application ignores everything — a scenario that left one behind made
  every later click, drag and menu do nothing at all, silently.
* **A double click has to be a real one**: the two clicks carry `clickCount` 1
  and 2. Two separate single clicks do not produce the event VTK derives
  `LeftButtonDoubleClickEvent` from, so the PV path and the kinematic lasso
  cannot be driven without it.
* **`QLineEdit` answers `set value`** through accessibility, unlike the
  custom-painted widgets — which is how a tool that requires a parameter
  (baseline subtraction refuses without line-free channels) can be driven.

What cannot be checked from outside is said so in the scenario's docstring
instead of being claimed: the ruler's distance and the region's live readout are
drawn into the VTK scene, where accessibility does not follow. Those are the
unit suites' job — `test_region_readout.cpp` covers the readout's arithmetic,
and the ruler's distance has no unit test of its own yet.

## Defects it found

* **Nothing but FITS and HDF5 could be opened from outside the application.**
  The desktop entry point — command line, Finder double-click, file association
  — went straight to the FITS/HDF5 route instead of the classifier, so an IPAC
  table, a `.speck`, a CSV, a VOTable or a VisIVO Binary Table was answered with
  "Unsupported file type. Expected FITS (.fits, .fit) or HDF5", although every
  one of them opens from the application's own Open… dialog. Fixed in
  `src/gui/MainWindow_Open.cpp` (`openDatasetPath` now goes through
  `dispatchOpenForPath`, which also refuses politely when the backend is down);
  `catalogue-opens` is the scenario that catches it.
* **Arming a region from the menu did nothing while the ruler was armed.**
  A click is dispatched to a measurement before a region, and
  `setMeasurementMode()` disarms any region for that reason — but
  `setRegionMode()` never disarmed a measurement. The menu entry went checked,
  the strip still said Ruler, and dragging drew a distance. Fixed in
  `src/gui/vtkWindowImage_Region.cpp`; `circle-region` is the scenario that
  catches it. The mode strip was unaffected, which is why it had never been
  noticed.
* `/v1/tasks/*` jobs were invisible in the Data Hub's activity panel: the task
  API's terminal status is `completed`, and the registry snapshot recognised
  only `done`, so every moment computed as a task appeared neither running nor
  finished. Fixed in `backend/app/tasks.py`, with a regression test in
  `backend/tests/test_operation_history.py`.
* **One crash, not reproduced**: `Tools ▸ Products ▸ Publication Figure` died
  once inside `QApplication::setOverrideCursor(Qt::WaitCursor)` — Qt converting
  the wait cursor to a `CGImage`, `CGImageCreate` trapping on invalid
  parameters — while accessibility was querying the application. The same
  action has run cleanly every time since. The report is in
  `~/Library/Logs/DiagnosticReports/`.
