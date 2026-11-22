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

<!-- Screenshot pending (see SHOTLIST.md): img/12-dust-clumps-sed-1.png -->

3. Right-click the row *HIGAL_FT_39.935089+0.135547 (Band-merged)* and choose
   *Fit SED of this source…*. The *Photometry* tab lists 12, 22, 24, 34, 46,
   70, 160, 250, 350 and 500 µm, with two greyed alternative counterparts at
   22 µm.

4. Untick *Use* on every row below 160 µm. Elia et al. (2021) fit one
   greybody to λ ≥ 160 µm, because the 70 µm and mid-infrared flux of a
   protostellar clump comes from warmer dust. With every row in, this fit ends
   at χ² = 5111 for 8 degrees of freedom.

5. In the *Fit* tab keep *Optically thin*, *κ(λ_ref)* 0.1 cm²/g at *λ_ref*
   300 µm (gas and dust, as in the catalogue), β fixed at 2 and *Apply PACS
   colour corrections*. Set *Distance* to 11850 pc; the window always starts
   at 1000 pc. Press *Fit greybody* and read the *Results* tab.

<!-- Screenshot pending (see SHOTLIST.md): img/12-dust-clumps-sed-2.png -->
<!-- Screenshot pending (see SHOTLIST.md): img/12-dust-clumps-sed-3.png -->

6. Repeat for the other two clumps with their distances.

## What you get

| Clump | Class | T (K) app / catalogue | M (M☉) app / catalogue | L (L☉) app / catalogue | χ²/dof |
|---|---|---|---|---|---|
| HIGALBM39.9345+0.1325 | protostellar | 18.5 ± 0.3 / 20.0 ± 0.2 | 652 ± 14 / 477 | 2540 / 4246 | 3.4 |
| HIGALBM40.1051-0.0520 | protostellar | 16.9 ± 0.1 / 21.4 ± 0.6 | 534 ± 9 / 179 | 1183 / 3437 | 14.2 |
| HIGALBM39.9398-0.1493 | pre-stellar | 11.6 ± 0.5 / 12.1 ± 0.9 | 1561 ± 99 / 1218 | 374 / 366 | 4.1 |

The app finds the clumps colder and heavier because it fits different 350 and
500 µm fluxes. The catalogue scales those two bands to the source size
measured at 250 µm, so that every band samples the same gas (Elia et al. 2017;
Giannini et al. 2012). The VLKB rows, and so the SED window, carry the
unscaled fluxes: for HIGALBM40.1051-0.0520, 2.294 Jy at 500 µm against
1.003 Jy scaled.

To check this, fit the scaled fluxes. Save these lines as `clumps_scaled.csv`:

```text
NAME,RA,DEC,flux160,err_flux160,flux250,err_flux250,flux350,err_flux350,flux500,err_flux500
HIGALBM39.9345+0.1325,285.94683,6.28971,25.534,1.219,16.89,0.872,6.578,0.281,2.558,0.092
HIGALBM40.1051-0.0520,286.19055,6.35670,12.437,0.37,10.239,0.644,2.944,0.083,1.003,0.038
HIGALBM39.9398-0.1493,286.20121,6.16519,3.291,0.308,5.758,0.599,2.95,0.533,2.192,0.494
```

Load the file with *Tools ▸ Catalogue ▸ Load Catalogue Overlay…*, which
replaces the VLKB overlay; each row becomes a four-point SED. Fit each row as
in steps 3 to 5:

| Clump | T (K) | M (M☉) | L (L☉) |
|---|---|---|---|
| HIGALBM39.9345+0.1325 | 20.0 ± 0.2 | 475 ± 10 | 2890 |
| HIGALBM40.1051-0.0520 | 21.3 ± 0.1 | 180 ± 3 | 1635 |
| HIGALBM39.9398-0.1493 | 12.1 ± 0.4 | 1199 ± 76 | 365 |

Temperatures now agree with the catalogue within 0.1 K and masses within 2 %.

<!-- Screenshot pending (see SHOTLIST.md): img/12-dust-clumps-sed-4.png -->

The luminosity of the pre-stellar clump agrees within 0.4 %, while the two
protostellar clumps stay 32 % and 52 % below the catalogue. The app
integrates the fitted greybody from 5 to 2000 µm. For a protostellar clump,
Elia et al. (2021) add the observed 70 µm and mid-infrared fluxes to the
greybody beyond 160 µm, which counts the warm dust a single cold greybody
leaves out.

## Caveats

- Mass and luminosity scale as d²: a distance 20 % too large raises both by
  44 %. The ± values here are Δχ² = 1 intervals on the catalogue flux errors
  and leave the distance out. The catalogue quotes a much larger mass error
  for the first clump (495 M☉).
- Reduced χ² of 3 to 14 means the flux errors are too small for a single
  temperature; read the ± values as lower limits.
- β trades off against temperature. With β free from 1 to 3 (21 steps), the
  first clump gives β = 1.53 ± 0.05, 22.6 K and 370 M☉. Keep β = 2 to
  compare with the catalogue.
- The fluxes are catalogue photometry, 2-D Gaussian fits over a local
  background (Molinari et al. 2011, 2016), not sums over the cutout's
  pixels. A region on the image would include the background.
- The VLKB table has its own `distance` column. It is empty for these clumps
  and differs from the catalogue elsewhere: HIGAL_FT_40.018313-0.115227 has
  6.51 kpc there, while Elia et al. (2021) give no distance and list its mass
  and luminosity at 1 kpc as negative numbers.
- The demo data has no 70 or 160 µm image, and the fit does not need one,
  since the catalogue rows carry the PACS fluxes. The *VLKB* tab at glon 40,
  glat 0 lists Hi-GAL 70 and 160 µm tiles of this area and a 160 µm mosaic.
- The rows at 34 and 46 µm hold the WISE 3.4 and 4.6 µm fluxes at the wrong
  wavelength. Leave them unticked.

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
