# Gadget snapshots

Issue [#31](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/31).
The reader is `backend/app/gadget_snapshot.py`; the user's view is
[Gadget snapshots](../../user/reference/gadget-snapshots.md).

## The seam: a particle type is a table of points

Every part of the application that draws or analyses a simulation's particles
reads them as a VBT point table: the point render session
(`render_session/points.py`), the ray-traced particle render
(`routers/particle_render.py`), the VR page's samples and series
(`routers/vr_layers.py`), the catalogue routes (`/v1/catalogue/query`) and the
copilot (`catalogue_summary`, `list_datasets`). All of them reach the data
through two places:

- `point_table.open_point_table(path)`: the columns, row count and default
  axes of the table that a render process or route reopens by path;
- `dependencies._catalogue_load_dataframe(path, format)`: the table that
  `/v1/catalogue/open` keeps in the dataset entry.

A Gadget particle type is named by a reference string,
`<first file>::gadget-type=<N>`. The string has the same form as a VBT field
(`::vbt-field=`) or a PLUTO variable (`::pluto-var=`), so it can be pickled
and sent to render processes and worker pools. Both places above resolve the
reference, and the functions in `point_table.py` under *Particle tables*
answer every other question about it: whether a format is a particle table
(`PARTICLE_FORMATS = ("vbt", "gadget")`), the file the data roots check
(`split_component`), the reference a file or a folder opens by
(`particle_dataset_path`), the other components (`particle_components`), and
the snapshot's metadata (`particle_metadata`). The routes that used to check
for `format == "vbt"` (`render.point_table_spec`, `/v1/points/info`,
`/v1/points/series`, SAMP, the copilot) now ask these functions, so a new
particle format needs a resolver here and no change in the routes.

On the desktop, the catalogue window treats a dataset as particles when the
open response says `particles: true`. Its name and export stem come from
`DatasetName.h`, which handles both kinds of reference. The *Open* dialog
offers the snapshot's `components` as returned by the classifier. Nothing in
the client is specific to Gadget except the name filter in the file browser.

## Reading

**Byte order and format.** The first 4-byte record marker of the file is 256
(format 1: the header record) or 8 followed by `HEAD` (format 2), read
little-endian or big-endian; the order that gives one of these values is the
file's order. For format 1, the header's closing marker must also be 256. An
HDF5 file is a snapshot when it has a `Header` group with
`NumPart_ThisFile`. Classification uses this test, not the file name.

**Records.** Every Fortran record is read through its markers, and the
closing marker must equal the opening one. A format-2 name record must be 8
bytes, and the size it announces must be the following block's size plus 8.
Any mismatch is an error that names the file and the byte offset.

**Blocks.** Format 2: each block is listed under its name, known or not.
Format 1 has no names, so each record is matched to the next block in
Gadget-2's order (POS, VEL, ID, MASS, U, RHO, NE and NH when
`flag_cooling` is set, HSML, SFR when `flag_sfr` is set, AGE, Z, POT, ACCE,
ENDT, TSTP) whose expected size it has. A record that matches none is listed
as `BLK<n>`. A block for which a file has no particles is not written in that
file, as Gadget does.

**Which types a block holds.** A block stores its types one after the
other, type 0 first. A known block (`BLOCKS`, the Gadget-2 and Gadget-3
names: POS/VEL/ID/POT/ACCE/TSTP for all types, MASS for the types with mass 0
in the mass table, the gas blocks for type 0, AGE/iM/HSMS for 4, Z and Zs for
0 and 4 with as many values per particle as the size says, BHMA/BHMD/BHPC/ACRB
for 5, and the SubFind blocks GPOS/MVIR/RVIR for groups and SPOS for
subhalos) is read with its own types when its size fits them in every file.
Any other block, or a known one whose size does not fit, is tried for every
set of the types present. A set must explain the block's length in every file
of the snapshot, files without the block included (Gadget writes no empty
block, so such a file must have none of the set's particles), with one
element size and at most `MAX_DIMS` (32) values per particle. If exactly one
reading fits, it is taken and flagged (`blocks[name].inferred`). If none
fits, or several do, the block is left out: `skipped_blocks` lists it and
`skipped_reasons` says why (`ambiguous: gas (2 values), gas+dark matter (1
value)`). The classifier's message names it too. Equal gas and dark-matter
counts in one file, or two black holes whose count divides almost any size,
make several readings fit; files with different mixtures of types usually
leave one. A missing column is preferred to a wrong one. The element type
comes from the positions (float or double) and from the block: IDs and the
other integer blocks (`INT_BLOCKS`) are unsigned 4- or 8-byte integers. A
block with several values per particle becomes several columns.

**Large blocks.** Record markers are read as unsigned 32-bit integers, so a
block of 2 to 4 GiB reads as written. Past 4 GiB the marker wraps. The true
length is then the marker plus a whole number of 4 GiB whose closing marker
agrees and after which a record, or the end of the file, follows. Only one
such length may fit, else the file is refused. The size in a format-2 name
record wraps the same way. Initial conditions with `num_files` 0 are read
as one file, and NULs that pad a block name are stripped.

**Several files.** `snap_NNN.0 … .{num_files-1}` beside one another are read
as one snapshot. The per-type sum of each file's `npart` must equal the
header's `npartTotal` (with the high word), and a missing file is named. For
each block and type, the snapshot keeps the list of pieces (file, offset,
dtype, rows). A column is the concatenation of its pieces, in file order.

**In place.** Each piece is an `np.memmap` at its offset, so nothing is read
until it is used. A component keeps each block's arrays once built, so a
compressed HDF5 `Coordinates` is read once for x, y and z. For a single-file snapshot, a column is a view of the map,
with no copy, in the point table and in the catalogue's DataFrame. For a
multi-file snapshot, a column is joined into memory the first time it is read
(`LazyColumns`), so a renderer pays only for the three or four columns it
draws. `/v1/catalogue/open` builds the DataFrame of every column of the
particle type, and that does copy. The VR page's samples reuse this DataFrame
instead of joining the columns again. An HDF5 dataset that is contiguous and
unfiltered is memory-mapped at its file offset (`dataset.id.get_offset()`);
a chunked or compressed one is read.

Measured on this Mac with synthetic data: 16 M dark-matter and 4 M gas
particles (626 MiB). Scanning headers and markers takes 0.5 ms for one file
and 0.8 ms for four. Opening the point table takes under 1 ms. A 200 k
sample takes 60 ms from one file and 99 ms from four. The catalogue
DataFrame takes 1 ms from one file (views) and 157 ms from four (7 columns of
16 M joined, about 450 MB in memory). The gas temperature takes 7 to 21 ms.

## Units

Columns stay in code units. `UnitSystem` states them, in this order of
precedence:

1. Gadget's defaults: lengths 1 kpc, masses 10¹⁰ M☉, velocities 1 km/s,
   in units of 1/h;
2. the run's `parameters-usedvalues` (or `param.txt-usedvalues` …) beside the
   snapshot or one folder up: `UnitLength_in_cm`, `UnitMass_in_g`,
   `UnitVelocity_in_cm_per_s`, `ComovingIntegrationOn`;
3. an HDF5 file's own `Units` / `Parameters` / `Header` attributes of the
   same names;
4. `VISIVO_GADGET_UNITS` ([Configuration](../configuration.md)).

The metadata names the source that was used. Whether the run is comoving
comes from `ComovingIntegrationOn` when one of these sources gives it. When
none does, the header decides: a cosmological run has Omega0 > 0, a time
0 < a ≤ 1 and a redshift equal to 1/a − 1. An isolated galaxy's header has a
time (0 for its initial conditions, 2.5 Gyr/h later) and none of the rest, so
it is not comoving, and its lengths, densities and velocities are not scaled
by a. The scale factor a is the header's `time` for a comoving run (1 if it
is not positive), else 1. h is the header's, or 1 if it has none. The
snapshot cache and the product cache are keyed on the parameter file and on
`VISIVO_GADGET_UNITS` too, so a change to either takes effect.

The derived `temperature` and `entropy` columns are computed in float64 when
first read and kept in float32. For each column, the conversion to
physical units is a factor and a unit (`component.physical` in the open
response):

| Column | Code unit | Physical |
|---|---|---|
| `x`, `y`, `z`, `hsml` | L/h, comoving | × L/kpc · a/h → kpc |
| `vx`, `vy`, `vz` | V, with Gadget's internal √a | × V/(km/s) · √a → km/s, peculiar |
| `mass`, `bh_mass` | M/h | × M/M☉ / h → M☉ |
| `rho` | (M/h)/(L/h)³, comoving | × M/L³ · h²/a³ → g/cm³ |
| `u` | V² | × V² → erg/g |

## Derived gas columns

With γ = 5/3, X = 0.76 and the specific energy u in erg/g:

- mean molecular weight μ = 4 / (1 + 3X + 4X·NE) with `NE` (n_e / n_H, as
  Gadget's cooling writes it); without `NE`, a fully ionised gas,
  μ = 4 / (3 + 5X). The column's description states which one was used;
- temperature T = (γ − 1) μ m_p u / k_B;
- electron density n_e = NE · X ρ / m_p, or (1 + X)/2 · ρ / m_p when fully
  ionised, with ρ physical;
- entropy K = k_B T / n_e^(2/3), in keV cm².

When `flag_entropy_instead_u` is set, the `U` block holds the entropic
function A, and u = A ρ_phys^(γ−1) / (γ − 1) with ρ_phys = ρ/a³ in code units.

## Files read beyond the one named

The other files of a multi-file snapshot, the file a `snapdir` folder holds
and the run's parameter file are read too. `/v1/catalogue/open` and
`/v1/files/classify` check each of them against the backend's data roots,
resolving symbolic links, as they check the named file. A snapshot with one
file outside the roots is refused.

## Desktop splats and linked quantities

The local Gaussian renderer sizes a uniform splat from the mean 3-D spacing
`cuberoot(volume / rendered rows)`. Its initial normalized size is 0.75, so the
radius passed to VTK is `spacing × 0.75 / 8`; the UI can still move below or
above it. The opacity is `clamp(1.5 / depth, 0.005, 0.25)`, where `depth` is
the estimated number of mean spacings along the longest axis. Thus a typical
line of sight starts near a total optical contribution of 1.5 instead of the
former value 7. The same normalized size is sent to server-side rendering.

Quantity-comparison windows share one backend dataset. Each window has its own
link flag. A change of camera, selected source or representation is broadcast
only when both sender and receiver are linked. Representation has a second,
independent `Match rendering` facet that must also be enabled at both ends. It
comprises shape, the shared size slider, render budget and ray-traced-light
mode; the physical quantity, colormap, colour range and stretch are
deliberately excluded. On relink, the joining view first adopts the enabled
linked properties from a peer, so the group is coherent immediately and no
echo loop is produced. Both flags and each autonomous camera survive snapshot
replacement on the timeline.

## Series

`snapshot_series.py` lists the snapshots that share a name and differ only in
a number. A multi-file snapshot lives in a folder named with the same number
(`snapdir_010/snap_010.0`), so when that folder holds no other member of the
series, the series runs over the sibling folders, with the file renumbered
(`snapdir_011/snap_011.0`). `/v1/points/series` keeps the particle type, so
each member's path is a reference to the same type.

The desktop asks for this route after opening a particle table. More than one
member adds the Snapshot Timeline card. A time step constructs the replacement
dataset first, then builds every linked quantity view from that one shared
backend dataset and closes the old group. This avoids reopening a large
snapshot once per quantity and prevents a partially updated comparison group.
Camera and per-view visual encoding are restored; selection and explicit
numeric colour limits are not, because neither particle identity nor equal
distributions across outputs may be assumed. Playback starts the next step only
after the current replacement has completed, so there are no stale concurrent
opens to apply out of order.

## SubFind (issue #392)

The block parser is independent of the particle semantics. A SubFind group
file in format 2 (`groups_NNN/sub_NNN.0`) uses the same header, with the
groups counted as type 0 and the subhalos as type 1, and named blocks.
`open_snapshot` already reads it (see the synthetic one in the demo data),
so the group catalogue reader of #392 can reuse `scan_file` and
`infer_block`.

## What to check against a real snapshot

- The block list: unknown names should be read as their own columns, and
  `skipped_blocks` should be empty. If not, add the block to `BLOCKS` with its
  types, or to `INT_BLOCKS` if it holds integers.
- The type mask of blocks with several elements (`Z` with several metals,
  `iM`, the BH blocks of the run's code version).
- Units: the run's `parameters-usedvalues`, or the values to set in
  `VISIVO_GADGET_UNITS`; check a halo's position and mass against the
  group's own catalogue.
- Temperature and entropy against the group's maps at the same redshift
  (the validation planned for #393).
