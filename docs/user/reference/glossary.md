# Glossary

Terms used in the application and in this documentation.

```{glossary}
Beam (synthesised beam)
  The resolution element of a radio interferometer, given by the header
  keywords `BMAJ`, `BMIN` and `BPA` (degrees). When present, the 2-D slice
  draws it as an ellipse in the bottom-left corner.

Blink
  Rapid alternation of layers 0 and 1 in the image viewer (*View ▸ Blink
  Layers*), to spot changes between two images. See [Image viewer](image-viewer.md).

BUNIT
  FITS keyword for the unit of the pixel values (`Jy/beam`, `K`). Region
  statistics and moment maps report values in this unit.

Channel
  One plane of a cube along its spectral axis.

Cone search (SCS)
  The IVOA *Simple Cone Search*: a service returns the sources within a radius
  of a position, as a VOTable. See [Catalogues and HiPS](catalogues-hips.md).

CTYPE
  FITS keyword naming each WCS axis (`RA---SIN`, `VELO-LSR`, `FREQ`). It
  decides which axis is spectral and which projection applies.

Cutting plane
  The plane in the 3-D cube view that shows the current 2-D slice and where it
  sits along the spectral axis.

EW (equivalent width)
  A model-free line width, `∫ I dv / max(I)`. Use it for non-Gaussian profiles.
  See [Spectral tools](spectral-tools.md).

FITS
  *Flexible Image Transport System*, the standard astronomy format for images,
  cubes, tables and headers.

FK5 / J2000
  Equatorial coordinates (RA, Dec) at epoch J2000.0. The default sky frame.

FWHM
  *Full width at half maximum* of a line or a source. For a Gaussian,
  `FWHM = 2.3548 σ`.

Galactic
  Coordinates aligned with the plane of the Milky Way (longitude `l`, latitude
  `b`).

HiPS
  *Hierarchical Progressive Survey*, the IVOA multi-resolution tile format for
  all-sky surveys. See [Catalogues and HiPS](catalogues-hips.md).

hips2fits
  A CDS service that reprojects a HiPS survey onto a WCS you choose and returns
  a FITS file you can measure on.

Isosurface
  The 3-D surface where the cube's intensity equals a chosen threshold.

Line identification
  Expected spectral-line positions drawn on the spectrum plot (*Load Lines…*),
  shifted by the source's systemic velocity or redshift. See
  [Spectral tools](spectral-tools.md).

LSRK / LSR
  *Local Standard of Rest (kinematic)*, the usual frame for radial velocities of
  Galactic objects, given by the `SPECSYS` keyword.

MAD
  *Median absolute deviation*. Multiplied by 1.4826 it gives a robust σ for
  Gaussian noise; used for noise estimates and region statistics.

Moment (M0, M1, M2)
  Maps that summarise a spectral line at each pixel: integrated intensity,
  intensity-weighted velocity, velocity dispersion. See [Moment maps](moment-maps.md).

PV diagram
  *Position-velocity diagram*: intensity along a path on the sky against
  velocity. See [Regions, PV and noise](region-pv-noise.md).

Probe
  The spectrum of a single pixel, picked on the 2-D slice or on the 3-D cutting
  plane. See [Cube viewer](cube-viewer.md).

ROI (Camera ROI)
  *Region of interest*. In the cube viewer, *Use Camera ROI* loads at full
  resolution only the part of the cube visible in the 3-D view.

SAMP
  *Simple Application Messaging Protocol*, the IVOA standard for exchanging
  images and catalogues with TOPCAT, Aladin or DS9. See
  [Interoperability](interoperability.md).

Session
  The set of datasets opened on one backend in one run of the application.
  Tools that combine cubes, such as stacking, work within one session.

SNR
  *Signal-to-noise ratio*, here peak over σ per pixel.

Spectral axis
  The cube axis carrying frequency, velocity or wavelength (`FREQ`, `VRAD`,
  `VOPT`, `VELO-LSR`). The cube viewer's status bar shows its kind and unit.

Spectral smoothing
  Display-only smoothing of the spectrum plot (*Smooth:* in the SPECTRUM panel):
  Hanning, boxcar of 3, 5 or 7 channels, Gaussian of σ = 1 or 2 channels. It
  does not change exported data.

SPECVAL, SPECTYPE, SPECUNIT
  Keywords written into a single channel exported as 2-D FITS: the channel's
  spectral value, the source cube's `CTYPE3` and `CUNIT3`. Tools that need a
  frequency read them.

TAP / ADQL
  *Table Access Protocol* and its SQL-like query language with astronomical
  functions. The VLKB catalogues are queried this way.

VBT
  *VisIVO Binary Table* (`.bin` + `.head`): a column format for large point
  clouds and for regular grids, whose fields open as cubes. See
  [VBT viewer](vbt-viewer.md).

VLKB
  *ViaLactea Knowledge Base*: a catalogue and cutout service for the Galactic
  plane. Sign in from *Settings ▸ Data Access ▸ VLKB ▸ Sign in*. See
  [VLKB archive](vlkb-archive.md).

WCS
  *World Coordinate System*: the mapping from pixels to sky and spectral
  coordinates, defined by `CTYPE`, `CRVAL`, `CRPIX`, `CDELT` or `CD` and related
  keywords.
```
