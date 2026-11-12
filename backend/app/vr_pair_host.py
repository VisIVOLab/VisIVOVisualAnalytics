"""This backend as a host of the VR pairing service (issue #212, contract
``visivo-pair/1``).

The desktop asks for a code (``POST /v1/vr/pair``); the backend opens an
outbound WebSocket to the pairing service (``VISIVO_VR_PAIR_URL``), keeps it
open, and reconnects with backoff when it drops. When a headset's page joins
with a code the backend issued, the service relays its WebRTC offer here; the
backend answers it with aiortc and serves the page's data channels in-process
(``vr_bridge``). From then on the page talks to this backend directly, on the
local network; the service only carried the offer and answer.

Why outbound: a backend on a user's Mac has no public address and no
certificate. It can always reach out, and WebRTC then finds the direct path.

What a code opens (session, dataset, path and the page's other parameters)
and whose token it acts with are remembered here, never sent to the service
or the page. A pair code is used once; the headset it connected gets a
resume code, so a page whose connection dropped joins again without a new
code from the desktop.

The service address is read once, when pairing is first used: a backend
picks up a changed ``VISIVO_VR_PAIR_URL`` when it restarts.

aiortc is optional: without it pairing reports itself unavailable and
nothing else changes.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import random
import secrets
import time
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import urlsplit

from .auth import token_equal

logger = logging.getLogger("visivo.vr_pair")

PAIR_URL_ENV = "VISIVO_VR_PAIR_URL"
PAGE_URL_ENV = "VISIVO_VR_PAGE_URL"
#: How long a request to the service may wait for its answer, and the desktop
#: for the connection to the service.
REPLY_TIMEOUT_S = 5.0
CONNECT_WAIT_S = 5.0
BACKOFF_MAX_S = 60.0
#: Resume codes: their lifetime (the service caps it) and how long a headset
#: whose connection dropped may come back with one.
RESUME_TTL_S = 12 * 3600
#: WebSockets one headset connection may have open at once (the page opens
#: one per render session and a few more).
MAX_WS_PER_HEADSET = 32


def aiortc_missing() -> str | None:
    """Why the WebRTC side cannot run here, or None when it can."""
    try:
        import aiortc  # noqa: F401, PLC0415
        import av  # noqa: F401, PLC0415
        import websockets  # noqa: F401, PLC0415
    except ImportError as exc:
        return f"{exc.name or 'aiortc'} is not installed"
    return None


class PairingUnavailable(Exception):
    """``reason`` is the contract's: ``not-configured`` or ``unreachable``."""

    def __init__(self, reason: str, detail: str) -> None:
        super().__init__(detail)
        self.reason = reason
        self.detail = detail


def page_url(base: str, code: str) -> str | None:
    """The public loader with ``#pair=<code>`` (the base's own fragment, if
    any, replaced: the code is the only thing the page needs)."""
    if not base:
        return None
    return f"{base.split('#', 1)[0]}#pair={code}"


def host_endpoint(url: str) -> str:
    """The service's host endpoint: ``VISIVO_VR_PAIR_URL`` names the service
    (``wss://pair.example.org``, or with a path prefix behind a proxy), and
    hosts connect to its ``/v1/host``. A query (``?key=…``, the host key a
    service may require) is kept."""
    base, _, query = url.split("#", 1)[0].partition("?")
    base = base.rstrip("/")
    path = base if base.endswith("/v1/host") else f"{base}/v1/host"
    return f"{path}?{query}" if query else path


def _service_label(url: str) -> str:
    """The service's address for the log, without any credentials in it."""
    try:
        parts = urlsplit(url)
        return f"{parts.scheme}://{parts.hostname}{':' + str(parts.port) if parts.port else ''}"
    except ValueError:
        return "<pairing service>"


@dataclass
class Pending:
    code: str
    owner: str
    params: dict
    app: Any
    expires_at: float          # Unix seconds


@dataclass
class Headset:
    id: str
    owner: str
    params: dict
    app: Any
    since: float = 0.0
    connected: bool = False
    transport: str = "webrtc"
    resume_code: str | None = None
    #: The service's owner key of the resume code: what registers it again
    #: after a reconnection (the page knows the code, never this key).
    resume_owner: str | None = None
    resume_expires_at: float = 0.0
    resume_task: asyncio.Task | None = None
    pc: Any = None
    ctl: Any = None
    bridges: list = field(default_factory=list)
    #: Disconnected by the desktop: no connection of it may come up again.
    revoked: bool = False


@dataclass
class _Peer:
    """A page in signalling: its candidates wait until the offer is set."""
    headset: Headset | None = None
    pc: Any = None
    ready: bool = False
    candidates: list = field(default_factory=list)


def _candidate(raw) -> Any:
    """An aiortc candidate from the page's (RTCIceCandidateInit or the
    candidate line), None for end-of-candidates."""
    from aiortc.sdp import candidate_from_sdp  # noqa: PLC0415

    if isinstance(raw, dict):
        line, mid, index = raw.get("candidate") or "", raw.get("sdpMid"), raw.get("sdpMLineIndex")
    else:
        line, mid, index = str(raw or ""), None, 0
    line = line.strip()
    if not line:
        return None
    if line.startswith("a="):
        line = line[2:]
    if line.startswith("candidate:"):
        line = line[len("candidate:"):]
    cand = candidate_from_sdp(line)
    cand.sdpMid = mid if isinstance(mid, str) else None
    cand.sdpMLineIndex = index if isinstance(index, int) else (0 if cand.sdpMid is None else None)
    return cand


def _selected_pair(pc) -> Any:
    """The nominated ICE candidate pair (aiortc keeps it in aioice's
    connection; private, so anything unexpected gives None)."""
    try:
        conn = pc.sctp.transport.transport._connection
        return next(iter(conn._nominated.values()))
    except Exception:  # noqa: BLE001
        return None


def _transport(pc) -> str:
    pair = _selected_pair(pc)
    if pair is None:
        return "webrtc"
    types = {pair.local_candidate.type, pair.remote_candidate.type}
    return "webrtc-relay" if "relay" in types else "webrtc-direct"


class PairHost:
    """The connection to the pairing service, the codes issued through it
    and the headsets connected with them."""

    def __init__(self, url: str, page_base: str = "") -> None:
        self.url = url
        self.page_base = page_base
        self.ws = None
        self.ice_servers: list[dict] = []
        self.pending: dict[str, Pending] = {}
        self.headsets: dict[str, Headset] = {}
        self.peers: dict[str, _Peer] = {}
        self.last_error: str | None = None
        self._connected = asyncio.Event()
        self._waiters: dict[int, asyncio.Future] = {}
        self._req = 0
        self._task: asyncio.Task | None = None
        self._closing = False
        self._tasks: set[asyncio.Task] = set()

    # ── The service connection ──────────────────────────────────────────

    def start(self) -> None:
        if self._task is None or self._task.done():
            self._closing = False
            self._task = asyncio.ensure_future(self._run())

    async def wait_connected(self, timeout: float = CONNECT_WAIT_S) -> bool:
        self.start()
        try:
            await asyncio.wait_for(self._connected.wait(), timeout)
            return True
        except TimeoutError:
            return False

    def _spawn(self, coro) -> asyncio.Task:
        task = asyncio.ensure_future(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
        return task

    async def _run(self) -> None:
        import websockets  # noqa: PLC0415

        delay = 1.0
        label = _service_label(self.url)
        while not self._closing:
            try:
                async with websockets.connect(host_endpoint(self.url), open_timeout=10, ping_interval=20, ping_timeout=20,
                                              close_timeout=3, max_size=256 * 1024) as ws:
                    self.ws = ws
                    delay = 1.0
                    self.last_error = None
                    logger.info("[vr-pair] connected to %s", label)
                    self._connected.set()
                    self._spawn(self._register_resume_codes())
                    async for raw in ws:
                        self._on_message(raw)
                    self.last_error = "closed by the service"
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001 - any failure is a reason to retry
                self.last_error = type(exc).__name__
                logger.warning("[vr-pair] %s: %s; retrying in %.0f s", label, self.last_error, delay)
            finally:
                self._lost()
            if self._closing:
                break
            await asyncio.sleep(delay * (0.5 + random.random() / 2))
            delay = min(delay * 2, BACKOFF_MAX_S)

    def _lost(self) -> None:
        """The service dropped: its pair codes are gone with it (it forgets a
        host's codes), and so are answers still awaited."""
        self.ws = None
        self._connected.clear()
        for fut in self._waiters.values():
            if not fut.done():
                fut.set_exception(PairingUnavailable("unreachable", "The pairing service went away."))
        self._waiters.clear()
        self.pending.clear()

    async def _send(self, msg: dict) -> None:
        ws = self.ws
        if ws is None:
            raise PairingUnavailable("unreachable", "Not connected to the pairing service.")
        try:
            await ws.send(json.dumps({"v": 1, **msg}, separators=(",", ":")))
        except Exception as exc:  # noqa: BLE001 - closed meanwhile
            raise PairingUnavailable("unreachable", "The pairing service went away.") from exc

    async def _request_code(self, kind: str, ttl: int | None = None, code: str | None = None,
                            owner: str | None = None) -> dict:
        self._req += 1
        req = self._req
        fut = asyncio.get_running_loop().create_future()
        self._waiters[req] = fut
        msg: dict = {"type": "new_code", "kind": kind, "req": req}
        if ttl is not None:
            msg["ttl"] = ttl
        if code is not None:
            msg["code"] = code
        if owner is not None:
            msg["owner"] = owner
        try:
            await self._send(msg)
            reply = await asyncio.wait_for(fut, REPLY_TIMEOUT_S)
        except TimeoutError:
            raise PairingUnavailable("unreachable", "The pairing service did not answer.") from None
        finally:
            self._waiters.pop(req, None)
        if reply.get("type") != "code":
            raise PairingUnavailable("unreachable", f"The pairing service refused a code: {reply.get('reason')}.")
        return reply

    def _on_message(self, raw) -> None:
        try:
            msg = json.loads(raw)
        except (TypeError, ValueError):
            return
        if not isinstance(msg, dict) or msg.get("v") != 1:
            return
        t = msg.get("type")
        if t == "ice_servers" and isinstance(msg.get("ice_servers"), list):
            self.ice_servers = msg["ice_servers"]
        elif t in ("code", "error") and msg.get("req") in self._waiters:
            fut = self._waiters[msg["req"]]
            if not fut.done():
                fut.set_result(msg)
        elif t == "join" and isinstance(msg.get("peer"), str):
            # Registered now, before any await: the page's candidates may
            # follow at once.
            self.peers[msg["peer"]] = _Peer()
            self._spawn(self._on_join(msg))
        elif t == "ice" and isinstance(msg.get("peer"), str):
            peer = self.peers.get(msg["peer"])
            if peer is not None:
                if peer.ready:
                    self._spawn(self._add_candidate(peer, msg.get("candidate")))
                else:
                    peer.candidates.append(msg.get("candidate"))

    async def _register_resume_codes(self) -> None:
        """After a reconnection, the headsets' resume codes again (the
        service forgot them with the old connection)."""
        now = time.time()
        for h in list(self.headsets.values()):
            if h.resume_code and h.resume_expires_at > now + 5:
                try:
                    await self._request_code("resume", int(h.resume_expires_at - now), h.resume_code,
                                             h.resume_owner)
                except PairingUnavailable as exc:
                    logger.warning("[vr-pair] a headset's resume code could not be registered again: %s", exc)

    # ── Codes for the desktop ────────────────────────────────────────────

    async def create(self, owner: str, params: dict, app) -> dict:
        if not await self.wait_connected():
            raise PairingUnavailable("unreachable", "The pairing service cannot be reached"
                                     + (f" ({self.last_error})." if self.last_error else "."))
        reply = await self._request_code("pair")
        code = reply["code"]
        expires_at = time.time() + float(reply.get("expires_in", 300))
        self.pending[code] = Pending(code=code, owner=owner, params=dict(params), app=app, expires_at=expires_at)
        return {"code": code, "expires_at": int(expires_at), "page_url": page_url(self.page_base, code)}

    def _prune(self) -> None:
        now = time.time()
        for code in [c for c, p in self.pending.items() if p.expires_at <= now]:
            del self.pending[code]
        for hid in [i for i, h in self.headsets.items()
                    if not h.connected and h.pc is None and h.resume_expires_at <= now
                    and (h.resume_task is None or h.resume_task.done())]:
            del self.headsets[hid]

    def listing(self, owner: str | None) -> dict:
        """Pending codes and connected headsets (``owner`` None: everyone's)."""
        self._prune()
        mine = (lambda o: True) if owner is None else (lambda o: token_equal(o, owner))
        return {
            "pending": [{"code": p.code, "expires_at": int(p.expires_at)}
                        for p in self.pending.values() if mine(p.owner)],
            "connected": [{"id": h.id, "since": int(h.since), "dataset": h.params.get("dataset", ""),
                           "transport": h.transport}
                          for h in self.headsets.values() if h.connected and mine(h.owner)],
        }

    async def remove(self, key: str, owner: str | None) -> str | None:
        """Revokes a pending code or disconnects a headset (by id or resume
        code). ``"revoked"``, ``"disconnected"`` or None when nothing of the
        caller's has that name."""
        mine = (lambda o: True) if owner is None else (lambda o: token_equal(o, owner))
        p = self.pending.get(key) or self.pending.get(key.strip().upper())
        if p is not None and mine(p.owner):
            del self.pending[p.code]
            await self._revoke(p.code)
            return "revoked"
        h = self.headsets.get(key) or next((x for x in self.headsets.values() if x.resume_code == key), None)
        if h is not None and mine(h.owner):
            await self.disconnect(h, "disconnected")
            return "disconnected"
        return None

    async def _revoke(self, code: str) -> None:
        try:
            await self._send({"type": "revoke", "code": code})
        except PairingUnavailable:      # a service that went away forgot it anyway
            pass

    async def disconnect(self, h: Headset, reason: str) -> None:
        # Marked before any await: a resume still negotiating must not come
        # up after this (its state callback and channels check the mark).
        h.revoked = True
        self.headsets.pop(h.id, None)
        for peer_id, peer in list(self.peers.items()):
            if peer.headset is h:
                self.peers.pop(peer_id, None)
                if peer.pc is not None:
                    self._spawn(peer.pc.close())
        if h.resume_code:
            await self._revoke(h.resume_code)
        if h.ctl is not None:
            h.ctl.bye(reason)
            # Let the bye leave before the connection closes.
            for _ in range(25):
                if getattr(h.ctl.channel, "bufferedAmount", 0) == 0:
                    break
                await asyncio.sleep(0.02)
            await asyncio.sleep(0.05)
        await self._close_pc(h)

    async def _close_pc(self, h: Headset) -> None:
        pc, h.pc = h.pc, None
        h.connected = False
        for b in h.bridges:
            b.close()
        h.bridges.clear()
        if pc is not None:
            try:
                await pc.close()
            except Exception:  # noqa: BLE001
                pass

    # ── A headset joins ──────────────────────────────────────────────────

    async def _reject(self, peer_id: str, reason: str) -> None:
        self.peers.pop(peer_id, None)
        try:
            await self._send({"type": "reject", "peer": peer_id, "reason": reason})
        except Exception:  # noqa: BLE001
            pass

    async def _on_join(self, msg: dict) -> None:
        peer_id, kind, code = msg["peer"], msg.get("kind"), msg.get("code")
        offer = msg.get("offer")
        sdp = offer.get("sdp") if isinstance(offer, dict) else offer
        peer = self.peers.get(peer_id)
        if peer is None or not isinstance(sdp, str) or not isinstance(code, str):
            await self._reject(peer_id, "bad-join")
            return
        now = time.time()
        if kind == "pair":
            p = self.pending.pop(code, None)
            if p is None or p.expires_at <= now:
                await self._reject(peer_id, "unknown-code")
                return
            h = Headset(id=secrets.token_hex(6), owner=p.owner, params=p.params, app=p.app)
            self.headsets[h.id] = h
            h.resume_task = self._spawn(self._issue_resume(h))
        elif kind == "resume":
            h = next((x for x in self.headsets.values() if x.resume_code == code), None)
            if h is None or h.resume_expires_at <= now:
                await self._reject(peer_id, "unknown-code")
                return
        else:
            await self._reject(peer_id, "bad-join")
            return
        peer.headset = h
        try:
            await self._answer(peer_id, peer, h, sdp)
        except Exception as exc:  # noqa: BLE001
            logger.warning("[vr-pair] answering a headset failed: %s", type(exc).__name__)
            await self._reject(peer_id, "error")
            if peer.pc is not None:
                try:
                    await peer.pc.close()
                except Exception:  # noqa: BLE001
                    pass
            if h.pc is None and kind == "pair":
                self.headsets.pop(h.id, None)

    async def _issue_resume(self, h: Headset) -> None:
        try:
            reply = await self._request_code("resume", RESUME_TTL_S)
        except PairingUnavailable as exc:
            logger.warning("[vr-pair] no resume code for a headset: %s", exc)
            return
        h.resume_code = reply["code"]
        h.resume_owner = reply.get("owner")
        h.resume_expires_at = time.time() + float(reply.get("expires_in", RESUME_TTL_S))

    def _rtc_configuration(self):
        from aiortc import RTCConfiguration, RTCIceServer  # noqa: PLC0415

        servers = []
        for s in self.ice_servers:
            if not isinstance(s, dict) or not s.get("urls"):
                continue
            try:
                servers.append(RTCIceServer(urls=s["urls"], username=s.get("username"),
                                            credential=s.get("credential")))
            except Exception:  # noqa: BLE001 - one bad entry does not stop the others
                continue
        return RTCConfiguration(iceServers=servers)

    async def _answer(self, peer_id: str, peer: _Peer, h: Headset, sdp: str) -> None:
        from aiortc import RTCPeerConnection, RTCSessionDescription  # noqa: PLC0415

        pc = RTCPeerConnection(self._rtc_configuration())
        peer.pc = pc
        bridges: list = []

        @pc.on("datachannel")
        def on_datachannel(channel) -> None:
            self._attach(h, pc, channel, bridges)

        @pc.on("connectionstatechange")
        async def on_state() -> None:
            state = pc.connectionState
            if state == "connected":
                self.peers.pop(peer_id, None)
                if h.revoked or self.headsets.get(h.id) is not h:
                    await pc.close()            # disconnected meanwhile
                    return
                # Installed before any await, so two resumes coming up at
                # once each replace the one before, and none is left open
                # untracked.
                old, old_bridges = h.pc, h.bridges
                h.pc, h.bridges = pc, bridges
                h.connected, h.since, h.transport = True, time.time(), _transport(pc)
                logger.info("[vr-pair] headset %s connected (%s)", h.id, h.transport)
                if old is not None and old is not pc:
                    # A resumed headset: its old connection gives way.
                    for b in old_bridges:
                        b.close()
                    try:
                        await old.close()
                    except Exception:  # noqa: BLE001
                        pass
            elif state in ("failed", "closed"):
                self.peers.pop(peer_id, None)
                if h.pc is pc:
                    h.connected, h.pc = False, None
                    for b in bridges:
                        b.close()
                    logger.info("[vr-pair] headset %s connection %s", h.id, state)
                if state == "failed":
                    await pc.close()

        await pc.setRemoteDescription(RTCSessionDescription(sdp=sdp, type="offer"))
        peer.ready = True
        for raw in peer.candidates:
            await self._add_candidate(peer, raw)
        peer.candidates.clear()
        await pc.setLocalDescription(await pc.createAnswer())
        # aiortc gathers every candidate before it gives the description: the
        # answer carries them, and no candidate of ours trickles after it.
        await self._send({"type": "answer", "peer": peer_id, "sdp": pc.localDescription.sdp})

    async def _add_candidate(self, peer: _Peer, raw) -> None:
        if peer.pc is None:
            return
        try:
            await peer.pc.addIceCandidate(_candidate(raw))
        except Exception as exc:  # noqa: BLE001 - one bad candidate is not fatal
            logger.debug("[vr-pair] a candidate was refused: %s", type(exc).__name__)

    def _client(self, pc) -> tuple[str, int]:
        pair = _selected_pair(pc)
        try:
            return (str(pair.remote_candidate.host), int(pair.remote_candidate.port))
        except Exception:  # noqa: BLE001
            return ("vr-pair", 0)

    def _attach(self, h: Headset, pc, channel, bridges: list) -> None:
        from . import vr_bridge  # noqa: PLC0415

        label = channel.label
        if h.revoked:
            channel.close()
            return
        if label == "ctl":
            # On a resumed headset, the ctl of the connection that replaces
            # the old one: the bye goes where the page now listens.
            h.ctl = vr_bridge.CtlChannel(channel, lambda resume: self._welcome(h, resume))
        elif label == "api":
            bridges.append(vr_bridge.ApiBridge(channel, h.app, h.owner, self._client(pc)))
        elif label == "ws":
            # Bounded per connection; a finished socket's bridge is let go.
            if sum(isinstance(b, vr_bridge.WsBridge) for b in bridges) >= MAX_WS_PER_HEADSET:
                channel.close()
                return
            bridge = vr_bridge.WsBridge(channel, h.app, h.owner, self._client(pc))
            if bridge.task is not None:
                bridges.append(bridge)
                bridge.task.add_done_callback(lambda _t: bridge in bridges and bridges.remove(bridge))
        else:
            channel.close()

    async def _welcome(self, h: Headset, resume) -> dict:
        from .routers.vr import page_build_async  # noqa: PLC0415

        if h.resume_task is not None and not h.resume_task.done():
            try:
                await asyncio.wait_for(asyncio.shield(h.resume_task), REPLY_TIMEOUT_S)
            except (TimeoutError, Exception):  # noqa: BLE001 - welcome goes without one
                pass
        left = max(0, int(h.resume_expires_at - time.time())) if h.resume_code else 0
        return {
            "session": h.params.get("session", ""),
            "dataset": h.params.get("dataset", ""),
            "path": h.params.get("path", ""),
            "params": dict(h.params),
            "page_build": await page_build_async(),
            "resume_code": h.resume_code if left else None,
            "resume_expires_in": left if left else None,
        }

    async def close(self) -> None:
        self._closing = True
        for h in list(self.headsets.values()):
            if h.ctl is not None:
                h.ctl.bye("backend-stopping")
            await self._close_pc(h)
        for peer in list(self.peers.values()):
            if peer.pc is not None:
                try:
                    await peer.pc.close()
                except Exception:  # noqa: BLE001
                    pass
        self.peers.clear()
        if self.ws is not None:
            try:
                await self.ws.close()
            except Exception:  # noqa: BLE001
                pass
        if self._task is not None:
            self._task.cancel()
            await asyncio.gather(self._task, return_exceptions=True)
        for t in list(self._tasks):
            t.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)


_HOST: PairHost | None = None


def configured_url() -> str:
    return os.environ.get(PAIR_URL_ENV, "").strip()


def get_host() -> PairHost:
    """The host, created at first use from the environment. Raises
    PairingUnavailable(not-configured) when pairing cannot run here."""
    global _HOST
    if _HOST is not None:
        return _HOST
    url = configured_url()
    if not url:
        raise PairingUnavailable("not-configured", f"Headset pairing is off: {PAIR_URL_ENV} is not set.")
    if not url.startswith(("ws://", "wss://")):
        raise PairingUnavailable("not-configured", f"{PAIR_URL_ENV} must be a ws:// or wss:// address.")
    missing = aiortc_missing()
    if missing:
        raise PairingUnavailable("not-configured", f"Headset pairing needs WebRTC support: {missing}.")
    _HOST = PairHost(url, os.environ.get(PAGE_URL_ENV, "").strip())
    return _HOST


def existing_host() -> PairHost | None:
    return _HOST


async def shutdown() -> None:
    global _HOST
    host, _HOST = _HOST, None
    if host is not None:
        await host.close()
