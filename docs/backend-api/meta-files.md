# Meta, sessions, files and diagnostics

Backend health, session management, backend-side file browsing and the
diagnostics log. Every route here needs `X-Visivo-Token`.

## Meta

### `GET /v1/health`
Backend liveness and capacity.
```json
{
  "ok": true,
  "workers": 4,
  "worker_pool_restarts": 0,
  "active_sessions": 1,
  "product_cache_entries": 3,
  "product_cache_capacity": 64,
  "task_registry_entries": 0,
  "task_ttl_enabled": true,
  "task_ttl_seconds": 3600,
  "running_tasks": 0,
  "queued_tasks": 0,
  "recent_tasks": [
    { "operation": "products/moment", "status": "done",
      "duration_seconds": 2.4, "age_seconds": 31.0, "cache_hit": false }
  ],
  "timestamp": "2026-10-31T09:00:00+00:00"
}
```

`worker_pool_restarts` counts how often the worker pool had to be rebuilt
after a worker died (requests in flight then failed). `queued_tasks` is the
part of `running_tasks` that has not started yet.

`recent_tasks` is what the Data Hub shows as **Last 5 jobs**, and it holds two
kinds of entry: the asynchronous tasks (`/v1/tasks/moment`, `/v1/tasks/pv`) and
every other compute request, recorded by an HTTP middleware when its response
finishes. Without the second kind the list was almost always empty — the rest of
the backend is synchronous and registered nothing.

`operation` is the route template (`photometry/aperture/{dataset_id}`), so the
same tool run on twenty datasets reads as twenty runs of one operation rather
than twenty different ones. `status` is `failed` both for an HTTP error and for
the `{"valid": false}` envelope that most routes return with HTTP 200.

`duration_seconds` is the backend's own time for that request, from its arrival
to the end of its response. A file refused at once (a truncated FITS, a file
that is not FITS) takes a millisecond or two, however long the client spent
before asking (its health check, the classification, a decompression, which
is listed as its own `decompress` task). A file the backend never received,
because the client refused it first (not found, a format nothing opens), is
not in this list.

The desktop client fills both gaps itself (`src/gui/OpenJobMerge.h`). It keeps
an in-memory record of its last opens (when the user asked, when the open
ended, how, whether it reached the backend) and merges it into the panel: an
open whose backend job finished inside the client's open, from request to
result (an `…/open` route preferred over a `decompress` task, then the one
closest to the client's end), is shown with the client's end-to-end time and
outcome, the backend's time in its tooltip; an open refused before the backend
is added as an entry of its own. The response names no file, so the match is
by time: each job finished `age_seconds` before the response arrived, which
needs no agreement between the two clocks.

A finished job reads `done` when the middleware recorded it and `completed` when
it came from the task registry — the task API's own terminal status. Both are
successes; a client that treats anything other than `done` as a failure marks
every moment as broken. (The registry itself used to recognise only `done`, so
no `/v1/tasks/*` job appeared in this list at all: not running, not finished.)

Not recorded: browsing and classification (`/v1/files/*`), session bookkeeping,
image tiles, SAMP, WebRTC signalling, interactive scrubbing (`/v1/cube/slice`,
`/v1/spectral/probe`), job polling (`/v1/vlkb/mcutout/phase`), the single-redshift
`/v1/cosmology/distance` lookup, and the `/result/binary` collection of a product
that was recorded when it was computed. The cosmology BATCH is recorded: it is
asked for once, when a user picks a different cosmology, and it recomputes the
position of every source in a catalogue.
Only `POST`/`PUT`/`PATCH` count. The history holds the last 50 operations.
Once per-user tokens are configured, `recent_tasks` is filled only for the
operator (primary token), since it names other users' operations; any other
token gets an empty list and the rest of the snapshot.

### `POST /v1/sessions/new`
Mint a fresh backend session owned by the calling token and return its id.
No request body.

```json
{ "valid": true, "session_id": "…" }
```

A client calls this once and sends the id as `X-Visivo-Session` on every later
request. A server-minted id is what makes session ids non-enumerable: with
`VISIVO_STRICT_SESSIONS` on (the default once per-user tokens are configured)
the backend refuses ids a client invented, so an unknown id and another user's
id both answer 404. When the token already holds its quota of sessions the
route answers 429.

### `GET /v1/sessions`
Aggregate session-registry statistics: `active_sessions`, `ttl_seconds` and
one entry per session with a redacted id. Server-wide, so once per-user tokens
are configured only the operator (primary token) may read it (403 otherwise).

### `GET /v1/sessions/{session_id}/datasets`
Enumerate the datasets currently open in a backend session. Used by
cross-dataset tools (Spectral Stacking, etc.) to populate selection lists in
the desktop client. Returns `404` if the session does not exist or belongs to another token.

```json
{
  "session_id": "anon-abc",
  "datasets": [
    { "dataset_id": "ds_f558b5d90c55", "kind": "cube",
      "path": "/data/cubehi-clean-m31.fits",
      "width": 512, "height": 512, "depth": 200 },
    { "dataset_id": "ds_…", "kind": "cube", "path": "…",
      "width": 512, "height": 512, "depth": 200 }
  ]
}
```

### `GET /v1/vr/backend_check`
Whether this backend can serve the desktop's VR headset (issue #187), so that
a backend running older code than the desktop (on a cluster, typically a
`--backend-src` overlay copied weeks ago) is visible before the headset's page
fails on a missing route. Needs the token. *Open in VR Headset…* asks it
before pairing (without following redirects) and shows the page build and any
warning.

```json
{
  "vr_api": 2,
  "page": { "present": true, "build": "c09cce3dcfba" },
  "missing_routes": [],
  "stereo": { "depth": "auto" }
}
```

`vr_api` is the headset protocol level (2: stereo frame headers carry
`reprojection` and `depth_unavailable`, a running push stream takes a new ack
window). `page.build` is a hash of the files under `app/static/vr`.
`missing_routes` names the routes the page calls that this backend does not
serve, among ping, sessions/new, files/list, datasets/open, the cube routes
the page uses (`reduced_info`, `reduced_bin`, `subvolume_bin`,
`kinematic_select_bin`, `mask_stats_bin`), spectral probe, pixel_to_sky, and
the render routes (`create_session`, the queue's DELETE, the session's DELETE,
its WebSocket), which are missing on a backend without server-side rendering.
`stereo.depth` is `"off"` when `VISIVO_XR_DEPTH=0` makes the backend send
stereo frames without depth, else `"auto"`.

The details for an operator are in one line of the backend's startup log, not
in the answer: `[startup] VR page c09cce3dcfba (20 files, newest …) at /vr/;
backend code newest … in /app/app; VR routes all present; stereo depth auto`.
The dates are those of the newest page file and of the newest Python file of
the backend's code: a copy made weeks ago shows there.

---

## Files

### `GET /v1/files/list?path=<dir>`
List backend-side directory. Returns `{valid, error, current_path, entries}`, `entries` being `FileEntry[]` with `name`, `path`, `type`, `size`, `modified_time`, `is_fits`. A missing or refused directory is `valid: false` ("Directory not found.").

### `POST /v1/files/header`
```json
{ "path": "/data/cube.fits" }
```
Returns `{valid, error, cards}`, the FITS header cards as `string[]` ("FITS file not found." with `valid: false` for a missing or refused path).
The cards are the image HDU's, followed by the primary header's descriptive
cards it does not repeat. For a `.fits.gz` they are read off the start of the
gzip stream, without decompressing the rest.

### `POST /v1/files/preview`
What the file browser shows for any selected file or directory. Only the start
of the file is read, so the preview is as quick through a remote backend as on
this machine.

```json
{ "path": "/data/catalogue.csv", "max_bytes": 16384, "max_lines": 60 }
```
`max_bytes` (default 16384) bounds both the file content read and the UTF-8
size of `text`; `max_lines` (default 60) bounds the lines of `text`. Values
above the caps (65536 bytes, 400 lines) are clamped; values below 1, or
not integers, are `422`.

Returns
```json
{ "path": "/data/catalogue.csv", "size_bytes": 81234, "kind": "text",
  "format": "Comma-separated table, 5 columns",
  "text": "id,ra,dec,flux,z\n0,0.0,0.0,0.0,0.00\n…", "truncated": true }
```
`path` echoes the request. `truncated` is true when the file holds more than
`text` shows. The kind is decided from the bytes, not the name:

| `kind` | `format` (examples) | `text` |
|---|---|---|
| `fits` | `FITS image 700×700×158`, `FITS image 9×8 (tile-compressed)`, `FITS image 7×5×6 (gzip)`, `FITS table, 2 columns, 4 rows` | One line per HDU (first 10), a blank line, then the cards `/v1/files/header` gives. A `.fits.gz` is read off the gzip stream, skipping at most 64 MiB of decompressed HDU data to reach later headers (the headers before that point are shown); a stream whose first header has no `END` card within 1000 blocks is reported as unreadable. |
| `text` | `Comma-separated table, 5 columns`, `Tab-separated table, 3 columns`, `Semicolon-separated table, …`, `VOTable`, `XML`, `JSON`, `IPAC table`, `speck point catalogue`, `Text`, `Empty file`; ` (Latin-1)` appended when the bytes are not UTF-8 | The first lines (a line cut by the byte bound is dropped). The separator and column count are the ones the catalogue reader uses. |
| `vbt` | `VisIVO Binary Table, volume 64×64×64, 2 fields`, `VisIVO Binary Table, 1000 points, 3 fields` | The `.head`. For a `.bin`, the `x.bin.head` (else `x.head`) beside it, when it is inside the data roots. A `.head` that is not a VBT header is `text`. |
| `hdf5` | `HDF5, 3 groups, 12 datasets` (`+` when the walk stopped) | The group/dataset tree, indented, with shapes and dtypes; at most 200 objects (and `max_lines`); no data read, external links not followed. |
| `compressed` | `Comma-separated table, 5 columns (gzip-compressed)`, `bzip2-compressed data`, `ZIP archive, 4 files`, `Text (zip: notes.txt)` | gzip/bzip2/xz: the first lines of the decompressed stream when it is text (at most `max_bytes` decompressed). zip: the member list, or the only member's first lines. |
| `binary` | `Binary data`, `Empty file`, `Special file (not read)` | A hex/ASCII dump of the first 256 bytes (none for a FIFO or device). |
| `directory` | `Directory, 12 entries` (`1000+` past 1000) | Folders first, then files, one name per line. |

Errors: `403` for a path outside the allowed data roots (existing or not, and
also for a symlink that resolves outside them) or a hidden path on a shared
backend, `404` for a missing path, `422` for bad arguments (including a path with a null byte).

### `POST /v1/files/header/patch`
Apply header edits to a **copy** of the file, in the Workspace. The input is
never modified.

```json
{ "path": "/data/broken.fits",
  "cards": [{ "key": "CTYPE1", "value": "RA---TAN", "comment": "" },
            { "key": "CTYPE2", "value": "DEC--TAN" }],
  "remove": ["EQUINOX"], "output_basename": "" }
```
Returns `output_path`, `workspace_filename`, `changes` (one line per edit,
naming the old value as well as the new), `hdu_index`, and `wcs_status` /
`wcs_message` / `wcs_celestial`.

That last part is the point. The legacy's header modifier appeared only when a
file failed to open, offered three keyword groups, and reported "File has been
saved!" whether or not the values typed produced a header anything could read.
Here the WCS is built from the result and the reply says what it amounts to — a
celestial solution, a degraded one, or still nothing.

Refused: `SIMPLE`, `BITPIX`, `NAXISn`, `EXTEND`, `XTENSION`, `PCOUNT`, `GCOUNT`,
`TFIELDS` — they describe the bytes on disk — and `BSCALE`, `BZERO`, `BLANK`,
which say how the stored integers map to physical values: editing one does not
annotate the data, it multiplies it. A value that reads as a number is written
as a number, since a quoted `'-0.001'` in `CDELT1` is a string card that every
WCS parser ignores; quote it explicitly to force a string.

### `POST /v1/files/classify`
What is this file? Answers from the file itself — magic bytes, extension, and at
most one FITS header; nothing is opened, converted or registered.

```json
{ "path": "/data/unknown.fits" }
```
```json
{ "valid": true, "error": "", "path": "/data/unknown.fits", "open_path": "",
  "kind": "cube", "opener": "dataset", "confident": true,
  "detail": "FITS cube (512 × 512 × 200)." }
```

| Field | Meaning |
|-------|---------|
| `kind` | `image`, `cube`, `dynspec`, `velocity-field`, `fits-table`, `catalogue`, `vbt`, `compressed`, `directory`, `unknown` |
| `opener` | `dataset` → `/v1/datasets/open`; `catalogue` → `/v1/catalogue/open`; `vbt` → `/v1/vbt/open`; `velocity` → `/v1/velocity/open`; `ask` → the content is ambiguous, let the user choose; `unsupported` → nothing here opens it, say so |
| `open_path` | The path to hand the opener when it differs from `path`: a VBT chosen by its `.bin` opens through the `.head` beside it. Empty means "use `path`". |
| `confident` | `false` when the answer is a guess — a FITS whose header would not parse, for instance |
| `detail` | One sentence naming what was found, shown to the user when we have to ask |

A VBT is classified by its header: a volume as `cube` / `dataset` (each field
opens as a cube), a table of points as `vbt` / `catalogue` (opened with
`format: "vbt"`). A gzip-compressed FITS is classified by what it holds
(read through the start of the gzip stream) and a tile-compressed one like
any FITS, the `detail` adding that it is decompressed into the workspace cache
when opened; `compressed` / `unsupported` is a compressed file that holds no
FITS (a gzipped CSV, say). A FITS **table** is reported as `fits-table` / `ask`: it can be a source
catalogue or a table to overlay, and only the user knows which. A missing path
and a refused one answer identically (`"File not found."`), so the classifier
cannot be used to enumerate what lies outside the allowed data roots.

This is what lets the client offer a single **Open…** instead of one action per
file type.

---

## Diagnostics

The backend keeps its recent log entries in an in-process ring buffer, so the
desktop client can mirror them in its own Diagnostics window
(`backend/app/routers/diagnostics.py`).

### `GET /v1/diagnostics/events?since=<id>&limit=<n>`
Long-poll read of the buffer: every entry with an id strictly greater than
`since`, oldest first.

| Query | Default | Meaning |
|---|---|---|
| `since` | `0` | Watermark: the `last_id` of the previous call |
| `limit` | `500` | Entries per response, 1 to 2000 |

```json
{ "valid": true, "error": "", "last_id": 1042,
  "events": [{ "id": 1041, "ts": 1761900000.123, "level": "warning",
               "category": "Backend", "source": "visivo.backend",
               "message": "…", "dataset_id": "", "session_id": "",
               "operation_tag": "" }] }
```

`last_id` is the highest id in the buffer; pass it as `since` on the next call.
`level` is `debug`, `info`, `warning` or `error`; `category` is one of
Scientific, Client, Backend, Task, WCS, Remote, Rendering, Performance.

The buffer mixes every user's activity, so once per-user tokens are configured
only the operator (primary token) may read it; another token gets 403.

### `GET /v1/diagnostics/stats`
Buffer occupancy for monitoring: `{entries, capacity, last_id}`. Any valid token.

---

## The VR page

The WebXR page of [Cubes in a VR headset](../user-guide/vr/index.md) and its
lifecycle routes (`backend/app/routers/vr.py`, epic #166).

### `GET /vr/` and `GET /vr/b/<build>/…`
The page, public like any static file (it holds no data and no token).
`/vr/` is always revalidated (`Cache-Control: no-cache`) and loads its
modules from `/vr/b/<build>/…`, where `<build>` is a 12-character hash of the
page's files (dotfiles are neither hashed nor served): an upgraded backend
serves the page from new addresses, so a fresh load never mixes yesterday's
modules with today's. The current build's files are cached for good
(`immutable`). Another build's address (a tab older than the backend) gets
today's file, revalidated: such a tab can run a mix of old cached modules and
new ones until it reloads, which its version check asks it to do. The
unversioned `/vr/js/…` addresses keep working, revalidated.

### `GET /v1/vr/info`
Public, like `/v1/ping`; `Cache-Control: no-store`. The page calls it at
start, every 5 s, and with a backoff (1, 2, 4, 5, 5… s) while nothing answers.
Each check gives up after 4 s (a connection that hangs counts as no answer, and
the next check is scheduled as usual); *Retry* cancels a check still pending and
makes a new one. The requests a recovery makes (datasets/open, sessions/new,
reduced_info) give up after 30 s each.

```json
{ "vr_api": 1, "vr_api_min": 1, "build": "8cc84c409df0",
  "instance": "2ba71142538a", "token_ok": true, "backend_version": "1.0.12" }
```

`vr_api` and `vr_api_min` bound the page API this backend serves (a page
outside them is told to reload, or to update the application); `build` is the
page build it serves; `instance` changes at every backend start (the page then
knows the backend's sessions and datasets are gone and opens its cube again);
`token_ok` says whether the `X-Visivo-Token` header sent is accepted (`null`
when none was sent). Token checks are limited to 20 a minute per client
(the last 256 clients are tracked) and 200 a minute in all: beyond that
`token_ok` is `null` and `"token_check": "rate-limited"` is added, and
repeated refused tokens from one client are logged (at 5, 20 and 100 in a
minute). A client is its socket address, except that every request carrying
`X-Forwarded-For`, `Forwarded` or `X-Real-IP` counts as one client: uvicorn
trusts those headers from 127.0.0.1 by default, which is how adb reverse and
ssh tunnels arrive, so they cannot be used to pick a fresh bucket. Locally
all clients are loopback, so the limit is in effect shared by all of them;
that is harmless, since a skipped check answers `null` and the page keeps the
state it knew.
`backend_version` is present only for an accepted token. A token with
non-ASCII characters is refused like any wrong token (401 on the other
routes), never an error.

### `POST /v1/vr/log`
The page's errors and lifecycle events, written to the backend log
(logger `visivo.vr`, one line per event: `[vr <build>] <event>: <message>`).
Needs the token.

```json
{ "build": "8cc84c409df0", "user_agent": "…",
  "events": [{ "level": "error", "event": "webgl-context-lost", "message": "in VR", "t": 1762873000000 }] }
```

| Limit | Value | Beyond it |
|---|---|---|
| Body | 16 KiB | 413 |
| Events per request | 25 | 422 |
| `level` | `debug`, `info`, `warning`, `error` | 422 |
| `event` | 1–64 of `A–Z a–z 0–9 _ . : -` | 422 |
| Per token, per sliding minute | 20 requests, 60 events | 429 with `Retry-After` |

Each message is scrubbed before it is written, in a worker thread (never on the
event loop), in this order. Steps 1 and 2 run over the whole message; the
pattern rules (3 to 5) only over its first 1200 characters, and every rule is
linear (a 4000-character adversarial message scrubs in a few milliseconds):

1. `%`-escapes are decoded first (a pairing link writes `path=%2FUsers%2F…`);
2. every accepted token, and the caller's own, becomes `<token>`;
3. the value of any key that contains `token` or `session` (`token=`,
   `session_id=`, `sessionId:`, `access_token=`, `X-Visivo-Token:`, JSON
   `"token": "…"`), quoted or not, before `=` or `:`, becomes `<redacted>`
   when it looks like a secret (6 or more characters with a digit, 8 or more
   hex characters, or 16 or more of any kind), so `sessions: 3` stays as it is;
4. the id in `/v1/render/<id>/…` (a render session) becomes `<id>`;
5. paths keep only their last component (`…/cube.fits`): `file://` URLs, the
   path of a URL with a host (the backend's own `/v1/…` and `/vr/…` routes
   are kept whole), Windows paths (`C:\…`, `C:/…`, `\\server\share\…`) and
   POSIX paths (`/…`, `~/…`). Directory names may hold single spaces
   (`/Users/John Doe/data/cube.fits` → `…/cube.fits`) only up to the first
   file name with an extension (also in a `file://` URL); otherwise
   directory names may hold single spaces when another `/` follows
   (`/mnt/Shared Drive/Alice Smith/x` → `…/x`: better too much than a name
   left behind), the last component never does, so the rest of the message is
   kept (`open /data/c.fits failed at 12/11` → `open …/c.fits failed at
   12/11`), and `3/5` or `1 / 2` are no paths. A home directory (`/Users/…`,
   `/home/…`, `C:\Users\…`, `C:/Users/…`, in a `file://` URL too) is never
   shown: `/Users/John Doe/data` → `…/data`, and a path that ends at the home
   directory gives `…/<user>` (`/home/alice` → `…/<user>`);
6. control and format characters (Unicode categories Cc, Cf, Zl and Zp:
   newlines, NEL, C1 controls such as CSI, bidirectional overrides, zero-width
   characters, U+2028/2029) become spaces;
7. a message is cut at 600 characters.

Not redacted: a known token shorter than 6 characters, and a short value
without a digit after a token key (`token=letmein`): they cannot be told from
ordinary words. Use generated tokens (the backend makes one when none is set;
`python -c "import secrets; print(secrets.token_urlsafe(32))"`).

The page applies the same rules before sending (`diagnostics.js`
`scrubText`); both are tested against the same cases
(`backend/tests/vr/scrub_cases.json`). Answer: `{"accepted": <n>}`.

### The render WebSocket's token (the VR page)
The page opens `/v1/render/<id>/ws` with the subprotocols `visivo.v1` and
`visivo-token.<token>`; the backend takes the token from there and answers
`visivo.v1` only, so the token is in no URL and no access log. A token with
characters a subprotocol cannot carry falls back to `?token=`. The desktop's
remote 3-D view still sends `?token=` (see the render API).

