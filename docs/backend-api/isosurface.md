# Isosurfaces for the headset and for export

Issue [#370](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/370).
Surfaces of a cube's field at chosen levels, extracted on the backend from the
original data (`app/isosurface.py`), sent to the VR page as a compact mesh and
exportable as PLY or binary glTF. Every route takes the token
(`X-Visivo-Token`); the extraction also takes the session
(`X-Visivo-Session`) that opened the dataset.

## How a surface is made

1. **Level of detail.** With `lod` −1 (the default) the finest level whose
   voxels are at most `max_voxels` (default 512³): level 0 is the cube
   itself, level `l` the cube halved `l` times. A level above 0 is read from
   the cube's brick pyramid when one is complete and current (each voxel the
   mean of its 2^l block, standing for the block's centre,
   `(2^l − 1) / 2` cube voxels on), else every 2^l-th voxel of the cube. No
   pyramid is built here ([brick pyramids](../brick-pyramid.md)).
2. **Block by block.** The level is read in slabs along z of at most 64 MB,
   each sharing its last plane with the next, and each slab is contoured with
   VTK's flying edges. The level is never held whole.
3. **Scale.** On `scale` `log`, log10 of the values is contoured at log10 of
   each level: the surface crosses the same voxel edges as on the values, and
   its vertices are placed along each edge in log space. `auto` is the
   reduced cube's rule (`/v1/cube/reduced_info`): log for a simulation volume
   whose values are positive over more than three decades, else linear.
   Blank voxels (NaN, and values at or below zero on a log scale) count as
   below every level.
4. **Budget.** The slabs' surfaces are joined (the vertices of the planes
   they share merged), then decimated to `max_triangles` (quadric
   decimation; a surface more than three times its budget is first
   clustered to about three times it). A surface of more than 16 M triangles
   before decimation is refused at an asked-for level; with `lod` −1 the next
   coarser level is used instead.
5. **Cache.** Each surface is kept (in memory, 512 MB, least recently used
   first out) under the file's version, the field, the level, the scale, the
   level of detail asked for (and whether a pyramid was there) and the
   budget.

## `POST /v1/isosurface/extract`

```json
{"dataset_id": "…", "field": "", "levels": [1e-6, 3e-6], "scale": "auto",
 "lod": -1, "max_voxels": 134217728, "max_triangles": 200000}
```

| Field | Meaning |
|---|---|
| `field` | Another field of the dataset's source (a PLUTO variable, a VBT field, by its name in `reduced_info`'s `fields`); empty: the dataset's own. |
| `levels` | 1 to 8 distinct finite values in the field's units (above zero on a log scale). |
| `scale` | `auto`, `linear` or `log`. |
| `lod` | −1 automatic, else a level of detail (refused past the cube's levels or past 1024³ voxels). |
| `max_voxels` | The automatic level's voxels at most (8³ to 1024³). |
| `max_triangles` | Each surface's triangles at most (1 000 to 1 000 000). |

Answers a task (`TaskCreateResponse`): `status` `running`, or `completed` with
`cache_hit` true when every level was made already. Follow it with
`GET /v1/tasks/{task_id}` (`progress` 0..1: 70 % contouring the slabs, 30 %
decimating) and cancel it with `DELETE /v1/tasks/{task_id}`. One extraction
runs at a time; the others wait. Its `result`:

```json
{"valid": true, "scale": "log", "cache_hits": 0, "triangles": 400000,
 "meshes": [{"mesh_id": "…", "level": 1e-6, "triangles": 200000, "vertices": 105978,
             "raw_triangles": 708832, "bytes": 3470360, "lod": 2, "source": "pyramid",
             "grid": [512, 512, 512], "factor": 4, "voxel_offset": 1.5,
             "full_shape": [2048, 2048, 2048], "contour_value": -6.0, "scale": "log",
             "read_s": 0.42, "contour_s": 0.46, "decimate_s": 1.56, "seconds": 4.66}]}
```

`meshes` follow the request's `levels`. A level outside the field's values
gives a mesh of 0 triangles. A refused level (no logarithm, unknown field) is
a 422; a surface too large at the asked-for level makes the task `failed`
with the reason.

## `GET /v1/isosurface/mesh/{mesh_id}`

The surface as the headset draws it, `application/octet-stream`,
little-endian:

| Bytes | Content |
|---|---|
| 0–3 | `VISO` |
| 4–7 | uint32 version (1) |
| 8–15 | uint32 vertices `n`, uint32 triangles `m` |
| 16–39 | float32 `lo[3]`, `hi[3]`: the bounding box in box coordinates |
| 40… | `n × 3` uint16 positions, `lo + q / 65535 · (hi − lo)`; zero padding to a multiple of 4 bytes |
| … | `n × 4` int8 normals (x, y, z, 0), `q / 127`, in box coordinates |
| … | `m × 3` uint32 vertex indices |

Box coordinates run 0..1 over the cube's full-resolution extent on each axis
(x, y, z), cube voxel `i` at `(i + 0.5) / N`, as the headset's volume. A
150 000-triangle surface is about 2.6 MB. 404 when the surface has left the
cache: extract it again.

## `GET /v1/isosurface/mesh/{mesh_id}/export`

Query: `format` `ply` (default) or `glb`, `color` (`#rrggbb`, the glTF
material), `save` (`true`: written in the workspace's exports under
`isosurfaces/` and answered `{"valid": true, "path", "bytes", "format"}`).
Otherwise the file itself, as an attachment.

The vertices are in the cube's world coordinates when its three axes are
linear lengths (a simulation volume's `X`, `Y`, `Z`: `CRVAL + (pixel + 1 −
CRPIX) · CDELT`, e.g. a PLUTO run's code units), else (sky or spectral axes,
a PLUTO grid opened on cell indices) in full-resolution voxel indices,
0-based. The PLY header says which (`comment coordinates world` or `voxel`)
and the level, scale and level of detail; the normals follow the coordinates.
PLY: binary little-endian, float `x y z nx ny nz`, faces as `uchar`-counted
`int` lists. glTF 2.0 binary: one mesh, `POSITION`, `NORMAL`, uint32 indices,
a double-sided material; the axes are written as they are (glTF's y up may
need a turn in a viewer).
