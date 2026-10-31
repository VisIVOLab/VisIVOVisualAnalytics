# Velocity fields

These routes serve cosmic-flow velocity fields: a three-component vector grid
(vx, vy, vz) rather than a scalar cube, read from a single FITS file whose
component axis has length 3 (numpy shape `(3, nz, ny, nx)`). The desktop client
fetches the raw vectors and runs streamlines and glyphs locally; derived scalar
grids (basins, divergence, field differences) are computed on the backend. See
the [Velocity-field viewer](../velocity-field-viewer.md) for the user side.

All binary routes return `Content-Type: application/octet-stream` with the
array as the whole body: little-endian values in C order over `(nz, ny, nx)`,
so x varies fastest (the point order of a `vtkImageData`). Vector grids
interleave the three components per point (`npts × 3` `float32`). Shape and
metadata travel in `X-Visivo-*` headers, including `X-Visivo-Width` (nx),
`X-Visivo-Height` (ny), `X-Visivo-Depth` (nz), `X-Visivo-Components` and
`X-Visivo-Byte-Order: little`; float header values are written with Python
`repr`. Unlike the cube routes, `X-Visivo-Full-Shape` here is ordered
`nz,ny,nx`.

Grid geometry is point-centred on the origin. When the field was opened with a
box size `box_mpc`, the spacing per axis is `box_mpc / (n - 1)` and the origin
is `-box_mpc / 2`; without it the spacing is 1 (grid units) and the origin is
`-(n - 1) / 2`. The velocity unit is the file's `BUNIT` (or `km/s (assumed)`),
the frame its `VFRAME` (or `supergalactic (assumed)`).

Errors on the binary routes are `text/plain`: 404 `unknown velocity dataset`
when the `dataset_id` is not a velocity field opened in this session, and 500
with the message for a failed computation. None of these routes takes a
per-session compute slot.

## `POST /v1/velocity/open`

Loads a velocity field, returns its summary and registers it in the session
with kind `velocity` (dataset ids start with `vf_`).

Request (`VelocityOpenRequest`): `path` (server-side path, confined to the
backend's allowed data roots) and `box_mpc` (default `null`: physical box size
in Mpc; without it the grid uses unit spacing).

```json
{ "path": "/data/cf4/CF4_new_64-z008_velocity.fits", "box_mpc": 1000.0 }
```

Response (`VelocityOpenResponse`): `valid`, `error`, `dataset_id`,
`session_id`, and `summary` with:

| Field | Content |
|---|---|
| `shape` | `[nz, ny, nx]` |
| `n_cells` | number of grid points |
| `spacing`, `origin` | per axis (x, y, z), Mpc or grid units |
| `unit`, `frame` | as described above |
| `box_known` | whether `box_mpc` was given |
| `warnings` | geometry caveats: negative `CDELT1..3`, a CD/PC matrix with negative determinant (possible mirror image), an assumed frame, no spatial `CTYPE` |
| `speed_min`, `speed_max`, `speed_mean` | range and mean of the speed over finite cells |
| `finite_fraction` | fraction of cells with a finite speed |

Errors: a path outside the allowed data roots (or a hidden path) returns HTTP
200 with `valid: false` and `error: "Velocity file not found."`; a missing file, a file that is not a 4-D FITS with a length-3
component axis, and other load failures, return `valid: false` with the
message.

## `POST /v1/velocity/grid_bin`

The vector grid for display, optionally block-averaged.

Request (`VelocityGridRequest`): `dataset_id`, `max_dim` (default 128). When
`max_dim > 0` and the largest grid dimension exceeds it, every component is
block-averaged by the integer factor `ceil(largest / max_dim)` (cropping each
axis to a multiple of the factor) and the spacing grows by the same factor.
`0` returns the full resolution.

Response: interleaved `float32` vectors, `nx × ny × nz × 3` values. Headers:

| Header | Content |
|---|---|
| `X-Visivo-Width`, `X-Visivo-Height`, `X-Visivo-Depth` | returned grid size |
| `X-Visivo-Full-Shape` | `nz,ny,nx` before block-averaging |
| `X-Visivo-Downsample-Factor` | applied factor (1 when none) |
| `X-Visivo-Components` | `3` |
| `X-Visivo-Spacing`, `X-Visivo-Origin` | `x,y,z` |
| `X-Visivo-Speed-Min`, `X-Visivo-Speed-Max` | speed range of the returned grid |
| `X-Visivo-Full-Speed-Min`, `X-Visivo-Full-Speed-Max` | speed range at full resolution (block-averaging lowers the maximum) |
| `X-Visivo-Unit`, `X-Visivo-Frame` | velocity unit and frame |

## `POST /v1/velocity/basins_bin`

Gravitational-basin (watershed) segmentation: the streamline from every grid
point is integrated until it converges, and points are labelled by the
attractor they reach. `direction: 1` gives basins of attraction
(superclusters), `-1` basins of repulsion (voids).

Request (`VelocityBasinsRequest`): `dataset_id`, `basin_max_dim` (default 64;
the field is block-averaged to this largest dimension before segmentation; 0
or negative means 64, and the value is clamped to the range 8 to 96),
`direction` (default 1; any value `>= 0` is treated as 1, negative as -1).

Response: an `int32` label grid on the coarse basin grid, 0 for unassigned
points and 1 to N for basins. Headers: `X-Visivo-Width`, `X-Visivo-Height`,
`X-Visivo-Depth` (basin grid size), `X-Visivo-Components: 1`,
`X-Visivo-Scalar-Type: int32`, `X-Visivo-N-Basins`. The spacing and origin of
the basin grid are not sent; they follow from the block-averaging factor and
the field's geometry.

## `POST /v1/velocity/density_bin`

Velocity divergence ∇·v as a density proxy: convergent flow (∇·v < 0) marks
overdensities, divergent flow (∇·v > 0) voids. Derivatives are central
differences (one-sided at the edges) with the physical spacing, computed in
float64. At full resolution, a field whose file size times three exceeds
`VISIVO_VELOCITY_STREAM_BYTES` (default 512 MiB) is processed in z-slabs
(`VISIVO_VELOCITY_SLAB`, default 32) from a memory map, with the same result as
the in-memory computation.

Request (`VelocityDensityRequest`): `dataset_id`, `max_dim` (default 0, full
resolution; `> 0` block-averages first, as in `grid_bin`).

Response: one `float32` scalar per grid point. Headers: `X-Visivo-Width`,
`X-Visivo-Height`, `X-Visivo-Depth`, `X-Visivo-Components: 1`,
`X-Visivo-Scalar-Type: float32`, `X-Visivo-Spacing`, `X-Visivo-Origin`,
`X-Visivo-Div-Min`, `X-Visivo-Div-Max`, `X-Visivo-Unit` (the velocity unit per
`Mpc`, or per `pixel` when the box is unknown), `X-Visivo-Calibrated: 0`, and,
only when the box is known, `X-Visivo-Density-Factor`. The grid is the raw,
uncalibrated ∇·v; a linear-theory overdensity is obtained as
δ ≈ -∇·v × factor, where the factor is 1 / (f H0) with Planck 2018 values
(f = Ωm^0.55, Ωm = 0.3111, H0 = 67.66 km/s/Mpc).

## `POST /v1/velocity/difference_bin`

Cell-wise magnitude of the vector difference |v_a - v_b| between two velocity
fields (for example two reconstructions, or a reconstruction and a
simulation).

Request (`VelocityDifferenceRequest`): `dataset_id_a`, `dataset_id_b` (both
velocity fields in this session), `max_dim` (default 0, full resolution;
`> 0` block-averages both fields first). Each field uses the `box_mpc` it was
opened with.

```json
{ "dataset_id_a": "vf_0123456789ab", "dataset_id_b": "vf_ba9876543210", "max_dim": 128 }
```

Response: one `float32` scalar per grid point. Headers: `X-Visivo-Width`,
`X-Visivo-Height`, `X-Visivo-Depth`, `X-Visivo-Components: 1`,
`X-Visivo-Scalar-Type: float32`, `X-Visivo-Spacing` and `X-Visivo-Origin` (of
field A), `X-Visivo-Range-Min`, `X-Visivo-Range-Max`, `X-Visivo-Unit` (field A's
unit), and `X-Visivo-Warnings` when there is anything to report: the two
fields' own geometry warnings, different box sizes, different units or
different frames, joined with `; ` (Latin-1, single line).

Errors: 404 (`text/plain`) when either id is not a velocity field in the
session, 422 (`text/plain`) when the two grids have different shapes, 500 for
other failures.

## `POST /v1/velocity/subgrid_bin`

A full-resolution region of the field, read through a memory map so only the
region is loaded.

Request (`VelocitySubgridRequest`): `dataset_id`, `x0`, `x1`, `y0`, `y1`,
`z0`, `z1` (full-resolution grid indices). The end index is exclusive here,
unlike the inclusive cube routes. Each start is clamped to `[0, n - 1]` and
each end to `[start + 1, n]`, so the region is never empty.

```json
{ "dataset_id": "vf_0123456789ab", "x0": 96, "x1": 160, "y0": 96, "y1": 160, "z0": 96, "z1": 160 }
```

Response: interleaved `float32` vectors of the region. Headers:
`X-Visivo-Width`, `X-Visivo-Height`, `X-Visivo-Depth` (region size),
`X-Visivo-Components: 3`, `X-Visivo-Roi-X0`, `X-Visivo-Roi-Y0`,
`X-Visivo-Roi-Z0` (clamped start indices, to place the region in the full
grid), `X-Visivo-Full-Shape` (`nz,ny,nx`), `X-Visivo-Speed-Min`,
`X-Visivo-Speed-Max`. Spacing, origin, unit and frame are not sent; the client
places the region from the start indices and the full grid's geometry.
