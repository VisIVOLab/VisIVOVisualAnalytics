# How the public loader works

When a headset is [paired by code](../../user/reference/vr.md#pair-by-code), it does not
open the backend's own address: it opens the **public loader**, a small static
page that an institute hosts once for all its users (`web/vr-loader/` in the
repository, issue
[#213](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/213)). This page
explains what the loader does. Hosting it, with the pairing service it talks
to, is described in [Hosting headset pairing](vr-pairing.md).

The loader holds no data and no page code of its own. The VR page's files
(`static/vr/**`) come from the user's own backend over the WebRTC link, so the
page always matches the backend it talks to, whatever version the loader's
server has.

## The parts

```
Quest Browser, https://vr.example.org/visivo/
  loader (index.html, js/loader.js)  ── holds the RTCPeerConnection: ctl, api, ws channels
   ├─ service worker (sw.js)         ── app/<tag>/vr/... → GET /vr/... over the api channel,
   │                                    vr/b/<build>/... cached as "visivo-vr-<build>"
   └─ <iframe allow="xr-spatial-tracking" src="app/<tag>/vr/?transport=pair#session=…">
        the VR page from the backend ── its requests and WebSockets over a MessagePort to the loader
```

- **Why an iframe.** The peer connection must outlive the page. The VR page
  reloads itself (a new backend build, a lost GPU), and a top-level
  navigation would close the link. The loader stays, and the page runs in a
  same-origin iframe. WebXR is allowed there by the permissions policy
  (`allow="xr-spatial-tracking"`), and *Enter VR* is pressed inside the
  frame, which is the user activation WebXR needs.
- **Launched from the app's icon.** When the loader runs as the installed
  app (its manifest's `display: standalone`), it adds `launch=app` to the
  page's address in the iframe
  ([#237](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/237)), and
  the page tries to enter VR by itself once its first cube is on show. A
  browser counts a user activation per frame: a press in the loader is not
  one in the iframe, and whether the Quest Browser lets the iframe use the
  launch from the icon is its own decision. The page therefore treats a
  refusal as the normal fallback: *Enter VR* stays, and is pressed once per
  headset session, since the desktop then sends further cubes into the same
  page and session. The page offers the session to the browser
  (`navigator.xr.offerSession`, the browser's own *Enter VR*) only when its
  address has `?offer`: the page always shows its own button, and an offer
  cannot be taken back once made
  ([#291](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/291)).
- **In the VisIVO VR app.** The headset app (`tools/quest-app/`,
  [#383](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/383))
  opens the loader as `https://<loader>/?launch=immersive#pair=CODE` after
  its scanner read the desktop's QR code (see
  [The VisIVO VR app's flow](#the-visivo-vr-apps-flow) below). With
  `launch=immersive` the loader passes `launch=immersive` to the page, which
  then enters VR at load, before its cube, with no press, as after the
  desktop's USB launch. The app's browser shows no 2-D page at all, so the
  loader never shows its form there: a code that cannot be used (or none)
  is said in VR by the loader itself (`loader-xr.js`, one card, a trigger or
  a pinch or 20 s to leave), and leaving it, like leaving the page's VR,
  closes the loader's window, which brings back the app's scanner.
- **One page.** Two loader windows share the loader's origin, so their pages
  meet on one channel: the one with a cube stays in use, an empty one pauses
  and names the cube in the other (see
  [One page for the whole session](../../user/reference/vr.md#one-page-for-the-whole-session)).
  A page served by the backend over the cable and one in the loader are
  different origins and do not see each other; both then say hello to the
  backend's hub, and the desktop sends a cube to the newest one.
- **Why a service worker.** The page's files are ES modules that import each
  other by relative address. The worker answers them from what the loader
  fetched from the backend, and keeps the versioned files (`vr/b/<build>/…`)
  in Cache Storage per build, so the page never mixes two builds.
- **The page's transport.** In the iframe the page sends its requests and
  WebSockets over the data channels instead of HTTP (`transport.js`;
  HTTP-over-channel framing in `pair-wire.js` and `pair-transport.js`, the
  same files shipped in the loader). The wire format is in
  [Headset pairing by code](../backend-api/vr-pair.md#what-a-paired-page-gets).
  Served directly by the backend (`localhost` through adb, a cluster's HTTPS
  portal), the page works exactly as before.
- **The code.** The loader reads it from `#pair=CODE` in its address (the QR
  code's link) or from its form, then removes it from the address bar and the
  history.
- **The pairing service** is the one named in the loader's `config.json`.
  `?pair_service=wss://…` overrides it, for development, and only when the
  loader runs on `localhost`, so a link cannot point a public loader at
  another service.

## The transport line

*Copy diagnostics* on the page names the transport, for example:

```
transport: WebRTC data channel via the pairing service vr.example.org; link up; ICE srflx/host udp; round trip 14 ms; 46 requests, 10.3 MB in, 0.1 MB out, 0 cancelled
```

- **via the pairing service** names the service's host, from `config.json`.
- **link** is `up`, `down (why)` while the loader resumes, or `lost (why)`
  once it gave up.
- **ICE** gives the candidate types of the pair in use, the headset's then the
  computer's, and the protocol. `host` is an address on the local network,
  `srflx` an address seen through NAT (STUN), `relay` a TURN server (the line
  then adds `via udp relay` or `via tcp relay`): the link goes through that
  server, which is slower. `host` or `srflx` on both sides is a direct link.
- **round trip** is the link's current round-trip time; the line is refreshed
  every 5 s.
- **resumed N×** appears after the link came back from a drop.
- The request counts and bytes are those of the page's API traffic over the
  link.

Served directly (no loader), the line reads `transport: direct HTTP (…)`.

The page's event log, also in the diagnostics, records the link's changes:
`pair-link-down` (with the reason) when it drops, `pair-link-up` when it is back,
`pair-link-lost` when it cannot be resumed.

## A dropped link

When the link drops (Wi-Fi, the computer asleep), the page shows *Backend not
reachable* and the loader reconnects by itself with the **resume code** the
backend gave at pairing, retrying for up to three minutes. No new code is
needed, the page is not reloaded, and the cube stays on show. Reloading the
loader's tab resumes as well (the resume code is kept for that tab only).

Requests that were under way when the link dropped and that change nothing
(`GET` and `HEAD`, such as the page's files, and the requests that load the
cube's reduced copy) are sent again once the link is back. A download that had
begun continues where it stopped only when the backend marks it as the same
file (the same strong ETag and length, as the page's own files are); otherwise
it ends with a network error. The others (a selection, a moment map) end with
*Backend not reachable*, as they would over HTTP.

When the resume fails (the code expired, or the backend restarted and forgot
it), or when the backend gave no resume code at pairing (the pairing service
did not answer in time), the page shows *The pairing with the computer
ended* with **Enter a new code**, which opens the loader's code form. The
same notice says *The computer disconnected this headset* when the desktop
disconnected it, and *VisIVO Visual Analytics on the computer was closed*
when the backend stopped: neither is resumed.

## The VisIVO VR app's flow

```
App Library ▸ VisIVO VR
  ScanActivity (2-D panel)       passthrough camera (Camera2) → ZXing → https://<loader>/#pair=CODE
    │  or the code typed           (PairCode: the code, and the QR code's host must be the app's loader)
    ▼
  LauncherActivity (immersive)   opens https://<loader>/?launch=immersive#pair=CODE in the browser's
    │                            immersive app mode; the browser checks <loader>/.well-known/assetlinks.json
    ▼
  loader, ?launch=immersive      pairs; the page in its iframe (launch=immersive) enters VR at load
    │
  leaving VR, or a bad code      the session ends, the browser's part closes, LauncherActivity resumes
    ▼                            and opens the scanner again (in Home)
  ScanActivity
```

- **The scanner** is a native panel (`tools/quest-app/android/ScanActivity.java`):
  the headset's passthrough camera through Android's Camera2 (Horizon OS's
  Passthrough Camera API, Quest 3 / 3S, Horizon OS v74+), permissions
  `android.permission.CAMERA` and `horizonos.permission.HEADSET_CAMERA`,
  asked for at the first start. Only the luminance plane is read
  (1280×960), decoded by ZXing on the headset, and dropped. The web page
  cannot do this: in the app's browser part no 2-D page is shown, and the
  Quest Browser's camera access from a page is still experimental.
- **The launch** opens the app's own start URL with the code
  (`PairCode.loaderLaunchFor`), whatever address the QR code had, so the
  loader always gets `?launch=immersive`. The desktop's USB launch
  (`http://localhost:<port>/vr/?launch=immersive#ticket=…`) is unchanged
  and still quits the app when VR ends.
- **The origin check.** The browser opens the loader in the immersive app
  mode only when the loader's host publishes the app in
  `/.well-known/assetlinks.json` ([Hosting headset pairing](vr-pairing.md#the-visivo-vr-apps-statement));
  the backend publishes it for `localhost` (the USB launch).
- **The headset's own QR scanner.** The app's VIEW filter for the loader's
  host has `autoVerify`, so a verified link `https://<loader>/#pair=CODE`
  opens the app (and the cube) instead of the browser. Horizon OS v2.9 (in
  its test channel since 28 September 2026) adds a system QR scanner on
  Quest 3 and 3S; what it does with a scanned link (the browser, or the app
  through Android's verified links) is not documented: it is on the device
  check list. Without the app, the same link opens the loader in the Quest
  Browser, which pairs and asks for one *Enter VR* press.
- **Installing the app** needs the headset in developer mode (the desktop
  installs it over USB): Horizon OS installs apps from outside the Meta
  Horizon Store only that way. A page cannot install it. Installing from a
  link without developer mode needs a Store listing of the app.

### Updating the INAF host (release 1.2.1)

The pairing service does not change. On the server (Debian, Apache), with
the release's `web/vr-loader/` copied to `/tmp/vr-loader/` (for example
`git archive 1.2.1 web/vr-loader | tar -x -C /tmp --strip-components=1`
on a checkout, then `scp -r`):

```sh
cd /var/www/html/vr_visivolab_oact_inaf_it
# The loader's files and the app's statement; the server's own config.json is kept.
sudo rsync -rlt --exclude config.json /tmp/vr-loader/ ./
# Same owner and modes as the files already there:
sudo chown -R --reference=index.html .well-known js
sudo find .well-known -type d -exec chmod 755 {} + ; sudo chmod 644 .well-known/assetlinks.json
```

Apache serves `.well-known/assetlinks.json` from the document root as it is.
If the site or the server blocks paths that start with a dot (look with
`grep -RniE 'well-known|DirectoryMatch|FilesMatch|RedirectMatch' /etc/apache2/`),
add to `/etc/apache2/sites-available/vr_visivolab_oact_inaf_it-ssl.conf`,
inside the `<VirtualHost>`:

```apache
<Location "/.well-known/assetlinks.json">
    Require all granted
    ForceType application/json
    Header always set Cache-Control "no-cache"
</Location>
```

then `sudo apachectl configtest && sudo systemctl reload apache2`.

**Check it** from any computer:

```sh
curl -sS -D - https://vr.visivolab.oact.inaf.it/.well-known/assetlinks.json
curl -sS 'https://digitalassetlinks.googleapis.com/v1/statements:list?source.web.site=https://vr.visivolab.oact.inaf.it&relation=delegate_permission/common.handle_all_urls'
curl -sS https://vr.visivolab.oact.inaf.it/js/loader-xr.js | head -3
```

The first answers `HTTP/1.1 200`, `content-type: application/json`, and
names `it.inaf.oact.visivo.vr` with the fingerprint
`A1:54:C4:5B:54:20:5F:B9:6B:B9:80:28:6D:86:F3:A6:78:17:E1:32:2E:4F:68:AB:44:40:30:C0:43:04:D8:D4`;
the second lists that statement (Google's view of it); the third shows the
new loader's file.

## Checking it without a headset

```
node validation/vr_lifecycle.mjs --paired [--regressions] --port 8820 --cdp-port 9420
```

This starts a backend on port 8820, serves `web/vr-loader/` on 8821 as a
static server would, and starts `validation/vr_pair_fake.py` on 8822 (a
minimal pairing service and bridge implementing the contract, with aiortc).
It then runs the smoke and regression checks in the loader's iframe, a
dropped link that resumes, a forgotten pairing that asks for a new code, and
the transport's overhead against direct HTTP.

```
node validation/vr_lifecycle.mjs --app-paired --port 8830 --cdp-port 9430
```

runs the VisIVO VR app's flow on the same setup, with a WebXR stand-in: the
loader opened with `?launch=immersive#pair=…` pairs and its page enters VR
with no press; leaving VR closes the loader's window; an unknown code and a
missing code are said in VR by the loader, and leaving that notice closes the
window; in a browser tab the loader still shows its form. The scanner, the
browser's immersive mode and the origin check are on the Quest check list.

To try the real pieces on one computer, run the pairing service locally
(`ws://127.0.0.1:8765`, see the
[service's README](https://github.com/VisIVOLab/VisIVOVisualAnalytics/blob/main/services/vr-pair/README.md#on-a-development-machine-next-to-the-backend)),
serve `web/vr-loader/` on `localhost` (a secure context, so no certificate),
and start the backend with `VISIVO_VR_PAIR_URL` and `VISIVO_VR_PAGE_URL`
pointing at them.
