"""VisIVO VR pairing service, protocol ``visivo-pair/1`` (issue #211).

A headset's page and a VisIVO backend find each other through a short code,
exchange a WebRTC offer, answer and ICE candidates here, and then talk to
each other directly. No cube data and no API traffic ever passes through
this service: it only relays the few signalling messages, and it forgets a
code once it expired.

Why a service at all: WebXR needs a secure context, so the headset's page is
an HTTPS page, and an HTTPS page may not call a backend on ``http://192.168…``.
It may open a WebRTC connection to it, though, and WebRTC needs somebody to
carry the offer and answer: that is all this does.

Two WebSocket endpoints, every message a JSON text frame ``{"v":1,"type":…}``:

``/v1/host`` (a backend, outbound, kept open with pings)
    ``new_code {ttl, kind:"pair"|"resume", req?}`` → ``code {code, kind,
    expires_in, req?}``; ``revoke {code}``. It receives ``join {peer, offer,
    kind, code}`` and ``ice {peer, candidate}``, and sends ``answer {peer,
    sdp}``, ``ice {peer, candidate}`` and ``reject {peer, reason}``.
``/v1/join`` (a page)
    ``join {code, offer}`` and ``ice {candidate}``; it receives ``answer
    {sdp}``, ``ice {candidate}`` or ``error {reason}``.

Both receive ``ice_servers {ice_servers:[…]}`` first: STUN, and TURN with
time-limited credentials when a TURN secret is configured (coturn's
``use-auth-secret``: the user name is ``<expiry>:<user>``, the password the
base64 HMAC-SHA1 of the user name under the shared secret).

Two additions to the host side beyond the contract text, both additive and
used by the VisIVO backend only: ``join`` names the ``code`` (a backend with
several pending codes must know which one the headset typed), and
``new_code`` may carry a ``code`` and its ``owner`` key for a ``resume`` code
the host issued before (after a reconnection to this service, its headsets'
resume codes are registered again; the key, given with the code and known
to the host only, keeps a page that knows the code from taking it over).
Hosts may be required to present a key (``VR_PAIR_HOST_KEYS``).

A ``pair`` code is 6 characters from a 32-symbol alphabet without 0/O/1/I
(30 bits), single use, 300 s by default. A ``resume`` code holds 192 random
bits (url-safe), may be used again until it expires or is revoked, 12 h by
default. Codes are held in memory only: a restart drops them, and a backend
registers its resume codes again when it reconnects (after a restart, only
when ``VR_PAIR_SECRET`` is set: it signs the owner keys).

Never logged: offers, answers, candidates, codes, TURN credentials. Only
counts and reasons.

The service is public: anyone may open ``/v1/join``, and ``/v1/host`` unless
host keys are set. Everything a client can make it hold is therefore
bounded (README.md, "Security"): host and page connections, per address,
connection attempts per address, bytes and messages per connection, the
write buffer of a client that does not read, live codes per host, per
address and in all, remembered codes, the relayed answers and candidates of
a page. A client that breaks a bound is refused before the upgrade (HTTP
429 or 503) or closed (1008), and costs one log line a minute at most.

Configuration is read from the environment (see README.md); run with
``python vr_pair_service.py``.
"""
from __future__ import annotations

import asyncio
import base64
import contextlib
import hashlib
import hmac
import heapq
import ipaddress
import itertools
import json
import logging
import math
import os
import re
import secrets
import signal
import ssl
import stat
import time
from collections import Counter, OrderedDict, deque
from dataclasses import dataclass, field
from typing import Any, Callable
from urllib.parse import parse_qs

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

PROTOCOL = "visivo-pair/1"
VERSION = 1

#: 32 symbols, none of 0/O/1/I: a code read aloud or typed on a headset's
#: keyboard has no look-alikes. 6 of them are 30 bits.
PAIR_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
PAIR_LEN = 6
#: A resume code: 24 random bytes, 32 url-safe characters (192 bits).
RESUME_BYTES = 24
#: What a host may register again as a resume code: url-safe, at least 22
#: characters (132 bits).
RESUME_RE = re.compile(r"^[A-Za-z0-9_-]{22,128}\Z")
#: An owner key: a url-safe base64 HMAC-SHA256 without padding.
OWNER_RE = re.compile(r"^[A-Za-z0-9_-]{43}\Z")

#: The reasons a page may be told, and what each code state answers.
PAGE_REASONS = ("unknown-code", "expired", "used", "rate-limited", "host-gone", "rejected")
_STATE_REASON = {"used": "used", "expired": "expired", "revoked": "expired", "host-gone": "host-gone"}

_LOCAL_ORIGIN = re.compile(r"^https?://(localhost|127\.0\.0\.1|\[::1\])(:\d{1,5})?$", re.I)

#: The longest ``req`` a host may have echoed, and the largest integer one.
REQ_MAX_LEN = 64
REQ_MAX_INT = 2 ** 53

logger = logging.getLogger("visivo.vr_pair")


def _env_list(env: dict, name: str) -> tuple[str, ...]:
    return tuple(p.strip() for p in env.get(name, "").replace(";", ",").split(",") if p.strip())


def _env_num(env: dict, name: str, default, cast=int):
    raw = env.get(name, "").strip()
    if not raw:
        return default
    try:
        value = cast(raw)
    except ValueError:
        raise SystemExit(f"{name}: not a number: {raw!r}") from None
    if value < 0:
        raise SystemExit(f"{name}: must not be negative")
    return value


@dataclass
class Config:
    """Everything that can be set, with the defaults. ``from_env`` reads the
    ``VR_PAIR_*`` variables (README.md lists them)."""

    bind: str = "127.0.0.1"
    port: int = 8765
    #: Origins a page may join from ("*" for any). Empty: localhost only,
    #: which is the development set-up (the loader served on localhost).
    allowed_origins: tuple[str, ...] = ()
    #: Behind a reverse proxy: the client address from X-Forwarded-For (its
    #: last entry, the one the proxy added) or X-Real-IP.
    trust_proxy: bool = False
    #: IPv6 clients are counted per network of this prefix: a machine is
    #: given a /64 and a customer usually a /56, so a per-address limit over
    #: single IPv6 addresses would hold nobody back. /48 is stricter still.
    ipv6_prefix: int = 56
    stun_urls: tuple[str, ...] = ()
    turn_urls: tuple[str, ...] = ()
    turn_secret: str = field(default="", repr=False)
    turn_ttl: int = 3600
    turn_user: str = "visivo"
    #: Static TURN credentials, for a TURN server without use-auth-secret.
    turn_username: str = ""
    turn_password: str = field(default="", repr=False)
    pair_ttl: int = 300
    pair_ttl_max: int = 900
    resume_ttl: int = 12 * 3600
    resume_ttl_max: int = 24 * 3600
    #: How long a used, expired or revoked code is remembered, so a page is
    #: told "used" or "expired" rather than "unknown-code".
    tombstone_s: int = 3600
    ping_interval: float = 20.0
    ping_timeout: float = 20.0
    #: How long a closing connection waits for the other side's close frame.
    close_timeout: float = 5.0
    #: A page must send its join this soon after connecting.
    join_wait_s: float = 10.0
    #: A page's signalling connection lasts at most this long.
    join_session_s: float = 120.0
    max_message: int = 64 * 1024
    #: Messages the library queues for a connection that is not read yet
    #: (each at most ``max_message``); beyond, it stops reading the socket.
    max_queue: int = 4
    #: Bytes waiting to go out to a client that does not read (pongs
    #: included, which the library writes without waiting): beyond, the
    #: connection is dropped.
    write_buffer_max: int = 256 * 1024
    #: Bytes a connection may send: a bucket of ``recv_burst`` bytes (at
    #: least one largest message and its framing) that refills at
    #: ``recv_rate`` bytes a second. Every frame, pings included, costs at
    #: least ``frame_cost``: the work is per frame, and an empty ping is 6
    #: bytes. Signalling is a few kilobytes; beyond, the client is dropped.
    recv_burst: int = 80 * 1024
    recv_rate: int = 16 * 1024
    frame_cost: int = 256
    #: Open WebSocket connections of hosts and of pages, each its own
    #: budget: beyond, an upgrade is answered 503 (the file descriptors and
    #: memory stay bounded: at most about 0.5 MiB each, so 640 of them fit
    #: the unit's MemoryMax). Apart, so idle hosts held open by somebody
    #: cannot keep pages from joining the hosts that are there, nor pages
    #: keep backends out.
    max_hosts: int = 512
    max_pages: int = 128
    #: Upgrade requests per address per minute: beyond, 429.
    conns_per_ip_per_min: int = 30
    host_conns_per_ip: int = 4
    #: Messages a host may send: a bucket of ``host_msg_burst`` refilled at
    #: ``host_msgs_per_s``; beyond, the connection is closed.
    host_msgs_per_s: float = 10.0
    host_msg_burst: int = 60
    codes_per_host_per_min: int = 30
    live_codes_per_host: int = 64
    #: Live codes per host address, whatever its connections.
    live_codes_per_ip: int = 128
    peers_per_host: int = 16
    #: Candidates a page may send in one signalling connection, and that its
    #: host may send to it; answers a host may send to one page.
    candidates_per_peer: int = 64
    answers_per_peer: int = 2
    #: The largest candidate relayed, as JSON.
    candidate_max: int = 2048
    #: Live codes in all, and codes remembered after they stopped being
    #: live (only so a page is told why; the oldest are forgotten first).
    max_codes: int = 20_000
    max_tombstones: int = 20_000
    #: New codes per host address per minute, whatever its connections.
    codes_per_ip_per_min: int = 60
    #: Keys a host must present (``?key=`` or ``Authorization: Bearer``);
    #: empty: any host may connect.
    host_keys: tuple[str, ...] = field(default=(), repr=False)
    #: TURN credentials handed to one address within ``turn_window_s``;
    #: beyond, that address gets STUN only.
    turn_per_ip: int = 30
    turn_window_s: float = 600.0
    join_conns_per_ip: int = 4
    #: Failed joins per address within ``fail_window_s``.
    join_fails_per_ip: int = 20
    fail_window_s: float = 600.0
    #: Joins of one code per minute (a resume code may be used again).
    joins_per_code_per_min: int = 10
    #: Failed joins per minute over all addresses (guessing from many). It
    #: is a soft brake, not a wall: beyond it, an address may fail only
    #: ``storm_fails_per_ip`` times in ``fail_window_s`` instead of
    #: ``join_fails_per_ip``, and a right code from an address with fewer
    #: failures still pairs. A hard global wall would let one attacker stop
    #: every pairing in the world by failing on purpose.
    global_fails_per_min: int = 600
    storm_fails_per_ip: int = 3
    #: Log lines about one address per minute (refusals, connections); the
    #: rest are only counted.
    log_lines_per_ip_per_min: int = 10
    #: Log lines about clients per minute over all addresses.
    log_lines_per_min: int = 300
    tls_cert: str = ""
    tls_key: str = ""
    #: Signs the owner keys of resume codes. Random per start when unset:
    #: then resume codes cannot be registered again after a restart.
    secret: str = field(default="", repr=False)

    @classmethod
    def from_env(cls, env: dict | None = None) -> "Config":
        env = dict(os.environ if env is None else env)
        secret = env.get("VR_PAIR_TURN_SECRET", "")
        secret_file = env.get("VR_PAIR_TURN_SECRET_FILE", "").strip()
        if secret_file:
            secret = _read_secret(secret_file, "VR_PAIR_TURN_SECRET_FILE")
        keys = _env_list(env, "VR_PAIR_HOST_KEYS")
        keys_file = env.get("VR_PAIR_HOST_KEYS_FILE", "").strip()
        if keys_file:
            keys += tuple(line.strip() for line in _read_secret(keys_file, "VR_PAIR_HOST_KEYS_FILE").splitlines()
                          if line.strip() and not line.startswith("#"))
        n = lambda name, default, cast=int: _env_num(env, name, default, cast)  # noqa: E731
        c = cls(
            max_hosts=n("VR_PAIR_MAX_HOSTS", cls.max_hosts),
            max_pages=n("VR_PAIR_MAX_PAGES", cls.max_pages),
            conns_per_ip_per_min=n("VR_PAIR_CONNS_PER_IP_PER_MIN", cls.conns_per_ip_per_min),
            join_conns_per_ip=n("VR_PAIR_JOIN_CONNS_PER_IP", cls.join_conns_per_ip),
            live_codes_per_host=n("VR_PAIR_LIVE_CODES_PER_HOST", cls.live_codes_per_host),
            live_codes_per_ip=n("VR_PAIR_LIVE_CODES_PER_IP", cls.live_codes_per_ip),
            peers_per_host=n("VR_PAIR_PEERS_PER_HOST", cls.peers_per_host),
            max_codes=n("VR_PAIR_MAX_CODES", cls.max_codes),
            joins_per_code_per_min=n("VR_PAIR_JOINS_PER_CODE_PER_MIN", cls.joins_per_code_per_min),
            storm_fails_per_ip=n("VR_PAIR_STORM_FAILS_PER_IP", cls.storm_fails_per_ip),
            ipv6_prefix=min(128, n("VR_PAIR_IPV6_PREFIX", cls.ipv6_prefix)),
            host_keys=keys,
            codes_per_ip_per_min=_env_num(env, "VR_PAIR_CODES_PER_IP_PER_MIN", cls.codes_per_ip_per_min),
            turn_per_ip=_env_num(env, "VR_PAIR_TURN_PER_IP", cls.turn_per_ip),
            bind=env.get("VR_PAIR_BIND", cls.bind).strip() or cls.bind,
            port=_env_num(env, "VR_PAIR_PORT", cls.port),
            allowed_origins=_env_list(env, "VR_PAIR_ALLOWED_ORIGINS"),
            trust_proxy=env.get("VR_PAIR_TRUST_PROXY", "").strip().lower() in ("1", "true", "yes"),
            stun_urls=_env_list(env, "VR_PAIR_STUN_URLS"),
            turn_urls=_env_list(env, "VR_PAIR_TURN_URLS"),
            turn_secret=secret,
            turn_ttl=_env_num(env, "VR_PAIR_TURN_TTL", cls.turn_ttl),
            turn_user=env.get("VR_PAIR_TURN_USER", cls.turn_user).strip() or cls.turn_user,
            turn_username=env.get("VR_PAIR_TURN_USERNAME", ""),
            turn_password=env.get("VR_PAIR_TURN_PASSWORD", ""),
            pair_ttl=_env_num(env, "VR_PAIR_CODE_TTL", cls.pair_ttl),
            pair_ttl_max=_env_num(env, "VR_PAIR_CODE_TTL_MAX", cls.pair_ttl_max),
            resume_ttl=_env_num(env, "VR_PAIR_RESUME_TTL", cls.resume_ttl),
            resume_ttl_max=_env_num(env, "VR_PAIR_RESUME_TTL_MAX", cls.resume_ttl_max),
            ping_interval=_env_num(env, "VR_PAIR_PING_S", cls.ping_interval, float),
            ping_timeout=_env_num(env, "VR_PAIR_PING_TIMEOUT_S", cls.ping_timeout, float),
            join_session_s=_env_num(env, "VR_PAIR_JOIN_SESSION_S", cls.join_session_s, float),
            host_conns_per_ip=_env_num(env, "VR_PAIR_HOSTS_PER_IP", cls.host_conns_per_ip),
            codes_per_host_per_min=_env_num(env, "VR_PAIR_CODES_PER_MIN", cls.codes_per_host_per_min),
            join_fails_per_ip=_env_num(env, "VR_PAIR_JOIN_FAILS_PER_IP", cls.join_fails_per_ip),
            global_fails_per_min=_env_num(env, "VR_PAIR_GLOBAL_FAILS_PER_MIN", cls.global_fails_per_min),
            tls_cert=env.get("VR_PAIR_TLS_CERT", "").strip(),
            tls_key=env.get("VR_PAIR_TLS_KEY", "").strip(),
            secret=_secret_from(env),
        )
        if bool(c.tls_cert) != bool(c.tls_key):
            raise SystemExit("VR_PAIR_TLS_CERT and VR_PAIR_TLS_KEY go together")
        return c


def _read_secret(path: str, name: str) -> str:
    """A secret file's content, stripped. An empty file is an error (a
    secret that silently became random would end resume after a restart),
    and a file that others than its owner may read is reported."""
    try:
        with open(path, encoding="utf-8") as fh:
            mode = os.fstat(fh.fileno()).st_mode
            text = fh.read().strip()
    except OSError as e:
        raise SystemExit(f"{name}: cannot read {path}: {e.strerror}") from None
    if not text:
        raise SystemExit(f"{name}: {path} is empty")
    if mode & (stat.S_IRWXG | stat.S_IRWXO):
        logger.warning("%s: %s is readable by others than its owner; chmod 600 it", name, path)
    return text


def _secret_from(env: dict) -> str:
    secret = env.get("VR_PAIR_SECRET", "")
    path = env.get("VR_PAIR_SECRET_FILE", "").strip()
    if path:
        secret = _read_secret(path, "VR_PAIR_SECRET_FILE")
    if secret and len(secret) < 32:
        logger.warning("VR_PAIR_SECRET is short (%d characters); use openssl rand -hex 32", len(secret))
    return secret


def normalize_code(raw: str) -> str:
    """A code as typed: a pair code in any case, with spaces or dashes in it
    ("k7q-4mx"), is its 6 upper-case symbols; anything else (a resume code,
    case-sensitive) is kept as given, stripped of surrounding blanks."""
    s = raw.strip()
    t = re.sub(r"[\s-]", "", s).upper()
    if len(t) == PAIR_LEN and all(ch in PAIR_ALPHABET for ch in t):
        return t
    return s


def turn_credentials(secret: str, ttl: int, user: str, now: float | None = None) -> tuple[str, str]:
    """coturn's REST credentials (``use-auth-secret``): the user name is the
    expiry (Unix seconds) and a label, the password the base64 HMAC-SHA1 of
    that name under the shared secret."""
    expiry = int((time.time() if now is None else now) + ttl)
    username = f"{expiry}:{user}"
    digest = hmac.new(secret.encode(), username.encode(), hashlib.sha1).digest()
    return username, base64.b64encode(digest).decode()


def clean_address(raw: str | None) -> str:
    """An address as text the log and the limits can take: a valid IP
    address in its canonical form (an IPv4-mapped IPv6 address as IPv4),
    else ``"invalid"``, so a client-controlled value never reaches the log.
    An IPv6 zone (``fe80::1%eth0``) is free text: refused too."""
    try:
        ip = ipaddress.ip_address((raw or "").strip().strip("[]"))
    except ValueError:
        return "invalid"
    if isinstance(ip, ipaddress.IPv6Address) and ip.scope_id is not None:
        return "invalid"
    if isinstance(ip, ipaddress.IPv6Address) and ip.ipv4_mapped is not None:
        ip = ip.ipv4_mapped
    return str(ip)


def rate_key(address: str, ipv6_prefix: int = 64) -> str:
    """What the per-address limits count: an IPv4 address, or the IPv6
    network of ``ipv6_prefix`` bits it belongs to."""
    if ":" not in address:
        return address
    try:
        return str(ipaddress.ip_network(f"{address}/{ipv6_prefix}", strict=False))
    except ValueError:
        return address


def header_values(headers, name: str) -> list[str]:
    """Every value of a header, never raising: the library's ``get`` fails
    on a header that is repeated."""
    try:
        return list(headers.get_all(name))
    except (AttributeError, LookupError):
        value = headers.get(name) if hasattr(headers, "get") else None
        return [value] if value is not None else []


class TokenBucket:
    """``burst`` tokens, refilled at ``rate`` a second; ``take`` says
    whether ``n`` were there."""

    __slots__ = ("rate", "burst", "level", "at")

    def __init__(self, rate: float, burst: float, now: float) -> None:
        self.rate, self.burst, self.level, self.at = rate, burst, float(burst), now

    def take(self, n: float, now: float) -> bool:
        self.level = min(self.burst, self.level + (now - self.at) * self.rate)
        self.at = now
        if self.level < n:
            return False
        self.level -= n
        return True


def candidate_ok(cand, limit: int) -> bool:
    """A candidate as WebRTC gives it: ``null`` (end of candidates), the
    candidate line, or a flat RTCIceCandidateInit object, at most ``limit``
    bytes as JSON. Nothing else is relayed, so a peer cannot use the service
    as a pipe for arbitrary data."""
    if cand is None:
        return True
    if isinstance(cand, str):
        return len(cand) <= limit
    if not isinstance(cand, dict) or len(cand) > 8:
        return False
    if not all(isinstance(k, str) and (v is None or isinstance(v, (str, int, float))) and not isinstance(v, bool)
               for k, v in cand.items()):
        return False
    return len(json.dumps(cand, separators=(",", ":"))) <= limit


class SlidingCount:
    """Events per key within a sliding window, for at most ``max_keys`` keys
    (the least recently seen dropped first)."""

    def __init__(self, window_s: float, max_keys: int = 10_000) -> None:
        self.window_s = window_s
        self.max_keys = max_keys
        self._seen: OrderedDict[str, deque[float]] = OrderedDict()

    def _hist(self, key: str, now: float) -> deque[float]:
        hist = self._seen.pop(key, None) or deque()
        while hist and now - hist[0] >= self.window_s:
            hist.popleft()
        self._seen[key] = hist
        while len(self._seen) > self.max_keys:
            self._seen.popitem(last=False)
        return hist

    def count(self, key: str, now: float) -> int:
        return len(self._hist(key, now))

    def hit(self, key: str, now: float) -> int:
        hist = self._hist(key, now)
        hist.append(now)
        return len(hist)

    def hit_below(self, key: str, limit: int, now: float) -> bool:
        """Count an event unless ``limit`` were already counted (then it is
        refused and leaves no history: a key's memory stays at ``limit``)."""
        hist = self._hist(key, now)
        if len(hist) >= limit:
            return False
        hist.append(now)
        return True


@dataclass(eq=False)
class Code:
    code: str
    kind: str                       # "pair" | "resume"
    host: "HostConn"
    expires: float                  # service clock
    state: str = "live"             # live | used | expired | revoked | host-gone
    forget_at: float = 0.0          # when a code that is no longer live is dropped


@dataclass(eq=False)
class HostConn:
    ws: Any
    ip: str
    id: str = field(default_factory=lambda: secrets.token_hex(4))
    peers: dict[str, "PeerConn"] = field(default_factory=dict)
    issued: deque = field(default_factory=deque)
    alive: bool = True
    #: Its live codes: counted per host without a scan of every code.
    live: set = field(default_factory=set)
    #: The per-address key its codes count against (``rate_key``).
    key: str = ""
    bucket: TokenBucket | None = None

    async def send(self, msg: dict) -> bool:
        try:
            await self.ws.send(json.dumps({"v": VERSION, **msg}, separators=(",", ":")))
            return True
        except ConnectionClosed:
            return False


@dataclass(eq=False)
class PeerConn:
    ws: Any
    id: str
    host: HostConn
    done: asyncio.Event = field(default_factory=asyncio.Event)
    #: What its host sent it so far (bounded: ``answers_per_peer``,
    #: ``candidates_per_peer``).
    answers: int = 0
    candidates: int = 0

    async def send(self, msg: dict) -> bool:
        try:
            await self.ws.send(json.dumps({"v": VERSION, **msg}, separators=(",", ":")))
            return True
        except ConnectionClosed:
            return False


def _parse(raw) -> dict | None:
    """A message, or None for anything that is not ``{"v":1,"type":str,…}``."""
    if not isinstance(raw, str):
        return None
    try:
        msg = json.loads(raw)
    except (ValueError, RecursionError):        # deep nesting raises RecursionError
        return None
    if (not isinstance(msg, dict) or msg.get("v") != VERSION or isinstance(msg.get("v"), bool)
            or not isinstance(msg.get("type"), str) or len(msg["type"]) > 32):
        return None
    return msg


def _offer_ok(offer) -> bool:
    """An offer as a page sends it: the SDP text, or ``{type:"offer", sdp}``."""
    if isinstance(offer, str):
        return offer.startswith("v=")
    return isinstance(offer, dict) and isinstance(offer.get("sdp"), str) and offer["sdp"].startswith("v=")


def bounded_connection(service: "PairService") -> type[ServerConnection]:
    """The library's server connection with two bounds it does not have:
    what a client may send (a bucket of bytes, each frame of every kind
    charged at least ``frame_cost``: a flood of pings costs like a flood of
    messages, and neither keeps the event loop from everyone else) and bytes
    waiting to go out to a client that does not read (the library answers
    each ping at once, without waiting for the buffer to drain). A client
    past either is dropped.

    The bytes are charged before the library parses them, so one read is at
    most ``recv_burst`` bytes of frames; the per-frame charge then drops a
    client within that read's frames."""
    cfg = service.config
    burst = max(cfg.recv_burst, cfg.max_message + 1024)

    class BoundedConnection(ServerConnection):
        def __init__(self, *args, **kwargs) -> None:
            super().__init__(*args, **kwargs)
            self._inflow = TokenBucket(cfg.recv_rate, burst, time.monotonic())
            self._dropped = False

        def _drop(self, why: str) -> None:
            if not self._dropped:
                self._dropped = True
                service.stats[why] += 1
                self.transport.abort()

        def data_received(self, data: bytes) -> None:
            if self._dropped:
                return
            if not self._inflow.take(len(data), time.monotonic()):
                self._drop("dropped_inflow")
                return
            super().data_received(data)
            if self.transport.get_write_buffer_size() > cfg.write_buffer_max:
                self._drop("dropped_not_reading")

        def process_event(self, event) -> None:
            if self._dropped:
                return
            if not self._inflow.take(cfg.frame_cost, time.monotonic()):
                self._drop("dropped_inflow")
                return
            super().process_event(event)

    return BoundedConnection


class PairService:
    """The codes, hosts and peers in memory, and the two connection handlers.
    ``clock`` is the service's monotonic clock (tests replace it).

    Codes are kept so that every operation costs the same however many there
    are: live and remembered codes are counted as they change (per host, per
    address, in all), and two heaps give the next code to expire and the
    next to forget. A heap entry whose code changed meanwhile is skipped,
    and a heap that holds mostly such entries is rebuilt."""

    def __init__(self, config: Config, clock: Callable[[], float] = time.monotonic) -> None:
        self.config = config
        self.clock = clock
        self.codes: dict[str, Code] = {}
        self.live_total = 0
        self.dead_total = 0
        self.ip_live: Counter[str] = Counter()
        self._expiring: list = []          # (expires, seq, Code) of live codes
        self._forgetting: list = []        # (forget_at, seq, Code) of codes no longer live
        self._seq = itertools.count()
        #: Stale heap entries tolerated beyond twice the records held.
        self.heap_slack = 1024
        self.host_conns: Counter[str] = Counter()
        self.join_conns: Counter[str] = Counter()
        #: Open WebSocket connections per endpoint.
        self.active: Counter[str] = Counter()
        self.fails = SlidingCount(config.fail_window_s)
        self.global_fails = SlidingCount(60.0, max_keys=1)
        self.code_joins = SlidingCount(60.0)
        self.ip_codes = SlidingCount(60.0)
        self.turn_handouts = SlidingCount(config.turn_window_s)
        self.attempts = SlidingCount(60.0)
        self._log_budget = SlidingCount(60.0)
        self._log_all = SlidingCount(60.0, max_keys=1)
        self.stats: Counter[str] = Counter()
        self._secret = (config.secret or secrets.token_urlsafe(32)).encode()
        self._swept = 0.0

    # ── Configuration-derived answers ────────────────────────────────────

    def origin_allowed(self, origin: str | None, required: bool) -> bool:
        """Pages must name an allowed origin; a host (no browser, no Origin
        header) is refused only when it claims an origin that is not allowed,
        so a web page cannot act as a host. Compared without case and without
        a trailing slash; a browser sends the origin exactly so, and a client
        that is not a browser can send any value, so the check keeps other
        web sites' pages out, not other programs."""
        if origin is None:
            return not required
        allowed = self.config.allowed_origins
        if not allowed:
            return bool(_LOCAL_ORIGIN.match(origin))
        return "*" in allowed or origin.rstrip("/").lower() in {a.rstrip("/").lower() for a in allowed}

    def ice_servers(self, ip: str | None = None) -> list[dict]:
        """STUN, and TURN unless ``ip`` was handed TURN credentials
        ``turn_per_ip`` times in the window: TURN relays traffic, and a page
        gets its credentials before its code is checked (it needs them to
        make its offer), so their issue is bounded per address."""
        c = self.config
        out: list[dict] = []
        if c.stun_urls:
            out.append({"urls": list(c.stun_urls)})
        turn = bool(c.turn_urls)
        if turn and ip is not None:
            # Counted only when handed out: refusals keep no history.
            if not self.turn_handouts.hit_below(self.key(ip), c.turn_per_ip, self.clock()):
                self.stats["turn_withheld"] += 1
                turn = False
        if turn:
            if c.turn_secret:
                user, cred = turn_credentials(c.turn_secret, c.turn_ttl, c.turn_user)
                out.append({"urls": list(c.turn_urls), "username": user, "credential": cred})
            elif c.turn_username:
                out.append({"urls": list(c.turn_urls), "username": c.turn_username,
                            "credential": c.turn_password})
        return out

    def address(self, conn, request=None) -> str:
        """The client's address. Behind a proxy (``trust_proxy``), the last
        X-Forwarded-For entry: the proxy appends the address it saw (Apache)
        or replaces the header with it (the nginx block in README.md), so
        that entry is the proxy's and whatever a client wrote before it is
        ignored. Repeated headers count as one list, as HTTP defines them."""
        if self.config.trust_proxy:
            h = (request or conn.request).headers
            fwd = ",".join(header_values(h, "X-Forwarded-For"))
            if fwd.strip():
                return clean_address(fwd.split(",")[-1])
            real = header_values(h, "X-Real-IP")
            if real:
                return clean_address(real[-1])
        addr = conn.remote_address
        return clean_address(str(addr[0]) if addr else None)

    def client_ip(self, conn: ServerConnection) -> str:
        return self.address(conn)

    def key(self, ip: str) -> str:
        return rate_key(ip, self.config.ipv6_prefix)

    def log(self, ip: str, fmt: str, *args) -> None:
        """An INFO line about ``ip``, at most ``log_lines_per_ip_per_min``
        a minute for its key and ``log_lines_per_min`` over all: clients
        cannot flood the log, from one address or from many."""
        now = self.clock()
        if (self._log_budget.hit_below(self.key(ip), self.config.log_lines_per_ip_per_min, now)
                and self._log_all.hit_below("*", self.config.log_lines_per_min, now)):
            logger.info(fmt, *args)
        else:
            self.stats["log_suppressed"] += 1

    # ── Codes ─────────────────────────────────────────────────────────────

    def owner_key(self, code: str) -> str:
        """The owner key of a resume code: given to the host that issued it,
        needed to register it again, and never to the page (which knows the
        code). An HMAC of the code, so it needs no record to be checked."""
        digest = hmac.new(self._secret, code.encode(), hashlib.sha256).digest()
        return base64.urlsafe_b64encode(digest).decode().rstrip("=")

    def _push(self, heap: list, when: float, c: Code) -> None:
        heapq.heappush(heap, (when, next(self._seq), c))

    def _compact(self) -> None:
        """Rebuild a heap that holds mostly entries of codes that changed
        since (used or revoked long before they expire, say)."""
        if len(self._expiring) > 2 * self.live_total + self.heap_slack:
            self._expiring = [e for e in self._expiring if e[2].state == "live" and self.codes.get(e[2].code) is e[2]]
            heapq.heapify(self._expiring)
            self.stats["heap_rebuilds"] += 1
        if len(self._forgetting) > 2 * self.dead_total + self.heap_slack:
            self._forgetting = [e for e in self._forgetting
                                if e[2].state != "live" and self.codes.get(e[2].code) is e[2]]
            heapq.heapify(self._forgetting)
            self.stats["heap_rebuilds"] += 1

    def _add(self, c: Code) -> None:
        old = self.codes.get(c.code)
        if old is not None:
            if old.state == "live":
                self._retire(old, "revoked", self.clock())
            self._forget(old)           # unless the tombstone cap already did
        self.codes[c.code] = c
        c.host.live.add(c.code)
        self.ip_live[c.host.key] += 1
        self.live_total += 1
        self._push(self._expiring, c.expires, c)

    def _forget(self, c: Code) -> None:
        if self.codes.get(c.code) is c and c.state != "live":
            del self.codes[c.code]
            self.dead_total -= 1

    def sweep(self, now: float | None = None, force: bool = False) -> None:
        """Live codes past their expiry become ``expired``; codes no longer
        live are forgotten once their tombstone time has passed."""
        now = self.clock() if now is None else now
        if not force and now - self._swept < 1.0:
            return
        self._swept = now
        while self._expiring and self._expiring[0][0] <= now:
            _, _, c = heapq.heappop(self._expiring)
            if c.state == "live" and self.codes.get(c.code) is c:
                self._retire(c, "expired", now, c.expires + self.config.tombstone_s)
        while self._forgetting and self._forgetting[0][0] <= now:
            _, _, c = heapq.heappop(self._forgetting)
            if c.forget_at <= now:
                self._forget(c)
        self._compact()

    def _retire(self, c: Code, state: str, now: float, forget_at: float | None = None) -> None:
        if c.state == "live":
            c.host.live.discard(c.code)
            self.ip_live[c.host.key] -= 1
            if self.ip_live[c.host.key] <= 0:
                del self.ip_live[c.host.key]
            self.live_total -= 1
            self.dead_total += 1
        c.state = state
        c.forget_at = now + self.config.tombstone_s if forget_at is None else forget_at
        self._push(self._forgetting, c.forget_at, c)
        self._cap_tombstones()

    def _cap_tombstones(self) -> None:
        """At most ``max_tombstones`` codes kept after they stopped being
        live; the ones to be forgotten soonest go first."""
        while self.dead_total > self.config.max_tombstones and self._forgetting:
            _, _, c = heapq.heappop(self._forgetting)
            self._forget(c)

    def issue(self, host: HostConn, kind: str, ttl, wanted: str | None = None,
              owner: str | None = None) -> tuple[dict | None, str | None]:
        """A new code for ``host`` (or ``wanted``, a resume code it issued
        before, with the ``owner`` key it was given then), or ``(None,
        reason)``. A resume code's answer carries its ``owner`` key."""
        cfg = self.config
        now = self.clock()
        self.sweep(now)
        if not host.key:
            host.key = self.key(host.ip)
        if kind not in ("pair", "resume"):
            return None, "bad-request"
        default, cap = (cfg.pair_ttl, cfg.pair_ttl_max) if kind == "pair" else (cfg.resume_ttl, cfg.resume_ttl_max)
        if ttl is None:
            ttl = default
        # An int is compared as is (math.isfinite overflows on a huge one).
        if (isinstance(ttl, bool) or not isinstance(ttl, (int, float))
                or (isinstance(ttl, float) and not math.isfinite(ttl)) or ttl <= 0):
            return None, "bad-request"
        ttl = max(1, int(min(ttl, cap)))
        while host.issued and now - host.issued[0] >= 60.0:
            host.issued.popleft()
        # Per connection and per address: reconnecting starts no new budget.
        if (len(host.issued) >= cfg.codes_per_host_per_min
                or self.ip_codes.count(host.key, now) >= cfg.codes_per_ip_per_min):
            return None, "rate-limited"
        held = None
        if wanted is not None:
            if kind != "resume" or not isinstance(wanted, str) or not RESUME_RE.match(wanted):
                return None, "bad-request"
            # The owner key is checked against the code itself, so it holds
            # whatever the service remembers (a forgotten record, a restart
            # with the same VR_PAIR_SECRET). Only url-safe text can match, and
            # only such text is encoded (a lone surrogate cannot be).
            given = owner if isinstance(owner, str) and OWNER_RE.match(owner) else ""
            if not hmac.compare_digest(self.owner_key(wanted).encode(), given.encode()):
                return None, "code-taken"
            held = self.codes.get(wanted)
            if held is not None and held.state != "live":
                held = None
        # A live code registered again with its owner key moves to this
        # host (the backend reconnected while its old connection is not yet
        # known to be dead): the number of live codes does not grow, so the
        # caps do not apply.
        if held is None and (len(host.live) >= cfg.live_codes_per_host
                             or self.ip_live[host.key] >= cfg.live_codes_per_ip
                             or self.live_total >= cfg.max_codes):
            return None, "too-many-codes"
        if wanted is not None:
            code = wanted
        else:
            for _ in range(32):
                code = ("".join(secrets.choice(PAIR_ALPHABET) for _ in range(PAIR_LEN))
                        if kind == "pair" else secrets.token_urlsafe(RESUME_BYTES))
                if code not in self.codes:
                    break
            else:
                return None, "too-many-codes"
        answer: dict = {"code": code, "kind": kind, "expires_in": ttl}
        if kind == "resume":
            answer["owner"] = self.owner_key(code)
        host.issued.append(now)
        self.ip_codes.hit(host.key, now)
        self._add(Code(code=code, kind=kind, host=host, expires=now + ttl))
        self.stats[f"issued_{kind}"] += 1
        return answer, None

    def revoke(self, host: HostConn, raw: str) -> bool:
        c = self.codes.get(normalize_code(raw)) if isinstance(raw, str) and len(raw) <= 256 else None
        if c is None or c.host is not host or c.state != "live":
            return False
        self._retire(c, "revoked", self.clock())
        self.stats["revoked"] += 1
        return True

    def claim(self, raw, ip: str) -> tuple[Code | None, str | None]:
        """The code a page joins with, consumed when it is a pair code, or
        ``(None, reason)``.

        An address's failures are checked first, so a guesser over its limit
        learns nothing: even a right code answers ``rate-limited``. The
        global count is a soft brake: while failures over all addresses
        exceed ``global_fails_per_min``, an address's limit drops to
        ``storm_fails_per_ip``. A user who types a right code from an address
        with fewer failures still pairs, so failing on purpose cannot stop
        everyone's pairing; a guesser spread over many addresses gets a few
        guesses from each."""
        cfg = self.config
        now = self.clock()
        self.sweep(now)
        key = self.key(ip)
        storm = self.global_fails.count("*", now) >= cfg.global_fails_per_min
        limit = min(cfg.join_fails_per_ip, cfg.storm_fails_per_ip) if storm else cfg.join_fails_per_ip
        if self.fails.count(key, now) >= limit:
            self.stats["storm_refused" if storm else "fails_refused"] += 1
            return None, "rate-limited"
        c = self.codes.get(normalize_code(raw)) if isinstance(raw, str) and len(raw) <= 256 else None
        if c is not None and c.state == "live" and now >= c.expires:
            self._retire(c, "expired", c.expires)
        reason = None
        if c is None:
            reason = "unknown-code"
        elif c.state != "live":
            reason = _STATE_REASON.get(c.state, "unknown-code")
        if reason is not None:
            self.fails.hit(key, now)
            # Counted up to the threshold only: enough to know there is a
            # storm, and the history stays that size.
            self.global_fails.hit_below("*", cfg.global_fails_per_min, now)
            return None, reason
        if not self.code_joins.hit_below(c.code, cfg.joins_per_code_per_min, now):
            return None, "rate-limited"
        if len(c.host.peers) >= cfg.peers_per_host:
            return None, "rate-limited"
        if c.kind == "pair":
            self._retire(c, "used", now)
        return c, None

    def host_gone(self, host: HostConn) -> None:
        host.alive = False
        now = self.clock()
        for code in list(host.live):
            c = self.codes.get(code)
            if c is not None and c.host is host and c.state == "live":
                self._retire(c, "host-gone", now)
        host.live.clear()

    # ── Connections ───────────────────────────────────────────────────────

    def process_request(self, conn: ServerConnection, request):
        """Routes, connection limits, the Origin check and a health check,
        before the upgrade: a refused client never holds a WebSocket."""
        path = request.path.split("?", 1)[0]
        if path in ("/healthz", "/v1/healthz"):
            return conn.respond(200, "ok\n")
        if path not in ("/v1/host", "/v1/join"):
            return conn.respond(404, "Not found\n")
        ip = self.address(conn, request)
        if not self.attempts.hit_below(self.key(ip), self.config.conns_per_ip_per_min, self.clock()):
            self.stats["attempts_refused"] += 1
            return conn.respond(429, "Too many connections\n")
        if self.active[path] >= self._cap(path):
            self.stats["busy_refused"] += 1
            return conn.respond(503, "Busy\n")
        origins = header_values(request.headers, "Origin")
        if len(origins) > 1 or not self.origin_allowed(origins[0] if origins else None,
                                                         required=(path == "/v1/join")):
            self.stats["origin_refused"] += 1
            return conn.respond(403, "Origin not allowed\n")
        if path == "/v1/host" and self.config.host_keys and not self.host_key_ok(request):
            self.stats["host_key_refused"] += 1
            return conn.respond(401, "Host key required\n")
        return None

    def host_key_ok(self, request) -> bool:
        """A configured host key in ``?key=`` or ``Authorization: Bearer``."""
        given = ""
        auth = header_values(request.headers, "Authorization")
        if len(auth) == 1 and auth[0].lower().startswith("bearer "):
            given = auth[0][7:].strip()
        if not given and "?" in request.path:
            given = (parse_qs(request.path.split("?", 1)[1]).get("key") or [""])[0]
        ok = False
        for key in self.config.host_keys:
            if hmac.compare_digest(key.encode(), given.encode()):
                ok = True                   # no early exit: the same work for every key
        return ok

    def _cap(self, path: str) -> int:
        return self.config.max_hosts if path == "/v1/host" else self.config.max_pages

    async def handler(self, conn: ServerConnection) -> None:
        path = conn.request.path.split("?", 1)[0]
        ip = self.client_ip(conn)
        self.active[path] += 1
        try:
            if self.active[path] > self._cap(path):     # upgrades that raced past the check
                self.stats["busy_refused"] += 1
                await conn.close(1013, "busy")
            elif path == "/v1/host":
                await self._host(conn, ip)
            else:
                await self._join(conn, ip)
        finally:
            self.active[path] -= 1

    async def _send(self, ws, msg: dict) -> bool:
        try:
            await ws.send(json.dumps({"v": VERSION, **msg}, separators=(",", ":")))
            return True
        except ConnectionClosed:
            return False

    @staticmethod
    async def _kick(ws, reason: str) -> None:
        """Close an abusive connection (1008) without waiting on it: its
        unread messages filled the library's queue, so the library stopped
        reading and would not see the answer to its close frame before
        ``close_timeout``."""
        with contextlib.suppress(Exception):
            await asyncio.wait_for(ws.close(1008, reason), 0.5)
        ws.transport.abort()

    async def _host(self, ws: ServerConnection, ip: str) -> None:
        cfg = self.config
        key = self.key(ip)
        if self.host_conns[key] >= cfg.host_conns_per_ip:
            self.stats["host_refused"] += 1
            await self._send(ws, {"type": "error", "reason": "rate-limited"})
            await ws.close(1013, "too many hosts from this address")
            return
        self.host_conns[key] += 1
        host = HostConn(ws=ws, ip=ip, key=key,
                        bucket=TokenBucket(cfg.host_msgs_per_s, cfg.host_msg_burst, self.clock()))
        self.stats["hosts"] += 1
        self.log(ip, "host %s connected from %s", host.id, ip)
        try:
            await host.send({"type": "ice_servers", "ice_servers": self.ice_servers(ip)})
            async for raw in ws:
                if not host.bucket.take(1, self.clock()):
                    self.stats["host_flood_closed"] += 1
                    await self._kick(ws, "too many messages")
                    break
                msg = _parse(raw)
                if msg is None:
                    await host.send({"type": "error", "reason": "bad-request"})
                    continue
                try:
                    await self._host_message(host, msg)
                except ConnectionClosed:
                    raise
                except Exception:       # noqa: BLE001 - a last resort: no message may end the host
                    self.stats["host_message_failed"] += 1
                    self.log(ip, "host %s: a message failed (%s)", host.id, "internal")
                    logger.debug("host message failed", exc_info=True)
                    await host.send({"type": "error", "reason": "bad-request"})
        except ConnectionClosed:
            pass
        finally:
            self.host_conns[key] -= 1
            if self.host_conns[key] <= 0:
                del self.host_conns[key]
            self.host_gone(host)
            for peer in list(host.peers.values()):
                await peer.send({"type": "error", "reason": "host-gone"})
                peer.done.set()
            self.log(ip, "host %s gone", host.id)

    async def _host_message(self, host: HostConn, msg: dict) -> None:
        cfg = self.config
        t = msg["type"]
        req = msg.get("req")
        echo = ({"req": req} if (isinstance(req, str) and len(req) <= REQ_MAX_LEN)
                or (isinstance(req, int) and not isinstance(req, bool) and abs(req) <= REQ_MAX_INT) else {})
        if t == "new_code":
            answer, reason = self.issue(host, msg.get("kind", "pair"), msg.get("ttl"), msg.get("code"),
                                        msg.get("owner"))
            if answer is None:
                self.stats[f"code_refused_{reason}"] += 1
                await host.send({"type": "error", "reason": reason, **echo})
            else:
                await host.send({"type": "code", **answer, **echo})
        elif t == "revoke":
            self.revoke(host, msg.get("code"))
        elif t in ("answer", "ice", "reject"):
            peer = host.peers.get(msg.get("peer")) if isinstance(msg.get("peer"), str) else None
            if peer is None:
                return          # the page went away meanwhile
            sdp = msg.get("sdp")
            if t == "answer" and (isinstance(sdp, str) or (isinstance(sdp, dict) and isinstance(sdp.get("sdp"), str))):
                if peer.answers >= cfg.answers_per_peer:
                    return
                peer.answers += 1
                self.stats["answers"] += 1
                if isinstance(sdp, dict):        # only what WebRTC needs
                    sdp = {"type": "answer", "sdp": sdp["sdp"]}
                await peer.send({"type": "answer", "sdp": sdp})
            elif t == "ice" and "candidate" in msg and candidate_ok(msg["candidate"], cfg.candidate_max):
                if peer.candidates >= cfg.candidates_per_peer:
                    return
                peer.candidates += 1
                await peer.send({"type": "ice", "candidate": msg["candidate"]})
            elif t == "reject":
                self.stats["rejected"] += 1
                await peer.send({"type": "error", "reason": "rejected"})
                peer.done.set()
        else:
            await host.send({"type": "error", "reason": "bad-request", **echo})

    async def _join(self, ws: ServerConnection, ip: str) -> None:
        cfg = self.config
        key = self.key(ip)
        if self.join_conns[key] >= cfg.join_conns_per_ip:
            await self._send(ws, {"type": "error", "reason": "rate-limited"})
            await ws.close(1013, "too many connections from this address")
            return
        self.join_conns[key] += 1
        peer: PeerConn | None = None
        try:
            await self._send(ws, {"type": "ice_servers", "ice_servers": self.ice_servers(ip)})
            try:
                msg = _parse(await asyncio.wait_for(ws.recv(), cfg.join_wait_s))
            except TimeoutError:
                await ws.close(1008, "no join")
                return
            if msg is None or msg["type"] != "join" or not _offer_ok(msg.get("offer")):
                await ws.close(1008, "a join with a code and an offer was expected")
                return
            code, reason = self.claim(msg.get("code"), ip)
            if code is None:
                self.stats[f"join_{reason}"] += 1
                self.log(ip, "join refused from %s: %s", ip, reason)
                await self._send(ws, {"type": "error", "reason": reason})
                await ws.close(1000)
                return
            host = code.host
            peer = PeerConn(ws=ws, id=secrets.token_urlsafe(9), host=host)
            host.peers[peer.id] = peer
            offer = msg["offer"]
            if isinstance(offer, dict):          # only what WebRTC needs, not what else came with it
                offer = {"type": "offer", "sdp": offer["sdp"]}
            sent = await host.send({"type": "join", "peer": peer.id, "offer": offer,
                                    "kind": code.kind, "code": code.code})
            if not sent or not host.alive:
                await peer.send({"type": "error", "reason": "host-gone"})
                return
            self.stats["joins"] += 1
            self.log(ip, "join %s (%s code) from %s to host %s", peer.id[:4], code.kind, ip, host.id)
            await self._relay(ws, peer, cfg.join_session_s)
        except ConnectionClosed:
            pass
        except Exception:       # noqa: BLE001 - a last resort: one line, no traceback for a client to provoke
            self.stats["join_failed"] += 1
            self.log(ip, "join from %s failed (%s)", ip, "internal")
            logger.debug("join failed", exc_info=True)
        finally:
            self.join_conns[key] -= 1
            if self.join_conns[key] <= 0:
                del self.join_conns[key]
            if peer is not None:
                peer.host.peers.pop(peer.id, None)
            await ws.close(1000)

    async def _relay(self, ws: ServerConnection, peer: PeerConn, limit_s: float) -> None:
        """The page's candidates to its host, until the page leaves, the host
        rejects it or goes, or the signalling time is over. A page that sends
        more messages than a signalling exchange needs is closed."""
        cfg = self.config

        async def pump() -> None:
            left = cfg.candidates_per_peer
            budget = cfg.candidates_per_peer + 16
            async for raw in ws:
                budget -= 1
                if budget < 0:
                    self.stats["page_flood_closed"] += 1
                    await self._kick(ws, "too many messages")
                    return
                msg = _parse(raw)
                if (msg is not None and msg["type"] == "ice" and "candidate" in msg
                        and candidate_ok(msg["candidate"], cfg.candidate_max)):
                    if left <= 0:
                        continue            # a page does not have that many: not relayed
                    left -= 1
                    if not await peer.host.send({"type": "ice", "peer": peer.id,
                                                 "candidate": msg["candidate"]}):
                        return

        pumping = asyncio.ensure_future(pump())
        ended = asyncio.ensure_future(peer.done.wait())
        try:
            await asyncio.wait({pumping, ended}, timeout=limit_s, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in (pumping, ended):
                task.cancel()
            await asyncio.gather(pumping, ended, return_exceptions=True)

    def serve(self, host: str | None = None, port: int | None = None, ssl_context=None):
        """The server (an async context manager, as ``websockets`` gives it)."""
        cfg = self.config
        return serve(self.handler, cfg.bind if host is None else host, cfg.port if port is None else port,
                     process_request=self.process_request, ssl=ssl_context,
                     create_connection=bounded_connection(self),
                     ping_interval=cfg.ping_interval or None, ping_timeout=cfg.ping_timeout or None,
                     max_size=cfg.max_message, max_queue=cfg.max_queue, open_timeout=10,
                     close_timeout=cfg.close_timeout, compression=None, server_header=None)


async def _main() -> None:
    logging.basicConfig(level=os.environ.get("VR_PAIR_LOG_LEVEL", "INFO").upper(),
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    # The websockets library logs handshake failures with the request line,
    # which is harmless, but keeps its frames' content to DEBUG: never raised.
    logging.getLogger("websockets").setLevel(logging.WARNING)
    cfg = Config.from_env()
    ctx = None
    if cfg.tls_cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cfg.tls_cert, cfg.tls_key)
    service = PairService(cfg)
    stop = asyncio.get_running_loop().create_future()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            asyncio.get_running_loop().add_signal_handler(
                sig, lambda: stop.done() or stop.set_result(None))
        except (NotImplementedError, RuntimeError):
            pass
    async with service.serve(ssl_context=ctx):
        logger.info("%s listening on %s:%d (%s); origins: %s; TURN: %s", PROTOCOL, cfg.bind, cfg.port,
                    "wss" if ctx else "ws", ", ".join(cfg.allowed_origins) or "localhost only",
                    "REST credentials" if cfg.turn_urls and cfg.turn_secret
                    else ("static" if cfg.turn_urls and cfg.turn_username else "none"))
        await stop
    logger.info("stopped; %s", dict(service.stats))


if __name__ == "__main__":
    asyncio.run(_main())
