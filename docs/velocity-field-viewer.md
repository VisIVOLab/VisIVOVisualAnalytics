# Velocity Field Viewer

Interactive 3-D viewer for **cosmic-flow / peculiar-velocity fields** — a
3-component vector field on a regular grid (e.g. CosmicFlows-4 reconstructions).
It renders **arrow glyphs** and **streamlines** coloured by speed, plus
**gravitational-basin (watershed)** regions (Laniakea-style superclusters /
voids), with a display-resolution LOD and full-resolution
**region-of-interest (ROI)** fetch so it stays interactive on large (256³/512³)
grids.

This is feature **F1** of the cosmic-flow roadmap.

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
| Basins | `POST /v1/velocity/basins_bin` `{dataset_id, basin_max_dim=64, direction=1}` | int32 watershed label volume + `X-Visivo-N-Basins` |

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

## Gravitational basins (watershed)

Tick **Show basins** to segment the field into *basins of attraction* — the
cosmic-flow definition of a supercluster (Laniakea; Dupuy et al. 2019, Courtois
et al. 2013). Each basin is the set of points whose velocity streamlines
converge to the same attractor.

Algorithm (`app/velocity_basins.py`, computed on a coarse grid — `basin_max_dim`,
default 64, capped at 96):

1. Integrate every grid voxel's streamline through the field, vectorised over all
   voxels (`scipy.ndimage.map_coordinates` trilinear sampling), with an adaptive
   step `min(step, |v|)` along the flow direction.
2. A streamline **converges** when `|v| ≤ eps`, when its direction reverses
   (it stepped past a critical point — robust on coarse grids), or when it
   stagnates in one interior cell. Boundary-pinned, limit-cycle (rotational) and
   truncated paths stay *unassigned* (label 0).
3. Converged endpoints pool into **attractor cells** (endpoint density ≥
   threshold); `scipy.ndimage.label` (26-connectivity) turns connected attractor
   cells into basins; each voxel inherits its sink's basin label.

`direction = +1` gives basins of attraction (superclusters); `-1` gives basins of
repulsion (voids / the central repeller) — selectable in the viewer via the
**Basin type** dropdown (Attraction / Repulsion), which re-segments on change. The
viewer renders one semi-transparent
coloured surface per basin (`vtkDiscreteFlyingEdges3D` + a categorical LUT, label
0 transparent) at 0.35 opacity so the streamlines show through. The labels are
scale-independent and cached, so changing the box size re-places them without
recomputing. `basin_max_dim` is clamped server-side so a direct API call can't
force a full-resolution integration sweep.

The status bar reports the basin count, the fraction of volume assigned, and the
largest basins' share of the assigned volume (which supercluster/void dominates).

This is a coarse **visualization heuristic** (adaptive Euler + reversal/stagnation
convergence on a smoothed, downsampled field), not a publication-grade watershed —
appropriate for interactive exploration, consistent with how the original
analyses work on smoothed fields.

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
| Show basins | gravitational-basin (watershed) regions — Laniakea-style superclusters |
| Basin type | attraction (superclusters) vs repulsion (voids) |

## Tests

`backend/tests/test_velocity_field.py` covers the loaders, box scaling,
`to_vtk_image`, streamline generation, LOD downsampling, and the `grid_bin` /
`subgrid_bin` routers. `backend/tests/test_velocity_basins.py` covers the basin
segmentation: single sink → 1 basin, two sinks → 2, pure rotation and outward
flow → mostly unassigned (no false basins), downsampling, and the `basins_bin`
router.
