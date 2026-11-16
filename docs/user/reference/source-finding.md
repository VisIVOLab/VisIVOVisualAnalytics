# Source finding and cross-matching

Two tools. SoFiA-2 searches a whole cube for sources and returns a catalogue and
a mask. Cross-match asks an external catalogue which of its sources lie on an
image. To segment one source you can already see, use the
[Kinematic Lasso](kinematic-lasso.md).

| Task | Command | Viewer |
|---|---|---|
| Find sources in a cube | *Tools ▸ Sources & Kinematics ▸ Source Finding (SoFiA-2)…* | cube |
| Catalogued sources on an image | *Tools ▸ Catalogue ▸ Cross-match with Catalogue…* | image |

For a walk-through, see
[Find and measure a source](../use-cases/02-find-and-measure-a-source.md).

## SoFiA-2

[SoFiA-2](https://gitlab.com/SoFiA-Admin/SoFiA-2) runs on the backend against
the open cube. It must be installed (version 2.7) where the backend runs, as
`sofia2` or `sofia` on its `PATH`, or named by `VISIVO_SOFIA_BIN`; otherwise the
run reports *SoFiA-2 is not available on this backend*. The parameter suggestion
works without it.

When the dialog opens, the backend measures the cube (about a second) and
suggests parameters.

### Dialog

- *Data type*: what the suggestion deduced: emission, absorption (searched in
  the inverted cube) or continuum not subtracted. The line below says why.
- *Line widths*: HI (lines of 15 to 120 km/s) or Galactic lines (2 to 20 km/s,
  not yet validated). A cube without a rest frequency does not say which.
- *Confirm*: appears when the evidence is unclear (*Please confirm…*). *Run
  SoFiA-2* waits until you pick or confirm a value; the parameters are then
  suggested again.
- Measurements: noise σ, beam in pixels, channel width in km/s, adjacent-channel
  correlation, number of resolution elements, noise variation over the band and
  the field, cube shape, and any warning (too few resolution elements for the
  reliability filter, exact-zero blanks, no beam or channel width in the header,
  a cube too large for the node's memory).
- Parameter table: one row per SoFiA-2 parameter with its *Value* (double-click
  to edit), status, SoFiA's default and the reason for the value. Values that
  differ from SoFiA's default are bold; your edits are in italics. A blank value
  is not sent, so SoFiA's default applies. Your edits survive a new suggestion.
- *Reset to suggested*: discards your edits and the rows you added.
- *Add parameter…*: adds a parameter by its `section.key` name, for example
  `dilation.enable` = `true`. Unknown names are refused at once; a value SoFiA-2
  would misread stops the run with the reason.
- *Suggest again*: asks again after a busy or failed answer.
- *Run SoFiA-2*: starts the run as a backend task, with a progress bar and the
  elapsed time.
- *Cancel run*: stops SoFiA-2 on the backend. Closing the dialog or quitting the
  application during a run asks first, then cancels it.

When SoFiA-2 needs more memory than the node has (about 2.3 times the cube), the
table proposes a sub-cube in `input.region` with a check box. It is sent only
when ticked.

### What the suggestion sets

- Spatial kernels: none, about half a beam and about one beam (at most 1.12
  beams). Sources much larger than 3 beams lose some sensitivity.
- Spectral kernels: from the expected line widths.
- Linker: one beam, and the narrowest expected line.
- Threshold: 4σ with the reliability filter, 5σ without. A small cube gets 5σ
  and no reliability filter.
- Noise scaling when the noise varies over the field or the band; continuum
  subtraction for a continuum offset or a band more than 20 times the widest
  line.
- No mask dilation. For total fluxes, add `dilation.enable` (and its
  iterations) with *Add parameter…*.

The values are a starting point written for SoFiA-2 2.7. The full rule table is
in the [API reference](../../technical/backend-api/tasks-finders.md#the-rules).

### Results

The dialog lists the detections and SoFiA's counts: the negative and positive
detections the reliability filter used, the sources kept, the channel
correlation SoFiA measured, and its warnings. *No sources left after filtering.*
is a result, recorded like any other run. On failure, SoFiA's `ERROR` lines and
the end of its output are shown.

- *Open mask in 3D*: opens the mask cube (one integer label per source) in a new
  cube viewer, in MIP. Fragments where you expected one object mean the kernels
  or linker are too small; a mask over the whole field means the threshold is
  too low.
- Session Data: each run appears under the cube as *SoFiA-2 · 12 sources ·
  14:32:05* (with *N edited* when you changed values). Double-click it to reopen
  the mask. Its Provenance lists every parameter and its origin, the data type
  and the paths of the mask, catalogue and log; *Export recipe as JSON…* writes
  the request that reruns it.
- The detection catalogue can be loaded and overlaid like any catalogue
  ([Catalogues and HiPS](catalogues-hips.md)).

Detections near the cube edge or in noisier regions are the least reliable. On a
small cube SoFiA warns that its reliability filter has too few negative
detections; take that warning seriously.

### Cubes it refuses

A 2-D image or a catalogue; a Stokes Q, U or V cube; a volume without a spectral
axis; fewer than 8 channels; a cube whose sky is not on axes 1 and 2 and spectrum
on axis 3; a cube in a FITS extension (SoFiA-2 reads the primary HDU only). The
dialog shows the reason and offers no run. Compressed cubes are fine: SoFiA-2
reads the decompressed copy.

### SoFiA-2 or the lasso

SoFiA-2 searches the whole field without input, finds faint sources by smoothing
over many voxels and gives a catalogue whose completeness and reliability can be
reasoned about. The lasso starts from a source you point at, works voxel by
voxel, and is adjusted while you watch. Integrated fluxes from the two differ by
a few percent on the same source because they include different voxels (see
[Kinematic Lasso](kinematic-lasso.md)).

## Cross-match with Catalogue

*Tools ▸ Catalogue ▸ Cross-match with Catalogue…* in the image viewer overlays
the catalogue's sources on the image as named markers. It needs a celestial WCS,
and the backend needs `astroquery` and network access.

| Field | Values |
|---|---|
| *Catalogue* | `simbad`, `2MASS`, `NVSS`, `FIRST` (the last three through VizieR) |
| *Search radius* | 1 to 3600 arcsec, default 60 |

The cone is centred on the image, with a radius equal to the larger of *Search
radius* and half the image diagonal, so on most images the whole field is
searched. Sources outside the image or on blank pixels are dropped. At most 500
are kept, sampled evenly over the field in a dense one; the nearest to the
centre is always kept.

Each run is recorded in Session Data as *Cross-match with simbad* (or the chosen
catalogue) with the radius and the number of sources, 0 included.

Before deciding a source has no counterpart, check the astrometry on a source
you are sure of: a systematic offset looks the same as a missing counterpart.

## Problems

| Symptom | Fix |
|---|---|
| *SoFiA-2 is not available on this backend* | Install it on the machine running the backend, then restart the backend. |
| Thousands of detections | Threshold too low, or the noise is measured on real emission. Raise the threshold; check the cube has line-free channels. |
| One galaxy split into several detections | Widen `scfind.kernelsZ` first for HI, then `linker.radiusXY` and `linker.radiusZ`. |
| *Cross-match requires celestial WCS information.* | The image has no sky coordinates. |
| *Cross-match failed* naming astroquery | `astroquery` is missing on the backend, or the backend cannot reach SIMBAD or VizieR. |

Details: [Tasks and finders API](../../technical/backend-api/tasks-finders.md).
