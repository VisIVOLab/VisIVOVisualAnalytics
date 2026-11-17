# Spectral tools: methods and implementation

User reference: [Spectral analysis tools](../../user/reference/spectral-tools.md).

## Placing a line list on the spectral axis

A line list carries rest frequencies. Two conversions place it on the plot,
and the dialog asks for both:

1. The unit. The dialog preselects it when the file declares one (a
   `# unit = GHz` comment, or a column named like Splatalogue's
   `Freq-GHz(rest frame)`), and otherwise asks.
2. The frame: `f_obs = f_rest / (1 + z)`, from a systemic velocity in km/s
   (read as an optical velocity, so `z = v/c`), from z directly, or no shift
   for a list already in the observed frame.

On a velocity axis the cube's own rest frequency (`RESTFRQ`/`RESTFREQ`) and the
axis's velocity convention, from its `CTYPE`, are both needed:

| CTYPE | Convention | Axis coordinate of an observed frequency *f* |
|---|---|---|
| `VOPT`, `FELO` | Optical | `v = c (f_rest/f − 1)` |
| `VRAD` | Radio | `v = c (1 − f/f_rest)` |
| `VELO` | Relativistic (FITS standard) | `v = c (f_rest² − f²)/(f_rest² + f²)` |
| `FREQ` | none | the observed frequency, in the axis unit |

Optical and radio velocities of the same line differ by about `v²/c`: 3.3 km/s
at 1000 km/s, several channels of a typical HI cube.

Markers are stored as coordinates on the axis they were computed for and are
dropped when the pane shows a product on another spectral axis. A list that
converts but lands outside the plotted band is reported (*3 lines placed, but
none falls inside the plotted range … check the unit and the systemic velocity
/ redshift*); a frequency list on a velocity axis without `RESTFRQ`, and a
velocity list on a frequency axis, are refused with a message.

### File formats

- A two-column `value,label` list, `#` for comments.
- A catalogue export: the loader reads the header row and finds the frequency
  column (skipping error columns), the species or chemical-name column and the
  quantum-number column, which it appends to the label (*CO 2-1*). Comma, tab,
  semicolon and colon separators are handled, and quoted fields are unquoted.

The bundled list (`resources/spectral_lines/common_lines.csv`) is rounded to
the kHz from the public CDMS / JPL values as distributed through Splatalogue.
There is no online catalogue query, because backends are often deployed on
compute nodes with no outbound network access.

### Check on WALLABY

The demo cube has `CTYPE3 = VOPT`, `CUNIT3 = m/s`, `RESTFRQ = 1420405751.79`,
covering about 900 to 1310 km/s. With the bundled list and a systemic velocity
of 1100 km/s the HI 21 cm marker lands at exactly 1.1 × 10⁶ m/s: for the
cube's own transition the axis coordinate is the systemic velocity.

## Gaussian fit

One Gaussian plus a linear baseline (five parameters: peak, centre, width and
two baseline terms) is fitted on the backend to the finite channels only; at
least 6 are needed so that the uncertainties can be estimated. Errors are 1σ;
where one cannot be determined no `±` is printed. When the channels are
correlated (instrumental or display smoothing) the backend scales the errors
when it can quantify the correlation (`errors ×1.4 for correlated channels`)
and otherwise says they are uncorrected. *Clear Fit* also invalidates a fit
still in flight.

## Line-width map

FWHM is 2.3548 σ of a per-pixel Gaussian fit; EW is M0 / peak = ∫ I dv /
max(I). The cube is split into row chunks fitted in parallel over the backend
worker pool. An SNR cutoff (default 3, `VISIVO_LINEWIDTH_SNR`) skips background
pixels before the fit. Results are cached by parameters and file identity
(size, modification time). The fit is per pixel with no spatial
regularisation.

## Baseline subtraction

- Each pixel is fitted independently over its own unflagged samples in the
  line-free selection; pixels sharing a flagging pattern are fitted together,
  and one pixel's fit never depends on what another pixel is missing.
- The polynomial is evaluated over the whole spectrum, so channels outside the
  line-free ranges are extrapolated, and a high order diverges as the *n*-th
  power of the distance: degree 10 fitted on channels 0 to 99 of a 1000-channel
  cube injects a false signal of about 4 into the far channels of a cube whose
  noise is 0.0026. The tool measures how much more uncertain the baseline is at
  its worst channel than where it was fitted, from the channels that carried
  data, and warns in the summary (also in the provenance), as it does when the
  residual RMS comes out higher than the input's.
- A pixel with fewer than `poly_order + 2` unflagged line-free channels is
  written as NaN.
- The polynomial is fitted in a channel coordinate rescaled so the line-free
  range spans [-1, 1], which keeps high orders well conditioned.
- The output is written as a 3-D FITS in the backend's temp directory; a 4-D
  input with a degenerate Stokes axis loses `CTYPE4`, `CRVAL4`, `CDELT4`,
  `CRPIX4`, and `BLANK` is dropped (defined only for integer data).

## Stacking

Each cube is averaged over its pixels into a mean spectrum; the N spectra are
then combined per channel. *Median* is the median across cubes of those
spatially averaged spectra, not a per-pixel median. With *Weight by: RMS* each
cube is weighted by 1/σ², σ being the RMS of its mean spectrum; with *Peak* by
the largest absolute value of that spectrum. In a weighted mean each channel is
divided only by the weights of cubes with finite data there. Explicit per-cube
weights are accepted only through `POST /v1/spectral/stack`.

The cubes are not reprojected. Bring them onto a common grid first: VisIVO
Visual Analytics has no cube reprojection (the mosaic collapses cubes to their
first plane); use `POST /v1/astrometry/reproject/{dataset_id}` per plane, or
CASA `imregrid` or Python `reproject`. In the stack dialog a cube registered
more than once (a baseline output opened again) is listed once.

See [Spectral API](../backend-api/spectral.md).
