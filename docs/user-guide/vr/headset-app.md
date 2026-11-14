# The headset app

**VisIVO VR** is the VR page installed on the Meta Quest as an app of its own.
Started from the desktop, it puts you in VR at once: no browser window, no
address, no Enter VR to press. The page and its updates still come from the
backend; the app only opens it in the headset's immersive mode.

The desktop installs the app over the USB cable and starts it from **Open in
VR Headset…** (below). It can also be built, installed and started by hand.

## From the desktop

In the cube window, **Tools → Views → Open in VR Headset…**, then **Advanced
(USB cable)**. With the headset connected over USB (developer mode on, as for
the browser page), the section says whether VisIVO VR is on the headset:

- *not installed*, with **Install**;
- *older* than the one this computer has, with **Update**;
- *installed and current*.

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
| *This backend does not list the VisIVO VR app…* | The backend does not serve the app's statement at `/.well-known/assetlinks.json`: a development build needs the backend on the same computer (restart it after the first build); a release build needs an up-to-date backend. Nothing was started. |
| *Another VisIVO VR is installed* (a question) | The headset has a build of the app signed by another key (someone's own development build). **Yes** uninstalls it and installs this one; **No** leaves it alone. The desktop never uninstalls without asking. |

While the app runs, **Open on Headset** and **Show on Headset** send the
cube to it: the app is never started a second time, so you stay in VR and
the new cube replaces the old one. **Open in Browser** opens the page in the
Quest Browser instead (then press Enter VR, as before).

Without an APK with this build of the desktop, **Open on Headset** opens the
page in the Quest Browser as before.

### Where the desktop finds the app

In this order:

1. `VISIVO_QUEST_APK`: the path to an `.apk`, with its `.json` beside it
   (`visivo-vr.apk` and `visivo-vr.json`, as `tools/quest-app/build.sh`
   writes them);
2. the desktop's own resources: `quest-app/visivo-vr.apk` (in
   `Contents/Resources` of the macOS app, beside the executable elsewhere);
3. a source tree the desktop was built in: `tools/quest-app/out/`
   (`visivo-vr-dev.apk` first, then `visivo-vr.apk`).

For development: build the app with `tools/quest-app/build.sh`, start the
desktop from its build directory (`build/` in the source tree), and it finds
`tools/quest-app/out/visivo-vr-dev.apk` by itself. The `.json` beside the APK
says which package and version it is; without it the APK is not used.

## What you see

- **Started from the desktop with a cube**: the headset goes straight into VR.
  A card in front of you says *Opening ngc3137.fits…* until the cube appears
  (with a small cube that is already loaded, the cube is there from the first
  frame).
- **Started from the desktop with no cube**: *Connected to the computer*. Open
  a cube on the computer and use **Show on Headset**: it appears in the room.
- **Started from its icon** (App Library → VisIVO VR): *No cube yet*, with
  the address of the computer it reaches (`localhost:<port>`, through the USB
  cable) and what to press on the computer. The icon opens the backend the
  desktop last started it with.
- **A launch that was not used in time** (each launch from the computer works
  once, within a minute): *This launch has expired*. Press **Open on
  Headset** on the computer again.

Leaving VR quits the app: the cards offer **Quit VisIVO VR**, and the Quest's
system menu quits it too. Cubes sent from the computer while the app runs
arrive in the same session (**Show on Headset**); the desktop never starts the
app a second time while it runs. If it is started again anyway, you stay in
VR: the running app takes the new cube (the view may blur for a moment).

## Build and install it by hand

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

## What to check on the Quest 3

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
6. Start the app from its icon in the App Library: *No cube yet* with the
   computer's address; then **Open on Headset** on the computer: the cube
   arrives without leaving VR.
7. With a cube on show, send another with **Show on Headset**: it replaces the
   first in VR.
8. Quit from the system menu (or **Quit VisIVO VR** on a card when one is
   shown): the app is gone, nothing is left in the room.
9. Started by hand with `&debug` after `?launch=immersive`, the frame-rate
   numbers are shown in the headset: report the frame rate and the missed
   frames.
