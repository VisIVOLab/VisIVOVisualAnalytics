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
| `shape` | `circle`, `ellipse`, `box`, `polygon`, `point`, `annulus`, `ellipse_annulus`, `box_annulus`, `line`, `vector` or `text` |
| `frame` | `image` or a sky frame: `icrs`, `fk5` (J2000), `fk4` (B1950), `galactic`, `ecliptic` (mean ecliptic of J2000) |
| `center` | Pixels when `frame` is `image` (1-based, as in ds9), otherwise decimal degrees in that frame. For a vector, its start |
| `radius` | Circle |
| `radii` | Annulus, `[inner, outer]` |
| `semi_axes` | Ellipse, `[a, b]`: `a` along `angle_deg`, `b` across it (ds9's order) |
| `size` | Box, `[width, height]` |
| `inner`, `outer` | Elliptical annulus: the semi-axes `[a, b]` of its inner and outer ellipse. Box annulus: the `[width, height]` of its inner and outer box |
| `length` | Vector, from `center` along `angle_deg` |
| `angle_deg` | Angle of an ellipse, box, their annuli and a vector, as ds9 writes it: counter-clockwise from pixel +x in the `image` frame, from the longitude axis in a sky frame |
| `points` | Polygon vertices, or the two (or more) vertices of a line, in pixels or degrees according to `frame` |
| `text` | The string of a text region |
| `unit` | Unit of the sizes and radii: `pix`, `deg`, `arcsec` or `arcmin`. One unit per region; pixels in a sky frame and angles in the `image` frame are allowed, as ds9 and CRTF allow them |
| `props` | Optional style, below |
| `source_line` | Line of the file the region was read from (1-based); regions made from one statement share it |

`props` keeps what the files say about a region: `color`, `width` (line
width), `dash`, `dashlist`, `font`, `text` (a label), `textangle`, `tag` (a
list), `include: false` for an excluded region (ds9 and CRTF `-` prefix, or
ds9 `include=0`), `source: false` for a ds9 background region, `point` and
`point_size` (ds9 point style; for CRTF symbols that have one), `symbol`
(the CRTF symbol as written), `arrows` (`[start, end]` of a line, as ds9
`line=0 1`; a CRTF vector is a line with `[0, 1]`), `arrow` (a ds9 vector),
`ruler` (a ds9 ruler read as a line), `projection_width`, `annotation` (CRTF
`ann`), and CRTF `fontsize`, `fontstyle`, `symsize`, `symthick`, `range`,
`corr`, `labelpos`, `labelcolor`. ds9 properties on `global` lines apply to
every region after them unless the region sets its own (a `+` prefix or
`source` on a region overrides a global `include=0` or `background`); a
number that is not finite is dropped from `props`; display-only flags
such as `select` or `highlite` are not kept.

## Reading the files

Every ds9 shape is read. `circle`, `ellipse`, `box`, `polygon`, `point` and
`text` map to the shapes of the same name; `point` takes every ds9 point
style, written as `point=diamond 11` or as `diamond point(x,y)`; `line`,
`ruler` and `segment` become lines, `projection` its centre line; `vector`
keeps its start, length and angle. An annulus with N radii
(`annulus(x,y,r1,r2,r3)`, or `inner outer n=N`) becomes N−1 annuli, one per
ring, with the same `source_line`, as ds9 shows them; so do elliptical
(`ellipse(x,y,a1,b1,a2,b2,…,angle)`) and box annuli, which become
`ellipse_annulus` and `box_annulus`. Shapes ds9 writes behind `#` (`# text(…)`,
`# vector(…)`, `# ruler(…)`, …) are read; any other commented line is a
comment. A `composite` is not kept as a group: the regions inside it are read
one by one, and the composite line is reported. `compass`, `panda`, `epanda`
and `bpanda` are reported, not read.

CRTF shapes: `circle`, `ellipse`, `box` (two corners), `centerbox`, `rotbox`,
`annulus`, `poly`, `symbol` (a point), `line`, `vector` (a line with an arrow
at its end) and `text`, with the `ann` and `-` prefixes. A CRTF ellipse
`[[x, y], [b1, b2], pa]` has `b1` along the position angle `pa`, measured
from north (from +y in pixels), so its canonical form is `semi_axes`
`[b2, b1]` at `angle_deg` `pa`, and the other way round on export; a
`rotbox` angle is read as ds9's, and a `centerbox` with an angle is a rotated
box. CRTF pixel positions
are 0-based, as in CASA; they become 1-based in the canonical model and go
back to 0-based when written.

### Statements that are not read

Nothing is dropped silently. Each statement that does not become a region is
reported with its line, its text and the reason: a shape that is not
supported, a coordinate system that is not (ds9 `wcs`, `linear`, `detector`,
`amplifier`; a CRTF `coord` other than those listed below), a wrong number of
values, a number that cannot be read or is not finite, a latitude beyond
±90°, a negative size, more than 1000 rings in one annulus, a
region mixing pixel and angular sizes, a line that is no region at all, and,
when the regions are converted, a sky region on a dataset without a celestial
WCS or one that does not land on the dataset's projection. The other regions
of the file are read either way.

### Sizes and frames

Every size in a file carries its own unit, so `ellipse(10,20,1',30",0)` is
read as semi-axes of 1 and 0.5 arcmin; angular sizes of one region are
expressed in the unit of its first size. A size without a unit is in pixels
in the image frame and in degrees in a sky frame, as in ds9. ds9 suffixes
`d` (degrees), `r` (radians), `p` and `i` (pixels), `'` and `"` are read, and
CRTF's `deg`, `rad`, `arcmin`, `arcsec`, `mas` and `pix`. A region that mixes
pixel and angular sizes is refused (422), since only a WCS relates them.

ds9 files may use `image`, `physical`, `fk5`/`J2000`, `fk4`/`B1950`, `icrs`,
`galactic` and `ecliptic`, switch between them anywhere in the file, also on
one line (`fk5;circle(...);image;point(...)`); regions under `wcs`,
`linear`, `amplifier` or `detector` are reported rather than read in the
wrong frame. `physical` coordinates are moved to image pixels with the
dataset's IRAF keywords, image = LTM · physical + LTV (`LTM1_1`, `LTM1_2`,
`LTM2_1`, `LTM2_2`, `LTV1`, `LTV2`; a missing one takes the identity's
value; a 2× blocked image has LTM 0.5, a Chandra cut LTV −3799.5):
positions, the sizes and angle of an ellipse, box or vector along their
axes, radii by the area scale √|det LTM|. Without a `dataset_id`, or
when the header has none of these keywords, physical equals image, as in
ds9. A size with the `p` suffix is a physical pixel and one with `i` an image
pixel in any frame; a bare size in `physical` is a physical pixel. CRTF takes `coord=` per region or on a `global` line: `J2000`,
`B1950` (FK4), `ICRS`, `GALACTIC`, `ECLIPTIC`; other systems are refused.
A CRTF sky region with no `coord` is read as J2000. Sexagesimal positions are
hours only for the RA of an equatorial frame.

## Frame conversion

`parse` and `export` take an optional `to_frame`: empty keeps each region in
its own frame, `image` converts to pixels, and a sky frame converts to that
frame (any other value means the dataset's own sky frame). Positions are
transformed with astropy from the region's frame to the frame of the
dataset's celestial WCS, then to pixels, so a galactic region lands in the
right place on an equatorial image and vice versa; a sky-to-sky conversion
(for example FK5 to galactic) moves the coordinates and writes the new frame.
Sizes are angles on the sky and do not change between sky frames. Between
angles and pixels a size is measured along its own axis, in the frame the
region was drawn in, through the grid at the region's centre: the width of a
sky box along its sky angle, the height across it, the length of a vector
along it; a pixel box with angular sizes along its pixel axes. Radii (circle,
annulus) use the local area scale √|det J|, where J is the grid's
pixels-per-degree matrix at the centre. So a rotated or skewed CD matrix and
unequal pixel scales give the right pixel sizes along each axis; only a
circle on a grid with unequal scales, which is an ellipse in pixels, is
approximated by the circle of the same area. On such a grid the height of a
box or ellipse, measured across its axis, is not the same length in pixels
and on the sky in both directions, so converting a region to pixels and back
changes it slightly (a 4-pixel height came back as 4.25 on a 1.3:1 grid).

An angle is measured from the axes of the region's frame, so when the frame
changes it is rotated by the angle between the two frames' north directions
at the region's centre (measured numerically from a 1" step north: about
58.6° between FK5 and galactic at the Galactic centre). To or from pixels
the axis is followed one pixel along the sky and read on the grid, so the
grid's rotation, parity (east to the right mirrors it), unequal pixel scales
and skew are all taken from the WCS; on a north-up, east-left grid with
square pixels the number is unchanged. An ellipse or box without an angle is
at 0 and turns the same way.

A conversion involving pixels needs `dataset_id`, whose celestial WCS is used
(for a cube, the WCS of its sky axes); a sky-to-sky conversion does not. In
`parse`, a region that cannot be converted (no `dataset_id`, a dataset
without a celestial WCS, a region that does not project onto the WCS) is
reported by its line like any other statement not read; `export` answers 422
instead. These two routes use the 1-based pixel convention of FITS and of the
region files. Positions are written with 12 significant digits.

The regions of one file are converted together, so a file of thousands of
sky regions costs little: 5000 FK5 ellipses onto an ICRS image in 0.1 s on a
laptop.

## Routes

### `POST /v1/regions/parse`
Parse CRTF or DS9 text into canonical regions.

```json
{ "text": "# Region file format: DS9 version 4.1\nfk5\ncircle(83.822,-5.391,30\")",
  "format": "auto", "dataset_id": "", "to_frame": "" }
```

`format` is `auto` (detected from the text), `crtf` or `ds9`; any other is
422. With a `dataset_id`, ds9 `physical` coordinates use its LTM/LTV even when
`to_frame` is empty. Response:

```json
{ "valid": true, "error": "", "format": "ds9", "n_regions": 2, "n_skipped": 1,
  "diagnostics": [{ "line": 4, "text": "panda(10,10,0,360,4,1,2,2)",
                    "reason": "pie-annulus (panda) regions are not supported" }],
  "diagnostics_truncated": false, "regions": [ ... ] }
```

`regions` are those read (and converted); `diagnostics` the statements that
were not, sorted by line, with `n_skipped` counting them. Only the first 1000
diagnostics are returned (`diagnostics_truncated` is then true; `n_skipped`
counts them all). When nothing could be read but something was reported,
`valid` is false and `error` names the first reason; text with neither
regions nor diagnostics (only comments) is valid with no regions.

### `POST /v1/regions/export`
Serialise canonical regions to CRTF or DS9 text.

```json
{ "regions": [{ "shape": "box", "frame": "image", "center": [100, 120],
                "size": [40, 20], "angle_deg": 0, "unit": "pix" }],
  "format": "crtf", "dataset_id": "", "to_frame": "" }
```

`format` (`crtf` or `ds9`) is required. Response:
`{valid, error, format, text}`. A region that cannot be written is 422; CRTF
has no elliptical or box annulus and no line of more than two vertices. ds9
text and vector regions are written behind `#`, as ds9 writes them, and a
line of more than two vertices as a ds9 `segment`; a vector becomes a CRTF
`vector` between its two ends.

### `POST /v1/regions/stats/{dataset_id}`
Pixel statistics inside one region.

```json
{ "region": { "shape": "circle", "frame": "image", "center": [120, 88],
              "radius": 6, "unit": "pix" },
  "channel": null }
```

An `image` region is in 0-based pixels; a sky-frame region (or an `image`
region with angular sizes) is converted to pixels through the dataset's WCS
first, as above. `channel` selects the plane of a cube
(default 0, clamped to the cube's channels; the response's `channel` is the
plane used). A pixel is in the region when its centre is: inside a circle,
ellipse or (rotated) box, between the radii of an annulus (both edges
included), inside a polygon (even-odd rule), or nearest a point. An elliptical or box
annulus takes the pixels inside its outer shape and not strictly inside its
inner one. Lines, vectors and text have no statistics (422).

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
region of 81 M pixels about 1 s. A compressed dataset (`.fits.gz`, `.fz`) is
read from its decompressed copy in the workspace cache (see
[Datasets](datasets.md)), in parts like any FITS file; a file compressed as a
whole that is read directly has its plane read whole. The
image viewer asks for them when it displays a reduced level, or when a region
reaches beyond the pixels it has loaded (issue #95). Response: `valid`, `error`, `shape`, `total_count`,
`valid_count`, `blanked_count`, `min`, `max`, `mean`, `rms` (standard
deviation), `sum`, `median`, `mad_sigma` (1.4826 × MAD), `bunit`, `channel`,
`flux_jy`, and `beam_area_pix` when a flux was computed. `flux_jy` is the sum
divided by the beam area in pixels, set only for per-beam data with a known
beam (otherwise `null`). The beam is the selected channel's: on a cube with a
CASA `BEAMS` table (one beam per plane) that table's entry for `channel`, and
the primary-header `BMAJ`/`BMIN` only when there is no table entry. A region of blank pixels only is an answer:
`valid_count` 0, `sum` 0, and the other statistics and `flux_jy` `null`. A region with no pixel of the
image at all is 422.
