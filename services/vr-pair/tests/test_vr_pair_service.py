"""The pairing service (issue #211), over real WebSockets on 127.0.0.1.

No network beyond the loopback: every test starts the service on a free port
of this machine, connects a host and pages to it, and stops it again. The
service's clock is a test clock, so expiry needs no waiting.
"""
from __future__ import annotations

import asyncio
import base64
import contextlib
import hashlib
import hmac
import json
import logging
import sys
from pathlib import Path

import pytest
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vr_pair_service as S  # noqa: E402

OFFER = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=SECRET-SDP-MARKER\r\nt=0 0\r\n"
CANDIDATE = {"candidate": "candidate:1 1 udp 2122260223 192.168.1.20 50000 typ host",
             "sdpMid": "0", "sdpMLineIndex": 0}
LOCAL = "http://localhost:8811"


class Clock:
    def __init__(self) -> None:
        self.t = 1000.0

    def __call__(self) -> float:
        return self.t


@contextlib.asynccontextmanager
async def running(**overrides):
    cfg = S.Config(**overrides)
    clock = Clock()
    service = S.PairService(cfg, clock=clock)
    async with service.serve("127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        service.url = f"ws://127.0.0.1:{port}"
        service.test_clock = clock
        yield service


def _msg(**kw) -> str:
    return json.dumps({"v": 1, **kw})


async def recv(ws, timeout: float = 5.0) -> dict:
    return json.loads(await asyncio.wait_for(ws.recv(), timeout))


async def host_conn(service, **kw):
    ws = await connect(f"{service.url}/v1/host", proxy=None, **kw)
    first = await recv(ws)
    assert first["type"] == "ice_servers"
    return ws, first


async def new_code(ws, kind="pair", **kw) -> dict:
    await ws.send(_msg(type="new_code", kind=kind, **kw))
    return await recv(ws)


async def page(service, code, offer=OFFER, origin=LOCAL):
    ws = await connect(f"{service.url}/v1/join", origin=origin, proxy=None)
    assert (await recv(ws))["type"] == "ice_servers"
    await ws.send(_msg(type="join", code=code, offer=offer))
    return ws


def run(coro):
    return asyncio.run(coro)


# ── Pure parts ────────────────────────────────────────────────────────────────


def test_the_alphabet_has_no_look_alikes_and_a_code_holds_30_bits():
    assert len(set(S.PAIR_ALPHABET)) == 32 and not set("0O1I") & set(S.PAIR_ALPHABET)
    assert S.PAIR_LEN * 5 >= 30
    assert S.RESUME_BYTES * 8 >= 128


def test_a_typed_code_is_normalised_but_a_resume_code_keeps_its_case():
    assert S.normalize_code(" k7q-4mx ") == "K7Q4MX"
    assert S.normalize_code("k7q 4mx") == "K7Q4MX"
    resume = "aB-cd_efGhijKLmnopQRstUVwx"
    assert S.normalize_code(resume) == resume
    assert S.normalize_code("O0O0O0") == "O0O0O0"     # not from the alphabet: as typed


def test_turn_credentials_follow_the_coturn_rest_convention():
    user, cred = S.turn_credentials("s3cret", 600, "visivo", now=1_700_000_000)
    assert user == "1700000600:visivo"
    want = base64.b64encode(hmac.new(b"s3cret", user.encode(), hashlib.sha1).digest()).decode()
    assert cred == want


def test_configuration_comes_from_the_environment(tmp_path):
    secret = tmp_path / "turn.secret"
    secret.write_text("from-a-file\n")
    cfg = S.Config.from_env({
        "VR_PAIR_PORT": "8812", "VR_PAIR_ALLOWED_ORIGINS": "https://a.example, https://b.example",
        "VR_PAIR_STUN_URLS": "stun:stun.example:3478", "VR_PAIR_TURN_URLS": "turn:t.example:3478?transport=udp",
        "VR_PAIR_TURN_SECRET_FILE": str(secret), "VR_PAIR_TRUST_PROXY": "1", "VR_PAIR_CODE_TTL": "120"})
    assert cfg.port == 8812 and cfg.trust_proxy and cfg.pair_ttl == 120
    assert cfg.allowed_origins == ("https://a.example", "https://b.example")
    assert cfg.turn_secret == "from-a-file"
    with pytest.raises(SystemExit):
        S.Config.from_env({"VR_PAIR_PORT": "x"})
    with pytest.raises(SystemExit):
        S.Config.from_env({"VR_PAIR_TLS_CERT": "/c.pem"})
    # No host name built in: nothing configured, no ICE servers.
    assert S.PairService(S.Config.from_env({})).ice_servers() == []


def test_origins():
    svc = S.PairService(S.Config())
    assert svc.origin_allowed("http://localhost:8080", required=True)
    assert svc.origin_allowed("https://127.0.0.1", required=True)
    assert not svc.origin_allowed("https://evil.example", required=True)
    assert not svc.origin_allowed(None, required=True)          # a page always has one
    assert svc.origin_allowed(None, required=False)             # a backend has none
    svc = S.PairService(S.Config(allowed_origins=("https://vr.example.org",)))
    assert svc.origin_allowed("https://vr.example.org", required=True)
    assert not svc.origin_allowed("http://localhost:8080", required=True)
    assert S.PairService(S.Config(allowed_origins=("*",))).origin_allowed("https://x", required=True)


# ── Over WebSockets ───────────────────────────────────────────────────────────


def test_a_page_pairs_with_a_host_and_signalling_flows_both_ways():
    async def go():
        async with running(turn_urls=("turn:turn.test:3478",), turn_secret="k",
                           stun_urls=("stun:stun.test:3478",)) as svc:
            host, first = await host_conn(svc)
            servers = first["ice_servers"]
            assert servers[0] == {"urls": ["stun:stun.test:3478"]}
            assert servers[1]["urls"] == ["turn:turn.test:3478"] and ":" in servers[1]["username"]
            code = await new_code(host, req=7)
            assert code["type"] == "code" and code["kind"] == "pair" and code["req"] == 7
            assert code["expires_in"] == 300 and len(code["code"]) == 6
            typed = code["code"].lower()[:3] + "-" + code["code"].lower()[3:]
            p = await page(svc, typed)
            join = await recv(host)
            assert join["type"] == "join" and join["offer"] == OFFER and join["kind"] == "pair"
            assert join["code"] == code["code"]
            await p.send(_msg(type="ice", candidate=CANDIDATE))
            assert await recv(host) == {"v": 1, "type": "ice", "peer": join["peer"], "candidate": CANDIDATE}
            await host.send(_msg(type="answer", peer=join["peer"], sdp="v=0 answer"))
            assert await recv(p) == {"v": 1, "type": "answer", "sdp": "v=0 answer"}
            await host.send(_msg(type="ice", peer=join["peer"], candidate=CANDIDATE))
            assert await recv(p) == {"v": 1, "type": "ice", "candidate": CANDIDATE}
            # An offer as an object is relayed as given.
            code2 = await new_code(host)
            p2 = await page(svc, code2["code"], offer={"type": "offer", "sdp": OFFER})
            assert (await recv(host))["offer"] == {"type": "offer", "sdp": OFFER}
            for ws in (p, p2, host):
                await ws.close()
    run(go())


def test_a_pair_code_is_single_use_and_expires():
    async def go():
        async with running() as svc:
            host, _ = await host_conn(svc)
            code = (await new_code(host))["code"]
            p = await page(svc, code)
            await recv(host)
            again = await page(svc, code)
            assert await recv(again) == {"v": 1, "type": "error", "reason": "used"}
            late = (await new_code(host, ttl=60))["code"]
            svc.test_clock.t += 61
            p3 = await page(svc, late)
            assert (await recv(p3))["reason"] == "expired"
            # Remembered for a while, then unknown.
            svc.test_clock.t += svc.config.tombstone_s + 120
            p4 = await page(svc, late)
            assert (await recv(p4))["reason"] == "unknown-code"
            p5 = await page(svc, "ZZZZZZ")
            assert (await recv(p5))["reason"] == "unknown-code"
            for ws in (p, again, p3, p4, p5, host):
                await ws.close()
    run(go())


def test_ttls_are_capped_and_bad_requests_answered():
    async def go():
        async with running() as svc:
            host, _ = await host_conn(svc)
            assert (await new_code(host, ttl=10 ** 6))["expires_in"] == svc.config.pair_ttl_max
            assert (await new_code(host, kind="resume"))["expires_in"] == 12 * 3600
            assert (await new_code(host, kind="other"))["reason"] == "bad-request"
            assert (await new_code(host, ttl=-1))["reason"] == "bad-request"
            for bad in ("NaN", "Infinity"):
                await host.send('{"v":1,"type":"new_code","ttl":%s}' % bad)
                assert (await recv(host))["reason"] == "bad-request"
            await host.send("not json")
            assert (await recv(host))["reason"] == "bad-request"
            await host.send(json.dumps({"v": 2, "type": "new_code"}))
            assert (await recv(host))["reason"] == "bad-request"
            await host.close()
    run(go())


def test_a_revoked_code_fails_as_expired():
    async def go():
        async with running() as svc:
            host, _ = await host_conn(svc)
            code = (await new_code(host))["code"]
            await host.send(_msg(type="revoke", code=code))
            await asyncio.sleep(0.05)
            p = await page(svc, code)
            assert (await recv(p))["reason"] == "expired"
            # Another host cannot revoke it.
            other, _ = await host_conn(svc)
            code2 = (await new_code(host))["code"]
            await other.send(_msg(type="revoke", code=code2))
            await asyncio.sleep(0.05)
            p2 = await page(svc, code2)
            assert (await recv(host))["type"] == "join"
            for ws in (p, p2, host, other):
                await ws.close()
    run(go())


def test_a_resume_code_is_reusable_and_registered_again_after_a_reconnection():
    async def go():
        async with running() as svc:
            host, _ = await host_conn(svc)
            code = await new_code(host, kind="resume")
            assert code["kind"] == "resume" and len(code["code"]) >= 22
            pages = []
            for _ in range(3):
                pages.append(await page(svc, code["code"]))
                join = await recv(host)
                assert join["kind"] == "resume" and join["code"] == code["code"]
            # Case matters for a resume code.
            wrong = await page(svc, code["code"].swapcase())
            assert (await recv(wrong))["reason"] == "unknown-code"
            await host.close()
            await asyncio.sleep(0.05)
            gone = await page(svc, code["code"])
            assert (await recv(gone))["reason"] == "host-gone"
            # Someone who knows the code (the page does) cannot claim it
            # while its host is away: the owner key is the host's alone.
            squatter, _ = await host_conn(svc)
            assert (await new_code(squatter, kind="resume", code=code["code"]))["reason"] == "code-taken"
            assert (await new_code(squatter, kind="resume", code=code["code"],
                                   owner="guess"))["reason"] == "code-taken"
            # The backend reconnects and registers it again, with its key.
            host2, _ = await host_conn(svc)
            back = await new_code(host2, kind="resume", code=code["code"], owner=code["owner"])
            assert back["code"] == code["code"] and back["owner"] == code["owner"]
            p = await page(svc, code["code"])
            assert (await recv(host2))["code"] == code["code"]
            # A live code of another host cannot be taken, nor a pair code chosen.
            thief, _ = await host_conn(svc)
            assert (await new_code(thief, kind="resume", code=code["code"]))["reason"] == "code-taken"
            assert (await new_code(thief, kind="pair", code="ABCDEF"))["reason"] == "bad-request"
            assert (await new_code(thief, kind="resume", code="short"))["reason"] == "bad-request"
            for ws in (*pages, wrong, gone, p, host2, thief, squatter):
                await ws.close()
    run(go())


def test_a_page_hears_when_its_host_goes_or_rejects_it():
    async def go():
        async with running() as svc:
            host, _ = await host_conn(svc)
            c1, c2 = (await new_code(host))["code"], (await new_code(host))["code"]
            p1 = await page(svc, c1)
            j1 = await recv(host)
            await host.send(_msg(type="reject", peer=j1["peer"], reason="busy"))
            assert await recv(p1) == {"v": 1, "type": "error", "reason": "rejected"}
            p2 = await page(svc, c2)
            await recv(host)
            await host.close()
            assert (await recv(p2))["reason"] == "host-gone"
            for ws in (p1, p2):
                await ws.close()
    run(go())


def test_failed_joins_are_rate_limited_per_address_whatever_the_code():
    async def go():
        async with running(join_fails_per_ip=3) as svc:
            host, _ = await host_conn(svc)
            good = (await new_code(host))["code"]
            reasons = []
            for guess in ("AAAAAA", "BBBBBB", "CCCCCC", "DDDDDD"):
                p = await page(svc, guess)
                reasons.append((await recv(p))["reason"])
                await p.close()
            assert reasons == ["unknown-code"] * 3 + ["rate-limited"]
            p = await page(svc, good)            # a right guess now teaches nothing
            assert (await recv(p))["reason"] == "rate-limited"
            await p.close()
            svc.test_clock.t += svc.config.fail_window_s + 1
            p = await page(svc, (await new_code(host))["code"])
            assert (await recv(host))["type"] == "join"
            await p.close()
            await host.close()
    run(go())


def test_codes_per_host_and_joins_per_code_are_limited():
    async def go():
        async with running(codes_per_host_per_min=3, joins_per_code_per_min=2) as svc:
            host, _ = await host_conn(svc)
            got = [await new_code(host, kind="resume") for _ in range(4)]
            assert [g["type"] for g in got] == ["code"] * 3 + ["error"]
            assert got[3]["reason"] == "rate-limited"
            code = got[0]["code"]
            ps = []
            for _ in range(2):
                ps.append(await page(svc, code))
                await recv(host)
            ps.append(await page(svc, code))
            assert (await recv(ps[-1]))["reason"] == "rate-limited"
            for ws in (*ps, host):
                await ws.close()
    run(go())


def test_hosts_per_address_are_limited():
    async def go():
        async with running(host_conns_per_ip=1) as svc:
            h1, _ = await host_conn(svc)
            h2 = await connect(f"{svc.url}/v1/host", proxy=None)
            assert (await recv(h2))["reason"] == "rate-limited"
            with pytest.raises(ConnectionClosed):
                await recv(h2)
            await h1.close()
    run(go())


def test_origins_and_paths_are_checked_before_the_upgrade():
    async def go():
        async with running(allowed_origins=("https://vr.example.org",)) as svc:
            with pytest.raises(InvalidStatus) as e:
                await connect(f"{svc.url}/v1/join", origin="https://evil.example", proxy=None)
            assert e.value.response.status_code == 403
            with pytest.raises(InvalidStatus) as e:
                await connect(f"{svc.url}/v1/join", proxy=None)       # no Origin at all
            assert e.value.response.status_code == 403
            with pytest.raises(InvalidStatus) as e:
                await connect(f"{svc.url}/v1/host", origin="https://evil.example", proxy=None)
            assert e.value.response.status_code == 403
            with pytest.raises(InvalidStatus) as e:
                await connect(f"{svc.url}/v1/other", proxy=None)
            assert e.value.response.status_code == 404
            ok = await connect(f"{svc.url}/v1/join", origin="https://vr.example.org", proxy=None)
            await ok.close()
            reader, writer = await asyncio.open_connection("127.0.0.1", int(svc.url.rsplit(":", 1)[1]))
            writer.write(b"GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n")
            assert (await reader.readline()).startswith(b"HTTP/1.1 200")
            writer.close()
    run(go())


def test_a_page_that_never_joins_or_stays_too_long_is_closed():
    async def go():
        async with running(join_wait_s=0.2, join_session_s=0.3) as svc:
            idle = await connect(f"{svc.url}/v1/join", origin=LOCAL, proxy=None)
            await recv(idle)
            with pytest.raises(ConnectionClosed):
                await recv(idle, timeout=2)
            assert idle.close_code == 1008
            host, _ = await host_conn(svc)
            p = await page(svc, (await new_code(host))["code"])
            await recv(host)
            with pytest.raises(ConnectionClosed):
                await recv(p, timeout=2)
            bad = await connect(f"{svc.url}/v1/join", origin=LOCAL, proxy=None)
            await recv(bad)
            await bad.send(_msg(type="join", code="ABCDEF", offer="not an sdp"))
            with pytest.raises(ConnectionClosed):
                await recv(bad)
            assert bad.close_code == 1008
            await host.close()
    run(go())


def test_the_host_is_pinged():
    async def go():
        async with running(ping_interval=0.1, ping_timeout=0.2, close_timeout=0.2) as svc:
            # A client that never answers pings is closed by the service.
            ws = await connect(f"{svc.url}/v1/host", proxy=None, ping_interval=None)
            await recv(ws)
            ws.protocol.state   # noqa: B018 - the connection is open
            # Stop reading: pongs are sent by the reader, so they stop too.
            ws.transport.pause_reading()
            await asyncio.sleep(0.8)
            assert not svc.host_conns          # the service dropped the host
            ws.transport.abort()
    run(go())


def test_no_sdp_candidate_or_code_is_logged(caplog):
    async def go():
        async with running() as svc:
            host, _ = await host_conn(svc)
            code = (await new_code(host))["code"]
            p = await page(svc, code)
            join = await recv(host)
            await p.send(_msg(type="ice", candidate=CANDIDATE))
            await recv(host)
            await host.send(_msg(type="answer", peer=join["peer"], sdp="v=0 SECRET-ANSWER"))
            await recv(p)
            bad = await page(svc, "QQQQQQ")
            await recv(bad)
            for ws in (p, bad, host):
                await ws.close()
            return code
    with caplog.at_level(logging.DEBUG, logger="visivo.vr_pair"):
        code = run(go())
    # The service's own log (the websockets library's frame dumps are DEBUG,
    # and the service runs that library at WARNING).
    text = "\n".join(r.getMessage() for r in caplog.records if r.name.startswith("visivo"))
    assert "join" in text                      # something was logged
    for secret in ("SECRET-SDP-MARKER", "SECRET-ANSWER", "192.168.1.20", code, "QQQQQQ"):
        assert secret not in text


def test_a_page_cannot_flood_its_host_with_candidates():
    async def go():
        async with running(candidates_per_peer=5) as svc:
            host, _ = await host_conn(svc)
            p = await page(svc, (await new_code(host))["code"])
            await recv(host)
            for _ in range(8):
                await p.send(_msg(type="ice", candidate=CANDIDATE))
            got = 0
            with pytest.raises(TimeoutError):
                while True:
                    assert (await recv(host, timeout=0.5))["type"] == "ice"
                    got += 1
            assert got == 5
            await p.close()
            await host.close()
    run(go())


def test_reconnecting_starts_no_new_code_budget_and_tombstones_are_bounded():
    svc = S.PairService(S.Config(codes_per_host_per_min=100, codes_per_ip_per_min=3, max_codes=10,
                                 max_tombstones=2), clock=Clock())
    reasons = []
    for _ in range(4):
        h = S.HostConn(ws=None, ip="10.0.0.9")
        reasons.append(svc.issue(h, "pair", None)[1])
        svc.host_gone(h)
    assert reasons == [None, None, None, "rate-limited"]
    svc.issue(S.HostConn(ws=None, ip="10.0.0.1"), "pair", None)
    assert sum(1 for c in svc.codes.values() if c.state != "live") == 2    # the newest kept
    # Tombstones never keep a legitimate host from a code.
    svc = S.PairService(S.Config(max_codes=2), clock=Clock())
    for i in range(5):
        h = S.HostConn(ws=None, ip=f"10.0.1.{i}")
        assert svc.issue(h, "pair", None)[1] is None
        svc.host_gone(h)


def test_turn_credentials_are_bounded_per_address():
    svc = S.PairService(S.Config(turn_urls=("turn:t.test",), turn_secret="k", turn_per_ip=2,
                                 stun_urls=("stun:s.test",)), clock=Clock())
    assert [len(svc.ice_servers("1.2.3.4")) for _ in range(3)] == [2, 2, 1]   # then STUN only
    assert len(svc.ice_servers("5.6.7.8")) == 2
    for _ in range(1000):
        svc.ice_servers("1.2.3.4")
    assert svc.turn_handouts.count("1.2.3.4", svc.clock()) == 2      # refusals keep no history


def test_a_resume_code_stays_its_hosts_after_the_service_forgot_it():
    clock = Clock()
    svc = S.PairService(S.Config(secret="s3"), clock=clock)
    owner_host = S.HostConn(ws=None, ip="10.0.0.1")
    reply, _ = svc.issue(owner_host, "resume", None)
    svc.host_gone(owner_host)
    clock.t += svc.config.tombstone_s + 12 * 3600 + 10
    svc.sweep(force=True)
    assert reply["code"] not in svc.codes                            # forgotten
    thief = S.HostConn(ws=None, ip="10.0.0.66")
    assert svc.issue(thief, "resume", None, reply["code"])[1] == "code-taken"
    assert svc.issue(thief, "resume", None, reply["code"], "x" * 43)[1] == "code-taken"
    # A restarted service with the same secret takes it back from its owner.
    again = S.PairService(S.Config(secret="s3"), clock=Clock())
    back, reason = again.issue(S.HostConn(ws=None, ip="10.0.0.1"), "resume", None, reply["code"], reply["owner"])
    assert reason is None and back["owner"] == reply["owner"]
    # Without the secret, a restart cannot check the key: refused.
    other = S.PairService(S.Config(), clock=Clock())
    assert other.issue(S.HostConn(ws=None, ip="10.0.0.1"), "resume", None, reply["code"],
                       reply["owner"])[1] == "code-taken"


def test_hosts_may_be_required_to_present_a_key():
    async def go():
        async with running(host_keys=("k-1", "k-2")) as svc:
            for suffix in ("", "?key=wrong"):
                with pytest.raises(InvalidStatus) as e:
                    await connect(f"{svc.url}/v1/host{suffix}", proxy=None)
                assert e.value.response.status_code == 401
            ws = await connect(f"{svc.url}/v1/host?key=k-2", proxy=None)
            assert (await recv(ws))["type"] == "ice_servers"
            ws2 = await connect(f"{svc.url}/v1/host", proxy=None,
                                additional_headers={"Authorization": "Bearer k-1"})
            assert (await recv(ws2))["type"] == "ice_servers"
            for w in (ws, ws2):
                await w.close()
    run(go())
