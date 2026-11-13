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

What has been checked so far, and what to check on a Quest 3, are under
[Recovery from failures](reference.md#recovery-from-failures) and
[Recovery and diagnostics, with the headset](reference.md#recovery-and-diagnostics-with-the-headset).
