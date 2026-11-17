# Measure the HI of the galaxies in a group with regions

You will import a region file with the members of the galaxy group HCG 44,
mark an annulus as the background, and read the integrated HI flux and mass
of each galaxy from a moment-0 map, to compare with the published ones.

## You need

- The HCG 44 cube `hgc44.fits` from the demo data (WSRT): pixels of 8″,
  beam 53.0″ × 32.7″, Jy/beam. The velocity axis is optical and barycentric
  (`VELO-HEL`, `SPECSYS = 'BARYCENT'`), in m/s, with Hanning-smoothed
  channels 8.24 km/s wide.
- A region file. Save these lines as `hcg44_group.reg`:

  ```text
  # Region file format: DS9 version 4.1
  global color=green width=2
  fk5
  circle(10:18:05.6,+21:49:56,120") # text={NGC 3190}
  circle(10:18:24.9,+21:53:38,90") # color=cyan text={NGC 3193}
  circle(10:17:47.8,+21:52:24,150") # text={NGC 3187}
  circle(10:17:38.6,+21:41:18,150") # text={NGC 3185}
  annulus(10:17:56.7,+21:51:10,240",320") # color=yellow text={Background}
  ```

  The circles sit on the optical positions of the galaxies and do not
  overlap. The annulus rings the central pair between 4′ and 5.3′.
- The basics of [Regions](04-regions.md).

## Steps

### Make the moment-0 map

1. Open `hgc44.fits` with *File ▸ Open…*. If it opens as a preview, press
   *Load full resolution*.

2. Choose *Tools ▸ Maps ▸ Compute Moment…*. Keep *Moment 0: Integrated
   intensity (M0)*, set *Start → End* to 21 → 101 (1053 to 1713 km/s, where
   the group's HI is) and leave *Enable threshold mask* off: a threshold
   drops the faint pixels of each galaxy and biases the flux low, and on a
   masked map the Inspector adds a *Flux warning* saying so. Press
   *Compute*. *Moment 0* opens in a pane.

   ```{figure} img/10-hi-in-a-galaxy-group-1.png
   :alt: The Moment Map Settings dialog: moment 0, channels 21 to 101 (81 of 158 selected), threshold mask off.
   :width: 50%

   Moment 0 over channels 21 to 101, mask off.
   ```

### Import the members and pick them in Session Data

3. Choose *Tools ▸ Regions ▸ Import Region (CRTF/DS9)…* and pick
   `hcg44_group.reg`. The five regions appear with their labels on the
   moment map.

4. In *Session Data*, under *Regions*, the file is a node,
   `hcg44_group.reg (5)`. Click *NGC 3190 · Circle* to select it on the map.
   Untick *NGC 3193* and *Background* to see the central pair alone
   (`hcg44_group.reg (5, 2 hidden)`), then tick them again.

   ```{figure} img/10-hi-in-a-galaxy-group-2.png
   :alt: The moment-0 map of HCG 44 with the five imported regions and their labels, and the Session Data tree listing hcg44_group.reg and its five regions with NGC 3190 selected.
   :width: 100%

   The five members and the background annulus on the moment-0 map, and the region list in Session Data.
   ```

### Mark the background and set the distance

5. Right-click the *Background* annulus, on the map or its row in *Session
   Data*, and tick *Background Region*. The row now ends in `· background`.
   *Analyse* on another region subtracts the mean of the nearest background
   region on the same map and takes its scatter as the noise. (A ds9 line
   with the `background` property comes in already marked.)

   <!-- Screenshot pending (see SHOTLIST.md): img/10-hi-in-a-galaxy-group-5.png -->

6. Choose *Tools ▸ Regions ▸ HI Mass Distance…* and type `25`, the distance
   in Mpc that Serra et al. (2013) adopt; the setting is kept. A redshift
   (`z=0.0046`) also works: the app takes the luminosity distance in
   Planck18 and divides the mass by 1 + z. For a group this near, a typed
   distance is safer.

### Measure each galaxy

7. Click the *Moment 0* pane to make it active: *Analyse* measures the active
   pane. Right-click the *NGC 3190* row and choose *Analyse*. The region's
   mean spectrum opens in a pane. *Inspector ▸ Parameters* shows, for
   *Moment 0 (pane N)*, *Integrated flux* 3.32 ± 0.27 Jy km/s, *Flux error
   from* (the annulus, its pixels, mean and σ), *Beam area* 30.66 pixels and
   *HI mass* (4.89 ± 0.39) × 10^8 M☉ at 25 Mpc.

   ```{figure} img/10-hi-in-a-galaxy-group-3.png
   :alt: The moment-0 map beside the NGC 3190 spectrum, and the Inspector reading the integrated flux, the background the error comes from, the beam area and the HI mass of NGC 3190.
   :width: 100%

   Analyse on NGC 3190: its mean spectrum, flux and HI mass.
   ```

8. Repeat for the other galaxies; each analysis stays in *Session Data*.
   The spectra put NGC 3187 at the high-velocity end of the range and
   NGC 3185 at the low end.

9. For the noise per channel, choose *Tools ▸ Statistics ▸ Estimate
   Noise...*, set *X (columns)* 50 → 200, *Y (rows)* 50 → 200, all channels,
   and press *Estimate*. The median σ is 2.24 × 10⁻⁴ Jy/beam; Serra et al.
   (2013) quote 0.22 mJy/beam. Not on the annulus: its bounding box holds the
   galaxies.

   ```{figure} img/10-hi-in-a-galaxy-group-4.png
   :alt: The Noise Estimation result for the box 50 to 200 in X and Y, with sigma and MAD per channel, beside the slice with the amber noise box in its lower-left corner.
   :width: 100%

   Noise per channel from a line-free box.
   ```

The app computes S = (Σ − N · mean_bg) / Ω from the region's N pixels
summing to Σ, the background mean and the beam area Ω in pixels, with m/s
turned into km/s. The error adds, in quadrature, the noise of the region's
N / Ω beams and that of the background mean. M_HI = 2.356 × 10⁵ D² S M☉.

## What you get

The background annulus has 2186 valid pixels, mean −3.26 and σ 48.17
Jy/beam m/s.

| Galaxy | Valid pixels | S (Jy km/s) | M_HI at 25 Mpc (10⁸ M☉) | Serra et al. 2013: S, M_HI | Ratio of S |
|---|---|---|---|---|---|
| NGC 3190 | 706 | 3.32 ± 0.27 | 4.89 ± 0.39 | 4.00, 5.8 | 0.83 |
| NGC 3187 | 1105 | 5.80 ± 0.35 | 8.54 ± 0.52 | 8.22, 12.0 | 0.71 |
| NGC 3185 | 1088 | 1.36 ± 0.35 | 2.01 ± 0.52 | 2.11, 3.1 | 0.65 |
| NGC 3193 | 396 | 0.44 ± 0.19 | | not given | |

NGC 3193 is below 3σ. Serra et al. (2013) give no HI for it
and note that only van Driel et al. (2001) claim a detection.

The spirals come out 17 to 35 % below the published fluxes, more so the
farther they are from the pointing centre (10:18:54.2, +21:59:30): 15′ for
NGC 3190, 17′ for NGC 3187, 25′ for NGC 3185. The published fluxes
are corrected for the WSRT primary beam (FWHM about 0.6°); this cube is not.
*Estimate Noise* gives the same σ, 2.24 to 2.26 × 10⁻⁴ Jy/beam, at the
centre and in the corners, where a corrected cube would be several times
noisier.

## Caveats

- To compare with a catalogue corrected for the primary beam, divide each
  flux by the WSRT beam response at the galaxy's position. The app does not
  apply that correction.
- Serra et al. (2013) summed the emission inside a 3-D mask and gave each
  pixel to one galaxy; a circle also holds noise and misses emission beyond
  its edge. Inside the published mask, which the authors provided, the app's
  selection statistics give 3.19 and 6.09 Jy km/s for NGC 3190 and NGC 3187
  on this cube, within 5 % of the circles, so the regions explain little of
  the gap.
- A 300 kpc HI tail runs north of the group, outside these regions (Serra et
  al. 2013); with KAT-7 and ALFALFA data, which recover more extended
  emission, Hess et al. (2017) find twice its mass. What the missing short
  spacings take from the galaxies themselves is not known.
- The background mean is about 2 % of the NGC 3190 mean here, but its error,
  subtracted over every pixel of a circle, raises the flux error of NGC 3187
  and NGC 3185 from 0.29 to 0.35 Jy km/s. A wider annulus lowers it.

## References

- Serra P., Koribalski B., Duc P.-A., et al., 2013, MNRAS, 428, 370.
- Hess K. M., Cluver M. E., Yahya S., et al., 2017, MNRAS, 464, 957.

## Next

- [Regions, PV diagrams, noise](../reference/region-pv-noise.md): region
  flux, background regions and the HI distance in detail.
- [Find and measure a source](02-find-and-measure-a-source.md): the flux of
  one galaxy from a 3-D selection instead of a region.
- [Moment maps and PV diagrams](03-moments-and-pv.md).
