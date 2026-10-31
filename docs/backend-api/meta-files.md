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
Only `POST`/`PUT`/`PATCH` count. The history holds the last 50 operations and is
served to admin tokens only, as the rest of the snapshot is.

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

---

## Files

### `GET /v1/files/list?path=<dir>`
List backend-side directory. Returns `FileEntry[]` with `name`, `path`, `type`, `size`, `modified_time`, `is_fits`.

### `POST /v1/files/header`
```json
{ "path": "/data/cube.fits" }
```
Returns FITS header cards as `string[]`.

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
| `kind` | `image`, `cube`, `dynspec`, `fits-table`, `catalogue`, `vbt`, `compressed`, `directory`, `unknown` |
| `opener` | `dataset` → `/v1/datasets/open`; `catalogue` → `/v1/catalogue/open`; `vbt` → `/v1/vbt/open`; `ask` → the content is ambiguous, let the user choose; `unsupported` → nothing here opens it, say so |
| `open_path` | The path to hand the opener when it differs from `path`: a VBT chosen by its `.bin` opens through the `.head` beside it. Empty means "use `path`". |
| `confident` | `false` when the answer is a guess — a FITS whose header would not parse, for instance |
| `detail` | One sentence naming what was found, shown to the user when we have to ask |

A FITS **table** is reported as `fits-table` / `ask`: it can be a source
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
