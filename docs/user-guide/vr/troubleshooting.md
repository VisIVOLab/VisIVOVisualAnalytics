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

Pairing problems (adb not found, the headset not authorised, no headset on
USB) are named by the *Open in VR Headset…* dialog itself, with the way out:
see [Open a cube from the desktop](quickstart.md#open-a-cube-from-the-desktop).
A cluster backend that runs an older copy of the code is reported by the
dialog's [backend check](cluster.md#the-dialogs-backend-check).

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
