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

- **Play / Pause** — the **SLICE ANIMATION** section of the left dock, or
  *View → Play Slice Animation* (or <kbd>Space</kbd>).
- **Speed** — preset only: 2, 5, 10, 15, 30 fps (picked from the dropdown
  to stay in sync with *View → Animation Speed*).
- **Mode** — *Loop*, *Bounce* (back and forth), or *Stop at End*.

All three sit together in the left dock's **SLICE ANIMATION** section, and
mirror the *View* menu entries either way you set them. This is animation, not
the channel scrubber below the 2-D view: the scrubber steps channels by hand,
these play them.

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

### Where the rendering happens: Auto / Local / Remote

Independently of *what* is rendered, there is a choice of *where* — the toolbar
segment above the 3-D pane:

- **Local** — the cube is loaded into this machine's GPU through VTK's volume
  mapper. Full interactivity, and the whole volume has to fit in GPU memory as a
  3-D texture (roughly `width × height × depth × 4` bytes).
- **Remote** — the backend renders with EGL and streams frames over a
  WebSocket. **Only if the backend has it**: the dataset-open response carries
  `server_side_rendering_available`, and a CPU-only backend — or any backend on
  **macOS**, where there is no EGL — reports false. The mode entry is then
  disabled with that reason and the recommendation banner does not offer the
  switch, because `/v1/render/*` would refuse the request. It uses the *same* mapper on the whole cube (it is **not**
  out-of-core), so the memory has to exist there instead of here; what you gain
  is that the backend loads the full grid where the client would fall back to a
  decimated proxy, and that a large cube never crosses the network. The cost is
  an encode/decode round trip per frame.
- **Auto** — starts local and lets the recommendation below appear.

A cube whose full resolution exceeds `Cube/local_full_threshold_mb`
(`~/VisIVO Visual Analytics/Settings.ini`, default **256 MB**), or one the
backend has tagged `preview_only`, is shown as a **decimated preview proxy**
locally — the status bar says so — and a banner offers to switch to remote
rendering.

```{note}
What that switch buys depends on **where the backend is**, and the banner says
which case you are in:

- a **remote** backend: the full-resolution cube never crosses the network — the
  frames do, and they are the same size whatever the cube is;
- a **local** backend (`127.0.0.1`): there is no elsewhere. Same machine, same
  GPU, the same whole volume in memory — just held by the backend process
  instead of this one — plus a per-frame encode. The only thing it gains you is
  the **full grid** instead of the decimated proxy, and that is a limit this
  client imposes on itself, not a property of the data.

So on a local backend the banner leads with **Load full resolution**, which is
what the threshold is actually holding back, and keeps *Switch to Remote* as the
second option **when the backend can do it at all** (on a local macOS backend it
cannot, and the button is not shown) — worth taking when you would rather not have the cube resident in
the viewer process. Dismissing the banner with ✕ is remembered for that cube, and
raising `Cube/local_full_threshold_mb` stops it being raised at all.

```{tip}
Server-side rendering earns its keep when the backend is on **another** machine:
a GPU node with 80 GB of VRAM, or a cube on a filesystem you do not want to pull
several GB across. On your own laptop, against your own backend, it is mostly a
way around this client's threshold.
```
```

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

## Tools

Every tool is reachable from two places, both fed by one list in the code, so
they cannot drift apart:

- the **Tools menu**, one submenu per group;
- the Inspector's **Analysis** tab on the right.

Wherever you reach a tool, it is the same action: a toggle switched on in one
place shows as on in the other, and one that is unavailable is greyed out in
both with the same explanation.

```{note}
The tool list is **not** in the left dock. That panel answers "how is this view
drawn" — colour map, scale, clip, layers, slice animation — while the tools
answer "what can I compute", which is the Inspector's question, and it is where
the state that goes with a tool lives: the reason a disabled tool gives, and the
SPECTRUM controls of the product it just produced. The Tools menu and
<kbd>⌘K</kbd> / <kbd>Ctrl+K</kbd> cover the case where the Inspector is
collapsed.
```

#### Preparing the cube: Smooth / Regrid

*Tools → Smooth / Regrid…* applies a Gaussian kernel and/or an integer shrink
to **every plane**, on the backend, and writes a new FITS into the Workspace,
which then opens as a cube of its own.

It is a *spatial* operation: the spectral axis is left alone, so a line in one
channel does not appear in its neighbours. Blank pixels stay blank, the beam
grows with the kernel, and a per-pixel `BUNIT` is summed rather than averaged —
see [Image viewer — Smoothing and regridding](image-viewer#smoothing-and-regridding)
for why each of those matters.

The header can be repaired the same way as in the image viewer: the **CUBE**
tag in the toolbar shows the FITS header, and **Edit…** writes a corrected copy.

#### Session Data: selecting vs showing

The left panel's tree is a **table of contents**, and the two gestures do
different things:

- **Click** a row — the dataset's views (*3D Volume*, *2D Slice*) or any product
  — to *inspect* it: the Inspector's Properties / Provenance follow the
  selection, and if a pane is already showing that row it becomes the **active**
  one. Nothing is mounted and no pane changes what it shows; a row that is not on
  screen anywhere simply leaves the panes alone.
- **Double-click** to *show* it: the row is mounted in a pane (a free one, else
  the layout grows). A product with no pane renderer — a baseline-subtracted
  cube, a noise estimate — raises its own window instead.
- **Right-click ▸ Show in ▸** to choose the pane, with each entry naming what it
  would replace.
- The small **pane number** badge on a row says which pane already shows it;
  clicking the badge activates that pane.

```{note}
Selecting a row used to mount it straight away. That replaced whatever the
active pane was showing just because you clicked a row to read its provenance —
and, since a right-click also makes a row current, it mounted the product before
its own *Show in ▸* menu could open, which made that menu pointless. Selection
now moves the focus at most: it follows a view that is already on screen, and
never mounts. That applies to the right-click too — it highlights the row it
acts on, and the highlight stays after the menu closes, so the focus has to go
with it or the panel is left pointing at one view while the dock describes
another. *Show in ▸* is built per pane and does not consult the active one, so
nothing there is disturbed.
```

```{tip}
Why the focus matters: the dock's **DISPLAY** section is titled after the active
pane and acts on it. If the tree says *2D Slice* while the active pane is the
3-D view, the colour map you change there is the volume's. That is why selecting
a row now takes the focus with it.
```

#### When a tool is available, and where it acts

A tool is bound to a **view**, not to the pane you happen to have selected:

- the seven slice tools (Extract Spectrum, Extract PV Diagram, the four region
  shapes, Import Region) are available whenever the **2D Slice** view is shown
  in some pane; the two 3-D tools (Kinematic Lasso, *Pick spectrum on cutting
  plane*) whenever the **3D View** is. If the view is not on screen the tool is
  greyed out and its tooltip says so — *"acts on the 2D Slice — show it in a
  pane (the pane's ▾ menu)"*. Un-arming a tool this way is **not** the same as
  closing it: taking the 3-D view off screen suspends the Kinematic Lasso and
  leaves its selection intact for when the pane comes back, whereas un-checking
  the tool yourself closes it and clears the selection.
- While a tool is armed, the pane showing the view it listens to wears a
  **thicker amber border** and **keeps taking the mouse** even when another pane
  is selected. So you can arm *Extract Spectrum*, click the spectrum pane to
  reach its controls in the Inspector, and go on clicking pixels on the slice —
  selecting a pane no longer switches the tool off under you.
- **<kbd>Esc</kbd>** disarms whatever is armed (and, with nothing armed, restores
  a maximized pane).
- A few tools are gated on **state** rather than on a view, and again say why:
  *Pin Spectrum* needs a probed spectrum, *Export Region* a drawn region, *Send
  Slice to Image Viewer* an open image viewer to send contours to, *Link Views*
  something to link to (a second pane or a second cube window).

```{list-table}
:header-rows: 1
:widths: 22 40 38

* - Group
  - Tools
  - When you reach for them
* - **Spectral**
  - Extract Spectrum, Pin Spectrum, Extract PV Diagram, Line-Width Map,
    Baseline Subtraction, Stack Cubes to a Spectrum
  - Anything along the velocity axis, from a single line of sight to a stack.
* - **Maps**
  - Compute Moment Map, Channel Maps
  - Collapse the cube to a 2-D map you can measure.
* - **Statistics**
  - Estimate Noise, Pixel Histogram (current slice)
  - What the numbers in this cube look like before you trust them.
* - **Regions**
  - Box / Circle / Polygon / Annulus Region Analysis, Mask 3-D Region,
    Import / Export Region (CRTF/DS9)
  - Draw a shape and measure inside it, or exchange one with CASA / DS9. See
    [Regions, PV, noise](region-pv-noise).
* - **Sources & Kinematics**
  - Source Finding (SoFiA-2), Kinematic Lasso, Kinematic Model Overlay
  - Pick out one object and work with it. See
    [Kinematic Lasso](kinematic-lasso) for click-to-select segmentation, and
    *Kinematic Model Overlay* to draw a tilted-ring model over the data.
* - **Catalogue**
  - Load / Show / Show Labels / Clear Catalogue Overlay
  - Source overlays drawn on the slice or the moment map. See
    [Catalogues](catalogues-hips#catalogue-overlay-on-cubes--images).
* - **Export**
  - Export Sub-Cube as FITS, Export Current Channel as 2-D FITS,
    Export Moment Map as FITS, Export Movie
  - Getting results out. See
    [Figures, movies and linked views](publication-output).
* - **Views**
  - Overlay Slice on an Open Image, Link Views, Open in VR
  - Move the data to another view, or keep several in step.
```

Everything in the Tools menu also answers to the command palette
(<kbd>⌘K</kbd> / <kbd>Ctrl+K</kbd>), which lists the active window's whole menu
bar under *This window ·* — so a tool you half-remember is faster to type than
to find.

```{note}
*Kinematic Model Overlay…* **draws** a tilted-ring model over the cube from
parameters you supply — inclination, position angle, rotation curve. It does
not fit them. Use it to test whether a model you already have is consistent
with the data, not to derive one.
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

1. **Tools → Extract Spectrum** (menu bar, Inspector ▸ Analysis, or ⌘K).
   Arming it gives the 2D Slice pane the focus, puts a cross-hair on it, and
   opens a *Spectrum (live)* pane — in a free pane, or by growing the layout
   (1 → 2 → 4), or, when the grid is full, by **borrowing** the least relevant
   pane and saying so. A borrowed pane goes back to what it was showing when
   the tool ends.
2. **Hover** the slice → the curve follows the cursor. The pane header reads
   *pixel (83, 116) · 🟢 live*.
3. **Click** a pixel → the spectrum freezes there; moving the mouse no longer
   overwrites it. The header becomes *🔒 pinned — click again to resume*.
4. **Click again** → resumes tracking and moves to the new pixel. (This is why
   a second click "changes" the spectrum you had just fixed.)
5. **Ending the tool keeps a frozen spectrum.** Switch *Extract Spectrum* off
   (or press <kbd>Esc</kbd>) and the spectrum you deliberately picked is
   promoted to a permanent **SPEC** product in Session Data, with its pixel in
   the Provenance tab; once it is saved the header says *💾 saved as "Spectrum
   2"*. A curve that was still tracking the cursor is a preview and goes with
   the tool — otherwise every arm/disarm would leave a row nobody asked for.
   **Tools ▸ Pin Spectrum** does the same saving at any moment, so you can keep
   one and carry on exploring.

```{note}
Two different things used to be called *Pin Spectrum*. **Tools ▸ Pin Spectrum**
saves the curve as a product in Session Data. The button in the Inspector's
SPECTRUM panel is now called **Compare** (with **Clear Overlays**): it keeps the
curve *on the plot* as a comparison overlay, display only, nothing saved.
```

### Picking a spectrum from the 3-D view

*Pick spectrum on cutting plane*, in the **INTERACTION** section of the left
dock (or *Tools → Spectral → Pick Spectrum on Plane Click*, also in
Inspector ▸ Analysis), arms a picker on the 3-D view.
While it is armed the 3-D pane carries an amber border, so it is clear which
view is listening, and it keeps taking clicks even if you select another pane —
selecting the spectrum it just produced no longer switches the picker off.
<kbd>Esc</kbd>, or the toggle itself, disarms it; a spectrum you picked is kept
the same way as one probed on the slice.

This is not a second way of doing what *Extract Spectrum* does on the 2-D slice.
It exists because a position–position–velocity rendering makes **coherent
structure across channels** recognisable, and a single channel map does not. A
tidal tail, extraplanar gas, the outer turn of a warp: in the cube these are
connected, tilted features in (RA, Dec, velocity), and in any one channel they
are a scatter of disconnected blobs you would not group by eye. When you have
spotted such a structure in the rendering, this lets you go straight from it to
a spectrum, instead of hunting for the right channel first.

```{note}
The gain is in **recognising the structure**, not in sensitivity. MIP takes the
largest sample along the ray; Composite accumulates weighted contributions through a non-linear opacity map.
Neither is a scientific integration over channels, and no sensitivity or
signal-to-noise gain is implied.
```

So the pick reads the volume, not just the plane. Click the rendering and you
get the spectrum at the position of the voxel the click resolves to — and which
voxel that is follows the blend mode you are looking through, rather than being
a separate rule to remember:

```{list-table}
:header-rows: 1
:widths: 26 36 38

* - Blend mode
  - What the pixel shows
  - What the click returns
* - **MIP**
  - the brightest sample along the ray
  - that maximum
* - **MinIP**
  - the faintest sample
  - that minimum
* - **Composite**
  - a weighted blend of everything along the ray
  - by convention, the first voxel at or above the opacity threshold — the front
    surface, not something brighter hidden behind it
* - **Isosurface**
  - the surface
  - where the ray meets it
* - *(cutting plane)*
  - the displayed channel
  - the voxel under the click, as before
```

All three are picking conventions over **voxel values**. The renderer samples
the volume with linear interpolation, so the extremum along the interpolated ray
is not always the extremum of the voxels it crosses; MIP and MinIP return the
brightest and faintest *voxel* on the ray, which is the same thing except in
marginal cases. Composite has no single producing voxel at all — the pixel is a
weighted sum — so there the rule is a convention outright, chosen because the
front surface is what you were looking at.

A sphere marks where the click landed, so you can see what you hit and rotate
around it.

```{caution}
The third axis is **velocity, not depth**, so what a ray crosses depends on
where the camera is. Looking down the spectral axis, the ray crosses channels
and the maximum is the line peak at that position on the sky. Looking side-on,
it crosses sky positions at roughly fixed velocity, and the maximum identifies
a *position* rather than a peak.

The status bar therefore names the channel the ray hit and its spectral value,
e.g. *"the ray hit channel 74 (1310661.63 m/s)"*. The plotted spectrum is
always the full profile at the hit (RA, Dec); the reported channel tells you
where along that profile the thing you clicked lives. Read the two together.
```

The picker acts on the 3-D view only, and only on what is actually being drawn:
with the **3D rendering** layer switched off, or in isosurface mode, a click on
empty space returns nothing rather than a spectrum out of hidden data.

**What the spectrum pane shows** (the plot is embedded in a pane; its controls
live in Inspector ▸ Analysis ▸ SPECTRUM, which follows the selected spectrum
pane — or, while a probe tool is armed, the live one it is driving):

```{list-table}
:header-rows: 1
:widths: 24 76

* - Area
  - Content
* - **Pane header**
  - The product name, plus the probe state and the pixel it belongs to:
    *pixel (83, 116) · 🟢 live*, *· 🔒 pinned — click again to resume*, or
    *· 💾 saved as "Spectrum 2"* once the curve is a product. (In a standalone
    profile window the same information appears in the plot's own header band,
    with the file name and the WCS coordinates.)
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
* - **Export (Inspector)**
  - *Save PNG…* exports the plot as an image; *Save CSV…* exports the actual
    data (two columns: spectral axis + intensity), prefaced by `#` comment lines
    carrying the dataset name, pixel + WCS coords, and sample count — so the
    file is self-documenting and parses straight into pandas / astropy / TOPCAT.
* - **Analysis (Inspector)**
  - Smoothing kernel, *Fit Gaussian* / *Clear Fit*, *Load Lines…* /
    *Clear Lines*, *Compare* / *Clear Overlays* — see [Spectral
    tools](spectral-tools#line-identification-overlaying-a-line-list).
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

**Load Lines…** overlays a line list on the plot — the bundled list of common
radio / mm transitions, or a file of your own, including a catalogue export.
The dialog asks for the unit and for the source's systemic velocity (or z),
because a line list is in the **rest** frame, and it converts to this axis using
the cube's `RESTFRQ` and the axis's velocity convention. **Clear Lines** removes
the markers.

The conventions, the file formats, the diagnostics when nothing lands in the
band, and a worked HI example are in [Spectral tools → Line
identification](spectral-tools#line-identification-overlaying-a-line-list).

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
A region spectrum (Box / Circle / Polygon / Annulus — see [Regions, PV,
noise](region-pv-noise)) is the same plot, in the same kind of pane, but it is a
**one-shot mean over the region** rather than a live probe: there is no
live / pinned state, and its region descriptor and 2-D statistics live in the
product's Provenance. Plot, stats bar, channel marker, smoothing, fit, line
overlay and CSV export behave identically.
```

#### Comparing regions (overlaid spectra)

The **Compare** button in the Inspector's SPECTRUM panel keeps the current
profile on the plot as a persistent comparison curve, so you can overlay
several regions' spectra on the same axes:

1. Draw a region (Box / Circle / Polygon / Annulus) → its mean-per-channel
   spectrum appears as the live **Current** curve (brand blue).
2. Click **Compare** → the current curve is frozen in place with its
   own colour and a **legend** entry labelled from its title (e.g.
   *"1. Mean per channel — Circle region"*).
3. Draw another region → its spectrum becomes the new **Current** curve
   while the compared one stays overlaid. Repeat to accumulate more.
4. **Clear Overlays (N)** removes all comparison curves and hides the legend.

The Y axis rescales to include every curve, so faint and bright regions
stay visible together. Each pin captures the curve **as displayed** — if a
smoothing kernel is active, the pinned curve is smoothed to match (CSV
export still saves the raw *Current* spectrum only). It also works on
the live single-pixel probe spectrum, so you can compare spectra from
different lines of sight. These overlays live on the plot and are **not**
products: to keep a curve in Session Data use *Tools ▸ Pin Spectrum*.

**Why it matters scientifically:** overlaying the mean spectra of several
regions is the quickest way to compare line profiles across a source —
e.g. a bright core vs. a faint outflow lobe, or the two horns of a
rotating disk — without exporting each one and replotting externally.

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

#### Stokes polarimetry

*Tools → Load Stokes Q/U/V Companions… / Polarised Intensity & Angle /
Rotation Measure Synthesis…* work on full-Stokes cubes — including surveys that
publish one cube per Stokes plane, such as MeerKAT MGCLS and ASKAP/POSSUM. The
companion cubes are opened on the backend and never loaded into the viewer,
which is what makes 7500² × 16 per plane workable; the results come back as
ordinary 2-D products.

Full workflow, naming rules and a worked MGCLS example:
[Polarimetry (Stokes Q, U, V)](polarimetry).

## Regions, PV diagrams, noise

These are all explained on a dedicated page:
[Regions, PV diagrams, noise](region-pv-noise).

## Pixel histogram (current slice)

*Tools → Pixel Histogram (current slice)…* opens a floating
QCustomPlot window with a 256-bin distribution of the **currently
displayed channel** of the cube. Two draggable cursors (red = low,
green = high) live-clip the slice LUT range:

- Drag a cursor and release — the slice LUT updates immediately so
  you can see the effect on the 2-D view without leaving the
  histogram window.
- The window title shows the channel number and the value range is
  reported in the header line.
- Re-open the menu entry on a different channel to rebuild the
  histogram against the new slice.

The tool is especially useful for **multi-plane FITS cubes** where
each "channel" is a different derived map rather than a frequency or
velocity slice — for example the planes of an MFImage RM cube (peak
polarized intensity, rotation measure, RM error, polarization
fraction, polarization angle at λ²=0). The shape of each plane's
distribution is enough to identify what it represents:

| Distribution shape | Likely product |
|---|---|
| Symmetric Gaussian centred near 0, ± hundreds | Rotation measure (rad/m²) |
| Symmetric around 0, ±90 | Polarization angle at λ²=0 |
| Positive-only, exponential-ish, large range | Peak polarized intensity |
| Positive-only, narrow, small range | Error map (e.g. RM error) |
| Bounded [0, 1] | Fractional polarization |

## WCS axes and overlays

- *View → Sky Grid on the 2-D Panes* paints sky-coordinate ticks on the 2-D slice and
  moment maps. Toggle between **sexagesimal** (HMS / DMS) and **decimal**
  with the *WCS format* radio in the same menu.
- *View → WCS Box in the 3-D View* draws a labelled bounding box in the 3-D view
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
  As you type the X/Y/Z bounds, the region they describe is drawn live in the
  viewer — an amber rectangle on the 2-D slice and a translucent box inside the
  volume — so you can see what you are about to cut out before you cut it.
  *Tools → Mask 3-D Region…* shows the same box.
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
- **Tools → Overlay Slice on an Open Image…** — draw the current 2-D slice
  to the first open image viewer as a contour overlay. No FITS
  round-trip required — the data travels in memory via the
  ``contourDataReady`` signal. Useful for quick radio + optical
  comparisons without persisting intermediate files.

Both flows take a **basename** (e.g. ``m31_m0.fits``) — a field in the tool's
own window, not a second prompt after it closes, which is how Export Sub-Cube
used to ask. The backend stores the file in the Workspace Exports dir and
auto-suffixes collisions (``cube.fits`` → ``cube_1.fits`` → …). The completion
dialog shows the chosen filename and on-disk path, and the tool window stays
open so you can crop another region straight away.

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
