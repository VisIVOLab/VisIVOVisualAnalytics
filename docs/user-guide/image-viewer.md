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
- **WCS Format** toggle: sexagesimal (HMS / DMS) or decimal degrees.
- Frame switch (when applicable): J2000 / FK5, Galactic (l, b), or Ecliptic.
  Conversions go through `wcscon()` from libwcs. The current frame label is
  shown in the bottom status bar.

If the WCS metadata is partial or invalid the backend sanitises it and the
**WCS** badge in the status bar turns yellow with a tooltip listing what
was changed.

## Color map & contrast

The *Display* sidebar exposes:

- **Color map** combo (Inferno, Viridis, Magma, Plasma, Cividis, …).
- **Range** — fixed (min/max), percentile (e.g. 1–99 %), or full data
  range. Click the range button after each LUT change to recompute.
- **Brightness / contrast** sliders for fine tuning without changing the
  underlying data.

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
