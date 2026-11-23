# Ray-traced particle rendering

Stills, movie frames and the desktop's GPU view of a table of points rendered
as a continuous medium (issue #101; user guide:
[Ray-traced particles](../../user/reference/raytraced-particles.md)). The renderer is
`app/particle_raytrace.py` (numba, no GPU needed); the routes are
`app/routers/particle_render.py`. Every route takes the session token.

## The request

A **render request** is shared by all the routes:

| Field | Meaning |
|---|---|
| `dataset_id` | A VBT point table or a catalogue opened in this session (else 422). |
| `x`, `y`, `z` | Position columns; empty: the table's own (a sky catalogue's computed `@x`/`@y`/`@z`). |
| `sky` | `{frame, distance_mode, cosmology}`: a sky catalogue's positions, as for the point session. |
| `filters` | `[{field, op, value}]`, numeric operators, or `{box}` / `{sphere}` selections, as the point session takes them. |
| `colour` | `{field, points, lut, min, max, log}`: the view's colours (its colour function `[[x, r, g, b], …]`, or a named map). |
| `radius_field`, `radius_scale`, `neighbours` | Each particle's smoothing length: a column, or half the distance to its `neighbours`-th nearest particle; times `radius_scale`. See [Smoothing lengths](#smoothing-lengths). |
| `period` | The period of the three position axes for that neighbour search: `null` the table's own (a periodic Gadget snapshot's `BoxSize`), `0` none, `L` a box of side `L` in the table's units. A box or sphere filter's own `period` applies to the selection only. |
| `intensity_field`, `intensity_log10`, `intensity_power` | The light: a column (holding its log10), raised to `intensity_power`; none: 1 for every particle. See [Light](#light). |
| `quantity` | `per_particle` (the light is an amount, weighted 1/h²) or `density` (weighted h, a column). |
| `absorption` | Grey absorption, as the optical depth where the emitted light is brightest (0: emission only). |
| `mode` | `light` (a 3-D render) or `projected` (a map: no perspective, log density). |
| `slab`, `colour_by`, `velocity` | A map's thickness around the focal plane; its hue (`density`, `colour` for the view's column, `velocity_dispersion` from `velocity` or the columns named VX/VY/VZ). |
| `width`, `height` | Pixels, 16 to 8192. |
| `cameras` | `[{position, focal_point, view_up, view_angle, parallel, parallel_scale}]`, VTK's camera; 1 to 7200 (one: a still). |
| `tone` | `{exposure, ev, curve, stretch, gamma, saturation, decades}`: `exposure` null is automatic (the 99.5th percentile of the lit pixels' luminance); `ev` stops over it; `curve` `asinh`, `log`, `exponential` or `linear`; `decades` a map's brightness range. |

### Smoothing lengths

Without `radius_field`, h is half the distance to the `neighbours`-th nearest
particle (`particle_raytrace.smoothing_lengths`, issue #414). The filters on a
position column (a box or sphere on the position axes, or a comparison of one)
cut the drawn particles. The other filters choose the population the
neighbours come from. A cut's particles find their neighbours in that
population, past the cut's faces. The search reads the population within a
margin of the cut's bounding box (three times the radius that holds
`neighbours` particles at the cut's mean density). Any particle whose
neighbours reach past that margin is searched again within its own distance,
so every h equals the one the whole population gives. On a periodic table
(`period`) the search uses the minimum image (`cKDTree(boxsize)`, positions
wrapped into [0, L)), and a cut across a face is one interval round the box.
The intensity does not change h: rows left out for their light still count
as neighbours. A projected map's `velocity_dispersion` takes its neighbours
the same way (past the cut's faces, round a periodic box). A box over position and other columns is split into the two
kinds of filter. An `outside` box, or a sphere on another column, chooses the
population.

### Light

`intensity_field` is normalised by `particle_raytrace.normalised_light`. The
light is 1 at the 99.5th percentile of the values (their log10), raised to
`intensity_power`. The largest value is not used as the reference, because a
few extreme rows would push every other row below float range. When the
values span more than 20 decades and an empty stretch of more than 10 decades
splits them into two populations (gas and stars in one table), the larger
population sets the reference. The other population is then capped at the
larger one's brightest row, and at least 2 decades above the reference.
Otherwise the light is only capped to keep the sum of every particle's light
within float32. The tone curve sets the contrast. Rows without a usable value
(not finite, zero or negative in linear, or 0 once in float32) are left out.

They are counted. The task result carries `dropped_rows`, `clipped_rows` and
`light_span_decades` (the span of the finite values, in decades). The preview
and particle routes carry `X-Visivo-Particle-Dropped` and
`X-Visivo-Particle-Clipped`. A client shows them when they are not 0. A large
`clipped_rows` with a span above 20 decades means the column mixes two
populations, and the user should filter by type.

## Routes

### `POST /v1/tasks/particle_render`

Renders one frame per camera as a task (`GET /v1/tasks/{task_id}` for its
progress, `DELETE` to cancel between frames). A movie's exposure is fixed on its
first frame. The result: `frames`, `width`, `height`, `particles`, `exposure`
(before `ev`), `seconds`, `hdr` (a still keeps its light), and `dropped_rows`,
`clipped_rows`, `light_span_decades` (see [Light](#light)). Files live in the
workspace's hidden `.renders/{task_id}`.

### `GET /v1/particle_render/{task_id}/frames/{index}`

A frame, PNG. 404 for another token's task, 409 while it runs.

### `POST /v1/particle_render/{task_id}/tone`

Body: a `tone`. The still graded again (PNG), nothing rendered again.

### `GET /v1/particle_render/{task_id}/light.fits`

The still before the tone curve, float32: three planes (red, green, blue), or
for a projected map two (`CTYPE3 = MAP`: the density and its weighted mean).
Only a still (one camera) keeps its light: a movie's task answers 404.

### `DELETE /v1/particle_render/{task_id}`

Removes the task's files, stopping a render still running first:
`{"deleted": true}`. 404 for an unknown task or another token's.

### `POST /v1/particle_render/preview`

One frame for an interactive preview, JPEG, at most 2048 px a side. The
prepared particles of the last two requests per token are cached, so a frame
that only moves the camera is only projected and composited. `max_particles`
draws an even sample, each particle carrying the light of those it stands for.
Headers: `X-Visivo-Exposure` (the exposure before `ev`, the automatic one when
none was given), `X-Visivo-Render-Ms`.

### `POST /v1/particle_render/particles`

The particles as the renderer draws them, for the desktop's GPU view: rows of
little-endian float32, their fields in `X-Visivo-Particle-Fields`
(`x,y,z,r,e,red,green,blue` for a 3-D render: the emission at the kernel's
peak, weighted as `quantity` says; `x,y,z,r,w,v` for a map: the mass at the
peak and the quantity its hue averages), their number in
`X-Visivo-Particle-Count`. Above `max_particles` (default 8 000 000) an even
sample, the step in `X-Visivo-Particle-Step`.

`local_frame` (default `false`) chooses the frame of `x,y,z` and `r`. With
`true`, they are float32 coordinates local to the particles, and the table's
coordinates are `origin + scale × row`, with `origin` from
`X-Visivo-Particle-Origin` (`x,y,z`, float64) and `scale` from
`X-Visivo-Particle-Scale`. These offsets keep particles far from 0 apart, where
1e9, 1e9 + 1 and 1e9 + 2 are a single float32 value. The client moves its
camera into that frame in double precision before drawing: position and focal
point `(v − origin) / scale`, `parallel_scale / scale`, and a map's slab
`/ scale`. With `false` (a client that predates the field), the rows are a
float32 cast of the table's coordinates, as before, and the headers are
`0.0,0.0,0.0` and `1.0`. The scale is 1 unless an axis's offsets cannot keep
their precision (a span beyond float32's range or below 1e-30). It is then
the same on every axis, so the kernels stay round.

### Positions far from 0

Every route reads positions as the remote point session does
(`point_sample.place_rows`, issue #208): float32 offsets from a float64 origin
per axis, at 12 bytes a particle as before. Smoothing lengths, the slab and
the camera are applied in that frame, and the camera is shifted in float64.
Particles at 1e9 + [0, 1, 2] are therefore three in a still, a movie frame and
the preview, at the places where the point session draws them. Cameras, `slab`,
`radius_field` and the box stay in the table's own coordinates. A row whose
position is not finite or lies beyond float32's range (3.4e38) is left out, as
before.

### Camera of a point session

`GET /v1/render/{session_id}/camera` (see [Render](render.md)) returns the camera
of a point session's view, for a still of the view on screen.
