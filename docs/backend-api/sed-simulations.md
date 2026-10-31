# SED fitting and simulations

## SED

Greybody fits of a compact source's photometry, and fits against the VLKB grid
of pre-computed theoretical models. Wavelengths are µm and fluxes Jy throughout
(the units of the Hi-GAL band-merged catalogue the SEDs are built from).

The fit lives here rather than in the desktop client on purpose: the legacy
ViaLactea client shelled out to `python sedfit_main.py` with `eval()`-ed argv
strings, so the result depended on the user's own Python. See
[SED fitting](../user-guide/sed-fitting) for the science and the workflow.

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
[Simulation snapshots](../user-guide/simulations).

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
