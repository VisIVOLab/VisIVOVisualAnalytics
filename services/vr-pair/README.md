# VisIVO VR pairing service

A small, self-hostable service that pairs a headset's VR page with a VisIVO
backend through a short code, then steps aside. The page and the backend
exchange a WebRTC offer, answer and ICE candidates through it and then talk
to each other directly. No cube data and no API traffic pass through the
service.

Why it exists: WebXR needs a secure context, so the headset's page is an
HTTPS page, and an HTTPS page may not call a backend on `http://192.168…`. It
may open a WebRTC connection to it, though, and something has to carry the
offer and the answer.

- One Python file, `vr_pair_service.py`, with one dependency (`websockets`).
- State in memory only: a restart drops the pending codes. A backend
  registers its headsets' resume codes again when it reconnects (after a
  restart, only when `VR_PAIR_SECRET` is set).
- No host name is built in. The service reads its settings from `VR_PAIR_*`
  variables; a backend finds it through `VISIVO_VR_PAIR_URL` (see
  [Moving the service](#moving-the-service)).

## Protocol (`visivo-pair/1`)

Every message is a JSON text frame `{"v":1,"type":…}`. Both sides receive
`ice_servers {ice_servers:[…]}` first.

**Host** (a backend), `wss://<service>/v1/host`, an outbound connection kept
open with pings:

| sends | receives |
|---|---|
| `new_code {ttl?, kind:"pair"\|"resume", req?, code?, owner?}` | `code {code, kind, expires_in, owner?, req?}` or `error {reason, req?}` |
| `revoke {code}` | |
| `answer {peer, sdp}`, `ice {peer, candidate}`, `reject {peer, reason}` | `join {peer, offer, kind, code}`, `ice {peer, candidate}` |

`req` is echoed so a host can match answers to requests. A resume code
comes with an `owner` key, which only the host knows (an HMAC of the code
under `VR_PAIR_SECRET`). After a reconnection the host sends `new_code` with
that `code` and `owner` to register the code again; whoever else knows the
code (its page does) cannot register it. With `VR_PAIR_SECRET` set, this
works across a restart of the service too. `join`
names the code the page typed, so a host with several pending codes knows
which one was used.

**Page**, `wss://<service>/v1/join`: sends `join {code, offer}` (the offer as
SDP text or as `{type, sdp}`) and `ice {candidate}`; receives `answer {sdp}`,
`ice {candidate}` or `error {reason}`, with `reason` one of `unknown-code`,
`expired` (a revoked code too), `used`, `rate-limited`, `host-gone`,
`rejected`.

**Codes.**
- `pair`: 6 characters from `23456789ABCDEFGHJKLMNPQRSTUVWXYZ` (no 0/O/1/I,
  30 bits), single use, 300 s by default (at most 900 s). Typed in any case,
  with spaces or a dash.
- `resume`: 32 url-safe characters (192 random bits), case-sensitive, usable
  again until it expires or is revoked, 12 h by default (at most 24 h).
- A used, expired or revoked code is remembered for an hour, so a page is
  told why it failed rather than `unknown-code`.

**Limits.** Every limit counts per *address*: an IPv4 address, or the IPv6
/56 network it belongs to (`VR_PAIR_IPV6_PREFIX`).

- Failed joins: 20 per address in 10 minutes. Once an address is over its
  limit even a right code answers `rate-limited`, so guessing teaches nothing.
- Failed joins over all addresses (600 a minute) are a soft brake, not a wall:
  past it, an address may fail only 3 times in 10 minutes, and a right code
  from an address with fewer failures still pairs. Failing on purpose
  therefore cannot stop everybody else's pairing.
- Joins per code: 10 a minute.
- Connections: WebSocket upgrades per address, 30 a minute (then HTTP 429);
  host connections per address, 4; signalling connections per address, 4;
  open host connections in all, 512, and open pages in all, 128 (then HTTP
  503). Hosts and pages have separate budgets, so neither can crowd out the
  other.
- Codes: new codes per host connection, 30 a minute; per address, 60 a minute
  (however often it reconnects); live codes per host, 64; per address, 128;
  in all, 20 000. Codes remembered after use are capped apart (20 000) and
  never block a new one.
- Pages in signalling per host: 16. A page must send its join within 10 s,
  and its signalling connection lasts at most 120 s.
- Messages are at most 64 KiB. A host sends at most 10 a second (bursts of
  60); a page at most 80 after its join.
- The relay carries only one or two answers and up to 64 candidates (each a
  flat object of at most 2 KiB) per page: never arbitrary data.
- A connection may send 80 KiB at once and 16 KiB a second after that, every
  frame (pings included) counting at least 256 bytes. A client that does not
  read what it is sent is dropped past 256 KiB waiting. Both are dropped
  without a close handshake.

**TURN credentials.** A page needs them before its code is checked (it makes
its offer with them), so any client that reaches `/v1/join` gets some; one
address gets them at most 30 times in 10 minutes, then STUN only. They expire
after `VR_PAIR_TURN_TTL`. With `VR_PAIR_HOST_KEYS` set, only hosts with a key
may connect at all.

**Logging.** Counts and reasons only: never an offer, an answer, a
candidate, a code or a TURN credential. Addresses are logged only once
validated as IP addresses, and lines about clients are capped (10 a minute
per address, 300 a minute in all), so nobody can flood the journal.

## Configuration

| Variable | Default | Meaning |
|---|---|---|
| `VR_PAIR_BIND` | `127.0.0.1` (`0.0.0.0` in the container) | Address to listen on |
| `VR_PAIR_PORT` | `8765` | Port |
| `VR_PAIR_ALLOWED_ORIGINS` | *(localhost only)* | Comma-separated origins a page may join from, e.g. `https://vr.example.org`; `*` for any. Unset: `http(s)://localhost`, `127.0.0.1` and `[::1]` on any port (development) |
| `VR_PAIR_TRUST_PROXY` | off | `1` behind a reverse proxy: the client address is the last `X-Forwarded-For` entry (or `X-Real-IP`). Leave off when clients connect directly, or they choose their own address for the rate limits |
| `VR_PAIR_STUN_URLS` | *(none)* | Comma-separated STUN URLs handed to both sides |
| `VR_PAIR_TURN_URLS` | *(none)* | Comma-separated TURN URLs |
| `VR_PAIR_TURN_SECRET` / `VR_PAIR_TURN_SECRET_FILE` | *(none)* | Shared secret of a TURN server with `use-auth-secret` (coturn): each connection gets credentials that expire |
| `VR_PAIR_TURN_TTL` | `3600` | Lifetime of those credentials, seconds |
| `VR_PAIR_TURN_USER` | `visivo` | Label in the TURN user name (`<expiry>:<label>`) |
| `VR_PAIR_TURN_USERNAME`, `VR_PAIR_TURN_PASSWORD` | *(none)* | Static TURN credentials, for a TURN server without a shared secret |
| `VR_PAIR_CODE_TTL`, `VR_PAIR_CODE_TTL_MAX` | `300`, `900` | Pair code lifetime and cap, seconds |
| `VR_PAIR_RESUME_TTL`, `VR_PAIR_RESUME_TTL_MAX` | `43200`, `86400` | Resume code lifetime and cap, seconds |
| `VR_PAIR_PING_S`, `VR_PAIR_PING_TIMEOUT_S` | `20`, `20` | WebSocket pings; a peer that does not answer is dropped |
| `VR_PAIR_JOIN_SESSION_S` | `120` | Longest signalling connection of a page |
| `VR_PAIR_MAX_HOSTS`, `VR_PAIR_MAX_PAGES` | `512`, `128` | Open host and page connections in all (then 503) |
| `VR_PAIR_CONNS_PER_IP_PER_MIN` | `30` | WebSocket upgrades per address per minute (then 429) |
| `VR_PAIR_HOSTS_PER_IP` | `4` | Host connections per address |
| `VR_PAIR_JOIN_CONNS_PER_IP` | `4` | Page connections per address |
| `VR_PAIR_CODES_PER_MIN` | `30` | New codes per host connection per minute |
| `VR_PAIR_CODES_PER_IP_PER_MIN` | `60` | New codes per host address per minute |
| `VR_PAIR_LIVE_CODES_PER_HOST`, `VR_PAIR_LIVE_CODES_PER_IP` | `64`, `128` | Live codes per host connection and per address |
| `VR_PAIR_MAX_CODES` | `20000` | Live codes in all |
| `VR_PAIR_PEERS_PER_HOST` | `16` | Pages in signalling per host |
| `VR_PAIR_JOINS_PER_CODE_PER_MIN` | `10` | Joins of one code per minute |
| `VR_PAIR_IPV6_PREFIX` | `56` | IPv6 clients are counted per network of this many bits |
| `VR_PAIR_TURN_PER_IP` | `30` | TURN credentials per address per 10 minutes |
| `VR_PAIR_HOST_KEYS` / `VR_PAIR_HOST_KEYS_FILE` | *(none: any host)* | Keys a backend must present to connect as a host (comma-separated, or one per line in the file). A backend gives its key in `VISIVO_VR_PAIR_URL` as `?key=…`, or as `Authorization: Bearer …` |
| `VR_PAIR_JOIN_FAILS_PER_IP` | `20` | Failed joins per address per 10 minutes |
| `VR_PAIR_GLOBAL_FAILS_PER_MIN` | `600` | Failed joins per minute over all addresses that turn the soft brake on |
| `VR_PAIR_STORM_FAILS_PER_IP` | `3` | Failed joins per address per 10 minutes while the brake is on |
| `VR_PAIR_SECRET` / `VR_PAIR_SECRET_FILE` | *(random per start)* | Signs the owner keys of resume codes; set it (e.g. `openssl rand -hex 32`) so a backend's headsets can resume after the service restarts. Prefer the file (mode 600): an empty file stops the service, and a file others may read is reported in the log |
| `VR_PAIR_TLS_CERT`, `VR_PAIR_TLS_KEY` | *(none)* | Serve `wss://` directly, without a proxy |
| `VR_PAIR_LOG_LEVEL` | `INFO` | Log level |

`GET /healthz` answers `ok` for a load balancer or a container health check.

## Running it

### On a development machine, next to the backend

```sh
cd services/vr-pair
python -m pip install -r requirements.txt
VR_PAIR_PORT=8765 python vr_pair_service.py
```

and start the backend with `VISIVO_VR_PAIR_URL=ws://127.0.0.1:8765` and
`VISIVO_VR_PAGE_URL` set to wherever the loader is served on `localhost` (a
secure context, so no certificate is needed). Without allowed origins only
`localhost` pages may join.

### Docker

```sh
docker build -t visivo-vr-pair services/vr-pair
docker run -d --restart unless-stopped -p 127.0.0.1:8765:8765 \
  -e VR_PAIR_ALLOWED_ORIGINS=https://vr.example.org -e VR_PAIR_TRUST_PROXY=1 \
  visivo-vr-pair
```

`docker-compose.example.yml` runs the service and, with `--profile turn`, a
coturn TURN server that shares its secret.

### Apptainer (hosts without Docker)

```sh
cd services/vr-pair
apptainer build vr-pair.sif vr-pair.def
VR_PAIR_BIND=127.0.0.1 VR_PAIR_PORT=8765 \
VR_PAIR_ALLOWED_ORIGINS=https://vr.example.org VR_PAIR_TRUST_PROXY=1 \
  apptainer run vr-pair.sif
```

Apptainer passes the environment through. To keep it running, start it from
a systemd unit (or the host's process manager) with `Restart=always`, or as
an Apptainer instance:

```sh
apptainer instance start --env VR_PAIR_ALLOWED_ORIGINS=https://vr.example.org vr-pair.sif vrpair
apptainer instance stop vrpair
```

Without a container: any Python 3.11 or later with `pip install -r
requirements.txt`, then `python vr_pair_service.py`.

### Behind a reverse proxy

Most hosts already have nginx or Apache with a certificate. Keep the service
on `127.0.0.1:8765`, set `VR_PAIR_TRUST_PROXY=1`, and let the proxy terminate
TLS and forward the two WebSocket paths. The proxy's read timeout must exceed
the ping interval (20 s), or it closes idle host connections.

nginx:

```nginx
location /v1/ {
    proxy_pass http://127.0.0.1:8765;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;   # one hop: the client itself
    proxy_read_timeout 120s;
    proxy_send_timeout 120s;
}
location = /healthz { proxy_pass http://127.0.0.1:8765; }
```

Apache 2.4.47 or later (`mod_proxy`, `mod_proxy_http`): use
[`deploy/apache-pair.conf`](deploy/apache-pair.conf), written for a `/pair/`
prefix. Its core is one line per path, each with its own small pool:

```apache
ProxyPass "/pair/v1/host" "http://127.0.0.1:8765/v1/host" upgrade=websocket timeout=120 connectiontimeout=5 max=8 acquire=50
ProxyPass "/pair/v1/join" "http://127.0.0.1:8765/v1/join" upgrade=websocket timeout=120 connectiontimeout=5 max=4 acquire=50
ProxyPass "/pair/healthz" "http://127.0.0.1:8765/healthz" timeout=5 connectiontimeout=5 max=2 acquire=50
```

On a server shared with other sites, `max` is essential. Apache holds a
worker thread for every open WebSocket, and a backend's host connection lasts
for hours, so without it the service can take every worker. Apache appends
the client's address to `X-Forwarded-For` itself. (Older Apache:
`mod_proxy_wstunnel` with `ProxyPass /v1/ ws://127.0.0.1:8765/v1/`.)

The service may share a host name with the loader page or live on its own
(`wss://pair.example.org`); with a prefix (`/vr-pair/v1/…`), strip it in the
proxy so the service sees `/v1/host` and `/v1/join`.

A TURN server is optional: on a network where the headset and the backend's
machine reach each other (the usual home or office network) WebRTC connects
directly. TURN relays the traffic on networks that isolate devices (guest
Wi-Fi), at the cost of the relay's bandwidth.

### Moving the service

Nothing names the server in code. To move it:

- on the new server, run the service with the same `VR_PAIR_*` settings;
- on each backend, change `VISIVO_VR_PAIR_URL` (and `VISIVO_VR_PAGE_URL`
  if the loader moves too, then add its origin to `VR_PAIR_ALLOWED_ORIGINS`)
  and restart the backend. A running backend keeps the address it started
  with;
- in the public loader's `config.json` (`web/vr-loader/`), change
  `pair_service` to the new address: the headset's page connects to the
  service too.

## Security

The service is public: anyone can open `/v1/join`, and `/v1/host` unless host
keys are set. It was reviewed against a hostile client in
[#345](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/345).
Operators should know these points:

- **Everything a client can make it hold is bounded**: connections,
  connection attempts, bytes, frames and messages per connection, the output
  waiting for a client that does not read, codes, remembered codes and log
  lines (see *Limits*). Bookkeeping costs the same however many codes are
  held. Malformed, oversized or deeply nested JSON, and fields of the wrong
  type, answer `bad-request`. No client input can end a connection's handler
  with an error.
- **Client addresses.** With `VR_PAIR_TRUST_PROXY=1` the address is the
  *last* `X-Forwarded-For` entry: the one the proxy appended or set. What a
  client writes in the header itself is ignored. The proxy must append (Apache)
  or replace (`proxy_set_header X-Forwarded-For $remote_addr` in nginx) that
  header. Never turn the setting on without a proxy in front, or clients pick
  their own address. IPv6 clients are counted per /56.
- **Origins** are compared without case and without a trailing slash, against
  the exact scheme, host and port. `null`, a missing or repeated `Origin`, a
  path, an explicit default port or another host are refused. The check keeps
  pages of other web sites out. A program that is not a browser can send any
  `Origin`, so the check is not an authentication.
- **Code guessing.** A pair code holds 30 bits and lives 5 minutes. An address
  gets 20 failures in 10 minutes. While failures over all addresses exceed 600
  a minute, each address gets only 3, but a right code from an address with
  fewer failures still pairs. An attacker spread over many addresses gets 3
  guesses from each. With 10 live codes, one guess hits with probability
  about 1 in 10^8.
- **Shared NAT.** The limits count per address, and an institute's computers
  often share one. On such a site, one colleague's typos (20 in 10 minutes)
  rate-limit every headset of the site for up to 10 minutes. Busy sites hit
  the host and code limits too. For a deployment that serves institutes,
  raise them:

  | Variable | Default | Institute |
  |---|---|---|
  | `VR_PAIR_HOSTS_PER_IP` | 4 | 64 |
  | `VR_PAIR_JOIN_CONNS_PER_IP` | 4 | 64 |
  | `VR_PAIR_CONNS_PER_IP_PER_MIN` | 30 | 600 |
  | `VR_PAIR_CODES_PER_IP_PER_MIN` | 60 | 300 |
  | `VR_PAIR_LIVE_CODES_PER_IP` | 128 | 512 |
  | `VR_PAIR_JOIN_FAILS_PER_IP` | 20 | 60 |

  A higher failure limit gives one address more guesses (60 in 10 minutes:
  still about 1 in 10^6 per 10 minutes against 10 live codes). Counting
  failures more finely (per connection, or forgiving typos close to a live
  code) would either let an attacker reset its count by reconnecting or tell
  it how close it came, so the service does not do it.
- **What remains.** The caps on connections protect the machine, not the
  service's availability. An attacker with several addresses can hold them
  full, and pairing then answers 503 until the connections end (a page's
  last at most 120 s, a host's for as long as it pings).
  - Behind Apache the effective cap is the tunnel pool of
    `deploy/apache-pair.conf`: `max` per Apache child, here 8 hosts and 4
    pages each. With the default 4 connections per address, a handful of
    addresses can fill it.
  - A client can also get 503 from a full child while another child still
    has room.
  - Raising the per-address limits for institutes (below) makes this easier.
    Raise Apache's `MaxRequestWorkers` and `max` together before relying on
    many simultaneous backends.
  - Host keys (`VR_PAIR_HOST_KEYS`) close `/v1/host` to unknown backends,
    where every backend can be given one.
  - On some mobile networks unrelated users share one IPv6 /56. During a
    guessing storm they then share its 3 failures.
- **Secrets.** Keep `VR_PAIR_SECRET` in a file of mode 600, read with
  `VR_PAIR_SECRET_FILE`, not in a unit's `Environment=` (which `systemctl
  show` prints to any local user). Secrets never appear in the log or in the
  configuration's text form.
- **Dependency.** `websockets` is pinned (`requirements.txt`). The service
  extends its server connection, so an upgrade is reviewed. Rerun the tests
  and `tools/attack_harness.py` before you raise it.
- **Errors** tell a client only a reason from the protocol (`bad-request`,
  `rate-limited`, ...), or an HTTP status with a fixed one-line body (403,
  404, 429, 503). The server sends no `Server` header and no tracebacks.

### Host hardening

[`deploy/`](deploy/) holds the files for a Debian host with Apache in front:

- [`visivo-vr-pair.service`](deploy/visivo-vr-pair.service), a systemd unit
  that confines the process. It has no capabilities, sees no other user's
  files and no other site's files, no devices and no other processes, and
  cannot write to disk. It cannot map memory both writable and executable, is
  limited to an ordinary service's system calls, talks to this machine only,
  can bind its own port only, and is bounded in memory, tasks, files and CPU.
  Each line says what it does. `systemd-analyze security` scores it 0.9
  (SAFE), against 8.4 (EXPOSED) for a unit with only `NoNewPrivileges`,
  `PrivateTmp`, `ProtectSystem=strict` and `ProtectHome=read-only`. Measured
  with systemd 257 on Debian 13, where the service ran and paired under it.
- [`apache-pair.conf`](deploy/apache-pair.conf), the proxy block: the three
  paths only, a small pool of tunnels per path, GET only, no request body,
  and mod_reqtimeout against slow clients.

## Tests

```sh
cd services/vr-pair
python -m pip install -r requirements-dev.txt
python -m pytest -q
```

They start the service on a free port of `127.0.0.1` and need no network.

`tools/attack_harness.py` runs the attacks of the security review against a
local copy with the production settings. It reports the service's memory, CPU,
descriptors, threads and log lines, and whether a legitimate pairing still
succeeds during each attack. It can also go through a private local Apache
(`--apache`), and measure what one new code costs when many are held
(`--micro`). It connects to 127.0.0.1 only; never point load at a public
server.
