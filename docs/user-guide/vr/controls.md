# Controls and the hand menu

The headset's controls follow a few fixed rules
([#201](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/201)):

- **Your two hands have roles.** The **primary** hand (the right one, or the
  left with the *left-handed* setting) points and acts: its trigger uses what
  its ray points at, its grip grabs. The **secondary** hand holds the menu and
  grabs too; with both grips you move, turn and scale the cube.
- **One menu**, held open by the secondary hand and drawn next to its wrist.
- **Three modes**, Explore, Select and Clip, always shown above the primary
  controller and in the cursor readout. In each mode each stick has one
  meaning; the buttons keep the same global actions in every mode.
- **Labels** next to the controllers say what each control does now. They
  are on the first time and can be turned off.

The first time you open the page, a short controls guide is shown on the
2-D page, before you enter VR (four parts: *Your two hands*, *The menu*,
*Three modes*, *Hands without controllers*); **Got it** hides it for good, and
*Controls guide* on the page shows it again. In VR the menu's **Help → Guide**
shows it on a card (*Next*, *Back*, *Close* or *Skip*) that stands beside the
cube on your primary hand's side, so it never covers the cube, the menu or the
labels; the menu and the labels are drawn over it when a hand brings them in
front of it ([#226](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/226)).
Until #226 the guide stood in front of you at the first entry into VR, over
the menu and the labels.

## Your hands

| | Primary hand (right) | Secondary hand (left) |
|---|---|---|
| Trigger | acts on what the ray points at (by mode, below) | — |
| Grip | grabs the cube: it follows the hand | grabs the cube; both grips move, turn and scale it between your hands |
| Lower face button | **A**: next mode (Explore → Select → Clip) | hold **X**: the menu |
| Upper face button | **B**: undo the last change of the look | **Y**: reset the view |
| Stick | the mode's continuous control (below) | ←/→: the mode's next tool (the colour map in Explore) |

The controllers are drawn with their real models: on a Quest 3 the Touch Plus
controllers, as they look in the hand. The trigger, the grip, the stick and the
face buttons move as you press or tilt them, by the amount you press them. The
models are those of the WebXR Input Profiles project (MIT licence), served by
the backend with the page; nothing is fetched from the internet. The page picks
the model from the names the browser gives the controller, most specific first:
`meta-quest-touch-plus-v2` (newer Quest 3 controllers),
`meta-quest-touch-plus` (Quest 3 and 3S), `meta-quest-touch-pro`,
`oculus-touch-v3` (Quest 2), then `generic-trigger-squeeze-thumbstick`. The
headset's browser remembers which models its controllers used and loads them
on the 2-D page, before you enter VR; the first time, they load when the
controllers appear. A controller none of these fits, or while its model is
still loading (a fraction of a second), is drawn as a small solid model made by the page, in the hand's
colour (orange the primary, blue the secondary). The short line from the
controller's front stays in the hand's colour, so you can still tell which
hand is the primary. Until #226 only the rays could be seen.

The models are shaded from their glTF materials: base colour (an sRGB
texture), metallic and roughness, emissive, and the alpha mode, lit by a soft
light from above and one at your eye. They look as the asset paints them:
the `meta-quest-touch-plus` model has a light grey grip with a dark
face plate and buttons; the `meta-quest-touch-plus-v2` model is
dark all over, with green accents. Their textures are kept at 1024 px a side
(about 5.3 MB of GPU memory each, one per hand), mipmapped and filtered
anisotropically so they do not shimmer.

With the *left-handed* setting the roles swap: the left hand points (its
**X** is next mode, **Y** undo) and the right holds the menu (hold **A**,
**B** resets the view). The setting is on the menu's Settings tab and on the
page (*left-handed*), and is kept in the browser.

- **Undo** (B, or *Undo* on the Mode tab) takes back one change of the look:
  threshold, colour max, opacity, colour map, emission or maximum intensity,
  stretch, and the clipping box and velocity slab. A whole push of a stick, or
  a whole drag of a menu slider, is one change; up to 30 are kept, and a new
  cube starts a new history. With nothing left to undo the controller gives two
  pulses.
- **Reset view** (Y, or *Reset view* on the Mode tab) puts the cube back in
  front of you at its first size; the result windows come with it.
- **Grabbing**: put the cube down by letting go: it stays where you leave it.
  The box, its ticks and labels are in world coordinates: R.A., Dec. and
  velocity (km/s).
- **The labels belong to the cube** (#226): the tick labels and axis titles
  are fixed to the box and turn only when you turn the cube, never towards
  your head. R.A. and Dec. are written on the front face (the one facing you
  when the cube first appears), the velocity labels along the right-hand
  bottom edge at 45° between the front and the right face, so both read from
  the start position. Seen from behind, a label reads the right way round
  (its text is mirrored back), at the same place.

## Modes

The mode is shown above the primary controller (*EXPLORE*, *SELECT · Kinematic
Lasso*, *CLIP · velocity slab*, …) and at the top of the cursor readout. **A**
steps to the next mode; the Mode tab of the menu chooses one directly. The
page (desktop browser) has the same choice (*Mode*), and a click there does
what the trigger does.

While a PV path is being drawn (Analysis tab → *Draw*), the trigger adds the
path's points in any mode (the mode line says *PV path*). Choosing a lasso seed
mode on the Analysis tab switches to Select with the Kinematic Lasso.

### Explore

| Control | Does |
|---|---|
| Primary stick ↕ | threshold |
| Primary stick ↔ | opacity |
| Primary trigger | pins the readout at the point it shows (an amber card that stays; another pull moves it, a pull on empty space removes it) |
| Secondary stick ←/→ | previous / next colour map (one step per flick) |
| Secondary stick ↕ | colour max |

### Select

The tools: **spectrum** (the spectrum of the full cube at the pixel the ray
picks), **Kinematic Lasso** (a selection grown on the full cube from the
picked seed) and **box region** (two corners, statistics of every voxel in
the box). The secondary stick pushed ←/→ and held for a moment (about a fifth of
a second) steps through them; the Mode tab chooses one. A push that is not
mostly sideways, one made while that hand holds the grip (moving the cube),
or one made while a tool is still working changes nothing until the stick is
back at rest, so a thumb resting on the stick does not change the tool. What they compute is described in [Analysis](analysis.md).

| Control | Does |
|---|---|
| Primary trigger | the tool at the ray: *spectrum here*, *lasso seed*, *box corner* (a corner can be placed with the controller's tip inside the cube) |
| Primary stick ↕ | threshold (what the pick lands on) |
| Secondary stick ←/→ | next tool |

### Clip

The tools: **velocity slab** (the channel range along the spectral axis) and
**clipping box** (all six faces).

| Control | Velocity slab | Clipping box |
|---|---|---|
| Primary trigger | centres the slab on the channel of the emission the ray meets (along every channel, so you can point at emission the slab hides) | hold: the face nearest to the controller (or to the point the ray picks) follows the controller |
| Primary stick ↕ | moves the slab along the velocity axis | shrinks or grows the box about its centre |
| Primary stick ↔ | widens or narrows the slab | — |
| Secondary stick ←/→ | next tool | next tool |

*All channels* and *Reset box* are on the Mode tab; *All channels* and the
channel range's two ends are on the Look tab too.

## The hand menu

**Hold X** (the secondary controller's lower button) and the menu opens above
that hand, toward the middle, turned to face you; **let go and it closes**.
Point the primary ray at it and pull the trigger: the ray ends on the menu with
a small yellow cross, the item under it lights up, and the trigger presses it.
While the ray is on the menu, or the trigger was pulled on it, the trigger does
nothing to the cube. A slider pressed keeps following the controller until the
trigger is released, even when the ray slips off its row. *Keep open* (Mode
tab) leaves the menu where it is when you let go, so both hands are free; the
next press of X closes it.

The menu is 0.37 × 0.33 m; its text is 0.8 to 1.1 cm high, about 1° to 1.4°
at the 0.45 m a menu by the wrist is seen from. The numbers on the wrist are
hidden while it is open (they sit where it opens). In a desktop browser the
same menu stands in the page's 3-D view with *show the headset's menu here*
(or `?panel3d` in the address) and is used with the mouse through the same ray
code: a click presses, a drag moves a slider.

The tabs:

- **Mode**: *Explore*, *Select*, *Clip*; the Select tools (*spectrum*,
  *Kinematic Lasso*, *box region*) and *Clear* (removes the selection and its
  marks); the Clip tools (*velocity slab*, *clipping box*), *All channels*,
  *Reset box*; *Undo*, *Reset view*, *Keep open*. Under them, the controls of
  the current mode, and the state line (below).
- **Look** (what used to be the panel beside the cube):
  - the cube's file name, two info lines (the cube's shape, the reduced
    shape, the binning and texture; the session: *the desktop's session*,
    *own session*, …);
  - Colour map: ◀ ▶ previous/next; *emission* or *max intensity*;
  - Threshold: a slider (fine steps near the noise, as on the page) with its
    value in the cube's unit; *Reset* puts back the default threshold and
    colour range; from: *for this binning* or *full cube (desktop)*;
  - Colour max, Opacity: sliders;
  - Channels: the velocity slab, a range with two ends (the nearer end moves),
    the channel range and its velocities in km/s with their convention below
    it; *All channels*;
  - Refine: on/off and its state (*refined (still view)*, *waiting*, *paused
    in passthrough*, *lost: retrying in N s*, *unavailable: …*);
  - Tool: spectrum, lasso or box (switches to Select); *Clear*.
- **Analysis** (what used to hang under the panel): Lasso seeds (*new*, *add*,
  *carve*), Selection (*Isolate*, *Remove*, *Show that cube*), Moment (*M0*,
  *M1*, *M2*, *Hide*), PV path (*Draw*, *Undo*, *New*, *w −*/*w +* for the slit
  width in pixels, *Hide*); below the buttons the slit width and the tools'
  state (seeds, the last cube made, the moment's range, the path being drawn).
  See [Analysis](analysis.md).
- **Layers** (what used to be the strip under the panel), for what is shown:
  a point table: *points/splats*, *colour:* (next column), *rows* (next
  budget), *size −*, *size +*; a velocity field: *streamlines*, *arrows*,
  *arrows −/+*, *new seeds*; a cube with a catalogue placed: *catalogue*,
  *labels*, *through velocity*, with the catalogue's summary or its first
  warning (in amber). See [Data types](data-types.md).
- **Settings**: *Left-handed*, *Labels by the buttons*, *Haptics*, *Numbers on
  the wrist*, *Hold the frame rate*, *Refine with the full cube*, *Reproject
  refined frames with depth*, *Refined frame size* (0.4× to 1×, one step per
  press), *Record measurements* with, on the same row, *Run protocol* (*Cancel
  run* while one runs); see [Recording measurements](reference.md#recording-measurements).
- **Help**: the controls of a mode (the current one; *Explore*, *Select*,
  *Clip* show the others), for each of its tools, and *Guide*, which brings
  back a notice folded on the menu first, else the first-run guide.

Under the Look and Mode pages a line says when the page lowered the quality to
hold the frame rate (*Quality lowered to hold 90 Hz: 0.50 samples/voxel (of 1),
eye buffer 0.85× (of 1×)*), what a selection is doing, or a notice folded away
(*⚠ Backend not reachable: retrying — Help → Guide shows it*).

The stretch, the velocity stretch, the binning, the texture, the Kinematic
Lasso parameters and the eye-buffer settings are set on the page before
entering VR (see [The settings on the page](reference.md#the-settings-on-the-page)).

## Labels, readout and haptics

**Labels.** A small card above each controller lists its controls as they
are now: above the primary one the mode, then *Trigger*, *Grip*, *Stick ↕*,
*Stick ↔*, *A*, *B*; above the secondary one *Hold X*, *Y*, *Grip*, *Stick ↔*,
*Stick ↕* (only the controls that do something in this mode). They change with
the mode, the tool and the left-handed setting, from the same table the
controls use, so a label always says what the control does. Settings → *Labels
by the buttons* turns them off (the mode stays above the primary controller);
they are on the first time.

**Cursor readout.** Where the primary ray meets the drawn emission, a card
above that point shows the mode, the sky position (*RA hh:mm:ss.ss Dec
±dd:mm:ss.s*, or the axes' own names and units), the velocity of that channel
with its convention and the channel number, and the value drawn there in the
cube's unit (marked *binned* when the cube is reduced; *blank* for a blank
voxel). The position is the full-resolution pixel the point falls in,
interpolated from the axis labels as the ticks are; the exact ICRS position of
a pick comes with a selection. Its text is sized for the headset (#226):
the body lines are at least 0.9° high at the eye at any distance from 30 cm to
4 m (the card grows with distance; about 22 cm wide at 75 cm), bold white on a
near-opaque dark card with a light edge (contrast over 15:1), and it is a
composition layer where the browser has them, so it stays sharp at any
eye-buffer scale. (Until #226 the text was under half a degree high.) The card
is redrawn at most 15 times a second, only when its text changes; its text
costs about 4 µs. On the page the same
lines follow the mouse, in a line above the numbers at the bottom of the
page's controls.

**Haptics.** Where the controller has a haptic actuator: a short pulse on a
press (a menu item, the trigger on the cube, a step of the stick), a stronger
one when a result arrives (a spectrum, a lasso, a box, an Isolate or Remove, a
moment, a PV diagram), two pulses on an error. Settings → *Haptics* turns them
off. Tracked hands have none.

## Result windows

The results stand in windows: **Selection** (the picked position, the
spectrum, the lasso's or the box's statistics; right of the cube), **Moment**
(above the cube's right side) and **PV diagram** (above its left side). Point
the ray at a window's title bar and hold the trigger to move it: it follows the
ray and turns to face you; **×** closes it. A new result opens it again. Where
a window was left is kept for the next sessions (relative to where the cube
first appears). The window's content takes the ray, so a trigger on it never
starts a selection on the cube behind it; when the cube stands between the
controller and a window, the cube takes the ray.

## Hand tracking

Put the controllers down and the page switches to your hands within a frame;
pick them up again and it switches back. The roles stay:

| Gesture | Does |
|---|---|
| **Pinch** (thumb tip to index tip), primary hand | the trigger: presses the menu's items, drags its sliders, moves windows, and acts on the cube along the hand's ray (the headset's own pointing ray; the ray is held still while the fingers close, so the pinch does not move what it points at) |
| **Fist** (middle, ring and little finger curled), either hand | the grip: one hand carries the cube, both move, turn and scale it |
| **Palm up**, secondary hand | the menu, open while the palm stays up |

After a grip, and for a hand just seen, a pinch counts only once the fingertips
have parted, so opening a fist with the thumb still on the index, or a hand
that appears already pinched, does not press anything. The palm counts as up
within about 45° of vertical and stops at about 63°; a closed hand never opens
the menu, and the primary hand's palm does nothing. The sticks and face buttons
have no gesture: their actions (modes, undo, reset view, threshold, opacity,
slab) are on the menu. Each tracked hand is drawn whole (#226): a small
sphere at each of its 25 joints and a bar along each bone, in the hand's
colour (orange primary, blue secondary); the thumb and index tips turn yellow
while pinching, the bones while gripping, and a line joins the two tips as
they close.

The pinch closes under 1.2 cm between the fingertips' surfaces and opens over
2.8 cm; the grip under 0.72 of the palm's length, opening over 0.95. These
thresholds, and the palm's, were set and tested on synthetic hands, not yet on
a headset (see the [checks](reference.md#controls-and-the-hand-menu-201)).

## Settings kept in this browser

The page keeps a few conveniences in the browser's local storage, for this
browser only (the headset's browser and the Mac's keep their own): the
**colour map**, the **threshold source** (*for this binning* or *full cube*),
the **refinement** settings (on or off, refined size, JPEG quality, reproject
with depth), the **last selection tool**, whether the **controls guide** was
seen (it is shown on the 2-D page until *Got it*), **left-handed**, the
**labels** and **haptics** settings, and where each **result window** was
left (relative to where the cube first appears).

Nothing else is stored there: not the token (unless *remember on this device*
is ticked), not the session, not any result. If the browser refuses storage (a
private window), the page works with the defaults. `?refine=0` or `?refine=1`
in the address overrides the stored refinement setting for that visit. The
mode is not kept: the page starts in Explore.

## The numbers on the wrist

Above the secondary controller (over its labels), or next to the cube without
controllers, the page shows its numbers, updated four times a second (the
page shows them too); Settings → *Numbers on the wrist* hides them, and they
are hidden while the menu is open:

- **FPS** and the **frame** interval (mean and 95th percentile), from the
  headset's frame callbacks, and the share of **missed** frames (an interval
  over 1.5 times the display period);
- **CPU**: the page's own time per frame; **GPU**: the GPU time of the frame
  when the browser offers timer queries (n/a otherwise);
- **samples/voxel** in use and the **max steps/ray** it allows;
- the **volume**, its texture size, the **binning**, the **threshold**;
- the **eye buffer** size, the resolution scale and the foveation level;
- **text**: *quad layers* (the menu, labels, readout, windows and these
  numbers as composition layers, sharp at any eye-buffer scale) or *in the
  scene* with the reason (#226);
- **GPU textures**: the page's own textures in MB (the cube and its
  empty-space grid, the panels, the selection mask); repeated selections must
  not make it grow (#226);
- the **selection tool** in use and the time of the last pick, lasso,
  statistics, spectrum and box requests;
- **menu redraw**: the time to redraw the menu and to upload it as a texture
  (medians of the last 60 redraws), and how many redraws; **readout redraw**
  and the mode;
- the *refine* line (see [Refinement](reference.md#refinement-full-quality-frames-from-the-backend)),
  the link and reprojection lines on a cluster (see [Cluster backends](cluster.md)),
  the *share* line (sent, received, publish and receive ms), and for a point
  table *points X M drawn of Y M sampled, Z M rows*;
- the *hub* line ([#237](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/237)):
  whether the page is linked to the desktop's headset hub and how it reaches
  it (`adb`, `pair` or `lan`), the cubes the desktop sent (*commands*), what
  the page reported (*look*, *clip*, *products*, *cubes*) and the time of the
  last report and of the last product saved for the desktop.

## What the desktop sees

The page tells the desktop what you choose and make in the headset
([#237](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/237)), in
the desktop's headset activity list:

| In the headset | Reported | When |
|---|---|---|
| Colour map, threshold, colour maximum, opacity, render mode (Look page, sticks) | the look; the threshold and colour maximum as data values of the cube | 0.5 s after the last change (a slider dragged is one report) |
| The clipping box (Clip mode, Look page) | the box in voxels of the full cube (`[[x0, x1], [y0, y1], [z0, z1]]`, ends included), or none | 0.5 s after the last change |
| A spectrum (Select ▸ probe) | its CSV in the Workspace Exports (`<cube>_spectrum_x<x>_y<y>.csv`), computed again from the full cube | when it is shown |
| A moment of the selection (Analysis ▸ M0, M1, M2) | its FITS (`<cube>_vr_m<order>.fits`), the same map | when it is shown |
| A PV slice (Analysis ▸ Draw PV path) | its FITS (`<cube>_vr_pv.fits`) and the path's vertices | 1.5 s after the last point |
| *Isolate*, *Remove* | the cube the backend wrote | when it is made |
| A catalogue over the cube, or a point table or velocity field opened on the page | its file on the backend | when it is shown |
| Another cube on show | its path and binning | when it is shown |

Each product comes with its recipe record (the requests that made it), as in
*Download recipes*. Nothing of this waits on the network: the tools answer as
before, and the reports go after them, from the page's timers, never during a
frame. A report that fails is in *Copy diagnostics* (`hub-failed`), never in
your way. A page that stood down for another window reports nothing.
