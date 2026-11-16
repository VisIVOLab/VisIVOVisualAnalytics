# Large cubes and simulations

You will open a 32 GB simulation density, look at it as a quick preview, have
the backend render it at full resolution, and bring out detail where you zoom
in.

## You need

- The density of a PLUTO run: `rho.0000.flt`, a 2048³ grid of single-precision
  values (32 GB), with the run's `flt.out` and `grid.out` in the same folder.
  The values span about seven decades, from 1.8 × 10⁻¹² to 2.1 × 10⁻⁵.
- About 40 GB of free disk on the backend's side, once, for the cube's brick
  pyramid (a set of coarser copies the backend builds the first time it needs
  them).
- A GPU on the machine running the backend. Your own Mac works when you are
  logged in at it; on a cluster, see
  [Working with a backend on a cluster](09-backend-on-a-cluster.md).

## Steps

### Open the density

1. Choose *File ▸ Open…*. In the file browser set *File type* to *All files*:
   PLUTO files are not in the default list. Select `rho.0000.flt` and press
   *Open*.

2. The cube window opens in well under a second. Nothing is copied: the
   backend reads the variable in place in the file. The grid is uniform and
   Cartesian, so the axes are X, Y and Z in the simulation's units. Tools that
   need a spectral axis or sky coordinates, such as moment maps and PV
   diagrams, are not offered, because the third axis is space.

3. The 3-D view shows a preview: a smaller cube made from the full one. The
   status line names it, for example *Preview: every 8th voxel*. A banner
   says the full cube is above the limit for this window and offers *Load
   full resolution* and *Switch to Remote*. Do not load 32 GB into the
   viewer.

4. The density spans many decades, so use a logarithmic colour scale: next to
   *Color map* in the *DISPLAY, 3D* section, press *Edit*, choose the *Log* scale and press *Apply*.

   ```{figure} img/05-large-cubes-and-simulations-1.png
   :alt: The 2048-cubed PLUTO density as a preview in the 3-D view, with the status line naming the preview and the banner offering Load full resolution and Switch to Remote.
   :width: 100%

   The preview of the 32 GB density, a log colour scale, and the banner.
   ```

### Render it on the backend

5. In the 3-D pane's header, the *Auto / Local / Remote* switch (also *View ▸
   Rendering*) decides where the cube is drawn. *Local* draws the preview on
   this machine's GPU. Choose *Remote*, or *Switch to Remote* in the banner.

6. The first time, the backend builds the cube's brick pyramid. The preview
   stays and the banner shows *Preparing the cube for full resolution on the
   backend…* with a percentage. For this cube it takes about a minute on a
   fast disk. When it ends, the remote view appears by itself.

7. The remote view draws the cube from the pyramid, a few blocks at a time:
   fine near where you look, coarse elsewhere.

   ```{figure} img/05-large-cubes-and-simulations-2.png
   :alt: The same density rendered remotely from its brick pyramid, with Remote selected in the pane header.
   :width: 100%

   The full-resolution view rendered by the backend.
   ```

### Level of detail on your own GPU

8. Switch back to *Local*. Now that the pyramid exists, the preview is made
   of block means instead of every Nth voxel, so thin filaments do not flicker
   or vanish. The status line says so: *Preview: block means, pyramid level
   3*.

9. Zoom in on a dense region and stop. About 200 ms after the camera stops,
   the viewer reads the part in view at a finer level and swaps it in once it
   has arrived. The status line then names the detail, for example *Detail:
   level 0 in x 512..1023, y 512..1023, z 768..1023; elsewhere block means,
   pyramid level 3*. Zoom out and it goes back to the plain preview.

   ```{figure} img/05-large-cubes-and-simulations-3.png
   :alt: A zoomed 3-D view of the density with the refined part sharper than the preview around it, and the status line naming the level of detail.
   :width: 100%

   The part in view refined to level 0, inside the block-mean preview.
   ```

10. To fix the detail on one box, choose *Tools ▸ Views ▸ Refine Selected
    Box…*. The box starts from the selected region, or from the middle half of
    the cube; an amber box in the volume follows the bounds while you edit
    them (*Z* counts from 0, like X and Y). Press *Refine*. A level-0 box of
    512 × 512 × 256 cells took about 3 seconds from the click on a local
    backend. The box stays whatever the camera does, until *Tools ▸ Views ▸
    Clear Refined Box*.

11. The settings are in *Settings ▸ General ▸ Large Cubes*: *Level of detail*,
    *Refine automatically when the view settles* and the *Detail budget*,
    the memory the fine blocks may use.

## What you get

The preview, the remote view and the refined boxes all show the same data at
different sampling. A block-mean preview voxel is the mean of the block it
stands for, so it keeps the total mass of a block; the strided preview is a
sample and can miss a structure thinner than its step. The status line always
tells you which one you are looking at.

The pyramid is built once per file and reused by every later remote view,
refinement and headset session, also after a restart. Opening the cube never
starts a build by itself.

The same steps work for any large FITS cube and for VisIVO Binary Table
volumes such as `volumesnap63.bin` from the demo data (a 512³ dark-matter
density, which you can also load at full resolution).

## Next

- [PLUTO simulation output](../../user-guide/pluto-output.md): what to open,
  variables and axes.
- [Spectral cube viewer](../../user-guide/cube-viewer.md): the preview, the
  level of detail and Auto / Local / Remote in detail.
- [Simulations](../../user-guide/simulations.md) and
  [Ray-traced volumes](../../user-guide/raytraced-volumes.md).
- [From the desktop to the VR headset](08-vr-from-the-desktop.md): the same
  density in a Meta Quest.
