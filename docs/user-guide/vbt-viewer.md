# VBT viewer (point clouds and volumes)

The **VisIVO Binary Table** is VisIVO's own tabular format: a plain binary block
of columns plus a small `.head` describing them. It is what you use for data
that does not fit the FITS model — an N-body snapshot, a galaxy catalogue with
hundreds of columns, a simulation grid — and VisIVO reads it natively without a
conversion step.

| | |
|---|---|
| **Open** | *File → Open VBT…* (select the `.head` file, not the binary) |
| **Point table** | one row per object → 3-D point cloud |
| **Volume table** | a regular grid → volume rendering |

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
count, then `nx ny nz`, then the cell spacing — and VisIVO opens it in the
volume renderer rather than as points.

```{caution}
Open the **`.head`**, not the binary. The binary alone carries no description of
what its bytes mean, and the viewer will tell you the header was not found
rather than guess.
```

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

A volume VBT renders through the same GPU path as a FITS cube.

```{list-table}
:header-rows: 1
:widths: 24 76

* - **Ray intensity**
  - Overall opacity of the accumulated ray. Raise it to bring out faint
    structure, lower it when the volume is a solid block of colour.
* - **Use MIP blending**
  - Maximum-intensity projection: each ray shows its brightest sample instead of
    accumulating. Good for finding peaks; misleading about how much material
    there is, because it discards everything but the maximum.
* - **Scalar**
  - Which field the volume renders, when the table carries several.
```

---

## Working with a remote backend

The VBT viewer reads through the backend like everything else, so the file must
be visible to the **backend's** filesystem — the HPC node, if that is where the
backend runs, not your laptop.

If the session expires (a backend restart, a dropped tunnel), the viewer says
*Session expired — reconnecting…* and *Re-open Dataset* restores the view. Your
filters and colour settings are part of the view, not the dataset, so they
survive.

---

## Troubleshooting

```{list-table}
:header-rows: 1
:widths: 42 58

* - Symptom
  - Cause / fix
* - *"VBT header file not found."*
  - You opened the binary, or the `.head` and its binary are not in the same
    directory. Both must be present, and the binary's name is the header's name
    with `.head` removed.
* - *"Unknown VBT endian"* or *"VBT header too short"*
  - The header is missing a line. The order is fixed: scalar type, field count,
    row count (plus geometry for a volume), byte order, then one name per field.
* - The cloud is a featureless blob.
  - Point size too large for the density, or no filter applied. Try solid colour
    and a smaller point size first, then add the colour field back.
* - Colours differ between two datasets you meant to compare.
  - Autoscale is on, so each is normalised to itself. Turn it off and set the
    same range on both.
* - Numbers look scrambled.
  - Byte order. A big-endian file read as little-endian produces finite,
    plausible-looking nonsense rather than an error — check the header's fourth
    line against how the file was written.
```

## See also

- [Getting started](getting-started) — opening your first dataset.
- [Catalogues and HiPS](catalogues-hips) — for CSV/VOTable catalogues rather than VBT.
- [Velocity fields](velocity-fields) — for vector-valued grids.
