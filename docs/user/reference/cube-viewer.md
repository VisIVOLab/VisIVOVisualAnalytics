# Spectral cube viewer

The cube viewer shows a 3-D FITS cube, usually two sky axes and one spectral
axis (frequency, optical or radio velocity), as a volume and as 2-D channels,
and holds the tools that work on it. It opens by itself when you open a file
the backend recognises as a cube (*File ▸ Open…*, or *Open…* in the Data Hub).

For a walk-through, see [First steps](../use-cases/01-first-steps.md); for
moments and PV diagrams, [Moment maps and PV diagrams](../use-cases/03-moments-and-pv.md);
for regions, [Regions](../use-cases/04-regions.md).

## Layout

- *Panes*: one, two or four, chosen with the pane toggle above them. The ▾
  menu in each pane header picks what it shows: *3D View*, *2D Slice*, a
  spectrum, a moment map or another product.
- *Left dock*: *Session Data* (the cube's views and the products made from it)
  above the display controls: *DISPLAY* (titled after the active pane, for
  example *DISPLAY, 3D*), *LAYERS*, *INTERACTION*, *SLICE ANIMATION*.
- *Inspector* (right): *Properties*, *Analysis* (the tools), *Parameters* (the
  armed tool's settings and the selected product's provenance) and, when
  available, *Copilot*. *View ▸ Inspector Panel* shows or hides it. Drag its
  left edge to widen it when a value (the beam, the unit, a path) is cut; the
  full text is in the tooltip.
- *Command bar*: the dataset path (▼ for recent datasets), the ⌘K search
  field, the *CUBE · W×H×D* tag (click it for the FITS header; *Edit…* there
  writes a corrected copy) and the backend chip.
- *Status bar*: the data state (*Preview*, *Loading full resolution…*, *Full
  resolution*), the WCS status (OK, repaired, degraded) and the *Sanity* badge
  (NaN fraction and metadata consistency). While a map or the PV diagram is
  the active pane it also says how that product was made, for example
  *Moment 0 · channels 1–512 · mask off*; hover it for the full recipe
  (*Inspector ▸ Parameters* has the same).
  A message the window posts (for example *spectrum recomputed at full
  resolution*) takes the room it needs on the left; the readouts that are
  in its way fold away until it goes.

⌘W (Ctrl+W on Linux and Windows) closes the window.

### Session Data

- Click a row to inspect it: the Inspector follows it, and if a pane already
  shows it that pane becomes active. Nothing changes on screen.
- Double-click a row to show it in a pane (a free one, or the layout grows).
  A product with no pane view (a baseline-subtracted cube, a noise estimate)
  opens its own window.
- *Right-click ▸ Show in ▸* chooses the pane; each entry says what it replaces.
- The pane-number badge on a row says which pane shows it; click it to make
  that pane active.
- The *Regions* node lists the window's regions, grouped by the file they
  were imported from, each with a check box that shows or hides it; see
  [Regions in Session Data](region-pv-noise.md#regions-in-session-data).

The *DISPLAY* section acts on the active pane, so check which pane is active
before changing a colour map.

## Preview and full resolution

A cube opens first as a preview (every 4th voxel on each axis, or block means
from the cube's brick pyramid if one exists), then the full resolution replaces
it in the background. You can rotate and change channel meanwhile. The status
line says which preview is shown (*Preview: every 8th voxel*, *Preview: block
means, pyramid level 3*).

A cube larger than the local limit (*Cube/local_full_threshold_mb* in
`Settings.ini`, default 256 MB) stays a preview, and a banner offers *Load
full resolution* and, when the backend can render, *Switch to Remote*. ✕
dismisses it for that cube.

- *View ▸ Load Full Resolution*: loads the whole cube into the viewer.
- *View ▸ Crop Full Resolution…*: loads only an X/Y/Z box at full resolution.
- *Camera ▸ Use Camera ROI*: limits the full-resolution load to the part in
  view (for cubes above about 2 GB).
- *Tools ▸ Views ▸ Refine Selected Box…*: shows one box at the finest level
  the detail budget allows, over the preview. The box starts from the selected
  region or the middle half of the cube and is drawn in amber while you edit
  it. It stays until *Tools ▸ Views ▸ Clear Refined Box*.

While a large cube is shown as a preview, the 3-D view also refines the part
in view when the camera stops. *Settings ▸ General ▸ Large
Cubes* holds *Level of detail*, *Refine automatically when the view settles*
and the *Detail budget* (memory for finer blocks; *Automatic* is the smaller of
512 MB and a sixteenth of the machine's memory). Loading the full resolution
or a region, MIP or MinIP, the isosurface, remote rendering and *Open in VR*
return to the plain preview.

The *3D cube* statistics in *Inspector ▸ Properties* describe the whole
cube, scanned by the backend; for a cube shown as a preview they come partly
from a sample, so Min and Max may lie inside the true range.

## Channels

- *Channel bar*: under the 2-D Slice and under the 3-D view. Shows the channel
  number and its spectral value; both bars move together.
- Wide channels: a channel wider than 2048 pixels arrives as every second,
  third… pixel, so it shows in a second or two even from a cluster. Zoom in and
  the part on screen fills in at full resolution once it is 2048 pixels or
  fewer across. The probe always shows the exact value of the pixel under the
  cursor: between samples it is read from the backend, and the readout shows
  "…" for the moment it takes. Statistics, the pixel histogram and region
  measurements on the slice use the samples, and say "sampled every N px"
  (a region's flux notes it as approximate).
- *Cutting plane*: the current channel drawn in the 3-D view. *View ▸ Cutting
  Plane Opacity*, or *Opacity* under *Cutting plane* in *LAYERS*, sets how much
  of the volume shows through.
- Regions are drawn on the cutting plane in their colours (the selected one
  thicker) and follow every edit made on the slice. They hide with the plane.
  With *Remote* rendering the backend draws them in its frames; a list of
  several thousand imported shapes is too large to send, and the status bar
  says so.
- *SLICE ANIMATION* (left dock), or *View ▸ Play Slices*, *View ▸ Animation
  Speed* and *View ▸ Animation Mode*: plays the channels as a movie at 2, 5,
  10, 15 or 30 fps, in *Loop*, *Bounce* or *Stop at End* mode.

## 3-D view

### Rendering modes

*Isosurface / Volume* in *DISPLAY, 3D*, or *View ▸ 3D Rendering*:

- *Volume*: semi-transparent rendering of the cube. *Blend* (or *View ▸
  Volume Rendering*): *Composite* (default), *MIP* (brightest sample along each
  line of sight, good for finding emission), *MinIP* (faintest sample, for
  absorption).
- *Isosurface*: a closed surface at the threshold, computed on the backend.
  A large cube is sampled to at most 256³ voxels; a surface still too large is
  refused with a request for a higher threshold.
- *Isosurface, several levels*: *Levels…* beside *Surface color* opens the
  levels editor. Give a count between two values, *Linear* or *Log* spaced (log
  suits a simulation's density over many decades; it needs both values above
  zero), or type the levels (`0.01, 0.1, 1`); at most 8. Each level has its
  own colour (by default the colour map's at that value) and opacity (by
  default 30 % for the lowest level, the outermost shell of an emission peak,
  rising to 100 % for the highest, the innermost), so the nested shells read
  through each other (depth peeling). *Apply* draws them without closing the
  editor; changing only colours or opacities needs no new extraction. A legend
  at the right edge lists each level highest first with its value and
  opacity. With levels in use the threshold no longer moves the surfaces (it
  still drives the volume); *One surface at the threshold* goes back. A level
  above the data's maximum has no surface and is named in the status bar; the
  surfaces together are bounded to 4 M triangles, sampling the cube as for one
  surface. Levels are drawn by the window's own 3-D view, so in a view rendered
  on the server (*Auto* on a GPU node, *Remote*) switch the 3-D view to local
  rendering; the backend can be local or remote.
- *Ray-traced light*: draws the cube as light graded by a tone curve, for
  figures and movies. See [Ray-traced volumes](raytraced-volumes.md).

*Camera* menu: *Front*, *Back*, *Top*, *Bottom*, *Left*, *Right* and *Use
Camera ROI*.

### Where the rendering happens: Auto / Local / Remote

The *Auto / Local / Remote* segment in the 3-D pane header, or *View ▸
Rendering*:

- *Local (CPU/GPU on this machine)*: the cube is drawn by this computer's GPU
  and must fit in its memory (about width × height × depth × 4 bytes).
- *Remote (GPU on backend)*: the backend renders and streams frames. Offered
  only when the backend can render (not a CPU-only backend, nor a macOS backend
  with nobody logged in). A cube larger than 6 GiB is drawn out of core from
  its brick pyramid; the first time, the banner shows *Preparing the cube for
  full resolution on the backend… N%*. A backend renders a limited number of
  views at once (8 by default); when all are busy the view waits in a queue,
  the banner shows its place and *Give up place* leaves the queue.
- *Auto*: on a remote backend that can render, a cube above the local limit
  goes to Remote; otherwise the view is local.

The *Camera* presets (Front, Back, Left, Right, Top, Bottom) turn the Remote
view too.

If the Remote view loses its stream (the computer slept, the network or the
SSH tunnel dropped, the backend closed the view), it reconnects by itself: the
banner shows the attempt and the wait before the next one, and the view comes
back with its colours, threshold, plane, regions and camera. After about two
minutes without success it stops and offers *Reconnect*; ✕ goes back to Local.

Remote rendering pays off when the backend is on another machine, since only
frames travel. Details: [Server-side rendering](../../technical/backend-api/render.md).

### The default threshold

The 3-D view starts from a threshold computed by the backend from the cube's
noise: the value above which about 100 noise voxels of the whole cube are
expected (about 4 to 5σ on typical HI cubes, higher for heavy-tailed noise).
Data that is not noise around a median (a simulation's density) starts from
the 99th percentile. Lower the threshold to look for fainter emission. Method
and validation: [Cube viewer: methods](../../technical/methods/cube-viewer.md).

### Threshold, colour range and colour bar

- *Threshold* (slider and field in *DISPLAY, 3D*): voxels below it are
  transparent; the isosurface is drawn at it. The share of voxels above it is
  written underneath (*1.00 % of voxels above*). It applies in MIP and MinIP
  too.
- Colour range (*Min* and *Max* in the LUT editor): values below Min take the
  first colour, above Max the last. It is independent of the threshold.
- Colour bar (right edge of the 3-D view): spans Min to Max, titled with
  `BUNIT`, with a notch at the threshold and the part below it dimmed. Hidden
  for the isosurface (the isosurface levels have their own legend).

### Colour maps

- *Color map* in *DISPLAY*: Inferno, Viridis, Magma, Plasma, Cividis and
  others, among them the white-to-hue maps *Purples*, *Blues*, *Greens*, *Reds*,
  *Greys* and *Oranges* (each also reversed, `_r`).
- *Edit* (3-D view active): the 3-D LUT editor, with the cube's histogram,
  the colour range (drag the cyan Min and orange Max lines, or type them), the
  map and the scale (*Linear*, *Log*, *Sqrt*, *Square*, *Power*), which applies
  to colours, opacity and the threshold slider. *Apply* sends it to the volume,
  local or remote. A positive cube spanning three or more decades opens in
  *Log*.
- *Edit* (2-D pane active): the 2-D LUT editor. Both editors stay open while
  you change channel.
- *Scale* (*Linear*, *Log*, *Sqrt*) in *DISPLAY* of the 2-D Slice and of
  every product pane: moment, line-width and Faraday maps, and the PV
  diagram. The colour bar follows it. Each pane keeps its own; the product
  remembers the last choice when it is shown again or recomputed. *Log* is
  refused, with a message, on an image with no positive values. Its lower end
  is the faint end of the positive values (their 1st percentile), but never
  more than twelve decades under the peak, so rounding residuals in an
  otherwise empty map do not flatten the display. The rule is relative to the
  data, so it also holds for physical units (a density of 1e-24 g/cm3).
- *Same range on every channel* (*DISPLAY* of the 2-D Slice): keeps one colour
  range across channels instead of stretching each channel to its own range.
  *Cube range* sets the range to the whole loaded cube. Choosing any range
  yourself turns the option on; *Reset* returns to per-channel ranges.

## Tools

Every tool is in the *Tools* menu, one submenu per group, and in *Inspector ▸
Analysis*; both are the same action. ⌘K (Ctrl+K) finds any menu entry by name.
An unavailable tool is greyed out and its tooltip says why.

Rules that decide availability:

- Slice tools (*Extract Spectrum*, *Extract PV Diagram…*, the region shapes,
  *Import Region (CRTF/DS9)…*) need the *2D Slice* or a map of the cube's sky
  plane on screen in some pane. 3-D tools (*Kinematic Lasso*, *Pick Spectrum
  on Plane Click*) need the *3D View* on screen.
- While a tool is armed, the pane it listens to has an amber border and keeps
  taking clicks even if you select another pane. *Esc* disarms it (and, with
  nothing armed, restores a maximised pane).
- Some tools depend on state: *Pin Spectrum* needs a probed spectrum, *Export
  Region* a region, *Region Properties…*, *Analyse Region* and *Delete Region*
  a selected region, *Overlay Slice on an Open Image* an open image viewer.
- Tools that need a spectral third axis (line-width map, baseline subtraction,
  stacking, polarimetry, source finding, kinematic model) are hidden when
  `CTYPE3` is spatial, as in a VBT volume. Catalogue tools are hidden without a
  celestial WCS.

### Spectral

- *Extract Spectrum*: the spectrum of the pixel under the cursor, in a
  *Spectrum (live)* pane. Hover to follow the cursor; click to freeze it, click
  again to resume. Turning the tool off (or *Esc*) keeps a frozen spectrum as a
  *SPEC* product in Session Data; a curve still following the cursor is
  discarded. On a preview the spectrum is read from the full-resolution cube.
- *Pick Spectrum on Plane Click* (also *Pick spectrum on cutting plane* in
  *INTERACTION*): click the 3-D rendering to get the spectrum at the voxel
  hit. Which voxel follows the blend: MIP the brightest on the ray, MinIP the
  faintest, Composite the first voxel above the threshold, the isosurface its
  surface, the cutting plane the voxel under the click. A sphere marks the hit
  and the status bar names the channel (*the ray hit channel 74 (…)*). The
  third axis is velocity, so what the ray crosses depends on the viewing
  direction.
- *Pin Spectrum*: saves the current probed spectrum as a product.
- *Extract PV Diagram…*: position-velocity diagram along a path. See
  [Regions, PV diagrams, noise](region-pv-noise.md).
- *Line-width Map…*, *Baseline Subtraction…*, *Stack Cubes to a Spectrum…*:
  see [Spectral analysis tools](spectral-tools.md).

### Maps

- *Compute Moment…*: see [Moment maps](moment-maps.md).
- *Channel Maps…*: a grid of channels with one colour map and one data range.
  Set *Start channel*, *End channel* (default: the current channel and the 63
  after it), *Stride*, *Columns* (default 8) and the colour map, then
  *Generate*. Every panel is titled with its channel and its value on the
  cube window's spectral axis (for example *CH 16* over *32.15 km/s LSRK*),
  keeps the pixel aspect of the image, and shows its axes only on the left and
  bottom edges; the window opens wide enough for the columns asked for.
  Double-click a panel to open it alone; *Save mosaic as PNG…* saves the grid.
- *Angular Power Spectrum of Moment Map*, *Skeleton / Voids of Moment Map*:
  need a moment map; see [Cosmology tools](cosmology.md).

### Statistics

- *Estimate Noise...*: see [Regions, PV diagrams, noise](region-pv-noise.md).
- *Pixel Histogram (current slice)*: a 256-bin histogram of the displayed
  channel. Drag the red (low) and green (high) cursors to set the slice's
  colour range. Useful to tell apart the planes of a multi-plane product cube
  (RM, angle, error, fraction).

### Polarimetry

*Load Stokes Q/U/V Companions…*, *Polarised Intensity & Angle (current
channel)*, *Rotation Measure Synthesis…*: for full-Stokes data, including
surveys with one cube per Stokes. See [Polarimetry](polarimetry.md) and
[the polarimetry use case](../use-cases/07-polarimetry.md).

### Regions

*Box Region Analysis*, *Circle Region Analysis*, *Polygon Region Analysis*,
*Annulus Region Analysis*, *Mask 3-D Region…*, *Region Properties…*,
*Analyse Region*, *Delete Region*, *Undo Region Edit*, *Redo Region Edit*,
*Import Region (CRTF/DS9)…*, *Export Region (CRTF/DS9)…*, *Send Selected Region
to Image…*, *Send All Regions to Image…* (to an open image viewer, placed on the same
sky). The window keeps an
editable list of regions, shown on the slice, on every moment or map pane of
the same sky grid and on the 3-D cutting plane. See
[Regions, PV diagrams, noise](region-pv-noise.md).

### Sources & Kinematics

- *Source Finding (SoFiA-2)…*: see [Source finding](source-finding.md).
- *Kinematic Lasso (click to select)*: double-click an object in the 3-D view
  to select it. See [Kinematic Lasso](kinematic-lasso.md).
- *Kinematic Model Overlay…*: draws a tilted-ring model from parameters you
  give (it does not fit them); *Fit from the lasso selection* fills them from
  a lasso selection. The position angle points to the receding half, from the
  top of the image toward the left (north through east for a north-up cube);
  some catalogues quote the approaching side. See
  [Kinematic model](../../technical/design/kinematic-model.md).

### Catalogue

*Load Catalogue Overlay…*, *Show Catalogue Overlay*, *Show Catalogue Labels*,
*Clear Catalogue Overlay*: sources from a CSV/TSV table drawn on the slice and
on moment maps. Needs a celestial WCS. A `.reg` or `.crtf` file chosen here is
read as regions. See [Catalogues and HiPS](catalogues-hips.md).

### Export

- *Export Selected View…*: exports what the focused pane shows; the entry
  renames itself to say what it will write:

  | Focused pane | Entry | Result |
  |---|---|---|
  | 3-D view | *Export Sub-Cube as FITS…* | a cropped cube (bounds start from the selected region), WCS kept |
  | 2-D Slice | *Export Current Channel as 2-D FITS…* | the channel as 2-D FITS |
  | A map product | *Export <map name> as FITS…* | the map as 2-D FITS, with its own `BUNIT`, the celestial WCS and its parameters as `HISTORY` |
  | A spectrum | *Export … as CSV…* | the spectrum as CSV |

  A moment map is recomputed at full precision for the export. Files go to the
  backend's Workspace Exports (default `~/.visivo/exports/`), browsable from
  the Data Hub; a name already taken gets a suffix (`cube_1.fits`).
- *Smooth / Regrid…*: Gaussian smoothing and/or integer binning of every
  plane on the backend, written as a new cube. Spatial only; the beam grows
  with the kernel. See [Image viewer](image-viewer.md).
- *Export Movie…*: an MP4 or PNG sequence. See
  [Figures, movies and linked views](publication-output.md).
- *Save View As…*: the 3-D view as a PNG (any size, optionally with a transparent
  background), PDF or SVG. See [Figures, movies and linked views](publication-output.md#save-view-as).

### Contours

- *Show Contours* (the left dock's *Contours* card, or the *Layers* row) draws
  the current 2-D slice as contours, and follows the channel. This also holds
  for the slices of simulation cubes. The settings appear in *Inspector ▸
  Parameters* while the tool is on: levels by count, by n × σ of the noise of
  the channel, logarithmic or a typed list; an optional Gaussian smoothing;
  one colour, a colour map or a colour per level; the line width; dashed
  negative levels. They are the same as for the image viewer's contours and
  are described with them in [Image viewer](image-viewer.md).
- *Export Contours…* in that panel saves the contours of the channel on screen
  as a DS9 region file or a table of vertices, in the sky frame of the window or
  in cube pixels.

### Views

- *Overlay Slice on an Open Image*: sends the current 2-D slice as contours to
  the first open image viewer.
- *Link Views*: keeps cube windows (and the panes of one window) in step:
  camera, channel (matched by spectral value), colour map and blend. The 2-D
  slice and the Product panes share pan and zoom, and every camera change is
  followed, including *Shift+R* and the *Camera* presets. The chips next to the
  button choose which facets are linked. See
  [Figures, movies and linked views](publication-output.md).
- *Link Views* also reaches image windows that have it on: the slice and the
  image follow each other by sky, and the cursor of one is a crosshair in the other.
  See [Link an image and a cube](publication-output.md#link-an-image-and-a-cube).
- *Send Selected Region to Linked Image*, *Send All Regions to Linked Image*
  (under Regions): copy regions to the image windows in the group, placed on the
  same sky.
- *Open in VR*: shows the cube on a PC VR headset through an OpenXR runtime
  (SteamVR, Oculus, Windows Mixed Reality, Monado). Needs a build with VR
  support (not the default macOS build); otherwise it says what is missing.
- *Open in VR Headset…*, *Show on Headset*, *Show Selection in VR Headset*,
  *Headset Activity*: for a Meta Quest, with no OpenXR runtime on this
  computer. See [VR](vr.md) and [VR from the desktop](../use-cases/08-vr-from-the-desktop.md).
- *Refine Selected Box…*, *Clear Refined Box*: see
  [Preview and full resolution](#preview-and-full-resolution).

## Spectrum panes

Spectra (probe, 3-D pick, region, stack) open in a pane; their controls are in
*Inspector ▸ Analysis ▸ SPECTRUM*.

- Plot: intensity (`BUNIT`) against the spectral axis from the WCS. Drag and
  wheel zoom. A dashed vertical line marks the channel shown in the slice.
- Stats bar: `N`, `Min`, `Max`, `Mean`, `RMS`, `Median`, `σ(MAD)` (1.4826 ×
  median absolute deviation) and `∫` (integral over the visible spectrum).
- *Smooth*: display-only kernel (*None*, *Hanning*, *Boxcar 3 / 5 / 7*,
  *Gaussian σ=1 / σ=2*). *Save CSV…* writes the raw data.
- *Fit Gaussian* / *Clear Fit*, *Load Lines…* / *Clear Lines*: see
  [Spectral analysis tools](spectral-tools.md).
- *Compare* / *Clear Overlays*: keeps the current curve on the plot as a
  coloured comparison curve with a legend, so spectra of several regions or
  pixels overlay. Display only; use *Tools ▸ Spectral ▸ Pin Spectrum* to keep a
  curve as a product.
- *Save PNG…*, *Save CSV…*: the picture, or the data as two columns with `#`
  lines giving the dataset, pixel and sky position.

## View and WCS menus

- *View ▸ Sky Grid on the 2-D Panes*: sky-coordinate ticks on the slice and
  maps. Frame (*Galactic*, *FK5*, *Ecliptic*) and format (*Sexagesimal*,
  *Decimal*) are in the *WCS* menu. Each axis is labelled with the coordinate
  along that edge.
- *View ▸ WCS Box in the 3-D View*: a labelled box with RA, Dec and velocity
  ticks around the volume.
- Beam: when the header has `BMAJ` and `BMIN` (and `BPA`), the 2-D slice shows
  the beam as a white ellipse in the bottom-left corner. Structures of about
  the beam's size are only marginally resolved. Beam sizes are written in the
  unit that fits them, with three significant digits: milliarcseconds below
  1″ (an ALMA beam reads *22.8 mas × 18.4 mas*), arcseconds, then arcminutes
  above 60″.

Details: [Cube viewer: methods](../../technical/methods/cube-viewer.md),
[Cube API](../../technical/backend-api/cube.md),
[Large cube loading](../../technical/design/large-cube-loading.md).
