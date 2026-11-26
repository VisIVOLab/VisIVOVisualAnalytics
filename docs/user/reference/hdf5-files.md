# HDF5 tables, volumes and images

Any HDF5 file that is not a Gadget or AREPO snapshot and not a beamformed
dynamic spectrum opens where it lies, with *File ▸ Open…*:

- the 1-D numeric datasets of one group that share a length, or a 1-D compound
  dataset, open as a table in the 3-D catalogue viewer (one column each);
- a rank-3 numeric dataset opens as a cube;
- a rank-2 numeric dataset opens as an image.

A dataset that is contiguous and uncompressed is read straight from the file.
A chunked or compressed one is unpacked once into the workspace cache and read
from there (the cube viewer, the pyramids and the moments all read a plain array
of samples). A table is never read whole: its columns are read by rows, a
chunk at a time, when the viewer asks.

## Choosing the dataset

A file with one thing in it opens at once. With several, the open dialog lists
them (path, shape and type, and the unit when the file gives one); pick one, then
type an optional hyperslab. The same choice can be written as a reference, for
example in *Open recent*, in a script or in a link:

```text
run.h5::hdf5=/grid/rho                 a volume or an image
run.h5::hdf5=/grid/rho[0:64,:,:]       the first 64 planes
run.h5::hdf5-table=/halos              a table (a group, or a compound dataset)
run.h5::hdf5-table=/halos[1000:5000]   rows 1000 to 5999
```

A hyperslab is `offset:count` for each axis, in the file's own axis order. An
empty part is the whole axis; a missing count runs to the end (`5:`). It
counts samples, not world units. A table has one part, its rows. A box that is
one run of the file's bytes (whole planes) is still read in place; any other
is copied once.

## Units and axes

Units come from the dataset's attributes `units`, `unit` or `BUNIT` and are
shown as stored (factor 1); type another factor and unit if you need one. A
volume's axes are plain X, Y, Z samples: the file states no grid. A table
takes `x`, `y` and `z` when it has them, else its first three columns; choose
other columns in the viewer. A table is read as Cartesian: positions in sky
coordinates are chosen as columns, not converted.

## What is not this

Gadget and AREPO snapshots and SubFind catalogues keep their own page
([Gadget snapshots](gadget-snapshots.md)). An HDF5 whose file or dataset
attributes name a time or frequency sampling (`TSAMP`, `FCH1`, ...), whose
dataset is named like a dynamic spectrum, or that holds the LOFAR BFData layout
opens as a dynamic spectrum, as before.
