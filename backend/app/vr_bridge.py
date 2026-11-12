"""The data-channel bridge of a paired headset (issue #212, contract
``visivo-pair/1``): the page's HTTP requests and WebSockets, carried over
WebRTC data channels, served by this backend's own ASGI app in-process.

Why in-process: the routes stay as they are, and every request goes through
the app exactly as it would over HTTP (middleware, auth, the numba lock and
worker pools of the routes themselves). The bridge only translates frames
into ASGI messages and back, and adds the backend token the pairing was made
with, so the token never reaches the page.

Channels, all created by the page:

``api`` (ordered, reliable): HTTP over the channel.
    Text frames are JSON control messages: ``req {id, method, path, headers,
    body}``, ``res {id, status, headers}``, ``cancel {id}`` (either side),
    ``err {id, message}``. Binary frames are body chunks: 4-byte big-endian
    id, 1 flag byte (bit 0 = last chunk), at most 16 KiB of payload; a body
    always ends with a chunk flagged last, possibly empty.
``ws``, one per page WebSocket, its ``protocol`` the JSON ``{"path",
    "protocols"}``. Every frame is binary: byte 0 the type (0x01 text, 0x02
    binary, 0x03 open ``{"protocol"}``, 0x04 close ``{"code","reason"}``),
    byte 1 the flags (bit 0 = last fragment), then at most 16 KiB. A message
    is one or more frames of its type, only the last flagged (contract
    amendment 1: Chromium's max-message-size is 256 KiB, and a stereo frame
    is larger).

Both directions pause while the channel's ``bufferedAmount`` is above 1 MiB,
so a 100 MB response streams with a bounded amount in memory: the route's
body iterator is not asked for its next piece until the channel has drained.

This module holds no aiortc import: the channels are duck-typed (``send``,
``bufferedAmount``, ``readyState``, ``on``), which keeps the framing testable
without it.
"""
from __future__ import annotations

import asyncio
import json
import logging
import struct
import zlib
from collections import deque
from typing import Any, Awaitable, Callable
from urllib.parse import parse_qsl, unquote, urlencode

logger = logging.getLogger("visivo.vr_bridge")

#: Largest payload of one frame, and the level above which a sender pauses.
CHUNK = 16 * 1024
HIGH_WATER = 1024 * 1024
#: Requests in flight on one ``api`` channel, and the largest request body.
MAX_REQUESTS = 64
MAX_REQUEST_BODY = 64 * 1024 * 1024
#: Largest WebSocket message the page may send through a ``ws`` channel,
#: and what the page's messages and request bodies may hold in memory while
#: the routes have not read them yet (per channel; beyond, the socket is
#: closed with 1009, or the request cancelled).
MAX_WS_MESSAGE = 64 * 1024 * 1024
MAX_WS_QUEUED = 64 * 1024 * 1024
MAX_API_QUEUED = 64 * 1024 * 1024

WS_TEXT, WS_BINARY, WS_OPEN, WS_CLOSE = 0x01, 0x02, 0x03, 0x04
LAST = 0x01

#: Routes a paired headset may not reach through the bridge: pairing itself
#: (a headset must not issue codes or disconnect other headsets).
_DENIED_PREFIXES = ("/v1/vr/pair",)
#: Headers the page may not set: the token is the bridge's to add, and the
#: rest describe a connection that does not exist.
_DROPPED_HEADERS = frozenset({"x-visivo-token", "host", "connection", "upgrade", "content-length",
                              "transfer-encoding", "sec-websocket-protocol", "sec-websocket-key",
                              "sec-websocket-version", "sec-websocket-extensions", "cookie",
                              # Bodies go to the page decoded: it is not asked
                              # to undo an encoding it never chose.
                              "accept-encoding"})
_TOKEN_SUBPROTOCOL = "visivo-token."


# ── Framing (pure) ───────────────────────────────────────────────────────────


def api_chunk(req_id: int, payload: bytes, last: bool) -> bytes:
    return struct.pack(">IB", req_id, LAST if last else 0) + payload


def parse_api_chunk(frame: bytes) -> tuple[int, bool, bytes]:
    if len(frame) < 5:
        raise ValueError("short chunk")
    req_id, flags = struct.unpack_from(">IB", frame)
    return req_id, bool(flags & LAST), frame[5:]


def ws_frames(kind: int, data: bytes) -> list[bytes]:
    """A WebSocket message as ``ws`` frames: pieces of at most CHUNK bytes,
    the last one flagged (an empty message is one empty frame)."""
    if not data:
        return [bytes((kind, LAST))]
    out = []
    for at in range(0, len(data), CHUNK):
        piece = data[at:at + CHUNK]
        out.append(bytes((kind, LAST if at + CHUNK >= len(data) else 0)) + piece)
    return out


class WsAssembler:
    """Joins ``ws`` frames into messages; ``feed`` gives ``(kind, payload)``
    once a message is complete, else None."""

    def __init__(self, limit: int = MAX_WS_MESSAGE) -> None:
        self.limit = limit
        self._kind: int | None = None
        self._parts: list[bytes] = []
        self._size = 0

    def feed(self, frame: bytes) -> tuple[int, bytes] | None:
        if len(frame) < 2:
            raise ValueError("short frame")
        kind, flags = frame[0], frame[1]
        if kind not in (WS_TEXT, WS_BINARY, WS_OPEN, WS_CLOSE):
            raise ValueError(f"unknown frame type {kind}")
        if self._kind is not None and kind != self._kind:
            raise ValueError("a message's fragments changed type")
        self._kind = kind
        self._parts.append(frame[2:])
        self._size += len(frame) - 2
        if self._size > self.limit:
            raise OverflowError("message too large")
        if not flags & LAST:
            return None
        data = b"".join(self._parts)
        self._kind, self._parts, self._size = None, [], 0
        return kind, data


def split_path(path: str) -> tuple[str, bytes]:
    """``(path, query_string)`` of a path relative to the backend root."""
    if not isinstance(path, str) or not path or len(path) > 8192:
        raise ValueError("bad path")
    if not path.startswith("/"):
        path = "/" + path
    if "://" in path.split("?", 1)[0] or path.startswith("//"):
        raise ValueError("bad path")
    p, _, q = path.partition("?")
    return p, q.encode("latin-1", "replace")


def denied(path: str) -> bool:
    p = unquote(path).rstrip("/")
    return any(p == d or p.startswith(d + "/") for d in _DENIED_PREFIXES)


def page_headers(raw: Any) -> list[tuple[bytes, bytes]]:
    """The page's request headers (an object, or a list of pairs), without
    the ones only the bridge may set."""
    items = raw.items() if isinstance(raw, dict) else (raw or [])
    out: list[tuple[bytes, bytes]] = []
    for item in items:
        try:
            name, value = item
        except (TypeError, ValueError):
            continue
        if not isinstance(name, str) or not isinstance(value, str):
            continue
        n = name.strip().lower()
        if not n or n in _DROPPED_HEADERS or len(out) >= 64:
            continue
        out.append((n.encode("latin-1", "replace"), value.encode("latin-1", "replace")[:8192]))
    return out


def response_headers(raw) -> tuple[dict[str, str], "zlib._Decompress | None"]:
    """The response's headers as the page gets them (a JSON object, names in
    lower case, repeated ones joined with ", "), and a decoder when the body
    is gzip or deflate encoded: the page always gets the decoded bytes, so
    ``content-encoding`` goes and ``content-length`` (the encoded length)
    with it."""
    out: dict[str, str] = {}
    for k, v in raw or []:
        name = k.decode("latin-1").lower()
        value = v.decode("latin-1")
        out[name] = f"{out[name]}, {value}" if name in out else value
    encoding = out.get("content-encoding", "").strip().lower()
    decoder = None
    if encoding in ("gzip", "x-gzip"):
        decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    elif encoding == "deflate":
        decoder = zlib.decompressobj()
    if decoder is not None:
        out.pop("content-encoding", None)
        out.pop("content-length", None)
    return out, decoder


def decoded_pieces(decoder, data: bytes, final: bool):
    """``data`` decoded in pieces of at most 4 CHUNK bytes (an encoded body
    may expand a thousandfold: never all at once)."""
    while data:
        piece = decoder.decompress(data, 4 * CHUNK)
        data = decoder.unconsumed_tail
        if piece:
            yield piece
    if final:
        rest = decoder.flush()
        if rest:
            yield rest


def without_token_query(query: bytes) -> bytes:
    """A query string without a ``token`` parameter (the bridge adds the
    token itself; a page has none, and must not choose one)."""
    pairs = [(k, v) for k, v in parse_qsl(query.decode("latin-1"), keep_blank_values=True) if k != "token"]
    return urlencode(pairs).encode("latin-1")


# ── Sending with backpressure ────────────────────────────────────────────────


class ChannelClosed(ConnectionError):
    """The data channel closed while something was being sent on it."""


async def drain(channel, high_water: int = HIGH_WATER) -> None:
    """Returns once the channel holds at most ``high_water`` bytes unsent.
    Raises ChannelClosed when it closes meanwhile."""
    while True:
        if channel.readyState != "open":
            raise ChannelClosed("data channel closed")
        if channel.bufferedAmount <= high_water:
            return
        loop = asyncio.get_running_loop()
        woken = loop.create_future()

        def wake(*_a) -> None:
            if not woken.done():
                woken.set_result(None)

        channel.bufferedAmountLowThreshold = high_water
        channel.on("bufferedamountlow", wake)
        channel.on("close", wake)
        try:
            # A timeout as a safety net: the event fires only when the amount
            # crosses the threshold, and is checked again after it.
            await asyncio.wait_for(asyncio.shield(woken), 1.0)
        except TimeoutError:
            pass
        finally:
            for event in ("bufferedamountlow", "close"):
                try:
                    channel.remove_listener(event, wake)
                except Exception:  # noqa: BLE001 - a closed channel dropped its listeners
                    pass


async def send_frames(channel, frames, high_water: int = HIGH_WATER) -> None:
    for frame in frames:
        await drain(channel, high_water)
        try:
            channel.send(frame)
        except Exception as exc:  # noqa: BLE001 - InvalidStateError once closed
            raise ChannelClosed(str(exc)) from exc


def close_when_sent(channel, timeout: float = 2.0) -> None:
    """Closes the channel once what was sent on it has left its buffer (at
    most ``timeout`` later). A data channel is closed by an SCTP stream
    reset, which keeps the data already handed to SCTP ahead of it, but drops
    what still waits in the channel's own buffer: the close frame that was
    just sent would be lost."""
    async def later() -> None:
        end = asyncio.get_running_loop().time() + timeout
        while channel.readyState == "open" and channel.bufferedAmount > 0:
            if asyncio.get_running_loop().time() > end:
                break
            await asyncio.sleep(0.01)
        try:
            channel.close()
        except Exception:  # noqa: BLE001
            pass

    asyncio.ensure_future(later())


def _send_json(channel, msg: dict) -> None:
    if channel.readyState == "open":
        channel.send(json.dumps(msg, separators=(",", ":")))


# ── The ``api`` channel ──────────────────────────────────────────────────────


class _Request:
    def __init__(self, req_id: int, has_body: bool) -> None:
        self.id = req_id
        self.body: asyncio.Queue = asyncio.Queue()
        self.body_done = not has_body
        self.body_size = 0
        self.queued = 0
        self.task: asyncio.Task | None = None
        self.disconnected = asyncio.Event()
        if not has_body:
            self.body.put_nowait((b"", False))


class ApiBridge:
    """Serves one ``api`` channel: each ``req`` runs the ASGI app in its own
    task, with the request's body fed from the channel and its response sent
    back in chunks, paced by ``drain``."""

    def __init__(self, channel, app, token: str, client: tuple[str, int] = ("vr-pair", 0)) -> None:
        self.channel = channel
        self.app = app
        self.token = token
        self.client = client
        self.requests: dict[int, _Request] = {}
        #: Request body bytes received and not yet read by their routes.
        self.queued = 0
        channel.on("message", self._on_message)
        channel.on("close", self.close)

    def close(self) -> None:
        for r in list(self.requests.values()):
            self._drop(r)
        self.requests.clear()

    def _drop(self, r: _Request) -> None:
        """Stops a request: its task cancelled (possibly before it ever ran,
        so the body it holds is let go here, not in the task)."""
        self.queued -= r.queued
        r.queued = 0
        r.disconnected.set()
        if r.task is not None:
            r.task.cancel()

    def _on_message(self, data) -> None:
        try:
            if isinstance(data, str):
                self._on_control(json.loads(data))
            else:
                self._on_chunk(bytes(data))
        except (ValueError, TypeError, KeyError) as exc:
            logger.debug("[vr-bridge] api: bad frame (%s)", type(exc).__name__)

    def _on_control(self, msg: dict) -> None:
        t = msg.get("t")
        req_id = msg.get("id")
        if not isinstance(req_id, int) or isinstance(req_id, bool) or not 0 <= req_id < 2 ** 32:
            return
        if t == "cancel":
            r = self.requests.pop(req_id, None)
            if r is not None:
                self._drop(r)
            return
        if t != "req":
            return
        if req_id in self.requests:
            _send_json(self.channel, {"t": "err", "id": req_id, "message": "request id in use"})
            return
        if len(self.requests) >= MAX_REQUESTS:
            _send_json(self.channel, {"t": "err", "id": req_id, "message": "too many requests in flight"})
            return
        method = msg.get("method")
        if not isinstance(method, str) or not method.isalpha() or len(method) > 16:
            _send_json(self.channel, {"t": "err", "id": req_id, "message": "bad method"})
            return
        try:
            path, query = split_path(msg.get("path"))
        except ValueError:
            _send_json(self.channel, {"t": "err", "id": req_id, "message": "bad path"})
            return
        r = _Request(req_id, bool(msg.get("body")))
        self.requests[req_id] = r
        r.task = asyncio.ensure_future(self._serve(r, method.upper(), path, query, msg.get("headers")))

    def _on_chunk(self, frame: bytes) -> None:
        req_id, last, payload = parse_api_chunk(frame)
        r = self.requests.get(req_id)
        if r is None or r.body_done:
            return
        r.body_size += len(payload)
        if r.body_size > MAX_REQUEST_BODY or self.queued + len(payload) > MAX_API_QUEUED:
            self.requests.pop(req_id, None)
            self._drop(r)
            _send_json(self.channel, {"t": "cancel", "id": req_id})
            _send_json(self.channel, {"t": "err", "id": req_id, "message": "request body too large"})
            return
        r.body_done = last
        self.queued += len(payload)
        r.queued += len(payload)
        r.body.put_nowait((payload, not last))

    async def _serve(self, r: _Request, method: str, path: str, query: bytes, headers) -> None:
        ch = self.channel
        state = {"started": False, "ended": False}
        hdrs = page_headers(headers)
        hdrs.append((b"x-visivo-token", self.token.encode()))
        hdrs.append((b"host", b"vr-pair"))
        scope = {
            "type": "http", "asgi": {"version": "3.0", "spec_version": "2.3"}, "http_version": "1.1",
            "method": method, "scheme": "http", "path": unquote(path), "raw_path": path.encode("latin-1", "replace"),
            "query_string": query, "root_path": "", "headers": hdrs,
            "client": self.client, "server": ("vr-pair", 0), "extensions": {},
        }

        async def receive() -> dict:
            if r.body_done and r.body.empty() and state.get("body_sent"):
                await r.disconnected.wait()
                return {"type": "http.disconnect"}
            payload, more = await r.body.get()
            self.queued -= len(payload)
            r.queued -= len(payload)
            if not more:
                state["body_sent"] = True
            return {"type": "http.request", "body": payload, "more_body": more}

        async def send(message: dict) -> None:
            kind = message["type"]
            if kind == "http.response.start":
                state["started"] = True
                out, state["decoder"] = response_headers(message.get("headers"))
                await drain(ch)
                _send_json(ch, {"t": "res", "id": r.id, "status": message["status"], "headers": out})
            elif kind == "http.response.body":
                body = message.get("body", b"") or b""
                more = message.get("more_body", False)
                decoder = state.get("decoder")
                pieces = decoded_pieces(decoder, body, not more) if decoder is not None else [body]
                for piece in pieces:
                    view = memoryview(piece)
                    for at in range(0, len(piece), CHUNK):
                        await send_frames(ch, [api_chunk(r.id, bytes(view[at:at + CHUNK]), False)])
                if not more:
                    # The body's end: an empty chunk flagged last (one frame
                    # more than strictly needed, the same for every body).
                    await send_frames(ch, [api_chunk(r.id, b"", True)])
                    state["ended"] = True

        try:
            if denied(path):
                await send({"type": "http.response.start", "status": 403,
                            "headers": [(b"content-type", b"application/json")]})
                await send({"type": "http.response.body",
                            "body": b'{"detail":"Not available over a paired connection."}'})
                return
            await self.app(scope, receive, send)
            if not state["ended"]:
                _send_json(ch, {"t": "err", "id": r.id, "message": "the response ended early"})
        except asyncio.CancelledError:
            pass
        except ChannelClosed:
            pass
        except Exception as exc:  # noqa: BLE001 - reported to the page, logged here
            if not state["ended"]:
                try:
                    _send_json(ch, {"t": "err", "id": r.id, "message": "internal error"})
                except Exception:  # noqa: BLE001
                    pass
            if not state["started"] or not state["ended"]:
                logger.warning("[vr-bridge] %s %s failed: %s", method, path, type(exc).__name__)
        finally:
            self.queued -= r.queued          # what the route never read
            r.queued = 0
            if self.requests.get(r.id) is r:
                del self.requests[r.id]


# ── A ``ws`` channel ─────────────────────────────────────────────────────────


class _ClientGone(OSError):
    """Raised to the app when it sends on a WebSocket the page closed (what
    uvicorn raises in the same case)."""


class WsBridge:
    """One page WebSocket over one ``ws`` channel, served by the app's
    WebSocket route for ``path``."""

    def __init__(self, channel, app, token: str, client: tuple[str, int] = ("vr-pair", 0)) -> None:
        self.channel = channel
        self.app = app
        self.token = token
        self.client = client
        self.incoming: asyncio.Queue = asyncio.Queue()
        self.assembler = WsAssembler()
        #: Bytes of the page's messages the route has not read yet, and the
        #: size of each, in the order they wait.
        self.queued = 0
        self._sizes: deque[int] = deque()
        self.closed_by_page = False
        self.closed_by_app = False
        self.task: asyncio.Task | None = None
        channel.on("message", self._on_message)
        channel.on("close", self._on_close)
        self._start()

    def _fail(self, code: int, reason: str) -> None:
        try:
            for f in ws_frames(WS_CLOSE, json.dumps({"code": code, "reason": reason}).encode()):
                self.channel.send(f)
        except Exception:  # noqa: BLE001
            pass
        close_when_sent(self.channel)

    def _start(self) -> None:
        try:
            spec = json.loads(self.channel.protocol or "{}")
            path, query = split_path(spec.get("path"))
            protocols = [p for p in spec.get("protocols") or [] if isinstance(p, str)]
        except (ValueError, TypeError, AttributeError):
            self._fail(1002, "bad ws channel protocol")
            return
        if denied(path):
            self._fail(1008, "not available over a paired connection")
            return
        protocols = [p for p in protocols if not p.startswith(_TOKEN_SUBPROTOCOL) and 0 < len(p) <= 128][:16]
        hdrs = page_headers({})
        hdrs += [(b"x-visivo-token", self.token.encode()), (b"host", b"vr-pair")]
        if protocols:
            hdrs.append((b"sec-websocket-protocol", ", ".join(protocols).encode()))
        scope = {
            "type": "websocket", "asgi": {"version": "3.0", "spec_version": "2.3"}, "http_version": "1.1",
            "scheme": "ws", "path": unquote(path), "raw_path": path.encode("latin-1", "replace"),
            "query_string": without_token_query(query), "root_path": "", "headers": hdrs,
            "client": self.client, "server": ("vr-pair", 0), "subprotocols": protocols, "extensions": {},
        }
        self.incoming.put_nowait({"type": "websocket.connect"})
        self.task = asyncio.ensure_future(self._run(scope))

    def _on_message(self, data) -> None:
        if isinstance(data, str):
            return                                   # every ws frame is binary
        try:
            done = self.assembler.feed(bytes(data))
        except OverflowError:
            self._page_closed(1009, "message too large")
            self._fail(1009, "message too large")
            return
        except ValueError:
            self._page_closed(1002, "bad frame")
            self._fail(1002, "bad frame")
            return
        if done is None:
            return
        kind, payload = done
        if kind in (WS_TEXT, WS_BINARY) and self.queued + len(payload) > MAX_WS_QUEUED:
            # The route reads slower than the page sends: closed rather than
            # held without bound.
            self._page_closed(1009, "too much unread")
            if not self.closed_by_app:
                self.closed_by_app = True
                self._fail(1009, "too much unread")
            return
        if kind in (WS_TEXT, WS_BINARY):
            self.queued += len(payload)
            self._sizes.append(len(payload))
        if kind == WS_TEXT:
            try:
                self.incoming.put_nowait({"type": "websocket.receive", "text": payload.decode("utf-8")})
            except UnicodeDecodeError:
                self._page_closed(1007, "text is not UTF-8")
                self._fail(1007, "text is not UTF-8")
        elif kind == WS_BINARY:
            self.incoming.put_nowait({"type": "websocket.receive", "bytes": payload})
        elif kind == WS_CLOSE:
            try:
                info = json.loads(payload or b"{}")
                code = int(info.get("code", 1000))
            except (ValueError, TypeError, AttributeError):
                code = 1000
            self._page_closed(code, "")
            if not self.closed_by_app:
                # Echoed, as a WebSocket server answers a close; the route
                # sees the disconnect, and its own close is not sent after it.
                self.closed_by_app = True
                self._fail(code if 1000 <= code < 5000 else 1000, "")

    def _page_closed(self, code: int, reason: str) -> None:
        if not self.closed_by_page:
            self.closed_by_page = True
            self.incoming.put_nowait({"type": "websocket.disconnect", "code": code, "reason": reason})

    def _on_close(self) -> None:
        self._page_closed(1001, "")

    async def _run(self, scope: dict) -> None:
        ch = self.channel
        accepted = False

        async def receive() -> dict:
            message = await self.incoming.get()
            if message["type"] == "websocket.receive" and self._sizes:
                self.queued -= self._sizes.popleft()
            return message

        async def send(message: dict) -> None:
            nonlocal accepted
            kind = message["type"]
            if self.closed_by_page or self.closed_by_app:
                raise _ClientGone("the page closed this WebSocket")
            if kind == "websocket.accept":
                accepted = True
                proto = message.get("subprotocol")
                await send_frames(ch, ws_frames(WS_OPEN, json.dumps({"protocol": proto or ""}).encode()))
            elif kind == "websocket.send":
                if message.get("text") is not None:
                    await send_frames(ch, ws_frames(WS_TEXT, message["text"].encode("utf-8")))
                else:
                    await send_frames(ch, ws_frames(WS_BINARY, message.get("bytes") or b""))
            elif kind == "websocket.close":
                self.closed_by_app = True
                # Closed before the accept: refused (1008 bad token, 1011
                # unknown session), and said with the app's own code.
                code = message.get("code") or 1000
                reason = message.get("reason") or ""
                if not isinstance(reason, str):
                    reason = json.dumps(reason)
                reason = reason[:123]               # a WebSocket close reason's limit
                await send_frames(ch, ws_frames(WS_CLOSE, json.dumps({"code": code, "reason": reason}).encode()))
                close_when_sent(ch)

        try:
            await self.app(scope, receive, send)
        except (asyncio.CancelledError, ChannelClosed, _ClientGone):
            pass
        except Exception as exc:  # noqa: BLE001
            logger.warning("[vr-bridge] ws %s failed: %s", scope["path"], type(exc).__name__)
            if not self.closed_by_app and not self.closed_by_page:
                self.closed_by_app = True
                self._fail(1011, "internal error")
            return
        if not self.closed_by_app and not self.closed_by_page:
            # The route returned without closing: closed normally, as a server would.
            self.closed_by_app = True
            self._fail(1000 if accepted else 1006, "")
        elif self.closed_by_page and ch.readyState == "open":
            close_when_sent(ch)

    def close(self) -> None:
        if self.task is not None:
            self.task.cancel()


# ── The ``ctl`` channel ──────────────────────────────────────────────────────


class CtlChannel:
    """``hello`` from the page, answered by ``welcome`` (built by
    ``welcome``, a coroutine of the hello's ``resume`` value); ``bye`` from
    the backend when it disconnects the headset."""

    def __init__(self, channel, welcome: Callable[[Any], Awaitable[dict]]) -> None:
        self.channel = channel
        self.welcome = welcome
        self.hello_seen = asyncio.Event()
        channel.on("message", self._on_message)

    def _on_message(self, data) -> None:
        if not isinstance(data, str):
            return
        try:
            msg = json.loads(data)
        except ValueError:
            return
        if isinstance(msg, dict) and msg.get("t") == "hello":
            asyncio.ensure_future(self._answer(msg.get("resume")))

    async def _answer(self, resume) -> None:
        try:
            body = await self.welcome(resume)
        except Exception as exc:  # noqa: BLE001
            logger.warning("[vr-bridge] welcome failed: %s", type(exc).__name__)
            return
        self.hello_seen.set()
        _send_json(self.channel, {"t": "welcome", **body})

    def bye(self, reason: str) -> None:
        try:
            _send_json(self.channel, {"t": "bye", "reason": reason})
        except Exception:  # noqa: BLE001
            pass
