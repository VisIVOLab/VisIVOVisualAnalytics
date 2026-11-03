# VBT viewer (point clouds and volumes)

The **VisIVO Binary Table** is VisIVO's own tabular format: a plain binary block
of columns plus a small `.head` describing them. It is what you use for data
that does not fit the FITS model — an N-body snapshot, a galaxy catalogue with
hundreds of columns, a simulation grid — and VisIVO reads it natively without a
conversion step.

| | |
|---|---|
| **Open** | *File → Open…* or *Data → Open VBT…* — either half of the pair, `.bin` or `.head` |
| **Point table** | one row per object → 3-D point cloud |
| **Volume table** | a regular grid → the [cube viewer](cube-viewer), one cube per field |

---

## The file pair

A VBT is two files that must sit side by side:

```text
mydata.bin.head     the description
mydata.bin          the columns, back to back
```

The header is plain text, one item per line:

```text
float          scalar type: float (32-bit) or double (64-bit)
3              number of fields
1048576        number of rows        (point table)
little         byte order: little or big
X              field names, one per line…
Y
Z
```

A **volume** table declares its geometry on the row-count line instead — row
count, then `nx ny nz`, then the cell spacing — and VisIVO opens it as a cube
rather than as points.

```{tip}
Either half of the pair will do. Pick the `.head` or the `.bin` — the backend
resolves a chosen binary to the header beside it. What it cannot do is open a
binary whose header is missing: the bytes alone carry no description of what
they mean, and you are told so rather than guessed at.
```

---

## The window

The point viewer is laid out like the image and cube viewers (a volume opens in
the cube viewer itself): the command bar across the top (path, a `VBT · …` tag that opens the
header, ⌘K, the backend chip), **Session Data** on the left with the display
controls, the **Inspector** on the right (Properties, Analysis, Parameters,
Copilot) collapsed to a rail until you need it, and the status rail along the
bottom. *View ▸ Session Data Panel* and *View ▸ Inspector Panel* fold either
column away.

---

## Point clouds

### Choosing what to plot

*Available Fields* lists every column in the table. Assign three to the spatial
axes, then pick a **Color field** to carry a fourth dimension — mass,
temperature, velocity dispersion, whatever the data has.

**Solid color** switches the colouring off, which is often clearer than a
colormap when you are looking at structure rather than at a quantity.

```{list-table}
:header-rows: 1
:widths: 24 76

* - **Point size**
  - Larger points read better on a sparse cloud; on a dense one they merge into
    a solid mass and hide the structure you are looking for.
* - **Colormap** / **Autoscale**
  - Autoscale maps the colour range to the data range. Turn it off and set
    *Range min* / *Range max* by hand when comparing two datasets — otherwise
    each is scaled to itself and the colours are not comparable.
* - **Log scale** / **Power γ**
  - For quantities spanning decades (mass, density). γ adjusts the emphasis
    without the hard floor a log takes at zero.
* - **Show bounding box** / **orientation axes** / **color bar**
  - Context for a figure. The bounding box in particular makes the projection
    legible in a still image, where the reader cannot rotate.
```

### Filtering

*Add Filter* restricts the displayed points by a column's value; several filters
combine, and *Active filters* shows what is currently applied. *Clear All*
removes them.

Filtering is how you make a large table legible: plot the whole cloud once to
see its extent, then cut to the population you actually mean — a mass range, a
redshift shell, one simulation subhalo.

```{tip}
*Data Range / Statistics* reports min, max and the distribution for the selected
field. Read it before setting a filter, so you cut at a value the data actually
has rather than a round number.
```

---

## Volume tables

A volume VBT opens in the **cube viewer**, with the same controls, tools and
look as a FITS cube. Nothing is converted: the backend reads each field in place
from the `.bin`, at its offset in the file, so opening a 512³ field costs no
copy and no disk space.

- **One cube per field.** The first field opens; the others are listed in
  *Session Data* as *Field: name*, and opening one opens that field as a cube.
  The window, the Recent entries and exported files name it `volume · field`
  (`volumesnap63 · CDM-Density`).
- **Axes.** The grid is a box, not the sky: its axes are X, Y and Z, in voxels
  times the spacing the header gives, from 0 at the first voxel. *LAYERS ▸ 3D
  axes* labels the box; the 2-D Slice steps along Z.
- **Colours.** A field whose values are all positive and span three decades or
  more (a density) opens on a **Log** scale; the scale and the colour range are
  set in the LUT editor (*Edit* next to the colour map), and the colour bar
  beside the volume shows both, with the threshold. See
  [Threshold, colour range and the colour bar](cube-viewer.md#threshold-colour-range-and-the-colour-bar).
- **Threshold.** It starts at the 99th percentile of the field, so 1 % of the
  voxels are drawn; the share above it is written under the slider.
- **Tools.** Everything that treats the grid as a 3-D field works: slices and
  the 3-D view, isosurfaces, moments (the 0th is the projection along Z),
  channel maps, regions, statistics, the kinematic lasso, export. Profiles
  (*Extract Spectrum*) and *Extract PV Diagram* run along Z. The tools that need
  a spectral axis or sky coordinates — line width, baseline, stacking,
  polarimetry, source finding, the kinematic model, catalogue overlays — are
  not shown ([Tools](cube-viewer.md#tools)).

---

## Working with a remote backend

The VBT viewer reads through the backend like everything else, so the file must
be visible to the **backend's** filesystem — the HPC node, if that is where the
backend runs, not your laptop.

If the session expires (a backend restart, a dropped tunnel) the **point**
viewer recovers by itself on the next page of data: it reopens the dataset and
carries on in the same window, with your filters and colour settings intact.
*Re-open Dataset*, the explicit route, opens a fresh window and does not carry
them over.

A **volume** is a cube like any other on a remote backend: it is read in place
there, a large field can be rendered server-side, and its pyramid and
isosurfaces are built where the file is.

---

## Troubleshooting

```{list-table}
:header-rows: 1
:widths: 42 58

* - Symptom
  - Cause / fix
* - *"VBT header file not found."*
  - The `.head` and its binary are not in the same directory, or one of them is
    missing. Both must be present, and the binary's name is the header's name
    with `.head` removed. (Which of the two you pick does not matter — the
    backend resolves a chosen binary to its header.)
* - *"Unknown VBT endian"* or *"VBT header too short"*
  - The header is missing a line. The order is fixed: scalar type, field count,
    row count (plus geometry for a volume), byte order, then one name per field.
* - The cloud is a featureless blob.
  - Point size too large for the density, or no filter applied. Try solid colour
    and a smaller point size first, then add the colour field back.
* - Colours differ between two datasets you meant to compare.
  - Autoscale is on, so each is normalised to itself. Turn it off and set the
    same range on both.
* - A volume looks almost empty.
  - The threshold is high for the scale in use. On a Linear scale a density
    field shows only its peaks; choose Log in the LUT editor (*Edit*) and read
    the share of voxels under the threshold slider while lowering it.
* - Numbers look scrambled.
  - Byte order. A big-endian file read as little-endian produces finite,
    plausible-looking nonsense rather than an error — check the header's fourth
    line against how the file was written.
```

## See also

- [Getting started](getting-started) — opening your first dataset.
- [Catalogues and HiPS](catalogues-hips) — for CSV/VOTable catalogues rather than VBT.
- [Velocity fields](velocity-fields) — for vector-valued grids.
