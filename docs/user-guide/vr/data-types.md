# Point tables, catalogues and velocity fields

The same page shows the other data types the desktop opens
([#190](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/190)), with
the same rule: the headset draws, the backend computes. The routes are in
[Points, catalogues and flows for the headset](../../backend-api/vr-layers.md).

| Data | What the headset draws | What the backend does | Refined when still |
|---|---|---|---|
| **Point table** (VBT `.bin.head`, a 3-D catalogue, a simulation snapshot) | an even sample of its rows within a budget, as points or additive splats, coloured by a column through the page's colour maps | samples the table, and its point render session draws **every row** for the headset's eyes | yes (the point session's stereo frames) |
| **Catalogue over a cube** (VLKB compact sources or filaments, a cross-match, a catalogue file) | markers, ellipses and contours in the cube's box | places each source in the cube's pixels with the file's full WCS | with the cube |
| **Velocity field** | its streamlines and arrows as lines, coloured by the speed | traces them on the desktop's display grid with the desktop's own filters | no |

## Opening one

From the desktop: *Tools → Open in VR Headset…* in a point
table's or 3-D catalogue's window, or the *Open in VR Headset…* button under
the velocity viewer's controls; the link then carries `kind=points` or
`kind=velocity` (and a velocity field's box size, `box=`), and the page opens
the desktop's dataset in the same session, or its path again when the session
no longer holds it, as for a cube (see
[Open a cube from the desktop](quickstart.md#open-a-cube-from-the-desktop)).
On the page: type the path, choose
**Data kind** (*auto* goes by the name: `.head`, `.bin`, `.csv`, `.xml`… are
point tables, a FITS whose name says `velocity` or `flow` a velocity field),
give a velocity field's **Box (Mpc)** if it has one, and press *Open and
load*. The page's cube controls are hidden meanwhile.

In the headset a point table or a velocity field replaces the cube, so its
controls are on the same tabs of the menu as a cube's
([#257](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/257); see
[The hand menu](controls.md#the-hand-menu)). On **Display**, for a point
table: *Colour by* (a chip per column), *Log* and the colour window, the
colour map, *Points* or *Smooth (splats)*, *Size −/+*, *Intensity*,
*Particles shown* (the next budget of rows), *Again* (sample again), *Box*
(the box and its ticks) and *Data ▸* (the snapshots, the X/Y/Z columns and the
splat sizes, below);
for a velocity field: *Streamlines*, *Arrows*, the arrow length, the seed
density (*Fewer*, *More*) and *Reseed streamlines*, the colour window over the
speed, the colour map, *Box and ticks* and the box size (read-only). On
**Range**, both have the spatial crop (*Crop x/y/z*, *Reset crop*): the points
and lines outside it are not drawn. A catalogue over a cube is a toggle on
Display (*Overlay*, *Labels*, *Through velocity*). On a point table Select
has the *Box select* (below); Measure is greyed on point tables and velocity
fields, and Select on velocity fields, with the reason under each control.

### Simulation controls (#259)

- **Snapshots.** *Data ▸ → Snapshot*: *◂ Previous* and *Next ▸* open the file
  before or after this one among those beside it whose names differ only in
  their number (`run_001.bin`, `run_002.bin`, ...; `/v1/points/series`). The
  next snapshot keeps the X/Y/Z columns, the colour column, the splat sizes,
  the crop and the box selection where it has those columns, and the view
  stays where it is. There is no time-series data model: a snapshot is a file
  opened as any table, so a step takes as long as opening that file (the
  sample is fetched again).
- **X, Y, Z.** *Data ▸ → X / Y / Z*: *◂* and *▸* (or the column's name) step
  through the table's columns for that axis; *Default axes* goes back to the
  table's own. A new column makes a new box: the crop and the box selection
  start again.
- **Splat sizes.** *Data ▸ → Splat size*: *Uniform* (one size from the mean
  spacing of the rows, as before) or *By hsml* (a smoothing-length column,
  found by its usual names: `h`, `hsml`, `sml`, `SmoothingLength`,
  `smoothing_length`; *Column ▸* picks another). Each splat is then a Gaussian
  of σ = h / √2 (the ray-traced renderer's kernel, exp(−d²/h²)) times the size
  step, larger in proportion when only a sample is drawn, and keeps its light
  (a splat twice as wide is a quarter as bright, within ×16). Splats only. The
  backend's refined frames draw one size, so they are off while the splats
  follow a column; the headset's own picture stays.
- **Units on the colour bar.** A table coloured by a column, or a velocity
  field, shows a colour bar beside the box's lower corner: the colour map over
  the colour window, its ticks (decades on a log scale) and the column with
  its unit (`mass [Msun]`) when the file declares it (a catalogue's VOTable,
  FITS or IPAC units, or a unit in the column's name, `T (K)`); a VBT has no
  units in its header, which the bar says. *Box* hides it with the box.
- **Box select.** Select tab → *Box select*: drag with the trigger across the
  particles to draw a box (as a new crop is drawn: the two corners across the
  axes facing the ray, the crop's depth kept; with the mouse, Shift and drag);
  its dots move a face, its inside moves it. Then *Show inside*, *Hide inside*
  or *Show all*; *Clear* drops it, *Undo* takes back each press. The box is a
  test in the headset's own point shader (the sample is not fetched again) and
  goes to the backend's refined frames as a filter, in the table's units
  (`filters` with a `box`).

## Point tables

The table is sampled evenly over its rows (the point session's own rule), so a
snapshot stored region by region is sampled everywhere, and each row costs 8
bytes: positions and colour quantised to 16 bits over the sample's box and the
colour range. **Rows drawn** chooses the budget (*quest-low* 0.5 M, *quest*
1 M, the default, *quest-high* 2 M, *desktop* 5 M, *desktop-high* 10 M, the
most a request may ask for). **Hold the frame rate** (on by default) draws
fewer of the sampled rows while a frame would be late, and more again when
there is room: the rows arrive in a low-discrepancy order (van der Corput), so
the rows drawn are spread over the whole sample, never a corner of it (no gap
wider than twice the even spacing). A filter set on the table runs before the
sample. Choosing another budget or column while a sample is still coming
cancels it: only the last one asked for is fetched and drawn. The HUD says *points X M drawn of
Y M sampled, Z M rows*.

*Colour by* a column (the backend puts a positive column spanning more than
three decades on a log scale the first time, as a mass or a density needs,
in the same request; untick *log* to undo), *Colour from / to* narrow the map, *Style* is *points* (pixels)
or *splats* (sized like the desktop's: the mean spacing of the table's rows
times the size step over 8, larger in proportion when only a sample is drawn).
The box is the sample's bounding box, its longest side 1 (scaled to 0.6 m in
the headset), with ticks in the table's units.

When you hold still, the backend's point render session draws every row of the
table for your eyes and the page blends that frame in, as for a cube (see
[Refinement](reference.md#refinement-full-quality-frames-from-the-backend)): the
page sends its colours, style and the map from the table to its box
(`data_to_model`) with each pose, so the frame lands on the page's points.
Both read the colour column in full precision and give each row the same
colour, on a linear or a log scale, so the refined frame does not change the
colours; values far from zero keep their own colours there too (1e9, 1e9 + 1
and 1e9 + 2 are three colours). With *splats* the backend draws them by the
page's own rule: the splat size the page uses for its sample, each row's light
weighted so the whole table shows the sample's brightness, and the same floor
that keeps splats smaller than a pixel visible. The refined frame then looks
like the page's picture with the sample's graininess gone, at any refined
frame size (it is drawn at the page's own eye size and then reduced). While
the frame-time governor draws only part of the sample (the HUD's *drawn of
... sampled*), the page's own picture is dimmer than the sample's and the
refined frame brightens it slightly.

## Catalogues over a cube

Under **Catalogue over the cube**, choose the sources: a *catalogue file on
the backend* (CSV, VOTable, FITS table, IPAC: RA/Dec or GLON/GLAT columns),
*VLKB compact sources* (a band, band-merged by default) or *VLKB filaments*
over the cube's footprint (with the desktop's margin; across l = 0 too, as
at the Galactic Centre), or a *cross-match* (SIMBAD, NVSS, FIRST, 2MASS) of
the cube's field. *Place on the cube* asks the backend to put them in the
cube's pixels with the file's full WCS, so a Galactic CAR cube (Hi-GAL,
CHIMPS cutouts) and an equatorial TAN one are both projected correctly, and a
catalogue in another frame is converted first.

A source with a spectral column (`freq`, `v_rad`, `v_opt`, `v_app`, `vlsr`…,
in its own unit when the file or the column name says it; SoFiA's `v_app`
without a unit in the cube axis's unit; a `velocity` of no stated convention
in the axis's own, with a warning) is a cross at its voxel; a source whose
channel is outside the cube is left out and counted (a wide-band survey over a
narrow-band cube keeps the sources it shares). Only when none falls in the
channels, and the column declares no unit, is a reading a thousand times
larger or smaller tried (m/s against km/s); when it places most of them it is
applied and named in a warning. A convention is never changed. Otherwise all
are placed on the sky only. Every warning is shown under *Place on the cube*
and, in the headset, on the menu's Display tab under *Catalogue* (the first
one, in amber);
the status line also warns when the
cube's or the catalogue's spectral frame calls for a frame change, which is
not applied. A source without a spectral column is
drawn on the back face of the clipping box with a faint line through every
channel (*through every channel* turns those lines off). VLKB ellipses are
built as on the desktop (FWHM in pixels, major axis along +x turned by the
PA), each band in the desktop's colour; filaments lie on the back face. The
status line says how many sources were placed, how many fell outside the
cube, and how they were placed in velocity.

## Velocity fields

The backend traces the streamlines and picks the arrows on the grid the
desktop's velocity viewer shows (block-averaged to 128 per side), with the
same filters and settings: 200 seeds in a ball at the centre of 0.3 times the
longest side, Runge-Kutta 4-5 both ways, 2000 steps at most; at most 50 000
arrows by uniform spatial sampling, each `3 × cell / speed max` long per unit
of speed. The seeds are random, as on the desktop, from a fixed sequence:
*Trace again* (or the menu's Display tab → *Reseed streamlines* in the headset) draws
another set. The lines are
coloured by the speed over the full-resolution range through the page's
colour map. The lines are traced with the box the desktop's viewer shows (its
*Box size*, carried by the link as `box=`; 0 is grid units), else the one the
field was opened with; the axes say which (Mpc or grid units), from what the
backend reports. The page draws lines one pixel wide where the desktop
draws tubes.

The frame rates, request times and placement accuracy of all three were
measured on the Mac: see
[Points, catalogues, flows](reference.md#points-catalogues-flows); the checks
to run with the headset are under
[Points, catalogues, flows, with the headset](reference.md#points-catalogues-flows-with-the-headset).
