# Figures, movies and linked views

Tools for getting results out as figures, movies, annotated images, region
files and FITS products, and for keeping several cube viewers in step while you
compare them.

| Task | Command | Viewer |
|---|---|---|
| A figure with colour bar and sky grid | *Tools ▸ Products ▸ Publication Figure…* | image |
| An animation | *Tools ▸ Export ▸ Export Movie…* | cube |
| Labels and arrows | *Tools ▸ Annotations ▸ Add Text Annotation…*, *Add Arrow Annotation…* | image |
| Views kept in step | *Tools ▸ Views ▸ Link Views* | cube |
| Regions for CASA or DS9 | *Tools ▸ Regions ▸ Import Region (CRTF/DS9)…*, *Export Region (CRTF/DS9)…* | both |

## Publication Figure

*Tools ▸ Products ▸ Publication Figure…* renders the current dataset on the
backend.

| Field | Values |
|---|---|
| *Stretch* | `linear`, `log`, `sqrt`, `asinh`, `power` |
| *Colormap* | `inferno`, `viridis`, `magma`, `gray`, `hot`, `cubehelix`, and the white-to-hue maps `Purples`, `Blues`, `Greens`, `Reds`, `Greys`, `Oranges` (each also reversed with `_r`) |
| *Title* | optional |
| *WCS sky grid* | on by default |
| *Colorbar (BUNIT)* | colour bar labelled with the data's `BUNIT`; on by default |

*Render* writes a 200 dpi PNG into the backend's Workspace Exports, lists it
among the image's products and offers to open it (this works when the backend
shares your machine's file system). For data a reader will judge
quantitatively, prefer a perceptually uniform colormap (`viridis`, `inferno`,
`magma`).

A figure does not record how it was made. Note in the caption the channel range
of a moment map and any mask, the stretch and a hand-set colour range, the beam,
and for a [Kinematic Lasso](kinematic-lasso.md) selection its *Reach* and λ_v.

## Export Movie

*Tools ▸ Export ▸ Export Movie…* in the cube viewer.

| Field | What it does |
|---|---|
| *Mode* | *Channel scan (2D channel maps)* steps through the channels, recording the 2-D slice (kinematic-model contours included). *Camera orbit (3D view)* turns the 3-D camera a full 360°. |
| *First channel*, *Last channel* | Channel range of a scan; the full range by default. |
| *Orbit frames (360°)* | Number of frames in an orbit. |
| *Frames per second* | Playback rate. A 300-channel scan at 30 fps lasts 10 s. |

Frames are taken from the view on screen at its size. In a one-pane layout the
pane switches to the needed view for the export and back; otherwise show that
view in a pane first. A scan is refused while the slice shows a map instead of
the channels. The camera is restored after an orbit, also when cancelled. The
movie is an MP4 saved in your Movies folder (Documents if there is none); without
`ffmpeg` it is a PNG sequence in Documents.

## Annotations

In the image viewer, *Tools ▸ Annotations*:

- *Add Text Annotation…*: places a text label on the image.
- *Add Arrow Annotation…*: places an arrow.
- *Clear Annotations*: removes them all.
- *Save Annotations…*, *Load Annotations…*: write them to a file and read them
  back, to rebuild a figure later or pass it on.

Annotations are fixed in image coordinates, so they stay on their feature when
you zoom or change the stretch.

## Link Views

*Tools ▸ Views ▸ Link Views* in the cube viewer keeps several cube viewers
showing the same thing. Enabling it aligns the linked viewers at once. The chips
beside the button switch each part on or off (for example, link the channel but
not the camera when the cubes cover different areas).

| Linked | How |
|---|---|
| Camera | Relative to each cube's data bounds, so cubes of different sizes on the same sky frame the same region. Rotation, pan, zoom, camera presets and reset are followed. |
| 2-D pan and zoom | Shared by the 2-D slice and product panes in the window and in linked windows on the same sky grid. Changing channel or resizing a pane is not shared. |
| Channel | Matched by spectral value, not channel index, so cubes with different channel widths show the same velocity. Both cubes need a trustworthy spectral axis; with one in frequency and one in velocity, check a known line first. |
| Colour | Colormap and stretch, adopted at the next change. |
| Representation and blend | Volume or isosurface; Composite, MIP or MinIP. |

## Regions in and out

*Import Region (CRTF/DS9)…* and *Export Region (CRTF/DS9)…*, under
*Tools ▸ Regions* in both viewers, read and write the CASA (CRTF) and DS9 formats.
Lines that cannot be read are listed with the reason.

- A region in another sky frame than the image's is moved onto the image through
  its WCS, angle included. Sizes keep their own units.
- ds9 files in `fk5` (`j2000`), `fk4` (`b1950`), `icrs`, `galactic`, `ecliptic`,
  `image` or `physical` are read. Files in ds9's `wcs` (`wcsa` to `wcsz`),
  `linear`, `amplifier` or `detector` systems are refused: save them again in a
  supported system.

Check a region you depend on by a round trip (export, reopen in the original
tool), especially polygons and annuli. See [Regions](../use-cases/04-regions.md).

## FITS products

For data you will measure again, export FITS. In the cube viewer, *Tools ▸
Export* has one entry whose label follows the focused pane:

- *Export Sub-Cube as FITS…* (3-D view): the selected region, with WCS.
- *Export Current Channel as 2-D FITS…* (2-D slice): one plane.
- *Export … as FITS…* (a moment or another map): the map with its own units.

All go to the Workspace Exports folder with their WCS. See
[Cube viewer](cube-viewer.md).

Details: [Linked views](../../technical/design/linked-views.md) and
[Movie export](../../technical/design/movie-export.md).
