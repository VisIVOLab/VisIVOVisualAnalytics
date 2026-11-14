# Controls and the hand menu

The headset's controls follow a few fixed rules
([#201](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/201),
[#257](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/257)):

- **Your two hands have roles.** The **primary** hand (the right one, or the
  left with the *left-handed* setting) points and acts: its trigger uses what
  its ray points at, its grip grabs. The **secondary** hand opens the menu and
  grabs too; with both grips you move, turn and scale the cube.
- **One menu**, opened by the secondary hand. It stands about a metre in front
  of you and stays there until it closes, so both hands are free. Its tabs
  follow the way you work: **Display**, **Range**, **Select**, **Measure**,
  **More**.
- **The tab sets what the trigger does.** Display (and Measure) look around,
  Select selects, Range moves the velocity range or the crop. The mode is
  shown on the wrist cards, in a toast when it changes, and in the cursor
  readout. In each mode each stick has one meaning; the buttons keep the same
  global actions in every mode.
- **Wrist cards**, shown only while you look at a wrist
  ([#256](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/256)):
  the primary wrist says what each control does now, the secondary wrist
  shows the cube, the mode and the selection's numbers. The rest of the time
  nothing stands in view but the cube and what you opened.

The first time you open the page, a short controls guide is shown on the
2-D page, before you enter VR (four parts: *Your two hands*, *The menu*,
*What the trigger does*, *Hands without controllers*); **Got it** hides it for
good, and *Controls guide* on the page shows it again. In VR the menu's
**More → Guide** shows it on a card (*Next*, *Back*, *Close* or *Skip*) that
stands beside the cube on your primary hand's side, so it never covers the
cube or the menu; the menu and the wrist cards are drawn over it when a hand
brings them in front of it
([#226](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/226)).
Until #226 the guide stood in front of you at the first entry into VR, over
the menu.

## Your hands

| | Primary hand (right) | Secondary hand (left) |
|---|---|---|
| Trigger | acts on what the ray points at (by mode, below) | — |
| Grip | grabs the cube: it follows the hand | grabs the cube; both grips move, turn and scale it between your hands |
| Lower face button | **A**: next mode (Explore → Select → Range) | hold **X**: the menu |
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
**X** is next mode, **Y** undo) and the right opens the menu (hold **A**,
**B** resets the view). The setting is on the menu's More tab (*Handedness*)
and on the page (*left-handed*), and is kept in the browser.

- **Undo** (B, or *Undo* in the menu's footer) takes back one change of the
  look: threshold, colour max, opacity, colour map, Emission or MIP, stretch,
  and the spatial crop and velocity range; on a point table or a velocity
  field the crop, the colour map and window, the style, size, intensity,
  arrows and box (not a new column, budget or seeds, which ask the backend
  again). A whole push of a stick, or a whole drag of a menu slider, is one
  change; up to 30 are kept, and new data starts a new history. With nothing left to undo the controller gives two
  pulses.
- **Reset view** (Y, or *Reset view* in the menu's footer) puts the cube back
  in front of you at its first size; the result windows come with it.
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

There are three modes: **Explore** (set by the Display and Measure tabs),
**Select** and **Range** (the Range tab; called Clip until #257). The mode is
shown on both wrist cards (*EXPLORE*, *SELECT · Kinematic Lasso*, *RANGE ·
Velocity range*, …), in a toast low in the view when it changes, and at the
top of the cursor readout. Choosing a tab sets its mode; **A** steps to the
next mode without the menu, and the menu then opens on that mode's tab. The
page (desktop browser) has the same choice (*Mode*), and a click there does
what the trigger does. Until #257 the menu had a Mode tab to choose them.

While a PV slice is being drawn (Measure → PV slice → *Draw*), the trigger
adds the slice's points in any mode (the mode line says *PV slice*). Choosing
*Lasso: New*, *Add* or *Subtract* on the Select tab switches to Select with
the Kinematic Lasso.

### Explore

| Control | Does |
|---|---|
| Primary stick ↕ | threshold |
| Primary stick ↔ | opacity |
| Primary trigger | pins the readout at the point it shows (an amber card that stays; another pull moves it, a pull on empty space removes it) |
| Secondary stick ←/→ | previous / next colour map (one step per flick) |
| Secondary stick ↕ | colour max |

### Select

The tools: **Spectrum probe** (the spectrum of the full cube at the pixel the
ray picks), **Kinematic Lasso** (a selection grown on the full cube from the
picked seed) and **Box region** (two corners, statistics of every voxel in
the box). The secondary stick pushed ←/→ and held for a moment (about a fifth
of a second) steps through them; the Select tab chooses one. A push that is
not mostly sideways, one made while that hand holds the grip (moving the
cube), or one made while a tool is still working changes nothing until the
stick is back at rest, so a thumb resting on the stick does not change the
tool. What they compute is described in [Analysis](analysis.md).

| Control | Does |
|---|---|
| Primary trigger | the tool at the ray: *spectrum probe here*, *lasso seed*, *box corner* (a corner can be placed with the controller's tip inside the cube) |
| Primary stick ↕ | threshold (what the pick lands on) |
| Secondary stick ←/→ | next tool |

### Range

What the trigger moves: the **Velocity range** (the channel range along the
spectral axis; *Spectral range* on a cube without a velocity axis) or the
**Spatial crop** (all six faces of the box).

| Control | Velocity range | Spatial crop |
|---|---|---|
| Primary trigger | centres the range on the channel of the emission the ray meets (along every channel, so you can point at emission the range hides) | hold: the face nearest to the controller (or to the point the ray picks) follows the controller |
| Primary stick ↕ | moves the range along the velocity axis | shrinks or grows the crop about its centre |
| Primary stick ↔ | widens or narrows the range | — |
| Secondary stick ←/→ | next tool | next tool |

*Full band*, *Reset crop*, the range's two ends and the crop's are on the
Range tab.

## The hand menu

**Hold X** (the secondary controller's lower button) and the menu opens in
front of you: about 0.95 m away along the direction you look (horizontally),
12 cm below your eyes, turned to face you. **Let go and it closes.** While it
is open it stays where it opened (world-locked): the hand that opened it is
free, and turning your head does not move it. To put it somewhere else, point
at its **title bar** (the top strip, *⠿ drag to move*), hold the trigger and
move the ray: the menu follows at the same distance, turned to face you.
*Keep open* (in the footer) leaves it open when you let go of X; the next
press of X closes it. With tracked hands, the palm turned up opens it.

Point the primary ray at it and pull the trigger: the ray ends on the menu with
a small yellow cross, the item under it lights up, and the trigger presses it.
While the ray is on the menu, or the trigger was pulled on it, the trigger does
nothing to the cube. A slider pressed keeps following the controller until the
trigger is released, even when the ray slips off its row.

The menu is 0.77 × 0.72 m at 0.95 m: the same angle as the menu by the wrist
before #257 (0.37 × 0.33 m at 0.45 m), so its text keeps its size in your
view, 1.0° to 1.4° high (about 22 pixels of the menu a degree). These are the
sizes the page asks for. The work on
[#267](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/267) found
that the Quest Browser may read a composition layer's width and height as
half extents, which would draw the panels twice as large as asked; that is
not yet measured on the headset, and the menu's size is one of the checks
below (see the [checks](reference.md#the-menu-in-front-of-you-organised-by-task-257)). It is a 2-D
canvas redrawn only when something on it changes. The secondary wrist's card is
hidden while the menu is open. In a desktop browser the same menu stands in
the page's 3-D view at half that size with *show the headset's menu here* (or
`?panel3d` in the address) and is used with the mouse through the same ray
code: a click presses, a drag moves a slider.

**The tabs are the same on every dataset.** What they hold depends on what is
shown: a cube, a point table (a simulation snapshot, a catalogue as points) or
a velocity field. Point tables and velocity fields replace the cube, so their
controls are on the same tabs. A control that does not apply to what is shown
is greyed, with a one-line reason under it (*Cubes only*, *A point table or a
velocity field has no spectral axis*, *Take a spectrum with the Probe
(Select) first*, …); it never disappears, so nothing moves around. The menu
opens on the tab of the current mode, or on the last tab used.

- **Display** (the tab the menu starts on)
  - *A cube*: **Colour map** as swatches (the name of the one in use below);
    **Threshold** first, a slider (fine steps near the noise) with its value
    in the cube's unit and *Reset* (the default threshold and colour range);
    **Colour max** (its value in the cube's unit); **Opacity**;
    **Rendering**: *Emission* or *MIP*; **Catalogue**: *Overlay*, *Labels*,
    *Through velocity*, with the catalogue's summary or its first warning
    (greyed until a catalogue is placed, on the page or from the desktop).
  - *A point table*: **Colour by** (one chip per column, *none* first, then the
    velocity columns such as *VX*, *VY*, *VZ*, then the others; *more ▸* when
    there are more than fit, and the menu opens on the first page); **Scale, range**: *Log* and the colour
    window (a range with two ends; its data values below); **Colour map**;
    **Style**: *Points* or *Smooth (splats)*, *Size −*, *Size +*;
    **Intensity** (splats); **Particles shown** (the next budget of rows),
    *Sample again*, *Box and ticks*.
  - *A velocity field*: *Streamlines*, *Arrows*; **Arrow length**: *Shorter*,
    *Longer*; **Seed density**: *Fewer*, *More* (about ×1.4 a press, traced
    again at once), *Reseed streamlines*; **Colour by speed**: the colour
    window over the speed range (its values below); **Colour map**; **Box**:
    *Box and ticks*, and the box size in Mpc (read-only).
- **Range**
  - **Velocity range** (*Spectral range* without a velocity axis): a range with
    two ends (the nearer end moves), its velocities in km/s with their
    convention and its channels below it; *Full band*.
  - **Trigger**: *Velocity range* or *Spatial crop*, what the trigger moves in
    the cube.
  - **Crop x**, **Crop y**, **Crop z**: the spatial crop, one range per axis
    (pixels of the full cube below; on a cube *Crop z* is greyed, the third
    axis being the velocity range); *Reset crop*.
  - A point table or a velocity field has only the spatial crop: the points
    and lines outside it are not drawn (a point table is not refined by the
    backend while it is cropped).
- **Select** (a cube; greyed on point tables and velocity fields, whose
  selections come with [#259](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/259))
  - **Tool**: *Spectrum probe*, *Box region*, *Kinematic Lasso* (the tool on a
    first visit, so a galaxy is lassoed and isolated in two presses: Select,
    the trigger on it, *Isolate*; afterwards the last tool used, kept in the
    browser).
  - **Lasso**: *New*, *Add*, *Subtract* (the next seed's mode).
  - **Selection**: *Clear* (removes the selection and its marks), with the
    selection's first numbers.
  - **Result**, only while something is selected: *Isolate*, *Remove*, *Show
    result cube* (greyed until Isolate or Remove made one; greyed on a box
    region, which has no mask). See [Analysis](analysis.md).
- **Measure** (a cube)
  - **Moment**: *Intensity (M0)*, *Velocity (M1)*, *Dispersion (M2)*, *Hide*.
    On a lasso selection it is the moment of that object (its mask). With no
    selection it is the moment of the whole field over the velocity range
    shown, with the desktop moment dialog's defaults (the automatic threshold
    mask).
  - **PV slice**: *Draw*, *Undo*, *New*, *Hide*; **PV width**: *PV width −*,
    *PV width +*, the width in pixels and in arcsec.
  - **Spectrum**: *Pin spectrum* keeps the last spectrum; the next ones are
    drawn over it (in gold) in the Selection window, until *Unpin spectrum*.
    Clear keeps it; another cube does not show it.
- **More**: **Handedness** (*Right-handed*, *Left-handed*), **Button labels**
  (the legend on the primary wrist), **Haptics**, **Wrist readout** (the cube
  card on the secondary wrist), **Guide** (brings back a notice folded on the
  menu first, else the first-run guide).

**The footer**, under every tab: *Undo*, *Reset view*, *Keep open*.

Under each tab the status lines say when the page lowered the quality to hold
the frame rate (*Quality lowered to hold 90 Hz: 0.50 samples/voxel (of 1),
eye buffer 0.85× (of 1×)*), what a selection or a measurement is doing, or a
notice folded away (*⚠ Backend not reachable: retrying — More → Guide shows
it*).

**Technical settings** are not on the menu: only with `?debug` in the
address does More show *Technical settings ▸*, a page with *Hold the frame
rate*, *Refine with the full cube*, *Reproject refined frames with depth*,
*Refined frame size* (0.4× to 1×, one step per press), *Record measurements*
with *Run protocol* (*Cancel run* while one runs; see
[Recording measurements](reference.md#recording-measurements)) and
*Threshold from*: *This binning* or *Full cube*. Without `?debug` the
threshold always starts from the full cube's, the value the desktop starts
from (`default_threshold` of `/v1/cube/reduced_info`: about a hundred noise
voxels of the whole cube above it), so the headset shows the emission the
desktop shows, in the same unit; a choice stored earlier counts only with
`?debug` (a synthetic cube, which has no full cube, uses its own). Refinement
stays on, with the settings kept in the browser.

Until #257 the menu stood by the left wrist with six tabs (Mode, Look,
Analysis, Layers, Settings, Help); the table maps the old names to the new
ones.

| Before #257 | Now |
|---|---|
| Look | Display |
| Analysis | Select and Measure |
| Mode page, Explore / Select / Clip | the tabs set the mode (Explore / Select / Range) |
| Layers | Display (point tables and velocity fields), Display → Catalogue |
| Settings | More; the technical rows under `?debug` only |
| Help | the primary wrist's legend and More → Guide |
| velocity slab, Channels | Velocity range |
| All channels | Full band |
| clipping box | Spatial crop |
| spectrum | Spectrum probe (Probe) |
| emission / max intensity | Emission / MIP |
| Show that cube | Show result cube |
| Lasso seeds new / add / carve | Lasso: New / Add / Subtract |
| w − / w + | PV width − / + |
| PV path | PV slice |
| M0 / M1 / M2 | Intensity / Velocity / Dispersion (M0 / M1 / M2) |
| Cube card on the wrist (Numbers on the wrist) | Wrist readout |
| Button legend on the wrist (Labels by the buttons) | Button labels |
| splats | Smooth (splats) |
| new seeds | Reseed streamlines |
| M rows | Particles shown |

The stretch, the velocity stretch, the binning, the texture, the Kinematic
Lasso parameters and the eye-buffer settings are set on the page before
entering VR (see [The settings on the page](reference.md#the-settings-on-the-page)).

## Wrist cards, toasts, readout and haptics

**The watch gesture.** Turn a wrist so that its back, where a watch face would
be, faces your eyes, as to read a watch, and keep it in front of you: a small
card, 8 cm wide, fades in on that wrist within a fifth of a second. Turn the
wrist away, lower it or look elsewhere, and it fades out
([#256](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/256)).
The card shows when the back of the wrist is within about 44° of the direction
to your eyes, within about 32° of the middle of your view and within 60 cm of
your eyes, held for 0.12 s; it goes when the back is more than about 63° away,
the card more than 45° off the middle of the view or farther than 70 cm, for
0.2 s. The gap between the two keeps it from flickering at the edge. The card
lies on the wrist, turned halfway towards your eyes, its text upright.

The gesture is the opposite of the menu's: with the palm up (the menu, with
tracked hands) the back of the hand faces the floor, and a hand whose palm is
up never shows a card. The secondary wrist's card also gives way to the menu
while it is open. Pointing at the cube with the arm out does not show a card
(the wrist is too far and its back does not face you).

- **Primary wrist (right, or left when left-handed): the button legend**
  (8 × 7.3 cm, up to eight rows) of
  the tool in use, under the mode and tool (*SELECT · Kinematic Lasso*): the
  primary hand's controls but the grip (*Trigger*, *Stick ↕*, *Stick ↔*, *A*,
  *B*, only those that do something now), then the secondary hand's that
  change with the mode (*Hold X* the menu, *L stick ↔* the tool or the colour
  map, *L stick ↕* the colour max). With tracked hands: *Pinch* and *Palm up*.
  It changes with the mode, the tool and the left-handed setting, from the
  same table the controls use. More → *Button labels* turns it off.
- **Secondary wrist: the cube card**: the cube's name, its full size (and the
  binning on show), the mode and tool, and the selection's numbers: for a
  lasso its voxels, flux (Jy km/s) and velocity range; for a box its valid
  voxels, mean and rms, flux; for a spectrum its position, the value at the
  channel picked and the peak; what is under way or the last error. More →
  *Wrist readout* turns it off.

Each card is redrawn only when its text changes, and uploaded (a 2-D texture)
only then or at one of four opacity steps while it fades.

Until #256 a label card stood above each controller all the time, the
frame-rate numbers above the secondary hand, and the Selection window as soon
as Select mode was on.

**Toasts.** A short message is a small strip low in your view (20° below
where you look, 32 cm wide at 70 cm, one or two lines) that fades after about
2 s: a cube shown from the computer, the backend restarted or the cube opened
again (*Repeat an unfinished selection*), a selection done (its first numbers,
*Lasso 12,345 voxels · flux 3.21 Jy km/s*), a moment or PV diagram done, an
error of the tools (*⚠ …*), and the mode or tool when it changes. Messages
that come together are told one after the other (at most three wait; a new
mode or result replaces the one of its kind still waiting). Notices that ask
for a choice (*Exit VR*, *Retry*, *Reload*), stay until their condition ends,
or say more than a line (the measurement run's) are still the message card in
front of you.

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
moment, a PV diagram), two pulses on an error. More → *Haptics* turns them
off. Tracked hands have none.

## Result windows

The results stand in windows: the **selection** window (the picked position,
the spectrum, the lasso's or the box's statistics; right of the cube; since
#256 it opens only with a result, work under way or an error, not as soon as
Select mode is on), **Moment** (above the cube's right side) and **PV
diagram** (above its left side). Each window's title bar sits above its
content and names what it shows: *Spectrum probe*, *Kinematic Lasso* or *Box
region* (the tool whose result it holds), *Moment 0*, *1* or *2*, *PV
diagram*. A line too long for a window wraps onto the next row (a product
window keeps three rows under its title and ends the last with … when more
would not fit). Point
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
| **Back of the wrist to the eyes** (as to read a watch), either hand | that wrist's card ([#256](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/256)); never with the palm up |

After a grip, and for a hand just seen, a pinch counts only once the fingertips
have parted, so opening a fist with the thumb still on the index, or a hand
that appears already pinched, does not press anything. The palm counts as up
within about 45° of vertical and stops at about 63°; a closed hand never opens
the menu, and the primary hand's palm does nothing. The sticks and face buttons
have no gesture: their actions (modes, undo, reset view, threshold, opacity,
velocity range) are on the menu. Each tracked hand is drawn whole (#226): a small
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
**colour map**, the **threshold source** (*for this binning* or *full cube*; a
stored choice counts only with `?debug`, see above),
the **refinement** settings (on or off, refined size, JPEG quality, reproject
with depth), the **last selection tool**, whether the **controls guide** was
seen (it is shown on the 2-D page until *Got it*), **left-handed**, the
**button labels** and **haptics** settings, and where each **result window** was
left (relative to where the cube first appears).

Nothing else is stored there: not the token (unless *remember on this device*
is ticked), not the session, not any result. If the browser refuses storage (a
private window), the page works with the defaults. `?refine=0` or `?refine=1`
in the address overrides the stored refinement setting for that visit. The
mode is not kept: the page starts in Explore.

## The numbers on the wrist

With `?debug` in the address (and during the headset benchmark, whose results
they show), the page shows its numbers above the secondary controller, or next
to the cube without controllers, updated four times a second (the 2-D page
shows them too); they are hidden while the menu is open, and `?debug&nohud`
leaves them out. Without `?debug` the headset shows no frame-rate or technical
numbers ([#256](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/256)):

- **FPS** and the **frame** interval (mean and 95th percentile), from the
  headset's frame callbacks, and the share of **missed** frames (an interval
  over 1.5 times the display period);
- **CPU**: the page's own time per frame; **GPU**: the GPU time of the frame
  when the browser offers timer queries (n/a otherwise);
- **samples/voxel** in use and the **max steps/ray** it allows;
- the **volume**, its texture size, the **binning**, the **threshold**;
- the **eye buffer** size, the resolution scale and the foveation level;
- **text**: *quad layers* (the menu, wrist cards, readout, windows and these
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
| Colour map, threshold, colour maximum, opacity, Emission / MIP (Display tab, sticks) | the look; the threshold and colour maximum as data values of the cube | 0.5 s after the last change (a slider dragged is one report) |
| The spatial crop and velocity range (Range tab and mode) | the box in voxels of the full cube (`[[x0, x1], [y0, y1], [z0, z1]]`, ends included), or none | 0.5 s after the last change |
| A spectrum (Select ▸ Spectrum probe) | its CSV in the Workspace Exports (`<cube>_spectrum_x<x>_y<y>.csv`), computed again from the full cube | when it is shown |
| A moment (Measure ▸ Intensity, Velocity, Dispersion), of the selection or of the whole field | its FITS (`<cube>_vr_m<order>.fits`), the same map | when it is shown |
| A PV slice (Measure ▸ PV slice ▸ Draw) | its FITS (`<cube>_vr_pv.fits`) and the path's vertices | 1.5 s after the last point |
| *Isolate*, *Remove* | the cube the backend wrote | when it is made |
| A catalogue over the cube, or a point table or velocity field opened on the page | its file on the backend | when it is shown |
| Another cube on show | its path and binning | when it is shown |

Each product comes with its recipe record (the requests that made it), as in
*Download recipes*. Nothing of this waits on the network: the tools answer as
before, and the reports go after them, from the page's timers, never during a
frame. A report that fails is in *Copy diagnostics* (`hub-failed`), never in
your way. A page that stood down for another window reports nothing.
