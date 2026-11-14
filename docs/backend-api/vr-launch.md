# Headset app launch

How the desktop starts the VisIVO VR headset app (the VR page packaged as an
immersive Horizon OS app, `tools/quest-app/`) so that the user is in VR with no
browser window and no Enter VR press
([#255](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/255), after
the spike in [#254](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/254)).
Code: `backend/app/vr_launch.py`, `backend/app/routers/vr_launch.py`.

The launch carries a **one-time ticket**, never the token: Android keeps a
launch intent, fragment included, in `dumpsys activity` and the recents list
(measured on a Quest 3). The page trades the ticket for the token, the
desktop's session and the cube, once.

## `GET /.well-known/assetlinks.json`

Public (the headset's browser fetches it with no token, at every launch of
the app, and closes the app when the app is not listed). The Digital Asset
Links statements of:

1. the release app, `it.inaf.oact.visivo.vr`, with the certificates in
   `backend/app/vr_app.json` (`release_sha256`) and in
   `VISIVO_VR_APP_RELEASE_SHA256` (comma-separated);
2. the development app, `it.inaf.oact.visivo.vr.dev`, when this computer has
   a development key: the fingerprint in
   `<VISIVO_QUEST_APP_DIR or ~/.visivo/quest-app>/dev-sha256.txt`, written by
   `tools/quest-app/build.sh`;
3. the statements in the file `VISIVO_VR_ASSETLINKS_FILE` (a JSON array), as
   they are.

```json
[{ "relation": ["delegate_permission/common.handle_all_urls"],
   "target": { "namespace": "android_app", "package_name": "it.inaf.oact.visivo.vr",
               "sha256_cert_fingerprints": ["AB:CD:…"] } }]
```

404 when there is no statement (a backend without the app says nothing) or
when `VISIVO_VR_ASSETLINKS_FILE` names something that is not a JSON array of
objects (larger than 64 KiB, NaN, too deep). `Cache-Control: no-cache`.

## `POST /v1/vr/launch`

Token (`X-Visivo-Token`) and session (`X-Visivo-Session`). A ticket for ONE
launch of the app in the caller's session, and the cube it opens:

```json
{ "cube": { "dataset_id": "…", "path": "/data/ngc3137.fits", "kind": "cube",
            "box": [[0, 255], [0, 255], [10, 90]] } }
```

`cube` is optional (no body, `{}` or `null`: the app's connected empty room);
`kind` and `box` as in the [headset hub](headset-hub.md)'s `show`. Response
(`Cache-Control: no-store`):

```json
{ "ticket": "Qx7v…(43 characters)", "launch": "sBYiD_0jN5yb", "expires_in": 60.0,
  "page": "/vr/?launch=immersive" }
```

The launch address is `http://localhost:<port><prefix><page>#ticket=<ticket>`
(the backend's path prefix, if any, before `page`). The ticket is made of
`[A-Za-z0-9_-]` only. Never log it.

A ticket is single use, valid for `expires_in` seconds (60), and the backend
keeps only its SHA-256 and the SHA-256 of the caller's token; at most 32 wait
at a time (the oldest go). Tickets live in memory: a backend restart forgets
them.

## `POST /v1/vr/launch/exchange`

Public: the ticket is the credential. The page sends it in a JSON body, never
in the address:

```json
{ "ticket": "Qx7v…" }
```

Response, once (`Cache-Control: no-store`):

```json
{ "token": "…", "session": "…", "launch": "sBYiD_0jN5yb",
  "cube": { "dataset_id": "…", "path": "…", "kind": "cube", "box": null } }
```

- **404** for a ticket unknown, already used, expired, or whose token the
  backend no longer accepts (one answer for all). The first exchange uses the
  ticket up, whatever follows.
- **415** for a body that is not `application/json` (a form another page
  could post without a preflight).
- **413** above 1 KiB, **422** for a body without `ticket`.
- **429** (`Retry-After: 60`) beyond 20 exchanges a minute per client and 120
  in all.

## `GET /v1/vr/launch/{launch}`

Token and session: only the launch's owner (the same token and session) sees
it; 404 otherwise. How the launch went:

```json
{ "launch": "sBYiD_0jN5yb", "state": "exchanged", "expires_in": 0.0,
  "exchanged_at": 1794672444.92, "client": "vr-p67u1musb0sh" }
```

- `state`: `pending` (the ticket waits), `exchanged` (the page started and
  traded it), `expired` (never used in time, or dropped by the bound);
- `exchanged_at`: Unix seconds of the backend's clock, or null;
- `client`: the [headset hub](headset-hub.md) page whose hello named this
  launch (`launch` in `POST /v1/vr/headset/hello`) and is still connected:
  the page is up and takes `show` commands. Null until then.

Launches are remembered for 10 minutes.

## The page

Started as the app (`/vr/?launch=immersive`), the page takes `#ticket=` out of
the address at once, exchanges it and names the launch in its hub hello. It
enters VR at load, before the cube (the room says *Opening …* until the cube
is shown, *Connected to the computer* with no cube, *This launch has expired*
for a used ticket). Leaving VR quits the app, so the way out is labelled
**Quit VisIVO VR**. A page in the app's VR session never stands down for
another page; a second launch's page hands its grant to it instead (see the
user guide's [The headset app](../user-guide/vr/headset-app.md)).

## Measured on a Quest 3 (2026-11-14, USB, Quest Browser 157)

| Step after `am start` | App not running | App already in VR |
|---|---|---|
| ticket exchanged | 1.39–1.44 s | 0.47–0.52 s |
| hub hello with the launch | +0.1 s after | +0.1 s after |
| first immersive frame | 3.46 s (session request 1.87 s) | already in VR |

The cube (WALLABY, 196×280×109, binned 2×2×1) was ready before the session
started, so it was in the first frame; a larger cube shows *Opening …* in VR
until it is written into the room kept for it.
