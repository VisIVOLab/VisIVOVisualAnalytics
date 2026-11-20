# A molecular cloud in CO isotopologues

You will map a clump and a filament of the inner Galaxy in ¹³CO and C¹⁸O
J = 3→2 from the CHIMPS survey, measure both lines at the same position, and
turn their ratio into an optical depth.

## You need

- The ¹³CO cube from the demo data,
  `vlkb-cutout_2019-04-11_13-06-35_409523_CHIMPS_CHIMPS_13CO_40p00+0p33.fits`:
  167 × 69 pixels of 7.6″ (l = 39.82° to 40.18°, b = 0.156° to 0.300°) and
  402 channels of 0.5 km/s from 150.5 down to −50.0 km/s, radio velocity in
  the LSRK frame. It has no `BUNIT` and no beam: `TEMPSCAL = 'TA*'` means
  corrected antenna temperature in K, and the beam is 15″ (Rigby et al. 2016).
- Network access for the backend. The demo C¹⁸O cube (`…_C18O_40p66+0p00_A.fits`)
  has data only at l > 40.41°, so it shares no sky with the ¹³CO cube; step 2
  fetches the matching one.
- A region file, saved as `chimps_g40.reg`:

  ```text
  # Region file format: DS9 version 4.1
  global color=green width=2
  galactic
  circle(40.157,0.169,23") # text={Clump}
  circle(40.070,0.188,23") # text={Filament}
  ```

## Steps

### Open both cubes

1. Open the ¹³CO cube with *File ▸ Open…*. Click the *CUBE · 167×69×402* tag
   in the toolbar to read the header (`MOLECULE`, `RESTFRQ`, `SPECSYS`,
   `TEMPSCAL`).

2. In the main window's *VLKB* tab, set *glon* 40.0, *glat* 0.228, *dl*
   0.352, *db* 0.144 and press *Query*. Double-click the C¹⁸O 3-2 row whose
   *Identifier* ends in `CHIMPS_C18O_40p00+0p33` (*Coverage* `full`). It opens
   in its own cube window, 69 × 166 pixels and 400 channels. This tile stores
   latitude along x, so its map looks turned by 90°; regions read from a file
   still land on the same sky. Its channels sit 0.25 km/s off the ¹³CO ones.

   <!-- Screenshot pending (see SHOTLIST.md): img/13-molecular-cloud-1.png -->

3. In each window, *Tools ▸ Statistics ▸ Estimate Noise...* on all channels of
   a line-free box (*X (columns)* 100 → 160, *Y (rows)* 35 → 65 on ¹³CO;
   35 → 64 and 7 → 67 on C¹⁸O) gives a median σ of 0.53 K and 0.56 K per
   channel. Rigby et al. (2016) quote about 0.6 K.

### Moments of the clump

4. In the ¹³CO window choose *Tools ▸ Maps ▸ Compute Moment…*, keep *Moment 0:
   Integrated intensity (M0)*, set *Start → End* to 130 → 139 (86.0 to
   81.5 km/s), untick *Enable threshold mask* and press *Compute*. The clump
   peaks at 11.09 K km/s at l = 40.157°, b = 0.169°.

5. Compute *Moment 1: Intensity-weighted coordinate (M1)* and *Moment 2:
   Spectral dispersion σ (M2)* on the same channels with the mask and *Auto
   threshold (≈ 3σ from MAD)* on (1.62 K).

6. In the C¹⁸O window compute Moment 0 over 129 → 138 (85.75 to 81.25 km/s).

   <!-- Screenshot pending (see SHOTLIST.md): img/13-molecular-cloud-2.png -->

### A PV cut along the filament

7. In the ¹³CO window compute Moment 0 over 280 → 288 (11.0 to 7.0 km/s): a
   filament at l ≈ 40.07° rises from the bottom edge of the map. Click that
   pane, choose *Tools ▸ Spectral ▸ Extract PV Diagram…*, click on the ridge
   at the bottom edge and again at b ≈ 0.22°, then double-click. Set *Width*
   to 3.

   <!-- Screenshot pending (see SHOTLIST.md): img/13-molecular-cloud-3.png -->

### Spectra at the same position

8. In each window choose *Tools ▸ Regions ▸ Import Region (CRTF/DS9)…* and
   pick `chimps_g40.reg`.

9. Click the clump's Moment 0 pane, right-click the *Clump* row in *Session
   Data* and choose *Analyse*. *Inspector ▸ Parameters* shows the map's *Mean*
   in the circle, and the mean spectrum opens in a pane. Press *Fit Gaussian*
   above it. Repeat in the C¹⁸O window.

   <!-- Screenshot pending (see SHOTLIST.md): img/13-molecular-cloud-4.png -->

### From the line ratio to the optical depth

If both lines share one excitation temperature and fill the beam alike, the
ratio of their integrals is R = (1 − e^(−τ₁₃)) / (1 − e^(−τ₁₃ / X)), with
X = [¹³CO]/[C¹⁸O] = (¹⁶O/¹⁸O)/(¹²C/¹³C). Inside the solar circle this
sightline stays between R₀ sin l ≈ 5.5 kpc and R₀ = 8.5 kpc from the Galactic
centre, where the gradients of Wilson & Rood (1994) give X = 7.4 to 7.5. The
T_A* scale cancels in R, since one efficiency applies to two lines 1.3 GHz
apart. The app does not solve for τ; do it by hand.

## What you get

| Clump, circle of 23″ | ¹³CO 3→2 | C¹⁸O 3→2 |
|---|---|---|
| Moment 0 *Mean* (K km/s, T_A*), pixels | 5.04, 28 | 1.15, 29 |
| Moment 1 / Moment 2 *Mean*, masked (km/s) | 84.01 / 0.59 | |
| Fit: peak (K) | 1.68 ± 0.08 | 0.61 ± 0.10 |
| Fit: centre (km/s) | 83.90 ± 0.06 | 84.11 ± 0.14 |
| Fit: FWHM (km/s) | 2.59 ± 0.14 | 1.75 ± 0.32 |
| Fit: ∫ (K km/s) | 4.63 ± 0.21 | 1.14 ± 0.18 |

The fit integrals give R = 4.06 ± 0.68, hence τ(C¹⁸O) = 0.22 (0.16 to 0.31)
and τ(¹³CO) = 1.6 (1.2 to 2.3) for X = 7.4. The Moment 0 means give
R = 4.4 ± 1.4 (error from the channel σ, 10 channels and a 15″ beam of 4.4
pixels). The broader ¹³CO line fits an optically thick ¹³CO. Moment 0 equals
the channel sum times 0.5 km/s computed with numpy, to 3 × 10⁻⁷ K km/s.

In the PV diagram the filament runs from about 30″ to 220″ along the cut and
peaks at 2.3 K. On the diagram saved as FITS its centroid stays between 8.8
and 9.4 km/s up to 190″, then reaches 9.7 km/s where it fades. C¹⁸O does not
detect the filament (Moment 0 mean 0.74 K km/s, about 2σ).

## Caveats

- Divide T_A* by η_mb = 0.72 for main-beam temperature (Rigby et al. 2016);
  `ETAL = 0.85` in the header is another efficiency.
- With no `BUNIT` the app labels Moment 0 *km/s* and writes that unit into
  the exported FITS. Read it as K km/s.
- The masked Moment 2 (0.59 km/s) is below the fitted σ (1.10 km/s) because
  the 3σ mask drops the line wings. Take widths from the fit.
- *Fit Gaussian* fits the whole 200 km/s spectrum; on the filament in C¹⁸O it
  lands on a noise spike at 149 km/s. Keep a fit only if its centre is on the
  line.
- *Combine ▸ Pixel Math / Spectral Index…* (*ratio*) reprojects the C¹⁸O map
  onto the ¹³CO grid, but the ratio map is noise wherever C¹⁸O is faint. With
  the demo C¹⁸O cube it returns an empty map and no warning.
- The app gives no excitation temperature, column density or mass. Rigby et
  al. (2019) derived them in LTE, voxel by voxel, with the excitation
  temperature from ¹²CO 3→2.
- The clump's distance and its CHIMPS catalogue entry were not checked.

## Next

- [Moment maps and PV diagrams](03-moments-and-pv.md) and
  [Regions](04-regions.md).
- [VLKB archive](../reference/vlkb-archive.md): inventory and cutouts.

## References

- Rigby A. J., Moore T. J. T., Plume R., et al., 2016, MNRAS, 456, 2885.
- Rigby A. J., Moore T. J. T., Eden D. J., et al., 2019, A&A, 632, A58.
- Wilson T. L., Rood R. T., 1994, ARA&A, 32, 191.
