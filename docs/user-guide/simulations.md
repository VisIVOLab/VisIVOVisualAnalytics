# Simulation snapshots (AREPO)

AREPO is a moving-mesh code: a snapshot is an unstructured set of cells that
follow the gas, not a grid. There is nothing to display until a region of it has
been **resampled onto a regular grid** — and once it has, it is an ordinary FITS
cube that every tool in VisIVO already understands.

*Data → **Extract from AREPO Snapshot…*** does that resampling. You pick the
file on the backend, choose a window and a resolution, and get one cube per
field.

```{note}
The snapshot is read, resampled and written **on the backend**: a simulation is
tens of gigabytes and lives where the compute is, and the resampling is yt's.
The legacy client instead spawned `python arepo-extractor.py` against an
interpreter path stored in its settings, so it worked on the machine it had been
set up on.
```

## Browsing a snapshot

The left half of the window is the snapshot's own HDF5 tree: groups, datasets
with their shape and type, and every attribute. This is where you find out what
the file actually contains — which particle types (`PartType0`, `PartType1`, …),
which fields, what the box size is, what units the simulation was run in.

Two things the listing will tell you about itself:

- A link that points **into another file** is named but not opened. The
  extraction was authorised to read this snapshot, not whatever else the file
  refers to.
- A very large or deeply nested file is listed only so far, and says so. The
  point of the tree is orientation, not a full dump.

## Choosing what to extract

```{list-table}
:header-rows: 1
:widths: 24 76

* - Setting
  - What it means
* - **Level**
  - yt refinement level of the **whole simulation domain**. A cell is
    `domain / (base grid × 2^level)`, so a level means one resolution whatever
    window you ask for — level 7 on a 100 kpc box is a 781 pc cell, whether you
    extract 5 kpc or 50.
* - **Particle type**
  - Which family to sample (`gas` for the mesh itself).
* - **Fields**
  - One or more field names, separated by spaces or commas. **Each one becomes
    its own FITS cube**, with its own unit in `BUNIT`.
* - **Size (x, y, z)**
  - The window, in kpc, centred on the simulation centre. It cannot be larger
    than the box.
* - **Offset from centre**
  - Moves that window, in kpc, away from the centre of the simulation.
```

Underneath, the panel shows what the job would produce **before** you start it:
the grid, the cell size and the memory per field. That is a real calculation
done by the backend, not an estimate — the legacy asked "level 9+ may take
minutes and results in a uniform cube of about 1+ GB, continue?" from a fixed
threshold that knew nothing about the window you had actually typed.

```{tip}
The cell size is fixed by the level, so your window is rounded to a whole number
of cells. When the extracted extent differs from what you asked for, the panel
says so — and the cube's header records both (`AREPOW*` is what was extracted,
`AREPOR*` what was requested).
```

## What you get

One FITS cube per field, in its own directory under the backend's exports, with
a header that describes the data:

- a **linear WCS in parsec** on all three axes, its reference pixel at the
  centre of the array, spanning exactly the extracted extent;
- `BUNIT` from yt's own units for that field;
- provenance: `AREPOSRC` (the snapshot), `AREPOLVL` (level), `AREPOPRT`
  (particle type), `AREPOFLD` (field), `AREPOW*`/`AREPOR*`/`AREPOO*` (extent,
  requested window, offset).

Every extraction gets a fresh directory, so running the same job twice with a
different window cannot overwrite the first result.

When the extraction finishes you are offered the cubes; opening them puts each
one in the cube viewer like any other FITS.

## Limits worth knowing

- **`yt` must be installed on the backend.** Browsing and planning need only
  h5py, which every deployment has, so the tree and the cost calculation always
  work; the Extract button is disabled with that explanation when the backend
  cannot run the resampling. Install it there with `pip install yt`.
- **Cosmological snapshots are refused.** Comoving coordinates bring in the
  scale factor and *h*, and this extraction places the window in proper kpc.
  Refusing is the honest answer; a silent factor of (1+z)/h in the extracted
  region is not.
- **Vector fields are refused.** A field that is not a scalar comes back with a
  component axis, which cannot be described by a three-axis WCS. Extract the
  components separately.
- The grid is dense — every cell is materialised — so the backend caps the
  total cell count and says so rather than running out of memory. An extraction
  also goes through the same admission control as the other long computes, so
  several of them cannot saturate the server at once.

## See also

- [Cube viewer](cube-viewer) — where the extracted cube opens.
- [Backend API](../backend-api) — `/v1/simulations/arepo/inspect`, `/plan`,
  `/extract`.
