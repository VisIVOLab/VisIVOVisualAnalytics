# Velocity Field Viewer

Interactive 3-D viewer for **cosmic-flow / peculiar-velocity fields** — a
3-component vector field on a regular grid (e.g. CosmicFlows-4 reconstructions).
It renders **arrow glyphs** and **streamlines** coloured by speed, with a
display-resolution LOD and full-resolution **region-of-interest (ROI)** fetch so
it stays interactive on large (256³/512³) grids.

This is feature **F1** of the cosmic-flow roadmap. The next phase (F1-2) is
gravitational-basin / watershed segmentation (Laniakea-style).

## Opening a velocity field

**Data ▸ Open Velocity Field…** → pick a FITS file from the backend file
browser. The viewer opens once the grid is fetched.

Accepted input (backend `app/velocity_field.py`):

- **Multi-component FITS** — a single FITS whose component axis is
  `NAXIS4 = 3`, i.e. numpy shape `(3, nz, ny, nx)` holding `vx, vy, vz`. CF4
  reconstructions follow this layout (`CF4_new_64-z008_velocity.fits` →
  `(3, 64, 64, 64)`).
- **Three separate FITS** — `vx.fits` / `vy.fits` / `vz.fits`, one component
  each (`load_component_fits`, not yet wired to a menu action).

Reconstruction FITS often carry no WCS, box size or units; the viewer therefore
treats the grid as unitless by default and exposes the physical scale as a UI
parameter (see *Box size*).

## Architecture (backend serves, desktop renders)

The desktop fetches the raw vector grid and runs all VTK locally so seeding,
the box scale and the ROI stay interactive.

| Step | Endpoint | Returns |
| --- | --- | --- |
| Open | `POST /v1/velocity/open` `{path, box_mpc?}` | `dataset_id`, `session_id`, summary (shape, full-res speed range) |
| Display grid | `POST /v1/velocity/grid_bin` `{dataset_id, max_dim=128}` | LOD vector grid (binary) + `X-Visivo-*` headers |
| Full-res ROI | `POST /v1/velocity/subgrid_bin` `{dataset_id, x0,x1,y0,y1,z0,z1}` | full-resolution sub-region (binary) |

The binary body is **interleaved little-endian float32** vectors, `npts × 3`
(`vx, vy, vz` per point, point order x-fastest = `vtkImageData` order). Response
headers carry `X-Visivo-Width/Height/Depth`, `X-Visivo-Components` (3),
`X-Visivo-Full-Shape` (pre-LOD `nz,ny,nx`), `X-Visivo-Downsample-Factor`,
`X-Visivo-Speed-Min/Max` (this grid) and `X-Visivo-Full-Speed-Min/Max`
(full-resolution field — used for the colour range so a full-res ROI never
clips). The desktop binding is `BackendClient::openVelocityField` /
`fetchVelocityGrid` / `fetchVelocitySubgrid`; the viewer is
`src/gui/vtkWindowVectorField.{h,cpp}`.

## Level of detail (large grids)

`/v1/velocity/grid_bin` block-averages the field so its largest dimension is
`≤ max_dim` (default **128**), preserving the physical extent (spacing grows by
the same factor). This bounds both the transfer and client memory — a raw 512³
field is ~1.6 GB, its 128³ LOD ~50 MB. The summary line shows `LOD ÷N of M³`
when downsampling is active.

A field already `≤ 128³` (e.g. the 64³ CF4 sample) is served at full resolution
(`downsample_factor = 1`) and the ROI controls are disabled — a full-res ROI
would only re-fetch the same data.

## Region of interest (full-res on demand)

When the display is a LOD, tick **Define ROI** to show an interactive box
(`vtkBoxWidget2`): drag its face handles to resize, faces to move. **Load
full-res ROI** converts the box's world bounds to full-resolution grid indices,
fetches that sub-region with `subgrid_bin` (memmap-sliced on the backend, so
only the ROI is read from disk) and renders full-res glyph/streamline actors
positioned exactly inside the LOD context. Changing the box size clears the ROI
(its placement depends on the scale).

The cube and image viewers already have the equivalent (`cube/subvolume_bin`,
image preview→full), so ROI-on-demand was only added here.

## Rendering

- **Arrows** — `vtkGlyph3DMapper` (GPU instancing) fed by `vtkMaskPoints` in
  random mode, capped by **Max arrows** (default 50 000) so cost is bounded
  regardless of grid size. Oriented/scaled by the `velocity` vector, coloured by
  the `speed` scalar.
- **Streamlines** — `vtkStreamTracer` (RK45, both directions) seeded on a sphere
  at the grid centre, tubed (`vtkTubeFilter`). Bounded by **Seeds** (default
  200), `MaximumNumberOfSteps = 2000`, propagation `2·extent`, vorticity off.
- **Colour** — shared `ColorMaps` LUT over the full-resolution speed range; the
  scalar bar shows `|v|`.

## Coordinate model

Point-centred: grid points span exactly `[-box/2, +box/2]` per axis, so
per-axis `cell = box/(n-1)` and the centred origin is `-box/2` (or unit spacing,
`-(n-1)/2`, when no box). The display (LOD) cell is `fullCell × downsampleFactor`.
ROI indices map as `idx = round((world − origin)/fullCell)`, clamped per axis —
all computed per axis so non-cubic grids are handled.

## Sidebar controls

| Control | Effect |
| --- | --- |
| Arrows / Streamlines | toggle each actor set |
| Max arrows | cap on glyphs drawn (uniform subsample) |
| Arrow scale | multiplies arrow length |
| Seeds | streamline seed count |
| Box size (Mpc) | physical scale; `0` = grid units. Live — rescales without re-fetching |
| Colormap | speed LUT |
| Define ROI / Load full-res ROI | full-resolution region fetch (LOD grids only) |

## Tests

`backend/tests/test_velocity_field.py` covers the loaders, box scaling,
`to_vtk_image`, streamline generation, LOD downsampling, and the `grid_bin` /
`subgrid_bin` routers.
