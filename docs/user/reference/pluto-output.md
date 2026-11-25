# PLUTO simulation output

A [PLUTO](https://plutocode.ph.unito.it/) run's binary output opens in the cube
viewer (a 2-D run in the image viewer), one variable at a time. The backend
reads the variable in place in the file, so even a 2048³ density (32 GB) opens
without a copy. Open it with *File ▸ Open…*; set *File type* to *All files*,
since PLUTO files are not in the default list.

For a walk-through, see
[Large cubes and simulations](../use-cases/05-large-cubes-and-simulations.md).

## What to open

PLUTO writes each output as raw samples, with two text files in the run
directory:

| File | Contents |
|---|---|
| `dbl.out` or `flt.out` | One line per output: number, time, step, layout (`single_file`, as `data.0042.dbl`, or `multiple_files`, as `rho.0042.flt`), byte order and variable names. |
| `grid.out` | The geometry and the cell edges along x1, x2 and x3. |

Open either:

- the `.out` file: the newest output it lists opens, on its first variable
  whose file is present;
- a data file (`data.0042.dbl`, `rho.0042.flt`): that output, on that variable
  (the first one for a `data.*` file).

Both text files must be next to the data; without `grid.out` the file browser
says so. A data file whose size does not match the grid and the `.out` line is
refused, with both sizes.

## Variables

The other variables of the output are listed in *Session Data* as
*Field: name*; opening one opens that variable. With `multiple_files`, only the
variables whose files are present are listed. Face-centred magnetic fields of
a constrained-transport run (`Bx1s` …) are not listed.

## Several variables in one view

To see two or three variables of the run together (a passive tracer over the
density, ρ beside p), open one and use *Add Field…* in the Inspector, *Fields*.
It lists the other variables of the output; pick one and it joins this window.
At most three fields are in one view, this one included. Each field has its own
block: *Shown*, *Opacity*, a colour map (it starts as a black-to-colour map so
that fields add up as dyes), *Log* (a positive field over three decades starts
on it), *Min* and *Max* of the colour map, and a *Threshold*. *Remove* takes
the field out. Every field must be on the same grid.

**In the slice** the fields become one picture. *Slice* picks how:

- *Layered*: each field drawn over the one below, weighted by its opacity. Where an upper field is at its lower limit (or below its threshold, when you typed one) it lets the field below show.
- *Additive*: the colours added and clipped at white, as the image viewer's
  Additive blend does.
- *Split*: the first field left of a seam, the second right of it. The seam is
  the middle, or the coordinate typed in the box beside it (in the units shown,
  along the slice's horizontal axis).
- *Mirrored*: for a run that holds one side of an axis. The first field is
  reflected to the left of the axis, the second drawn as it is to the right.
  The axis is x = 0 when the grid holds it, else the grid's edge, or the
  coordinate typed.

Split and Mirrored take the first two fields shown. Each field has its own
colour bar, side by side at the right edge. The probe, the regions, the
contours and the spectrum tools keep reading this window's own field. The
planes of the other fields are read at the same level as this one's, a plane
each, so the memory is a few planes.

**In the 3-D box** the fields are drawn in one ray cast, so they interleave
along every ray: a dense tracer behind lighter gas is seen behind it. Each
field has its own colour and opacity functions; emission and absorption are
summed along the ray, as in the headset's *Overlay* in *Sum* mode (there is no
*MIP* for several fields on the desktop, and the clipping box of the headset does not cut the cast: in *MIP* or *MinIP*, or while the headset's clip crops the box, the box shows this window's field alone, with a message, and the fields come back in *Composite* without the clip). Each field is read once, on the grid
the box is drawn on (the preview, or the full grid when that is loaded), so a
field costs one cube of that size; the box follows the grid if it changes. The
colour map, *Min*/*Max*, *Threshold* and *Opacity* of this window's own field
are the window's usual controls. Each field has its own colour bar beside the
window's. The headset keeps drawing this window's field alone; its *Overlay*
does the same job there.

Three limits. The extra fields are drawn in *Composite* only, as above. The 3-D box needs the local renderer: in *Remote* the backend draws
the box and knows one field, so added fields show in the slice only, with a
message. And a refined box (Refine Selected Box) and several fields do not go
together: clear the refined box first.

Fields added follow the units of this window. When you switch between code and
physical units, their planes and voxels are read again in the new ones, and a
*Min*, *Max* or *Threshold* you typed (and the seam) keeps its physical value,
converted to the new units.

## Slices along x, y or z

A simulation volume is not a spectral cube, so its slice is not a channel. In
the Inspector, *Slice*:

- *Along* picks the axis the plane is normal to: X, Y or Z. The 2-D view then
  shows the other two axes (a Y slice is x by z), the cutting plane in the 3-D
  view follows, and the bar under the view reads `X 13 ── x = 0.05 pc` in the
  grid's coordinates instead of `CH 13`.
- *At* takes a coordinate in the units shown: type `0` and press *Enter* and
  the slice goes to the nearest plane. A value off the grid says so on the
  status bar and shows the nearest edge plane.

Changing the axis starts at the middle plane. The tools that read cube pixels
(*Extract Spectrum*, *Extract PV Diagram*, the region tools) work on Z slices
and are greyed out with the reason on X and Y slices. Moment maps, channel maps
and exports are unchanged. Along X or Y, a remote rendering session keeps its
own cutting plane at the last Z channel.

## Physical units

PLUTO writes values, coordinates and time in code units. The run's units are
three numbers in `definitions.h`:

| Name | Unit |
|---|---|
| `UNIT_DENSITY` | g/cm³ |
| `UNIT_LENGTH` | cm |
| `UNIT_VELOCITY` | cm/s |

When `definitions.h` is next to the data (or one or two directories above it),
VisIVO reads them and shows the variable in physical units: density in g/cm³,
velocities in cm/s, pressure (ρV²) in dyn/cm², the magnetic field (V√(4πρ)) in
G, coordinates in cm and the time (L/V) in s. Tracers stay as they are.
Expressions such as `(1.0e-24*CONST_mp)` are evaluated with PLUTO's constants.
A grid shown in cell indices (see below) has no coordinates to convert.

*Units* in the Inspector shows what is in force and where it came from.

- *Physical* switches between physical and code units. The colour bars, slice
  and probe values, statistics, histogram, isosurface and threshold levels, the
  axes of the 2-D view and the time all follow; the data in the file do not
  change.
- *Units…* takes the run's three units, or a factor and a unit for this
  variable (for example `Msun/pc^3`) and for the coordinates (for example `pc`).
  A field left empty keeps what the file gives; *Forget typed* goes back to
  `definitions.h`. What you type is kept with the run, so every variable of it
  and a later session use it, and it replaces `definitions.h` for that number.

*Export Current Channel as 2-D FITS* writes the plane being shown, along X, Y
or Z, with its axes (`CTYPE`, `CDELT`, `CUNIT`) and position (`SLICEAX`,
`SLICEIDX`, `SLICEPOS`). With *Physical* on, the values are multiplied by the
factor, `BUNIT` is the physical unit, and the header records `CODEUNIT`,
`UNITFAC`, `LENFAC` and `UNITSRC` so the file says what was done to the data.
Moment maps and other derived products are computed on the file's code units.

## Axes and header

- Uniform Cartesian grid: the axes are X, Y and Z (x1, x2, x3) in code units
  (physical when [`definitions.h`](#physical-units) gives them), with the cell
  width as spacing and the first cell's centre as origin.
- Stretched grid, or a polar, cylindrical or spherical run: the axes count
  cells, starting at 1 as in `grid.out`. The header says why (`PLUTOGRD =
  'INDEX'` and `PLUTOWHY`, for example *non-uniform cells along x1*). The array
  is shown as stored, not resampled.
- The header also gives the time (`PLUTOTIM`), step (`PLUTOSTP`), output
  number (`PLUTONUM`) and geometry (`PLUTOGEO`). Click the *CUBE · …* tag in
  the cube viewer's toolbar to see the header.

The axes are space, so tools that need a spectral axis or sky coordinates are
hidden, as for a [VBT volume](vbt-viewer.md). A large variable is rendered by
the backend from its brick pyramid, built the first time it is needed; after
that the desktop's preview is read from the pyramid too.

See also [Ray-traced volumes](raytraced-volumes.md) and [VR](vr.md).

Details: [Datasets API](../../technical/backend-api/datasets.md).
