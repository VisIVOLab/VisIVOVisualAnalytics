# The headset app

**VisIVO VR** is the VR page installed on the Meta Quest as an app of its own.
Started from the desktop, it puts you in VR at once: no browser window, no
address, no Enter VR to press. The page and its updates still come from the
backend; the app only opens it in the headset's immersive mode.

The desktop side (installing the app over the USB cable and starting it from
**Open in VR Headset…**) is being added; until then the app can be built,
installed and started by hand, as below.

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

1. Start the app from the desktop (or by hand, above) with a small cube
   (WALLABY or HCG 44). You should be in VR within about 4 s, with
   *Opening …* then the cube. The backend log has the times
   (`app-launch: ticket exchanged in … ms`, then `xr-start`).
2. Open the hand menu, check the cube's controls work as in the browser.
3. Start the app from its icon in the App Library: *No cube yet* with the
   computer's address; then **Open on Headset** on the computer: the cube
   arrives without leaving VR.
4. With a cube on show, send another with **Show on Headset**: it replaces the
   first in VR.
5. Quit from the system menu (or **Quit VisIVO VR** on a card when one is
   shown): the app is gone, nothing is left in the room.
6. Started by hand with `&debug` after `?launch=immersive`, the frame-rate
   numbers are shown in the headset: report the frame rate and the missed
   frames.
