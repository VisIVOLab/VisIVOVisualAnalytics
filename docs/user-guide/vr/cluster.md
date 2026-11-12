# Headset with a cluster backend

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

## Steps

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
   page: from here on as with a local backend (see
   [Open a cube from the desktop](quickstart.md#open-a-cube-from-the-desktop)).

## The dialog's backend check

Before pairing, the dialog asks the backend
`GET /v1/vr/backend_check` (see the [API](../../backend-api/meta-files.md#get-v1vrbackend_check)),
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

## A backend under a path prefix

Some HPC portals publish a node's port
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

## Refined frames over the tunnel

The page measures, frame by frame, the
round trip of its poses (pose sent to frame received, minus the time the
backend spent on the frame). Over a far link (round trip above 30 ms) the
numbers on the wrist (see [Controls](controls.md#the-numbers-on-the-wrist))
say `link far rtt <ms> ms  window <n>`: the page keeps enough
poses in flight (and asks the backend for a matching ack window) to keep the
cluster's GPU busy during the round trip, ceil((rtt + work) / work) with
work = render + depth + JPEG, between 2 and 6. The eye size is **not**
reduced over a far link: a smaller frame arrives sooner, but scaled up to the
eye it is worse than the headset's own picture of the reduced cube (HCG 44,
measured on the Mac with an emulated 80 ms link, against the backend's
full-size reference: page alone 35.9 dB PSNR / 0.968 SSIM on the emission;
refined with the eye at 768 px 35.4 dB / 0.947; refined at full size
43.9 dB / 0.983). Lower *Refined size* on the page by hand if you prefer
rate to sharpness. How refinement works is described under
[Refinement](reference.md#refinement-full-quality-frames-from-the-backend).

## Frames without depth

A refined frame normally carries a depth image, so
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
refined MIP frames are always warped this way (the menu says *no first-hit
depth in MIP*). The numbers on the wrist say `reprojection: pose only (no
depth)` and the reason on the next line. An operator can force this with
`VISIVO_XR_DEPTH=0` in the backend's environment (the dialog then says so).

## Measured, and what to measure

The figures measured on PLEIADI (a V100 node) and on the Mac behind a
prefix-stripping proxy are under
[Cluster backend: PLEIADI and a path prefix](reference.md#cluster-backend-pleiadi-and-a-path-prefix);
what to measure on PLEIADI with the headset is under
[On a cluster backend](reference.md#on-a-cluster-backend).
