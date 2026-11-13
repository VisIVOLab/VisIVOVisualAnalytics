# Analysis in the headset

Everything on this page is computed by the backend on the full cube, through
the routes the desktop uses; the headset only draws. The tools are chosen and
used as described in [Controls and the hand menu](controls.md): the selection
tools in the Select mode (see [Modes](controls.md#modes)), the rest on the
menu's Analysis tab (see [The hand menu](controls.md#the-hand-menu)).

## Selections in the headset

The primary controller casts a ray (a thin white line); a small white cross
shows where the trigger would land. On the Mac the same tools work with the
mouse: a click (press and release without dragging) is the trigger, a drag
still turns the cube. The tool is chosen in the Select mode (spectrum, lasso,
box; see [Modes](controls.md#modes)) or on the page (*Selection tool*).

**Where a pick lands.** The ray is marched through the reduced cube the
headset draws, with the same values, threshold and clipping box as the
renderer: in *emission* mode the pick is the first sample the renderer draws
(the first at or above the threshold); in *maximum intensity* mode it is the
brightest sample of the ray, which is what that mode shows. From there the
seed is, by default, the brightest sample of the object the ray entered (the
run of samples above the threshold that starts at the first one, not an
object behind it), then the brightest full-resolution voxel of the reduced
voxel it is in (for *max* binning, the voxel the drawn value came from). The
page offers *Seed at: first drawn sample* and can turn off *brightest voxel of
its block*. The reduced voxel is mapped to the full cube through its block (a
partial last block covers only the pixels it holds), so the seed is always a
pixel and channel of the full cube. When a reduced voxel stands for more than
27 voxels (for example 8 × 8 × 4), the seed is the voxel with the brightest
3 × 3 spatial mean of its block instead, so a lone noise spike at the block's
edge cannot win; the results window says which rule was used (*its brightest*
or *its brightest 3×3 mean*).

The results window (in the headset; it can be grabbed by its title bar, moved
and closed, see [Controls](controls.md); under the page's controls on the Mac)
shows the seed's position: right ascension and declination in ICRS from the
file's full WCS (`/v1/datasets/pixel_to_sky`, what the desktop sends over
SAMP), the velocity of its channel in km/s with its convention, the pixel and
channel, and the reduced voxel.

| Tool | Trigger | What the backend computes | Results window |
|---|---|---|---|
| Spectrum probe | once, on the emission | the spectrum along the line of sight at the seed's pixel (`/v1/spectral/probe`, the desktop's probe) | the spectrum against velocity, the seed's channel marked |
| Kinematic Lasso | once, on a source | the selection grown from the seed on the full cube (`/v1/cube/kinematic_select_bin`, with the desktop's parameters), then its statistics (`/v1/cube/mask_stats_bin`) | voxels selected, flux in Jy km/s, sum, velocity and channel range, peak, the selection's integrated spectrum |
| Box region | twice, one per corner | the statistics of every voxel of the box, in one call (`/v1/cube/mask_stats_bin` with `box=true`) | voxels, blanks, mean, rms, min, max, sum, flux in Jy km/s |

The lasso's mask comes back at full resolution and is drawn in **green** on the
reduced cube: a drawn voxel is green when any full-resolution voxel of its
block is selected, so nothing selected disappears, and selected voxels below
the display threshold (the lasso grows into the faint margin) are drawn too;
its bounding box is outlined. The lasso's parameters are the desktop's
defaults (reach 8, velocity coupling 1, threshold 2.5 σ, the noise measured by
the backend, the Reach slider's level-field budget 64, no smoothing) and can be
changed on the page (*Kinematic Lasso parameters*). The same seed and
parameters give the same mask as the desktop, voxel for voxel (see
[Selections and analysis](reference.md#selections-and-analysis)).

A box corner is placed at the primary controller's tip when the tip is inside
the cube (reach into it): two such corners make a 3-D box. A corner placed by
pointing from outside the cube, or with the mouse, takes the right ascension
and declination of the point the ray picks (the first drawn sample, or the
middle of the ray inside the box when nothing is drawn) and no depth; the box
then spans the channels of the velocity slab (the clipping range on the
spectral axis, set with the Clip mode's slab, see [Modes](controls.md#modes)).
The sky rectangle is therefore exactly the one you
pointed at only when you look down the spectral axis; from an oblique view the
two picked points may lie at other depths than you meant. The box measured is
outlined in pink after the answer, and the results window gives its pixel and
channel range: check it there. The box is measured in one call on the full
cube (every voxel, read one channel at a time): 22 ms for a 66 × 29 × 158 box
of HCG 44. The rms is the population standard deviation, as the region
statistics give it; no median; the flux is each channel's sum over its own
beam area times its own width.

The lasso's green overlay stays while the other tools are used (the results
window says so); *Clear*, a new lasso or a new cube removes it. Switching tool
drops a request still in flight: it neither draws nor records.

**Recipes.** Each spectrum, lasso selection and box is recorded as the desktop
records its products (`visivo-recipe/1`, see
[Kinematic Lasso](../kinematic-lasso.md)): the dataset's path, the calls with their
requests under the API's field names (for the lasso, the grow with exactly the
request the desktop's recipe holds, then the statistics of the mask it
returns), the parameters as the desktop's Provenance tab labels them, and where
it was measured. *Download recipes (.json)* saves all of them, *Copy last
recipe* copies the latest. A replay opens the path and posts the same
requests: the selection is the one the desktop shows for that seed.

### Refining the selection: add, carve, Isolate, Remove

The lasso's seed mode (*new*, *add*, *carve*: the menu's Analysis tab → *Lasso
seeds*, or the page's *Lasso seed*) decides what the next lasso trigger does,
as the desktop's plain, Shift and Alt double-clicks do: *new* starts a new
selection, *add* grows a second source into the selection (union), *carve*
removes the region the grow from that seed reaches; the primary seed stays.
Each press is one grow on the full cube
(`/v1/cube/kinematic_select_bin` with `add_seeds` and `sub_seeds`, the
desktop's request), so the selection is always the one its seeds and
parameters make; the extra seeds are drawn as green (added) and red (carved)
crosses, and the results window counts them.

*Isolate* (a new cube with everything outside the selection blanked) and
*Remove* (the selection blanked), on the menu's Analysis tab → *Selection* or
on the page, post the mask shown, exactly (not a re-grow), to
`/v1/cube/mask_apply_bin` with `blank_outside` or `blank_inside`, as the
desktop's buttons do: the new cube goes to the Workspace Exports with the
selection's recipe in its FITS HISTORY, and is registered in the session.
*Show that cube* loads it in the headset in place of the cube shown; in VR
the new cube is read at once but shown only when you leave VR (a notice says
*The new cube is shown when you leave VR*; the cube on show stays until
then), because the headset's browser cannot take a new cube's textures
during a VR session (see
[GPU context loss](troubleshooting.md#gpu-context-loss-at-the-first-lasso-on-a-quest-3)).
The same holds for any cube loaded while you are in VR. Each
mode makes one cube per selection: pressing *Isolate* or *Remove* again, or
both at once, reuses the cube already made (or being made), and a moment uses
the cube *Isolate* made for the selection. The seeds are read from the
selection's own grow request, so a selection that arrives from elsewhere (a
shared one) continues with its own seeds. A selection is applied with its own
mask whenever it has one (even beside a grow request: a shared mask is
authoritative, and may have been edited by hand); one that arrives with only
its grow is grown again
from its request, and applied only if the grow gives back its voxel count,
box origin and size (else the page refuses and says what differs); one edited
by hand and arriving without its mask is refused (its grow is not it: apply it
on the desktop). Isolate, Remove and a moment belong to the cube they started
on: if another cube is loaded while they run, nothing is applied to it (a
cube already made from the previous one is deleted if temporary, or kept and
not recorded), and the menu's Analysis tab says so, even after the new cube's
reset. A shared mask edited by hand is recorded as the desktop records one:
*Hand-edited* yes, `hand_edited=yes` in the new cube's HISTORY, the apply
step's hand-edited body and the desktop's caveat (the parameters regrow only
its geodesic part; the cube itself is the exact record). A selection received
from the session's selection hub is recorded as the desktop records one it
received: a box as one `mask_apply_bin` step over the box (its origin and
size, the only apply step that records its ROI, since the box is the
selection); a selection that came without its grow request as the mask only,
with the desktop's caveat that no call regenerates it; one with its creator's
grow as that grow and the apply. *Made in* says where it was made, and the
moment of such a selection records the same steps before its own. No recipe
ever has a grow step without a request.

The Analysis tab also shows the tools' state: the seeds, the last cube made,
the moment's range, the path being drawn and the slit width. On the page the
same tools are under the selection controls (*Lasso seed*, *Isolate*,
*Remove*, *Show that cube*, *M0 M1 M2*, *Draw PV path*, *Width*), and the
moment map and PV diagram are shown there as well.

### The moment of the selection

*M0*, *M1*, *M2* (the menu's Analysis tab → *Moment*, or the page) compute the
moment map of the selection the desktop's way:
the selection is isolated (as *Isolate*, reused when it was already made for
this mask) and the moment is that cube's, through `/v1/products/moment` with
the fields the desktop sends, over the whole channel range (the desktop
dialog's default) and with the threshold mask **off**: the desktop dialog
starts with an automatic ~3σ mask, but on an isolated selection the
selection is the mask, and a further cut would drop the faint margin the lasso
grew into. No backend route computes a moment from a mask without writing a
cube (`kinematic_fit_bin` returns only a noise-cut velocity field, with a
fit), so an isolated cube is written to the Workspace Exports: the one
*Isolate* made for this selection, or else a **temporary** one named
`<cube>_vr-moment-tmp-<tag>.fits` (never the `_lasso-outside` name an Isolate
gets). The temporary cube is **deleted** when the selection changes (a new
grow, *Clear*, a new cube), file and session entry, and the Analysis tab
says so (or says why it stays, for example when the token may not delete
Workspace Exports); the page deletes only files carrying that marker. It is
named temporary in the moment's recipe too. While it exists it is an ordinary
dataset of the session: the desktop's lists of the session's datasets show
it, and a desktop tool opened on it in that time would lose it when the
headset's selection changes (the backend has no way to mark a dataset as
hidden). A moment of a selection no longer shown is removed from the room.

The map is shown cropped to the selection's bounding box (plus a tenth of its
size, at least 3 pixels), x and y in pixels of the cube, in a window above the
cube's right side with its colour bar and unit (the window can be grabbed by
its title bar, moved and closed; *Hide* on the Analysis tab hides it). Colours
as on the desktop: inferno for M0, a diverging cool-warm for M1 on the
desktop's range (median ± 4 robust sigma, values outside it transparent, as
the desktop draws a velocity field), viridis for M2. A velocity in m/s (also
written `m s-1`, `m.s**-1`) is shown in km/s (the numbers divided by 1000);
the recipe keeps the backend's unit.

### A PV slice along a path

*Draw* (the menu's Analysis tab → *PV path*, or *Draw PV path* on the page)
arms the PV path: each trigger press (or click on the page) adds a
point where the ray picks the cube (the first drawn sample, else the middle of
the ray inside the box), as a fractional pixel of the full cube (pixel
centres at integers, as the desktop's path). *Undo* removes the last point,
*New* starts another path, *w −*/*w +* set the slit width in pixels, *Hide*
hides the diagram. From two points on, the diagram
is computed on the full cube (`/v1/cube/pv`, the desktop's request:
`vertices` and `width_pixels`, default 1) and follows the path as it grows, as
the desktop's editable path does (one request at a time, the newest path next).
The path is drawn on the cube as orange lines on both faces of the velocity
slab, with a post at each point and, while drawing, a line to the ray. The
diagram hangs in a window above the cube's left side (movable like the
others): offset along the path in arcsec
across, the rows in the order of the channels' spectral coordinate, lowest at
the bottom (the desktop's orientation, so a cube with CDELT3 < 0 is not
mirrored), labelled with the channels' velocities (the caption says whether
they increase or decrease upward: on a frequency cube they decrease; the ticks
are placed between the rows' own velocities, since an optical velocity is not
linear in frequency), colour bar in the cube's unit. One row per channel: for
a non-linear spectral axis the desktop resamples onto a finer uniform grid,
the page does not. Choosing a selection tool stops the drawing.

**Recipes** (`visivo-recipe/1`). The seeds, Isolate and Remove are recorded as
the desktop records them: the grow with its seed lists; an isolated or removed
cube as the grow and the apply step (the mode only: the mask is the grow's
answer), with the desktop's Provenance (*Mode*, *Selected voxels*, *File*),
once per cube. Moments and PV diagrams are **recorded by the page**: the
desktop registers them with their Provenance but records no replay steps for
them. A moment is the grow, the isolate and the moment request, whose
`dataset_id` is the placeholder `{new_dataset_id}` (the isolate's answer), with
a `replay_caveat` saying that posting the opened dataset's id there would
measure the whole cube; its parameters use the desktop's *Order*,
*Channels*, *Mask*, *Scope*, *Unit*. One product per order and selection (M0
pressed again is recorded again in place); a cube isolated only for a moment
is not recorded as a product of its own. A PV diagram is its request, with
*Path*, *Width*, *From*, *To* and the desktop's `pvpath` origin; a path that
grows is one product, recorded again with its new points.

The seeds, Isolate, Remove, the moments and the PV diagram were compared with
the desktop's call by call on HCG 44, and the timings measured: see
[Selections and analysis](reference.md#selections-and-analysis).

## Selections shared with the desktop

A Kinematic Lasso or a box made in the headset appears in the desktop's cube
viewer, and a lasso made on the desktop can be sent to the headset
([#188](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/188)): one
person in the headset and others at the screen look at the same object. It
needs the headset's page to work in the desktop's session, which is what
*Open in VR Headset…* (or its *Copy Link*) gives it.

**From the headset to the desktop.** Every lasso and box made in the page is
published to the session's selection hub on the backend
([Shared selections](../../backend-api/share.md)), which measures it on the full
cube. Once *Open in VR Headset…* has been used in a cube window, that window
follows the hub (a long poll, nothing to press): the headset's selection is
shown at once as the Kinematic Lasso's own selection, the mask on the 2-D
channel map, the isosurface in the 3-D view and the channel profile, with
its voxel count in the lasso's panel, and a note in the status bar and under
*Show in VR Headset* in the panel, for example *Kinematic Lasso from the
headset: 13216 voxels, flux 6.061 Jy km/s, 1240.4 to 1500.9 km/s*. Nothing has
to be dismissed. A box arrives as a selection of every voxel of the box. The
desktop's Reach and Velocity coupling sliders act on selections made on the
desktop: they do not regrow a selection received from the headset, and a
double-click starts a new one there. *Isolate → new cube* and *Remove → new
cube* apply a received selection as shown.

**From the desktop to the headset.** With a Kinematic Lasso selection on the
desktop, press **Show in VR Headset** in the lasso's panel (or *Tools → Views →
Show Selection in VR Headset*). The page draws it in green as its own lasso, with
its statistics in the results window, which says *voxels from the desktop*; the
status bar says *Shown in the VR headset*. A hand-refined mask is sent as it is
shown.

**The same numbers.** Both sides show the statistics the backend measured on
the full cube when the selection was shared, from the same mask: the voxel
count and the flux in Jy km/s are the same on the page and on the desktop.

**The latest wins.** The hub keeps one selection per cube, the latest,
whichever side made it, in the order the selections were sent (a selection
sent earlier but measured later does not replace a later one), and each
side keeps reading the hub until what it shows is the hub's selection, so both
end on the same one; a selection
that arrives while one is being grown on the other side replaces it (the grow
in flight is dropped when it lands). When a page loads, or a cube window opens
the headset, the selection already in the hub is not shown: only those made
from then on are.

**Recorded once.** The side that makes a selection records it: the page
records its lasso or box in its recipes, the desktop records nothing until a
selection is applied. The side that receives a selection draws it with the
creator's record attached and records no step of its own: the page adds no
recipe for a desktop lasso; on the desktop, *Isolate* or *Remove* of a
headset lasso records the headset's grow (the request under the API's names,
its seed and parameters, with *Made in: VR headset* in the Provenance tab and
`made_in=VR headset` in the FITS `HISTORY`), and of a headset box the box
itself (`mask_apply_bin` with a mask of ones over it, its pixel and channel
range in the Provenance tab and the `HISTORY`). A lasso received without the
request it was grown with is recorded as the mask alone (*Recorded as: the
mask only*, with a replay caveat: the cube and its `HISTORY` are the exact
record), never as a request it was not grown with. Applying never records an
ROI of its own, as before.

How fast a selection crosses is under
[Shared selections](reference.md#shared-selections); what to check with the
headset is under
[Selections shared with the desktop, with the headset](reference.md#selections-shared-with-the-desktop-with-the-headset).
