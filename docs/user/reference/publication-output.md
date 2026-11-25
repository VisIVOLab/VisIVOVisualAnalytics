# Figures, movies and linked views

Tools for getting results out as figures, movies, annotated images, region
files and FITS products, and for keeping several cube viewers in step while you
compare them.

| Task | Command | Viewer |
|---|---|---|
| A figure with preview, fonts, titles, overlays, beam, scale bar and compass: PNG, PDF, SVG or EPS | *Tools ▸ Products ▸ Publication Figure…* | image |
| An animation | *Tools ▸ Export ▸ Export Movie…* | cube |
| A still of a 3-D view: PNG (also with a transparent background), PDF or SVG | *Tools ▸ Export ▸ Save View As…* (cube), *View ▸ Save View As…* (volume table, point viewer) | cube, volume table, points |
| Labels and arrows | *Tools ▸ Annotations ▸ Add Text Annotation…*, *Add Arrow Annotation…* | image |
| Views kept in step | *Tools ▸ Views ▸ Link Views* | cube |
| Regions for CASA or DS9 | *Tools ▸ Regions ▸ Import Region (CRTF/DS9)…*, *Export Region (CRTF/DS9)…* | both |

## Publication Figure

*Tools ▸ Products ▸ Publication Figure…* renders the current image on the
backend. The dialog has the options on the left and a preview on the right. The
preview is the backend's own figure at low resolution and is redrawn a moment
after each change, so what you see is what *Render* writes. The dialog starts
from the window: its colour map, stretch and colour range, its sky frame, its
contours, regions and annotations. With no change in the dialog the figure is
the window's own, not the plain one-click figure (Inferno, linear, 99.5 %
clip) that a request naming nothing gets. The window's own colour map is the first entry of the list
(*Glow (window)*, say), drawn with the exact colours the window shows, whatever
the map is, a hand-made one included. The dialog opens at once and narrows its list when the
backend answers, keeping what you had chosen.

| Tab | Options |
|---|---|
| *Image* | *Colour map*: the viewer's colour maps that the backend can set (`Inferno`, `Viridis`, `Magma`, `Plasma`, `Cividis`, `CoolWarm`, `Gray`, the white-to-hue maps `Purples`, `Blues`, `Greens`, `Reds`, `Greys`, `Oranges` with their `_r`, and a few with a close stand-in, such as `Glow` as `hot`; the dialog says under the list when the chosen one is only close to the window's, and the figure repeats it among its notes; matplotlib's own `hot`, `cubehelix`, `turbo` and a few more follow after a separator). *Stretch*: `linear`, `sqrt`, `squared`, `log` (the window's logarithm), `asinh`, `sinh`, `power` with its exponent. *Minimum* and *Maximum* as the window shows them, or *Clip at* a percentile of the pixels. |
| *Text* | *Title*, *X axis title* and *Y axis title* (the sky axes' names by default), the font (`sans-serif`, `serif`, `monospace`, or a family installed on the backend) and the sizes in points of the title, the axis titles, the tick labels and the colour bar label. |
| *Axes* | *WCS sky grid*; blank (NaN) pixels are left white, not painted with the top colour; *Coordinate frame* of ticks and grid (`native`, `fk5`, `icrs`, `galactic`, `ecliptic`; the window's Galactic or Ecliptic choice is the starting value); *Tick format* (`auto`, `sexagesimal`, `degrees`); the colour bar on or off, on the right, left, top or bottom, with its label (the data's `BUNIT` by default); a horizontal bar has as many ticks as its length allows, with a common power of ten written once at its end. |
| *Overlays* | *Beam*, the header's (or the one typed for the dataset) as an ellipse of its true size and orientation; *Scale bar*, angular, with a length in arcsec, arcmin or degrees (the length is typed once a unit is picked; with *auto* it is a round length, written in the unit that suits the image: arcsec below 60″, arcmin below 60′, else degrees, unless you pick one); *Compass*, north and east of the frame the ticks and grid are in, as the pixel grid has them (b and l for Galactic), so an image drawn with another frame than its own gets that frame's compass. Each goes in the corner you pick. *Contours of the window* (the image's own and every external set the window shows, all together, with the levels, colours, widths and dashes you see), *Regions* and *Text and arrow annotations* are drawn when the window has them. |
| *Output* | *Format* `png`, `pdf`, `svg` or `eps` (vector for the last three: in an SVG the labels stay <text> elements and in a PDF or EPS they are embedded TrueType, so an editor or a journal can change them); width and height in inches; resolution in dpi (200 by default); *Trim the white margin* (on by default, so the file is a little smaller than the size asked for). |

Anything that is asked for and cannot be drawn is said, not skipped: a beam
needs `BMAJ` and `BMIN` in the header, the scale bar and the compass need a sky
grid, and a font that is not installed on the backend is refused rather than
replaced. The preview shows these notes under the picture, and *Render* repeats
them.

*Render* writes the file into the backend's Workspace Exports and lists it among
the image's products. The message that follows offers to open it (this works
when the backend shares your machine's file system) and to save a copy
anywhere, which also works for a remote backend. For data a reader will judge
quantitatively, prefer a perceptually uniform colour map (`viridis`, `inferno`,
`magma`). The `log` stretch is the window's: colours evenly spaced in the logarithm of
the value, a minimum at or below zero raised to a positive floor as in the
window, so preview and window agree.

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

## Save View As

*Tools ▸ Export ▸ Save View As…* in the cube viewer, and *View ▸ Save View As…* in the
volume-table and 3-D point viewers, saves the 3-D view as a figure.

| Field | What it does |
|---|---|
| *Format* | *PNG image*; *PDF* or *SVG*, in which the axes, colour bar and title are vectors (text stays text) over the render, which is an image inside the file. |
| *Size* | The view's size, 2× or 4× it, or a width in pixels. The height follows the shape of the view and the camera is kept. The 3-D scene is rendered again at that size, not enlarged. A side is at most 16 384 pixels and a picture at most 100 million pixels; the dialog says when a size is too large. |
| *Transparent background* | PNG only. The picture has an alpha channel: nothing is drawn where the scene is empty, and a translucent volume fades into it. |
| *Axes, colour bar and title* | On by default. In a PNG the axes box and orientation marker the view draws stay as on screen (the orientation marker only at the view's size: a larger picture leaves it out) and the colour bar is added; in a PDF or SVG they are all redrawn as vectors (the box with the name and range of each axis). *Title* is optional and drawn at the top left. |

A PDF or SVG page is the size of the view on screen, so its text reads at the size it
has in the window; the render inside it has the pixels of the chosen size. The
render itself is a raster: a ray-cast volume has no vector form.

A view rendered on the backend (*Remote*) is saved as the frame the server last streamed,
at the size it was streamed in and with its own background, so the size and
transparent options are off; the colour bar and title are added. The ray-traced views
keep their own picture output, at up to 8K (see [Ray-traced volumes](raytraced-volumes.md)
and [Ray-traced particles](raytraced-particles.md)).

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
| 2-D pan and zoom | Shared by the 2-D slice and product panes in the window and in linked windows on the same sky grid; an image window in the group follows by sky (see below). Changing channel or resizing a pane is not shared. |
| Channel | Matched by spectral value, not channel index, so cubes with different channel widths show the same velocity. Both cubes need a trustworthy spectral axis; with one in frequency and one in velocity, check a known line first. |
| Colour | Colormap and stretch, adopted at the next change. |
| Representation and blend | Volume or isosurface; Composite, MIP or MinIP. |

## Link an image and a cube

An image and a cube of the same sky share the cube viewers' *Link Views*: switch
*Link Views* on in the image viewer (*Tools ▸ Views*) and in the cube window, and
the two follow each other in sky coordinates, as CARTA's XY matching does. There
is one link, the same one that keeps cubes in step; the image is another kind of
peer in it. Turn *Link Views* off in either window to leave the group.

| Linked | How |
|---|---|
| Pan and zoom | By sky centre and angular height, not by pixels: the cube's slice (and the map panes of that cube window) shows the same patch of sky as the image, however different their pixel size, projection or sky frame. North is not turned, so two images rotated against each other keep their own orientation. The cube's *camera* chip gates it. |
| Cursor | An orange crosshair in each window at the other's cursor position. |
| Regions | *Send Selected Region to Linked Cube* and *Send All Regions to Linked Cube* in the image viewer (*… to Linked Image* in the cube viewer) copy regions to the windows in the group, placed on the same sky. |
| PV path | In the image viewer, select a line region and use *Extract PV in Linked Cube*: the cube cuts a PV diagram along its vertices. |

Both datasets need a celestial WCS; a window whose dataset has none does not
follow, and regions cannot be sent to it. The view and the cursor are those of the
cube's 2-D slice, so the slice must be on screen. A region sent across keeps its shape and its sky
position: the backend places it on the sky with the first image's WCS and back on
the pixels with the second's, the same conversion as the region properties
editor, so a circle stays a circle and a box turns with the other grid. Regions
the other image does not cover are listed in the report that opens.

A path is a line region drawn with the Line tool; right-click it and choose *Add
Vertex Here* where the cut has to bend, or *Remove This Vertex*. In the cube, the PV
diagram is the one of *Extract PV Diagram…*: width, spectral range and the path
editing work as usual, and the path stays editable.

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
