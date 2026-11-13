# Quickstart and pairing

There are two ways to bring a cube from the desktop application into a Meta
Quest:

- **[Pair by code](#pair-by-code)**, the usual way: a short code shown on the
  computer, typed on the headset (or a QR code scanned). No cable, no adb, no
  developer mode. It needs an internet connection and a pairing server that
  your institute hosts.
- **[Advanced: USB cable](#advanced-usb-cable)**: adb and a cable (or adb over
  Wi-Fi). It works offline, so it is the way for a computer without internet,
  for demos on a network you do not control, and when no pairing server is
  configured.

Both open the cube in the same backend session as the desktop, and the page in
the headset is the same.

## Pair by code

1. **Open the cube** in the desktop application.
2. Choose **Tools → Views → Open in VR Headset…** (or the same entry in
   Inspector ▸ Analysis). The dialog shows a large code, such as `K7Q-4MX`, the
   time it has left (five minutes), and a QR code.
3. **On the Quest**, open the *VisIVO VR* page in the Quest Browser (the
   address your institute gives you), or its icon once installed (step 4).
   Type the code and press **Connect**. Or scan the QR code with the headset
   (where the Quest's software offers QR scanning): it opens the page with the
   code already in it. The cube loads in the page: press **Enter VR**.
4. **Once:** install the page as an app, from the Quest Browser's menu
   (*Install app*). It then has an icon in the headset's app library, opens
   without the browser's address bar, and starts on the code form.

The first time in VR, a short controls guide stands in front of you (*Next*,
*Close*; the hand menu's Help tab shows it again). The grips move the cube,
and the hand menu has the rest: see [Controls and the hand menu](controls.md).

```{note}
**Before the first time**, the desktop application needs to know your
institute's pairing server: two addresses, typed once in **Settings ▸ Backends
▸ Headset Pairing** (see [below](#the-pairing-settings)). Until then, the dialog
opens its USB section and says that pairing is not configured.
```

### What the dialog shows

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

### On the headset

- The page shows the cube with the status *in the desktop's session*, as over
  the cable.
- The headset and the computer talk to each other directly, over the local
  network, once paired: keep both on the same network for the best speed. On a
  network that keeps devices apart, the link goes through a relay server, if
  your institute runs one, more slowly (see
  [Troubleshooting](troubleshooting.md#pairing-by-code)).
- If the link drops (Wi-Fi, the computer asleep), the page says *Backend not
  reachable* and reconnects by itself for up to three minutes, without a new
  code and without reloading; the cube stays on show. If it cannot, it says
  *The pairing with the computer ended* with **Enter a new code**: ask the
  dialog for a new code (**New Code**) and type it there.
- *Copy diagnostics* on the page names the link (the `transport:` line):
  see [The transport line](public-loader.md#the-transport-line).

How the page works behind the code is in
[How the public loader works](public-loader.md).

### The pairing settings

Pairing works with the local backend and with a remote one alike: the backend,
not the desktop, talks to the pairing service, and it needs to know two
addresses. For the backend the desktop starts, they are set in **Settings ▸
Backends ▸ Headset Pairing**:

| Field | Passed to the backend as | What it is |
|---|---|---|
| *Pairing service* | `VISIVO_VR_PAIR_URL` | the pairing service, `wss://…` |
| *Headset page* | `VISIVO_VR_PAGE_URL` | the *VisIVO VR* page the headset opens, `https://…/` |

Your institute gives you both values. Both fields are empty after
installation, and no server is built in. They are read when the backend
starts, so restart the application after changing them. The same variables set
in the environment the application starts from take precedence over the
fields. A remote backend (a cluster's) uses its own configuration. Running the
server is described in [Hosting headset pairing](../../deployment/vr-pairing.md).

### When pairing is not available

When the backend has no pairing service (*This backend has no pairing service
configured*), cannot reach it (*The backend cannot reach its pairing service
right now*), or is older than the desktop, the dialog opens **Advanced (USB
cable)** by itself and says why: the [USB way](#advanced-usb-cable) then works
as before. It can also be opened by hand at any time; adb is only run once it
is open. See [Troubleshooting](troubleshooting.md#pairing-by-code).

## Advanced: USB cable

The USB way needs adb on the computer and the headset in developer mode, but
no internet and no pairing server: use it offline, for demos on a network you
do not control, or where pairing is not configured. In the *Open in VR
Headset…* dialog it is under **Advanced (USB cable)**.

### Quickstart over USB

1. **Once:** install adb (`brew install android-platform-tools` on a Mac) and
   turn on **developer mode** for the headset in the Meta Horizon app on your
   phone (Devices → your headset → Headset settings → Developer mode).
2. Open the cube in the desktop application and connect the Quest with a
   USB-C data cable.
3. **Tools → Views → Open in VR Headset…**, then open **Advanced (USB
   cable)**. The dialog says what is missing, step by step; the first time,
   put the headset on and accept **Allow USB debugging** (tick *Always allow
   from this computer*).
4. Press **Open on Headset**, put the headset on: the cube loads in the Quest
   Browser. Press **Enter VR**.
5. The first time, a short controls guide stands in front of you (*Next*,
   *Close*; the hand menu's Help tab shows it again). The grips move the cube,
   and the hand menu has the rest: see [Controls and the hand menu](controls.md).

If something goes wrong, the page says so in the headset and offers the way
out: see [Troubleshooting](troubleshooting.md).

### Open a cube from the desktop

With a cube open in the desktop application, **Tools → Views → Open in VR
Headset…** (or the same entry in Inspector ▸ Analysis), under **Advanced (USB
cable)**, opens it on a Quest connected to the Mac, in the same backend
session: the headset's page
receives the token, the session, the dataset and the cube's path, so there is
nothing to paste or type on the headset. This is not *Open in VR*, which
sends the desktop's own 3-D view to an OpenXR runtime on this computer (see
[Open in VR](../cube-viewer.md#open-in-vr)); it needs no VR runtime here.

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
[Headset with a cluster backend](cluster.md).

Point tables, 3-D catalogues and velocity fields open from their own windows
the same way: see [Opening one](data-types.md#opening-one).

### On the Quest 3, over USB, by hand

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

### On the Quest 3, over Wi-Fi

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
[pairing by code](#pair-by-code), or adb over Wi-Fi.

Over Wi-Fi the refined frames can travel as video: see
[Over Wi-Fi: refined frames as video](reference.md#over-wi-fi-refined-frames-as-video-webrtc).

## Opening it on the Mac (no headset)

1. Start the backend as usual (the desktop application starts one on
   `http://127.0.0.1:8000`; or by hand, see [Getting started](../getting-started.md)).
2. Open `http://127.0.0.1:8000/vr/` in Chrome, Edge or Safari and paste the
   backend token (the contents of `~/.visivo_token`) into the *Backend token*
   field. Opening `http://127.0.0.1:8000/vr/#token=<token>` works too: the
   page reads the token from the part after `#`, which a browser never sends
   to the server (so it is in no log), and removes it from the address bar.
3. Type the path of a cube on the backend (or *Browse…*), choose the binning,
   and press *Open and load*. Drag to turn the cube, scroll to zoom.

*Synthetic* draws a generated cube of the chosen size without any backend, for
checking the page and measuring frame rates. The hand menu can be shown in the
page's 3-D view and used with the mouse (see [The hand menu](controls.md#the-hand-menu)).
