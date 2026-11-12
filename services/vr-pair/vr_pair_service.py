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
``new_code`` may carry a ``code`` for a ``resume`` code the host issued
before (after a reconnection to this service, its headsets' resume codes are
registered again).

A ``pair`` code is 6 characters from a 32-symbol alphabet without 0/O/1/I
(30 bits), single use, 300 s by default. A ``resume`` code holds 192 random
bits (url-safe), may be used again until it expires or is revoked, 12 h by
default. Codes are held in memory only: a restart drops them, and a backend
registers its resume codes again when it reconnects.

Never logged: offers, answers, candidates, codes, TURN credentials. Only
counts and reasons.

Configuration is read from the environment (see README.md); run with
``python vr_pair_service.py``.
"""
from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import json
import logging
import os
import re
import secrets
import signal
import ssl
import time
from collections import Counter, OrderedDict, deque
from dataclasses import dataclass, field
from typing import Any, Callable

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
RESUME_RE = re.compile(r"^[A-Za-z0-9_-]{22,128}$")

#: The reasons a page may be told, and what each code state answers.
PAGE_REASONS = ("unknown-code", "expired", "used", "rate-limited", "host-gone", "rejected")
_STATE_REASON = {"used": "used", "expired": "expired", "revoked": "expired", "host-gone": "host-gone"}

_LOCAL_ORIGIN = re.compile(r"^https?://(localhost|127\.0\.0\.1|\[::1\])(:\d{1,5})?$", re.I)

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
    stun_urls: tuple[str, ...] = ()
    turn_urls: tuple[str, ...] = ()
    turn_secret: str = ""
    turn_ttl: int = 3600
    turn_user: str = "visivo"
    #: Static TURN credentials, for a TURN server without use-auth-secret.
    turn_username: str = ""
    turn_password: str = ""
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
    host_conns_per_ip: int = 8
    codes_per_host_per_min: int = 30
    live_codes_per_host: int = 64
    peers_per_host: int = 16
    max_codes: int = 100_000
    join_conns_per_ip: int = 16
    #: Failed joins per address within ``fail_window_s``.
    join_fails_per_ip: int = 20
    fail_window_s: float = 600.0
    #: Joins of one code per minute (a resume code may be used again).
    joins_per_code_per_min: int = 10
    #: Failed joins per minute over all addresses (guessing from many).
    global_fails_per_min: int = 600
    tls_cert: str = ""
    tls_key: str = ""

    @classmethod
    def from_env(cls, env: dict | None = None) -> "Config":
        env = dict(os.environ if env is None else env)
        secret = env.get("VR_PAIR_TURN_SECRET", "")
        secret_file = env.get("VR_PAIR_TURN_SECRET_FILE", "").strip()
        if secret_file:
            with open(secret_file, encoding="utf-8") as fh:
                secret = fh.read().strip()
        c = cls(
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
        )
        if bool(c.tls_cert) != bool(c.tls_key):
            raise SystemExit("VR_PAIR_TLS_CERT and VR_PAIR_TLS_KEY go together")
        return c


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


@dataclass
class Code:
    code: str
    kind: str                       # "pair" | "resume"
    host: "HostConn"
    expires: float                  # service clock
    state: str = "live"             # live | used | expired | revoked | host-gone
    forget_at: float = 0.0          # when a code that is no longer live is dropped


@dataclass
class HostConn:
    ws: Any
    ip: str
    id: str = field(default_factory=lambda: secrets.token_hex(4))
    peers: dict[str, "PeerConn"] = field(default_factory=dict)
    issued: deque = field(default_factory=deque)
    alive: bool = True

    async def send(self, msg: dict) -> bool:
        try:
            await self.ws.send(json.dumps({"v": VERSION, **msg}, separators=(",", ":")))
            return True
        except ConnectionClosed:
            return False


@dataclass
class PeerConn:
    ws: Any
    id: str
    host: HostConn
    done: asyncio.Event = field(default_factory=asyncio.Event)

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
    except ValueError:
        return None
    if not isinstance(msg, dict) or msg.get("v") != VERSION or not isinstance(msg.get("type"), str):
        return None
    return msg


def _offer_ok(offer) -> bool:
    """An offer as a page sends it: the SDP text, or ``{type:"offer", sdp}``."""
    if isinstance(offer, str):
        return offer.startswith("v=")
    return isinstance(offer, dict) and isinstance(offer.get("sdp"), str) and offer["sdp"].startswith("v=")


class PairService:
    """The codes, hosts and peers in memory, and the two connection handlers.
    ``clock`` is the service's monotonic clock (tests replace it)."""

    def __init__(self, config: Config, clock: Callable[[], float] = time.monotonic) -> None:
        self.config = config
        self.clock = clock
        self.codes: dict[str, Code] = {}
        self.host_conns: Counter[str] = Counter()
        self.join_conns: Counter[str] = Counter()
        self.fails = SlidingCount(config.fail_window_s)
        self.global_fails = SlidingCount(60.0, max_keys=1)
        self.code_joins = SlidingCount(60.0)
        self.stats: Counter[str] = Counter()
        self._swept = 0.0

    # ── Configuration-derived answers ────────────────────────────────────

    def origin_allowed(self, origin: str | None, required: bool) -> bool:
        """Pages must name an allowed origin; a host (no browser, no Origin
        header) is refused only when it claims an origin that is not allowed,
        so a web page cannot act as a host."""
        if origin is None:
            return not required
        allowed = self.config.allowed_origins
        if not allowed:
            return bool(_LOCAL_ORIGIN.match(origin))
        return "*" in allowed or origin.rstrip("/") in {a.rstrip("/") for a in allowed}

    def ice_servers(self) -> list[dict]:
        c = self.config
        out: list[dict] = []
        if c.stun_urls:
            out.append({"urls": list(c.stun_urls)})
        if c.turn_urls:
            if c.turn_secret:
                user, cred = turn_credentials(c.turn_secret, c.turn_ttl, c.turn_user)
                out.append({"urls": list(c.turn_urls), "username": user, "credential": cred})
            elif c.turn_username:
                out.append({"urls": list(c.turn_urls), "username": c.turn_username,
                            "credential": c.turn_password})
        return out

    def client_ip(self, conn: ServerConnection) -> str:
        if self.config.trust_proxy:
            h = conn.request.headers
            fwd = h.get("X-Forwarded-For")
            if fwd:
                return fwd.split(",")[-1].strip() or "?"
            real = h.get("X-Real-IP")
            if real:
                return real.strip()
        addr = conn.remote_address
        return str(addr[0]) if addr else "?"

    # ── Codes ─────────────────────────────────────────────────────────────

    def sweep(self, now: float | None = None, force: bool = False) -> None:
        """Live codes past their expiry become ``expired``; codes no longer
        live are forgotten once their tombstone time has passed."""
        now = self.clock() if now is None else now
        if not force and now - self._swept < 1.0:
            return
        self._swept = now
        for key, c in list(self.codes.items()):
            if c.state == "live" and now >= c.expires:
                c.state, c.forget_at = "expired", c.expires + self.config.tombstone_s
            if c.state != "live" and now >= c.forget_at:
                del self.codes[key]

    def _retire(self, c: Code, state: str, now: float) -> None:
        c.state = state
        c.forget_at = now + self.config.tombstone_s

    def issue(self, host: HostConn, kind: str, ttl, wanted: str | None = None) -> tuple[dict | None, str | None]:
        """A new code for ``host`` (or ``wanted``, a resume code it issued
        before), or ``(None, reason)``."""
        cfg = self.config
        now = self.clock()
        self.sweep(now)
        if kind not in ("pair", "resume"):
            return None, "bad-request"
        default, cap = (cfg.pair_ttl, cfg.pair_ttl_max) if kind == "pair" else (cfg.resume_ttl, cfg.resume_ttl_max)
        if ttl is None:
            ttl = default
        if not isinstance(ttl, (int, float)) or isinstance(ttl, bool) or ttl <= 0:
            return None, "bad-request"
        ttl = int(min(ttl, cap))
        while host.issued and now - host.issued[0] >= 60.0:
            host.issued.popleft()
        if len(host.issued) >= cfg.codes_per_host_per_min:
            return None, "rate-limited"
        live = sum(1 for c in self.codes.values() if c.host is host and c.state == "live")
        if live >= cfg.live_codes_per_host or len(self.codes) >= cfg.max_codes:
            return None, "too-many-codes"
        if wanted is not None:
            if kind != "resume" or not isinstance(wanted, str) or not RESUME_RE.match(wanted):
                return None, "bad-request"
            held = self.codes.get(wanted)
            if held is not None and held.state == "live" and held.host is not host and held.host.alive:
                return None, "code-taken"
            code = wanted
        else:
            for _ in range(32):
                code = ("".join(secrets.choice(PAIR_ALPHABET) for _ in range(PAIR_LEN))
                        if kind == "pair" else secrets.token_urlsafe(RESUME_BYTES))
                if code not in self.codes:
                    break
            else:
                return None, "too-many-codes"
        host.issued.append(now)
        self.codes[code] = Code(code=code, kind=kind, host=host, expires=now + ttl)
        self.stats[f"issued_{kind}"] += 1
        return {"code": code, "kind": kind, "expires_in": ttl}, None

    def revoke(self, host: HostConn, raw: str) -> bool:
        c = self.codes.get(normalize_code(raw)) if isinstance(raw, str) else None
        if c is None or c.host is not host or c.state != "live":
            return False
        self._retire(c, "revoked", self.clock())
        self.stats["revoked"] += 1
        return True

    def claim(self, raw, ip: str) -> tuple[Code | None, str | None]:
        """The code a page joins with, consumed when it is a pair code, or
        ``(None, reason)``. The address's failures are checked first, so a
        guesser that is rate-limited learns nothing about its guesses."""
        cfg = self.config
        now = self.clock()
        self.sweep(now)
        if (self.fails.count(ip, now) >= cfg.join_fails_per_ip
                or self.global_fails.count("*", now) >= cfg.global_fails_per_min):
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
            self.fails.hit(ip, now)
            self.global_fails.hit("*", now)
            return None, reason
        if self.code_joins.count(c.code, now) >= cfg.joins_per_code_per_min:
            return None, "rate-limited"
        if len(c.host.peers) >= cfg.peers_per_host:
            return None, "rate-limited"
        self.code_joins.hit(c.code, now)
        if c.kind == "pair":
            self._retire(c, "used", now)
        return c, None

    def host_gone(self, host: HostConn) -> None:
        host.alive = False
        now = self.clock()
        for c in self.codes.values():
            if c.host is host and c.state == "live":
                self._retire(c, "host-gone", now)

    # ── Connections ───────────────────────────────────────────────────────

    def process_request(self, conn: ServerConnection, request):
        """Routes, the Origin check and a health check, before the upgrade."""
        path = request.path.split("?", 1)[0]
        if path in ("/healthz", "/v1/healthz"):
            return conn.respond(200, "ok\n")
        if path not in ("/v1/host", "/v1/join"):
            return conn.respond(404, "Not found\n")
        origin = request.headers.get("Origin")
        if not self.origin_allowed(origin, required=(path == "/v1/join")):
            self.stats["origin_refused"] += 1
            return conn.respond(403, "Origin not allowed\n")
        return None

    async def handler(self, conn: ServerConnection) -> None:
        path = conn.request.path.split("?", 1)[0]
        ip = self.client_ip(conn)
        if path == "/v1/host":
            await self._host(conn, ip)
        else:
            await self._join(conn, ip)

    async def _send(self, ws, msg: dict) -> bool:
        try:
            await ws.send(json.dumps({"v": VERSION, **msg}, separators=(",", ":")))
            return True
        except ConnectionClosed:
            return False

    async def _host(self, ws: ServerConnection, ip: str) -> None:
        cfg = self.config
        if self.host_conns[ip] >= cfg.host_conns_per_ip:
            self.stats["host_refused"] += 1
            await self._send(ws, {"type": "error", "reason": "rate-limited"})
            await ws.close(1013, "too many hosts from this address")
            return
        self.host_conns[ip] += 1
        host = HostConn(ws=ws, ip=ip)
        self.stats["hosts"] += 1
        logger.info("host %s connected from %s", host.id, ip)
        try:
            await host.send({"type": "ice_servers", "ice_servers": self.ice_servers()})
            async for raw in ws:
                msg = _parse(raw)
                if msg is None:
                    await host.send({"type": "error", "reason": "bad-request"})
                    continue
                await self._host_message(host, msg)
        except ConnectionClosed:
            pass
        finally:
            self.host_conns[ip] -= 1
            if self.host_conns[ip] <= 0:
                del self.host_conns[ip]
            self.host_gone(host)
            for peer in list(host.peers.values()):
                await peer.send({"type": "error", "reason": "host-gone"})
                peer.done.set()
            logger.info("host %s gone", host.id)

    async def _host_message(self, host: HostConn, msg: dict) -> None:
        t = msg["type"]
        req = msg.get("req")
        echo = {"req": req} if isinstance(req, (str, int)) and not isinstance(req, bool) else {}
        if t == "new_code":
            answer, reason = self.issue(host, msg.get("kind", "pair"), msg.get("ttl"), msg.get("code"))
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
            if t == "answer" and isinstance(msg.get("sdp"), (str, dict)):
                self.stats["answers"] += 1
                await peer.send({"type": "answer", "sdp": msg["sdp"]})
            elif t == "ice" and "candidate" in msg:
                await peer.send({"type": "ice", "candidate": msg["candidate"]})
            elif t == "reject":
                self.stats["rejected"] += 1
                await peer.send({"type": "error", "reason": "rejected"})
                peer.done.set()
        else:
            await host.send({"type": "error", "reason": "bad-request", **echo})

    async def _join(self, ws: ServerConnection, ip: str) -> None:
        cfg = self.config
        if self.join_conns[ip] >= cfg.join_conns_per_ip:
            await self._send(ws, {"type": "error", "reason": "rate-limited"})
            await ws.close(1013, "too many connections from this address")
            return
        self.join_conns[ip] += 1
        peer: PeerConn | None = None
        try:
            await self._send(ws, {"type": "ice_servers", "ice_servers": self.ice_servers()})
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
                logger.info("join refused from %s: %s", ip, reason)
                await self._send(ws, {"type": "error", "reason": reason})
                await ws.close(1000)
                return
            host = code.host
            peer = PeerConn(ws=ws, id=secrets.token_urlsafe(9), host=host)
            host.peers[peer.id] = peer
            sent = await host.send({"type": "join", "peer": peer.id, "offer": msg["offer"],
                                    "kind": code.kind, "code": code.code})
            if not sent or not host.alive:
                await peer.send({"type": "error", "reason": "host-gone"})
                return
            self.stats["joins"] += 1
            logger.info("join %s (%s code) from %s to host %s", peer.id[:4], code.kind, ip, host.id)
            await self._relay(ws, peer, cfg.join_session_s)
        except ConnectionClosed:
            pass
        finally:
            self.join_conns[ip] -= 1
            if self.join_conns[ip] <= 0:
                del self.join_conns[ip]
            if peer is not None:
                peer.host.peers.pop(peer.id, None)
            await ws.close(1000)

    async def _relay(self, ws: ServerConnection, peer: PeerConn, limit_s: float) -> None:
        """The page's candidates to its host, until the page leaves, the host
        rejects it or goes, or the signalling time is over."""
        async def pump() -> None:
            async for raw in ws:
                msg = _parse(raw)
                if msg is not None and msg["type"] == "ice" and "candidate" in msg:
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
                     ping_interval=cfg.ping_interval or None, ping_timeout=cfg.ping_timeout or None,
                     max_size=cfg.max_message, open_timeout=10, close_timeout=cfg.close_timeout,
                     compression=None, server_header=None)


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
