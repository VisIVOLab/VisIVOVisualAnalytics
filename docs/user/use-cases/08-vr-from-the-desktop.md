# From the desktop to the VR headset

You will send a cube from the desktop to a Meta Quest 3 by typing a short
code, work on it in VR with the hand menu and the tool palette, and then send
a simulation volume to the same headset.

## You need

- A Meta Quest 3 with its controllers, on a network with internet access. No cable, developer mode or extra
  software is needed for pairing by code.
- The HCG 44 cube `hgc44.fits` from the demo data, and for the second part
  the PLUTO density `rho.0000.flt` from
  [Large cubes and simulations](05-large-cubes-and-simulations.md).
- VisIVO Visual Analytics on the desktop. It comes set up for the pairing
  service hosted by INAF; *Settings ▸ Backends ▸ Headset Pairing* shows it,
  with *Restore Pairing Defaults* if it was changed.

## Steps

### Pair the headset

1. Open `hgc44.fits` on the desktop. Choose *Tools ▸ Views ▸ Open in VR
   Headset…*. The *Open in VR Headset* dialog shows a large pairing code, the
   time it has left (five minutes) and a QR code.

   ```{figure} img/08-vr-from-the-desktop-1.png
   :alt: The Open in VR Headset dialog with a pairing code, its countdown and the QR code.
   :width: 70%

   The pairing code and its QR code.
   ```

2. On the Quest, open the Quest Browser at
   `https://vr.visivolab.oact.inaf.it/`, type the code and press *Connect*.
   You can scan the QR code with the headset instead; it opens the page with
   the code filled in. The first time, install the page as an app from the
   Quest Browser's menu.

3. The dialog follows the pairing: *The code was entered: the headset is
   connecting…*, then *Headset connected.* with a *Disconnect* button. If the
   code expires, *New Code* asks for another.

4. The cube loads on the page. Press *Enter VR*. You enter VR once per
   session: further cubes from the desktop go into the same page, in VR.

### Work in VR

5. Hold the cube with a grip: it follows your hand. With both grips you move,
   turn and scale it between your hands. Y resets the view, B undoes the last
   change of the look.

6. Press X on the left controller to open the hand menu. It stands in front of
   you until you press X again. Its tabs are *Display*, *Range*, *Select*,
   *Measure* and *More*; point the right controller's ray at a control and
   pull the trigger. *Display* sets the colour map, the threshold (also in
   multiples of the noise) and the opacity. *More ▸ Guide* explains the
   controls.

   ```{figure} img/08-vr-from-the-desktop-2.png
   :alt: View from the headset with the HCG 44 cube and the hand menu open on the Display tab.
   :width: 100%

   The hand menu, open on the Display tab.
   ```

7. Press A on the right controller to open the tool palette. Its icons are
   grouped by task: *Explore*; *Kinematic Lasso*, *Spectrum probe* and *Box
   region*; *Velocity range* and *Spatial crop*; *Moment* and *PV slice*.
   Point at an icon to read what it does, pull the trigger to pick it. Look
   at your wrists for help on the controls and for the selection's numbers.

   ```{figure} img/08-vr-from-the-desktop-3.png
   :alt: View from the headset with the tool palette open, the Kinematic Lasso icon highlighted and its description below.
   :width: 100%

   The tool palette.
   ```

8. Pick *Kinematic Lasso* and pull the trigger on a galaxy. The selection
   is drawn in the cube, and its window gives the HI measurements: the
   integrated flux with its error, the systemic velocity, W20 and W50.

9. Pick *Spatial crop*. The crop box has a dot on each face: pull the trigger
   on a dot and drag to move that face, or drag the inside to move the whole
   crop. When the crop stays still, the page reads the cropped part again at
   a finer level and draws it in place. The label over the box says so, for
   example *the crop binned 2× (level 1)*.

10. What you make in the headset (spectra, moment maps, PV slices,
    selections) reaches the desktop: *Tools ▸ Views ▸ Headset Activity* lists
    it.

### A simulation volume

11. On the desktop, open `rho.0000.flt`. With the headset still connected,
    choose *Tools ▸ Views ▸ Show on Headset*: the cube goes to the page
    already open, with no new code. If the backend has not built the cube's
    brick pyramid yet, the page shows *Preparing the cube…* with a percentage,
    then the cube.

12. The page treats it as a simulation. The density spans seven decades, so it
    arrives on a log scale; the box has the grid's proportions and its axes
    are X, Y and Z. The default threshold is the 99th percentile. Spectral
    tools (lasso, spectrum, moments, PV) are greyed, with the reason.

    ```{figure} img/08-vr-from-the-desktop-4.png
    :alt: View from the headset with the 2048-cubed PLUTO density on a log scale, the colour bar in values and a spatial crop shown in more detail.
    :width: 100%

    The 2048³ density in VR, with a crop shown in more detail.
    ```

13. For a file with several fields (a PLUTO output with several variables on
    disk, a VBT volume), the last row of the *Display* tab is *Variable*, one
    chip per field. Each field keeps its own look, and the crop stays. Here
    only the density is on disk, so the row is not shown.

<!-- TODO #357: overlay two or three variables of a simulation volume in one view. -->

### Without internet: the USB cable

14. Open *Advanced (USB cable)* in the same dialog. With the Quest in
    developer mode and connected by USB, *Open on Headset* installs and starts
    the VisIVO VR app, which goes straight into VR.

## What you get

The headset and the desktop share one backend session. The headset draws a
reduced copy of the cube (256³ at most by default) and refines the crop from
the brick pyramid; the lasso and the spectrum probe work on the full cube.
The selection's numbers follow SoFiA-2's definitions. Each product made in
VR is saved in the desktop's Workspace Exports with the steps that made it.

## Next

- [Cubes in a VR headset](../../user-guide/vr/index.md) and
  [Quickstart and pairing](../../user-guide/vr/quickstart.md).
- [Controls and the hand menu](../../user-guide/vr/controls.md).
- [Analysis in the headset](../../user-guide/vr/analysis.md) and
  [Point tables, catalogues and velocity fields](../../user-guide/vr/data-types.md).
- [The headset from the desktop](../../user-guide/vr/desktop.md) and
  [Troubleshooting](../../user-guide/vr/troubleshooting.md).
