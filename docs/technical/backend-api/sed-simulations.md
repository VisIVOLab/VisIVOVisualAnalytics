# SED fitting and simulations

## SED

Greybody fits of a compact source's photometry, and fits against the VLKB grid
of pre-computed theoretical models. Wavelengths are µm and fluxes Jy throughout
(the units of the Hi-GAL band-merged catalogue the SEDs are built from).

The fit lives here rather than in the desktop client on purpose: the legacy
ViaLactea client shelled out to `python sedfit_main.py` with `eval()`-ed argv
strings, so the result depended on the user's own Python. See
[SED fitting](../../user/reference/sed-fitting.md) for the science and the workflow.

### `GET /v1/sed/defaults`
The fit ranges and constants the server suggests, so the client does not carry a
second copy of them.
Returns `mass_range`, `temp_range`, `beta_range`, `lambda0_range`,
`scale_range` (each `[min, max, steps]`), `kappa_ref`, `lambda_ref_um`,
`distance_pc`, `models_service_url`.

### `POST /v1/sed/fit`
Fit a modified blackbody. `model` is `thin`, `thick`, or `both`.
```json
{
  "model": "both",
  "wavelengths_um": [70, 160, 250, 350, 500, 870],
  "fluxes_jy":      [12.5, 40.1, 38.0, 22.4, 11.9, 2.1],
  "errors_jy":      [1.2, 4.0, 3.8, 2.2, 1.2, 0.3],
  "upper_limits":   [false, false, false, false, false, true],
  "distance_pc": 1500,
  "kappa_ref": 0.1,
  "lambda_ref_um": 300,
  "colour_correction": true,
  "size_arcsec": 30.0,
  "mass_range":    [1, 5000, 30],
  "temp_range":    [5, 50, 30],
  "beta_range":    [2, 2, 1],
  "lambda0_range": [10, 300, 30],
  "scale_range":   [1, 1, 1],
  "source_label": "HIGALBM000.0000+00.0000"
}
```

Every range is `[min, max, steps]`; **one step holds that parameter fixed at its
minimum**. Ranges left out fall back to the values `/v1/sed/defaults` reports.
An all-zero `errors_jy` should be omitted rather than sent — it means "no
errors", not "zero uncertainty".

Response: `{valid, error, fits[], best}`. `best` names the model with the lower
reduced χ² (`both` runs two fits; a thick fit that cannot run — no source size —
is skipped rather than failing the request). Each entry of `fits` carries:

| Field | Meaning |
|-------|---------|
| `model` | `thin` or `thick` |
| `parameters` | `mass_msun`, `temperature_k`, `beta`, and for thick `lambda0_um`, `size_arcsec`, `scale`; plus `luminosity_lsun` and `l_over_m` |
| `uncertainties` | Half-width of the Δχ² = 1 profile interval, per parameter |
| `parameter_bounds` | The interval itself, `[lo, hi]` |
| `unbounded` | Parameters whose interval ran off the end of the searched range — their uncertainty is a **lower bound** |
| `at_boundary` | Parameters whose best value sits on the edge of the range |
| `chi2`, `dof` | dof is `N_detections − N_free_parameters` and **can be ≤ 0** |
| `chi2_reduced` | `null` when `dof ≤ 0`: an under-determined fit has no reduced χ². Do not read a missing value as 0 |
| `model_wavelength_um`, `model_flux_jy` | The fitted SED, sampled 5–2000 µm |
| `band_wavelength_um`, `fitted_flux_jy` | The model at the input bands, **in the order they were sent** |
| `luminosity_lsun` | Integrated over the sampled range only, so a lower bound on L_bol |
| `bands_used`, `bands_upper_limit` | How many bands entered the χ², how many vetoed models |
| `warnings` | Free text the caller should surface: unbounded intervals, missing errors, dof ≤ 0, refinement that did not settle |

Upper limits are excluded from the χ² and **veto** any model brighter than the
limit. 422 on an unusable SED: fewer than two detections, a non-positive
distance / opacity / reference wavelength, a non-physical range, or a grid too
large to evaluate.

### `POST /v1/sed/models`
Fit against the VLKB grid of theoretical models. Proxies the VLKB `searchd`
service, whose query is a single underscore-joined positional string.
```json
{
  "wavelengths_um": [70, 160, 250, 350, 500],
  "fluxes_jy": [12.5, 40.1, 38.0, 22.4, 11.9],
  "errors_jy": [1.2, 4.0, 3.8, 2.2, 1.2],
  "flags": [1, 1, 1, 1, 0],
  "distance_pc": 1500,
  "prefilter": 0.0,
  "weight_mid_ir": 1.0, "weight_far_ir": 1.0, "weight_submm": 1.0,
  "delta_chi2": 1.0,
  "service_url": "",
  "max_models": 200
}
```
`service_url` defaults to `VISIVO_VLKB_SEDFIT_URL`. Response:
`{valid, error, columns[], models[], best, total_models, skipped_models,
truncated}` — models sorted by χ², each carrying the service's own physical
columns plus `model_wavelength_um` / `model_flux_jy`. Rows with no usable χ²
are dropped and counted in `skipped_models`; non-finite numbers anywhere in a
row become `null` rather than breaking serialisation.

The URL is validated against SSRF **on every hop**, not only the first: a public
service is otherwise free to answer `302` with a loopback or link-local address.
503 when no service is configured, 400 on a refused URL, 502 when the service
fails or answers something that is not a model table.

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_VLKB_SEDFIT_URL` | *(unset)* | Base URL of the VLKB SED-fit service |
| `VISIVO_VLKB_ALLOWED_HOSTS` | *(unset)* | Comma-separated host allowlist: when set, only these hosts are accepted (and they skip the private-address check) |
| `VISIVO_SED_MODELS_TIMEOUT` | 300 s | Wall-clock deadline for the whole model-fit request |
| `VISIVO_SED_MODELS_MAX_BYTES` | 64 MiB | Cap on the service's response |
| `VISIVO_SED_MAX_MODELS` | 2000 | Server-side cap on `max_models` |

---

## Simulations

AREPO snapshots: browse one, cost an extraction, run it. See
[Simulation snapshots](../../user/reference/simulations.md).

### `POST /v1/simulations/arepo/inspect`
`{path}` → `{valid, error, path, tree, box_size_kpc, unit_length_cm,
particle_types, tree_truncated, yt_available}`. `tree` is nested
`{name, kind, shape, dtype, attributes, children}`. Needs only h5py, so it works
on a backend that cannot run an extraction — `yt_available` says which that is.
External links are named, never followed; the walk is bounded in depth and node
count and reports `tree_truncated`.

### `POST /v1/simulations/arepo/plan`
`{path, level, window_kpc, offset_kpc}` → the grid the extraction *would*
produce: `{shape, cell_size_pc, window_kpc, extent_kpc, offset_kpc,
domain_width_kpc, level, base_dimensions, cells, bytes_estimate}`.

A covering-grid cell is `domain / (base · 2^level)` — the level fixes the
resolution and the window only says how many cells to take — so `extent_kpc`
(what will be extracted) differs from `window_kpc` (what was asked for) by the
rounding to whole cells. 422 on a non-physical window, a window larger than the
box, or a grid past the cell limit.

### `POST /v1/simulations/arepo/extract`
`{path, level, particle, fields, window_kpc, offset_kpc}` →
`{valid, error, plan, outputs: [{field, particle, path, bunit, shape}],
domain_width_kpc}`. One FITS per field in a directory of its own, so re-running
with a different window cannot overwrite an earlier result.

Each cube carries a linear WCS in parsec centred on the window, `BUNIT` from
yt's units, and the provenance of the extraction (`AREPOSRC`, `AREPOLVL`,
`AREPOPRT`, `AREPOFLD`, `AREPOW*` extracted extent, `AREPOR*` requested window,
`AREPOO*` offset).

503 when `yt` is not installed; 422 for a cosmological snapshot (comoving
coordinates are not handled), a non-scalar field, or an unusable grid. The route
goes through the heavy-task throttle, so several extractions cannot saturate the
backend.

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_AREPO_MAX_CELLS` | 600,000,000 | Largest grid an extraction may materialise |
| `VISIVO_AREPO_MAX_LEVEL` | 14 | Highest refinement level accepted |

### `POST /v1/simulations/pluto/cartesian/plan`
`{path, shape?, max_voxels?, mirror?}` → `{valid, error, path, plan,
index_reason}`; `path` is a PLUTO variable reference, a data file or the run's
`.out`. `plan` is `{geometry, mode, axes, lower, upper, shape, cell, mirror,
interpolation, voxels, bytes_estimate}`: the Cartesian view of a polar,
cylindrical, spherical or stretched output. `mode` is `cube` (x, y, z), `plane`
(x, y), `meridional` (x, z; axisymmetric) or `grid` (a stretched Cartesian
run); `shape` is in FITS axis order. Without `shape` the voxel is the finest
cell along the run's length directions, coarsened to `max_voxels` (default
2^26; an explicit `shape` may go to 2^30). `mirror` applies to `meridional`
only. Reads `grid.out` only. 422 for a 1-D run, an unknown geometry or a bad
`shape`.

### `POST /v1/simulations/pluto/cartesian`
`{path, fields?, shape?, max_voxels?, mirror?}` → `{valid, error, plan,
outputs: [{field, path, shape}], source_geometry}`. `fields` omitted: the
variable `path` names; `["*"]`: every cell variable on disk. One FITS per
variable (`<var>.pluto-cartesian.fits`) in a directory of its own under the
exports directory. The resampling is trilinear in the native coordinates (cell
centres half-way between the edges; periodic across a full φ circle; constant
within the half cell at the domain edge), computed in chunks of 2^20 voxels
straight into the file with the source memory-mapped, so memory does not
depend on the output size. Voxels outside the domain are NaN. The header
carries `PLUTOSRC`, `PLUTOGEO`, `PLUTOVEW`, `PLUTOMOD`, `PLUTOINT`, `PLUTOMIR`
and the variable's `PLUTOVAR`, `PLUTOTIM`, `PLUTOSTP`, `PLUTONUM`.

A file named `*.pluto-cartesian.fits` whose `PLUTOSRC` names a PLUTO variable
takes that variable's physical units (`app.physical_units.pluto_ref_of`): the
run's `definitions.h` or what the user typed for it, with coordinates in
length. Heavy-task throttle, as AREPO's. Implementation:
`app/compute/pluto_cartesian.py`.

## AMR outputs (issue #550)

Views of an AMR mesh that need no code's file layout: they read
`app.amr_source.AmrHierarchy` (RAMSES now; the producers of #554 add
`open_hierarchy`). Paths are a RAMSES output (folder, a file of it or a
`::ramses-family=cells` reference); every file of it must lie inside the data
roots (403 otherwise). Implementation: `app/compute/amr_views.py`.

### `POST /v1/simulations/amr/plan`
`{path, field?, level?, lo?, hi?, region_unit?, max_voxels?}` → `{valid, error,
path, code, plan}`. `field` is a hydro variable (default `density`), `mass`
(the gas mass in a voxel, Msun) or `level`. `lo`/`hi` are the region's corners
(three numbers each; none: the whole run) in `region_unit` `box` (a fraction of
the box, default) or `physical` (the cube's own axes, kpc counted from the
box's corner); the grid is the region snapped outward to the lattice of the
target level. `level` omitted: the finest level the region holds that fits
`max_voxels` (default 2^26; at most 2^30, the PLUTO Cartesian view's limits).
`plan` is `{field, level, shape [x, y, z], voxels, bytes_estimate, voxel_size,
voxel_size_physical, lower, upper, lower_physical, upper_physical, length_unit,
value_unit, fits, max_voxels, hard_max_voxels, max_level_within_limit, reason?,
passes, slab_planes, peak_memory_bytes_estimate, finest_level_in_region,
levels, fields, extent}`. `finest_level_in_region` counts a coarse cell that
only reaches into the box (one pass over the mesh geometry, under 0.4 s on 16
million leaves; the mesh's finest level, free, with no region): it is the level
"refine this box" asks for. A cube above `max_voxels` is planned with
`fits: false` and a `reason`; above 2^30, 422. 422 also for an unknown field, a
level above the mesh's finest, an empty region.

### `POST /v1/simulations/amr/uniform`
Same request. Resamples the field onto the plan's grid: a leaf at or coarser
than the target level fills the voxels it covers; the leaves finer than a voxel
give their volume-weighted mean (`level`: the finest level among them), so
the gas mass `density x voxel volume` is conserved at every level. A region and
no `level` is "refine this box": the finest level it holds, if it fits. Heavy
job (the throttle and wall-clock timeout of AREPO's and PLUTO's). Answer
`{valid, error, plan, outputs: [{field, path, shape}], finest_level}`;
422 with the plan's `reason` when the cube does not fit. One FITS per call,
`<field>-L<level>.fits` in a folder of its own under `amr-uniform/` of the
exports directory, opening like any volume: axes `X`/`Y`/`Z` in the run's
length unit (proper at the output's scale factor; `CRVAL` the first voxel's
centre from the box's corner), `BUNIT` the physical unit of the field (the
info file's: g/cm^3, km/s, erg/cm^3, Msun), and the cards `AMRSRC`, `AMRCODE`,
`AMRFLD`, `AMRLEV`, `AMRMODE` (`mean` or `max`), `AMRFCT` (code value x
`AMRFCT` = value), `AMRLMAX`, `AMRLO1..3`/`AMRHI1..3` (the grid's box in box
units), `AMRBOXL` (length units per box unit), `AEXP`, `REDSHIFT`, `AMRTIME`.
Voxels no leaf reaches are NaN. Memory is one slab (at most 2^24 voxels, 9
bytes each) plus a leaf block, whatever the cube's size: the run's passes
over the mesh are `plan.passes`. Measured on `output_00185` (15,974,645
leaves, levels 7 to 13): the density cube at level 9 (512^3, 537 MB) in 4.3 s
with a peak resident set of 1.3 GB, mostly the memory-mapped files.

### `POST /v1/simulations/amr/level-map`
`{path, axis, coordinate, unit?, lo?, hi?, region_unit?, max_rects?}` →
`{valid, error, path, axis, coordinate, unit, plane_axes, levels, counts,
cells, truncated, area, window_area, to_physical, length_unit}`. The leaf cells
that the plane `axis = coordinate` cuts (a plane on the edge between two cells
is the upper one's), as rectangles `[u0, v0, u1, v1]` in the other two axes
(`plane_axes`, x, y, z order) in `unit` (`box`, or `physical`: the cube's own
axes), grouped by level in `levels` (`{"9": [[...], ...]}`). `counts` are all
the cells per level, `area` their total area inside the window (`window_area`
when the mesh tiles it). `max_rects` (default 200,000) keeps the coarsest
levels and sets `truncated`. A slice of the whole `output_00185` is 44,000
cells, in 0.35 s. 422 for a plane outside the run or an axis that is not x, y
or z.

### Cells as points
The leaf cells also open as a particle component, `<output
folder>::ramses-family=cells` ("gas cells", after the particle families in
`components`; a mesh with gas and no particles opens as it): columns `x y z dx
level mass density vx vy vz pressure metallicity` (the hydro names the output
gives), `dx` the cell side and `mass = density dx^3` in box units, both derived
(from `level`, `density`), the rest read in one pass when first asked for.
`describe()` names `size_column: "dx"`; the ray-traced particle renderers
(`/v1/particles/render`, layers) then draw each cell as a kernel of the rms
width half its side per axis (a Gaussian of h = dx/sqrt(2); a Gaussian of
that width per axis sums to a flat sheet within 1.4 % over a lattice, where
the cube's own rms, dx/sqrt(12), would ripple by 39 %) instead of estimating a
size from the 32 nearest neighbours; a `radius_field` in the request still wins.
Units through `physical_units.scales()`: lengths in kpc, density in g/cm^3,
mass in Msun (the factor includes the box length cubed). The total gas mass of
`output_00185` is the same from the cells (sum of `mass`) and from the cubes
(density times voxel volume) at levels 7, 8 and 9, within 2e-10 (float32
rounding of the cube).
