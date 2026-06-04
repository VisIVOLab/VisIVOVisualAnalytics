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

## Preview → full upgrade

Like the cube viewer, the image viewer first shows a downsampled **preview**
(`/v1/image/preview` with a max-longest-side cap) so the window is
interactive immediately. The full resolution (`/v1/image/full`) loads in
the background; the LUT and WCS overlay continue to work during the
upgrade.

## Adding layers

You can overlay or compare multiple images in the same window:

- **Add New Layer…** — pick another FITS image from the *remote file
  browser*. Backend-side check (`ImageLayerImportService`) validates the
  layer's WCS against the base image and warns if the pixel grids don't
  overlap.
- Use the layer panel to **reorder**, **toggle visibility**, set a
  **per-layer colour map**, **opacity** and **z-order**.
- Manual local files: drop a `.fits` from the OS file browser, or pick it
  via *File → Add New FITS File…*

All layer sources go through the same `loadImageLayer()` / `AstroUtils` /
`libwcs` pipeline, so alignment between layers is handled the same way
regardless of whether they come from VLKB, the remote backend, or local
disk.

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

- **Color map** combo (Inferno, Viridis, Magma, Plasma, Cividis, …) with
  gradient preview icons.
- **Scale** — a **Linear | Log** segmented toggle (same pill-style
  control used throughout the cube viewer). Log scale compresses the
  dynamic range and is useful for images spanning several orders of
  magnitude (e.g. Hα narrowband, X-ray mosaics).
- **Layer opacity** slider for blending when multiple layers are stacked.

For more precise control open the *2-D LUT editor* (`Advanced…` button) —
a non-modal QCustomPlot editor where you can drag the transfer-function
control points.

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
  *Tools → Send Slice to Image Viewer…* in the cube window. The
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

## Pixel histogram

*Tools → Pixel Histogram…* (or the **Histogram** card in the sidebar)
opens a floating QCustomPlot window showing the pixel-value distribution
of the master layer as a 256-bin bar chart.

Two draggable vertical cursors mark the current LUT clip range
(red = low, green = high). Dragging either cursor updates the LUT
range live, so you can interactively clip the display without opening
the advanced LUT editor.

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
3. Files found are loaded as additional layers, tagged with their
   Stokes role. Missing roles trigger a file dialog so you can locate
   them manually.

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
*Tools → Load Catalogue Overlay*. The sources are projected through the
image WCS, drawn as glyphs, and listed in a sortable side table. Click a
row to centre the view on the source; click on a glyph to highlight the
matching table row.

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
