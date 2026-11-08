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

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/v1/samp/send` | Send a SAMP message to one / all peers (legacy generic send). |
| `POST` | `/v1/samp/receive` | The client records a received load or position here (row highlights and selections are not forwarded). |
| `GET`  | `/v1/samp/pending` | Pull pending out-bound messages enqueued by the client. |
| `GET`  | `/v1/samp/inbox` | Pull messages addressed to the running session. |

## Status / hub lifecycle

| Method | Path | Purpose |
|--------|------|---------|
| `GET`  | `/v1/samp/status` | Current hub connection state. |
| `POST` | `/v1/samp/hub-status` | Register the local hub status (heartbeat). |
| `POST` | `/v1/samp/connect` | Bring the session up against the active hub. |

## File transfer / registration

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/v1/samp/files/register` | Register a backend-side FITS file as a SAMP-shareable token. |
| `POST` | `/v1/samp/import-url` | Fetch a public URL into the SAMP store, through the guarded fetch (see below). |
| `POST` | `/v1/samp/upload-file` | Stage a file the client holds (`filename`, `content_base64`) in the SAMP store; returns `backend_path`. |
| `GET`  | `/v1/samp/files/{token}` | Serve a previously registered file by token. |
| `POST` | `/v1/samp/send-fits` | Broadcast a FITS to peers via SAMP. |
| `POST` | `/v1/samp/send-catalogue` | Broadcast a catalogue (votable) to peers. |

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
