# The public loader (for administrators)

The public loader lets a headset open the VR page from a **public HTTPS
address**, with no USB cable, no `adb` and no certificate on the user's
computer (issue #213, part of #210). The headset pairs with the user's
backend through a **pairing service**, and the two talk over a WebRTC data
channel. The loader is a small static directory, `web/vr-loader/` in the
repository, that an institute hosts once for all its users.

What the loader does **not** do: it holds no data and no page code of its
own. The VR page's files (`static/vr/**`) come from the user's own backend
over the data channel, so the page always matches the backend it talks to.

```{note}
The headset side (this loader, and the page's data-channel transport) is in
place. The pairing service (#211), the backend's bridge (#212) and the
desktop's pairing dialog (#214) are separate steps. Until they land, the
loader is checked against an in-repository fake of the backend's side
(`validation/vr_pair_fake.py`, below).
```

## Deploying it

1. Copy the directory `web/vr-loader/` to any web server that serves static
   files over **HTTPS** (nginx, Apache, …), under any path, for example
   `https://vr.example.org/visivo/`. Everything in it uses relative
   addresses, and the service worker's scope is the directory itself.
2. Edit `config.json` in that directory:

   ```json
   { "pair_service": "wss://pair.example.org/visivo-pair" }
   ```

   `pair_service` is the pairing service's base address (the loader adds
   `/v1/join`). It may also be relative to `config.json`, for a service on
   the same host behind the same proxy, for example `"../pair"`. An `https://`
   address is read as `wss://`. Plain `ws://` is refused unless the loader
   itself runs on `localhost`.
3. Done. Moving to another server means copying the directory again and
   editing `config.json` there: there is no build step.

Serve `config.json` without long caching (the loader asks for it with
`no-store`, and its service worker never caches it). The other files can be
cached normally. The service worker revalidates them over the network first.

On the backends, `VISIVO_VR_PAGE_URL` names the loader's address (here
`https://vr.example.org/visivo/`), and `VISIVO_VR_PAIR_URL` the pairing
service. The desktop's pairing link is that address with `#pair=<code>`.

## What the user sees

- The loader asks for the code the desktop shows, or reads it from
  `#pair=CODE` in its address. It then removes the code from the address bar
  and the history.
- Once paired, the VR page fills the window. Its panel has no token field
  (the backend's bridge authenticates; the token never reaches the headset).
- *Copy diagnostics* names the transport, for example: `transport: WebRTC
  data channel via the pairing service pair.example.org; link up; ICE
  srflx/host udp; round trip 14 ms; 46 requests, 10.3 MB in`. With
  `relay` in the ICE line the link goes through a TURN server, which is slower.
- The loader can be installed as an app (Quest Browser's menu: *Install
  app*). The app starts at the code page.

### A dropped link

When the link drops (Wi-Fi, the computer asleep), the page shows *Backend not
reachable* and the loader reconnects by itself with the **resume code** the
backend gave at pairing, retrying for up to three minutes. No new code is
needed, the page is not reloaded, and the cube stays on show. Reloading the
loader's tab resumes as well (the resume code is kept for that tab only).
Requests that were under way when the link dropped and that change nothing
(`GET` and `HEAD`, such as the page's files, and the requests that load the
cube's reduced copy) are sent again once the link is back. A download that had begun continues where it stopped only when the
backend marks it as the same file (the same strong ETag and length, as the
page's own files are); otherwise it ends with a network error. The others
(a selection, a moment map) end with *Backend not reachable*, as they would
over HTTP.

When the resume fails (the code expired, or the backend restarted and forgot
it), or when the backend gave no resume code at pairing (the pairing service
did not answer in time), the page shows *The pairing with the computer
ended* with **Enter a new code**, which opens the loader's code form. The
same notice says *The computer disconnected this headset* when the desktop
disconnected it, and *VisIVO Visual Analytics on the computer was closed*
when the backend stopped: neither is resumed.

## How it works

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
- **Why a service worker.** The page's files are ES modules that import each
  other by relative address. The worker answers them from what the loader
  fetched from the backend, and keeps the versioned files (`vr/b/<build>/…`)
  in Cache Storage per build, so the page never mixes two builds.
- **The page's transport.** In the iframe the page sends its requests and
  WebSockets over the data channels instead of HTTP (`transport.js`;
  HTTP-over-channel framing in `pair-wire.js` and `pair-transport.js`, the
  same files shipped in the loader). Served directly by the backend
  (`localhost` through adb, a cluster's HTTPS portal), the page works exactly
  as before.
- `?pair_service=wss://…` overrides `config.json`, for development, and only
  when the loader runs on `localhost`, so a link cannot point a public loader
  at another service.

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
