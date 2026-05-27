# Image viewer

The image viewer (`vtkWindowImage`) handles 2-D FITS images: single-plane
maps, mosaics, moment maps exported from a cube, and any other 2-D product
the backend exposes. It opens automatically when you open a FITS file the
backend classifies as `image`.

## Layout

Single 2-D dock with the image and a side panel for:

- **Layers** — stacked images and overlays (multiple FITS files can be
  composited).
- **Display** — color map, scaling, contrast / brightness, WCS overlay.
- **Tools** — region statistics, profile / probe, WCS frame switch.
- **Info / Stats** — pixel-value summary, BUNIT, image extent.

## Preview → full upgrade

Like the cube viewer, the image viewer first shows a downsampled **preview**
(`/v1/image/preview` with a max-longest-side cap) so the window is
interactive immediately. The full resolution (`/v1/image/full`) loads in
the background; the LUT and WCS overlay continue to work during the
upgrade.

## Adding layers

You can overlay or compare multiple images in the same window:

- **Add New Layer…** — pick another FITS image from the *remote file
  browser*. Backend-side check (`ImageLayerImportService`) validates the
  layer's WCS against the base image and warns if the pixel grids don't
  overlap.
- Use the layer panel to **reorder**, **toggle visibility**, set a
  **per-layer colour map**, **opacity** and **z-order**.
- Manual local files: drop a `.fits` from the OS file browser, or pick it
  via *File → Add New FITS File…*

All layer sources go through the same `loadImageLayer()` / `AstroUtils` /
`libwcs` pipeline, so alignment between layers is handled the same way
regardless of whether they come from VLKB, the remote backend, or local
disk.

## WCS overlay and frame

- **Show WCS Axes** in the *View* menu paints ticks along the image axes
  according to its WCS metadata.
- **Coordinate format** — a **Sexagesimal | Decimal** segmented toggle in
  the sidebar's *Tools → WCS Display* card.
- **Coordinate frame** — a **Galactic | FK5 | Ecliptic** segmented toggle
  in the same card. Conversions go through `wcscon()` from libwcs. The
  current frame label is shown in the bottom status bar.

### Beam indicator

When the FITS header contains `BMAJ` and `BMIN` (and optionally `BPA`),
a filled semi-transparent white ellipse is drawn in the bottom-left
corner of the image. The ellipse is sized in pixels using the angular
beam axes divided by `|CDELT1|`, and rotated by `BPA`. If the beam
keywords are absent the indicator is hidden automatically.

If the WCS metadata is partial or invalid the backend sanitises it and the
**WCS** badge in the status bar turns yellow with a tooltip listing what
was changed.

## Color map & contrast

The *Layer Settings* sidebar exposes:

- **Color map** combo (Inferno, Viridis, Magma, Plasma, Cividis, …) with
  gradient preview icons.
- **Scale** — a **Linear | Log** segmented toggle (same pill-style
  control used throughout the cube viewer). Log scale compresses the
  dynamic range and is useful for images spanning several orders of
  magnitude (e.g. Hα narrowband, X-ray mosaics).
- **Layer opacity** slider for blending when multiple layers are stacked.

For more precise control open the *2-D LUT editor* (`Advanced…` button) —
a non-modal QCustomPlot editor where you can drag the transfer-function
control points.

## Regions and probes

The image viewer shares the same region / probe machinery as the cube
viewer's 2-D dock:

- **Probe** — click a pixel to see its value (DN / BUNIT). Hover updates a
  read-out in the bottom-right.
- **Box / Circle / Polygon / Annulus regions** — compute statistics
  (mean, median, MAD-based sigma, min, max, sum) over the region.
- Right-click on a region to copy the value summary or remove it.

The detailed semantics are documented in
[Regions, PV, noise](region-pv-noise).

## Contour overlay

Iso-contour lines can be drawn on top of the image from two sources:

- **Self contours** — computed from the image's own pixel values.
  Toggle **Show Contours** in the *Tools* sidebar (or *Tools* menu),
  then adjust **Level** (number of contour lines), **Lower** and
  **Upper** (value range). The pipeline uses `vtkFlyingEdges2D`, the
  same filter as the cube viewer's slice contours.
- **External FITS contours** — *Tools → Load Contour from FITS…*
  lets you overlay contours from a different FITS file (e.g. a radio
  moment-0 map on an optical image). Contour levels are entered
  manually. Multiple external layers can be stacked.
- **Clear All Contours** removes every contour layer.

- **Cube → Image direct** — if a cube viewer is open, use
  *Tools → Send Slice to Image Viewer…* in the cube window. The
  current 2-D slice is sent to the first open image viewer as a
  contour overlay without requiring a FITS export round-trip.

```{tip}
The cube viewer's *Tools → Export Moment Map as FITS…* writes the
moment into the Workspace Exports directory. You can then load that
FITS here as an external contour overlay — the most common radio +
optical comparison workflow.
```

## Measurement tools

Two interactive measurement modes are available in the *Tools* sidebar
**Measurement** card (or *Tools* menu):

- **Ruler (Distance)** — click two points on the image. A dashed
  orange line is drawn between them and the distance is displayed in
  pixels and, when WCS metadata is available, in arcseconds (or
  arcminutes / degrees for large separations). The angular distance
  uses the Haversine formula on the WCS sky coordinates.
- **Angle (3 Points)** — click three points A, B, C. Lines A–B and
  B–C are drawn and the angle at vertex B is displayed in degrees.
- **Clear Measurement** removes the current measurement overlay.

Only one measurement mode can be active at a time (ExclusiveOptional
group). Activating a measurement mode deactivates any active probe or
region tool.

## Pixel histogram

*Tools → Pixel Histogram…* (or the **Histogram** card in the sidebar)
opens a floating QCustomPlot window showing the pixel-value distribution
of the master layer as a 256-bin bar chart.

Two draggable vertical cursors mark the current LUT clip range
(red = low, green = high). Dragging either cursor updates the LUT
range live, so you can interactively clip the display without opening
the advanced LUT editor.

## Annotations

Text and arrow annotations can be placed interactively on the image:

- **Add Text…** — a dialog asks for the text, then the cursor changes
  to a crosshair. Click anywhere on the image to place the label.
- **Add Arrow…** — a dialog asks for an optional label, then the
  cursor changes to a crosshair. **First click** sets the arrow tip;
  **second click** sets the label position. A line connects the two.
- Right-click cancels placement without adding the annotation.
- **Clear All** removes every annotation.
- **Save / Load** — annotations are persisted to a JSON sidecar file
  (`<fits-path>.annotations.json`) next to the dataset. Reloading the
  same dataset and clicking **Load** restores them.

Annotation text is rendered as bold yellow `vtkTextActor` labels;
arrows use `vtkLineSource` with matching colour.

## Blink / Compare

When two or more layers are loaded, the **Blink Layers** toggle
(*View* menu or sidebar **Blink / Compare** card) rapidly alternates
the visibility of layers 0 and 1 at a configurable speed
(50 – 1000 ms, adjustable via the sidebar slider).

This is a standard technique for detecting transient sources, proper
motion, or artefact differences between two epochs or bands.

## Saving and exporting

- **Export** in the top-right toolbar saves a PNG of the current view.
- **Tools → Export to Workspace as FITS…** copies the dataset's FITS
  file into the persistent Workspace Exports directory. From there you
  can download it to your computer, re-open it in VisIVO, or delete it
  — all via the **Workspace Exports** panel in the Data Hub.

## Catalogue overlay

Same machinery as the cube viewer: load a CSV / VOTable from
*Tools → Load Catalogue Overlay*. The sources are projected through the
image WCS, drawn as glyphs, and listed in a sortable side table. Click a
row to centre the view on the source; click on a glyph to highlight the
matching table row.

## Diagnostics & errors

Backend errors during load (`/v1/datasets/open`, `/v1/image/full`) and
WCS sanitisation warnings flow into the same *Diagnostics* panel that the
cube viewer uses — open it with **View → Diagnostics**.

## Comparing 2-D vs. 3-D workspaces

If you open a moment map that the cube viewer just generated, the image
viewer is the right tool to compare it against an external image, do region
photometry, or send it via SAMP. Going the other way around: to inspect a
cube channel-by-channel you need the cube viewer — the image viewer only
shows a single 2-D plane.
