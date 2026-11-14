# Headset hub

The headset pages of a backend session, what the desktop asks of them, and
what they report back
([#236](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/236), part of
[#234](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/234)). The
headset page (`/vr/`) and the desktop work in the same backend session (the
pairing link, the adb tab and the LAN page all carry the desktop's session).
Code: `backend/app/headset_hub.py`, `backend/app/routers/headset.py`.

Every route takes the token in `X-Visivo-Token` (over the pairing bridge, the
bridge adds it) and the session in `X-Visivo-Session`: only the clients of a
session see each other. No body may carry the token: a request whose label,
paths, summary or payload contains it is refused (422), so nothing stored here
or relayed to the other side can leak it. The routes work the same over HTTP,
through the pairing bridge's in-process ASGI path, and on a cluster backend.

**Order.** One counter (`seq`) numbers the commands and the events. It
restarts with the backend: every answer carries `instance` (the backend
process), and a cursor (`after`) above the last seq issued, one from a
backend that has since restarted, counts as 0. A client that sees another
`instance` resets its cursor.

**Bounds.** At most 8 pages per session (a new one drops the least recently
seen); 16 commands queued per page (the oldest goes); 256 events per session
and 1 MiB of their payloads (the oldest go); a payload of at most 16 KiB of
JSON (413 beyond), of finite numbers (422 for NaN or infinity), in a request
body of at most 64 KiB (413 beyond, counted before it is parsed); at most 8
waiting long polls of each kind per session (429 beyond) and one command
poll per page (a newer one ends the older, empty); a long poll waits at most
25 s. A page silent for 300 s is forgotten with its commands. The hub lives as
long as its session (`VISIVO_SESSION_TTL`).

**Shutdown.** Every long poll wakes at least once a second and ends, with
what it has, as soon as the server begins to exit (uvicorn's signal handler,
before uvicorn waits for the open requests) or the lifespan's shutdown runs.

## Presence

A page is **connected** (listed, and takes commands) while seen in the last
45 s: it is seen at its hello, its state, its activity, and throughout its
command poll (so a page that polls with `wait` up to 25 s stays listed).

### `POST /v1/vr/headset/hello`
The page announces itself at start and after any reconnect, and whenever a
hub route answers 404 for its client (the backend restarted or forgot it).
Its queued commands are kept across a reconnect under the same `client`.
```json
{ "client": "q3-7f2a", "transport": "pair", "label": "Quest 3" }
```
`client` matches `^[A-Za-z0-9_.:-]{1,64}$` (the page's own id, kept in its
storage); `transport` is `adb`, `pair` or `lan`; `label` (optional, at most 80
characters); `launch` (optional, `^[A-Za-z0-9_-]{8,32}$`): the
[headset app launch](vr-launch.md) the page came from, which
`GET /v1/vr/launch/{launch}` reports as its `client`; a later hello without
it keeps it. Response (`HeadsetHelloResponse`):
```json
{ "client": "q3-7f2a", "server_time": 1794700000.25, "instance": "9c1e…" }
```

### `POST /v1/vr/headset/state`
What the page shows, on every change. 404 for a client that did not say
hello.
```json
{ "client": "q3-7f2a", "in_vr": true,
  "cube": { "dataset_id": "…", "path": "/data/ngc3137.fits", "kind": "cube",
            "box": [[0, 255], [0, 255], [10, 90]] },
  "staged_cube": null }
```
`cube` and `staged_cube` (the cube the page holds to show next, #229) are
`HeadsetCube` objects or null: `dataset_id`, `path`, `kind` (default `cube`),
`box` (optional: inclusive voxel ranges of the full cube,
`[[x0, x1], [y0, y1], [z0, z1]]`, `0 <= lo <= hi`). Response as for hello.

### `GET /v1/vr/headset`
For the desktop: the connected pages, the newest hello first.
```json
{ "headsets": [ { "client": "q3-7f2a", "transport": "pair", "label": "Quest 3",
                  "since": 1794699000.0, "last_seen": 1794700000.2,
                  "in_vr": true, "cube": { … }, "staged_cube": null, "launch": null } ],
  "server_time": 1794700001.0, "instance": "9c1e…" }
```
`since` (first hello) and `last_seen` are Unix seconds of the backend's
clock, as `server_time`: compare them with it, not with the client's clock.

### `DELETE /v1/vr/headset?client=`
The page closes (best effort): no longer listed, its commands go, its waiting
poll ends. `{"removed": true|false}`.

## Commands (desktop to page)

### `POST /v1/vr/headset/command`
```json
{ "client": "q3-7f2a", "type": "show", "dataset_id": "…",
  "path": "/data/ngc3137.fits", "kind": "cube", "box": [[0, 255], [0, 255], [10, 90]] }
```
`client` is optional: without it, the newest connected page. `type` is
`show` (show this cube: `dataset_id` of this session; the page opens `path`
again when it does not hold that id); `kind` and `box` are optional and
passed on only when given. **404** when that page is not connected (the
desktop then opens one the usual way). A `show` replaces an older `show` the
page has not acknowledged. Response: `{"seq": 41, "client": "q3-7f2a"}`.

### `GET /v1/vr/headset/commands?client=&after=&wait=`
The page's long poll. `after` acknowledges every command up to it (they are
dropped); the answer holds the commands above it, waiting up to `wait`
seconds (0 to 25) for one:
```json
{ "commands": [ { "seq": 41, "type": "show", "dataset_id": "…", "path": "…",
                  "kind": "cube", "box": [[0, 255], [0, 255], [10, 90]] } ],
  "next": 41, "instance": "9c1e…" }
```
Pass `next` as the next `after` once the commands are applied: a command is
handed again until acknowledged. 404 for a client that did not say hello.

## Activity (page to desktop)

### `POST /v1/vr/activity`
```json
{ "client": "q3-7f2a", "kind": "product", "cube_path": "/data/ngc3137.fits",
  "summary": "Moment 0 of the selection",
  "payload": { "product": "moment", "file": "ngc3137_m0.fits",
               "request": { "moment_order": 0, "…": "…" }, "recipe": { … } } }
```
`kind`:

- `look`: colormap, threshold, colour max, opacity, gamma, render mode,
  stretch; the latest per cube replaces the previous;
- `clip`: `payload.box`, the clip box in voxels of the full cube, or null; the
  latest per cube replaces the previous;
- `product`: `payload.product` is `moment`, `pv`, `spectrum`, `isolate`,
  `remove`, `catalogue` or `flow`, with what the desktop needs to open it: a
  `file` the backend wrote (a Workspace Exports filename, which must exist;
  the backend adds its `output_path`; a payload that sets `output_path`
  itself, or a file that leads out of the exports directory through a
  symlink, is 422) or a `dataset_id`, the `request` under the API's names,
  the `recipe` record;
- `cube`: the page switched cube (`payload.path`, `payload.binning`).

`summary` (at most 300 characters) is what the desktop shows. 404 for a
client that did not say hello. Response: `{"seq": 57}`.

### `GET /v1/vr/activity?after=&wait=`
The desktop's long poll: the events above `after`, oldest first, waiting up
to `wait` seconds (0 to 25) for one.
```json
{ "events": [ { "seq": 57, "client": "q3-7f2a", "time": 1794700002.1,
                "kind": "product", "cube_path": "/data/ngc3137.fits",
                "summary": "…", "payload": { … } } ],
  "next": 57, "lost": false, "instance": "9c1e…" }
```
`lost` is true when events above `after` (not 0) were dropped by the bounds
before they were read. A `look` or `clip` replaced by a later one of the same
cube is not lost: only the latest matters, and it carries a higher `seq`.

## Saving what the headset makes

The page computes moment maps, PV slices and spectra in memory. To hand one to
the desktop it saves it in the Workspace Exports with the desktop's routes,
then posts a `product` event naming the file:

- a moment map: [`POST /v1/products/moment_fits`](products.md#post-v1productsmoment_fits),
  with the request of `/v1/products/moment` (for a selection, on the
  isolated cube's dataset id);
- a PV slice: [`POST /v1/products/pv_fits`](products.md#post-v1productspv_fits),
  with the vertices and width of `/v1/cube/pv`;
- a spectrum: [`POST /v1/products/spectrum_table`](products.md#post-v1productsspectrum_table),
  with the pixel of `/v1/spectral/probe` (or the aperture of
  `/v1/spectral/aperture`);
- an isolated or removed cube: [`POST /v1/cube/mask_apply_bin`](cube.md#post-v1cubemask_apply_bin)
  already writes it there.
