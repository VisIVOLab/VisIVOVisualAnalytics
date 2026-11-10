# Ray-traced particles

A simulation snapshot drawn as a continuous medium rather than as points: each
particle is a cloud of light of its own size, and the light of all of them is
summed along each line of sight, in floating point, then graded into a picture.
It is meant for figures, outreach and movies; the analysis view (points,
splats, filters, colours) stays as it is and is what the rendering starts from.

| | |
|---|---|
| **In the window** | *Ray-traced light* in a [point table's window](vbt-viewer.md#point-clouds): the view itself, every row of the table, on this machine's GPU |
| **Stills and movies** | *Output* in the same card: rendered by the backend, at up to 8K, saved as PNG, a floating-point FITS or an MP4 |
| **Data** | any table of points: a VBT point table, a cartesian or sky catalogue |

---

## The method

Each particle is a Gaussian kernel whose width is its smoothing length. A ray
through the scene meets the particles back to front; each one adds its light
and, optionally, dims what lies behind it, as a slab that emits and absorbs.
The light is kept in floating point, without a ceiling, and turned into a
picture by a tone curve. This is the ray-traced splatting of SPH particles
described by Dolag et al. (2008, *New J. Phys.* 10, 125006).

Two details keep a picture faithful at any zoom. A particle smaller than a pixel
is drawn one pixel wide with the same light, so dense cores neither vanish nor
flicker; and a particle's light is weighted by what it measures (below), so a
gas shows its projected density and stars their mass.

The **projected map** style shows a simulation as cosmological simulations are
usually shown, for instance the Millennium Simulation's images (Springel et
al. 2005, *Nature* 435, 629): the density summed along the line of sight
without perspective, its brightness the logarithm of that column, its colour
the density itself or a second quantity averaged along the ray, such as the
local velocity dispersion.

---

## In the window: *Ray-traced light*

Checking **Ray-traced light**, under *Shape* in *Source representation* (as
in the cube viewer, where it sits under the 3-D rendering controls), replaces
the 3-D view with the ray-traced one and shows its settings in a
**RAY-TRACED** card below. It is a way of drawing the table, not a shape: the
shape stays chosen, greyed out with the other controls that do not apply to
the ray-traced view (the scale, the render budget, the size column, labels
and the scene card), and is drawn again when the check is cleared. The view uses every row of the table (the window itself loads a
sample), drawn on this machine's GPU:

- **drag** to turn, **Shift-drag** or the right button to move, **scroll** to
  zoom, as in the 3-D views; while the view moves it keeps the last grading,
  so the light does not jump;
- the camera starts from the 3-D view's, and the 3-D view takes the ray-traced
  camera back when *Ray-traced light* is cleared;
- the column and colour map of *Visual encoding*, and the filters, set the
  particles' colours and which rows are drawn, as for the other shapes.

The particles are prepared by the backend once (sizes, light, colours), then
sent to the desktop: about 120 MB for 3.7 million particles. Above 8 million an
even sample is sent, each particle carrying the light of those it stands for.
With a backend far away and a large table, the backend's own rendering
(*View → Rendering → Remote (GPU on backend)*) is the better choice.

### Settings

The card shows the essentials; **More settings** opens the rest.

| Setting | What it does |
|---|---|
| **Mode** | *3-D light*: the particles emit (and absorb) light, in perspective. *Projected map*: the column of density along each line of sight, without perspective. |
| **Light** | The column a particle's brightness comes from; *log* when the column holds its logarithm (LogMass, LogD…). *Same for all*: every particle alike. |
| **Contrast** | The light raised to this power. Above 1 the faint, diffuse particles fade and the dense structure stands out; 1.5 is a good start when a gas looks foggy. |
| **Exposure** | In stops over the automatic exposure (the 99.5th percentile of the lit pixels): +1 doubles the light. |
| **Colour by**, **Map** | A projected map's colour: the density itself, the view's colour column averaged along each ray, or the velocity dispersion (from VX, VY, VZ), through a named map. |
| *More settings:* **Size** | Each particle's width: a smoothing-length column, or half the distance to its *N*-th nearest particle (N = 32 by default, as an SPH kernel's neighbours); *×* scales it. Smaller is sharper, larger smoother. |
| **Measures** | What the light is. *Per particle* (a mass, a luminosity): a particle sends its light however far it is spread. *Density*: a ray sees the column it crosses. A density column read as *Per particle* is often the clearer picture: large particles in voids then stay dim instead of fogging the view. |
| **Absorption** | Grey absorption, as the optical depth reached where the light is brightest: 0 is emission only; 1 or more lets dense regions hide what lies behind them. Rendered in stills and movies; the GPU view shows emission only. |
| **Slab**, **Range** | A map's thickness around the focal plane (the Millennium images use a 15 Mpc/h slab), and the decades of density its brightness spans. |
| **Curve**, **Stretch**, **Gamma**, **Saturation** | The tone curve of a 3-D render: *Asinh* and *Logarithmic* lift the faint light (more with a higher stretch), *Exponential* compresses the bright, *Linear* does neither. |

---

## Box and colour bar

*Box* draws the data's bounding box, *Colour bar* the scale of the colours:
in a 3-D render the view's colour column (its colour map, range and scale, as
*Visual encoding* sets them); in a projected map the column of density,
relative, on a log scale over the range the brightness spans, or the quantity
of the hue (the velocity dispersion, the view's colour column). Both are drawn
over the view and onto the stills and movie frames, so what is saved is what
is shown.

---

## Stills and movies: *Output*

The *Output* section of the card renders the view as it is on screen: its
camera, settings, box and colour bar.

- **Size**: 1920 × 1080, 3840 × 2160, 7680 × 4320, 2048 × 2048, or the view's
  shape.
- **Render Still**: rendered by the backend, so it does not depend on the
  desktop's GPU and includes absorption. It is shown in place of the view until
  the view is turned; change the tone meanwhile and it is graded again without
  rendering it again.
- **Save Image…** writes the still as graded, with its box and colour bar;
  **Save Light…** the light before the tone curve, a floating-point FITS (three
  planes, red, green, blue; for a projected map, the density and its weighted
  mean), to grade it elsewhere.
- **Movie…**: an orbit about the view's up axis (*Turn*, *Length*, *Rate*).
  The frames take the exposure of the still, or of the view, so their
  brightness does not flicker. They are encoded as an MP4 (H.264) with `ffmpeg`
  when it is installed, otherwise saved as a numbered PNG sequence.
- A render in progress shows its progress below the buttons, with *Cancel*.

Without a GPU view on this machine the backend draws the view too, a frame
each time it stops moving.

On a laptop (Apple M-series) a 3.7-million-particle frame takes about a second
at 1920 × 1080, and a 21-million-particle one about seven seconds at
3840 × 2160.

---

## Recipes

A gas snapshot with a density and a temperature, coloured by temperature:

- *Visual encoding*: Color the temperature column, *Edit* → Log scale, Min
  10⁴, Max 3 × 10⁸ (K), colour map *Glow*;
- *Ray-traced*: Mode *3-D light*, Light the density column, Contrast 1.5,
  *More settings*: Size N = 16, × 0.8, Measures *Per particle*, Absorption 1.5,
  Curve *Asinh*, Stretch 10.

The cosmic web as in the Millennium images:

- Mode *Projected map*, Colour by *Density* with *Magma*, or *Velocity
  dispersion* when the table has VX, VY, VZ;
- zoom in until the voids, filaments and nodes fill the view; a *Slab* of a few
  tens of the data's units and a *Range* of 3 to 4 decades.

---

## See also

- [VBT tables](vbt-viewer) — point tables and their window.
- [Catalogues and HiPS](catalogues-hips) — the 3-D catalogue viewer.
- [Rendering API](../backend-api/particle-render) — the routes behind the stills, movies and GPU view.
