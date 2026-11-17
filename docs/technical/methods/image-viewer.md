# Image viewer: methods and implementation

User reference: [Image viewer](../../user/reference/image-viewer.md).

## Preview, full resolution and level of detail

The viewer first shows a downsampled preview (`/v1/image/preview`, capped on
its longest side) and loads the full resolution (`/v1/image/full`) in the
background; the LUT and WCS overlay keep working during the upgrade.

For very large mosaics the full upgrade is assembled from tiles
(`/v1/image/tile`) at a decimated level of detail capped at about 64 Mpx. All
overlays and tools (probe, profile, region statistics, ruler and angle,
catalogue markers) report full-resolution pixel and WCS coordinates. A
displayed pixel then stands for a block of source pixels, and the status bar
and the cursor readout say which (*each pixel the mean of N×N*, or *sampled:
1 pixel in N×N*).

### Viewport-driven level of detail

On a multi-level pyramid image the viewer streams the visible region at the
zoom-appropriate resolution: when a pan or zoom settles, it fetches only the
tiles covering the current view at the pyramid level where one source pixel is
about one screen pixel, and swaps that sub-image in while keeping the camera in
place. Memory stays bounded by the viewport. The streamed sub-image carries a
non-zero origin, but its world coordinates are still full-resolution source
pixels, so readouts and overlays stay correct. Zooming back out returns to a
coarse whole-image level.

### Averaged overviews of large images

An image larger than 256 MiB (as float32; `VISIVO_IMAGE_PYRAMID_BYTES` on the
backend) gets averaged overviews: each coarse level is the mean of the pixels
it covers, as for the cube pyramid ([Brick
pyramid](../design/brick-pyramid.md#images)). Sampling one pixel in 4, 16 or 64,
as a smaller image's coarse levels do, lets noise and small sources alias;
averaging shows what the area contains, and serves every tile from a compact
file instead of reading pages across the whole image.

The first time such an image is opened, the backend builds the overviews in
the background: the status bar shows *Building averaged overviews of the image
on the backend… N% (sampled meanwhile)*, the image is shown sampled meanwhile,
and the view is loaded again, averaged, when they are ready. They are kept
with the cube pyramids (`<exports>/.pyramids`) and reused. They take about 4/3
of the image's size as float32 (about 2.7 times a 16-bit file, 5.3 times an
8-bit one), are not removed automatically, and an image that changes gets new
ones. A compressed image (`.fits.gz`, `.fz`) gets them too, built from the
decompressed copy in the backend's cache.

## Layer loading

Before anything is downloaded, the two headers are compared: a file that
covers none of the sky the current image covers is not added. The sky bounds
come from `wcsrange()`, a bounding box, so a rotated field straddling RA = 0
can measure narrower than it is; *Add Anyway* overrides the check.

All layer sources go through the same `loadImageLayer()` / `AstroUtils` /
`libwcs` pipeline. Which path a file takes is decided by where the backend is:
against a remote backend the layer is opened there and its WCS built from the
header the backend returns, so a path that also exists locally cannot load a
different file. Frame conversions use `wcscon()`.

The beam ellipse is sized in pixels from the angular axes divided by
`|CDELT1|` and rotated by `BPA`. The master layer's LUT has `NanColor = (0, 0,
0, 0)`, so blank pixels are transparent.

## Region statistics below full resolution

At full resolution a region's statistics are computed from the displayed
pixels, which are the file's. Below it they would not be: on averaged pixels
the mean is right but σ and the MAD are those of block means (about N times
smaller for uncorrelated noise), and on sampled pixels the sum is short by the
block's area. So when the view is reduced, the statistics shown on release come
from the file at full resolution: the backend reads the region in blocks and
computes count, blanks, minimum, maximum, sum, mean, σ, median and σ_MAD
exactly (`POST /v1/regions/stats`, see [Regions API](../backend-api/regions.md)).
The region is handed over as the area it covers on screen (on averaged levels,
exactly the blocks of its pixels). The same happens at full resolution when the
region reaches beyond the loaded part of the image. If the backend cannot
answer, the dialog falls back to the displayed pixels and says so. The
per-Stokes rows still come from the displayed layers.

The live readout while dragging stays on the displayed pixels; on regions over
about 200 000 pixels it reads a grid and scales up (`≈`). Below full
resolution it says so (*overview, N×N means* or *1 in N×N*) and its sum is
scaled by the block's area.

## Smoothing and regridding

*Smooth / Regrid…* convolves with a Gaussian normalised by the smoothed
coverage, so blank pixels do not eat their neighbourhood. `BMAJ`/`BMIN` grow in
quadrature with the kernel, and a Jy/beam map is scaled by the beam-area ratio
so the integrated flux (pixel sum over beam area) is preserved; the dialog
reports the factor. A regrid of a per-pixel unit (`Jy/pixel`) sums the block
instead of averaging it, which would lose flux by the square of the factor;
`BUNIT` decides and the result says which rule was used. SIP coefficients are
rescaled with the pixel size.

## Polarisation on 2-D images

### Companion search

*Load Stokes Q/U/V Companions…* searches the directory of the Stokes I file
(on the backend's filesystem when the backend is remote) with these patterns:

- `*StokesI*`, with `I` replaced by `Q`, `U`, `V`;
- `*_I.fits` and `*_I_PB.fits`, with the role tag replaced;
- `.fits.gz` variants of each (ASKAP and MeerKAT compressed releases).

The search is reported once as a summary; files found are loaded as layers
tagged with their Stokes role.

### Derived maps

All are computed in the client from the in-memory layers.

| Action | Formula | LUT |
|---|---|---|
| *Compute Pol. Intensity (P)* | `P = √(Q² + U²)` | Inferno |
| *Compute Pol. Angle (PA)* | `PA = ½ · atan2(U, Q)` (degrees, −90 to +90) | Spectrum |
| *Compute Frac. Pol. (P/I)* | `P / I`, clamped to [0, 1] | Viridis |
| *Compute Debiased P* | `√(max(0, Q² + U² − σ²))`, σ the mean of the MAD-based σ of Q and U | |
| *Compute Spectral Index…* | `α = log(S_B / S_A) / log(ν_B / ν_A)`, pixels with a flux ≤ 0 skipped | Spectrum, [−2.5, +1.5] |
| *Compute Faraday RM…* | least-squares `PA = PA₀ + RM · λ²`, `λ² = (c/ν)²`, three or more frequencies | Spectrum, [−500, +500] rad m⁻² |

The debiased σ is a uniform field average, a quick approximation of a
per-pixel noise map. The RM fit does no PA unwrapping and is only correct while
`|RM · Δλ²| < π/2` between consecutive frequencies; for large RM use RM
synthesis on the backend ([Polarisation API](../backend-api/polarisation.md)).
The spectral-index range covers synchrotron (α ≈ −0.7) to thermal (α ≈ +2)
regimes. Maps on different grids go through the Data Hub's *Pixel Math /
Spectral Index…* (`POST /v1/products/map_math`), which reprojects the second
map onto the first one's grid with bilinear interpolation whenever their sky
coordinates differ, blanks α wherever a flux is not positive, and does not
match beams.

### Vector overlay

Segments are drawn every *Grid step* pixels where P exceeds *SNR threshold* ×
the robust σ (MAD) of the P distribution, oriented along PA (north through
east), with `dx = −len·sin(PA)`, `dy = +len·cos(PA)`. This assumes the standard
orientation (`CDELT1 < 0`, north up); on a rotated WCS the orientation may need
correction.

### Region statistics, radio fields

σ_MAD is `1.4826 × MAD(values − median)`. With `BMAJ`/`BMIN` and at least one
Stokes layer, the beam area in pixels is `Ω_beam = π · BMAJ · BMIN / (4 ·
ln 2)` (in pixel units), the integrated flux is `sum / Ω_beam` in Jy, and the
peak SNR is in σ_MAD units. Each loaded Stokes role (I, Q, U, V, P, PA, P/I) is
re-analysed over the region: integrated Jy for flux-density maps, mean degrees
for PA, mean percentage for P/I.

## Dynamic spectra

### Detection and HDF5 input

| Format | Detection rule |
|---|---|
| FITS 2-D dynamic spectrum | `CTYPE1` / `CTYPE2` contain at least one time-like axis (`TIME`, `MJD`, `UTC`) and one frequency-like axis (`FREQ`, `FREQUENCY`, `WAVE`, `AWAV`). |
| HDF5 / LOFAR BFData | Extension `.h5`, `.hdf5`, `.hdf` or `.bfdata`. The backend walks the `SUB_ARRAY_POINTING_xxx / BEAM_yyy / STOKES_z` hierarchy and picks the first 2-D plane; otherwise it takes the largest 2-D float dataset in the file. |

The chosen HDF5 plane is converted to a sidecar FITS file
`<source>.dynspec.fits` next to the source, reused as long as it is newer than
the source. Sampling metadata is read from common attribute names (`TSAMP`,
`DT_S`, `CADENCE_S` for time; `FREQ_START_HZ`, `FCH1`, `FREQ_LOW_HZ` for
frequency; `DELTA_FREQ_HZ`, `FOFF`, `CHANNEL_BW_HZ` for the channel width).
Missing values default to 1.0.

### Supported PSRFITS

The backend also recognises PSRFITS files (multi-HDU FITS with a `SUBINT`
binary-table extension). On open they are converted to a 2-D sidecar dynspec
named `<source>.dynspec.fits`, so the viewer treats them like any other dynamic
spectrum. The layout is read from the headers, not guessed from the array
sizes:

- Folded files (`OBS_MODE` = `PSR` or `CAL`) give one time column per
  sub-integration, `TSUBINT` seconds wide, averaged across the `NBIN` phase
  bins.
- Search files (`OBS_MODE` = `SEARCH`) give `NSUB × NSBLK` time columns of
  `TBIN` seconds. 8-, 16- and 32-bit samples are read as they are; 1-, 2- and
  4-bit packed samples are unpacked; `SIGNINT = 1` reads them as signed. Other
  bit widths, or an `NBITS` that does not match how `DATA` is stored, are
  refused with a message naming `NBITS`.

Rows are placed on the time axis by `OFFS_SUB`, so the axis counts seconds
from the start of the observation and a dropped sub-integration leaves a blank
(NaN) column instead of shifting every later time. When `OFFS_SUB` is missing
or not on a regular grid, or the gaps would make the time axis more than four
times longer than the data it holds, the rows are joined end to end, and a
`HISTORY` card in the sidecar says why and that the times after a gap are
approximate.

Stored samples are converted to physical values with the per-channel,
per-polarisation `DAT_SCL` and `DAT_OFFS` (and `ZERO_OFF` for search data).
Channels with a zero `DAT_WTS` weight become NaN, so they are skipped by
averages and the pulse profile and are flagged by the RFI mask; other weights
are not applied. Stokes I is `AA + BB` when `POL_TYPE` is `AABB` or
`AABBCRCI`, otherwise the first polarisation. Per-channel central frequencies
come from the `DAT_FREQ` column when available, otherwise from `OBSFREQ` /
`OBSBW`. A file whose headers do not match its data (for example a `DATA` row
of the wrong size) is refused with the reason.

The dynspec tools work on time axes stored in either order: a file whose
`CDELT1` is negative folds and dedisperses to the same result as the same data
stored forwards.

### Time-series tools

- Dedispersion: `Δt(ν) = K · DM · (1/ν² − 1/ν_ref²)` per channel, with `K =
  4148.808 MHz² pc⁻¹ cm³ s`, shifting each channel by the nearest sample.
  ν_ref is the top of the band; the output header carries `DM` and `DMREFMHZ`.
- Pulse profile: the frequency-averaged time series is folded into 64 phase
  bins; a DM typed in the dedispersion field is applied in memory first.
- RFI mask: a pixel is flagged when its deviation from its channel's robust
  median exceeds `N × 1.4826 × MAD`, ignoring NaN samples. Output
  `<source>_rfimask_channel_<N>sigma.fits`, uint8 (0 good, 1 flagged).

The routes are in [Dynamic spectra](../backend-api/dynspec.md).
