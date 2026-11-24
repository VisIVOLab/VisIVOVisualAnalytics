# VBT tables (point clouds and volumes)

The VisIVO Binary Table (VBT) is VisIVO's own tabular format: a binary block of
columns plus a small text `.head` that describes them. Use it for an N-body
snapshot, a catalogue with many columns or a simulation grid. Open it with
*File ▸ Open…* or *Data ▸ Open VBT…*, choosing either half of the pair. A point
table opens in the 3-D catalogue viewer, a volume table in the cube viewer.

For a walk-through with a simulation grid, see
[Large cubes and simulations](../use-cases/05-large-cubes-and-simulations.md).

## The file pair

The two files must sit side by side, and the binary's name is the header's
name without `.head` (`mydata.bin` and `mydata.bin.head`). A binary without its
header is refused. The header is plain text, one item per line, in this fixed
order:

```text
float          scalar type: float (32-bit) or double (64-bit)
3              number of fields
1048576        number of rows (point table)
little         byte order: little or big
X              field names, one per line
Y
Z
```

A volume table puts its geometry on the row-count line: the row count, then
`nx ny nz`, then the cell spacing.

The file must be visible to the backend's filesystem: on a cluster, that is
the cluster, not your laptop.

## Point tables

A point table opens in the 3-D catalogue viewer, titled *3D Points*, with the
tag *VBT · N rows*. The backend reads the columns in place. Above 100 000 rows
the window loads an even sample of the table:

- *Load more…*: loads further rows between those already loaded.
- *Load all*: loads every row.
- *AXIS MAPPING*: the X, Y, Z columns, else the first three; any three columns
  can be chosen.
- *VISUAL ENCODING ▸ Colour by*: the column that colours the points, by default the
  first one that is not a position. *Single colour* draws every point alike.
  *Range and scale…* sets Min, Max and the scale; a positive column
  spanning three decades or more opens on a Log scale.
- *SOURCE REPRESENTATION ▸ Shape*: *Point*, or *Gaussian splats* (the default
  above a million rows). Splats are emissive, so dense regions add up to bright
  and faint filaments stay visible. Splat brightness does not depend on the number
  of particles; for numbers use a projected map of
  [ray-traced particles](raytraced-particles.md). *Scale* sets the point or splat size. The
  ellipsoid, sphere and cross glyphs of object catalogues are not offered.
- *Ray-traced light*: draws every row as a continuous medium on this machine's
  GPU, for figures and movies. See [Ray-traced particles](raytraced-particles.md).
- *View ▸ Save View As…*: a PNG (any size, optionally with a transparent background),
  PDF or SVG of the view. See [Figures, movies and linked views](publication-output.md#save-view-as).
- *FILTERS*: restrict the rows by value or by a named 3-D region; several
  filters combine. *Tools ▸ Selection Diagnostics…* and *Tools ▸ Compare
  Quantity…* work as for [Gadget snapshots](gadget-snapshots.md).
- Hover or click a point to see its row. *Esc*, a click on empty space or
  *View ▸ Clear Selection* deselects it. The *Catalogue* dock lists the loaded
  rows.

There is no sky frame, cosmology or redshift shells for a point table. For the
other controls, see [Catalogues and HiPS](catalogues-hips.md). A table larger
than the window loads can be drawn by a remote backend (see the same page).

## Volume tables

A volume table opens in the cube viewer with the same controls and tools as a
FITS cube. Each field is read in place from the `.bin`, without a copy.

- One cube per field: the first field opens; the others are listed in
  *Session Data* as *Field: name*, and opening one opens that field. Windows
  and exported files are named `volume · field`.
- Several fields in one view: *Add Field…* in the Inspector (*Fields*) puts
  another field of the volume in the same window: in the slice (layered,
  additive, split or mirrored about an axis) and in the 3-D box (one ray cast,
  each field with its own colours and opacity, one colour bar per field). See
  [Several variables in one view](pluto-output.md#several-variables-in-one-view).
- Axes: X, Y and Z, in voxels times the header's spacing, starting at 0.
  *LAYERS ▸ 3D axes* labels the box. The 2-D slice is a plane of constant X,
  Y or Z, picked in the Inspector (*Slice ▸ Along*), stepped with the slider or
  put at a typed coordinate (*At*); the bar reads `Z 12 ── z = 0.6`, not
  `CH 12`. Tools that read cube pixels work on Z slices, as for a
  [PLUTO output](pluto-output.md#slices-along-x-y-or-z).
- Units: *Units…* in the Inspector takes a factor and a unit for a field's
  values and for the coordinates, kept with the file, as for
  [PLUTO](pluto-output.md#physical-units).
- Colours: a positive field spanning three decades or more opens on a Log
  scale. Set the scale and range with *Edit* next to the colour map.
- Threshold: starts at the field's 99th percentile, so 1 % of the voxels are
  drawn; the share above it is shown under the slider.
- Tools: everything that treats the grid as a 3-D field works (slices,
  isosurfaces, moments, channel maps, regions, statistics, the kinematic
  lasso, export). The 0th moment is the projection along Z; *Extract Spectrum*
  and *Extract PV Diagram* run along Z. Tools that need a spectral axis or sky
  coordinates are hidden.

*View ▸ Save View As…* in the volume window, or *Tools ▸ Export ▸ Save View As…* in the cube
viewer, saves the volume as a PNG, PDF or SVG; see
[Figures, movies and linked views](publication-output.md#save-view-as).

On a remote backend a volume behaves like any cube: it is read in place there
and a large field can be rendered on the backend.

## Wrong byte order

Some converters write the values in one byte order and the `.head` in the
other. On opening, the backend samples each field both ways. If the values are
absurd as declared and plausible the other way, a dialog says so (for
example *This table's header says little-endian, but its values look
big-endian.*) and shows the share of absurd values each way.

- *Open as big-endian* (or *little-endian*): reads the table in the other
  order. The file is not changed.
- *Open as declared*: keeps the header's order.
- *Escape*: opens the table in the order it is read now and remembers nothing.

The backend keeps your answer for that file, for every window and user, until
the `.head` changes. Opening the file again yourself asks again. Large physical
values (cgs masses, densities) do not trigger the question. To fix the file,
correct the fourth line of its `.head`.

## Messages

| Message or symptom | Cause |
|---|---|
| *VBT header file not found.* | The `.head` and the binary are not in the same directory, or one is missing. |
| *Unknown VBT endian* or *VBT header too short* | A header line is missing; check the order above. |
| Numbers look plausible but wrong | The byte order on the header's fourth line does not match the data. |
| A volume looks almost empty | The threshold is high on a Linear scale; choose Log in *Edit*. |

Details: [VBT tables: methods and implementation](../../technical/methods/vbt-viewer.md).
