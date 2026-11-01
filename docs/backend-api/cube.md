# Cube

Slices, previews, sub-volumes, PV diagrams and noise of spectral cubes, and the
binary routes behind the Kinematic Lasso and kinematic modelling. All routes
need the token and the session that opened the dataset.

## `POST /v1/cube/preview`
Downsampled cube for fast initial display.
```json
{ "dataset_id": "...", "downsample": 4 }
```
Returns `scalar_type`, `width`, `height`, `depth`, `range_min`, `range_max` + base64 `data`, and `default_threshold`: the cube's default 3-D threshold, from its noise ([The default threshold](../user-guide/cube-viewer.md#the-default-threshold)), computed on the preview (a stride sample, so its noise is the cube's) for the number of voxels of the whole cube; `null` when the preview has no finite value. The desktop starts its local view from it, and a remote view of the same cube starts from the same rule.

## `POST /v1/cube/slice`
Single 2-D slice.
```json
{ "dataset_id": "...", "axis": "z", "index": 42 }
```

## `POST /v1/cube/subvolume`
Spatial + spectral sub-region.
```json
{ "dataset_id": "...", "x0": 0, "x1": 255, "y0": 0, "y1": 255, "channel_start": 10, "channel_end": 50 }
```

## `POST /v1/cube/pv`
Position-velocity diagram along a polyline.
```json
{
  "dataset_id": "...",
  "vertices": [[12.5, 40.0], [80.25, 41.0], [130.0, 90.0]],
  "width_pixels": 3
}
```
- `vertices` — 0-based cube pixels `(x, y)`; fractional values are honoured
  (rounded to 1/1000 pixel, the same values key the product cache and feed the
  computation). At least two distinct vertices.
- `width_pixels` — samples across the path, one pixel apart, averaged.

Sampling: one sample per step along the path's arc length, in **angular** space
(the full pixel→sky matrix, so non-square and skewed pixels are handled; one
step = the finer pixel); NaN-aware bilinear interpolation. The cube must have
its spectral axis last (else 422 / `valid: false`).

Response: `num_samples`, `depth`, `scalar_type`, `compression`,
`positions_base64` (offsets in steps), `positions_arcsec_base64`,
`pixel_scale_arcsec_per_pixel` (= arcsec per step), `spatial_unit`,
`data_base64` (`depth × num_samples` float32, channel-major), `width_pixels`,
`vertex_count`, `total_length` (pixels), `valid_samples`, `step_pixels`,
`spectral_axis_type`, `spectral_axis_unit`,
`spectral_coordinates_base64` (float64 little-endian, **channel 0 first**, may
decrease), `spectral_crval`, `spectral_cdelt`, `sky_matrix_arcsec`
(row-major 2×2, pixel offset → projection-plane arcsec), `bunit`,
`beam_major`, `beam_minor`, `beam_pa`.

## `POST /v1/cube/noise`
Noise statistics (MAD and sigma) over a spatial region and channel range.
```json
{ "dataset_id": "...", "x0": 0, "x1": 127, "y0": 0, "y1": 127, "channel_start": 0, "channel_end": 63 }
```
Returns `channel_start`, `channel_end`, `num_channels`, `mad[]`, `sigma[]`, `region`.

## Binary and mask routes

The `*_bin` routes below return binary bodies in one of two layouts.

**Header-described volumes** (`/v1/cube/subvolume_bin`, `kinematic_model_bin`,
`kinematic_levels_bin`, `kinematic_select_bin`). The response has
`Content-Type: application/octet-stream` and the body is the bare array with no
in-band header: little-endian samples in C order over `(depth, height, width)`,
so x varies fastest (the point order of a `vtkImageData`). Shape and metadata
travel in `X-Visivo-*` response headers: `X-Visivo-Width`, `X-Visivo-Height`,
`X-Visivo-Depth`, `X-Visivo-Scalar-Type` (`float32` or `uint8`) and, for the
float32 routes, `X-Visivo-Byte-Order: little`. Floating-point header values are
written with Python `repr`, so they round-trip exactly. Errors raised inside the
computation come back as `text/plain` bodies carrying the message (not the JSON
`detail` envelope); errors raised before it (unknown dataset, wrong dataset
kind, request validation) use the usual FastAPI JSON `detail`.

**12-byte frames** (`/v1/moment/result/binary`, `/v1/pv/result/binary`). The
body is a 12-byte big-endian header of three `uint32` values, `rows`, `cols`
and `flags` (always 0), followed by `rows × cols` little-endian `float32`
values in row-major order, with NaN for masked or invalid pixels. This is the
layout `BackendClient::parseBinaryFrame()` reads.

Request bodies that carry a mask (`mask_apply_bin`, `kinematic_fit_bin`,
`kinematic_residual_bin`) use the same wire as the `kinematic_select_bin`
response: a raw little-endian `float32` array of `width × height × depth`
values (x fastest, a voxel is selected when its value is above 0.5), with its
origin and dimensions in query parameters. The server reads the raw body and
checks only its length (`width × height × depth × 4` bytes, else 422); it does
not inspect the `Content-Type`.

Common errors: 404 when `dataset_id` is not open in the session, 422 when the
dataset is not a spectral cube. Routes that take a per-session compute slot
answer 429 when all slots stay busy (`VISIVO_MAX_CONCURRENT_TASKS`, default 3,
waited for up to `VISIVO_TASK_SLOT_WAIT_S`, default 15 s); routes on the heavy
pool answer 504 when the computation exceeds `VISIVO_HEAVY_TIMEOUT_S` (default
900 s). Each entry says which of these apply.

### `POST /v1/cube/subvolume_bin`

Binary variant of `/v1/cube/subvolume`: reads the voxel box
`[z0..z1, y0..y1, x0..x1]` (0-indexed, inclusive) and streams it as raw
`float32` with no base64 and no compression. The bounds are clamped to the
cube.

Request (`CubeSubvolumeRequest`):

```json
{ "dataset_id": "ds_1a2b3c4d5e6f", "x0": 0, "x1": 255, "y0": 0, "y1": 255, "z0": 100, "z1": 163 }
```

Response: the box as little-endian `float32`. Headers: `X-Visivo-Width`,
`X-Visivo-Height`, `X-Visivo-Depth` (of the clamped box),
`X-Visivo-Scalar-Type: float32`, `X-Visivo-Range-Min` and `X-Visivo-Range-Max`
(finite range of the box), `X-Visivo-Spectral-Axis-Type`,
`X-Visivo-Spectral-Axis-Unit` (empty when the spectral WCS cannot be read),
`X-Visivo-Bunit`, `X-Visivo-Byte-Order: little`.

This route does not take a per-session compute slot, so a client can issue
several slab requests in parallel without hitting 429.

Errors: 404, 422 (not a cube, or a box that is empty after clamping:
`Invalid subvolume ROI.` as `text/plain`), 500 (`text/plain`) for other
failures.

### `POST /v1/cube/kinematic_model_bin`

Builds a tilted-ring model PPV cube of a rotating disc with the same spatial
and spectral grid as the dataset, for overlay on the data (see
[Kinematic model](../kinematic-model.md)). Each sky pixel carries a
unit-amplitude Gaussian line at its line-of-sight velocity; the cube is not
renormalised.

Request (`CubeKinematicModelRequest`, all floats must be finite):

| Field | Type | Notes |
|---|---|---|
| `dataset_id` | string | |
| `x0`, `y0` | float | disc centre, pixels |
| `vsys` | float | systemic velocity |
| `sigma` | float | line dispersion, `> 0` |
| `pa_deg` | float or null | position angle, image-pixel convention (0 along +y, increasing toward +x) |
| `incl_deg` | float or null | inclination, 0 to 90 |
| `vrot` | float or null | flat rotation velocity |
| `rmax_pix` | float or null | disc radius in pixels, `> 0` |
| `rings` | list | optional per-ring profile, each `{r_pix > 0, pa_deg, incl_deg in [0, 90], vrot}` |

Without `rings`, all four scalar parameters (`pa_deg`, `incl_deg`, `vrot`,
`rmax_pix`) are required (422 otherwise). With `rings`, the scalar ones are
ignored, the radii must be distinct, and the disc extends to the outermost
ring.

Velocities are in the unit the worker reports in `X-Visivo-Spectral-Axis-Unit`.
A velocity axis is converted to km/s; a frequency axis with `RESTFRQ` (or
`RESTFREQ`) becomes optical velocity (cz) in km/s, the same axis the kinematic
fit uses; any other usable axis stays in its native unit; when the spectral WCS
is unusable the axis is the channel index.

```json
{ "dataset_id": "ds_1a2b3c4d5e6f", "x0": 128.0, "y0": 120.5, "vsys": 1450.0,
  "sigma": 10.0, "pa_deg": 35.0, "incl_deg": 60.0, "vrot": 180.0, "rmax_pix": 60.0 }
```

Response: the model cube as little-endian `float32` (full cube dimensions).
Headers: `X-Visivo-Width`, `X-Visivo-Height`, `X-Visivo-Depth`,
`X-Visivo-Scalar-Type: float32`, `X-Visivo-Peak` (maximum model value),
`X-Visivo-Spectral-Axis-Unit` (`km/s`, the native unit, or `channel`),
`X-Visivo-Spectral-Mode` (`velocity`, `native` or `channel`),
`X-Visivo-Byte-Order: little`.

Errors: 404, 422 (validation, not a cube), 413 (`text/plain`) when the cube
exceeds the model voxel cap (`VISIVO_KINEMATIC_MAX_VOXELS`, default
268,435,456 voxels), 429, 504, 500 (`text/plain`).

### `POST /v1/cube/save_subregion`

Crops the cube to `[z0..z1, y0..y1, x0..x1]` (0-indexed, inclusive, clamped to
the cube), writes it as a FITS file in the Workspace Exports directory and
registers it as a new cube dataset in the session. The header is copied from
the source with `NAXISn` updated, `CRPIX1/2/3` shifted by the crop origin and a
`HISTORY` card recording the crop.

Request (`CubeSaveSubregionRequest`): `dataset_id`, `x0`, `x1`, `y0`, `y1`,
`z0`, `z1` (int), `output_basename` (default `""`). An empty basename becomes
`<source stem>_subcube_x<x0>-<x1>_y<y0>-<y1>_z<z0>-<z1>.fits`. The basename is
reduced to its final path component, leading dots are stripped, `.fits` is
appended when it has no extension, and an existing name gets a `_1`, `_2`, ...
suffix instead of being overwritten.

Response (`CubeSaveSubregionResponse`): `valid`, `error`, `new_dataset_id`,
`output_path`, `workspace_filename`, `width`, `height`, `depth`, `range_min`,
`range_max`.

Errors: 404, 422 (not a cube), 429. A box that is empty after clamping, and
other worker failures, return HTTP 200 with `valid: false` and the message in
`error`.

### `POST /v1/cube/mask`

Blanks (sets to NaN) the voxels inside or outside a box and registers the
result as a new cube dataset, persisted in Workspace Exports. The output keeps
the full cube dimensions, header and WCS: the source file is copied and the
copy is edited in place through a memory map, so masking does not load the
cube into memory. A `HISTORY` card records the mode and the box.

Request (`CubeMaskRequest`): `dataset_id`, `x0`, `x1`, `y0`, `y1`, `z0`, `z1`
(int, inclusive, clamped), `mode` (`"blank_outside"`, the default, isolates
the box; `"blank_inside"` removes it; anything else is 422),
`output_basename` (default `""`, giving
`<stem>_mask-<inside|outside>_x..._y..._z....fits`; same sanitising as
`save_subregion`).

```json
{ "dataset_id": "ds_1a2b3c4d5e6f", "x0": 40, "x1": 90, "y0": 35, "y1": 80,
  "z0": 12, "z1": 48, "mode": "blank_outside" }
```

Response (`CubeMaskResponse`): `valid`, `error`, `new_dataset_id`,
`output_path`, `workspace_filename`, `width`, `height`, `depth`, `range_min`,
`range_max`. The range is that of the kept data, measured on the box for
`blank_outside` and on a strided sample of the cube for `blank_inside`.

Errors: 404, 422 (not a cube, invalid mode), 429, 504. An empty box and other
worker failures return HTTP 200 with `valid: false`.

### `POST /v1/cube/kinematic_levels_bin`

Geodesic level fields for the interactive Reach slider of the
[Kinematic Lasso](../kinematic-lasso.md). The backend grows the selection once,
up to a budget, and returns two `uint8` fields over a shared bounding box; the
client then derives the selection at any integer budget `r` locally as
`add <= r` and not `sub <= r`, with no further request.

Request (`CubeKinematicSelectRequest`, shared with `kinematic_select_bin` and
extended by `mask_selection`):

| Field | Default | Limits | Meaning |
|---|---|---|---|
| `dataset_id` | | | |
| `seed_z`, `seed_y`, `seed_x` | required | inside the cube, on a finite voxel | the clicked seed voxel |
| `add_seeds` | `[]` | each `[z, y, x]` | extra seeds whose selections are added (union) |
| `sub_seeds` | `[]` | each `[z, y, x]` | seeds whose selections are carved out; a sub-seed on a blank voxel is skipped |
| `reach` | 8.0 | `> 0`, finite | geodesic cost budget |
| `lambda_v` | 1.0 | `> 0`, finite | weight of a step along the velocity axis (1 is isotropic) |
| `sigma` | 0.0 | `>= 0` | intensity scale of the cost; 0 uses a robust (MAD) noise estimate of the cube |
| `nsigma` | 2.5 | `> 0` | threshold, in `sigma` units, of the emission component the grow is confined to |
| `roi_radius` | 0 | 0 to 256 | 0 grows over the whole connected object (inside a 256-voxel half-size safety window around the seed); `> 0` confines the grow to a half-box of this size |
| `reach_max` | 0.0 | 0 to 254 | budget of the level field; 0 uses `reach` |
| `smooth_v`, `smooth_xy` | 0.0 | 0 to 16 | Gaussian smoothing (sigma in voxels, velocity and both spatial axes) applied before the threshold; 0 is off |

The budget is `reach_max` when it is positive, else `reach`; a budget above
254 is rejected (the field is `uint8` with 255 as the unreachable sentinel).
Smoothing affects only which voxels are selected, never the values written by
the apply routes.

```json
{ "dataset_id": "ds_1a2b3c4d5e6f", "seed_z": 57, "seed_y": 131, "seed_x": 140,
  "add_seeds": [[60, 98, 152]], "sub_seeds": [], "reach": 8.0, "reach_max": 40.0,
  "lambda_v": 1.0, "nsigma": 2.5 }
```

Response: two `uint8` fields of `width × height × depth` bytes each,
concatenated (the `add` field first, then `sub`), in the volume layout above.
A voxel's level is the ceiling of its geodesic distance, so for an integer
budget `r` the test `level <= r` is exact. Headers:

| Header | Content |
|---|---|
| `X-Visivo-Width`, `X-Visivo-Height`, `X-Visivo-Depth` | dimensions of the shared bounding box |
| `X-Visivo-Origin` | `x,y,z` of the box in cube voxels |
| `X-Visivo-Full-Shape` | `width,height,depth` of the cube |
| `X-Visivo-Scalar-Type` | `uint8` |
| `X-Visivo-Components` | `2` |
| `X-Visivo-Range-Max` | highest usable integer budget (floor of the budget) |
| `X-Visivo-Peak` | the unreachable sentinel level (255) |
| `X-Visivo-Trim-Box` | 0, or the half-width of the seed-centred box the domain was cut to when the object exceeded the 4,000,000-node graph cap (the field is then exact for that box only) |

Errors: 404, 422 (validation, not a cube, and as `text/plain`: seed outside
the cube or on a blank voxel, budget above 254), 429, 504, 500 (`text/plain`).

### `POST /v1/cube/kinematic_select_bin`

Grows the Kinematic Lasso selection at a single budget and returns it as a
binary mask, for the isosurface preview.

Request: `CubeKinematicSelectRequest` (see `kinematic_levels_bin`). The mask is
thresholded at `reach`. When `reach_max > 0` it sizes the faint-margin shell
around the object exactly as in `kinematic_levels_bin`, so the preview and the
level field are built on the same graph; smoothing is honoured.

Response: the selection's bounding box as little-endian `float32` (1 selected,
0 not), not the full cube. Headers: `X-Visivo-Width`, `X-Visivo-Height`,
`X-Visivo-Depth` (box), `X-Visivo-Origin` (`x,y,z`),
`X-Visivo-Full-Shape` (`width,height,depth` of the cube),
`X-Visivo-Scalar-Type: float32`, `X-Visivo-Selected-Count` (selected voxels),
`X-Visivo-Bbox` (`x0,x1,y0,y1,z0,z1`, inclusive), `X-Visivo-Trim-Box` (as in
`kinematic_levels_bin`), `X-Visivo-Byte-Order: little`. When nothing is
selected the count is 0, the bounding box is all zeros and the body is a single
zero voxel.

The body together with its origin and dimensions is exactly the input that
`mask_apply_bin`, `kinematic_fit_bin` and `kinematic_residual_bin` accept.

Errors: 404, 422 (validation, not a cube, and as `text/plain`: seed outside the
cube or on a blank voxel), 429, 504, 500 (`text/plain`).

### `POST /v1/cube/mask_selection`

Re-grows a Kinematic Lasso selection from its seeds and blanks the voxels
inside or outside it, registering the masked cube as a new dataset in
Workspace Exports. Like `/v1/cube/mask` it keeps full dimensions and WCS, edits
a copy in place, and adds a `HISTORY` card (mode, seed counts, `reach`,
`lambda_v`).

Request (`CubeMaskSelectionRequest`): every field of
`CubeKinematicSelectRequest` plus `mode` (`"blank_outside"`, the default,
isolates the selection; `"blank_inside"` removes it) and `output_basename`
(default `""`, giving `<stem>_lasso-<inside|outside>.fits`). `reach_max`, when
positive, sizes the margin shell as in the preview. `smooth_v` and `smooth_xy`
are accepted but not used by this route; to apply a smoothed or hand-edited
selection exactly as previewed, use `mask_apply_bin`.

Response: `CubeMaskResponse` (see `/v1/cube/mask`).

Errors: 404, 422 (validation, not a cube), 429, 504. An empty selection, a seed
outside the cube or on a blank voxel, and other worker failures return HTTP 200
with `valid: false`.

### `POST /v1/cube/mask_apply_bin`

Applies an explicit selection mask, sent in the request body, without
re-growing from seeds, so a hand-refined mask is written exactly as shown. The
masked cube is registered as a new dataset in Workspace Exports.

Query parameters:

| Parameter | Default | Notes |
|---|---|---|
| `dataset_id` | required | |
| `origin_x`, `origin_y`, `origin_z` | required | mask box origin in cube voxels |
| `width`, `height`, `depth` | required | mask box dimensions, each `>= 1` |
| `mode` | `blank_outside` | or `blank_inside` |
| `output_basename` | `""` | empty gives `<stem>_lasso-<inside|outside>.fits` |
| `recipe` | `""` | at most 4000 characters, `key=value; key=value`; each item is written to the output header as a `HISTORY` card prefixed `Lasso ` |

Body: the mask as raw little-endian `float32`, `width × height × depth` values,
x fastest (the `kinematic_select_bin` wire).

```
POST /v1/cube/mask_apply_bin?dataset_id=ds_1a2b3c4d5e6f&origin_x=118&origin_y=96&origin_z=40&width=48&height=52&depth=30&mode=blank_outside&recipe=seed=(57,131,140);%20reach=8
Content-Type: application/octet-stream

<299,520 bytes: 48 × 52 × 30 float32 values>
```

`blank_outside` keeps only the selected voxels; `blank_inside` blanks them.
The output keeps the full cube dimensions and WCS, and a `HISTORY` card records
the mode, the selected-voxel count and the origin.

Response: `CubeMaskResponse` (see `/v1/cube/mask`).

Errors: 422 for an invalid `mode` or a body whose length is not
`width × height × depth × 4` bytes (both checked before the dataset lookup),
404, 422 (not a cube), 429, 504. An all-zero mask, a box that does not fit
inside the cube, and other worker failures return HTTP 200 with
`valid: false`.

### `POST /v1/cube/kinematic_fit_bin`

Measures and fits a selection on the raw cube: integrated flux, line widths,
geometry from the moment-0 map, a flat-disc tilted-ring fit and a per-ring
fit, model velocity fields and their residuals against the observed velocity
field. This is the input of the Kinematic Lasso comparator.

Query parameters: `dataset_id`, `origin_x`, `origin_y`, `origin_z`, `width`,
`height`, `depth` (as in `mask_apply_bin`), plus `sigma` (default 0.0, `>= 0`:
the noise used to cut the velocity field; 0 uses 1.4826 × MAD over every
finite voxel of the cube) and `min_ring_pix` (default 150, `>= 10`: minimum
pixel count of an annulus in the per-ring fit). Body: the selection mask
(`kinematic_select_bin` wire). A mask box that extends past the cube is
clipped to it.

The spectral axis must be the cube's third axis and expressible in km/s: a
velocity axis is used as it is, a frequency axis needs `RESTFRQ` and is
converted to optical velocity (cz).

Response (JSON, NaN and infinite values replaced by `null`):

| Field | Content |
|---|---|
| `width`, `height` | size of the 2-D maps (cube width and height) |
| `velocity_unit`, `velocity_convention` | `km/s`, and the convention used |
| `sigma`, `sigma_source` | noise used, and `"noise estimate"` or `"robust, whole cube"` |
| `beam_area_px` | beam area in pixels, 0 when the unit is not per beam or there is no beam |
| `measures` | `flux`, `flux_is_jy`, `w20`, `w50`, `vsys_w20`, `n_velocity_field` |
| `geometry` | `x0`, `y0`, `pa_morph`, `axis_ratio`, `incl`, `rmax` (from moment 0) |
| `flat` | `pa`, `vsys`, `vrot`, `vrot_sini`, `rms` |
| `rings` | list of `{r, pa, incl, vrot, n_pix}` |
| `line_sigma`, `channel_width` | line dispersion used for the model cubes, mean channel width (km/s) |
| `residual_flat`, `residual_warp` | `{n_pix, median, p90, within_channel}`; `residual_warp` is `null` with fewer than 3 rings |
| `spectrum`, `spectrum_velocity` | integrated spectrum of the selection and its velocity axis |
| `velocity_field_base64` | observed velocity field (moment 1), `height × width` little-endian `float32`, base64 |
| `model_flat_base64`, `model_warp_base64` | model velocity fields, same encoding; `model_warp_base64` is `""` with fewer than 3 rings |

Errors (JSON `detail`): 422 for a body of the wrong length, 404, 422 (not a
cube, an empty selection, a selection outside the cube, a spectral axis that is
not the third axis or cannot be expressed in km/s), 413 when the cube exceeds
`VISIVO_KINEMATIC_MAX_VOXELS` (default 268,435,456 voxels), 429, 504.

### `POST /v1/cube/kinematic_residual_bin`

Residual of a given model (fitted or edited in the comparator) against the
selection's velocity field, with the same selection wire and noise rule as
`kinematic_fit_bin`.

Query parameters: `dataset_id`, `origin_x`, `origin_y`, `origin_z`, `width`,
`height`, `depth`, `sigma` (default 0.0) as in `kinematic_fit_bin`, plus the
model, all finite:

| Parameter | Notes |
|---|---|
| `x0`, `y0`, `vsys` | required |
| `line_sigma` | required, `> 0` |
| `pa`, `incl`, `vrot`, `rmax` | single-disc model; `incl` in [0, 90], `rmax > 0`; all four required when `rings` is empty |
| `rings` | `r:pa:incl:vrot,r:pa:incl:vrot,...` (at most 4000 characters); each `r > 0`, `incl` in [0, 90], radii distinct; when given, the scalar model is ignored |

Body: the selection mask.

Response (JSON, NaN replaced by `null`): `width`, `height`, `sigma`,
`sigma_source`, `velocity_convention`, `residual` (`{n_pix, median, p90,
within_channel}`), `model_base64` (model velocity field) and
`velocity_field_base64` (observed), both `height × width` little-endian
`float32` in base64.

Errors (JSON `detail`): 422 for a body of the wrong length, a malformed
`rings` string, duplicate radii, or a missing single-disc parameter; then as
`kinematic_fit_bin` (404, 422, 413, 429, 504).

### `POST /v1/cube/save_channel_2d`

Extracts one channel of a cube as a standalone 2-D FITS image (`NAXIS=2`, not a
degenerate 3-D cube) in Workspace Exports and registers it as a new `image`
dataset. The header carries the celestial WCS of the source (written without
WCS when it cannot be extracted), `BUNIT`, `OBJECT`, `TELESCOP`, `INSTRUME`
when present, the channel's spectral coordinate as `SPECVAL` (with `SPECTYPE`
and `SPECUNIT` from `CTYPE3` and `CUNIT3`) when `CRVAL3`, `CRPIX3` and `CDELT3`
exist, and a `HISTORY` card.

Request (`CubeSaveChannel2dRequest`): `dataset_id`, `channel` (0-indexed),
`output_basename` (default `""`, giving `<stem>_ch<NNNN>.fits` with a
four-digit channel number).

Response (`CubeSaveChannel2dResponse`): `valid`, `error`, `new_dataset_id`,
`output_path`, `workspace_filename`, `width`, `height`, `channel`,
`range_min`, `range_max`.

Errors: 404, 422 (not a cube), 429. A channel out of range and other worker
failures return HTTP 200 with `valid: false`.

### `POST /v1/moment/result/binary`

Moment map as a 12-byte frame (`rows = height`, `cols = width`). It shares the
computation and the product cache of `/v1/products/moment` and
`/v1/tasks/moment`, so an identical request is served from the cache.

Request (`MomentProductRequest`): `dataset_id`, `moment_order` (required),
`channel_start` and `channel_end` (default 0 and 0; inclusive, clamped to the
cube), `mask_enabled` (default false), `threshold_value` (default 0.0),
`threshold_auto` (default false: when true with `mask_enabled`, the threshold
is derived from the data as median + 3σ via MAD), `moment2_variance` (default
false: order 2 returns the velocity dispersion; true returns the variance).

```json
{ "dataset_id": "ds_1a2b3c4d5e6f", "moment_order": 1, "channel_start": 20,
  "channel_end": 80, "mask_enabled": true, "threshold_auto": true }
```

Response: one 12-byte frame of `float32`. Header `X-Visivo-From-Cache`:
`true` or `false`. The unit and spectral metadata are not in the binary
response; the JSON routes return them.

Errors: 404, 422 (a `ValueError` from the computation, for example a channel
range with `channel_start > channel_end` or a dataset that is not a cube), 504,
500 for other failures (all as JSON `detail`). This route does not take a
per-session compute slot.

### `POST /v1/pv/result/binary`

Position-velocity diagram as a 12-byte frame (`rows = depth`, the number of
channels; `cols = num_samples` along the path). It shares the computation and
cache of `/v1/cube/pv` and `/v1/tasks/pv`.

Request (`CubePvRequest`): `dataset_id`, `vertices` (list of `[x, y]` cube
pixel positions, 0-based, fractional values honoured; at least two distinct
vertices), `width_pixels` (default 1; values below 1 are treated as 1).

```json
{ "dataset_id": "ds_1a2b3c4d5e6f", "vertices": [[30.5, 40.0], [200.0, 180.25]], "width_pixels": 3 }
```

Response: one 12-byte frame of `float32` and the `X-Visivo-From-Cache` header.
Sample positions, the arcsec scale and the spectral axis are only in the JSON
`/v1/cube/pv` response.

Errors: 404, 422 (not a cube, fewer than two distinct vertices or another
`ValueError`), 504, 500. No per-session compute slot.
