# SAMP

The SAMP router (`backend/app/routers/samp.py`) is mounted without the
router-level token dependency and declares its auth per route. Every route
needs the **operator** token (the primary token once per-user tokens are
configured, since the SAMP hub is machine-wide), except
`GET /v1/samp/files/{token}`, which is public so that other VO tools on the
hub can fetch a registered file by its token.

The desktop client is the SAMP client: it registers with the hub, receives
the hub's callbacks on a loopback XML-RPC endpoint of its own, and sends what
the backend queues. The backend never talks to the hub unless
`VISIVO_SAMP_DIRECT=1` (local backends only).

## Messaging

The backend does not join the SAMP hub itself unless `VISIVO_SAMP_DIRECT=1`
and it runs on the hub's machine. Otherwise the desktop client (`SampClient`)
is the hub client: it forwards what the backend queues and hands the backend
what it receives.

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/v1/samp/send` | `{mtype, params, target_id?}` (no `target_id`: every peer). Sent directly when the backend holds a hub connection, else queued for the desktop client. `{valid, error, job_id, sent_direct}`; an MType outside `image.load.fits`, `table.load.votable`, `table.load.fits`, `coord.pointAt.sky` and `visivo.dataset.open` is `valid: false`. |
| `POST` | `/v1/samp/receive` | The desktop client records a received load or position here (row highlights and selections are handled in the client and not forwarded): `{mtype, params, sender_id, msg_id}` → `{valid, error, msg_id}`. The message is stored in the inbox (any MType); nothing in the backend acts on it. |
| `GET`  | `/v1/samp/pending` | The outbound queue: `{count, messages}`, each message with its `job_id`. Polled by the desktop client, which sends them to the hub. With `lease=true` (the desktop client since #130) nothing is removed: the messages are held for the caller for `lease_seconds` (60, in the answer) and handed out again after that unless acknowledged. Without it the queue is drained as it is answered (older clients). |
| `POST` | `/v1/samp/pending/ack` | `{job_ids}`: remove the leased messages the client has sent → `{acked}` (how many were still queued). |
| `GET`  | `/v1/samp/inbox` | The inbox: `{count, inbox_remaining, messages}`. `limit` (1 to 256, default 32); `drain=true` removes what it returns. |

## Status / hub lifecycle

| Method | Path | Purpose |
|--------|------|---------|
| `GET`  | `/v1/samp/status` | `{direct_connected, direct_client_id, qt_client_status, inbox_size, pending_send}`: the backend's own hub connection, the one the desktop client last reported, and the two queues. |
| `POST` | `/v1/samp/hub-status` | The desktop client reports its hub connection: `{connected, hub_url, client_id, client_name, registered_clients}` → `{ok: true}`. |
| `POST` | `/v1/samp/connect` | `{action: "connect" \| "disconnect"}`: the backend's own hub connection (effective only with `VISIVO_SAMP_DIRECT=1` on the hub's machine). `{ok, connected}`; another action is 400. |

## File transfer / registration

Files shared over SAMP are copied to a temporary store and served by token for
`VISIVO_SAMP_FILE_TTL` seconds (default 3600).

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/v1/samp/files/register` | `{path, filename?}`: register a backend file → `{valid, error, token, url_path}` (`url_path` is `/v1/samp/files/{token}`). |
| `POST` | `/v1/samp/import-url` | `{url, filename?}`: download a URL into the store → `{valid, error, token, backend_path, filename}`. |
| `POST` | `/v1/samp/upload-file` | `{filename, content_base64}`: stage a file of the client's on the backend → `{valid, error, token, backend_path, filename}`. |
| `GET`  | `/v1/samp/files/{token}` | Serve a registered file by token (no auth); 404 `"SAMP file token '<token>' not found or expired."`. |
| `POST` | `/v1/samp/send-fits` | `{path, filename?}`, query `target_id`: register a FITS file and send `image.load.fits` → as `/send`. Also `deliver` and `rows`/`size_bytes` (below). |
| `POST` | `/v1/samp/send-catalogue` | `{path, format, filename?}`, query `target_id`: stage a catalogue and send `table.load.votable` (`csv`, `votable`/`vot`/`xml`; `ipac`, `tbl` and `speck` converted to VOTable) or `table.load.fits` (`fits`, `fit`, `fits-table`) → as `/send`; another format is `valid: false`. Also `table_id`, `deliver` and `rows`/`size_bytes` (below). |

### Receiving a file from another tool

A SAMP sender gives a URL that is usually local to the hub's machine
(`http://127.0.0.1:2525/dynamic/…` from TOPCAT). A remote backend cannot reach
it, so the client downloads it itself and stages it with `upload-file`, for
local and remote backends alike; a `file:` URL is opened in place by a local
backend. Uploads are limited to 512 MB by the client.

`import-url` fetches with `safe_urlopen`: only `http`/`https`, no private,
loopback or link-local addresses, checked again on every redirect. Hosts listed
in `VISIVO_SAMP_ALLOWED_HOSTS` (comma-separated) are the only ones allowed when
it is set.

### The outbound queue: leases

A client that fetched a message and failed before sending it (it crashed,
the answer was lost, the hub went away) used to lose it, since the fetch
emptied the queue. With `lease=true` a message stays queued until the client
acknowledges it with `POST /v1/samp/pending/ack`, which the desktop client
does once the hub has taken the message (or refused it for good, e.g. an
unknown recipient). A message whose lease runs out is handed out again.

Delivery is therefore **at least once**, not exactly once: a send that timed
out (the hub did not answer in time, though it may have delivered) is not
acknowledged and goes out again after the 60 s lease, and so does every
message of a batch whose acknowledgement failed (the client gives a batch up
after 5 failed posts). The client skips a `job_id` it is still sending. A
queued message waits for a desktop client to poll: with none connected it
stays in the queue, which keeps about the newest 512 messages (older ones
are dropped).

### Pixel positions: `POST /v1/datasets/pixel_to_sky`

`{dataset_id, x, y, z?}` → `{valid, error, ra_deg, dec_deg, frame, lon_deg,
lat_deg}`. The ICRS position of a 0-based pixel of an open image or cube along
the viewer's axes (the dataset's non-degenerate axes, `z` the channel), from
the file's full WCS: rotation, CD/PC matrices, SIP and other distortions,
every projection, and the frame (`frame` and `lon_deg`/`lat_deg` give the
position in the image's own frame). The desktop sends it as
`coord.pointAt.sky` for a clicked pixel. A dataset without sky axes is
`valid: false`; an unknown `dataset_id` is 404 with its own detail. A backend
without the route gets the client's own conversion instead, from CRVAL, CRPIX
and CDELT (TAN, SIN, otherwise linear), with no rotation or distortion.

### Sending: `deliver`, `rows`, `size_bytes`

`send-fits` and `send-catalogue` stage the file off the event loop (a 2.3
million-row table takes about 40 s; `/v1/health` keeps answering). A table
converted to VOTable (CSV, IPAC, `.speck`) is written as BINARY2. A `csv`
table is any delimited text, read with the separator `/v1/catalogue/open`
detects (comma, tab or semicolon).

With `"deliver": "return"` the file is staged but the message is neither sent
nor queued: the response carries it as `message` (`{"mtype", "params"}`), with
the `__BACKEND_BASE_URL__` placeholder in its URL. The desktop client sends it
itself as a SAMP call (`samp.hub.callAll`) and reports each tool's answer. The
default, `"queue"`, keeps the old behaviour (direct connection, else the
outbound queue). Both report `rows` (when the backend read the table, else -1)
and `size_bytes` of the staged file.

### Sending a catalogue: `table_id`

`POST /v1/samp/send-catalogue` takes an optional `table_id`. When given, the
`table.load.*` message carries it as `table-id`, so that the receiving tool and
VisIVO can name the table in `table.highlight.row` and `table.select.rowList`
(SAMP matches a table by `table-id` or `url`). The desktop client sends
`visivo-<random>`.

### Sending a subset: `row_indices`

`POST /v1/samp/send-catalogue` takes an optional `row_indices`: 0-based row
positions in the table as `/v1/catalogue/open` reads it (the `row_index` that
`/v1/catalogue/query` reports), at most 5 000 000. Only those rows are staged,
in the order given, as a new BINARY2 VOTable named `<stem of filename>.vot`,
and the message is `table.load.votable` whatever the source format. A VOTable
or FITS source is subset as an astropy table, so its columns keep their units
(and a VOTable's UCDs and descriptions). An empty list, or a row outside the
table, is `valid: false`. The response carries `subset: true` when the rows
were honoured: a backend that predates `row_indices` ignores it and stages the
whole table, and the client refuses to send that as the subset. `rows` is the
number of rows sent.

The client gives the subset a table-id of its own and keeps which catalogue
row each of its rows is, so row messages about the subset are mapped back to
the catalogue (and the other way round).

### Row numbers

SAMP names rows by their 0-based position in the original table.
`POST /v1/catalogue/query` returns `row_index`, one per returned row: its
position in the table as opened, before any filter, sort or sample. The client
maps SAMP rows through it. See [datasets](datasets.md).
