"""The self-hosted copilot's configuration from the environment (#75).

Two surprises this pins down:

* with ``VISIVO_LLM_BASE_URL`` set and no ``VISIVO_ASSISTANT_MODEL``, the
  provider used to ask the server for ``gpt-4o``. It now asks the server what
  it serves, uses the only model when there is one, and otherwise says which
  to choose from;
* the environment's base URL used to skip the checks a URL from the client
  gets. It now gets the same ones, and a refused value turns the copilot off,
  with the reason, without taking the backend down.

The model list is served by a real local HTTP server, so the OpenAI SDK's own
request path is what is exercised.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

from app.assistant import provider as P
from app.assistant.provider import NullProvider, OpenAIProvider

pytest.importorskip("openai")

_TEST_TOKEN = "test-token-visivo-12345"
_BACKEND = Path(__file__).resolve().parents[1]


# ── a stand-in OpenAI-compatible server ──────────────────────────────────────

class _Server:
    """Serves ``/v1/models`` with the given ids and records chat requests."""

    def __init__(self, models: list[str]) -> None:
        self.models = models
        self.chat_models: list[str] = []
        self.model_calls = 0
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_a):  # quiet
                pass

            def _send(self, payload: dict) -> None:
                body = json.dumps(payload).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):  # noqa: N802
                if self.path.rstrip("/") == "/v1/models":
                    outer.model_calls += 1
                    self._send({"object": "list", "data": [
                        {"id": m, "object": "model", "created": 0, "owned_by": "me"}
                        for m in outer.models]})
                else:
                    self.send_error(404)

            def do_POST(self):  # noqa: N802
                length = int(self.headers.get("Content-Length", "0"))
                req = json.loads(self.rfile.read(length) or b"{}")
                outer.chat_models.append(req.get("model"))
                self._send({
                    "id": "c1", "object": "chat.completion", "created": 0,
                    "model": req.get("model"),
                    "choices": [{"index": 0, "finish_reason": "stop",
                                 "message": {"role": "assistant", "content": "hello"}}]})

        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base_url = f"http://127.0.0.1:{self._httpd.server_address[1]}/v1"
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *_exc):
        self._httpd.shutdown()
        self._httpd.server_close()


def _closed_port_url() -> str:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    return f"http://127.0.0.1:{port}/v1"


def _ask(provider) -> str:
    return provider.complete("SYS", [{"role": "user", "text": "hi"}], []).final_text


# ── model: never a silent gpt-4o ─────────────────────────────────────────────

def test_a_self_hosted_endpoint_is_not_sent_gpt_4o(monkeypatch):
    monkeypatch.delenv("VISIVO_ASSISTANT_MODEL", raising=False)
    with _Server(["qwen2.5-7b-instruct"]) as server:
        provider = OpenAIProvider(None, server.base_url, None, name="self-hosted")
        assert _ask(provider) == "hello"
        assert server.chat_models == ["qwen2.5-7b-instruct"]
        assert provider.problem == ""
        # Resolved once, then remembered.
        assert _ask(provider) == "hello"
        assert server.model_calls == 1


def test_several_models_are_listed_and_none_is_picked(monkeypatch):
    monkeypatch.delenv("VISIVO_ASSISTANT_MODEL", raising=False)
    with _Server(["b-model", "a-model"]) as server:
        provider = OpenAIProvider(None, server.base_url, None, name="self-hosted")
        answer = _ask(provider)
        assert server.chat_models == []          # nothing was sent to a model
    assert "serves 2: a-model, b-model" in answer
    assert "VISIVO_ASSISTANT_MODEL" in answer
    assert provider.problem in answer


def test_a_long_model_list_is_cut(monkeypatch):
    monkeypatch.delenv("VISIVO_ASSISTANT_MODEL", raising=False)
    with _Server([f"m{i:02d}" for i in range(13)]) as server:
        provider = OpenAIProvider(None, server.base_url, None, name="self-hosted")
        answer = _ask(provider)
    assert "serves 13:" in answer and "m09" in answer and "m10" not in answer
    assert "and 3 more" in answer


def test_an_empty_model_list_is_reported(monkeypatch):
    monkeypatch.delenv("VISIVO_ASSISTANT_MODEL", raising=False)
    with _Server([]) as server:
        provider = OpenAIProvider(None, server.base_url, None, name="self-hosted")
        answer = _ask(provider)
    assert "lists none" in answer and "VISIVO_ASSISTANT_MODEL" in answer


def test_a_server_that_is_down_is_reported_not_raised(monkeypatch):
    monkeypatch.delenv("VISIVO_ASSISTANT_MODEL", raising=False)
    provider = OpenAIProvider(None, _closed_port_url(), None, name="self-hosted")
    answer = _ask(provider)
    assert "could not be read" in answer and "VISIVO_ASSISTANT_MODEL" in answer


def test_a_named_model_is_used_without_asking(monkeypatch):
    monkeypatch.setenv("VISIVO_ASSISTANT_MODEL", "my-served-name")
    with _Server(["something-else"]) as server:
        provider = OpenAIProvider(None, server.base_url, None, name="self-hosted")
        assert _ask(provider) == "hello"
        assert server.chat_models == ["my-served-name"]
        assert server.model_calls == 0


def test_building_the_provider_does_not_touch_the_network(monkeypatch):
    # Building happens at the status route; a dead server must not stall it.
    monkeypatch.delenv("VISIVO_ASSISTANT_MODEL", raising=False)
    cfg = P.default_config()
    P.configure(cfg, provider="auto", base_url=_closed_port_url(), base_url_key="",
                model="", openai_key="", anthropic_key="")
    provider = P.provider_from_env(cfg)
    assert isinstance(provider, OpenAIProvider)
    assert provider.configured and provider.problem == ""


def test_concurrent_questions_discover_the_model_once(monkeypatch):
    """Two questions on the session's cached provider: one listing, one answer.

    Without the lock a slow failing lookup could finish after a fast success
    and leave a stale `problem` behind a model that works.
    """
    from types import SimpleNamespace

    monkeypatch.delenv("VISIVO_ASSISTANT_MODEL", raising=False)
    provider = OpenAIProvider(None, _closed_port_url(), None, name="self-hosted")
    entered, release = threading.Event(), threading.Event()
    calls: list[str] = []

    class _Client:
        def with_options(self, **_kw):
            return self

        @property
        def models(self):
            return self

        def list(self):
            calls.append(threading.current_thread().name)
            entered.set()
            release.wait(5)
            return [SimpleNamespace(id="the-model")]

    class _WatchedLock:
        """The provider's lock, saying when the second question reaches it."""

        def __init__(self, inner):
            self._inner = inner
            self.second_waiting = threading.Event()

        def __enter__(self):
            if threading.current_thread().name == "second":
                self.second_waiting.set()
            return self._inner.__enter__()

        def __exit__(self, *exc):
            return self._inner.__exit__(*exc)

    provider._client = _Client()
    lock = _WatchedLock(provider._model_lock)
    provider._model_lock = lock
    results: dict[str, tuple] = {}
    first = threading.Thread(name="first",
                             target=lambda: results.__setitem__("first", provider._resolve_model()))
    second = threading.Thread(name="second",
                              target=lambda: results.__setitem__("second", provider._resolve_model()))
    first.start()
    assert entered.wait(5)
    second.start()
    # The second question is at the lock while the first is still listing:
    # the contention the lock exists for, not a question that came later.
    assert lock.second_waiting.wait(5)
    release.set()
    first.join(5)
    second.join(5)
    assert calls == ["first"]
    assert results == {"first": ("the-model", ""), "second": ("the-model", "")}
    assert provider.problem == ""


def test_cloud_openai_keeps_its_default(monkeypatch):
    monkeypatch.delenv("VISIVO_ASSISTANT_MODEL", raising=False)
    assert OpenAIProvider("sk-test", None, None)._model == "gpt-4o"


# ── base URL from the environment: the client's checks ───────────────────────

@pytest.mark.parametrize("bad, words", [
    ("http://user:s3cr3t@gpu:8000/v1", "embedded credentials"),
    ("http://gpu:8000/v1?token=s3cr3t", "query string"),
    ("http://gpu:8000/v1#s3cr3t", "fragment"),
    ("ftp://gpu/v1", "http(s) URL"),
    ("gpu:8000/v1", "http(s) URL"),
    # The parser itself refuses this one, quoting the netloc: never repeated.
    ("https://u:s3cr3t@gpu／example.org/v1", "not a valid URL"),
])
def test_a_bad_environment_url_turns_the_copilot_off(monkeypatch, bad, words):
    monkeypatch.setenv("VISIVO_LLM_BASE_URL", bad)
    monkeypatch.setenv("OPENAI_API_KEY", "sk-cloud")      # must NOT be fallen back to
    monkeypatch.setenv("VISIVO_ASSISTANT_PROVIDER", "auto")
    cfg = P.default_config()
    assert cfg["base_url"] is None
    assert words in cfg["base_url_error"]
    assert "VISIVO_LLM_BASE_URL" in cfg["base_url_error"]
    assert "s3cr3t" not in cfg["base_url_error"]
    provider = P.provider_from_env(cfg)
    assert isinstance(provider, NullProvider)
    assert provider.problem == cfg["base_url_error"]
    answer = _ask(provider)
    assert words in answer and "s3cr3t" not in answer
    assert "s3cr3t" not in str(P.public_config(cfg))


def test_an_explicit_openai_choice_does_not_fall_back_to_the_cloud(monkeypatch):
    monkeypatch.setenv("VISIVO_LLM_BASE_URL", "http://gpu:8000/v1?x=1")
    monkeypatch.setenv("OPENAI_API_KEY", "sk-cloud")
    monkeypatch.setenv("VISIVO_ASSISTANT_PROVIDER", "openai")
    assert isinstance(P.provider_from_env(P.default_config()), NullProvider)


def test_an_explicit_anthropic_choice_still_works(monkeypatch):
    # The refusal is about the self-hosted endpoint, not about every provider.
    monkeypatch.setenv("VISIVO_LLM_BASE_URL", "http://gpu:8000/v1?x=1")
    monkeypatch.setenv("VISIVO_ASSISTANT_PROVIDER", "anthropic")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-ant-test")
    cfg = P.default_config()
    assert P._resolve_provider_name(cfg) == "anthropic"
    # Built only where the SDK is installed; without it, the usual null.
    try:
        import anthropic  # noqa: F401, PLC0415
    except ImportError:
        return
    assert P.provider_from_env(cfg).name == "anthropic"


def test_a_good_environment_url_is_normalised(monkeypatch):
    monkeypatch.setenv("VISIVO_LLM_BASE_URL", "  http://gpu:8000/v1/  ")
    cfg = P.default_config()
    assert cfg["base_url"] == "http://gpu:8000/v1"
    assert cfg["base_url_error"] is None


def test_the_session_can_replace_a_refused_environment_url(monkeypatch):
    monkeypatch.setenv("VISIVO_LLM_BASE_URL", "http://u:p@gpu:8000/v1")
    cfg = P.default_config()
    P.configure(cfg, base_url="http://gpu:8000/v1")
    assert cfg["base_url_error"] is None
    assert cfg["base_url"] == "http://gpu:8000/v1"


@pytest.mark.parametrize("bad, words", [
    ("http://u:s3cr3t@gpu:8000/v1", "embedded credentials"),
    ("https://u:s3cr3t@gpu／example.org/v1", "not a valid URL"),
])
def test_the_status_route_says_why(monkeypatch, bad, words):
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient

    from app.main import app

    monkeypatch.setenv("VISIVO_LLM_BASE_URL", bad)
    monkeypatch.setenv("VISIVO_ASSISTANT_PROVIDER", "auto")
    client = TestClient(app)
    auth = {"X-Visivo-Token": _TEST_TOKEN}
    sid = client.post("/v1/sessions/new", headers=auth).json()["session_id"]
    r = client.get("/v1/assistant/status", headers={**auth, "X-Visivo-Session": sid})
    assert r.status_code == 200
    body = r.json()
    assert body["configured"] is False
    assert words in body["problem"]
    assert "s3cr3t" not in r.text


@pytest.mark.parametrize("bad, words", [
    ("https://u:s3cr3t@gpu.example.org/v1", "must not contain embedded credentials"),
    ("https://u:s3cr3t@gpu／example.org/v1", "is not a valid URL"),
])
def test_the_backend_starts_with_a_bad_url_and_does_not_log_it(bad, words):
    """'Refused at start-up' means refused, not fatal: the copilot is optional."""
    env = {**os.environ, "VISIVO_TOKEN": "x", "VISIVO_LLM_BASE_URL": bad}
    proc = subprocess.run(
        [sys.executable, "-c", "import logging; logging.basicConfig(); import app.main"],
        cwd=_BACKEND, env=env, capture_output=True, text=True, timeout=120)
    assert proc.returncode == 0, proc.stderr[-2000:]
    out = proc.stdout + proc.stderr
    assert f"VISIVO_LLM_BASE_URL {words}" in out
    assert "s3cr3t" not in out


def test_reset_goes_back_to_the_environment(monkeypatch):
    """The desktop's "use the server's configuration": a reset undoes what the
    session chose and brings back the operator's VISIVO_LLM_BASE_URL, refused
    or not, with its problem."""
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient

    from app.main import app

    monkeypatch.setenv("VISIVO_LLM_BASE_URL", "http://u:s3cr3t@gpu:8000/v1")
    monkeypatch.setenv("VISIVO_ASSISTANT_PROVIDER", "auto")
    client = TestClient(app)
    auth = {"X-Visivo-Token": _TEST_TOKEN}
    sid = client.post("/v1/sessions/new", headers=auth).json()["session_id"]
    h = {**auth, "X-Visivo-Session": sid}

    # An explicit choice overrides the environment, and clears its problem.
    r = client.post("/v1/assistant/config", json={"provider": "null", "base_url": ""},
                    headers=h)
    assert r.status_code == 200 and r.json()["problem"] == ""

    # The reset brings the environment back, problem and all.
    r = client.post("/v1/assistant/config", json={"reset": True}, headers=h)
    assert r.status_code == 200
    body = r.json()
    assert body["configured"] is False
    assert "embedded credentials" in body["problem"]
    assert body["config"]["provider"] == "auto"
    assert "s3cr3t" not in r.text
    assert "embedded credentials" in client.get(
        "/v1/assistant/status", headers=h).json()["problem"]

    # A refused field in the same call keeps the session as it was.
    client.post("/v1/assistant/config", json={"provider": "null", "base_url": ""}, headers=h)
    r = client.post("/v1/assistant/config",
                    json={"reset": True, "base_url": "http://u:p@gpu/v1"}, headers=h)
    assert r.status_code == 422
    status = client.get("/v1/assistant/status", headers=h).json()
    assert status["config"]["provider"] == "null" and status["problem"] == ""

    # Without reset, an empty body still keeps everything (the older contract).
    r = client.post("/v1/assistant/config", json={}, headers=h)
    assert r.status_code == 200 and r.json()["config"]["provider"] == "null"


def test_the_status_route_names_the_model_the_backend_picked(monkeypatch):
    """config.model is what was configured; ``model`` is what questions go to,
    including the server's only model once the first question picked it."""
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient

    from app.main import app

    monkeypatch.delenv("VISIVO_ASSISTANT_MODEL", raising=False)
    client = TestClient(app)
    auth = {"X-Visivo-Token": _TEST_TOKEN}
    sid = client.post("/v1/sessions/new", headers=auth).json()["session_id"]
    h = {**auth, "X-Visivo-Session": sid}
    with _Server(["qwen2.5-7b-instruct"]) as server:
        r = client.post("/v1/assistant/config",
                        json={"provider": "openai", "base_url": server.base_url,
                              "model": ""}, headers=h)
        assert r.status_code == 200 and r.json()["model"] == ""
        assert client.post("/v1/assistant/query", json={"question": "hi"},
                           headers=h).status_code == 200
        status = client.get("/v1/assistant/status", headers=h).json()
        assert status["model"] == "qwen2.5-7b-instruct"
        assert status["config"]["model"] is None
    # A named model is reported as soon as it is configured.
    r = client.post("/v1/assistant/config", json={"model": "named"}, headers=h)
    assert r.json()["model"] == "named"
