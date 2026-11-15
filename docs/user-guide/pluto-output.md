# PLUTO simulation output

A [PLUTO](https://plutocode.ph.unito.it/) run's binary output opens in the
**cube viewer** (a 2-D run in the image viewer), one variable at a time.
Nothing is converted: the backend reads the variable where it is, at its offset
in the file, so a 2048³ density field (32 GB) opens from its text files alone
and costs no copy and no disk space.

## What to open

PLUTO writes each output as raw samples, with two text files beside them in the
run directory:

| File | What it says |
|------|--------------|
| `dbl.out` / `flt.out` | One line per output: its number, time, step, whether all variables are in one file (`single_file`, `data.0042.dbl`) or one file each (`multiple_files`, `rho.0042.flt`), the byte order, and the variable names. |
| `grid.out` | The geometry and every cell's edges along x1, x2 and x3. |

Choose **Open…** on any of:

- the **`.out` file**: the newest output it lists opens, on its first variable
  whose file is present;
- a **data file** (`data.0042.dbl`, `rho.0042.flt`): that output, on that
  variable (or the first, for a `data.*` file).

Both text files must sit beside the data. Without `grid.out` the file browser
says so instead of opening; copy it next to the data. A data file whose size
does not match the grid and the `.out` line is refused with both numbers, so a
truncated copy never opens as garbage.

## Variables

The other variables of the output are listed in *Session Data* as
*Field: name*; opening one opens that variable as a cube. In `multiple_files`
mode only the variables whose files are present are listed: a run copied one
variable at a time (only `rho.0000.flt`, say) offers just that one.
Face-centred magnetic fields of a constrained-transport run (`Bx1s` …) are not
cell values and are not listed.

## Axes

- **Uniform Cartesian grid.** The axes are X, Y and Z (x1, x2, x3) in the
  simulation's code units: the cell width as spacing, the first cell's centre
  as origin.
- **Stretched grid, or a polar, cylindrical or spherical run.** The axes count
  cells (1 at the first cell, as `grid.out` numbers them). The header says why:
  `PLUTOGRD = 'INDEX'` and `PLUTOWHY`, for instance *non-uniform cells along
  x1*. The picture is the array as stored, not resampled.

The header also carries the output's time (`PLUTOTIM`), step (`PLUTOSTP`),
number (`PLUTONUM`) and geometry (`PLUTOGEO`); *File ▸ Header* shows them.

As for a [VBT volume](vbt-viewer.md#volume-tables), the axes are space, so the
tools that need a spectral axis or sky coordinates are not shown; everything
that treats the grid as a 3-D field works, and a large variable is drawn on the
backend from its brick pyramid, built once the first time.

## See also

- [VBT tables](vbt-viewer.md)
- [Ray-traced volumes](raytraced-volumes.md)
- [VR headset](vr-headset.md)
