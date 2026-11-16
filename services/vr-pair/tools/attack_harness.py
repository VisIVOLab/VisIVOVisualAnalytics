"""Attack harness for the pairing service (issue #345). LOCAL USE ONLY.

It starts ``vr_pair_service.py`` on a free port of 127.0.0.1 with the
production settings (allowed origin, ``VR_PAIR_TRUST_PROXY=1``, a secret
file), runs each attack against it, and reports for each one what the
service process used (peak resident memory, CPU seconds, open descriptors,
threads), how many log lines it wrote, and whether a legitimate pairing
(host gets a code, a page joins with it, the answer comes back) still
succeeded while the attack ran, and how long it took.

Every client connects to 127.0.0.1 only. Without ``--apache`` the harness
plays the reverse proxy itself: it sends the ``X-Forwarded-For`` header that
Apache would send (the client's own value, if any, then the client's
address), so many client addresses can be simulated from one machine. With
``--apache`` it starts a private Apache httpd (the local binary, a generated
configuration on a free port, the proxy block of ``deploy/apache-pair.conf``)
and goes through it, which shows what the service really sees behind Apache
(all clients are then 127.0.0.1).

There is no option for a target: every address is 127.0.0.1, built in.

    python tools/attack_harness.py                 # every attack, simulated proxy
    python tools/attack_harness.py --only guess_storm,ping_flood
    python tools/attack_harness.py --apache        # the attacks that make sense behind Apache
    python tools/attack_harness.py --micro         # new_code cost against the number of codes held
    python tools/attack_harness.py --service old/vr_pair_service.py --json before.json

Needs ``websockets`` and ``psutil`` (``requirements-dev.txt``).
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import importlib.util
import json
import os
import random
import resource
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import psutil
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed

HERE = Path(__file__).resolve().parent
SERVICE = HERE.parent / "vr_pair_service.py"
APACHE_SNIPPET = HERE.parent / "deploy" / "apache-pair.conf"
ORIGIN = "https://vr.visivolab.oact.inaf.it"
OFFER = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
LOCALHOST = "127.0.0.1"
#: Worker threads of the private Apache (one child process).
APACHE_THREADS = 20


def free_port() -> int:
    with socket.socket() as s:
        s.bind((LOCALHOST, 0))
        return s.getsockname()[1]


def raise_fd_limit(n: int = 8192) -> None:
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = n if hard == resource.RLIM_INFINITY else min(n, hard)
    if soft < want:
        resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))


def ip_of(i: int, net: str = "10") -> str:
    return f"{net}.{(i >> 16) & 255}.{(i >> 8) & 255}.{i & 255}"


class Target:
    """The service under test, as a subprocess, and how clients reach it."""

    def __init__(self, apache: bool, extra_env: dict | None = None, nofile: int = 4096,
                 service: Path = SERVICE, apache_conf: Path = APACHE_SNIPPET) -> None:
        self.apache = apache
        self.apache_conf = apache_conf
        self.port = free_port()
        self.tmp = Path(tempfile.mkdtemp(prefix="vrpair-harness-"))
        secret = self.tmp / "secret"
        secret.write_text(os.urandom(32).hex() + "\n")
        secret.chmod(0o600)
        self.log = self.tmp / "service.log"
        env = {**os.environ, "VR_PAIR_BIND": LOCALHOST, "VR_PAIR_PORT": str(self.port),
               "VR_PAIR_ALLOWED_ORIGINS": ORIGIN, "VR_PAIR_TRUST_PROXY": "1",
               "VR_PAIR_SECRET_FILE": str(secret), "PYTHONDONTWRITEBYTECODE": "1",
               **(extra_env or {})}

        def limits() -> None:          # the production unit's LimitNOFILE
            resource.setrlimit(resource.RLIMIT_NOFILE, (nofile, nofile))

        self._logfh = open(self.log, "w")
        self.proc = subprocess.Popen([sys.executable, str(service)], env=env, stdout=self._logfh,
                                     stderr=subprocess.STDOUT, preexec_fn=limits)
        self.ps = psutil.Process(self.proc.pid)
        self.httpd = None
        self.front = self.port
        try:
            if apache:
                self.front = self._start_apache()
            self._wait_ready()
        except BaseException:
            self.stop()                  # never leave a service or an httpd behind
            raise

    def _start_apache(self) -> int:
        httpd = shutil.which("httpd") or shutil.which("apache2") or "/usr/sbin/httpd"
        mods = next((d for d in ("/usr/libexec/apache2", "/usr/lib/apache2/modules",
                                 "/usr/lib64/httpd/modules") if Path(d, "mod_proxy.so").exists()), None)
        if not Path(httpd).exists() or mods is None:
            raise SystemExit("--apache: no local httpd with mod_proxy found")
        port = free_port()
        snippet = self.apache_conf.read_text().replace("127.0.0.1:8765", f"127.0.0.1:{self.port}")
        www = self.tmp / "www"
        www.mkdir()
        (www / "other-site.txt").write_text("another site on the same server\n")
        load = "\n".join(f"LoadModule {m}_module {mods}/mod_{m}.so" for m in (
            "mpm_event", "authz_core", "unixd", "proxy", "proxy_http", "reqtimeout", "headers",
            "log_config", "version"))
        conf = self.tmp / "httpd.conf"
        conf.write_text(f"""{load}
ServerRoot "{self.tmp}"
Listen 127.0.0.1:{port}
PidFile "{self.tmp}/httpd.pid"
DefaultRuntimeDir "{self.tmp}"
Mutex file:{self.tmp} default
ErrorLog "{self.tmp}/apache_error.log"
LogFormat "%h \\"%r\\" %>s %{{X-Forwarded-For}}i" xff
CustomLog "{self.tmp}/apache_access.log" xff
ServerName localhost
# A small worker pool, so that what the pairing service may hold of it shows.
StartServers 1
ServerLimit 1
ThreadsPerChild {APACHE_THREADS}
MaxRequestWorkers {APACHE_THREADS}
<VirtualHost 127.0.0.1:{port}>
DocumentRoot "{www}"
<Directory "{www}">
    Require all granted
</Directory>
{snippet}
</VirtualHost>
""")
        self.httpd = subprocess.Popen([httpd, "-f", str(conf), "-DFOREGROUND"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        return port

    def _wait_ready(self) -> None:
        for _ in range(100):
            try:
                with socket.create_connection((LOCALHOST, self.front), timeout=0.2) as s:
                    path = "/pair/healthz" if self.apache else "/healthz"
                    s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
                    if s.recv(64).startswith(b"HTTP/1.1 200"):
                        return
            except OSError:
                pass
            time.sleep(0.1)
        raise SystemExit(f"service did not start; its log:\n{self.log.read_text()}"
                         f"Apache, if any: {self.tmp}/apache_error.log")

    def url(self, path: str) -> str:
        return f"ws://{LOCALHOST}:{self.front}{'/pair' if self.apache else ''}{path}"

    def headers(self, ip: str | None, own_xff: str | None = None) -> dict:
        """What the proxy forwards: Apache appends the client's address to
        whatever X-Forwarded-For the client sent."""
        if self.apache:
            return {"X-Forwarded-For": own_xff} if own_xff else {}
        parts = [p for p in (own_xff, ip) if p]
        return {"X-Forwarded-For": ", ".join(parts)} if parts else {}

    def log_lines(self) -> int:
        self._logfh.flush()
        return sum(1 for _ in open(self.log))

    def stop(self) -> None:
        for p in (self.httpd, self.proc):
            if p is not None and p.poll() is None:
                p.terminate()
                try:
                    p.wait(5)
                except subprocess.TimeoutExpired:
                    p.kill()
        self._logfh.close()


class Monitor:
    """Samples the service process every 0.1 s."""

    def __init__(self, target: Target) -> None:
        self.t = target
        self.peak_rss = self.peak_fds = self.peak_threads = 0
        self.alive = True
        self._task = None

    def sample(self) -> None:
        try:
            p = self.t.ps
            self.peak_rss = max(self.peak_rss, p.memory_info().rss)
            self.peak_fds = max(self.peak_fds, p.num_fds())
            self.peak_threads = max(self.peak_threads, p.num_threads())
        except psutil.Error:
            self.alive = False

    async def _run(self) -> None:
        while True:
            self.sample()
            await asyncio.sleep(0.1)

    def __enter__(self):
        self.wall0 = time.monotonic()
        self.cpu0 = sum(self.t.ps.cpu_times()[:2])
        self.lines0 = self.t.log_lines()
        self.sample()
        self._task = asyncio.ensure_future(self._run())
        return self

    def __exit__(self, *exc) -> None:
        self._task.cancel()
        self.sample()
        self.wall = time.monotonic() - self.wall0
        try:
            self.cpu = sum(self.t.ps.cpu_times()[:2]) - self.cpu0
            self.rss_end = self.t.ps.memory_info().rss
            self.fds_end = self.t.ps.num_fds()
        except psutil.Error:
            self.alive, self.cpu, self.rss_end, self.fds_end = False, float("nan"), 0, 0
        self.lines = self.t.log_lines() - self.lines0


# ── Clients ───────────────────────────────────────────────────────────────────


def msg(**kw) -> str:
    return json.dumps({"v": 1, **kw})


async def recv(ws, timeout: float = 5.0) -> dict:
    return json.loads(await asyncio.wait_for(ws.recv(), timeout))


async def ws_open(t: Target, path: str, ip: str | None, origin: str | None = None,
                  own_xff: str | None = None, **kw):
    return await connect(t.url(path), origin=origin, additional_headers=t.headers(ip, own_xff),
                         proxy=None, open_timeout=10, ping_interval=None, max_size=2 ** 20, **kw)


async def legit_pairing(t: Target, host_ip: str = "198.51.100.10", page_ip: str = "203.0.113.5") -> dict:
    """A backend gets a code, a headset joins with it, the answer comes back."""
    t0 = time.perf_counter()
    try:
        host = await ws_open(t, "/v1/host", host_ip)
        try:
            assert (await recv(host))["type"] == "ice_servers"
            await host.send(msg(type="new_code", kind="pair", req=1))
            code = await recv(host)
            if code.get("type") != "code":
                return {"ok": False, "why": f"new_code: {code.get('reason')}", "s": time.perf_counter() - t0}
            page = await ws_open(t, "/v1/join", page_ip, origin=ORIGIN)
            try:
                await recv(page)
                await page.send(msg(type="join", code=code["code"], offer=OFFER))
                while True:          # skip anything else the host hears
                    first = await recv(host)
                    if first.get("type") == "join":
                        break
                await host.send(msg(type="answer", peer=first["peer"], sdp="v=0 answer"))
                ans = await recv(page)
                ok = ans.get("type") == "answer"
                return {"ok": ok, "why": None if ok else f"page got {ans}", "s": round(time.perf_counter() - t0, 3)}
            finally:
                await page.close()
        finally:
            await host.close()
    except Exception as e:  # noqa: BLE001 - any failure is a failed pairing
        return {"ok": False, "why": f"{type(e).__name__}: {e}"[:160], "s": round(time.perf_counter() - t0, 3)}


async def raw_upgrade(t: Target, path: str, headers: dict | None = None, origin: str | None = ORIGIN,
                      extra: str = "") -> tuple[asyncio.StreamReader, asyncio.StreamWriter, bytes]:
    """A WebSocket upgrade written by hand: returns the status line."""
    r, w = await asyncio.open_connection(LOCALHOST, t.front)
    prefix = "/pair" if t.apache else ""
    lines = [f"GET {prefix}{path} HTTP/1.1", f"Host: {LOCALHOST}", "Upgrade: websocket",
             "Connection: Upgrade", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==",
             "Sec-WebSocket-Version: 13"]
    if origin:
        lines.append(f"Origin: {origin}")
    lines += [f"{k}: {v}" for k, v in (headers or {}).items()]
    w.write(("\r\n".join(lines) + "\r\n" + extra + "\r\n").encode("latin-1"))
    await w.drain()
    status = await asyncio.wait_for(r.readline(), 10)
    while await asyncio.wait_for(r.readline(), 10) not in (b"\r\n", b""):
        pass
    return r, w, status


def frame(opcode: int, payload: bytes) -> bytes:
    """A masked client frame (zero mask: the payload is sent as is)."""
    n = len(payload)
    head = bytes([0x80 | opcode])
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 65536:
        head += bytes([0x80 | 126]) + n.to_bytes(2, "big")
    else:
        head += bytes([0x80 | 127]) + n.to_bytes(8, "big")
    return head + b"\0\0\0\0" + payload


# ── Attacks ───────────────────────────────────────────────────────────────────
# Each returns a dict of its own observations; the runner adds the resources
# and a legitimate pairing made while the attack still holds its resources.


async def conn_flood(t: Target, n_ips: int = 400, per_ip: int = 20) -> dict:
    """Many idle page connections from many addresses (the per-address cap
    is 16): what the service lets in, and what it costs."""
    opened, refused = [], 0

    async def one(i: int) -> None:
        nonlocal refused
        try:
            ws = await ws_open(t, "/v1/join", ip_of(i), origin=ORIGIN)
            opened.append(ws)
        except Exception:  # noqa: BLE001 - any refusal counts
            refused += 1

    for start in range(0, n_ips * per_ip, 200):
        await asyncio.gather(*(one(k // per_ip) for k in range(start, min(start + 200, n_ips * per_ip))))
    await asyncio.sleep(0.5)
    alive = sum(1 for ws in opened if ws.state.name == "OPEN")
    pairing = await legit_pairing(t)
    await asyncio.gather(*(ws.close() for ws in opened), return_exceptions=True)
    return {"attempted": n_ips * per_ip, "upgraded": len(opened), "open_after_0.5s": alive,
            "refused_http": refused, "pairing_during": pairing}


async def host_flood(t: Target, n_ips: int = 300, hosts_per_ip: int = 8) -> dict:
    """Anyone may be a host (no host keys in production): many idle hosts."""
    hosts, refused = [], 0

    async def one(i: int) -> None:
        nonlocal refused
        try:
            ws = await ws_open(t, "/v1/host", ip_of(i, "11"))
            hosts.append(ws)
        except Exception:  # noqa: BLE001 - any refusal counts
            refused += 1

    for start in range(0, n_ips * hosts_per_ip, 200):
        await asyncio.gather(*(one(k // hosts_per_ip) for k in range(start, min(start + 200, n_ips * hosts_per_ip))))
    await asyncio.sleep(0.5)
    alive = sum(1 for ws in hosts if ws.state.name == "OPEN")
    pairing = await legit_pairing(t)
    await asyncio.gather(*(ws.close() for ws in hosts), return_exceptions=True)
    return {"attempted": n_ips * hosts_per_ip, "upgraded": len(hosts), "open_after_0.5s": alive,
            "refused_http": refused, "pairing_during": pairing}


async def code_fill(t: Target, n_ips: int = 300) -> dict:
    """Hosts on many addresses ask for as many 24-hour resume codes as they
    may: how many live codes one attacker can hold, and what a legitimate
    host's new_code costs then."""
    hosts = []
    issued = 0

    async def one(i: int) -> None:
        nonlocal issued
        try:
            ws = await ws_open(t, "/v1/host", ip_of(i, "12"))
        except Exception:  # noqa: BLE001 - any refusal counts
            return
        hosts.append(ws)
        await recv(ws)
        for k in range(70):
            await ws.send(msg(type="new_code", kind="resume", ttl=86400, req=k))
            if (await recv(ws)).get("type") != "code":
                break
            issued += 1

    for start in range(0, n_ips, 100):
        await asyncio.gather(*(one(i) for i in range(start, min(start + 100, n_ips))), return_exceptions=True)
    lat = []
    for _ in range(5):
        r = await legit_pairing(t)
        lat.append(r["s"] if r["ok"] else None)
    pairing = await legit_pairing(t)
    await asyncio.gather(*(ws.close() for ws in hosts), return_exceptions=True)
    return {"hosts": len(hosts), "live_codes_issued": issued, "pairing_latency_s": lat,
            "pairing_during": pairing}


async def guess_storm(t: Target, n_ips: int = 60, guesses_per_ip: int = 20) -> dict:
    """Distributed code guessing that exhausts the global failure budget
    (600 a minute by default), then a legitimate user types a right code."""
    outcomes: dict = {}

    async def one(i: int) -> None:
        for _ in range(guesses_per_ip):
            try:
                ws = await ws_open(t, "/v1/join", ip_of(i, "13"), origin=ORIGIN)
                await recv(ws)
                code = "".join(random.choice("23456789ABCDEFGHJKLMNPQRSTUVWXYZ") for _ in range(6))
                await ws.send(msg(type="join", code=code, offer=OFFER))
                reason = (await recv(ws)).get("reason")
                outcomes[reason] = outcomes.get(reason, 0) + 1
                await ws.close()
            except Exception as e:  # noqa: BLE001
                outcomes[type(e).__name__] = outcomes.get(type(e).__name__, 0) + 1

    await asyncio.gather(*(one(i) for i in range(n_ips)))
    pairing = await legit_pairing(t)
    return {"guesses": n_ips * guesses_per_ip, "answers": outcomes, "pairing_during": pairing}


async def nat_colleague(t: Target) -> dict:
    """One user behind an institute's NAT mistypes codes 25 times; a colleague
    behind the same address then pairs."""
    nat = "192.0.2.77"
    reasons = []
    for _ in range(25):
        ws = await ws_open(t, "/v1/join", nat, origin=ORIGIN)
        await recv(ws)
        await ws.send(msg(type="join", code="ZZZZZZ", offer=OFFER))
        reasons.append((await recv(ws)).get("reason"))
        await ws.close()
    pairing = await legit_pairing(t, page_ip=nat)
    return {"typo_answers": {r: reasons.count(r) for r in set(reasons)}, "pairing_during": pairing}


async def junk_input(t: Target) -> dict:
    """Malformed, oversized, deeply nested, wrong-typed and out-of-order
    messages on both endpoints."""
    payloads = [
        "not json", "[" * 50000, '{"v":1,"type":"new_code","x":' + "[" * 30000 + "]" * 30000 + "}",
        '{"v":1,"type":"new_code","ttl":1' + "0" * 5000 + "}", '{"v":1,"type":' + '"x' * 30000 + '"}',
        json.dumps({"v": 1, "type": "new_code", "req": "R" * 60000}),
        json.dumps({"v": 1, "type": "new_code", "kind": {"a": 1}, "ttl": "5"}),
        json.dumps({"v": 1, "type": "revoke", "code": ["x"]}), json.dumps({"v": 1, "type": "answer", "peer": 5}),
        '{"v":1,"type":"\\ud800\\udfff\\ud800","code":"\\u0000\\n\\r"}', json.dumps({"v": True, "type": "x"}),
        json.dumps([1, 2, 3]), "\x00" * 1000, "é" * 30000,
    ]
    out = {"host_errors": 0, "host_no_reply": 0, "host_closed_by": None, "page_closes": []}
    host = await ws_open(t, "/v1/host", "198.51.100.66")
    await recv(host)
    try:
        for p in payloads:
            await host.send(p)
            try:
                r = await recv(host, timeout=1.0)          # revoke and relays have no reply
                out["host_errors"] += r.get("type") == "error"
            except TimeoutError:
                out["host_no_reply"] += 1
        await host.send(b"\x01\x02binary")
        await host.send("x" * (70 * 1024))           # over max_message
        await recv(host)
    except ConnectionClosed as e:
        out["host_closed_by"] = f"{e.rcvd.code if e.rcvd else None}"
    except TimeoutError:
        out["host_closed_by"] = "timeout"
    await host.close()
    for p in (msg(type="ice", candidate={}), msg(type="join"), msg(type="join", code=None, offer=OFFER),
              msg(type="join", code="A" * 10000, offer=OFFER), msg(type="join", code="ABCDEF", offer={"sdp": 5}),
              "[" * 50000, msg(type="join", code="‮ABCDEF\n", offer=OFFER)):
        ws = await ws_open(t, "/v1/join", "198.51.100.67", origin=ORIGIN)
        await recv(ws)
        await ws.send(p)
        try:
            r = await recv(ws)
            out["page_closes"].append(r.get("reason"))
            await ws.close()
        except ConnectionClosed as e:
            out["page_closes"].append(e.rcvd.code if e.rcvd else None)
    out["pairing_during"] = await legit_pairing(t)
    return out


async def relay_abuse(t: Target, seconds: float = 8.0) -> dict:
    """An attacker runs a host and a page and pushes data host -> page and
    page -> host through the service as fast as it can."""
    host = await ws_open(t, "/v1/host", "198.51.100.80")
    await recv(host)
    await host.send(msg(type="new_code", kind="pair"))
    code = (await recv(host))["code"]
    page = await ws_open(t, "/v1/join", "198.51.100.81", origin=ORIGIN)
    await recv(page)
    await page.send(msg(type="join", code=code, offer=OFFER))
    peer = (await recv(host))["peer"]
    blob = "x" * 60000
    got = {"to_page": 0, "to_host": 0}

    async def push_host() -> None:
        with contextlib.suppress(ConnectionClosed):
            while True:
                await host.send(msg(type="ice", peer=peer, candidate={"candidate": blob}))
                await host.send(msg(type="answer", peer=peer, sdp=blob))

    async def push_page() -> None:
        with contextlib.suppress(ConnectionClosed):
            while True:
                await page.send(msg(type="ice", candidate={"candidate": blob}))

    async def drain(ws, key) -> None:
        with contextlib.suppress(ConnectionClosed):
            async for m in ws:
                got[key] += len(m)

    tasks = [asyncio.ensure_future(c) for c in (push_host(), push_page(), drain(page, "to_page"),
                                                 drain(host, "to_host"))]
    await asyncio.sleep(seconds)
    pairing = await legit_pairing(t)
    for k in tasks:
        k.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    for ws in (host, page):
        await ws.close()
    return {"seconds": seconds, "relayed_to_page_MB": round(got["to_page"] / 1e6, 2),
            "relayed_to_host_MB": round(got["to_host"] / 1e6, 2), "pairing_during": pairing}


async def ping_flood(t: Target, seconds: float = 8.0, conns: int = 4, size: int = 125) -> dict:
    """Pings as fast as possible, never reading the pongs."""
    sent = 0
    writers = []
    for i in range(conns):
        _, w, status = await raw_upgrade(t, "/v1/host", t.headers(ip_of(i, "14")), origin=None)
        assert b"101" in status, status
        writers.append(w)
    ping = frame(0x9, b"p" * size) * (65536 // (size + 6))
    t_end = time.monotonic() + seconds

    async def flood(w) -> None:
        nonlocal sent
        with contextlib.suppress(OSError, ConnectionError):
            while time.monotonic() < t_end and not w.is_closing():
                w.write(ping)
                sent += len(ping)
                await asyncio.wait_for(w.drain(), 2)

    with contextlib.suppress(TimeoutError):
        await asyncio.gather(*(flood(w) for w in writers))
    pairing = await legit_pairing(t)
    for w in writers:
        w.transport.abort()
    return {"client_wrote_MB": round(sent / 1e6, 1), "pairing_during": pairing}


async def empty_ping_flood(t: Target) -> dict:
    """Empty pings (6 bytes each): the most frames per byte."""
    return await ping_flood(t, size=0, conns=8)


async def slow_reader(t: Target, seconds: float = 8.0) -> dict:
    """A page that never reads while its (attacker's own) host sends to it."""
    host = await ws_open(t, "/v1/host", "198.51.100.90")
    await recv(host)
    await host.send(msg(type="new_code", kind="pair"))
    code = (await recv(host))["code"]
    page = await ws_open(t, "/v1/join", "198.51.100.91", origin=ORIGIN)
    await recv(page)
    await page.send(msg(type="join", code=code, offer=OFFER))
    peer = (await recv(host))["peer"]
    page.transport.pause_reading()
    blob = "y" * 60000
    sent = 0

    async def push() -> None:
        nonlocal sent
        with contextlib.suppress(ConnectionClosed):
            while True:
                await host.send(msg(type="ice", peer=peer, candidate={"candidate": blob}))
                sent += len(blob)

    task = asyncio.ensure_future(push())
    await asyncio.sleep(seconds)
    pairing = await legit_pairing(t)
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)
    page.transport.abort()
    await host.close()
    return {"host_wrote_MB": round(sent / 1e6, 1), "pairing_during": pairing}


async def slowloris(t: Target, n: int = 600) -> dict:
    """Connections that send their request headers a byte at a time."""
    writers = []
    for i in range(n):
        try:
            _, w = await asyncio.open_connection(LOCALHOST, t.front)
            w.write(b"GET /v1/join HTTP/1.1\r\nHost: x\r\n")
            writers.append(w)
        except OSError:
            break
    t0 = time.monotonic()
    while time.monotonic() - t0 < 12:
        for w in writers:
            if not w.is_closing():
                w.write(b"X-a: b\r\n")
        await asyncio.sleep(1)
    still = 0
    for w in writers:
        try:
            w.write(b"X-a: b\r\n")
            await asyncio.wait_for(w.drain(), 1)
            still += 0 if w.transport.is_closing() else 1
        except (OSError, ConnectionError, TimeoutError):
            pass
    await asyncio.sleep(0.5)
    still = sum(1 for w in writers if not w.transport.is_closing() and not w.transport.get_extra_info("socket") is None
                and _peer_open(w))
    pairing = await legit_pairing(t)
    for w in writers:
        w.transport.abort()
    return {"opened": len(writers), "still_open_after_12s": still, "pairing_during": pairing}


def _peer_open(w) -> bool:
    sock = w.transport.get_extra_info("socket")
    try:
        data = sock.recv(1, socket.MSG_PEEK | socket.MSG_DONTWAIT)
        return data != b""
    except BlockingIOError:
        return True
    except OSError:
        return False


async def origin_edges(t: Target) -> dict:
    """The status of an upgrade to /v1/join for each Origin variant."""
    cases = {"exact": ORIGIN, "trailing slash": ORIGIN + "/", "upper case": ORIGIN.upper(),
             "explicit :443": ORIGIN + ":443", "http": ORIGIN.replace("https", "http"),
             "with path": ORIGIN + "/evil", "suffix host": ORIGIN + ".evil.example",
             "null": "null", "missing": None, "empty": "", "localhost": "http://localhost:8080",
             "trailing dot": ORIGIN + "."}
    out = {}
    for name, origin in cases.items():
        _, w, status = await raw_upgrade(t, "/v1/join", t.headers("198.51.100.100"), origin=origin)
        out[name] = status.split()[1].decode() if status else "?"
        w.transport.abort()
    _, w, status = await raw_upgrade(t, "/v1/join", t.headers("198.51.100.100"), origin=ORIGIN,
                                     extra="Origin: https://evil.example\r\n")
    out["two Origin headers"] = status.split()[1].decode() if status else "?"
    w.transport.abort()
    return out


async def xff_spoof(t: Target) -> dict:
    """A client sends its own X-Forwarded-For to dodge the per-address
    failure limit. Through Apache (``--apache``) this is the real test; in
    the simulated proxy the harness appends the real address as Apache does."""
    answers = []
    for i in range(30):
        own = f"{ip_of(i, '172')}"
        headers = t.headers("192.0.2.200", own_xff=own)
        ws = await connect(t.url("/v1/join"), origin=ORIGIN, additional_headers=headers, proxy=None)
        await recv(ws)
        await ws.send(msg(type="join", code="QQQQQQ", offer=OFFER))
        answers.append((await recv(ws)).get("reason"))
        await ws.close()
    # Two X-Forwarded-For header lines.
    _, w, status = await raw_upgrade(t, "/v1/join", t.headers("192.0.2.201"), extra="X-Forwarded-For: 1.1.1.1\r\n")
    w.transport.abort()
    return {"answers": {r: answers.count(r) for r in set(answers)},
            "rate_limited_after": answers.index("rate-limited") if "rate-limited" in answers else None,
            "two_xff_headers_status": status.split()[1].decode() if status else "?"}


async def log_injection(t: Target) -> dict:
    """Control characters in the fields the service may log."""
    before = t.log.read_text()
    weird = "1.2.3.4\x1b[31mFAKE\tline"
    with contextlib.suppress(Exception):
        _, w, _ = await raw_upgrade(t, "/v1/join", {"X-Forwarded-For": weird})
        w.transport.abort()
    ws = await ws_open(t, "/v1/join", "198.51.100.120", origin=ORIGIN)
    await recv(ws)
    await ws.send(msg(type="join", code="ABC\nINFO forged line", offer=OFFER))
    with contextlib.suppress(Exception):
        await recv(ws)
    await ws.close()
    await asyncio.sleep(0.2)
    new = t.log.read_text()[len(before):]
    return {"forged_line_in_log": "forged line" in new, "escape_in_log": "\x1b" in new,
            "new_log_lines": new.count("\n")}


async def apache_workers(t: Target, n: int = 40) -> dict:
    """Behind Apache: open many WebSockets to the service through it, then
    fetch a static file of "another site" on the same Apache. Every open
    WebSocket holds an Apache worker thread; the snippet's ``max=`` decides
    whether the pairing service can take them all. Run it with the service's
    per-address limits raised (all clients are 127.0.0.1 here)."""
    if not t.apache:
        return {"skipped": "needs --apache"}
    held, statuses = [], {}
    for i in range(n):
        try:
            r, w, status = await asyncio.wait_for(raw_upgrade(t, "/v1/host", origin=None), 6)
            code = status.split()[1].decode() if status else "?"
        except Exception as e:  # noqa: BLE001
            code, w = type(e).__name__, None
        statuses[code] = statuses.get(code, 0) + 1
        if w is not None:
            held.append(w)
    t0 = time.perf_counter()
    try:
        r, w = await asyncio.open_connection(LOCALHOST, t.front)
        w.write(b"GET /other-site.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        line = await asyncio.wait_for(r.readline(), 10)
        other = {"status": line.split()[1].decode(), "s": round(time.perf_counter() - t0, 3)}
        w.close()
    except Exception as e:  # noqa: BLE001
        other = {"status": type(e).__name__, "s": round(time.perf_counter() - t0, 3)}
    # A headset's page while the hosts hold their pool.
    try:
        _, w, status = await asyncio.wait_for(raw_upgrade(t, "/v1/join"), 6)
        page = status.split()[1].decode() if status else "?"
        w.transport.abort()
    except Exception as e:  # noqa: BLE001
        page = type(e).__name__
    for w in held:
        w.transport.abort()
    return {"apache_threads": APACHE_THREADS, "upgrades": statuses, "other_site_while_held": other,
            "page_upgrade_while_held": page}


ATTACKS = {f.__name__: f for f in (origin_edges, xff_spoof, log_injection, junk_input, nat_colleague, guess_storm,
                                    conn_flood, host_flood, code_fill, relay_abuse, ping_flood, empty_ping_flood, slow_reader,
                                    slowloris, apache_workers)}
#: Behind Apache every client is 127.0.0.1: only these make sense there.
APACHE_ATTACKS = ("origin_edges", "xff_spoof", "log_injection", "junk_input", "relay_abuse", "ping_flood", "empty_ping_flood",
                  "slow_reader", "slowloris", "apache_workers")


async def run_one(name: str, apache: bool, env: dict | None, service: Path, apache_conf: Path) -> dict:
    t = Target(apache, env, service=service, apache_conf=apache_conf)
    try:
        with Monitor(t) as m:
            base = await legit_pairing(t)
            result = await ATTACKS[name](t)
            await asyncio.sleep(1.0)
        after = await legit_pairing(t)
        return {"attack": name, "baseline_pairing": base, **result, "pairing_after": after,
                "service_alive": t.proc.poll() is None and m.alive,
                "peak_rss_MB": round(m.peak_rss / 2 ** 20, 1), "end_rss_MB": round(m.rss_end / 2 ** 20, 1),
                "cpu_s": round(m.cpu, 2), "wall_s": round(m.wall, 1), "peak_fds": m.peak_fds,
                "end_fds": m.fds_end, "peak_threads": m.peak_threads, "log_lines": m.lines,
                "tracebacks": t.log.read_text().count("Traceback")}
    finally:
        t.stop()


def micro(service: Path, sizes=(1_000, 20_000, 100_000)) -> list[dict]:
    """In-process: what one new_code costs when the service already holds
    ``n`` live codes and ``n`` remembered ones (what an attacker with many
    addresses can make it hold). The codes are put in place through the
    service's own bookkeeping where it has one, so both versions compare."""
    spec = importlib.util.spec_from_file_location("svc_under_test", service)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod            # dataclasses look their module up
    spec.loader.exec_module(mod)
    out = []
    for n in sizes:
        cfg = mod.Config(max_codes=10 * n, max_tombstones=10 * n, codes_per_host_per_min=10 ** 9,
                         codes_per_ip_per_min=10 ** 9, live_codes_per_host=10 ** 9)
        if hasattr(cfg, "live_codes_per_ip"):
            cfg.live_codes_per_ip = 10 ** 9
        svc = mod.PairService(cfg, clock=lambda: 1000.0)
        owner = mod.HostConn(ws=None, ip="10.0.0.1")
        gone = mod.HostConn(ws=None, ip="10.0.0.2")
        for i in range(2 * n):
            h = owner if i < n else gone
            c = mod.Code(code=f"C{i:08d}", kind="resume", host=h, expires=1000.0 + 3600)
            if hasattr(svc, "_add"):
                svc._add(c)
            else:
                svc.codes[c.code] = c
        svc.host_gone(gone)
        h = mod.HostConn(ws=None, ip="10.0.0.3")
        t0 = time.perf_counter()
        k = 200
        for _ in range(k):
            assert svc.issue(h, "pair", None)[1] is None
        per = (time.perf_counter() - t0) / k
        t0 = time.perf_counter()
        svc.sweep(force=True)
        out.append({"codes_live": n, "codes_remembered": n, "new_code_ms": round(per * 1e3, 3),
                    "sweep_ms": round((time.perf_counter() - t0) * 1e3, 3)})
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--only", help="comma-separated attack names: " + ", ".join(ATTACKS))
    ap.add_argument("--apache", action="store_true", help="go through a private local Apache httpd")
    ap.add_argument("--env", action="append", default=[], help="extra VR_PAIR_*=value for the service")
    ap.add_argument("--service", type=Path, default=SERVICE, help="the service file to attack (default: this one)")
    ap.add_argument("--apache-conf", type=Path, default=APACHE_SNIPPET,
                    help="the proxy block to put in the private Apache's virtual host")
    ap.add_argument("--micro", action="store_true", help="only the in-process new_code benchmark")
    ap.add_argument("--json", help="write the results to this file")
    a = ap.parse_args()
    if a.micro:
        results = micro(a.service)
        for r in results:
            print(json.dumps(r), flush=True)
    else:
        raise_fd_limit()
        names = a.only.split(",") if a.only else [n for n in ATTACKS if not a.apache or n in APACHE_ATTACKS]
        env = dict(e.split("=", 1) for e in a.env)
        results = []
        for name in names:
            r = asyncio.run(run_one(name, a.apache, env, a.service, a.apache_conf))
            print(json.dumps(r), flush=True)
            results.append(r)
    if a.json:
        Path(a.json).write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()
