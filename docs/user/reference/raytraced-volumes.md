# Ray-traced volumes

Ray-traced volumes draw a cube as light: each voxel emits, and optionally
absorbs, and the light along each line of sight is summed in floating point,
then graded into a picture by a tone curve. A cube with a large dynamic range
keeps both its faint diffuse emission and its bright cores, with no opacity
curve to tune. The colour can come from a second quantity, such as a
simulation's temperature or a spectral cube's velocity. Use it for figures,
talks and movies; the analysis rendering (Composite, MIP, MinIP, threshold)
stays as it is. Turn it on with the *Ray-traced light* check in the cube
viewer's *DISPLAY, 3D* section. It works on a FITS cube or a field of a VBT
volume.

## In the window

*Ray-traced light* replaces the 3-D view with the ray-traced one and shows its
settings under the check, in place of the analysis rendering's controls. The
colour map and camera start from the 3-D view's, and the 3-D view takes the
camera back when the check is cleared.

- Drag to turn, Shift-drag or the right button to move, scroll to zoom.
- The view is drawn on this machine's GPU from a grid of at most 256³ voxels,
  sampled from a larger cube. Stills and movies use the full cube.
- Without a GPU view on this machine, the backend draws a frame each time the
  view stops moving.
- A backend older than the client has no ray-traced views: the check clears
  itself and the status bar names the backend and says to update it.

## Settings

| Setting | What it does |
|---|---|
| *Mode* | *3-D light*: emission and absorption, in perspective. *Projected map*: the column along each line of sight, without perspective, for spatial grids (for a spectral cube, use [moment maps](moment-maps.md)). |
| *Values are log₁₀* | The cube holds a logarithm; its light is 10 to the value. |
| *Dark below the threshold* | Values under the 3-D view's *Threshold* emit nothing, so a radio cube's noise does not add up to a veil. On by default for a cube with negative values, off for a simulation grid. |
| *Contrast* | The light raised to this power; above 1, diffuse emission fades and dense structure stands out (2 gives a density squared). |
| *Colour by*, *Map*, *Log scale* | The colour: *Its own values*, another field of a VBT volume, or, for a cube with a spectral axis, *Velocity*. The range covers the 1st to 99.9th percentile where there is light. *Velocity* is in km/s (optical velocity, cz, from the rest frequency for a frequency axis), linear, over the velocities holding 98 % of the light, and sets the map to *Coolwarm* (approaching blue, receding red). |
| *Exposure* | Stops over the automatic exposure (the 99.5th percentile of the lit pixels); +1 doubles the light. |
| *Absorption* | Grey absorption, as the optical depth reached where the light is brightest; 0 is emission only. Drawn in the GPU view too. |
| *Samples* | Samples per voxel along a ray (2 by default): more is finer and slower. |
| *Slab*, *Range* | A projected map's thickness around the focal plane, in voxels, and the decades of column its brightness spans. |
| *Curve*, *Stretch*, *Gamma*, *Saturation* | The tone curve: *Asinh* and *Logarithmic* lift faint light, *Exponential* compresses the bright, *Linear* does neither. |

A projected map coloured by velocity and seen along the spectral axis shows the
intensity-weighted mean velocity, the first moment.

*Box* and *Colour bar* draw the cube's box and colour scale in the view and on
stills and frames.

## Output

The *Output* section works as for [ray-traced particles](raytraced-particles.md):
*Size*, *Render Still* (from the full cube, by the backend), *Save Image…*,
*Save Light…* (floating-point FITS before the tone curve) and *Movie…* (an orbit,
MP4 with `ffmpeg`, otherwise PNG frames). A 512³ cube renders in about a second
at 1920 × 1080 on an Apple M-series laptop. Quitting the application stops a
render on the backend.

Details: [Volume rendering API](../../technical/backend-api/volume-render.md).
