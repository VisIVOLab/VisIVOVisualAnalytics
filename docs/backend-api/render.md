# Server-side rendering

The render routes let a client view a 3-D cube rendered on the backend's GPU (VTK with an EGL context) instead of loading the cube locally. A client creates a render session for a cube it has already opened, then drives the camera and the rendering options over a WebSocket and receives JPEG frames back. Where the backend was built with WebRTC support, the same session can also be streamed as video over a WebRTC peer connection.

The router is only mounted when the backend can import its render modules. On a backend built without EGL the routes do not exist at all, and `POST /v1/render/create_session` answers 503 when the import succeeded but no EGL render window is available.

## Authentication

The router is mounted without a router-level auth dependency, because the token dependency used elsewhere cannot run on a WebSocket. Authentication is applied per route instead:

- Every HTTP route requires the `X-Visivo-Token` header. A missing or wrong token gives 401 (`{"detail": "Missing or invalid X-Visivo-Token header."}`).
- The WebSocket checks the token itself, from the `token` query parameter or the `X-Visivo-Token` header (see [WebSocket](#websocket) below).

Every render session is bound to the token that created it. Another token gets 404 for that session on every route (the same answer as an unknown id, so session ids cannot be probed). A session with no owner is reachable by any token only on a single-token backend.

## Session lifecycle

### `POST /v1/render/create_session`

Creates a render session for a dataset already opened in the caller's VisIVO session, renders a first frame and returns it.

The dataset path is looked up from the VisIVO session (the `X-Visivo-Session` header, as for the other dataset routes), never taken from the request, so the data-root restrictions still apply. When several tokens are configured the `X-Visivo-Session` header is mandatory (400 without it).

Request body:

| Field | Type | Description |
|---|---|---|
| `dataset_id` | string | Id returned when the dataset was opened. Required. |

Response:

| Field | Type | Description |
|---|---|---|
| `valid` | bool | `true` on success. |
| `error` | string | Empty on success. |
| `session_id` | string | The render session id (a 32-character hex UUID). |
| `initial_frame_jpeg_b64` | string | The first frame, JPEG, base64-encoded. |
| `width`, `height` | int | Size of the render window, in pixels (1920 x 1080 until the client sends `resize`). |

```json
{"dataset_id": "c0ffee"}
```

```json
{"valid": true, "error": "", "session_id": "9f1c...", "initial_frame_jpeg_b64": "/9j/4AAQ...", "width": 1920, "height": 1080}
```

Errors:

- 401: missing or invalid token.
- 404: unknown `dataset_id` in the caller's session.
- 422: the dataset is not a cube (only cube datasets can be rendered on the server).
- 503: server-side rendering is not available on this backend, or the session cap is reached (`"Server-side rendering capacity reached (N sessions active)."`).

The cap is `VISIVO_RENDER_MAX_SESSIONS` (default 8) sessions per backend. A session that receives no command for `VISIVO_RENDER_IDLE_TTL_SECONDS` (default 300) is closed by a reaper that runs every 30 s; a client that keeps a still view open sends `keepalive` to stay alive. Closing the WebSocket does not close the session, so a client can reconnect.

### `DELETE /v1/render/{session_id}`

Closes a render session and releases its GPU resources.

Response: `{"closed": true}`.

Errors: 401 (token), 404 (`"Unknown render session_id."`, also for a session owned by another token).

### `GET /v1/render/diagnostics`

A snapshot of the caller's render sessions and of the transports this backend serves. Only the sessions owned by the caller's token are listed (unowned sessions too on a single-token backend).

Response:

| Field | Type | Description |
|---|---|---|
| `active_sessions` | int | Number of sessions in the listing. |
| `capacity` | int | The session cap (`VISIVO_RENDER_MAX_SESSIONS`). |
| `idle_ttl_seconds` | float | The idle timeout (`VISIVO_RENDER_IDLE_TTL_SECONDS`). |
| `sessions` | array | One object per session, see below. |
| `transports` | array of string | `"jpeg-ws"` and `"jpeg-ws-push"` always; `"h265-webrtc"` added when the WebRTC module imports and its encoder probe succeeds. |

Each element of `sessions`: `session_id`, `dataset_path`, `frames_rendered`, `bytes_sent`, `commands_received`, `last_render_ms`, `last_jpeg_kb`, `age_seconds`, `idle_seconds`.

```json
{
  "active_sessions": 1,
  "capacity": 8,
  "idle_ttl_seconds": 300.0,
  "sessions": [{"session_id": "9f1c...", "dataset_path": "/data/cube.fits",
                "frames_rendered": 412, "bytes_sent": 51234567, "commands_received": 980,
                "last_render_ms": 21.4, "last_jpeg_kb": 96.2,
                "age_seconds": 183.0, "idle_seconds": 1.2}],
  "transports": ["jpeg-ws", "jpeg-ws-push"]
}
```

Errors: 401 (token).

## WebRTC signalling

These two routes exist on every backend that mounts the render router, but they only work where the optional WebRTC module (aiortc) is installed. The video track carries H.264, encoded with NVENC (`h264_nvenc`) when the encoder probe succeeds and with aiortc's software encoder otherwise. Note that the transport identifier advertised by `/v1/render/diagnostics` is `h265-webrtc`.

Camera and rendering commands travel over an `RTCDataChannel` labelled `commands` that the client opens before posting its offer. It accepts the same JSON commands as the WebSocket (see [the command table](#client-commands)); `request_frame` is ignored there, since the video track streams frames continuously, and invalid JSON is logged and dropped.

### `POST /v1/render/{session_id}/sdp/offer`

Posts the client's SDP offer and returns the server's SDP answer. Each call creates a new peer connection, so several clients can watch the same render session.

Request body and response share one shape:

| Field | Type | Description |
|---|---|---|
| `sdp` | string | The session description. |
| `type` | string | `"offer"` in the request, `"answer"` in the response. |

Errors:

- 401: token.
- 404: unknown render session (or owned by another token).
- 500: `"WebRTC negotiation failed: <ExceptionType>: <message>"`.
- 503: `"WebRTC transport not built in this backend."` (module not installed) or `"WebRTC transport disabled (no NVENC)."` (encoder probe failed).

### `POST /v1/render/{session_id}/sdp/ice`

Adds one trickled ICE candidate to the session's active peer connection. The body mirrors the browser's `RTCIceCandidateInit`.

| Field | Type | Description |
|---|---|---|
| `candidate` | string | The candidate line, with or without the `candidate:` prefix. Required. |
| `sdpMid` | string or null | Media section id. Default `null`. |
| `sdpMLineIndex` | int or null | Media section index. Default `null`. |

Response: `{"added": true}`.

Errors:

- 400: `"Bad ICE candidate: ..."` (the candidate could not be parsed or added).
- 401: token.
- 404: unknown render session (or owned by another token).
- 410: no active peer connection for this session (no offer was accepted, or the connection has closed).
- 503: WebRTC module not installed.

## WebSocket

### `WebSocket /v1/render/{session_id}/ws`

The interactive channel of a render session: JSON commands from the client, JPEG frames from the server.

**Authentication.** The token is read from the `token` query parameter, or from the `X-Visivo-Token` header when the transport forwards it. Every configured token is accepted. Before the connection is accepted the server closes it with:

- code 1008 (`"invalid token"`) when no token matches;
- code 1011 (`"unknown session_id"`) when the session does not exist or belongs to another token.

```text
wss://node.example.org/v1/render/9f1c.../ws?token=<token>
```

**Messages.** The client sends JSON text frames, one command per frame, each with a `type` field. The server sends:

- binary frames: one JPEG image each;
- text frames: JSON, namely `{"error": "invalid JSON"}` for a frame that is not valid JSON, and the `{"stream": "on", ...}` reply to `stream`.

Commands do not render by themselves. In the default (pull) mode a frame is rendered and sent only in answer to `request_frame`, so a client typically sends its camera commands and then one `request_frame` per display tick. In push mode (after `stream`) the server sends frames on its own, see [Push mode](#push-mode).

The server closes the connection with code 1011 on an unexpected server error (`"server error"`) or when the push stream fails to render or send (`"render failed"`).

#### Client commands

Commands handled by the WebSocket handler itself:

| `type` | Fields | Effect |
|---|---|---|
| `request_frame` | none | Pull mode: renders the current view and sends it as a JPEG. Push mode: only wakes the push loop; nothing is sent if the scene has not changed. |
| `stream` | `window` (int, 1 to 8, default 3), `max_fps` (number, 1 to 60, default 30) | Switches the connection to push mode. Values outside the ranges are clamped; a value that is not a number leaves the default. A second `stream` on the same connection is ignored. |
| `frame_ack` | none | Push mode: acknowledges one received frame, freeing one place in the window. Never counts below zero. |
| `keepalive` | none | Resets the idle timer, changes nothing else. |
| `close` | none | Ends the WebSocket loop (the render session stays open; use `DELETE` to close it). |

Every other command is applied to the session's view. Each one counts as activity for the idle timer and marks the scene as changed. Unknown types are logged and ignored. Note that `request_frame`, `stream` and `frame_ack` do not reset the idle timer: a client that only watches a still view must send `keepalive` within the idle timeout. The desktop sends one every 60 s while a remote view is connected; a keepalive does not render or push a frame.

| `type` | Fields | Effect |
|---|---|---|
| `rotate` | `d_azimuth`, `d_elevation` (degrees, default 0) | Rotates the camera by these increments. |
| `zoom` | `factor` (default 1.0, clamped to 0.05 to 20) | Zooms the camera by this factor. |
| `pan` | `dx`, `dy` (default 0) | Accumulates a pan offset in the view state. |
| `reset` | none | Ends a drag in progress, restores the default view state (keeping the window size; threshold at 3 % of the data range above the minimum, cutting plane on the middle channel) and resets the camera to fit the cube. |
| `set_lut` | `name` (string) | Colour map of the volume (and of the isosurface). Unknown names fall back to Inferno. `colormap` is an accepted alias. |
| `set_slice_lut` | `name` (string) | Colour map of the cutting-plane slice. |
| `set_threshold` | `value` (number) | Lower anchor of the opacity transfer function, also the isosurface value. |
| `set_blend_mode` | `mode` (int: 0 composite, 1 maximum intensity, 2 minimum intensity; default 0) | Ray-casting blend mode. |
| `set_cube_render_mode` | `mode` (`"volume"` or `"isosurface"`, default `"volume"`) | Volume rendering or isosurface. Any other value means `"volume"`. |
| `set_interactive` | `value` (bool, default `false`) | `true` when a drag starts, `false` when it ends: switches the drag level of detail on or off (see [Server side](#server-side)). |
| `set_cutting_plane` | any subset of `visible` (bool), `opacity` (0 to 1), `slice_index` (int) | Shows or hides the textured cutting plane, sets its opacity and the channel it shows. |
| `set_show_outline` | `visible` (bool, default `true`) | Bounding-box wireframe. |
| `set_show_axes` | `visible` (bool, default `true`) | Orientation axes. |
| `set_show_rendering` | `visible` (bool, default `true`) | Shows or hides the volume or isosurface itself (outline and cutting plane stay). |
| `resize` | `width`, `height` (pixels, each clamped to 64 to 4096) | Render window size. During a drag the new size is applied when the drag ends. |
| `jpeg_quality` | `quality` (1 to 100; the session default is 95) | JPEG quality of the frames. During a drag frames are encoded at no more than 60. |
| `opacity` | `scale` (0 to 1, default 0.5) | Legacy: stored, has no visible effect; use `set_threshold`. |

```json
{"type": "set_interactive", "value": true}
{"type": "rotate", "d_azimuth": 4.5, "d_elevation": -1.0}
{"type": "request_frame"}
```

#### Push mode

In push mode the client no longer asks for each frame: the server renders and sends a JPEG whenever the scene changes, with flow control by acknowledgement.

```json
{"type": "stream", "window": 3, "max_fps": 30}
```

The server answers with the values it will use, then sends the current view as the first frame:

```json
{"stream": "on", "window": 3, "max_fps": 30.0}
```

From then on:

- A frame is sent when the scene changed since the last frame sent: after any command, and also while a large cube is still loading in the background (the push loop checks for newly loaded data every 0.25 s even when no command arrives).
- At most `window` frames are unacknowledged at any time. The client sends `{"type": "frame_ack"}` for every frame it receives; when the window is full the server waits, and renders nothing it could not send.
- Frames are spaced at least `1 / max_fps` seconds apart.
- While one frame is being sent, the next one is already rendering if the view has moved on and the window has room.
- `request_frame` sends nothing by itself in this mode.

Push mode lasts until the connection closes.

## Server side

Each render session renders in its own child process, so a render or a JPEG encode does not block the backend's event loop and a crash in VTK or the GPU driver ends only that session (`VISIVO_RENDER_IN_PROCESS=1` keeps sessions in the backend process instead). Sessions are spread over the GPUs the job may use, one GPU per session in turn. During a drag (`set_interactive` `true`) the render window is halved in each dimension, the cutting plane and the axes are hidden and the outline is drawn bright; a volume is ray-cast with fewer rays and fewer samples per ray, at one of four levels (image sample distance and sample-distance factor 2, 3, 4 and 6), and an isosurface is replaced by a low-polygon wireframe built from the cube shrunk four times. The volume level adapts to the measured render and readback time of each drag frame: one level coarser when a frame takes more than 30 ms (1.2 times a 25 ms target), one level finer when the finer level is expected to fit the 25 ms target, and the level reached is kept for the next drag. Releasing the drag restores the full window size and full sampling. For running the backend with GPU rendering on a cluster, see [HPC deployment with SLURM](../deployment/hpc-slurm.md).
