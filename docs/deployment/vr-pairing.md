# Hosting headset pairing

This page is for whoever runs the server that lets users pair a Meta Quest
with VisIVO Visual Analytics by code
([#210](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/210)). Users
see the result in [Pair by code](../user-guide/vr/quickstart.md#pair-by-code).

Pairing needs two things on a public HTTPS server, hosted once for all users:

| Piece | What it is | Where it lives in the repository |
|---|---|---|
| **Pairing service** | A small WebSocket service that hands out codes and relays the WebRTC offer, answer and candidates between a headset and a backend, then steps aside. No cube data and no API traffic pass through it. Optionally a TURN server next to it. | `services/vr-pair/` |
| **Public loader** | A static directory that the headset opens: it asks for the code, holds the WebRTC link and runs the VR page, whose files come from the user's own backend. Installable as an app. | `web/vr-loader/` |

Each backend then names both (`VISIVO_VR_PAIR_URL`, `VISIVO_VR_PAGE_URL`). No
address is built into the application, the backend, the service or the loader:
the operator chooses the server, and moving it means changing a few values
(see [Moving to another server](#moving-to-another-server)).

```
  backend (user's computer) ──outbound wss──►  pairing service  ◄──wss + code── loader (Quest Browser, https)
          │                                                                         │
          └────────────── WebRTC, direct on the local network (or through TURN) ─────┘
```

The service's protocol, its full list of settings with their defaults and
limits, and its container files are described once, in the service's README:
[`services/vr-pair/README.md`](https://github.com/VisIVOLab/VisIVOVisualAnalytics/blob/main/services/vr-pair/README.md).
This page says which of them to use for a deployment and how the pieces fit.

## The example used on this page

One host, `vr.example.org`, with a certificate, serves both pieces behind
nginx or Apache:

| Address | What answers |
|---|---|
| `https://vr.example.org/visivo/` | the loader (static files) |
| `wss://vr.example.org/pair` | the pairing service, on `127.0.0.1:8765`, the `/pair` prefix stripped by the proxy |
| `turn:turn.example.org:3478` | coturn, optional |

The service may as well have its own host name (`wss://pair.example.org`), and
the loader may sit at the root of a host; only the values below change.

## 1. Run the pairing service

The service is one Python file with one dependency (`websockets`), state in
memory only. Run it, on `127.0.0.1:8765`, in one of these ways; the commands are
in the README:

- **Docker:** build `services/vr-pair/` and run it with
  `-p 127.0.0.1:8765:8765` ([README: Docker](https://github.com/VisIVOLab/VisIVOVisualAnalytics/blob/main/services/vr-pair/README.md#docker)).
- **docker compose, with coturn:** copy
  `services/vr-pair/docker-compose.example.yml`, replace the values marked
  `CHANGE`, write the two secrets (`openssl rand -hex 32 > turn_secret.txt`,
  the same for `pair_secret.txt`), then `docker compose up -d` for the service
  alone, or `docker compose --profile turn up -d` with the TURN server. The
  coturn container uses host networking and relays on UDP ports 49160 to 49200:
  open 3478 (UDP and TCP) and that range in the host's firewall.
- **Apptainer**, on hosts without Docker: build `vr-pair.def`, run the image or
  an Apptainer instance, and keep it alive with a systemd unit
  ([README: Apptainer](https://github.com/VisIVOLab/VisIVOVisualAnalytics/blob/main/services/vr-pair/README.md#apptainer-hosts-without-docker)).
- **Plain Python 3.11 or later:** `pip install -r requirements.txt`, then
  `python vr_pair_service.py`.

`GET /healthz` answers `ok`, for a health check.

### The settings to choose

Every setting is a `VR_PAIR_*` environment variable. Their defaults and limits
are in the [README's configuration table](https://github.com/VisIVOLab/VisIVOVisualAnalytics/blob/main/services/vr-pair/README.md#configuration);
this is what each group is for in a deployment.

| Group | Variables | For this deployment |
|---|---|---|
| Listening | `VR_PAIR_BIND`, `VR_PAIR_PORT` | `127.0.0.1` and `8765` behind a proxy (the container image binds `0.0.0.0`: publish the port on `127.0.0.1` only). |
| Pages allowed | `VR_PAIR_ALLOWED_ORIGINS` | **Set it**: the loader's origin, scheme and host only, `https://vr.example.org`. Unset, only `localhost` pages may join. |
| Behind a proxy | `VR_PAIR_TRUST_PROXY` | `1` behind nginx or Apache, so the rate limits see the client's address. Leave it off when clients connect directly. |
| Direct TLS | `VR_PAIR_TLS_CERT`, `VR_PAIR_TLS_KEY` | Only to serve `wss://` without a proxy. |
| Resume across restarts | `VR_PAIR_SECRET`, `VR_PAIR_SECRET_FILE` | **Set it** (`openssl rand -hex 32`). Without it a restart of the service makes every headset pair again with a new code. |
| Which backends | `VR_PAIR_HOST_KEYS`, `VR_PAIR_HOST_KEYS_FILE` | Optional. With keys set, only backends that present one may ask for codes (see [Security](#security)). |
| STUN and TURN | `VR_PAIR_STUN_URLS`, `VR_PAIR_TURN_URLS`, `VR_PAIR_TURN_SECRET`, `VR_PAIR_TURN_SECRET_FILE`, `VR_PAIR_TURN_TTL`, `VR_PAIR_TURN_USER`, `VR_PAIR_TURN_USERNAME`, `VR_PAIR_TURN_PASSWORD` | Optional. With coturn's `use-auth-secret`, give the shared secret and each connection gets credentials that expire after `VR_PAIR_TURN_TTL`; the static user name and password are for a TURN server without a shared secret. |
| Code lifetimes | `VR_PAIR_CODE_TTL`, `VR_PAIR_CODE_TTL_MAX`, `VR_PAIR_RESUME_TTL`, `VR_PAIR_RESUME_TTL_MAX` | The defaults (5 minutes, 12 hours) suit most uses. |
| Rate limits | `VR_PAIR_HOSTS_PER_IP`, `VR_PAIR_CODES_PER_MIN`, `VR_PAIR_CODES_PER_IP_PER_MIN`, `VR_PAIR_TURN_PER_IP`, `VR_PAIR_JOIN_FAILS_PER_IP`, `VR_PAIR_GLOBAL_FAILS_PER_MIN` | See the note below. |
| Connections | `VR_PAIR_PING_S`, `VR_PAIR_PING_TIMEOUT_S`, `VR_PAIR_JOIN_SESSION_S` | The defaults; the proxy's read timeout must exceed the ping interval. |
| Logging | `VR_PAIR_LOG_LEVEL` | `INFO`. The service logs counts and reasons only, never a code, an offer, a candidate or a credential. |

```{note}
The rate limits count per address. Every running backend keeps one host
connection to the service, so behind an institute's NAT, where many users'
computers share one public address, `VR_PAIR_HOSTS_PER_IP` (8 by default)
caps how many backends of that site can be paired at the same time, and
`VR_PAIR_CODES_PER_IP_PER_MIN` (60) how many codes they ask for. Raise both for
a site that shares an address.
```

### TURN

A TURN server is optional. On a network where the headset and the user's
computer reach each other (a home or office Wi-Fi), WebRTC connects directly
and the service is not used after the pairing. On networks that keep devices
apart (guest networks, client or AP isolation), there is no
direct path, and without TURN the headset's link fails. With TURN the traffic
is relayed through it: it works, more slowly, and costs the relay's bandwidth
(a reduced cube is several megabytes). The compose example runs coturn with
the service's TURN secret; give the service its URLs in
`VR_PAIR_STUN_URLS` and `VR_PAIR_TURN_URLS`, with both `?transport=udp` and
`?transport=tcp`.

## 2. Host the loader

1. Copy the directory `web/vr-loader/` to the web server, under any path.
   Here: `/srv/www/visivo/`, served as `https://vr.example.org/visivo/`.
   Everything in it uses relative addresses, the service worker's scope is
   the directory itself, and there is no build step.
2. Edit `config.json` in the copy:

   ```json
   { "pair_service": "wss://vr.example.org/pair" }
   ```

   `pair_service` is the service's base address: the loader adds `/v1/join`.
   It may be relative to `config.json` (`"../pair"` here), for a service behind
   the same proxy. `https://` is read as `wss://`; plain `ws://` is refused
   unless the loader runs on `localhost`.
3. Serve `config.json` with `Cache-Control: no-store`. The other files may be
   cached normally.

**The trailing slash.** The loader resolves `config.json`, its scripts and
its service worker against its own address, so that address must end with
`/` (`https://vr.example.org/visivo/`). The backend adds the slash to
`VISIVO_VR_PAGE_URL` (and so to the desktop's *Headset page* field and its QR
code) when it is missing, unless the last segment names a file such as
`index.html`. Links you give users yourself must include it, and the web
server should redirect `/visivo` to `/visivo/` too (Apache's `mod_dir` does it
by default; nginx needs the line below).

**Types.** Module scripts and service workers are refused unless `.js` comes
with a JavaScript type; `.webmanifest` should come as
`application/manifest+json` for the app install.

## 3. The reverse proxy, with TLS

The proxy terminates TLS, serves the loader and forwards the service's two
WebSocket paths (`/v1/host`, `/v1/join`) with the `/pair` prefix stripped. Its
read timeout must exceed the service's ping interval (20 s). The README has
the service-only blocks
([Behind a reverse proxy](https://github.com/VisIVOLab/VisIVOVisualAnalytics/blob/main/services/vr-pair/README.md#behind-a-reverse-proxy));
here is the whole host.

nginx:

```nginx
server {
    listen 443 ssl;
    server_name vr.example.org;
    ssl_certificate     /etc/letsencrypt/live/vr.example.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/vr.example.org/privkey.pem;

    # The loader: /srv/www/visivo/ is a copy of web/vr-loader/.
    root /srv/www;
    location = /visivo { return 301 /visivo/; }
    location = /visivo/config.json { add_header Cache-Control "no-store" always; }

    # The pairing service, /pair/v1/... -> 127.0.0.1:8765/v1/...
    location /pair/ {
        proxy_pass http://127.0.0.1:8765/;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $remote_addr;   # one hop: the client itself
        proxy_read_timeout 120s;
        proxy_send_timeout 120s;
    }
}
```

If nginx's `mime.types` has no `webmanifest` entry, add
`application/manifest+json webmanifest;` to it (a `types` block inside
`server` would replace every inherited type, `.js` included).

Apache 2.4.47 or later (`mod_ssl`, `mod_proxy`, `mod_proxy_http`, `mod_headers`):

```apache
<VirtualHost *:443>
    ServerName vr.example.org
    SSLEngine on
    SSLCertificateFile    /etc/letsencrypt/live/vr.example.org/fullchain.pem
    SSLCertificateKeyFile /etc/letsencrypt/live/vr.example.org/privkey.pem

    # The loader: /srv/www/visivo/ is a copy of web/vr-loader/.
    DocumentRoot /srv/www
    AddType application/manifest+json .webmanifest
    <Location "/visivo/config.json">
        Header always set Cache-Control "no-store"
    </Location>

    # The pairing service, /pair/v1/... -> 127.0.0.1:8765/v1/...
    ProxyPass        /pair/ http://127.0.0.1:8765/ upgrade=websocket timeout=120
    ProxyPassReverse /pair/ http://127.0.0.1:8765/
</VirtualHost>
```

(Older Apache: `mod_proxy_wstunnel` with `ProxyPass /pair/v1/ ws://127.0.0.1:8765/v1/`.)
Apache adds `X-Forwarded-For` itself.

The service then runs with at least:

```sh
VR_PAIR_BIND=127.0.0.1 VR_PAIR_PORT=8765 \
VR_PAIR_ALLOWED_ORIGINS=https://vr.example.org \
VR_PAIR_TRUST_PROXY=1 \
VR_PAIR_SECRET_FILE=/etc/visivo-vr-pair/secret \
python vr_pair_service.py
```

**Check it.** `curl https://vr.example.org/pair/healthz` answers `ok`. Opening
`https://vr.example.org/visivo/` in a desktop browser shows the code form and,
under it, *Pairing service: vr.example.org*; an unknown code such as `AAAAAA`
answers *This code is not known*, which proves the loader reached the service
and the service accepted its origin.

## 4. Configure the backends and the desktop

A backend pairs headsets only when it knows the service and the loader:

| Variable | Value here | Effect |
|---|---|---|
| `VISIVO_VR_PAIR_URL` | `wss://vr.example.org/pair` | The service. The backend connects out to its `/v1/host`. A host key goes in the query: `wss://vr.example.org/pair?key=…`. Unset: pairing is off. |
| `VISIVO_VR_PAGE_URL` | `https://vr.example.org/visivo/` | The loader. The desktop's QR code is this address with `#pair=<code>`. |

Both are described with the other backend settings in
[Configuration: VR headset pairing](../configuration.md#vr-headset-pairing),
which also gives the requirements (aiortc and PyAV, in the release bundles;
macOS 14 or newer on Apple Silicon). A backend reads them once, when pairing is
first used: restart it after a change.

**The desktop application** starts its own backend and passes it the two values
of **Settings ▸ Backends ▸ Headset Pairing**:

| Field | Passed as |
|---|---|
| *Pairing service* | `VISIVO_VR_PAIR_URL` |
| *Headset page* | `VISIVO_VR_PAGE_URL` |

Both fields are empty after installation: give users the two values to type
there. The same variables in the environment the application starts from take
precedence over the fields; the change applies at the backend's next start, so
users restart the application. A backend started by hand, or a remote one,
uses its own environment (on a cluster, export them with the `APPTAINERENV_`
prefix as in [Other backend settings](hpc-slurm.md#other-backend-settings)).

## Moving to another server

Nothing names the server in code. Each side holds exactly one value per piece
that moves; change it there and nowhere else.

**The pairing service moves** (to `wss://new.example.org/pair`):

| Side | The one value to change |
|---|---|
| Loader | `pair_service` in its `config.json` |
| Each backend | `VISIVO_VR_PAIR_URL` (for the desktop's own backend: the *Pairing service* field), then restart it |
| Pairing service | nothing: run it on the new server with the same `VR_PAIR_*` settings, the same `VR_PAIR_SECRET` included |

**The loader moves** (to `https://new.example.org/visivo/`):

| Side | The one value to change |
|---|---|
| Each backend | `VISIVO_VR_PAGE_URL` (for the desktop's own backend: the *Headset page* field), then restart it |
| Pairing service | `VR_PAIR_ALLOWED_ORIGINS`: add the new origin (remove the old one once nobody uses it) |
| Loader | nothing: copy the directory, `config.json` included, unchanged |

Headsets that installed the old loader as an app keep opening the old address:
users open the new one and install it again. Moving either piece ends the
headsets connected at the time (their backends restart); users pair again with
a new code.

## Security

- **Pair codes** are 6 characters from an alphabet without look-alikes (30
  bits), single use, valid 5 minutes. The desktop revokes a code nobody used
  when its dialog closes. Failed joins are rate-limited per address and over
  all addresses; once an address is over its limit even a right code answers
  `rate-limited`, so guessing teaches nothing.
- **Resume codes** are 192 random bits, issued by the backend at pairing, valid
  12 hours and kept in the loader's tab only (session storage). They let a
  dropped link come back without a new code. The desktop's **Disconnect**
  revokes the headset's resume code.
- **Owner keys.** A resume code is registered with an owner key, an HMAC of the
  code under `VR_PAIR_SECRET`, that only the backend holds, so whoever else
  knows the code (the headset does) cannot register it as theirs. Keep
  `VR_PAIR_SECRET` in a file readable only by the service's account.
- **Host keys.** With `VR_PAIR_HOST_KEYS` set, only backends that present a key
  may connect to `/v1/host` and ask for codes; otherwise any backend may use
  the service. The key travels in `VISIVO_VR_PAIR_URL` (`?key=…`); the backend
  and the desktop log that address without its query.
- **Origins.** Only pages from `VR_PAIR_ALLOWED_ORIGINS` may join, so a copy of
  the loader on another site cannot use your service. A loader's
  `?pair_service=` override works only when the loader itself runs on
  `localhost` (development), so a link cannot point your loader at another
  service.
- **The backend token never leaves the user's computer.** It is not in the
  code, the QR code, the loader's address or anything the service relays. The
  backend adds it to the headset's requests itself and removes any token the
  page sends; the pairing routes are not reachable from the headset.
- **What the service sees.** Codes, the WebRTC offer, answer and candidates
  (which carry the computer's and the headset's network addresses), and TURN
  credentials, none of which it logs. Data travels over WebRTC, encrypted end
  to end with DTLS, through TURN too.
- **TURN credentials** are handed to any page that reaches `/v1/join` (a page
  needs them before its code is checked), at most `VR_PAIR_TURN_PER_IP` times
  per address in 10 minutes, and expire after `VR_PAIR_TURN_TTL`. The coturn
  example refuses to relay to loopback and `0.0.0.0/8` addresses; add the
  host's own private networks to its `--denied-peer-ip` list if the TURN host
  sits on an internal network.
