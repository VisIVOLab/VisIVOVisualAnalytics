# Source finding and cross-matching

Two complementary jobs. **SoFiA-2** searches a whole cube and tells you what is
in it. **Cross-match** asks an external catalogue what it already lists on an
image.

| Task | Tool | Where |
|------|------|-------|
| Find sources in a cube | SoFiA-2 | *Tools → Sources & Kinematics → Source Finding (SoFiA-2)…* (cube viewer) |
| See which catalogued sources lie on an image | Cross-match | *Tools → Cross-match with Catalogue…* (image viewer) |
| Segment a source you can see | [Kinematic Lasso](kinematic-lasso) | cube viewer |

---

## SoFiA-2 source finding

[SoFiA-2](https://gitlab.com/SoFiA-Admin/SoFiA-2) is the standard HI source
finder. VisIVO runs it on the backend against the open cube and brings the
detections back as a catalogue and a mask you can render.

### Requirements

SoFiA-2 (version 2.7) must be installed where the backend runs, on its `PATH`
as `sofia2` or `sofia`, or named by `VISIVO_SOFIA_BIN`. If it is not, the run
says so ("SoFiA-2 is not available on this backend") — check on the machine
running the **backend**, which may not be the machine running the client. The
suggestion below does not need SoFiA-2.

### Running it

Open the dialog from the cube viewer: *Tools → Sources & Kinematics → Source
Finding (SoFiA-2)…*. On opening it asks the backend to
[suggest parameters](#suggested-parameters) from the cube, which takes about a
second, and shows them:

```{list-table}
:header-rows: 1
:widths: 24 76

* - Part of the dialog
  - What it does
* - **Data type**, **Line widths**
  - What the suggester deduced: emission, absorption (searched in the
    inverted cube) or continuum not subtracted; HI (lines of 15-120 km/s) or
    Galactic lines (2-20 km/s). The line under each says why, and how sure it
    is. When the evidence is unclear it reads *Please confirm…*, a
    **Confirm** button appears and **Run** waits for your answer: pick a value
    or confirm the one shown, and the parameters are suggested again for it.
* - Measurements
  - The noise σ, the beam in pixels, the channel width in km/s, the
    adjacent-channel correlation ρ, the resolution elements N_res, how much the
    noise varies over the band and across the field, and the cube's shape;
    then any warning (few resolution elements for the reliability filter,
    exact-zero blanks, no beam or no channel width in the header, a cube too
    large for the node's memory).
* - Parameter table
  - One row per SoFiA-2 parameter: its **Value** (double-click to edit), its
    **Status**, SoFiA's own default, and **Why** it has that value. Values that
    differ from SoFiA's default are bold; your edits are in italics and marked
    *edited (suggested …)*. A blank value is not sent, so SoFiA's default runs.
* - **Reset to suggested**
  - Puts every value you edited back to the suggestion, and drops the rows you
    added.
* - **Add parameter…**
  - Adds a SoFiA-2 parameter the suggestion leaves out, by its `section.key`
    name and value, e.g. `dilation.enable` = `true` for masks that take in the
    faint outskirts. It is kept like an edit, with SoFiA's default shown
    next to it. A name the backend does not accept is refused when you add
    it; the value is checked when the run starts, and a value SoFiA-2 would
    misread (a word it does not know, `abc` for a number) stops the run with
    the reason before SoFiA starts.
* - **Suggest again**
  - Asks again, after a busy or failed answer.
```

Your edits win: when a new suggestion arrives (another data type or line
width), the values you typed stay and everything else follows the suggestion.

When the cube is larger than SoFiA-2 can hold in the node's memory (about
2.3× the cube's size), the table proposes a sub-cube, `input.region`, with a
check box. It is sent only once you tick it (or type your own region); without
it SoFiA-2 searches the whole cube.

Press **Run SoFiA-2**. The run is a task of the backend: a bar and the elapsed
time show it is running, and **Cancel run** (or closing the dialog, after a
question) asks the backend to stop SoFiA-2; the dialog says whether the
backend accepted. Quitting the application during a run asks once, then
cancels the run on the backend before the app exits. When it finishes, the dialog lists the
detected sources and SoFiA's own counts: the negative and positive detections
the reliability filter worked with, the sources left after filtering, the
channel correlation SoFiA measured, and SoFiA's warnings and errors. A run
that leaves nothing after filtering reads *No sources left after filtering.*,
with the same counts and no mask to open; it is recorded like any other run
(an empty result is what an inversion test expects). When SoFiA-2 fails, its
`ERROR` lines and the end of its output are shown in full, in a box that
scrolls.

Each run is recorded under the cube in *Session Data*, as *SoFiA-2 · 12
sources · 14:32:05* (with "N edited" when you changed values), so
two runs stay apart: Inspector ›
Provenance lists every parameter that ran and whether it was suggested, edited
by you or (for a sub-cube) confirmed by you, the data type, the
paths of the mask, catalogue and log, and **Export recipe as JSON…** writes
the request that reruns it (`POST /v1/tasks/sourcefind`). Double-clicking the
row opens the mask cube again. A SoFiA-2 mask opens in MIP: it holds a few
small integer labels on zeros, which the default Composite rendering leaves
nearly invisible; switch the blend mode in *Volume Rendering* if you prefer.

When the backend refuses the cube, the dialog shows its reason as given and
offers no run: a 2-D image or a catalogue, a Stokes Q, U or V cube, a volume
without a spectral axis, fewer than 8 channels, a cube whose sky is not on
axes 1-2 and spectrum on axis 3, or a cube in a FITS extension (SoFiA-2 reads
the primary HDU only). A `.fits.gz` or tile-compressed cube is decompressed
when VisIVO opens it, and SoFiA-2 reads that plain copy. A busy backend (every compute slot of the session in use) is said as
such; press **Suggest again** later. A backend older than the suggester runs
SoFiA-2 with its defaults.

```{tip}
Start on a sub-cube before running on the whole thing. Kernel size matters
more than threshold for whether a given source is found at all, and it is
quicker to learn that on something small.
```

### Suggested parameters

The backend can propose SoFiA-2 parameters from the cube itself, each with
the reason it has that value. It reads the cube once (a sample of channel
pairs spread over the band, so it takes about a second even on cubes larger
than memory) and measures:

- the noise σ, as SoFiA measures it: from the values on the side of zero
  that holds no signal (below zero for emission, above it for absorption).
  Faint emission spread over most of the field pulls this σ low, for SoFiA
  as for the suggestion;
- the beam in pixels, and the channel width in km/s in the rest frame;
- how strongly adjacent channels are correlated (Hanning smoothing makes
  them so);
- how much the noise varies across the field and across the band, compared
  with what the measurement's own scatter gives on a cube of that size;
- the blank (NaN) and exact-zero fractions, and whether the spectra sit off
  zero (continuum not subtracted);
- the number of independent resolution elements, which decides whether
  SoFiA's reliability filter has enough negative detections to work with.

From these it sets the spatial kernels to none, about half a beam and about
one beam (1.12 beams at most, so the smoothed resolution stays near 1.5 beams
and neighbouring sources are not merged; sources much larger than 3 beams
lose some sensitivity), the spectral kernels to HI line widths of 15-120 km/s
(or 2-20 km/s for Galactic lines, not yet validated), the linker sizes to one
beam and the narrowest expected line, and the threshold to 4σ with the
reliability filter or 5σ without it (a small cube gets 5σ and no reliability
filter). Noise that varies across the field or the band turns on noise
scaling; a continuum offset, or a band more than 20 times wider than the
widest expected line, turns on continuum subtraction; absorption is searched
in the inverted cube. The full rule table
is in the [API reference](../backend-api/tasks-finders.md#the-rules).

Two things are deduced and may need you: whether the cube holds emission,
absorption or unsubtracted continuum, and which line widths to expect (a cube
without a rest frequency does not say). When the evidence is unclear the
suggestion says so and you choose. The suggestion leaves the masks at the
clip, with no mask dilation: for total fluxes, which need the faint outskirts,
set `dilation.enable` (and its iterations) yourself with **Add parameter…**.

A search that finds nothing is a result, not an error: SoFiA-2 ends with "No
sources left after filtering", and the run is reported as completed with no
sources, its log kept. When SoFiA-2 does fail, the message shows SoFiA's own
`ERROR` lines and the end of its output.

The suggestions are a starting point, not an optimum: they are written for
SoFiA-2 2.7 and are being validated against reference masks on HCG 44 and
WALLABY. After a run, SoFiA's own counts come back with the result
("Reliability ran on 33 negative and 53 positive detections"), so you can
see whether the reliability filter had enough to work with.

### Reading the result

**Open mask in 3D** opens the detection mask cube in a new cube viewer window
(in MIP), one integer label per source. This is the fastest way to see whether the parameters did
what you meant: fragments where you expected one object mean the kernels are too
small, and a mask covering the whole field means the threshold is too low.

The detection catalogue (its path is in Inspector › Provenance) can be loaded
and overlaid like any other catalogue.

```{caution}
Detections near the edge of a cube, or in a region of elevated noise, are the
least reliable. SoFiA's own reliability filter needs enough negative detections
to model the noise — on a small cube it will warn that it cannot do this well,
and that warning is worth heeding.
```

### Finder or lasso?

They answer different questions and the results are not interchangeable.

```{list-table}
:header-rows: 1
:widths: 50 50

* - **SoFiA-2**
  - **Kinematic Lasso**
* - Searches the whole field with no input from you.
  - Starts from a source you point at.
* - Finds faint sources by smoothing over many voxels.
  - Works at the voxel level; cannot follow emission that is nowhere above
    threshold.
* - Gives a catalogue with completeness and reliability you can reason about.
  - Gives one object's extent, interactively adjustable.
* - Parameters set before the run.
  - Parameters adjusted while you watch the result.
```

Integrated fluxes from the two will differ by a few percent on the same source,
because they include different voxels — see
[Kinematic Lasso](kinematic-lasso.md#comparing-fluxes-with-another-mask) before
treating that as an error.

---

## Cross-matching against an external catalogue

In the image viewer, *Tools → Cross-match with Catalogue…* asks an external
catalogue which of its sources fall on the image, and overlays them as markers
labelled with their names. The dialog has two fields: **Catalogue** (`simbad`,
`2MASS`, `NVSS` or `FIRST`, the last three queried through VizieR) and
**Search radius** (1-3600 arcsec, default 60). The query runs on the backend,
which needs `astroquery` and network access.

Requires **celestial WCS** — the tool says so and stops if the image has none,
rather than matching against meaningless coordinates.

### What the radius does

The backend queries a cone around the image centre whose radius is the larger
of the **Search radius** and half the image diagonal, so on most images the
whole field is searched whatever the radius. The sources returned are then
cropped to the image: those outside the pixel grid, or on blank (NaN) pixels,
are dropped. At most 500 are kept; in a denser field they are sub-sampled
evenly over the field rather than taken nearest the centre, and the nearest
one is always kept.

```{tip}
Before concluding that a source has no counterpart, check
[astrometry](../user-guide/image-viewer) first: a systematic offset between
your image and the catalogue markers shows up as "no counterpart" just as
convincingly as a genuinely new population does. Look at one source you are
certain about and see whether the offset is systematic.
```

### Working with the result

The catalogue sources are overlaid on the image as markers with their names.
Each run is recorded in *Session Data* as *Cross-match with simbad* (or the catalogue you chose), with
the catalogue, the radius and the number of sources; a run that finds nothing
in the footprint is recorded too, with 0.

---

## Troubleshooting

```{list-table}
:header-rows: 1
:widths: 42 58

* - Symptom
  - Cause / fix
* - *"SoFiA-2 is not available on this backend"*
  - Not installed, or not on the `PATH` of the machine running the **backend**.
    Restart the backend after installing.
* - The run finds thousands of sources.
  - Threshold too low, or the noise estimate is contaminated by real emission.
    Raise the threshold and check the cube has line-free channels to measure
    noise in.
* - One galaxy comes back as several detections.
  - Kernels too small, or linker radii too small. Widen `scfind.kernelsZ`
    first for HI (a line broken across channels is the usual cause), then
    `linker.radiusXY` / `linker.radiusZ`.
* - *"Cross-match requires celestial WCS information."*
  - The image has no sky coordinates (a raw detector frame, or a header with
    only linear axes). Nothing to match against.
* - *"Cross-match failed"* naming astroquery.
  - `astroquery` is not installed where the backend runs, or the backend has
    no network access to SIMBAD / VizieR.
```

## See also

- [Kinematic Lasso](kinematic-lasso) — interactive segmentation of one source.
- [Catalogues and HiPS](catalogues-hips) — loading, overlaying and querying catalogues.
- [Moment maps](moment-maps) — measuring the sources once you have them.
