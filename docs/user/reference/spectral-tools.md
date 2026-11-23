# Spectral analysis tools

Tools that work along the spectral axis of a cube in the
[cube viewer](cube-viewer.md). Two act on a spectrum already in a pane (line
identification, Gaussian fit), from *Inspector ▸ Analysis ▸ SPECTRUM*. Three
compute over the cube, from *Tools ▸ Spectral*: line-width map, baseline
subtraction and stacking. Their dialogs do not block the window.

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

- At least 6 finite channels are needed.
- The fit tries several starting positions and widths and keeps the best
  resolved solution, so a narrow line on a wide spectrum is found as long as
  it stands out. A strong edge spike or a brighter noise peak can still
  capture the fit; the result then carries a warning that it is unreliable.
  The profiler does not yet restrict the fit to the visible range.
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
