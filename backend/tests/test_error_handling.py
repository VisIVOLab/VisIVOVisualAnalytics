"""E-05: the global exception handler must not leak internals.

An exception that escapes a route handler should come back as a compact
``{error, error_code, request_id}`` 500 body — never the exception string,
filesystem paths, or a traceback (those go to the server log only).

Uses an ISOLATED FastAPI app wired with the same handler so the shared
``app.main.app`` (and its OpenAPI route snapshot, see test_contract.py) is never
mutated by the test route.
"""
from __future__ import annotations

_SECRET = "SENSITIVE_INTERNAL_PATH_/etc/passwd"


def _isolated_app():
    from fastapi import FastAPI
    from app.main import _unhandled_exception_handler

    app = FastAPI()
    app.add_exception_handler(Exception, _unhandled_exception_handler)

    @app.get("/boom")
    async def _boom():
        raise RuntimeError(_SECRET)

    return app


def test_unhandled_exception_is_sanitized():
    from fastapi.testclient import TestClient

    client = TestClient(_isolated_app(), raise_server_exceptions=False)
    r = client.get("/boom")
    assert r.status_code == 500
    body = r.json()
    assert body["error_code"] == "internal_error"
    assert body["error"] == "Internal server error."
    # The exception message / internal path must NOT appear in the response.
    assert _SECRET not in r.text
    assert "Traceback" not in r.text
    # A correlation id is returned (and mirrored in the header) for log lookup.
    assert body["request_id"]
    assert r.headers.get("X-Request-ID") == body["request_id"]


async def test_heavy_task_times_out(monkeypatch):
    """E-03: a heavy compute that exceeds the wall-clock ceiling raises HTTP 504
    instead of hanging the worker indefinitely."""
    import asyncio

    import app.dependencies as deps
    from fastapi import HTTPException

    async def _slow(*_a):
        await asyncio.sleep(0.3)
        return "never"

    monkeypatch.setattr(deps, "_run", _slow)
    monkeypatch.setattr(deps, "_HEAVY_TIMEOUT_S", 0.05)
    # Reset the lazily-bound semaphore so it attaches to this test's loop.
    monkeypatch.setattr(deps, "_HEAVY_SEM", None)

    try:
        await deps._run_heavy(lambda: None)
        assert False, "expected a timeout"
    except HTTPException as exc:
        assert exc.status_code == 504
        assert "time limit" in exc.detail

    # The shielded background task must still settle and release its slot
    # (accurate admission control) — wait for it, then the semaphore is free.
    await asyncio.sleep(0.4)
    sem = deps._heavy_semaphore()
    assert sem._value == deps._HEAVY_SLOTS  # slot released after real completion
