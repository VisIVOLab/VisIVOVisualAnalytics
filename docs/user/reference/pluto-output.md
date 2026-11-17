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

## Axes and header

- Uniform Cartesian grid: the axes are X, Y and Z (x1, x2, x3) in code units,
  with the cell width as spacing and the first cell's centre as origin.
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
