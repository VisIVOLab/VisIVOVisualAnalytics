# Find and measure a source

You will find the sources in an HI cube with SoFiA-2, select one of them in
3-D with the Kinematic Lasso, and read its flux, line widths and systemic
velocity.

## You need

- The HCG 44 cube `hgc44.fits` from the demo data: 700 × 700 pixels and 158
  channels around the galaxy group HCG 44, with a heliocentric velocity axis.
- SoFiA-2 (version 2.7) installed where the backend runs. The suggestion of
  parameters works without it; the run needs it. The lasso and the
  measurements do not need SoFiA-2.
- The basics of the cube window from [First steps](01-first-steps.md).

## Steps

### Find the sources with SoFiA-2

1. Open `hgc44.fits` with *File ▸ Open…*. The cube is larger than the
   viewer's full-resolution limit, so it opens as a preview and a banner
   offers *Load full resolution*. Press it: the cube is small enough for your
   machine, and the lasso then works on every voxel.

2. Choose *Tools ▸ Sources & Kinematics ▸ Source Finding (SoFiA-2)…*. The
   *SoFiA-2 Source Finding* dialog asks the backend to measure the cube, which
   takes about a second. It then shows what it deduced (*Data type*: emission;
   *Line widths*: HI), the measurements (noise, beam in pixels, channel width)
   and a table of suggested parameters, each with the reason for its value
   under *Why*.

   ```{figure} img/02-find-and-measure-a-source-1.png
   :alt: The SoFiA-2 Source Finding dialog with the deduced data type, the measurements and the parameter table.
   :width: 100%

   The suggested parameters, each with the reason for its value.
   ```

3. If a line reads *Please confirm…*, pick the value or press *Confirm*. You
   can edit any value by double-clicking it; *Reset to suggested* undoes your
   edits.

4. Press *Run SoFiA-2*. A bar shows the run; *Cancel run* stops it. At the end
   the dialog lists the detected sources and SoFiA's own counts. The run is
   recorded in *Session Data* as *SoFiA-2 · N sources* with the time.

5. Press *Open mask in 3D* to see the detections as a cube of labels. It
   opens with *MIP* blending, so each detection shows as a solid blob.

### Select one source with the Kinematic Lasso

6. Back in the HCG 44 window, show the *3D View* in a pane and choose
   *Tools ▸ Sources & Kinematics ▸ Kinematic Lasso (click to select)*. The
   Inspector's *Parameters* tab opens on the lasso's controls.

7. Rotate the cube until NGC 3187 (the spiral with the long tidal tail) is
   not hidden behind another galaxy, then double-click it. A yellow marker
   shows where you clicked; the selection appears as a green surface, and its
   outline is drawn on the 2-D slice as you step through the channels. A
   single drag still rotates the view.

   ```{figure} img/02-find-and-measure-a-source-2.png
   :alt: 3-D view of HCG 44 with one galaxy selected as a green surface and the lasso controls in the Inspector.
   :width: 100%

   One double-click selects the whole connected object.
   ```

8. Tune the selection in the Inspector:
   - *Reach* decides how far into the faint outskirts the selection may go.
     Drag it and watch the surface; the default of 8 suits compact sources,
     an extended disc may need 30 or more.
   - *Velocity coupling* below 1 lets the selection follow the rotation
     across channels; above 1 keeps two objects at different velocities
     apart.
   - *Threshold* (default 2.5σ) and *Smoothing* decide which faint emission
     counts as connected.

   The panel shows the voxel count (*N voxels selected*) and the selected
   pixels per channel.

9. Shift-double-click adds a second source; Option-double-click (Alt on
   Linux and Windows) carves a region out. For a foreground artefact, use
   *Refine on 2-D map…* and draw a polygon on a channel with *Add* or
   *Subtract*. Do this last: moving *Reach* again discards hand refinements.

10. Press *Isolate → new cube* to write a cube with everything outside the
    selection blanked. It keeps the full size and WCS of the original, appears
    in *Session Data*, and records the click and every parameter.

### Measure it

11. With the selection still on screen, choose *Tools ▸ Sources & Kinematics
    ▸ Kinematic Model Overlay…*. In the *Tilted-Ring Kinematic Model* dialog,
    press *Fit from the lasso selection*.

12. The fit adds rows to *Session Data*: the selection's velocity field, the
    model and residual maps, the integrated spectrum, and *Source properties*
    with the flux. Click *Source properties* to read the numbers in the
    Inspector.

    ```{figure} img/02-find-and-measure-a-source-3.png
    :alt: Session Data with the Source properties row selected and its flux, w20, w50 and systemic velocity in the Inspector.
    :width: 100%

    The measurements of the selection, with the parameters they came from.
    ```

## What you get

*Flux* is the integrated HI flux of the selection in Jy km/s. With the
distance to the group it gives the HI mass. *w20 / w50* are the widths of the
integrated spectrum at 20 % and 50 % of its peak, in km/s; *V_sys (w20
midpoint)* is the systemic velocity, halfway between the two w20 edges. The
position angle and the axis ratio come from the moment-0 map of the
selection, and the inclination from the axis ratio.

SoFiA-2 and the lasso answer different questions. SoFiA-2 searches the whole
cube and finds sources you did not know about. The lasso starts from a source
you can see and decides where it ends, and it does not find faint sources
that only show after smoothing. Fluxes of the same galaxy from the two masks
usually differ by a few per cent, because the masks contain different voxels.

When you publish a number from the lasso, state the Reach you used. Every
product's *Parameters* tab lists what made it.

## Next

- [Kinematic Lasso](../reference/kinematic-lasso.md): the controls, what
  the lasso is good at, and the selection shared with the headset.
- [Source finding](../reference/source-finding.md): the SoFiA-2 dialog,
  the suggested parameters and troubleshooting.
- [Kinematic model](../../technical/design/kinematic-model.md): how the geometry and the
  widths are derived.
