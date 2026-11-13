# Ray-traced particle rendering

Stills, movie frames and the desktop's GPU view of a table of points rendered
as a continuous medium (issue #101; user guide:
[Ray-traced particles](../user-guide/raytraced-particles.md)). The renderer is
`app/particle_raytrace.py` (numba, no GPU needed); the routes are
`app/routers/particle_render.py`. Every route takes the session token.

## The request

A **render request** is shared by all the routes:

| Field | Meaning |
|---|---|
| `dataset_id` | A VBT point table or a catalogue opened in this session (else 422). |
| `x`, `y`, `z` | Position columns; empty: the table's own (a sky catalogue's computed `@x`/`@y`/`@z`). |
| `sky` | `{frame, distance_mode, cosmology}`: a sky catalogue's positions, as for the point session. |
| `filters` | `[{field, op, value}]`, numeric operators, as the point session takes them. |
| `colour` | `{field, points, lut, min, max, log}`: the view's colours (its colour function `[[x, r, g, b], …]`, or a named map). |
| `radius_field`, `radius_scale`, `neighbours` | Each particle's smoothing length: a column, or half the distance to its `neighbours`-th nearest particle; times `radius_scale`. |
| `intensity_field`, `intensity_log10`, `intensity_power` | The light: a column (holding its log10), raised to `intensity_power`; none: 1 for every particle. |
| `quantity` | `per_particle` (the light is an amount, weighted 1/h²) or `density` (weighted h, a column). |
| `absorption` | Grey absorption, as the optical depth where the emitted light is brightest (0: emission only). |
| `mode` | `light` (a 3-D render) or `projected` (a map: no perspective, log density). |
| `slab`, `colour_by`, `velocity` | A map's thickness around the focal plane; its hue (`density`, `colour` for the view's column, `velocity_dispersion` from `velocity` or the columns named VX/VY/VZ). |
| `width`, `height` | Pixels, 16 to 8192. |
| `cameras` | `[{position, focal_point, view_up, view_angle, parallel, parallel_scale}]`, VTK's camera; 1 to 7200 (one: a still). |
| `tone` | `{exposure, ev, curve, stretch, gamma, saturation, decades}`: `exposure` null is automatic (the 99.5th percentile of the lit pixels' luminance); `ev` stops over it; `curve` `asinh`, `log`, `exponential` or `linear`; `decades` a map's brightness range. |

## Routes

### `POST /v1/tasks/particle_render`

Renders one frame per camera as a task (`GET /v1/tasks/{task_id}` for its
progress, `DELETE` to cancel between frames). A movie's exposure is fixed on its
first frame. The result: `frames`, `width`, `height`, `particles`, `exposure`
(before `ev`), `seconds`, `hdr` (a still keeps its light). Files live in the
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
