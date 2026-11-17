# 3-D catalogues, HiPS and cross-match

You will explore a table of 2.3 million points in 3-D, browse an all-sky
survey around a target, and ask an external catalogue which of its sources
lie on an image.

## You need

- `NKATZ_2.3M.tbl` from the demo data: an IPAC table of 2.3 million
  simulation elements with positions `X`, `Y`, `Z` in Mpc, velocities and
  the logarithms of mass, temperature and density.
- `vlkb-cutout_2023-09-08_04-44-11_833602_continuum-higal_mosaic_mosaic_wgls_m030039_PSW.fits`
  from the demo data: a 180 × 181 pixel Hi-GAL cutout at 250 µm in galactic
  coordinates.
- A network connection for the HiPS surveys and the cross-match (the
  backend queries CDS, SIMBAD and VizieR).

## Steps

### A table of points in 3-D

1. Choose *Data ▸ Open 3D Catalogue (CSV)…* (or *File ▸ Open…*) and select
   `NKATZ_2.3M.tbl`. The 3-D catalogue viewer opens. Positions are taken
   from the `X`, `Y` and `Z` columns; the *AXIS MAPPING* section lets you pick
   other columns.

2. A table this size is loaded in pages: the window shows the first page and
   a *Load more…* button with the number of rows left. *Load all* loads every
   row. On a cluster backend that renders on its GPU, the backend can draw
   every row while the window keeps a page for picking.

3. Colour the points by a column. In *VISUAL ENCODING*, choose `LDensity`
   for the colour and a colour map. *Edit* beside the colour map sets the
   range and the scale. A colour bar on the right shows the column's values.

4. Set the shape to *Gaussian splats* in *SOURCE REPRESENTATION*. Splats add
   up their light, so dense knots are bright and thin filaments stay visible.
   The *Scale* slider sets their size.

   ```{figure} img/06-catalogues-hips-crossmatch-1.png
   :alt: The 3-D catalogue viewer with 2.3 million points drawn as Gaussian splats coloured by LDensity, with a colour bar.
   :width: 100%

   A simulated volume as splats, coloured by density.
   ```

5. Click a point to select it. A ring marks it and the *Source Info* page
   shows its row. Esc or a click on empty space clears the selection.

6. Narrow the table with filters. In the Inspector's *Analysis* tab, *Add
   Filter* stacks conditions such as `LDensity > 8`; they are applied on the
   backend and the cloud is rebuilt with the matching rows.

### An all-sky survey

7. Choose *Data ▸ HiPS Viewer…* (also under *Archives* in the Data Hub). The
   viewer opens as a tab of the main window.

8. Press *Browse…*. The *Choose a HiPS Survey* list holds the image surveys
   registered at CDS. Type in *Search:*, for example `herschel 250`, pick a
   survey and press *Open*.

9. Type a name in *Target:* (`M31`, `HCG 44`, a source name) and press *Go*.
   Drag to pan, use the wheel or *−* and *+* to zoom; *⌂* resets the view.
   The *Sky projection* selector changes the projection; *AIT* or *MOL* show
   the whole sky when you zoom out.

   ```{figure} img/06-catalogues-hips-crossmatch-2.png
   :alt: The HiPS Viewer tab with a survey open around a resolved target, the order and field of view shown in the toolbar.
   :width: 100%

   A HiPS survey around a target, at the order the zoom needs.
   ```

10. *Overlay:* takes the address of a second survey, drawn over the first
    with its own opacity.

### Cross-match an image with a catalogue

11. Open the Hi-GAL cutout with *File ▸ Open…*. It opens in the image viewer.

12. Choose *Tools ▸ Catalogue ▸ Cross-match with Catalogue…*. Set *Catalogue*
    to `simbad` (or `2MASS`, `NVSS`, `FIRST`) and *Search radius* in
    arcseconds, then press *OK*.

13. The sources that fall on the image are drawn as markers with their names.
    The run is recorded in *Session Data* as *Cross-match with simbad*, with
    the catalogue, the radius and the number of sources.

    ```{figure} img/06-catalogues-hips-crossmatch-3.png
    :alt: The Hi-GAL 250 micron cutout in the image viewer with SIMBAD sources overlaid as markers, the cross-match in Session Data and the catalogue table with names and positions below.
    :width: 100%

    SIMBAD sources on a Hi-GAL cutout, with their names in the table.
    ```

14. To overlay your own table, choose *Tools ▸ Catalogue ▸ Load Catalogue
    Overlay…* and pick a CSV with `RA` and `DEC` columns in degrees. *Show
    Catalogue Overlay* and *Show Catalogue Labels* turn the markers and the
    names on and off. Names are drawn on the image once you zoom in to 24
    sources or fewer in view; the source under the pointer always shows its
    name.

## What you get

In the 3-D viewer the axes are the table's own columns, here comoving Mpc.
For a sky catalogue with right ascension, declination and a redshift or a
distance, the viewer places each source in 3-D instead. It converts redshift
to distance with the cosmology you choose (Planck18 by default) and says, for
each selected source, where its distance came from. Sources with no usable
distance are left out and counted, and *Unplaced Sources…* lists them.

The HiPS viewer reads only the tiles the view needs, so a deep zoom on a
survey of many terabytes is quick. Galactic surveys line up with equatorial
ones because the backend converts every tile to ICRS.

The cross-match searches the whole image (at least half its diagonal around
the centre) and keeps at most 500 sources, spread over the field. Before you
decide that a source has no counterpart, check one source you are sure of:
a systematic offset in the image's astrometry looks the same as a missing
counterpart.

## Next

- [Catalogues, HiPS, and SAMP](../reference/catalogues-hips.md): the 3-D
  viewer's frames, cosmologies and glyphs, overlays and the HiPS viewer.
- [Source finding and cross-matching](../reference/source-finding.md).
- [VBT tables](../reference/vbt-viewer.md): VisIVO Binary Tables of
  points.
- [Working with TOPCAT, Aladin and DS9](../reference/interoperability.md):
  sending tables and images over SAMP.
