# Server-side rendering

The render routes let a client view a 3-D cube rendered on the backend's GPU (VTK with an EGL context on Linux, a hidden Cocoa window on macOS) instead of loading the cube locally. A client creates a render session for a cube it has already opened, then drives the camera and the rendering options over a WebSocket and receives JPEG frames back. Where the backend was built with WebRTC support, the same session can also be streamed as video over a WebRTC peer connection.

The router is only mounted when the backend can import its render modules. On a backend built without them the routes do not exist at all, and `POST /v1/render/create_session` answers 503 when the import succeeded but no offscreen render window is available: no EGL in the VTK build on Linux, or, on macOS, no window server session (the backend was started over ssh with nobody logged in at the Mac, or as a LaunchDaemon). On macOS the window is a `vtkCocoaRenderWindow` drawn offscreen and never shown, on the Mac's GPU (Apple's OpenGL 4.1 over Metal); the render process does not appear in the Dock.

## Authentication

The router is mounted without a router-level auth dependency, because the token dependency used elsewhere cannot run on a WebSocket. Authentication is applied per route instead:

- Every HTTP route requires the `X-Visivo-Token` header. A missing or wrong token gives 401 (`{"detail": "Missing or invalid X-Visivo-Token header."}`).
- The WebSocket checks the token itself, from the `token` query parameter or the `X-Visivo-Token` header (see [WebSocket](#websocket) below).

Every render session is bound to the token that created it. Another token gets 404 for that session on every route (the same answer as an unknown id, so session ids cannot be probed). A session with no owner is reachable by any token only on a single-token backend.

## Session lifecycle

### `POST /v1/render/create_session`

Creates a render session for a dataset already opened in the caller's VisIVO session, renders a first frame and returns it. The dataset is a cube (FITS, or a field of a VBT volume), or a VBT **point table** ([Point tables](#point-tables)).

The dataset path is looked up from the VisIVO session (the `X-Visivo-Session` header, as for the other dataset routes), never taken from the request, so the data-root restrictions still apply. When several tokens are configured the `X-Visivo-Session` header is mandatory (400 without it).

Request body:

| Field | Type | Description |
|---|---|---|
| `dataset_id` | string | Id returned when the dataset was opened. Required. |
| `queue` | bool | `true`: when every slot is busy, wait in the [queue](#the-queue) (202 with a place) instead of being refused (503). Default `false`. |
| `queue_id` | string or null | A place given by an earlier 202: asks again for it (implies `queue`). Default `null`. |

Response:

| Field | Type | Description |
|---|---|---|
| `valid` | bool | `true` on success. |
| `error` | string | Empty on success. |
| `session_id` | string | The render session id (a 32-character hex UUID). |
| `initial_frame_jpeg_b64` | string | The first frame, JPEG, base64-encoded. |
| `width`, `height` | int | Size of the render window, in pixels (1920 x 1080 until the client sends `resize`). |
| `threshold` | float or null | The threshold the session renders with, until a `set_threshold` changes it: the cube's default threshold from its noise ([issue #93](../user-guide/cube-viewer.md#the-default-threshold): about 100 noise voxels of the whole cube above it), from a regular sample of its voxels (out of core, from a sample of full-resolution bricks). A client shows it rather than sending a default of its own. |
| `data_min`, `data_max` | float or null | The cube's data range, at full resolution. |
| `levels_pending` | bool | `true` while a cube read in the background (above 256 MB, in core) is still arriving: `threshold` and the range are null until the volume is shown (after a quarter of the cube, with the threshold from the part loaded), and change once more when the cube is complete (the threshold from all of it), unless a `set_threshold` came first; a `set_threshold` is kept, even 0. Ask [`GET /v1/render/{session_id}/levels`](#get-v1rendersession_idlevels) until it is `false`. |

A cube larger than `VISIVO_RENDER_OUT_OF_CORE_BYTES` is drawn from its brick pyramid ([Brick pyramid](../brick-pyramid.md#rendering-from-the-pyramid)). When the pyramid is missing or out of date, the backend starts building it in a separate process and answers **202** without creating a session:

| Field | Type | Description |
|---|---|---|
| `valid` | bool | `false`. |
| `preparing` | bool | `true`. |
| `progress` | float | Fraction of the build done, 0 to 1. |
| `error` | string | A sentence to show while waiting. |

Ask again (the desktop does every 2 s); every request for the same cube joins the same build, also from another backend sharing `VISIVO_PYRAMID_DIR`. Once the pyramid is ready the answer is the session above. If the build failed, the answer is 500 with `detail` starting "Preparing the cube for full resolution failed:" and the reason; it is not retried for a minute.

```json
{"dataset_id": "c0ffee"}
```

```json
{"valid": true, "error": "", "session_id": "9f1c...", "initial_frame_jpeg_b64": "/9j/4AAQ...", "width": 1920, "height": 1080,
 "threshold": 2.31, "data_min": -5.02, "data_max": 48.7, "levels_pending": false}
```

Errors:

- 401: missing or invalid token.
- 404: unknown `dataset_id` in the caller's session.
- 410: `queue_id` is no longer in the queue (see [The queue](#the-queue)).
- 422: the dataset is neither a cube nor a VBT point table (`"Server-side rendering supports cubes and VBT point tables."`), or a catalogue without RA/Dec or X/Y/Z columns (`"Drawing a catalogue in 3-D needs RA/Dec or X/Y/Z columns."`).
- 503: server-side rendering is not available on this backend; or every slot is busy and the request has no queue place: it did not ask for one, or the queue is off (`"Server-side rendering capacity reached (N sessions active)."`), the queue is full (`"... (N sessions active, M requests waiting)."`), or it waited past the timeout (`"Server-side rendering capacity reached: waited T s in the queue without a free slot."`).

The cap is `VISIVO_RENDER_MAX_SESSIONS` (default 8) sessions per backend, cubes and point tables together. A session that receives no command for `VISIVO_RENDER_IDLE_TTL_SECONDS` (default 300) is closed by a reaper that runs every 5 s; a client that keeps a still view open sends `keepalive` to stay alive. Closing the WebSocket does not close the session, so a client can reconnect, except when requests are waiting (see below). A session whose render process ends (a crash in VTK or the GPU driver, a kill) is removed at once and its slot freed.

#### The queue

A request with `queue: true` that finds every slot busy waits instead of being refused. The answer is **202** with a place in the queue and a `Retry-After` header (seconds):

| Field | Type | Description |
|---|---|---|
| `valid` | bool | `false`. |
| `queued` | bool | `true` (a pyramid build answers 202 with `preparing` instead). |
| `queue_id` | string | The place: send it back as `queue_id` to ask again. |
| `position` | int | 1 for the next request to be served. (In the `queue` of `/v1/render/diagnostics`, 0 means a slot is already reserved for it: its next request starts the session, so a 202 never says 0.) |
| `queue_length` | int | Requests waiting without a slot. |
| `active_sessions`, `capacity` | int | Sessions running on the backend, and the cap. |
| `waited_seconds` | float | Time since the place was given. |
| `wait_timeout_seconds` | float | After this long without a slot the request is refused (503). |
| `poll_after_seconds` | float | When to ask again (2 s; the `Retry-After` header, rounded up to at least 1). |
| `expires_after_seconds` | float | A place not asked for within this long is dropped (the client is taken to be gone). |
| `error` | string | A sentence to show, such as `"All 8 server-side rendering slots are busy: number 2 of 3 in the queue."`. |

```json
{"valid": false, "queued": true, "queue_id": "5be1...", "position": 2, "queue_length": 3,
 "active_sessions": 8, "capacity": 8, "waited_seconds": 12.4, "wait_timeout_seconds": 600.0,
 "poll_after_seconds": 2.0, "expires_after_seconds": 15.0,
 "error": "All 8 server-side rendering slots are busy: number 2 of 3 in the queue."}
```

The client asks again with the same request plus `queue_id` every `poll_after_seconds`. Each 202 updates the position; when its turn comes the request is answered like any `create_session` (200 with the session and its first frame). There is no push channel for the position: the request itself is how the client says it is still waiting, so a client that dies or loses the network simply stops asking and its place, or the slot reserved for it, is passed on after `expires_after_seconds`. A client that stops waiting (the view closed, the user went back to local rendering) gives its place up with [`DELETE /v1/render/queue/{queue_id}`](#delete-v1renderqueuequeue_id).

A `queue_id` is bound to the token and the dataset it was given for: asked for with another token or dataset, or after it expired, was cancelled or was used, it answers **410** (`"This request is no longer in the rendering queue. Ask again without queue_id."`); the client then asks without `queue_id` and gets a new place at the end of the queue.

Order: a free slot goes to the waiting request whose token holds the fewest render sessions (running or reserved), ties in order of arrival. With one token, as on the desktop's local backend or a cluster job of one user, this is first come, first served. On a backend shared by several tokens a user with no view is served before a further view of a user who already has some, so a position can grow by one when such a request arrives. A new request never takes a slot reserved for a waiting one.

While requests wait, a session no client is connected to (no WebSocket open, no WebRTC peer) for `VISIVO_RENDER_DISCONNECT_GRACE_SECONDS` (default 30) is closed and its slot passed on, the longest unattended first and only as many as are needed. The grace counts from the last WebSocket closing, or from the last reaper pass (every 5 s) that saw a WebRTC peer, or, for a session nobody connected to, from the end of its start. A request that waited past `wait_timeout_seconds` is given no slot. With nobody waiting only the idle timeout applies, so a view switched to local rendering keeps its session for a quick switch back. A client that reconnects to such a closed session has its WebSocket handshake refused (see [WebSocket](#websocket)) and should create a new session.

| Variable | Default | Meaning |
|---|---|---|
| `VISIVO_RENDER_QUEUE_MAX` | `16` | Requests that may wait; beyond it 503. `0` turns the queue off (always 503 at the cap). |
| `VISIVO_RENDER_QUEUE_TIMEOUT_SECONDS` | `600` | Longest wait for a slot; then 503. |
| `VISIVO_RENDER_QUEUE_STALE_SECONDS` | `15` | A place not asked for this long is dropped (never less than 6 s, three polls). |
| `VISIVO_RENDER_DISCONNECT_GRACE_SECONDS` | `30` | While requests wait, a session with no client connected this long gives up its slot. |

### `DELETE /v1/render/queue/{queue_id}`

Gives up a place in the queue (or the slot reserved for it).

Response: `{"cancelled": true}`.

Errors: 401 (token), 404 (`"Unknown queue_id."`, also for another token's place).

### `GET /v1/render/{session_id}/levels`

The session's current threshold and data range, as in the answer of `create_session`: `threshold`, `data_min`, `data_max` and `pending` (the `levels_pending` of that answer). The desktop asks every 2 s while `pending` is `true`, and shows the values in the Threshold field and slider unless the user has set a threshold.

```json
{"threshold": 2.31, "data_min": -5.02, "data_max": 48.7, "pending": false}
```

Errors: 401 (token), 404 (`"Unknown render session_id."`, also for a session owned by another token).

### `GET /v1/render/{session_id}/camera`

A session's camera (points or a cube), after the commands sent before the
request: `{position, focal_point, view_up, view_angle, parallel,
parallel_scale}`, a cube's in its voxel coordinates. A ray-traced still of the
view on screen is rendered from it ([particles](particle-render.md),
[volumes](volume-render.md)). (`/levels` does not return it: its answer has only
the four fields above.) 404 for an unknown session (`"Unknown render
session_id."`), 409 for a session with no camera to report (`"This render
session has no camera to report."`).

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
| `queued` | int | Requests waiting in the whole backend (reserved slots included). |
| `queue_capacity` | int | `VISIVO_RENDER_QUEUE_MAX`. |
| `queue_timeout_seconds` | float | `VISIVO_RENDER_QUEUE_TIMEOUT_SECONDS`. |
| `queue` | array | The caller's own places, each as in the 202 answer above (without `valid`, `queued`, `error`). |
| `sessions` | array | One object per session, see below. |
| `transports` | array of string | `"jpeg-ws"` and `"jpeg-ws-push"` always; `"h264-webrtc"` and `"h265-webrtc"` added when the WebRTC module imports and its encoder probe succeeds. Both name the same H.264 stream: `"h265-webrtc"` is the earlier name, kept for older clients. |

Each element of `sessions`: `session_id`, `dataset_path`, `frames_rendered`, `bytes_sent`, `commands_received`, `last_render_ms`, `last_jpeg_kb`, `age_seconds`, `idle_seconds`, `clients` (WebSockets connected now).

```json
{
  "active_sessions": 1,
  "capacity": 8,
  "idle_ttl_seconds": 300.0,
  "queued": 0,
  "queue_capacity": 16,
  "queue_timeout_seconds": 600.0,
  "queue": [],
  "sessions": [{"session_id": "9f1c...", "dataset_path": "/data/cube.fits",
                "frames_rendered": 412, "bytes_sent": 51234567, "commands_received": 980,
                "last_render_ms": 21.4, "last_jpeg_kb": 96.2,
                "age_seconds": 183.0, "idle_seconds": 1.2, "clients": 1}],
  "transports": ["jpeg-ws", "jpeg-ws-push"]
}
```

Errors: 401 (token).

## Point tables

A VBT point table opened with `/v1/vbt/open`, or a catalogue opened with `/v1/catalogue/open` (whose reply says `server_side_rendering_available`; a sky or cartesian one), is drawn by a point session (`app/render_session/points.py`, issue #83), as the desktop's point viewer draws it: points or emissive Gaussian splats, coloured by one column through a colour map, with the labelled bounding box, a colour bar and the orientation marker. The table is memory-mapped where it lies; the columns drawn are copied once to the GPU. At most `VISIVO_POINT_RENDER_MAX_POINTS` rows are drawn (default 100 000 000; a larger table is drawn from an even sample of its rows), and during a drag at most `VISIVO_POINT_RENDER_LOD_POINTS` (default 1 000 000).

A catalogue is read again in the session's process (its files and format, several files concatenated) and its numeric columns are drawn. A sky catalogue is drawn at positions computed as the desktop computes them (`app/catalogue_positions.py`): the distance from a redshift-space Hubble distance, another cosmology's comoving distance, a distance column (a luminosity or angular-diameter one converted to comoving), the Planck18 comoving distance of the redshift, or 300 Mpc, in that order; the direction in the FK5, galactic or supergalactic frame. They are the columns `@x`, `@y`, `@z`, the session's default coordinates.

`threshold` is null; `data_min` / `data_max` are the colour column's range over the rows drawn (null in solid colour), also from `GET /v1/render/{session_id}/levels` after a colour change.

The camera, viewport and encoding commands are those of a cube (`rotate`, `zoom`, `pan`, `reset`, `set_interactive`, `resize`, `jpeg_quality`); `pan` moves the camera in the view plane. The point commands:

| `type` | Fields | Effect |
|---|---|---|
| `set_point_fields` | `x`, `y`, `z` (column names; any subset) | The columns used as coordinates. Rebuilds the points. Default: `X`/`Y`/`Z` when the table has them, else its first three columns. |
| `set_color` | `field` (column, or null for solid colour), `lut` (colour map name), `min`, `max` (null: the column's range) | Colouring. |
| `set_point_style` | `mode` (`"points"` or `"gaussian"`), `size` (points: pixels of the frame, 1 to 64; splats: 0.25 to 12), `intensity` (1 to 100) | Point size in the frame's pixels (a high-DPI client scales it), or the splats' radius (mean spacing × size / 8, the spacing ∛(volume / N) of the box the points fill) and opacity (min(0.9, 20 / D) × intensity / 100, within 0.05 to 0.9, D = longest side / spacing, the splats along the longest sightline), as on the desktop: a filter that cuts a slab changes neither. |
| `set_filters` | `filters`: list of `{field, op, value}`, `op` one of `=`, `!=`, `>`, `>=`, `<`, `<=` | The rows drawn: those passing every filter. |
| `set_background` | `name` (`"Dark"`, `"Dark gray"`, `"White"`; anything else is dark) | Background, and the colour of the box and of the colour bar's text. |
| `set_show_outline`, `set_show_axes`, `set_show_colorbar` | `visible` (bool) | Labelled box, orientation marker, colour bar. |
| `set_camera` | `position`, `focal_point`, `view_up` (three numbers each), optional `view_angle` (degrees), `parallel` (bool), `parallel_scale` | The whole camera, as `GET /v1/render/{id}/camera` returns it: the desktop hands back the camera of its ray-traced view. Refused without the three vectors, or with the position on the focal point. |
| `set_sky_positions` | `frame` (`"fk5"`, `"galactic"`, `"supergalactic"`), `distance_mode` (`"comoving"`, `"redshift_space"`), `cosmology` (a model of `/v1/cosmology/distance`; `"Planck18"` is the desktop's own integration) | A sky catalogue's positions, recomputed (`@x`, `@y`, `@z`); refused for a VBT or a cartesian catalogue. |

An unknown column or operator is refused: the command is logged and the view unchanged.

## WebRTC signalling

These two routes exist on every backend that mounts the render router, but they only work where the optional WebRTC module (aiortc) is installed. The video track carries H.264, encoded with NVENC (`h264_nvenc`) when the encoder probe succeeds and with aiortc's software encoder otherwise. `/v1/render/diagnostics` advertises this transport as `h264-webrtc`, and also as `h265-webrtc`, its earlier name.

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

**Authentication.** The token is read from the `token` query parameter, or from the `X-Visivo-Token` header when the transport forwards it. Every configured token is accepted. The server refuses the connection before accepting it, so the handshake fails (with uvicorn, an HTTP 403 answer to the upgrade request; the close code and reason below are not delivered to the client):

- close code 1008 (`"invalid token"`) when no token matches;
- close code 1011 (`"unknown session_id"`) when the session does not exist (closed, for example by the idle timeout or the disconnect grace) or belongs to another token. A client cannot tell the two apart from the handshake; one whose token is valid should create a new session.

```text
wss://node.example.org/v1/render/9f1c.../ws?token=<token>
```

**Messages.** The client sends JSON text frames, one command per frame, each with a `type` field. The server sends:

- binary frames: one JPEG image each;
- text frames: JSON, namely `{"error": "invalid JSON"}` for a frame that is not valid JSON, and the `{"stream": "on", ...}` reply to `stream`.

Commands do not render by themselves. In the default (pull) mode a frame is rendered and sent only in answer to `request_frame`, so a client typically sends its camera commands and then one `request_frame` per display tick. In push mode (after `stream`) the server sends frames on its own, see [Push mode](#push-mode).

The server closes the connection with code 1011 on an unexpected server error (`"server error"`), when the push stream fails to render or send (`"render failed"`), or when the session itself has ended under the connection, its render process having exited or been killed (`"session ended"`). After `"session ended"` a reconnect is refused (`"unknown session_id"`): the client creates a new session. The push stream notices an ended session within a poll (0.25 s) even when the client sends nothing. The desktop's cube and point-table views create the new session by themselves, once: a replacement that ends again before it has streamed for 10 s is not replaced again, and the cube view offers Retry (a point table goes back to its loaded rows). After any other close the view offers Retry. A WebRTC stream has no close reason: the desktop then asks `GET /v1/render/{session_id}/levels`, which answers 404 for a session that has ended. The new session starts from the default camera.

A local backend started by hand must listen where the client's WebSocket goes: Qt resolves `localhost` to `::1` first for WebSockets (HTTP falls back to IPv4), so a backend bound to `127.0.0.1` alone answers the REST calls while every render WebSocket fails. Start it with `--host localhost`, as the desktop's own launcher does.

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
| `reset` | none | Ends a drag in progress, restores the default view state (keeping the window size; threshold back to the session's default, cutting plane on the middle channel) and resets the camera to fit the cube. |
| `set_camera` | `position`, `focal_point`, `view_up` (three numbers each), optional `view_angle` (degrees), `parallel` (bool), `parallel_scale` | The whole camera, in the cube's voxel coordinates as `GET /v1/render/{id}/camera` gives it: the desktop hands back the camera its ray-traced view was turned to. Refused (the stream kept) without the three vectors, or with the position on the focal point or the view up along the line of sight. |
| `set_lut` | `name` (string) | Colour map of the volume (and of the isosurface). Unknown names fall back to Inferno. `colormap` is an accepted alias. |
| `set_slice_lut` | `name` (string) | Colour map of the cutting-plane slice. |
| `set_transfer_functions` | `color`: list of `[x, r, g, b]`, `opacity`: list of `[x, a]` (1 to 4096 finite points each), optional `blend_mode` (int, as `set_blend_mode`) | The client's own transfer functions, point by point (a VBT volume on a log scale). A malformed list is refused (logged, view unchanged). A later `set_lut` or `set_threshold` goes back to the colour map and threshold. |
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

Each render session renders in its own child process, so a render or a JPEG encode does not block the backend's event loop and a crash in VTK or the GPU driver ends only that session (`VISIVO_RENDER_IN_PROCESS=1` keeps sessions in the backend process instead; on macOS a session there renders on the event loop's thread, since a Cocoa window can only be used from the main thread, and blocks the backend while it draws). Sessions are spread over the GPUs the job may use, one GPU per session in turn. During a drag (`set_interactive` `true`) the render window is halved in each dimension, the cutting plane and the axes are hidden and the outline is drawn bright; a volume is ray-cast with fewer rays and fewer samples per ray, at one of four levels (image sample distance and sample-distance factor 2, 3, 4 and 6), and an isosurface is replaced by a low-polygon wireframe built from the cube shrunk four times. The volume level adapts to the measured render and readback time of each drag frame: one level coarser when a frame takes more than 30 ms (1.2 times a 25 ms target), one level finer when the finer level is expected to fit the 25 ms target, and the level reached is kept for the next drag. Releasing the drag restores the full window size and full sampling. For running the backend with GPU rendering on a cluster, see [HPC deployment with SLURM](../deployment/hpc-slurm.md).
