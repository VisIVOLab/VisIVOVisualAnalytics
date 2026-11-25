# RAMSES outputs

Issue [#549](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/549),
epic [#556](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/556). The
reader is `backend/app/ramses_output.py`, the code-independent mesh interface
`backend/app/amr_source.py`; the user's view is
[RAMSES outputs](../../user/reference/ramses-outputs.md).

## Files

Every `.outCCCCC` file is Fortran unformatted: records framed by their length.
The first record of each file is one integer, so its marker is 4 in the
writer's byte order (4- or 8-byte markers); every marker is checked when a
record is located, and a record is read as a numpy array straight from a
read-only map. Record offsets are found by hopping marker to marker.

- `amr_*`: the header (cells per level and process, the domain decomposition),
  then per level and per process the grid arrays: centre (`xg`), son index per
  cell and others. Each file holds the *virtual* grids of its neighbours too
  (the `numbl` table counts them); only the file's own process's grids are
  kept, or every cell would be counted more than once.
- `hydro_*`: per level and process, per cell of a grid, per variable, one
  record of one value per grid.
- `part_*`: eight header records, then one record per field.

Leaf = a cell with son index 0. The leaves tile the box: the sum of their
volumes is `boxlen^3` (checked on a real output). Cell `ind` (0..7) of a grid at
level `l` has side `2^-l` (box units) and centre `xg + (bit - 1/2) 2^-l`.

## Field names

`hydro_file_descriptor.txt` and `part_file_descriptor.txt` give the names, in
record order. Without a descriptor, the particle fields come from the `Particle
fields` line of `header_*.txt`, else from the pre-descriptor order (`pos vel mass
id level`, then 1-byte records `family`, `tag`, then floating-point records
`birth_time`, `metallicity`); hydro: `density vx vy vz pressure`, then passive
scalars (`metallicity`, `scalar_2`, ...). Value widths (single or double
precision, 4- or 8-byte identifiers) come from the record lengths.

## The particle seam

One component per family, named `<output folder>::ramses-family=<slug>`, is the
duck-typed `Component` of `gadget_snapshot.py` (`names`, `num_rows`, `take`,
`column`, `columns()`, `column_dtype`, `periods`, `units`, `physical`,
`describe`), so every reader of a particle table opens it unchanged.
`point_table.py` dispatches on the reference (`COMPONENT_FORMATS`,
`resolve_component`, `component_module`, `has_component`); the routes that
used to call `gadget_snapshot.resolve` call `resolve_component`. `take` reads
only the rows asked for, file by file; a family that is a part of a file
(stars among dark matter) is looked up through an index built once per file
(`int32` per particle of that family). A series is the sibling `output_*`
folders (`snapshot_series.py`); availability of a family is answered from the
header totals, so no particle file is read per member.

## Units

`RamsesOutput.unit_table()`, served by `physical_units.scales()`: lengths
`x * boxlen * unit_l` cm, masses `unit_d * unit_l^3` g, velocities
`unit_l / unit_t` cm/s, density `unit_d`, pressure `unit_d (unit_l/unit_t)^2`.
`unit_l` contains `aexp` in a cosmological run, so these are proper; the
comoving length is divided by `aexp`.

## The mesh: `AmrHierarchy`

`amr_source.AmrHierarchy` is what the AMR views (#550) and further producers
(#554) build on: `extent`, `levels`, `fields`, `leaf_counts()`,
`leaf_blocks(fields, levels, lo, hi, max_block)` (bounded memory: one block
per file and level, at most `max_block` cells), `leaf_cells(...)` (joined, with
a cell cap), `scales()`, `describe()`. A run whose leaves all lie on one level
is a uniform grid (`uniform_level()`); the cube is `extent * 2^level` cells a
side and a cell's index is `floor(centre / dx)`, so such an output opens as a
plain volume without any AMR step. RAMSES leaves at the coarse level carry no
hydro value (the output has none) and read as NaN.

Measured on `output_00185` (2018 zoom, 16 processes, 2,097,152 dark-matter
particles, 15,974,645 leaf cells over levels 7 to 13): the info file is parsed
in under 10 ms; leaf counts take about 0.4 s; the leaf cells of all levels with
two hydro variables stream in about 1.3 s with blocks of at most 0.4 million
cells; the sum of `density * dx^3` over the leaves is `omega_b / omega_m`.
