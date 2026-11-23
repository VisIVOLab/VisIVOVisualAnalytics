# Several particle components in one picture

Gas, dark matter, stars and black holes of one snapshot drawn together, each
with its own quantity, colour map, window, intensity and visibility (issue
#394). The maths is in `app/particle_layers.py`, the routes in
`app/routers/particle_layers.py`. Each layer's particles are prepared as a
[single-layer render](particle-render.md) prepares them (positions, smoothing
lengths, filters), so every format behind `app/point_table.py` works: the
particle types of a Gadget snapshot, a VBT point table, a catalogue (CSV,
IPAC, VOTable, FITS). Every route takes the session token.

## How a picture is made

1. **Maps.** Each visible particle layer becomes two float maps with the
   renderer's Gaussian kernels and no absorption:
   - the **column density** `Σ`: each particle's weight `m` (its mass) spread
     over a Gaussian of its smoothing length `h`, peak column `m / (π h²)`
     (renormalised for the kernel's cut at 2.5 h, so the map holds all the
     mass). The map is the weight per unit area along each ray, in the
     table's own units: for a Gadget snapshot the code's mass unit per length
     unit squared (by default 10¹⁰ M☉/h per (kpc/h)², comoving in a
     cosmological run; physical: × h / a²). It does not depend on how
     many particles carry the mass: a run at eight times the resolution gives
     the same map (the rule of issue #457). Without a mass column the weight is
     1 per particle and the map is a number density, which does depend on the
     particle count;
   - the **mass-weighted mean** of the layer's `quantity_field` along each ray,
     of its linear values (`quantity_log` takes the log after averaging); a
     particle without a finite value is left out of the mean; NaN where
     nothing lies.
2. **Light.** A layer's brightness `b` in [0, 1] is `Σ` through the layer's
   **window** `range = [bottom, top]` on its `scale`:
   - `log`: `log(Σ / bottom) / log(top / bottom)`;
   - `linear`: `(Σ - bottom) / (top - bottom)`;
   - `asinh`: `asinh(Σ / bottom) / asinh(top / bottom)`, where `bottom` is the
     softening;

   clipped to [0, 1]. Without a quantity the colour is the colour map at `b`,
   and nothing is drawn where `b = 0`. With a quantity the hue is the map at
   `foot + (1 - foot) f`, with `f` the mean quantity through `quantity_range`
   (log10 when `quantity_log`) and `foot` the layer's `hue_foot` (0.3: a map's
   dark foot would darken faint pixels twice; 0 for a map that does not start
   dark, such as `RdYlBu_r`), times `b`. The light is that colour times
   `intensity`.
3. **Composite**, back to front in the order of `layers`:
   - `emission`: `C ← C + L`. Emission layers add up in any order, and the
     composite is the sum of the single-layer pictures;
   - `absorption`: a slab in front of every layer listed before it, with
     optical depth `τ = opacity · Σ / top`. It dims what lies behind it and
     adds its own light as a uniform slab does:
     `C ← C e^-τ + L (1 - e^-τ) / τ` (`L` as `τ → 0`). Layers listed after it
     are in front of it and are not dimmed. To keep dense gas from hiding the
     stars, list the stars after the gas.

   This is a compositing convention, not radiative transfer. The layers are
   whole images, so one layer is entirely in front of or behind another and
   their particles do not interleave in depth. The attenuation is grey, and
   `τ` is tied to the window's top rather than to a physical opacity. Exact
   depth interleaving of emission and absorption is the single-layer
   renderer's (Dolag et al. 2008, `particle_raytrace.py`).
4. **Finish.** A pixel above 1 in a channel is scaled down as a whole, so its
   hue is kept; then `gamma` (default 1: colour maps are already display
   colours).
5. **Markers** (`kind: "markers"`, black holes) are drawn last, on top of the
   composite. Each particle is a ring or a disc whose radius in pixels is
   `min_px + (max_px - min_px) t`, with
   `t = log(m / m_bottom) / log(m_top / m_bottom)` clipped to [0, 1]. Every
   marker gets `max_px` when the window holds one mass. Markers have the same
   size at any zoom and in every frame. The window is the layer's `range`;
   when that is unset, it runs from the smallest to the largest mass of all
   the layer's particles. At most the 20 000 most massive in view are drawn.

**Windows are fixed, never automatic per frame.** A layer without a `range`
(or a `quantity_range`) is measured once, on the first frame. For `Σ`, the top
is the 99.9th percentile of the lit pixels; the bottom is the 1st percentile
(`log`), 0 (`linear`) or the median (`asinh`), at most 6 decades below the
top. For the quantity, the 1st to the 99th percentile. These windows are
returned so the client can send them back. A movie keeps its first frame's
windows. Two panes that should compare must use the same windows.

## The request

| Field | Meaning |
|---|---|
| `dataset_id` | The dataset of the layers that name none: a snapshot's particle type, a VBT, a catalogue. |
| `layers` | 1 to 16 layers, back to front (see below). |
| `width`, `height` | Pixels, 16 to 8192 (preview: 640 × 360 by default, at most 2048). |
| `cameras` | VTK cameras as for [particles](particle-render.md#the-request); one is a still. |
| `gamma` | Of the finished picture, default 1. |

A **layer**:

| Field | Meaning |
|---|---|
| `name` | Shown in errors, results and FITS extension names. |
| `kind` | `particles` (default) or `markers`. |
| `dataset_id` | Another dataset of the session (a component opened on its own). |
| `gadget_type` | 0 to 5: that particle type of the request's Gadget snapshot (0 gas, 1 dark matter, 4 stars, 5 black holes). 422 if the request's dataset is not a snapshot. |
| `type_field`, `type_value` | The rows where `type_field == type_value`: one type of a table that mixes them (a VBT, an IPAC table). |
| `filters` | More filters, as the single-layer request takes them. |
| `x`, `y`, `z` | Position columns; empty: the table's own. |
| `radius_field`, `radius_scale`, `neighbours` | The Gaussian width `h` of `exp(-d²/h²)`: a column times `radius_scale`, or half the distance to the `neighbours`-th nearest particle of the layer. A Gadget `hsml` is the cubic spline's support `H`; `radius_scale` 0.39 matches the spline's spread (σ = 0.274 H), and 0.5 matches the neighbour estimate. |
| `weight_field` | The weight. If empty, the table's mass column is used (`mass`, `Mass`, `MASS`; for markers `bh_mass` first, the black hole's own mass rather than its dynamical one), else 1 per particle. `m` and `M` are not guessed: in a catalogue they are mostly magnitudes. |
| `quantity_field`, `quantity_log`, `quantity_range`, `hue_foot` | The column whose mass-weighted mean sets the hue; its window; where the hue starts on the map (default 0.3). |
| `colormap` | A map name the point viewer knows (Inferno, Viridis, Gray, `afmhot`, any matplotlib name), or control points `[[t, r, g, b], …]` with `t`, `r`, `g`, `b` in [0, 1]. |
| `scale`, `range` | The brightness curve and window of `Σ` (for markers, the mass window). `log` and `asinh` windows, and mass windows, must start above 0. |
| `intensity` | 0 to 100, default 1. |
| `visible` | False: the layer is not drawn and does not absorb. |
| `composite` | `emission` (default) or `absorption`. |
| `opacity` | An absorption layer's optical depth at the top of its window (0 to 50, default 1). |
| `marker` | `{min_px, max_px, style: ring/disc, colour: [r, g, b], opacity}`; sizes 0.5 to 64 px. |

## Routes

### `POST /v1/tasks/particle_layers`

Renders one frame per camera as a task. Progress, cancellation, frames and
deletion go through the single-layer routes:
`GET /v1/particle_render/{task_id}/frames/{index}`,
`GET /v1/particle_render/{task_id}/light.fits` (the linear composite, RGB),
`DELETE /v1/particle_render/{task_id}`. `POST /v1/particle_render/{task_id}/tone`
answers 422 for a layered render; use `compose` instead. The result holds
`frames`, `width`, `height`, `seconds`, `hdr` and `layers`:
`[{name, particles, weight, range, quantity_range}]`, where `weight` is the
column used or `"count"` and the windows are those used. A layer that cannot
be drawn fails the task with its name in the error.

### `POST /v1/particle_layers/preview`

One frame as JPEG, for following a drag. The body is a request with one camera
and an optional `max_particles`: at most that many particles per particle
layer, as an even sample, each carrying the mass of the particles it stands
for. A marker layer is never sampled. The headers are `X-Visivo-Layer-Windows`
(JSON `[{"range", "quantity_range"}, …]`, one entry per layer; send them back
as each layer's `range` / `quantity_range` so the brightness stays the same as
the view turns) and `X-Visivo-Render-Ms`. Windows measured on a draft
(`max_particles`) are noisier than the full map's: their top runs about
10 % high at 10⁵ particles and about 30 % high at 2 × 10⁴. Take the windows
from a preview without `max_particles` before a still or a movie. Each layer's particles are prepared
once and kept, keyed by the fields that choose and weigh them, so a camera
move draws only. The last view's maps are kept per layer, so a change of
colour map, window, intensity, composite or visibility composes them again
without drawing a particle. 422 when a layer cannot be drawn.

### `POST /v1/particle_layers/{task_id}/compose`

A still composed again with other styles, as PNG, without rendering again.
The body is `{layers, gamma}`: the same number of layers, with the same fields
that choose the particles (`kind`, the source, `type_*`, `filters`, axes,
radius, `weight_field`, `quantity_field`). A different source is answered
with 409. Colour maps, windows, intensities, composites, opacities and
visibility may change. A layer whose window was measured is measured again
when its `scale` (or its `quantity_log`) changes and no window is given. A
layer hidden in the render has no maps and stays hidden. A marker layer may change its colour, style and opacity, but not its
sizes (409).

### `GET /v1/particle_layers/{task_id}/maps.fits`

A still's maps before any colour: one image extension per particle layer drawn
(`EXTNAME` = its name), with two planes: the column density and the
mass-weighted mean quantity. `WEIGHT` names the weight column used (or
`count`), and `MAPUNIT` gives the first plane's unit. Rows run bottom up. 404 for a movie or a
single-layer task.

### `GET /v1/particle_layers/presets`

`{"presets": [{name, label, gadget_type, layer}]}`: common looks as data
(`gas-density-hot`, `gas-temperature`, `gas-dust` (absorbing),
`dark-matter-blue`, `dark-matter-grey`, `stars-white-yellow`, `black-holes`).
`layer` holds the style fields a preset sets. `gas-dust` also sets a filter,
`temperature < 10⁵ K`, because hot gas such as a cluster's ICM holds no dust.
`gadget_type` is the type the preset is meant for, a suggestion only. In a
zoom-in, types 2 and 3 hold the low-resolution dark matter, which the type-1
presets leave out.

## For a client

- Build the layer list from the snapshot's components (`components` of the
  dataset, issue #31) or from a type column, apply a preset per layer, and
  keep the list order as the drawing order.
- Preview with `max_particles` while the view moves, and without it when it
  stops. Keep the returned windows and send them back. For a still or a
  movie, send the same layers to the task.
- Restyle a still with `compose`, and save a look per component as the
  layer's style fields.
