"""The pairing service's bounds (issue #345): what a hostile client can make
it hold or do. Over real WebSockets on 127.0.0.1, or on the service's state
directly; no network beyond the loopback.
"""
from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import random
import socket
from collections import Counter

import pytest
from websockets.asyncio.client import connect
from websockets.datastructures import Headers
from websockets.exceptions import ConnectionClosed, InvalidStatus

from test_vr_pair_service import CANDIDATE, LOCAL, OFFER, Clock, S, _msg, host_conn, new_code, page, recv, run, running


# ── Join failures: per address, and the global brake ─────────────────────────


def test_a_failure_storm_cannot_stop_a_right_code_from_a_clean_address():
    svc = S.PairService(S.Config(global_fails_per_min=5, join_fails_per_ip=20, storm_fails_per_ip=2),
                        clock=Clock())
    host = S.HostConn(ws=None, ip="10.9.9.9")
    # Guessers on many addresses push the failures over the global budget.
    for i in range(10):
        assert svc.claim("ZZZZZZ", f"10.0.0.{i}")[1] == "unknown-code"
    # A user typing the right code from an address with no failures pairs.
    code = svc.issue(host, "pair", None)[0]["code"]
    assert svc.claim(code, "203.0.113.5")[0] is not None
    # During the storm an address may fail storm_fails_per_ip times, then even
    # a right code answers rate-limited (guessing still teaches nothing).
    assert [svc.claim("YYYYYY", "198.51.100.1")[1] for _ in range(3)] == ["unknown-code", "unknown-code",
                                                                          "rate-limited"]
    code2 = svc.issue(host, "pair", None)[0]["code"]
    assert svc.claim(code2, "198.51.100.1")[1] == "rate-limited"
    # One typo before the right code still pairs.
    assert svc.claim("XXXXXX", "198.51.100.2")[1] == "unknown-code"
    assert svc.claim(code2, "198.51.100.2")[0] is not None
    # The global history stays at its threshold however many fail.
    assert svc.global_fails.count("*", svc.clock()) <= 5
    # Out of the storm, the normal per-address limit applies again.
    svc.clock.t += 61
    assert [svc.claim("WWWWWW", "198.51.100.3")[1] for _ in range(3)] == ["unknown-code"] * 3


def test_ipv6_clients_are_limited_per_network_and_addresses_are_cleaned():
    assert S.clean_address("::ffff:192.0.2.1") == "192.0.2.1"
    assert S.clean_address("2001:DB8::1") == "2001:db8::1"
    assert S.clean_address("[2001:db8::1]") == "2001:db8::1"
    for bad in ("1.2.3.4\nINFO forged", "\x1b[31m", "", None, "unknown", "1.2.3.4:80", "fe80::1%eth0", "fe80::1%x INFO forged"):
        assert S.clean_address(bad) == "invalid"
    assert S.rate_key("2001:db8:1:2:aaaa::1") == S.rate_key("2001:db8:1:2:bbbb::9") == "2001:db8:1:2::/64"
    assert S.rate_key("2001:db8:1:3::1") != S.rate_key("2001:db8:1:2::1")
    assert S.rate_key("192.0.2.1") == "192.0.2.1"
    svc = S.PairService(S.Config(join_fails_per_ip=2), clock=Clock())
    reasons = [svc.claim("ZZZZZZ", f"2001:db8:1:2::{i:x}")[1] for i in range(1, 4)]
    assert reasons == ["unknown-code", "unknown-code", "rate-limited"]


class _Conn:
    remote_address = ("127.0.0.1", 5555)

    def __init__(self, headers):
        self.request = type("R", (), {"headers": headers})()


def test_the_client_address_is_the_proxys_last_forwarded_entry():
    svc = S.PairService(S.Config(trust_proxy=True))
    # Apache appends the address it saw to what the client sent.
    assert svc.client_ip(_Conn(Headers([("X-Forwarded-For", "6.6.6.6, 203.0.113.9")]))) == "203.0.113.9"
    # A repeated header is one list, never an error.
    assert svc.client_ip(_Conn(Headers([("X-Forwarded-For", "6.6.6.6"),
                                        ("X-Forwarded-For", "203.0.113.9")]))) == "203.0.113.9"
    assert svc.client_ip(_Conn(Headers([("X-Forwarded-For", "1.2.3.4 INFO forged")]))) == "invalid"
    assert svc.client_ip(_Conn(Headers([("X-Real-IP", "203.0.113.7")]))) == "203.0.113.7"
    assert svc.client_ip(_Conn(Headers())) == "127.0.0.1"
    # Without a proxy the header is ignored: the socket's address counts.
    assert S.PairService(S.Config()).client_ip(_Conn(Headers([("X-Forwarded-For", "6.6.6.6")]))) == "127.0.0.1"


def test_spoofed_forwarded_addresses_do_not_reset_the_failure_limit():
    async def go():
        async with running(trust_proxy=True, join_fails_per_ip=3) as svc:
            reasons = []
            for i in range(5):
                # What Apache forwards when the client wrote its own header.
                ws = await connect(f"{svc.url}/v1/join", origin=LOCAL, proxy=None,
                                   additional_headers={"X-Forwarded-For": f"10.0.0.{i}, 192.0.2.50"})
                await recv(ws)
                await ws.send(_msg(type="join", code="ZZZZZZ", offer=OFFER))
                reasons.append((await recv(ws))["reason"])
                await ws.close()
            assert reasons == ["unknown-code"] * 3 + ["rate-limited"] * 2
    run(go())


# ── Origins and input ─────────────────────────────────────────────────────────


def test_origin_edge_cases():
    svc = S.PairService(S.Config(allowed_origins=("https://vr.example.org",)))
    ok = ("https://vr.example.org", "https://vr.example.org/", "HTTPS://VR.EXAMPLE.ORG")
    bad = ("null", "", "http://vr.example.org", "https://vr.example.org:443", "https://vr.example.org/x",
           "https://vr.example.org.evil.example", "https://vr.example.org.", "https://evil.example")
    assert all(svc.origin_allowed(o, required=True) for o in ok)
    assert not any(svc.origin_allowed(o, required=True) for o in bad)

    async def go():
        async with running(allowed_origins=("https://vr.example.org",)) as svc2:
            port = int(svc2.url.rsplit(":", 1)[1])
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(b"GET /v1/join HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                         b"Origin: https://vr.example.org\r\nOrigin: https://evil.example\r\n\r\n")
            assert (await reader.readline()).startswith(b"HTTP/1.1 403")     # not a 500
            writer.close()
    run(go())


def test_malformed_and_nested_json_is_a_bad_request_not_a_crash():
    assert S._parse("[" * 100_000) is None
    assert S._parse('{"v":1,"type":' + "[" * 100_000 + "}") is None
    assert S._parse('{"v":true,"type":"new_code"}') is None
    assert S._parse(json.dumps({"v": 1, "type": "x" * 33})) is None
    assert S._parse('{"v":1,"type":"new_code","ttl":1' + "0" * 5000 + "}") is None   # int digit limit

    async def go():
        async with running() as svc:
            host, _ = await host_conn(svc)
            for raw in ("[" * 60_000, '{"v":1,"type":"\\ud800","code":"\\u0000"}', "\x00" * 100):
                await host.send(raw)
                assert (await recv(host))["reason"] == "bad-request"
            await host.send(_msg(type="new_code", req="R" * 1000))
            assert "req" not in await recv(host)                # a long req is not echoed
            await host.send(_msg(type="new_code", req=2 ** 60))
            assert "req" not in await recv(host)
            assert (await new_code(host, req="ok"))["req"] == "ok"   # the connection is still fine
            await host.close()
    run(go())


# ── Messages, relay, connections ──────────────────────────────────────────────


def test_a_host_that_floods_messages_is_closed():
    async def go():
        async with running(host_msg_burst=5) as svc:
            host, _ = await host_conn(svc)
            for _ in range(10):
                await host.send(_msg(type="revoke", code="ABCDEF"))
            with pytest.raises(ConnectionClosed):
                for _ in range(20):
                    await recv(host)
            assert host.close_code == 1008
    run(go())


def test_the_relay_carries_signalling_only():
    async def go():
        async with running(candidates_per_peer=3, answers_per_peer=1, candidate_max=300) as svc:
            host, _ = await host_conn(svc)
            p = await page(svc, (await new_code(host))["code"])
            peer = (await recv(host))["peer"]
            # Host to page: one answer, three candidates, nothing else.
            for _ in range(3):
                await host.send(_msg(type="answer", peer=peer, sdp="v=0 a"))
            await host.send(_msg(type="ice", peer=peer, candidate={"candidate": "x" * 1000}))   # too big
            await host.send(_msg(type="ice", peer=peer, candidate={"nested": {"a": [1]}}))      # not flat
            await host.send(_msg(type="answer", peer=peer, sdp=["not", "sdp"]))
            for _ in range(5):
                await host.send(_msg(type="ice", peer=peer, candidate=CANDIDATE))
            got = []
            with pytest.raises(TimeoutError):
                while True:
                    got.append((await recv(p, timeout=0.5))["type"])
            assert got == ["answer", "ice", "ice", "ice"]
            # Page to host: a page that keeps sending past what a signalling
            # exchange needs is closed.
            for _ in range(3 + 16 + 2):
                with contextlib.suppress(ConnectionClosed):
                    await p.send(_msg(type="ice", candidate={"nested": {"a": 1}}))
            with pytest.raises(ConnectionClosed):
                while True:
                    await recv(p, timeout=2)
            assert p.close_code == 1008
            await host.close()
    run(go())


def test_connections_are_bounded_per_endpoint_and_per_address_per_minute():
    async def go():
        async with running(max_hosts=2, max_pages=1) as svc:
            a, _ = await host_conn(svc)
            b, _ = await host_conn(svc)
            with pytest.raises(InvalidStatus) as e:
                await connect(f"{svc.url}/v1/host", proxy=None)
            assert e.value.response.status_code == 503
            # Hosts at their cap keep no page out: a page joins a host.
            p = await page(svc, (await new_code(a))["code"])
            assert (await recv(a))["type"] == "join"
            with pytest.raises(InvalidStatus) as e:
                await connect(f"{svc.url}/v1/join", origin=LOCAL, proxy=None)
            assert e.value.response.status_code == 503
            await p.close()
            await a.close()
            await asyncio.sleep(0.05)
            c, _ = await host_conn(svc)                        # a slot came free
            for w in (b, c):
                await w.close()
        async with running(conns_per_ip_per_min=3) as svc:
            for _ in range(3):
                w, _ = await host_conn(svc)
                await w.close()
            with pytest.raises(InvalidStatus) as e:
                await connect(f"{svc.url}/v1/host", proxy=None)
            assert e.value.response.status_code == 429
            svc.test_clock.t += 61
            w, _ = await host_conn(svc)
            await w.close()
    run(go())


async def _raw_ws(svc, path="/v1/host"):
    port = int(svc.url.rsplit(":", 1)[1])
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    writer.write(f"GET {path} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
    assert (await reader.readline()).startswith(b"HTTP/1.1 101")
    while await reader.readline() not in (b"\r\n", b""):
        pass
    return reader, writer


def _ping_frame(n=125):
    return bytes([0x89, 0x80 | n]) + b"\0\0\0\0" + b"p" * n


def test_a_ping_flood_is_dropped():
    async def go():
        async with running(recv_burst=4096, recv_rate=1024, max_message=1024) as svc:
            reader, writer = await _raw_ws(svc)
            writer.write(_ping_frame() * 200)              # 26 KB at once
            await writer.drain()
            with contextlib.suppress(ConnectionError, TimeoutError):
                while await asyncio.wait_for(reader.read(65536), 2):
                    pass
            assert svc.stats["dropped_inflow"] == 1
            writer.close()
    run(go())


def test_a_client_that_does_not_read_is_dropped():
    async def go():
        # A generous inflow bucket: only the write buffer bounds here.
        async with running(write_buffer_max=8192, recv_burst=10 ** 9, recv_rate=10 ** 9) as svc:
            _, writer = await _raw_ws(svc)
            writer.transport.pause_reading()
            writer.get_extra_info("socket").setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            # Until the server's write buffer passes its cap, not for a fixed
            # count: the kernel's socket buffers in between are sized by the
            # platform (Linux grows loopback buffers to several MB), so the
            # pongs needed to fill them differ by an order of magnitude.
            loop = asyncio.get_running_loop()
            deadline = loop.time() + 30.0
            while loop.time() < deadline:
                if writer.is_closing() or svc.stats["dropped_not_reading"]:
                    break
                writer.write(_ping_frame() * 1000)        # ~131 KB of pings, as many pongs back
                with contextlib.suppress(ConnectionError):
                    await writer.drain()
                await asyncio.sleep(0.005)
            assert svc.stats["dropped_not_reading"] == 1
            writer.transport.abort()
    run(go())


# ── Codes ─────────────────────────────────────────────────────────────────────


def test_code_bookkeeping_stays_consistent_and_bounded():
    rnd = random.Random(345)
    clock = Clock()
    svc = S.PairService(S.Config(max_tombstones=50, max_codes=400, live_codes_per_ip=40, live_codes_per_host=30,
                                 codes_per_host_per_min=10 ** 6, codes_per_ip_per_min=10 ** 6,
                                 join_fails_per_ip=10 ** 6, global_fails_per_min=10 ** 6), clock=clock)
    svc.heap_slack = 8                      # so that the rebuilds run
    hosts = [S.HostConn(ws=None, ip=f"10.0.{i % 5}.{i}") for i in range(12)]
    issued = []
    for _ in range(4000):
        op = rnd.random()
        h = rnd.choice(hosts)
        if op < 0.45:
            r, _ = svc.issue(h, rnd.choice(("pair", "resume")), rnd.choice((None, 1, 30, 600)))
            if r:
                issued.append((h, r))
        elif op < 0.6 and issued:
            h2, r = rnd.choice(issued)
            svc.revoke(h2, r["code"])
        elif op < 0.75 and issued:
            svc.claim(rnd.choice(issued)[1]["code"], f"192.0.2.{rnd.randrange(200)}")
        elif op < 0.8 and issued:
            _, r = rnd.choice(issued)
            if r["kind"] == "resume":
                svc.issue(rnd.choice(hosts), "resume", None, r["code"], r.get("owner"))
        elif op < 0.83:
            svc.host_gone(h)
            hosts[hosts.index(h)] = S.HostConn(ws=None, ip=h.ip)
        else:
            clock.t += rnd.choice((0.5, 5, 60, 700))
        svc.sweep(force=True)
        live = [c for c in svc.codes.values() if c.state == "live"]
        assert svc.live_total == len(live) <= 400
        assert svc.dead_total == len(svc.codes) - len(live) <= 50
        for host in hosts:
            assert host.live == {c.code for c in live if c.host is host}
        assert svc.ip_live == +Counter(svc.key(c.host.ip) for c in live)
        assert all(n <= 40 for n in svc.ip_live.values())
        assert len(svc._expiring) <= 2 * svc.live_total + svc.heap_slack + 1
        assert len(svc._forgetting) <= 2 * svc.dead_total + svc.heap_slack + 1
    assert svc.stats["heap_rebuilds"] > 0                  # compaction ran, and kept the invariants


def test_live_codes_are_bounded_per_address():
    svc = S.PairService(S.Config(live_codes_per_ip=3, live_codes_per_host=10), clock=Clock())
    a, b = S.HostConn(ws=None, ip="10.0.0.1"), S.HostConn(ws=None, ip="10.0.0.1")
    assert [svc.issue(h, "pair", None)[1] for h in (a, b, a, b)] == [None, None, None, "too-many-codes"]


def test_the_owner_key_moves_a_live_resume_code_to_a_reconnected_host():
    # The backend's old connection is half-open (not yet known dead) when it
    # reconnects: its owner key moves the code, even at the caps.
    svc = S.PairService(S.Config(live_codes_per_ip=1, live_codes_per_host=1), clock=Clock())
    old = S.HostConn(ws=None, ip="10.0.0.1")
    reply, _ = svc.issue(old, "resume", None)
    new = S.HostConn(ws=None, ip="10.0.0.1")
    back, reason = svc.issue(new, "resume", None, reply["code"], reply["owner"])
    assert reason is None and svc.codes[reply["code"]].host is new
    assert old.live == set() and new.live == {reply["code"]} and svc.live_total == 1
    # Without the key, or with one that is not url-safe text, nobody takes it.
    thief = S.HostConn(ws=None, ip="10.0.0.9")
    for owner in (None, "x" * 43, "\ud800" * 43, reply["owner"] + "\n"):
        assert svc.issue(thief, "resume", None, reply["code"], owner)[1] == "code-taken"
    assert svc.issue(thief, "resume", None, reply["code"] + "\n", reply["owner"])[1] == "bad-request"


def test_no_field_value_can_end_a_host_or_a_page():
    weird = [10 ** 400, -10 ** 400, 1e308 * 10, "\ud800", "x" * 5000, [], {}, {"a": [1, {"b": None}]}, None, True]

    async def go():
        # Budgets out of the way: this is about the handlers, not the limits.
        async with running(host_msg_burst=10 ** 6, recv_burst=10 ** 9, recv_rate=10 ** 9) as svc:
            host, _ = await host_conn(svc)
            for field_ in ("ttl", "kind", "code", "owner", "req", "peer"):
                for v in weird:
                    for t in ("new_code", "revoke", "answer", "ice", "reject"):
                        body = {"type": t, field_: v}
                        if t == "new_code" and field_ == "owner":
                            body.update(kind="resume", code="A" * 30)
                        await host.send(json.dumps({"v": 1, **body}))
            # Still serving: a code comes back.
            await host.send(_msg(type="new_code", req="end"))
            got = None
            with contextlib.suppress(TimeoutError):
                while True:
                    m = await recv(host, timeout=1)
                    if m.get("req") == "end":
                        got = m
            assert got is not None and got["type"] == "code"
            for v in weird:
                ws = await connect(f"{svc.url}/v1/join", origin=LOCAL, proxy=None)
                await recv(ws)
                await ws.send(json.dumps({"v": 1, "type": "join", "code": v, "offer": {"sdp": "v=0", "x": v}}))
                with contextlib.suppress(ConnectionClosed):
                    assert (await recv(ws))["type"] == "error"
                await ws.close()
            assert svc.stats["host_message_failed"] == 0 and svc.stats["join_failed"] == 0
            await host.close()
    run(go())


def test_a_flood_of_empty_frames_is_dropped_by_the_per_frame_charge():
    async def go():
        async with running() as svc:                      # the defaults
            reader, writer = await _raw_ws(svc)
            empty_ping = bytes([0x89, 0x80]) + b"\0\0\0\0"
            writer.write(empty_ping * 5000)                # 30 KB: under the byte budget alone
            await writer.drain()
            with contextlib.suppress(ConnectionError, TimeoutError):
                while await asyncio.wait_for(reader.read(65536), 2):
                    pass
            assert svc.stats["dropped_inflow"] == 1
            writer.close()
    run(go())


# ── Secrets and the log ───────────────────────────────────────────────────────


def test_secret_files_are_checked(tmp_path, caplog):
    empty = tmp_path / "empty"
    empty.write_text("\n")
    with pytest.raises(SystemExit):
        S.Config.from_env({"VR_PAIR_SECRET_FILE": str(empty)})
    with pytest.raises(SystemExit):
        S.Config.from_env({"VR_PAIR_SECRET_FILE": str(tmp_path / "missing")})
    loose = tmp_path / "loose"
    loose.write_text("a" * 64)
    loose.chmod(0o644)
    with caplog.at_level(logging.WARNING, logger="visivo.vr_pair"):
        cfg = S.Config.from_env({"VR_PAIR_SECRET_FILE": str(loose)})
    assert cfg.secret == "a" * 64 and "readable by others" in caplog.text
    assert "a" * 64 not in caplog.text and "a" * 64 not in repr(cfg)
    shown = repr(S.Config(host_keys=("hk-1",), turn_password="pw-1", turn_secret="ts-1"))
    assert not any(s in shown for s in ("hk-1", "pw-1", "ts-1"))


def test_log_lines_per_address_are_bounded(caplog):
    svc = S.PairService(S.Config(log_lines_per_ip_per_min=3), clock=Clock())
    with caplog.at_level(logging.INFO, logger="visivo.vr_pair"):
        for _ in range(50):
            svc.log("192.0.2.1", "join refused from %s: %s", "192.0.2.1", "unknown-code")
    assert len(caplog.records) == 3 and svc.stats["log_suppressed"] == 47
