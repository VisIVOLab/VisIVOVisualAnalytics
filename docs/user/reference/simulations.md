# Simulation snapshots (AREPO)

An AREPO snapshot is an unstructured moving mesh, so a region of it has to be
resampled onto a regular grid before it can be shown. *Data ▸ Extract from
AREPO Snapshot…* does this on the backend, with yt: pick the snapshot on the
backend, choose a window and a resolution, and get one FITS cube per field,
which opens in the [cube viewer](cube-viewer.md).

## Contents

The left half of the dialog lists the snapshot's HDF5 tree: groups, datasets
with shape and type, and attributes. Use it to see which particle types
(`PartType0`, `PartType1`, …) and fields the file holds, its box size and its
units. Links into other files are named but not opened, and a very large file
is listed only in part.

## What to extract

| Field | Meaning |
|---|---|
| *Level* | yt refinement level of the whole simulation domain. A cell is `domain / (base grid × 2^level)`, so a level fixes the cell size whatever the window: level 7 on a 100 kpc box is a 781 pc cell. |
| *Particle type* | The family to sample (`gas` for the mesh). |
| *Fields* | One or more field names, separated by spaces or commas. Each becomes its own FITS cube. |
| *Size (x, y, z)* | The window, in kpc, centred on the simulation centre; at most the box. |
| *Offset from centre* | Moves the window, in kpc. |

Under these settings the dialog shows the resulting grid, the cell size and the
memory per field, computed by the backend before you start. The window is
rounded to a whole number of cells; when the extracted extent differs from the
request, the dialog shows *Extracted extent*.

*Extract* runs the job; when it ends you are offered the cubes to open.

## Output

Each field is a FITS cube in a new directory under the backend's exports, so a
second run never overwrites the first. The header has:

- a linear WCS in parsec on all three axes, centred on the array;
- `BUNIT` from yt's units for the field;
- provenance: `AREPOSRC` (snapshot), `AREPOLVL` (level), `AREPOPRT` (particle
  type), `AREPOFLD` (field), and `AREPOW*`, `AREPOR*`, `AREPOO*` (extracted
  extent, requested window, offset).

## Limits

- yt must be installed on the backend (`pip install yt`). Without it, browsing
  and planning still work and *Extract* is disabled with that explanation.
- Cosmological (comoving) snapshots are refused: the window is in proper kpc.
- Vector fields are refused; extract the components separately.
- The grid is dense, so the backend caps the total cell count and says so.

Details: [Simulations API](../../technical/backend-api/sed-simulations.md).
