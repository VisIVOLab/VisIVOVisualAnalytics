# Spectral cube viewer

The cube viewer (`vtkWindowCube`) is the workspace for any 3-D FITS spectral
cube — typically with two spatial axes (RA, Dec or l, b) and one spectral
axis (frequency, optical velocity, radio velocity, or generic). It opens
automatically when you open a FITS file that the backend classifies as a
`cube` dataset.

## Layout

The window is split into two synchronised dock areas plus a side panel:

| Dock | What it shows |
|------|---------------|
| **3-D view** | Volume / isosurface rendering of the whole cube, with an orientation marker and the *cutting plane* indicator. |
| **2-D view** | The currently selected slice along the spectral axis (or a moment map, see below). |
| **Sidebar** | Tabs for *3-D View Settings*, *2-D View Settings*, *Tools*, *Info / Stats*. |
| **Toolbar (top)** | Dataset path pill, tag chips (resolution, mode), and the **Find / Command / Export** buttons. |

The bottom **status bar** carries three live indicators: the **data state**
(*Preview*, *Loading full resolution…*, *Full resolution*), the **WCS
status** (OK / repaired / degraded), and the **Sanity** badge (NaN fraction
and metadata consistency).

## Preview → full-resolution flow

When you open a cube, the backend first sends a small downsampled **preview**
(factor 4 on each axis) so the viewer becomes interactive in a fraction of a
second. In the background it then prepares the **full-resolution** cube and
swaps it in. The transition is asynchronous: you can rotate the camera and
scrub through slices while the upgrade is in flight.

You can constrain the full-resolution fetch to the **viewport ROI** of the
3-D camera (toggle *View → Use Camera ROI*). This is useful for very large
cubes when you only want detail in the region you're inspecting.

## Slice navigation

Use the slice slider and spin box (right of the 2-D dock) or the keyboard
arrows. The active slice index is shown in the *Cutting plane* card; the
cutting-plane indicator in the 3-D view updates in real time.

```{tip}
The cutting plane is **textured with the live slice contents** — what you
see on the plane is exactly what is shown in the 2-D dock. Its opacity is
adjustable from *View → Cutting Plane Opacity* (or the **CUTTING PLANE**
section of the *3-D View Settings* sidebar): lower opacity to see the
volume behind, higher opacity to emphasise the slice.
```

### Slice animation (movie mode)

Auto-advance the slice axis as a movie — useful for spotting coherent
structures across velocity channels:

- **Play / Pause** — *View → Play Slice Animation* (or <kbd>Space</kbd>),
  or the **SLICE ANIMATION** section in the *2-D View Settings* sidebar.
- **Speed** — preset only: 2, 5, 10, 15, 30 fps (picked from the dropdown
  to stay in sync with *View → Animation Speed*).
- **Mode** — *Loop*, *Bounce* (back and forth), or *Stop at End*.

## 3-D rendering modes

Pick between two complementary 3-D views from the top-right viewer toolbar:

- **Volume rendering** — semi-transparent rendering of the whole cube using
  `vtkGPUVolumeRayCastMapper`. Sub-modes (*View → Volume Rendering*):
  - *Composite* — opacity-weighted ray casting (default).
  - *MIP* (maximum intensity projection) — emphasises the brightest emission
    along each ray; great for finding line peaks.
  - *MinIP* (minimum intensity) — symmetric to MIP; useful on absorption.
- **Isosurface** — extracts a closed surface at a user-given threshold
  (`vtkFlyingEdges3D` server-side). Adjust the threshold with the slider in
  the *3-D View Settings* sidebar. The compute is asynchronous; the volume
  stays visible until the mesh is ready.

You can switch render mode at any time. The colour map is shared between
volume rendering and the slice view.

## Color maps and transfer function

- The active **color map** is picked from the *3-D View Settings* sidebar
  (Inferno, Viridis, Magma, Plasma, Cividis, …).
- For volume rendering, the **opacity transfer function** maps low values
  to fully transparent and high values to fully opaque. Customise it from
  *3-D View → Advanced…* (LUT editor) — drag the control points to
  emphasise emission peaks or remove background.
- For the slice / moment maps, the **2-D LUT editor** opens from the
  *2-D View Settings* sidebar. Both editors are non-modal: keep them open
  while you scrub through slices to fine-tune in real time.

## Tools menu

The cube viewer's *Tools* menu (and the matching *Tools* sidebar tab) is
organised in four groups that go from cheap interactive lookups down to
heavier compute, in this order:

```{list-table}
:header-rows: 1
:widths: 18 36 46

* - Group
  - Tools
  - When you reach for them
* - **EXPLORATION**
  - Extract Spectrum, Open in VR
  - Cheap, interactive — look at one spectrum, immerse in the cube.
* - **ANALYSIS**
  - Estimate Noise, Compute Moment, Line-width Map, Baseline Subtraction,
    Stack Spectral Cubes, Extract PV Diagram, **Channel Maps**,
    Export Sub-Cube as FITS, Export Current Channel as 2-D FITS,
    Export Moment Map as FITS
  - Compute-on-cube tools that produce a derived map / cube / spectrum.
    All run on the backend with the heavy-task throttle, so the viewer
    stays interactive.
* - **REGIONS**
  - Box / Circle / Polygon / Annulus
  - Draw a shape on the slice → instant region statistics + per-region
    spectral profile. See [Regions, PV, noise](region-pv-noise).
* - **CATALOGUE**
  - Load / Show / Show Labels / Clear Catalogue Overlay
  - Manage source overlays drawn on top of the slice / moment.
    See [Catalogues](catalogues-hips#catalogue-overlay-on-cubes--images).
```

Every entry is available both in the menu bar **and** as a tool button on
the *Tools* sidebar tab — same action under the hood, so checked state and
enable/disable propagate automatically between the two surfaces. Catalogue
overlay management is menu-only (the four entries are state toggles, not
"do-this-action" buttons).

### Extract Spectrum (probe a single pixel)

The cheapest interactive lookup the cube can offer. You pick a pixel; the
backend returns the 1-D intensity profile along the spectral axis at that
line of sight, drawn in a *Spectral Profile* window.

**How to use it:**

1. **Tools → Extract Spectrum** in the cube viewer (or the *Data Probe*
   sidebar button, or the probe icon in the 2-D toolbar). The cursor
   becomes a cross-hair on the slice.
2. **Hover** the slice → the profile updates live to whichever pixel is
   under the cursor. The header status badge says **🟢 Live** in this
   mode.
3. **Click** a pixel → that spectrum gets **pinned** (frozen). Moving the
   mouse afterwards no longer overwrites it. The badge switches to **🔒
   Pinned** and the meta row hint changes to *"Click the slice again to
   unpin and resume live updates."*
4. **Click again** → unpins, live mode resumes.

You can also pick a spectrum directly from the **3-D view**: enable
*View → Pick Spectrum on Plane Click* (or the **3D INTERACTION** toggle
button in the *3-D View Settings* sidebar). Clicking on the textured
cutting plane extracts the spectral profile at that (RA, Dec) position
and raises the *Spectral Profile* window automatically.

**What the Spectral Profile window shows:**

```{list-table}
:header-rows: 1
:widths: 24 76

* - Area
  - Content
* - **Header band**
  - File name · pixel coords *(x, y)* · WCS coords (RA/Dec or l/b,
    formatted in the active sky frame, sexagesimal or decimal as set in
    the cube viewer). Plus the live / pinned status badge and a one-line
    interaction hint.
* - **Plot**
  - Intensity vs spectral axis (velocity / frequency / channel — picked
    from the cube WCS, with BUNIT on the Y axis). Drag and scroll-wheel
    zoom both axes. A dashed brand-blue vertical line marks the channel
    currently shown in the 2-D slice viewer — so when you scroll the
    slice slider, the marker moves with it and you can see exactly which
    sample of the spectrum corresponds to the slice on screen.
* - **Stats bar**
  - `N` (finite samples) · `Min` / `Max` / `Mean` · `RMS` · `∫`
    (integrated value over the visible spectrum, ≈ flux density × `Δv`,
    a quick column-density / line-flux proxy). All recomputed on every
    new probe.
* - **Footer**
  - *Save spectrum as PNG…* exports the plot as an image; *Save spectrum
    as CSV…* exports the actual data (two columns: spectral axis +
    intensity), prefaced by `#` comment lines carrying the dataset name,
    pixel + WCS coords, and sample count — so the file is self-
    documenting and parses straight into pandas / astropy / TOPCAT.
```

#### Spectral smoothing

The **Smooth:** combo box in the spectrum header lets you apply a 1-D
convolution kernel to the displayed profile without altering the
underlying data. Available kernels:

- **None** — raw spectrum (default).
- **Hanning** — three-point [0.25, 0.5, 0.25] smoothing; good for
  suppressing Gibbs ringing.
- **Boxcar 3 / 5 / 7** — simple running average over 3, 5, or 7
  channels.
- **Gaussian σ=1 / σ=2** — Gaussian convolution with standard deviation
  1 or 2 channels.

The kernel is **NaN-safe**: channels flagged as NaN are excluded from
the convolution so they don't propagate into neighbouring values. The
stats bar (Min, Max, Mean, RMS, ∫) is recomputed on the smoothed data.
Smoothing is also active during **live probe hover**, so you can compare
kernels in real time as you move across the slice.

```{note}
Smoothing is **display-only**. *Save spectrum as CSV…* always exports
the raw (unsmoothed) data so downstream analysis tools receive the
original channel values.
```

#### Line identification overlay

The **Load Lines…** button in the spectrum header lets you overlay
expected spectral-line positions on the plot:

1. Click **Load Lines…** and select a CSV (or tab-separated) text file
   with two columns: `frequency,label` (e.g. `115.271,CO(1-0)`). Lines
   starting with `#` are ignored.
2. Each entry is drawn as a **vertical dashed amber line** at the given
   frequency, with a **rotated label** alongside it.
3. Click **Clear Lines** to remove all markers.

```{note}
Frequencies in the file must be in the **same unit as the plot's X
axis**. If the spectrum is displayed in velocity, convert your rest
frequencies to velocity first (or switch the cube's spectral axis to
frequency). No automatic unit conversion is performed.
```

**Why it matters scientifically:** moment maps and region statistics
average away the per-channel detail. The single-pixel spectrum is what
you need to identify line shape (Gaussian vs multi-peak vs absorption),
spot self-absorption dips, hand-pick line-free channels before running
[Baseline Subtraction](spectral-tools#baseline-subtraction-s-03), or
visually confirm a [moment-1 velocity gradient](moment-maps) is driven by
a real shift in line centroid rather than by a noise feature. The CSV
export is the bridge to downstream analysis tools (Gauss-fit, line
identification, comparison with synthetic spectra) — keep the
provenance preamble in the file and your future self will know exactly
which pixel of which cube that spectrum came from.

```{note}
The same window is reused as the **Region Spectral Profile** when you
draw a Box / Circle / Polygon / Annulus region (see
[Regions, PV, noise](region-pv-noise)). In that mode the Live / Pinned
status badge is hidden (the spectrum is a one-shot mean over the region,
not a live probe) and the header shows the region descriptor instead of
the pixel hint — e.g. *"Circle region · 195 / 195 valid pixels · 2D
stats: current slice · spectrum: full cube"*. Plot, stats bar, channel
marker and CSV export behave identically.
```

### Open in VR

Open the current cube in a VR headset and explore the volume in stereo.
The same `vtkVolume` actor used by the desktop renderer is shared with
the OpenXR render window, so the LUT, threshold and opacity transfer
function you've tuned on the desktop are mirrored live into the headset
view.

**Requirements** (see also the
[VR enablement guide in the architecture note](../architecture-note)):

- The VisIVO client must be built with `-DVISIVO_ENABLE_VR=ON`, against a
  VTK with the `RenderingOpenXR` module compiled in. Default Mac builds
  ship VR off (Apple removed OpenXR support); Windows / Linux builds need
  to opt in explicitly.
- A working **OpenXR runtime** must be installed and active on the host:
  SteamVR, Oculus runtime, Windows Mixed Reality, or Monado on Linux.
- A **headset** must be connected and recognised by that runtime.

**How to use it:**

1. Make sure your OpenXR runtime (SteamVR / Oculus / WMR / Monado) is
   running and the HMD is detected by it.
2. Open a cube and wait for the full-resolution swap to complete (the
   bottom-status indicator goes green).
3. **Tools → Open in VR** in the cube window (or *Open in VR* in the
   *Tools* sidebar).
4. Put the headset on. The cube appears in front of you, sized to a
   comfortable arm-reach by default.

If anything is missing — runtime not started, headset disconnected, VR
not compiled in — the action either pops up a friendly *"Could not start
an OpenXR session"* dialog or, in the not-compiled case, shows a
disabled menu item with a tooltip listing the rebuild flags. Nothing
crashes; the desktop viewer stays usable.

```{caution}
The current iteration runs the OpenXR session on the UI thread: the
desktop window is blocked until you exit VR (take the headset off and
press the runtime's quit gesture, or close the VR session in SteamVR /
Oculus dashboard). A future revision will move the session to a worker
QThread.
```

**Why it matters scientifically:** stereoscopic rendering exposes
3-D structure (spectral-spatial coherence, filaments, shells) that flat
slice scrolling or even rotating the desktop volume tends to hide. Useful
for spotting where lines from different velocity ranges connect spatially
— typical use cases are outflow lobes, expanding bubbles, and filament
networks where the eye benefits from real parallax.

### Channel Maps (velocity mosaic)

A standard radio-astronomy display: an N × M grid of 2-D channel slices,
all rendered with the same colour map and data range, so coherent
structures across the spectral axis jump out at a glance — outflow
lobes, expanding shells, velocity gradients, and cloud morphology.

**How to use it:**

1. **Tools → Channel Maps…** in the cube viewer (also available in the
   *Tools* sidebar under ANALYSIS).
2. Set the **start** and **end** channel, the **stride** (skip every
   N-th channel), the number of **columns** in the grid (default 8),
   and the **colour map** (with gradient preview in the dropdown).
3. Click **Generate**. The backend fetches the entire z-range in one
   shot; the mosaic window renders all panels within a few seconds even
   for 64+ channels.
4. The header shows the channel range, stride and LUT; each cell shows
   **CH N** (the 0-indexed channel index). Axis ticks appear only on
   edge panels (bottom row = X, left column = Y) to keep the grid
   clean.
5. **Double-click** any panel to open it in a standalone resizable
   window with the full colour bar, axes, and drag + zoom. Useful for
   inspecting a single channel in detail without leaving the mosaic
   context.
6. **Save mosaic as PNG…** composites all panels (retina-quality) into
   one image with a title bar and saves it at the user-chosen path.

```{tip}
Start with a small stride and wide range to identify the interesting
velocity window, then narrow the range and set stride=1 for a
publication-ready mosaic. The colour scale is shared across all panels
(auto-ranged from the finite min/max of the entire sub-volume), so
faint emission and strong peaks are directly comparable.
```

## Regions, PV diagrams, noise

These are all explained on a dedicated page:
[Regions, PV diagrams, noise](region-pv-noise).

## WCS axes and overlays

- *View → Show WCS Axes* paints sky-coordinate ticks on the 2-D slice and
  moment maps. Toggle between **sexagesimal** (HMS / DMS) and **decimal**
  with the *WCS format* radio in the same menu.
- *View → Show 3D WCS Axes* draws a labelled bounding box in the 3-D view
  with RA / Dec / Velocity ticks derived from the cube WCS. Useful as a
  spatial reference when rotating the camera.
- *Tools → Load Catalogue Overlay* lets you overlay sources from a CSV /
  VOTable on the slice; see [Catalogues](catalogues-hips).

### Beam indicator

When the FITS header contains `BMAJ` and `BMIN` (and optionally `BPA`),
the 2-D slice view draws a **filled white semi-transparent ellipse** in
the bottom-left corner representing the synthesised beam. The ellipse is
sized in pixels using the angular beam axes (`BMAJ`, `BMIN` in degrees)
divided by `|CDELT1|`, so it scales correctly with the image pixel scale.
`BPA` (beam position angle, degrees) controls the orientation.

If `BMAJ` or `BMIN` are absent from the header (e.g. single-dish data
without a restoring beam), the ellipse is hidden automatically — no
action needed.

```{tip}
The beam indicator helps you judge whether spatial structures in the
slice are resolved. Any feature whose angular size is comparable to the
beam ellipse is only marginally resolved — treat its morphology with
caution.
```

## Saving and exporting

- **Export** in the top-right toolbar saves a PNG of the current 2-D view
  or a PNG snapshot of the 3-D view (camera position respected).
- Moment-map results can be re-opened as a new dataset (the backend
  registers them in the same session); see [Moment maps](moment-maps).
- Baseline-subtracted cubes are also registered as a new `dataset_id` you
  can open in a fresh cube viewer; see [Spectral tools](spectral-tools).

### FITS exports → Workspace

Two entries under **Tools** persist derived FITS artefacts into a backend
*Workspace Exports* directory (default ``~/.visivo/exports/``, override
with the ``VISIVO_EXPORTS_DIR`` environment variable):

- **Tools → Export Sub-Cube as FITS…** — crop the current cube to a
  spatial+spectral ROI and save it as a standalone FITS. The bounds
  dialog defaults to the AABB of the currently-drawn region (if any),
  otherwise to the full cube extent. WCS is preserved: ``CRPIX1/2/3`` is
  shifted so every pixel in the cropped FITS keeps its original sky /
  spectral coordinates. The sub-cube is also registered as a new dataset
  in the active session, so you can immediately open it in a new cube
  viewer.
- **Tools → Export Current Channel as 2-D FITS…** — save the channel
  that is currently displayed on the slice slider as a **standalone
  2-D FITS image** (``NAXIS=2`` — the spectral axis is dropped, not
  kept as a degenerate dimension). Used by the Stokes / spectral
  index / Faraday-RM workflows in the image viewer: it gives you a
  single-frequency 2-D map per click. Header keeps ``BUNIT``,
  ``OBJECT``, the celestial WCS (axes 1 + 2) plus ``SPECVAL`` /
  ``SPECTYPE`` / ``SPECUNIT`` recording the spectral coordinate of the
  exported channel, so you can later look up the frequency / velocity
  for the spectral-index and RM dialogs.
- **Tools → Export Moment Map as FITS…** — persist the moment currently
  on screen as a 2-D FITS (celestial WCS, ``BUNIT`` derived from the
  cube). Disabled until a moment has been computed.
- **Tools → Send Slice to Image Viewer…** — send the current 2-D slice
  to the first open image viewer as a contour overlay. No FITS
  round-trip required — the data travels in memory via the
  ``contourDataReady`` signal. Useful for quick radio + optical
  comparisons without persisting intermediate files.

Both flows ask only for a **basename** (e.g. ``m31_m0.fits``). The
backend stores the file in the Workspace Exports dir and auto-suffixes
collisions (``cube.fits`` → ``cube_1.fits`` → …). The completion dialog
shows the chosen filename and on-disk path.

Once in the workspace, artefacts are listed in the **Workspace Exports**
panel of the Data Hub. Per-entry actions:

- **Open** — register the FITS as a new session dataset and open the
  matching viewer (cube viewer for cubes, image viewer for 2-D maps).
- **Download…** — native Save As dialog → streams the bytes from the
  backend (over HTTP, so it works the same when the backend lives on a
  remote host) and writes to the chosen local path.
- **Delete** — removes the FITS from the workspace (confirmation
  prompt; irreversible).

The panel auto-refreshes every few seconds, so new exports from any
cube viewer appear without manual action.

## Performance notes

- The first full-resolution swap can take a few hundred ms because of the
  initial GPU texture upload; subsequent slice changes are sub-frame.
- The backend reuses a multi-process worker pool. Long computations
  (moment, isosurface, line-width, baseline, stacking) are gated so they
  never starve interactive requests like slice fetches — you can keep
  scrolling and rotating the cube while a heavy compute is running. See
  the [heavy-task throttle](../async-patterns#heavy-task-throttle-backend-side)
  in the developer reference for the gritty details.
- *Camera ROI* in the View menu limits the full-resolution fetch to the
  current viewport (recommended for cubes > ~2 GB).
