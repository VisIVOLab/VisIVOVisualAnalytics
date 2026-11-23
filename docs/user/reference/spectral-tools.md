# Spectral analysis tools

Tools that work along the spectral axis of a cube in the
[cube viewer](cube-viewer.md). Two act on a spectrum already in a pane (line
identification, Gaussian fit), from *Inspector ▸ Analysis ▸ SPECTRUM*. Four
compute over the cube, from *Tools ▸ Spectral*: line-width map, baseline
subtraction, stacking and the LTE analysis of CO isotopologues. Their dialogs
do not block the window.

How to get a spectrum (probe, 3-D pick, region) is on the
[Cube viewer](cube-viewer.md) page; a walk-through is in
[First steps](../use-cases/01-first-steps.md).

## Line identification

*Load Lines…* in the SPECTRUM panel draws a line list on the plot as dashed
amber markers with labels. *Clear Lines* removes them; they are also removed
when the pane shows a product on a different spectral axis.

The *Spectral Lines* dialog:

- *Lines from:* *Common radio / mm lines (bundled)* (HI, OH, CH₃OH and H₂O
  masers, NH₃, HCN, HCO⁺, HNC, N₂H⁺, CS, the CO isotopologue ladders to 3-2,
  H₂CO, C I; works offline) or *File…*: a `value,label` list, or a catalogue
  export (for example Splatalogue CSV, whose frequency, species and
  quantum-number columns are recognised).
- *Values are in:* GHz, MHz, kHz, Hz, km/s or m/s. Preselected when the file
  declares a unit.
- *Rest frame → observed:* *Systemic velocity (km/s, optical)*, *Redshift z*
  or *Already in the observed frame*, with the *Value:*. Observed frequency is
  f_rest / (1 + z), with z = v/c.

The dialog shows the axis and the cube's rest frequency. On a velocity axis
the lines are placed with the header's `RESTFRQ` and the axis's convention
from `CTYPE3` (`VOPT`/`FELO` optical, `VRAD` radio, `VELO` relativistic).
Optical and radio velocities differ by about v²/c, 3.3 km/s at 1000 km/s.
Without `RESTFRQ` a frequency list cannot be placed on a velocity axis, and
when no line falls inside the plotted band the dialog says so.

There is no online catalogue query. The bundled list is rounded to the kHz;
for a published identification load a CDMS, JPL or Splatalogue export.

## Gaussian line fit

*Fit Gaussian* in the SPECTRUM panel fits one Gaussian plus a linear baseline
to the displayed spectrum on the backend and draws the model as an orange
dashed curve. The stats bar shows peak, centre and FWHM with 1σ errors, the
integral and the residual rms. *Clear Fit* removes it.

- The *Fit* menu beside the button chooses the channels: *Fit visible
  range* (the default) fits only the part of the spectrum on screen, so zoom
  on the line first; *Fit typed range…* asks for two axis values; *Fit whole
  spectrum* uses every channel. The summary ends with the range fitted, and
  the model is drawn over that range only.
- At least 6 finite channels are needed in the range.
- The fit tries several starting positions and widths and keeps the best
  resolved solution. A strong edge spike or a brighter noise peak inside the
  range can still capture it. The summary then starts with *⚠ CHECK THE FIT*,
  the label on the plot turns red, and the tooltip gives the reason: the
  backend's warning, or a negative peak on an emission spectrum (a dip in the
  baseline, not the line). Narrow the range and fit again.
- When channels are correlated (instrumental or display smoothing) the
  summary says by how much the errors were scaled (*errors ×1.4 for
  correlated channels*) or that they are uncorrected.
- The residual is not shown. A double-horned, self-absorbed or blended
  profile is not a Gaussian; use the line-width map's equivalent width for
  those.

## Line-width maps

*Tools ▸ Spectral ▸ Line-width Map…* computes a width per pixel:

- *Method*: *FWHM, Gaussian fit (slow, exact)* (2.3548 σ of a per-pixel
  Gaussian fit), *FWHM, moment-2 dispersion (fast)*, or *Equivalent width
  (fast, no fit)* (M0 / peak, a model-free width for any profile).
- *Start channel*, *End channel*: the channels with the line.
- *Mask* (*Enable threshold mask*) and *Threshold*: only pixels above the
  threshold. After *Estimate Noise...* the mask is on at 3σ of the estimate.
- *Rest frequency* (GHz): for a frequency axis, prefilled from `RESTFRQ`, so
  the widths come out in km/s.
- *Compute*: the map is added to Session Data and shown in a pane, with its
  unit and provenance.

Reading the map: FWHM much larger than M2 points to a non-Gaussian line; EW
much smaller than FWHM to a peaked profile, much larger to a flat or
double-peaked one. NaN pixels are failed fits (low signal, divergence).
Low-signal maps can be patchy; try a smoothed cube.

## Baseline subtraction

*Tools ▸ Spectral ▸ Baseline Subtraction…* fits a baseline to each pixel's
spectrum over line-free channels and subtracts it from the whole spectrum.

- *Fit*: *Polynomial*, or *Median* (a constant).
- *Polynomial order*: 0 to 10, default 1.
- *Line-free channels ▸ Ranges*: required; 1-based inclusive ranges such as
  `1-50, 200-300`. They must contain no emission.
- *Subtract Baseline*: runs it. The summary gives the *RMS before / after* in
  the line-free channels, the number of fit channels, the new dataset id and a
  warning when the fit deserves one.

The result is a new cube: a `BASE` row in Session Data. Double-click it to
open it in its own cube window (use *Link Views* to compare with the original).
The file is temporary; right-click the row for *Save a Permanent Copy
(Workspace Exports)*, *Open as a New Cube* or *Copy File Path*.

Limits:

- Each pixel is fitted on its own; flagged channels are skipped. A pixel with
  fewer than order + 2 usable channels comes out blank.
- Channels outside the line-free ranges are extrapolated, and a high order
  diverges there. Put line-free ranges at both ends of the band; the summary
  warns when the extrapolation is large.
- A 4-D input with a degenerate Stokes axis is written as a 3-D cube.

## Stack cubes to a spectrum

*Tools ▸ Spectral ▸ Stack Cubes to a Spectrum…* averages each selected cube
over all its pixels into one mean spectrum, then combines those spectra
channel by channel into one 1-D spectrum. It does not merge cubes.
(*Combine ▸ Mosaic (noise-weighted)…* in the Data Hub co-adds overlapping
observations, and gives a 2-D mosaic even from cubes.)

- The list shows the cubes open in the session; the current one is
  preselected. Cubes of a different size are greyed out. Tick at least 2.
- *Method*: *Mean*, *Median* (across cubes, per channel, of the mean
  spectra) or *Weighted Mean*.
- *Weight by* (Weighted Mean only): *Uniform*, *RMS* (1/σ² of each cube's
  mean spectrum) or *Peak*. A channel blank in one cube is averaged over the
  others.
- *Stack*: the result is a SPEC product, *Stacked (method, N=…)*, in a
  spectrum pane.

The cubes are not aligned: they must already share the spatial grid and the
spectral axis (channels, width, frame). Nothing warns you if they do not.

Details: [Spectral tools: methods](../../technical/methods/spectral-tools.md),
[Spectral API](../../technical/backend-api/spectral.md).

## CO isotopologues (LTE)

*Tools ▸ Spectral ▸ CO Isotopologues (LTE)…*, in a cube window or a 2-D map
window, takes ¹³CO and C¹⁸O, and optionally ¹²CO, in J = 1–0 or 3–2. It gives
the optical depth from the ¹³CO/C¹⁸O ratio, the excitation temperature, the
opacity-corrected column densities, N(H₂) and the mass, assuming LTE.

- *¹³CO*, *C¹⁸O*, *¹²CO (optional)*: any cube or 2-D map open in the session.
  They are guessed from the file names and the window's rest frequency.
  A cube is integrated over the *Velocity window*. A 2-D map is read as an
  integrated intensity in K km/s, except a ¹²CO map, which is read as a peak
  temperature in K. Maps are on the T_A* scale unless η_mb is 1. C¹⁸O and
  ¹²CO are reprojected onto the ¹³CO grid when their grids differ; beams are
  not matched, so smooth the data to a common resolution first.
- *Velocity window* (km/s): taken from the spectrum's fit range when one is
  active, else from the channels of the moment map (padded by half a channel
  at each end). The channels outside the window give each pixel's noise, so
  at least 10 must remain outside it.
- *η_mb*: leave it empty to read it from the header. A header with TEMPSCAL,
  BUNIT or BTYPE naming T_mb gives 1; otherwise BEAMEFF/FORWEFF or ETAMB is
  used. ETAL is never used. A typed value overrides the header (for CHIMPS,
  whose data are T_A*, Rigby et al. 2016 adopt 0.72). When the header has
  none, the run stops and the field is marked as required. After a run, the value used and its source are
  shown under the fields.
- *Distance*, *R_GC*, *X(¹³CO)/X(C¹⁸O)*: X comes from the galactocentric
  gradients at R_GC, and R_GC comes from the distance and the map centre. Type
  either to override it; one of the three is required. The distance is also
  needed for the mass.
  *¹²C/¹³C gradient*: Milam et al. 2005 (the default) or Wilson & Rood 1994.
  *N(H₂) from*: C¹⁸O (the default) or ¹³CO. *X(¹²CO)*: 8.5 × 10⁻⁵ unless
  typed.
- *Fixed T_ex*: leave it empty to measure T_ex. It then comes from the ¹²CO
  peak (assumed optically thick), or, without ¹²CO, from the ¹³CO peak with τ
  at the peak, which needs ¹³CO and C¹⁸O as cubes. A typed value is recorded
  as an assumption.
- *S/N threshold*: default 3σ. Pixels below it in either line are not solved.
- *Regions*: the window's regions, one table row each. They are used only
  when ¹³CO is the window's own dataset, because regions are measured on the
  ¹³CO grid.

*Run* fills the *Results* tab with three parts:

- a table per region: W_mb of each line [K km/s], R, τ(¹³CO) and its range at
  R ± σ_R (an open end, such as "≥ 1.1", means no upper bound), τ(C¹⁸O),
  T_ex [K], N(¹³CO), N(C¹⁸O), N(H₂) [cm⁻²] and the mass [M☉]. Values are on
  the T_mb scale, and "–" marks a value that cannot be computed;
- the maps written: ratio, τ(¹³CO), τ(C¹⁸O), T_ex, N(¹³CO), N(C¹⁸O), N(H₂)
  and a flag map (0 ok, 1 optically thin, 2 no LTE solution, 3 below S/N).
  Double-click a map, or select it and click *Open Map*, to open it in a viewer.
  Each map's FITS header records the assumptions;
- the assumptions, with where each number came from, the pixel counts per
  flag, and any warnings.

*Save CSV…* writes the table at full precision, with the values and their σ
in separate columns. The header lines (starting with `#`) record the input
files, the request and the assumptions. Each run is also recorded as an `LTE`
row in Session Data.

Details: [Products API](../../technical/backend-api/products.md)
(`POST /v1/products/lte_co`).
