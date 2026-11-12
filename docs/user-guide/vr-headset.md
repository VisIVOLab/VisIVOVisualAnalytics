# Cubes in a VR headset (Meta Quest, from a Mac)

A spectral cube explored in a Meta Quest 3 driven from a Mac, with no Windows
PC and no OpenXR runtime: the backend serves a web page, the headset's browser
opens it with WebXR, and the headset's own GPU draws a **reduced copy** of the
cube, so turning your head or moving the cube with the controllers never
waits for the network. Everything computed (moments, masks, the Kinematic
Lasso, spectra) still runs on the full cube on the backend.

The work is tracked in
[#166](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/166): the
headset renders
([#167](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/167));
selections made in the headset (a spectrum, the Kinematic Lasso, a box
region) are computed on the full cube by the backend
([#171](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/171), see
[Selections in the headset](#selections-in-the-headset)); while the view is
still, the Mac's GPU renders the full cube for the headset's eyes and the page
blends those frames in
([#170](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/170), see
[Refinement](#refinement-full-quality-frames-from-the-backend)); and a cube
open on the desktop continues in the headset in the same backend session, with
a panel in the headset for the controls
([#172](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/172), see
[Open a cube from the desktop](#open-a-cube-from-the-desktop) and
[The panel in the headset](#the-panel-in-the-headset)); a selection made in the
headset appears in the desktop's cube viewer and a desktop lasso can be sent
to the headset
([#188](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/188), see
[Selections shared with the desktop](#selections-shared-with-the-desktop)).
What is not done yet is listed under [Known limits](#known-limits).

| | |
|---|---|
| **Where** | `http://<backend>/vr/`, served by the backend itself |
| **Headset** | Meta Quest 3 (Quest Browser, WebXR); any WebXR browser should work |
| **Without a headset** | the same page in a desktop browser, with the mouse (for development and checks) |
| **Data** | a FITS cube the backend can open |

## Quickstart

1. **Once:** install adb (`brew install android-platform-tools` on a Mac) and
   turn on **developer mode** for the headset in the Meta Horizon app on your
   phone (Devices → your headset → Headset settings → Developer mode).
2. Open the cube in the desktop application and connect the Quest with a
   USB-C data cable.
3. **Tools → Views → Open in VR Headset…**. The dialog says what is missing,
   step by step; the first time, put the headset on and accept **Allow USB
   debugging** (tick *Always allow from this computer*).
4. Press **Open on Headset**, put the headset on: the cube loads in the Quest
   Browser. Press **Enter VR**.
5. The first time, a short controls guide stands in front of you (*Next*,
   *Close*; the **?** on the panel shows it again). Grip moves the cube, the
   right stick sets threshold and opacity, the left stick the velocity slab,
   the right trigger the tool; the panel to the left of the cube has the rest.

If something goes wrong, the page says so in the headset and offers the way
out: see [Troubleshooting](#troubleshooting).

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

## Open a cube from the desktop

With a cube open in the desktop application, **Tools → Views → Open in VR
Headset…** (or the same entry in Inspector ▸ Analysis) opens it on a Quest
connected to the Mac, in the same backend session: the headset's page
receives the token, the session, the dataset and the cube's path, so there is
nothing to paste or type on the headset. This is not *Open in VR*, which
sends the desktop's own 3-D view to an OpenXR runtime on this computer (see
[Open in VR](cube-viewer.md#open-in-vr)); it needs no VR runtime here.

**Once, before the first time:**

1. **Install adb**, the Android platform tools. On a Mac:
   `brew install android-platform-tools`; on Linux, the distribution's
   `android-tools` / `adb` package; on Windows, the *SDK Platform-Tools*
   from developer.android.com, unpacked anywhere. The application finds adb
   on the `PATH`, in Homebrew's `/opt/homebrew/bin` and `/usr/local/bin`, in
   the Android SDK (`$ANDROID_HOME/platform-tools`,
   `~/Library/Android/sdk/platform-tools`, `~/Android/Sdk/platform-tools`,
   `%LOCALAPPDATA%\Android\Sdk\platform-tools`), even when it was started
   from the Dock with a short `PATH`.
2. **Turn on developer mode** on the headset (Meta Horizon app on the phone:
   *Devices → Headset settings → Developer mode*).
3. On Linux, adb may need the udev rule for Android devices to open the
   headset's USB device; the dialog says so when it is missing.

**Each time:**

1. Connect the headset with a USB-C cable (or connect adb over Wi-Fi, see
   [below](#on-the-quest-3-over-wi-fi)) and put it on.
2. In the cube window, **Tools → Views → Open in VR Headset…**. The dialog
   says what it finds and the next steps, numbered, with a link to this
   guide; it checks again every few seconds, and **Retry** checks at once:
   - *adb was not found*: the exact command that installs it on this system
     (`brew install android-platform-tools`, `sudo apt install adb`,
     `winget install Google.PlatformTools`);
   - *The Quest is connected by USB, but adb does not see it: developer mode
     or USB debugging is off on the headset* (macOS: the dialog also looks at
     the Mac's USB devices, for Meta's vendor id 0x2833, when adb lists
     nothing): turn developer mode on, restart the headset, reconnect;
   - *No headset on USB* (macOS) or *No headset connected*: the cable (a
     data cable, not a charge-only one), the headset switched on, or
     developer mode;
   - *The headset has not allowed this computer yet*: in the headset, accept
     **Allow USB debugging** (tick *Always allow from this computer* so it is
     not asked again);
   - *Waiting for the headset to authorise this computer*: adb is still
     connecting; accept the prompt in the headset if it shows one;
   - *The headset is offline*: wake it up, or reconnect the cable;
   - *Ready*: the headset's model. With several devices attached, choose the
     headset in the list; the one used last time is chosen first.

   When a step fails, **Copy Error** copies the error, the state and the adb
   found (never the token) for a support request.
3. Press **Open on Headset**. The application forwards the backend's port to
   the headset (`adb reverse tcp:<port> tcp:<port>`) and opens the page in
   the Quest Browser (`am start` with a view intent; in the headset's default
   browser if the Quest Browser is not installed). The cube loads on the
   page; press **Enter VR**.

Repeat *Open on Headset* after unplugging the cable or restarting the
headset: the port forwarding ends then.

**Copy Link** copies the same address for opening it by hand: forward the
port first (`adb reverse tcp:<port> tcp:<port>`, the port is the one in the
link), then open the link in the headset's browser.

The address is `http://localhost:<port>/vr/#token=…&session=…&dataset=…&path=…`.
Everything after `#` is never sent to the server, so neither the token nor
the cube's path is in any server log, and the page removes the whole pairing
(token, session, dataset and path) from the address bar and from the
browser's history entry as soon as it has read it.

**On the headset**, the page then, with nothing to type:

1. takes the token as if it had been pasted (kept for this tab only, in the
   browser's session storage; in its local storage only if you tick
   *remember on this device*);
2. uses the desktop's **session** for every request (`X-Visivo-Session`), so
   the headset works on the datasets the desktop has open, and every recipe
   it records names that session;
3. loads the cube: the desktop's **dataset** itself when the session still
   holds it, with the binning set on the page (*auto* by default). The status
   line and the panel in the headset say *in the desktop's session*.

When that is not possible the page falls back and says so:

| What happened | What the page does | It says |
|---|---|---|
| The session no longer holds the dataset (the backend was restarted, or the cube was closed) | opens the cube's **path** again, in the same session | *in the desktop's session (cube opened again)* |
| The backend does not know or refuses the session id (another token's session, a strict multi-user backend after a restart) | opens the path in a **new session** of its own | *in a new session (the desktop's is not available here)* |
| The link has no token, or a malformed one | nothing is loaded | asks for the token: paste it and press *Open and load* |
| A value of the link is malformed (a broken `%` escape) | that value is ignored; the others are used | names the value |

The session id, the dataset and the path are kept for this tab only, like the
token: reloading the page in the headset loads the same cube again; opening
another cube on the page with *Open and load* forgets them.

```{caution}
The link carries the backend's access token: whoever has it can use the
backend as you. The application never writes it to its log, but *Open on
Headset* passes it on the command line of the `adb` process on this computer
and of `am` on the headset, where other programs of the same user can read
it, and *Copy Link* leaves it on the clipboard. Use it with your own computer
and headset.
```

**Remote and cluster backends.** The headset reaches the backend as
`localhost:<port>` through adb, so the client's backend address must be
`localhost`, `127.0.0.1` or `[::1]`: the local backend, or a remote one reached
through a local tunnel, as a cluster connection is. A backend the client
reaches at another address (a machine on the network at
`http://192.168.x.y:8000`) cannot be reached from the headset this way, and
the dialog says so: connect to it through an ssh tunnel
(`ssh -L 8000:localhost:8000 host`) and open the cube from there. See
[Headset with a cluster backend](#headset-with-a-cluster-backend).

## Headset with a cluster backend

The same headset session works with the backend on an HPC node (PLEIADI or
any other site, issue #187): the headset draws its reduced cube itself, so
motion never waits for the network, and the cluster's GPU renders the refined
stereo frames for the headset's pose when you hold still. The page stays
`localhost` on the headset: `adb reverse` forwards the headset's
`localhost:<port>` to the Mac's `localhost:<port>`, which is the local end of
the desktop's SSH tunnel to the node.

```
Quest  localhost:18900 --adb reverse--> Mac localhost:18900 --ssh tunnel--> node:8000 (backend, GPU)
```

**Steps.**

1. Start the backend on the cluster and its tunnel as usual: *Connect to a
   cluster…* in the Data Hub, or `scripts/visivo_slurm_tunnel.sh` (local port
   18900 by default; the Data Hub picks a free one from 18901). Connect the
   desktop to it (the Data Hub then shows the backend as *remote*).
2. Open the cube in the desktop from the cluster's file browser.
3. **Tools → Views → Open in VR Headset…**. The dialog's first line names the
   backend: *Backend: remote, through this computer's tunnel at
   localhost:18900*, then the headset page's build the backend serves (a
   12-character hash). The link carries **the cluster backend's token** and
   the desktop's session on it, never the local backend's.
4. *Open on Headset* runs `adb reverse tcp:18900 tcp:18900` and opens the
   page: from here on as with a local backend.

**The dialog's backend check.** Before pairing, the dialog asks the backend
`GET /v1/vr/backend_check` (see the [API](../backend-api/meta-files.md)),
without following a redirect (a portal's sign-in page is reported, and the
token does not go along). It warns when
the backend predates that route, lacks a route the page calls, or speaks an
older headset protocol; on a cluster that almost always means the backend
runs an **older copy of its code**: the `--backend-src` overlay (a copy of
`backend/app` bound over the image's) or the image itself is older than the
desktop. The page and its routes live in that same tree
(`backend/app/static/vr`, `backend/app/routers`), so refreshing the overlay
with the desktop's `backend/app` updates both. The backend's job log has one
line at startup that tells the same:
`[startup] VR page <build> (<n> files, newest <date>); backend code newest
<date> in <dir>; VR routes all present; stereo depth auto`. Compare
`<build>` with what a local backend of the same version logs, and check the
dates. The dialog refuses to open the page only when the backend says it
serves no headset page, or refuses the token; an answer it does not
understand is a warning.

The dialog calls a backend *remote* when it is not the desktop's local backend
URL (by default `localhost:8000`). A tunnel to a cluster opened on that same
port (`ssh -L 8000:node:8000`) is therefore labelled *local*: the pairing
still works, since the link carries the token of the backend the desktop is
using, but use another local port for a cluster tunnel (18900 and up, as the
tunnel helpers do) so the two are never mixed.

**A backend under a path prefix.** Some HPC portals publish a node's port
through a reverse proxy under a path, for example
`https://portal.example.org/node/r33c1s02/proxy/8000/`, which strips that
path before passing the request on. Reached through a local tunnel to the
proxy, the desktop's backend address is then
`http://localhost:<port>/node/r33c1s02/proxy/8000`; the link opens
`http://localhost:<port>/node/r33c1s02/proxy/8000/vr/`, and the page finds
the prefix in its own address and sends every request and its render
WebSocket under it. The proxy must pass WebSockets. A proxy that does **not**
strip the prefix is not supported (the backend has no root-path setting).
A portal's proxy logs the URLs it passes, often on a machine you do not
control: the page's requests carry the token in a header, never in the URL,
and its render WebSocket sends it as a WebSocket subprotocol rather than in
the address once the backend accepts that (the VR hardening release); with an
older backend the WebSocket address still holds `?token=`, which a portal
logs.
Over `https` the headset must trust the certificate for `localhost`, which a
portal's certificate is not: prefer a plain `http` tunnel.

**Refined frames over the tunnel.** The page measures, frame by frame, the
round trip of its poses (pose sent to frame received, minus the time the
backend spent on the frame). Over a far link (round trip above 30 ms) the
numbers panel says `link far rtt <ms> ms  window <n>`: the page keeps enough
poses in flight (and asks the backend for a matching ack window) to keep the
cluster's GPU busy during the round trip, ceil((rtt + work) / work) with
work = render + depth + JPEG, between 2 and 6. The eye size is **not**
reduced over a far link: a smaller frame arrives sooner, but scaled up to the
eye it is worse than the headset's own picture of the reduced cube (HCG 44,
measured on the Mac with an emulated 80 ms link, against the backend's
full-size reference: page alone 35.9 dB PSNR / 0.968 SSIM on the emission;
refined with the eye at 768 px 35.4 dB / 0.947; refined at full size
43.9 dB / 0.983). Lower *Refined size* on the page by hand if you prefer
rate to sharpness.

**Frames without depth.** A refined frame normally carries a depth image, so
the page can warp it to your current pose with parallax. Some GL stacks (GLES
contexts, some Mesa or EGL builds) give the colour back but not the depth.
The backend checks every depth against its own picture; when it fails, the
frame goes without it, the frame header says `"reprojection": "pose"`, and
after two such frames the backend stops trying for that session. The page
then warps the frame by the pose alone, through a plane at the cube's centre:
exact for turning your head, approximate for moving it, so the frame is
shown only while your eye has moved less than 1 cm since its pose (4 cm with
depth). Without depth the error of a step grows sharply as the eye nears the
cube: leaning into it, or with the eye inside it, the near emission is
misplaced by much more than the few pixels of a step at arm's length. In
maximum intensity (MIP) the ray caster has no first-hit depth at all, so
refined MIP frames are always warped this way (the panel says *no first-hit
depth in MIP*). The numbers panel says `reprojection: pose only (no depth)` and the
reason on the next line. An operator can force this with `VISIVO_XR_DEPTH=0`
in the backend's environment (the dialog then says so).

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

**What to measure on PLEIADI.** With the headset paired as above:

1. Note the dialog's first two lines (remote, the tunnel's port, the page
   build) and any warning.
2. In VR, hold still on the cube and read the numbers panel: the line
   `refine refined … remote … fps`, **pose->frame** p50 and p95, **stop->refined**
   (and *full*), `server … ms render … total`, and `link far rtt … ms
   window …`. Turn your head slowly and stop five times; note stop->refined
   each time.
3. Check that no `reprojection: pose only` line appears (depth works on the
   V100 EGL path); if it does, note its reason.
4. Repeat with adb over Wi-Fi.
5. From the job log, copy the `[startup] VR page …` line.

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
| Y | show or hide the numbers (frame rate, latency) on the left controller |
| Left stick press | show or hide the panel in the headset |
| Right stick press | frame-rate governor on/off |
| Right trigger, pointing at the panel | press a button, or drag a slider (see [The panel in the headset](#the-panel-in-the-headset)) |
| Right trigger, pointing at the cube | the selection tool along the right controller's ray (see [Selections](#selections-in-the-headset)) |
| Left trigger | next selection tool: spectrum probe, Kinematic Lasso, box region |

**With your hands** (hand tracking, no controllers): put the controllers down
and the page switches to your hands within a frame; pick them up again and it
switches back. A **pinch** (thumb tip to index tip) is the right trigger: it
presses the panels' buttons, drags their sliders and fires the selection tool
along the hand's ray (the headset's own pointing ray; the ray is held still
while the fingers close, so the pinch does not move what it points at). A
**closed hand** (middle, ring and little finger curled) is the grip (after
a grip, and for a hand just seen, a pinch counts only once the fingertips have
parted, so opening a fist with the thumb still on the index, or a hand that
appears already pinched, does not press anything): one hand
carries the cube, both hands move, turn and scale it. The sticks and the
A/B/X/Y buttons have no hand gesture: their controls are on the panels (the
panel itself stays shown). A left pinch does not change the tool (it happens
while grabbing); choose the tool on the panel. The fingertips are drawn as
small crosses, the pinch as a line that turns yellow when it closes, the palm
as a cross that grows when the hand grips.

The spatial clipping box, the stretch, the velocity stretch, the binning and
the eye-buffer settings are set on the page before entering VR; the rest is on
the panel in the headset too.

## The panel in the headset

A panel floats in the room to the left of the cube, turned toward you, about
0.8 m away (0.48 × 0.39 m, text 1.0 to 1.5 cm high). It does **not** follow
your head: it stays where it is when you look around or move the cube.
Point the right controller at it: the ray ends on the panel with a small
yellow cross, the widget under it lights up, and the trigger uses it. While
the ray is on the panel, or the trigger was pulled on it, the selection tool
does not fire. When the cube stands between the controller and the panel, the
cube takes the ray; with the controller inside the cube (a cube scaled up
around you), the panel does.

| Row | What it does |
|---|---|
| title bar | the cube's file name; pull the trigger on it and move the controller to **move the panel** (it turns to face you); the place is kept for next time. **?** at its right end shows the controls guide again |
| (two lines) | the cube's shape, the reduced shape, the binning and texture; the session (*the desktop's session*, *own session*, …) |
| Colour map | ◀ ▶ the previous or next colour map; *emission* or *max intensity* |
| Threshold | a slider (fine steps near the noise, as on the page) with its value in the cube's unit; *Reset* puts back the default threshold and colour range |
| from | the default threshold *for this binning* or that of the *full cube* (the desktop's) |
| Colour max, Opacity | sliders |
| Channels | the velocity slab: a range with two ends (the nearer end moves); the channel range and its velocities in km/s with their convention below it; *All channels* |
| Refine | refinement on or off, and its state (*refined (still view)*, *waiting*, *paused in passthrough*, *lost: retrying in N s*, *unavailable: …*) |
| Tool | spectrum, lasso or box; *Clear* removes the selection and the marks |

Under the rows, a line says when the page lowered the quality to hold the
frame rate (*Quality lowered to hold 90 Hz: 0.50 samples/voxel (of 1), eye
buffer 0.85× (of 1×)*), or what a selection is doing.

A slider pressed keeps following the controller until the trigger is released,
even when the ray slips off its row. The results of a selection (numbers and
spectrum) are on the second panel, to the right of the cube. The left stick
press hides or shows the control panel; the numbers on the left controller
(frame rate, latency) are toggled with Y.

**Without a headset**, the same panel is drawn in the page's 3-D view with
*show the headset's panel here* (or `?panel3d` in the address) and used with
the mouse through the same ray code: a click presses, a drag moves a slider
or, on the title bar, the panel.

The panel is redrawn only when what it shows changes, at most 30 times a
second. On the Mac (M5 Max, headless Chrome) a redraw of its 1024 × 832
canvas takes 0.10 ms (0.20 ms 95th percentile) and its upload as a texture
under 0.1 ms on the CPU; redraw, upload and drawing it, waited for on the
GPU, 0.8 ms (1.2 ms). A frame with the panel shown costs 0.03 ms more CPU and
no measurable GPU time than without it. The Quest 3's own figures are on its
numbers panel (*panel redraw … ms + upload … ms*).

### The analysis panel

Under the control panel hangs a second panel, moved and hidden with it, used
the same way (ray and trigger, or pinch):

| Row | What it does |
|---|---|
| Lasso seeds | *new*: the next lasso seed starts a new selection; *add*: it adds a source to the selection shown; *carve*: it cuts a region out of it (the desktop's plain, Shift and Alt double-clicks) |
| Selection | *Isolate* (a new cube with everything outside the selection blanked), *Remove* (the selection blanked), *Show that cube* (load the cube just made in its place) |
| Moment | *M0*, *M1*, *M2*: the moment map of the selection on a plane in the room; *Hide* |
| PV path | *Draw*: the trigger adds the path's points; *Undo*, *New*, *w −*/*w +* (the slit width in pixels), *Hide* |

Below the buttons: the slit width and the tools' state (seeds, the last cube
made, the moment's range, the path being drawn). On the page the same tools
are under the selection controls (*Lasso seed*, *Isolate*, *Remove*, *Show
that cube*, *M0 M1 M2*, *Draw PV path*, *Width*), and the moment map and PV
diagram are shown there as well.

## Settings kept in this browser

The page keeps a few conveniences in the browser's local storage, for this
browser only (the headset's browser and the Mac's keep their own): the
**colour map**, the **threshold source** (*for this binning* or *full cube*),
the **refinement** settings (on or off, refined size, JPEG quality, reproject
with depth), the **panel's place** in the headset (relative to where the cube
first appears), the **last selection tool**, and whether the **controls
guide** was seen (it is shown at the first entry into VR only). Nothing else is stored there:
not the token (unless *remember on this device* is ticked), not the session,
not any result. If the browser refuses storage (a private window), the page
works with the defaults. `?refine=0` or `?refine=1` in the address overrides
the stored refinement setting for that visit.

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
  With *hold the frame rate* on (the default; right stick press toggles it in
  the headset), the page lowers it when frames are missed and raises it back
  slowly, never above this value; while you hold the cube it drops to 60 %.
  When the governor has already lowered it to 0.5 or less (or to its own
  floor, 0.25) and more than 10 % of the frames are
  still missed over 3 s, the page makes the **eye buffer** smaller, one step
  at a time through the scale factors 1.25, 1, 0.85 and 0.7 (absolute
  values of *Resolution*, starting below the one chosen; never below 0.7),
  and does not raise it again in that VR session (each change costs a hitch);
  the next *Enter VR* starts from the chosen resolution. The panel and the
  numbers say so.
- **Memory budget**: before the reduced cube is read, the page checks that it
  fits: the GPU's 3-D texture side, and the texture's size against what this
  device gives one cube (a headset: 256 MiB with 8 GB of memory, 128 MiB with
  4 GB; a computer's browser: 1 GiB). A cube over it is refused with the
  reason (*… needs 300 MiB of texture (900 MiB while loading); this headset
  allows 256 MiB for one cube*): choose a larger binning. The headset's limit
  is a conservative figure, not yet measured on a Quest 3.
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
rendering only (the panel says *paused in passthrough*). The backend's
frame has no transparency (it is composited over black), so over the room it
would either darken the room where the headset draws emission that the full
cube does not have, or glow where only the full cube has emission. Refinement
is also off while the headset benchmark runs. In both cases the page **closes
its render session** (so it holds no render slot or GPU memory on the backend
meanwhile) and opens a new one when you leave passthrough or the benchmark
ends.

**Selections stay visible.** The backend's frame has no selection in it: the
box outline, the axes, the labels, the controller rays and the panels are
drawn after it. While a refined frame is on show (fading included), the
selection mask (green) is left out of the headset's own volume and drawn
once, by the headset, on top of the refined frame (only the selected
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
(*refine unavailable: no backend cube*). A cube the backend would draw out of
core (larger than its in-core limit, `VISIVO_RENDER_OUT_OF_CORE_BYTES`)
cannot give stereo frames yet: the page learns it from `reduced_info`
(`render_out_of_core`) and asks for no session at all, so no brick pyramid is
built for it; the panel says *refine unavailable: the backend draws this cube
out of core*. A clipping box the backend would refuse is not sent (nor any
pose with it), and the panel says why.

**Settings** (on the page, section above *Selection tool*; on or off also on
the panel in the headset; all kept in this browser):

- **refine with the backend's full cube**: on by default (`?refine=0` in the
  address turns it off for that visit).
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
still turns the cube. The tool is chosen with the left trigger, on the panel in the headset or on the page
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

### Refining the selection: add, carve, Isolate, Remove

The lasso's seed mode (*new*, *add*, *carve*, on the analysis panel or the
page) decides what the next lasso trigger does, as the desktop's plain, Shift
and Alt double-clicks do: *add* grows a second source into the selection
(union), *carve* removes the region the grow from that seed reaches; the
primary seed stays. Each press is one grow on the full cube
(`/v1/cube/kinematic_select_bin` with `add_seeds` and `sub_seeds`, the
desktop's request), so the selection is always the one its seeds and
parameters make; the extra seeds are drawn as green (added) and red (carved)
crosses, and the results panel counts them.

*Isolate* and *Remove* post the mask shown, exactly (not a re-grow), to
`/v1/cube/mask_apply_bin` with `blank_outside` or `blank_inside`, as the
desktop's buttons do: the new cube goes to the Workspace Exports with the
selection's recipe in its FITS HISTORY, and is registered in the session.
*Show that cube* loads it in the headset in place of the cube shown. Each
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
not recorded), and the panel says so, even after the new cube's reset. A
shared mask edited by hand is recorded as the desktop records one: *Hand-edited*
yes, `hand_edited=yes` in the new cube's HISTORY, the apply step's
hand-edited body and the desktop's caveat (the parameters regrow only its
geodesic part; the cube itself is the exact record). A selection received from the
session's selection hub is recorded as the desktop records one it received:
a box as one `mask_apply_bin` step over the box (its origin and size, the
only apply step that records its ROI, since the box is the selection); a
selection that came without its grow request as the mask only, with the
desktop's caveat that no call regenerates it; one with its creator's grow as
that grow and the apply. *Made in* says where it was made, and the moment of
such a selection records the same steps before its own. No recipe ever has a
grow step without a request.

### The moment of the selection

*M0*, *M1*, *M2* compute the moment map of the selection the desktop's way:
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
grow, *Clear*, a new cube), file and session entry, and the analysis panel
says so (or says why it stays, for example when the token may not delete
Workspace Exports); the page deletes only files carrying that marker. It is
named temporary in the moment's recipe too. While it exists it is an ordinary
dataset of the session: the desktop's lists of the session's datasets show
it, and a desktop tool opened on it in that time would lose it when the
headset's selection changes (the backend has no way to mark a dataset as
hidden). A moment of a selection no longer shown is removed from the room. The map is shown cropped to the selection's
bounding box (plus a tenth of its size, at least 3 pixels), x and y in pixels
of the cube, on a plane above the cube's right side with its colour bar and
unit. Colours as on the desktop: inferno for M0, a diverging cool-warm for
M1 on the desktop's range (median ± 4 robust sigma, values outside it
transparent, as the desktop draws a velocity field), viridis for M2. A
velocity in m/s (also written `m s-1`, `m.s**-1`) is shown in km/s (the numbers
divided by 1000); the recipe keeps the backend's unit.

### A PV slice along a path

*Draw* arms the PV path: each trigger press (or click on the page) adds a
point where the ray picks the cube (the first drawn sample, else the middle of
the ray inside the box), as a fractional pixel of the full cube (pixel
centres at integers, as the desktop's path). From two points on, the diagram
is computed on the full cube (`/v1/cube/pv`, the desktop's request:
`vertices` and `width_pixels`, default 1) and follows the path as it grows, as
the desktop's editable path does (one request at a time, the newest path next).
The path is drawn on the cube as orange lines on both faces of the velocity
slab, with a post at each point and, while drawing, a line to the ray. The
diagram hangs above the cube's left side: offset along the path in arcsec
across, the rows in the order of the channels' spectral coordinate, lowest at
the bottom (the desktop's orientation, so a cube with CDELT3 < 0 is not
mirrored), labelled with the channels' velocities (the caption says whether
they increase or decrease upward: on a frequency cube they decrease; the ticks
are placed between the rows' own velocities, since an optical velocity is not
linear in frequency), colour bar in the cube's unit. One row per channel: for
a non-linear spectral axis the desktop resamples onto a finer uniform grid,
the page does not. Choosing a
selection tool stops the drawing.

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

**Timings** (Apple M5 Max, Chrome, HCG 44, the mouse on the page): a grow with
seeds 0.6-0.7 s as before, Isolate 0.18 s, Remove 0.12 s, a moment 0.13-0.38 s
(isolate reused), a PV of 50 samples × 158 channels 0.06 s. Redrawing the
analysis panel takes 0.4 ms and a product plane 0.15 ms of CPU, only when they
change; with the panels and both planes shown the page holds 60 fps.

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

## Selections shared with the desktop

A Kinematic Lasso or a box made in the headset appears in the desktop's cube
viewer, and a lasso made on the desktop can be sent to the headset
([#188](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/188)): one
person in the headset and others at the screen look at the same object. It
needs the headset's page to work in the desktop's session, which is what
*Open in VR Headset…* (or its *Copy Link*) gives it.

**From the headset to the desktop.** Every lasso and box made in the page is
published to the session's selection hub on the backend
([Shared selections](../backend-api/share.md)), which measures it on the full
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
its statistics on the results panel, which says *voxels from the desktop*; the
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

**Measured** (backend on an Apple M5 Max, the page in headless Chrome on the
same Mac, HCG 44, `validation/vr_share_e2e.mjs`): NGC 3187's lasso made in the
page (13 216 voxels, 6.061 Jy km/s) reaches a client waiting on the hub
48-98 ms after the page's lasso ended (three runs; the publish with the
backend's statistics is 45-94 ms of it), with the same voxels, the same flux, the same mask
(Dice 1.000 with a regrow of the recorded request) and the page's request as
its record; a desktop lasso of 10 684 voxels is drawn by the page 53 ms after
the desktop starts sending it, and the page adds no recipe; a 56 × 43 × 158
box made in the page reaches the desktop with the same 380 464 voxels and
flux. The desktop's own client (`SharedSelectionLink`) receives a selection
another client of the session publishes in about 100 ms.

**What to check** (desktop and headset together):

1. On the Mac, open HCG 44, *Tools → Views → Open in VR Headset…*, *Open on
   Headset*, and enter VR.
2. In the headset, choose the Kinematic Lasso and select NGC 3187. Note the
   voxels and the flux on the results panel. On the Mac (ask someone, or look
   after taking the headset off), the cube window must show the selection
   within about a second without any click, and the status bar and the
   lasso's panel must say *Kinematic Lasso from the headset: … voxels, flux …
   Jy km/s* with the same two numbers.
3. In the headset, make a box (two corners). The desktop must show it as a
   selection with the same voxel count as the panel's *voxels … valid / …*.
4. On the Mac, double-click another galaxy of HCG 44, wait for the selection,
   press *Show in VR Headset*. In the headset the green mask must move to that
   galaxy within about a second and the results panel must say *voxels from
   the desktop*. *Download recipes (.json)* on the page must not have a new
   entry for it.
5. Back on the Mac, press *Isolate → new cube* on a headset lasso: a new cube
   must be made, and its product's Provenance must say *Made in: VR headset*
   and list the headset's seed; do the same on a headset box: its Provenance
   gives the box's pixels and channels. Note the numbers panel's **share** line in the headset (sent,
   received, publish and receive ms), and the times of steps 2 and 4.
6. Repeat 2 and 4 over Wi-Fi.

## The numbers on the left controller

The small panel of numbers (on the left controller, or next to the cube
without controllers; Y hides it) and the page show, updated four times a
second:

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
- **panel redraw**: the time to redraw the panel in the headset and to upload
  it as a texture (medians of the last 60 redraws), and how many redraws.

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

## What to check on the Quest 3: from the desktop to the headset

These checks need the headset; the page shows the numbers to read. Note each
value and whether each step behaves as described.

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
3. **Enter VR.** The control panel must stand to the left of the cube, about
   arm's length away, turned toward you, and stay put when you turn your head
   or walk a step. Read its text at a comfortable distance (0.5 to 1 m): note
   whether every line is readable without leaning in, and which is not.
4. **Pointing.** Point the right controller at the panel: a yellow cross must
   follow the ray on it and the widget under it must light up; the cube's
   white pick cross must disappear meanwhile. Press ▶ (next colour map),
   *max intensity* then *emission*, *Reset*, *full cube (desktop)*; drag the
   *Threshold* and *Opacity* sliders and the two ends of *Channels*: the cube
   must change at once, the slider follow the controller without visible lag,
   and the velocities under *Channels* match the slab on the cube's axis.
   Note the numbers panel's **FPS**, **missed %** and **panel redraw … ms +
   upload … ms** while dragging a slider, and FPS with the panel hidden
   (left stick press): they should be the same.
5. **Selections from the panel.** Choose *spectrum* on the panel and pull the
   trigger on NGC 3187: the spectrum must appear on the results panel to the
   right. Choose *lasso* and select the galaxy: the green mask and the
   numbers must appear; *Clear* must remove them. A trigger pulled on the
   panel must never start a selection on the cube behind it.
6. **Same session.** Back on the Mac (headset still on, or after), *Download
   recipes (.json)* on the page or copy the last recipe: its `dataset.path` is
   the desktop's file, and its `backend_session_id` the desktop's session (do
   not paste it anywhere public).
7. **Moving the panel.** Pull the trigger on the panel's title bar, move the
   controller, release: the panel must follow and turn toward you. Leave VR,
   enter again: the panel must be back where you put it, the colour map and
   the tool must be the last ones used. Reload the page (it loads the same cube
   again) and check the same.
8. **Passthrough.** Enter passthrough: the panel must say *paused in
   passthrough*; leave it and enter VR: within a few seconds the refinement
   must work again (*refined (still view)* when you hold still).
9. **Wi-Fi.** Repeat 1 and 4 with adb connected over Wi-Fi; note the time to
   load and the refinement's **pose->frame** on the numbers panel.
10. **A cluster backend** (if you use one): follow
    [What to measure on PLEIADI](#headset-with-a-cluster-backend).

## Troubleshooting

The page watches the backend (one small request every 5 s, and every 1 to
5 s while it does not answer) and says what went wrong where you are: on a
card in front of you in the headset (point the right ray at its buttons and
pull the trigger), and in a banner on the 2-D page. Nothing waits on it: the
cube stays on show and moves with your hands, and the card never stays in
the way. A card that lasts (the backend not reachable, the token refused, an
out-of-date page) follows your gaze for 6 s, then moves aside, 40° to the
left and lower, and stays there. **Hide** folds it into a line on the panel
(*⚠ Backend not reachable: retrying — "?" shows it*) until its state
changes; **?** on the panel brings it back. The card is redrawn only when its
text changes (at most once per check, every 1 to 5 s).

| Message | What happened | The way out |
|---|---|---|
| **Backend not reachable**, *the headset cannot reach the backend on the computer* | in the headset over USB: the cable was unplugged, or the headset or adb restarted, so the port forwarding (`adb reverse`) is gone; or the backend is restarting | the page retries by itself (*Trying again by itself every few seconds*); if it does not clear, on the computer press **Open on Headset** again (it restores the forwarding). *Retry now*, *Hide*, *Exit VR* |
| **Backend not reachable**, *the backend does not answer* | in a desktop browser or over the LAN: the backend stopped or the network is down | start the desktop application (it starts the backend); the page retries by itself |
| **The backend is not answering behind its proxy** / **answered with an error** | the address answers with an HTTP error (a proxy's 502, a 500) | the page retries by itself; *Retry now*, *Hide* |
| **The backend restarted** | it came back with its sessions gone | nothing: the page opened the cube again by its path, the view is kept; repeat the last selection if it did not finish |
| **The cube was opened again** | a request found the cube's dataset gone (the desktop's session ended) | nothing: the page opened it again; repeat the selection |
| **The cube could not be opened again** | the file moved or changed shape | *Try again*, or open it again from the desktop |
| **The backend refused the token** | the backend was restarted with a new token, or the token expired | on the computer: **Open in VR Headset… → Open on Headset** (a fresh link); or paste the token on the page, then *Check again*; *Hide* meanwhile |
| **The backend was updated** | the backend serves a newer version of this page than the one open (a tab kept from before an upgrade) | *Reload page* (the cube from the desktop opens again by itself) |
| **This page is out of date** / **Update the application** | the page and the backend speak different versions of the page API (or the backend is older than the page: it has no `/v1/vr/info`) | *Reload page*; if that does not help, update VisIVO Visual Analytics on the computer |
| **The graphics were reset** | the browser took the GPU away (memory pressure, another app) | the page waits for it and uploads the cube again by itself (*Graphics restored*); a VR session cannot outlive it, so press *Enter VR* again |
| **The graphics did not come back** | not restored after 10 s | *Reload page* |
| **The VR session ended** (on the 2-D page) | the headset or the browser ended it: the system menu, the headset taken off for a while | *Enter VR*: the cube and the settings are kept |
| **This cube is too large for this device** | the reduced cube would not fit the [memory budget](#the-settings-on-the-page) | a larger *Spatial bin* or *Spectral bin*, or a smaller *Max side* |
| Refine: *lost: retrying in N s, the headset's own rendering meanwhile* | the render session's connection dropped | nothing: the headset shows its own rendering, and the page reconnects (2, 4, 8 … 30 s) and refines again; if the backend no longer holds the cube, the page opens it again first |

**Upgrades.** The page's code is served from addresses that change with every
version (`/vr/b/<build>/…`) and the page itself is always revalidated, so
after an upgrade a reload always gets the new code. A tab left open across an
upgrade may run a mix of old and new code until it reloads; it notices the
upgrade at its next check and says *The backend was updated*.

**Diagnostics for support.** The page sends its errors and the key events
(page errors, context loss, the end of a VR session, reconnections, the frame
rate once a minute in VR) to the backend's log, a few at a time (lines
`[vr <build>] <event>: …`, logger `visivo.vr`). Before it leaves the page, and
again on the backend, each message is scrubbed: `%`-escapes decoded, tokens
replaced, the value of any key containing *token* or *session* redacted
when it looks like a secret, render session ids replaced, paths cut to their
file name (spaces inside directory names up to a file name with an extension;
`file://`, URLs with a host, Windows `C:\`, `C:/` and `\\server\` paths), and
control and format characters (newlines, bidirectional overrides,
zero-width characters) turned into spaces; the full list is in the
[backend API](../backend-api/meta-files.md#post-v1vrlog). **Copy diagnostics** on the page puts the versions, the GPU and
its limits, WebXR, the cube, the frame rate, the quality, the settings and the
last events on the clipboard (or shows them, when the clipboard is not
available): paste them into a support request.

**Checked so far** in a desktop Chrome (headless, ANGLE Metal on an M5 Max,
and SwiftShader) against a local backend, with each failure driven by
`validation/vr_lifecycle.mjs`: the backend killed (the notice within 0–5 s,
retries every 1–5 s, the cube opened again about 10 s after the backend is
back), the token changed under the page, the cube's dataset deleted, the
render session deleted (the page's own rendering at once, refined again
about 8 s later), the WebGL context lost and restored with
`WEBGL_lose_context` (the cube drawn again 0.6 s later), a stale page build
and API, the guide's buttons pressed through a ray. Not yet on a Quest 3:
the checks below.

**What to check on the Quest 3** (and the numbers to report):

1. Enter VR for the first time: the controls guide stands in front of you;
   *Next*, *Back* and *Close* work with the right trigger; enter VR again: it
   is not shown; **?** on the panel shows it again.
2. In VR, unplug the USB cable: within 5 s the card says *Backend not
   reachable* and names *adb reverse* and *Open on Headset*, while the cube
   still moves with your hands. Plug it back and press *Open on Headset* on
   the Mac: the card goes by itself (how many seconds?).
3. Quit and restart the desktop application while in VR: *The backend
   restarted*, the cube stays, a spectrum works afterwards; refinement comes
   back (*Refine* row).
4. Press the Quest's system button and quit the VR session: the 2-D page says
   *The VR session ended* with *Enter VR*.
5. Load a large cube (*Max side* 512, 16-bit) and stay still, then move:
   does the panel say *Quality lowered to hold … Hz*? Read the numbers (Y):
   FPS, samples/voxel, eye buffer size before and after.
6. **Copy diagnostics** on the 2-D page: does the clipboard work over
   `localhost` (paste the text in the report)?

## What to check on the Quest 3: analysis and hands (#191)

1. Load HCG 44 and enter VR. Lasso tool, seed mode *new*: pull the trigger on
   NGC 3187. The panel says 13 216 voxels and 6.06 Jy km/s.
2. Seed mode *add* (analysis panel, under the control panel), trigger on
   NGC 3190 (the source left of NGC 3187 at the same velocity): about 23 900
   voxels, a green cross at the added seed. *carve*, trigger inside NGC 3187:
   the count drops, a red cross. Report the counts and the lasso ms on the
   left controller's numbers.
3. *Isolate*, then *M0*: a map appears above the cube's right side. Read its
   title, colour bar and unit (JY/BEAM km/s): are the labels readable from
   where you stand? *M1*, *M2*. Then *Show that cube*: the isolated cube
   replaces the cube.
4. *Draw*, then trigger at three places across NGC 3187: the orange path on
   the cube and the PV diagram above the cube's left side, velocity up
   (888 at the bottom, 2183 at the top for HCG 44). *w +* twice, *Undo*.
5. *Download recipes* afterwards (on the page, out of VR): one record per
   seed press, Isolate, moment and PV path.
6. Hands: put the controllers down. Pinch with the right hand while pointing
   at the analysis panel's buttons: does one pinch press once, and only
   once? Drag the threshold slider on the control panel with a held pinch.
   Point at the cube and pinch: does the cross stay where you pointed? Close
   a hand on the cube and move it; close both hands and pull them apart.
   Pick a controller up again mid-way: the ray and grip go on without
   reloading. Report any missed or double press, and the FPS while a hand
   grabs the cube.
7. With the two product planes shown, the FPS and GPU ms on the left
   controller, against the same view without them (*Hide*).

## Known limits

- **Tokens in the access log.** The VR page sends its token in request
  headers and, on the render WebSocket, as a subprotocol: it is in no URL.
  The desktop's remote 3-D view still puts it in its WebSocket address
  (`/v1/render/<id>/ws?token=…`), and uvicorn's access log records that
  address, so the backend's log can hold the token: keep that log private.

- **Checks on the headset are pending.** Everything above was measured in a
  desktop browser on the Mac (Chrome, the same code path with the mouse); the
  frame rates, latencies, panel readability and controller handling on a
  Quest 3 come from the checks listed in this page and are not yet recorded
  here.
- **One cube at a time**: at most 2048 reduced voxels on a side (the GPU's 3-D
  texture limit), 64 Mi voxels and 64 MiB of texture (so 32 Mi voxels in
  16 bits), for what a headset browser can allocate; the backend refuses a
  larger reduced cube before reading anything, and the page checks its GPU's
  limit before asking. Choose a larger binning.
- **Tools in the headset**: a spectrum, the Kinematic Lasso with add and
  carve seeds, Isolate and Remove, the moment maps M0-M2 of the selection, a
  PV slice along a path, and a box region. No hand refinement of the mask on
  a channel (the desktop's polygon edit), no kinematic fit; the moment is
  always over the whole channel range with no threshold mask, the PV path
  is not edited point by point (*Undo* removes the last point), and the PV
  path is drawn with points, not a curve. The product planes stay where they
  are put (above the cube's sides); they cannot be moved. The desktop has no
  recipe import: a VR recipe is replayed by posting its requests, as a desktop
  recipe is.
- **Shared selections**: the desktop follows the headset's selections once
  *Open in VR Headset…* was used in that cube window (or a selection was sent
  from it), in the desktop's session only; one selection per cube is shared,
  the latest, of at most 16 Mi voxels in its bounding box (a larger box made
  in the headset is measured there but not shared, and the page says so). The
  desktop sends a selection only when asked (*Show in VR Headset*); its
  sliders do not regrow a selection received from the headset.
- **Refinement**: needs a cube the backend holds in core: a cube drawn out of
  core from its brick pyramid is not refined (the panel says so). Refined
  frames travel as JPEG over the render session's WebSocket: there is **no
  WebRTC (H.264) stereo stream** yet, so a slow link lowers their rate. They
  are **off in passthrough** and during the benchmark. The opacity of the
  refined frame is matched to the headset's by path length, which is
  **approximate**: with coarse binning the refined image can look a little
  denser or fainter than the reduced one. Not cropped in isosurface mode (the
  page does not use it).
- **Pairing**: *Open in VR Headset…* needs adb and a headset in developer mode,
  and opens the page through `adb reverse` only (USB or adb over Wi-Fi), not by
  the Mac's network address; a backend at a network address must be reached
  through a local tunnel. A backend under a path prefix works only behind a
  proxy that strips the prefix. The link carries the token (see the caution above).
  The settings kept in the browser are per browser: the Mac's and the
  headset's are separate.
- **The panel in the headset** has the main controls only: the spatial clipping
  box, stretch, velocity stretch, binning, texture, Kinematic Lasso parameters
  and eye-buffer settings are on the page (before entering VR). The panel is
  used with the right controller or the right hand's pinch. **Hand tracking
  has not been tried on a headset**: the pinch and grip thresholds (pinch
  closes under 1.2 cm between the fingertips' surfaces, opens over 2.8 cm;
  grip under 0.72 of the palm's length, opens over 0.95) were set and tested
  on synthetic hands; see the checks below.
- The labels follow the central row and column of the field; far from them on
  a wide projected field the true coordinate of a pixel differs a little.
- The page reads the whole cube once to reduce it (0.6 s for HCG 44, 1.1 s
  with a cold cache); a cube of hundreds of GB takes correspondingly long.
- Comfort: prefer short, seated sessions; passthrough mode helps.
