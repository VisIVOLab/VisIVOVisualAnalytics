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
| Right-click a region | *Properties…*, *Analyse*, *Background Region*, *Delete* |
| Delete or Backspace | deletes the selected region (a 2-D pane must have the focus) |
| ⌘Z, ⇧⌘Z | undo and redo region changes (a 2-D pane must have the focus) |

The same commands are in *Tools ▸ Regions*: *Region Properties…*, *Analyse
Region*, *Delete Region*, *Undo Region Edit*, *Redo Region Edit*, and the
setting *HI Mass Distance…*. Where regions overlap, a click on an outline
picks that region, and inside, the smallest one wins. A click on a catalogue
marker selects the source.

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
| Right-click a row | *Properties…*, *Analyse*, *Rename…*, *Hide* or *Show*, *Background Region*, *Delete*, *Export…* |
| Right-click a file or *Regions* | *Show All*, *Hide All*, *Delete All*, *Export All…* |

A hidden region stays in the list but is not drawn anywhere: not on the
slice, the map panes, the 3-D cutting plane or the *Remote* 3-D view, and a
click on the image does not pick it. A hidden region selected from its row is
edited, deleted or exported from that row: *Region Properties…* and *Delete
Region* in *Tools ▸ Regions*, and Delete or Backspace on the image, do not
act on it. A file's node counts its hidden regions, for example
`hcg44.reg (4, 1 hidden)`. Hiding, renaming and deleting are region
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
*Std Dev*, measured on the plane the region was drawn on, and on a channel or a
moment-0 map the flux (see [Region flux and HI mass](#region-flux-and-hi-mass)).
In the image viewer a *Region Analysis* window shows them and adds σ (MAD)
(1.4826 × median absolute deviation), the flux (*Flux*) with the peak
SNR, and per-layer values for a Stokes
set. Blank pixels are left out.

### Region flux and HI mass

*Analyse* gives what the sum of a region means in the map's unit (`BUNIT`,
or the unit inferred from `TEMPSCAL`/`BTYPE`/`LABEL`), in both viewers:

| Unit | Result |
|---|---|
| Jy/beam (with a beam) | *Integrated flux* S = (Σ − N · mean_bg) / Ω, Jy or Jy km/s |
| Jy/pixel | *Integrated flux* S = Σ − N · mean_bg, Jy or Jy km/s |
| MJy/sr, Jy/sr | *Integrated flux* S = (Σ − N · mean_bg) · Ω_pix, Jy (SPIRE maps are extended-source calibrated) |
| K, K km/s | *Mean W* (or *Mean T*) and *Σ W ΔΩ* in K km/s arcsec², with ΔΩ the pixel area. Not converted to Jy: T_A* needs the main-beam efficiency η_mb, and *Flux note* says so |
| unknown, or Jy/beam with no beam | no flux, and the reason |

| Value | Meaning |
|---|---|
| *Integrated flux* | as above, with its error |
| *Flux error from* | the background region and its pixel count, mean and σ, or the map noise |
| *Beam area* | Ω = π · BMAJ · BMIN / (4 ln 2) in pixels, and the beam |
| *Flux note* | how the unit was read, and when the numbers are approximate |
| *Flux warning* | the map looks masked or thresholded (see below) |
| *HI mass* | M_HI = 2.356 × 10⁵ D² S M☉ (D in Mpc, S in Jy km/s), on a moment-0 map in Jy of the 21-cm line: none when the rest frequency is another line's (CO), and a note when there is no rest frequency |

Σ is the sum of the region's N valid pixels. A moment-0 map is in Jy/beam
times the cube's spectral unit, and a velocity axis in m/s is converted to
km/s. A frequency axis is converted with the rest frequency (Δv = c Δν / ν₀)
in the cube viewer, as the backend's HI products do; without a rest frequency,
and in the image viewer (an exported map has none), it stays in Jy Hz. A peak
or mean map gets no flux. A map whose unit is unknown is never taken as
Jy/beam: it gets no flux, and the row says the unit is unknown.

*Error*. The pixels within a beam are correlated, so the region holds N / Ω
independent beams (one, when it is smaller than a beam), and with a
background region the error of its mean, subtracted N times, adds in
quadrature. For Jy/beam:

σ_S² = σ² N min(N, Ω) / Ω² + σ² N² min(N_bg, Ω) / (N_bg Ω²)

with N_bg the background region's valid pixels. The other units use the same
count times their own per-pixel factor; without a beam the pixels count as
independent, and *Flux note* says the error is then a lower limit. A σ that cannot be measured
(zero, or fewer than two pixels) gives no error.

*Background region*. Right-click a region (on the image or in Session Data)
and tick *Background Region*. *Analyse* on another region then subtracts the
mean of the nearest background region, measured on the same map, and takes its
standard deviation as σ. Session Data marks it `· background`; it stays one
when hidden. It is ds9's `background` property, so a region file with
`# background` brings it in and an export writes it back. Without a background
region nothing is subtracted, σ is the MAD σ of the whole map (in the image
viewer, of the pixels loaded; below full resolution, no error), and the second
term goes.

*Masked moment maps*. A moment computed with *Enable threshold mask* drops the
faint emission and the noise: the flux comes out high and σ small or zero.
*Flux warning* says so when the moment was computed with the mask, or when
more than a fifth of the noise pixels are exactly 0 or blank (an exported
masked map). Compute the moment with the mask off for fluxes.

*Beam*. A cube with one beam per channel (a `BEAMS` table) uses its header
beam, and *Flux note* says so; an exported moment map carries the beam of the
central channel of its range.

*Distance*. *Tools ▸ Regions ▸ HI Mass Distance…* takes a distance in Mpc
(`25`) or a redshift (`z=0.0046`). A redshift gives the luminosity distance
D_L in Planck18 and the mass M_HI = 2.356 × 10⁵ D_L² S / (1 + z) M☉, the flux
being measured in the observed frame (Meyer et al. 2017, PASA 34, 52). Below
cz = 1000 km/s peculiar velocities (about 300 km/s) dominate a distance from
the redshift, and the result says so: type a distance instead. The setting is
kept between sessions and used by both viewers. Leave it empty to show no
mass.

On a reduced map (a preview) the flux is computed from the displayed pixels,
each counted for the block it stands for. The σ of block means is not a
pixel's, so flux and error are marked approximate.

Example: HCG 44 (`hgc44.fits`), moment 0 over channels 21 → 101 with the mask
off, circles on the galaxies and an annulus between 4′ and 5.3′ around the
central pair marked as background (2186 pixels). NGC 3190, NGC 3187 and
NGC 3185 give 3.32 ± 0.27, 5.80 ± 0.35 and 1.36 ± 0.35 Jy km/s (Ω = 30.66
pixels, σ_bg = 48.17 Jy/beam m/s), and at 25 Mpc 4.9, 8.5 and 2.0 × 10⁸ M☉.

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
that region; *Export All…* on a file or on *Regions* writes those regions,
all of them unless you choose the visible ones only.

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
