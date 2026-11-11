"""The VR page's lifecycle routes (epic #166): what page build and page API
this backend serves, the page itself with versioned asset addresses, and a
small log for the page's errors and lifecycle events.

Why versioned addresses: a headset's browser keeps tabs and caches alive for
days. ``/vr/`` (the HTML) is always revalidated; the modules it loads are
served under ``/vr/b/<build>/…``, where ``<build>`` is a hash of the page's
files, so an upgraded backend gives the page new addresses (never a mix of
yesterday's and today's modules) and a module of the current build can be
cached for good. The page compares its own build and API with
``/v1/vr/info`` at start and after every reconnection, and says "reload" or
"update the app" instead of failing obscurely.

``/v1/vr/info`` is public like ``/v1/ping``: it says which page this backend
serves, nothing about users or data. With an ``X-Visivo-Token`` header it
also says whether that token is accepted, so one cheap call tells the page
"reachable, same build, token still good".

``/v1/vr/log`` needs the token, is rate limited per token and capped in
size, and scrubs tokens and paths from what it writes to the backend log.
"""
from __future__ import annotations

import asyncio
import hashlib
import unicodedata
from collections import OrderedDict, deque
from urllib.parse import unquote
import logging
import os
import re
import secrets
import threading
import time
import uuid
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException, Request
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse
from pydantic import BaseModel, Field, ValidationError

from ..auth import accepted_tokens, token_equal, verify_token

router = APIRouter(tags=["vr"])
logger = logging.getLogger("visivo.vr")

#: The page's API: the routes, headers and binary layouts it relies on. Bump
#: VR_API when the backend adds what a newer page needs; bump VR_API_MIN when a
#: change breaks pages older than that (they are then told to reload).
VR_API = 1
VR_API_MIN = 1

STATIC_DIR = Path(__file__).resolve().parents[1] / "static" / "vr"
#: A new value at every backend start: a page that sees it change knows the
#: backend restarted, so the sessions and datasets it held are gone.
INSTANCE = uuid.uuid4().hex[:12]
STARTED_AT = time.time()

_BUILD_PLACEHOLDER = "__VR_BUILD__"
_build_lock = threading.Lock()
_build_cache: dict = {"sig": None, "build": "none"}


def _page_signature(root: Path) -> tuple:
    """(relative path, mtime, size) of every page file, dotfiles left out (they
    are neither hashed nor served). A file that vanishes while it is listed (an
    upgrade replacing the files) is skipped, never an error."""
    out: list[tuple[str, int, int]] = []

    def walk(d: Path, rel: str) -> None:
        try:
            entries = list(os.scandir(d))
        except OSError:
            return
        for e in entries:
            if e.name.startswith("."):
                continue
            try:
                if e.is_dir(follow_symlinks=False):
                    walk(Path(e.path), f"{rel}{e.name}/")
                elif e.is_file():
                    st = e.stat()
                    out.append((f"{rel}{e.name}", st.st_mtime_ns, st.st_size))
            except OSError:
                continue

    walk(root, "")
    return tuple(sorted(out))


def page_build(root: Path = STATIC_DIR) -> str:
    """A short hash of the page's files (names and contents). Recomputed only
    when a file's size or modification time changed, so a page edited while
    the backend runs gets a new build without a restart. Blocking (stat calls,
    and reads when something changed): async routes call it in a thread."""
    sig = _page_signature(root)
    with _build_lock:
        if _build_cache["sig"] == sig and _build_cache.get("root") == str(root):
            return _build_cache["build"]
        h = hashlib.sha256()
        for rel, _mt, _sz in sig:
            h.update(rel.encode())
            h.update(b"\0")
            try:
                h.update((root / rel).read_bytes())
            except OSError:
                h.update(b"<missing>")
            h.update(b"\0")
        build = h.hexdigest()[:12] if sig else "none"
        _build_cache.update(sig=sig, build=build, root=str(root))
        return build


async def page_build_async() -> str:
    return await asyncio.to_thread(page_build)


def backend_version(request: Request) -> str:
    return os.environ.get("VISIVO_VERSION") or request.app.version


def _token_ok(token: str | None) -> bool:
    matched = False
    if token:
        for accepted in accepted_tokens():
            if token_equal(token, accepted):
                matched = True   # no short circuit: same work either way
    return matched


class _TokenCheckLimiter:
    """At most ``per_minute`` token checks per client key and sliding minute,
    for at most ``max_clients`` keys (the least recently seen are dropped), and
    at most ``global_per_minute`` checks a minute in all, whatever the keys:
    /v1/vr/info is public and must not be a fast token oracle, even for a
    caller that varies its address."""

    def __init__(self, per_minute: int = 20, max_clients: int = 256, global_per_minute: int = 200) -> None:
        self.per_minute = per_minute
        self.max_clients = max_clients
        self.global_per_minute = global_per_minute
        self._lock = threading.Lock()
        self._seen: OrderedDict[str, list[float]] = OrderedDict()
        self._all: deque[float] = deque()
        self._misses: OrderedDict[str, tuple[float, int]] = OrderedDict()

    def allow(self, client: str, now: float | None = None) -> bool:
        now = time.monotonic() if now is None else now
        with self._lock:
            while self._all and now - self._all[0] >= 60.0:
                self._all.popleft()
            hist = [t for t in self._seen.pop(client, []) if now - t < 60.0]
            ok = len(hist) < self.per_minute and len(self._all) < self.global_per_minute
            if ok:
                hist.append(now)
                self._all.append(now)
            self._seen[client] = hist
            while len(self._seen) > self.max_clients:
                self._seen.popitem(last=False)
            return ok

    def miss(self, client: str, now: float | None = None) -> int:
        """Counts a refused token; returns the misses in the current minute."""
        now = time.monotonic() if now is None else now
        with self._lock:
            start, n = self._misses.pop(client, (now, 0))
            if now - start >= 60.0:
                start, n = now, 0
            n += 1
            self._misses[client] = (start, n)
            while len(self._misses) > self.max_clients:
                self._misses.popitem(last=False)
            return n


TOKEN_CHECKS = _TokenCheckLimiter()


def token_check_key(request: Request) -> str:
    """The key a token check is counted under. ``request.client`` is the
    socket peer unless uvicorn's proxy headers handling rewrote it from
    ``X-Forwarded-For``, which it does by default for connections from
    127.0.0.1, exactly how adb reverse and ssh tunnels arrive, so a caller
    could choose its own key. A request that carries such a header is
    therefore counted under one shared key, whatever it claims; others under
    their address. Locally every client is loopback anyway, so the limit is in
    effect shared by them all: acceptable, since a check skipped answers
    ``null`` and the page keeps the state it knew."""
    h = request.headers
    if "x-forwarded-for" in h or "forwarded" in h or "x-real-ip" in h:
        return "forwarded"
    return request.client.host if request.client else "?"


@router.get("/v1/vr/info")
async def vr_info(request: Request) -> JSONResponse:
    """The page build and page API this backend serves and its instance (a
    restart changes it). With an ``X-Visivo-Token`` header, whether it is
    accepted (``token_ok``; ``null`` when none was sent or when this address
    checked too often) and, for an accepted token only, the backend version."""
    token = request.headers.get("X-Visivo-Token")
    body = {
        "vr_api": VR_API,
        "vr_api_min": VR_API_MIN,
        "build": await page_build_async(),
        "instance": INSTANCE,
        "token_ok": None,
    }
    if token is not None:
        client = token_check_key(request)
        if TOKEN_CHECKS.allow(client):
            ok = _token_ok(token)
            body["token_ok"] = ok
            if ok:
                body["backend_version"] = backend_version(request)
            else:
                n = TOKEN_CHECKS.miss(client)
                if n in (5, 20, 100):
                    logger.warning("[vr] %d refused tokens from %r on /v1/vr/info within a minute", n, client)
        else:
            body["token_check"] = "rate-limited"
    return JSONResponse(body, headers={"Cache-Control": "no-store"})


# ── The page, with versioned asset addresses ────────────────────────────────

async def _index_html() -> str:
    build = await page_build_async()
    text = await asyncio.to_thread((STATIC_DIR / "index.html").read_text, encoding="utf-8")
    return text.replace(_BUILD_PLACEHOLDER, build)


@router.get("/vr/", include_in_schema=False)
@router.get("/vr/index.html", include_in_schema=False)
async def vr_index() -> HTMLResponse:
    """The page: always revalidated, its modules under this build's address."""
    if not (STATIC_DIR / "index.html").is_file():
        raise HTTPException(status_code=404, detail="The VR page is not installed.")
    return HTMLResponse(await _index_html(), headers={"Cache-Control": "no-cache"})


@router.get("/vr/b/{build}/{path:path}", include_in_schema=False)
async def vr_versioned_asset(build: str, path: str) -> FileResponse:
    """A page file under a build address. The current build's files are cached
    for good. Another build's address (a tab older than the backend) gets
    today's file, revalidated: such a tab may run a mix of old cached modules
    and new ones until it reloads, which its version check asks it to do.
    The hash is taken just before the file is served, so a file replaced in
    between is served once under the previous build's address; the next
    request revalidates it. Harmless, and not worth a lock."""
    root = STATIC_DIR.resolve()
    target = (root / path).resolve()
    if (root not in target.parents or not target.is_file() or target.name == "index.html"
            or any(part.startswith(".") for part in target.relative_to(root).parts)):
        raise HTTPException(status_code=404, detail="Not found")
    current = build == await page_build_async()
    cache = "public, max-age=31536000, immutable" if current else "no-cache"
    return FileResponse(target, headers={"Cache-Control": cache})


# ── The page's log ──────────────────────────────────────────────────────────

#: Request body cap (bytes), events per request, characters per message.
MAX_BODY = 16 * 1024
MAX_EVENTS = 25
MAX_MESSAGE = 600
#: Per token, per sliding minute.
RATE_EVENTS = 60
RATE_REQUESTS = 20
_WINDOW_S = 60.0


class VrLogEvent(BaseModel):
    level: str = Field("info", pattern=r"^(debug|info|warning|error)$")
    event: str = Field(..., min_length=1, max_length=64, pattern=r"^[A-Za-z0-9_.:-]+$")
    message: str = Field("", max_length=4000)
    t: float | None = None     # the page's clock (ms since it loaded)


class VrLogBody(BaseModel):
    build: str = Field("", max_length=64)
    user_agent: str = Field("", max_length=300)
    events: list[VrLogEvent] = Field(..., max_length=MAX_EVENTS)


class _RateLimiter:
    """A sliding minute of requests and events per token (by hash)."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._seen: dict[str, list[tuple[float, int]]] = {}

    def allow(self, key: str, events: int, now: float | None = None) -> tuple[bool, float]:
        now = time.monotonic() if now is None else now
        with self._lock:
            hist = [(t, n) for t, n in self._seen.get(key, []) if now - t < _WINDOW_S]
            n_events = sum(n for _, n in hist)
            if len(hist) >= RATE_REQUESTS or n_events + events > RATE_EVENTS:
                self._seen[key] = hist
                retry = _WINDOW_S - (now - hist[0][0]) if hist else _WINDOW_S
                return False, max(1.0, retry)
            hist.append((now, events))
            self._seen[key] = hist
            # Keys of tokens no longer seen are dropped as they age out.
            if len(self._seen) > 256:
                for k in [k for k, v in self._seen.items() if not v or now - v[-1][0] >= _WINDOW_S]:
                    self._seen.pop(k, None)
            return True, 0.0


RATE = _RateLimiter()

# ── Scrubbing (the same rules as the page's diagnostics.js scrubText) ──────
#
# What is removed from a message before it is written:
#   1. %-escapes are decoded first (a pairing link writes path=%2FUsers%2F…);
#   2. every known token, and the caller's, becomes <token>;
#   3. the value of any key containing "token" or "session" (session_id,
#      sessionId, access_token, X-Visivo-Token, JSON "token": "…"), quoted or
#      not, before = or :, becomes <redacted> when it looks like a secret
#      (_looks_secret: 6+ characters with a digit, 8+ hex, or 16+ of any
#      kind), so "sessions: 3" stays readable;
#   4. the id in /v1/render/<id>/… (a render session) becomes <id>;
#   5. paths keep only their last component: file:// URLs, the path of a URL
#      with a host (except the backend's own /v1/… and /vr/… routes), Windows
#      paths (C:\…, C:/…, \\server\share\…) and POSIX paths (/…, ~/…). A path
#      may hold spaces inside its directory names ("John Doe") up to the first
#      file name with an extension; without one, in any directory name that
#      another separator follows. The last component never holds a space, so
#      the rest of a sentence is kept, and "3/5" or "1 / 2" are no paths. A
#      home directory (/Users/<n>, /home/<n>, C:\Users\<n>, in file:// URLs
#      too) is never shown: a path ending there gives …/<user>;
#   6. control and format characters (Unicode Cc, Cf, Zl, Zp: newlines, NEL,
#      C1 controls, bidi overrides, U+2028/2029) become spaces;
#   7. at most MAX_MESSAGE characters are kept.
_ESCAPES = re.compile(r"(?:%[0-9A-Fa-f]{2})+")
# A key: a whole word of at most 64 characters that names a token or a
# session, found by a bounded lookahead (so the match is linear: an unbounded
# [\w-]*token[\w-]* backtracked for seconds on a long run of "xtoken"). Only
# such keys match at all, so an ordinary "host:port/…" cannot swallow a
# ?token= that follows it.
_KEY_VALUE = re.compile(
    r"""(?i)(["']?)(?<![\w-])(?=[\w-]{0,60}?(?:token|session))([\w-]{1,64})(?![\w-])\1(\s*[=:]\s*)"""
    r"""("[^"]*"|'[^']*'|[^\s&#,;'"}\]]+)""")
_SECRET_KEY = re.compile(r"(?i)token|session")
#: What the regex rules see at most (they run after the known secrets are
#: replaced, before the final cut to MAX_MESSAGE).
_SCRUB_WINDOW = 2 * 600
_RENDER_ID = re.compile(r"(/v1/render/(?:queue/)?)([0-9A-Fa-f-]{8,})")
# file:// URLs are scrubbed by the path rules below: their path is marked
# (file://<MARK>/…) so that it starts a path, and the mark removed at the end.
_MARK = "\ue000"
_FILE_PREFIX = re.compile(r"(?i)\bfile:///?")
_HOST_URL = re.compile(r"(?i)\b(?!file:)([a-z][a-z0-9+.-]*://[^\s/\"'<>?#]+)(/[^\s\"'<>?#]*)?")
# A directory name with single spaces inside it ("John Doe"), never at its ends.
_SEG_SP = r"[^/\s\"'<>]+(?: [^/\s\"'<>]+)*"
_SEG_SP_WIN = r"[^\\/\s\"'<>|]+(?: [^\\/\s\"'<>|]+)*"
_EXT_NAME = r"[^/\\\s\"'<>|]+\.[A-Za-z0-9]{1,8}\b"
_WIN_START = r"(?:\b[A-Za-z]:[\\/]|\\\\)"
_POSIX_START = r"(?<![\w.:/\\…<>])~?/(?!v1/|vr/)"
_WIN_PATH_EXT = re.compile(_WIN_START + r"(?:" + _SEG_SP_WIN + r"[\\/])*?(" + _EXT_NAME + r")")
# Without a file name with an extension. A home directory (Users\<name>,
# /Users/<name>, /home/<name>) is told apart: its name may hold spaces, and
# when the path ends there the name itself is replaced by <user>. Elsewhere
# directory names may hold single spaces too: better to remove too much than
# to leave a name behind ("/mnt/Shared Drive/Alice Smith/x" -> "…/x").
_WIN_PATH = re.compile(_WIN_START + r"(?:[Uu]sers[\\/](" + _SEG_SP_WIN + r")(?:[\\/](?:" + _SEG_SP_WIN
                       + r"[\\/])*([^\\/\s\"'<>|]*))?|(?:" + _SEG_SP_WIN + r"[\\/])*([^\\/\s\"'<>|]*))")
_WIN_USER_DIR = re.compile(r"^(?:[A-Za-z]:[\\/]|\\\\)[Uu]sers[\\/][^\\/]+$")
_POSIX_USER_DIR = re.compile(r"^~?/(?:Users|home)/[^/]+$")
_POSIX_PATH_EXT = re.compile(_POSIX_START + r"(?:" + _SEG_SP + r"/)+?(" + _EXT_NAME + r")")
_POSIX_PATH = re.compile(_POSIX_START + r"(?:(?:Users|home)/(" + _SEG_SP + r")(?:/(?:" + _SEG_SP
                         + r"/)*([^/\s\"'<>]*))?|(?:" + _SEG_SP + r"/)+([^/\s\"'<>]*))")


def _path_sub(sep: str):
    """The replacement of a path without an extension: its last component, or
    <user> when it ends at a home directory."""
    def sub(m: re.Match) -> str:
        user, last, other = m.group(1), m.group(2), m.group(3)
        if user is not None:
            return f"…{sep}{last}" if last else f"…{sep}<user>"
        return f"…{sep}{other}"
    return sub


def _ext_sub(sep: str, user_dir: re.Pattern):
    """The replacement of a path ending in a name with an extension; a home
    directory whose name has a dot (/home/alice.smith) is still <user>."""
    def sub(m: re.Match) -> str:
        return f"…{sep}<user>" if user_dir.match(m.group(0)) else f"…{sep}{m.group(1)}"
    return sub
_SPACES = re.compile(r" {2,}")


def _decode(text: str) -> str:
    def one(m: re.Match) -> str:
        try:
            return unquote(m.group(0), errors="strict")
        except UnicodeDecodeError:
            return m.group(0)
    return _ESCAPES.sub(one, text)


def _controls_to_spaces(text: str) -> str:
    return "".join(" " if unicodedata.category(ch) in ("Cc", "Cf", "Zl", "Zp") else ch for ch in text)


def _looks_secret(value: str) -> bool:
    v = value.strip("\"'")
    return ((len(v) >= 6 and any(c.isdigit() for c in v))
            or (len(v) >= 8 and re.fullmatch(r"[0-9A-Fa-f-]+", v) is not None)
            or len(v) >= 16)


def _key_value(m: re.Match) -> str:
    if not _SECRET_KEY.search(m.group(2)) or not _looks_secret(m.group(4)):
        return m.group(0)
    return f"{m.group(1)}{m.group(2)}{m.group(1)}{m.group(3)}<redacted>"


def _url_path(m: re.Match) -> str:
    base, path = m.group(1), m.group(2) or ""
    if not path or path.startswith(("/v1/", "/vr/")) or path == "/":
        return base + path
    last = path.rstrip("/").rsplit("/", 1)[-1]
    return f"{base}/…/{last}"


def scrub(text: str, secrets_: tuple[str, ...] = ()) -> str:
    """``text`` safe for the log (see the rules above)."""
    # 1. decoded and the known secrets replaced over the whole text (linear),
    # 2. cut, 3. the pattern rules, on at most _SCRUB_WINDOW characters.
    out = _controls_to_spaces(_decode(str(text))).replace(_MARK, " ")
    for s in secrets_:
        if s and len(s) >= 6:
            out = out.replace(s, "<token>")
    out = out[:_SCRUB_WINDOW]
    out = _KEY_VALUE.sub(_key_value, out)
    out = _RENDER_ID.sub(lambda m: f"{m.group(1)}<id>", out)
    out = _HOST_URL.sub(_url_path, out)
    out = _FILE_PREFIX.sub("file://" + _MARK + "/", out)
    out = _WIN_PATH_EXT.sub(_ext_sub("\\", _WIN_USER_DIR), out)
    out = _WIN_PATH.sub(_path_sub("\\"), out)
    out = _POSIX_PATH_EXT.sub(_ext_sub("/", _POSIX_USER_DIR), out)
    out = _POSIX_PATH.sub(_path_sub("/"), out)
    out = out.replace(_MARK, "")
    out = _SPACES.sub(" ", out).strip()
    return out[:MAX_MESSAGE] + ("…" if len(out) > MAX_MESSAGE else "")


async def _read_capped(request: Request) -> bytes:
    declared = request.headers.get("content-length")
    if declared is not None:
        try:
            if int(declared) > MAX_BODY:
                raise HTTPException(status_code=413, detail=f"At most {MAX_BODY} bytes per request.")
        except ValueError:
            raise HTTPException(status_code=400, detail="Bad Content-Length.") from None
    body = bytearray()
    async for chunk in request.stream():
        body += chunk
        if len(body) > MAX_BODY:
            raise HTTPException(status_code=413, detail=f"At most {MAX_BODY} bytes per request.")
    return bytes(body)


@router.post("/v1/vr/log")
async def vr_log(request: Request, token: str = Depends(verify_token)) -> dict:
    """Writes the page's errors and lifecycle events to the backend log (one
    line each, scrubbed of tokens and directory paths). At most
    ``MAX_EVENTS`` per request, ``MAX_BODY`` bytes, and per token
    ``RATE_REQUESTS`` requests and ``RATE_EVENTS`` events a minute (429 with
    Retry-After beyond)."""
    raw = await _read_capped(request)
    try:
        body = VrLogBody.model_validate_json(raw or b"{}")
    except ValidationError as exc:
        raise HTTPException(status_code=422, detail=f"Bad log body: {exc.error_count()} error(s).") from None
    key = hashlib.sha256(token.encode()).hexdigest()[:16]
    ok, retry = RATE.allow(key, len(body.events))
    if not ok:
        raise HTTPException(status_code=429, detail="Too many log events: slow down.",
                            headers={"Retry-After": str(int(retry + 0.999))})
    known = tuple(accepted_tokens()) + (token,)

    def scrubbed() -> tuple[str, list[str], str]:
        return (scrub(body.build, known)[:16] or "?", [scrub(ev.message, known) for ev in body.events],
                scrub(body.user_agent, known) if body.user_agent else "")

    # In a thread: scrubbing is regex work on up to 16 KiB, which must never
    # hold the event loop.
    build, messages, agent = await asyncio.to_thread(scrubbed)
    for ev, message in zip(body.events, messages):
        level = {"debug": logging.DEBUG, "info": logging.INFO, "warning": logging.WARNING,
                 "error": logging.ERROR}[ev.level]
        logger.log(level, "[vr %s] %s: %s", build, ev.event, message)
    if agent:
        logger.debug("[vr %s] user agent: %s", build, agent)
    return {"accepted": len(body.events)}
