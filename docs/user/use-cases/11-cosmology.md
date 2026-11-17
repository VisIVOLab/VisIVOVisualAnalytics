# Cosmology: a matter power spectrum and distances

You will project the dark-matter density of a simulation box, measure the
power spectrum of the projection and read its slope. Then you will turn a few
redshifts into distances under Planck18 and WMAP9 and see where the two
cosmologies disagree.

## You need

- `volumesnap63.bin` and its `.head` from the demo data: a VisIVO Binary Table
  volume with one field, `CDM-Density`, on a 512³ grid. Its values run from
  5 × 10⁻¹² to 3.6 × 10⁻⁴ around a mean of 6.9 × 10⁻⁸.
- A running backend; the local one is enough.
- The cube window basics from [First steps](01-first-steps.md).

## Steps

### Project the density

1. Choose *File ▸ Open…* and select `volumesnap63.bin`. The volume opens in
   the cube viewer on `CDM-Density`, on a Log colour scale.

2. Choose *Tools ▸ Maps ▸ Compute Moment…*. In *Moment Map Settings*, keep
   *Moment 0: Integrated intensity (M0)* and the whole channel range (the 512
   planes along Z), untick *Enable threshold mask* so the underdense cells
   count too, and press *Compute*. For a VBT volume the 0th moment is the sum
   along Z, so the map that replaces the slice is the column density of the
   box.

   ```{figure} img/11-cosmology-1.png
   :alt: The 3-D view of the density box beside the moment-0 map, the column density of the box, with filaments and haloes, and the moment product in Session Data.
   :width: 100%

   The density box in 3-D, and its column density as a moment-0 map over all 512 planes with the mask off.
   ```

### Measure the power spectrum

3. Choose *Tools ▸ Maps ▸ Angular Power Spectrum of Moment Map*. The
   Inspector's *Parameters* tab names the moment map, *channels 1–512*. Keep
   *Bins* at 64 and press *Compute P(ℓ)*. The backend writes the map to the
   Workspace Exports as FITS, then computes the spectrum.

4. A window shows the spectrum on log axes. The map has no sky coordinates,
   so the axis is the wavenumber k in cycles per pixel. Of the 64 bins, 55 hold
   modes. The slope line reads *Slope n = -2.008 ± 0.081 (P ∝ kⁿ) over k
   0.008166–0.4793, 46 bins, each weighted by 1/σ² of log P. One power law does
   not fit these bins (χ²/dof = 47.2)…*. Blue bins are in the fit; open grey
   circles (*Left out* in the legend) hold fewer than 8 Fourier modes or lie
   outside the fit range. The high-k bins hold thousands of modes and so carry
   almost all the weight: the red line follows them and passes well above the
   bins at low k, and the error is widened by √(χ²/dof) because a straight
   line does not describe the whole range.

   ```{figure} img/11-cosmology-2.png
   :alt: The angular power spectrum window: log axes, bins in the fit in blue, the red fitted line following the high-k bins, and the slope line n = -2.008 ± 0.081 over k 0.008166 to 0.4793, 46 bins, with the warning that one power law does not fit.
   :width: 80%

   The power spectrum of the projection with the default fit.
   ```

5. The default fit mixes the middle range with the steeper small scales. Type 0.02 in *Fit k from* and 0.2 in
   *to*, pressing Enter after each: the slope becomes *n = -1.200 ± 0.034*
   over 26 bins (χ²/dof = 1.36), wavelengths from 50 down to 5 cells, and the
   line runs through the blue bins. *Session Data* keeps the
   run under the first slope (*n = -2.01 · …*); its *Parameters* tab records
   the latest fit and range. *Export CSV…* writes the bins and the fit.

   ```{figure} img/11-cosmology-3.png
   :alt: The same window with the fit restricted to k from 0.02 to 0.2: the bins outside are open circles and the slope line reads n = -1.200 over 26 bins.
   :width: 80%

   The fit restricted to 0.02 to 0.2 cycles per pixel.
   ```

### Convert redshifts to distances

6. In the main window (the Data Hub, not the cube window) choose *Tools ▸
   Cosmology Calculator…*. With *Model* on
   Planck18, type `0.1, 0.5, 1, 2, 3` in the redshift box and press
   *Compute*. *Copy* puts the table on the clipboard.

7. Switch *Model* to WMAP9 and press *Compute* again.

   ```{figure} img/11-cosmology-4.png
   :alt: The Cosmology Calculator under WMAP9 for z = 0.1, 0.5, 1, 2 and 3, with the comoving and luminosity distances and the proper scale filled, and the other columns empty.
   :width: 100%

   Distances and scale under WMAP9; the D_A, lookback and age columns are empty for this preset.
   ```

## What you get

### The slope of the spectrum

| Fit range (cycles/pixel) | Measured in the app (2-D, projection) | Check with numpy (3-D, full cube) |
|---|---|---|
| 0.02 to 0.2 | -1.200 ± 0.034 | -1.19 |
| 0.1 to 0.48 | -2.402 ± 0.113 (χ²/dof 47.7) | -2.62 |
| all bins (default) | -2.008 ± 0.081 (χ²/dof 47.2) | |

A projection through the whole box keeps the 3-D modes with no component
along the line of sight, so its 2-D spectrum has the shape of the 3-D P(k).
The right-hand column is a separate Fourier transform of the full 512³ cube,
done outside the app as a check. From 0.02 to 0.2 the two slopes agree to
0.01. At the smallest scales the spectrum curves, so the slope depends on how
the bins are weighted (the unweighted 3-D fit gives -2.03). Over the two wider
ranges χ² rejects a single power law, so the app widens those errors by
√(χ²/dof) and says so under the plot.

The linear spectrum steepens towards k⁻³ on small scales; -1.2 is much
shallower because these scales are nonlinear (the cell-to-cell rms density
contrast is 7.4). Above k = 0.1 the spectrum falls faster. No cell of the grid
is empty, so each particle was spread over several cells, and that smoothing
removes power at the smallest scales. The file does not record the kernel, so
you cannot tell its effect apart from the inner structure of the haloes.

### Distances under two cosmologies

| z | D_C Planck18 (Mpc) | D_C WMAP9 (Mpc) | WMAP9 − Planck18 | kpc/arcsec Planck18 | kpc/arcsec WMAP9 |
|---|---|---|---|---|---|
| 0.1 | 432.566 | 422.982 | -2.2 % | 1.9065 | 1.8643 |
| 0.5 | 1946.417 | 1916.069 | -1.6 % | 6.2910 | 6.1929 |
| 1 | 3395.634 | 3363.071 | -1.0 % | 8.2313 | 8.1523 |
| 2 | 5308.189 | 5291.730 | -0.3 % | 8.5783 | 8.5517 |
| 3 | 6504.004 | 6503.902 | 0.0 % | 7.8831 | 7.8830 |

D_L differs by the same fraction. At low redshift a distance goes as c z / H₀,
and WMAP9's H₀ (69.32 km/s/Mpc against 67.66) makes it shorter. WMAP9's lower
Ωm (0.2865 against 0.3097) lengthens distances at higher redshift, and by
z = 3 the two effects cancel. Planck18 also fills D_A, lookback time and age:
at z = 1 they are 1697.817 Mpc, 7.9355 Gyr and 5.8513 Gyr.

The backend computes these with astropy's own Planck18 and WMAP9 objects;
astropy 8.0.0, called directly, gives the same values to every digit shown.

## Limits

- A VBT header has no box size, redshift or cosmology, so k stays in cycles
  per pixel. To convert to h/Mpc, multiply by 512 over the box side in Mpc/h,
  taken from the simulation's own notes.
- The spectrum is relative: Hann-windowed, with no deconvolution of the window
  or of the grid's smoothing. Compare slopes and shapes, not amplitudes.
- The moment always projects along Z.
- The file does not say how many particles made the grid, so the shot-noise
  level is not known; the spectrum shows no flat floor up to the Nyquist
  frequency.
- The presets are flat ΛCDM. WMAP9, Planck15 and Planck13 give D_C, D_L and the
  scale only. *Custom ΛCDM* has no radiation and may be flat or open; closed
  models are refused. Full support for curved models is
  [issue #36](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/36).

## Next

- [Cosmology tools](../reference/cosmology.md): the calculator's columns, the
  custom model, and the skeleton and void tracer for the same map.
- [Moment maps](../reference/moment-maps.md) and
  [VBT tables](../reference/vbt-viewer.md).
- [Catalogues, HiPS and cross-match](06-catalogues-hips-crossmatch.md): in the
  3-D catalogue viewer, *Model* in the *Cosmology* card places sources by
  redshift under the same presets.
- [Cosmology: methods and implementation](../../technical/methods/cosmology.md).
