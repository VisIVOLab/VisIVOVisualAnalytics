# Velocity Field Viewer

Interactive 3-D viewer for **cosmic-flow / peculiar-velocity fields** — a
3-component vector field on a regular grid (e.g. CosmicFlows-4 reconstructions).
It renders **arrow glyphs** and **streamlines** coloured by speed, plus
**gravitational-basin (watershed)** regions (Laniakea-style superclusters /
voids), with a display-resolution LOD and full-resolution
**region-of-interest (ROI)** fetch so it stays interactive on large (256³/512³)
grids.

This is feature **F1** of the cosmic-flow roadmap.

> Ready-made test data: see [Demo datasets](demo-datasets) —
> `velocity_field.fits` (two-sink field for basins) and the co-registered
> `combo/velocity.fits` + `combo/galaxies.csv` for the galaxy overlay.

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
the same factor, per axis; an axis too short for the factor is reduced less, see
[`grid_bin`](backend-api/velocity.md#post-v1velocitygrid_bin)). The returned
origin is the centre of the first block. The viewer places the LOD grid with
the geometry the backend reports: the per-axis factors of
`X-Visivo-Downsample-Factors` and the `X-Visivo-Origin`/`X-Visivo-Spacing`
headers, rescaled to the viewer's own **Box size** (both frames are centred on
0, so only the cell changes). With an older backend that sends only the single
`X-Visivo-Downsample-Factor`, it applies that factor to every axis that was
reduced and still puts each sample at its block centre. The maths is in
`src/gui/VelocityLodGeometry.h`, unit-tested in
`tests/test_velocity_lod_geometry.cpp`. So glyphs and streamlines drawn from the
LOD line up with a full-resolution ROI, and a thin grid such as `(2, 512, 512)`
keeps its two slices a box apart. This bounds both the transfer and client memory — a raw 512³
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
   voxels (`scipy.ndimage.map_coordinates` trilinear sampling), with a fixed
   step of half a cell along the unit flow direction (velocities converted to
   cells per axis, so an anisotropic grid keeps the physical direction). A
   basin is a property of the streamline geometry, which depends only on the
   direction field: the same field at any overall speed scale gives the same
   basins. An earlier step `min(step, |v|)` compared a speed with a length, so a
   slow but regular flow crawled and was declared converged everywhere (a
   one-attractor field at ×0.001 came out as 8 basins covering 64 of 4913
   voxels).
2. A streamline **ends** at a critical point: when `|v| ≤ eps` (relative to
   the field's median speed), when its direction reverses (it stepped over a
   critical point, which is usually how a fixed step ends at a sink), or when
   it stays in one interior cell for 5 steps (with a fixed step, only while
   circling a critical point). Boundary-pinned, limit-cycle (rotational) and
   truncated paths stay *unassigned* (label 0). NaN voxels are left out of
   the trilinear interpolation (field and validity mask interpolated with NaN
   as 0, velocity renormalised by the valid weight); a sample with less than
   half its weight on valid voxels is missing data, and its path stays
   unassigned too.
3. Endpoints pool into **attractor cells** (endpoint density ≥ threshold);
   `scipy.ndimage.label` (26-connectivity) groups connected cells into
   candidate attractors.
4. Each candidate is kept only if it is a **sink**: six probes started one cell
   away along ±x, ±y, ±z are traced for 8 more cells and must all end back in
   the attractor's cells or within 2 cells of its centre. A saddle sheds the
   probe on its unstable axis, however weak that axis is, so it is rejected:
   on a double well the separatrix plane between the wells stays unassigned
   rather than becoming a third basin, and a pure saddle gives no basin. A
   probe that runs into missing data farther out than it started has escaped
   (so a saddle with blank voxels on its unstable axis is still rejected); one
   that stops at or inside its start distance is neutral. Only if an axis has
   no live probe left is the candidate kept unjudged, so a sink next to a blank
   patch keeps
   its basin. Each voxel inherits the label of the sink its path ended at.

**Box size and non-cubic grids.** The direction used is the physical one:
velocities are divided by the physical cell size per axis. Without a box size
the cells are unit cubes; with one they are `box / (n - 1)` per axis. On a
cubic grid that is a common factor and the labels do not depend on the box. On
a non-cubic grid (for example 64×64×32) the cells are anisotropic once a box is
given, so the box size changes the segmentation, and the viewer must recompute
the basins when the box changes rather than only re-placing them. The same
holds for the ∇·v density. `basins_bin` and `density_bin` take the viewer's
box as `box_mpc` (`0` = unit cells, omitted = the box the field was opened
with), and their spacing/origin headers follow it.

`direction = +1` gives basins of attraction (superclusters); `-1` gives basins of
repulsion (voids / the central repeller) — selectable in the viewer via the
**Basin type** dropdown (Attraction / Repulsion), which re-segments on change. The
viewer renders one semi-transparent
coloured surface per basin (`vtkDiscreteFlyingEdges3D` + a categorical LUT, label
0 transparent) at 0.35 opacity so the streamlines show through. The basin grid
is the field block-averaged to `basin_max_dim`, so the viewer places it like the
LOD grid, each label at its block centre and not stretched end to end over the
box: from the `X-Visivo-Downsample-Factors`, `X-Visivo-Origin` and
`X-Visivo-Spacing` that `basins_bin` sends, rescaled to the box. With an older
backend that sends none of them it uses the backend's `lod_factors` for the
request, checked against the size that came back.

The labels are cached. Changing the box size re-places them without
recomputing when every cell scales by the same factor: any change on a cubic
grid, and a change between two boxes on any grid. On a non-cubic grid a change
to or from "no box" changes the ratio of the cells (unit cells against
box/(n_i - 1)), and with it the flow direction in cells that the segmentation
follows, so the viewer asks for the basins again. Every basins request carries
the viewer's current box as `box_mpc` (0 for unit cells, never left out: an
omitted box would mean the dataset's own box), so the backend segments in the
cells the viewer draws. `basin_max_dim` is clamped server-side so a direct API call can't
force a full-resolution integration sweep.

The status bar reports the basin count, the fraction of volume assigned, and the
largest basins' share of the assigned volume (which supercluster/void dominates).

This is a coarse **visualization heuristic** (fixed-step Euler + reversal/stagnation
convergence on a smoothed, downsampled field), not a publication-grade watershed —
appropriate for interactive exploration, consistent with how the original
analyses work on smoothed fields.

## Density from the velocity divergence

**Overdensity (−∇·v)** renders the *density field implied by the flow*: in linear
theory the density contrast δ ∝ −∇·v, so **converging flow (∇·v < 0) marks
overdensities**. `buildFieldDensity()` runs `vtkGradientFilter`
(`ComputeDivergenceOn`) on the display grid's `velocity` array to get ∇·v, and
`updateFieldDensityContour()` draws a translucent orange `vtkFlyingEdges3D`
isosurface of the convergent cores at *Overdensity level %* of the most-negative
∇·v (higher % → tighter, denser cores). The ∇·v field is cached and
re-contoured on the slider; a box-size change invalidates it (the divergence
depends on the grid spacing) and rebuilds if shown.

**Void (+∇·v)** is the symmetric companion: an isosurface of the *divergent*
cores (∇·v > 0), i.e. underdensities / repellers (the Local Void, the Dipole
Repeller). It shares the ∇·v field and the level slider (which maps its % to each
side's extremum independently), and draws a cool-blue surface. Overdensity and
Void can be shown together — an attractor glows orange, a repeller blue.

For a **downsampled (large) field**, the ≤128³ display LOD is too coarse to
resolve the ∇·v cores, so the viewer fetches a higher-resolution divergence from
the backend (`POST /v1/velocity/density_bin` → `BackendClient::fetchVelocityDensity`,
capped at 256³) and places it at the client box geometry with the same
block-centre placement as the LOD grid (its reported origin and spacing,
rescaled to the box) — full-res for fields ≤256³. The request carries the
viewer's current box as `box_mpc` (0 included), so ∇·v is per the viewer's
cells, as in the local fallback. On a box change the basins' rule applies: if
every cell scales by one factor k the cached grid is rescaled in place
(positions by k, ∇·v by 1/k, so the contours keep their shape); if the ratio of
the cells changes (a non-cubic grid going to or from "no box") it is fetched
again. A full-resolution display computes ∇·v locally with no round-trip. The
backend worker (`velocity_density_worker`) is unit-tested (radial in/outflow →
∇·v = ∓3 at unit spacing).

Because each isosurface is a *level set* of ∇·v, it needs a **structured** field:
a flow with distinct convergence/divergence peaks shows cores at those peaks,
while a uniform-divergence flow (a pure radial infall) has none — the status bar
says so. These are **density proxies** (∝ δ, not calibrated by faH), the "where's
the matter / where's the emptiness" complement to the flow itself.

## Rendering

- **Arrows** — `vtkGlyph3DMapper` (GPU instancing) fed by `vtkMaskPoints` in
  random mode, capped by **Max arrows** (default 50 000) so cost is bounded
  regardless of grid size. Oriented/scaled by the `velocity` vector, coloured by
  the `speed` scalar.
- **Streamlines** — `vtkStreamTracer` (RK45, both directions) seeded on a sphere
  at the grid centre, tubed (`vtkTubeFilter`). Bounded by **Seeds** (default
  200), `MaximumNumberOfSteps = 2000`, propagation `2·extent`.
- **Colour** — shared `ColorMaps` LUT over the full-resolution speed range; the
  scalar bar shows `|v|`.
- **Colour streamlines by vorticity** (sidebar checkbox, default off) — when on,
  `vtkStreamTracer` computes the vorticity and the streamlines are coloured by
  its magnitude `|∇×v|` (via `vtkArrayCalculator`) instead of speed, highlighting
  regions of rotational flow (a cheap cosmic-web / shear indicator).

## Coordinate model

Point-centred: grid points span exactly `[-box/2, +box/2]` per axis, so
per-axis `cell = box/(n-1)` and the centred origin is `-box/2` (or unit spacing,
`-(n-1)/2`, when no box). The display (LOD) cell is `fullCell × downsampleFactor`.
ROI indices map as `idx = round((world − origin)/fullCell)`, clamped per axis —
all computed per axis so non-cubic grids are handled.

## Sidebar controls

The panel is grouped into **Flow** (glyphs / streamlines / display scale),
**Structure** (ROI + derived cosmography: basins, ∇·v overdensity/void), and
**Galaxy overlay** sections, in a scroll area so the (now sizeable) toolkit fits
any window height.

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
| Overdensity (−∇·v) / Void (+∇·v) | density-from-divergence isosurfaces — convergent (orange) / divergent (blue) cores |
| ∇·v level % | shared isosurface level for both (% of peak convergence/divergence) |
| Field bulk flow vs radius | plot the reconstruction's own volume-weighted bulk flow \|⟨v⟩\| in spheres (no catalogue needed; ≈0 for a symmetric field, non-zero for a coherent one) |
| Basin type | attraction (superclusters) vs repulsion (voids) |
| Overlay galaxy catalogue… | load a catalogue and draw its galaxies as points in this field's frame |
| Show galaxies / Galaxy size | toggle the overlay / point size |
| Galaxy color | Uniform, or colour by a numeric attribute / Speed \|v\| |
| Galaxy shape | Points, or Velocity arrows (peculiar-velocity glyphs) when vx/vy/vz exist |
| Galaxy density / Density level % | overdensity isosurface from the galaxies + its level |

## Galaxy overlay (CosmicFlows combo)

*Overlay galaxy catalogue…* draws a galaxy point cloud **in the same frame as
the velocity field** — the "where do galaxies sit in the flow" view. The viewer
reuses its live backend session to open a catalogue (`BackendClient::openCatalogue`
→ `queryTabularCatalogue` → `Catalogue3DParser::parseBackendSubset`, the same path
as the 3-D catalogue viewer, capped at 100 000 rows for context) and adds the
parsed `sceneX/Y/Z` positions as a gold point actor to the field's renderer.

Because the overlay uses **physical Cartesian Mpc** positions, two conditions
matter for alignment, and the viewer guards both:

- **Cartesian positions** — if the catalogue provides X/Y/Z (backend `cartesian`
  always implies them) the parser's Cartesian branch is forced, so a catalogue
  that *also* carries RA/Dec still uses its physical XYZ rather than an
  RA/Dec-derived shell. A non-Cartesian (sky) catalogue is still overlaid but the
  status warns its positions may not match the field frame.
- **Physical box set** — the field is only in Mpc once *Box size (Mpc)* is set;
  while it is `0` (grid units) the overlay asks for confirmation, since Mpc
  galaxies won't align with a unitless grid.

The camera reframes on the first overlay (so the galaxies are visible — or a
zoom-out makes a frame mismatch obvious); later re-overlays keep the current
view. *Show galaxies* toggles visibility and *Galaxy size* sets the point size
live. **Remove overlay** drops all overlay actors (points/arrows, density,
legend) and resets the controls. When Field agreement is computed the status
also reports the **mean field agreement** over the in-box galaxies — a single
number for how well the whole catalogue matches the reconstruction.

**Galaxy color** colours the points by a catalogue attribute rather than a flat
gold. On overlay, a named `vtkFloatArray` is attached to the point cloud for each
numeric column (values via the shared parser accessor), plus a derived
**Speed |v|** = √(vx²+vy²+vz²) when vx/vy/vz are present, and — when vx/vy/vz are
present *and* a field is loaded — two field-comparison scalars from a single
`vtkProbeFilter` sampling of the field at each galaxy (with the galaxies' own
arrays not passed through so the interpolated field velocity isn't shadowed):

- **Field agreement** — cos(angle) between the galaxy's peculiar velocity and
  the field at its position. +1 = flows *with* the reconstruction, −1 = against,
  0 = outside the field box.
- **Residual |v|** — the magnitude |v_galaxy − v_field|: the part of the observed
  motion the reconstruction *fails to explain* (highlights missing attractors /
  bulk flow beyond the model).

After loading, the status bar reports the sample statistics over the in-box
galaxies: **agreement** (mean cosine), **residual** (mean |v_galaxy − v_field|),
and **bulk** (the coherent bulk flow, |mean v_galaxy|) — the last two are genuine
cosmic-flow quantities (bulk-flow amplitude tests ΛCDM). All velocity numbers
assume the field and catalogue share units (km/s in the CosmicFlows data path).

**Bulk flow vs radius…** opens a plot of the bulk-flow amplitude |⟨v⟩| in spheres
of growing radius about the observer (origin) — the Watkins–Feldman–Hudson curve.
Two curves share the *same galaxy selection*: the **galaxies** (observed
peculiar velocities) and the **reconstruction at galaxies** (the field sampled at
each galaxy position, stored as `field_velocity` during the overlay probe). Where
the two diverge, the reconstruction under- or over-predicts the coherent flow at
that scale. (The radius axis is Mpc when a box size is set, else grid units.) Selecting a key drives
the mapper via `SetScalarModeToUsePointFieldData` + `SelectColorArray` through a
blue→red LUT auto-ranged to the attribute; *Uniform* returns to the flat colour.
Re-colouring never re-fetches.

A second **scalar bar** (legend) titled with the attribute appears on the **left**
(clear of the field's speed bar on the right and the orientation-axes marker in
the corner). It shares the galaxy LUT, retitles/reranges as you switch keys, and
is shown only while the galaxies are visible *and* attribute-coloured (hidden on
*Uniform*, when *Show galaxies* is off, or on a fresh overlay).

**Galaxy shape** switches the overlay between **Points**, **Velocity arrows**
(when the catalogue carries vx/vy/vz), and **Residual arrows** (when a field is
also loaded). A `vtkGlyph3DMapper` draws a `vtkArrowSource` per galaxy, oriented
and scaled by the chosen 3-component array — `velocity` (peculiar velocity) or
`residual` (v_galaxy − v_field, the *unexplained* motion). Both arrow modes share
**one physical scale** (from the peculiar-velocity magnitude — largest velocity
arrow ≈ 4 % of the data diagonal), so residual arrows are directly comparable:
short where the reconstruction fits, long and coherent where it misses. This is
the CosmicFlows comparison — individual galaxy motions, and their reconstruction
residuals, against the flow field. Colour still follows
*Galaxy color* (so *Speed |v|* colours the arrows too); *Galaxy size* is a
point-size and is disabled in arrow mode. The overlay keeps both a points mapper
and a glyph mapper on the same point cloud, so shape/colour switch with no
re-fetch.

**Galaxy density** overlays an **overdensity isosurface** built from the galaxies
themselves — where large-scale structure (filaments/clusters) sits relative to
the flow. On first toggle the points are accumulated onto a 48³ grid over the
catalogue bounds, Gaussian-smoothed into a density field (`vtkImageGaussianSmooth`),
and contoured (`vtkFlyingEdges3D`) at *Density level %* of the peak — a translucent
pale-cyan surface you can see the flow through. The grid is cached (rebuilt only
on a new overlay); moving the level slider just re-contours. (All client-side on
the GUI thread — a 48³ smooth of ≤100k points is milliseconds.)

The open/query/parse runs **off the GUI thread** (`QtConcurrent` +
`QFutureWatcher`), so a large catalogue doesn't freeze the UI: the worker
(`fetchCatalogueOverlay`, capturing only value copies, exceptions caught →
`ok=false`) returns the parsed positions + schema, and `onCatalogueOverlayReady()`
builds the VTK actor on the GUI thread. The button is disabled while a fetch runs
(re-entry guarded by `isRunning()`). (Still future: multiple overlays.)

## Multi-field compare

Two velocity fields on the same grid (e.g. CF3 vs CF4, or a reconstruction vs a
simulation) can be differenced cell-by-cell: `POST /v1/velocity/difference_bin`
returns the magnitude of the vector difference **|v_a − v_b|** as a scalar grid,
rendered with the same machinery as the divergence density. The two fields must
share a grid shape (else 422); the worker warns (via `X-Visivo-Warnings`) when
they differ in **unit**, **frame**, or **box size**, since a cell-wise diff is
only physical when both cover the same space. The range headers are the generic
`X-Visivo-Range-Min/Max` (a non-negative magnitude, not a signed divergence).

## Tests

`backend/tests/test_velocity_field.py` covers the loaders, box scaling,
`to_vtk_image`, streamline generation, LOD downsampling, and the `grid_bin` /
`subgrid_bin` routers. `backend/tests/test_velocity_basins.py` covers the basin
segmentation: single sink → 1 basin, two sinks → 2, pure rotation and outward
flow → mostly unassigned (no false basins), downsampling, and the `basins_bin`
router.
