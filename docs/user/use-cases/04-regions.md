# Regions: draw, place at catalogue coordinates, import and export

You will draw regions on a cube, place one exactly on a galaxy from its
catalogue coordinates, import a ds9 region file and write your regions back
out for ds9, CARTA or CASA.

## You need

- The HCG 44 cube `hgc44.fits` from the demo data.
- The ICRS position of a source from a catalogue. This page uses NGC 3190 in
  HCG 44, at RA 10:18:05.6, Dec +21:49:56 (NED gives this to a fraction of an
  arcsecond).
- A ds9 region file. To follow along, save these lines as `hcg44.reg`:

  ```text
  # Region file format: DS9 version 4.1
  global color=green
  fk5
  circle(10:18:05.6,+21:49:56,90") # text={NGC 3190}
  ellipse(10:18:24.9,+21:53:38,60",40",30) # color=cyan text={NGC 3193}
  annulus(10:17:47.8,+21:52:24,60",120") # text={NGC 3187 background}
  point(10:17:38.6,+21:41:18) # point=cross text={NGC 3185}
  panda(10:18:05.6,+21:49:56,0,360,4,30",90",2)
  ```

## Steps

### Draw and edit

1. Open `hgc44.fits` and show the *2D Slice* in a pane, with the *3D View* in
   the other (*Two panes*).

2. Choose *Tools ▸ Regions ▸ Circle Region Analysis*. Click on a galaxy and
   drag out the radius. When you release, the region's mean spectrum opens in
   a pane and the region stays on the slice. The other shapes are *Box Region
   Analysis*, *Polygon Region Analysis* (click the vertices, double-click to
   close) and *Annulus Region Analysis* (drag the outer radius, then type the
   inner one).

3. Edit the region. Click selects it (it is drawn thicker, with white
   handles), a drag moves it, a handle resizes it. Delete or Backspace removes
   the selected region, Cmd+Z (Ctrl+Z) undoes the last change and
   Shift+Cmd+Z redoes it. Right-click a region for *Properties…*, *Analyse*
   and *Delete*. Regions stay while you change channel.

   ```{figure} img/04-regions-1.png
   :alt: A selected circle region with its handles on the HCG 44 slice, and its mean spectrum in the next pane.
   :width: 100%

   A selected region and its mean spectrum over all channels.
   ```

### Place a region at catalogue coordinates

4. Double-click the region, or choose *Tools ▸ Regions ▸ Region Properties…*.

5. In *Region Properties*, set *Frame* to *ICRS, sexagesimal*. Type the
   centre: RA `10:18:05.6`, Dec `+21:49:56`. Set the radius in arcseconds,
   for example 90.

6. Press *Apply*. The region moves to NGC 3190. The backend converts the
   position with the cube's full WCS, so it lands on the pixel ds9 would put
   it on.

   ```{figure} img/04-regions-2.png
   :alt: The Region Properties dialog in ICRS sexagesimal with the coordinates of NGC 3190, and the circle on the galaxy.
   :width: 100%

   Coordinates typed from a catalogue place the region on the source.
   ```

7. Choose *Tools ▸ Regions ▸ Analyse Region* to measure the moved region. Its
   mean spectrum and its statistics (valid pixels, minimum, maximum, mean,
   median, standard deviation) are a new product in *Session Data*.

### Import a region file

8. Choose *Tools ▸ Regions ▸ Import Region (CRTF/DS9)…* and pick `hcg44.reg`.
   Every region of the file joins the list as one change, so a single undo
   takes the whole import back. Colours and `text={…}` labels are kept.

9. The lines that did not become regions are listed in a *Region Import*
   window next to the viewer, with the line number, the text and the reason.
   Here the `panda` line is listed because that shape is not supported. The
   status bar says how many regions were read and how many lines were not.

   ```{figure} img/04-regions-3.png
   :alt: The imported regions with their labels on the slice, and the Region Import report listing the panda line and its reason.
   :width: 100%

   Imported regions, and the report of the line that was not read.
   ```

### Regions on the 3-D cutting plane

10. Look at the *3D View*: every region of the list is drawn on the cutting
    plane, in its colour, at the channel shown. Rotate the view or move the
    channel bar and the regions move with the plane. You edit regions on the
    slice; the plane shows them. Unticking *Cutting plane* in the *LAYERS*
    section hides the plane and its regions.

    ```{figure} img/04-regions-4.png
    :alt: The 3-D view of HCG 44 with the imported regions drawn on the cutting plane.
    :width: 100%

    The same regions on the cutting plane in the 3-D view.
    ```

    The regions show on the plane with *Remote* rendering too (the 3-D view
    drawn by the backend), placed and hidden behind the volume as in *Local*
    rendering.

### Export

11. Choose *Tools ▸ Regions ▸ Export Region (CRTF/DS9)…*. Under *Regions to
    write:* choose all the regions or the selected one. Under *Format and
    coordinate frame:* choose, for example, *DS9, sky (FK5)* for ds9 or
    *CASA CRTF, sky (FK5)* for CASA and CARTA. Pick a file name and save.

## What you get

Each region you draw, type or import is kept at sub-pixel precision in the
cube's full-resolution pixels, so it does not drift when the view changes
from preview to full resolution. A region's spectrum is the mean over the
region in every channel; its statistics are those of the channel on which you
drew it. An annulus measures the ring between its two radii, which is the
shape for the background around a source: subtract its mean, times the
aperture's pixel count, from the aperture's sum.

The selected region is also where several tools start: *Estimate Noise*,
*Export Sub-Cube*, *Mask 3-D Region*, *Refine Selected Box* and *RM
Synthesis* take its bounding box.

The image viewer has the same region list, the same *Region Properties* and
the same import and export, so the steps above work on a 2-D image too.

The list is also drawn on every pane that shows a map of the cube's sky
plane: moment maps, line-width maps, the *Channels* pane and the Faraday maps
of an RM synthesis box. You can draw on those panes as on the slice; *Analyse*
then measures the map in that pane.

## Next

- [Regions, PV diagrams, noise](../reference/region-pv-noise.md): every
  shape, gesture and statistic, and the import rules.
- [Image viewer](../reference/image-viewer.md): regions on 2-D images and
  the region analysis window.
- [Working with TOPCAT, Aladin and DS9](../reference/interoperability.md):
  sending images, catalogues and sky positions over SAMP.
