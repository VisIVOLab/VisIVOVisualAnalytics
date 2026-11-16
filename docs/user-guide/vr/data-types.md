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

## Simulation volumes

A gridded field of a simulation (a VBT volume's field,
`<data>.bin::vbt-field=N`; a PLUTO variable, `<data>::pluto-var=<name>`; a
FITS cube whose axes are neither spectral nor on the sky) opens as a cube,
whatever its file is called
([#317](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/317)). The
backend says it is a *simulation volume* and the page treats it as one:

- **Log scale.** A field whose values are all positive and span more than
  three decades (a density from 1.8e-12 to 2.1e-5 g/cm³) is sent as codes of
  log10 of its values, so every decade keeps its share of the 8-bit codes; a
  linear scale would put all but the densest voxels in the first code, and no
  stretch on the headset brings them back. Values at or below zero have no
  logarithm and are blank. A signed field (a velocity component) stays
  linear, and so does every cube that is not a simulation volume (radio,
  masked or moment-derived) unless *log* is chosen. *Scale* on the page (*auto*, *log*,
  *linear*) overrides the choice. The threshold, the colour max, the
  readout, the info line and the colour bar beside the box always show
  values, never their logarithms. A log-scaled field starts with the
  *Linear* stretch, linear in log10 of the values, which is the desktop's
  *Log* (the desktop is told so).
- **The box is drawn to the grid spacing**: a grid of 64 × 48 × 24 cells of
  1 × 1 × 2 kpc is drawn 1 : 0.75 : 0.75, not by its cell counts. The axes
  are ticked in their own units (X, Y, Z, in scientific notation for lengths
  in cm). The full-quality frames from the backend use the same box. When the
  axes' units differ or a spacing is missing the box is drawn in cells, and
  the scale line says so.
- **No spectral wording or tools**: no velocity or spectral range (*Range*
  crops z as it crops x and y), no velocity stretch, no noise σ (*Display*
  offers the 90th and 99th percentiles where a cube has 3σ and 5σ), no beam
  or rest line, and the Kinematic Lasso, the Spectrum probe, moments and PV
  slices are greyed with the reason. The *Box region* gives the statistics of
  a box. The default threshold is the 99th percentile of the values, not the
  noise rule. The readout gives X, Y, Z in the axes' unit, the cell, and the
  value.

- **The variable picker.** When the file holds more than one field (a VBT
  volume's fields, a PLUTO output's variables present on disk), the last row
  of the hand menu's *Display* page is *Variable*: one chip per variable,
  the one on show lit. With more than five, four chips are shown (the page
  holding the variable on show) and *next ▸* steps to the next variable. A
  PLUTO output copied one variable at a time (only `rho` on disk) has one
  field, and the row is the catalogue's as on any cube. A press loads that
  variable at the same binning. In VR it is written into the room kept for
  cubes (no new 3-D texture during the session) and shown at once. The crop
  stays (the same grid). Each variable keeps its own look, its colour map,
  stretch, scale (log, linear), threshold and colour max, and gets it back
  when it is shown again. A variable shown for the first time starts from the
  backend's choices for it: a density on log, a velocity component linear.
  The desktop is told (*Headset Activity*: *Variable vx1 of rho.0000.flt*).
  A cube window of the variable left behind, with *Follow the headset's
  look* on, brings forward the window of the new variable when one is open,
  else opens its *Field* product.

- **Two or three variables overlaid.** With two variables or more, the
  *Rendering* row of *Display* has *Overlay ▸*. Its page overlays two or
  three variables of the same grid in one view, for example a density with a
  temperature, or the abundances of O, Si and Fe:
  - *Fields*: a press adds a variable or takes it out. Its number is its
    place (1, 2, 3) and its colour bar's.
  - *Edit*: the field the rows below set. Each field keeps its own colour
    map (red, green and blue to start with, as a three-colour composite),
    *Log* (its scale: a press loads it again on the other scale), threshold,
    colour max and opacity. *Shown*/*Hidden* leaves a field out without
    loading again.
  - *Sum* (the default): the fields emit and absorb together, each with its
    opacity; where two overlap, their colours mix in proportion to their
    opacity there. *MIP*: along each ray, the brightest field value in its
    own colour window, in that field's colours.
  - *On*/*Off*: the overlay, or the single variable back.

  One colour bar per field stands beside the box, numbered as on the page.
  The readout gives each field's value by name. While the overlay is on, the
  single variable's look on *Display* is greyed, the refined frames from the
  desktop are not shown (they hold one variable), and the crop is drawn at
  the cube's own binning (no detail). The fields are loaded at the cube's
  binning. In VR they are written into room kept for them before the
  session, so no 3-D texture is allocated during it. That room exists only
  when it fits the device's share beside the cube's; otherwise the page says
  how much it needs. Picking a variable on *Display* turns the overlay off.
  The desktop is told (*Headset Activity*: *Overlay (emission): rho red log,
  T green log, vx blue*).

- **The model taken apart.** On a simulation volume the *Rendering* row of
  *Display* also has *Explode ▸*. Its page pulls the model apart into
  pieces, to look at each one and put them back:
  - *Pieces*: *Fields* (the overlaid fields, each in its own colours; the
    overlay must be on) or *Value ranges* (the variable on show, or the
    field edited on the *Overlay* page, split into 2, 3 or 4 ranges of equal
    width between its threshold and its colour max: on a log scale, equal in
    decades). Each range has its own colour (blue, green, orange, red from
    the lowest) over its own values; the top one keeps everything above the
    colour max.
  - *Layout*: *Row* (side by side), *Column* (one above the other) or
    *Ring*; *Spread*: the gap between pieces. The pieces are scaled down so
    that together they cover no more than the cube alone did.
  - *Shown*: one chip a piece; a press hides it or shows it again. A
    field's chip is the same switch as its *Shown* on the *Overlay* page.
  - *Explode* / *Assemble* animate between the two: the model first shrinks
    to the pieces' size, then the pieces part (and the reverse). *Reset*
    puts every piece back in its place in the layout and shows them all.

  With the pieces apart, a grip that starts inside a piece moves and turns
  that piece; a grip elsewhere moves the whole model, pieces included. One
  colour bar per piece stands beside the box. While the model is apart the
  box's outline, ticks and labels, the refined frames and the crop's detail
  are not shown, and the trigger still picks on the assembled box. With
  *Fields*, switching the overlay off puts the model back together.

## Opening one

From the desktop: *Tools → Open in VR Headset…* in a point
table's or 3-D catalogue's window, or the *Open in VR Headset…* button under
the velocity viewer's controls; the link then carries `kind=points` or
`kind=velocity` (and a velocity field's box size, `box=`), and the page opens
the desktop's dataset in the same session, or its path again when the session
no longer holds it, as for a cube (see
[Open a cube from the desktop](quickstart.md#open-a-cube-from-the-desktop)).

A sky catalogue (RA/Dec with a redshift or a distance) is placed as the
desktop window places it: the link, the pairing code, the app launch and the
desktop's *Show on headset* carry the window's frame (FK5, galactic,
supergalactic), distance mode (comoving or redshift space) and named
cosmology, and the backend computes the positions with them. A **Custom**
cosmology is not sent: the backend knows Planck18, Planck15, Planck13 and
WMAP9 only, so opening the headset puts the window back on Planck18 and says
so, as the ray-traced view does. Sources with no distance (no usable
distance value and a redshift ≤ 0 or missing) are left out, as on the
desktop. The page's status line names the placement and counts them, for
example *supergalactic · comoving · Planck18 · 26 unplaced*. A catalogue with
no redshift and no distance column (positions only) is drawn on the
celestial sphere, and the line reads *fk5 · celestial sphere (no distances)*. Changing the placement
in the headset comes later.
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
colour map, *Points*, *Smooth (splats)* or *Ray-traced*, *Size −/+*, *Intensity*,
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
  stays where it is. The crop and the box keep their place in the table's
  units (the same region of space), not their fraction of a box whose bounds
  changed. A crop wholly outside the new snapshot's box draws nothing and the
  menu says so (*Reset crop* shows it all); with the *Spatial crop* tool its
  box is the table's outline in red, with no dots, and a drag draws a new
  crop; a box wholly outside it shows
  nothing with *Show inside* and everything with *Hide inside*, also said;
  neither is replaced by the whole table, and both come back on a snapshot
  that holds them. *None* (one colour) stays one colour. Quick presses take
  the crop and the box from the snapshot on show when they began. There
  is no time-series data model: a snapshot is a file opened as any table, so
  a step takes as long as opening that file (the sample is fetched again).
  Quick presses go on from the step under way (the last one wins; one
  overtaken is not shown), a press made before the series is known waits for
  it, and a cube, a field or another table opened meanwhile cancels the step. The snapshot left behind is closed on the backend when the page
  opened it itself; the table the desktop sent is never closed by the page.
- **X, Y, Z.** *Data ▸ → X / Y / Z*: *◂* and *▸* (or the column's name) step
  through the table's columns for that axis; *Default axes* goes back to the
  table's own. A new column makes a new box: the crop and the box selection
  start again.
- **Splat sizes.** *Data ▸ → Splat size*: *Uniform* (one size from the mean
  spacing of the rows, as before) or *By hsml* (a smoothing-length column,
  found by its usual names: `h`, `hsml`, `sml`, `SmoothingLength`,
  `smoothing_length`; *Column ▸* picks another). Each splat is then a Gaussian
  of σ = h / √2 (the ray-traced renderer's kernel, exp(−d²/h²)) times the size
  step, larger in proportion when only a sample is drawn, and gives the light
  of the uniform splat: its sprite's area as drawn (clamped to the sprite
  sizes the GPU draws, and the sub-pixel rule) over the uniform one's, so a
  splat twice as wide is a quarter as bright and a clamped sprite keeps its
  light too. A column with no positive value gives uniform splats and a note
  under the row, and *Column ▸* passes over it. Splats only. The
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

Splats on a headset
([#295](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/295)):

- **Why splats are expensive.** They are bound by fill rate. On a Quest 3 at size 1, about 200 000 hold 72 Hz on a half-float target and about 50 000 on float32, where points hold a million.
- **Defaults.** A headset adds its splats into half floats. The refined frame of a still view brings back the faint light that half floats drop. The splats start from 200 000 of the sample. The sample is still the budget's, used for points and for refinement.
- **The governor works without a GPU timer.** The Quest Browser has no GPU timer, so in a VR session the rows drawn now follow the frame intervals, once the display rate has settled: missed frames, or a median interval above 1.12 display periods, cut them at once. Clean frames add 12 % a quarter second, up to just under the count that missed. Frames drawn under a refined frame earn no clean time. A desktop browser without a GPU timer keeps the count, because its display rate is not known.
- **Rows move in half-octave steps** (…, 185 k, 262 k, 370 k, …), with hysteresis, and are drawn for the same rows as the refined frames, so a step makes the frame on show stale. While a still view is refined the rows are cut at once when frames are late, but never probed above the count that last missed, and they do not grow while a refined frame is on its way or fading in. If a frame shown in full does not cover the view (a particle close to the eye, an edge after a turn), the rows may rise one step once it has been shown for 5 s. The step drops that frame and the 5 s start again, so the rows recover from a cut in a few steps and the refined frame does not flicker.
- **Still views cost nothing.** Splat refined frames are asked for with a 6 % margin on every side, about 3° on a Quest 3. While the frame on show, warped to the current pose, covers the whole view, the splats under it are not drawn. A turn or step that would uncover an edge brings them back. A frame with no particle in its view (an empty depth image) is warped at the plane through the box's centre, and is tested at that plane alone.
- **To measure another target**, add `?splattarget=f32`, `f16` or `8bit` to the page address.

*Colour by* a column (the backend puts a positive column spanning more than
three decades on a log scale the first time, as a mass or a density needs,
in the same request; untick *log* to undo), *Colour from / to* narrow the map, *Style* is *points* (pixels),
*splats* (sized like the desktop's: the mean spacing of the table's rows
times the size step over 8, larger in proportion when only a sample is drawn)
or *ray-traced* (the same splats on the page; the still view ray-traced by the
backend, see below). A VBT point table (a simulation's particles) opens
*ray-traced*, a catalogue as *points* (its rows are objects, not a medium),
until you choose a style yourself.

The splats' look ([#263](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/263)):

- **Tone mapping.** The splats are added in floating point and tone-mapped onto the view. Dense halos keep their colour and their structure instead of burning out to white, and faint splats are no longer rounded away one 8-bit addition at a time. The refined frames use the same curve.
- **Size without brightness.** *Size −/+* changes the splats' size, not their brightness: each splat's opacity is divided by the size step squared, so size 4 shows the same light as size 1, spread wider.
- **Close splats.** Splats near your head keep growing up to 256 pixels instead of stopping at 64 when you fly inside the box.
- **Default colour.** A table opens coloured by a density-like column (`*dens*`, `*rho*`, `LogD`) when it has one. Otherwise it uses the first column that is not a position, velocity, sky coordinate or identifier (a catalogue's redshift rather than its RA).
- **Axis units.** A sky catalogue's box is labelled in Mpc (*x (Mpc)* instead of `@x`).
- **Older headsets.** On a headset that cannot render into a float target, the HUD's points line says *8-bit* and splats are added as before, with the brightness growing with the size (divided by the size, large faint splats would round away to nothing in 8 bits).
- **Page and backend versions.** The backend draws refined frames by the page's own splat rule and says so in each frame. A backend older than the page draws the old look: its splat frames are not shown (the panel says *backend draws splats by an older rule*), so nothing pops in at another brightness; update the backend.
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

With *ray-traced* the still frame comes from the backend's ray-traced particle
renderer (the one behind the desktop's *Ray-traced* shape, see
[Ray-traced particles](../raytraced-particles.md)) over every row the session
holds, [#264](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/264).
Each splat becomes a smooth Gaussian kernel carrying the light the page's splat
carries, so the swap from the preview to the refined frame keeps the
brightness and the colours (measured on a 3.9 M-row snapshot: the refined frame
adds the same light over the background as the *splats* frame, within 0.1 %).
The renderer also sends the depth the light of each block of pixels comes from
(its light-weighted mean distance), so the frame is reprojected through that
depth like a cube's, instead of as a plane: it stays on show while the head
moves up to the full refinement limit rather than 1 cm, and a frame warped to a
pose 1 to 4 degrees around the box is 2.7 to 3.7 dB closer to that pose's own
frame than the plane gives. On an M-series Mac it costs about 40 ms more per
refined frame than *splats* (about 60 ms per 1600 × 1000 eye for 3.9 M rows;
from the camera stopping to the refined frame shown, about 385 ms against
335 ms; 535 ms for 11.5 M rows).

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
drawn on the back face of the spatial crop with a faint line through every
channel (*through every channel* turns those lines off). VLKB ellipses are
built as on the desktop (FWHM in pixels, major axis along +x turned by the
PA), each band in the desktop's colour; filaments lie on the back face. The
status line says how many sources were placed, how many fell outside the
cube, and how they were placed in velocity.

### The source list: browse, visit, classify

In the headset, **Select ▸ Sources ▸ List** opens the *Sources* window: the
sources of the catalogue over the cube, ten a page, with their name, sky
position (RA/Dec, or l/b for a Galactic catalogue such as the VLKB's) and
velocity (the cube's velocity at the source's channel, as the readout gives
it; a dash for a source placed on the sky only). Point at a column heading
and pull the trigger to sort by it (again: the other way); *◂ Page* and
*Page ▸* page a large catalogue.

- **Visit**: a row pressed (or *Visit*) makes that source the current one,
  drawn with a white bracket, and brings it to the centre of the view: the
  cube is moved and scaled so the source sits about 0.55 m in front of you
  and its region (2.5 times its ellipse, or 8 % of the field) is about 25 cm
  across; the cube keeps its orientation. A source outside the velocity range
  shown moves the range to hold it (its width kept; Undo takes it back). On
  the page the camera turns about the source.
- **◂ Prev / Next ▸** (in the window or on the Select tab) visit the previous
  or next source of the sorted list.
- **Classify**: the flag buttons (*real*, *doubtful*, *spurious* by default,
  each with its colour, which the source's marker then takes) and the note
  presets (*blended*, *edge*, *RFI*, *extended*, *faint*, *check*) mark the
  current source; pressing the one it has takes it back. Both sets are set on
  the page under *Catalogue over the cube* (*Flags*, *Notes*, comma-separated,
  at most 4 flags and 6 notes of 12 characters each); they apply to the next
  catalogue placed.
- **Found by eye**: select an object the catalogue does not hold with the
  Kinematic Lasso (or point the Spectrum probe at it), then **Select ▸
  Sources ▸ + Found**: it is added to the list as *VR-1*, *VR-2*… (at the
  lasso's brightest voxel, or the probe's voxel; its ICRS position from the
  cube's full WCS), drawn as a cyan diamond, and can be flagged like the
  others; *Remove* takes a found source out.
- **Send** writes the classification as a VOTable in the Workspace Exports
  (every original column and row of the catalogue, the flag column `vr_flag`,
  `vr_note`, `vr_origin`, the pixel and the velocity, the found sources
  appended) and reports it to the desktop as headset activity (see
  [The headset from the desktop](desktop.md#headset-activity)). Each Send
  writes a new file (`<catalogue>_vr_classified.xml`, then `_1`, `_2`…). The
  window's line says *sending* until the file is written and the desktop
  told, then *sent* with the file's name (or why it was not, in red), and
  *changed since sent* when there is more to send. When the file was written
  but the desktop could not be told (a backend older than the headset
  activity, the page stood down meanwhile) it says *Saved as <file>, desktop
  not told* and why, in amber: the VOTable is in the Exports, and Send again
  (nothing changed) only tells the desktop about that file, without writing
  another. A VOTable keeps the
  catalogue's units (an RA in hours stays in hours, found sources written in
  it). For an overlay with no catalogue file behind it (VLKB sources, a
  cross-match) every source goes, flagged or not.

The flags, notes and found sources are kept in this browser per cube (its
full path) and catalogue (its sources: their number, names and positions), so
a reload (or a restarted headset browser) finds them again; a catalogue of
the same name with other sources starts with none, and only the marks of
sources listed now come back.
With a large catalogue the markers drawn are the 3 000 nearest the current
source and the labels the 40 nearest.

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
