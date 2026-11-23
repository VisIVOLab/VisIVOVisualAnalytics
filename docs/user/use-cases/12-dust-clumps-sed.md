# The far-infrared SED of dense dust clumps

You will overlay the compact sources of Hi-GAL (Molinari et al. 2010) on a
Herschel 250 µm image, fit a greybody to the spectral energy distribution
(SED) of three clumps, and compare temperature, mass and luminosity with the
Hi-GAL catalogue of clump properties.

## You need

- The SPIRE 250 µm cutout
  `vlkb-cutout_2023-09-08_04-44-11_833602_continuum-higal_mosaic_mosaic_wgls_m030039_PSW.fits`
  from the demo data: 180 × 181 pixels of 6″ in Galactic CAR projection over
  l = 39.85° to 40.15°, b = −0.15° to +0.15°, in MJy/sr.
- Network access, for the VLKB catalogue.
- The distances of the clumps, from the catalogue of Elia et al. (2021)
  (table `higal360clump` at IRSA), which takes them from Mège et al. (2021):

  | Clump (Elia et al. 2021) | VLKB band-merged source | Distance |
  |---|---|---|
  | HIGALBM39.9345+0.1325 | HIGAL_FT_39.935089+0.135547 | 11.85 kpc |
  | HIGALBM40.1051-0.0520 | HIGAL_FT_40.104907-0.051594 | 12.10 kpc |
  | HIGALBM39.9398-0.1493 | HIGAL_FT_39.940121-0.147913 | 12.63 kpc |

## Steps

1. Open the cutout with *File ▸ Open…*.

2. Choose *Tools ▸ Catalogue ▸ Overlay VLKB Compact Sources* and click once
   on the image to query the whole footprint. The ellipses of 70 band-merged
   sources appear, with a table under the image: one parent row per source,
   its per-band detections as children. *Inspector ▸ Parameters* has a check
   box per band.

   ```{figure} img/12-dust-clumps-sed-1.png
   :alt: The 250 micron cutout with the white ellipses of the VLKB band-merged sources, the source HIGAL_FT_39.935089+0.135547 selected and expanded in the catalogue table, and the band check boxes in the Inspector.
   :width: 100%

   The VLKB compact sources over the cutout. The selected source is the first clump.
   ```

3. Right-click the row *HIGAL_FT_39.935089+0.135547 (Band-merged)* and choose
   *Fit SED of this source…*. The *Photometry* tab lists every measurement of
   the source in wavelength order: 2MASS J, H and Ks, WISE 3.4, 4.6 and 12 µm,
   MSX 8.28 and 12.13 µm, then Hi-GAL at 22, 24, 70, 160, 250, 350 and 500 µm.
   Where a band has several counterparts the brightest is used and the others
   are greyed alternatives.

4. Only the four rows at 160, 250, 350 and 500 µm are ticked, because *Fit λ ≥*
   starts at 160 µm. Elia et al. (2021) fit one greybody to λ ≥ 160 µm, since
   the 70 µm and mid-infrared flux of a protostellar clump comes from warmer
   dust. To try another limit, type it in *Fit λ ≥* and press *Tick these
   bands*; 0 ticks every band. A flux or error can be edited by double-click.

5. In the *Fit* tab keep *Optically thin*, *κ(λ_ref)* 0.1 cm²/g at *λ_ref*
   300 µm (gas and dust, as in the catalogue), β fixed at 2 and *Apply PACS
   colour corrections*. Set *Distance* to 11850 pc; the window always starts
   at 1000 pc. Press *Fit greybody* and read the *Results* tab: *Mass* 652 ±
   14 M☉, *Temperature* 18.5 ± 0.3 K, *L, greybody only* 2540 L☉, *L_bol,
   observed + greybody* 4278 L☉, χ² 6.9 for 2 degrees of freedom.

   The *L_bol* row adds the observed fluxes from 20 to 160 µm to the greybody
   beyond 160 µm, as Elia et al. (2017) do for a clump with a 70 µm detection.
   It appears only for sources that have one.

6. Repeat for the other two clumps with their distances.

## What you get

| Clump | Class | T (K) app / catalogue | M (M☉) app / catalogue | L (L☉) app / catalogue | χ²/dof |
|---|---|---|---|---|---|
| HIGALBM39.9345+0.1325 | protostellar | 18.5 ± 0.3 / 20.0 ± 0.2 | 652 ± 14 / 477 | 4278 / 4246 | 3.4 |
| HIGALBM40.1051-0.0520 | protostellar | 16.9 ± 0.1 / 21.4 ± 0.6 | 534 ± 9 / 179 | 3510 / 3437 | 14.2 |
| HIGALBM39.9398-0.1493 | pre-stellar | 11.6 ± 0.5 / 12.1 ± 0.9 | 1561 ± 99 / 1218 | 374 / 366 | 4.1 |

L is *L_bol, observed + greybody* for the two protostellar clumps. The
pre-stellar clump has no 70 µm detection and so no such row; its L is
*L, greybody only*. The greybody alone gives 2540 and 1183 L☉ for the first
two, well below the catalogue.

The app finds the clumps colder and heavier because it fits different 350 and
500 µm fluxes. The catalogue scales those two bands to the source size
measured at 250 µm, so that every band samples the same gas (Elia et al. 2017;
Giannini et al. 2012). The VLKB rows carry the unscaled fluxes: for
HIGALBM40.1051-0.0520, 2.294 Jy at 500 µm against 1.003 Jy scaled.

To do the same, tick *Scale 350/500 µm to the 250 µm source size* at the top
of the *Photometry* tab, then press *Fit greybody* again at the same
distance. The log under the results gives the factors: for the first clump,
350 µm × 0.892 and 500 µm × 0.811 at a 250 µm size of 21.9″.

| Clump | T (K) | M (M☉) | L (L☉) | χ²/dof |
|---|---|---|---|---|
| HIGALBM39.9345+0.1325 | 19.9 ± 0.1 | 476 ± 10 | 4235 | 3.0 |
| HIGALBM40.1051-0.0520 | 21.2 ± 0.3 | 185 ± 3 | 3404 | 14.4 |
| HIGALBM39.9398-0.1493 | 12.1 ± 0.4 | 1199 ± 75 | 365 | 2.1 |

Temperatures now agree with the catalogue within 0.2 K and masses within 4 %.
The luminosities are within 1 % of the catalogue.

<!-- Screenshot pending (see SHOTLIST.md): img/12-dust-clumps-sed-2.png -->
<!-- Screenshot pending (see SHOTLIST.md): img/12-dust-clumps-sed-3.png -->
<!-- Screenshot pending (see SHOTLIST.md): img/12-dust-clumps-sed-4.png -->

## Caveats

- Mass and luminosity scale as d²: a distance 20 % too large raises both by
  44 %. The ± values here are Δχ² = 1 intervals on the catalogue flux errors
  and leave the distance out. The catalogue quotes a much larger mass error
  for the first clump (495 M☉).
- Reduced χ² of 2 to 14 means the flux errors are too small for a single
  temperature; read the ± values as lower limits.
- β trades off against temperature. With β free from 1 to 3 (21 steps) and the
  unscaled fluxes, the first clump gives β = 1.53 ± 0.05, 22.6 K and 370 M☉.
  Keep β = 2 to compare with the catalogue.
- The fluxes are catalogue photometry, 2-D Gaussian fits over a local
  background (Molinari et al. 2011, 2016), not sums over the cutout's
  pixels. A region on the image would include the background.
- The VLKB table has its own `distance` column. The column says pc but holds
  kpc, and it is empty for these three clumps. When a row has a value, the
  *Use catalogue (X kpc)* button next to *Distance* copies it in, converted to
  pc. It is never applied on its own, because these values differ from the
  catalogue of Elia et al. (2021): HIGAL_FT_40.018313-0.115227 has 6.51 kpc
  in the VLKB, while Elia et al. (2021) give no distance and list its mass and
  luminosity at 1 kpc as negative numbers.
- The demo data has no 70 or 160 µm image, and the fit does not need one,
  since the catalogue rows carry the PACS fluxes. The *VLKB* tab at glon 40,
  glat 0 lists Hi-GAL 70 and 160 µm tiles of this area and a 160 µm mosaic.

## Next

- [SED fitting](../reference/sed-fitting.md): the models, branches and log
  messages.
- [Catalogues, HiPS and cross-match](06-catalogues-hips-crossmatch.md) and
  the [VLKB archive](../reference/vlkb-archive.md).

## References

- Molinari S., Swinyard B., Bally J., et al., 2010, PASP, 122, 314.
- Molinari S., Schisano E., Faustini F., et al., 2011, A&A, 530, A133.
- Molinari S., Schisano E., Elia D., et al., 2016, A&A, 591, A149.
- Giannini T., Elia D., Lorenzetti D., et al., 2012, A&A, 539, A156.
- Elia D., Molinari S., Schisano E., et al., 2017, MNRAS, 471, 100.
- Elia D., Merello M., Molinari S., et al., 2021, MNRAS, 504, 2742.
- Mège P., Russeil D., Zavagno A., et al., 2021, A&A, 646, A74.
