# Glossary

Quick reference for terms used across the app and this documentation.
Astronomy and tooling vocabulary mixed; see also the cited pages for
deeper explanations.

```{glossary}
Beam (synthesised beam)
  The angular resolution element of a radio interferometer, described by
  the FITS header keywords `BMAJ` (major axis), `BMIN` (minor axis), and
  `BPA` (position angle), all in degrees. VisIVO draws a filled
  semi-transparent ellipse in the bottom-left corner of the 2-D slice
  view when these keywords are present. See
  [Cube viewer — Beam indicator](cube-viewer#beam-indicator).

BUNIT
  FITS header keyword carrying the brightness / pixel-value unit (e.g.
  `Jy/beam`, `K`, `counts`). All region statistics and moment maps
  report values in BUNIT when it is set on the source cube / image.

CTYPE
  FITS header keyword that names each WCS axis (e.g. `RA---SIN`,
  `DEC--SIN`, `VELO-LSR`). VisIVO inspects CTYPE to decide axis order,
  whether axis 3 is spectral, and which projection to use.

Channel
  One pixel along the spectral axis of a cube. The cube has `nchan`
  channels; navigating with the slice slider moves you channel-by-channel.

Cutting plane
  The plane indicator in the 3-D cube view that shows where the current
  2-D slice sits along the spectral axis. The plane is textured with the
  slice contents and is interactive (see [Cube viewer](cube-viewer)).

dataset_id
  Backend identifier assigned to a FITS file the first time you open it
  via `/v1/datasets/open`. The client carries it as `X-Visivo-Dataset`
  in subsequent requests. Different opens of the same file inside the
  same session reuse the same ID.

EW
  *Equivalent width* — a model-free width estimate equal to
  `∫ I dv / max(I)`. Use it when the line profile is non-Gaussian. See
  [Line-width maps](spectral-tools#line-width-maps-s-02).

FWHM
  *Full width at half maximum* — the spectral width at which the
  intensity drops to half of the peak. For a Gaussian profile,
  `FWHM = 2.3548 σ`.

FITS
  *Flexible Image Transport System* — the standard astronomy file format
  for images, cubes, tables, and headers.

FK5 / J2000
  Equatorial coordinate system (RA, Dec) defined by the FK5 catalogue at
  epoch J2000.0. Default sky frame in VisIVO.

Galactic
  Coordinate system aligned with the plane of the Milky Way (longitude
  `l`, latitude `b`). VisIVO converts to/from FK5 with `wcscon()`.

HiPS
  *Hierarchical Progressive Survey* — multi-resolution tiled image format
  used by the IVOA to serve all-sky surveys. See
  [HiPS viewer](catalogues-hips#hips-viewer).

Isosurface
  A closed 3-D surface where the cube's intensity equals a given
  threshold. Useful to highlight dense clumps, jets, shells. Extracted
  server-side with `vtkFlyingEdges3D`.

Line identification (spectral-line overlay)
  Overlay of expected spectral-line positions on the spectrum plot.
  Loaded from a two-column CSV file (`frequency,label`); lines are drawn
  as vertical dashed amber markers with rotated labels. See
  [Cube viewer — Line identification overlay](cube-viewer#line-identification-overlay).

LRU cache
  *Least Recently Used* — eviction policy of the backend's
  `PRODUCT_CACHE`. Compute results (moments, isosurfaces, line-width
  maps, PV diagrams) are memoised so identical re-requests are free.

LSRK / LSR
  *Local Standard of Rest (Kinematic)* — reference frame for radial
  velocities of Galactic objects. Carried via the `SPECSYS` FITS keyword;
  VisIVO reports it in the cube's spectral axis tooltip.

MAD
  *Median Absolute Deviation* — robust estimator of dispersion.
  Multiplied by 1.4826 it gives a robust 1σ estimate for Gaussian data,
  insensitive to outliers. Used in noise estimation and region stats.

Moment (M0, M1, M2, …)
  Statistical summary of a spectral line projected on the spatial axes.
  See [Moment maps](moment-maps) for the formulas and use cases.

PV
  *Position-Velocity diagram* — 2-D plot of intensity along a path on the
  sky (X) vs. velocity (Y). Standard tool for kinematic inspection. See
  [Regions, PV, noise](region-pv-noise#position-velocity-diagrams).

Probe
  Click-to-extract spectral profile at a single pixel of the cube's 2-D
  slice (or via 3-D plane click). See
  [Cube viewer](cube-viewer#probing-a-spectrum).

ROI
  *Region of Interest*. In the cube viewer, the *Camera ROI* is the
  spatial sub-volume corresponding to the current 3-D camera viewport;
  the backend serves only that ROI at full resolution.

SAMP
  *Simple Application Messaging Protocol* — IVOA standard for inter-VO
  application messaging. Lets VisIVO exchange FITS / catalogues with
  TOPCAT / Aladin / DS9 at runtime. See
  [SAMP](catalogues-hips#samp).

Session
  Backend-side container of all the datasets opened in one workflow.
  Identified by `X-Visivo-Session`, returned by `/v1/datasets/open`.
  Multi-cube tools (Spectral Stacking) operate within a single session.

SNR
  *Signal-to-Noise Ratio*. Computed per pixel as `peak / σ` (with σ from
  MAD). VisIVO uses an SNR cutoff to skip background pixels in the
  line-width Gauss fit.

Spectral smoothing
  Display-only 1-D convolution applied to the spectrum plot in the
  *Spectral Profile* window. Available kernels: Hanning, Boxcar (3/5/7
  channels), Gaussian (σ=1/2 channels). The smoothing is NaN-safe and
  affects the stats bar but not CSV export. See
  [Cube viewer — Spectral smoothing](cube-viewer#spectral-smoothing).

Spectral axis
  Third FITS axis carrying frequency / velocity / wavelength information.
  CTYPE3 examples: `VELO-LSR`, `VRAD`, `VOPT`, `FREQ`. VisIVO reports the
  active axis kind, unit, and trustworthiness in the cube viewer status
  bar.

VBT
  *VisIVO Binary Table* — efficient column-oriented binary format used
  by some VisIVO datasets for million-row point clouds.

VLKB
  *Via Lactea Knowledge Base* — VisIVO's data catalogue / cutout service.
  Reachable via the OIDC PKCE flow from the Startup Dialog.

VTK
  *Visualization Toolkit* — the open-source 3-D rendering library
  underpinning the viewer windows.

WCS
  *World Coordinate System* — the mapping from pixel indices to physical
  sky / spectral coordinates, defined by FITS keywords `CTYPE`, `CRVAL`,
  `CRPIX`, `CDELT`, `CD`, …. VisIVO reads it via libwcs.

worker pool
  Backend process pool that runs the CPU-bound work. Default size 4
  (controlled by `VISIVO_WORKERS`). Heavy tasks are gated by a
  semaphore (`VISIVO_HEAVY_SLOTS`) so interactive requests always have
  a free worker; see [heavy-task throttle](../async-patterns#heavy-task-throttle-backend-side).
```
