# Cubes in a VR headset (Meta Quest, from a Mac)

A spectral cube explored in a Meta Quest 3 driven from a Mac, with no Windows
PC and no OpenXR runtime: the backend serves a web page, the headset's browser
opens it with WebXR, and the headset's own GPU draws a **reduced copy** of the
cube, so turning your head or moving the cube with the controllers never
waits for the network. Everything computed (moments, masks, the Kinematic
Lasso, spectra) still runs on the full cube on the backend.

This is the first step of the immersive work
([#166](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/166),
[#167](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/167)): the
headset renders, and selections made in the headset (a spectrum, the
Kinematic Lasso, a box region) are computed on the full cube by the backend
([#171](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/171), see
[Selections in the headset](#selections-in-the-headset)), and while the view
is still the Mac's GPU renders the full cube for the headset's eyes and the
page blends those frames in
([#170](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/170), see
[Refinement](#refinement-full-quality-frames-from-the-backend)).

| | |
|---|---|
| **Where** | `http://<backend>/vr/`, served by the backend itself |
| **Headset** | Meta Quest 3 (Quest Browser, WebXR); any WebXR browser should work |
| **Without a headset** | the same page in a desktop browser, with the mouse (for development and checks) |
| **Data** | a FITS cube the backend can open |

---

## What you see

The cube is binned by an integer factor on each axis and drawn by ray marching
in the browser (WebGL2): front to back, stopping when a ray is opaque, jumping
over empty regions, with a colour map, a threshold and a clipping box. The
binning in use is written over the box (for example *binned 3×3×1 (mean),
234×234×158 of 700×700×158*).

The axes are in world coordinates: right ascension and declination from the
file's full WCS (read along the central row and column of the field), and the
spectral axis as **velocity in km/s** when it can be expressed so (a velocity
axis in its own convention; a frequency axis with a rest frequency as optical
velocity, cz). Channel 0 is at the back of the box: a cube ordered in frequency
shows its velocity decreasing toward you. Right ascension increases to the
left and declination upwards when the file's pixel axes run that way, as on
the sky.

The depth of the box is a choice, so it is stated: by default **one
full-resolution channel is drawn as long as one full-resolution spatial
pixel**, and the label over the box gives the scale, for example *depth: 1
channel 8.24 km/s = 8.0″ (×1.00)*; when the spectral axis is binned too, the
label adds how many channels a drawn voxel holds (*2 per voxel*). *Velocity
stretch* on the page changes it, and the label follows. Pixels are drawn square
on the sky even when a file's two pixel scales differ. The box spans the cube's
own pixels: when an axis is not a multiple of the binning factor, its last
drawn voxel covers only the pixels that block holds.

## Opening it on the Mac (no headset)

1. Start the backend as usual (the desktop application starts one on
   `http://127.0.0.1:8000`; or by hand, see [Getting started](getting-started)).
2. Open `http://127.0.0.1:8000/vr/` in Chrome, Edge or Safari and paste the
   backend token (the contents of `~/.visivo_token`) into the *Backend token*
   field. Opening `http://127.0.0.1:8000/vr/#token=<token>` works too: the
   page reads the token from the part after `#`, which a browser never sends
   to the server (so it is in no log), and removes it from the address bar.
3. Type the path of a cube on the backend (or *Browse…*), choose the binning,
   and press *Open and load*. Drag to turn the cube, scroll to zoom.

*Synthetic* draws a generated cube of the chosen size without any backend, for
checking the page and measuring frame rates.

## On the Quest 3, over USB (recommended)

WebXR needs a **secure context**: an `https` page, or `http://localhost`.
Forwarding the backend's port to the headset over USB makes the page
`localhost` for the headset, so no certificate is needed.

1. On the headset, turn on developer mode (Meta Horizon app on the phone:
   *Devices → Headset settings → Developer mode*).
2. On the Mac, install the Android platform tools (`brew install
   android-platform-tools`), connect the headset with a USB-C cable, put it
   on and accept *Allow USB debugging*. `adb devices` should list it.
3. Forward the port (8000, or the port your backend uses):

   ```bash
   adb reverse tcp:8000 tcp:8000
   ```

4. Open `http://localhost:8000/vr/` in the headset's browser and paste the
   token into the *Backend token* field (typed with the headset's keyboard,
   or pasted if you have it on the headset's clipboard). Tick *remember on
   this device* to keep it for the next sessions.

   Optionally, the page can be opened from the Mac with the token in it:

   ```bash
   adb shell am start -a android.intent.action.VIEW \
       -d "http://localhost:8000/vr/#token=$(cat ~/.visivo_token)" com.oculus.browser
   ```

   This puts the token on the command lines of processes on the Mac and on
   the headset, where other programs of the same user can read it; use it
   only on a machine and headset that are yours. The page removes the token
   from the headset's address bar and history entry.
5. Load a cube on the page, then press **Enter VR** (or **Enter passthrough**
   to see the cube in your room).

`adb reverse` has to be repeated after the cable is unplugged or the headset
restarts.

## On the Quest 3, over Wi-Fi

The same forwarding works over Wi-Fi once adb is connected wirelessly, and the
page is still `localhost`:

```bash
adb tcpip 5555                      # once, with the cable connected
adb connect <headset-ip>:5555       # then unplug
adb reverse tcp:8000 tcp:8000
```

Opening the backend directly by the Mac's address
(`http://192.168.x.y:8000/vr/`) is **not** a secure context, and the Quest
Browser then offers no WebXR. That route needs the backend on `https` (uvicorn's
`--ssl-keyfile`/`--ssl-certfile` with a certificate the headset trusts) and the
backend listening on the network (`--host 0.0.0.0`, which exposes the whole
API to the network, protected only by the token). Prefer adb over Wi-Fi.

## Controls

| Control | Action |
|---|---|
| Grip (either hand) | grab the cube: it follows the hand |
| Both grips | move, turn and scale the cube between your hands |
| Right stick up/down | threshold up/down |
| Right stick left/right | opacity |
| Left stick up/down | move the velocity slab (clipping along the spectral axis) |
| Left stick left/right | narrow or widen the velocity slab |
| A | next colour map |
| B | reset the clipping box |
| X | emission / maximum intensity |
| Y | show or hide the panel on the left controller |
| Right stick press | frame-rate governor on/off |
| Right trigger | the selection tool along the right controller's ray (see below) |
| Left trigger | next selection tool: spectrum probe, Kinematic Lasso, box region |

The spatial clipping box, the colour range, the stretch and the velocity
stretch are set on the page before entering VR.

## The settings on the page

- **Spatial bin, Spectral bin**: the binning factors; *auto* is the smallest
  factor that brings every axis to at most *Max side* voxels (256 by default:
  HCG 44's 700 × 700 × 158 becomes 3 × 3 × 1, 234 × 234 × 158).
- **Binning**: *mean* (the default) or *max*; see [Fidelity](#fidelity-of-the-reduced-cube).
- **Texture**: 8-bit (default, half the memory) or 16-bit float. The 8-bit
  codes span the reduced cube's whole range, so a cube with a very bright peak
  gets coarse steps near the noise (one peak at 1000 σ makes a code about
  1.3 σ wide): choose 16-bit there, which keeps three significant digits at
  every level.
- **Threshold from**: *for this binning* (default) or *full cube* (the
  threshold the desktop's 3-D view starts from). For mean binning *for this
  binning* is the [default-threshold rule](cube-viewer.md#the-default-threshold)
  applied to the reduced cube, whose noise is lower than the cube's; for max
  binning it is the full cube's threshold, since a block maximum is above it
  exactly when one of the block's voxels is (the noise rule, read from the
  lower tail, would set too low a cut on block maxima and draw noise).
- **Samples / voxel**: samples per reduced voxel along each ray (1 by default).
  With *hold the frame rate* on, the page lowers it when frames are missed and
  raises it back slowly, never above this value; while you hold the cube it
  drops to 60 %.
- **Resolution**: the headset's eye-buffer scale (`framebufferScaleFactor`),
  **Foveation**: the Quest's fixed foveated rendering, **Refresh**: the display
  rate asked of the headset (72, 90 or 120 Hz, if it offers it). Set them
  before entering VR.

## Refinement: full-quality frames from the backend

The headset always draws its reduced cube, every frame, so turning your head
or moving the cube never waits for the network. When the view is **still or
moving slowly**, the page also asks the backend to render the **full cube**
for the headset's current eye poses (both eyes, on the Mac's GPU, issue
[#170](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/170); the
protocol is [Stereo frames for a headset](../backend-api/render.md#stereo-frames-for-a-headset)),
and blends the frame that comes back over its own picture. When you move
again, the refined frame fades out within 60 ms and the reduced cube carries
the motion alone.

**What is refined.** The backend draws with the page's own look: the colour
map, threshold, colour maximum, stretch, opacity, emission or maximum
intensity, the clipping box and the velocity slab, and the velocity stretch.
The page sends them as the session's transfer function and crop on the full
cube's values (`set_transfer_functions`, `set_crop`), so a refined frame
differs from the headset's only by what the binning and the headset's ray
step take away. The opacity is matched per path length: a full-resolution
voxel absorbs 1/f of a reduced one, f the geometric mean of the binning
factors. Any change of the look makes the refined frames in flight and on show
stale: they are dropped, and the next still pose is rendered with the new
look.

**When it asks.** The page measures how fast the eye moves *relative to the
cube* (head motion and moving the cube with the controllers count alike),
smoothed over 35 ms:

| State | Angular speed | Speed | Refinement |
|---|---|---|---|
| moving | above 30 °/s, or | above 0.15 m/s | none asked; the refined frame fades out |
| slow | below 15 °/s and (for 60 ms) | below 0.06 m/s | asked for the current pose, at most two in flight |
| still | below 3 °/s and (for 60 ms) | below 1 cm/s | asked once the pose has changed by 0.2° or 1.5 mm |

Head tracking held still jitters at a few degrees per second at most; slow
looking around is 5–20 °/s; a turn of the head is 50–300 °/s. Between 15 and
30 °/s the previous state is kept (hysteresis), so the image does not flicker
at the boundary. When the view is still and its refined frame is on show, no
new pose is sent and the backend renders nothing.

**How it is drawn.** Each frame from the backend carries the pose it was
rendered for and a small depth image (the first sample above the threshold
along each ray, one value per 4 × 4 pixels). The page reprojects it to the
current eye pose: a mesh with a vertex per depth sample is unprojected
through the frame's projection, moved by the change of the eye relative to
the cube since the frame's pose, and projected again; rays with no sample
take the depth of the cube's centre. A pure head turn needs no depth (any
depth gives the same picture); a head translation does, because the near and
far parts of the cube shift by different amounts (parallax: 1 cm sideways at
0.75 m moves the front of a 0.6 m cube about 0.7° more than its back, about 12
pixels on the headset). A frame is shown while the eye is within 6° and 4 cm
of the frame's pose; beyond that it fades out until a new one arrives. The
measured gain of depth over a plane is in the table below.

The backend composites its frame over black (over the page's own background
colour in a desktop browser). Where the reprojected frame lands, it
**replaces** the headset's picture of the volume, with a weight that fades in
over 120 ms (out over 60 ms): `w · refined + (1 − w) · reduced`. It is a
replacement, not an additive blend, because both are pictures of the same
emission; where the backend saw no emission (fog of the binning, noise drawn
by the reduced cube) the refined frame shows none. Outside the frame (after
a head turn, at the edges) the headset's own picture shows.

**Not in passthrough.** In *Enter passthrough* the page shows its own
rendering only (the panel says *refine off in passthrough*). The backend's
frame has no transparency (it is composited over black), so over the room it
would either darken the room where the headset draws emission that the full
cube does not have, or glow where only the full cube has emission. Refinement
is also off while the headset benchmark runs.

**Selections stay visible.** The backend's frame has no selection in it: the
box outline, the axes, the labels, the controller rays and the panels are
drawn after it. While a refined frame is on show (fading included), the
selection mask (green) is left out of the headset's own volume and drawn
once, by the headset, on top of the refined frame (only the selected
voxels). The mask shown over a refined frame is therefore not hidden by
emission in front of it, as it is in the reduced rendering.

**The render session.** The page opens a render session of its own for the
cube (`POST /v1/render/create_session`, waiting in the queue if every slot
is busy), on one WebSocket; the desktop's sessions are not used. It is closed,
and its render slot freed, when another cube is loaded, when refinement is
turned off (turning it on again opens a new one) and when the page is closed
or left (it is opened again when the browser brings the page back); otherwise
the backend's idle timeout closes it. A synthetic cube has no session
(*refine unavailable: no backend cube*). A cube the backend would draw out of
core (larger than its in-core limit, `VISIVO_RENDER_OUT_OF_CORE_BYTES`)
cannot give stereo frames yet: the page learns it from `reduced_info`
(`render_out_of_core`) and asks for no session at all, so no brick pyramid is
built for it; the panel says *refine unavailable: the backend draws this cube
out of core*. A clipping box the backend would refuse is not sent (nor any
pose with it), and the panel says why.

**Settings** (on the page, section above *Selection tool*):

- **refine with the backend's full cube**: on by default (`?refine=0` in the
  address turns it off at start).
- **Refined size**: the size of each refined eye image relative to the eye
  buffer (headset) or the window (desktop browser): 1× by default, the eye
  buffer's own resolution (about 1680 × 1760 per eye on a Quest 3 at
  resolution 1×), at most 2560 pixels a side. A smaller frame comes sooner
  and lets the backend follow slow motion at a higher rate (the Mac renders
  1024 × 1024 per eye at about 45 frames per second on HCG 44,
  [#169](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/169)), but it
  is blurred when stretched over the eye: at 0.6× the refined image is
  *less* similar to the full-quality reference than the headset's own
  rendering in SSIM (see the measurements), so 0.6× is for slow networks or
  large cubes only.
- **JPEG**: 95 by default.
- **reproject with depth**: on by default; off moves the frame as a plane
  through the cube's centre (for comparisons).

In a desktop browser the same path runs with the mouse camera as both eyes
(the left eye's image is shown), so all of this can be checked without a
headset.

**On the panel** (the line starting *refine*):

- the state: *off*, *connecting*, *waiting* (no usable refined frame),
  *refined NN %* (the fade weight), or *unavailable* with the reason; then
  the motion state (*moving*, *slow*, *still*);
- **age**: time since the pose of the frame on show was sent, in ms;
- **remote**: refined frames received per second;
- **pose->frame**: from sending a pose to its frame decoded, median and 95th
  percentile;
- **stop->refined**: from the view leaving *moving* to the first refined frame
  of a pose sent after that moment shown, and to it fully faded in (*full*);
- **server**: the backend's render and total time for the last frame, its
  size, and the page's texture upload time.

### Measured on the Mac (desktop browser, local backend)

HCG 44 (700 × 700 × 158), headless Chrome on an M5 Max (ANGLE Metal), a
1600 × 913 window, the backend on the same Mac
(`validation/vr_refinement.mjs`, metrics by `validation/vr_refinement.py`,
images and numbers in `validation/out/vr_refinement/` and
`validation/out/vr_refinement.md`).

**Image quality** at one still view, against the backend's own reference of
the full cube (full window size, JPEG 100, rays sampled every quarter voxel),
on the pixels of the emission:

| Page draws | Shown | PSNR | SSIM |
|---|---|---|---|
| 3 × 3 × 1 (auto, 234 × 234 × 158) | page alone | 33.0 dB | 0.965 |
| | refined, 1×, JPEG 95 (default) | **46.0 dB** | **0.988** |
| | refined, 0.6×, JPEG 85 | 37.8 dB | 0.957 |
| 2 × 2 × 1 (350 × 350 × 158) | page alone | 36.7 dB | 0.982 |
| | refined, 1×, JPEG 95 (default) | **45.0 dB** | **0.986** |
| | refined, 0.6×, JPEG 85 | 36.6 dB | 0.950 |

At 0.6× the refined image has a better PSNR than the page's but a worse SSIM:
stretched over the window, it loses fine structure.

**Reprojection**: a refined frame for one pose drawn at another, against the
backend's frame for the second pose (PSNR on the emission, 3 × 3 × 1). An
*orbit* turns the eye about the cube's centre at 1 m (1.7 cm of translation
per degree); a *turn* turns the head in place.

| Motion | Frame as it was | Moved as a plane | Moved with depth | Page alone |
|---|---|---|---|---|
| orbit 0.5° | 37.6 dB | 36.6 dB | **42.6 dB** | 31.5 dB |
| orbit 1° | 32.4 | 31.4 | **38.8** | 31.4 |
| orbit 2° (3.5 cm) | 27.8 | 26.7 | **34.8** | 31.7 |
| orbit 4° (7 cm) | 24.3 | 23.3 | 30.4 | **32.0** |
| head turn 2° | 19.4 | **46.0** | 45.9 | 33.7 |
| head turn 5° | 19.9 | **46.1** | 46.0 | 34.0 |

A head turn needs no depth; a translation does: with depth the reprojected
frame stays better than the page's own picture up to about 3.5 cm of eye
motion relative to the cube, and is worse at 7 cm, hence the 4 cm limit.
(With 2 × 2 × 1 the page's own picture is better than the reprojected frame
from 2° of orbit on: 35.5 against 37.7 dB at 2°, 35.7 against 33.2 at 4°.)

**Times** (15 turns of the view at 90 °/s for 0.6 s, then still):

- camera stopped to the refined frame first shown: **268 ms** median
  (270 ms 95th percentile), fully faded in **383 ms**; of this, the page takes
  about 185 ms to decide the view has stopped (smoothing and hold), and the
  frame about 80 ms (the panel's *stop->refined* counts from the decision:
  83 ms and 200 ms). A second run, with the Mac busier (server 108 ms render
  instead of 37 ms), gave 408 ms and 516 ms;
- pose sent to frame decoded: 72 ms median, 83 ms 95th percentile (two eyes
  of 1600 × 913: render 37 ms, depth 5 ms, JPEG 18 ms on the backend; 65 KB a
  frame); JPEG and depth decode in the browser 4.4 ms (off the frame loop),
  texture upload 1.3 ms in the frame;
- the page's own frame: GPU 1.31 ms without, 1.47 ms with the refined frame
  composited (+0.16 ms for a 400 × 229 mesh per eye), CPU 0.10 ms either way:
  compositing does not slow the headset's own rendering measurably here.

On the Quest 3 the eye buffer at resolution 1× is 1680 × 1760, about twice
the pixels of this window for both eyes: expect the backend to take about
twice as long per frame (pose to frame around 130 ms over USB), and the
upload and the mesh (420 × 440 per eye) to cost more on the headset's GPU;
the panel shows both.

### What to check on the Quest 3

1. Start the backend on the Mac and connect the headset over USB
   (`adb reverse tcp:8000 tcp:8000`), open `http://localhost:8000/vr/` with
   the token, load HCG 44 (auto binning) and check that *refine with the
   backend's full cube* is ticked. Enter VR.
2. Hold your head still for a second: the panel on the left controller must
   go from *waiting* to *refined 100 %*. Read **stop->refined** (both values),
   **pose->frame** (median, p95), **remote** fps, **server** render ms and
   **upload** ms. Do this five times and note the values.
3. Look at the cube while it refines: the image should get sharper (finer
   structure, no binning blocks) **without jumping**: if it shifts or doubles
   when it fades in, note in which direction and how much (that is a pose or
   axis mismatch).
4. Turn your head quickly left and right: there must be **no lag and no
   smearing** of the cube: the refined frame must disappear at once and the
   reduced cube follow your head. Read **FPS** and **missed %** on the panel
   while turning, with refinement on and off (untick it on the page, or
   compare with `?refine=0`): they must be the same.
5. Move your head slowly sideways (a few cm per second) while looking at the
   cube: the refined frame should stay and follow the cube without visible
   swimming; untick *reproject with depth* on the page and repeat, and note
   which looks steadier.
6. Make a Kinematic Lasso selection, then hold still: the green mask must
   stay visible over the refined frame; the box outline and labels too.
7. Move the velocity slab with the left stick, then hold still: the refined
   frame must show the same slab (no emission outside it).
8. Repeat 2 and 4 over Wi-Fi (adb connected wirelessly) and note
   **pose->frame** and **stop->refined**.

## Selections in the headset

The right controller casts a ray (a thin white line); a small white cross
shows where the trigger would land. On the Mac the same tools work with the
mouse: a click (press and release without dragging) is the trigger, a drag
still turns the cube. The tool is chosen with the left trigger or on the page
(*Selection tool*). Everything below is computed by the backend on the full
cube, through the routes the desktop uses; the headset only draws.

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
edge cannot win; the panel says which rule was used (*its brightest* or *its
brightest 3×3 mean*).

The results panel (to the right of the cube in the headset, under the page's
controls on the Mac) shows the seed's position: right ascension and
declination in ICRS from the file's full WCS (`/v1/datasets/pixel_to_sky`,
what the desktop sends over SAMP), the velocity of its channel in km/s with its
convention, the pixel and channel, and the reduced voxel.

| Tool | Trigger | What the backend computes | Panel |
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
parameters give the same mask as the desktop, voxel for voxel (see below).

A box corner is placed at the right controller's tip when the tip is inside
the cube (reach into it): two such corners make a 3-D box. A corner placed by
pointing from outside the cube, or with the mouse, takes the right ascension
and declination of the point the ray picks (the first drawn sample, or the
middle of the ray inside the box when nothing is drawn) and no depth; the box
then spans the channels of the velocity slab (the clipping range on the
spectral axis, left stick). The sky rectangle is therefore exactly the one you
pointed at only when you look down the spectral axis; from an oblique view the
two picked points may lie at other depths than you meant. The box measured is
outlined in pink after the answer, and the panel gives its pixel and channel
range: check it there. The box is measured in one call on the full cube (every
voxel, read one channel at a time): 22 ms for a 66 × 29 × 158 box of HCG 44.
The rms is the population standard deviation, as the region statistics give
it; no median; the flux is each channel's sum over its own beam area times its
own width.

The lasso's green overlay stays while the other tools are used (the panel
says so); *Clear*, a new lasso or a new cube removes it. Switching tool drops a
request still in flight: it neither draws nor records.

**Recipes.** Each spectrum, lasso selection and box is recorded as the desktop
records its products (`visivo-recipe/1`, see
[Kinematic Lasso](kinematic-lasso.md)): the dataset's path, the calls with their
requests under the API's field names (for the lasso, the grow with exactly the
request the desktop's recipe holds, then the statistics of the mask it
returns), the parameters as the desktop's Provenance tab labels them, and where
it was measured. *Download recipes (.json)* saves all of them, *Copy last
recipe* copies the latest. A replay opens the path and posts the same
requests: the selection is the one the desktop shows for that seed.

**Identity with the desktop.** `validation/vr_selection_identity.py` runs the
page's own picking code (under node) on the reduced cube of HCG 44 and the
requests it builds, and compares them with the desktop's two paths for
NGC 3187 (the level field the Reach slider thresholds, and its recipe's grow).
Result (`validation/out/vr_selection_identity.md`): for rays along the
spectral axis and from the page's default view, with either seed rule, the VR
selection is the desktop's (Dice 1.000 against both, 13 216 voxels, the same
as from the paper's seed at the source's peak); the spectrum is the desktop's
and the cube's own line of sight; against the references the selection scores
Dice 0.610 with the published mask of Serra et al. (2013) and 0.844 with our
SoFiA-2 run, the paper's values for this source; flux 6.061 Jy km/s.

**Timings** (backend on an Apple M5 Max, page in Chrome on the same Mac,
HCG 44 binned 3 × 3 × 1): pick and seed 40 ms (the ray pick itself takes under
0.1 ms, so the cross follows the ray every other frame), spectrum 45-85 ms,
lasso 570-710 ms plus 40-80 ms of statistics, box over 158 channels 22 ms.
The green overlay costs GPU time: 0.64 ms without it, 0.84 ms with it, for one
1600 × 1000 view on that Mac; the headset's own numbers are on its panel.

## The numbers on the panel

The panel (on the left controller, or next to the cube without controllers)
and the page show, updated four times a second:

- **FPS** and the **frame** interval (mean and 95th percentile), from the
  headset's frame callbacks, and the share of **missed** frames (an interval
  over 1.5 times the display period);
- **CPU**: the page's own time per frame; **GPU**: the GPU time of the frame
  when the browser offers timer queries (n/a otherwise);
- **samples/voxel** in use and the **max steps/ray** it allows;
- the **volume**, its texture size, the **binning**, the **threshold**;
- the **eye buffer** size, the resolution scale and the foveation level;
- the **selection tool** in use and the time of the last pick, lasso,
  statistics, spectrum and box requests.

## Measuring the headset: the benchmark

**Run the headset benchmark** enters VR and draws generated cubes of 128³,
256³, 384³ and 512³ voxels, each at 0.5, 1 and 2 samples per voxel (and the
loaded cube, if there is one), turning slowly 0.75 m in front of you,
0.75 s to settle and 3 s measured each, with the governor off. It ends by
itself (about a minute) and leaves VR; the page then shows a table (FPS, frame
mean and 95th percentile, missed frames, GPU and CPU time, display rate, eye
buffer, foveation) and *Copy benchmark results*. Keep your head still while it
runs.

For reference, the same page on a Mac (Apple M5 Max, Chrome, two
1680 × 1760 eye buffers, 90° field of view; GPU time of a stereo frame):

| Cube | Samples/voxel | Skipping on | Skipping off | Mean steps per ray (on / off) |
|---|---|---|---|---|
| 128³ | 1 | 0.3 ms | 1.0 ms | 17 / 70 |
| 256³ | 1 | 0.6 ms | 2.0 ms | 31 / 138 |
| 256³ | 2 | 0.7 ms | 4.1 ms | 36 / 277 |
| 384³ | 1 | 0.8 ms | 3.0 ms | 43 / 198 |
| 512³ | 1 | 1.0 ms | 4.5 ms | 56 / 262 |
| 512³ | 2 | 1.3 ms | 8.8 ms | 63 / 524 |

A Quest 3 is several times slower than this GPU; its own numbers come from the
benchmark above. (`?bench` on the page URL reruns this desktop measurement.)

## Fidelity of the reduced cube

The headset draws the reduced cube, the analysis uses the full one, so what the
reduction hides was measured (`validation/vr_reduction_fidelity.py`, table in
`validation/out/vr_reduction_fidelity.md`): for each binning, the share of a
reference mask's voxels and flux that is still drawn above the threshold, and
the voxels drawn outside it. Flux kept, 8-bit texture, the page's default
threshold (for mean binning, with the full cube's threshold in brackets; max
binning uses the full cube's threshold):

| Binning | HCG 44 vs Serra+2013 | HCG 44 vs SoFiA-2 | WALLABY NGC 3137 vs SoFiA-2 |
|---|---|---|---|
| none | 56.7 % | 68.7 % | 69.7 % |
| 2×2×1 mean | 60.0 % (56.3) | 72.6 % (68.2) | 72.0 % (69.2) |
| 3×3×1 mean | 61.5 % (54.7) | 74.0 % (66.1) | 73.6 % (69.0) |
| 4×4×1 mean | 63.6 % (53.5) | 75.7 % (64.5) | 75.6 % (67.3) |
| 8×8×1 mean | 68.8 % (43.2) | 79.2 % (51.8) | 80.0 % (64.0) |
| 3×3×1 max | 67.1 % | 79.9 % | 75.9 % |
| 8×8×1 max | 76.2 % | 87.0 % | 83.5 % |

How to read it:

- With **mean** binning and the threshold set on the reduced cube, the reduced
  cube shows *more* of the faint emission than the full cube does at its own
  threshold, because averaging lowers the noise; with the full cube's
  threshold it shows a little less (the faint edges are diluted).
- **Max** binning keeps more flux and draws each detected voxel at its full
  brightness. With the full cube's threshold it draws little noise: 2 % of the
  drawn voxels on HCG 44 hold no voxel of the Serra et al. mask at 4×4×1, 6 %
  at 8×8×1 (5 % and 17 % with mean binning). With the noise rule applied to
  the reduced cube instead, the first version of the page, it was 16 % and
  48 %.
- Coarser binning draws each detected voxel as a larger block: the full-size
  voxels outside the SoFiA-2 mask covered by drawn blocks are 3 % of the mask's
  size at 3×3×1 mean, 6 % at 4×4×1 and 41 % at 8×8×1 (on HCG 44; 6 %, 11 %
  and 64 % with max binning). The shapes are blurred, not lost.
- The 8-bit texture changes the flux kept by about half a percentage point
  against the 16-bit one (56.7 against 57.1 % unbinned on HCG 44).

The defaults (mean, threshold for this binning, 3×3×1 on HCG 44) keep much of
the faint emission for little noise and blur; max binning is the choice for
compact sources that mean binning dilutes. A selection made in the headset
is computed on the full cube, not on what is drawn.

## Limits

- One cube at a time: at most 2048 reduced voxels on a side (the GPU's 3-D
  texture limit), 64 Mi voxels and 64 MiB of texture (so 32 Mi voxels in
  16 bits), for what a headset browser can allocate; the backend refuses a
  larger reduced cube before reading anything, and the page checks its GPU's
  limit before asking. Choose a larger binning.
- In the headset: a spectrum, the Kinematic Lasso (one seed: no add or carve
  seeds, no hand refinement, no Isolate or Remove, which stay on the desktop)
  and a box region; no moments yet. Refined frames need a cube the backend
  holds in core (not one drawn out of core from its brick pyramid), and are
  not cropped in isosurface mode (the page does not use it). The desktop has no recipe import: a VR recipe is replayed by
  posting its requests, as a desktop recipe is.
- The labels follow the central row and column of the field; far from them on
  a wide projected field the true coordinate of a pixel differs a little.
- The page reads the whole cube once to reduce it (0.6 s for HCG 44); a cube
  of hundreds of GB takes correspondingly long.
- Comfort: prefer short, seated sessions; passthrough mode helps.
