"""Launching the headset app (issue #255): one-time launch tickets, and the
Digital Asset Links statements that let the packaged app open this origin.

The desktop starts the Horizon OS app packaged from the VR page with ONE
``am start`` whose address carries a ticket (``#ticket=…``), never the token:
Android keeps a launch intent, fragment included, in ``dumpsys activity``
and the recents list (measured on a Quest 3, #254). The page exchanges the
ticket for the token, the desktop's session and the cube to show, once.

A ticket is

* random (256 bits), handed to an authenticated caller for its own session;
* short-lived (``TICKET_TTL_S``) and single use: the exchange removes it,
  whatever its outcome after that, and an expired one is refused;
* stored only as its SHA-256, with the SHA-256 of the caller's token: the
  token handed back is looked up among the tokens the backend accepts at the
  exchange, so a token revoked in between is not handed out.

Each ticket names a *launch* (a short public id) that the desktop follows:
pending, then exchanged (the page started), then the headset hub client
whose hello named it (the page is connected). Launches are kept a few
minutes for that, and only their owner (same token and session) sees them.

Everything here is in memory: a backend restart forgets the tickets, and
the desktop issues a new one with its next launch.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import logging
import os
import re
import secrets
import threading
import time
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

logger = logging.getLogger("visivo.vr")

#: How long a ticket can be exchanged, in seconds. A launch reaches the page
#: in 1 to 3 s on a Quest 3 over USB; a headset asleep drops the launch
#: altogether (#254), and the desktop then issues a new ticket.
TICKET_TTL_S = 60.0
#: Tickets waiting at most per token (the token's oldest go), so with several
#: users (VISIVO_TOKENS) one user's launches never evict another's.
MAX_TICKETS = 32
#: Launches remembered per token for the desktop's status checks, and for how long.
MAX_LAUNCHES = 64
LAUNCH_KEEP_S = 600.0
#: What a ticket looks like (secrets.token_urlsafe(32) gives 43 characters).
TICKET_RE = re.compile(r"^[A-Za-z0-9_-]{32,128}$")
LAUNCH_RE = re.compile(r"^[A-Za-z0-9_-]{8,32}$")


def _sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


@dataclass
class _Ticket:
    launch: str
    token_hash: str
    session_id: str
    cube: dict[str, Any] | None
    expires: float                    # monotonic


@dataclass
class _Launch:
    launch: str
    token_hash: str
    session_id: str
    issued: float                     # monotonic
    expires: float                    # monotonic: the ticket's end
    exchanged_at: float | None = None  # wall clock (Unix s)


class LaunchTickets:
    """The tickets and launches of this backend (thread-safe)."""

    def __init__(self, ttl: float = TICKET_TTL_S, max_tickets: int = MAX_TICKETS,
                 max_launches: int = MAX_LAUNCHES, keep: float = LAUNCH_KEEP_S,
                 clock: Callable[[], float] = time.monotonic,
                 wall: Callable[[], float] = time.time) -> None:
        self.ttl = ttl
        self.max_tickets = max_tickets
        self.max_launches = max_launches
        self.keep = keep
        self._clock = clock
        self._wall = wall
        self._lock = threading.Lock()
        self._tickets: OrderedDict[str, _Ticket] = OrderedDict()   # by the ticket's SHA-256
        self._launches: OrderedDict[str, _Launch] = OrderedDict()

    def _prune(self, now: float) -> None:
        for h in [h for h, t in self._tickets.items() if t.expires <= now]:
            del self._tickets[h]
        for k in [k for k, v in self._launches.items() if now - v.issued > self.keep]:
            del self._launches[k]

    def issue(self, token: str, session_id: str, cube: dict[str, Any] | None) -> dict[str, Any]:
        """A new ticket for ``token``'s ``session_id``: {ticket, launch,
        expires_in}. The ticket itself is returned here only."""
        ticket = secrets.token_urlsafe(32)
        launch = secrets.token_urlsafe(9)
        token_hash = _sha256(token)
        with self._lock:
            now = self._clock()
            self._prune(now)
            self._tickets[_sha256(ticket)] = _Ticket(launch=launch, token_hash=token_hash,
                                                     session_id=session_id, cube=cube, expires=now + self.ttl)
            self._launches[launch] = _Launch(launch=launch, token_hash=token_hash, session_id=session_id,
                                             issued=now, expires=now + self.ttl)
            _evict_oldest(self._tickets, token_hash, self.max_tickets)
            _evict_oldest(self._launches, token_hash, self.max_launches)
        return {"ticket": ticket, "launch": launch, "expires_in": self.ttl}

    def exchange(self, ticket: str, accepted: tuple[str, ...]) -> dict[str, Any] | None:
        """The ticket's {token, session, launch, cube}, once; None when it is
        malformed, unknown, used, expired, or its token is no longer accepted."""
        if not isinstance(ticket, str) or not TICKET_RE.match(ticket):
            return None
        key = _sha256(ticket)
        with self._lock:
            now = self._clock()
            t = self._tickets.pop(key, None)      # single use: gone whatever follows
            if t is None or t.expires <= now:
                self._prune(now)
                return None
            launch = self._launches.get(t.launch)
        token = None
        for candidate in accepted:
            # Every accepted token compared, no short circuit (as verify_token).
            if hmac.compare_digest(_sha256(candidate), t.token_hash):
                token = candidate
        if token is None:
            return None
        if launch is not None:
            with self._lock:
                launch.exchanged_at = self._wall()
        return {"token": token, "session": t.session_id, "launch": t.launch, "cube": t.cube}

    def status(self, launch: str, token: str, session_id: str) -> dict[str, Any] | None:
        """{launch, state, expires_in, exchanged_at} for its owner; None for
        an unknown launch or another caller's."""
        if not isinstance(launch, str) or not LAUNCH_RE.match(launch):
            return None
        with self._lock:
            now = self._clock()
            self._prune(now)
            v = self._launches.get(launch)
            if v is None:
                return None
            owner = hmac.compare_digest(_sha256(token), v.token_hash) and hmac.compare_digest(
                session_id.encode("utf-8"), v.session_id.encode("utf-8"))
            if not owner:
                return None
            if v.exchanged_at is not None:
                state = "exchanged"
            elif v.expires > now and any(t.launch == launch for t in self._tickets.values()):
                state = "pending"
            else:
                state = "expired"
            return {"launch": launch, "state": state, "expires_in": max(0.0, round(v.expires - now, 1)),
                    "exchanged_at": v.exchanged_at}


def _evict_oldest(store: OrderedDict, token_hash: str, limit: int) -> None:
    """Drops ``token_hash``'s oldest entries of ``store`` (in issue order)
    beyond ``limit``; other tokens' entries stay."""
    mine = [k for k, v in store.items() if v.token_hash == token_hash]
    for k in mine[:max(0, len(mine) - limit)]:
        del store[k]


TICKETS = LaunchTickets()


# ── Digital Asset Links ──────────────────────────────────────────────────────
#
# The headset's browser opens the packaged app full screen only when the
# origin's /.well-known/assetlinks.json names the app's package and signing
# certificate; it fetches it at every launch. The statements served are:
#
#   1. the release app (its package and certificates in vr_app.json, plus any
#      in VISIVO_VR_APP_RELEASE_SHA256), built by CI with the project's key;
#   2. the development app, signed with a key made on this computer by
#      tools/quest-app/build.sh, when its fingerprint file exists
#      (<VISIVO_QUEST_APP_DIR or ~/.visivo/quest-app>/dev-sha256.txt);
#   3. the statements in the file VISIVO_VR_ASSETLINKS_FILE, as they are.
#
# A fingerprint names a public certificate, nothing about users or data:
# the route is public, as the browser fetches it with no token.

APP_CONFIG = Path(__file__).resolve().parent / "vr_app.json"
ASSETLINKS_ENV = "VISIVO_VR_ASSETLINKS_FILE"
RELEASE_SHA256_ENV = "VISIVO_VR_APP_RELEASE_SHA256"
APP_DIR_ENV = "VISIVO_QUEST_APP_DIR"
DEV_FINGERPRINT_FILE = "dev-sha256.txt"
#: Statement files larger than this are not read (a file named by mistake).
MAX_ASSETLINKS = 64 * 1024
_FINGERPRINT = re.compile(r"^(?:[0-9A-F]{2}:){31}[0-9A-F]{2}$")
_RELATION = ["delegate_permission/common.handle_all_urls"]


def normalise_fingerprint(text: str) -> str | None:
    """``AA:BB:…`` (32 bytes, upper case) from a SHA-256 written with or
    without colons, in any case; None when it is not one."""
    s = str(text or "").strip().upper()
    if re.fullmatch(r"[0-9A-F]{64}", s):
        s = ":".join(s[i:i + 2] for i in range(0, 64, 2))
    return s if _FINGERPRINT.match(s) else None


def _no_constant(name: str):
    raise ValueError(f"{name} is not JSON")


def _read_json(path: Path) -> Any:
    if not path.is_file() or path.stat().st_size > MAX_ASSETLINKS:
        return None
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=_no_constant)


def read_statements_file(path: str) -> list[dict] | None:
    """The statements in the file named by ``path`` (a JSON array of
    objects); None when the file cannot be read, is too large, is not such an
    array, or holds what strict JSON cannot carry (NaN, an infinite number)
    or nests too deeply to parse."""
    try:
        data = _read_json(Path(path))
        if not isinstance(data, list) or not all(isinstance(s, dict) for s in data):
            return None
        json.dumps(data, allow_nan=False)
        return data
    except (OSError, ValueError, RecursionError):
        return None


def app_config(path: Path | None = None) -> dict[str, Any]:
    """The packaged app's identity (vr_app.json): packages and the release
    certificates' fingerprints. Missing or broken: no release statement."""
    try:
        data = _read_json(path or APP_CONFIG)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError, RecursionError):
        return {}


def app_dir() -> Path:
    return Path(os.environ.get(APP_DIR_ENV, "").strip() or (Path.home() / ".visivo" / "quest-app"))


def _fingerprints(values: Any) -> list[str]:
    out: list[str] = []
    for v in values if isinstance(values, list) else []:
        f = normalise_fingerprint(v) if isinstance(v, str) else None
        if f and f not in out:
            out.append(f)
    return out


def _statement(package: str, fingerprints: list[str]) -> dict[str, Any]:
    return {"relation": list(_RELATION),
            "target": {"namespace": "android_app", "package_name": package,
                       "sha256_cert_fingerprints": fingerprints}}


_PACKAGE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+$")


def assetlinks_statements() -> list[dict[str, Any]]:
    """The statements to serve (see above), in that order; empty when none."""
    cfg = app_config()
    packages = cfg.get("packages") if isinstance(cfg.get("packages"), dict) else {}
    out: list[dict[str, Any]] = []
    release_pkg = packages.get("release")
    release = _fingerprints(cfg.get("release_sha256"))
    release += [f for f in _fingerprints(os.environ.get(RELEASE_SHA256_ENV, "").replace(";", ",").split(","))
                if f not in release]
    if isinstance(release_pkg, str) and _PACKAGE.match(release_pkg) and release:
        out.append(_statement(release_pkg, release))
    dev_pkg = packages.get("development")
    if isinstance(dev_pkg, str) and _PACKAGE.match(dev_pkg):
        try:
            text = (app_dir() / DEV_FINGERPRINT_FILE).read_text(encoding="utf-8")[:4096]
        except OSError:
            text = ""
        dev = _fingerprints([line for line in text.splitlines() if line.strip()])
        if dev:
            out.append(_statement(dev_pkg, dev))
    extra = os.environ.get(ASSETLINKS_ENV, "").strip()
    if extra:
        statements = read_statements_file(extra)
        if statements is None:
            # Skipped, not fatal: the release and development statements are
            # still served, so the packaged app keeps opening full screen.
            logger.warning("[vr] %s does not name a readable JSON array of statements: skipped", ASSETLINKS_ENV)
        else:
            out.extend(statements)
    return out
