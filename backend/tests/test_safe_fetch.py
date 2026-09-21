"""Tests for the one fetch helper the archive routes use.

Checking the URL the client sent is the easy half. The half that gets forgotten
is the redirect: `urlopen` follows them by default, so a perfectly public host
can answer `302 Location: http://169.254.169.254/` and walk the backend into the
private network on its behalf. This file exists so that stays fixed.
"""
from __future__ import annotations

import http.server
import threading
import urllib.error

import pytest

from app.safe_fetch import safe_urlopen


@pytest.fixture
def redirecting_server():
    """A local server that answers every request with a 302 somewhere else.

    It is itself on loopback, so the allowlist has to let it through for the
    test to reach the redirect at all — which is the point: the first URL is
    trusted, the target is not.
    """
    target = {"location": "http://127.0.0.1:1/private"}

    class _Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802 — the base class spells it this way
            self.send_response(302)
            self.send_header("Location", target["location"])
            self.end_headers()

        def log_message(self, *args):  # noqa: D102 — silence the test output
            return

    server = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield server, target
    server.shutdown()
    server.server_close()


def test_a_refused_url_never_opens_a_connection(monkeypatch):
    monkeypatch.delenv("VISIVO_TEST_ALLOWED_HOSTS", raising=False)
    with pytest.raises(ValueError, match="SSRF"):
        safe_urlopen("http://127.0.0.1:9/whatever", allowlist_env="VISIVO_TEST_ALLOWED_HOSTS")


def test_a_non_http_scheme_is_refused():
    with pytest.raises(ValueError, match="http"):
        safe_urlopen("file:///etc/passwd", allowlist_env="VISIVO_TEST_ALLOWED_HOSTS")


def test_a_redirect_into_the_private_network_is_refused(monkeypatch, redirecting_server):
    """The first hop is allowed; the second one is where the attack lives."""
    server, target = redirecting_server
    host, port = server.server_address
    # Trust the first host only. Without the redirect handler the request would
    # follow the Location header regardless of what it points at.
    monkeypatch.setenv("VISIVO_TEST_ALLOWED_HOSTS", f"{host}")
    target["location"] = "http://169.254.169.254/latest/meta-data/"
    with pytest.raises(ValueError, match="not in VISIVO_TEST_ALLOWED_HOSTS"):
        safe_urlopen(f"http://{host}:{port}/start",
                     allowlist_env="VISIVO_TEST_ALLOWED_HOSTS", timeout=5)


def test_a_redirect_to_an_allowed_host_is_followed(monkeypatch, redirecting_server):
    """The guard must not become "no redirects at all": services move."""
    server, target = redirecting_server
    host, port = server.server_address
    monkeypatch.setenv("VISIVO_TEST_ALLOWED_HOSTS", f"{host}")
    # Redirect back to the same (allowed) host, at a port nothing listens on:
    # the guard lets it through, so the failure is a connection error.
    target["location"] = f"http://{host}:1/next"
    with pytest.raises(urllib.error.URLError):
        safe_urlopen(f"http://{host}:{port}/start",
                     allowlist_env="VISIVO_TEST_ALLOWED_HOSTS", timeout=5)
