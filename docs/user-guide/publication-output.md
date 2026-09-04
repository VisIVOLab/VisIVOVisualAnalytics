# Figures, movies and linked views

Getting results out of VisIVO in a form you can put in a paper or a talk, and
keeping several views in step while you compare them.

| Task | Where |
|------|-------|
| A single figure with a colour bar and sky grid | *Analysis → Publication Figure…* |
| An animation | *Tools → Export Movie…* (cube viewer) |
| Labels and arrows on an image | *Tools → Add Text / Arrow Annotation…* (image viewer) |
| Two views showing the same thing | *Tools → Link Views* |
| Regions shared with CASA or DS9 | *Tools → Import / Export Region (CRTF/DS9)…* |

---

## Publication figures

*Publication Figure…* renders the current dataset with the elements a figure
needs rather than the ones an interactive session needs:

- a **colormap** from the perceptually uniform set — `viridis`, `inferno`,
  `magma` — with a linear or non-linear stretch;
- a **colorbar** labelled with the data's own `BUNIT`, so the reader knows the
  units without being told in the caption;
- a **WCS sky grid**, so positions are readable off the figure itself.

```{tip}
Prefer a perceptually uniform colormap for anything a reader will judge
quantitatively. Rainbow-type maps introduce visual boundaries where the data has
none, and readers reliably see structure in them that is not there.
```

### What to record in the caption

A figure is not reproducible from its own pixels. Whatever you export, note:

- the **channel range** for a moment map, and whether a mask was applied;
- the **stretch** and the colour range, if you set it by hand;
- for a [Kinematic Lasso](kinematic-lasso) selection, the **Reach** and **λ_v**
  you used — there is no universal default, so the numbers matter;
- the **beam**, if the figure shows one.

---

## Movies

*Export Movie…* in the cube viewer offers two modes:

```{list-table}
:header-rows: 1
:widths: 30 70

* - **Channel scan**
  - Steps through the velocity channels, recording each 2-D channel map. Any
    kinematic-model contour you have enabled is captured too, so this is how you
    make a model-versus-data animation. Defaults to the full channel range;
    narrow it to the channels the line actually occupies.
* - **Camera orbit**
  - A full 360° turn of the 3-D camera over *N* frames. Your interactive camera
    is snapshotted and restored afterwards, whether the export finishes or you
    cancel it — the view you were working in is not disturbed.
```

Both take a frames-per-second setting. For a channel scan, match it to the
number of channels: a 300-channel cube at 30 fps is ten seconds, which is about
right for a talk; the same cube at 5 fps is a minute, which is not.

---

## Annotations

In the image viewer, *Add Text Annotation…* and *Add Arrow Annotation…* place
labels and pointers directly on the image, and *Save / Load Annotations* stores
them alongside so a figure can be rebuilt later or handed to a co-author.

Annotations live in image coordinates, so they stay attached to the feature they
point at when you zoom or change the stretch.

---

## Linked views

*Link Views (sync camera / channel / colour)* keeps several cube viewers showing
the same thing — the same region, the same velocity, the same colours — which is
what makes a genuine comparison possible between two cubes of the same field.

### What is synchronised

```{list-table}
:header-rows: 1
:widths: 26 74

* - **Camera**
  - Shared **relative to each cube's data bounds**, so two cubes of different
    sizes covering the same sky still frame the same region rather than the same
    pixel coordinates.
* - **Channel**
  - Matched by **spectral value**, not channel index. Two cubes with different
    channel widths will show the same velocity, which is the comparison you
    actually want.
* - **Colour**
  - Colormap name plus the stretch. Adopted on the next change rather than
    immediately, so linking does not silently overwrite colours a peer already
    chose.
* - **Representation and blend**
  - Volume ↔ isosurface, and Composite / MIP / MinIP.
```

Enabling Link Views pushes the camera and channel to the already-linked viewers
straight away, so they snap into alignment. Individual facets can be toggled
with the chips next to the master button — link the channel but not the camera,
for instance, when the two cubes cover different areas.

```{caution}
Channel matching by velocity requires both cubes to declare a spectral axis you
can trust. If one is in frequency and the other in optical velocity, check a
known line lands at the same place in both before believing a comparison — see
[the runbook's WCS checks](kinematic-lasso) for how to verify that.
```

---

## Regions in and out

*Import / Export Region (CRTF/DS9)* reads and writes the two formats CASA and
DS9 use, so a region defined in either tool can be measured here and vice versa.

Round-trip a region you care about before relying on it: export it, reopen it in
the tool that wrote the original, and check the shape and position survived.
Polygons and annuli are the ones most worth checking.

---

## Exports as data, not pictures

For anything you intend to measure again rather than look at, export FITS rather
than an image:

- *Export Sub-Cube as FITS…* — the region you selected, with WCS.
- *Export Current Channel as 2-D FITS…* — one plane.
- *Export Moment Map as FITS…* — the map with its own units.

All land in the Workspace Exports folder and keep the WCS, so they reopen
correctly here and anywhere else.

## See also

- [Cube viewer](cube-viewer) — the rendering modes a figure captures.
- [Kinematic Lasso](kinematic-lasso) — record Reach and λ_v in your caption.
- Developer reference: [Linked views](../linked-views) and
  [Movie export](../movie-export) — what is hooked, and the current limits.
