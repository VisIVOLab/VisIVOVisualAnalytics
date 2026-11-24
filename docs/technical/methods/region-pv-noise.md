# Regions, PV diagrams, noise: methods and implementation

User reference: [Regions, PV diagrams, noise](../../user/reference/region-pv-noise.md).

## Region spectrum on a preview

The region spectrum is read from the cube the 3-D view holds. At full
resolution every pixel of the region counts. On a preview (every *n*-th pixel,
or block means of the brick pyramid) the preview's samples whose positions fall
inside the region count, and the provenance's *Spectrum from* line says which.
A region smaller than the preview's spacing takes the sample nearest its
centre.

## Region statistics and flux

`POST /v1/regions/stats/{dataset_id}` returns the statistics and a flux. On a
cube with a per-channel `BEAMS` table it uses the beam of the requested channel,
and the header beam otherwise. Integrated flux is the pixel sum divided by the
beam area in pixels. See [Regions API](../backend-api/regions.md).

## Region coordinates

Regions are kept in the cube's full-resolution pixels at double precision.
Positions typed in *Region Properties* and positions read from a region file
are converted by the backend with the dataset's full WCS (projection,
rotation, skew, unequal pixel scales, distortion), never with the viewer's
axis labels, at sub-pixel precision. ds9 `physical` coordinates go through the
image's `LTM`/`LTV` keywords; each size keeps its own unit (`1'`, `30"`,
`0.01d`). An annulus written with several radii (`annulus(x,y,r1,r2,r3)`)
becomes one ring per pair of radii, named *Annulus, line 4, ring 1/2*, moved
together.

## PV sampling

- One sample every pixel along the path, using the finer pixel size when
  pixels are not square (measured on the sky), so the offset axis is exactly
  linear for diagonal and curved paths.
- Values are bilinear at sub-pixel positions; a blank (NaN) neighbour is left
  out instead of blanking the sample.
- Across the path, *Width* samples one pixel apart are averaged. The band is
  that many sample steps wide on the sky, so its width in arcsec is *Width* times
  the step; the display draws the same band (`pvpath::bandEdges`).
- A spectral range (channels, or values of the chosen axis) is turned into
  channels before anything is read: only those are sampled, and every spectral
  axis written describes just them.
- *Smooth curve* uses a centripetal Catmull-Rom spline through the vertices.
- The spectral coordinate is placed by value, so `CDELT3 < 0` (usual for HI
  cubes) and non-linear axes (`VOPT-F2W`) are drawn where they belong. Up to
  version 1.0.1 a decreasing axis was drawn mirrored.
- The computation runs on the backend at full resolution and reads only the
  pixels around the path. A transposed cube (spectral axis not last) is
  refused.
- While a path is dragged the diagram is recomputed shortly after the pointer
  pauses; if a computation is running, only the latest shape is computed next.

The FITS written by *Save as FITS…* is computed from the path of the displayed
diagram:

| Card | Content |
|---|---|
| `CTYPE1 = 'OFFSET'`, `CUNIT1 = 'arcsec'` | offset along the path, `CRVAL1 = 0` at the first vertex, `CDELT1` = sample step |
| `CTYPE2`, `CUNIT2`, `CRVAL2`, `CDELT2`, `CRPIX2` | the cube's own spectral axis, algorithm and parameters included |
| `BUNIT`, `BMAJ`, `BMIN`, `BPA` | from the cube |
| `RESTFRQ`, `SPECSYS`, … | copied when present |
| `PVWIDTH`, `PVWIDARC`, `PVSTEP` | width (cube pixels and arcsec) and sample step (arcsec) |
| `PVCHAN1`, `PVCHAN2` | first and last cube channel kept (0-based) |
| `HISTORY` | every vertex, numbered from 1 as on the display, in pixels and in sky coordinates |

See [`POST /v1/products/pv_fits`](../backend-api/products.md).

## Noise estimate and blank pixels

σ = 1.4826 × MAD per channel over the box. MAD × 1.4826 equals the standard
deviation for Gaussian noise and is insensitive to up to 50 % contamination by
sources or artefacts.

Many cubes use exactly 0.0 for off-source pixels. Over a box that mostly
covers them, the median and the MAD are both 0 and σ = 0. When more than half
the finite pixels of a channel are exactly 0, the zeros are excluded from that
channel's MAD; when they are a minority they are kept, because baseline-
subtracted or calibrated cubes have noise centred on zero.

The response's `blank_fraction` (0 to 1, the fraction of finite pixels in the
box that are exactly 0) drives the warning: above 20 % a note that the result
was computed over all finite pixels; above 50 % the advice to pick a box inside
the data, with σ and MAD computed on the non-zero pixels only.

The value passed to other tools is the median of the per-channel σ. See
[`POST /v1/cube/noise`](../backend-api/cube.md).
