# Technical reference

The page's settings, what the reduced cube keeps, how refinement works, the
routes the page calls, and everything measured so far, with the checks still
to run on a Quest 3.

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
- **Threshold from**: *for this binning* or *full cube* (the threshold the
  desktop's 3-D view starts from). Every cube from the backend starts from the
  *full cube*'s threshold, as the desktop does
  ([#257](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/257)); a
  synthetic cube, which has no full cube, from its own. A choice made here
  applies to the cube on show; it is kept in this browser but used for the
  next cubes only with `?debug` in the address, where the hand menu shows the
  setting too (More → *Technical settings*). Until #257 a cube started from
  the threshold *for this binning* when refinement was off on a small cube.
  The refined frames are drawn on the full cube, where the threshold for a
  coarse binning lets the cube's noise through as fog (see
  [Large cubes](#large-cubes-from-the-brick-pyramid)).
  For mean binning *for this
  binning* is the [default-threshold rule](../cube-viewer.md#the-default-threshold)
  applied to the reduced cube, whose noise is lower than the cube's; for max
  binning it is the full cube's threshold, since a block maximum is above it
  exactly when one of the block's voxels is (the noise rule, read from the
  lower tail, would set too low a cut on block maxima and draw noise).
- **Samples / voxel**: samples per reduced voxel along each ray (1 by default).
  With *hold the frame rate* on (the default; it can be turned off in the
  headset too, see [Controls](controls.md)), the page lowers it when frames
  are missed and raises it back
  slowly, never above this value; while you hold the cube it drops to 60 %.
  Samples per voxel are given up first, all the way to the governor's floor
  (0.25). Only when they are at that floor and more than 10 % of the frames are
  still missed over 3 s does the page make the **eye buffer** smaller, as a
  last resort: one step at a time through the scale factors 1.25, 1 and 0.85
  (absolute values of *Resolution*, starting below the one chosen; **never
  below 0.85**), and it does not raise it again in that VR session (each
  change costs a hitch); the next *Enter VR* starts from the chosen
  resolution. The menu and the
  [numbers on the wrist](controls.md#the-numbers-on-the-wrist) say so, and the
  diagnostics' *quality* line and the measurement record's *xr-start* event
  name the policy (*72 Hz; samples/voxel first (1 down to 0.25), then the eye
  buffer as a last resort (1× down to 0.85× at most)*). Until #226 the eye
  buffer went down to 0.7× from 0.5 samples per voxel on: on the first Quest 3
  session that made the text and the cube blurry for about 89 fps at 90 Hz.
- **Memory budget**: before the reduced cube is read, the page checks that it
  fits: the GPU's 3-D texture side, and what the cube holds on the GPU
  against what this device gives one cube (a headset: 256 MiB with 8 GB of
  memory, 128 MiB with 4 GB; a computer's browser: 1 GiB). On the GPU a cube
  holds its texture (1 byte a voxel at 8 bits, 2 at 16 bits), the selection
  mask's storage over the same grid (1 byte a voxel, allocated with the cube:
  see [GPU context loss](troubleshooting.md#gpu-context-loss-at-the-first-lasso-on-a-quest-3))
  and the empty-space grid (4 bytes per 512 voxels). A cube over it is
  refused with the reason (*… needs 300 MiB of GPU memory (200 MiB of
  texture, 100 MiB for the selection mask; 500 MiB while loading); this
  headset allows 256 MiB for one cube*): choose a larger binning. The
  headset's limit is a conservative figure, not yet measured on a Quest 3.
  The same share holds the room kept for cubes in VR (issue #235): two
  cubes of *Max side* voxels per axis and one mask, allocated before VR, into
  which a cube that fits is written (it then needs no memory of its own); a
  larger cube is held in storage of its own beside it, or, when both would
  not fit, alone (and then waits for the end of a VR session).
  The backend's own caps (64 Mi voxels, 64 MiB of texture) keep any cube it
  serves under 129 MiB on the GPU.
- **Resolution**: the headset's eye-buffer scale (`framebufferScaleFactor`),
  **Foveation**: the Quest's fixed foveated rendering, **Refresh**: the display
  rate asked of the headset (**72 Hz by default**, or 90 or 120 Hz, if it
  offers it: 72 Hz leaves 13.9 ms a frame, so the cube keeps more samples per
  voxel). Set them before entering VR.
- **Text in the headset**: the hand menu, the wrist cards and the toast, the
  cursor readout, the result windows, the numbers on the wrist (`?debug`) and
  the message card
  are WebXR composition layers (quad layers) where the browser has them (Quest
  Browser does): the headset's compositor draws them at the display's own
  resolution, so they stay sharp whatever eye-buffer scale the governor
  chose. Each is the same canvas, at the same place and size, as the panel the
  page would draw itself, so pointing at them works the same way. The Quest
  Browser draws a quad layer from minus to plus its `width` and `height`
  about its centre, so there the page gives it half the panel's size (the
  diagnostics line ends in *±width*); elsewhere it gives the full size, as
  the specification words it. Until #267 the Quest got the full size too and
  every overlay showed at twice its size, a window's title bar inside its
  content. `?layerhalf=1` or `?layerhalf=0` forces half or full size, should
  a browser read them the other way (a panel then looks twice or half the
  size of where the ray presses). Where the
  browser has no layers, or a layer fails, the page draws them in the scene as
  before (the diagnostics' *text in the headset* line says which);
  `?nolayers` in the address forces that, `?layerflip=0` draws the layers'
  text without flipping it (should a browser show it upside down). Layers are
  drawn over the scene: where a hand or a ray passes in front of a panel, the
  panel stays on top. A layer's pixels are copied only when its canvas
  changed; its place every frame. On the Mac, with a WebXR stand-in
  (`validation/vr_lifecycle.mjs --xrfake`), the page's CPU time for a stereo
  frame is about the same with layers and without (0.77–0.87 and 0.65–0.82
  ms mean over three runs).

The refinement's own settings are under
[Refinement](#refinement-full-quality-frames-from-the-backend); which settings
the browser keeps between visits is under
[Settings kept in this browser](controls.md#settings-kept-in-this-browser).

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

The defaults (mean binning, 3×3×1 on HCG 44) keep much of the faint emission
for little noise and blur; max binning is the choice for compact sources that
mean binning dilutes. These fractions were measured with the threshold *for
this binning*; with refinement on, the page now starts from the *full cube*'s
threshold (see *Threshold from* above), the bracketed figures. A selection made
in the headset is computed on the full cube, not on what is drawn.

## Large cubes: from the brick pyramid

A cube larger than the backend's in-core limit (`VISIVO_RENDER_OUT_OF_CORE_BYTES`,
6 GiB as 32-bit floats; `VISIVO_VR_PYRAMID_BYTES` sets the page's own limit)
gets its reduced cube and its refined frames from its
[brick pyramid](../../brick-pyramid.md), the same file the desktop's out-of-core
view uses, not from a pass over the whole cube. The whole cube is still read
in three cases: max binning, *Read from: whole cube*, and a pyramid the
backend cannot build (a directory it cannot write, a failed build: the status
line then says so, and the whole cube is read in bounded pieces).

**The first time.** When the cube has no pyramid yet, the backend builds it
in a process of its own and the page shows *Building the cube's brick pyramid
on the backend… N %*; every view of the cube shares that build (the
desktop's too), and it is done once per cube (a changed file gets a new
one). The status line then says *brick pyramid built in N s*. A build whose
progress does not move for 5 minutes, or that takes over 2 hours, is given up
by the page, which says to choose *Read from: whole cube*. To skip the wait,
build it beforehand (`python -m app.brick_pyramid build cube.fits`, see
[Building](../../brick-pyramid.md#building)).

**The reduced cube.** Level K of the pyramid is the cube averaged over blocks
of 2^K pixels on every axis. The page's binning is read from the coarsest
level that gives it exactly, binned further by integer factors. When that
level is larger than 1 GiB (`VISIVO_VR_PYRAMID_READ_BYTES`):

- automatic factors are read from the finest level within 1 GiB, rounded up
  to its blocks, but never more than twice what was asked on any axis: past
  that, a finer level is read, in pieces of at most 128 MiB, up to 16 GiB
  (`VISIVO_VR_PYRAMID_MAX_READ_BYTES`). Only when even that level is larger
  is the coarse level kept, and the spectral axis may then be binned up to
  2^K times more than asked (`pyramid.asked_factors` in `reduced_info`
  gives both). A 2400 × 2400 × 520 cube asks 10 × 10 × 3 and gets
  12 × 12 × 4 from level 2; a 20000 × 20000 × 300 cube asks 79 × 79 × 2 and
  gets 80 × 80 × 4 from level 2 (7.5 GB read in pieces), not 80 × 80 × 8 from
  level 3;
- factors you give are kept exactly, from a level they are multiples of; when
  that level is larger than 1 GiB they are refused, with the nearest factors
  that would fit (for example *factor_xy 32* for 33).

The label says where the cube came from: *binned 12×12×4 (mean, pyramid
level 2)*. What this changes:

- The voxels cover exactly the blocks of the factors shown, as a reduction of
  the whole cube does, so picking, the lasso seed, the box and the green mask
  map to the same full-cube pixels and channels (tested on odd sizes and
  partial last blocks, `backend/tests/test_vr_reduced_pyramid.py`).
- Their values are the full cube's block means. A level voxel is a mean of
  means (each of its sub-blocks counts alike, whatever it holds), so the level
  is weighted by the pixels under each voxel, and every level voxel next to a
  blank one, which may hold only a few finite pixels, is recomputed from
  level 0 (its exact sum and count). The repair is all or none: the
  level-0 reads it needs are counted from the border voxels' positions
  before any is made, and when they exceed 1 GiB per cube
  (`VISIVO_VR_PYRAMID_REPAIR_BYTES`) none is repaired (the header
  `X-Visivo-Border-Voxels` gives the border voxels and how many were
  repaired: all of them, or 0). On noise cut by a circle of blanks this gives the whole cube's block
  means to float precision; without it, voxels on the cut were up to 8.6 times
  the reduced noise, and the noise up to 1.45 times higher. Not recomputed: blank
  pixels with no blank level voxel next to them (isolated blanks), and the
  sub-blocks of a partial last level voxel, which keep equal weights; when
  the repair is over its budget, every border voxel keeps the level's value.
- Max binning is not available from the pyramid (its levels are means):
  *auto* then reads the whole cube.
- The threshold *of the full cube* is the default (see *Threshold from*),
  taken from a sample of full-resolution bricks, as the desktop's
  out-of-core view takes it. The threshold *for this binning* sits far inside
  the full cube's noise with coarse binning, and the refined frames would
  show that noise as fog.

**Refinement.** The backend draws the headset's eyes from one whole level of
the pyramid, the finest within its GPU budget (`VISIVO_RENDER_OOC_BUDGET_BYTES`,
2 GiB) and the GPU's largest 3-D texture side on every axis: level 1
(1200 × 1200 × 260) of the 2400 × 2400 × 520 cube. It reads it once, when
the first pose arrives (0.4 to 0.7 s here), and keeps it for every pose, so a
moving head never makes the backend read other blocks; after the headset's
session stops, its mono view chooses its own blocks again. The placement,
threshold, colours, clipping box, velocity slab and depth image are those of
an in-core cube, and the ray step is in voxels of the level drawn; the HUD's
*server* line adds *pyramid level 1*. At 1680 pixels per eye a level of about
1000 voxels across already gives about one voxel per pixel for a cube at
arm's length; leaning into the cube shows the level's voxels. The desktop's
out-of-core view, which chooses finer blocks near the camera, is not used for
the headset: its blocks change with the view, and a stereo frame takes its
depth from one block.

**Selections** run on the full cube through the same routes. Their memory
does not grow with the cube: the lasso works in a window of at most 513³
voxels around the seed by default (`compute/selection.py`), which bounds it
(about 2.5 GB more resident memory measured below); the statistics of a mask
or a box read their box one channel at a time, and a box over 64 Mi voxels is
refused.

## Refinement: full-quality frames from the backend

The headset always draws its reduced cube, every frame, so turning your head
or moving the cube never waits for the network. When the view is **still or
moving slowly**, the page also asks the backend to render the **full cube**
for the headset's current eye poses (both eyes, on the Mac's GPU, issue
[#170](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/170); the
protocol is [Stereo frames for a headset](../../backend-api/render.md#stereo-frames-for-a-headset)),
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
measured gain of depth over a plane is under
[Refined frames on the Mac](#refined-frames-on-the-mac-desktop-browser-local-backend).
Frames that come without depth are described under
[Frames without depth](cluster.md#frames-without-depth).

The backend composites its frame over black (over the page's own background
colour in a desktop browser). Where the reprojected frame lands, it
**replaces** the headset's picture of the volume, with a weight that fades in
over 120 ms (out over 60 ms): `w · refined + (1 − w) · reduced`. It is a
replacement, not an additive blend, because both are pictures of the same
emission; where the backend saw no emission (fog of the binning, noise drawn
by the reduced cube) the refined frame shows none. Outside the frame (after
a head turn, at the edges) the headset's own picture shows.

**Not in passthrough.** In *Enter passthrough* the page shows its own
rendering only (the menu says *paused in passthrough*). The backend's
frame has no transparency (it is composited over black), so over the room it
would either darken the room where the headset draws emission that the full
cube does not have, or glow where only the full cube has emission. Refinement
is also off while the headset benchmark runs. In both cases the page **closes
its render session** (so it holds no render slot or GPU memory on the backend
meanwhile) and opens a new one when you leave passthrough or the benchmark
ends.

**Selections stay visible.** The backend's frame has no selection in it: the
box outline, the axes, the labels, the controller rays, the hand menu and the
result windows are drawn after it. While a refined frame is on show (fading
included), the selection mask (green) is left out of the headset's own volume
and drawn once, by the headset, on top of the refined frame (only the selected
voxels). The mask shown over a refined frame is therefore not hidden by
emission in front of it, as it is in the reduced rendering.

**The render session.** The page opens a render session of its own for the
cube (`POST /v1/render/create_session`, waiting in the queue if every slot
is busy), on one WebSocket; the desktop's render sessions are not used (the
page shares the desktop's *backend* session, its datasets, when opened from
the desktop). It is closed, and its render slot freed, when another cube is
loaded, when refinement is turned off (turning it on again opens a new one),
in passthrough and during the benchmark, and when the page is closed
or left (it is opened again when the browser brings the page back); otherwise
the backend's idle timeout closes it. A synthetic cube has no session
(*refine unavailable: no backend cube*). A cube the backend draws out of core
(larger than its in-core limit, `VISIVO_RENDER_OUT_OF_CORE_BYTES`) is refined
too, from one level of its brick pyramid: see [Large cubes](#large-cubes-from-the-brick-pyramid).
A clipping box the backend would refuse is not sent (nor any
pose with it), and the menu says why.

**Settings** (on the page, section above *Selection tool*; with `?debug` also
on the hand menu's More → *Technical settings*; all kept in this browser):

- **refine with the backend's full cube**: on by default (`?refine=0` in the
  address turns it off for that visit; `?refine=0` or `?refine=1` overrides
  the stored setting for that visit).
- **Refined size**: the size of each refined eye image relative to the eye
  buffer (headset) or the window (desktop browser): 1× by default, the eye
  buffer's own resolution (about 1680 × 1760 per eye on a Quest 3 at
  resolution 1×), at most 2560 pixels a side. A smaller frame comes sooner
  and lets the backend follow slow motion at a higher rate (the Mac renders
  1024 × 1024 per eye at about 45 frames per second on HCG 44,
  [#169](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/169)), but it
  is blurred when stretched over the eye: at 0.6× the refined image is
  *less* similar to the full-quality reference than the headset's own
  rendering in SSIM (see the [measurements](#refined-frames-on-the-mac-desktop-browser-local-backend)),
  so 0.6× is for slow networks or large cubes only.
- **JPEG**: 95 by default.
- **reproject with depth**: on by default; off moves the frame as a plane
  through the cube's centre (for comparisons).

In a desktop browser the same path runs with the mouse camera as both eyes
(the left eye's image is shown), so all of this can be checked without a
headset.

**On the numbers on the wrist** (the line starting *refine*; see
[The numbers on the wrist](controls.md#the-numbers-on-the-wrist)):

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
- **via**: how the refined colour travels: *via JPEG* with the median network
  time of the JPEG frames (and the threshold above which the video is tried),
  *via JPEG (WebRTC: reason)* after the video failed, or *via WebRTC* with the
  video's bit rate, its decode time and the frames paired so far.

Over a cluster backend the page also keeps more poses in flight: see
[Refined frames over the tunnel](cluster.md#refined-frames-over-the-tunnel).

### Over Wi-Fi: refined frames as video (WebRTC)

Over a slow link the JPEG frames limit how soon and how often a refined frame
arrives. The page can then receive the colour of the refined frames as H.264
video over WebRTC instead
([#189](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/189); the
protocol is [Stereo frames as WebRTC video](../../backend-api/render.md#stereo-frames-as-webrtc-video)):
both eyes side by side in one video frame, encoded by the backend's render
process, while the frame's pose and depth image still come over the
WebSocket. Each video frame carries its pose id in a thin stripe of black and
white cells under the eyes; the page reads it after decoding and shows the
frame only with the pose it was rendered for (a stripe that does not read is
dropped), so the reprojection is the same as with JPEG frames.

**When.** By default (*auto*) the page starts with JPEG and measures the
network time of its frames (pose sent to frame received, less the backend's
own time). When the median of the last eight is above **40 ms** it offers the
backend a WebRTC link, and stays on video while it works. It falls back to
JPEG when the browser or the backend has no WebRTC (the backend needs the
optional `aiortc` package), when the link does not connect within 5 s, when it
fails, when the backend's encoder fails, when 3 s pass after a frame's header
arrived with no video frame paired since (however many headers came in
between), or after 10 video frames in a row whose stripe
does not read (or 5 that fail to be read at all); it tries again a
minute later. On falling back, a still view asks for its pose again, now as
JPEG, so the refined frame does not stay missing. `?refine_transport=webrtc` in the address uses the video at
once, `?refine_transport=jpeg` never. The video goes over UDP straight
between the headset and the Mac, so it needs the Mac reachable on the local
network: through `adb reverse` alone (USB, no Wi-Fi) it cannot connect, and the
page stays on JPEG.

What it costs and gains is under
[Refined frames as video](#refined-frames-as-video-webrtc).

## Routes the page uses

The page calls the same backend routes as the desktop; the ones written for
it, and those it shares, are:

- the reduced cube: [`POST /v1/cube/reduced_info` and `POST /v1/cube/reduced_bin`](../../backend-api/cube.md#post-v1cubereduced_info-and-post-v1cubereduced_bin);
- the page, its builds and its lifecycle: [The VR page](../../backend-api/meta-files.md#the-vr-page)
  ([`GET /v1/vr/info`](../../backend-api/meta-files.md#get-v1vrinfo),
  [`POST /v1/vr/log`](../../backend-api/meta-files.md#post-v1vrlog),
  [`POST /v1/vr/measurements`](../../backend-api/meta-files.md#post-v1vrmeasurements),
  [the render WebSocket's token](../../backend-api/meta-files.md#the-render-websockets-token-the-vr-page)),
  and the desktop dialog's [`GET /v1/vr/backend_check`](../../backend-api/meta-files.md#get-v1vrbackend_check);
- refinement: [`POST /v1/render/create_session`](../../backend-api/render.md#post-v1rendercreate_session),
  the render [WebSocket](../../backend-api/render.md#websocket),
  [Stereo frames for a headset](../../backend-api/render.md#stereo-frames-for-a-headset)
  and [Stereo frames as WebRTC video](../../backend-api/render.md#stereo-frames-as-webrtc-video);
- selections and analysis: [`POST /v1/spectral/probe`](../../backend-api/spectral.md#post-v1spectralprobe),
  [`POST /v1/cube/kinematic_select_bin`](../../backend-api/cube.md#post-v1cubekinematic_select_bin),
  [`POST /v1/cube/mask_stats_bin`](../../backend-api/cube.md#post-v1cubemask_stats_bin),
  [`POST /v1/cube/mask_apply_bin`](../../backend-api/cube.md#post-v1cubemask_apply_bin),
  [`POST /v1/products/moment`](../../backend-api/products.md#post-v1productsmoment),
  [`POST /v1/cube/pv`](../../backend-api/cube.md#post-v1cubepv),
  [`POST /v1/datasets/pixel_to_sky`](../../backend-api/datasets.md#post-v1datasetspixel_to_sky);
- selections shared with the desktop: [Shared selections](../../backend-api/share.md);
- the page's presence, the desktop's commands to it and its activity for the
  desktop: [Headset hub](../../backend-api/headset-hub.md)
  ([`POST /v1/vr/headset/hello`](../../backend-api/headset-hub.md#post-v1vrheadsethello),
  [`POST /v1/vr/headset/state`](../../backend-api/headset-hub.md#post-v1vrheadsetstate),
  [`GET /v1/vr/headset`](../../backend-api/headset-hub.md#get-v1vrheadset),
  [`DELETE /v1/vr/headset`](../../backend-api/headset-hub.md#delete-v1vrheadsetclient),
  [`POST /v1/vr/headset/command`](../../backend-api/headset-hub.md#post-v1vrheadsetcommand),
  [`GET /v1/vr/headset/commands`](../../backend-api/headset-hub.md#get-v1vrheadsetcommandsclientafterwait),
  [`POST /v1/vr/activity`](../../backend-api/headset-hub.md#post-v1vractivity),
  [`GET /v1/vr/activity`](../../backend-api/headset-hub.md#get-v1vractivityafterwait)),
  and the products it saves for the desktop:
  [`POST /v1/products/moment_fits`](../../backend-api/products.md#post-v1productsmoment_fits),
  [`POST /v1/products/pv_fits`](../../backend-api/products.md#post-v1productspv_fits),
  [`POST /v1/products/spectrum_table`](../../backend-api/products.md#post-v1productsspectrum_table);
- point tables, catalogues and velocity fields:
  [Points, catalogues and flows for the headset](../../backend-api/vr-layers.md).

## Measuring the headset: the benchmark

**Prepare the headset benchmark** makes generated cubes of 128³, 256³, 384³
and 512³ voxels and puts them on the GPU on the page (about 210 MB of
texture; a size that would take the four over this device's
[memory budget](#the-settings-on-the-page) is left out, and the page says
so); the same button, now **Start the headset benchmark**, enters VR. No
cube is made in VR: see
[GPU context loss](troubleshooting.md#gpu-context-loss-at-the-first-lasso-on-a-quest-3).
It draws each cube at 0.5, 1 and 2 samples per voxel (and the
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

## Recording measurements

The benchmark measures the GPU on generated cubes; a **record** keeps a real
session: one sample a second and the session's events, each with its time,
stored by the backend as one file per record (issue
[#224](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/224)). Two
records made with the same cube and settings can be compared line by line,
and `validation/vr_measure_report.py` turns them into tables and a plot.

**Starting and stopping.** Any of:

- on the page: tick *record measurements* (under *Controls guide* and *Copy
  diagnostics*); untick to stop;
- in the headset: the hand menu's **Settings** tab, *Record measurements*;
- `?record` in the page's address starts a record as soon as the page opens.

The line under the switch says what is recorded (*Recording
20261113-142233-a1b2c3d4: 95 samples, 31 events, 120 lines stored*) and the
file's name once the backend has written it. Recording works over USB, over
the network and paired through the public loader alike (its batches go
through the same link as every other request).

**Where the files land.** On the computer that runs the backend, in
`VISIVO_VR_MEASURE_DIR` (set it in the backend's environment), by default
`~/.visivo/vr-measurements/`, as `vr-measure-<record>.jsonl`: one JSON object
per line. A record holds at most 64 MiB (about 18 hours) and no new record is
started once the directory holds 2 GiB. When the backend cannot write (a full
disk, a directory it may not write, a record over its size), the line under
the switch says *not stored: …* and the session goes on unchanged.

**What a record holds.** Three kinds of line (`"k"`), each with `"q"`, the
number of the batch it came in (the batch's last line also has `"qn"`, its
number of lines):

- the **header** (`"k": "h"`), first: the format (`visivo-vr-measure/1`), the
  record's id and start time (UTC), the page build and API, the backend's
  version, the device (user agent, GPU name, its 3-D texture limit, timer
  query, device memory), the link by kind only (`direct` over `loopback`,
  `lan` or `remote`, or `paired`), VR's display rate, eye buffer, scale and
  foveation when the record starts in VR, the settings, the cube (dataset id,
  dimensions, encoding, binning, full shape, brick-pyramid level), and the
  **units and definitions** of every field below;
- a **sample** (`"k": "s"`) a second: `t` (ms since the record started) and
  `dur` (ms its window covers); `frames`: `n`, `fps`, `mean`, `p50`, `p95`
  (frame interval, ms), `missed_pct`, `cpu_mean`, `gpu_mean` (with `cpu_n`,
`gpu_n`, how many values each is over), `target_hz`; `iv`:
  the window's frame intervals themselves; `wrist`: the numbers on the wrist at
  that moment; `display` (display rate, eye buffer, scale, foveation);
  `render` (samples per voxel used and chosen, governor, steps per ray);
  `refine` (on, status, motion, weight, `via` `jpeg` or `webrtc`, the
  transport asked for, refined size, JPEG quality, depth, remote fps,
  `pose_frame` n/p50/p95 as on the wrist, `pf`: the pose→frame times of the
  frames that arrived in the window, the last stop→refined first and full,
  server time and frame size); `video` (the WebRTC video's kbit/s, decode and
  jitter ms, round trip and ICE candidate types); `pair` (the pairing data
  channel's state, ICE, round trip, resumes, bytes in and out); `cube`; `look`
  (a short hash of the transfer function and crop: equal hashes, equal look);
  `ui` (menu open, card on show, numbers on the wrist); `proto` during a run;
  `rec_ms` (the time the page took to build the previous sample);
- an **event** (`"k": "e"`) with its own `t`, `name` and `data`: `record-start`,
  `record-stop`, `xr-start` (mode, display rate, eye buffer, foveation, the
  rates offered), `xr-end`, `load-start` and `load-end` (ms, MB, reduction,
  transfer and upload ms, dimensions, binning), `refined-first` and
  `refined-full` (stop→refined, ms), `change` (transport, refinement on or
  off, refined size, JPEG quality, binning, eye buffer or display rate:
  `from`, `to`), `link` (the pairing's link down, up, lost), `log` (the
  page's own diagnostic events: backend unreachable and back, render socket
  closed and reconnected, eye buffer lowered…), and the run's `protocol-start`,
  `condition`, `phase`, `cue`, `condition-degraded`, `condition-skipped` and
  `protocol-end`.

```json
{"k":"h","format":"visivo-vr-measure/1","record":"20261113-142233-a1b2c3d4","started":"2026-11-13T14:22:33.120Z","sample_ms":1000,"page":{"build":"53b7389ff5d5","api":1},"backend":{"version":"1.0.12","build":"53b7389ff5d5","vr_api":1},"device":{"user_agent":"Mozilla/5.0 (X11; Linux x86_64; Quest 3) … OculusBrowser/40.0 …","gpu":"Adreno (TM) 740","max_tex3d":2048,"timer_query":true,"device_memory_gb":8,"headset":true},"link":{"kind":"direct","host":"loopback"},"xr":{"mode":"vr","display_hz":72,"eye":[3360,1760],"scale":1,"chosen_scale":1,"foveation":1},"cube":{"dataset":"…","dims":[234,234,158],"encoding":"uint8","factors":[3,3,1],"binning":"mean"},"units":{…},"definitions":{…},"q":0}
{"k":"s","t":12003.4,"dur":1000.2,"proto":{"condition":"jpeg","phase":"still","part":"measure"},"xr":"vr","mode":"explore","frames":{"n":72,"fps":71.95,"mean":13.9,"p50":13.89,"p95":14.2,"missed_pct":0,"cpu_mean":2.1,"gpu_mean":6.4,"target_hz":72},"iv":[13.89,13.9,…],"refine":{"on":true,"status":"refined","motion":"still","via":"jpeg","pose_frame":{"n":120,"p50":96,"p95":141},"pf":[95.2,101.7],…},"look":"9f2c41d0","rec_ms":0.3,"q":2}
{"k":"e","t":12210.5,"name":"refined-first","data":{"ms":150},"proto":{"condition":"jpeg","phase":"stops","part":"measure"},"q":3}
```

Every figure is computed by the code that computes what the page shows, not
by a second definition: the frame figures are volume.js `FrameStats` (the
wrist's FPS, frame mean and 95th percentile, missed %, CPU and GPU time) over
the sample's own one-second window instead of the wrist's last 240 frames;
FPS is 1000 / the mean frame interval; a frame is **missed** when its interval
is above 1.5 × the display's frame time (none is counted in a desktop
browser, which has no display rate: `missed_pct` is `null` there);
`p50`, `p95` of frame intervals are `sorted[floor(q (n − 1))]`. pose→frame
(from the pose sent to the refined frame decoded and ready to show) and
stop→refined (from the end of a motion to the first refined frame shown, and
to the frame at full weight) are the refiner's own, as on the wrist; their
quantiles are `sorted[min(n − 1, floor(q n))]`. GPU time is the GL timer
query's latest result at each frame, so it may repeat a value while the next
query is pending.

The record holds **no token, no path and no personal data**: the cube is
named by its dataset id, never its file; the link by its kind, never its
address; the page's log events lose their paths; and the backend scrubs
every string again with the [log's rules](../../backend-api/meta-files.md#post-v1vrlog)
before writing.

**What recording costs.** Measured in a desktop browser on the Mac
(validation/vr_lifecycle.mjs `--measure`, 3 × 5 s off and 3 × 5 s on,
alternated): frame interval 16.666 ms with recording off and on, CPU per frame
0.21 ms off and on (within ±0.01 ms between runs), and 0.3 ms (at most
0.4 ms) to build a sample once a second, outside the frame's own timing. On
the Quest 3 each sample's `rec_ms` says the same for the headset.

### The guided measurement run

*Run measurement protocol* (on the page next to the switch, or the hand
menu's **Settings** → *Run protocol*, on the *Record measurements* row) takes
the loaded cube through fixed **conditions**, each through the same timed
**phases**; a record is started for the run if none is on (and stopped at
its end). Tick *also at 0.6× refined size* on the page (or add
`?measure_second`) for the two optional conditions.

| Condition | What it is |
|---|---|
| `local` | the reduced cube on the headset only, refinement off |
| `jpeg` | refinement over JPEG frames on the WebSocket |
| `webrtc` | refinement over WebRTC (H.264) video |
| `jpeg-small`, `webrtc-small` (optional) | the same two with refined frames at 0.6× |

| Phase | Lead-in (prompt in front) | Measured | Asked of you |
|---|---|---|---|
| settle | 5 s | none | the condition is set up; hold still and look at the cube |
| still | 3 s | 15 s | keep your head still, looking at the cube |
| turn | 3 s | 15 s | turn your head slowly left and right, keeping the cube in view |
| stops | 3 s | 20 s | at each long pulse turn, at each short pulse stop (2 s each, five times) |

A condition takes 64 s, the three of them 3 min 12 s (5 min 20 s with the
optional two), plus the time a refined condition needs to connect: its
settle phase starts once the refined frames come over the condition's
transport, and a condition not ready within 20 s (WebRTC blocked by a
firewall, no render sessions for a synthetic cube) is skipped with its
reason in the record. A refined condition that stops holding during its steps
(WebRTC given up for JPEG, the render session gone for good) is skipped from
there, so nothing is measured under its name over the other transport; a
moment's gap (a socket reconnecting) is recorded once per step as
`condition-degraded`.

Each step's prompt appears on the message card in front of you during its
lead-in, then moves aside (left and lower) for the measured part, and a
pulse on both controllers marks the start of each measured part. Every
sample and event carries `proto` (condition, phase, and `part`: `lead` or
`measure`), so only the measured parts are compared. **Cancel** on the card
stops the run at once (leaving VR, or loading another cube, cancels it too);
what was measured stays in the record.

Within a run nothing changes but the condition: the transfer function, the
crop, the binning and the threshold are left as they are; the **governor is
held off** for the whole run (it would lower the samples per voxel or the eye
buffer under one condition and keep them lowered for the next); each refined
condition starts from a new render session; and the prompt's text changes
only when a step starts. When it ends, refinement, its transport and refined
size, and the governor are set back as they were. The report says, for each
condition and phase, whether the look, binning, eye buffer, display rate and
samples per voxel were held equal and, for a refined condition, whether
every sample came over its transport.

### The report

```sh
python validation/vr_measure_report.py ~/.visivo/vr-measurements/vr-measure-<record>.jsonl [more records] --out DIR
```

writes `report.md` (per record: device, link, cube; per condition and phase:
seconds, frames, FPS, frame mean, p50 and p95, missed %, display rate, CPU
and GPU ms, pose→frame p50/p95 and count, stop→refined first and full p50/p95
and count, the share of samples via JPEG and via WebRTC, whether the
conditions were held equal (look, binning, eye buffer, display rate,
samples per voxel, and the transport for a refined condition); then loads, link drops and resumes, render socket
reconnections and skipped conditions), `per_phase.csv` (the same rows) and
`frame_time.png` (every frame interval over time, one colour per condition,
with the display's frame time and the missed-frame limit). It pools the raw
values of a phase's measured samples with the same definitions as the page
(CPU and GPU means weighted by `cpu_n`, `gpu_n`).
Samples recorded outside a run are grouped as `session:local`,
`session:refine-jpeg` or `session:refine-webrtc`.

## Measurements

Everything here was measured in a desktop browser (or by script) on the Mac,
or on a cluster node from the Mac; the headset's own figures come from the
[checks](#what-to-check-on-the-quest-3).

### Refined frames on the Mac (desktop browser, local backend)

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
  frame about 80 ms (the *stop->refined* on the numbers counts from the
  decision: 83 ms and 200 ms). A second run, with the Mac busier (server
  108 ms render instead of 37 ms), gave 408 ms and 516 ms;
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
the numbers on the wrist show both.

### Refined frames as video (WebRTC)

HCG 44, auto binning, headless Chrome on the M5 Max against a local backend,
`validation/vr_refine_webrtc.mjs`; the view turning slowly at 2.5 °/s, so a
new pose is asked continuously. *15 Mbit/s (WebSocket only)* throttles the
page's network through the DevTools protocol
(`Network.emulateNetworkConditions`). That slows the WebSocket (poses, the
JPEG frames, the video frames' headers and depth images) but **not** the WebRTC
video, which goes over UDP: it is not a Wi-Fi link, and it favours the video.
On real Wi-Fi both share the link; the 3–4 Mbit/s of the video would fit a
15 Mbit/s link, but its latency there is not measured yet (see the
[check](#refinement-with-the-headset)). The 1600 × 913 rows were measured on
2026-11-12; the Quest-size rows on 2026-11-11 with the same page code for
reading and drawing the video (a run on 2026-11-12 was not usable: the Mac
was busy with other GPU work, and the backend's render took 58–177 ms instead
of 37).

| Eye size | Link | Path | pose->frame p50 / p95 | refined fps | Mbit/s | kB per frame | stop->refined p50 | PSNR on emission |
|---|---|---|---|---|---|---|---|---|
| 1600 × 913 | local | JPEG 95 | 48 / 51 ms | 19.8 | 10.8 | 67 | 251 ms | 41.9 dB |
| | | WebRTC | 43 / 53 ms | 19.6 | 2.9 | 18 | 250 ms | 39.2 dB |
| | 15 Mbit/s (WebSocket only) | JPEG 95 | 90 / 95 ms | 19.6 | 10.7 | 67 | 317 ms | 41.9 dB |
| | | WebRTC | 51 / 62 ms | 19.8 | 2.9 | 18 | 267 ms | 39.0 dB |
| | | auto (chose WebRTC) | 62 / 77 ms | 19.6 | 2.9 | 18 | 285 ms | 39.1 dB |
| 1700 × 1813 (a Quest 3 eye) | local | JPEG 95 | 149 / 157 ms | 13.3 | 14.9 | 136 | 304 ms | 44.7 dB |
| | | WebRTC | 153 / 167 ms | 13.0 | 4.0 | 37 | 300 ms | 41.7 dB |
| | 15 Mbit/s (WebSocket only) | JPEG 95 | 176 / 182 ms | 11.2 | 12.5 | 136 | 405 ms | 44.7 dB |
| | | WebRTC | 142 / 153 ms | 13.1 | 4.0 | 37 | 316 ms | 41.6 dB |
| | | auto (chose WebRTC) | 149 / 162 ms | 13.6 | 4.1 | 37 | 367 ms | 41.7 dB |

The page's own picture is 26 dB on the same pixels: either refined frame is far
closer to the full cube. The video takes a quarter of the bandwidth of the
JPEG frames (most of what is left is the depth image, on the WebSocket) and
costs about 3 dB of PSNR on the emission (4:2:0 chroma on small coloured
structures; x264 at constant quality 14). On an unthrottled link
pose->frame is the same within a few milliseconds either way: the page reads
the stripe in 1.2–1.7 ms by copying two rows of the decoded frame's visible
rectangle (`VideoFrame.copyTo`; Chrome on the Mac decodes to NV12) and draws
the frame itself, no copy. Where a decoder's frames cannot be copied (a
headset's hardware decoder may give frames without a format, or refuse the
copy) the page reads the stripe through an image instead: 5.6 ms at Quest
size, pose->frame 156 against 153 ms (`--stripe-fallback` in the harness).
With the WebSocket throttled the video arrives sooner than the JPEG, but see
above for what that throttle does and does not slow. In every run each video frame was shown
with the header of its own pose: every stripe read (none unreadable, none
dropped), and, checked independently of the stripe, the shown frame is
38–42 dB from a JPEG the backend rendered for the same pose and 17–20 dB from
one of the pose before (3° away). The backend's render and the wait for the
frame before it remain the largest part of pose->frame.

### Cluster backend: PLEIADI and a path prefix

**Measured on PLEIADI (2026-11-11, by hand).** Backend in a SLURM job on a
V100 node, image `backend-egl.sif` (EGL, VTK 9.3.1) with a `--backend-src`
overlay of the then current `backend/app`; HCG 44; poses at 72 Hz from the Mac
through the SSH tunnel with `benchmark/remote_render/stereo_pose_latency.py`
(two frames in flight, before the window followed the round trip). Colour
**and** depth read back through the volume mapper on that EGL path: the
colour-only fallback is a safety net there, not the normal case.

| Eye size | Stereo frames/s | Pose→frame, client p50 / p90 | Server p50 | Render | Depth | JPEG | Frame size |
|---|---|---|---|---|---|---|---|
| 1024² | 9.1 | 175 / 196 ms | 115 ms | 80 ms | 7.9 ms | 19 ms | 37 kB |
| 512² | 16.4 | 122 / 154 ms | 41 ms | 28 ms | 2.2 ms | 4.5 ms | 11 kB |

About 80 ms of each latency is the tunnel's round trip; with two frames in
flight the rate was capped at about window / round trip. A Quest 3 eye at the
default *Refined size* (1×) is about 2064 × 2208 px, four times the pixels of
1024²: expect a render near 300 ms there, so a stop→refined of roughly half a
second over the tunnel, until measured.

**Measured on the Mac (headless Chrome, HCG 44, the backend behind a
prefix-stripping proxy on localhost, 2026-11-11).** Paired with the desktop's
link code under the prefix `/portal/visivo`; cube drawn and *refined* 7.6 to
11 s after the page opened (cold page and a 0.6 to 0.8 s reduction).
Measured from the motion detector's stop:

| Link | Depth | Pose→frame p50 / p95 | Stop→refined p50 | Window |
|---|---|---|---|---|
| loopback | yes | 77 / 616 ms (n 7) | 83 ms | 2 |
| loopback | off (`VISIVO_XR_DEPTH=0`) | 53 / 68 ms | 50 ms | 2 |
| +80 ms on the uplink | yes | 141 / 160 ms | 150 ms | 3 (far) |

### Selections and analysis

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

**The same numbers as the desktop.** `validation/vr_analysis_identity.py`
places the seeds and the path's points with the page's own pick (node runs
its modules) on HCG 44 and compares every call with the desktop's
(`validation/out/vr_analysis_identity.md`; the requests against the keys the
desktop's C++ sends, read from its source, the PV rows against a port of its
row ordering and the page's own drawn pixels against those rows): seed on NGC 3187, 13 216 voxels,
6.061 Jy km/s; add NGC 3190, 23 891 voxels, 9.191 Jy km/s; carve inside
NGC 3187, 10 675 voxels, 3.131 Jy km/s; each the desktop's displayed and
replayed selection (Dice 1.000) and the desktop's request. Isolate and Remove
make the desktop's cubes, voxel for voxel. M0, M1, M2 are the desktop's maps
bit for bit; the sum of M0 over the map divided by the beam (30.66 pixels) is
the selection's flux (6.0607 = 6.0607 Jy km/s), and the M0-weighted M1 is its
spectrum's flux-weighted velocity (1572.44 km/s both). The PV diagram along a
three-point path across NGC 3187 is the desktop's, its emission at
1515 km/s, and the same path on a copy of the cube with the spectral axis
reversed (CDELT3 < 0) gives the same picture row for row.

**Timings of the selections** (backend on an Apple M5 Max, page in Chrome on
the same Mac, HCG 44 binned 3 × 3 × 1): pick and seed 40 ms (the ray pick
itself takes under 0.1 ms, so the cross follows the ray every other frame),
spectrum 45-85 ms, lasso 570-710 ms plus 40-80 ms of statistics, box over 158
channels 22 ms. The green overlay costs GPU time: 0.64 ms without it, 0.84 ms
with it, for one 1600 × 1000 view on that Mac; the headset's own numbers are
on its [numbers on the wrist](controls.md#the-numbers-on-the-wrist).

**Timings of the analysis** (Apple M5 Max, Chrome, HCG 44, the mouse on the
page): a grow with seeds 0.6-0.7 s as before, Isolate 0.18 s, Remove 0.12 s, a
moment 0.13-0.38 s (isolate reused), a PV of 50 samples × 158 channels 0.06 s.
The drawing cost of the menu's Analysis page and the result windows is under
[Menu and panel drawing cost](#menu-and-panel-drawing-cost).

### Shared selections

Backend on an Apple M5 Max, the page in headless Chrome on the same Mac,
HCG 44, `validation/vr_share_e2e.mjs`: NGC 3187's lasso made in the
page (13 216 voxels, 6.061 Jy km/s) reaches a client waiting on the hub
48-98 ms after the page's lasso ended (three runs; the publish with the
backend's statistics is 45-94 ms of it), with the same voxels, the same flux, the same mask
(Dice 1.000 with a regrow of the recorded request) and the page's request as
its record; a desktop lasso of 10 684 voxels is drawn by the page 53 ms after
the desktop starts sending it, and the page adds no recipe; a 56 × 43 × 158
box made in the page reaches the desktop with the same 380 464 voxels and
flux. The desktop's own client (`SharedSelectionLink`) receives a selection
another client of the session publishes in about 100 ms.

### Points, catalogues, flows

Chrome headless with ANGLE Metal on an M5 Max, one view of 1680 × 1673 pixels
(about one Quest 3 eye at 1×), GPU time from the timer query; synthetic data
in the scratch area (a VBT of 10 000 000 rows with five columns, a 96³
velocity field, a GLON-CAR cube), HCG 44 with its SoFiA-2 catalogue.

**Point table, 10⁷ rows**, all sampled rows drawn (*hold the frame rate*
off), GPU ms for one view (two runs; the second's numbers, the first's within
0.8 ms):

| Rows drawn | points | splats | sample | request (local) |
|---|---|---|---|---|
| 0.5 M | 3.3 | 4.0 | 4 MB | 50 ms |
| 1 M | 4.0 | 4.2 | 8 MB | 97 ms |
| 2 M | 4.2 | 4.4 | 16 MB | 160 ms |
| 5 M | 6.2 | 8.1 | 40 MB | 480 ms |
| 10 M | 13.6 | 18.5 | 80 MB | 1030 ms |

With *hold the frame rate* on and all 10 M rows sampled, the page settled at
3.4 to 6.0 M rows drawn, 9.2 to 10.1 ms of GPU per frame (0.6 of a 60 Hz
frame) and 60.0 FPS with no missed frame. The cost is per point (2 px squares
cost little more than 1 px ones: 4.4 against 3.7 ms for 2 M rows), not per
pixel. Refinement: the first refined frame of all 10 M rows came 1.6 s after
loading (the render session is made then), fully blended at 1.75 s; the
backend drew both eyes in 141 ms, pose to frame 203 ms.

**Catalogue over a cube.** HCG 44 (`VELO-HEL` read as optical velocity) and
its SoFiA-2 catalogue (VOTable, `ra`, `dec`, `v_app` in m/s): all 18 sources
placed, within 0.0022 px in x and y and 10⁻¹³ channel in z of the pixel
SoFiA itself reports, and drawn on the galaxies' emission. A synthetic
GLON-CAR cube with 12 Gaussian sources whose catalogue gives `glon`, `glat`
and `vlsr` (km/s): every source within 3 × 10⁻⁶ px of its true voxel. The
placement took 23 ms; the overlay costs 0.2 to 0.4 ms of GPU.

**Velocity field**, 96³ with a box of 200 Mpc: 400 streamlines of 407 758
points and 50 000 arrows, 7.7 MB, traced and sent in 0.24 to 0.29 s, 2.0 to
3.1 ms of GPU for one view of 1600 × 913. The streamlines are point for point
those of the desktop's pipeline on the same grid (a test runs the desktop's
filters, written from its C++, with the same seeds on the grid placed by the
spacing and origin the grid route reports: identical within 10⁻⁴ Mpc), on the
full grid and on a block-averaged one. Opened from a pairing link: 0.42 s to
the lines drawn.

### Recovery from failures

Checked so far in a desktop Chrome (headless, ANGLE Metal on an M5 Max,
and SwiftShader) against a local backend, with each failure driven by
`validation/vr_lifecycle.mjs`: the backend killed (the notice within 0–5 s,
retries every 1–5 s, the cube opened again about 10 s after the backend is
back), the token changed under the page, the cube's dataset deleted, the
render session deleted (the page's own rendering at once, refined again
about 8 s later), the WebGL context lost and restored with
`WEBGL_lose_context` (the cube drawn again 0.6 s later), a stale page build
and API, the guide's buttons pressed through a ray. Not yet on a Quest 3: see
[Recovery and diagnostics, with the headset](#recovery-and-diagnostics-with-the-headset).

### Repeated selections and GPU memory (#226)

On the Quest 3 the WebGL context was lost after about ten Kinematic Lasso
picks and spectra in about five seconds and a mask statistics request, and
was not given back. The same pattern on the Mac (headless Chrome, ANGLE Metal,
M5 Max; `validation/vr_lifecycle.mjs --alloc`, a 350 × 350 × 158 synthetic
cube binned 2 × 2 × 1; four bursts of 20 alternating lasso picks and spectra in
5 s, each ending with a lasso and its statistics; WebGL counted by a wrapper
installed before the page's scripts):

| | before #226 | after |
|---|---|---|
| live textures after each burst | 14, 14, 14, 14 | 14, 14, 14, 14 |
| texture bytes held | 15.2 MB, constant | 15.6 MB, constant |
| texture storage re-specified (`texImage2D/3D`) during bursts 2–4 | 244 times, 661 MB | 0 |
| GPU process footprint (macOS `footprint`, noisy) | 316–532 MB | 335–386 MB |

No texture or buffer was made per action and not freed: the selection mask,
the result window, the readout and label cards, the menu and the product
planes each have one texture. What did grow with the pace of actions was the
churn: every redraw of a panel (the menu up to 30 times a second, the readout
15, the result window at each state of a pick) and every new selection mask
gave its texture new storage, about 45 MB a second here, which a driver may
hold until the frames in flight are done. Now a panel's storage is allocated
once per canvas size and each redraw writes into it; the mask texture is
allocated once with the cube, over its whole grid (issue #229: no 3-D
texture is allocated in VR), and each selection is written into it; the label atlas reuses one canvas and one storage; the
controllers' lines reuse their buffer; and a newer pick, a change of tool or
Clear cancels the requests of the one before (the lasso's mask, its
statistics, the seed's sub-volume), so fast picks do not pile up downloads.
The check that later bursts re-specify (almost) no storage, and add no live
WebGL object, runs with `--regressions`. Whether this was the Quest's cause
can only be told on the headset ([the check](#the-first-quest-3-session-226)).
When the browser does not give the context back within 10 s, the card says
*The graphics did not come back* with *Reload page*, which now reopens the
cube on show by its path.

### Menu and panel drawing cost

These were measured on the earlier panel in the headset, whose drawing code
the hand menu reuses. The panel was redrawn only when what it showed changed,
at most 30 times a second. On the Mac (M5 Max, headless Chrome) a redraw of
its 1024 × 832 canvas takes 0.10 ms (0.20 ms 95th percentile) and its upload
as a texture under 0.1 ms on the CPU; redraw, upload and drawing it, waited
for on the GPU, 0.8 ms (1.2 ms). A frame with the panel shown costs 0.03 ms
more CPU and no measurable GPU time than without it. Redrawing the analysis
panel takes 0.4 ms and a product plane (a moment map or PV diagram) 0.15 ms of
CPU, only when they change; with the panels and both planes shown the page
holds 60 fps (Apple M5 Max, Chrome, HCG 44).

**The hand menu (issue #201)**, measured on the Mac (M5 Max, headless Chrome,
Metal; `validation/vr_lifecycle.mjs --menu`, 40 forced redraws per page, the
timer's resolution is 0.1 ms): a redraw of the 1024 × 928 menu canvas takes
0.1 ms median (0.2-0.3 ms 95th percentile) on every page (Mode, Look,
Analysis, Settings, Help), its upload under 0.1 ms, and redraw, upload and
drawing it, waited for on the GPU, 0.5-0.8 ms median (0.8-1.6 ms 95th
percentile). Like the panel, the menu is redrawn only when what it shows
changes, at most 30 times a second, and only while it is open. The cursor
readout's text costs 4 µs a call (2000 calls); its card is redrawn at most 15
times a second, when its text changes. The Quest 3's own figures are on its
[numbers on the wrist](controls.md#the-numbers-on-the-wrist) (*menu redraw … ms
+ upload … ms* and *readout redraw … ms*).

### Large cube, out of core

A synthetic 2400 × 2400 × 520 cube (11.2 GiB, noise with six rotating discs,
`validation/vr_large_cube_make.py`), headless Chrome on an M5 Max, a
1600 × 1000 window, the backend on the same Mac, the threshold of the full
cube (`validation/vr_large_cube.mjs`; HCG 44 in core in the same run for
comparison; `validation/vr_large_cube_selection.py` for the selections):

| | Large cube, out of core | HCG 44, in core |
|---|---|---|
| Brick pyramid built (first open only) | 9–18 s (15.9 GB file) | none |
| Open → first reduced frame, with the build (three runs) | 10.8–18.9 s | |
| Open → first reduced frame, pyramid ready | 1.46–1.77 s | 1.16–1.24 s |
| of which the reduction | 1.17–1.46 s (level 2, 5.2 MB; 135 MB of level 0 read to recompute 434 400 border voxels) | 0.65–0.78 s (whole cube, 8.7 MB) |
| The same reduction from the whole cube | 10.9 s | |
| Open → first refined frame (new render session) | 5.8–6.7 s | 4.8–5.6 s |
| pose→frame, median / 95th percentile | 135–149 / 178–199 ms | 88–93 / 149–151 ms |
| backend render of both eyes | 47 ms | 23 ms |
| stop→refined, first frame / fully faded in (median) | 83 / 200 ms | 83 / 200 ms |
| Lasso on a disc (375 353 voxels) | 10–15 s, +2.5 GB | 0.6 s |
| Statistics of that mask / of a 400³ box | 0.17 s / 2.9 s | |

pose→frame here is measured in a desktop browser while the view turns
slowly (poses stream), so it includes waiting for the frame before; the
backend draws level 1 (374 M voxels) about twice as long as HCG 44. With the
threshold *for this binning* (12 × 12 × 4) instead, the refined frames were
full of the full cube's noise: 405 KB JPEGs, 64 ms to encode, pose→frame
236 ms. The memory figures are the backend's resident size, which counts the
pages of the memory-mapped cube. The lasso is slow on a large cube because
its window (513³ voxels) is larger than all of HCG 44: it is bounded, but not
yet fast.

## What to check on the Quest 3

These checks need the headset; the page shows the numbers to read. Note each
value and whether each step behaves as described. How to work the hand menu,
the modes and the tools is in [Controls and the hand menu](controls.md).

### The first Quest 3 session (#226)

The first session on a Quest 3 (Quest Browser 154, Adreno 740, HCG 44 binned
3 × 3 × 1, over USB) found eight things to fix. Check each again, with
*record measurements* on (with `?debug`: More → *Technical settings* →
*Record measurements*) so the numbers
come back with the record.

1. **Frame rate and quality.** Enter VR with HCG 44 at the default settings.
   The numbers on the wrist must read *display 72 Hz*; the eye buffer
   *3360×1760 scale 1* (or, after a long struggle, *2856×1496 scale 0.85*,
   never smaller); note FPS, *missed %* and *samples/voxel* while still and
   while turning the cube. Does the volume look less grainy than at 90 Hz?
   **Copy diagnostics**: the *quality* line names the policy (*72 Hz;
   samples/voxel first … then the eye buffer as a last resort*).
2. **Sharp text.** The diagnostics' *text in the headset* line must say
   *quad layers*. Open the menu, point at the cube for the readout, take a
   spectrum: menu, labels, readout, numbers and the result window must be
   crisp (compare with `?nolayers` in the address, which draws them in the
   scene as before). All text must be upright and not mirrored (if it is
   upside down, report it and try `?layerflip=0`). The ray must still press
   the menu's buttons and move a window by its title bar.
3. **Labels fixed to the cube.** Walk around the cube and turn your head: the
   tick labels and axis titles must not turn towards you; they move only when
   you turn the cube (grip). From the start position the RA and Dec labels
   read on the front face, the velocity labels at 45° on the right-hand edge.
   From behind, the labels read the right way round, not mirrored.
4. **The readout.** Point at the source at arm's length and at 2 m: can you
   read every line of the card without leaning in? (Its body text is at least
   0.9° high from 30 cm to 4 m, white on near-black.)
5. **One window.** With the page open in the Quest Browser, press *Open on
   Headset* on the Mac again (or open the page a second time): the older
   window must show *Open in another window*, its *Enter VR* greyed out; the
   new one works. *Use this window* in the old one gives it back (and the
   other one stands down). The same with two windows of the public loader.
6. **The guide.** Clear the page's data (or a fresh browser profile) and open
   the page: the guide is on the 2-D page with *Got it*, and Enter VR does not
   show it. In VR, More → Guide: the card stands beside the cube on the
   primary hand's side, never in front of the cube, the menu or the labels
   (open the menu next to it: the menu is drawn over it).
7. **Controllers and hands.** With controllers: two solid controllers
   (orange primary, blue secondary) with a ring, a stick and a trigger that
   lights while pulled. Put them down: both hands drawn as joints and bones,
   the thumb and index tips yellow while pinching.
8. **No lost context.** Lasso tool: ten picks and spectra as fast as you can
   (about 5 s), then a lasso and its statistics; repeat three times. The
   graphics must not reset. The wrist's *GPU textures* line must stay the same
   after the first burst (note its MB). If the graphics do reset: wait 10 s;
   the card says *The graphics did not come back* with *Reload page*; after
   *Reload page* the cube must open again by itself.

### Recording measurements, with the headset (#224)

A record replaces copying numbers by hand for the checks below; it is what
the [report](#the-report) is made from.

1. Start the backend (or the desktop application) on the Mac; to choose where
   the records go, set `VISIVO_VR_MEASURE_DIR` before starting it (else
   `~/.visivo/vr-measurements/`). Connect the headset over USB (`adb reverse
   tcp:8000 tcp:8000`) and open the page with `?record` added to its address
   (or tick *record measurements*), load HCG 44 with auto binning, leave
   *Refresh* at 72 Hz, *Resolution* 1 and *Foveation* as it is, and note them.
2. Enter VR. Open the hand menu's **Settings**: *Record measurements* is on.
   Press **Run protocol** on the same row. Follow the card: hold still, turn
   slowly, turn and stop at the pulses; read each prompt during its 3 s
   lead-in (the card then moves aside). The run takes about 3 min 12 s plus
   the refined conditions' connection; don't open the menu during it. If a
   condition is skipped, the record says why; note it.
3. At the end the card says *Measurement run complete* with the record's
   file. Leave VR; the line under *record measurements* gives the record's id,
   its samples and the lines stored (*0 dropped* expected). Untick it.
4. Repeat 1–3 over Wi-Fi (`adb connect`, or the page opened by the Mac's
   address), and once paired by code through the public loader, each giving
   a record of its own.
5. On the Mac:
   `python validation/vr_measure_report.py ~/.visivo/vr-measurements/vr-measure-*.jsonl --out report`,
   and send `report/report.md`, `report/per_phase.csv`, `report/frame_time.png`
   and the `.jsonl` files. Check in the report that every row says *held
   equal: yes*, that the `webrtc` rows are 100 % *via WebRTC* and the `jpeg`
   rows 100 % *via JPEG*, and read FPS, missed % and frame p95 for `local` vs
   `jpeg` vs `webrtc` in the *turn* phase (they must be the same within the
   noise: refinement never holds a frame back), pose→frame p50/p95 in *still*
   and stop→refined in *stops*.
6. The cost of recording itself: each sample's `rec_ms` (time to build the
   previous sample, once a second) should stay under 1 ms on the Quest; the
   report's FPS and missed % with a record running should equal what the
   wrist shows without one.

### The menu in front of you, organised by task (#257)

Over USB (`adb reverse`) or Wi-Fi, with the HCG 44 cube and then a simulation
snapshot (a VBT point table), the page opened **without** `?debug`, then with
it for the last step. Count the presses from opening the menu (the hold of X
does not count). The menu asks for 0.77 × 0.72 m at 0.95 m (about 44° wide);
the work on [#267](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/267)
suspects the Quest Browser draws composition layers twice as large as asked,
so note in step 1 how wide the menu looks (about half the view's width is as
asked; wider than the view means doubled) and whether #267's fix is in.

1. **Where it opens.** Look at the cube and hold X. The menu must stand about
   a metre in front of you, a little below your eyes, facing you, with its
   text as easy to read as the old menu by the wrist (about 1° high). Turn
   your head and move your left hand while holding X: the menu must not move.
   Report whether the distance is comfortable for the ray (too far, too near).
2. **Moving it.** Point at the title bar (top strip), hold the trigger and
   swing the ray: the menu follows at the same distance, turned to you. Let
   go: it stays.
3. **Keep open.** Footer → *Keep open*, let go of X: the menu stays; your left
   hand is free. The next press of X closes it.
4. **The tabs.** Display, Range, Select, Measure, More, and the footer (Undo,
   Reset view, Keep open) under every tab. No Help tab, no Mode page, no
   refinement, governor, recording or threshold-source row anywhere.
5. **The Done list, HCG 44** (each at most two presses): faint emission
   (Display → a colour map swatch; Display → drag Threshold, its value in
   Jy/beam under it); lasso a galaxy (Select → *Kinematic Lasso*, trigger on
   the galaxy) and *Isolate* it (Select → *Isolate*, shown only now that
   something is selected); the moment of the selection (Measure →
   *Intensity (M0)*); a PV slice (Measure → *Draw*, then trigger points); a
   velocity range (Range → drag *Velocity range*: km/s, convention and
   channels under it). Note any task that took more presses or a search.
6. **The Done list, simulation**: colour by velocity (Display → the *VX*
   chip, or *more ▸* first if the column is on the next page) and crop
   (Range → drag *Crop x*): the points outside disappear at once.
7. **Greyed controls.** With the snapshot: Select and Measure greyed, each
   with its reason under it; Range → *Velocity range* greyed. Nothing moves
   when you switch between the cube and the snapshot.
8. **Whole-field moment.** Clear the selection, Measure → *Intensity (M0)*:
   the moment of the whole field over the velocity range, in its window.
   Compare it with the desktop's moment dialog at its defaults over the same
   channels.
9. **Pin spectrum.** Select → *Spectrum probe*, take a spectrum, Measure →
   *Pin spectrum*, take another one elsewhere: both in the Selection window,
   the pinned one in gold.
10. **Threshold.** The initial threshold must be the desktop's for the same
    cube (compare the value under Threshold with the desktop's 3-D view).
11. **Costs.** With `?debug`, read FPS and missed % with the menu closed and
    open (and while dragging a slider): the menu must not cost frames. More
    → *Technical settings ▸* must be there, and only with `?debug`.

### Controls and the hand menu (#201)

The menu's names here are those before #257 (see the table in
[The hand menu](controls.md#the-hand-menu)).

Over USB (`adb reverse`), the cube of your choice (HCG 44 if you have it), the
settings left at their defaults the first time.

1. **First entry.** Clear the page's storage first (or use a new browser
   profile) and press **Enter VR**: the guide must stand in front of you with
   four pages (*Your two hands*, *The menu*, *Three modes*, *Hands without
   controllers*), *Next*, *Back*, *Close*. Labels must float above both
   controllers. Report whether you understood what to do without this page.
2. **Labels.** Above the right controller: *EXPLORE*, then *Trigger: pin the
   readout*, *Grip: grab*, *Stick ↕: threshold*, *Stick ↔: opacity*,
   *A: next mode*, *B: undo*. Above the left: *Hold X: menu*, *Y: reset view*,
   *Grip: grab (both: turn, scale)*, *Stick ↔: colour map*, *Stick ↕: colour
   max*. Are they readable from where you hold the controllers? Do they cover
   anything you need to see?
3. **The menu.** Hold **X**: the menu opens above the left hand, toward the
   middle, facing you, with a pulse in the left controller. Point the right
   ray at it: the yellow cross follows; pull the trigger on each tab (Mode,
   Look, Analysis, Layers, Settings, Help): a short pulse in the right
   controller each time. Let go of X: it closes. Is the text sharp enough
   (the smallest is about 1° high)? Does the menu move with the hand without
   jitter? Note the numbers' **menu redraw … ms + upload … ms**.
4. **Keep open.** Mode tab → *Keep open*, let go of X: the menu stays where it
   was. Press X once more and let go: it closes.
5. **Modes.** Press **A** three times: the mode above the right controller,
   and in the readout, goes EXPLORE → SELECT → CLIP → EXPLORE, with the labels
   changing to match. In each mode check what the labels say against what
   happens:
   - Explore: stick up/down changes the threshold, left/right the opacity;
     left stick flicked left/right steps the colour map once per flick;
     left stick up/down the colour max. Trigger on the emission pins a
     readout (amber background) there.
   - Select: left stick flicked steps *spectrum → Kinematic Lasso → box
     region*; the trigger takes a spectrum, a lasso, two box corners. Results
     open in the *Selection* window right of the cube.
   - Clip, *velocity slab*: right stick up/down moves the slab along the
     velocity axis, left/right widens or narrows it; the trigger on emission
     outside the slab centres the slab on that emission. Clip, *clipping box*
     (left stick flick): hold the trigger with the controller on (or pointing
     at) a face of the box and move: that face follows; right stick up/down
     shrinks or grows the box.
6. **Undo and reset.** After a few threshold, colour map and slab changes,
   press **B** repeatedly: each press takes back one change (a whole stick
   push is one change); with nothing left, a double pulse. Grab the cube and
   carry it away, then press **Y**: it comes back in front of you at its first
   size, the windows with it.
7. **The cursor readout.** Point the right ray at the emission: a card above
   the point shows the mode, *RA hh:mm:ss.ss Dec ±dd:mm:ss.s*, *v … km/s
   (convention) ch …* and the value with its unit (and the binning). Compare
   one position with the desktop's cube viewer at the same pixel: RA, Dec and
   velocity must agree to the binning's precision. Does the frame rate change
   while the readout follows the ray (FPS, missed %)?
8. **Result windows.** Take a spectrum, then (Analysis tab) *M0* and a PV
   path: three windows. Each title bar is above its window, never over the
   text, and names it (*Spectrum probe*, *Moment 0*, *PV diagram*); the
   velocity line wraps inside the window
   ([#267](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/267)).
   Point the ray at the far edge of a menu button or window: the cursor dot
   must sit on the drawn edge (panel and layer the same size). Point at a
   window's title bar, hold the trigger and
   move: it follows and turns to face you. × closes it; a new result opens it
   again. Leave VR and enter again: the windows are where you left them.
9. **Haptics.** A short pulse on every press (menu, trigger on the cube),
   a stronger one when a spectrum, lasso or box result arrives, two pulses on
   an error (for example a trigger in Select mode pointing at empty space).
10. **Left-handed.** Settings tab → *Left-handed*: the roles swap at once:
    the button legend is now on the left wrist (*X: next mode*, *Y: undo*,
    *Hold A: menu*), the cube card on the right wrist; the left ray acts.
    Reload the page and enter again: still left-handed. Turn it back.
11. **Wrist cards ([#256](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/256)).**
    Without `?debug`, nothing stands in view but the cube (no numbers, no
    card above the controllers, no Selection window in Select mode before a
    result). Raise the left forearm in front of the chest, the back of the
    wrist towards your eyes, as to read a watch: the cube card fades in on the
    wrist (cube, size, mode, the selection's numbers) and is readable. Turn
    the wrist away: it fades out. Do the same with the right wrist: the
    button legend of the tool in use; change the tool (left stick) and the
    legend follows. Hold the wrist at the edge of the gesture and turn it
    slowly: does the card flicker (report how often)? Point at the cube with
    the arm out and grab it: does a card appear by accident? With tracked
    hands, turn the left palm up: the menu opens and no card shows. Settings
    → *Button legend on the wrist* and *Cube card on the wrist* off: the
    cards no longer show. Take a spectrum: a toast low in the view says its
    numbers and goes after about 2 s; press **A**: a toast says the new
    mode.
12. **Hands.** Put the controllers down. Pinch with the right hand on the
    cube in Select mode: a spectrum. Turn the left palm up: the menu opens by
    the left hand; pinch with the right on a tab; turn the palm down: it
    closes. Make a fist with either hand: the cube is grabbed; both fists:
    turn and scale. Does the palm-up gesture open the menu by accident while
    you grab or point (report how often)?
13. **Nothing lost.** From the menu alone, check that you can reach:
    emission or maximum intensity, threshold source and *Reset*, all
    channels, the clipping box reset, refinement on and off, *Hold the frame
    rate*, *Cube card on the wrist*, the guide, the Layers tab's buttons with a
    point table and with a catalogue over a cube.

### Refinement, with the headset

1. Start the backend on the Mac and connect the headset over USB
   (`adb reverse tcp:8000 tcp:8000`), open `http://localhost:8000/vr/?debug`
   (the numbers on the wrist need `?debug` since #256) with the token, load HCG 44 (auto binning) and check that *refine with the
   backend's full cube* is ticked. Enter VR.
2. Hold your head still for a second: the numbers on the wrist must go from
   *waiting* to *refined 100 %*. Read **stop->refined** (both values),
   **pose->frame** (median, p95), **remote** fps, **server** render ms and
   **upload** ms. Do this five times and note the values.
3. Look at the cube while it refines: the image should get sharper (finer
   structure, no binning blocks) **without jumping**: if it shifts or doubles
   when it fades in, note in which direction and how much (that is a pose or
   axis mismatch).
4. Turn your head quickly left and right: there must be **no lag and no
   smearing** of the cube: the refined frame must disappear at once and the
   reduced cube follow your head. Read **FPS** and **missed %** on the numbers
   while turning, with refinement on and off (untick it on the page, or
   compare with `?refine=0`): they must be the same.
5. Move your head slowly sideways (a few cm per second) while looking at the
   cube: the refined frame should stay and follow the cube without visible
   swimming; untick *reproject with depth* on the page and repeat, and note
   which looks steadier.
6. Make a Kinematic Lasso selection, then hold still: the green mask must
   stay visible over the refined frame; the box outline and labels too.
7. Move the velocity slab (the Clip mode's slab, see [Modes](controls.md#modes)),
   then hold still: the refined frame must show the same slab (no emission
   outside it).
8. Repeat 2 and 4 over Wi-Fi (adb connected wirelessly) and note
   **pose->frame** and **stop->refined**.
9. Over Wi-Fi, compare JPEG and video: open the page with
   `?refine_transport=jpeg`, then with `?refine_transport=webrtc` (and once
   with neither, *auto*). Each time, hold still five times and move your head
   slowly for ten seconds, and note from the numbers **pose->frame** (median,
   p95), **stop->refined**, **remote** fps and the *via* line (*via WebRTC N
   Mbit/s*, decode ms, paired; or *via JPEG* and why). With *webrtc* the line
   must read *via WebRTC* within a few seconds; if it says *via JPEG (WebRTC:
   …)*, note the reason (a Mac firewall that blocks incoming UDP for Python is
   the likely one). Look at the refined image with video: it must sit exactly
   where the JPEG one did (no jump when it fades in) and show no blocks or
   smearing; note any colour difference. With *auto*, note which path it
   chose (the *via* line).

### From the desktop to the headset

1. **Pairing over USB.** On the Mac, open HCG 44 in the desktop application.
   Connect the Quest 3 with a USB-C cable, choose *Tools → Views → Open in VR
   Headset…*: the dialog must say *Ready* with the headset's model. Press
   *Open on Headset* and put the headset on. Within about 10 s the Quest
   Browser must show the page with the cube loaded and the status ending in
   *in the desktop's session*, without typing anything. Note the time from
   the press to the cube drawn.
2. **Nothing left in the address.** Tap the Quest Browser's address bar: it
   must read `http://localhost:<port>/vr/` with nothing after `#`. Open the
   browser's history: the entry must not contain `token=` or `session=`.
3. **Enter VR.** Open the hand menu. Read its text at a comfortable distance
   (0.5 to 1 m): note whether every line is readable without leaning in, and
   which is not.
4. **Pointing.** Point the primary controller at the menu: a yellow cross must
   follow the ray on it and the widget under it must light up; the cube's
   white pick cross must disappear meanwhile. On the Look tab press ▶ (next
   colour map), *max intensity* then *emission*, *Reset*, *full cube
   (desktop)*; drag the *Threshold* and *Opacity* sliders and the two ends of
   *Channels*: the cube must change at once, the slider follow the controller
   without visible lag, and the velocities under *Channels* match the slab on
   the cube's axis. Note the numbers' **FPS**, **missed %** and **menu redraw
   … ms + upload … ms** while dragging a slider, and FPS with the menu closed:
   they should be the same.
5. **Selections.** Choose *spectrum* (Select mode) and pull the trigger on
   NGC 3187: the spectrum must appear in the results window. Choose *lasso*
   and select the galaxy: the green mask and the numbers must appear;
   *Clear* must remove them. A trigger pulled on the menu must never start a
   selection on the cube behind it.
6. **Same session.** Back on the Mac (headset still on, or after), *Download
   recipes (.json)* on the page or copy the last recipe: its `dataset.path` is
   the desktop's file, and its `backend_session_id` the desktop's session (do
   not paste it anywhere public).
7. **Kept settings.** Leave VR, enter again: the colour map and the tool must
   be the last ones used. Reload the page (it loads the same cube again) and
   check the same.
8. **Passthrough.** Enter passthrough: the menu must say *paused in
   passthrough*; leave it and enter VR: within a few seconds the refinement
   must work again (*refined (still view)* when you hold still).
9. **Wi-Fi.** Repeat 1 and 4 with adb connected over Wi-Fi; note the time to
   load and the refinement's **pose->frame** on the numbers.
10. **A cluster backend** (if you use one): follow
    [On a cluster backend](#on-a-cluster-backend).

### Pairing by code, with the headset (#210)

Pairing by code has been checked only in Chrome on the Mac, against a local
pairing service and the in-repository fake
([Checking it without a headset](public-loader.md#checking-it-without-a-headset)).
On the Quest 3, with the pairing service and the loader hosted as in
[Hosting headset pairing](../../deployment/vr-pairing.md), and the desktop's
**Settings ▸ Backends ▸ Headset Pairing** filled in, check the following. After
each step that connects, copy the page's diagnostics (*Copy diagnostics*) and
keep the `transport:` line.

1. **Pair by typing the code.** Open HCG 44 on the desktop, then *Open in VR
   Headset…*. On the Quest, open the loader's address in the Quest Browser,
   type the code and press *Connect*. The dialog must go from *Waiting for the
   headset* to *The code was entered* to **Headset connected**, and the cube
   must load with *in the desktop's session*. Note the time from *Connect* to
   the cube drawn, and whether the headset's keyboard made the code easy to
   type.
2. **Pair by QR code.** *New Code* in the dialog, then scan its QR code with
   the headset. Note whether the Quest offers to scan it, whether it opens the
   Quest Browser on the loader with the code filled in, and whether the
   pairing then starts without typing anything. The address bar must not keep
   `#pair=` after the page has read it.
3. **Enter VR inside the loader's frame (the main risk).** Press **Enter VR**
   on the paired page: the immersive session must start, from inside the
   loader's iframe, with both controllers and the hand menu working. Note
   anything the Quest Browser asks or refuses. Try *Enter passthrough* as
   well.
4. **The link on Wi-Fi.** From the `transport:` line, note the ICE types (for
   example `host/host udp`, `srflx/host udp`) and the round trip, with the
   headset and the Mac on the same Wi-Fi. `relay` must not appear on a network
   that lets devices talk.
5. **Frame rate and refinement over the pairing.** In VR, read the wrist
   numbers: **FPS** and **missed %** with the menu closed and open; hold still
   and note **pose->frame** (p50 and p95), **stop->refined** and the *via* line
   (*via WebRTC* or *via JPEG*). Compare with the same cube over the USB cable
   in the same session.
6. **Wi-Fi off and on.** In VR, turn the headset's Wi-Fi off for about 20 s,
   then on (or move out of the access point's range and back). The card must
   say *Backend not reachable*, then clear by itself without a new code and
   without reloading; note how many seconds. The `transport:` line must then
   read `link up` with `resumed 1×`, and the events list `pair-link-down` then
   `pair-link-up`. A spectrum must work afterwards.
7. **Large transfers.** Load a large cube (*Max side* 512, 16-bit) over the
   pairing, and a large point table if you have one. Note whether the Quest
   Browser drops the link during the download (Chrome on the Mac sometimes
   does): if it does, the `transport:` line shows a resume and the cube must
   still finish loading. Note the load time against the USB cable.
8. **Install as an app.** From the Quest Browser's menu, install the loader as
   an app. Close the browser, start *VisIVO VR* from the app library: it must
   open on the code form without the address bar. Pair with a new code, enter
   VR, leave it, quit the app and start it again: it must ask for a code again
   (or resume, if the same tab is restored), and pair again.
9. **A network that isolates devices.** On a guest Wi-Fi or a hotspot with
   client isolation (the headset on it, the Mac on another network or isolated
   too): without TURN on the service, pairing must fail with *No network path
   to the computer was found* or *The computer did not answer in time*, and
   the dialog say *The code was used, but no headset connected*. With TURN
   configured, it must connect, the `transport:` line read `relay`, and the
   cube load; note its load time, **FPS** and **stop->refined**.
10. **Disconnect and close.** Press **Disconnect** in the dialog: the headset
    must show *The computer disconnected this headset* with *Enter a new code*.
    Pair again, then quit the desktop application: *VisIVO Visual Analytics on
    the computer was closed*.

### Recovery and diagnostics, with the headset

1. Open the page for the first time: the controls guide is on the 2-D page
   with *Got it*; Enter VR does not show it. In VR, the hand menu's Help →
   Guide shows it beside the cube (see [#226](#the-first-quest-3-session-226));
   *Next*, *Back* and *Close* work with the primary trigger.
2. In VR, unplug the USB cable: within 5 s the card says *Backend not
   reachable* and names *adb reverse* and *Open on Headset*, while the cube
   still moves with your hands. Plug it back and press *Open on Headset* on
   the Mac: the card goes by itself (how many seconds?).
3. Quit and restart the desktop application while in VR: *The backend
   restarted*, the cube stays, a spectrum works afterwards; refinement comes
   back (the *Refine* line on the menu's Look tab).
4. Press the Quest's system button and quit the VR session: the 2-D page says
   *The VR session ended* with *Enter VR*.
5. Load a large cube (*Max side* 512, 16-bit) and stay still, then move:
   does the menu say *Quality lowered to hold … Hz*? Read the numbers on the
   wrist: FPS, samples/voxel, eye buffer size before and after.
6. **Copy diagnostics** on the 2-D page: does the clipboard work over
   `localhost` (paste the text in the report)?

### Analysis and hands (#191)

1. Load HCG 44 and enter VR. Lasso tool, seed mode *new*: pull the trigger on
   NGC 3187. The results window says 13 216 voxels and 6.06 Jy km/s.
2. Seed mode *add* (the menu's Analysis tab), trigger on
   NGC 3190 (the source left of NGC 3187 at the same velocity): about 23 900
   voxels, a green cross at the added seed. *carve*, trigger inside NGC 3187:
   the count drops, a red cross. Report the counts and the lasso ms on the
   numbers on the wrist.
3. *Isolate*, then *M0*: a map appears above the cube's right side. Read its
   title, colour bar and unit (JY/BEAM km/s): are the labels readable from
   where you stand? *M1*, *M2*. Then *Show that cube*: the isolated cube
   replaces the cube.
4. *Draw*, then trigger at three places across NGC 3187: the orange path on
   the cube and the PV diagram above the cube's left side, velocity up
   (888 at the bottom, 2183 at the top for HCG 44). *w +* twice, *Undo*.
5. *Download recipes* afterwards (on the page, out of VR): one record per
   seed press, Isolate, moment and PV path.
6. Hands (see [Hand tracking](controls.md#hand-tracking)): put the
   controllers down. Pinch with the primary hand while pointing at the
   Analysis tab's buttons: does one pinch press once, and only once? Drag the
   threshold slider on the Look tab with a held pinch. Point at the cube and
   pinch: does the cross stay where you pointed? Close a hand on the cube and
   move it; close both hands and pull them apart. Pick a controller up again
   mid-way: the ray and grip go on without reloading. Report any missed or
   double press, and the FPS while a hand grabs the cube.
7. With the moment and PV windows shown, the FPS and GPU ms on the numbers
   on the wrist, against the same view without them (*Hide*).

### Selections shared with the desktop, with the headset

Desktop and headset together:

1. On the Mac, open HCG 44, *Tools → Views → Open in VR Headset…*, *Open on
   Headset*, and enter VR.
2. In the headset, choose the Kinematic Lasso and select NGC 3187. Note the
   voxels and the flux in the results window. On the Mac (ask someone, or look
   after taking the headset off), the cube window must show the selection
   within about a second without any click, and the status bar and the
   lasso's panel must say *Kinematic Lasso from the headset: … voxels, flux …
   Jy km/s* with the same two numbers.
3. In the headset, make a box (two corners). The desktop must show it as a
   selection with the same voxel count as the results window's *voxels …
   valid / …*.
4. On the Mac, double-click another galaxy of HCG 44, wait for the selection,
   press *Show in VR Headset*. In the headset the green mask must move to that
   galaxy within about a second and the results window must say *voxels from
   the desktop*. *Download recipes (.json)* on the page must not have a new
   entry for it.
5. Back on the Mac, press *Isolate → new cube* on a headset lasso: a new cube
   must be made, and its product's Provenance must say *Made in: VR headset*
   and list the headset's seed; do the same on a headset box: its Provenance
   gives the box's pixels and channels. Note the **share** line on the numbers
   on the wrist (sent, received, publish and receive ms), and the times of
   steps 2 and 4.
6. Repeat 2 and 4 over Wi-Fi.

### Points, catalogues, flows, with the headset

1. **A point table from the desktop.** Open a large VBT (at least 10⁷ rows,
   for example the snapshot under DemoData converted to VBT) in the desktop,
   *Tools → Open in VR Headset…*, put the headset on: the table must load
   with nothing typed. Enter VR; on the numbers on the wrist note **FPS**,
   **missed %**, **GPU ms** and the line *points … drawn of … sampled*, at
   *quest-low*, *quest* and *quest-high* (the menu's Layers tab → *rows*),
   with *hold the frame rate* on and then off (on the page before entering
   VR). Move your head quickly: the points must never lag.
2. **Splats.** Press *points/splats* on the Layers tab: note the same numbers.
3. **Refinement of a point table.** Hold still: within about a second the
   denser image of every row must fade in, on the same place as the headset's
   points; note **pose->frame** and **stop->refined**.
4. **A catalogue.** Open HCG 44 from the desktop; on the page (before entering
   VR) place `sofia_hgc44_cat.xml` (catalogue file); in VR, check that every
   cross sits on a galaxy's emission at its velocity, and that *labels* and
   *catalogue* on the Layers tab work. If the VLKB is reachable, open a Hi-GAL
   or CHIMPS cutout and place *VLKB compact sources*: the ellipses must sit on
   the compact emission.
5. **A velocity field.** Open one in the desktop's velocity viewer, *Open in
   VR Headset…*: compare the streamlines in the headset with the desktop's at
   the same seeds count (they are drawn from another random set of seeds:
   compare the flow's structure, the attractors and the speeds' colours). Note
   FPS and GPU ms with streamlines and arrows on.

### Large cube (out of core), with the headset

1. Open a cube above 6 GiB on the desktop (or make one with
   `validation/vr_large_cube_make.py` in a scratch directory) and *Open on
   Headset*. Without a pyramid, the page must show *Building the cube's brick
   pyramid… N %* rising, then the cube; note the build time and the time to
   the cube drawn. Close and open it again: note the time (no build now).
2. The status line and the menu's Look tab must say *pyramid level K* in the binning.
   Note the headset's **FPS** and **missed %** with the reduced cube.
3. Set the threshold source to *full cube (as on the desktop)*, hold still:
   the menu must show *refined* and the wrist numbers' *server* line must
   say *pyramid level 1* (or the level the backend can hold). Note
   **pose->frame** (p50 and p95) and **stop->refined** over USB and over
   Wi-Fi, and compare with HCG 44 in the same session.
4. Move the clipping box and the channel slab: the refined frame must clip
   where the headset's own picture does. Lean into the cube: the refined
   image must stay aligned with the reduced one (no shift while you hold still).
5. Pick a spectrum and run the lasso on a source: the results must arrive
   (the lasso can take 10-15 s on a large cube); note the times shown.

### On a cluster backend

What to measure on PLEIADI (or another site), with the headset paired as in
[Headset with a cluster backend](cluster.md):

1. Note the dialog's first two lines (remote, the tunnel's port, the page
   build) and any warning.
2. In VR, hold still on the cube and read the numbers on the wrist: the line
   `refine refined … remote … fps`, **pose->frame** p50 and p95, **stop->refined**
   (and *full*), `server … ms render … total`, and `link far rtt … ms
   window …`. Turn your head slowly and stop five times; note stop->refined
   each time.
3. Check that no `reprojection: pose only` line appears (depth works on the
   V100 EGL path); if it does, note its reason.
4. Repeat with adb over Wi-Fi.
5. From the job log, copy the `[startup] VR page …` line.
