# VR headset: methods and implementation

This page holds the implementation details of the VR headset page and the desktop's headset tools. How to use them is in the user reference, [Cubes in a VR headset](../../user/reference/vr.md); the page's settings, fidelity, refinement, routes and measurements are in the [VR technical reference](../vr/reference.md).

## Opening the page

### One page for the whole session

([#237](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/237)):

- **Further cubes go into it.** *Open in VR Headset…* from another cube window
  sends that cube to the page already open (paired by code or over the cable),
  with no new code and no new browser window. The page loads it and shows it,
  in VR if you are in VR: a short card says *Opening ngc3137.fits from the
  computer…*, then *Showing ngc3137.fits from the computer*. A cube larger
  than the room the page keeps for cubes in VR waits for the end of the
  session, as a cube loaded on the page does (*The new cube is shown when you
  leave VR*).
- **Enter VR once.** The page stays in VR across the cubes the desktop sends,
  so *Enter VR* is pressed once per headset session, not once per cube. When
  the page was **launched from the installed app's icon** (step 4), it tries
  to enter VR by itself as soon as its first cube is on show; if the browser
  wants a press for it, the page says *press Enter VR* next to the WebXR
  status and the button works as usual. The page's own **Enter VR** is the
  way in: it works on the first press. The page does not ask the browser to
  offer its own *Enter VR* (`navigator.xr.offerSession`, in the Quest
  Browser's address bar) unless the address has `?offer`
  ([#291](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/291)):
  an offer cannot be taken back, and a Quest Browser holding one was seen to
  refuse the page's button. With `?offer`, the page's button still enters while the
  offer is pending; a session the browser's button starts while the page is
  already in VR is ended at once, so there is never a second session. Which
  of these the Quest Browser allows inside the installed loader is the
  browser's call: *Copy diagnostics* says what happened (the `entering VR:`
  line).
- **When Enter VR fails, the page says what to do.** The status line and the
  WebXR status name the reason and the way out: another VR session open in
  the browser (leave it first), VR not allowed for the site (allow it in the
  site's settings), the browser's own offer holding the session (press the
  browser's *Enter VR*, or reload the page without `?offer`), a session the
  browser started that has not closed (reload the page if it stays), or a
  page that is paused (*Use this window*). An entry by itself after a launch
  from the app's icon says the same, except a refusal for want of a press,
  which only adds *press Enter VR*.
- **What you do in the headset reaches the desktop.** The colour map and
  threshold you choose, the spatial crop and velocity range, and every product (a spectrum, a
  moment map, a PV slice, an isolated or removed cube, a catalogue or a
  velocity field opened on the page) are reported to the desktop, about half
  a second after the last change of a slider. Each product is first saved in
  the Workspace Exports (`<cube>_vr_m0.fits`, `<cube>_vr_pv.fits`,
  `<cube>_spectrum_x<x>_y<y>.csv`; an isolated cube is already there), with
  the requests that made it, so the desktop can open it, save it and record
  it.
- **A second window stands down by itself.** A page that shows nothing (a tab
  the browser restored, a loader opened again) never takes over from the page
  with a cube: it pauses and says which cube is open in the other window
  (*ngc3137.fits is open in another window*). *Use this window* still makes
  it the page in use, if that is what you want.

### The pairing dialog and link

The dialog follows the pairing by itself:

- *Waiting for the headset. The code expires in 4:52.*
- *The code was entered: the headset is connecting…*
- **Headset connected**, with a **Disconnect** button.
- *The code expired*: **New Code** asks for another one. *The code was used,
  but no headset connected* (nothing arrived within 45 seconds of the code
  being entered) also offers **New Code**.
- A headset paired earlier that is still connected is named under a new code,
  with **Disconnect**.
- If the backend stops answering for more than 15 seconds, the dialog says so
  and keeps trying.

The headset joins the desktop's backend session, on the same dataset and path
(and, for a point table, a catalogue or a velocity field, the same kind and
box). Closing the dialog revokes a code nobody used; a connected headset stays
connected until **Disconnect**, or until the application closes.

The backend's token never leaves the computer: it is not in the code, the QR
code or the page's address, and the page has no token field. The code is shown
on purpose but is not written to the application's log.

- The headset and the computer talk to each other directly, over the local
  network, once paired: keep both on the same network for the best speed. On a
  network that keeps devices apart, the link goes through a relay server, if
  your institute runs one, more slowly (see
  [Troubleshooting](../vr/troubleshooting.md#pairing-by-code)).
- If the link drops (Wi-Fi, the computer asleep), the page says *Backend not
  reachable* and reconnects by itself for up to three minutes, without a new
  code and without reloading; the cube stays on show. If it cannot, it says
  *The pairing with the computer ended* with **Enter a new code**: ask the
  dialog for a new code (**New Code**) and type it there.
- *Copy diagnostics* on the page names the link (the `transport:` line):
  see [The transport line](../deployment/vr-public-loader.md#the-transport-line).

How the page works behind the code is in
[How the public loader works](../deployment/vr-public-loader.md).

### The pairing settings

Pairing works with the local backend and with a remote one alike: the backend,
not the desktop, talks to the pairing service, and it needs to know two
addresses. For the backend the desktop starts, they are set in **Settings ▸
Backends ▸ Headset Pairing**:

| Field | Passed to the backend as | What it is |
|---|---|---|
| *Pairing service* | `VISIVO_VR_PAIR_URL` | the pairing service, `wss://…` |
| *Headset page* | `VISIVO_VR_PAGE_URL` | the *VisIVO VR* page the headset opens, `https://…/` |

Both fields are empty after installation, and an empty field uses the
default shown in it in grey: the server hosted by INAF,
`wss://vr.visivolab.oact.inaf.it/pair` and `https://vr.visivolab.oact.inaf.it/`
(a build made for a site may show that site's server instead). Type an address
only to use another server, such as your institute's. The default page goes
with the default service only: with another service typed, also type its page,
or the dialog shows the code without a QR code.

- **Pair headsets by code**, on by default: switched off, the desktop passes
  nothing to the backend (only the variables below, if you set them in the
  environment, still reach it), and the dialog opens its USB section.
- **Restore Pairing Defaults** empties both fields and switches pairing on again (it
  applies when you press OK).

The values are read when the backend starts, so restart the application after
changing them. The same variables set in the environment the application
starts from take precedence over the fields and the defaults. A remote backend
(a cluster's) uses its own configuration. Running the server is described in
[Hosting headset pairing](../deployment/vr-pairing.md).

### The USB way in detail

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
   [below](#over-wi-fi)) and put it on.
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
   the headset (`adb reverse tcp:<port> tcp:<port>`). When it has the
   VisIVO VR app, it installs or updates the app when needed and starts it
   in VR with the cube, with a one-time launch ticket instead of the token
   (see [The headset app](../../user/reference/vr.md)). Without the app, or with
   **Open in Browser**, it opens the page in the Quest Browser (`am start`
   with a view intent; in the headset's default browser if the Quest Browser
   is not installed): the cube loads on the page; press **Enter VR**. The
   rest of this section describes that browser page.

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
   line and the hand menu say *in the desktop's session*.

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
[Headset with a cluster backend](../deployment/vr-cluster.md).

Point tables, 3-D catalogues and velocity fields open from their own windows
the same way: see [Opening one](../../user/reference/vr.md).

### Over USB by hand

WebXR needs a **secure context**: an `https` page, or `http://localhost`.
Forwarding the backend's port to the headset over USB makes the page
`localhost` for the headset, so no certificate is needed. Without the desktop
application (or to do by hand what *Open on Headset* does):

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

### Over Wi-Fi

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
API to the network, protected only by the token). Prefer
[pairing by code](../../user/reference/vr.md), or adb over Wi-Fi.

Over Wi-Fi the refined frames can travel as video: see
[Over Wi-Fi: refined frames as video](../vr/reference.md#over-wi-fi-refined-frames-as-video-webrtc).

### On the Mac, without a headset

1. Start the backend as usual (the desktop application starts one on
   `http://127.0.0.1:8000`; or by hand, see [Getting started](../../user/reference/getting-started.md)).
2. Open `http://127.0.0.1:8000/vr/` in Chrome, Edge or Safari and paste the
   backend token (the contents of `~/.visivo_token`) into the *Backend token*
   field. Opening `http://127.0.0.1:8000/vr/#token=<token>` works too: the
   page reads the token from the part after `#`, which a browser never sends
   to the server (so it is in no log), and removes it from the address bar.
3. Type the path of a cube on the backend (or *Browse…*), choose the binning,
   and press *Open and load*. Drag to turn the cube, scroll to zoom.

*Synthetic* draws a generated cube of the chosen size without any backend, for
checking the page and measuring frame rates. The hand menu can be shown in the
page's 3-D view and used with the mouse (see [The hand menu](../../user/reference/vr.md)).

## The VisIVO VR app

### From the desktop

Put the headset on and press **Open on Headset**. The desktop installs or
updates the app when needed (*Installing VisIVO VR on the headset…*, a few
seconds), checks that the backend lists the app, forwards the backend's port
(`adb reverse`), checks that the headset is awake, and starts the app once:
*Starting on the headset…*, then *In VR*. The cube is in the room within a
few seconds. There is no browser window and no Enter VR to press.

What the desktop may say instead:

| Message | What to do |
|---|---|
| *Put the headset on, then press Open again.* | The headset was asleep (or took the headset off before the app started): wear it and press **Open on Headset** again. Nothing was started. |
| *VisIVO VR started but could not connect to this computer.* | The app opened but never reached the backend: check the cable and the backend, then press **Open on Headset** again. |
| *This backend does not list the VisIVO VR app…* | The backend does not serve the app's statement at `/.well-known/assetlinks.json`: a development build needs the backend on the same computer (restart it after the first build); the release app needs a backend as recent as the desktop that bundles it (an older one does not list it yet; on a cluster, update the backend too, or start it with `VISIVO_VR_APP_RELEASE_SHA256` set to the certificate in `tools/quest-app/README.md`). Nothing was started. |
| *Another VisIVO VR is installed* (a question) | The headset has a build of the app signed by another key (someone's own development build). **Yes** uninstalls it and installs this one; **No** leaves it alone. The desktop never uninstalls without asking. |

### From the scanner

Opened from its icon, the app shows its QR scanner, a 2-D panel with the
headset's passthrough camera
([#383](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/383)).
Looking at the QR code of *Open in VR Headset…* (or typing its code) opens
the public loader inside the app with the code; the loader pairs over WebRTC
as in the Quest Browser, and its page enters VR at load with no press. The
design, the permissions and the host's statement are in
[The VisIVO VR app's flow](../deployment/vr-public-loader.md#the-visivo-vr-apps-flow);
what the scanner says is in
[Troubleshooting](../vr/troubleshooting.md).

While the app runs, **Open on Headset** and **Show on Headset** send the
cube to it: the app is never started a second time, so you stay in VR and
the new cube replaces the old one. **Open in Browser** opens the page in the
Quest Browser instead (then press Enter VR).

The release packages of the desktop (macOS, Linux, Windows) carry the app,
signed with the project's key, and every backend lists it: there is nothing
to download or configure. A desktop with no APK (built from source without
`tools/quest-app/build.sh`, or a package built without the signing key)
opens the page in the Quest Browser instead. The APK is also on
each release's page (`visivo-vr.apk`), to install by hand.

The app is started once per Open, and never while a page of it (or any page
of this session) is connected: **Open on Headset** then sends the cube with
the hub's `show`, like **Show on Headset**. The desktop looks at the hub both
before it does anything and again right before the launch. An asleep headset
is told apart before the launch (*Put the headset on, then press Open
again.*); a launch the headset never takes up within 8 s says the same, and
an app that starts but does not connect within 10 s more says *VisIVO VR
started but could not connect to this computer*.

The launch carries a one-time ticket, never the token; neither the ticket
nor the token appears in the desktop's log, its messages or **Copy Error**.
The routes are in the backend API's
[headset app launch](../backend-api/vr-launch.md).

### Where the desktop finds the app

In this order:

1. `VISIVO_QUEST_APK`: the path to an `.apk`, with its `.json` beside it
   (`visivo-vr.apk` and `visivo-vr.json`, as `tools/quest-app/build.sh`
   writes them);
2. the desktop's own resources: `quest-app/visivo-vr.apk` (in
   `Contents/Resources` of the macOS app, beside the executable elsewhere:
   `app/usr/bin/quest-app/` in the Linux package, `bin\quest-app\` in the
   Windows one), where the release packages put it;
3. a source tree the desktop was built in: `tools/quest-app/out/`
   (`visivo-vr-dev.apk` first, then `visivo-vr.apk`).

For development: build the app with `tools/quest-app/build.sh`, start the
desktop from its build directory (`build/` in the source tree), and it finds
`tools/quest-app/out/visivo-vr-dev.apk` by itself. The `.json` beside the APK
says which package and version it is; without it the APK is not used.

### Build and install it by hand

On the computer that runs the backend, with a JDK 17 and an Android SDK (see
`tools/quest-app/README.md`):

```bash
tools/quest-app/build.sh                       # makes your development key once
adb install -r tools/quest-app/out/visivo-vr-dev.apk
adb reverse tcp:8000 tcp:8000                  # your backend's port
```

The backend on this computer serves the development app's statement at
`/.well-known/assetlinks.json` by itself (restart it if it was running before
the first build). The headset's browser checks it at every launch and closes
the app when the app is not listed.

To start it as the desktop will, ask the backend for a launch ticket and pass
it in the address (the ticket works once, within 60 s; the token never goes
in the address):

```bash
curl -s -X POST http://127.0.0.1:8000/v1/vr/launch \
  -H "X-Visivo-Token: $TOKEN" -H "X-Visivo-Session: $SESSION" \
  -H "Content-Type: application/json" -d '{"cube": null}'
adb shell "am start -a android.intent.action.VIEW \
  -n it.inaf.oact.visivo.vr.dev/.LauncherActivity \
  -d 'http://localhost:8000/vr/?launch=immersive#ticket=<ticket>'"
```

Put the headset on first: an asleep headset ignores the launch.

### What to check on the Quest 3

1. With the app not installed, open **Open in VR Headset… → Advanced (USB
   cable)**: the section says it is not installed and offers **Install**.
   Press **Open on Headset** with the headset on: *Installing…*, *Starting on
   the headset…*, *In VR*, and you are in VR with the cube, no Enter VR.
2. Press **Open on Headset** again while in VR: the cube is sent to the
   running app (*Sent: the headset shows this cube now.*), and the app is not
   started again.
3. With the headset off your head (asleep), press **Open on Headset**: *Put
   the headset on, then press Open again.*, and nothing starts on the headset.
4. Start the app from the desktop (or by hand, above) with a small cube
   (WALLABY or HCG 44). You should be in VR within about 4 s, with
   *Opening …* then the cube. The backend log has the times
   (`app-launch: ticket exchanged in … ms`, then `xr-start`).
5. Open the hand menu, check the cube's controls work as in the browser.
6. Start the app from its icon in the App Library: its QR scanner opens (a
   2-D panel with the camera view). Scanning the dialog's QR code pairs and
   enters VR (see [From the scanner](#from-the-scanner)).
7. With a cube on show, send another with **Show on Headset**: it replaces the
   first in VR.
8. Quit from the system menu (or **Quit VisIVO VR** on a card when one is
   shown): the app is gone, nothing is left in the room.
9. Started by hand with `&debug` after `?launch=immersive`, the frame-rate
   numbers are shown in the headset: report the frame rate and the missed
   frames.

## Headset Activity in detail

A catalogue window whose backend session expired opens its catalogue again in
a new session when it needs it (for example on **Load more**). An open dialog
follows it: a code shown and not yet used stops working and a new one, for
the new session, takes its place; a show or a launch still under way is
dropped (press the button again); and the dialog asks the new session for a
headset page already open. A headset that was paired with the old session
stays on it: pair it again with the new code.

The dialog grows with its text but stays on the screen: it moves up when its
bottom would go past the screen's, and on a short screen its sections scroll
while **Close** stays at the bottom.

A backend older than this feature has no headset hub, and the panel says so.
When the answer is ambiguous (a 404 page from a proxy or tunnel rather than
the backend's own, as while a remote backend restarts behind its tunnel), the
windows ask again a minute later, and at once when another cube window of the
session opens: once the hub answers, **Show on Headset**, the activity list
and following the headset's look come back by themselves.

Each product has an **Open** button, which opens it in the desktop's own
viewer:

| Made in the headset | Opens in |
|---|---|
| Moment map | the image viewer (its FITS file) |
| PV slice | this cube's PV view, along the headset's path; from another cube, its FITS file |
| Spectrum | a spectrum product of its cube's window (pane and Session Data), from its CSV or FITS table |
| Isolate or Remove | a new cube window (the masked cube's FITS file) |
| Catalogue or flow layer | its file, when it is a FITS file; otherwise it is named, to open from the Data Hub's Workspace Exports |
| Refined mask (#282) | a new cube window (its FITS file, same grid and IDs) |
| Classified catalogue (#279) | this cube's Catalogue table: the classified and found sources, at the backend's own pixels, with the flag, note, velocity, origin and catalogue row as columns; **Save VOTable…** above the table saves the full catalogue with the flag column |

A product opened here is recorded in Session Data ▸ Provenance with the
headset's own record: its parameters, the requests that made it (under the
API's names) and its replay caveat, if any. It is recorded as a product of the
cube it was made from, so by that cube's window: opened from another cube's
window it is shown but not recorded there. Opening it again does not add a
second row. A PV slice opened in this cube's PV view is recorded once, in the
view's PV diagram row, which carries the headset's record (*Made in: the VR
headset*, its file, its requests). When the view moves on to another cut (a
path drawn here, the headset's path edited, another PV from the headset), the
headset's PV gets a row of its own.

A moment map the headset measured on a selection says so under *Mask*
(*the Kinematic Lasso selection (outside blanked); no threshold*): the page
records it as *Mask: none*, the desktop's word for "no threshold mask".

### Follow the headset's look

- the colour map when the desktop has one of the same name (*grey* is *Gray*;
  the headset's *hot* has no counterpart, and the status bar says so);
- the threshold, in the cube's units, as if typed in the threshold field;
- the clip box crops the 3-D volume, also when the backend renders the view
  (the slice views are not clipped). A velocity range set in the headset
  (the menu's Range tab) is a clip box along the spectral axis, so it follows
  too.

Each change taken from the headset is said briefly in the status bar, for
example *Colour map and threshold from the headset*.

Only what the headset shows now is applied: a look it had before switching
to another cube is not, and a change is applied once. Nothing the desktop
does is sent back to the headset as a look, so a change made here never
returns to undo itself.

Untick the box to keep the window's own look: the clip box is removed, the
colours and threshold stay, now the window's own. The choice is remembered
for the cube windows opened afterwards; tick it again to follow the headset
from what it shows now.

### Watch the Headset

A window opens with the headset's view: the left eye, cropped to the middle
of the view, with the cube, the controllers and their rays, the menu, the
wrist cards and the result windows. The window's line gives the frame rate
seen here, the frame's size, how old the frame was when it came, and what
capturing it costs the headset per frame.

- It is off until you press Watch, and it stops when the window closes.
  While it is watched, the headset shows a small **Desktop watching** badge
  at the upper left of the view. A desktop browser page shows a line at the
  top instead.
- **Snapshot** saves the current view as a PNG in the Workspace Exports
  (`headset_<headset>_<date-time>.png`). The status bar names the file.
- The view comes at about 15 frames a second, 512 pixels wide. A slow
  network skips frames; it never falls behind.
- Closed without a word (the computer slept, the network dropped), the
  watch ends by itself after 10 seconds and the badge goes. The window
  starts it again when it can.
- The menu, the wrist cards and the windows are drawn by the headset's
  compositor on top of the 3-D view. The page draws them into the copy at
  the same place, so the copy shows them as the headset does. Passthrough
  (the room) is not in the copy: it shows black there.

### Notes

- The headsets and the activity belong to the backend session, so every cube
  window of the session shows the same list.
- **Show on Headset** sends the dataset, its path and its kind (cube, point
  table, velocity field). A velocity field's box in Mpc is not part of the
  command: to see a field with its box, pair it from its window.
- The token is never in what the desktop sends or reads here: it travels in
  the request headers only.
- The routes are `GET /v1/vr/headset`, `POST /v1/vr/headset/command` and
  `GET /v1/vr/activity` (see the backend's API reference).

## Controls in detail

### Controller models

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

The models are shaded from their glTF materials: base colour (an sRGB
texture), metallic and roughness, emissive, and the alpha mode, lit by a soft
light from above and one at your eye. They look as the asset paints them:
the `meta-quest-touch-plus` model has a light grey grip with a dark
face plate and buttons; the `meta-quest-touch-plus-v2` model is
dark all over, with green accents. Their textures are kept at 1024 px a side
(about 5.3 MB of GPU memory each, one per hand), mipmapped and filtered
anisotropically so they do not shimmer.

### Undo, grabbing and labels

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

### Stepping between tools

the box). The secondary stick pushed ←/→ and held for a moment (about a fifth
of a second) steps to the next tool of the palette, in its order (Explore,
Kinematic Lasso, Spectrum probe, Box region, Velocity range, Spatial crop,
Moment, PV slice, then Explore again; those that do not apply to the data are
skipped). Stepping on Moment computes the intensity map, as picking it does,
and the next step goes on to PV slice; any step ends a PV slice being drawn.
The palette and the Select tab choose one directly. A push that is
not mostly sideways, one made while that hand holds the grip (moving the
cube), or one made while a tool is still working changes nothing until the
stick is back at rest, so a thumb resting on the stick does not change the
tool. What they compute is described in [Analysis](../../user/reference/vr.md).

### The clipping plane of a simulation volume

The first row of the Range tab: **On/Off**, **Flip** (keep the other side),
**Snap** (turn the plane to the nearest box axis), **Slab** (keep only a slab
between two parallel planes, a tenth of the box thick to begin with),
**Reset** (back through the centre, across x). The status line gives the
plane in the cube's pixels: its point, its normal and what it keeps. With
the tool in use the plane's outline is drawn in yellow, with an arrow
towards the side it keeps and a dot at its point. On the 2-D page, with the
tool on, a drag on the plane moves it along its normal and Shift and a drag
tilts it. The full-quality frames from the backend are cut by the same plane
and show the same section (a backend from before #369 cannot, and its frames
are then not shown). The plane stays when you switch to another variable or
snapshot of the same grid.

### The crop

While the Spatial crop tool is on, the crop box is drawn: its twelve edges
(light blue), a faint fill, and a **dot on each of its six faces**. The dot
under the ray, or within 6 cm of the controller, turns yellow and grows
before you pull the trigger, so you can see which face will move. Then:

- **Trigger on a dot and drag**: that face follows the ray along its own axis
  (never past the opposite face, never out of the box). With the ray almost
  along that axis (within about 13°) the face stays where it is rather than
  racing away. A dot taken with the controller's tip (no ray on it) follows
  the tip instead.
- **Trigger on the inside of the crop and drag**: the whole crop moves, its
  size kept, inside the box. The edges and the fill turn yellow while the
  inside is pointed at.
- **Trigger anywhere else and drag diagonally**, then let go: a new crop with
  those two points as opposite corners, as Box region does in Select. It spans
  the two axes across the ray; along the axis the ray looks down, the crop
  keeps its depth (on a cube seen along the velocity axis: the velocity range).
  A trigger pulled without a drag changes nothing. On a box not cropped yet
  (or cropped only along the ray) a trigger on the box draws a new crop.

A toast says this in one line when the tool is picked, and the right wrist's
legend says *Trigger: drag: dot, inside, or new*. It works the same on a cube,
a point table and a velocity field (their points and lines outside the crop
are culled in the shaders). On a cube the faces across the velocity axis are
the velocity range's ends. In a desktop browser the mouse drags a dot or the
inside; with Shift held a drag elsewhere draws a new crop (any other drag
turns the view). A drag that changes the crop is one undo step; a pull that
changes nothing leaves none. Changing the tool or the mode in the middle of a
drag ends it where it is; B (undo) in the middle of a drag undoes that drag. A flat point table (all its points in one plane)
has no dots on its flat axis, which is never cropped.

**What the crop changes.** What is drawn: the cube (the headset's own
rendering and the refined frames from the computer, whose render session gets
the same box), a point table's points and a velocity field's lines, the
catalogue over a cube. What the tools take: every pick (readout, Spectrum
probe, a lasso's seed, Box region's corners, PV points) lands only on what is
drawn inside the crop; the moment of the whole field shows, and counts, only
the cropped pixels (*Moment 0 of the cropped field*, its x and y range under
the title; the product saved for the desktop is the whole-field map of the
same request). It is cut with the crop as it was when Moment was pressed, and
cut again when the crop changes afterwards and stays still for a quarter of a
second, with no new request (only while its window is shown). The colour
range of a re-cut is exact for Moments 0 and 2 (the lowest and highest
finite pixel of the crop); the robust range of a velocity field (Moment 1)
comes from at most 65 536 of the crop's finite pixels, or all of them when
it has fewer, so a NaN-masked map with a compact source keeps its range. Box
region and the PV slice take the velocity range. A lasso
grows from its seed over the whole cube as on the desktop (its object is not
cut at the crop), and a spectrum is the full band at its pixel. The crop of a
cube is sent to the desktop as clip activity (voxels of the full cube) half a
second after the last change; a point table's or a velocity field's is not
(they have no voxels).

### Palette and menu geometry

The palette is 0.62 × 0.23 m (1024 × 380 px at 1650 px/m), each icon
6.8 cm across, about 3.5° at the 1.1 m it is seen from; its 26-30 px text is
0.8-0.9° high. It is a 2-D canvas redrawn only when what it shows changes.
In the desktop view (`?panel3d`) it stands 1 m in front of the camera while
open, at half that size.

**Press X** (the secondary controller's lower button) and the menu opens just
above and beyond that controller: about 0.95 m from your eyes, along the line
from your eyes through the controller (at most 50° to the side of where you
look), its lower edge 5 cm above the controller (its centre never more than
8 cm above your eyes nor 30 cm below them), turned to face you. **It stays
open** where it opened (world-locked): the hand that opened it is free, and
turning your head does not move it. **Press X again, or the × at the right of
its title bar, to close it** ([#276](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/276)).
To put it somewhere else, point at its **title bar** (the top strip, *⠿ drag
to move*), hold the trigger and move the ray: the menu follows at the same
distance, turned to face you. With tracked hands, the palm turned up does the
same as X: up once opens it, down and up again closes it. The palm must stay
up for 0.15 s to count, and a dropout of the tracking shorter than 0.25 s does
not end it, so holding the palm up neither flickers the menu nor closes it.

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
below (see the [checks](../vr/reference.md#the-menu-in-front-of-you-organised-by-task-257)). It is a 2-D
canvas redrawn only when something on it changes. The secondary wrist's card is
hidden while the menu is open. In a desktop browser the same menu stands in
the page's 3-D view at half that size with *show the headset's menu here* (or
`?panel3d` in the address) and is used with the mouse through the same ray
code: a click presses, a drag moves a slider.

**Every text fits its place** ([#276](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/276)).
A text too long for its box wraps, or shrinks a little (to 85 % of its size at
most), and only then is cut with an ellipsis. A button whose full name does
not fit carries a short label and the full name as its **tooltip**, written
under its row in blue while the ray is on it: the moments are *M0*, *M1*, *M2*
(*Intensity (M0)*, *Velocity (M1)*, *Dispersion (M2)*), the particles' style
*Splats* (*Smooth (splats)*). A greyed control's reason takes that place
when the ray is on it. A point table's column names are cut when long; the
tooltip has them whole. The same holds for the palette, the wrist cards, the
toasts, the windows and the notices: a notice's hint line (*Controls guide:
…*) has a row of its own above the buttons.

### Isosurfaces

    The backend extracts each surface from the original data, not from the
    headset's reduced copy: the full cube when it is small, else the finest
    level of its brick pyramid that fits (512³ of a 2048³ run), decimated to
    150 000 triangles. Under the rows each surface says how many triangles it
    has, from how many, and from which level; while one is made it says
    *extracting n %*, and it appears whole when ready (a level moved back to
    one already made comes back at once). The surfaces are cut by the crop
    and, on a simulation volume, by the clipping plane, as the volume is.
    While the model is exploded (*Explode ▸*) they are not drawn.

### Levels: σ, percentiles and stretches

    written below. **Stretch**: *Linear*, *Log*, *Sqrt* (the default, the look
    the page always had), *Square*, *Power*, the desktop's stretches, from the
    threshold to the colour max; they shape both the colour and the opacity
    of each sample. The log is the desktop's, log10(v / threshold) /
    log10(colour max / threshold) (a = 1000 when the threshold is not above
    zero). **Power γ**: *γ −*, *γ +* (×1.25 a press, 0.1 to 10). The info line,
    the velocity convention and where σ comes from are under the rows.
    σ is the full cube's noise measured as the desktop's *Estimate Noise*
    measures it over its default region (the whole field, every channel), so
    *3σ* in the headset is the same value as 3 × the desktop's σ from an
    unchanged dialog; a σ measured on the desktop over a chosen emission-free
    region is not sent to the headset yet. n σ means the value n × σ (about
    zero, as the desktop's 3σ moment mask). The threshold applies to the drawn
    cube, which is binned: with mean binning of N = fx·fy·fz voxels its noise
    is about σ/√N, so a 3σ threshold hides more of the faint emission than the
    desktop's 3σ on the full cube; the menu says both, *= 3.0 σ full-resolution
    (≈17σ drawn)* (an upper estimate where the beam correlates
    neighbouring pixels). With max binning the drawn noise is not σ scaled,
    and the menu says so. The refined frames (the full cube) show what the
    desktop shows. A cube read from its brick pyramid takes σ from level-0
    bricks about one median over all channels: the desktop's number only for
    a flat baseline. The percentile presets come from a sorted sample of a
    million of the drawn cube's values, so they are right on data spanning
    many decades too (the histogram's bins are only its picture).

### Technical settings

**Technical settings** are not on the menu: only with `?debug` in the
address does More show *Technical settings ▸*, a page with *Hold the frame
rate*, *Refine with the full cube*, *Reproject refined frames with depth*,
*Refined frame size* (0.4× to 1×, one step per press), *Record measurements*
with *Run protocol* (*Cancel run* while one runs; see
[Recording measurements](../vr/reference.md#recording-measurements)) and
*Threshold from*: *This binning* or *Full cube*. Without `?debug` the
threshold always starts from the full cube's, the value the desktop starts
from (`default_threshold` of `/v1/cube/reduced_info`: about a hundred noise
voxels of the whole cube above it), so the headset shows the emission the
desktop shows, in the same unit; a choice stored earlier counts only with
`?debug` (a synthetic cube, which has no full cube, uses its own). Refinement
stays on, with the settings kept in the browser.

### Wrist cards

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
  change with the mode (*X* the menu, open and close, *L stick ↔* the next
  tool, *L stick ↕* the colour max). With tracked hands: *Pinch* and *Palm up*.
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

### Toasts, readout and haptics

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
eye-buffer scale. The card
is redrawn at most 15 times a second, only when its text changes; its text
costs about 4 µs. On the page the same
lines follow the mouse, in a line above the numbers at the bottom of the
page's controls.

**Haptics.** Where the controller has a haptic actuator: a short pulse on a
press (a menu item, the trigger on the cube, a step of the stick), a stronger
one when a result arrives (a spectrum, a lasso, a box, an Isolate or Remove, a
moment, a PV diagram), two pulses on an error. More → *Haptics* turns them
off. Tracked hands have none.

### Result windows

The results stand in windows: the **selection** window (the picked position,
the spectrum, the lasso's or the box's statistics; right of the cube; since
#256 it opens only with a result, work under way or an error, not as soon as
Select mode is on), **Moment** (above the cube's right side) and **PV
diagram** (above its left side), and **Sources** (left of the cube, low;
opened by *Select ▸ Sources ▸ List*, its rows, headings and buttons used with
the ray and the trigger like the menu's) and **Mask** (right of the cube, low;
*Select ▸ Mask ▸ Mask*). Each window's title bar sits above its
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

### Hand tracking

Put the controllers down and the page switches to your hands within a frame;
pick them up again and it switches back. The roles stay:

| Gesture | Does |
|---|---|
| **Pinch** (thumb tip to index tip), primary hand | the trigger: presses the menu's items, drags its sliders, moves windows, and acts on the cube along the hand's ray (the headset's own pointing ray; the ray is held still while the fingers close, so the pinch does not move what it points at) |
| **Fist** (middle, ring and little finger curled), either hand | the grip: one hand carries the cube, both move, turn and scale it |
| **Palm up**, secondary hand | the menu: up opens it, down and up again closes it (as X) |
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
a headset (see the [checks](../vr/reference.md#controls-and-the-hand-menu-201)).

### Settings kept in this browser

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

### The numbers on the wrist

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
- the *refine* line (see [Refinement](../vr/reference.md#refinement-full-quality-frames-from-the-backend)),
  the link and reprojection lines on a cluster (see [Cluster backends](../deployment/vr-cluster.md)),
  the *share* line (sent, received, publish and receive ms), and for a point
  table *points X M drawn of Y M sampled, Z M rows*;
- the *hub* line ([#237](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/237)):
  whether the page is linked to the desktop's headset hub and how it reaches
  it (`adb`, `pair` or `lan`), the cubes the desktop sent (*commands*), what
  the page reported (*look*, *clip*, *products*, *cubes*) and the time of the
  last report and of the last product saved for the desktop.

### What the desktop sees

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

## Analysis

### Where a pick lands

**Where a pick lands.** The ray is marched through the reduced cube the
headset draws, with the same values, threshold and spatial crop as the
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
and closed, see [Controls](../../user/reference/vr.md); under the page's controls on the Mac)
shows the seed's position: right ascension and declination in ICRS from the
file's full WCS (`/v1/datasets/pixel_to_sky`, what the desktop sends over
SAMP), the velocity of its channel in km/s with its convention, the pixel and
channel, and the reduced voxel.

### HI measurements of a selection

**HI measurements of a selection.** Under the flux, the results window lists
what an HI catalogue quotes for a source, measured by the backend on the full
cube with SoFiA-2's definitions:

- the integrated flux in Jy km/s (beam-corrected) **±** its uncertainty from
  the noise, corrected for the noise correlation between neighbouring
  channels, measured around the selection; the line above the peak says by
  how much (*channels correlated (ρ1 0.74): error corrected ×1.8* on HCG 44).
  With a typed *Noise σ* the surroundings are not read, and the line says
  *channel correlation not measured*. A selection narrower than a beam still
  underestimates the error;
- the peak flux density, the **peak S/N** (peak / σ) and the **integrated
  S/N** (flux / its uncertainty);
- **W20** and **W50** from the integrated spectrum (from each end inwards to
  20 % or 50 % of its peak, interpolated between channels) and the systemic
  velocity **Vsys**, the midpoint of the W20 crossings, with the velocity
  convention and frame named on the next line (for example *optical (AIPS
  VELO-HEL, no VELREF), barycentric*). A line cut by the selection's edge is
  flagged: its widths are lower limits;
- the flux-weighted centroid (positive voxels) in right ascension and
  declination (ICRS, sexagesimal) and in velocity;
- the size: the semi-major and semi-minor axes of the moment-0 footprint in
  arcsec (the uniform ellipse with the same second moments, as SoFiA-2's
  `ell_maj`/`ell_min`; not deconvolved from the beam) and its position angle
  east of north (*E of N*, from the coordinates at the centroid);
- σ, the noise used: the lasso's *Noise σ* when you typed one (the same σ the
  grow used), otherwise measured around the selection (1.4826 × the median
  absolute value of the unselected voxels within 3 beams and 5 channels);
- notes from the backend, when a number is a limit or missing (a topocentric
  cube, a footprint narrower than a pixel or round, no frame in the header).

The secondary wrist card shows the flux with its uncertainty, Vsys and W50.
The numbers go into the selection's record (its Provenance), so the desktop
receives them with the recipe, and the desktop's note for a selection shared
from the headset adds the uncertainty with its channel-correlation label, the
peak S/N, Vsys with its convention and frame, W20 and W50. Given
SoFiA-2's mask, a selection reproduces SoFiA-2's catalogue (flux, widths,
centroid, size to its precision; the locally measured noise within 7 %, see
[the backend API](../backend-api/cube.md#post-v1cubemask_stats_bin)).

The lasso's mask comes back at full resolution and is drawn in **green** on the
reduced cube: a drawn voxel is green when any full-resolution voxel of its
block is selected, so nothing selected disappears, and selected voxels below
the display threshold (the lasso grows into the faint margin) are drawn too;
its bounding box is outlined. The lasso's parameters are the desktop's
defaults (reach 8, velocity coupling 1, threshold 2.5 σ, the noise measured by
the backend, the Reach slider's level-field budget 64, no smoothing) and can be
changed on the page (*Kinematic Lasso parameters*). The same seed and
parameters give the same mask as the desktop, voxel for voxel (see
[Selections and analysis](../vr/reference.md#selections-and-analysis)).

### Box region

A box corner is placed at the primary controller's tip when the tip is inside
the cube (reach into it): two such corners make a 3-D box. A corner placed by
pointing from outside the cube, or with the mouse, takes the right ascension
and declination of the point the ray picks (the first drawn sample, or the
middle of the ray inside the box when nothing is drawn) and no depth; the box
then spans the channels of the velocity range (the crop along the spectral
axis, set on the Range tab or with the Range mode's trigger, see
[Modes](../../user/reference/vr.md)).
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

### Recipes

**Recipes.** Each spectrum, lasso selection and box is recorded as the desktop
records its products (`visivo-recipe/1`, see
[Kinematic Lasso](../../user/reference/kinematic-lasso.md)): the dataset's path, the calls with their
requests under the API's field names (for the lasso, the grow with exactly the
request the desktop's recipe holds, then the statistics of the mask it
returns), the parameters as the desktop's Provenance tab labels them, and where
it was measured. *Download recipes (.json)* saves all of them, *Copy last
recipe* copies the latest. A replay opens the path and posts the same
requests: the selection is the one the desktop shows for that seed.

### Add, Subtract, Isolate, Remove

The lasso's seed mode (*Lasso: New*, *Add*, *Subtract*: the menu's Select tab,
or the page's *Lasso seed*) decides what the next lasso trigger does,
as the desktop's plain, Shift and Alt double-clicks do: *New* starts a new
selection, *Add* grows a second source into the selection (union), *Subtract*
removes the region the grow from that seed reaches; the primary seed stays.
Each press is one grow on the full cube
(`/v1/cube/kinematic_select_bin` with `add_seeds` and `sub_seeds`, the
desktop's request), so the selection is always the one its seeds and
parameters make; the extra seeds are drawn as green (added) and red (carved)
crosses, and the results window counts them.

*Isolate* (a new cube with everything outside the selection blanked) and
*Remove* (the selection blanked), on the menu's Select tab → *Result* (shown
only while something is selected) or on the page, post the mask shown, exactly (not a re-grow), to
`/v1/cube/mask_apply_bin` with `blank_outside` or `blank_inside`, as the
desktop's buttons do: the new cube goes to the Workspace Exports with the
selection's recipe in its FITS HISTORY, and is registered in the session.
*Show result cube* loads it in the headset in place of the cube shown, in VR
too: it is written into the room the page keeps for cubes before you enter
VR. Only a cube larger than that room waits until you leave VR (a notice
says *The new cube is shown when you leave VR* and why), because the
headset's browser cannot take a new cube's textures during a VR session (see
[GPU context loss](../vr/troubleshooting.md#gpu-context-loss-at-the-first-lasso-on-a-quest-3)).
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
not recorded), and the menu's Select tab says so, even after the new cube's
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

The Select tab also shows the seeds and the last cube made; the Measure tab
the moment's range, the path being drawn and the PV width. On the page the
same tools are under the selection controls (*Lasso seed*, *Isolate*,
*Remove*, *Show result cube*, *Intensity (M0)*, *Velocity (M1)*, *Dispersion
(M2)*, *Draw PV slice*, *PV width*), and the moment map and PV diagram are
shown there as well.

### Moments

**With no selection**, the moment is that of the opened cube, through
`/v1/products/moment` with the desktop moment dialog's defaults: the channels
of the velocity range shown (those whose centres lie inside it; the whole
band after *Full band*) and the threshold mask **on** with its automatic
threshold (`mask_enabled` and `threshold_auto` true, as the desktop's remote
moment sends them). The map is shown whole, recorded with a one-step recipe on
the opened dataset (*Measured on: the whole field over the velocity range
shown*) and reported to the desktop like the others. A lasso grown afterwards
does not remove it; the next moment pressed is then of the lasso object.

**With a lasso selection**, the moment is computed the desktop's way:
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
grow, *Clear*, a new cube), file and session entry, and the Measure tab
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
its title bar, moved and closed; *Hide* on the Measure tab hides it). Colours
as on the desktop: inferno for M0, a diverging cool-warm for M1 on the
desktop's range (median ± 4 robust sigma, values outside it transparent, as
the desktop draws a velocity field), viridis for M2. The headset measures the
M1 median and sigma on up to 65 536 of the map's finite pixels, one in each
equal share of them, and on every pixel when there are fewer, so the range
takes milliseconds when the map is first shown and when its crop changes
([#302](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/302)). On
a synthetic dense 2048² velocity map (a gradient along x plus noise) the
range differed from the desktop's (every pixel sorted) by 0.1 % of its width. The M0 and M2 ranges are the exact
minimum and maximum. A velocity in m/s (also
written `m s-1`, `m.s**-1`) is shown in km/s (the numbers divided by 1000);
the recipe keeps the backend's unit.

### PV slice

*Draw* (the menu's Measure tab → *PV slice*, or *Draw PV slice* on the page)
arms the PV path: each trigger press (or click on the page) adds a
point where the ray picks the cube (the first drawn sample, else the middle of
the ray inside the box), as a fractional pixel of the full cube (pixel
centres at integers, as the desktop's path). *Undo* removes the last point,
*New* starts another path, *PV width −*/*PV width +* set the slit width in
pixels (the menu gives it in arcsec too, from the cube's pixel size), *Hide*
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
[Selections and analysis](../vr/reference.md#selections-and-analysis).

### Mask layers

A mask cube made by a source finder (SoFiA-2 and others) or on the desktop
can be shown over the cube in the headset
([#282](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/282)). It
must be on the cube's grid: the same pixels on every axis and the same WCS
(CTYPE, CRVAL, CDELT, CRPIX); a mask on another grid is refused with what
differs. Only its sources' voxels travel to the headset (HCG 44's SoFiA-2
mask: 42 171 voxels of 77 million, loaded in about 0.1 s).

**Load it.** On the desktop, open the mask as a cube and press **Show on
Headset** in its window. For a file whose name ends in `mask`
(`*_mask.fits`, `sofia_out_mask.fits`, the headset's `*_vr_mask_1.fits`) the
desktop asks: **Mask Layer** sends it as a layer over the cube the headset
shows, **Show as Cube** shows it as a cube of its own (a data cube may be
named so). Send the cube first: a mask that arrives while the headset is
still loading a cube waits for that cube. On the page, *Mask over the cube*
under *Catalogue over the cube* takes a path on the backend. A mask with an
ID above 2 147 483 647 (a 32-bit signed integer) is refused.

**Show it.** **Select ▸ Mask ▸ Mask** opens the *Mask* window:

- *Outline*: the cube as usual, each source's surface drawn as a faint amber
  skin (its voxels on the edge of the mask, below the threshold too);
- *Mask only*: only the masked emission, every voxel of the mask drawn
  (below the threshold too, faintly);
- *Residuals*: the cube with the masked emission removed: what the mask
  missed. On HCG 44 with its SoFiA-2 mask almost nothing is left: outside the
  mask 61 voxels exceed 5σ and none 6σ;
- *Off*; *Labels* writes each source's ID at its centroid (the source being
  refined in green, larger).

Refined frames from the backend show the whole cube, so under *Mask only* and
*Residuals* the headset draws alone (no refinement).

**Refine a source.** *Pick* (in the window or on the Select tab), then the
trigger on the cube: the source under the ray is picked and drawn green.
*Pick* switches to Select, so it works from Explore or Clip too, and the
trigger picks before a PV path or the readout takes it. With
the Kinematic Lasso, each trigger then grows from the point it hits (that
seed alone) and edits the picked source: *Replace* (the lasso's *New*) makes
the source the grow, *Add* adds the grow, *Subtract* removes it. An edit never
takes a voxel of another source: *Replace* and *Add* claim free voxels only.
*New* in the window starts a new source, which takes the next free ID above
the mask's highest. *Undo* takes back the last edit (up to 32). *◂ / ▸* step
through the sources and *Visit* brings the picked one to the centre, as the
source list does.

**Save it.** *Save to desktop* (window) or *Select ▸ Mask ▸ Save*: the
backend writes the refined mask in the Workspace Exports as FITS
(`<mask>_vr_mask.fits`): the original mask with the edited sources' voxels
replaced and emptied sources cleared, the same grid, header and IDs, new
sources under their new IDs, HISTORY cards saying what changed. It goes to
the desktop as headset activity with its recipe (the grows, each with the
edit it made); Headset Activity ▸ Open opens it as a cube. The window says
*sent to the desktop* only once the desktop was told; when the file was
written but the desktop was not told, it says so and why. A mask opened by
the desktop is saved by its file, so it can still be saved after the
backend restarted or the desktop's session ended.

A mask layer belongs to its cube: opening another cube, a point table or a
velocity field takes it away. A mask the desktop sends while its cube is
still loading waits for that cube, and is dropped (with a note in the
window) when the cube cannot be opened; it is never laid over another cube.

### Selections shared with the desktop

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
[Shared selections](../vr/reference.md#shared-selections); what to check with the
headset is under
[Selections shared with the desktop, with the headset](../vr/reference.md#selections-shared-with-the-desktop-with-the-headset).

## Data types

### Simulation volumes

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
  a box. Instead of the velocity range, *Range* has a **clipping plane** you
  hold and tilt with the controller, or a slab of two parallel planes
  ([controls](../../user/reference/vr.md), #369). The default threshold is the 99th percentile of the values, not the
  noise rule. The readout gives X, Y, Z in the axes' unit, the cell, and the
  value.

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

  With the pieces apart, a grip that starts inside a piece moves and turns
  that piece; a grip elsewhere moves the whole model, pieces included. One
  colour bar per piece stands beside the box. While the model is apart the
  box's outline, ticks and labels, the refined frames and the crop's detail
  are not shown, and the trigger still picks on the assembled box. With
  *Fields*, switching the overlay off puts the model back together.

### Opening a point table, catalogue or velocity field

From the desktop: *Tools → Open in VR Headset…* in a point
table's or 3-D catalogue's window, or the *Open in VR Headset…* button under
the velocity viewer's controls; the link then carries `kind=points` or
`kind=velocity` (and a velocity field's box size, `box=`), and the page opens
the desktop's dataset in the same session, or its path again when the session
no longer holds it, as for a cube (see
[Open a cube from the desktop](../../user/reference/vr.md)).

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

### Simulation controls

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

### Point tables

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
[Refinement](../vr/reference.md#refinement-full-quality-frames-from-the-backend)): the
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
[Ray-traced particles](../../user/reference/raytraced-particles.md)) over every row the session
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

### Catalogues over a cube

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

### The source list

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
  [The headset from the desktop](../../user/reference/vr.md)). Each Send
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

### Velocity fields

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
[Points, catalogues, flows](../vr/reference.md#points-catalogues-flows); the checks
to run with the headset are under
[Points, catalogues, flows, with the headset](../vr/reference.md#points-catalogues-flows-with-the-headset).
