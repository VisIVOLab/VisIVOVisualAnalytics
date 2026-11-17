# Regions, PV diagrams, noise

Three analyses drawn on a 2-D view of a cube: region statistics and spectra,
position-velocity (PV) diagrams, and noise estimation. Regions also work in the
[image viewer](image-viewer.md). The commands are in the cube viewer's *Tools*
menu and in *Inspector ▸ Analysis*.

For walk-throughs, see [Regions](../use-cases/04-regions.md) and
[Moment maps and PV diagrams](../use-cases/03-moments-and-pv.md).

## Regions

### Drawing

*Tools ▸ Regions*. One drawing tool is armed at a time; choose it again to
disarm it. With a tool armed, a press on an existing region still selects it,
and a press on empty image draws a new one.

- *Box Region Analysis*: drag between two opposite corners.
- *Circle Region Analysis*: drag from the centre to the edge.
- *Polygon Region Analysis*: click each vertex, double-click to close.
- *Annulus Region Analysis*: drag the outer radius, then type the inner one
  (drawn in amber as you type). Use it for the local background around a
  source.

Releasing the shape measures it (see [Region spectrum and statistics](#region-spectrum-and-statistics))
and adds it to the window's region list.

### The region list

Every region you draw, import or reopen from Session Data stays until you
delete it, through channel changes and after its spectrum is closed. In the
cube viewer the list is shown on the 2-D slice, on every pane showing a map
of the cube's sky plane (moment maps, line-width maps, the *Channels* pane,
Faraday maps of an RM synthesis box) and on the 3-D cutting plane, also with
*Remote* rendering. An edit in one place shows at once in the others. The
selected region is drawn thicker, lightly filled, with white handles.

| Action | Effect |
|---|---|
| Click a region | selects it; a click on empty image deselects |
| Drag a region | moves it |
| Drag a handle | reshapes it (box corner, radius, annulus radii, ellipse semi-axis, polygon vertex); the knob above a box or ellipse rotates it |
| Double-click a region | opens *Region Properties…* |
| Right-click a region | *Properties…*, *Analyse*, *Delete* |
| Delete or Backspace | deletes the selected region (a 2-D pane must have the focus) |
| ⌘Z, ⇧⌘Z | undo and redo region changes (a 2-D pane must have the focus) |

The same commands are in *Tools ▸ Regions*: *Region Properties…*, *Analyse
Region*, *Delete Region*, *Undo Region Edit*, *Redo Region Edit*. Where
regions overlap, a click on an outline picks that region, and inside, the
smallest one wins. A click on a catalogue marker selects the source.

### Regions in Session Data

*Session Data* lists the regions under the dataset, in a *Regions* node (in
the cube viewer below *3D Volume* and *2D Slice*). Each row shows the
region's name, its shape and a swatch of its colour. The regions read from
one file sit under a node named after the file, for example
`hcg44.reg (4)`.

| Action | Effect |
|---|---|
| Click a row | selects the region on the image; selecting a region on the image highlights its row |
| Check box of a row | shows or hides that region |
| Check box of a file or of *Regions* | shows or hides all of them |
| Double-click a row | opens *Region Properties…* |
| Right-click a row | *Properties…*, *Analyse*, *Rename…*, *Hide* or *Show*, *Delete*, *Export…* |
| Right-click a file or *Regions* | *Show All*, *Hide All*, *Delete All*, *Export All…* |

A hidden region stays in the list but is not drawn anywhere: not on the
slice, the map panes, the 3-D cutting plane or the *Remote* 3-D view, and a
click on the image does not pick it. Hiding, renaming and deleting are region
changes like the others: *Undo Region Edit* takes them back. Spectra and
statistics made from a region keep their own rows and carry the region's name
at the time they were measured.

### Regions on moment maps

The drawing tools also draw on a moment or map pane: arm a shape and draw in
that pane. Every pane the tool can draw in has the amber border. A shape
unfinished when its pane leaves the screen or changes map is dropped.
*Analyse* on a map pane gives the statistics of that map (the Inspector names
it); the spectrum is always the region's mean over the cube. A map not on the
cube's pixel grid (a kinematic fit over a selection's box) shows no regions.

### Region Properties

Sets a region by numbers: centre or polygon vertices, sizes, and the angle of
a box or ellipse. *Frame* chooses what the numbers mean:

- *Image pixels (1-based, as ds9)*: sizes in pixels.
- *ICRS* or *FK5 (J2000)*: degrees or sexagesimal (`hh:mm:ss.s`,
  `±dd:mm:ss`; `13h29m52.7s` and `+47 11 43` are read too).
- *Galactic*: degrees.

Sky sizes are in arcseconds; the angle is counter-clockwise from the longitude
axis, as in ds9. Sky frames need a celestial WCS. *Apply* moves the region to
the typed values.

### Region spectrum and statistics

Releasing a shape, or *Analyse Region*, produces a SPEC product (*Box 1
spectrum*) in Session Data, shown in a pane: the mean per channel over the
region, over all channels. On a preview it is the mean of the preview's
samples inside the region; the provenance's *Spectrum from* line says which.
The spectrum pane works as for a probed pixel (stats bar, smoothing,
*Compare*, *Load Lines…*, *Fit Gaussian*, CSV export; see
[Cube viewer](cube-viewer.md)).

The 2-D statistics are in the product's provenance (*Inspector ▸
Parameters*): *Valid pixels*, *Blanked / NaN*, *Min*, *Max*, *Mean*, *Median*,
*Std Dev*, measured on the plane the region was drawn on. In the image viewer a
*Region Analysis* window shows them and adds σ (MAD) (1.4826 × median absolute
deviation), the beam-aware integrated flux (*Radio (beam-aware)*, pixel sum ÷
beam area, Jy for a Jy/beam image, when the header has `BMAJ`/`BMIN`) with the
peak SNR, and per-layer values for a Stokes set. Blank pixels are left out.

Reopening the row in Session Data redraws the region, selected, and its
numbers. The Inspector's values can be copied (right-click ▸ *Copy*), and
*Export recipe as JSON…* saves them with their parameters.

Commands that take a box start from the selected region's bounding box:
*Estimate Noise...*, *Export Sub-Cube as FITS…*, *Mask 3-D Region…*, *Refine
Selected Box…* and *Rotation Measure Synthesis…*.

### Exporting regions

*Tools ▸ Regions ▸ Export Region (CRTF/DS9)…* writes regions for CARTA, CASA
or ds9, in sky (FK5) or pixel coordinates. It first asks which regions to
write: the visible ones, all of them with the hidden ones, or the selected
one. Hidden regions are written only when you choose so. It also offers a
third frame: each region in the frame it was typed in. Colours, labels and
other properties are written back. *Export…* on a row of Session Data writes
that region; *Export All…* on a file or on *Regions* writes those regions.

### Importing a region file

*Tools ▸ Regions ▸ Import Region (CRTF/DS9)…* reads a ds9 or CASA CRTF file
into the region list, in both viewers. One Undo takes the whole import back.

- Shapes: circle, ellipse, box, polygon, point, annulus (also elliptical and
  box annuli; a multi-radius annulus becomes one ring per pair of radii), and,
  drawn but not measured, lines, vectors and text.
- Frames: FK5, ICRS, FK4 (B1950), galactic and ecliptic, in degrees or
  sexagesimal, placed through the dataset's full WCS; ds9 `physical` through
  `LTM`/`LTV`. CRTF pixel positions count from 0, as CASA writes them.
- Colours, labels (`text={…}`) and other properties are kept.

A report next to the viewer lists every line that did not become a region,
with the reason (unsupported shape such as `panda` or `compass`, a frame the
backend cannot convert, a sky region without a celestial WCS, a region off the
projection, a zero size). A `.reg` or `.crtf` file opened with *Tools ▸ Catalogue ▸ Load Catalogue
Overlay…* is read the same way.

## Position-velocity diagrams

### Drawing the path

1. *Tools ▸ Spectral ▸ Extract PV Diagram…*.
2. Click on the 2-D slice (or a moment map) to place the vertices.
3. Double-click or right-click to finish. The diagram is computed on the
   backend at full resolution and opens in a pane.

While the tool is on, the path can be edited and the diagram follows:

| Gesture | Effect |
|---|---|
| Drag a handle | moves that vertex |
| Drag the path | moves the path |
| Shift + drag the path | rotates it about its centre |
| Double-click on the path | adds a vertex |
| Right-click a vertex | removes it (at least two remain) |

The wheel zooms and the middle button pans. Turning the tool off freezes the
path; turning it on again resumes editing.

### Drawing on a moment map

Click the moment pane to make it active, then arm *Extract PV Diagram…* or
the probe: the tool works on the map, which keeps its title and colours. If
the 2-D Slice was in another pane, that pane becomes *Channels*: it follows
the channel bar and shows the path too. The path is drawn on every map pane of
the same field. *2D Slice* in a pane's menu (or in Session Data) brings the
channels back.

### The diagram

Footer controls:

- *Width*: width of the cut across the path, in cube pixels; values across it
  are averaged.
- *Smooth curve*: a smooth curve through the vertices instead of straight
  segments, for a warp or spiral arm.
- *New path*: discards the path and starts a new one.
- *Save as FITS…*: writes a 2-D FITS to Workspace Exports: offset along the
  path in arcsec (`CTYPE1 = 'OFFSET'`), the cube's own spectral axis, `BUNIT`
  and beam, the width and step, and every vertex as `HISTORY`.
- *Save as PNG…*: saves the picture.

The X axis is the offset along the path, from 0 at the first vertex, in
arcsec with a celestial WCS (pixels otherwise). The Y axis is the spectral
coordinate placed by value, so a decreasing or non-linear axis is drawn
correctly. PV diagrams made with version 1.0.1 or earlier on a cube with a
decreasing spectral axis were mirrored and should be redone. The spectral axis
must be the cube's third axis.

## Noise estimation

*Tools ▸ Statistics ▸ Estimate Noise...* measures σ = 1.4826 × MAD (median
absolute deviation) per channel over a box of the cube. Pick a box in the same
field as the source, free of emission in the chosen channels, and large (a few
thousand pixels per channel).

The *Noise Estimation, Select Region* dialog:

- *Spatial region (pixels)*: *X (columns)* and *Y (rows)* ranges. Default:
  the whole cube, or the selected region's bounding box. *Reset to Full
  Extent* restores the whole cube.
- *Spectral range (channels)*: *Start → End*; default all channels.
- *Estimate*: runs it.

The box is remembered per cube, and while the dialog and its result are open
it is drawn in amber on the slice and in the 3-D view.

The result panel gives σ and MAD per channel as *Mean / Median / Min / Max*,
and warns when much of the box is blank: above 20 % blank, a note; above 50 %,
σ is computed without the exact zeros, and you should move the box inside the
data. The estimate is kept in Session Data (`Noise σ=… (x …, y …)`);
double-click the row, or right-click ▸ *Show the Numbers Again*, to reopen the
panel. A new box adds a row; the same box updates it.

The median of the per-channel σ is then used by *Compute Moment…* (*3σ of the
noise estimate*), the *Line-width Map…* mask (3σ) and the *Kinematic Lasso*
threshold, and each records it in its provenance. Without an estimate each tool
uses its own σ over the whole cube.

Details: [Regions, PV, noise: methods](../../technical/methods/region-pv-noise.md),
[Cube API](../../technical/backend-api/cube.md),
[Regions API](../../technical/backend-api/regions.md).
