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

**Limits.** Failed joins per address (20 per 10 minutes) and over all
addresses (600 a minute); once an address is over its limit even a right
code answers `rate-limited`, so guessing teaches nothing. Joins per code
(10 a minute), host connections per address (8), new codes per host
connection (30 a minute) and per address (60 a minute, however often it
reconnects), live codes per host (64), live codes in all (100 000; codes
remembered after use are capped apart and never block a new one), pages in
signalling per
host (16), signalling connections per address (16). A page must send its join
within 10 s, and its signalling connection lasts at most 120 s. Messages are
at most 64 KiB, and a page relays at most 64 candidates.

**TURN credentials.** A page needs them before its code is checked (it makes
its offer with them), so any client that reaches `/v1/join` gets some; one
address gets them at most 30 times in 10 minutes, then STUN only. They expire
after `VR_PAIR_TURN_TTL`. With `VR_PAIR_HOST_KEYS` set, only hosts with a key
may connect at all.

**Logging.** Counts and reasons only: never an offer, an answer, a
candidate, a code or a TURN credential.

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
| `VR_PAIR_HOSTS_PER_IP` | `8` | Host connections per address |
| `VR_PAIR_CODES_PER_MIN` | `30` | New codes per host connection per minute |
| `VR_PAIR_CODES_PER_IP_PER_MIN` | `60` | New codes per host address per minute |
| `VR_PAIR_TURN_PER_IP` | `30` | TURN credentials per address per 10 minutes |
| `VR_PAIR_HOST_KEYS` / `VR_PAIR_HOST_KEYS_FILE` | *(none: any host)* | Keys a backend must present to connect as a host (comma-separated, or one per line in the file). A backend gives its key in `VISIVO_VR_PAIR_URL` as `?key=…`, or as `Authorization: Bearer …` |
| `VR_PAIR_JOIN_FAILS_PER_IP` | `20` | Failed joins per address per 10 minutes |
| `VR_PAIR_GLOBAL_FAILS_PER_MIN` | `600` | Failed joins per minute over all addresses |
| `VR_PAIR_SECRET` / `VR_PAIR_SECRET_FILE` | *(random per start)* | Signs the owner keys of resume codes; set it (e.g. `openssl rand -hex 32`) so a backend's headsets can resume after the service restarts |
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

Apache 2.4.47 or later (`mod_proxy`, `mod_proxy_http`):

```apache
ProxyPass        /v1/ http://127.0.0.1:8765/v1/ upgrade=websocket timeout=120
ProxyPassReverse /v1/ http://127.0.0.1:8765/v1/
ProxyPass        /healthz http://127.0.0.1:8765/healthz
```

(Older Apache: `mod_proxy_wstunnel` with `ProxyPass /v1/ ws://127.0.0.1:8765/v1/`.)
Apache adds `X-Forwarded-For` itself.

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
  with.

## Tests

```sh
cd services/vr-pair
python -m pip install -r requirements-dev.txt
python -m pytest -q
```

They start the service on a free port of `127.0.0.1` and need no network.
