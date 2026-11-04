# Ray-traced volume rendering

Stills, movie frames and the desktop's GPU view of a cube rendered as light
(issue #106; user guide: [Ray-traced volumes](../user-guide/raytraced-volumes)).
The renderer is `app/volume_raytrace.py` (numba, no GPU needed); the routes are
`app/routers/volume_render.py`. A render is a task of the particles' kind: its
frames, tone, light and deletion are the routes of
[particle rendering](particle-render), by task id. Every route takes the
session token.

## The request

| Field | Meaning |
|---|---|
| `dataset_id` | A cube opened in this session: a FITS cube or a field of a VBT volume, at least two voxels along each axis (else 422). |
| `box` | `[x0, x1, y0, y1, z0, z1]`: the world box the grid's points span (the desktop's view places its cube there); null: the cube's voxel coordinates, 0 to n − 1. |
| `brightness_log10`, `brightness_power` | The light: the cube's values (holding their log10), raised to this power (the contrast); scaled so their 99.5th percentile is 1. |
| `brightness_floor` | Values at or below it emit nothing (the dataset's own values, a logarithm when they hold one): the desktop sends its 3-D view's threshold, so a radio cube's noise, summed over every channel along a ray, does not veil the source. Null: only values at or below 0 are dark. |
| `colour_by`, `colour_field` | The colour: of the brightness (`density`) or of another field of the same VBT volume (`colour`, `colour_field` its index; a FITS cube has none: 422). |
| `colour` | `{lut, points, min, max, log}`: a named map or the client's colour function, over `[min, max]` (null ends: the 1st and 99.9th percentiles of the quantity where there is light). |
| `absorption` | Grey absorption, as the optical depth where the emitted light is brightest (0: emission only). |
| `mode` | `light` (a 3-D render) or `projected` (the column along each line of sight, no perspective). |
| `slab` | A map's thickness around the focal plane, in world units. |
| `quality` | Samples per voxel along a ray, 0.25 to 16 (default 2). |
| `max_voxels` | Larger cubes are read with one stride along every axis to at most this many voxels (default 512³, at most 1024³). The grid's box then ends at its last point, the cube's voxel (m − 1) × stride. |
| `width`, `height`, `cameras`, `tone` | As for [particles](particle-render#the-request). |

## Routes

### `POST /v1/tasks/volume_render`

Renders one frame per camera as a task. The result: `frames`, `width`,
`height`, `voxels`, `colour_range`, `exposure` (before `ev`), `seconds`, `hdr`.
Frames, tone, light and deletion: `/v1/particle_render/{task_id}/…`. A
projected still's light (`light.fits`) has two planes: the column and the
column-weighted mean colour index.

### `POST /v1/volume_render/grid`

The grids the desktop's GPU view marches, prepared as for a render (default
`max_voxels` 256³, at most 512³; `max_axis`, default 2048, bounds each axis
to what the GPU's 3-D textures hold): little-endian, x fastest, the emissivity as float32
(nz · ny · nx), then the colour index in [0, 1] as float16, then the colour
table as 256 float32 RGB. Headers: `X-Visivo-Volume-Shape` (`nz,ny,nx`),
`X-Visivo-Volume-Box` (the world box), `X-Visivo-Colour-Range` (`lo,hi,log`).
The last two grids prepared per client are kept, so the tone and the camera
never prepare them again.

A projected map's colour is the column-weighted mean of the colour index (not
of the quantity), so the GPU view, which averages the same index, shows what a
still saves.

## See also

- [Render sessions](render) — `GET /v1/render/{id}/camera` gives a cube
  session's camera, in the cube's voxel coordinates, for a still of the backend
  view.
