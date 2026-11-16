# Regions, PV diagrams, noise

Three commonly-used interactive analyses on a 2-D slice or moment map:

| Tool | What it gives you | Where in UI |
|------|-------------------|-------------|
| [Region statistics](#region-statistics) | mean / median / σ / sum over a shape | drag on the slice |
| [PV diagram](#position-velocity-diagrams) | intensity along a path × velocity | draw a path; edit it live |
| [Noise estimation](#noise-estimation) | per-channel MAD + 1σ | choose a line-free region |

---

## Region statistics

Region tools let you draw a shape on the 2-D slice / moment map and read off
pixel statistics — useful for source photometry, background measurement,
quick comparisons.

### Drawing a region

Pick a shape from *Tools → Regions* in the viewer:

| Shape | How to draw | Notes |
|-------|-------------|-------|
| **Box** | Click + drag the two opposite corners | Axis-aligned. Fast and predictable. |
| **Circle** | Click + drag from centre to edge | Diameter snaps to integer pixel. |
| **Polygon** | Click each vertex, double-click to close | Arbitrary shape. Concave polygons are supported. |
| **Annulus** | Click centre, drag the outer radius, then type the inner one | Useful for background subtraction around a source. |

While drawing, the live preview is rendered on the slice. Once it is
released the region joins the window's list of regions you can edit (below),
in the image viewer and in the cube viewer alike.

### Editing regions

Every region you draw, import or bring back from Session Data stays on the
image until you delete it, so you can keep a source aperture and a background
ring side by side and come back to either. In the cube viewer the regions stay
on the 2-D slice while you change channel, and after their spectra are closed.
They are also drawn on every map of the cube's sky plane shown in a pane (see
[Regions on moment maps](#regions-on-moment-maps)). One region at a time is
**selected**: it is drawn thicker, lightly filled, with white handles.

| Action | Effect |
|---|---|
| Click a region | selects it; clicking empty image deselects |
| Drag a region | moves it |
| Drag a handle | box corner (the opposite corner stays put), circle radius, annulus inner or outer radius, ellipse semi-axis, polygon vertex; the knob above a box or ellipse rotates it |
| Double-click a region | opens **Region Properties** |
| Right-click a region | *Properties…*, *Analyse*, *Delete* |
| **Delete** / **Backspace** | deletes the selected region (the image, the slice or a map pane must have the keyboard focus) |
| **Cmd+Z** / **Shift+Cmd+Z** | undoes / redoes the last change to the regions: drawing, moving, reshaping, typing, deleting |

The same entries are in *Tools ▸ Regions* (*Region Properties…*, *Analyse
Region*, *Delete Region*, *Undo Region Edit*, *Redo Region Edit*). In the cube
viewer Cmd+Z undoes a region change while the slice or a map pane has the keyboard focus and
there is one to undo; elsewhere it still undoes the assistant's display
changes. A click on a catalogue marker still
selects the source, even inside a region. Where regions overlap, a click on an
outline picks that region; inside, the smallest region under the cursor wins.
*Analyse* (or the menu entry *Analyse Region*) measures the selected region
exactly as drawing it does; the Σ / μ readout follows the selected region while
you drag it.

### Regions on moment maps

In the cube viewer every pane that shows a map of the cube's sky plane carries
the same region list as the 2-D slice: moment maps, line-width maps, the
*Channels* pane, and the Faraday maps of an RM synthesis run over a box of the
field (placed at that box). Selection, handles, dragging, double-click for
Region Properties, the right-click menu, Delete and Cmd+Z work there as on
the slice, and a change made in one pane shows at once in the others and on the
3-D cutting plane.

The drawing tools draw on the map itself: arm *Box*, *Circle*, *Polygon* or
*Annulus* and draw in the moment pane, and the region joins the list. While a
tool is armed, every pane it can draw in wears the amber border. A region is
drawn in the pane it was started in; if that pane leaves the screen or changes
map before the region is finished, the unfinished region is dropped.

*Analyse* measures the map of the pane you are working in: on a moment pane the
statistics are those of the moment map (the Inspector names it, with the pane),
and the spectrum is, as always, the region's mean over the cube. A map that is
not on the cube's pixel grid (a kinematic fit over a selection's box) does not
show the regions.

**Region Properties** sets a region by numbers: its centre (or polygon
vertices), sizes, and angle for a box or an ellipse. The *Frame* list chooses
what the numbers mean:

- *Image pixels (1-based, as ds9)*: the first pixel is 1, sizes in pixels;
- *ICRS* or *FK5 (J2000)*, in degrees or sexagesimal (`hh:mm:ss.s`,
  `±dd:mm:ss`; `13h29m52.7s`, `+47 11 43` and plain degrees are read too);
- *Galactic*, in degrees.

Sizes on the sky are in arcseconds, and an angle is counter-clockwise from the
longitude axis, as ds9 writes it. Sky frames are offered only when the image
has a celestial WCS. Every conversion is done by the backend with the image's
full WCS (projection, rotation, distortion), never by the viewer's own axis
labels, so a catalogue position typed in ICRS lands on the pixel ds9 would put
it on. The numbers follow the region while you move it on screen, unless you
have started typing; **Apply** puts the typed values on the image. The dialog
opens in the frame the region was last typed in, or the image's own sky frame.

```{note}
Only **one** drawing tool can be armed at a time: the four region actions
(Box / Circle / Polygon / Annulus) are an exclusive group in both the menu and
the sidebar. Clicking the armed shape again disarms it. With a tool armed, a
press on an existing region still selects and edits it; a press on empty image
draws a new one.
```

In the cube viewer the analyses that take a box start from the **selected
region's bounding box**: *Estimate Noise*, *Export Sub-Cube*, *Mask 3-D
Region*, *Refine Selected Box* and *RM Synthesis*. *Export Region* writes all
regions or the selected one. Regions are kept in the cube's full-resolution
pixels at double precision, so a region typed or imported at a fractional
position stays there.

### Region spectrum (per-channel mean over the region)

Releasing the region (or *Analyse* on a selected one) produces a **SPEC
product** named after the region (*Box 1 spectrum*): it appears in Session
Data and is placed in a pane straight away — a free one, or by growing the
layout; with no room it opens in a window of its own. Opening the row again
puts its region back in the list, selected.
The 2-D statistics of the region are **not** a separate window: they are the
product's Provenance, so selecting the row in Session Data and opening
Inspector ▸ Provenance shows the region shape, the valid-pixel count and
min / max / mean / median / σ, while the status bar reports the headline numbers.

The plot itself is **mean per channel over the region** across **all
cube channels** (not just the current slice). It is read from the cube the
3-D view holds: at full resolution every pixel of the region counts; on a
preview (every *n*-th pixel, or block means) the preview's samples whose
positions fall in the region count, and the provenance's *Spectrum from* line
says which (a region smaller than the preview's spacing takes the sample
nearest its centre). The stats bar
(`N / Min / Max / Mean / RMS / ∫`), channel marker, theme, and CSV
export all work identically to pixel-probe mode; so do
[line identification](spectral-tools.md#line-identification-overlaying-a-line-list)
and the [Gaussian fit](spectral-tools.md#gaussian-line-fit).

To **compare several regions**, click **Compare** in the Inspector's SPECTRUM
panel before drawing the next region: the current mean spectrum is kept as a
colour-coded comparison curve and the next region's spectrum overlays on
the same axes with a legend. See
[Comparing regions (overlaid spectra)](cube-viewer.md#comparing-regions-overlaid-spectra).

### Statistics shown

In the **cube viewer** the region's Provenance lists **Valid pixels**,
**Blanked / NaN**, **Min**, **Max**, **Mean**, **Median** and **Std Dev**,
computed on the plane the region was drawn on. In the **image viewer** a
*Region Analysis* window shows them, and adds:

```{list-table}
:header-rows: 1
:widths: 22 78

* - Field
  - Meaning
* - **σ (MAD)**
  - Median absolute deviation × 1.4826 (robust 1σ estimate). Use this for
    background-noise estimates; less affected by faint sources than the
    sample standard deviation.
* - **Integrated flux** (*Radio (beam-aware)*)
  - Pixel sum ÷ beam area in pixels, in Jy for a Jy/beam image. Shown only
    when the header has a beam (`BMAJ`/`BMIN`), together with the beam, its
    area in pixels and the **Peak SNR** (peak / σ_MAD).
* - **Stokes (per-layer)**
  - With a Stokes set loaded, the same region measured on each Stokes and
    derived layer (flux for I/Q/U/V/P, mean for angle, fraction, α and RM).
```

Pixels that are blank (NaN) are left out of every number. The backend's
`POST /v1/regions/stats/{dataset_id}` also returns a flux; on a cube with a
per-channel `BEAMS` table it uses the beam of the requested channel, and the
header beam otherwise.

```{tip}
An **annulus** measures the ring between its inner and outer radius, so it is
the shape for the local background around a source: subtract its mean, times
the source aperture's pixel count, from the aperture's sum.
```

Every region analysis leaves a row in **Session Data** carrying both its numbers
and the shape they were measured on (in the image viewer, one row per region:
measuring the same region again replaces its row). Opening that row **redraws the
region** (in the image viewer it goes back into the region list, selected) and
reopens the statistics, so a measurement taken twenty minutes and three regions
ago can still be traced back to the pixels that produced it. The same holds for a
pinned spectrum in the cube viewer: opening its row puts the crosshair back on
the pixel it was probed at and returns to that channel.

Drawing an annulus takes two steps: drag out the **outer** radius, then type the
**inner** one when prompted. The inner ring is drawn **as you type it**, in the
same amber the crop and mask previews use, so you can see the band you are about
to measure before committing to it; it takes the region's own colour once you
accept, and disappears if you cancel. (Before, the prompt was a bare number box
and the ring appeared only after you had already chosen.)

### Sending the result somewhere

- Select the region's row in **Session Data**: the Inspector shows its
  numbers, which can be selected and copied (right-click ▸ *Copy*), and
  **Export recipe as JSON…** saves them with the parameters they were
  measured with.
- *Tools ▸ Regions ▸ Export Region (CRTF/DS9)…* writes the region itself, for
  CARTA, CASA or DS9, in sky (FK5) or pixel coordinates; FK5 is written
  whatever the image's own frame. In the image viewer it asks first whether to
  write all the regions or the selected one, and offers a third frame choice:
  each region in the frame it was typed in (FK5 for a region drawn on the
  image).

### Importing a region file

*Tools ▸ Regions ▸ Import Region (CRTF/DS9)…*, in the image viewer and in the
cube viewer, reads a ds9 or CASA CRTF file into the window's regions:

- **Every region of the file**, as one change: one Undo takes the whole
  import back. The first region is selected (in the cube viewer the first one
  with an area is measured).
- **Every shape**: circle, ellipse, box, polygon, point, annulus, and the
  shapes that are drawn but not measured, which are lines (ds9 `line`,
  `ruler`, `segment`, `projection`; CRTF `line`), vectors (an arrow from a
  start, a length and an angle) and text. Elliptical and box annuli are
  measured between their two edges. An annulus written with several radii
  (`annulus(x,y,r1,r2,r3)`) becomes one ring per pair of radii, named
  *Annulus, line 4, ring 1/2*; dragging one ring moves them all.
- **Every frame and coordinate format**: FK5, ICRS, FK4 (B1950), galactic and
  ecliptic, in degrees or sexagesimal, are placed through the dataset's full
  WCS by the backend (rotated or skewed grids and unequal pixel scales
  included), at sub-pixel precision; ds9 `physical` goes through the image's
  LTM/LTV keywords, and each size keeps its own unit (`1'`, `30"`, `0.01d`).
  CRTF pixel positions are read as CASA writes them, counting from 0.
- **Colours, labels and the other properties** of the file are kept: a
  `text={…}` label is drawn above its region, and an export writes the
  properties back.

Every line that does not become a region is listed in a report that opens
next to the viewer (it does not block it): the line number, the line as
written, and the reason (a shape not supported, such as ds9 `panda` or
`compass`; a frame the backend cannot convert, such as ds9 `linear` or CRTF
`SUPERGAL`; a sky region on an image without a celestial WCS; a region off
the projection; a size of zero). The status bar says how many regions were
read and how many lines were not.

A `.reg` or `.crtf` file picked in *Tools ▸ Load Catalogue Overlay* is read
the same way, as regions; the catalogue overlay takes tables.

---

## Position-velocity diagrams

A **position–velocity (PV) diagram** is a 2-D plot that shows intensity
along a chosen spatial path (one axis) versus velocity / frequency (the
spectral axis of the cube, the other axis). It is the standard way to
inspect kinematic structure: rotation curves, warps, outflows, jets,
expansion.

### Drawing the path

1. In the cube viewer choose **Tools → Spectral → Extract PV Diagram…**. The cursor
   becomes a cross.
2. Click on the 2-D slice to place the vertices of the path, as many as you
   like. Vertices are kept at sub-pixel precision.
3. **Double-click** or **right-click** to finish the path. The diagram is
   computed on the backend and opens in a pane.

You can draw the path on a **moment map** instead of a channel: arm the tool
while the map is on screen (see
[Drawing on a moment map](#drawing-on-a-moment-map)).

### Editing the path

The finished path stays on the image with a handle on each vertex, and while
*Extract PV Diagram* is on it can be edited. The diagram follows every change.

| Gesture | Effect |
|---|---|
| Drag a handle | moves that vertex |
| Drag the path | moves the whole path |
| **Shift** + drag the path | rotates it about its centre |
| Double-click on the path | adds a vertex there |
| Right-click on a vertex | removes it (a path keeps at least two) |

The mouse wheel zooms and the middle button pans while the tool is armed.

While you drag, the diagram is recomputed a fraction of a second after the
pointer pauses; if a computation is still running, only the latest shape is
computed next, so nothing piles up.

Turning *Extract PV Diagram* off keeps the path on screen but frozen; turning
it on again resumes editing the same path. **New path** in the diagram starts
over.

### The diagram window

The footer of the diagram holds the path controls, so none of them needs a
dialog:

| Control | Meaning |
|---|---|
| **Width** | width of the cut across the path; the values across it are averaged |
| **Smooth curve** | cut along a smooth curve through the vertices (centripetal Catmull-Rom) instead of straight segments |
| **New path** | discard this path and draw a new one |
| **Save as FITS…** | write the diagram to the Workspace Exports (below) |
| **Save as PNG…** | save the picture |

The header repeats the path (start, end, vertices, length) and the sampling
(width, spectral axis, spatial unit, scale). A colour bar on the right carries
the cube's `BUNIT`.

### What is computed

- **X axis**: offset along the path, in arcsec when the cube has a celestial
  WCS (pixels otherwise), starting at 0 at the first vertex.
- **Y axis**: the cube's spectral coordinate (velocity, frequency, …), placed
  by **value**, so a spectral axis that decreases (`CDELT3 < 0`, the usual case
  for HI cubes) or that is not linear (e.g. `VOPT-F2W`) is drawn where it
  belongs.
- **Sampling**: one sample every pixel *along* the path (the finer pixel size
  when pixels are not square, measured on the sky), so the offset axis is
  exactly linear also for diagonal and curved paths. Values are bilinear at
  sub-pixel positions; a blank (NaN) neighbour is left out instead of blanking
  the sample. Across the path, *Width* samples one pixel apart are averaged.
- The computation runs on the backend, at **full resolution**, and reads only
  the pixels around the path, so it works on large and remote cubes.

```{important}
Up to version 1.0.1 the diagram of a cube with a **decreasing** spectral axis was drawn
mirrored against its own axis: emission appeared at the wrong velocity (e.g. a
source at the centre of the band near one end). Diagrams made with earlier
versions on such cubes should be redone.
```

The PV needs the spectral axis to be the cube's last axis (e.g. `NAXIS3`); a
transposed cube is refused with a message rather than cut along a spatial axis.

### Saving the diagram as FITS

**Save as FITS…** asks for a file name and writes a 2-D FITS image to the
Workspace Exports (browsable from the Data Hub):

| Card | Content |
|---|---|
| `CTYPE1 = 'OFFSET'`, `CUNIT1 = 'arcsec'` | offset along the path, `CRVAL1 = 0` at the first vertex, `CDELT1` = sample step |
| `CTYPE2`, `CUNIT2`, `CRVAL2`, `CDELT2`, `CRPIX2` | the cube's **own** spectral axis, algorithm and parameters included, so a non-linear axis stays exact |
| `BUNIT`, `BMAJ`, `BMIN`, `BPA` | from the cube |
| `RESTFRQ`, `SPECSYS`, … | copied when present |
| `PVWIDTH`, `PVSTEP` | width (cube pixels) and sample step (arcsec) |
| `HISTORY` | every vertex, in pixels and in sky coordinates |

The file is computed from the path the **displayed** diagram was made from.

### Drawing on a moment map

Open the moment map in a pane (Session Data, or the pane's menu), then arm the
tool — *Extract PV Diagram* or the probe. The tool draws on the map:
the pane keeps the map's title and colours, and the path or probe sit
on its pixels. (Regions are drawn on the map pane directly, see
[Regions on moment maps](#regions-on-moment-maps).) This is the way to draw a PV path along a disk or a filament seen
in moment 0, or across the velocity gradient of moment 1.

Click in the moment pane first, so that it is the active one: the tool always
works in the active pane. If the 2-D Slice was on screen in another pane, that
pane becomes **Channels**: it keeps showing the current channel (display only),
follows the channel bar, which moves onto it, and shows the PV path too, so you
can follow the cut through the channels while you draw it on the map. The path
is also drawn on every other map pane of the same field.

Choose *2D Slice* in either pane's menu (or in Session Data) to stop drawing on
the map: the panes go back to how they were.
A region measured on the map records the map (and its version) as what it
measured, and the probe reports the map's unit.

### Tips

- Place the path **along** the kinematic axis (the major axis of a disk, the
  outflow axis) for the clearest signature; use *Smooth curve* for a warp or
  a spiral arm.
- Use *Slice playback* or a moment map first to see where the emission lives.
- For channel-by-channel comparison keep one colour scale: *DISPLAY → Same
  range on every channel* (see
  [One colour range for every channel](cube-viewer.md#one-colour-range-for-every-channel)).

### Caveats

- The PV is computed on the full-resolution cube on the backend, not on the
  downsampled preview; a long, wide path on a very large cube takes longer.
- It is a heavy backend task, throttled so that slice scrolling stays
  responsive while it runs.

---

## Noise estimation

A robust noise (σ) estimate per channel is essential for setting moment
masks, deciding line-free channels for baseline subtraction, and computing
SNR for sources. VisIVO computes the noise on the **backend** using a
**median absolute deviation (MAD)** estimator over a user-defined spatial
region and channel range.

### How to use it

1. **Tools → Statistics → Estimate Noise...** in the cube viewer.
2. The *Noise Estimation, Select Region* dialog appears with six spinboxes:
   - **Spatial region (pixels)** — *X (columns)* and *Y (rows)*, each a
     start → end range in slice pixel coordinates. Defaults to the full cube
     extent. **Reset to Full Extent** restores the defaults.
   - **Spectral range (channels)** — *Start → End*. Defaults to all channels.
     For per-channel σ across the full cube, leave defaults; for a single
     scalar σ over a specific line-free band, narrow to those channels.

   The region you pick is **remembered per dataset**: the next time you open
   this cube the dialog re-opens pre-filled with your last noise region, so you
   don't have to hunt for the same line-free box again. With a region selected
   on the slice, its bounding box fills the spatial range instead (the
   channels stay as last used).
3. Click **Estimate**. The dialog closes and the backend starts computing.
4. The result panel shows summary stats over the per-channel arrays:
   - **σ per channel** = 1.4826 × MAD (robust 1σ for Gaussian noise),
     summarised as *Mean / Median / Min / Max*.
   - **MAD per channel**, same four summary stats.
   - **Subtitle** with the selected region and a warning when too much of
     the selection is blank — see [Blank-pixel handling](#blank-pixel-handling).
5. The estimate is also recorded in **Session Data** as a statistics row
   (`Noise σ=… (x …, y …)`), with σ/MAD, the region and the channel range in
   its **Provenance** tab. The result panel is a transient window, and σ is the
   number you reach for afterwards — the moment-map threshold, the isosurface
   level, the mask cut-off — so closing the panel no longer loses it. Re-running
   the *same* region updates that row in place; a different region adds its own,
   so two noise regions can be compared side by side.
   **Double-click that row (or right-click ▸ Show the Numbers Again) to bring the
   result panel back** exactly as it was: it is rebuilt from what the product
   recorded, so it outlives the panel you closed, the tool window and anything
   else you do in between.
6. **σ is then used by the tools that need a noise level** — measured once, on
   a region you chose, instead of each tool guessing its own:
   - *Compute Moment* offers **3σ of the noise estimate** as the mask threshold
     (chosen by default; Auto and a typed value remain available);
   - the *Line-Width Map* turns its mask on at 3σ of it;
   - the *Kinematic Lasso* counts its threshold in units of it (its panel shows
     which σ it uses).

   Each records the σ and the region in its product's provenance. The value is
   the median of the per-channel σ. Without an estimate every tool falls back to
   its own robust σ over the whole cube, as before. A new estimate replaces the
   previous one; a lasso selection on screen is grown again with it.

### Live preview overlays

While the *Noise Estimation* dialog is open — and through the result dialog's
lifetime — the cube viewer paints two synchronised amber overlays so you
can see exactly where σ is being sampled:

- **2D slice** — an amber outline rectangle following `(x0..x1, y0..y1)`.
  Distinct colour from the cyan Box-region tool so the two never collide.
- **3D volume** — a semi-transparent amber box (translucent face +
  wireframe edge) that mirrors the same `(x, y)` rectangle and extrudes
  it along the spectral axis over `chStart..chEnd`. The volume rendering
  stays visible through the translucent face so you can see emission
  features inside the sampling box.

Both overlays update **live** as you tune any of the six spinboxes —
including the channel range, so the 3D box stretches/shrinks along Z in
real time.

The overlays survive the picker → compute → result-dialog sequence so
you can read the σ numbers while still seeing where they came from. They
are torn down automatically when you close the result dialog.

```{tip}
The 3D box is the quickest way to verify your channel range avoids the
line: rotate the camera so you look down the spectral axis, then resize
the box along Z until it sits in a flat (line-free) region of the volume.
```

### Blank-pixel handling

Many radio / IR cubes use exactly `0.0` for off-source pixels instead of
NaN. If you compute MAD over a region that mostly overlaps that blanked
area, the formula collapses (`median(finite)=0` AND `median(|finite-0|)=0`)
and you get `σ=0` for every channel — clearly wrong, but easy to miss
without context.

The backend has a **zero-as-blank heuristic** in
`worker_noise_estimate`: when more than half the finite pixels in a
channel are exactly 0, those zeros are excluded from that channel's MAD
(treated as blanks). Zeros are kept in when they are a minority because
some cubes (post-baseline subtraction, calibrated brightness
temperatures) legitimately have noise centred on zero.

The response also carries a `blank_fraction` field (0–1) covering the
whole selection. The result-dialog subtitle surfaces it:

- `blank_fraction > 20 %` → soft warning "X % of the selected pixels are
  blank, result still computed over all finite pixels".
- `blank_fraction > 50 %` → stronger advice "σ excluding zeros — pick a
  tighter region inside the data footprint for a clean estimate", and
  σ/MAD are computed on the non-zero subset only.

When you see this warning, restrict the spatial region to a clearly-non-
blank area (use the amber overlays as a guide — anywhere the box sits
over the black border of the slice is contributing zeros).

### Why MAD instead of stddev?

A sample standard deviation is dominated by outliers (faint sources, RFI,
edge artefacts). MAD × 1.4826 returns the same value for purely Gaussian
data but is unaffected by ≤50 % contamination. This is the de-facto
robust estimator in radio astronomy.

### Picking a noise region

The region should be:

- **In the same field of view** as your source (similar PSF, primary
  beam attenuation).
- **Line-free** in the spectral range of interest (otherwise you'll
  measure line + noise, not noise).
- **Large enough** to be statistically meaningful (a few thousand pixels
  per channel is comfortable; smaller regions become noisy at the σ
  level).

```{tip}
A common workflow: open the cube, draw a large Box region (its mean spectrum
appears in a pane) to spot the line-free channels, then estimate noise over a generous spatial
region restricted to those channels.
```

---

## See also

- [Moment maps](moment-maps) — apply the noise as RMS / threshold mask.
- [Spectral tools](spectral-tools) — baseline subtraction uses line-free
  channels you can identify with the noise tool.
- Developer reference: [`/v1/cube/noise`](../backend-api/cube.md),
  [`/v1/cube/pv`](../backend-api/cube.md).
