# Gadget snapshots

A Gadget snapshot opens as a table of particles, one particle type at a time,
in the same 3-D point viewer as a [VBT table](vbt-viewer.md). The backend reads
the particles where they are in the snapshot's files, so nothing is converted
first. Open it with *File ▸ Open…*: *File type* ▸ *Gadget snapshots* lists the
names Gadget writes (`snap_…`, `snapshot_…`). A snapshot with any other name
opens as well, from *All files*, because the backend recognises it from its
header.

## What to open

| Snapshot | Open |
|---|---|
| One file (`snap_010`) | the file |
| Several files (`snapdir_010/snap_010.0` … `.N`) | any one of the files, or the `snapdir_010` folder; all the files are read together |
| HDF5 (`snap_010.hdf5`, `snap_010.0.hdf5` …) | as above |

Binary format 1 (no block names) and format 2 (named blocks) are both read,
in either byte order. If a file of a multi-file snapshot is missing, or the
particle counts do not add up to the header's totals, the file browser says
which and nothing opens.

## Particle types

When you open a snapshot, a dialog asks which component to open: *gas*,
*dark matter*, *stars*, *black holes* (types 0, 1, 4 and 5; types 2 and 3 are
listed when the run has them). Each opens in its own window, named
*snapshot · type* (for example *snap_010.0 · gas*), and is listed in
*Session Data* and *Recent* under that name.

## Columns

Each block of the snapshot becomes one or more columns for that particle
type:

| Block | Columns |
|---|---|
| `POS`, `VEL` | `x`, `y`, `z`; `vx`, `vy`, `vz` |
| `ID`, `MASS` | `id`, `mass` (from the header's mass table when the block has none for the type, as is usual for dark matter) |
| gas: `U`, `RHO`, `HSML`, `NE`, `NH`, `SFR` | `u`, `rho`, `hsml`, `ne`, `nh`, `sfr` |
| `AGE`, `Z`, `BHMA`, `BHMD` | `age`, `metallicity`, `bh_mass`, `bh_mdot` |
| any other block | its name in lower case (`name_0`, `name_1` … for several values per particle) |

Gas also has two computed columns:

- `temperature` in K, from `U` and `NE`. Without an `NE` block the gas is
  taken as fully ionised (X = 0.76), and the snapshot's information says so.
- `entropy` K = kT / n_e^(2/3) in keV cm².

## Units

The other columns are in the run's code units. By default these are Gadget's:
lengths in comoving kpc/h, masses in 10¹⁰ M☉/h, and velocities in km/s
including Gadget's internal factor √a. If the run's `parameters-usedvalues`
file is next to the snapshot or one folder up, its `UnitLength_in_cm`,
`UnitMass_in_g`, `UnitVelocity_in_cm_per_s` and `ComovingIntegrationOn`
values are used instead (a run in Mpc/h, for example). An administrator can
set them on the backend with `VISIVO_GADGET_UNITS`
(see [Configuration](../../technical/configuration.md)).

The copilot reports the snapshot's redshift, scale factor, box size and
cosmology, the units of each column, and the factor that converts each one
to physical units: kpc (× a/h), M☉ (/h), and peculiar km/s (× √a).

## Rendering and analysis

You can use a particle type wherever you can use a VBT point table: filters and
colour by any column, the remote 3-D view on the backend,
[ray-traced particles](raytraced-particles.md), the [VR headset](vr.md), and
the *Previous* and *Next* snapshot buttons in the headset, which step through
`snapdir_000`, `snapdir_001` … (or `snap_000`, `snap_001` … in one folder)
and keep the same particle type.

Details: [technical notes](../../technical/design/gadget-snapshots.md).
