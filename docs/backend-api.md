# Backend API Reference

Reference for the FastAPI backend (`backend/app/main.py`).
All endpoints require `X-Visivo-Token` header (bearer token).
Dataset-scoped endpoints also require `X-Visivo-Session` (returned by `open` calls).

---

## Auth

```
X-Visivo-Token: <token>          # every request
X-Visivo-Session: <session_id>  # dataset-scoped requests
```

Token written to `~/.visivo_token` at backend startup; read by `BackendClient::readTokenFile()`.

---

## Meta

### `GET /v1/health`
Backend liveness and capacity.
```json
{
  "ok": true,
  "workers": 4,
  "active_sessions": 1,
  "product_cache_entries": 3,
  "product_cache_capacity": 64,
  "task_registry_entries": 0,
  "task_ttl_enabled": true,
  "task_ttl_seconds": 3600,
  "running_tasks": 0,
  "recent_tasks": [
    { "operation": "products/moment", "status": "done",
      "duration_seconds": 2.4, "age_seconds": 31.0, "cache_hit": false }
  ]
}
```

`recent_tasks` is what the Data Hub shows as **Last 5 jobs**, and it holds two
kinds of entry: the asynchronous tasks (`/v1/tasks/moment`, `/v1/tasks/pv`) and
every other compute request, recorded by an HTTP middleware when its response
finishes. Without the second kind the list was almost always empty — the rest of
the backend is synchronous and registered nothing.

`operation` is the route template (`photometry/aperture/{dataset_id}`), so the
same tool run on twenty datasets reads as twenty runs of one operation rather
than twenty different ones. `status` is `failed` both for an HTTP error and for
the `{"valid": false}` envelope that most routes return with HTTP 200.

A finished job reads `done` when the middleware recorded it and `completed` when
it came from the task registry — the task API's own terminal status. Both are
successes; a client that treats anything other than `done` as a failure marks
every moment as broken. (The registry itself used to recognise only `done`, so
no `/v1/tasks/*` job appeared in this list at all: not running, not finished.)

Not recorded: browsing and classification (`/v1/files/*`), session bookkeeping,
image tiles, SAMP, WebRTC signalling, interactive scrubbing (`/v1/cube/slice`,
`/v1/spectral/probe`), job polling (`/v1/vlkb/mcutout/phase`), the single-redshift
`/v1/cosmology/distance` lookup, and the `/result/binary` collection of a product
that was recorded when it was computed. The cosmology BATCH is recorded: it is
asked for once, when a user picks a different cosmology, and it recomputes the
position of every source in a catalogue.
Only `POST`/`PUT`/`PATCH` count. The history holds the last 50 operations and is
served to admin tokens only, as the rest of the snapshot is.

### `GET /v1/sessions`
Aggregate session-registry statistics.

### `GET /v1/sessions/{session_id}/datasets`
Enumerate the datasets currently open in a backend session. Used by
cross-dataset tools (Spectral Stacking, etc.) to populate selection lists in
the desktop client. Returns `404` if the session does not exist.

```json
{
  "session_id": "anon-abc",
  "datasets": [
    { "dataset_id": "ds_f558b5d90c55", "kind": "cube",
      "path": "/data/cubehi-clean-m31.fits",
      "width": 512, "height": 512, "depth": 200 },
    { "dataset_id": "ds_…", "kind": "cube", "path": "…",
      "width": 512, "height": 512, "depth": 200 }
  ]
}
```

---

## Files

### `GET /v1/files/list?path=<dir>`
List backend-side directory. Returns `FileEntry[]` with `name`, `path`, `type`, `size`, `modified_time`, `is_fits`.

### `POST /v1/files/header`
```json
{ "path": "/data/cube.fits" }
```
Returns FITS header cards as `string[]`.

### `POST /v1/files/header/patch`
Apply header edits to a **copy** of the file, in the Workspace. The input is
never modified.

```json
{ "path": "/data/broken.fits",
  "cards": [{ "key": "CTYPE1", "value": "RA---TAN", "comment": "" },
            { "key": "CTYPE2", "value": "DEC--TAN" }],
  "remove": ["EQUINOX"], "output_basename": "" }
```
Returns `output_path`, `workspace_filename`, `changes` (one line per edit,
naming the old value as well as the new), `hdu_index`, and `wcs_status` /
`wcs_message` / `wcs_celestial`.

That last part is the point. The legacy's header modifier appeared only when a
file failed to open, offered three keyword groups, and reported "File has been
saved!" whether or not the values typed produced a header anything could read.
Here the WCS is built from the result and the reply says what it amounts to — a
celestial solution, a degraded one, or still nothing.

Refused: `SIMPLE`, `BITPIX`, `NAXISn`, `EXTEND`, `XTENSION`, `PCOUNT`, `GCOUNT`,
`TFIELDS` — they describe the bytes on disk — and `BSCALE`, `BZERO`, `BLANK`,
which say how the stored integers map to physical values: editing one does not
annotate the data, it multiplies it. A value that reads as a number is written
as a number, since a quoted `'-0.001'` in `CDELT1` is a string card that every
WCS parser ignores; quote it explicitly to force a string.

### `POST /v1/files/classify`
What is this file? Answers from the file itself — magic bytes, extension, and at
most one FITS header; nothing is opened, converted or registered.

```json
{ "path": "/data/unknown.fits" }
```
```json
{ "valid": true, "error": "", "path": "/data/unknown.fits", "open_path": "",
  "kind": "cube", "opener": "dataset", "confident": true,
  "detail": "FITS cube (512 × 512 × 200)." }
```

| Field | Meaning |
|-------|---------|
| `kind` | `image`, `cube`, `dynspec`, `fits-table`, `catalogue`, `vbt`, `compressed`, `directory`, `unknown` |
| `opener` | `dataset` → `/v1/datasets/open`; `catalogue` → `/v1/catalogue/open`; `vbt` → `/v1/vbt/open`; `ask` → the content is ambiguous, let the user choose; `unsupported` → nothing here opens it, say so |
| `open_path` | The path to hand the opener when it differs from `path`: a VBT chosen by its `.bin` opens through the `.head` beside it. Empty means "use `path`". |
| `confident` | `false` when the answer is a guess — a FITS whose header would not parse, for instance |
| `detail` | One sentence naming what was found, shown to the user when we have to ask |

A FITS **table** is reported as `fits-table` / `ask`: it can be a source
catalogue or a table to overlay, and only the user knows which. A missing path
and a refused one answer identically (`"File not found."`), so the classifier
cannot be used to enumerate what lies outside the allowed data roots.

This is what lets the client offer a single **Open…** instead of one action per
file type.

---

## Datasets

### `POST /v1/datasets/open`
Open a FITS file; registers a session.
```json
{ "path": "/data/cube.fits" }
```
Response includes `dataset_id`, `session_id`, `kind` (`image`|`cube`), dimensions, WCS metadata (`wcs_status`, `wcs_warning_message`, `wcs_sanitized_axes`, `spacing`, `origin`, `ctype[]`, `cunit[]`, `crval[]`, `crpix[]`, `cdelt[]`), the spectral-axis summary (`spectral_axis_type` = the spectral `CTYPE`, `spectral_axis_unit`), beam (`beam_major`, `beam_minor`, `beam_pa`), `bunit`, and:

| Field | Type | Meaning |
|-------|------|---------|
| `rest_freq_hz` | `float \| null` | Header `RESTFRQ`, else `RESTFREQ`, in Hz. `null` when the header carries neither, or carries a non-positive value — a client must be able to tell "no rest frequency" from one it would divide by. Needed to place a rest-frame spectral-line list on a velocity axis, and to convert a frequency axis to velocity. |

---

## Catalogue

### `POST /v1/catalogue/open`
Open a remote CSV file.
```json
{ "path": "/data/catalogue.csv", "format": "csv" }
```
Returns `dataset_id`, `session_id`, field list with types.

### `POST /v1/catalogue/subset`
Legacy: return up to `max_rows` rows without filtering.

### `POST /v1/catalogue/query`
Paginated, filtered query.
```json
{
  "dataset_id": "...",
  "limit": 50000,
  "offset": 0,
  "filters": [
    { "field": "flux", "op": ">", "value": "0.1" }
  ],
  "sort_field": "flux",
  "sort_direction": "desc"
}
```
Response: `total_rows`, `returned_rows`, `rows[]` (key-value maps).

Supported filter operators: `<`, `<=`, `>`, `>=`, `=`, `!=`, `contains`, `startswith`, `endswith`.

---

## VBT

### `POST /v1/vbt/open`
Open a VBT file.
Returns `dataset_id`, `session_id`, `kind` (`point`|`volume`), `fields[]`, `num_rows`, `scalar_type`.

### `POST /v1/vbt/subset`
Legacy bulk fetch (no filter, `max_rows` cap).

### `POST /v1/vbt/query`
Same request shape as `/v1/catalogue/query`.
Response: `total_rows`, `returned_rows`, `field_names[]`, `columns` (base64-encoded float64 column vectors), `num_rows`.

---

## Cube

### `POST /v1/cube/preview`
Downsampled cube for fast initial display.
```json
{ "dataset_id": "...", "downsample": 4 }
```
Returns `scalar_type`, `width`, `height`, `depth`, `range_min`, `range_max` + base64 `data`.

### `POST /v1/cube/slice`
Single 2-D slice.
```json
{ "dataset_id": "...", "axis": "z", "index": 42 }
```

### `POST /v1/cube/subvolume`
Spatial + spectral sub-region.
```json
{ "dataset_id": "...", "x0": 0, "x1": 255, "y0": 0, "y1": 255, "channel_start": 10, "channel_end": 50 }
```

### `POST /v1/cube/pv`
Position-velocity diagram along a polyline.
```json
{
  "dataset_id": "...",
  "points_ra_dec": [[ra1, dec1], [ra2, dec2]],
  "width_pixels": 3
}
```
Returns `num_samples`, `depth`, `scalar_type`, `positions_arcsec_base64`, `total_length`, `valid_samples`, `pixel_scale_arcsec_per_pixel`, `spatial_unit`, `spectral_axis_type`, `spectral_axis_unit`, `bunit`, `beam_major`, `beam_minor`, `beam_pa`.

### `POST /v1/cube/noise`
Noise statistics (MAD and sigma) over a spatial region and channel range.
```json
{ "dataset_id": "...", "x0": 0, "x1": 127, "y0": 0, "y1": 127, "channel_start": 0, "channel_end": 63 }
```
Returns `channel_start`, `channel_end`, `num_channels`, `mad[]`, `sigma[]`, `region`.

---

## Products

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

Response: `valid`, `width`, `height`, `scalar_type`, `range_min`, `range_max`, `spectral_axis_type`, `spectral_axis_unit`, `moment_unit`, `bunit`, `threshold_used` (the resolved threshold, `null` when no mask was applied), `wcs_status`, `wcs_warning_message`, base64 `data`.

**Physical-range guard (M1/M2).** Regardless of masking, the backend blanks M1 pixels outside the sampled spectral axis `[vmin, vmax]` and M2 pixels outside `[0, span²]`. These can only arise from mixed-sign (noise) weights and would otherwise produce non-physical (e.g. super-luminal) values; the guard never removes a legitimate positive-weight voxel.

Supported `order` values and their definitions:

| Order | Name | Formula | Unit |
|-------|------|---------|------|
| 0 | Integrated intensity | Σ(I·dv) | BUNIT·spectral |
| 1 | Intensity-weighted coordinate | Σ(I·v·dv)/M0 | spectral |
| 2 | Intensity-weighted variance | Σ(I·(v−M1)²·dv)/M0 | spectral² |
| 3 | Skewness | μ₃/σ³ | dimensionless |
| 4 | Excess kurtosis | μ₄/σ⁴ − 3 | dimensionless |
| 5 | Standardised 5th moment | μ₅/σ⁵ | dimensionless |
| 6 | RMS | √(Σ(I²·dv)/Σ(dv)) | BUNIT |
| 8 | Maximum value | max(I) | BUNIT |
| 10 | Minimum value | min(I) | BUNIT |

For orders 3–5, `spectral_delta` (channel spacing) is used as the integration weight. NaN/blanked voxels are excluded. Variance must be positive; pixels where σ=0 or M0=0 are set to NaN.

### `POST /v1/products/isosurface`
```json
{ "dataset_id": "...", "threshold": 0.05 }
```
Returns base64-encoded mesh data + bounds.

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

## Tasks (async)

For long-running operations the backend queues a task; the client polls for completion.

### `POST /v1/tasks/moment`
### `POST /v1/tasks/pv`
Same request body as the synchronous equivalents. Returns `{ "task_id": "uuid" }`.

### `GET /v1/tasks/{task_id}`
```json
{ "task_id": "...", "status": "pending"|"running"|"done"|"error", "result": { … } }
```
`result` is populated when `status == "done"`.

### `DELETE /v1/tasks/{task_id}`
Cancel and remove.

`BackendClient::waitForTaskCompletion()` wraps the polling loop with exponential back-off.

---

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

## HiPS

### `POST /v1/hips/open`
```json
{ "url": "http://alasky.u-strasbg.fr/DSS/DSS2Merged" }
```
Returns `survey_id`, `order_min`, `order_max`, `frame`, `tile_format`, `ra_center`, `dec_center`, `fov`.

### `POST /v1/hips/hips2fits`
Cut a FITS (or a picture) out of any HiPS survey, via the CDS hips2fits service,
into the Workspace.

```json
{ "hips": "CDS/P/DSS2/color", "width": 512, "height": 512,
  "ra": 83.82, "dec": -5.39, "fov": 0.5, "projection": "TAN",
  "coordsys": "icrs", "rotation_angle": 0.0, "format": "fits" }
```
Returns `output_path`, `workspace_filename`, `bytes`, `is_fits` and `url` — the
request as sent, so a cutout is reproducible outside the application.

Every parameter is checked before the request leaves: an empty survey, a field
of view of zero, a declination off the sky or a cutout over 30 Mpx is refused
with the reason. `min_cut`, `max_cut` and `stretch` are sent only for `jpg`/`png`
— they turn numbers into pixels, and a FITS carries the numbers. A response that
is not a FITS when one was asked for is reported with the service's own
`description` rather than saved under a `.fits` name.

### `GET /v1/hips/hips2fits/surveys`
The starting survey list plus the accepted projections and stretches. The legacy
carried these as a `QStringList` inside the dialog, so correcting a survey id
meant releasing a client.

### `GET /v1/hips/surveys?kind=image&refresh=false`
Every HiPS registered with the CDS MOCServer: `id`, `title`, `url`, `category`,
`regime`, `frame`, `tile_formats`, `kind` and `max_order`. `kind` is the
registry's `dataproduct_type` (`image`, `catalog`, `cube`, …) and the default
filter keeps only the imagery, which is all the viewer can draw as tiles; pass
an empty `kind` for the lot. Surveys whose frame is a planetary body rather
than a sky (`mars`, `moon`, `io`, …) are never returned — they have no
celestial position. The raw document is cached under the HiPS cache
directory for a week, in a file named after the query so changing the requested
fields invalidates it; `refresh=true` fetches it again. A registry that cannot
be reached answers `valid: false` with the reason rather than failing.

### `GET /v1/hips/{survey_id}/allsky?order=<N>`
AllSky mosaic PNG/JPEG bytes.

### `GET /v1/hips/{survey_id}/tile/{order}/{pix}`
Single HiPS tile bytes.

### `POST /v1/hips/{survey_id}/query_tiles`
Given a viewport (RA/Dec center + FOV), return the HEALPix tiles covering it —
NESTED pixel index, centre and four corners per tile — at the requested order,
clamped to the survey's `max_order`. A field of 360° means the whole sphere and
returns every tile of the order (`12 × 4^order` of them), which is what the
all-sky projections ask for. `radius_deg` is the angular radius the client
measured for its own viewport; without it the disc is derived from `fov_deg`,
which is quoted across the width and so misses the corners of a tall window or
a curved projection. **The position is in ICRS and so is every position
returned**, whatever frame the survey is tiled in: the centre is converted into
the survey's frame to pick the tiles and the tile centres and corners are
converted back. Over half the CDS registry is galactic, and reading those tile
coordinates as RA/Dec puts the imagery nowhere near the field asked for. Requires **healpy** on the backend; without it the
route answers `valid: false` with an install hint rather than failing.

### `POST /v1/hips/catalogue_overlay`
Given a HiPS survey viewport and the path of a **CSV/TSV on the backend
filesystem** (confined to the configured data roots), return the rows falling
inside the field as `BackendHiPSCatalogueSource[]`. Selection is by angular
separation from the centre, not by a box in RA — near a pole every right
ascension is in view and no interval says so. `radius_deg` is the radius the
client measured for its own viewport, corners included; without it the radius
is guessed from `fov_deg` and the corners are cut off. It does not query Simbad or
VizieR — use `/v1/resolve/cone_search` for that and overlay the CSV it writes.

---

## Polarisation

Implemented in `backend/app/routers/polarisation.py`, computed in
`backend/app/compute/polarisation.py`. All endpoints require `X-Visivo-Token`
and `X-Visivo-Session`.

### Where the Stokes planes come from

Every endpoint here accepts data in either of the two layouts radio surveys
publish:

- **one file with a `STOKES` axis of length ≥ 2** — the planes are read out of
  the dataset named in the path, and nothing else is needed;
- **one file per plane**, each with a degenerate `STOKES` axis of length 1 —
  MeerKAT MGCLS, ASKAP/POSSUM, anything from Obit MFImage. The caller names the
  companion datasets, which must have been opened in the same session.

```json
"companions": {
  "i_dataset_id": "", "q_dataset_id": "ds_…", "u_dataset_id": "ds_…", "v_dataset_id": ""
}
```

A plane the primary dataset carries itself always wins; companions are consulted
only for what is missing. If a plane is neither present nor supplied, the error
names it:

> `Stokes Q, U required but not available: the dataset carries ['I'] and no
> companion file was supplied for Q, U. Load the Stokes companions first.`

Two rules are worth knowing because published data needs them:

- **A companion's own Stokes header is advisory.** When the file holds a single
  Stokes plane, the role the caller assigned it wins and the disagreement is
  logged. The MGCLS V cubes carry `CRVAL4 = 1`, which decodes as Stokes I;
  refusing them would reject a real dataset over one wrong keyword.
- **λ² comes from the file that supplies Q**, never from the primary. In MGCLS
  the I cube and the Q/U cubes of the same field are on different frequency
  grids (1.3426 vs 1.2838 GHz), and using the wrong one biases every rotation
  measure. Q and U on grids that disagree are refused.

### `GET /v1/stokes/{dataset_id}` · `GET …/binary`
One Stokes plane as a 2-D float32 image.

| Query | Meaning |
|---|---|
| `plane` | `I`, `Q`, `U` or `V` (default `I`) |
| `channel` | spectral plane to take; default 0 |
| `companion_dataset_id` | the dataset holding `plane`, when the primary does not |

JSON returns `{valid, plane, channel, width, height, range_min, range_max,
data_base64}`; `/binary` returns one Visivo binary frame.

### `POST /v1/polarisation/pi/{dataset_id}` · `POST /v1/polarisation/pi/result/binary`
Bias-corrected polarised intensity and position angle.

```json
{
  "dataset_id": "...",
  "channel": 0,
  "line_free_channels": [0, 1, 2],
  "companions": { "q_dataset_id": "ds_…", "u_dataset_id": "ds_…" }
}
```

`PI = sqrt(max(Q² + U² − σ²_QU, 0))`, `PA = ½·atan2(U, Q)` in degrees
(−90…+90). σ²_QU is estimated from `line_free_channels` when at least two are
given, and is 0 otherwise. The binary variant returns **two** frames, PI then
PA.

### `POST /v1/polarisation/rmsynthesis/{dataset_id}` · `POST …/binary`
RM synthesis (Brentjens & de Bruyn 2005).

```json
{
  "dataset_id": "...",
  "phi_range_rad_m2": [-100.0, 100.0],
  "dphi_rad_m2": 1.0,
  "reduce": "peak",
  "region": [x0, y0, width, height],
  "companions": { "q_dataset_id": "ds_…", "u_dataset_id": "ds_…" }
}
```

| Field | Meaning |
|---|---|
| `reduce` | `"cube"` (default) for the whole FDF, `"peak"` for the two peak maps |
| `region` | pixel box; the whole image when absent |

`reduce: "peak"` returns peak `|F(φ)|` and the φ at which each pixel peaks — the
RM map — as two binary frames, plus `phi_peak_min` / `phi_peak_max`. This is
what the desktop client asks for.

`reduce: "cube"` returns the whole dispersion cube as one frame of
`(n_phi · height)` rows × `width` columns. It is refused above **512 MiB**,
with the message naming the ways out:

> `The requested FDF cube is 6250 MiB (400000 φ × 64 × 64), over the 512 MiB
> limit. Ask for reduce='peak', a smaller region, or a coarser φ grid.`

Both variants always return `phi_grid`, `rmsf`, `n_phi` and `n_freq`.

## Spectral

Implemented in `backend/app/routers/spectral.py`. All endpoints require
`X-Visivo-Token` and `X-Visivo-Session`. Three feature blocks:
S-02 (linewidth maps), S-03 (baseline subtraction), S-04 (spectral stacking).

### `POST /v1/spectral/linewidth`
Per-pixel line-width map. Returns both FWHM (Gaussian fit) and equivalent-width maps as base64 float32.

```json
{
  "dataset_id": "...",
  "channel_start": 10,
  "channel_end": 50,
  "mask_enabled": false,
  "threshold_value": 0.0,
  "method": null,
  "rest_freq_hz": null
}
```
`method`: `null` = both maps (JSON path); `"fwhm"` or `"ew"` selects one (binary path).
`rest_freq_hz`: rest frequency in Hz for velocity-axis conversion (FWHM only).

Response: `valid`, `width`, `height`, `scalar_type`, `fwhm_unit`, `ew_unit`, `bunit`, `fwhm_range_min/max`, `ew_range_min/max`, `fwhm_base64`, `ew_base64`.

### `POST /v1/spectral/linewidth/binary`
Same request. Returns a binary payload with two concatenated frames (FWHM then EW), each prefixed with a 12-byte header (width int32, height int32, n_bytes int32). Use `BackendClient::parseBinaryFrame()` to decode.

### `GET /v1/spectral/linewidth/{dataset_id}`
Retrieve a previously computed linewidth result by dataset ID.

---

### `POST /v1/spectral/baseline/{session_id}/{dataset_id}`
Fit and subtract a per-pixel polynomial or median baseline from a spectral cube.

```json
{
  "session_id": "...",
  "dataset_id": "...",
  "channel_start": 0,
  "channel_end": 0,
  "poly_order": 1,
  "method": "polynomial",
  "line_free_channels": [[5, 15], [80, 120]]
}
```
`method`: `"polynomial"` or `"median"`.
`line_free_channels`: flat list `[5, 6, 7]` or list of ranges `[[5, 15], [80, 120]]`.

Response: `valid`, `new_dataset_id` (registered in session — use it directly for moment computation), `width`, `height`, `depth`, `range_min/max`, `poly_order`, `method`, `fit_channels`, `rms_before`, `rms_after`, `output_path`.

---

### `POST /v1/spectral/stack`
Stack N open cubes into a single combined spectrum map.

```json
{
  "dataset_ids": ["...", "..."],
  "method": "mean",
  "weight_by": "uniform",
  "weights": []
}
```
`method`: `"mean"` (default), `"median"`, `"weighted_mean"`.
`weight_by`: `"uniform"`, `"rms"`, `"peak"` (used when `method="weighted_mean"` and `weights` is empty).
`weights`: explicit per-cube weights (used only for `"weighted_mean"`).

Response: `valid`, `rows`, `cols`, `scalar_type`, `method`, `n_cubes`, `range_min/max`, `data_base64`.

### `POST /v1/spectral/stack/binary`
Same request. Returns a single binary frame (same format as `/linewidth/binary`).

---

## SAMP

The SAMP router (`backend/app/routers/samp.py`) is included **without auth dependencies**
(no `X-Visivo-Token`) so a SAMP hub running on the user's machine can reach it directly.

### Messaging

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/v1/samp/send` | Send a SAMP message to one / all peers (legacy generic send). |
| `POST` | `/v1/samp/receive` | Inbound delivery hook used by the bundled hub bridge. |
| `GET`  | `/v1/samp/pending` | Pull pending out-bound messages enqueued by the client. |
| `GET`  | `/v1/samp/inbox` | Pull messages addressed to the running session. |

### Status / hub lifecycle

| Method | Path | Purpose |
|--------|------|---------|
| `GET`  | `/v1/samp/status` | Current hub connection state. |
| `POST` | `/v1/samp/hub-status` | Register the local hub status (heartbeat). |
| `POST` | `/v1/samp/connect` | Bring the session up against the active hub. |

### File transfer / registration

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/v1/samp/files/register` | Register a backend-side FITS file as a SAMP-shareable token. |
| `POST` | `/v1/samp/import-url` | Import a remote URL into the session. |
| `POST` | `/v1/samp/upload-file` | Upload a local FITS to the backend for sharing. |
| `GET`  | `/v1/samp/files/{token}` | Serve a previously registered file by token. |
| `POST` | `/v1/samp/send-fits` | Broadcast a FITS to peers via SAMP. |
| `POST` | `/v1/samp/send-catalogue` | Broadcast a catalogue (votable) to peers. |

---

## Resolve

### `POST /v1/resolve/target`
```json
{ "name": "M87" }
```
Returns `ra_deg`, `dec_deg`, `resolved_name`.

### `POST /v1/resolve/cone_search`
A VO Simple Cone Search against any service.

```json
{ "url": "https://vizier.cds.unistra.fr/viz-bin/conesearch/II/246/out",
  "ra": 83.82, "dec": -5.39, "radius": 0.05, "verbosity": 2, "max_rows": 5000 }
```
Returns `url`, `columns`, `rows` (capped at `max_rows`), `total_rows`,
`truncated`, `position_columns` (`{"ra": …, "dec": …}` when they could be
identified) and `output_path` — **the whole result saved as CSV**, which is what
lets a search end in a catalogue the viewers can open.

Two documents are refused before astropy sees them: one carrying a `STREAM`
with an `href` — the parser would fetch that address itself, outside every
guard, so a service could have `file:///etc/passwd` or an internal port come
back as table cells — and one carrying a DTD, which no VOTable needs and which
is an entity-expansion denial of service waiting to happen. The check parses the
document rather than matching a pattern, because an attribute value may contain
`>`.

`RA`/`DEC`/`SR`/`VERB` are merged into whatever query the service URL already
carries (a VizieR cone-search URL has its own parameters, and appending a second
`?` produces something that is not a URL). The URL is user-supplied, so it goes
through the SSRF guard; `VISIVO_CONE_SEARCH_ALLOWED_HOSTS` trusts specific hosts.
A service reports failure *inside* a valid VOTable, so an empty table is not
necessarily an empty sky — that `INFO value="ERROR"` is raised as the error.

### `GET /v1/resolve/cone_search/services`
A few known endpoints, so the field starts with something.

---

## VLKB

### `POST /v1/vlkb/fetch_cutout`
Download a cutout from a *remote* SODA service to the backend's filesystem.
```json
{ "soda_url": "https://…/soda/sync?ID=…&POS=CIRCLE%2040%200%200.3&POSSYS=GALACTIC",
  "vlkb_bearer": "" }
```
Returns `{valid, error, path}` — `path` is on the **backend**, and is what the
dataset-open route is then given.

The URL comes from the client, so it is checked against SSRF before anything is
fetched (`VISIVO_VLKB_ALLOWED_HOSTS`; private, loopback and link-local targets
are refused unless a host is explicitly trusted).

**What came back is checked before it is called a FITS file.** A SODA service
answers an error with HTTP 200 and a VOTable or an HTML page, and mislabels its
Content-Type in both directions — the observed failure carried
`Content-Type: application/fits` on a plain-text error. So the check is on the
bytes:

| First bytes | Reported as |
|-------------|-------------|
| `SIMPLE` | a FITS file; the path is returned |
| `\x1f\x8b` | gzip — ask for it uncompressed |
| `ustar` at offset 257 | a tar archive — this ID needs the multi-cutout endpoint |
| anything else | the service's own message, extracted from the body |

The message is extracted from a VOTable `<INFO value="ERROR">`, an HTML
`<title>`/`<h1>`/`<p>`, or plain text, and is returned as **plain text**: markup
is stripped, because it is written by a remote service and ends up in a dialog.
A response that is not FITS is deleted rather than kept, and a *cached* response
is re-checked for the same reason — a bad file saved before this check would
otherwise be handed back for ever, and re-requesting the same URL is exactly
what a user does after a failure.

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_VLKB_MAX_BYTES` | 8 GiB | Cap on a single cutout download |
| `VISIVO_VLKB_ALLOWED_HOSTS` | *(unset)* | Hosts that skip the private-address check |

### Multi-cutout (asynchronous batch)

One SODA `sync` request cuts one dataset; the archive's answer to "cut twenty"
is a UWS job. Four routes drive it, and the protocol lives in
`app/vlkb_mcutout.py`.

#### `POST /v1/vlkb/mcutout/submit`
```json
{ "items": [{ "id": "ivo://…", "pos": "CIRCLE 40 0 0.3", "possys": "GALACTIC" }],
  "service_url": "", "vlkb_bearer": "" }
```
`pos` is the same POS string the single-cutout path sends, so a batch covers
exactly the region the user was shown; it is parsed server-side into the
service's `{circle}`/`{range}` shape. Returns
`{valid, error, job_id, phase, count}`.

**A submitted job is never lost.** If the submit succeeds but the first phase
read fails, the route still returns the `job_id` with `phase: "UNKNOWN"` —
reporting an error without the id would leave the batch running on the archive
with nobody able to collect it, and a retry would submit a second one.

#### `POST /v1/vlkb/mcutout/phase`
`{job_id, service_url?, vlkb_bearer?}` → `{valid, error, phase, terminal}`.
A job the archive left `HELD` (accepted but not started) is started here rather
than reported as stuck.

#### `POST /v1/vlkb/mcutout/report`
→ `{valid, error, results: [{index, ok, filename, detail}]}`, in submission
order. `detail` keeps whatever the archive said about a failure — the only clue
to why one dataset of twenty came back empty.

#### `POST /v1/vlkb/mcutout/fetch`
→ `{valid, error, archive, files, bytes}`. Downloads the job's `.tar.gz` **and
unpacks it**, into a staging directory of its own per fetch; `files` are the
cutouts, ready for the ordinary dataset-open route. Goes through the heavy-task
throttle.

**What is refused, and why.** A `job_id` is interpolated into a URL *and* into
that directory, so it must be a plain identifier (`.` and `..` included in the
refusal). Redirects are never followed on these calls: `urllib` copies the
`Authorization` header into the redirected request, so a service answering 302
with a host of its choosing would be handed the user's VLKB token — the two
calls whose answer *is* a 3xx (submit, and the RUN command) read the `Location`
without following it. The token is never sent over plain HTTP. On extraction, a
member that resolves outside the directory, a symlink, a device, or a **sparse**
member is refused — for a sparse member `tarfile` does not check that the map
agrees with the declared size, so the size stops bounding what gets written —
and the totals are capped.

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_VLKB_MCUTOUT_TIMEOUT` | 300 s | Socket timeout for each archive call |
| `VISIVO_VLKB_MCUTOUT_MAX_ITEMS` | 500 | Datasets in one batch |
| `VISIVO_VLKB_MCUTOUT_MAX_BYTES` | 16 GiB | Results archive download |
| `VISIVO_VLKB_MCUTOUT_MAX_EXTRACTED` | 32 GiB | Total unpacked size |
| `VISIVO_VLKB_MCUTOUT_MAX_MEMBERS` | 2000 | Entries in the archive |
| `VISIVO_VLKB_MCUTOUT_MAX_REPORT` | 8 MiB | Job report |
| `VISIVO_VLKB_URL` | *(unset)* | Default archive base URL |

---

### `POST /v1/vlkb/tap`
Run a VLKB catalogue query — one of the named presets over a Galactic box, or
ADQL of your own — and save the result as a CSV catalogue in the Workspace.

```json
{ "preset": "bubbles", "glon_min": 10.0, "glon_max": 12.0,
  "glat_min": -1.0, "glat_max": 1.0, "band": "", "max_rows": 0 }
```
Returns `url`, `query` (the ADQL actually run — a preset is built on the backend,
so this is how the user sees what it asked for), `columns`, `rows` (a preview),
`total_rows`, `truncated`, `output_path`, `workspace_filename` and
`hit_row_limit`.

| Preset | Table |
|--------|-------|
| `bandmerged` | `compactsources.sed_view_final` (positions are `glonft`/`glatft`) |
| `band` | `compactsources.higal<70\|160\|250\|350\|500>` |
| `filaments` | `filaments.filaments` joined to `filaments.branches` |
| `bubbles` | `bubbles.bubbles` |
| `distances` | `compactsources.distances`, with `x`/`y`/`z` derived from `dist`, `glon`, `glat` — this is what makes a 3-D selection a point cloud rather than a list |

These are the legacy's own queries, with one addition: it had a bubble mode —
a button, a selector and an importer — and no bubble query, because
`generateQuery()` had no branch for it, so choosing bubbles ran the compact-source
query. Only `SELECT` statements are forwarded (TAP sync is a read interface), and
the service URL goes through the SSRF guard (`VISIVO_VLKB_ALLOWED_HOSTS`).

### `GET /v1/vlkb/tap/presets`
The presets, the Hi-GAL bands and the configured service URL, so the client does
not carry the VLKB schema inside it.

---

## SED

Greybody fits of a compact source's photometry, and fits against the VLKB grid
of pre-computed theoretical models. Wavelengths are µm and fluxes Jy throughout
(the units of the Hi-GAL band-merged catalogue the SEDs are built from).

The fit lives here rather than in the desktop client on purpose: the legacy
ViaLactea client shelled out to `python sedfit_main.py` with `eval()`-ed argv
strings, so the result depended on the user's own Python. See
[SED fitting](user-guide/sed-fitting) for the science and the workflow.

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
| `VISIVO_VLKB_ALLOWED_HOSTS` | *(unset)* | Comma-separated host allowlist; allowlisted hosts skip the private-address check |
| `VISIVO_SED_MODELS_TIMEOUT` | 300 s | Wall-clock deadline for the whole model-fit request |
| `VISIVO_SED_MODELS_MAX_BYTES` | 64 MiB | Cap on the service's response |
| `VISIVO_SED_MAX_MODELS` | 2000 | Server-side cap on `max_models` |

---

## Simulations

AREPO snapshots: browse one, cost an extraction, run it. See
[Simulation snapshots](user-guide/simulations).

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

---

### Fetching on the client's behalf

`/v1/resolve/cone_search`, `/v1/hips/hips2fits` and `/v1/vlkb/tap` all fetch a
URL the client chose or can override. They go through one helper
(`app/safe_fetch.py`), which applies the SSRF policy to the URL **and to every
redirect target** — validating only the first URL is not enough, since a public
host is free to answer `302 Location: http://169.254.169.254/` and `urlopen`
follows redirects by default. Each has its own allowlist variable
(`VISIVO_CONE_SEARCH_ALLOWED_HOSTS`, `VISIVO_HIPS2FITS_ALLOWED_HOSTS`,
`VISIVO_VLKB_ALLOWED_HOSTS`) for trusting a specific host — which is also how a
service on a private network is reached deliberately.

Failures these routes can explain — a bad parameter, a service's own error
message — come back in `error`. Anything unforeseen is logged with its traceback
and answered with a fixed sentence: `str(exc)` on an unexpected exception
carries server paths and mount points to whoever asked.

---

## Error convention
All endpoints return `{ "valid": false, "error": "<message>" }` on failure.
HTTP status is typically 200 even for logical errors; the client checks `valid`.

---

## Worker pool & heavy-task throttle

CPU-bound work runs in a single process pool of `VISIVO_WORKERS` (default 4)
workers. To keep the GUI responsive while a long compute is running, endpoints
are classified into two tiers:

| Tier | Helper | Examples |
|------|--------|----------|
| **Interactive** | `_run` / `_run_with_limit` | `/v1/cube/preview`, `/slice`, `/subvolume`, `/noise`, `/image/*` |
| **Heavy** | `_run_heavy` / `_run_heavy_with_limit` | `/v1/products/moment`, `/products/isosurface`, `/v1/cube/pv`, `/v1/spectral/*` |

Heavy invocations are gated by a global `asyncio.Semaphore` (`_HEAVY_SEM`)
with capacity `VISIVO_HEAVY_SLOTS` — defaulting to `max(1, VISIVO_WORKERS-1)`
— so the pool is never fully saturated by long jobs and an interactive
request (slice scroll, ROI subvolume, probe) always finds a free worker.

Tunables:

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_WORKERS` | 4 | Total ProcessPoolExecutor workers |
| `VISIVO_HEAVY_SLOTS` | `max(1, WORKERS-1)` | Max concurrent heavy tasks (rest of the pool stays available for interactive requests) |
| `VISIVO_LINEWIDTH_CHUNKS` | `VISIVO_WORKERS` | Row-chunks the linewidth compute is split into (already throttled by `_HEAVY_SEM`) |
| `VISIVO_LINEWIDTH_SNR` | 3.0 | SNR cutoff for skipping background pixels in linewidth fit (0 disables skip) |
| `VISIVO_MOMENT_STREAM_BYTES` | 2 GiB | Materialised-subset size above which M0/M1/M2 stream in spectral slabs (out-of-core) instead of loading the whole range |
| `VISIVO_MOMENT_SLAB_CHANNELS` | 64 | Channels per slab in the streaming moment path |
| `VISIVO_DASK_MODE` | `auto` | Moment compute backend: `auto` (distributed if a scheduler is set, else local threaded Dask for large jobs), `off` (plain worker), `local` (always local threaded Dask), `distributed` (require a cluster) |
| `VISIVO_DASK_MIN_BYTES` | 512 MB | In `auto`, minimum materialised-subset size before a moment is routed to the local threaded Dask path |
| `VISIVO_DASK_SCHEDULER` | *(unset)* | Address of a `dask.distributed` scheduler; when set, moments fan out across the cluster |
