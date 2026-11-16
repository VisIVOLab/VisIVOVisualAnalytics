# Ray-traced particles

Ray-traced particles draw a table of points as a continuous medium: each
particle is a Gaussian cloud of light as wide as its smoothing length, and the
light along each line of sight is summed in floating point, then graded into a
picture by a tone curve. Use it for figures, talks and movies of a simulation
snapshot or a catalogue; the analysis view of the 3-D catalogue viewer stays as
it is. Turn it on with the *Ray-traced light* check under *SOURCE
REPRESENTATION ▸ Shape* in a point table's window (a VBT point table, a
Cartesian or sky catalogue).

## In the window

*Ray-traced light* replaces the 3-D view with the ray-traced one, drawn from
every row of the table on this machine's GPU, and shows its settings in a
*RAY-TRACED* card. The shape, scale, render budget, size column, labels and
scene card are greyed out until the check is cleared.

- Drag to turn, Shift-drag or the right button to move, scroll to zoom.
- The camera starts from the 3-D view's, and the 3-D view takes it back when
  the check is cleared.
- The *VISUAL ENCODING* colour column and map, and the *FILTERS*, set the
  particles' colours and which rows are drawn.
- Above 8 million rows an even sample is sent to the desktop, each particle
  carrying the light of those it stands for. With a distant backend and a
  large table, *View ▸ Rendering ▸ Remote (GPU on backend)* is the better
  choice.
- Without a GPU view on this machine, the backend draws a frame each time the
  view stops moving.
- A backend older than the client has no ray-traced views: the check clears
  itself and the status bar says the backend needs updating.

## Settings

The card shows the main settings; *More settings* opens the rest.

| Setting | What it does |
|---|---|
| *Mode* | *3-D light*: particles emit (and optionally absorb) light, in perspective. *Projected map*: the column of density along each line of sight, without perspective, as cosmological simulations are usually shown. |
| *Light* | The column a particle's brightness comes from; *log* when the column holds a logarithm (LogMass, LogD). *Same for all*: every particle alike. |
| *Contrast* | The light raised to this power. Above 1, diffuse particles fade and dense structure stands out; try 1.5 when a gas looks foggy. |
| *Exposure* | Stops over the automatic exposure (the 99.5th percentile of the lit pixels); +1 doubles the light. |
| *Colour by*, *Map* | A projected map's colour: *Density*, *View's colours* (the colour column averaged along each ray) or *Velocity dispersion* (needs VX, VY, VZ columns). |
| *Size* | Each particle's width: a smoothing-length column, or *From neighbours*, half the distance to its N-th nearest particle (N = 32 by default); *×* scales it. |
| *Measures* | *Per particle* (a mass, a luminosity): a particle sends the same total light however wide it is. *Density*: a ray sees the column it crosses. A density read as *Per particle* often gives the clearer picture, because large particles in voids stay dim. |
| *Absorption* | Grey absorption, as the optical depth reached where the light is brightest. 0 is emission only; 1 or more lets dense regions hide what is behind them. Only in stills and movies; the GPU view shows emission only. |
| *Slab*, *Range* | A projected map's thickness around the focal plane, in the table's units (*Everything* by default), and the decades of density its brightness spans (*Automatic* by default). |
| *Curve*, *Stretch*, *Gamma*, *Saturation* | The tone curve of a 3-D render. *Asinh* and *Logarithmic* lift faint light (more with a higher stretch), *Exponential* compresses the bright, *Linear* does neither. |

*Box* draws the data's bounding box and *Colour bar* the colour scale (the
colour column in a 3-D render; the relative column density, or the hue's
quantity, in a projected map). Both appear in the view and on saved stills and
frames.

## Output

The *Output* section renders the view as it is on screen, with its camera,
settings, box and colour bar.

- *Size*: 1920 × 1080, 3840 × 2160 (4K), 7680 × 4320 (8K), 2048 × 2048, or
  *The view's shape*.
- *Render Still*: rendered by the backend from all particles, including
  absorption. It replaces the view until the view is turned; tone changes
  regrade it without rendering again.
- *Save Image…*: the still as graded, as PNG.
- *Save Light…*: the light before the tone curve, as a floating-point FITS
  (red, green and blue planes; for a projected map, the density and its
  weighted mean).
- *Movie…*: an orbit about the view's up axis (*Turn*, *Length*, *Rate*), with
  the still's exposure so frames do not flicker. Encoded as MP4 (H.264) when
  `ffmpeg` is installed, otherwise saved as numbered PNG frames.
- *Cancel*: stops a render in progress. Quitting the application also stops it
  on the backend.

A 3.7-million-particle frame at 1920 × 1080 takes about a second on an Apple
M-series laptop.

The same renderer draws a point table in the headset when its style is
*Ray-traced* (see [VR](vr.md)).

## Starting points

- Gas coloured by temperature: *VISUAL ENCODING* colour the temperature column,
  Log scale, Min 10⁴, Max 3 × 10⁸ K, map *Glow*; *Mode* *3-D light*, *Light*
  the density, *Contrast* 1.5, *Size* N = 16 × 0.8, *Measures* *Per particle*,
  *Absorption* 1.5, *Curve* *Asinh*, *Stretch* 10.
- Cosmic web: *Mode* *Projected map*, *Colour by* *Density* with *Magma* (or
  *Velocity dispersion*), a *Slab* of a few tens of the data's units and a
  *Range* of 3 to 4 decades.

Details: [Ray-traced particles: methods and implementation](../../technical/methods/raytraced-particles.md)
and the [rendering API](../../technical/backend-api/particle-render.md).
