# VBT tables (point clouds and volumes)

The **VisIVO Binary Table** is VisIVO's own tabular format: a plain binary block
of columns plus a small `.head` describing them. It is what you use for data
that does not fit the FITS model — an N-body snapshot, a galaxy catalogue with
hundreds of columns, a simulation grid — and VisIVO reads it natively without a
conversion step.

| | |
|---|---|
| **Open** | *File → Open…* or *Data → Open VBT…* — either half of the pair, `.bin` or `.head` |
| **Point table** | one row per object → the [3-D catalogue viewer](catalogues-hips.md#3-d-catalogue-viewer) |
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

## Point clouds

A table of points opens in the **3-D catalogue viewer**, the window every point
catalogue opens in, so a simulation snapshot and a galaxy catalogue are explored
with the same controls. Nothing is converted: the backend reads the columns
where they lie in the binary. When the table has more rows than a page (100 000),
the window loads an even sample of them rather than its first rows, which in a
snapshot are a few blocks of space; *Load more…* fills in the rows between those
loaded, *Load all* loads every row.

The window is titled *3D Points*, the tag says *VBT · N rows*, and only what
applies to a table of points is shown: the table lists the file's own columns,
and there is no sky frame, cosmology or redshift shells.

- **Positions** are the table's X/Y/Z columns, else its first three (any three
  columns can be chosen under *Axis mapping*).
- **Colour** opens on the first column that is not a position; *Single colour*
  draws every point alike. *Edit*, beside the colour map, sets Min, Max and the
  scale as for a cube; a quantity that is positive and spans three decades or
  more (a density) opens in Log. A colour bar shows the column's colours.
- **Shape** opens as *Point*, or as *Gaussian splats* above a million rows:
  emissive, so dense regions add up to bright and sparse filaments stay faint
  but visible. The *Scale* slider sets the point or splat size. The
  **Ray-traced light** check under it draws every row as a continuous medium
  on this machine's GPU, for figures and movies: see
  [Ray-traced particles](raytraced-particles). (The glyph
  shapes of object catalogues, ellipsoids, spheres and crosses, are not offered
  for a point table.)
- **Filters** restrict the rows by value; several combine.
- Hovering and clicking a point shows its row, marked by a small ring; Esc, a
  click on empty space or *View ▸ Clear Selection* deselects it. The
  *Catalogue* dock lists the rows loaded.

See [3-D catalogue viewer](catalogues-hips.md#3-d-catalogue-viewer) for the
details, and [Drawn on the backend](catalogues-hips.md#drawn-on-the-backend)
for a table larger than what the window loads, on a cluster.

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

A VBT is read through the backend like everything else, so the file must be
visible to the **backend's** filesystem — the HPC node, if that is where the
backend runs, not your laptop.

A **point table** larger than what the window loads is drawn on the backend
when the backend is remote and renders server-side: see
[Drawn on the backend](catalogues-hips.md#drawn-on-the-backend).

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
  - Point size too large for the density, or no filter applied. Try *Single
    colour* and a smaller *Scale* first, then add the colour column back.
* - Colours differ between two datasets you meant to compare.
  - Each window takes its colour range from its own data. Set the same Min and
    Max (and scale) in *Edit*, beside the colour map, in both.
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
- [Ray-traced particles](raytraced-particles) — stills, movies and the GPU view of a point table.
