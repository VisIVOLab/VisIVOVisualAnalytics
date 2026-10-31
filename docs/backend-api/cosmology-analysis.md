# Cosmology and analysis

Distances and power spectra, full-resolution cube statistics, HI products
and skeletons.

## Cosmology

### `POST /v1/cosmology/distance`
Single redshift.
```json
{ "redshift": 0.5, "model": "Planck18" }
```

### `POST /v1/cosmology/distance/batch`
```json
{ "redshifts": [0.1, 0.5, 1.0], "model": "Planck15" }
```
Returns `distances_Mpc[]` in the same order. Supported models: `Planck18`,
`Planck15`, `Planck13`, `WMAP9`. Up to 100 000 redshifts; a negative or
non-finite entry comes back as `null` at its position rather than as 0 Mpc,
which would be indistinguishable from a genuine z = 0.

### `GET /v1/cosmology/distances`
The same quantities for one redshift plus lookback time and the age of the
universe, for an arbitrary flat-or-open `LambdaCDM` (`H0`, `Om0`, `Ode0` as query
parameters; omitted ones default to Planck18). No client binding yet — the
desktop only offers the four named models above.

### `POST /v1/cosmology/angular_power/{dataset_id}` · `GET /v1/cosmology/powerspectrum/{dataset_id}`
Azimuthally averaged 2-D power spectrum of an image plane (the GET is the
spec-conformant alias of the POST; same worker).

It is a **relative texture spectrum, not a calibrated Cℓ**: a Hann-windowed
`|FFT|²` with the window's power loss corrected, no mask or beam deconvolution.
`calibrated` is `false` in every response for that reason.

Returns `k_bins` (pixel⁻¹), `ell`, `cl` / `power`, `cl_err`, `n_modes`,
`mode_coupling`, plus:

| field | meaning |
| --- | --- |
| `ell_is_angular` | whether `ell` is an angular multipole at all. `false` when the header carries no celestial WCS — or when its celestial axes are not the plane that was sliced out — and `ell` then repeats `k_bins`. |
| `pixel_scale_arcsec` | the scale the conversion used, `null` when there was none. |

The distinction matters: `spatial_pixel_scale_arcsec()` answers 3600″ for a
header with no WCS (astropy's one-degree-per-pixel default), so asking it
unconditionally produced a multipole axis computed from a pixel size nobody had
stated. `cl_err` is the uncertainty of THIS estimator and is `null` for a bin
with fewer than 3 modes; judge a bin by its EFFECTIVE mode count,
`n_modes / mode_coupling²`.

---

## Analysis

Implemented in `backend/app/routers/analysis.py`. Errors are HTTP: 422 for an
unusable input, 503 for a missing optional package, 500 for an unexpected
failure.

### `GET /v1/analysis/cube-stats/{dataset_id}`
Whole-dataset pixel statistics on the full-resolution data (the client's own
statistics are computed on the decimated preview). Streamed out-of-core in
z-slabs, so it is safe on cubes larger than memory, and cached per file
(path, size, modification time; up to 64 files).

Response: `valid`, `error`, `cached`, `total_count`, `valid_count`,
`blanked_count`, `min`, `max`, `mean`, `median`, `std`, `rms`, `mad_sigma`
(1.4826 × MAD), `sum`, `bunit`, `median_exact` (`false` when the median was
estimated from a strided sample).

### `POST /v1/analysis/hi-products/{dataset_id}`
Optically thin HI column density and integrated HI mass from the moment-0 map.

```json
{ "channel_start": 0, "channel_end": 0, "distance_mpc": 12.3,
  "mask_enabled": true, "threshold_value": 0.0, "threshold_auto": true,
  "rest_freq_hz": null }
```

The cube must be in a per-beam unit with a known beam (`BMAJ`/`BMIN`), with a
velocity spectral axis or, for a frequency axis, a `rest_freq_hz`; otherwise
422. A non-cube dataset is 422. Masking works as for
[`/v1/products/moment`](products.md#post-v1productsmoment). Without a mask the
integral covers every finite pixel of the channel range (noise and other
sources included), and the response carries a warning saying so.

Response: `valid`, `error`, `channel_start`, `channel_end` (the clamped range
used), `n_channels_in_cube`, `beam_arcsec`, `beam_area_pix`, `mask_enabled`,
`threshold_auto`, `threshold_used`, `n_pixels_summed`, `warnings`,
`nhi_unit` (`cm^-2`), `nhi_min`, `nhi_max`, `nhi_median`,
`integrated_flux_jy_kms`, `distance_mpc`, `hi_mass_msun`,
`hi_mass_log10_msun` (`null` without a distance), `optically_thin` (`true`).

### `POST /v1/analysis/skeleton/{dataset_id}` · `POST /v1/analysis/skeleton/{dataset_id}/binary` · `POST /v1/cosmology/skeleton/{dataset_id}`
Morphological skeleton (filaments) or void mask of one plane. The three routes
run the same computation; `/v1/cosmology/skeleton` is an alias kept for the
cosmology grouping. Requires scikit-image (503 without it).

```json
{ "channel": 0, "threshold": null, "mode": "skeleton",
  "smooth_sigma_pix": 0.0, "persistence_threshold": null }
```

| Field | Meaning |
|---|---|
| `channel` | Plane of a cube to use |
| `threshold` | Binarisation threshold; `null` = mean of the finite pixels |
| `mode` | `skeleton` (medial-axis thinning of the bright regions) or `void` (skeleton of the regions below the threshold) |
| `smooth_sigma_pix` | Gaussian pre-smoothing σ in pixels; 0 disables it |
| `persistence_threshold` | Drops skeleton pixels whose intensity is below it; `null` disables it |

JSON response: `valid`, `error`, `channel`, `width`, `height`, `threshold`
(the value used), `mode`, `smooth_sigma_pix`, `n_skeleton_pixels`,
`range_min`, `range_max`, `data_base64` (float32 mask, 0 or 255) and
`endpoints` (`[row, col]` pairs). `/binary` returns the mask as one Visivo
binary frame (`application/octet-stream`: a 12-byte big-endian header of
rows, columns and flags, then little-endian float32).
