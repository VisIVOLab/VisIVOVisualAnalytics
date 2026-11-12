"""The VR page's lifecycle routes (epic #166): versioned page addresses,
/v1/vr/info (build, API, instance, token check) and the capped, rate-limited,
scrubbed /v1/vr/log."""
from __future__ import annotations

import json
import logging
import re
from pathlib import Path

import pytest
from httpx import AsyncClient

from app.routers import vr as vr_routes


def _build_of(html: str) -> str:
    m = re.search(r'src="b/([0-9a-f]{12})/js/main\.js"', html)
    assert m, html[:400]
    return m.group(1)


@pytest.fixture(autouse=True)
def _fresh_rate_limit(monkeypatch):
    monkeypatch.setattr(vr_routes, "RATE", vr_routes._RateLimiter())
    monkeypatch.setattr(vr_routes, "TOKEN_CHECKS", vr_routes._TokenCheckLimiter())


# ── Versioned page ──────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_index_names_the_current_build_and_is_revalidated(client: AsyncClient) -> None:
    r = await client.get("/vr/")
    assert r.status_code == 200
    assert r.headers["cache-control"] == "no-cache"
    build = _build_of(r.text)
    assert f'href="b/{build}/style.css"' in r.text
    assert f'content="{build}"' in r.text
    info = (await client.get("/v1/vr/info")).json()
    assert info["build"] == build
    assert (await client.get("/vr/index.html")).text == r.text


@pytest.mark.asyncio
async def test_current_build_assets_are_immutable_old_ones_revalidated(client: AsyncClient) -> None:
    build = _build_of((await client.get("/vr/")).text)
    cur = await client.get(f"/vr/b/{build}/js/main.js")
    assert cur.status_code == 200
    assert "javascript" in cur.headers["content-type"]
    assert "immutable" in cur.headers["cache-control"]
    old = await client.get("/vr/b/000000000000/js/main.js")
    assert old.status_code == 200
    assert old.headers["cache-control"] == "no-cache"
    assert old.content == cur.content
    # The unversioned addresses an older index page uses keep working.
    plain = await client.get("/vr/js/main.js")
    assert plain.status_code == 200 and plain.headers["cache-control"] == "no-cache"


@pytest.mark.asyncio
async def test_versioned_route_refuses_traversal_and_missing_files(client: AsyncClient) -> None:
    build = _build_of((await client.get("/vr/")).text)
    for bad in ("../../main.py", "js/../../../routers/vr.py", "js/missing.js", "index.html", "%2e%2e/%2e%2e/main.py"):
        r = await client.get(f"/vr/b/{build}/{bad}")
        assert r.status_code == 404, bad


def test_build_changes_when_a_page_file_changes(tmp_path) -> None:
    (tmp_path / "js").mkdir()
    (tmp_path / "index.html").write_text("<html>__VR_BUILD__</html>")
    f = tmp_path / "js" / "main.js"
    f.write_text("export const a = 1;\n")
    b1 = vr_routes.page_build(tmp_path)
    assert re.fullmatch(r"[0-9a-f]{12}", b1)
    assert vr_routes.page_build(tmp_path) == b1
    f.write_text("export const a = 22;\n")          # another size: a new signature
    b2 = vr_routes.page_build(tmp_path)
    assert b2 != b1


# ── /v1/vr/info ─────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_info_is_public_and_checks_a_token_when_given(client: AsyncClient, auth_headers) -> None:
    r = await client.get("/v1/vr/info")
    assert r.status_code == 200
    assert r.headers["cache-control"] == "no-store"
    body = r.json()
    assert body["vr_api"] >= body["vr_api_min"] >= 1
    assert body["token_ok"] is None
    assert re.fullmatch(r"[0-9a-f]{12}", body["instance"])
    assert "backend_version" not in body          # only for an accepted token
    assert (await client.get("/v1/vr/info", headers=auth_headers)).json()["token_ok"] is True
    assert (await client.get("/v1/vr/info", headers={"X-Visivo-Token": "wrong"})).json()["token_ok"] is False


def test_page_and_backend_agree_on_the_api() -> None:
    """The page's PAGE_VR_API (lifecycle.js) is within what the backend serves."""
    js = (vr_routes.STATIC_DIR / "js" / "lifecycle.js").read_text(encoding="utf-8")
    m = re.search(r"export const PAGE_VR_API = (\d+);", js)
    assert m
    assert vr_routes.VR_API_MIN <= int(m.group(1)) <= vr_routes.VR_API


# ── /v1/vr/log ──────────────────────────────────────────────────────────────

def _events(n: int, message: str = "x") -> dict:
    return {"build": "abc", "user_agent": "test", "events": [{"level": "error", "event": "page-error", "message": message} for _ in range(n)]}


@pytest.mark.asyncio
async def test_log_needs_the_token(client: AsyncClient) -> None:
    r = await client.post("/v1/vr/log", json=_events(1))
    assert r.status_code == 401


@pytest.mark.asyncio
async def test_log_writes_scrubbed_lines(client: AsyncClient, auth_headers, caplog) -> None:
    token = auth_headers["X-Visivo-Token"]
    msg = (f"fetch failed for /Users/alice/secret/proj/cube.fits with token={token} "
           f"ws://localhost:8000/v1/render/s/ws?token={token}&x=1 and {token}\nsecond line "
           r"C:\Users\bob\data\m33.fits")
    with caplog.at_level(logging.INFO, logger="visivo.vr"):
        r = await client.post("/v1/vr/log", json=_events(1, msg), headers=auth_headers)
    assert r.status_code == 200 and r.json() == {"accepted": 1}
    text = "\n".join(rec.getMessage() for rec in caplog.records if rec.name == "visivo.vr")
    assert token not in text
    assert "alice" not in text and "secret" not in text and "bob" not in text
    assert "cube.fits" in text and "m33.fits" in text
    assert "page-error" in text
    # One event, one line: the newline was flattened.
    assert len([rec for rec in caplog.records if rec.name == "visivo.vr" and "page-error" in rec.getMessage()]) == 1
    assert "\n" not in text.split("page-error", 1)[1]


@pytest.mark.asyncio
async def test_log_caps_size_events_and_fields(client: AsyncClient, auth_headers) -> None:
    big = json.dumps(_events(1, "y" * 3000)).encode() + b" " * (vr_routes.MAX_BODY)
    r = await client.post("/v1/vr/log", content=big, headers={**auth_headers, "Content-Type": "application/json"})
    assert r.status_code == 413
    r = await client.post("/v1/vr/log", json=_events(vr_routes.MAX_EVENTS + 1), headers=auth_headers)
    assert r.status_code == 422
    bad = {"events": [{"level": "fatal", "event": "x"}]}
    assert (await client.post("/v1/vr/log", json=bad, headers=auth_headers)).status_code == 422
    bad = {"events": [{"level": "info", "event": "has spaces"}]}
    assert (await client.post("/v1/vr/log", json=bad, headers=auth_headers)).status_code == 422
    assert (await client.post("/v1/vr/log", content=b"not json", headers=auth_headers)).status_code == 422


@pytest.mark.asyncio
async def test_log_is_rate_limited_per_token(client: AsyncClient, auth_headers) -> None:
    sent = 0
    for _ in range(vr_routes.RATE_EVENTS // 20):
        r = await client.post("/v1/vr/log", json=_events(20), headers=auth_headers)
        assert r.status_code == 200
        sent += 20
    r = await client.post("/v1/vr/log", json=_events(1), headers=auth_headers)
    assert r.status_code == 429
    assert int(r.headers["retry-after"]) >= 1


def test_rate_limiter_window_slides() -> None:
    rl = vr_routes._RateLimiter()
    t = 1000.0
    assert rl.allow("k", vr_routes.RATE_EVENTS, now=t)[0]
    ok, retry = rl.allow("k", 1, now=t + 1)
    assert not ok and 50 <= retry <= 60
    assert rl.allow("k", 1, now=t + vr_routes._WINDOW_S + 0.1)[0]
    assert rl.allow("other", 1, now=t + 1)[0]


def test_scrub_keeps_hosts_and_file_names() -> None:
    s = vr_routes.scrub("GET http://127.0.0.1:8000/v1/vr/info failed; open ~/data/a/b.fits session=abcdef123", ("abcdef123",))
    assert "127.0.0.1:8000" in s
    assert "…/b.fits" in s and "data" not in s
    assert "abcdef123" not in s
    assert vr_routes.scrub("POST /v1/cube/subvolume_bin: 404") == "POST /v1/cube/subvolume_bin: 404"


# ── Scrubbing: the same cases as the page (scrub_cases.json) ────────────────

_SCRUB_CASES = json.loads((Path(__file__).resolve().parent / "vr" / "scrub_cases.json").read_text(encoding="utf-8"))["cases"]


@pytest.mark.parametrize("case", _SCRUB_CASES, ids=[c["name"] for c in _SCRUB_CASES])
def test_scrub_case(case) -> None:
    out = vr_routes.scrub(case["input"], tuple(case.get("secrets", ())))
    for a in case.get("absent", []):
        assert a not in out, (a, out)
    for p in case.get("present", []):
        assert p in out, (p, out)
    if "expected" in case:
        assert out == case["expected"]


# ── Token checks on /v1/vr/info ─────────────────────────────────────────────

@pytest.mark.asyncio
async def test_info_token_checks_are_rate_limited_per_client(client: AsyncClient, auth_headers, monkeypatch) -> None:
    monkeypatch.setattr(vr_routes, "TOKEN_CHECKS", vr_routes._TokenCheckLimiter(per_minute=3))
    for _ in range(3):
        assert (await client.get("/v1/vr/info", headers={"X-Visivo-Token": "wrong"})).json()["token_ok"] is False
    limited = (await client.get("/v1/vr/info", headers=auth_headers)).json()
    assert limited["token_ok"] is None and limited["token_check"] == "rate-limited"
    assert "backend_version" not in limited


@pytest.mark.asyncio
async def test_info_gives_the_version_only_to_an_accepted_token(client: AsyncClient, auth_headers, monkeypatch) -> None:
    monkeypatch.setattr(vr_routes, "TOKEN_CHECKS", vr_routes._TokenCheckLimiter())
    assert "backend_version" not in (await client.get("/v1/vr/info")).json()
    assert "backend_version" not in (await client.get("/v1/vr/info", headers={"X-Visivo-Token": "nope"})).json()
    assert (await client.get("/v1/vr/info", headers=auth_headers)).json()["backend_version"]


def test_token_check_limiter_is_bounded() -> None:
    lim = vr_routes._TokenCheckLimiter(per_minute=1, max_clients=4)
    for i in range(10):
        assert lim.allow(f"10.0.0.{i}", now=0.0)
    assert len(lim._seen) == 4
    assert not lim.allow("10.0.0.9", now=1.0)
    assert lim.allow("10.0.0.9", now=61.0)
    assert [lim.miss("a", now=t) for t in (0.0, 1.0, 70.0)] == [1, 2, 1]


@pytest.mark.asyncio
async def test_a_non_ascii_token_is_refused_not_a_500(client: AsyncClient, monkeypatch) -> None:
    monkeypatch.setattr(vr_routes, "TOKEN_CHECKS", vr_routes._TokenCheckLimiter())
    bad = {"X-Visivo-Token": "t\xe9st-token".encode("latin-1")}
    r = await client.get("/v1/vr/info", headers=bad)
    assert r.status_code == 200 and r.json()["token_ok"] is False
    r = await client.post("/v1/vr/log", json=_events(1), headers=bad)
    assert r.status_code == 401
    r = await client.get("/v1/health", headers=bad)
    assert r.status_code == 401


def test_token_equal_handles_non_ascii() -> None:
    from app.auth import token_equal
    assert token_equal("abc", "abc")
    assert not token_equal("tést", "test")
    assert token_equal("tést", "tést")


def test_build_signature_survives_vanishing_files_and_skips_dotfiles(tmp_path, monkeypatch) -> None:
    (tmp_path / "js").mkdir()
    (tmp_path / "js" / "a.js").write_text("a")
    (tmp_path / ".DS_Store").write_text("x")
    assert [rel for rel, *_ in vr_routes._page_signature(tmp_path)] == ["js/a.js"]

    def gone(path):
        raise FileNotFoundError(path)
    monkeypatch.setattr(vr_routes.os, "scandir", gone)
    assert vr_routes._page_signature(tmp_path) == ()


@pytest.mark.asyncio
async def test_dotfiles_are_not_served(client: AsyncClient) -> None:
    build = _build_of((await client.get("/vr/")).text)
    dot = vr_routes.STATIC_DIR / "js" / ".hidden-test.js"
    dot.write_text("secret")
    try:
        assert (await client.get(f"/vr/b/{build}/js/.hidden-test.js")).status_code == 404
        assert (await client.get("/vr/js/.hidden-test.js")).status_code == 404
    finally:
        dot.unlink()


def test_websocket_token_from_subprotocols() -> None:
    from app.routers.render import _token_from_subprotocols
    assert _token_from_subprotocols("visivo.v1, visivo-token.AbC_123") == ("AbC_123", "visivo.v1")
    assert _token_from_subprotocols("visivo-token.x") == ("x", None)
    assert _token_from_subprotocols("") == ("", None)
    assert _token_from_subprotocols("chat, superchat") == ("", None)


@pytest.mark.asyncio
async def test_info_token_checks_cannot_be_spread_over_forged_addresses(client: AsyncClient, monkeypatch) -> None:
    """X-Forwarded-For (which uvicorn trusts from 127.0.0.1) does not pick
    the bucket: every request carrying it shares one."""
    monkeypatch.setattr(vr_routes, "TOKEN_CHECKS", vr_routes._TokenCheckLimiter(per_minute=3))
    answers = []
    for i in range(6):
        r = await client.get("/v1/vr/info", headers={"X-Visivo-Token": "wrong", "X-Forwarded-For": f"10.9.8.{i}"})
        answers.append(r.json()["token_ok"])
    assert answers == [False, False, False, None, None, None]


def test_token_check_limiter_has_a_global_cap() -> None:
    lim = vr_routes._TokenCheckLimiter(per_minute=5, max_clients=1000, global_per_minute=10)
    allowed = sum(lim.allow(f"c{i}", now=0.0) for i in range(50))
    assert allowed == 10
    assert lim.allow("fresh", now=61.0)


def test_token_check_key() -> None:
    from starlette.requests import Request as _Req

    def req(headers, client=("127.0.0.1", 5000)):
        return _Req({"type": "http", "headers": [(k.lower().encode(), v.encode()) for k, v in headers.items()], "client": client})
    assert vr_routes.token_check_key(req({})) == "127.0.0.1"
    assert vr_routes.token_check_key(req({"X-Forwarded-For": "1.2.3.4"})) == "forwarded"
    assert vr_routes.token_check_key(req({"Forwarded": "for=1.2.3.4"})) == "forwarded"
    assert vr_routes.token_check_key(req({}, client=None)) == "?"


# ── Scrubbing stays linear (adversarial inputs from scrub_cases.json) ───────

_ADVERSARIAL = json.loads((Path(__file__).resolve().parent / "vr" / "scrub_cases.json").read_text(encoding="utf-8"))["adversarial"]


@pytest.mark.parametrize("unit", _ADVERSARIAL["units"])
def test_scrub_is_fast_on_adversarial_input(unit) -> None:
    import time as _time
    n = _ADVERSARIAL["length"]
    text = (unit * (n // len(unit) + 1))[:n]
    vr_routes.scrub(text[:50])                         # compiled and warm
    t0 = _time.perf_counter()
    vr_routes.scrub(text, ("secret-token-xyz",))
    ms = (_time.perf_counter() - t0) * 1000
    assert ms < _ADVERSARIAL["max_ms"], f"{unit!r}×n: {ms:.1f} ms"


def test_known_secrets_are_replaced_before_the_cut() -> None:
    secret = "Zx81secretTOKEN"
    out = vr_routes.scrub("x" * 3000 + " " + secret, (secret,))
    assert secret not in out


@pytest.mark.asyncio
async def test_log_route_scrubs_off_the_event_loop(client: AsyncClient, auth_headers, monkeypatch) -> None:
    """The scrubbing of a log request runs in a worker thread."""
    import threading
    seen = []
    real = vr_routes.scrub

    def spy(text, secrets_=()):
        seen.append(threading.current_thread() is threading.main_thread())
        return real(text, secrets_)
    monkeypatch.setattr(vr_routes, "scrub", spy)
    r = await client.post("/v1/vr/log", json=_events(2, "xtoken" * 600), headers=auth_headers)
    assert r.status_code == 200
    assert seen and not any(seen)


@pytest.mark.asyncio
async def test_log_scrubs_the_secret_from_every_field(client: AsyncClient, auth_headers, caplog) -> None:
    """The event's name went to the log as sent: its pattern admits a token."""
    token = auth_headers["X-Visivo-Token"]
    body = {"build": token, "user_agent": f"agent {token}",
            "events": [{"level": "error", "event": token, "message": f"m {token}"},
                       {"level": "info", "event": f"x:{token}", "message": token}]}
    with caplog.at_level(logging.DEBUG, logger="visivo.vr"):
        r = await client.post("/v1/vr/log", json=body, headers=auth_headers)
    assert r.status_code == 200 and r.json() == {"accepted": 2}
    lines = [rec.getMessage() for rec in caplog.records if rec.name == "visivo.vr"]
    assert len(lines) == 3                                # two events and the user agent
    for line in lines:
        assert token not in line, line
    assert any(line.startswith("[vr <token>] <token>: m <token>") for line in lines), lines
    assert any("x:<token>: <token>" in line for line in lines), lines


@pytest.mark.asyncio
async def test_an_upgrade_is_never_cached_under_the_old_build(client: AsyncClient) -> None:
    """A file replaced after the build was hashed (here with its size and
    modification time kept, so the cached build still names it) used to be
    served under that build as immutable: new bytes cached for a year under the
    old address. Only the bytes the build was hashed from are immutable."""
    import os
    f = vr_routes.STATIC_DIR / "js" / "zz-immutable-test.js"
    f.write_text("export const a = 1;\n")
    try:
        build = _build_of((await client.get("/vr/")).text)
        r = await client.get(f"/vr/b/{build}/js/zz-immutable-test.js")
        assert r.status_code == 200 and "immutable" in r.headers["cache-control"]
        assert r.text == "export const a = 1;\n"
        st = f.stat()
        f.write_text("export const a = 2;\n")             # the upgrade: same size
        os.utime(f, ns=(st.st_atime_ns, st.st_mtime_ns))  # and the same mtime
        r = await client.get(f"/vr/b/{build}/js/zz-immutable-test.js")
        assert r.status_code == 200
        assert r.text == "export const a = 2;\n"
        assert r.headers["cache-control"] == "no-cache"
        # Back to the bytes the build was hashed from: immutable again.
        f.write_text("export const a = 1;\n")
        os.utime(f, ns=(st.st_atime_ns, st.st_mtime_ns))
        r = await client.get(f"/vr/b/{build}/js/zz-immutable-test.js")
        assert "immutable" in r.headers["cache-control"] and r.headers.get("etag")
    finally:
        f.unlink(missing_ok=True)
