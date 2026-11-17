# First steps: open an HI cube and explore it

You will open an HI cube, look at it in 3-D, step through its channels and
read the spectrum of any pixel.

## You need

- VisIVO Visual Analytics, installed from a release package and started once,
  so that its backend is set up (see
  [Getting started](../reference/getting-started.md)).
- An HI cube. This page uses `WALLABY.fits` from the demo data: a WALLABY
  cutout of the galaxy J100903-290239 in the Hydra field, 196 × 280 pixels
  and 109 channels, with an optical velocity axis.

## Steps

1. Start VisIVO Visual Analytics. The startup window checks the backend and
   then opens the main window on the *Data Hub* tab.

2. Choose *File ▸ Open…* (⌘O), or *Open…* in the Data Hub. A file browser
   opens on the backend's files. Its title says which backend you are on
   (*Backend: Local* when the app started its own). Select `WALLABY.fits`:
   the *Preview* beside the list shows its header. Press *Open*.

   ```{figure} img/01-first-steps-1.png
   :alt: The Open dialog with WALLABY.fits selected and its FITS header in the preview.
   :width: 100%

   The file browser lists the backend's files and previews the selected header.
   ```

3. The cube opens in a cube window. The centre holds the panes, the left dock
   holds *Session Data* and the display controls, and the Inspector on the
   right has the *Properties*, *Analysis* and *Parameters* tabs. The status
   bar at the bottom shows the data state (*Full resolution* for a cube this
   small), the WCS status and a *Sanity* badge.

4. Use the pane toggle in the toolbar above the panes and choose *Two panes*.
   In each pane, the ▾ menu in the pane header picks what the pane shows: set
   one to *3D View* and the other to *2D Slice*.

   ```{figure} img/01-first-steps-2.png
   :alt: Cube window with the 3-D view on the left and the 2-D slice on the right.
   :width: 100%

   Two panes: the volume rendering of the cube and the current channel.
   ```

5. Explore the 3-D view. Drag to rotate, use the wheel to zoom. The *Camera*
   menu has fixed views (*Front*, *Top*, *Right* and the others). In the left
   dock, the *DISPLAY, 3D* section switches between *Volume* and
   *Isosurface*, sets the *Blend* (*Composite* or *MIP*) and the colour map.
   *MIP* shows the brightest sample along each line of sight and makes the
   galaxy's disc easy to see.

6. Step through the channels with the channel bar under the 2-D slice. It
   shows the channel number and its velocity. The cutting plane in the 3-D
   view moves with it and carries the same picture. To play the channels as
   a movie, use *Play* in the *SLICE ANIMATION* section of the left
   dock, or *View ▸ Play Slices*.

7. To see the volume behind the plane, lower its opacity with the *Opacity*
   slider under *Cutting plane* in the *LAYERS* section, or turn the plane off
   with the *Cutting plane* check box.

8. Read a spectrum. Choose *Tools ▸ Spectral ▸ Extract Spectrum*. A spectrum
   pane opens next to the slice and the cursor on the slice becomes a cross.
   Move over the galaxy: the spectrum follows the cursor, and the pane header
   shows the pixel and *live*.

   ```{figure} img/01-first-steps-3.png
   :alt: The 2-D slice with the probe cross-hair on the galaxy and the live spectrum in the next pane.
   :width: 100%

   The spectrum follows the cursor over the slice.
   ```

9. Click a pixel to freeze its spectrum. The header then reads *pinned*.
   Click again to go back to following the cursor.

10. Keep the spectrum. Choose *Tools ▸ Spectral ▸ Pin Spectrum*, or switch
    *Extract Spectrum* off (or press Esc) while a spectrum is frozen. It is
    saved as a spectrum product in *Session Data*, with its pixel recorded in
    the Inspector's *Parameters* tab.

11. You can also pick a spectrum from the 3-D view. Choose *Tools ▸ Spectral
    ▸ Pick Spectrum on Plane Click* (or *Pick spectrum on cutting plane* in
    the *INTERACTION* section), then click the rendered emission. The 3-D
    pane gets an amber border while the picker is armed.

## What you get

The spectrum pane shows the flux density in the cube's unit (Jy/beam here)
against velocity, for one line of sight. A galaxy seen at an angle gives a
double-horned profile; the velocity of each horn tells you how fast the two
sides of the disc move towards or away from you. The stats bar under the plot
gives the minimum, maximum, mean, RMS and integral of the curve.

In the Inspector's spectrum panel, *Compare* keeps the current curve on the
plot so the next one is drawn over it, and *Save CSV…* writes the numbers to
a file.

A pick in the 3-D view depends on the blend mode you look through: with
*MIP* it takes the brightest voxel on the ray, with *Composite* the first
voxel above the threshold. Either way you get the spectrum at that sky
position.

Every product you keep (spectra, maps, selections) is listed under the cube
in *Session Data*. Click a row to see its parameters in the Inspector;
double-click it to show it in a pane.

```{figure} img/01-first-steps-4.png
:alt: Session Data with a saved spectrum selected and its parameters in the Inspector.
:width: 100%

A saved spectrum in Session Data, with the pixel it was taken at.
```

## Next

- [Spectral cube viewer](../reference/cube-viewer.md): every control of the
  cube window.
- [Spectral analysis](../reference/spectral-tools.md): smoothing, line
  lists, Gaussian fits and comparison of spectra.
- [Moment maps and PV diagrams](03-moments-and-pv.md), the next use case.
