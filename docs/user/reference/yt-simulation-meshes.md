# Other simulation codes (through yt)

Enzo, FLASH, Athena and Athena++, Chombo (PLUTO-AMR included), AMReX/BoxLib
(Nyx, Castro, ...), GAMER and ART outputs open like a RAMSES mesh: as the gas
cells of the mesh, in the 3-D point viewer, with the same views of the mesh
(a uniform cube of one field, the refinement level of a slice, refine a box;
see [RAMSES outputs](ramses-outputs.md)).

This needs the [yt](https://yt-project.org) package next to the backend
(`pip install yt`). It is not part of the release: without it these files are
not recognised, and a request for one of their meshes answers that yt is
missing. Nothing else changes without it.

## What to open

Choose the output itself in *File ▸ Open…*: the checkpoint or plot file of
FLASH, GAMER and Chombo (an HDF5 file), the `.vtk` of Athena, the parameter
file of an Enzo output (`DD0011/DD0011`, the folder works too), the plot folder
of AMReX (the one with a `Header` and a `Level_0` folder; any other folder with a file called `Header` stays a plain folder). The backend asks yt's own checks which code
wrote it; a RAMSES output keeps its own reader and a Gadget or AREPO snapshot
its own, and neither is taken by yt.

## What you get

* A refined mesh opens as a table of its gas cells (density, temperature,
  pressure, velocity and the fields of the code), the size of a cell being its
  smoothing length. Lengths are in the box's own scale (pc, kpc or Mpc;
  centimetres for a box below 0.3 pc), densities in g/cm^3, velocities in km/s
  (cm/s for a small box), the cell mass in Msun (g).
* A classic uniform-grid output (a single level: unigrid Enzo, uniform FLASH,
  Athena) opens as a cube of its density at its own resolution, with no
  resampling. Any other field: the uniform cube of the mesh view.
* The origin of the box is its lower corner; where that lies in the code's
  coordinates is stated in the output's description.

## Limits

Only 3-D meshes whose levels refine by 2 and whose cells are cubes are read;
anything else is refused with the reason. An octree mesh (ART) is read a chunk
of yt's own I/O at a time but its cells are counted by reading their sizes once.
Moving-mesh and particle codes (AREPO, Gadget, Gizmo) are not meshes of this
kind.
