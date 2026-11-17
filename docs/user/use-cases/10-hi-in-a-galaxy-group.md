# Measure the HI of the galaxies in a group with regions

You will import a region file with the members of the galaxy group HCG 44,
measure each galaxy on a moment-0 map, subtract a background measured in an
annulus, and turn the numbers into HI fluxes and masses to compare with the
published ones.

## You need

- The HCG 44 cube `hgc44.fits` from the demo data, observed with WSRT:
  700 × 700 pixels of 8″, beam 53.0″ × 32.7″, unit Jy/beam. The velocity
  axis is optical and barycentric (`VELO-HEL`, `SPECSYS = 'BARYCENT'`), in
  m/s, with channels 8.24 km/s wide. The channels were Hanning-smoothed, so
  neighbours are correlated.
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
   drops the faint pixels of each galaxy and biases the flux low. Press
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
   Untick *NGC 3193* and *Background* to see the central pair alone (the node
   reads `hcg44_group.reg (5, 2 hidden)`), then tick them again.

   ```{figure} img/10-hi-in-a-galaxy-group-2.png
   :alt: The moment-0 map of HCG 44 with the five imported regions and their labels, and the Session Data tree listing hcg44_group.reg and its five regions with NGC 3190 selected.
   :width: 100%

   The five members and the background annulus on the moment-0 map, and the region list in Session Data.
   ```

### Measure each galaxy and the background

5. Click the *Moment 0* pane to make it active: *Analyse* measures the active
   pane. Right-click the *NGC 3190* row and choose *Analyse*. The region's
   mean spectrum opens in a pane, and *Inspector ▸ Parameters* shows *2D
   stats from: Moment 0 (pane N)*, *Valid pixels* and the *Mean* of the map
   inside the circle.

   ```{figure} img/10-hi-in-a-galaxy-group-3.png
   :alt: The moment-0 map beside the NGC 3190 spectrum with its double-horned profile, and the Inspector reading the 2D stats from Moment 0 (pane 1): 706 valid pixels, mean 140.92.
   :width: 100%

   Analyse on NGC 3190: its mean spectrum and the map statistics inside the circle.
   ```

6. Repeat for the other galaxies and for *Background*. Each analysis stays in
   *Session Data* under its region's name. In the spectra, NGC 3187 sits at
   the high-velocity end of the range and NGC 3185 at the low end.

7. For the noise per channel, choose *Tools ▸ Statistics ▸ Estimate
   Noise...*, set *X (columns)* 50 → 200, *Y (rows)* 50 → 200, all channels,
   and press *Estimate*. The median σ is 2.24 × 10⁻⁴ Jy/beam; Serra et al.
   (2013) quote 0.22 mJy/beam. Do not estimate on the annulus: the noise box
   would be its bounding box, galaxies included.

   ```{figure} img/10-hi-in-a-galaxy-group-4.png
   :alt: The Noise Estimation result for the box 50 to 200 in X and Y, with sigma and MAD per channel, beside the slice with the amber noise box in its lower-left corner.
   :width: 100%

   Noise per channel from a line-free box.
   ```

### From map values to Jy km/s

The moment-0 map is in Jy/beam × m/s, because the cube's velocity axis is in
m/s. Subtract the background mean, multiply by the pixel count, and divide by
the beam area in pixels and by 1000:

S = (Mean − Mean_bg) × N / Ω / 1000 Jy km/s

with Ω = π θ_maj θ_min / (4 ln 2) / (8″)² = 30.66 pixels. Pixels are
correlated over a beam, so the error of S is σ_bg √(N / Ω) / 1000, where
σ_bg is the *Std Dev* in the annulus. The app gives no HI mass for a region;
use M_HI = 2.356 × 10⁵ D² S M☉ (D in Mpc), the relation of VisIVO's HI
products, with D = 25 Mpc as in Serra et al. (2013).

## What you get

*Background* gives 2186 valid pixels, *Mean* −3.26 and *Std Dev* 48.17
(Jy/beam m/s).

| Galaxy | Valid pixels | Mean (Jy/beam m/s) | S (Jy km/s) | M_HI (10⁸ M☉) | Serra et al. 2013: S, M_HI | Ratio of S |
|---|---|---|---|---|---|---|
| NGC 3190 | 706 | 140.92 | 3.32 ± 0.23 | 4.9 | 4.00, 5.8 | 0.83 |
| NGC 3187 | 1105 | 157.66 | 5.80 ± 0.29 | 8.5 | 8.22, 12.0 | 0.71 |
| NGC 3185 | 1088 | 35.15 | 1.36 ± 0.29 | 2.0 | 2.11, 3.1 | 0.65 |
| NGC 3193 | 396 | 31.06 | 0.44 ± 0.17 | | not given | |

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
  pixel to one galaxy. A circle also holds noise and misses emission beyond
  its edge, such as the tidal tails of NGC 3187. Inside the published mask,
  which the authors provided, the app's selection statistics give 3.19 and
  6.09 Jy km/s for NGC 3190 and NGC 3187 on this cube, within 5 % of the
  circles, so the regions explain little of the gap.
- A 300 kpc HI tail runs north of the group, outside these regions (Serra et
  al. 2013). Adding KAT-7 and ALFALFA data, which recover more extended
  emission, Hess et al. (2017) find about twice its mass. How much flux the
  missing short spacings take from the galaxies, a few arcminutes across, is
  not known.
- The background is about 2 % of the NGC 3190 mean here; near a negative
  bowl or residual continuum it matters more.

## References

- Serra P., Koribalski B., Duc P.-A., et al., 2013, MNRAS, 428, 370.
- Hess K. M., Cluver M. E., Yahya S., et al., 2017, MNRAS, 464, 957.

## Next

- [Regions, PV diagrams, noise](../reference/region-pv-noise.md): the region
  list in Session Data, the statistics and the noise estimate.
- [Find and measure a source](02-find-and-measure-a-source.md): the flux of
  one galaxy from a 3-D selection instead of a region.
- [Moment maps and PV diagrams](03-moments-and-pv.md).
