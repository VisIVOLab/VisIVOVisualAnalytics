"""A minimal, in-repo fake of the backend side of the VR pairing contract
(``visivo-pair/1``, issue #210, with its amendment 1), used to check the VR
page in paired mode (issue #213) without the real pairing service (#211) and
bridge (#212). It is one process:

- the page side of the pairing service, ``ws://localhost:<port>/v1/join``:
  ``ice_servers`` first, then ``join {code, offer}`` → ``answer {sdp}``, trickled
  ``ice`` taken in, ``error {reason}`` for an unknown or used code;
- the bridge: an aiortc peer that answers the page's offer, says ``welcome`` on
  ``ctl``, carries HTTP over ``api`` to the real backend (the token added here,
  never sent to the page) and one backend WebSocket per ``ws`` channel.

SIGUSR1 drops every link (a lost Wi-Fi, for the resume check); SIGUSR2 makes
the resume codes unknown (a backend that forgot the pairing). Not a product:
the real service and bridge are #211 and #212.

    python validation/vr_pair_fake.py --port 8822 --backend http://localhost:8820 --code ABC123
        [--session S --dataset D --path P]

The backend token is read from VISIVO_TOKEN and never printed.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import secrets
import signal
import struct
import sys

import httpx
import websockets
from aiortc import RTCPeerConnection, RTCSessionDescription
from aiortc.sdp import candidate_from_sdp

CHUNK = 16 * 1024
HIGH = 1024 * 1024
HOP = {"connection", "keep-alive", "transfer-encoding", "content-encoding", "upgrade"}


def log(*a):
    print("[pair-fake]", *a, file=sys.stderr, flush=True)


class Fake:
    def __init__(self, args):
        self.args = args
        self.token = os.environ.get("VISIVO_TOKEN", "")
        self.pair_code = args.code
        self.pair_used = False
        self.resume_codes: set[str] = set()
        self.pcs: set[RTCPeerConnection] = set()
        self.http = httpx.AsyncClient(base_url=args.backend, timeout=None)
        self.joins = 0

    # ── signalling ──────────────────────────────────────────────────────────
    async def join(self, sock):
        path = getattr(getattr(sock, "request", None), "path", "")
        if not path.startswith("/v1/join"):
            await sock.close(1008, "not here")
            return
        await sock.send(json.dumps({"v": 1, "type": "ice_servers", "ice_servers": []}))
        pc = None
        async for raw in sock:
            m = json.loads(raw)
            if m.get("v") != 1:
                continue
            if m.get("type") == "join" and pc is None:
                code = m.get("code", "")
                kind = None
                if code == self.pair_code and not self.pair_used:
                    kind = "pair"
                    self.pair_used = True
                elif code in self.resume_codes:
                    kind = "resume"
                if kind is None:
                    reason = "used" if code == self.pair_code else "unknown-code"
                    await sock.send(json.dumps({"v": 1, "type": "error", "reason": reason}))
                    continue
                self.joins += 1
                pc = RTCPeerConnection()
                self.pcs.add(pc)
                self.wire(pc)
                await pc.setRemoteDescription(RTCSessionDescription(sdp=m["offer"], type="offer"))
                await pc.setLocalDescription(await pc.createAnswer())
                await sock.send(json.dumps({"v": 1, "type": "answer", "sdp": pc.localDescription.sdp}))
            elif m.get("type") == "ice" and pc is not None:
                c = m.get("candidate") or {}
                text = c.get("candidate", "")
                if text:
                    cand = candidate_from_sdp(text.split(":", 1)[1])
                    cand.sdpMid = c.get("sdpMid")
                    cand.sdpMLineIndex = c.get("sdpMLineIndex")
                    await pc.addIceCandidate(cand)

    # ── the peer ────────────────────────────────────────────────────────────
    def wire(self, pc):
        @pc.on("datachannel")
        def on_dc(dc):
            if dc.label == "ctl":
                self.ctl(dc)
            elif dc.label == "api":
                Api(self, dc)
            elif dc.label == "ws":
                asyncio.ensure_future(WsRelay(self, dc).run())

        @pc.on("connectionstatechange")
        async def on_state():
            if pc.connectionState in ("failed", "closed"):
                self.pcs.discard(pc)

    def ctl(self, dc):
        @dc.on("message")
        def on_msg(raw):
            m = json.loads(raw)
            if m.get("t") != "hello":
                return
            asyncio.ensure_future(self.welcome(dc))

    async def welcome(self, dc):
        info = (await self.http.get("/v1/vr/info")).json()
        code = secrets.token_urlsafe(16)
        self.resume_codes.add(code)
        a = self.args
        params = {k: v for k, v in (("session", a.session), ("dataset", a.dataset), ("path", a.path)) if v}
        dc.send(json.dumps({"t": "welcome", "session": a.session, "dataset": a.dataset, "path": a.path,
                            "params": params, "page_build": info.get("build", ""),
                            "resume_code": code, "resume_expires_in": 3600}))

    async def drop_all(self):
        for pc in list(self.pcs):
            await pc.close()
        self.pcs.clear()
        log("dropped every link")


async def paced(dc, frame):
    while dc.bufferedAmount > HIGH:
        await asyncio.sleep(0.005)
    dc.send(frame)


class Api:
    """HTTP over the api channel, to the real backend."""

    def __init__(self, fake, dc):
        self.fake = fake
        self.dc = dc
        self.reqs: dict[int, dict] = {}
        dc.on("message", self.on_msg)

    def on_msg(self, raw):
        if isinstance(raw, str):
            m = json.loads(raw)
            rid = m.get("id")
            if m.get("t") == "req":
                r = {"m": m, "body": bytearray(), "ready": asyncio.Event()}
                self.reqs[rid] = r
                if not m.get("body"):
                    r["ready"].set()
                r["task"] = asyncio.ensure_future(self.run(rid, r))
            elif m.get("t") == "cancel" and rid in self.reqs:
                self.reqs.pop(rid)["task"].cancel()
            return
        rid, flags = struct.unpack(">IB", raw[:5])
        r = self.reqs.get(rid)
        if r is None:
            return
        r["body"] += raw[5:]
        if flags & 1:
            r["ready"].set()

    async def run(self, rid, r):
        m = r["m"]
        try:
            await r["ready"].wait()
            headers = {k: v for k, v in (m.get("headers") or {}).items() if k.lower() != "x-visivo-token"}
            headers["X-Visivo-Token"] = self.fake.token
            headers["Accept-Encoding"] = "identity"
            req = self.fake.http.build_request(m["method"], m["path"], headers=headers,
                                               content=bytes(r["body"]) if m.get("body") else None)
            resp = await self.fake.http.send(req, stream=True)
            try:
                out = {k: v for k, v in resp.headers.items() if k.lower() not in HOP}
                self.dc.send(json.dumps({"t": "res", "id": rid, "status": resp.status_code, "headers": out}))
                buf = b""
                async for part in resp.aiter_bytes():
                    buf += part
                    while len(buf) > CHUNK:
                        await paced(self.dc, struct.pack(">IB", rid, 0) + buf[:CHUNK])
                        buf = buf[CHUNK:]
                await paced(self.dc, struct.pack(">IB", rid, 1) + buf)
            finally:
                await resp.aclose()
        except asyncio.CancelledError:
            pass
        except Exception as err:  # the backend unreachable
            try:
                self.dc.send(json.dumps({"t": "err", "id": rid, "message": f"bridge: {type(err).__name__}"}))
            except Exception:
                pass
        finally:
            self.reqs.pop(rid, None)


class WsRelay:
    """One page WebSocket over a ws channel, to the backend (fragmented frames)."""

    def __init__(self, fake, dc):
        self.fake = fake
        self.dc = dc
        self.parts = bytearray()
        self.queue: asyncio.Queue = asyncio.Queue()
        dc.on("message", lambda raw: self.queue.put_nowait(raw))
        dc.on("close", lambda: self.queue.put_nowait(None))

    async def run(self):
        spec = json.loads(self.dc.protocol or "{}")
        url = self.fake.args.backend.replace("http", "ws", 1) + spec.get("path", "/")
        protos = list(spec.get("protocols") or []) + [f"visivo-token.{self.fake.token}"]
        try:
            backend = await websockets.connect(url, subprotocols=protos, max_size=None)
        except Exception as err:
            code = getattr(getattr(err, "response", None), "status_code", 0)
            await self.close_page(1006 if not code else 1011, "handshake refused")
            return
        await paced(self.dc, bytes([3, 1]) + json.dumps({"protocol": backend.subprotocol or ""}).encode())
        up = asyncio.ensure_future(self.to_backend(backend))
        try:
            async for msg in backend:
                kind = 1 if isinstance(msg, str) else 2
                data = msg.encode() if isinstance(msg, str) else msg
                for i in range(0, max(len(data), 1), CHUNK):
                    last = i + CHUNK >= len(data)
                    await paced(self.dc, bytes([kind, 1 if last else 0]) + data[i:i + CHUNK])
            code, reason = backend.close_code or 1000, backend.close_reason or ""
        except websockets.ConnectionClosed as e:
            code, reason = e.code or 1006, e.reason or ""
        up.cancel()
        await self.close_page(code, reason)

    async def close_page(self, code, reason):
        try:
            await paced(self.dc, bytes([4, 1]) + json.dumps({"code": code, "reason": reason}).encode())
        except Exception:
            pass

    async def to_backend(self, backend):
        while True:
            raw = await self.queue.get()
            if raw is None:
                await backend.close()
                return
            t, flags = raw[0], raw[1]
            if t == 4:
                c = json.loads(raw[2:] or b"{}")
                await backend.close(c.get("code", 1000), c.get("reason", ""))
                return
            self.parts += raw[2:]
            if flags & 1:
                data = bytes(self.parts)
                self.parts = bytearray()
                await backend.send(data.decode() if t == 1 else data)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8822)
    ap.add_argument("--backend", default="http://localhost:8820")
    ap.add_argument("--code", default="ABC123")
    ap.add_argument("--session", default="")
    ap.add_argument("--dataset", default="")
    ap.add_argument("--path", default="")
    fake = Fake(ap.parse_args())
    loop = asyncio.get_running_loop()
    loop.add_signal_handler(signal.SIGUSR1, lambda: asyncio.ensure_future(fake.drop_all()))
    loop.add_signal_handler(signal.SIGUSR2, fake.resume_codes.clear)
    async with websockets.serve(fake.join, "localhost", fake.args.port, max_size=2 ** 20):
        log(f"listening on ws://localhost:{fake.args.port}/v1/join")
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
