# Raw points, raw grids and FLY files

Three formats of the VisIVO Importer, `rawPointsDesc`, `rawGridsDesc` and
`flyDesc`, open directly: no conversion to a VisIVO Binary Table. A small text
file, the *descriptor*, lists the binary files of a simulation, one per
snapshot, with a time tag each. Open the descriptor with *File ▸ Open…* (set
*File type* to *All files*). Points open as a table in the 3-D catalogue
viewer, a grid as a cube (an image when it is 2-D). Each listed file is one
dataset, and the list is a series you can step through.

These formats carry no physical units. Values and coordinates show as the
numbers in the files; type a factor and a unit for a dataset if you know them.

## Raw points

```text
rawPointsDesc
dark            variable name
Float           Float or Double
time            (unused, but present)
130000          points per file
50 50 50        box size
b               byte order: b (big) or l (little)
0.0 16ml_096    time tag and file name, one pair per snapshot
0.5 16ml_104
1.0 16ml_112
```

Each file holds `x y z` of every point interleaved, then each scalar field as
a block of one value per point. The number of scalar fields is not written
anywhere: it is what the file's size holds. Columns are named `X`, `Y`, `Z`,
`scalar0`, `scalar1`, and so on.

## Raw grids

```text
rawGridsDesc
density         variable name
3               number of dimensions (1 to 3)
Float           Float or Double
64 64 64        cells along each dimension
time            (unused, but present)
l               byte order
0.0 JET8Xhj.f064.dat.pff
0.7 JET8Xeh.f064.dat.pff
```

Each file is one value per cell, the first dimension varying fastest. A grid
opens in the cube viewer with its axes as `X`, `Y`, `Z` in cell units (the
format states no box). A 1-D grid has nothing to show and is refused.

A file whose size does not match the declared type is refused, and the message
says if it matches the other type. The importer itself always read a grid as
float; here the declared type is used.

## FLY

```text
flyDesc
2m_test         name
double          Float or Double
time
2000000         particles per file
50 50 50        box size
l               byte order
0.0 snapshot_000
1.0 snapshot_001
```

A FLY file holds `x y z` of every particle interleaved, then `vx vy vz`
interleaved. The columns are `X`, `Y`, `Z`, `Vx`, `Vy`, `Vz`.

A FLY file with no descriptor (named `*.fly`) opens by itself when its size
and values leave one reading of its type and byte order. When they do not
(a float file whose size also fits doubles, or a file of zeros), the file
browser says so: write a `flyDesc` descriptor with the type, the number of
particles and the byte order, and open that.

## The files a descriptor lists

File names are taken relative to the descriptor (or as absolute paths). Every
file must be inside the folders the backend may read; files at `http://` or
`ftp://` addresses are not fetched, copy them first.

## In VR

The series of a points descriptor steps in the headset like a numbered series
of snapshots, in the order listed.
