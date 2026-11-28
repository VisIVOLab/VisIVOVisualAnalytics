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
| SubFind (`groups_010/sub_010.0` …, or the HDF5 `fof_subhalo_tab_010.hdf5` of Gadget-4 and Arepo) | any file in the group catalogue; choose **FoF groups** or **subhalos** |

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

### FoF groups and SubFind subhalos

A SubFind group catalogue opens as objects rather than particles. FoF groups
start as spherical markers sized by `rvir` and coloured by `mvir`; subhalos use
their half-mass radius and mass when those blocks are present. Use **Size by**
and **Colour by** to select R500/R2500 or another mass definition instead.
SubFind defines these radii against the **mean background density** of the
universe, not the critical density many other catalogues use: `rvir` is
relative to the mean background density, `r500` and `r2500` enclose 500 and
2500 times it. The Size list, Source Info and the status line say so next to
each radius.

Select a marker or table row to inspect its physical properties. Double-click
the marker (in the Local, Remote or ray-traced view) or its table row to
centre the camera on it and frame 1.25 times its radius. The radius the
markers are sized by is used when it is one of them; otherwise `rvir`, then
R500/R2500 (half-mass radius or Rmax for a subhalo). The camera keeps the
direction it looks from. A line naming the target and the radius used
appears in the window's status bar for a few seconds and stays in the
Inspector's selection line (also when the view was already framed). A row highlighted from another tool
over SAMP is only recentred, at the same distance, and never reframed.

The Inspector states that the dataset is a SubFind catalogue and distinguishes
FoF groups from subhalos. Stored code units and their kpc/M☉/km s⁻¹ conversions
are shown alongside the other Gadget metadata.

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

In a SubFind catalogue, subhalo velocities are taken as peculiar km/s and Vmax as physical km/s, as Gadget-4 and Arepo write them; an HDF5 catalogue's group velocity is converted with 1/a. The convention of the P-Gadget3 SubFind writer (binary `sub_…` files) is not public and is unconfirmed: `svel=snapshot` or `svel=canonical` (and `srad=` for the subhalo radii) in `VISIVO_GADGET_UNITS` state another one.
For a Gadget-4 catalogue with its snapshot beside it, ask the copilot to check the velocity convention: it compares the stored velocity of the most massive subhalos with that of their particles and names the convention.
A folder holding both a snapshot and its group catalogue opens the snapshot; the file browser names the catalogue, which you open as its own file. Gadget-4 lightcone catalogues (`lc_fof_…`) keep their velocities as written (already peculiar), and their positions, centred on the observer, never wrap round the box.

Without a parameter file, the header decides whether the run is
cosmological. A run with a matter density and a redshift that matches its
scale factor is comoving. Any other run, an isolated galaxy for example, is
not: its lengths and velocities are not scaled by a.

A block whose particle types cannot be told from its size (for example,
equal numbers of gas and dark-matter particles in a single file) is left out
rather than guessed. So is a block whose `INFO` entry does not match its size
or names a type the reader does not know. The snapshot's information and the
file browser list it and say why; the other blocks open as usual.

The copilot reports the snapshot's redshift, scale factor, box size and
cosmology, the units of each column, and the factor that converts each one
to physical units: kpc (× a/h), M☉ (/h), and peculiar km/s (× √a for particles; × 1 for SubFind subhalos).

A large particle table starts as Gaussian splats 0.75 mean particle spacings
wide, with an opacity that falls as the cloud gets deeper, so the same field
looks equally bright from 100,000 or 30 million particles at one intensity:
in the Local and *Remote* views and in a headset with a float target. A
headset without one still brightens large tables. For quantitative
comparisons use a [projected map](raytraced-particles.md). Lower
**Splat size** if dense regions saturate. The whole slider changes the size,
in the Local and Remote views alike: splats from 0.25 to 12 on the scale of
the 0.75 above (3 at the slider's default), points from 1 to 16 pixels.
Switching the shape to *Point* restores the default size, as long as you have
not moved the slider.

## Rendering and analysis

A particle type works wherever a VBT point table does: filters and colour by
any column, the *Camera* presets, the remote 3-D view on the backend,
[ray-traced particles](raytraced-particles.md) and the [VR headset](vr.md),
whose *Previous* and *Next* buttons step through the numbered outputs. The
3-D regions and *Selection Diagnostics…* (histogram, radial profile and
phase diagram) described in
[Catalogues and HiPS](catalogues-hips.md#3-d-regions-and-selection-diagnostics)
evaluate every particle of the type, not only the loaded sample. A profile
can be centred on a SubFind group or subhalo selected in the SubFind
catalogue's window.

In a cosmological snapshot the box is periodic: a box or sphere selection near
one face also takes the particles just across the opposite face. A run that
does not say whether it is periodic is taken as periodic when it is comoving,
which is wrong for a zoom or isolated run in comoving coordinates; the
administrator can set `periodic=0` in `VISIVO_GADGET_UNITS`
([Configuration](../../technical/configuration.md)).

### Move through simulation time

When the backend finds other numbered outputs beside the open one
(`snap_000`, `snap_001` … or `snapdir_000/snap_000.0` …), a **Snapshot
Timeline** card appears in the Visualization panel with the output number,
its position in the series, the time and the redshift. **Previous**, **Next**,
the slider and **Play** move through the series with the same particle type.
Outputs without that type (an early output with no stars yet), or that cannot
be read, are skipped, and the card says how many. A catalogue opened from
several files at once has no timeline.

Each linked quantity window moves with the step and keeps its quantity (by
column name), colour map, stretch, representation, size, render budget,
camera and position. The colour range is recomputed for the new output. A
missing quantity, or an output that fails to open, is reported in the status
bar. The selection is cleared at every step: a row in one output is not
claimed to be the same particle in the next.

### Compare physical quantities

*Tools ▸ Compare Quantity…* adds a pane to the window: another view of the
**same loaded particles**, coloured by a different quantity (for example `rho`
beside `temperature`), in a 1, 2 or 4 pane layout with pop-out
([Compare quantities in panes](particle-panes.md)). The comparison card at the top of the Visualization panel
names the view's quantity and offers *Add another quantity…*; the toolbar above the
panes (1 / 2 / 4, **Linked**, **render**) is there from the start. In *Visual
Encoding*, **Colour by** chooses the quantity, **Colormap** only the palette,
and **Range and scale…** the view's range and stretch.

- **Linked** (on by default; the toolbar button for every pane, the button in a
  pane's header for that pane): the panes share camera, selection, filters,
  shape, size, render budget and ray-traced light. Quantity, colour map, range
  and stretch stay per view. Turn it off to make one view `UNLINKED`.
- **render** (the toolbar chip, on by default): turn it off to keep camera and
  selection linked but let shape, size, budget and light differ.
- A filter or *Load more* in one view keeps the group's camera; *Reset
  Camera* frames the new rows in every view.
- A view drawn on the backend (*View ▸ Rendering ▸ Remote*) follows the
  group's camera but does not lead it; switch it to *Local* to lead.

Details: [technical notes](../../technical/design/gadget-snapshots.md).
