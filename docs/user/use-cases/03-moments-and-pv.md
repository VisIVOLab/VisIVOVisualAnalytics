# Moment maps and position-velocity diagrams

You will collapse an HI cube into maps of intensity, velocity and dispersion,
then cut a position-velocity diagram along the galaxy's major axis.

## You need

- The cube `WALLABY.fits` from the demo data: the WALLABY galaxy
  J100903-290239, 196 × 280 pixels and 109 channels, with an optical
  velocity axis in m/s, so the velocity maps come out in km/s.
- The basics of the cube window from [First steps](01-first-steps.md).

## Steps

### Moment maps

1. Open `WALLABY.fits` with *File ▸ Open…*. Step through the channels once to
   see which ones hold the galaxy's emission; the channel bar shows each
   channel's velocity.

2. Optional: measure the noise first with *Tools ▸ Statistics ▸ Estimate
   Noise...* on a line-free part of the cube. The moment dialog then offers a
   threshold of 3σ of that measurement.

3. Choose *Tools ▸ Maps ▸ Compute Moment…*. The *Moment Map Settings* dialog
   opens. It does not block the window, so you can keep looking at the cube
   while it computes.

4. Set the dialog:
   - *Moment order*: *Moment 0: Integrated intensity (M0)*.
   - *Channel range*: the first and last channel with emission. Leaving out
     line-free channels keeps their noise out of the map.
   - Leave *Enable threshold mask* on. With no noise estimate, *Auto
     threshold (≈ 3σ from MAD)* is chosen; with one, *3σ of the noise
     estimate*.

   ```{figure} img/03-moments-and-pv-1.png
   :alt: The Moment Map Settings dialog with Moment 0 chosen, a channel range set and the threshold mask on.
   :width: 70%

   The dialog remembers the channel range and mask for this cube.
   ```

5. Press *Compute*. The map replaces the channel in the *2D Slice* pane and
   appears in *Session Data*. The status bar sums it up: order, unit, channel
   range and mask.

6. Compute *Moment 1: Intensity-weighted coordinate (M1)* and *Moment 2:
   Spectral dispersion σ (M2)* the same way. Each is a new row in *Session
   Data*.

7. Choose *Four panes* in the pane toggle and double-click the three moment
   rows in *Session Data* to place them. Keep the *3D View* in the fourth
   pane.

   ```{figure} img/03-moments-and-pv-2.png
   :alt: Four panes with the 3-D view and the moment 0, moment 1 and moment 2 maps of the WALLABY galaxy.
   :width: 100%

   M0, M1 and M2 of the same galaxy, each with its own colour map.
   ```

### A PV diagram along the major axis

8. Click in the moment 1 pane so that it is the active pane. Tools work in
   the active pane.

9. Choose *Tools ▸ Spectral ▸ Extract PV Diagram…*. The cursor becomes a
   cross. Click on one side of the disc, then on the other, along the line
   where the velocity changes fastest (from the reddest to the bluest part of
   the map). Double-click or right-click to finish the path.

10. The diagram is computed on the backend at full resolution and opens in a
    pane. A pane that was showing the channels becomes *Channels*: it follows
    the channel bar and shows the path too.

    ```{figure} img/03-moments-and-pv-3.png
    :alt: The PV path drawn across the moment 1 map and the resulting PV diagram in the next pane.
    :width: 100%

    The path on the velocity field and the diagram it gives.
    ```

11. Adjust the cut while *Extract PV Diagram* is on. Drag a handle to move a
    vertex, drag the path to move it, Shift-drag to rotate it, double-click
    the path to add a vertex, right-click a vertex to remove it. The diagram
    follows every change.

12. In the diagram's footer, *Width* averages a band across the path (in cube
    pixels). *Smooth curve* follows a curved path through the vertices, for a
    warp or a spiral arm. *New path* starts over.

13. Save. *Save as FITS…* writes the diagram to the Workspace Exports, with
    the offset and the cube's own spectral axis in the header and every
    vertex in `HISTORY`. *Save as PNG…* saves the picture. To keep a moment
    map, click its pane and choose *Tools ▸ Export*; the entry carries the
    map's name (*Export … as FITS…*).

14. To measure the map itself, choose *Tools ▸ Regions ▸ Box Region Analysis*
    and drag a box on the *Moment 0* pane. The panes you can draw in get an
    amber border. *Analyse* (right-click the region) gives the statistics of
    the moment map, and the Inspector names the map it measured. The region
    joins the window's region list, so it shows on the slice and on the other
    map panes too, and moving it in one pane moves it everywhere.

15. With *Tools ▸ Views ▸ Link Views* on, the 2-D slice and the map panes
    share pan and zoom: zoom into the galaxy on one pane and the others
    follow. *Shift+R* on any of them resets them all.

## What you get

The moment 0 map is the integrated intensity, in the cube's unit times the
spectral unit (the status bar names it): where the gas is and how much. Moment 1 is the
intensity-weighted mean velocity, drawn with a diverging colour map: a
rotating disc shows a gradient from one side to the other, and the direction
of the gradient gives the kinematic major axis. Moment 2 is the velocity
dispersion. It is not the line's FWHM; for that, use *Tools ▸ Spectral ▸
Line-width Map…*.

The threshold mask keeps noise pixels out of M1 and M2, where they would
give velocities far outside the band. VisIVO Visual Analytics also blanks any
M1 value outside the cube's velocity range, even without a mask.

In the PV diagram the horizontal axis is the offset along the path in
arcseconds, starting at 0 on the first vertex, and the vertical axis is the
velocity. A rotating disc gives an S-shaped curve: the flat parts at both
ends are the rotation speed seen at the galaxy's inclination, and the centre
crosses the systemic velocity. Asymmetries between the two arms point to a
warp or to gas outside the disc.

## Next

- [Moment maps](../reference/moment-maps.md): all the orders, how to
  choose channels and thresholds, and common pitfalls.
- [Regions, PV diagrams, noise](../reference/region-pv-noise.md): the PV
  diagram in detail and the noise estimate.
- [Spectral analysis](../reference/spectral-tools.md): line-width maps
  and baseline subtraction.
