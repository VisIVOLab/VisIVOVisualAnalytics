"""Tests for POST /v1/vlkb/fetch_cutout — remote SODA fetch + SSRF guard.

A throwaway HTTP server on 127.0.0.1 stands in for the remote SODA service.
Because 127.0.0.1 is a loopback address, the happy-path tests must explicitly
allowlist it via VISIVO_VLKB_ALLOWED_HOSTS — which is exactly the production
mechanism for trusting a real VLKB host. The SSRF tests assert that, without
that allowlist, loopback/private targets are refused.
"""
from __future__ import annotations

import os
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

_TEST_TOKEN = "test-token-visivo-12345"
_FITS_BODY = b"SIMPLE  =                    T" + b" " * 2850 + b"END" + b" " * 77


class _SodaHandler(BaseHTTPRequestHandler):
    received_auth: str = ""

    def do_GET(self):  # noqa: N802
        _SodaHandler.received_auth = self.headers.get("Authorization", "")
        self.send_response(200)
        self.send_header("Content-Type", "application/fits")
        self.end_headers()
        self.wfile.write(_FITS_BODY)

    def log_message(self, *args):  # silence test server
        pass


class TestVlkbFetch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": _TEST_TOKEN}
        cls.server = HTTPServer(("127.0.0.1", 0), _SodaHandler)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        os.environ.pop("VISIVO_VLKB_ALLOWED_HOSTS", None)

    def tearDown(self):
        os.environ.pop("VISIVO_VLKB_ALLOWED_HOSTS", None)

    def _post(self, **body):
        return self.client.post("/v1/vlkb/fetch_cutout", json=body, headers=self._auth)

    # ── SSRF guard ────────────────────────────────────────────────────────────

    def test_loopback_blocked_without_allowlist(self):
        r = self._post(soda_url=f"{self.base}/cutout.fits")
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertFalse(data["valid"])
        self.assertIn("SSRF", data["error"])

    def test_non_http_scheme_rejected(self):
        r = self._post(soda_url="ftp://example.org/x.fits")
        self.assertFalse(r.json()["valid"])
        self.assertIn("http", r.json()["error"].lower())

    def test_host_not_in_allowlist_rejected(self):
        os.environ["VISIVO_VLKB_ALLOWED_HOSTS"] = "vlkb.ia2.inaf.it"
        r = self._post(soda_url=f"{self.base}/cutout.fits")
        self.assertFalse(r.json()["valid"])
        self.assertIn("allowed_hosts", r.json()["error"].lower())

    # ── Happy path (host explicitly allowlisted) ──────────────────────────────

    def test_fetch_succeeds_when_allowlisted(self):
        os.environ["VISIVO_VLKB_ALLOWED_HOSTS"] = "127.0.0.1"
        r = self._post(soda_url=f"{self.base}/mycutout.fits")
        data = r.json()
        self.assertTrue(data["valid"], data.get("error"))
        self.assertTrue(data["path"])
        with open(data["path"], "rb") as fh:
            self.assertTrue(fh.read().startswith(b"SIMPLE"))

    def test_bearer_token_forwarded(self):
        os.environ["VISIVO_VLKB_ALLOWED_HOSTS"] = "127.0.0.1"
        _SodaHandler.received_auth = ""
        r = self._post(soda_url=f"{self.base}/withauth.fits", vlkb_bearer="secret-xyz")
        self.assertTrue(r.json()["valid"])
        self.assertEqual(_SodaHandler.received_auth, "Bearer secret-xyz")


class TestSsrfGuard(unittest.TestCase):
    """Unit tests for the shared assert_safe_fetch_url guard (no network)."""

    def setUp(self):
        from app.dependencies import assert_safe_fetch_url
        self.guard = assert_safe_fetch_url
        for var in ("VISIVO_T_ALLOW",):
            os.environ.pop(var, None)

    def tearDown(self):
        os.environ.pop("VISIVO_T_ALLOW", None)

    def test_non_http_scheme_rejected(self):
        with self.assertRaises(ValueError):
            self.guard("ftp://example.org/x", allowlist_env="VISIVO_T_ALLOW")

    def test_missing_host_rejected(self):
        with self.assertRaises(ValueError):
            self.guard("http:///nohost", allowlist_env="VISIVO_T_ALLOW")

    def test_allowlist_denies_unlisted_host(self):
        os.environ["VISIVO_T_ALLOW"] = "trusted.inaf.it"
        with self.assertRaises(ValueError):
            self.guard("https://evil.example/x", allowlist_env="VISIVO_T_ALLOW")

    def test_allowlist_permits_listed_host(self):
        os.environ["VISIVO_T_ALLOW"] = "trusted.inaf.it"
        # Listed host returns before any DNS resolution.
        self.guard("https://trusted.inaf.it/x", allowlist_env="VISIVO_T_ALLOW")

    def test_block_private_rejects_loopback(self):
        with self.assertRaises(ValueError):
            self.guard("http://127.0.0.1:8080/x", allowlist_env="VISIVO_T_ALLOW", block_private=True)

    def test_block_private_false_permits_loopback(self):
        # Opt-out mode (the open_url default): private targets are allowed.
        self.guard("http://127.0.0.1:8080/x", allowlist_env="VISIVO_T_ALLOW", block_private=False)


class TestOpenUrlGuard(unittest.TestCase):
    """open_url honours the SSRF guard when hardening is enabled."""

    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": _TEST_TOKEN}

    def tearDown(self):
        os.environ.pop("VISIVO_OPENURL_BLOCK_PRIVATE", None)
        os.environ.pop("VISIVO_OPENURL_ALLOWED_HOSTS", None)

    def test_loopback_rejected_when_block_private_enabled(self):
        os.environ["VISIVO_OPENURL_BLOCK_PRIVATE"] = "1"
        r = self.client.post("/v1/datasets/open_url",
                             json={"url": "http://127.0.0.1:9/x.fits"}, headers=self._auth)
        data = r.json()
        self.assertFalse(data["valid"])
        self.assertIn("SSRF", data["error"])

    def test_non_http_rejected_by_default(self):
        r = self.client.post("/v1/datasets/open_url",
                             json={"url": "ftp://example.org/x.fits"}, headers=self._auth)
        self.assertFalse(r.json()["valid"])
        self.assertIn("http", r.json()["error"].lower())


if __name__ == "__main__":
    unittest.main()
