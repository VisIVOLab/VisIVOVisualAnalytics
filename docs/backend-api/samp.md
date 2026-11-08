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
| `GET`  | `/v1/samp/pending` | The outbound queue, drained: `{count, messages}`. Polled by the desktop client, which sends them to the hub. |
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

### Sending: `deliver`, `rows`, `size_bytes`

`send-fits` and `send-catalogue` stage the file off the event loop (a 2.3
million-row table takes about 40 s; `/v1/health` keeps answering). A table
converted to VOTable (CSV, IPAC, `.speck`) is written as BINARY2.

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

### Row numbers

SAMP names rows by their 0-based position in the original table.
`POST /v1/catalogue/query` returns `row_index`, one per returned row: its
position in the table as opened, before any filter, sort or sample. The client
maps SAMP rows through it. See [datasets](datasets.md).
