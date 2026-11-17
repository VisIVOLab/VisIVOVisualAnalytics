# Image viewer

The image viewer shows 2-D FITS images: single-plane maps, mosaics, moment
maps exported from a cube and other 2-D products. It opens when you open a
file the backend classifies as an image (*File ▸ Open…*). It also opens
dynamic spectra and beamformed data (time × frequency), with a set of
time-series tools in place of the sky tools (see
[Dynamic spectrum mode](#dynamic-spectrum-mode)).

For walk-throughs, see [Regions](../use-cases/04-regions.md),
[Catalogues, HiPS and cross-match](../use-cases/06-catalogues-hips-crossmatch.md)
and [Polarimetry](../use-cases/07-polarimetry.md).

## Window layout

- Left column: *LAYER SETTINGS* (*Color map* with its *Edit*, layer opacity),
  the *Layers* list with *＋ Add layer…*, and the overlays list.
- Inspector (*View ▸ Inspector Panel*): *Properties* (dataset, path, pixel
  values, `BUNIT`, extent), *Analysis* (every tool, grouped as in the Tools
  menu), *Parameters* (the settings of the tool in use, shown when you start
  it) and, on a backend dataset, *Copilot*.
- Mode strip under the toolbar: *Pan*, *Probe*, *Region*, *Ruler*,
  *Annotate*. One mode is armed at a time and decides what a click on the
  image does; *Region* adds a shape strip (*Box*, *Circle*, *Poly*,
  *Annulus*). A short line to the right says what the mode expects.
- Status bar: cursor readout, coordinate frame, a *WCS* badge (yellow when
  the backend had to repair the header; the tooltip lists the changes) and a
  *full-res* badge once the full image has loaded.

The window first shows a downsampled preview and loads the full image in the
background. Very large images are shown at a reduced level of detail that
follows your zoom: readouts and overlays always give full-resolution pixel and
sky coordinates, while the status bar says when a displayed pixel stands for a
block of source pixels (*each pixel the mean of N×N*, or *sampled: 1 pixel in
N×N*). Images over 256 MiB (as float32) get averaged overviews, built once on
the backend and kept in the exports folder under `.pyramids` (about 4/3 of the
image's float32 size; delete them to reclaim the space).

## File menu

- *Add Image Layer…* (⌘L, also *＋ Add layer…*): overlays another FITS image
  from the backend, aligned through both WCSs. A file that covers none of
  this image's sky is not added; you can choose *Open in New Window*, *Add
  Anyway* (the overlap check uses a bounding box, which can be wrong for a
  rotated field across RA = 0) or *Cancel*. An image with no celestial WCS
  is refused.

Each layer is a card with its visibility box, name, colour-map strip, stretch
and opacity; the base layer is tagged `base`. Drag the cards to change the
stacking order.

## Header

Click the *IMAGE* tag in the toolbar to see the FITS header, then *Edit…* to
change, add or remove keywords. The result is saved as a new file in the
Workspace; the original is never changed. `SIMPLE`, `BITPIX`, `NAXISn`,
`BSCALE`, `BZERO` and `BLANK` cannot be edited. On saving, the backend reports
the WCS the edited header describes.

## WCS menu

- *Galactic*, *FK5*, *Ecliptic*: the frame of the coordinate readout and of the
  axes.
- *Sexagesimal*, *Decimal*: the coordinate format.

When the header has `BMAJ` and `BMIN` (and optionally `BPA`), the beam is drawn
as an ellipse in the bottom-left corner.

## View menu

- *Show WCS Axes*: ticks with sky coordinates along the bottom and left edges.
- *Blink Layers*: alternates layers 0 and 1 (default every 250 ms; the *Blink
  / Compare* card sets the interval, 50 to 1000 ms). Needs two layers.
- *Inspector Panel*: shows or hides the Inspector.
- *Diagnostics*: the application log, including load errors and WCS repairs.

## Colour map and stretch

- *Color map*: the colour table, with a preview of each.
- *Edit*: the 2-D LUT editor. Drag the control points; the scale (Linear, Log,
  Sqrt, Square, Power γ) is set here.
- Layer opacity: blends the selected layer over those below.

Blank (NaN) pixels are transparent.

## Overlays list

One row per overlay drawn on the image (catalogue sources, labels,
polarisation vectors, WCS axes, annotations), each with its object count. A
row is a switch, the same one as the matching menu entry; it is greyed out
when nothing is loaded.

## Tools menu

The *Tools* menu and *Inspector ▸ Analysis* list the same tools in the same
groups.

### Tools ▸ Spectral

- *Extract Spectrum*: only in dynamic-spectrum mode. Collapses the waterfall
  over the selected region's bounding box, or over the whole image when no
  region is selected.

### Tools ▸ Regions

- *Box Region Stats*, *Circle Region Stats*, *Polygon Region Analysis*,
  *Annulus Region Analysis*: arm a region shape. Drag on the image (polygon:
  click the vertices, double-click to close).
- *Region Properties…*, *Analyse Region*, *Delete Region*, *Undo Region Edit*,
  *Redo Region Edit*: act on the selected region.
- *HI Mass Distance…*: the distance for the HI mass of a region on a
  moment-0 map.
- *Import Region (CRTF/DS9)…*, *Export Region (CRTF/DS9)…*: read and write
  region files.

While you drag, a box in the bottom-left corner shows the running sum (Σ) and
mean (μ); on regions over about 200 000 pixels these are sampled and marked
`≈`. On release, the *Region Analysis* window gives count, minimum, maximum,
sum, mean, median, standard deviation and σ_MAD (1.4826 × the median absolute
deviation). When the view is reduced, or the region reaches beyond the loaded
part of the image, these are computed on the full-resolution file, and the
subtitle says what they were computed on. Quote these figures, not the live
readout. With a beam in the header, *Radio (beam-aware)* gives the beam size,
the beam area Ω = π·BMAJ·BMIN/(4 ln 2) in pixels, the integrated flux sum / Ω
with its error, and the peak SNR in σ_MAD. The flux is in the image's unit
over the beam: Jy for a Jy/beam image, Jy km/s for a moment-0 map in Jy/beam
m/s or Jy/beam km/s (an exported moment map keeps the cube's beam). An image
with no `BUNIT` is taken as Jy/beam, and the window says so; an image not per
beam (K, MJy/sr) gets no flux. The error and the background follow the rules
of the cube viewer ([Region flux](region-pv-noise.md#region-flux-and-hi-mass)):
a background region's mean is subtracted, or the noise of the loaded pixels
gives the error. On a moment-0 map, *HI mass* follows at the distance set in
*Tools ▸ Regions ▸ HI Mass Distance…*. With Stokes layers loaded, per-Stokes
values follow (integrated Jy, mean PA in degrees, mean P/I in percent).

The regions are also listed in *Session Data*, under a *Regions* node, where
each can be selected, hidden, renamed, deleted or exported
([Regions in Session Data](region-pv-noise.md#regions-in-session-data)).

Selecting, moving and reshaping regions, typing positions in a sky frame, the
file formats and linked 2-D views are described in
[Regions, PV diagrams and noise](region-pv-noise.md).

### Tools ▸ Catalogue

Needs a celestial WCS.

- *Load Catalogue Overlay…*: draws the sources of a delimited text table
  (`.csv`, `.tsv`, `.txt`) with `RA` and `DEC` columns in degrees. A `.reg` or
  `.crtf` file chosen here is read as regions.
- *Cross-match with Catalogue…*: queries *Catalogue* (`simbad`, `2MASS`,
  `NVSS` or `FIRST`) around the image and overlays the sources that fall on
  valid pixels. *Search radius* is in arcseconds; the search always covers at
  least the image half-diagonal. At most 500 sources are kept, spread evenly
  over the field, and the result says when it was truncated. The backend must
  have network access. Catalogue coordinates are equatorial: switch *WCS ▸
  FK5* to compare them with the table.
- *Show Catalogue Overlay*, *Show Catalogue Labels*: show or hide the markers
  and the names without unloading.
- *Clear Catalogue Overlay*: removes the catalogue, its markers and its table.
- *Overlay VLKB Compact Sources*, *Overlay VLKB Filaments*, *Overlay VLKB
  Bubbles*: query the VLKB over the image footprint, or over a rectangle you
  drag, and overlay the result.
- *VLKB Catalogue Query…*: preset or hand-written ADQL queries on the VLKB
  catalogues over a Galactic box prefilled with the footprint.
- *Export Catalogue…*: writes the overlay to CSV, with the sky frame named in
  the header.

The sources are listed in a table under the image. Click a row to highlight
the source, double-click to zoom to it, right-click for *Fit SED of this
source…* and *Zoom to this source*. The table, the file formats and the VLKB
catalogues are described in [Catalogues, HiPS, and SAMP](catalogues-hips.md);
fitting in [SED fitting](sed-fitting.md).

### Tools ▸ Contours

- *Show Contours*: contours of the image itself. *Level* (number of lines),
  *Lower* and *Upper* (value range) are in *Inspector ▸ Parameters*.
- *Load Contour from FITS…*: contours from another FITS file (for example a
  radio moment 0 over an optical image), with levels you enter. Several can be
  stacked.
- *Clear All Contours*: removes every contour layer.

From a cube viewer, *Tools ▸ Overlay Slice on an Open Image* sends the current
slice here as a contour overlay.

### Tools ▸ Measure

- *Profile* (⌘P / Ctrl+P, the *Probe* mode): click or hover to read the pixel
  value in `BUNIT` and its profile.
- *Ruler (Distance)*: click two points. The distance is shown in pixels and,
  with a WCS, in arcseconds, arcminutes or degrees (great-circle distance).
- *Angle (3 Points)*: click A, B, C; shows the angle at B in degrees.
- *Clear Measurement*: removes the measurement.

### Tools ▸ Statistics

- *Pixel Histogram*: 256-bin histogram of the base layer, with two cursors
  (red low, green high) that set the display range as you drag them.
  `Clip to 0.5–99.5 %` bins over a robust range (on by default); *Log counts* uses a
  logarithmic count axis.

### Tools ▸ Structure

- *Angular Power Spectrum*: P(ℓ) of the map with its fitted slope.
- *Skeleton / Voids*: traces filaments or voids and draws them on the map.

Settings are in *Inspector ▸ Parameters*; each run is a product. See
[Cosmology tools](cosmology.md).

### Tools ▸ Annotations

- *Add Text Annotation…*: type the text, then click to place it.
- *Add Arrow Annotation…*: type an optional label, click the arrow tip, then
  the label position. Right-click cancels.
- *Clear Annotations*: removes them all.
- *Save Annotations…*, *Load Annotations…*: JSON file (default name
  `<fits-name>.annotations.json`).

### Tools ▸ Polarisation

These work on 2-D Stokes images loaded as layers, pixel by pixel in the
client. For full-Stokes cubes use the cube viewer
([Polarimetry](polarimetry.md)).

- *Load Stokes Q/U/V Companions…*: finds the Q, U and V files next to the
  Stokes I file (on the backend's filesystem) by their names, reports what it
  found and what is missing, and loads them as layers. You can locate missing
  ones by hand.
- *Compute Pol. Intensity (P)*: P = √(Q² + U²).
- *Compute Pol. Angle (PA)*: PA = ½ atan2(U, Q), in degrees from −90 to +90,
  north through east.
- *Compute Frac. Pol. (P/I)*: P / I, clamped to 0 to 1.
- *Compute Debiased P*: √(max(0, Q² + U² − σ²)), with σ the mean MAD-based
  noise of Q and U over the whole field; the layer name shows σ. For rigorous
  work use a per-pixel noise map.
- *Compute Spectral Index…*: α = log(S_B/S_A) / log(ν_B/ν_A) from two layers
  and their frequencies in GHz. Both layers must be on the same pixel grid;
  pixels with a flux ≤ 0 are skipped. For maps on different grids use
  *Combine ▸ Pixel Math / Spectral Index…* in the Data Hub, which reprojects
  the second map (beams are not matched: smooth both to a common resolution
  first).
- *Compute Faraday RM…*: fits PA = PA₀ + RM·λ² to three or more (Q, U, ν GHz)
  layer triplets; RM in rad m⁻². PA is not unwrapped, so the result is only
  right while |RM·Δλ²| < π/2 between consecutive frequencies; for larger RM
  use *Rotation Measure Synthesis…* in the cube viewer.
- *Show Polarization Vectors*: line segments along PA with length ∝ P. In
  *Inspector ▸ Parameters*: *Grid step (px)* (default 12), *SNR threshold*
  (P above N × the MAD-based σ of P, default 3.0) and *Length scale* (10 % to
  300 %). The orientation assumes north up and RA increasing to the left
  (`CDELT1` < 0); check it on a rotated WCS.

Each computed map is a new layer, usable with contours, regions and export.
Multi-channel Stokes cubes must first be reduced to 2-D maps in the cube
viewer (a single channel, or a moment 0 over a channel range); the procedure
is on [Polarimetry](polarimetry.md).

### Tools ▸ Products

- *Publication Figure…*: renders the map as a PNG on the backend, with a
  stretch (linear, log, sqrt, asinh, power), colour map, optional title, sky
  grid and a colour bar labelled with `BUNIT`. Saved in Workspace Exports.
- *Image Quality / Artifacts*: blanked fraction, dynamic range, noise, and
  striping and negative-bowl indicators. A striping value of 1 000 000 is the
  ceiling, for perfectly uniform bands.

Both use only the first plane of data with more than two axes.

### Tools ▸ Export

- *Smooth / Regrid…*: Gaussian smoothing (*FWHM* in pixels, shown in
  arcseconds) and/or *Regrid (shrink)* by an integer *Factor*, computed on the
  backend and added as a layer. Blank pixels stay blank; the beam in the
  header grows with the kernel (in quadrature), and a Jy/beam map is rescaled
  so its integrated flux is unchanged. *Combine* chooses mean (surface
  brightness) or sum (per-pixel units such as Jy/pixel) for the regrid;
  *Automatic (from BUNIT)* decides from the header. SIP distortion terms are
  rescaled with the pixel size.
- *Export to Workspace as FITS…*: copies the dataset's FITS file into
  Workspace Exports, where the Data Hub's *Workspace Exports* panel can
  download, reopen or delete it.

## Dynamic spectrum mode

The viewer switches to this mode for a FITS image with one time-like axis
(`TIME`, `MJD`, `UTC`) and one frequency-like axis (`FREQ`, `FREQUENCY`,
`WAVE`, `AWAV`), for HDF5 beamformed files (`.h5`, `.hdf5`, `.hdf`,
`.bfdata`, including LOFAR BFData) and for PSRFITS files. HDF5 and PSRFITS
inputs are converted on the backend to a FITS file next to the source,
`<source>.dynspec.fits`. Sampling values missing from an HDF5 file default to
1.0; correct them in that file's header if needed.

In this mode the axes read *Time* and *Frequency* with their units, the
status bar shows a *Dynamic Spectrum* badge, and the beam, catalogue, frame
and polarisation tools are off. Colour maps, histogram, regions, measurements,
annotations and blink still work (region statistics without the beam-aware
section).

The *Time-Series Tools* card runs on the backend:

- *Dedispersion (DM, pc cm⁻³)* and *Compute Dedispersion*: shifts each
  channel by Δt = 4148.808 s · DM · (1/ν² − 1/ν_ref²), ν in MHz, with ν_ref the
  top of the band. Writes `<source>_DM<value>.fits` to Workspace Exports.
- *Pulse profile (period, s)* and *Compute Pulse Profile*: folds the
  frequency-averaged time series at the period into 64 phase bins (after
  dedispersion, if a DM is set) and plots it with σ_MAD and peak SNR. A peak
  SNR above 5 is the usual detection threshold.
- *RFI mask (σ-clip, σ-MAD multiple)* and *Compute RFI Mask*: flags pixels
  that deviate from their channel's median by more than N × 1.4826 × MAD
  (default 5). Writes a uint8 mask (0 good, 1 flagged) to Workspace Exports
  and reports the flagged fraction.

Details: [Image viewer: methods and implementation](../../technical/methods/image-viewer.md)
(PSRFITS and HDF5 reading, statistics on reduced views, the polarisation
formulas), [Dynamic spectra API](../../technical/backend-api/dynspec.md),
[Brick pyramid: images](../../technical/design/brick-pyramid.md#images).
