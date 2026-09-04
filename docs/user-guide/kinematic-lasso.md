# Kinematic Lasso

Select a source in a spectral cube by clicking on it. The lasso grows a
**geodesic** region from your click whose traversal cost follows the emission —
bright voxels are cheap to cross, faint ones expensive, NaN is a wall — so the
selection follows the object's real shape instead of a threshold contour or a
box.

The novelty over a flood fill is the **velocity coupling** λ_v: a step along the
spectral axis costs λ_v × the base cost, so you can bias the grow toward
kinematically coherent structure — a rotating disc, an outflow — rather than
whatever happens to be spatially adjacent.

| | |
|---|---|
| **Where** | *Tools → Kinematic Lasso (click to select)* in the cube viewer |
| **Select** | **double-click** a source in the 3-D view |
| **Add** | ⇧ double-click a second source |
| **Carve** | ⌥ double-click a region to remove |
| **Apply** | *Isolate → new cube* or *Remove → new cube* |

```{caution}
The lasso is **assisted segmentation, not detection**. It starts from a source
you can already see and decides where that source ends. It will not find sources
for you — for that use [Source finding](source-finding).
```

---

## Selecting a source

Enable the tool, rotate the volume until the source you want is unobstructed,
then **double-click it**. Selection is bound to the double-click on purpose:
a single drag stays free for rotating, so you can line up the view and pick
without switching modes.

What comes back is the whole connected object, however large — the grow takes
the connected component of the ≥ nσ emission that contains your click, so one
click on a spiral arm grabs the whole galaxy. Isolated noise peaks above nσ are
their own tiny components and are dropped.

A yellow marker shows the seed voxel. The selection appears as a green
isosurface, and its outline is drawn on the 2-D channel map as you step through
channels.

```{tip}
If the double-click lands on empty sky you get *"click on visible emission"* —
the seed voxel must be on the source, not near it. Rotating so the source is
face-on rather than seen through the front of the cube usually fixes a stubborn
pick.
```

### Several sources at once

⇧ double-click adds another source to the same selection; ⌥ double-click carves
a region out of it. Both are evaluated at the current Reach, so a carve that has
not reached a voxel yet will start removing it as you raise the control.

Typical use: select an interacting pair with two ⇧-clicks, or select a galaxy
and carve away the companion that touches it in projection with one ⌥-click.

---

## The two controls

```{list-table}
:header-rows: 1
:widths: 18 20 62

* - Control
  - Range
  - What it does
* - **Reach**
  - 1 – 64
  - The geodesic cost budget. A voxel at the nσ contour costs ≈ 1 to enter and a
    bright one costs almost nothing, so Reach is roughly "how far into the faint
    outskirts may the selection bleed". Raise it to take in more of an extended
    source; lower it to keep to the bright core.
* - **Velocity coupling** λ_v
  - 0.1 – 5.0
  - Cost multiplier for a step along the spectral axis. Below 1 the selection
    grows readily along velocity (good for a rotating disc, whose emission moves
    across channels); above 1 it stays spatially compact (good for separating two
    objects that overlap on the sky at different velocities).
```

Dragging **Reach** updates the surface immediately, with no round trip to the
backend: the geodesic field is computed once when you click and every budget is
a threshold on it. Changing **λ_v** alters the cost function itself, so it
recomputes.

### There is no universal default for Reach

The default of 8 suits compact, well-detected sources. An extended, low-surface-
brightness object may need 30 or more.

This is measured, not an oversight. Across ten sources on two HI cubes the best
value ranged from 4 to 64, and it tracks the source's **surface brightness
relative to the noise** — not the beam, not the pixel scale, not the source's
size. Two rules for deriving it automatically were tested and both performed
worse than leaving the default alone. Deriving it from the header does not work
either: the two cubes have near-identical beams and their best Reach differs
eightfold.

So treat Reach as the control it is — drag it and watch — and **state the value
you used** in any published figure.

```{tip}
Raising Reach can only add voxels, never remove them. If the surface stops
growing well before you expect, you have hit the edge of the connected component
rather than the budget: the object is separated from the rest by sub-nσ voxels,
and lowering nσ (not raising Reach) is what would join them.
```

---

## Refining on the 2-D map

*Refine on 2-D map…* lets you draw a polygon on the current channel and add or
subtract those pixels from the mask, channel by channel. Use it for the cases a
geodesic cannot know about: a foreground star inside the disc, an artefact
ridge, a companion that is genuinely connected in the data but not the object
you mean.

```{warning}
Hand refinements are discarded if you then change **Reach**, because that
recomputes the mask from the geodesic field. The status bar says so when it
happens. Do your 2-D refinement **after** you have settled the controls.
```

---

## Applying the selection

```{list-table}
:header-rows: 1
:widths: 26 74

* - **Isolate → new cube**
  - Blanks everything *outside* the selection. Gives you the object alone, with
    full dimensions and WCS preserved, registered as a new dataset.
* - **Remove → new cube**
  - Blanks everything *inside* it. Gives you the field with the object taken
    out — useful for checking what a source was hiding, or for building a
    residual.
```

Both write the mask exactly as displayed, including any 2-D refinements, so what
you see is what is written. Neither modifies your original file.

---

## What it is good at, and what it is not

```{list-table}
:header-rows: 1
:widths: 34 66

* - Compact, well-detected sources
  - Its best case. Overlap with an independently produced SoFiA-2 mask is
    typically Dice > 0.8 from a single click at default settings, and the result
    is essentially independent of where inside the source you click.
* - Large, low-surface-brightness objects
  - Works, but needs a higher Reach, and the result depends more on where you
    click. Try a few positions and compare.
* - Sources detectable only after smoothing
  - **Out of scope.** A finder like SoFiA detects these by smoothing over many
    channels; individually their voxels sit below the nσ contour, and a
    voxel-level geodesic cannot follow emission that is nowhere above threshold.
    Use the finder for those.
* - Blind detection over a field
  - Not what this is. See [Source finding](source-finding).
```

### Comparing fluxes with another mask

If you integrate the flux inside a lasso selection and compare it against the
same source measured from a SoFiA mask, expect a difference of a few percent —
and expect that difference to be **statistically significant**, not noise. The
two methods include different voxels; the noise floor sits an order of magnitude
below the disagreement.

Quote such a comparison as an agreement between two *segmentations*. It is not
consistency within errors, and reporting it as such would be wrong.

The full validation against SoFiA-2 on two HI cubes — Dice, completeness,
reliability, seed robustness, flux uncertainties, and the scripts to reproduce
all of it — is in `validation/README.md` in the repository.

---

## Troubleshooting

```{list-table}
:header-rows: 1
:widths: 42 58

* - Symptom
  - Cause / fix
* - *"Seed voxel is blank/NaN — click on visible emission."*
  - The double-click ray hit a blanked voxel. Rotate so the source faces you and
    click nearer its centre.
* - The selection takes in a neighbouring galaxy.
  - They are connected above nσ. Carve the neighbour with ⌥ double-click, or
    raise λ_v if they overlap on the sky at different velocities.
* - The selection is a small blob at the peak only.
  - Reach is too low for this source. Drag it up and watch the surface grow.
* - Raising Reach floods into noise.
  - You are past the object's edge. The useful setting is the largest one before
    the surface stops resembling the source.
* - My 2-D edits vanished.
  - You changed Reach afterwards. Refine last.
* - Nothing happens on double-click.
  - The tool is not enabled (*Tools → Kinematic Lasso*), or the cube is still
    loading its preview.
```

## See also

- [Cube viewer](cube-viewer) — rendering modes, full-resolution loading, exports.
- [Source finding](source-finding) — SoFiA-2 detection over a whole field.
- [Moment maps](moment-maps) — for measuring what you have isolated.
- Developer reference: [Kinematic Lasso internals](../kinematic-lasso) — the cost
  function, the geodesic level field, and the out-of-core design.
