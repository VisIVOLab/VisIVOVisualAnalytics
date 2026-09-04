# Source finding and cross-matching

Two complementary jobs. **SoFiA-2** searches a whole cube and tells you what is
in it. **Cross-match** takes what you already have and asks an external
catalogue what it is.

| Task | Tool | Where |
|------|------|-------|
| Find sources in a cube | SoFiA-2 | *Analysis → Source Finding (SoFiA-2)…* |
| Identify a detection | Cross-match | *Tools → Cross-match with Catalogue…* (image viewer) |
| Segment a source you can see | [Kinematic Lasso](kinematic-lasso) | cube viewer |

---

## SoFiA-2 source finding

[SoFiA-2](https://gitlab.com/SoFiA-Admin/SoFiA-2) is the standard HI source
finder. VisIVO runs it on the backend against the open cube and brings the
detections back as a catalogue and a mask you can render.

### Requirements

SoFiA-2 must be installed and on the backend's `PATH`. If it is not, the dialog
says so rather than failing mid-run — check on the machine running the
**backend**, which may not be the machine running the client.

### Running it

```{list-table}
:header-rows: 1
:widths: 26 16 58

* - Control
  - Default
  - What it does
* - **S+C threshold**
  - 5 σ
  - Detection threshold in units of the local noise, after smoothing. Lower it
    to find fainter sources and more spurious ones; raise it for a clean,
    conservative list.
* - **Kernel X / Y (pix)**
  - 3
  - Spatial smoothing kernel. Set it near the size of the sources you want —
    smoothing to the scale of the signal is what makes a faint extended source
    detectable at all.
* - **Kernel Z (chan)**
  - 3
  - Spectral smoothing, in channels. For HI, roughly the number of channels a
    line spans.
* - **Merge sources**
  - on
  - Links neighbouring detections into single objects rather than reporting
    fragments.
```

Press **Run SoFiA-2**. The run happens on the backend; the count appears as
*Detected sources: N*.

```{tip}
Start at the defaults on a sub-cube before running on the whole thing. Kernel
size matters more than threshold for whether a given source is found at all,
and it is quicker to learn that on something small.
```

### Reading the result

**Open mask in 3D** renders the detection mask as an isosurface over the cube,
one region per source. This is the fastest way to see whether the parameters did
what you meant: fragments where you expected one object mean the kernels are too
small, and a mask covering the whole field means the threshold is too low.

The detection catalogue can be overlaid on images and cross-matched like any
other catalogue.

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
[Kinematic Lasso](kinematic-lasso#comparing-fluxes-with-another-mask) before
treating that as an error.

---

## Cross-matching against an external catalogue

In the image viewer, *Tools → Cross-match with Catalogue…* takes the sources you
have on screen and looks for counterparts in an external catalogue within a
search radius.

Requires **celestial WCS** — the tool says so and stops if the image has none,
rather than matching against meaningless coordinates.

### Choosing a radius

The right radius is a physical judgement, not a default:

- Too small and you miss real counterparts whose position differs because they
  were measured at another wavelength, or because your astrometry has an offset.
- Too large and you accumulate chance associations, especially in a crowded
  field or against a deep catalogue.

A useful sanity check is to run the match at two radii. If the number of matches
grows roughly with the area of the search circle, most of the new ones are
chance associations rather than counterparts.

```{tip}
Before concluding that your sources have no counterparts, check
[astrometry](../user-guide/image-viewer) first: a systematic offset shows up as
"no matches" just as convincingly as a genuinely new population does. Match one
source you are certain about and see whether the offset is systematic.
```

### Working with the result

Matches are overlaid on the image and can be labelled. The match table travels
with the catalogue, so you can filter and sort on it like any other column set.

---

## Troubleshooting

```{list-table}
:header-rows: 1
:widths: 42 58

* - Symptom
  - Cause / fix
* - *"SoFiA-2 Not Available"*
  - Not installed, or not on the `PATH` of the machine running the **backend**.
    Restart the backend after installing.
* - The run finds thousands of sources.
  - Threshold too low, or the noise estimate is contaminated by real emission.
    Raise the threshold and check the cube has line-free channels to measure
    noise in.
* - One galaxy comes back as several detections.
  - Kernels too small, or merging off. Raise Kernel Z first for HI — a line
    broken across channels is the usual cause.
* - *"Cross-match requires celestial WCS information."*
  - The image has no sky coordinates (a raw detector frame, or a header with
    only linear axes). Nothing to match against.
* - Every source matches something.
  - Radius too large. Halve it and see how many survive.
```

## See also

- [Kinematic Lasso](kinematic-lasso) — interactive segmentation of one source.
- [Catalogues and HiPS](catalogues-hips) — loading, overlaying and querying catalogues.
- [Moment maps](moment-maps) — measuring the sources once you have them.
