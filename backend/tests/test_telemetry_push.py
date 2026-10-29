"""The node's own telemetry reports to its SKAVA catalogue."""
from __future__ import annotations

import asyncio
import json
import logging
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest

from app import telemetry_push
from app.telemetry_push import PushConfig, PushError, config_from_env, post_report, run

ENV = {"VISIVO_SKAVA_TELEMETRY_URL": "http://skava.test:8000/", "VISIVO_SKAVA_NODE_TOKEN": "0123456789abcdef0123",
       "VISIVO_NODE_CODE": "BIG"}


def test_off_unless_fully_configured(caplog):
    assert config_from_env({}) is None
    with caplog.at_level(logging.WARNING, logger="visivo.telemetry_push"):
        assert config_from_env({**ENV, "VISIVO_NODE_CODE": ""}) is None
        assert config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_URL": "ftp://x"}) is None
        assert config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_URL": "http://u:p@x"}) is None
        assert config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_URL": "http://:p@x:8000"}) is None
        assert config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_URL": "http://@x"}) is None
        assert config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_URL": "http://x/a\r\nX: y"}) is None
        assert config_from_env({**ENV, "VISIVO_SKAVA_NODE_TOKEN": "secret-part\ninjected"}) is None
    assert len(caplog.records) == 7
    assert "secret-part" not in caplog.text


def test_config_reads_the_url_and_bounds_the_interval():
    cfg = config_from_env(ENV)
    assert cfg.report_url == "http://skava.test:8000/internal/telemetry/report"
    assert cfg.interval == 10.0 and cfg.node_code == "BIG" and cfg.key == ENV["VISIVO_SKAVA_NODE_TOKEN"]
    assert config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_INTERVAL": "0.01"}).interval == 1.0
    assert config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_INTERVAL": "nan"}).interval == 10.0
    assert config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_INTERVAL": "soon"}).interval == 10.0


def test_the_loop_sends_the_previous_round_trip_and_survives_failures():
    cfg = PushConfig("http://x/internal/telemetry/report", "k", "BIG", interval=0.0)
    seen: list[float | None] = []
    outcomes = iter([12.0, RuntimeError("down"), 7.0, 5.0])

    async def post(_cfg, status, rtt_ms):
        assert status == {"cpu": 1}
        seen.append(rtt_ms)
        result = next(outcomes)
        if isinstance(result, Exception):
            raise result
        return result

    async def collect():
        return {"cpu": 1}

    asyncio.run(run(cfg, collect, post=post, max_reports=4))
    # First report has no round trip; after a failure the stale one is not reused.
    assert seen == [None, 12.0, None, 7.0]


class _Recorder(BaseHTTPRequestHandler):
    """A catalogue (or, with ``redirect_to`` set, a catalogue that redirects)."""
    received: list[tuple[dict, dict]]
    redirect_to: str | None = None

    def do_POST(self):  # noqa: N802
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.received.append((dict(self.headers), body))
        if self.redirect_to:
            self.send_response(307)
            self.send_header("Location", self.redirect_to)
            self.end_headers()
            return
        payload = json.dumps({"node": body["node_code"], "accepted": True}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


def _server(redirect_to=None):
    handler = type("Handler", (_Recorder,), {"received": [], "redirect_to": redirect_to})
    server = HTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, handler


@pytest.fixture
def servers():
    started = []

    def make(redirect_to=None):
        server, handler = _server(redirect_to)
        started.append(server)
        return f"http://127.0.0.1:{server.server_port}", handler

    yield make
    for server in started:
        server.shutdown()
        server.server_close()


@pytest.fixture
def catalogue(servers):
    url, handler = servers()
    global _Catalogue
    _Catalogue = handler
    return url


def test_post_report_speaks_the_catalogues_endpoint(catalogue):
    cfg = config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_URL": catalogue})
    rtt = asyncio.run(post_report(cfg, {"cpu": {"pressure_pct": 3.0}}, 4.25))
    assert rtt > 0
    headers, body = _Catalogue.received[0]
    assert headers["X-Skava-Node-Token"] == ENV["VISIVO_SKAVA_NODE_TOKEN"]
    assert body == {"node_code": "BIG", "status": {"cpu": {"pressure_pct": 3.0}}, "rtt_ms": 4.25}


def test_a_redirect_is_not_followed_with_the_key(servers):
    elsewhere, thief = servers()
    redirecting, catalogue = servers(redirect_to=elsewhere + "/steal")
    cfg = config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_URL": redirecting})
    with pytest.raises(PushError, match="HTTP 307"):
        asyncio.run(post_report(cfg, {}, None))
    assert len(catalogue.received) == 1 and thief.received == []


def test_an_interim_answer_is_not_the_verdict():
    import socket

    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    sock.listen()

    def answer():
        conn, _ = sock.accept()
        conn.recv(65536)
        conn.sendall(b"HTTP/1.1 103 Early Hints\r\nLink: </x>; rel=preload\r\n\r\n"
                     b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}")
        conn.close()

    threading.Thread(target=answer, daemon=True).start()
    cfg = config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_URL": f"http://127.0.0.1:{sock.getsockname()[1]}"})
    try:
        assert asyncio.run(post_report(cfg, {}, None)) > 0
    finally:
        sock.close()


@pytest.fixture
def silent_catalogue():
    """Accepts the connection, then never answers."""
    import socket
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    sock.listen()
    yield f"http://127.0.0.1:{sock.getsockname()[1]}"
    sock.close()


def test_one_deadline_covers_the_exchange(silent_catalogue, monkeypatch):
    import time
    monkeypatch.setattr(telemetry_push, "TIMEOUT", 0.3)
    cfg = config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_URL": silent_catalogue})
    started = time.perf_counter()
    with pytest.raises(PushError, match="no answer"):
        asyncio.run(post_report(cfg, {}, None))
    assert time.perf_counter() - started < 2.0


def test_cancelling_the_loop_ends_a_report_on_the_wire(silent_catalogue):
    import time
    cfg = config_from_env({**ENV, "VISIVO_SKAVA_TELEMETRY_URL": silent_catalogue})

    async def collect():
        return {}

    async def scenario():
        task = asyncio.create_task(run(cfg, collect))
        await asyncio.sleep(0.2)          # the report is waiting for an answer
        started = time.perf_counter()
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        return time.perf_counter() - started

    assert asyncio.run(scenario()) < 0.5


def test_start_is_a_no_op_without_configuration(monkeypatch):
    for name in ENV:
        monkeypatch.delenv(name, raising=False)
    assert telemetry_push.start("1.0") is None


def test_the_pushed_status_is_the_node_status_document(monkeypatch):
    from app.routers.node import collect_status
    monkeypatch.setenv("VISIVO_NODE_CODE", "BIG")
    status = asyncio.run(collect_status("9.9"))
    assert status["node_code"] == "BIG" and "cpu" in status and "memory" in status
