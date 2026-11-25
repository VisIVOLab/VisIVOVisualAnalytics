# RAMSES outputs

A RAMSES output opens as a table of particles, one family at a time (dark
matter, stars, sinks, ...), in the same 3-D point viewer as a
[Gadget snapshot](gadget-snapshots.md) or a [VBT table](vbt-viewer.md). The
backend reads the particles where they lie in the output's files: nothing is
converted first, and yt is not needed.

## What to open

An output is a folder `output_NNNNN` with `info_NNNNN.txt`, one
`part_NNNNN.outCCCCC` per process (and `amr_`, `hydro_`, `grav_` files for the
gas and the mesh). In *File ▸ Open…*, choose *File type* ▸ *RAMSES outputs* and
pick any of `info_…`, `part_…` or `amr_…` in the folder (the dialog opens files,
not folders). The
backend recognises an output by its info file, not by a name. A folder of
outputs (`output_00080`, `output_00081`, ...) is a series: the timeline of the
VR page steps through them, skipping an output that has no particles of the
family shown.

Both byte orders are read. If the file of one process is missing, the file
browser says which and nothing opens.

## Families

The *Open* dialog lists the families the output has, with their counts: *dark
matter*, *stars*, *sinks*, and the other families RAMSES defines. Each opens in
its own window named *output · family*. Runs that wrote a `family` record give
it directly; older outputs have none, and then all particles are dark matter,
except that a particle with a birth time is a star when the header counts
stars.

## Columns

`x`, `y`, `z` (a fraction of the box, 0 to 1), `vx`, `vy`, `vz`, `mass`, `id`,
`level` and, when the output has them, `family`, `tag`, `birth_time` and
`metallicity`. The columns come from `part_file_descriptor.txt` when the run
wrote one; an output without one is read in the order RAMSES used before
descriptors (positions, velocities, mass, identity, level, then what follows by
its size), so a 2018 output opens like a recent one.

## Units

Values stay in code units; the Inspector and the copilot give the factor that
makes each one physical, from `unit_l`, `unit_d` and `unit_t` of the info file:
lengths in kpc (`x * boxlen * unit_l`), masses in M☉, velocities in km/s. In a
cosmological run `unit_l` already contains the expansion factor, so these are
proper (physical) values at the output's scale factor, as for Gadget
snapshots; the comoving length is the same divided by `aexp`, which the
information panel gives. A run in a periodic box wraps positions at the box
when a selection reaches its edge.

In the 3-D points window the axes are drawn as stored, so their titles name the
stored unit: `X (box units)` for a RAMSES output, the snapshot's own length unit
(`kpc/h comoving`, for example) for Gadget. A table that states no unit (a VBT)
keeps `(Mpc)` on its axes. Session Data names the table by its format, RAMSES
here, and the window opens coloured by the first of density, temperature, mass,
and so on that varies over the rows loaded: the mass of dark-matter particles is
one value, so the colour column is then another that varies or *Single colour*,
and choosing a constant column yourself gives a bar with that one value. The
same holds when the backend draws the points (the axis titles and the bar are
the same) and over a ray-traced picture, whose bar reads *mass (constant)* with
the value.

## Gas cells

The mesh's gas opens as one more component, *gas cells*, beside the families:
one row per leaf cell with `x`, `y`, `z`, the cell size `dx`, its refinement
`level`, `mass` (density times volume), `density`, the velocities, `pressure`
and the passive scalars the output has (`metallicity`, ...). A mesh with gas and
no particle files opens as its cells. The ray-traced view draws each cell with
its own size, at the resolution the run wrote, without resampling. Uniform
cubes of a field at a chosen level, the refinement level of a slice and
"refine a box" come from the same cells (a uniform cube averages the cells
finer than a voxel by volume for density, pressure and mass, and by mass for
velocities and passive scalars; it is stored in code units and Display >
Physical shows the run's units); the backend routes are in
[AMR outputs](../../technical/backend-api/sed-simulations.md).

## A uniform cube of one field

*Data ▸ Uniform Cube of an AMR Output…* asks for any file of the output folder,
then for a field (`density`, a velocity, `pressure`, `mass`, the refinement
`level`...), a level and, if you want, a region of the box. A voxel is the size
of a cell of the level you choose, so each level halves it; *Finest that fits*
takes the finest level whose cube stays within the voxel limit (64 million by
default). The dialog shows the plan before anything runs: the level, the grid,
the size of the voxel in kpc, the memory the job holds, the finest level of the
mesh inside the region, and, if the cube is too large, the level that does fit.
*Resample* runs on the backend and the cube opens like any volume, with X, Y
and Z in kpc from the corner of the box, and the field's unit. Cells larger than
a voxel fill it; cells finer than a voxel give their volume-weighted mean, so
the mass is conserved (the sum of a `mass` cube is the gas mass of the region).
The header records the output, the field, the level and the finest level of the
mesh in `AMR*` cards.

## The refinement blocks of a slice

In the window of such a cube, *Inspector ▸ AMR mesh ▸ Show AMR blocks* outlines
the cells of the mesh that the slice cuts, coloured by level (blue the coarsest,
red the finest). It follows the slice and the slice axis; the status bar counts
the cells, and if a slice cuts too many to draw, the finest levels are left out
and it says so. The blocks come from the mesh, not from the cube, so they show
the true refinement even where the cube is coarser.

## Refine a box

*Tools ▸ Refine Box of the AMR Mesh…* (or *Refine this box…* in the same
Inspector section) uses the same box dialog as *Refine Selected Box*: it starts
on the drawn region, else on the middle half of the cube, and draws the box in
the volume while you edit it. *Refine* asks the backend for the plan of that
box at the finest level the mesh holds inside it, shows the level, the grid and
the memory, and on *Yes* resamples it. The result opens in a new window, in the
same coordinates as the cube it came from; the first window stays as it is, so
the two can be compared.

## Not read yet

`grav_` files are not read. A mesh with no `hydro_` file has no gas to show and
is named as a RAMSES mesh without being opened.
