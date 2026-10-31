# Image, astrometry and photometry

Two-dimensional images: previews, tiles and filtering, cross-matching and
reprojection, and photometry.

## Image

### `POST /v1/image/preview`
```json
{ "dataset_id": "...", "max_longest_side": 1024 }
```
Returns a downsampled float32 image (base64) + `width`, `height`, `full_width`, `full_height`, `range_min`, `range_max`, `is_preview`, `preview_scale_factor`, `bunit`. **Out-of-core**: reads only the decimated pixels from the memmap (via `np.ix_`), so a multi-GB mosaic is never fully loaded just to preview it.

### `POST /v1/image/full`
Same, no size cap. **Loads the whole plane into RAM** — use `/v1/image/tile` (or `/v1/image/preview`) for large mosaics.

### `POST /v1/image/filter`
Gaussian-smooth and/or shrink a FITS **file** into a new one in the Workspace.
Either step may be skipped, but not both. A cube is filtered plane by plane —
this is a spatial operation, and smoothing along the spectral axis would be a
different instrument.

```json
{ "path": "/data/map.fits", "smooth_fwhm_pixels": 3.0,
  "regrid_factor": 2, "combine": "auto", "output_basename": "" }
```
Returns `output_path`, `workspace_filename`, `width`, `height`, `depth`,
`range_min`, `range_max`, `bunit`, `combine`, `smooth_fwhm_pixels`,
`smooth_fwhm_arcsec`, `regrid_factor` and `flux_scale`.

This is the legacy's *Filter FITS*, with three corrections that only show up
later, in the photometry:

| | |
|---|---|
| **Blanks stay blank** | The convolution is normalised by the smoothed coverage, so a NaN neither spreads over the kernel footprint nor drags its neighbours towards zero. libwcs' `FiltFITS` ate a 4σ-wide hole around every blank pixel. |
| **The beam grows — and the flux follows it** | `BMAJ`/`BMIN` are summed in quadrature with the kernel, *and* the pixels are scaled by the ratio of beam areas when the unit is per beam (`flux_scale` in the reply, and a `HISTORY` card). Convolution preserves the **sum** of the pixels; in Jy/beam the integrated flux is that sum divided by the beam area, so a wider beam with unchanged numbers means less flux — a source smoothed with a kernel equal to its own beam would appear to lose half of it. Per-pixel and per-solid-angle units are not scaled: their sum, respectively their surface brightness, is already what the convolution preserves. |
| **The unit decides how to combine** | `combine: "auto"` reads `BUNIT`: a per-pixel unit (Jy/pixel) is **summed**, surface brightness (Jy/beam, MJy/sr) **averaged**. Averaging a Jy/pixel map loses flux by the square of the factor. `"mean"`/`"sum"` force it. |

The WCS is moved onto the new grid: `CRPIX' = (CRPIX − 0.5)/f + 0.5` (the
half-pixel terms are the difference between FITS' 1-based pixel centres and the
block edges — dropping them puts the map half a block off), `CDELT` and any
`CD`/`PC` matrix scaled by `f`, and the WCS keywords of axes squeezed out of the
data are dropped and the rest renumbered.

### `POST /v1/image/tile`
Out-of-core pyramid tile of a 2-D image mosaic — reads only the requested tile
region from the memmap, so multi-GB mosaics can be panned / zoomed without
materialising the full plane (unlike `full` / `preview`).
```json
{ "dataset_id": "...", "level": 0, "tile_x": 0, "tile_y": 0, "tile_size": 256 }
```
- `level` — pyramid level: `0` = full resolution, `L` decimates by `2**L` (nearest-neighbour). Must be `0..num_levels-1`.
- `tile_x` / `tile_y` — tile indices at that level; each tile covers `tile_size` downsampled pixels per axis.
- `tile_size` — output tile edge in pixels (1–4096, default 256).

Response: `width` / `height` (actual tile dims, smaller at edges), `full_width` / `full_height`, `level`, `num_levels`, `tile_x`, `tile_y`, `tile_size`, `range_min` / `range_max`, `compression`, base64 `data`. Out-of-range tile or `level ≥ num_levels` → 422; non-image dataset → 422.

---

## Astrometry

### `GET /v1/astrometry/crossmatch/{dataset_id}`
Cross-match the image footprint against an external catalogue via `astroquery`.

Query params: `catalogue` (`simbad` | `2MASS` | `NVSS` | `FIRST`, default `simbad`), `radius_arcsec` (default 60), `max_sources` (1–5000, default 500).

Response: `{ valid, error, n_sources, sources, truncated, centre_ra_deg, centre_dec_deg }`, where each source is `{ ra_deg, dec_deg, name, catalogue_id, separation_arcsec }`. The `X-Truncated` header mirrors `truncated`. Sources are cropped to the **valid (finite) image pixels** — those projecting onto the blank/NaN border of a non-rectangular footprint are dropped. When the field exceeds `max_sources`, the retained set is a **uniform spatial sub-sample** across the footprint (queried against a generous upstream row cap) rather than only the sources nearest the image centre, so coverage stays representative. Catalogue column names are resolved case-insensitively (robust across `astroquery` versions), and unparseable rows are skipped rather than failing the whole query. Errors map to HTTP: missing `astroquery` → 503, unknown catalogue → 422.

### `POST /v1/astrometry/reproject/{dataset_id}`
```json
{ "target_wcs": { "...": "FITS WCS header cards" }, "target_shape": [ny, nx], "order": 1 }
```
Reprojects an image plane onto a target WCS/grid, writes a new FITS to the session workspace, and registers it. Response: `{ valid, new_dataset_id, width, height, range_min, range_max, output_path, order }`. Missing `reproject` package → 503.

---

## Photometry

Implemented in `backend/app/routers/photometry.py`. Positions are sky
coordinates (degrees), so the dataset needs a celestial WCS. `channel`
selects the plane of a cube (`null` = the first).

### `POST /v1/photometry/aperture/{dataset_id}`
Circular-aperture flux with an optional background annulus.

```json
{ "ra_deg": 83.822, "dec_deg": -5.391, "aperture_arcsec": 10.0,
  "annulus_inner_arcsec": 15.0, "annulus_outer_arcsec": 25.0, "channel": null }
```

An annulus of 0/0 (the default) means no background subtraction. Response:
`valid`, `error`, `flux_jy`, `flux_err_jy`, `flux`, `flux_err`, `flux_unit`,
`beam_area_pix`, `background_per_pix`, `background_rms_per_pix`, `bunit`,
`n_pix_aperture`, `channel`, `pix_x`, `pix_y`, `pixel_scale_arcsec`.
`flux` is always the summed brightness in `flux_unit`; `flux_jy` is the
beam-corrected flux in Jy and is `null` unless the data are per beam with a
known beam. Errors: 503 without photutils, 422 without a celestial WCS or for
an unusable aperture, 500 otherwise.

### `POST /v1/photometry/gaussfit/{dataset_id}`
Fit a 2-D elliptical Gaussian plus a constant background in a box around a
position.

```json
{ "ra_deg": 83.822, "dec_deg": -5.391, "box_arcsec": 60.0, "channel": null }
```

Response: `valid`, `error`, `peak_jy_beam`, `peak_err`,
`integrated_flux_jy` and `integrated_flux_err_jy` (`null` unless per-beam
data with a known beam), `integrated_flux`, `integrated_flux_err`,
`integrated_flux_unit`, `beam_area_pix`, `bunit`, `major_arcsec`,
`minor_arcsec` (FWHM) with `_err_arcsec` companions, `pa_deg`, `pa_err_deg`,
`deconvolved_major_arcsec`, `deconvolved_minor_arcsec`,
`deconvolved_pa_deg`, `fit_rms`, `error_method`, `error_trials`,
`diagnostic_covariance_err`, `diagnostic_covariance_integrated_err`,
`center_pix_x`, `center_pix_y`, `background`.

`error_method` says how the error bars were obtained: `empirical` (the model
injected into source-free parts of the image and refitted, which accounts for
beam-correlated noise) or `covariance` (the fit covariance, which assumes
independent pixels and underestimates errors on interferometric images; used
when the field is too crowded). The `diagnostic_covariance_*` fields are set
only in the second case. No WCS, a box that is too small or a position off the
image is 422; a fit that fails is 500.
