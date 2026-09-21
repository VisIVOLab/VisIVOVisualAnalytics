# Image viewer

The image viewer (`vtkWindowImage`) handles 2-D FITS images: single-plane
maps, mosaics, moment maps exported from a cube, and any other 2-D product
the backend exposes. It opens automatically when you open a FITS file the
backend classifies as `image`.

It also handles **dynamic-spectrum / beamformed** datasets — FITS files
with `CTYPE` = `TIME` + `FREQ` (or HDF5 LOFAR BFData files). The viewer
auto-detects them, relabels the axes as *Time* / *Frequency*, hides the
celestial-WCS tools (catalogue overlay, beam, Stokes polarimetry, frame
switch), and surfaces a placeholder **Time-Series Tools** card in the
sidebar for future specialist tools (pulse profile, dedispersion, RFI
flagging). See *Dynamic spectrum / beamformed mode* below.

## Layout

Single 2-D dock with the image and a side panel for:

- **Layers** — stacked images and overlays (multiple FITS files can be
  composited).
- **Display** — color map, scaling, contrast / brightness, WCS overlay.
- **Tools** — region statistics, profile / probe, WCS frame switch.
- **Info / Stats** — pixel-value summary, BUNIT, image extent.

Tools that have settings do not each open their own panel. Picking one mounts
its controls in a single contextual **Parameters** tab in the Inspector, and
switches to it — so "where are this tool's options" has one answer, and the
answer is visible the moment you start the tool rather than behind a dock you
have to find. The tab also carries the provenance of what the tool measured.

## Preview → full upgrade

Like the cube viewer, the image viewer first shows a downsampled **preview**
(`/v1/image/preview` with a max-longest-side cap) so the window is
interactive immediately. The full resolution (`/v1/image/full`) loads in
the background; the LUT and WCS overlay continue to work during the
upgrade.

For very large mosaics the full upgrade is assembled from tiles
(`/v1/image/tile`) at a **decimated level of detail** capped at ~64 Mpx,
rather than uploading every pixel. The decimated image is visually identical
(display is downsampled for rendering anyway), and all overlays and tools —
probe, profile, region statistics, ruler/angle, catalogue markers — report
**full-resolution pixel and WCS coordinates**, so measurements are unaffected
by the LOD. The badge next to the file path shows `full-res` once the upgrade
has loaded.

### Viewport-driven level of detail

On a multi-level pyramid image the viewer then **streams the visible region
at the zoom-appropriate resolution**: when a pan or zoom settles, it fetches
only the tiles covering the current view at the pyramid level where one source
pixel ≈ one screen pixel, and swaps that sub-image in while keeping your camera
in place. So zooming into a source reveals its **full-resolution** detail
without ever materialising the whole gigapixel mosaic, and panning loads new
regions on demand; memory stays bounded by the viewport rather than the image.
The streamed sub-image carries a non-zero origin, but its world coordinates are
still full-resolution source pixels, so every readout and overlay stays correct.
Zooming back out returns to a coarse whole-image level. This is transparent —
there are no controls; it just tracks your view.

## Adding layers

You can overlay or compare multiple images in the same window:

- **Add Image Layer…** (*File* menu, ⌘L, or the **+ Add…** beside the
  *Layers* list) — pick another FITS image from the *remote file browser*.
- Use the layer panel to **reorder**, **toggle visibility**, set a
  **per-layer colour map**, **opacity** and **z-order**.
- You can also drop a `.fits` from the OS file browser onto the window.

### A layer has to cover the same sky

Before anything is downloaded, the two headers are compared: a file that
covers none of the sky the current image covers is not added. It is not a
broken file — the WCS alignment would place it correctly, off the edge of the
image, so the load would "succeed" and nothing would appear.

Instead the viewer says so and offers:

- **Open in New Window** — the sensible answer for an image of somewhere else;
- **Add Anyway** — the sky bounds behind the refusal come from `wcsrange()`,
  which is a bounding box, and a rotated field straddling RA = 0 can measure
  narrower than it really is. This is how you overrule the check;
- **Cancel**.

An image whose header carries no celestial solution (pixel coordinates only)
is refused outright: there is no position to line it up with.

All layer sources go through the same `loadImageLayer()` / `AstroUtils` /
`libwcs` pipeline, so alignment between layers is handled the same way
regardless of whether they come from VLKB, the remote backend, or local
disk. Which path a file takes is decided by **where the backend is**, not
by whether the path happens to exist on your own disk: against a remote
backend the layer is opened there and its WCS built from the header the
backend returns, so a path that also exists locally cannot silently load
a different file.

## Smoothing and regridding

*Tools → Smooth / Regrid…* (the legacy's *Filter FITS*) applies a Gaussian
kernel and/or an integer shrink on the backend and adds the result as a layer.
The dialog says what each choice means before you commit: the kernel FWHM in
arcseconds, and the pixel size the regrid would produce.

Three things it does that the legacy did not:

- **blank pixels stay blank** and do not eat their neighbourhood — the
  convolution is normalised by the smoothed coverage;
- **the beam grows with the kernel** (`BMAJ`/`BMIN` summed in quadrature), so
  the output still describes its own resolution — and for a map in Jy/beam the
  pixels are scaled with it, so the integrated flux is the same before and
  after. (Smoothing preserves the sum of the pixels; the flux is that sum over
  the beam area, so a wider beam with unchanged numbers means less flux.) The
  dialog reports the factor it applied;
- **a per-pixel unit is summed, not averaged.** Block-averaging a `Jy/pixel` map
  loses flux by the square of the factor; `BUNIT` decides, and the result says
  which rule was used.

The same entry exists in the cube viewer, where every plane is filtered — a
spatial operation, so the spectral axis is left alone.

## Editing the header

Click the **IMAGE** tag in the toolbar to see the FITS header, then **Edit…**.
Any keyword can be changed, added or removed; the result is written as a **new
file** in the Workspace and offered for opening, and the file on disk is never
modified.

Structural keywords (`SIMPLE`, `BITPIX`, `NAXISn`) and the data-scaling ones
(`BSCALE`, `BZERO`, `BLANK`) are not editable: they describe the bytes rather
than the science, and editing them makes the header disagree with its own data.

When you save, the backend builds the WCS from the edited header and tells you
what it amounts to — *Celestial WCS (RA---TAN, DEC--TAN)*, or that there still
is none. The legacy's header dialog reported "File has been saved!" either way,
so a wrong guess was only discovered by reopening the copy.

## WCS overlay and frame

- **Show WCS Axes** in the *View* menu paints ticks along the image axes
  according to its WCS metadata.
- **Coordinate format** — a **Sexagesimal | Decimal** segmented toggle in
  the sidebar's *Tools → WCS Display* card.
- **Coordinate frame** — a **Galactic | FK5 | Ecliptic** segmented toggle
  in the same card. Conversions go through `wcscon()` from libwcs. The
  current frame label is shown in the bottom status bar.

### Beam indicator

When the FITS header contains `BMAJ` and `BMIN` (and optionally `BPA`),
a filled semi-transparent white ellipse is drawn in the bottom-left
corner of the image. The ellipse is sized in pixels using the angular
beam axes divided by `|CDELT1|`, and rotated by `BPA`. If the beam
keywords are absent the indicator is hidden automatically.

If the WCS metadata is partial or invalid the backend sanitises it and the
**WCS** badge in the status bar turns yellow with a tooltip listing what
was changed.

## Color map & contrast

The *Layer Settings* sidebar exposes:

- **Color map** — the combo (Inferno, Viridis, Magma, Plasma, Cividis, …)
  with gradient preview icons, and an inline **Edit** beside it, as in the
  cube viewer.
- **Layer opacity** slider for blending when multiple layers are stacked.

**Edit** opens the *2-D LUT editor* — a non-modal QCustomPlot editor where you
drag the transfer-function control points. The scale (Linear, Log, Sqrt,
Square, Power γ) lives there too: one place with the full set, rather than an
inline Linear | Log toggle offering two of five.

## Regions and probes

The image viewer shares the same region / probe machinery as the cube
viewer's 2-D dock:

- **Probe** — click a pixel to see its value (DN / BUNIT). Hover updates a
  read-out in the bottom-right.
- **Box / Circle / Polygon / Annulus regions** — compute statistics
  (mean, median, MAD-based sigma, min, max, sum) over the region.
- Right-click on a region to copy the value summary or remove it.

The detailed semantics are documented in
[Regions, PV, noise](region-pv-noise).

## Contour overlay

Iso-contour lines can be drawn on top of the image from two sources:

- **Self contours** — computed from the image's own pixel values.
  Toggle **Show Contours** in the *Tools* sidebar (or *Tools* menu),
  then adjust **Level** (number of contour lines), **Lower** and
  **Upper** (value range). The pipeline uses `vtkFlyingEdges2D`, the
  same filter as the cube viewer's slice contours.
- **External FITS contours** — *Tools → Load Contour from FITS…*
  lets you overlay contours from a different FITS file (e.g. a radio
  moment-0 map on an optical image). Contour levels are entered
  manually. Multiple external layers can be stacked.
- **Clear All Contours** removes every contour layer.

- **Cube → Image direct** — if a cube viewer is open, use
  *Tools → Overlay Slice on an Open Image…* in the cube window. The
  current 2-D slice is sent to the first open image viewer as a
  contour overlay without requiring a FITS export round-trip.

```{tip}
The cube viewer's *Tools → Export Moment Map as FITS…* writes the
moment into the Workspace Exports directory. You can then load that
FITS here as an external contour overlay — the most common radio +
optical comparison workflow.
```

## Measurement tools

Two interactive measurement modes are available in the *Tools* sidebar
**Measurement** card (or *Tools* menu):

- **Ruler (Distance)** — click two points on the image. A dashed
  orange line is drawn between them and the distance is displayed in
  pixels and, when WCS metadata is available, in arcseconds (or
  arcminutes / degrees for large separations). The angular distance
  uses the Haversine formula on the WCS sky coordinates.
- **Angle (3 Points)** — click three points A, B, C. Lines A–B and
  B–C are drawn and the angle at vertex B is displayed in degrees.
- **Clear Measurement** removes the current measurement overlay.

Only one measurement mode can be active at a time (ExclusiveOptional
group). Activating a measurement mode deactivates any active probe or
region tool.

## Products from this map

*Inspector ▸ Analysis ▸ Products* (or the *Tools* menu) has two operations that
take the single 2-D map this window shows:

- **Publication Figure…** renders the map as a publication-ready PNG through
  matplotlib on the backend: choose the stretch (linear / log / sqrt / asinh /
  power), the colormap, an optional title, and whether to draw the WCS sky grid
  and a colorbar labelled with BUNIT. The file lands in Workspace Exports.
- **Image Quality / Artifacts…** reports the diagnostics you want before
  trusting a map: blanked fraction, dynamic range, noise, and indicators for
  striping and negative-bowl artefacts.

```{note}
Both were previously in the Data Hub's *Science* menu, where they ran against
whichever dataset had been opened last rather than the one you were looking at.
They now act on this window's dataset. Both reduce anything with more than two
axes to its first plane, so on a cube they describe one channel; to render or
measure a cube's collapsed emission, compute a moment map first and open that.
```

## Pixel histogram

*Tools → Pixel Histogram…* (or the **Histogram** card in the sidebar)
opens a window showing the pixel-value distribution of the master layer as a
256-bin bar chart.

Two draggable vertical cursors mark the current LUT clip range
(red = low, green = high). Dragging either cursor updates the LUT
range live, so you can interactively clip the display without opening
the advanced LUT editor.

Two checkboxes decide whether the histogram is readable at all:

- **Clip to 0.5–99.5 %** — bins over a robust percentile range instead of the
  full data range. Astronomical images are mostly sky with a few bright
  outliers; over the full range every source lands in one bin at the far right
  and the sky in one at the far left. On by default, unless a LUT cursor would
  fall outside the clipped range — the cursors have to stay reachable.
- **Log counts** — a logarithmic count axis. The sky bin is orders of magnitude
  taller than everything else, so on a linear axis the astronomically
  interesting tail is a flat line along the bottom.

## Annotations

Text and arrow annotations can be placed interactively on the image:

- **Add Text…** — a dialog asks for the text, then the cursor changes
  to a crosshair. Click anywhere on the image to place the label.
- **Add Arrow…** — a dialog asks for an optional label, then the
  cursor changes to a crosshair. **First click** sets the arrow tip;
  a live preview follows the mouse showing the shaft and arrowhead.
  **Second click** sets the label position.
- Right-click cancels placement without adding the annotation.
- **Clear All** removes every annotation.
- **Save** — opens a native Save dialog (default name
  `<fits-name>.annotations.json`) so you can choose any location.
- **Load** — opens a native Open dialog to pick an annotation JSON
  file.

Annotation text is rendered as bold yellow `vtkTextActor` labels;
arrows have a fixed-size arrowhead (8 px, capped at 30 % of the shaft
for very short arrows) so the indicator stays readable at any zoom.

## Blink / Compare

When two or more layers are loaded, the **Blink Layers** toggle
(*View* menu or sidebar **Blink / Compare** card) rapidly alternates
the visibility of layers 0 and 1 at a configurable speed
(50 – 1000 ms, adjustable via the sidebar slider).

This is a standard technique for detecting transient sources, proper
motion, or artefact differences between two epochs or bands.

## Stokes / Radio polarimetry analysis

For continuum radio data with full polarimetry (Stokes I, Q, U, V), the
image viewer has a dedicated **Stokes Analysis** workflow card in the
*Tools* sidebar (also available under *Tools* menu):

### Loading a Stokes set

1. Open your Stokes I file as usual — it becomes the master layer and
   is tagged automatically as Stokes I.
2. Click **Load Stokes Q/U/V Companions…**. The viewer searches the
   same directory of the I file for matching filenames using common
   patterns:
   - `*StokesI*` → replaces with `Q`, `U`, `V`
   - `*_I.fits` / `*_I_PB.fits` → replaces the role tag
   - Also tries `.fits.gz` variants for ASKAP / MeerKAT compressed
     releases
3. The search is reported **once**, as a summary of what was found and
   what was not, rather than as one file dialog per missing role. From
   there you either accept the set or locate the missing companions
   yourself; cancelling means cancelling, not being asked again for the
   next role.
4. Files found are loaded as additional layers, tagged with their Stokes
   role, one after another.

```{note}
The search runs wherever the data actually is. With a **remote backend**
the companions are looked for — and browsed for, if you have to locate
them by hand — on the backend's filesystem, not on your laptop: the
Stokes I file you opened lives there too, so its siblings almost
certainly do.
```

### Derived maps

Once Q and U are loaded, three derived quantities can be computed
client-side from the in-memory `vtkImageData` (no backend trip):

| Action | Formula | LUT |
|---|---|---|
| **Compute Pol. Intensity (P)** | `P = √(Q² + U²)` | Inferno |
| **Compute Pol. Angle (PA)** | `PA = ½ · atan2(U, Q)` (degrees, −90…+90) | Spectrum (good for cyclic data) |
| **Compute Frac. Pol. (P/I)** | `P / I` (clamped to [0, 1]) | Viridis |

Each computation appends a new layer to the layer list. From there you
can:
- Apply contours, change LUT, export to Workspace as FITS
- Use the region tools — region stats automatically include per-layer
  values for all loaded Stokes layers (see below)
- Use them as base for polarization vector overlay parameters

### Polarization vector overlay

Toggle **Show Polarization Vectors** (or the menu action of the same
name) to draw short line segments at every Nth pixel, oriented along
PA and with length proportional to P. Parameters in the sidebar:

- **Vector grid step (px)** — sampling step on the image grid
  (default 12). Smaller = denser overlay; larger = clearer view.
- **SNR threshold (σ_MAD multiples)** — vectors are drawn only where
  P exceeds N × robust σ of the P distribution (default 3.0). Higher
  values filter out noise-only pixels.
- **Vector length scale** — global scale factor (10 % to 300 %) so
  you can adapt the visual density of the field to the colour image
  underneath.

Convention used: PA is measured **from north through east**. The
overlay assumes the standard FITS pixel orientation (CDELT1 < 0, north
up), so `dx = −len·sin(PA)`, `dy = +len·cos(PA)`. For rotated WCS the
vector orientation may need correction — check against your reduction
pipeline.

### Region statistics: radio-aware fields

When the FITS header has BMAJ/BMIN and at least one Stokes layer is
loaded, a region analysis dialog gains two extra sections:

- **Radio (beam-aware)** — beam major × minor in arcsec, beam area in
  pixels (Ω_beam = π · BMAJ · BMIN / (4 · ln 2) in pixel units), the
  **integrated flux in Jy** (`sum / Ω_beam`), and the peak SNR in σ_MAD
  units.
- **Stokes (per-layer)** — for each loaded Stokes role (I, Q, U, V,
  P, PA, P/I) the region is re-analysed and the relevant scalar is
  shown: integrated Jy for flux-density maps, mean degrees for PA,
  mean percentage for P/I.

The standard **Statistics** section now also reports σ_MAD —
`1.4826 × MAD(values − median)` — which is much more reliable than
std-dev when the region contains bright sources.

### NaN handling

The master layer's LUT is configured with `NanColor = (0, 0, 0, 0)` so
mosaic edges and blanked pixels render **transparent** instead of
white. This matches radio-imaging convention and makes contour
overlays / multi-layer compositions readable.

### Advanced derived products

Three more advanced quantities are supported (all client-side, all
appearing as ordinary layers):

#### Debiased polarization intensity

**Compute Debiased P** estimates the per-channel noise σ as the
average of MAD-σ of the Q and U layers, then computes
`P_debiased = √(max(0, Q² + U² − σ²))`. The result has any pixel where
`Q² + U² < σ²` clamped to zero, which suppresses the positive bias of
naive `√(Q² + U²)` near the noise floor. The new layer is labelled
`P_deb (σ=…)` so the σ used is visible in the layer list.

For rigorous work the noise map per pixel should be used; the
MAD-σ estimate is a uniform field-average and is meant as a quick
approximation.

#### Spectral index (α)

**Compute Spectral Index…** opens a dialog where you pick two layers
and enter their reference frequencies in GHz. The per-pixel formula is

```
α = log(S_B / S_A) / log(ν_B / ν_A)
```

Pixels where either flux is ≤ 0 are skipped (logarithm undefined).
The result is shown with the Spectrum LUT clamped to [−2.5, +1.5] —
the typical range covering synchrotron-dominant (α ≈ −0.7) and
thermal/free-free (α ≈ +2) regimes.

**Requirement:** both layers must share the same pixel grid (same
extent). Typical workflow: open two FITS mosaics at different
frequencies, or two Stokes I moment-0 maps extracted from the same
sub-cube spectral range.

#### Faraday rotation measure (RM)

**Compute Faraday RM…** opens a table dialog where you add ≥ 3
`(Stokes Q layer, Stokes U layer, ν GHz)` triplets. For each pixel
the tool:

1. Computes `PA_i = ½·atan2(U_i, Q_i)` at every frequency
2. Computes `λ_i² = (c/ν_i)²`
3. Linearly fits `PA = PA₀ + RM · λ²` → the slope is RM (rad m⁻²)

The result is added as a layer with the Spectrum LUT clamped to
[−500, +500] rad m⁻² for visibility.

```{warning}
**No PA unwrapping** is performed. The naive linear fit is only
correct in the low-RM regime where `|RM · Δλ²| < π/2` between
consecutive frequencies. For aggressive RM use the upcoming
backend-side RM-synthesis tool (not yet available).
```

The user typically obtains the 3+ Q/U pairs by extracting moment-0
maps from sub-band channel ranges of a single Q/U cube in the cube
viewer.

### Workflow note for the MeerKAT Milky-Way Bulge survey

For the MeerKAT 1.3 GHz Milky Way Bulge survey (Cotton et al. 2025)
the Q/U files are **multi-channel subband cubes**, not single images.
To use the 2-D Stokes workflow you must first extract 2-D maps from
the cubes. Two shortcuts are available depending on what you need:

#### Single-frequency 2-D maps — for P, PA, P/I, P_deb

Use *Tools → Export Current Channel as 2-D FITS…* in the cube viewer
on the **same channel** for I, Q, U (and V if you want). The export
produces a true 2-D FITS (NAXIS=2) — no degenerate spectral axis —
ready to be loaded as a layer here. Recommended path for polarimetry
at one specific frequency.

#### Broadband 2-D maps — for the highest-SNR P/PA/P/I

Use *Tools → Export Moment Map as FITS…* with a moment-0 over **all
channels** for each Stokes cube. The result averages out frequency
detail but maximises SNR — best for visualisation overlays and
contour comparisons.

#### Multi-frequency 2-D maps — for spectral index and Faraday RM

- **Spectral index (α)**: extract 2 moment-0 maps from two distinct
  channel ranges of the Stokes I cube (e.g. the low and high halves
  of the subband sequence). The header ``SPECVAL`` value of each
  exported map tells you the spectral coordinate to enter in the
  spectral-index dialog.
- **Faraday RM**: extract 3+ moment-0 maps from three or more channel
  ranges, in both the Q and U cubes (so you get N pairs at N
  frequencies). Enter the frequency triplets in the RM dialog.

Once the 2-D maps are in the workspace, open them here, click
**Load Stokes Q/U/V Companions…** for the polarimetry products, or
use the spectral-index / Faraday-RM dialogs directly for the
multi-frequency analyses.

## Dynamic spectrum / beamformed mode

### Supported inputs

| Format | Detection rule |
|---|---|
| **FITS 2-D dynamic spectrum** | `CTYPE1` / `CTYPE2` contain at least one **time-like** axis (`TIME`, `MJD`, `UTC`) and one **frequency-like** axis (`FREQ`, `FREQUENCY`, `WAVE`, `AWAV`). |
| **HDF5 / LOFAR BFData** | File extension `.h5`, `.hdf5`, `.hdf` or `.bfdata`. The backend walks the `SUB_ARRAY_POINTING_xxx / BEAM_yyy / STOKES_z` hierarchy and picks the first 2-D plane. Generic fall-back: pick the largest 2-D float dataset in the file. |

For HDF5 inputs the backend transparently converts the chosen 2-D plane
to a **sidecar FITS file** named `<source>.dynspec.fits` next to the
source, then the standard FITS pipeline takes over. The conversion is
cached: subsequent opens of the same HDF5 file re-use the sidecar as
long as it is newer than the source.

Sampling metadata is extracted on a best-effort basis from common
attribute names (`TSAMP`, `DT_S`, `CADENCE_S` for time; `FREQ_START_HZ`,
`FCH1`, `FREQ_LOW_HZ` for frequency; `DELTA_FREQ_HZ`, `FOFF`,
`CHANNEL_BW_HZ` for the channel width). Missing values default to
**1.0** so the file still opens — you can edit the sidecar header
manually afterwards if needed.

### What changes when dynamic-spectrum mode is on

- **Axis labels** are forced to *Time* / *Frequency* with the FITS
  `CUNIT` appended (e.g. *Time (s)* / *Frequency (Hz)*).
- **Status-bar sanity panel** shows a stable *Dynamic Spectrum* badge
  instead of running the celestial-WCS checks (which would always
  complain about missing RA / Dec).
- **Beam indicator** is hidden (no synthesised beam concept).
- **Catalogue overlay**, **WCS Display** (Galactic / FK5 / Ecliptic
  frame & sexagesimal / decimal format), and **Stokes Analysis** cards
  are hidden in the sidebar; the corresponding menu actions are also
  disabled.
- A **Time-Series Tools** card appears in the sidebar with a
  placeholder describing what tools will arrive in a follow-up
  (dedispersion, pulse profile, RFI flagging).

### What still works

- LUT, log scale, opacity, contrast — all colour-mapping tools work
  exactly as for image files.
- **Pixel Histogram** — useful for setting clip ranges or identifying
  bands of strong RFI.
- **Region statistics** — box / circle / polygon / annulus stats on a
  region of the waterfall (mean, median, σ_MAD). The beam-aware
  integrated-flux section is suppressed because no beam is available.
- **Measurement tools** — ruler / angle on the time × frequency plane.
- **Annotations** — text and arrow overlays for labelling features.
- **Blink / Compare** — alternate between layers (e.g. RFI-flagged vs
  raw) when multiple are loaded.

### Time-Series Tools card

Three analysis tools live in the **Time-Series Tools** sidebar card
(only visible in dynspec mode). All three run on the backend and
return either a workspace file or in-memory data.

#### Incoherent dedispersion

Set **Dedispersion (DM, pc cm⁻³)** to the desired dispersion measure
and click **Compute Dedispersion**. The backend computes
`Δt(ν) = K · DM · (1/ν² − 1/ν_ref²)` per channel
(`K = 4148.808 MHz²·pc⁻¹·cm³·s`) and shifts each channel by the
nearest sample. The output is a 2-D FITS file in the Workspace Exports
named like `<source>_DM<value>.fits`. Open it as a new viewer or as a
layer to see the dispersion-corrected waterfall — pulses should appear
as **vertical** features in the time axis instead of the characteristic
diagonal sweep of the original data.

Reference frequency is the top of the band by default; the resulting
header carries `DM` and `DMREFMHZ` cards for provenance.

#### Pulse profile (phase folding)

Set **Pulse profile (period, s)** to the trial period in **seconds**
and click **Compute Pulse Profile**. If you already typed a DM in the
dedispersion field above, the same DM is applied in-memory before
folding (no extra file is written). The backend folds the
frequency-averaged time series into 64 phase bins and returns the
profile array.

A floating QCustomPlot window opens with:
- The folded profile (intensity vs phase, with a translucent band fill)
- A header line showing period, DM, σ_MAD, peak SNR
- Phase range 0…1 (one full pulse cycle)

A meaningful pulse stands out as a narrow peak well above the off-peak
noise level. **Peak SNR > 5** is the usual detection threshold.

#### RFI mask (σ-clipping)

Set **RFI mask (σ-clip, σ-MAD multiple)** to the threshold (default
**5**) and click **Compute RFI Mask**. The backend flags every pixel
whose deviation from its channel's robust median exceeds `N × 1.4826 ×
MAD`, ignoring NaN samples. The output is a uint8 FITS (`0=good`,
`1=flagged`) in the Workspace Exports, named like
`<source>_rfimask_channel_5.0sigma.fits`.

The status message reports the **flagged fraction** of pixels. Open
the mask as a layer over the source dynspec to see which frequency
bands / time intervals were caught.

You can chain operations:

1. Compute the RFI mask first.
2. Apply the mask (out of scope today — soon: paint masked pixels as
   transparent).
3. Run dedispersion on the cleaner data.
4. Fold for the pulse profile.

### Supported PSRFITS

The backend also recognises **PSRFITS** files (multi-HDU FITS with a
`SUBINT` binary-table extension). On open they are converted to a 2-D
sidecar dynspec (Stokes I; phase-averaged when the source is folded
PSRFITS) so the viewer treats them like any other dynamic spectrum.
Per-channel central frequencies come from the `DAT_FREQ` column when
available, otherwise from `OBSFREQ` / `OBSBW`.

## Saving and exporting

- **Export** in the top-right toolbar saves a PNG of the current view.
- **Tools → Export to Workspace as FITS…** copies the dataset's FITS
  file into the persistent Workspace Exports directory. From there you
  can download it to your computer, re-open it in VisIVO, or delete it
  — all via the **Workspace Exports** panel in the Data Hub.

## Catalogue overlay

Same machinery as the cube viewer: load a CSV / VOTable from
*Tools → Load Catalogue Overlay*, or query the VLKB directly with
*Tools → Overlay VLKB Compact Sources* / *Overlay VLKB Filaments*. The sources are projected
through the image WCS, drawn as glyphs, and listed in a table dock at the
bottom of the window.

Click a row to highlight the source, double-click to zoom to it, and
right-click for *Fit SED of this source…*. The table shows every column the
catalogue carries, groups a band-merged catalogue's per-band detections under
their parent source, and exports to CSV. See
[Catalogues · Overlay](catalogues-hips#catalogue-overlay-on-cubes--images) for
the full description and [SED fitting](sed-fitting) for what to do with a
source's fluxes once you have them.

### Cross-match with a catalogue

*Tools → Cross-match with Catalogue…* queries an external catalogue over the
footprint of the current image and overlays the matches — no file needed. It
requires a celestial WCS and a loaded dataset.

1. Pick a catalogue — **SIMBAD**, **2MASS**, **NVSS**, or **FIRST** — and a
   search radius in arcseconds (the effective radius is at least the image
   half-diagonal, so the whole footprint is always covered).
2. The backend resolves the image centre + footprint, queries the catalogue
   (via `astroquery`), and **crops the matches to the valid image pixels** —
   sources that project onto the blank/unobserved (NaN) border of a
   non-rectangular mosaic are discarded, so the overlay only marks real data.
3. The results are drawn and listed exactly like a file overlay (same
   projection and side table), so you can click through them. The side table
   includes **RA / Dec (deg)** alongside the projected image **X / Y**.

The query runs asynchronously, so the UI stays responsive; results are capped
at the backend's `max_sources` and flagged as *truncated* if there were more.
When a dense field exceeds the cap, the retained sources are a **uniform
spatial sub-sample** across the whole footprint (not just the ones nearest the
centre), so the overlay stays representative instead of collapsing into a
central clump. Catalogue column names are resolved case-insensitively, so the
feature stays robust across `astroquery` versions.

Overlay markers are sized relative to the image, so they stay visible on very
large (multi-thousand-pixel) mosaics. Catalogue coordinates are equatorial
(RA/Dec); if the status bar reads Galactic, switch **WCS → FK5** to compare
them directly against the table.

## Diagnostics & errors

Backend errors during load (`/v1/datasets/open`, `/v1/image/full`) and
WCS sanitisation warnings flow into the same *Diagnostics* panel that the
cube viewer uses — open it with **View → Diagnostics**.

## Comparing 2-D vs. 3-D workspaces

If you open a moment map that the cube viewer just generated, the image
viewer is the right tool to compare it against an external image, do region
photometry, or send it via SAMP. Going the other way around: to inspect a
cube channel-by-channel you need the cube viewer — the image viewer only
shows a single 2-D plane.
