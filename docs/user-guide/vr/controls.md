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

The first time you enter VR a short guide stands in front of you (four pages:
*Your two hands*, *The menu*, *Three modes*, *Hands without controllers*; *Next*,
*Back*, *Close* or *Skip*). It is shown at the first entry only; the menu's
**Help → Guide** (or *Controls guide* on the page) shows it again.

## Your hands

| | Primary hand (right) | Secondary hand (left) |
|---|---|---|
| Trigger | acts on what the ray points at (by mode, below) | — |
| Grip | grabs the cube: it follows the hand | grabs the cube; both grips move, turn and scale it between your hands |
| Lower face button | **A**: next mode (Explore → Select → Clip) | hold **X**: the menu |
| Upper face button | **B**: undo the last change of the look | **Y**: reset the view |
| Stick | the mode's continuous control (below) | ←/→: the mode's next tool (the colour map in Explore) |

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
the box). The secondary stick flicked ←/→ steps through them; the Mode tab
chooses one. What they compute is described in [Analysis](analysis.md).

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
a pick comes with a selection. The card is redrawn at most 15 times a second,
only when its text changes; its text costs about 4 µs. On the page the same
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
slab) are on the menu. The fingertips are drawn as small crosses, the pinch as
a line that turns yellow when it closes, the palm as a cross that grows when
the hand grips.

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
seen (it is shown at the first entry into VR only), **left-handed**, the
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
- the **selection tool** in use and the time of the last pick, lasso,
  statistics, spectrum and box requests;
- **menu redraw**: the time to redraw the menu and to upload it as a texture
  (medians of the last 60 redraws), and how many redraws; **readout redraw**
  and the mode;
- the *refine* line (see [Refinement](reference.md#refinement-full-quality-frames-from-the-backend)),
  the link and reprojection lines on a cluster (see [Cluster backends](cluster.md)),
  the *share* line (sent, received, publish and receive ms), and for a point
  table *points X M drawn of Y M sampled, Z M rows*.
