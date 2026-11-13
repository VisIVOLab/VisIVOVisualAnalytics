# Headset pairing by code

These routes let the desktop pair a headset's VR page with this backend
through a short code
([#212](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/212), part of
[#210](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/210)). The
page is a public HTTPS page. It cannot call a backend on `http://` on the
local network, but it can open a WebRTC connection to it. The backend
connects out to a pairing service, which carries the WebRTC offer and answer.
After that the page and the backend talk to each other directly, and the
page's API requests and WebSockets reach this backend's own routes over
WebRTC data channels. The protocol is `visivo-pair/1`. The pairing service,
its protocol and its deployment are described in
[`services/vr-pair/README.md`](https://github.com/VisIVOLab/VisIVOVisualAnalytics/blob/main/services/vr-pair/README.md).

The routes are in `backend/app/routers/vr_pair.py`. The connection to the
service is in `app/vr_pair_host.py` and the data-channel bridge in
`app/vr_bridge.py`.

## Configuration

The backend pairs headsets when `VISIVO_VR_PAIR_URL` names a pairing service
and `VISIVO_VR_PAGE_URL` the public loader; both, and what pairing needs
(aiortc and PyAV from `backend/requirements-webrtc.txt`, macOS 14 or newer on
Apple Silicon), are in
[Configuration: VR headset pairing](../configuration.md#vr-headset-pairing).
Running the service and the loader is in
[Hosting headset pairing](../deployment/vr-pairing.md). A backend without
pairing answers 409 `not-configured` on these routes, and every other route
works as before.

## `POST /v1/vr/pair`

```json
{"session": "…", "dataset": "…", "path": "/data/cube.fits", "kind": "velocity", "box": 125}
```

The request creates a pairing code for the headset. `session` is required.
`dataset` and `path` are optional. Any other field is a page parameter, as
the desktop's `#…` link carries it today (`kind` and `box` for point tables,
catalogues and velocity fields). Those fields have the following limits:

- names of the form `[a-z][a-z0-9_]*`;
- at most 16 of them;
- each one a string, a number or a boolean;
- `token` is refused with 422.

The parameters stay on this backend. The page receives them in `welcome`.

```json
{"code": "K7Q4MX", "expires_at": 1762963200, "page_url": "https://vr.example.org/#pair=K7Q4MX"}
```

- A code is good for one join and for 5 minutes, unless the service says
  otherwise.
- `expires_at` is in Unix seconds.
- `page_url` is `null` when `VISIVO_VR_PAGE_URL` is unset.

The route answers 409 `{"detail", "reason"}` in two cases:

- `not-configured`: `VISIVO_VR_PAIR_URL` is unset or is not a `ws(s)://`
  address, or this backend has no WebRTC support;
- `unreachable`: the service did not answer within 5 seconds. The backend
  keeps trying in the background, with backoff up to a minute.

## `GET /v1/vr/pair`

```json
{"enabled": true,
 "pending": [{"code": "K7Q4MX", "expires_at": 1762963200}],
 "connected": [{"id": "3f9a01c2b7e4", "since": 1762962950, "dataset": "…", "transport": "webrtc-direct"}]}
```

- `enabled` is false when pairing is not configured or not available.
- `transport` is `webrtc-direct` for a direct path, typically on the local
  network, and `webrtc-relay` through a TURN server.
- With per-user tokens, each token sees only its own codes and headsets.

## `DELETE /v1/vr/pair/{code_or_id}`

This revokes a pending code (`{"result": "revoked"}`) or disconnects a
headset by its `id` (`{"result": "disconnected"}`). A disconnected headset:

- receives `bye` and is closed;
- loses its resume code.

An unknown name answers 404.

## What a paired page gets

The page creates three kinds of data channel.

**`ctl`.** The page sends `{"t":"hello","resume":<code|null>}`. The backend
answers:

```json
{"t": "welcome", "session": "…", "dataset": "…", "path": "…",
 "params": {"session": "…", "dataset": "…", "path": "…", "kind": "velocity", "box": 125},
 "page_build": "c09cce3dcfba", "resume_code": "…", "resume_expires_in": 43195}
```

The resume code works for 12 hours. A page whose connection dropped joins
again with it, without a new code from the desktop, and finds the same
session, dataset and parameters. When the backend disconnects the headset it
sends `{"t":"bye","reason"}`. The `reason` is `disconnected` (from the
desktop) or `backend-stopping`.

**`api`.** HTTP requests:

- **Control messages.** These are JSON text frames: `req {id, method, path,
  headers, body}`, `res {id, status, headers}`, `cancel {id}` and `err {id,
  message}`. The `path` is relative to the backend root, with its query
  string.
- **Headers.** Response `headers` are a JSON object, the names in lower
  case and repeated headers joined with `, `. Request headers may be an
  object or a list of `[name, value]` pairs. The backend drops the page's
  `X-Visivo-Token` and adds its own.
- **Bodies.** A body travels as binary chunks: a 4-byte big-endian id, a
  flag byte (bit 0 = last) and at most 16 KiB of payload.
- **Dispatch.** Every request is served by this backend's own app,
  in-process. The response streams back as the route produces it. Its next
  piece is not read while the channel holds more than 1 MiB, so a large
  response does not accumulate in memory.
- **Cancel.** A `cancel` stops the route.
- **Limits.** At most 64 requests can be in flight per channel. A request
  body can be at most 64 MiB, and request bodies that the routes have not
  read yet can hold at most 64 MiB per channel. A request over either limit
  is cancelled (`cancel`, then `err`).
- **Pairing routes.** `/v1/vr/pair` is not reachable through the channel:
  403.
- **Decoded bodies.** The page always receives the decoded bytes. The
  bridge drops the page's `Accept-Encoding`. If a route encodes its body
  anyway (gzip or deflate), the bridge decodes it as it streams and drops
  `content-encoding` and `content-length`. Otherwise `content-length` is the
  length of the bytes that follow.

**`ws`.** There is one channel per page WebSocket. Its `protocol` is
`{"path", "protocols"}`, for example `/v1/render/<id>/ws` with
`["visivo.v1"]`.

- **Frames.** Every frame is binary: byte 0 is the type (`0x01` text,
  `0x02` binary, `0x03` open `{"protocol"}`, `0x04` close
  `{"code","reason"}`), byte 1 the flags (bit 0 = last fragment), then at
  most 16 KiB.
- **Fragmentation.** A message larger than 16 KiB is split into frames of
  its type, and only the last one is flagged. A stereo frame of several MB
  therefore crosses intact, under the 256 KiB message limit of Chromium-based
  browsers.
- **Refusal.** A socket the route refuses (an unknown render session, for
  example) is closed with the route's own code.
- **Closing.** When the page sends a close, the bridge echoes it, as a
  WebSocket server does, and the route sees the disconnect.
- **Unread messages.** Messages that the route has not read yet can hold at
  most 64 MiB. Beyond that the socket is closed with 1009.
- **Stereo video.** The stereo video of
  [#189](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/189)
  negotiates its peer connection over the bridged render socket, as it does
  over a plain WebSocket.

**Authentication.** The backend adds the token of the desktop that issued
the code to every bridged request and socket, and the page never sees that
token. A token or `visivo-token.*` subprotocol sent by the page is removed.

## Measured

These figures come from `backend/tests/test_vr_pair.py`, with both peers in
one process on a MacBook over loopback:

| Measurement | Result |
|---|---|
| Time from join to `welcome` | about 80 ms |
| Time from join to the first API byte | about 80 ms |
| A 100 MiB streamed response | 6 s (17 MiB/s) |
| Python memory traced during that transfer | 2.1 MiB at peak |
| The VR page's 8.4 MB reduced cube, from Chrome on the same Mac (median of 7 runs) | 0.45 to 0.51 s (16 to 19 MB/s) paired, 0.28 to 0.32 s direct over HTTP |

The throughput is limited by aiortc's SCTP, which is pure Python, and here
both ends share one CPU core. Between a headset and a Mac on the local
network the receiving end is the browser's.
