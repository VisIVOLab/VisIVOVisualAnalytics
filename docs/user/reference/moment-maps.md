# Moment maps

A moment map collapses the spectral axis of a cube into a 2-D image: each
pixel holds one property of the spectrum on that line of sight (integrated
intensity, mean velocity, dispersion and so on). Moments are computed on the
backend from the [cube viewer](cube-viewer.md) with *Tools ▸ Maps ▸ Compute
Moment…* (or *Compute Moment* in *Inspector ▸ Analysis*).

For a walk-through, see [Moment maps and PV diagrams](../use-cases/03-moments-and-pv.md).

## Moment Map Settings dialog

The dialog does not block the window.

- *Moment order*: one of the orders in the table below.
- *Channel range ▸ Start → End*: the channels to integrate over (default: the
  whole cube). Restrict it to the channels with the line; line-free channels
  add noise to M0 and bias M1 and M2.
- *Enable threshold mask*: on by default. Only voxels above the threshold
  contribute. Untick it for an unmasked moment.
- *3σ of the noise estimate*: offered, and chosen by default, after *Tools ▸
  Statistics ▸ Estimate Noise...* in this session. The provenance records it.
- *Auto threshold (≈ 3σ from MAD)*: the default without a noise estimate;
  median + 3σ, with σ from the MAD of the cube.
- *Mask threshold*: a value typed by hand when both options above are
  unticked.
- *Compute*: starts the computation. A progress window shows whether the
  task is queued or running; *Cancel* stops it on the backend, as does
  closing the cube window. A second *Compute* waits for the first.

The channel range and mask are remembered per cube.

When done, the map replaces the channels in the *2D Slice* pane and is added
to Session Data; *2D Slice* in the pane's ▾ menu brings the channels back.
Switching between them does not recompute. An identical request returns the
cached result; a file changed on disk is computed again.

Whatever the mask, M1 pixels outside the cube's spectral range and M2 pixels
outside 0 to span² are blanked: those values come only from noise.

## Orders

Sums run over the channel range; Δv is the channel width. Blank (NaN) voxels
are left out. Where M₀ or σ is zero the pixel is NaN.

| Order | Name in the dialog | Definition | Unit |
|---|---|---|---|
| 0 | *Moment 0: Integrated intensity (M0)* | M₀ = Σ Iᵢ Δvᵢ | `BUNIT` × spectral unit |
| 1 | *Moment 1: Intensity-weighted coordinate (M1)* | Σ Iᵢ vᵢ Δvᵢ / M₀, the line centroid | spectral unit |
| 2 | *Moment 2: Spectral dispersion σ (M2)* | √(Σ Iᵢ (vᵢ − M₁)² Δvᵢ / M₀), a standard deviation, not a FWHM | spectral unit |
| 3 | *Moment 3: Skewness (M3)* | μ₃ / σ³; positive means a tail on the high-velocity side | none |
| 4 | *Moment 4: Excess kurtosis (M4)* | μ₄ / σ⁴ − 3; positive is peakier than a Gaussian, negative flatter or double-peaked | none |
| 5 | *Moment 5: 5th-order diagnostic (M5)* | μ₅ / (M₀ σ⁵) | none |
| 6 | *Moment 6: RMS (M6)* | √(Σ Iᵢ² Δvᵢ / Σ Δvᵢ) | `BUNIT` |
| 8 | *Moment 8: Maximum (M8)* | max(I), the peak map | `BUNIT` |
| 10 | *Moment 10: Minimum (M10)* | min(I), for absorption | `BUNIT` |

For a FWHM or equivalent width per pixel use the line-width map
([Spectral analysis tools](spectral-tools.md)).

## Choosing order and channels

| Goal | Setting |
|---|---|
| Where is the line? | M0 over the line's channels |
| Is it rotating? | M1 over a tight range around the line, mask on |
| Where is the line wide? | M2, same range and mask as M1 |
| Is the line asymmetric? | M3 over a wider range that keeps the tails |
| Where is the peak? | M8; less noisy than M0 for a narrow line |

Find the channel range with *Extract Spectrum* or a region spectrum
([Cube viewer](cube-viewer.md)), and the noise with *Estimate Noise...*
([Regions, PV diagrams, noise](region-pv-noise.md)).

## Reading the result

The status bar sums the result up (*Moment map: … | Unit: … | Ch: … | Mask:
… | Scope: …*). Its tooltip and the product's provenance (*Inspector ▸
Parameters*) give the unit, the cube's `BUNIT`, the spectral axis, the channel
range, the mask and threshold, and any WCS warning.

The colour map depends on the order: *Inferno* for M0 and orders above 2,
*CoolWarm* for M1, *Viridis* for M2. Change it with *Color map* in *DISPLAY*
while the moment's pane is active; *Edit* opens the LUT editor.

A moment pane can be drawn on: regions, *Extract PV Diagram…* and the probe
work on the map (see [Regions, PV diagrams, noise](region-pv-noise.md)).

## Using a moment map elsewhere

- *Tools ▸ Export ▸ Export Selected View…*, with the moment's pane focused
  (it reads *Export … as FITS…* with the moment's name): writes the moment as
  2-D FITS in Workspace Exports, recomputed at full precision, with the
  celestial WCS and `BUNIT`. Open it with *File ▸ Open…* in the
  [Image viewer](image-viewer.md).
- *Tools ▸ Views ▸ Overlay Slice on an Open Image*: draws the map as contours
  on an open image viewer.
- *Tools ▸ Maps ▸ Angular Power Spectrum of Moment Map* and *Skeleton / Voids
  of Moment Map*: analyse the moment on screen. See [Cosmology tools](cosmology.md).

## Large cubes

A moment over a large cube runs as a backend task with no time limit (an M2 of
a 450 GB cube took about 15 minutes on a cluster file system). Channel ranges
too large for memory are streamed from disk, so the cube is never loaded
whole. The memory and Dask settings are in the
[Configuration reference](../../technical/configuration.md#moments-and-dask).

Details: [Products API, moments](../../technical/backend-api/products.md).
