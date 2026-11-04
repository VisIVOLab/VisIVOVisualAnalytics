# Ray-traced volumes

A cube drawn as light: each voxel emits, and optionally absorbs, and the light
along each line of sight is summed in floating point, without a ceiling, then
graded into a picture by a tone curve. A cube with a large dynamic range keeps
both its faint diffuse emission and its bright cores, with no opacity curve to
tune by hand. Its colour can come from a second quantity: a simulation's
density lit and coloured by its temperature, or a spectral cube's emission
coloured by the velocity of each channel, its motions in 3-D. It is meant for figures, talks and
movies; the analysis rendering (Composite, MIP, MinIP, threshold, colour bar)
stays as it is.

| | |
|---|---|
| **In the window** | *Ray-traced light* in the cube viewer's *Display, 3D* section: the 3-D view itself, on this machine's GPU, or drawn by the backend without one |
| **Stills and movies** | *Output* under it: rendered by the backend from the full cube, at up to 8K, saved as PNG, a floating-point FITS or an MP4 |
| **Data** | a FITS cube, or a field of a VBT volume; the colour from the same values, from another field of the volume, or from a spectral cube's velocity |

---

## The method

The cube is a grid of emissivities. Each ray is marched front to back through
it, sampling it trilinearly (two samples per voxel by default); each sample
adds its light and, with absorption, dims what lies behind it, the step
integrated exactly as a slab that emits and absorbs. The model, the tone curves
and the colour tables are those of the [ray-traced particles](raytraced-particles):
the same light, graded the same way.

The **projected map** shows the column of the cube's quantity along each line
of sight, without perspective, optionally within a slab around the focal
plane. Its brightness is the logarithm of the column; its colour, the column
itself or the column-weighted mean of a second quantity. For a spectral cube a
projection only has a meaning along the velocity axis, which the
[moment maps](moment-maps) already cover: the map is meant for spatial grids.
Coloured by velocity and seen along the spectrum, its colour is the
intensity-weighted mean velocity, the first moment.

---

## In the window: *Ray-traced light*

Checking *Ray-traced light* replaces the 3-D view with the ray-traced one and
opens its settings under the check, in place of the analysis rendering's
(blend, lighting, colour map and the 3-D layers, which draw nothing in it). Its map starts from the 3-D view's, and its colour bar is drawn like
the 3-D view's. The camera starts from the 3-D view's (the backend view's, when
that is shown), and the 3-D view takes it back, with its controls, when the
check is cleared.

- **drag** to turn, **Shift-drag** or the right button to move, **scroll** to
  zoom; while the view moves it keeps the last grading, so the light does not
  jump;
- the backend prepares the grid once (at most 256³ voxels, sampled with a
  stride from a larger cube) and sends it to the desktop: 96 MB for 256³,
  compressed from a remote backend, where what lies below the threshold
  costs almost nothing (a radio cube's grid comes through a tunnel in a
  second or less);
- with a cube open on a remote backend, the view starts from the backend
  view's camera and gives it back when the check is cleared: the backend
  view goes on from where the ray-traced one was turned to.
- on a machine without a GPU view (a remote desktop, an old driver) the
  backend draws the view instead, from the same grid, a frame each time the
  view stops moving; the box and the colour bar are drawn over it as over the
  GPU view, and onto the stills and frames. A projected map coloured by its
  own column shows no colour bar there: its scale is relative to the column the
  GPU view measures.

### Settings

| Setting | What it does |
|---|---|
| **Mode** | *3-D light*: emission and absorption, in perspective. *Projected map*: the column along each line of sight, without perspective. |
| **Light** | *Values are log₁₀*: the cube holds the logarithm of its quantity, its light is 10 to it. |
| **Dark below the threshold** | Values under the 3-D view's *Threshold* (shown above the settings while this is on) emit nothing. A ray sums every channel it crosses, so a radio cube's noise, a little light in each, adds up to a veil over the source: on by default for a cube with negative values (noise), off for a simulation's grid, whose faint gas is light, not noise. |
| **Contrast** | The light raised to this power: above 1 the faint, diffuse emission fades and the dense structure stands out (2: a gas's density squared). |
| **Colour by**, **Map**, **Log scale** | The colour: of the light's own values, of another field of a VBT volume, or, for a cube whose third axis is spectral, of each channel's *Velocity*, through a colour map. Its range is that of the quantity where there is light (its 1st to 99.9th percentile), not of an empty background. A velocity is in km/s (a frequency axis as optical velocity, cz, from the cube's rest frequency), on a linear scale and over the velocities holding 98 % of the light, so that faint noise peaks in every channel do not stretch it over the whole band; choosing it sets the map to *Coolwarm*, approaching blue and receding red. |
| **Exposure** | In stops over the automatic exposure (the 99.5th percentile of the lit pixels): +1 doubles the light. |
| *More settings:* **Absorption** | Grey absorption, as the optical depth reached where the light is brightest: 0 is emission only; 1 or more lets dense regions hide what lies behind them. Drawn in the GPU view too. |
| **Samples** | Samples per voxel along a ray: more, finer and slower. |
| **Slab**, **Range** | A map's thickness around the focal plane (in voxels), and the decades of column its brightness spans. |
| **Curve**, **Stretch**, **Gamma**, **Saturation** | The tone curve of a 3-D render: *Asinh* and *Logarithmic* lift the faint light, *Exponential* compresses the bright, *Linear* does neither. |

*Box* and *Colour bar* draw the cube's box and the scale of its colours over
the view and onto the stills and frames.

---

## Stills and movies: *Output*

As for particles (see [Ray-traced particles](raytraced-particles#stills-and-movies-output)):
**Render Still** renders the view as it is on screen, from the full cube rather
than the GPU's grid; **Save Image…** writes it as graded, **Save Light…** the
light before the tone curve as a floating-point FITS; **Movie…** renders an
orbit about the view's up axis, encoded as an MP4 with `ffmpeg` when it is
installed. A 512³ cube renders in about a second at 1920 × 1080 on a laptop
(Apple M-series).

---

## See also

- [Cube viewer](cube-viewer) — the 3-D view and its analysis rendering.
- [Ray-traced particles](raytraced-particles) — the same rendering for point tables.
- [Rendering API](../backend-api/volume-render) — the routes behind the stills, movies and GPU view.
