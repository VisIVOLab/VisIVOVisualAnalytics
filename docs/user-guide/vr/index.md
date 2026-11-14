# Cubes in a VR headset

A spectral cube explored in a Meta Quest 3 driven from a Mac, with no Windows
PC and no OpenXR runtime: the backend serves a web page, the headset's browser
opens it with WebXR, and the headset's own GPU draws a **reduced copy** of the
cube, so turning your head or moving the cube with the controllers never
waits for the network. Everything computed (moments, masks, the Kinematic
Lasso, spectra) still runs on the full cube on the backend.

| | |
|---|---|
| **Where** | the *VisIVO VR* page your institute hosts, paired with the backend by a code; or `http://<backend>/vr/`, served by the backend itself (USB cable, a cluster's portal) |
| **Headset** | Meta Quest 3 (Quest Browser, WebXR); any WebXR browser should work |
| **Without a headset** | the same page in a desktop browser, with the mouse (for development and checks) |
| **Data** | a FITS cube the backend can open; a point table, a catalogue over a cube, a velocity field |

```{toctree}
:hidden:
:maxdepth: 2

quickstart
desktop
controls
analysis
data-types
cluster
public-loader
troubleshooting
reference
```

- [Quickstart and pairing](quickstart.md): pairing the headset by code, the
  USB cable for offline use, the page on the Mac without a headset.
- [The headset from the desktop](desktop.md): Show on Headset for a headset
  already connected, the Headset Activity list and its products, following
  the headset's look.
- [Controls and the hand menu](controls.md): hands, modes, the hand menu and
  its tabs, hand tracking.
- [Analysis in the headset](analysis.md): spectra, the Kinematic Lasso, box
  regions, Isolate and Remove, moments, PV slices, selections shared with the
  desktop.
- [Point tables, catalogues and velocity fields](data-types.md).
- [Headset with a cluster backend](cluster.md).
- [How the public loader works](public-loader.md): the page a paired headset
  opens, its link and the `transport:` line. Hosting it is in
  [Hosting headset pairing](../../deployment/vr-pairing.md).
- [Troubleshooting](troubleshooting.md).
- [Technical reference](reference.md): the page's settings, the reduced cube's
  fidelity, refinement, the routes, the benchmark, the measurements and the
  checks to run on a Quest 3.

## The work so far

The work is tracked in
[#166](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/166): the
headset renders
([#167](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/167));
selections made in the headset (a spectrum, the Kinematic Lasso, a box
region) are computed on the full cube by the backend
([#171](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/171), see
[Selections in the headset](analysis.md#selections-in-the-headset)); while the
view is still, the Mac's GPU renders the full cube for the headset's eyes and
the page blends those frames in
([#170](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/170), see
[Refinement](reference.md#refinement-full-quality-frames-from-the-backend));
and a cube open on the desktop continues in the headset in the same backend
session, with its controls in the headset
([#172](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/172), see
[Open a cube from the desktop](quickstart.md#open-a-cube-from-the-desktop) and
[the hand menu](controls.md#the-hand-menu)); point tables, catalogues over a
cube and velocity fields are shown too
([#190](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/190), see
[Point tables, catalogues and velocity fields](data-types.md)). The headset
pairs with the desktop by a code, without a cable or adb
([#210](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/210), see
[Pair by code](quickstart.md#pair-by-code)). A selection
made in the headset appears in the desktop's cube viewer and a desktop lasso
can be sent to the headset
([#188](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/188), see
[Selections shared with the desktop](analysis.md#selections-shared-with-the-desktop)).
What is not done yet is listed under [Known limits](#known-limits).

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

How much of the emission the reduced cube shows, against the full cube, is
measured under [Fidelity of the reduced cube](reference.md#fidelity-of-the-reduced-cube).

## Known limits

- **Tokens in the access log.** The VR page sends its token in request
  headers and, on the render WebSocket, as a subprotocol: it is in no URL.
  The desktop's remote 3-D view still puts it in its WebSocket address
  (`/v1/render/<id>/ws?token=…`), and uvicorn's access log records that
  address, so the backend's log can hold the token: keep that log private.
- **Checks on the headset are pending.** Everything in this guide was
  measured in a desktop browser on the Mac (Chrome, the same code path with
  the mouse); the frame rates, latencies, menu readability and controller
  handling on a Quest 3 come from the checks listed under
  [What to check on the Quest 3](reference.md#what-to-check-on-the-quest-3)
  and are not yet recorded here.
- **One cube at a time**: at most 2048 reduced voxels on a side (the GPU's 3-D
  texture limit), 64 Mi voxels and 64 MiB of texture (so 32 Mi voxels in
  16 bits), for what a headset browser can allocate; the backend refuses a
  larger reduced cube before reading anything, and the page checks its GPU's
  limit before asking. Choose a larger binning.
- **Tools in the headset**: a spectrum, the Kinematic Lasso with Add and
  Subtract seeds, Isolate and Remove, the moment maps M0-M2 of the selection or of the whole field, a
  PV slice along a path, and a box region. No hand refinement of the mask on
  a channel (the desktop's polygon edit), no kinematic fit; the moment of a selection is
  always over the whole channel range with no threshold mask, the PV path
  is not edited point by point (*Undo* removes the last point), and the PV
  path is drawn with points, not a curve. The desktop has no recipe import: a
  VR recipe is replayed by posting its requests, as a desktop recipe is.
- **Shared selections**: the desktop follows the headset's selections once
  *Open in VR Headset…* was used in that cube window (or a selection was sent
  from it), in the desktop's session only; one selection per cube is shared,
  the latest, of at most 16 Mi voxels in its bounding box (a larger box made
  in the headset is measured there but not shared, and the page says so). The
  desktop sends a selection only when asked (*Show in VR Headset*); its
  sliders do not regrow a selection received from the headset.
- **Refinement**: a cube drawn out of core is refined from one whole level of
  its brick pyramid, the finest within the GPU budget
  (`VISIVO_RENDER_OOC_BUDGET_BYTES`, 2 GiB), not from its full resolution;
  the view-dependent blocks of the desktop's out-of-core view are not used
  for the headset (see [Large cubes](reference.md#large-cubes-from-the-brick-pyramid)). Refined
  frames travel as JPEG over the render session's WebSocket, or as H.264 video
  over WebRTC when the link is slow (see
  [Over Wi-Fi: refined frames as video](reference.md#over-wi-fi-refined-frames-as-video-webrtc));
  through `adb reverse` alone (USB, no Wi-Fi) the video cannot connect, so a
  slow link lowers their rate there. They are **off in passthrough** and
  during the benchmark. The opacity of the refined frame is matched to the
  headset's by path length, which is **approximate**: with coarse binning the
  refined image can look a little denser or fainter than the reduced one. Not
  cropped in isosurface mode (the page does not use it).
- **Pairing by code** needs an internet connection on both sides, a pairing
  service and loader hosted by your institute (no server is built in), and,
  on a Mac with Apple Silicon, macOS 14 or newer. The headset and the computer
  must reach each other on the local network, or go through a TURN relay,
  more slowly. It has not yet been checked on a Quest 3 (see
  [the checks](reference.md#pairing-by-code-with-the-headset-210)).
- **The USB way**: *Advanced (USB cable)* needs adb and a headset in developer
  mode, and opens the page through `adb reverse` only (USB or adb over Wi-Fi),
  not by the Mac's network address; a backend at a network address must be
  reached through a local tunnel. A backend under a path prefix works only
  behind a proxy that strips the prefix. The link carries the token (see the
  [caution](quickstart.md#open-a-cube-from-the-desktop)). The settings kept in
  the browser are per browser: the Mac's and the headset's are separate.
- **The hand menu** has the main controls only: the stretch, velocity stretch,
  binning, texture, Kinematic Lasso parameters and eye-buffer settings are on
  the page (before entering VR); see [The settings on the page](reference.md#the-settings-on-the-page).
  **Hand tracking has not been tried on a headset**: the pinch and grip
  thresholds (pinch closes under 1.2 cm between the fingertips' surfaces,
  opens over 2.8 cm; grip under 0.72 of the palm's length, opens over 0.95)
  were set and tested on synthetic hands; see
  [Hand tracking](controls.md#hand-tracking) and the
  [checks](reference.md#analysis-and-hands-191).
- The labels follow the central row and column of the field; far from them on
  a wide projected field the true coordinate of a pixel differs a little.
- **Reduction**: a cube below the in-core limit is read whole once to reduce
  it (0.6 s for HCG 44, 1.1 s with a cold cache). A larger one is reduced
  from a level of its brick pyramid, by the mean only: **max binning**,
  *Read from: whole cube*, and a pyramid the backend cannot build read the
  whole cube (in bounded pieces, slowly: 11 s for 11 GiB). Automatic factors
  may come out up to twice what was asked on an axis (more only when even a
  16 GiB level would not keep them); given factors that would need a level
  over 1 GiB are refused with the nearest ones that fit (see
  [Large cubes](reference.md#large-cubes-from-the-brick-pyramid)). The first time, the
  pyramid is built (14 s for an 11 GiB cube on an M5 Max).
- **Box statistics** (`mask_stats_bin`) are refused above 64 Mi voxels: on a
  large cube draw a smaller box.
- **Point tables, catalogues, velocity fields** (#190): a point table is
  refined in the headset's own colours only (no selections, no filters set on
  the page; the desktop's filters are not carried over); the ray-traced
  particle render is not used for refinement (seconds per frame on the CPU,
  where a stereo frame must come in a few hundred milliseconds). A velocity
  field is not refined, its streamlines are lines one pixel wide (the
  desktop's are tubes), a later change of the desktop's *Box size* reaches
  the headset only with a new link, and
  basins and the divergence isosurfaces stay on the desktop. A catalogue's
  spectral column is read in the cube's own reference frame (no barycentric
  to LSR change); DS9 region files are not placed (the desktop's cube overlay
  reads them); the page's catalogue choice is not carried by the pairing link.
  The desktop's cube overlay of a catalogue file still uses its linear
  approximation of the WCS; the headset's uses the full projection.
- Comfort: prefer short, seated sessions; passthrough mode helps.
