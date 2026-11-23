# Products

Derived products: moment maps, PV and moment FITS files, isosurfaces, and the
2-D image products (pixel arithmetic, mosaics, figures, quality metrics). The
written files land in the Workspace Exports directory (see
[Exports](exports.md)).

## Moments, PV and isosurfaces

### `POST /v1/products/moment`
Synchronous moment map computation (small datasets / fast moments).
```json
{
  "dataset_id": "...", "moment_order": 1,
  "channel_start": 10, "channel_end": 50,
  "mask_enabled": true, "threshold_value": 0.0, "threshold_auto": true
}
```
- `mask_enabled` (default `false` at the API; the desktop client defaults it to `true`) — only voxels above the threshold contribute.
- `threshold_auto` (default `false`) — when set with `mask_enabled`, the backend derives the threshold from the data as `median + 3·σ` (σ from the MAD), ignoring `threshold_value`. The estimate is strided in all three axes so it stays out-of-core on large cubes.
- `threshold_value` — the manual threshold, used only when `mask_enabled` and not `threshold_auto`.
- `moment2_variance` (default `false`) — for order 2, return the variance σ² instead of the dispersion σ. Ignored for other orders.

Response: `valid`, `error`, `width`, `height`, `scalar_type`, `range_min`, `range_max`, `spectral_axis_type`, `spectral_axis_unit`, `moment_unit`, `bunit`, `threshold_used` (the resolved threshold, `null` when no mask was applied), `data_base64` (float32). A non-cube dataset is 422; a compute failure is `valid: false`.

**Physical-range guard (M1/M2).** Regardless of masking, the backend blanks M1 pixels outside the sampled spectral axis `[vmin, vmax]` and M2 pixels outside `[0, span²]`. These can only arise from mixed-sign (noise) weights and would otherwise produce non-physical (e.g. super-luminal) values; the guard never removes a legitimate positive-weight voxel.

Supported `moment_order` values and their definitions (any other is `valid: false`, "Unsupported moment order"):

| Order | Name | Formula | Unit |
|-------|------|---------|------|
| 0 | Integrated intensity | Σ(I·dv) | BUNIT·spectral |
| 1 | Intensity-weighted coordinate | Σ(I·v·dv)/M0 | spectral |
| 2 | Velocity dispersion σ (default) or intensity-weighted variance | √(Σ(I·(v−M1)²·dv)/M0), or without the root when `moment2_variance` is `true` | spectral (σ), spectral² (variance) |
| 3 | Skewness | μ₃/σ³ | dimensionless |
| 4 | Excess kurtosis | μ₄/σ⁴ − 3 | dimensionless |
| 5 | Standardised 5th moment | μ₅/σ⁵ | dimensionless |
| 6 | RMS | √(Σ(I²·dv)/Σ(dv)) | BUNIT |
| 8 | Maximum value | max(I) | BUNIT |
| 10 | Minimum value | min(I) | BUNIT |

For orders 3–5, `spectral_delta` (channel spacing) is used as the integration weight. NaN/blanked voxels are excluded. Variance must be positive; pixels where σ=0 or M0=0 are set to NaN.

### `POST /v1/products/pv_fits`
Compute the PV diagram along a path and save it as a 2-D FITS file in the
Workspace Exports.
```json
{ "dataset_id": "...", "vertices": [[12.5, 40.0], [130.0, 90.0]], "width_pixels": 3,
  "output_basename": "ngc3137_pv.fits" }
```
Axis 1 is `OFFSET` (arcsec, 0 at the first vertex, `CDELT1` = step); axis 2 is
the cube's own spectral axis WCS (CTYPE/CUNIT/CRPIX/CRVAL, CDELT×PC or CD,
`PVn_m`/`PSn_m`). `BUNIT`, beam, `RESTFRQ`/`RESTWAV`, `SPECSYS` are copied;
`PVWIDTH`, `PVSTEP` and the vertices (`HISTORY`, pixel and sky) are recorded.
An empty `output_basename` becomes `<cube stem>_pv.fits`; an existing file is never overwritten (the name is suffixed). A non-cube dataset is 422; a compute failure is `valid: false`. Response:
`valid`, `error`, `output_path`, `workspace_filename`, `width` (samples),
`height` (channels), `range_min`, `range_max`, `bunit`.

### `POST /v1/products/spectrum_table`
Extract a spectrum and save it in the Workspace Exports as a small table, so a
spectrum made in the headset can be opened on the desktop
([Headset hub](headset-hub.md#saving-what-the-headset-makes)).
```json
{ "dataset_id": "...", "x": 120, "y": 88, "radius": 0, "format": "csv",
  "output_basename": "" }
```
`radius` 0 is the spectrum of pixel (x, y), as `/v1/spectral/probe` answers
it; `radius` > 0 (at most 4096) the mean over a circle of that many pixels,
as `/v1/spectral/aperture`. Computed again from the cube, not taken from the
client. `format` `csv` writes the desktop's own spectrum CSV (a `#` preamble
with the dataset, pixel, aperture and samples, then `<spectral type> (<unit>),
Intensity (<BUNIT>)` and one row per channel, blank channels as `nan`);
`fits` a binary table `SPECTRUM` (columns `CHANNEL`, `SPECTRAL`, `VALUE` with
their `TUNIT`s; `SPECTYPE`, `XPIX`, `YPIX`, `RADIUS`, `NPIX` in its header).
The file's suffix follows `format`; an empty `output_basename` becomes
`<cube stem>_spectrum_x<x>_y<y>`; an existing file is never overwritten.
A non-cube dataset or a non-finite pixel is 422; a pixel outside the cube is
`valid: false`. Response: `valid`, `error`, `output_path`,
`workspace_filename`, `n_channels`, `spectral_unit`, `spectral_type`,
`bunit`, `x`, `y`, `radius`, `n_pix`, `range_min`, `range_max`.

### `POST /v1/products/isosurface`
```json
{ "dataset_id": "...", "threshold": 0.05 }
```
Returns `{valid, error, compression: "qt-zlib", points_base64, polys_base64,
num_points, num_polys}`: float32 xyz points in the cube's voxel coordinates
and the int32 VTK polygon connectivity, each zlib-compressed in Qt's
`qCompress` framing, then base64. A large cube is sampled with per-axis
strides (at most 256³ voxels) and coarsened while the surface has more than
4 M triangles. A non-cube dataset is 422; an empty surface, a surface still
too large at the coarsest stride, or a server without the VTK Python
bindings is `valid: false`.

### `POST /v1/products/moment_fits`
Compute a moment and save it as a 2-D FITS file in the Workspace Exports. The
request is the `/v1/products/moment` request plus `output_basename`:

```json
{ "dataset_id": "...", "moment_order": 0, "channel_start": 10, "channel_end": 50,
  "mask_enabled": true, "threshold_value": 0.0, "threshold_auto": true,
  "moment2_variance": false, "output_basename": "" }
```

An empty `output_basename` becomes `<source stem>_m<order>.fits`; an existing
name gets a numeric suffix. The file carries the celestial WCS, a `BUNIT`
derived from the cube and the cube's restoring beam (`BMAJ`, `BMIN`, `BPA`), so
the image viewer gives the flux of a region of the map; a cube with one beam
per channel (a `BEAMS` table) gets the beam of the central channel of the
range and a `COMMENT` saying so. Response: `valid`, `error`, `output_path`,
`workspace_filename`, `width`, `height`, `range_min`, `range_max`, `bunit`,
`moment_unit`. A non-cube dataset is 422; a compute failure is `valid: false`.
The map is computed exactly as `/v1/products/moment` computes it for the same
request (same backend selection, Dask path for large ranges, memory bound and
product cache), then written: the file holds, bit for bit, the map the viewer
shows. It takes one of the session's heavy slots, as `/v1/products/moment`
does (HTTP 429 when they are all busy), and runs in the heavy tier (see
[Worker pool](../backend-api.md#worker-pool-and-heavy-task-throttle)).

## Advanced 2-D products

Four routes over 2-D images (the first plane of a cube when a cube is given;
degenerate axes are squeezed). Failures come back as `valid: false` with the
reason; an unknown dataset is 404.

### `POST /v1/products/map_math`
Pixel-wise arithmetic on one or two maps, written to a FITS file in the
Workspace.

```json
{ "dataset_id": "ds_a", "op": "spectral_index", "dataset_id_b": "ds_b",
  "freq_a": 1.4e9, "freq_b": 3.0e9, "output_basename": "" }
```

| `op` | Needs | Result |
|---|---|---|
| `log10`, `abs`, `square` | map A | applied to A |
| `add`, `subtract`, `multiply` | A and B | A + B, A − B, A × B |
| `divide`, `ratio` | A and B | A / B |
| `spectral_index` | A, B, `freq_a`, `freq_b` (Hz, positive and different) | α = log10(A/B) / log10(ν_a/ν_b); NaN wherever A or B is not finite and > 0 |

**Alignment.** The maps are combined pixel by pixel only when they are on the
same sky grid: same shape, and a grid of up to 33 × 33 of A's pixels (edges
included) falls within 0.01 pixel of the same pixels of B (compared on the sky,
so a CD matrix and the equivalent CDELT, or ICRS and Galactic, count as the
same grid; SIP distortion is included, and a map with a lookup-table
distortion is always reprojected). The sky WCS of a cube's first plane is read
from its first two axes when the full header cannot be parsed (astropy refuses
SIP on a 3-D header); lookup-table distortions are read from the file's
extension HDUs. A map whose header names sky axes but whose WCS cannot be read
at all is refused.
Otherwise B is reprojected onto A's grid with `reproject_interp` (bilinear,
the convention of `/v1/astrometry/reproject/{dataset_id}` at order 1) — an interpolation, which
keeps a surface brightness (Jy/beam, MJy/sr, K) per beam or solid angle, not a
flux-conserving per-pixel resampling — and B pixels outside its footprint are
NaN. The beams are not matched: for a spectral index, convolve the maps to a
common resolution first. If one map has a celestial WCS and the other has
none, the request fails (`valid: false`) rather than combine unrelated pixels;
two maps without a WCS are combined by shape alone and must have the same
shape.

The output keeps A's header; `BUNIT` is cleared for `divide`, `ratio`
and `spectral_index`, and a `HISTORY` card names the operation (and a second
one the reprojection, when there was one). The default
file name is `<A stem>_<op>.fits`. Response (`ProductFitsResponse`):
`valid`, `error`, `output_path`, `workspace_filename`, `width`, `height`,
`range_min`, `range_max`, `info` (`{op, reprojected, overlap_fraction,
warning}`; `reprojected` is `true` when B was resampled onto A's grid).

**Overlap.** For a two-map operation, `overlap_fraction` is the fraction of
A's finite pixels where B, on A's grid, is finite too (`null` for a one-map
operation). Maps that do not overlap at all are refused (`valid: false`,
"Maps A and B do not overlap…") and no file is written. A partial overlap is
computed, and `warning` says how much of A is covered ("Maps A and B overlap
on 42.0% of map A's pixels (…); the result is blank elsewhere."); it is empty
otherwise. Any result with no finite pixel at all (for example a spectral
index of two maps with no positive pixel in common) is refused the same way
rather than written as a blank map.

### `POST /v1/products/mosaic`
Reproject two or more images onto a common optimal celestial WCS and co-add
them.

```json
{ "dataset_ids": ["ds_1", "ds_2", "ds_3"], "weight_mode": "noise",
  "output_basename": "mosaic.fits" }
```

`weight_mode: "noise"` (default) weights each input by 1/σ², σ being its
MAD-based robust standard deviation; `"uniform"` weights them equally. Fewer
than two datasets is 422. The response has the same shape as `map_math`, with
`info` = `{n_inputs, weight_mode, coverage_fraction}` (fraction of output
pixels covered by at least one input).

### `POST /v1/products/publication_figure`
Render an image as a figure file with a WCS sky grid and a colour bar labelled
with `BUNIT`.

```json
{ "dataset_id": "...", "cmap": "inferno", "stretch": "asinh", "percent": 99.5,
  "title": "", "add_grid": true, "add_colorbar": true, "dpi": 200,
  "output_basename": "field.pdf" }
```

`stretch` is `linear`, `log`, `sqrt`, `asinh` or `power` (astropy
`simple_norm`, clipped at `percent`). The format follows the extension of
`output_basename` (`.png`, `.pdf`, `.svg`; anything else gets `.png`
appended); the default name is `<stem>_figure.png`. Without a celestial WCS the
axes are in pixels. Response: `valid`, `error`, `output_path`,
`workspace_filename`, `width`, `height` (image pixels), `has_wcs`.

### `POST /v1/products/image_quality`
Quality and artefact metrics for an image. Request: `{dataset_id}`.
Response: `{valid, error, metrics}` with

| Metric | Meaning |
|---|---|
| `total_pixels`, `blanked_fraction` | Pixel count, fraction that is NaN or infinite |
| `zero_fraction`, `negative_fraction` | Among finite pixels |
| `min`, `max`, `mean`, `median`, `robust_std` | `robust_std` = 1.4826 × MAD |
| `dynamic_range` | `max / robust_std` |
| `negative_bowl` | `min < −8 × robust_std` |
| `striping_metric` | Ratio of the larger to the smaller variance of row means and column means, capped at 10⁶. Perfect stripes (one variance zero, the other not) are 10⁶; a flat image (both zero) is 0 |
| `flags` | Any of `high_blanked_fraction` (> 0.2), `negative_bowl`, `striping` (metric > 5), `low_dynamic_range` (< 5), `all_blanked` |

### `POST /v1/products/lte_co`
LTE analysis of CO isotopologues (issue #416). It takes ¹³CO and C¹⁸O, and optionally ¹²CO, in the same transition. It returns the optical depth from the line ratio, the excitation temperature, opacity-corrected column densities, N(H₂) and mass. Results come back per region and as FITS maps.

You can give the input in one of two forms.

**Datasets.** Pass `dataset_id_13co` and `dataset_id_c18o`, and optionally `dataset_id_12co`.
- A cube is integrated over `v_min_kms` to `v_max_kms`, each channel weighted by its own width in km/s. Its peak in that window is also kept.
- A 2-D map is read as an integrated intensity in K km/s. For ¹²CO a 2-D map is read as a peak temperature in K.
- C¹⁸O and ¹²CO are reprojected onto the ¹³CO grid when their sky grids differ (bilinear, as in Pixel Math). The answer warns when this happens.

**Measurements.** Pass `measurements` with the integrals of one region measured elsewhere, for example Gaussian fits of its mean spectra:
```json
{ "w_13co": 5.10, "sigma_13co": 0.21, "w_c18o": 1.22, "sigma_c18o": 0.19,
  "scale": "ta*", "peak_13co": 1.98, "peak_c18o": 0.617, "peak_12co": null,
  "glon_deg": 40.157, "glat_deg": 0.169, "n_pixels": 29, "pixel_arcsec": 7.6 }
```

Request fields:

| Field | Meaning |
|---|---|
| `transition` | `1-0` or `3-2` (required) |
| `v_min_kms`, `v_max_kms` | The line's window, required for a cube |
| `rms_13co`, `rms_c18o` | Noise: K per channel for a cube, K km/s for a map. When absent, a cube uses the MAD σ of the channels outside the window at each pixel, and a map uses the MAD σ of the whole map (emission included, so an overestimate). For a cube σ_W = σ_chan √ΣΔv², which takes the channels as independent and underestimates σ_W when they are correlated |
| `snr_min` | Default 3. A pixel is computed only where both integrals reach `snr_min` σ |
| `eta_mb_13co`, `eta_mb_c18o`, `eta_mb_12co` | Main-beam efficiencies, T_mb = T_A*/η_mb. The order of precedence is described below the table |
| `x_ratio` | X(¹³CO)/X(C¹⁸O). Without it, X = (¹⁶O/¹⁸O)/(¹²C/¹³C) at R_GC |
| `r_gc_kpc`, `distance_kpc` | R_GC directly, or computed from the distance and the map centre (or `glon_deg`/`glat_deg`) with R₀ = 8.5 kpc |
| `carbon_gradient` | `milam2005` (6.21 R_GC + 18.71, the default) or `wilson_rood1994` (7.5 R_GC + 7.6). ¹⁶O/¹⁸O is always 58.8 R_GC + 37.1 (Wilson & Rood 1994) |
| `t_ex_k` | A fixed T_ex, recorded as an assumption. The fallbacks are described below the table |
| `h2_from` | `c18o` (the default) or `13co` |
| `x12co` | Default 8.5 × 10⁻⁵ (Frerking et al. 1982). X(C¹⁸O) = x12co/(¹⁶O/¹⁸O) and X(¹³CO) = x12co/(¹²C/¹³C) at R_GC |
| `abundance_c18o`, `abundance_13co` | Override the X(C¹⁸O) or X(¹³CO) abundance relative to H₂ |
| `mu_h2` | Default 2.8 (Kauffmann et al. 2008) |
| `regions` | Canonical image-frame regions (0-based pixels) on the ¹³CO grid |
| `write_maps` | Default true |
| `output_basename` | Stem of the map files |

η_mb is chosen in this order:
1. the value in the request;
2. 1 when TEMPSCAL, BUNIT or BTYPE names T_mb;
3. BEAMEFF/FORWEFF (GILDAS/CLASS);
4. ETAMB.

Otherwise the request is refused. ETAL is never used, because it is the rear-spillover efficiency and is already in T_A*. The ¹²CO efficiency is needed only when ¹²CO gives T_ex; a ¹²CO dataset sent with a typed `t_ex_k` is not used, and `warnings` says so. Channel numbers in the notes count from 1, as in the cube viewer.

Without `t_ex_k`, T_ex comes from the peak of ¹²CO, taken as optically thick (a cube or a peak map). Without ¹²CO it comes from the ¹³CO peak with τ at the peak, solved from the ratio of the ¹³CO and C¹⁸O peaks: both must be cubes, or `peak_13co` and `peak_c18o` must be given. The τ from the integrated ratio is a line average, smaller than at the peak, and would raise T_ex. Where the peaks give no optical depth (thin, or no solution), T_ex and the column densities are unknown. A region's peaks are those of its mean spectra, not means of per-pixel peaks. Per pixel the two peaks are each the maximum over the window, so where C¹⁸O is weak (S/N below about 5) its noise raises the C¹⁸O peak and lowers T_ex: read the `tex` map there with care, or use regions. No uncertainty is given for T_ex. A T_ex at or below T_bg is refused when typed and left unknown when derived.

Formulas (`backend/app/lte_co.py`), in CGS units, with T_bg = 2.73 K:

- **Ratio and optical depth.** R = W(¹³CO)/W(C¹⁸O) = (1 − e^(−τ₁₃))/(1 − e^(−τ₁₃/X)), with τ(C¹⁸O) = τ₁₃/X.
  - The ratio uses the integrated intensities.
  - Both lines are assumed to share the same J(T_ex) − J(T_bg). Their frequencies differ by 0.4 %, so τ is off by at most 0.75 % for T_ex ≥ 10 K in 3–2 (R ≈ 4) and 0.15 % in 1–0.
- **Column density.** N = 8πν³/(c³A_ul) · Q(T_ex)/g_u · e^(E_u/kT_ex)/(e^(hν/kT_ex) − 1) / (J(T_ex) − J(T_bg)) · ∫T_mb dv · τ/(1 − e^(−τ)).
  - This is the A_ul form of Mangum & Shirley 2015, eqs. 29–31.
  - Q is the exact sum Σ(2J+1)e^(−hBJ(J+1)/kT), and g_u = 2J_u + 1.
- **Line constants.** From CDMS entries 029501 (¹³CO), 030502 (C¹⁸O) and 028503 (CO), listed in the code.
- **Excitation temperature.**
  - From ¹²CO: T_ex = T₀/ln(1 + T₀/(T_mb,peak + J(T_bg))).
  - From ¹³CO: J(T_ex) = T_mb,peak/(1 − e^(−τ)) + J(T_bg).
- **H₂ and mass.** N(H₂) = N(C¹⁸O)/X(C¹⁸O), or N(¹³CO)/X(¹³CO) with `h2_from = 13co`. M = μ_H₂ m_H Σ N(H₂) A_pix, with A_pix = d²|Δθ₁Δθ₂|.

Each pixel gets a flag:
- 0: ok.
- 1: optically thin (R ≥ X). τ = 0 and the columns carry no opacity correction.
- 2: no LTE solution (R ≤ 1). τ, T_ex and the columns are NaN; the `ratio` map keeps R.
- 3: below `snr_min`. τ, T_ex and the columns are NaN; the `ratio` map keeps R.

Pixels without data are NaN in every map. Columns are also NaN wherever T_ex is unknown; `pixel_flags.no_tex` counts those pixels.

Response (values illustrative):
```json
{ "valid": true, "error": "", "width": 167, "height": 69,
  "assumptions": { "transition": "3-2", "eta_mb": {"13CO": [0.72, "user"], ...},
                   "x_ratio": [7.4, "user"], "r_gc_kpc": [5.68, "from l = ..."],
                   "t_ex": {"source": "..."}, "h2": {...}, "line_constants": {...}, ... },
  "warnings": ["C18O was reprojected onto the 13CO grid (bilinear)."],
  "regions": [ { "index": 0, "n_pixels": 29, "beam_pixels": 3.5,
                 "w_13co_kkms": 7.08, "sigma_w_13co_kkms": 0.29, "w_c18o_kkms": 1.69, "sigma_w_c18o_kkms": 0.26,
                 "ratio": 4.18, "sigma_ratio": 0.67, "tau_13co": 1.54, "tau_c18o": 0.21,
                 "tau_13co_interval": [1.09, 2.15], "flag": 0, "t_ex_k": 15.0,
                 "n_13co_cm2": 1.0e16, "n_c18o_cm2": 1.3e15, "n_h2_cm2": 5e21, "mass_msun": 120.0 } ],
  "maps": [ { "quantity": "tau_13co", "output_path": "...", "workspace_filename": "..._tau_13co.fits",
              "bunit": "", "range_min": 0.2, "range_max": 3.1 }, ... ],
  "pixel_flags": { "ok": 812, "thin": 40, "no_solution": 3, "below_snr": 9000, "no_tex": 40 } }
```

Region values:
- The integrals are region means on the T_mb scale, over pixels where both lines and their σ are finite. Values that cannot be computed are null.
- Their σ is divided by √(n / pixels per beam). The beam is the header's, or the one typed with `/v1/datasets/beam`.
- `tau_13co_interval` is [τ(R + σ_R), τ(R − σ_R)]. An end is null when it has no solution.

The maps are `ratio`, `tau_13co`, `tau_c18o`, `tex` (K), `n_13co`, `n_c18o`, `n_h2` (cm⁻², only when an abundance is known) and `flag`.

Each map's header records:
- `BUNIT`, `LTEQUANT`, `LTETRANS`;
- `ETAMB13`/`ETAMB18`/`ETAMB12`, `XRATIO`, `RGC_KPC`, `TEX_K`, `TBG_K`, `SNRMIN`;
- the line constants (`NU13`, `AUL13`, `EU13`, …, `GU`), and `XH2`, `DIST_KPC`;
- `HISTORY` lines with the formulas, the sources, each assumption and where it came from.
