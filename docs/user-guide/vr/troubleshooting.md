# Troubleshooting

The page watches the backend (one small request every 5 s, and every 1 to
5 s while it does not answer) and says what went wrong where you are: on a
card in front of you in the headset (point the primary hand's ray at its
buttons and pull the trigger), and in a banner on the 2-D page. Nothing waits
on it: the cube stays on show and moves with your hands, and the card never
stays in the way. A card that lasts (the backend not reachable, the token
refused, an out-of-date page) follows your gaze for 6 s, then moves aside, 40°
to the left and lower, and stays there. **Hide** folds it into a line on the
hand menu (*⚠ Backend not reachable: retrying — Help → Guide shows it*) until
its state changes; the menu's **Help → Guide** brings it back (see
[The hand menu](controls.md#the-hand-menu)).
The card is redrawn only when its text changes (at most once per check, every
1 to 5 s).

| Message | What happened | The way out |
|---|---|---|
| **Backend not reachable**, *the headset cannot reach the backend on the computer* | in the headset over USB: the cable was unplugged, or the headset or adb restarted, so the port forwarding (`adb reverse`) is gone; or the backend is restarting | the page retries by itself (*Trying again by itself every few seconds*); if it does not clear, on the computer press **Open on Headset** again (it restores the forwarding). *Retry now*, *Hide*, *Exit VR* |
| **Backend not reachable**, *the backend does not answer* | in a desktop browser or over the LAN: the backend stopped or the network is down | start the desktop application (it starts the backend); the page retries by itself |
| **The backend is not answering behind its proxy** / **answered with an error** | the address answers with an HTTP error (a proxy's 502, a 500) | the page retries by itself; *Retry now*, *Hide* |
| **The backend restarted** | it came back with its sessions gone | nothing: the page opened the cube again by its path, the view is kept; repeat the last selection if it did not finish |
| **The cube was opened again** | a request found the cube's dataset gone (the desktop's session ended) | nothing: the page opened it again; repeat the selection |
| **The cube could not be opened again** | the file moved, changed, or its revision cannot be verified; or the backend did not answer in time, was busy or failed | for a file that changed: use **Open and load**, or open it again from the desktop, to reload its voxels and coordinates. A backend that did not answer, was busy or failed is tried again by itself at most every 10 s while it answers its checks; unchanged files recover automatically |
| **The backend refused the token** | the backend was restarted with a new token, or the token expired | on the computer: **Open in VR Headset… → Open on Headset** (a fresh link); or paste the token on the page, then *Check again*; *Hide* meanwhile |
| **The backend was updated** | the backend serves a newer version of this page than the one open (a tab kept from before an upgrade) | *Reload page* (the cube from the desktop opens again by itself) |
| **This page is out of date** / **Update the application** | the page and the backend speak different versions of the page API (or the backend is older than the page: it has no `/v1/vr/info`) | *Reload page*; if that does not help, update VisIVO Visual Analytics on the computer |
| **The graphics were reset** | the browser took the GPU away (memory pressure, another app) | the page waits for it and uploads the cube again by itself (*Graphics restored*); a VR session cannot outlive it, so press *Enter VR* again |
| **The graphics did not come back** | not restored after 10 s | *Reload page* |
| **The VR session ended** (on the 2-D page) | the headset or the browser ended it: the system menu, the headset taken off for a while | *Enter VR*: the cube and the settings are kept |
| **ngc3137.fits is open in another window** (a page that shows nothing) | another VisIVO VR window or tab of the same address shows a cube; a page with nothing on show never takes over from it (#237) | switch to that window in the browser's window list; close this one; or *Use this window* to make this one the page in use |
| **Open in another window** (a page that showed a cube) | a newer page with a cube was opened (another *Open on Headset*, a second loader window) | use the newer one; or *Use this window* here |
| **Opening ngc3137.fits from the computer…** | the desktop sent that cube to this page (#237) | nothing: it is shown when loaded (in VR if you are in VR) |
| *press Enter VR* beside the WebXR status, after a launch from the app's icon | the page tried to enter VR by itself and the browser wanted a press (#237) | press **Enter VR** once; the cubes the desktop sends then come into the same session |
| **This cube is too large for this device** | the reduced cube would not fit the [memory budget](reference.md#the-settings-on-the-page) | a larger *Spatial bin* or *Spectral bin*, or a smaller *Max side* |
| Refine: *lost: retrying in N s, the headset's own rendering meanwhile* | the render session's connection dropped | nothing: the headset shows its own rendering, and the page reconnects (2, 4, 8 … 30 s) and refines again; if the backend no longer holds the cube, the page opens it again first |

Problems of the USB way (adb not found, the headset not authorised, no
headset on USB) are named by the *Open in VR Headset…* dialog itself, with the
way out: see [Open a cube from the desktop](quickstart.md#open-a-cube-from-the-desktop).
A cluster backend that runs an older copy of the code is reported by the
dialog's [backend check](cluster.md#the-dialogs-backend-check).

## Pairing by code

The messages of [pairing by code](quickstart.md#pair-by-code) come from three
places: the *Open in VR Headset…* dialog on the computer, the *VisIVO VR* code
page on the headset (under the code form), and the page's card once the cube
is open. Whatever happens, the [USB cable](quickstart.md#advanced-usb-cable)
still works.

| Message | What happened | The way out |
|---|---|---|
| Headset: *This code is not known*, *This code has expired*, *This code was already used* | the code was mistyped, is older than its five minutes, or was used already: a code works once, even when that pairing then failed. Closing the dialog, or pressing **New Code**, ends the old code too | press **New Code** in the dialog and type the new code. Case, spaces and the dash do not matter |
| Headset: *Too many attempts: wait a minute, then try again* | too many wrong codes from this network | wait, then type the code shown now in the dialog |
| Dialog: *This backend has no pairing service configured, so the headset connects through a USB cable instead* (the backend answered 409 `not-configured`) | **Settings ▸ Backends ▸ Headset Pairing** is empty (or `VISIVO_VR_PAIR_URL` is not set for a backend started by hand); or the backend has no WebRTC support. On a Mac with Apple Silicon, pairing needs **macOS 14 or newer**: on macOS 13 the backend cannot load its WebRTC library and answers the same way | type the two addresses your institute gives you in the settings and restart the application; on macOS 13, update macOS or use the USB cable. The dialog has opened its USB section meanwhile |
| Dialog: *The backend cannot reach its pairing service right now, so the headset connects through a USB cable instead* (409 `unreachable`) | the computer has no internet, the *Pairing service* address is wrong, the server is down, or a network filter blocks the connection | check the address in the settings (`wss://…`) and the internet connection. The backend keeps trying in the background (at most a minute apart): close the dialog and open it again later. Or use the USB cable |
| Headset: *The pairing service cannot be reached* | the headset has no internet, or the server is down or refuses this page (an administrator's setting) | check the headset's Wi-Fi; if it persists, tell whoever runs the server ([Hosting headset pairing](../../deployment/vr-pairing.md)) |
| Headset: *The computer is not connected to the pairing service (is VisIVO Visual Analytics running?)* | the code was right, but the backend that issued it is no longer connected to the service: the application was closed, the computer went to sleep, or its network dropped | start the application (or wake the computer), then **New Code** |
| Headset: *No network path to the computer was found*, or *The computer did not answer in time*; dialog: *The code was used, but no headset connected* | the code was accepted, but the headset and the computer could not reach each other. Usually the network keeps devices apart (guest Wi-Fi, client or AP isolation), or they are on different networks; or the Mac's firewall blocks the backend (next row) | put both on the same network, one that lets devices talk (a home or office Wi-Fi, a phone's hotspot). If your institute runs a relay (TURN) server, the link goes through it instead: it works, more slowly, and the `transport:` line in *Copy diagnostics* shows `relay`. Otherwise use the USB cable |
| Mac: *Do you want the application "Python" to accept incoming network connections?* | the first time a headset connects, macOS's firewall asks about the backend (it runs in Python), which receives the headset's link | **Allow**. If it was denied, allow it in System Settings ▸ Network ▸ Firewall ▸ Options. While it is blocked, a direct link fails as in the row above; a relayed one still works |
| No internet at all (a demo room, a train) | pairing needs the server, so neither side can pair | use the [USB cable](quickstart.md#advanced-usb-cable): it needs no network |
| Headset: *Backend not reachable*, then it clears by itself | the link dropped (Wi-Fi, the computer asleep, the browser closing a connection during a large transfer) and the page resumed it with its resume code, without a new code | nothing. *Copy diagnostics* shows it: while resuming the `transport:` line reads `link down (the link was interrupted…)`, afterwards `link up` with `resumed 1×`, and the events list `pair-link-down` and `pair-link-up`. If it happens often, keep the computer awake and the headset near the access point |
| Headset: **The pairing with the computer ended**, with *The pairing could not be resumed in time* or *The computer gave this headset no resume code* | the link stayed down for three minutes, the backend restarted and forgot the headset, or the service did not give a resume code in time | **Enter a new code** on the card, and **New Code** in the dialog |
| Headset: **The pairing with the computer ended**, *The computer disconnected this headset* | **Disconnect** was pressed in the dialog on the computer | pair again with a new code, if that was not meant |
| Headset: **The pairing with the computer ended**, *VisIVO Visual Analytics on the computer was closed* | the application (and its backend) stopped | start the application, open the cube, and pair again |
| Headset: *The computer refused this headset* | the backend did not accept the join (it no longer knows the code, for example after a restart) | **New Code** |

**For support**, *Copy diagnostics* on the page gives the `transport:` line
(the pairing service's host, the link's state, the ICE candidate types, the
round trip and the resumes, explained in
[The transport line](public-loader.md#the-transport-line)), and the dialog's
state says whether the backend sees the headset as connected.

## Upgrades

The page's code is served from addresses that change with every
version (`/vr/b/<build>/…`) and the page itself is always revalidated, so
after an upgrade a reload always gets the new code. A tab left open across an
upgrade may run a mix of old and new code until it reloads; it notices the
upgrade at its next check and says *The backend was updated*.

## Diagnostics for support

The page sends its errors and the key events
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
[backend API](../../backend-api/meta-files.md#post-v1vrlog). **Copy diagnostics** on the page puts the versions, the GPU and
its limits, WebXR, the cube, the frame rate, the quality, the settings and the
last events on the clipboard (or shows them, when the clipboard is not
available): paste them into a support request.

Two lines of the diagnostics are about the desktop driving the page
([#237](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/237)):

- `headset hub:` the page's link to the desktop's hub (as the *hub* line on
  the wrist) and its client id. *this backend has none*: the backend is older
  than the page; the page works as before, but the desktop cannot send cubes
  to it nor see what it makes. The events `hub-show` (a cube the desktop
  sent), `hub-failed` (a report that did not reach the desktop, with the
  reason: for a product, *not saved for the desktop*) and `hub-hello` are in
  the last events.
- `entering VR:` how *Enter VR* is reached on this page: *launched as an app*
  (from the installed app's icon) with what happened when it tried to enter by
  itself (*entered*, or *refused without a press*), and *offerSession*:
  *offered* (the browser shows its own Enter VR), *not in this browser*, or
  *stopped* with the browser's reason.

What has been checked so far, and what to check on a Quest 3, are under
[Recovery from failures](reference.md#recovery-from-failures) and
[Recovery and diagnostics, with the headset](reference.md#recovery-and-diagnostics-with-the-headset).

## GPU context loss at the first lasso on a Quest 3

On a Quest 3 (Quest Browser 154, Adreno 740) the page lost its WebGL context
at the first Kinematic Lasso, and the browser's GPU process restarted
([#229](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/229)).
The cause, found on the headset: the browser's GPU process rejects a
`texImage3D` (the call that allocates a 3-D texture) issued while a WebXR
session is running. A trace of the browser showed the command decoder's parse
error on the GPU thread right before the process was terminated, and
`chrome://gpu` logged *Error: 5 for Command kTexImage3D*, then *Restarting GPU
process due to unrecoverable error. Context was lost.* The first lasso was the
first time the page allocated a texture for the selection mask, in VR. With
the mask's storage allocated on the page beforehand and only written into in
VR, eight lassos in a row in VR lost nothing.

Since then the page allocates every 3-D texture outside VR:

- **Room for the cubes (issue #235)**: when the page starts, and again
  before each **Enter VR** if *Max side* or *Texture* changed, the page keeps
  room on the GPU for a cube of *Max side* voxels on every axis, twice (the
  cube on show and the next one), with one selection mask over it. On a
  headset with 8 GB, at the default 256 and 8-bit, that is 48 MiB
  (256³ × 2 for the two cubes, 256³ for the mask, and their empty-space
  grids); at 16-bit 80 MiB. When it does not fit this device's share the
  side is reduced to what fits. A cube shown outside VR that is larger
  widens the room when twice it still fits. The diagnostics' memory line
  says it: *VR capacity 256×256×256 8-bit × 2 banks + mask 50.6 MB (cube in
  it, spare 16.9 MB) of 256 MiB for cubes*.
- **A cube that arrives in VR** (*Show that cube* after *Isolate* or
  *Remove*, a load at another binning, a cube of another dataset, a load
  started from the desktop) is written into that room and **shown at once**,
  with no texture allocated. Only a cube **larger than the room** on one axis
  (or of the other texture, 16-bit into an 8-bit room) still waits: it is read
  at once and kept, the cube on show stays, and the notice *The new cube is
  shown when you leave VR* says why (*it is larger than the room kept for
  cubes in VR (256×256×256)*). Leaving VR puts it on show; enter VR again to
  see it in the headset. A newer cube, shown or waiting, replaces one already
  waiting. To avoid the wait, choose a larger binning, or a larger *Max side*
  before entering VR (it costs the memory above).
- **The selection mask** is the room's own (one byte per voxel); in VR each
  selection is only written into it.
- **The headset benchmark** makes its cubes on the page first (*Prepare the
  headset benchmark*), then enters VR (*Start the headset benchmark*).
- **A context restored before the VR session ended** is rebuilt when the
  session ends.

Any other 3-D allocation attempted in VR is refused before it reaches the GPU
and logged as `tex3d-in-xr` in the diagnostics: if you ever see that line,
report it with **Copy diagnostics**.

What to check on the Quest 3:

1. Restart Quest Browser, open the page (`http://localhost:<port>/vr/`, with
   the `#token=…` part as usual), load the cube, **Enter VR**.
2. Make ten Kinematic Lassos in a row (new seeds, then *add* and *carve*),
   moving the controller and your head over the selection for about ten
   seconds after each. The picture must not go black, and the page must not
   show *The graphics were reset* afterwards.
3. In VR: *Isolate*, then *Show that cube*: the isolated cube replaces the
   one on show at once, without leaving VR and without a notice. Then switch
   cubes ten times in VR (*Show that cube*, a load from the desktop, the
   other binning): each is shown at once, the picture never goes black, and
   no *The graphics were reset* appears.
4. On the page, **Copy diagnostics**: there must be no `webgl-context-lost`
   and no `tex3d-in-xr` line; each switch is a `cube-switched-in-vr` line with
   its time in ms; note the `GPU textures … MB` line with its *VR capacity*
   part.
5. **Prepare the headset benchmark**, wait for *Benchmark ready*, then **Start
   the headset benchmark** and keep still until it ends; copy the table.

## Diagnosing a GPU context loss

This section is for debugging only. To find the cause of the context loss
above, the address could leave out one per-frame path at a time; the
switches remain for any later loss. Each switch works only together with
`?debug`:

| Address switch | What the page leaves out |
|---|---|
| `nohover` | The hover pick under the primary ray in VR, its marker and the live readout. A click still picks, snaps and selects. |
| `nohud` | The numbers on the wrist: no redraw, no upload, no quad. |
| `noreadout` | The live and the pinned readout cards. The controller labels stay. |
| `noselpanel` | The results panel: neither the Selection window in the headset nor the panel on the page is painted. |
| `noseloutline` | The selection's own lines (pick marker, box, bounding box). The analysis lines are still uploaded but not drawn. |
| `novolume` | The cube's volume rendering, mask included. Lines, panels, input and the XR frames stay. |

No switch clears or changes a selection: the mask upload, the statistics, the
recipe and the share with the desktop go on as usual. The active switches are
named in **Copy diagnostics** (`debug cuts: …`) and, when the HUD is shown, on
the first line of the numbers on the wrist.

Each run on the headset is: restart Quest Browser, open the address, load
the cube, enter VR, make one lasso, then move the controller and your head
over the selection for about ten seconds. For example
`http://localhost:<port>/vr/?debug&nohover&nohud&noreadout&noselpanel&noseloutline`
(add the `#token=…` part as usual). If the page survives, put the switches
back one at a time; if it still loses the context, try `?debug&novolume`.
Note which address lost the context and which did not.
