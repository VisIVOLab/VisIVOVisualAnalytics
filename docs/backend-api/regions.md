# Regions

Region interchange with CASA CRTF and DS9 region files, and statistics inside
a region (`backend/app/routers/regions.py`, parsing in `backend/app/regions.py`).
Regions drawn in CARTA, CASA or ds9 can be loaded into the client and the
client's regions written back out.

## Canonical region

Every route speaks the same JSON region model:

```json
{ "shape": "circle", "frame": "icrs", "center": [83.822, -5.391],
  "radius": 30.0, "unit": "arcsec", "props": { "color": "green", "text": "" } }
```

| Field | Meaning |
|---|---|
| `shape` | `circle`, `ellipse`, `box`, `polygon`, `point` or `annulus` |
| `frame` | `image` or a world frame: `icrs`, `fk5`, `galactic` |
| `center` | Pixels when `frame` is `image`, otherwise decimal degrees |
| `radius` | Circle |
| `radii` | Annulus, `[inner, outer]` |
| `semi_axes` | Ellipse, `[major, minor]` |
| `size` | Box, `[width, height]` |
| `angle_deg` | Ellipse and box position angle |
| `points` | Polygon vertices, pixels or degrees according to `frame` |
| `unit` | Unit of the sizes and radii: `pix`, `deg`, `arcsec` or `arcmin` (always `pix` in the `image` frame) |
| `props` | Optional cosmetics such as `color` and `text` |

## Frame conversion

`parse` and `export` take an optional `to_frame`: empty keeps each region in
its own frame, `image` converts to pixels, and a world frame converts to that
frame (any other value means the dataset's own world frame). A conversion
needs `dataset_id`, whose celestial WCS is used (422 without `dataset_id`, or
when the dataset has no celestial WCS). These two routes use the 1-based pixel
convention of FITS and of the region files.

## Routes

### `POST /v1/regions/parse`
Parse CRTF or DS9 text into canonical regions.

```json
{ "text": "# Region file format: DS9 version 4.1\nfk5\ncircle(83.822,-5.391,30\")",
  "format": "auto", "dataset_id": "", "to_frame": "" }
```

`format` is `auto` (detected from the text), `crtf` or `ds9`. Response:
`{valid, error, format, n_regions, regions}`. Text that cannot be parsed is
422.

### `POST /v1/regions/export`
Serialise canonical regions to CRTF or DS9 text.

```json
{ "regions": [{ "shape": "box", "frame": "image", "center": [100, 120],
                "size": [40, 20], "angle_deg": 0, "unit": "pix" }],
  "format": "crtf", "dataset_id": "", "to_frame": "" }
```

`format` (`crtf` or `ds9`) is required. Response:
`{valid, error, format, text}`. A region that cannot be written is 422.

### `POST /v1/regions/stats/{dataset_id}`
Pixel statistics inside one region.

```json
{ "region": { "shape": "circle", "frame": "image", "center": [120, 88],
              "radius": 6, "unit": "pix" },
  "channel": null }
```

An `image` region is in 0-based pixels; a world-frame region is converted to
pixels through the dataset's WCS first. `channel` selects the plane of a cube
(default 0). Response: `valid`, `error`, `shape`, `total_count`,
`valid_count`, `blanked_count`, `min`, `max`, `mean`, `rms` (standard
deviation), `sum`, `median`, `mad_sigma` (1.4826 × MAD), `bunit`, `channel`,
`flux_jy`, and `beam_area_pix` when a flux was computed. `flux_jy` is the sum
divided by the beam area in pixels, set only for per-beam data with a known
beam (otherwise `null`). A region with no finite pixel is 422.
