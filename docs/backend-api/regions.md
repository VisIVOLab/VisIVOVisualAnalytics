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
(default 0). A pixel is in the region when its centre is: inside a circle,
ellipse or (rotated) box, between the radii of an annulus (both edges
included), inside a polygon (even-odd rule), or nearest a point.

The statistics are computed on the file at full resolution, out of core: the
region's bounding rows are read in blocks (`app/region_stats.py`), so a region
of any size fits in bounded memory, in double precision (float64 data keeps its
digits). They are exact: the variance is combined block by block, and the
median and MAD come from the values themselves up to 32 M of them. Beyond
that, a random sample of 4 M values drawn in the first pass brackets the rank
of each, and one more pass counts the values below the bracket and keeps those
inside it, which hold the exact value: three passes over the region in all,
one for the moments, one for the median, one for the MAD (issue #99). Should a
bracket miss its rank (about once in 10^15), or hold more values than memory
takes (regions above about 8 G pixels, or values with many ties), the rank is
found by histogram passes that end on the exact value; the result is the same.
The passes after the first read a copy of the region's valid values that the
first pass writes to local disk (float32 when every value is one, else
float64), so the file is read once: on a shared filesystem each pass over the
file would read it again. The copy goes to `VISIVO_REGION_SPILL_DIR` when it
is set; otherwise to the system's temporary directory (`$TMPDIR` when the
scheduler sets one per job), unless that is held in memory (a tmpfs `/tmp`,
common on clusters and in containers, where the copy would count against the
job's memory). It is deleted when the statistics are done; without a place or
room for it, the passes read the file. A region of 1.07 G pixels takes about
9 s on a laptop SSD (it took 16 to 23 s with the histogram passes), and a
region of 81 M pixels about 1 s. A file compressed
as a whole (`.fits.gz`) cannot be read in parts: its plane is read whole. The
image viewer asks for them when it displays a reduced level, or when a region
reaches beyond the pixels it has loaded (issue #95). Response: `valid`, `error`, `shape`, `total_count`,
`valid_count`, `blanked_count`, `min`, `max`, `mean`, `rms` (standard
deviation), `sum`, `median`, `mad_sigma` (1.4826 × MAD), `bunit`, `channel`,
`flux_jy`, and `beam_area_pix` when a flux was computed. `flux_jy` is the sum
divided by the beam area in pixels, set only for per-beam data with a known
beam (otherwise `null`). The beam is the selected channel's: on a cube with a
CASA `BEAMS` table (one beam per plane) that table's entry for `channel`, and
the primary-header `BMAJ`/`BMIN` only when there is no table entry. A region of blank pixels only is an answer:
`valid_count` 0, and the statistics `null`. A region with no pixel of the
image at all is 422.
