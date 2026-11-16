# Polarimetry: RM synthesis

You will load the Stokes Q, U and V cubes that go with a Stokes I cube, make
maps of polarised intensity and angle, and run rotation measure synthesis to
get a Faraday depth map.

## You need

- The four demo cubes in `polarimetry/`: `DEMO_I.fits`, `DEMO_Q.fits`,
  `DEMO_U.fits` and `DEMO_V.fits`, each 512 × 512 pixels and 16 planes. They
  are synthetic. They copy the layout of real MeerKAT releases, including
  their pitfalls, and the Faraday depths in them are known, so you can check
  the result.
- For real data, the MeerKAT Milky Way Bulge field G000.0+2.5 (Cotton et al.
  2025) in `meerkat_bulge/`: `G000.0+2.5_I.fits`, `G000.0+2.5_Q.fits` and
  `G000.0+2.5_U.fits`, each 7500 × 7500 pixels and 16 planes (3.4 GB), and
  the polarisation catalogue `Mosaic_Pol_Cat.csv`.

## Steps

1. Open `DEMO_I.fits` with *File ▸ Open…*. It opens in the cube viewer.

2. Choose *Tools ▸ Polarimetry ▸ Load Stokes Q/U/V Companions…*. The viewer
   looks next to the file on the backend for the Q, U and V files by name and
   finds the three demo files. A file it cannot find by name, you pick
   yourself. The polarimetry tools are enabled once Q and U are loaded.

3. Move the channel bar to a sub-band: the first two planes of these files
   are fitted planes (total flux and spectral index), not sub-bands. Choose
   *Tools ▸ Polarimetry ▸ Polarised Intensity & Angle (current channel)*. Two
   maps appear in *Session Data*, *Polarised intensity (ch N)* and
   *Polarisation angle (ch N)*. Double-click one to show it in a pane.

   ```{figure} img/07-polarimetry-1.png
   :alt: The DEMO_I cube with the polarised intensity and polarisation angle maps of one channel in two panes.
   :width: 100%

   Polarised intensity and angle for one sub-band.
   ```

4. Choose *Tools ▸ Polarimetry ▸ Rotation Measure Synthesis…*. Set *φ min* to
   -80, *φ max* to 80 and *φ step* to 1 (in rad/m²). The dialog shows how
   many samples that gives (161), over how many pixels, and how large the
   full dispersion cube would be. If a region is drawn on the slice, *Restrict
   to the drawn region* limits the computation to it. Press *OK*.

   ```{figure} img/07-polarimetry-2.png
   :alt: The Rotation Measure Synthesis dialog with phi from -80 to 80 in steps of 1 and the sample count.
   :width: 60%

   The Faraday depth range and step.
   ```

5. Three maps come back and are listed in *Session Data*: *Peak |F(φ)|*,
   *Faraday depth (RM)* and *Faraday depth (peak |F| > 6σ)*. Double-click the
   masked Faraday depth map to show it in a pane, next to the peak map. Its
   colour map is centred on zero.

   ```{figure} img/07-polarimetry-3.png
   :alt: The Faraday depth map of the synthetic field, with a left-to-right gradient and two compact blobs, next to the peak polarised intensity map.
   :width: 100%

   Faraday depth and peak polarised intensity of the synthetic field.
   ```

6. Select a map in *Session Data* to see in the Inspector how it was made:
   the φ grid, the channels used, and which file supplied Q and U.

7. To keep a map, click its pane and choose *Tools ▸ Export*. The entry is
   named after the map, for example *Export Faraday depth (RM) as FITS…*. It
   writes a 2-D FITS file to the Workspace Exports, with the sky coordinates
   of the cube, the map's own unit (`rad m-2`) and the parameters as
   `HISTORY`.

8. Repeat with the real data. Open `meerkat_bulge/G000.0+2.5_I.fits` and load
   its companions the same way. Each cube is 3.4 GB, so the viewer shows a
   preview and the backend does the arithmetic on the full files; Q and U are
   never both in the viewer's memory. Draw a box on the slice and tick
   *Restrict to the drawn region* to start with a smaller area.

9. To measure the rotation measure over part of the field, choose *Tools ▸
   Regions ▸ Box Region Analysis* and drag a box on the *Faraday depth (RM)*
   pane. *Analyse* (right-click the region) gives the mean, median and
   spread of the map inside the box; the Inspector names the map it
   measured.

## What you get

*Peak |F(φ)|* tells how strongly each pixel is polarised at its strongest
Faraday depth. *Faraday depth (RM)* is the φ where that peak sits: the
rotation measure, in rad/m², drawn with a diverging colour map because its
sign carries the direction of the magnetic field along the line of sight.
Where there is no polarised signal the "depth" is just the largest noise
peak, so read rotation measures from *Faraday depth (peak |F| > 6σ)*, which
keeps only pixels whose peak stands 6σ above the noise of the peak map.

For the synthetic field the masked map shows a gradient from about -30
rad/m² on the left to +30 on the right, a positive blob near pixel (180, 150)
reaching about +36, and a negative one near (330, 340) down to about -47. If
the map is flat or shows other values, the frequencies were read from the
wrong place.

That is the trap these files carry. Their third axis holds two fitted planes
and fourteen sub-bands, whose real frequencies (908 to 1656 MHz) are in
keywords of their own, while the axis in the header describes 1284 to 2147
MHz. VisIVO Visual Analytics reads the sub-band frequencies from the keywords
of the file that supplies Q. Taking them from the axis gives a map that looks
plausible and is wrong: on the Bulge field it gives 63 rad/m² where the true
value is 25.

In G000.0+2.5 two sub-bands, at 1199 and 1256 MHz, carry no data. RM
synthesis leaves them out and records them in the product as *Sub-bands
dropped*. The catalogue `Mosaic_Pol_Cat.csv` lists the rotation measure of
each polarised source in the mosaic, so you can compare your map with it
source by source.

## Next

- [Polarimetry (Stokes Q, U, V)](../reference/polarimetry.md): naming
  rules for the companions, the formulas and the worked example on the Bulge
  field.
- [Image viewer](../reference/image-viewer.md): polarisation on 2-D Stokes
  images.
- [Regions: draw, place at catalogue coordinates, import and export](04-regions.md).
