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

Without a parameter file, the header decides whether the run is
cosmological. A run with a matter density and a redshift that matches its
scale factor is comoving. Any other run, an isolated galaxy for example, is
not: its lengths and velocities are not scaled by a.

A block whose particle types cannot be told from its size (for example,
equal numbers of gas and dark-matter particles in a single file) is left out
rather than guessed. The snapshot's information lists it and says why.

The copilot reports the snapshot's redshift, scale factor, box size and
cosmology, the units of each column, and the factor that converts each one
to physical units: kpc (× a/h), M☉ (/h), and peculiar km/s (× √a).

For a large particle table, the initial Gaussian-splat size is derived from
the mean particle spacing and starts at 0.75 times that spacing. Opacity is
reduced for deeper point clouds so the initial view retains structure instead
of immediately saturating. **Splat size** remains available for deliberately
emphasizing compact or diffuse features.

## Rendering and analysis

You can use a particle type wherever you can use a VBT point table: filters and
colour by any column, the remote 3-D view on the backend,
[ray-traced particles](raytraced-particles.md), the [VR headset](vr.md), and
the *Previous* and *Next* snapshot buttons in the headset, which step through
`snapdir_000`, `snapdir_001` … (or `snap_000`, `snap_001` … in one folder)
and keep the same particle type.

### Move through simulation time

When the backend finds other numbered outputs beside the open one, a **Snapshot
Timeline** card appears in the Visualization panel. It shows the output number,
its position in the discovered series, the header time and redshift when
available. Use **Previous**, **Next**, the slider or **Play** to move through
the series. Both flat series (`snap_000`, `snap_001`, …) and Gadget's numbered
folders (`snapdir_000/snap_000.0`, …) are supported; the selected particle type
is retained.

If several physical-quantity windows are linked, the new output is opened for
the complete group before the old windows close. Each view keeps its quantity,
colormap, stretch, representation, render budget, camera and window placement.
The numerical colour range is recalculated for the new output because its data
distribution may differ.

Source selection is deliberately cleared at every step. The timeline compares
the same particle *type* and region, but it does not claim that a row in one
snapshot identifies the same particle or structure in the next. Cross-snapshot
identity requires a separate validated tracking workflow.

### Compare physical quantities

Choose *Tools ▸ Compare Quantity…* in a Gadget particle window to open another
view of the **same loaded particles**. Each linked window visualizes a different
physical quantity through colour. For example, a `rho` view and a `temperature`
view have the same particle positions but obtain their colours from different
columns and use different numerical ranges.

The comparison card at the top of the Visualization inspector identifies linked
windows and shows the quantity used by the current view. It also provides *Add
another quantity…* for creating a third view. In Visual Encoding:

- **Colour by** selects the data or physical quantity being visualized;
- **Colormap** selects only the colour palette used to represent that quantity;
- **Range and scale…** controls that view's numerical range and stretch.

The **Linked** button is on by default. Linked views share camera, source
selection and representation: shape, splat or point size, render budget and
ray-traced-light mode. This keeps rendering choices from masquerading as
physical differences. Quantity, colormap, colour range and stretch remain
independent because different physical fields need different encodings.

**Match rendering** is also on by default. Turn it off only when the rendering
itself is the intended comparison: camera and source selection remain linked,
but shape, size, render budget and ray-traced-light mode become autonomous for
that view. Turning it on again immediately adopts the rendering of a linked
peer.

Turn **Linked** off to make one view autonomous. Its comparison card and title
show `UNLINKED`, but it remains a member of the comparison. Turning **Linked**
on again immediately adopts the camera, selection and representation of an
already linked peer (unless **Match rendering** is off); it does not overwrite
that view's quantity or colour mapping.

Details: [technical notes](../../technical/design/gadget-snapshots.md).
