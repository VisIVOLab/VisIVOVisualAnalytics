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
| `stream` | `window` (int, 1 to 8, default 3), `max_fps` (number, 1 to 120, default 30) | Switches the connection to push mode. Values outside the ranges are clamped; a value that is not a number leaves the default. A second `stream` on the same connection changes the window and rate cap of the running stream and is answered with the same `{"stream": "on", ...}` (issue #187: the headset's page widens its window over a slow link); a wider window lets a frame waiting for an ack go at once. |
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
| `set_crop` | `box`: `{"min": [x, y, z], "max": [x, y, z]}` or `null` | Crops the volume to a box given as fractions 0 to 1 of the cube along x (NAXIS1), y (NAXIS2) and z (the spectral axis), its faces at the outer pixel edges: the VR page's clipping box, so that its refined frames show what the headset draws ([issue #170](../user-guide/vr/reference.md#refinement-full-quality-frames-from-the-backend)). `null`, or the whole cube, removes the crop; `reset` removes it too. A malformed box is refused (logged, crop unchanged). The isosurface is not cropped; a cube drawn out of core ignores it. |
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

#### Stereo frames for a headset

A cube session (FITS cube or VBT volume field; not a cube drawn out of core) can render the two eyes of a VR headset for the pose the headset sends, issue #169, and so can a point session (issue #190, `render_session/_stereo_points.py`): its pose also carries `data_to_model`, 16 numbers in column-major order mapping the table's coordinates to the page's model space (a uniform scale and a translation; without it, the page's own rule on the drawn rows' bounds: centred on the origin, the longest side 1). A point session draws every row it holds for the eyes (never the drag's sample), without its box, colour bar or orientation marker; points send their depth (as a surface), additive splats none. The desktop's mono view is not affected: a session renders stereo frames only between an `xr_pose` and an `xr_stop`, and a connection that sent `xr_pose` sends `xr_stop` itself when it closes, so the next client gets the mono view back.

The stereo mode belongs to the session, so it is given to one connection alone. An `xr_pose` is refused (`xr_error`) while another WebSocket or a WebRTC peer is connected to the session; once a connection has the stereo mode, another WebSocket is accepted and closed at once (close code 1013, `"session in stereo use"`), a WebRTC offer gets 409, and `xr_*` commands over a WebRTC data channel are ignored. A headset uses a render session of its own; the desktop's sessions never receive stereo frames. While in stereo, `set_interactive` is ignored (the drag level of detail is the mono view's).

| `type` | Fields | Effect |
|---|---|---|
| `xr_pose` | see below | Renders both eyes for this pose from now on. Checked on arrival: a malformed pose is answered at once with `{"type": "xr_error", "pose_id": ..., "error": "..."}` (text) and never reaches the renderer. |
| `xr_stop` | none | Back to the mono view. |

Matrices are 16 numbers in column-major order, as WebGL and WebXR give them; distances in metres.

| Field | Type | Description |
|---|---|---|
| `pose_id` | int, 0 to 2^53 | The client's id of this pose, echoed in the frame rendered for it. Required. |
| `t_client` | number | Optional; echoed (the probe script sends its clock, seconds, to measure the round trip). |
| `model` | 16 numbers | The cube's placement: model space to world. A rotation, a translation and a uniform scale (refused otherwise). Model space is the VR page's: the box centred on the origin, its longest side 1, x along NAXIS1, y along NAXIS2, z along the spectral axis times `z_stretch`, its faces at the outer pixel edges. Required. |
| `z_stretch` | number, (0, 100] | Spectral stretch, as on the VR page. Default 1. |
| `views` | list of two objects | One per eye, any order: `eye` (`"left"` or `"right"`), `view` (16 numbers, world to eye, rigid), either `projection` (16 numbers, OpenGL perspective, the WebXR view's `projectionMatrix`) or `fov` (`left`, `right`, `up`, `down` half angles in radians, OpenXR style with `left` and `down` negative, and `near`, `far`), and `width`, `height` (one eye's frame, 16 to 2560 pixels, the same for both eyes). Required. |
| `depth` | object or `false` | `{"scale": n}` (1 to 16, default 4): the depth image reduced n times on both axes; `false`: no depth image. Default `{"scale": 4}`. |
| `quality` | int, 1 to 100 | JPEG quality. Default: the session's (`jpeg_quality`, 95). |
| `background` | three numbers, 0 to 1 | Colour behind the volume. Default black, which is what a headset that blends the frame over its own rendering wants. |
| `sample_distance` | number, 0.25 to 4 | Ray step in voxels. Default 0.5 (see below). |

```json
{"type": "xr_pose", "pose_id": 1812, "t_client": 1794348308.2,
 "model": [0.6,0,0,0, 0,0.6,0,0, 0,0,0.6,0, 0,1.48,-0.75,1],
 "views": [
   {"eye": "left",  "view": [...16], "projection": [...16], "width": 1024, "height": 1024},
   {"eye": "right", "view": [...16], "projection": [...16], "width": 1024, "height": 1024}],
 "depth": {"scale": 4}}
```

Frames go out as any other frame does: over the push stream after `stream` (a headset asks for `max_fps` 72 or 90), or one per `request_frame`. Poses arriving faster than frames are rendered are merged: the render process applies only the newest of the poses waiting for it (other commands, `xr_stop` included, keep their order), so a frame is rendered for the newest pose, carries its `pose_id`, and older poses never queue up behind a slow frame. A stereo frame is one binary message:

| Bytes | Content |
|---|---|
| 0 to 3 | `VXR1` |
| 4 to 7 | header length n, little-endian uint32 |
| 8 to 8+n | header, UTF-8 JSON |
| then | payloads, at the offsets the header gives from the end of the header |

A binary message that does not start with `VXR1` is a mono JPEG. The header:

| Field | Description |
|---|---|
| `v` | 1 |
| `pose_id`, `t_client` | Those of the pose the frame was rendered for. |
| `eyes` | `[{"eye": "left", "x": 0, "y": 0, "width": w, "height": h}, {"eye": "right", "x": w, ...}]`: each eye's rectangle in the image. |
| `color` | `{"format": "jpeg", "width": 2w, "height": h, "offset", "length"}`: the two eyes side by side, left first, rows top first. |
| `depth` | `null`, or `{"format": "u16-zlib", "width", "height", "scale", "z_near", "z_far", "source", "units", "offset", "length"}`: deflated (zlib) little-endian uint16 rows, top first, both eyes side by side like the colour. 0 is no sample; a value q is the distance `z_near + (q - 1) / 65534 * (z_far - z_near)` metres along the eye's -z axis. `z_near` and `z_far` are the frame's own nearest and farthest samples. Each value is the nearest sample of its `scale` x `scale` block. On the silhouette of the emission a few pixels (12, about 2% of the hits, for a synthetic sphere at 192 x 192 per eye) report a sample where the exact surface has none, because interpolation spreads the surface by up to a voxel. `source`: `"volume-first-hit"` (the first sample along the ray whose opacity is above zero, that is the first above the threshold), or `"surface"` in isosurface mode. Absent (`null`) when no depth was asked for, or none can be read (see `depth_unavailable`). |
| `reprojection` | `"depth"` when the frame carries a depth, else `"pose"`: the client may warp it to a later pose only by the pose (issue #187). |
| `depth_unavailable` | `null` with a depth; otherwise why there is none, for the user: `"not requested"`, `"turned off on this backend (VISIVO_XR_DEPTH=0)"`, `"nothing on show gives a depth (neither a volume nor a surface)"`, or what the readback did (below). |
| `timing` | Seconds since the epoch, on the backend machine's clock: `received` (the pose reached the WebSocket), `render_start`, `render_end` (both eyes drawn and read back), `depth_end`, `encode_end` (JPEG), `ready` (the frame back in the backend from its render process), `sent` (handed to the socket). |
| `timing_ms` | The same as durations: `wait` (received to render start: the frame before was still rendering), `render`, `depth`, `encode`, `handoff`, `queue` (ack window and rate cap), `total` (received to sent). |
| `eye_ms` | Per eye: `render` (the GL draw) and `readback`. |

A frame the session cannot render (a cube drawn out of core) is answered with an `xr_error` text message instead. An `xr_error` may carry a `code` for a client to act on without reading the text: `out_of_core` (a cube drawn out of core) and `unsupported` (a session that draws no stereo frames) mean no stereo frame will come from this session.

Both eyes are drawn on the session's one GPU copy of the cube, one camera after the other into a window of one eye's size. The scene stays in the cube's voxel coordinates and the camera moves: the placement is folded into each eye's view and projection (`render_session/_stereo.py`). The volume's colour and its depth come from the volume mapper's render-to-image output, composited over `background`; it matches the window's own picture (measured: identical over black, at most 4 levels apart over a grey). Rays start half a voxel in front of the eye (or at the headset's near plane, if nearer), not at the headset's near plane: VTK's ray caster loses the volume when the camera is inside its box and the near plane is several voxels away, which is the case when the viewer leans into the cube (a 0.1 m near plane is 80 voxels of a 400-voxel cube 0.5 m wide). The picture is the headset's; emission nearer than its near plane is drawn rather than clipped, and with the eye inside the emission the whole view is lit from the first sample. The far plane is just past the box's farthest corner. The ray step is fixed at `sample_distance` for the stereo frame and the mono view's sampling is restored after it: at half a voxel the depth of a synthetic sphere is within half a voxel of its surface (median; 95th percentile 0.9 voxel), where the mono view's adaptive step of about one voxel gives 0.7 and 1.4. The probe `benchmark/remote_render/stereo_pose_latency.py` measures rate and latency (see its docstring).

#### Stereo frames as WebRTC video

Over Wi-Fi the colour of a stereo frame can travel as H.264 video instead of a JPEG on the WebSocket ([issue #189](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/189)). The WebSocket that drives the stereo mode negotiates a WebRTC peer of its own, over the same WebSocket. This peer is not one of the session's mono WebRTC peers ([WebRTC signalling](#webrtc-signalling)): it carries only that connection's stereo frames and does not count as another client. The rules above still hold: the link is refused while another client views the session or for a point table, and a desktop's WebRTC offer gets 409 once the headset's connection holds the stereo mode, that is from its first `xr_pose` on (before it, a desktop peer may still connect, and the headset's pose is then refused with `xr_error`). The frame's header and depth image stay on the WebSocket, so the push stream's acknowledgements and window work as before. The backend needs the optional WebRTC modules (aiortc, PyAV), as for the mono path.

| `type` (client) | Fields | Effect |
|---|---|---|
| `xr_rtc_offer` | `sdp` (the browser's offer, ICE gathered: no trickle), optional `crf` (10 to 40, default 14: constant quality, x264's scale, lower is better) and `max_kbps` (1000 to 200000, default 40000: the encoder's ceiling) | Answered with `{"type": "xr_rtc_answer", "sdp", "sdp_type": "answer", "crf", "max_kbps"}`, or `{"type": "xr_rtc_error", "error"}`; after an error the frames go on as JPEG. A new offer replaces the connection's link. |
| `xr_rtc_stop` | none | Closes the link: frames go as JPEG again. Closing the WebSocket closes it too. |

An offer is refused (`xr_rtc_error`, with the reason) when the backend has no WebRTC modules, the session is a point table or has another client, the connection has made 6 offers in the last minute, or the offer is larger than 64 KB, has other than exactly one media section (video), or that section does not only receive (`a=recvonly`). Before it is answered, the offer's loopback, multicast and unspecified ICE candidates, and any that are neither an IP address nor a browser's mDNS name (`….local`), are taken out; private, link-local, CGNAT or Tailscale (100.64.0.0/10) and public addresses are kept, so a headset on a network with public IPv4 or IPv6 addresses still connects. This filter is best effort, not a firewall. The backend's peer gathers host candidates only (no STUN server): it is meant for the Mac's own network. A headset reaching the backend through `adb reverse` alone has no UDP path to it, and its link does not connect: the page then stays on JPEG.

While the link is connected and sending its track, each `xr_pose` is encoded as H.264 by the session's render process, where it is rendered: the two eyes go to YUV 4:2:0 (BT.709, limited range) straight from the render, the pose id stripe is drawn in, and the encoder (`h264_nvenc` on Linux, otherwise `libx264` `superfast`, constrained baseline, `zerolatency`, no B-frames, constant quality `crf`; `VISIVO_XR_H264_ENCODER` names another; an encoder that holds a frame back, as VideoToolbox does, is skipped; when the frame size changes every encoder is tried again, the one in use first) hands back tens of kilobytes instead of megabytes of pixels. The backend forwards them, in order, to the link's track. A pose may ask for a JPEG anyway with `"frame_color": "jpeg"` (the page's reference frames for measurements); any other `frame_color` from a client is ignored, and so are client commands whose `type` starts with `xr_video` (the backend's own commands to the render process). Eyes narrower than 192 pixels, or a video frame larger than 4096 pixels on a side (both eyes side by side, stripe included: what H.264 decoders and NVENC take), go as JPEG; an odd eye height gets a 17-row stripe whose first row is black. If the encoder fails, the frame goes as a JPEG, the render process stops encoding, and the backend sends `{"type": "xr_rtc_error", "error": "H.264 encoding failed: …"}` and closes the link: the connection itself stays open. The WebSocket message of a video frame is the usual `VXR1` message whose `color` has no payload:

```json
"color": {"format": "h264-webrtc", "width": 2048, "height": 1024, "offset": 0, "length": 0,
          "stripe": {"y": 1024, "rows": 16, "cells": 48, "cell": 42, "id_bits": 32}},
"video": {"frame": 117, "id": 1812, "keyframe": false, "bytes": 18211, "encoder": "libx264"}
```

**Frames not sent, and keyframes.** Each H.264 frame refers to the one before, so the backend never sends a frame after one it did not send. A frame is not sent when the link closed meanwhile (the render process is told to make JPEGs again and renders the newest pose again at once), when the link is not connected at that moment, or when the track already holds 4 frames aiortc has not taken (all of them are dropped). In the last two cases the track drops every frame up to the next keyframe. While the link is not connected nothing more happens (a keyframe asked for then would be rendered and dropped again, in a loop); otherwise, and whenever a frame meets the track still waiting (so a first keyframe lost before the link came up does not stall the video), a keyframe is asked for: the render process renders the newest pose again as one. Keyframe requests, the browser's and the track's alike, reach the render process at most every 0.25 s; one held back is sent at the end of that time if the track is still waiting. A frame not sent takes no place in the acknowledgement window and has no message on the WebSocket. A keyframe is also sent first, every 120 frames, and on the browser's request (a picture loss, PLI or FIR, after packets were lost beyond what retransmission repaired): the keyframe re-render gives the decoder a whole picture of the newest pose again, so a still view that lost its last frame gets its refined frame back without moving. aiortc keeps the last 4096 RTP packets (128 by default) for retransmissions, since a keyframe of two large eyes is a few hundred packets.

**Pairing a video frame with its pose.** The video frame is the two eyes side by side (left first, as in the JPEG) with a stripe of 16 rows under them (17 when the eye height is odd, so the frame's height is even). The stripe holds 48 cells, each `floor(frame width / 48)` pixels wide, white (1) or black (0), past them black: the start pattern 1 1 0 1, the pose id's low 32 bits (most significant first), a CRC-8 of those four bytes (big-endian; polynomial 0x07, initial value 0), and the end pattern 0 1 0 0. The page reads the stripe of every decoded frame on the row 8 rows above the bottom of the frame's visible rectangle (inside the stripe either way; a decoder rounds the coded size up to whole macroblocks, so the coded size is not used). It copies those two rows out with `VideoFrame.copyTo` (the Y plane of an NV12 or I420 frame, or RGB); a frame that cannot be copied (no `format`, or `copyTo` refusing, as a hardware decoder's frame may) is drawn into a one-row canvas instead, and the page keeps doing so from then on, each cell the mean of its middle half thresholded at 128, and shows the frame only with the header whose `video.id` it reads; a stripe whose patterns or CRC do not match is dropped, never paired. The page draws only the eyes' rows of the frame (no copy). This needs no RTP timestamps (aiortc starts them at a random value) and no data channel: a frame lost on the network, a header whose frame never comes (the page drops it when a newer frame pairs) or a pose rendered again as a keyframe still pairs exactly. The patterns and the CRC refuse a damaged stripe rather than misread it: every burst of up to 8 damaged cells, and in the tests 3000 rows of noise, gave no wrong id (a row of random cells would pass by chance once in 65 536). In a synthetic stream of 20 000 poses with missing frames, repeats, damaged stripes and headers and frames racing each other the pairing made no mismatch (`backend/tests/vr/refine-webrtc.test.mjs`); through libx264 and a real aiortc peer every decoded frame read its own id (`backend/tests/test_render_stereo_webrtc.py`). In the browser runs the pairing was also checked independently of the stripe: the shown video frame against a JPEG the backend rendered for the same pose (`validation/vr_refine_webrtc.mjs`, 38–42 dB, where frames of neighbouring poses differ far more).
**Colour-only frames (issue #187).** A GL may give the colour back and not the depth: a GLES context cannot read a depth buffer with `glReadPixels`, and a driver's render-to-image depth texture may come back empty, cleared or unwritten (Mesa, some EGL stacks). Every depth is checked against its own picture before it is sent. The checks tell two kinds of failure apart. *Structural* (the readback itself failed): no depth image, a wrong size, values that are not finite or not in 0..1, an image of one value that contradicts the picture (1 everywhere under opaque pixels, or one value below 1 with transparent pixels), and for a surface a `GetZbufferData` that did not return VTK_OK (a GLES context) or a buffer of one value below 1. *Soft*: more than 5 % of the opaque pixels (alpha at least 16 of 255) without a first hit. Either way the frame is sent with its colour only, `reprojection: "pose"` and the reason in `depth_unavailable` (for example `"62 % of the drawn pixels have no depth"` or `"the window's depth buffer could not be read (status 2)"`). After two structural failures with no good depth in between the session stops reading depth and logs `stereo: depth readback unusable (...)` once; soft ones are never counted. A change of blend mode or of render mode (volume, isosurface) tries depth again. In maximum or minimum intensity blending the ray caster writes no first-hit depth (it fills the colour after the ray loop): those frames go without depth, nothing is read or counted, and `depth_unavailable` is `"no first-hit depth in MIP"` (or `MinIP`). A mapper that gives no image at all (not even the colour) makes the session read the window instead, as before, with `"the volume mapper cannot render to an image on this GL ..."`. `VISIVO_XR_DEPTH=0` in the backend's environment turns depth off from the start (`GET /v1/vr/backend_check` then says `"stereo": {"depth": "off"}`). On PLEIADI (V100, EGL, VTK 9.3.1) colour and depth both read back through the volume mapper (measured 2026-11-11), so there the fallback is a safety net.

## Server side

Each render session renders in its own child process, so a render or a JPEG encode does not block the backend's event loop and a crash in VTK or the GPU driver ends only that session (`VISIVO_RENDER_IN_PROCESS=1` keeps sessions in the backend process instead; on macOS a session there renders on the event loop's thread, since a Cocoa window can only be used from the main thread, and blocks the backend while it draws). Sessions are spread over the GPUs the job may use, one GPU per session in turn. During a drag (`set_interactive` `true`) the render window is halved in each dimension, the cutting plane and the axes are hidden and the outline is drawn bright; a volume is ray-cast with fewer rays and fewer samples per ray, at one of four levels (image sample distance and sample-distance factor 2, 3, 4 and 6), and an isosurface is replaced by a low-polygon wireframe built from the cube shrunk four times. The volume level adapts to the measured render and readback time of each drag frame: one level coarser when a frame takes more than 30 ms (1.2 times a 25 ms target), one level finer when the finer level is expected to fit the 25 ms target, and the level reached is kept for the next drag. Releasing the drag restores the full window size and full sampling. For running the backend with GPU rendering on a cluster, see [HPC deployment with SLURM](../deployment/hpc-slurm.md).
