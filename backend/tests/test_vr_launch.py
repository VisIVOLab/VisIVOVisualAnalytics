"""The headset app's launch (issue #255): Digital Asset Links statements, and
the one-time, short-lived launch tickets the page trades for the token."""
from __future__ import annotations

import json
import logging
from pathlib import Path

import pytest
from httpx import AsyncClient

from app import vr_launch as L
from app.routers import vr_launch as launch_routes

FP_A = ":".join(["AA"] * 32)
FP_B = ":".join(["0B"] * 32)


@pytest.fixture(autouse=True)
def _fresh(monkeypatch, tmp_path):
    """Fresh tickets and rate limits; no release certificate, no development
    key and no extra file unless a test sets them (never the user's own)."""
    monkeypatch.setattr(L, "TICKETS", L.LaunchTickets())
    monkeypatch.setattr(launch_routes, "EXCHANGES", type(launch_routes.EXCHANGES)(per_minute=20, global_per_minute=120))
    cfg = tmp_path / "vr_app.json"
    cfg.write_text(json.dumps({"packages": {"release": "it.inaf.oact.visivo.vr",
                                            "development": "it.inaf.oact.visivo.vr.dev"},
                               "release_sha256": []}), encoding="utf-8")
    monkeypatch.setattr(L, "APP_CONFIG", cfg)
    monkeypatch.setenv(L.APP_DIR_ENV, str(tmp_path / "keys"))
    monkeypatch.delenv(L.ASSETLINKS_ENV, raising=False)
    monkeypatch.delenv(L.RELEASE_SHA256_ENV, raising=False)
    return cfg


@pytest.fixture(autouse=True)
def _drop_the_sessions_made():
    from app.sessions import REGISTRY
    before = set(REGISTRY._sessions)
    yield
    for sid in [s for s in REGISTRY._sessions if s not in before]:
        REGISTRY._sessions.pop(sid, None)


async def _session(client: AsyncClient, auth_headers: dict) -> dict:
    r = await client.post("/v1/sessions/new", json={}, headers=auth_headers)
    assert r.status_code == 200
    return {**auth_headers, "X-Visivo-Session": r.json()["session_id"]}


# ── Digital Asset Links ──────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_assetlinks_absent_without_any_certificate(client: AsyncClient) -> None:
    assert (await client.get("/.well-known/assetlinks.json")).status_code == 404


@pytest.mark.asyncio
async def test_assetlinks_list_release_and_development_certificates(client: AsyncClient, monkeypatch,
                                                                    tmp_path: Path, _fresh: Path) -> None:
    cfg = json.loads(_fresh.read_text())
    cfg["release_sha256"] = [FP_A.lower()]
    _fresh.write_text(json.dumps(cfg))
    keys = tmp_path / "keys"
    keys.mkdir()
    (keys / L.DEV_FINGERPRINT_FILE).write_text(FP_B.replace(":", "") + "\n", encoding="utf-8")
    r = await client.get("/.well-known/assetlinks.json")
    assert r.status_code == 200, r.text
    assert r.headers["content-type"].startswith("application/json")
    assert r.headers["cache-control"] == "no-cache"
    body = r.json()
    assert [s["target"]["package_name"] for s in body] == ["it.inaf.oact.visivo.vr", "it.inaf.oact.visivo.vr.dev"]
    assert body[0]["target"]["sha256_cert_fingerprints"] == [FP_A]
    assert body[1]["target"]["sha256_cert_fingerprints"] == [FP_B]
    assert all(s["relation"] == ["delegate_permission/common.handle_all_urls"] for s in body)
    assert all(s["target"]["namespace"] == "android_app" for s in body)


@pytest.mark.asyncio
async def test_assetlinks_release_fingerprint_from_the_environment(client: AsyncClient, monkeypatch) -> None:
    monkeypatch.setenv(L.RELEASE_SHA256_ENV, f"{FP_A}, not-a-fingerprint ,{FP_B}")
    body = (await client.get("/.well-known/assetlinks.json")).json()
    assert body == [L._statement("it.inaf.oact.visivo.vr", [FP_A, FP_B])]


@pytest.mark.asyncio
async def test_assetlinks_extra_statements_file(client: AsyncClient, monkeypatch, tmp_path: Path) -> None:
    statements = [{
        "relation": ["delegate_permission/common.handle_all_urls"],
        "target": {"namespace": "android_app", "package_name": "org.example.vr",
                   "sha256_cert_fingerprints": [FP_A]},
    }]
    f = tmp_path / "assetlinks.json"
    f.write_text(json.dumps(statements), encoding="utf-8")
    monkeypatch.setenv(L.ASSETLINKS_ENV, str(f))
    r = await client.get("/.well-known/assetlinks.json")
    assert r.status_code == 200
    assert r.json() == statements


@pytest.mark.asyncio
@pytest.mark.parametrize("content", ['{"not": "a list"}', "[1, 2]", "not json", "",
                                     '[{"v": NaN}]', '[{"v": Infinity}]', '[{"v": 1e999}]',
                                     '[{"v": ' + "[" * 5000 + "0" + "]" * 5000 + "}]"])
async def test_assetlinks_refuses_a_file_that_is_not_statements(client: AsyncClient, monkeypatch, tmp_path: Path,
                                                                content: str) -> None:
    f = tmp_path / "assetlinks.json"
    f.write_text(content, encoding="utf-8")
    monkeypatch.setenv(L.ASSETLINKS_ENV, str(f))
    assert (await client.get("/.well-known/assetlinks.json")).status_code == 404
    monkeypatch.setenv(L.ASSETLINKS_ENV, str(tmp_path / "missing.json"))
    assert (await client.get("/.well-known/assetlinks.json")).status_code == 404


def test_fingerprints_are_normalised_or_refused() -> None:
    assert L.normalise_fingerprint(FP_A.lower()) == FP_A
    assert L.normalise_fingerprint("ab" * 32) == ":".join(["AB"] * 32)
    for bad in ("", "AA:BB", "ZZ" * 32, FP_A + ":CC", None):
        assert L.normalise_fingerprint(bad) is None


def test_a_broken_app_config_gives_no_release_statement(_fresh: Path) -> None:
    _fresh.write_text("{broken", encoding="utf-8")
    assert L.assetlinks_statements() == []
    _fresh.write_text(json.dumps({"packages": {"release": "not a package"}, "release_sha256": [FP_A]}))
    assert L.assetlinks_statements() == []


def test_the_shipped_config_names_both_packages() -> None:
    cfg = L.app_config(Path(L.__file__).resolve().parent / "vr_app.json")
    assert cfg["packages"] == {"release": "it.inaf.oact.visivo.vr", "development": "it.inaf.oact.visivo.vr.dev"}
    assert all(L.normalise_fingerprint(f) for f in cfg["release_sha256"])


#: The project's release signing certificate (public: tools/quest-app/README.md).
RELEASE_CERT = "A1:54:C4:5B:54:20:5F:B9:6B:B9:80:28:6D:86:F3:A6:78:17:E1:32:2E:4F:68:AB:44:40:30:C0:43:04:D8:D4"


@pytest.mark.asyncio
async def test_every_backend_lists_the_release_app(client: AsyncClient, monkeypatch) -> None:
    """With the shipped vr_app.json alone (no development key, no extra
    file, no environment), the origin's statements name the release app with
    the project's certificate: what the headset's browser checks before it
    opens the app bundled with the desktop."""
    monkeypatch.setattr(L, "APP_CONFIG", Path(L.__file__).resolve().parent / "vr_app.json")
    r = await client.get("/.well-known/assetlinks.json")
    assert r.status_code == 200
    assert r.json() == [{"relation": ["delegate_permission/common.handle_all_urls"],
                         "target": {"namespace": "android_app", "package_name": "it.inaf.oact.visivo.vr",
                                    "sha256_cert_fingerprints": [RELEASE_CERT]}}]


# ── Tickets: the store ───────────────────────────────────────────────────────

class _Clock:
    def __init__(self) -> None:
        self.t = 1000.0

    def __call__(self) -> float:
        return self.t


def test_a_ticket_is_exchanged_once() -> None:
    T = L.LaunchTickets()
    got = T.issue("tok-123456789", "sess-1", {"dataset_id": "d", "path": "/x.fits", "kind": "cube", "box": None})
    assert L.TICKET_RE.match(got["ticket"]) and L.LAUNCH_RE.match(got["launch"])
    assert got["expires_in"] == L.TICKET_TTL_S
    out = T.exchange(got["ticket"], ("other-token-xyz", "tok-123456789"))
    assert out == {"token": "tok-123456789", "session": "sess-1", "launch": got["launch"],
                   "cube": {"dataset_id": "d", "path": "/x.fits", "kind": "cube", "box": None}}
    assert T.exchange(got["ticket"], ("tok-123456789",)) is None


def test_only_hashes_are_stored() -> None:
    T = L.LaunchTickets()
    got = T.issue("tok-123456789", "sess-1", None)
    held = repr(T._tickets) + repr(T._launches)
    assert got["ticket"] not in held
    assert "tok-123456789" not in held
    assert L._sha256(got["ticket"]) in T._tickets


def test_an_expired_ticket_is_refused() -> None:
    clock = _Clock()
    T = L.LaunchTickets(ttl=60.0, clock=clock)
    got = T.issue("tok-123456789", "s", None)
    clock.t += 60.0
    assert T.exchange(got["ticket"], ("tok-123456789",)) is None
    assert T.status(got["launch"], "tok-123456789", "s")["state"] == "expired"


def test_a_ticket_whose_token_was_revoked_is_refused() -> None:
    T = L.LaunchTickets()
    got = T.issue("old-token-12345", "s", None)
    assert T.exchange(got["ticket"], ("new-token-67890",)) is None
    # Consumed by that attempt, whatever its outcome.
    assert T.exchange(got["ticket"], ("old-token-12345",)) is None


@pytest.mark.parametrize("bad", ["", "short", "x" * 300, "a b" * 20, None, 123, "../" * 20])
def test_malformed_tickets_are_refused(bad) -> None:
    T = L.LaunchTickets()
    T.issue("tok-123456789", "s", None)
    assert T.exchange(bad, ("tok-123456789",)) is None


def test_tickets_are_bounded_the_oldest_go() -> None:
    T = L.LaunchTickets(max_tickets=3, max_launches=4)
    issued = [T.issue("tok-123456789", "s", None) for _ in range(5)]
    assert len(T._tickets) == 3 and len(T._launches) == 4
    assert T.exchange(issued[0]["ticket"], ("tok-123456789",)) is None
    assert T.exchange(issued[4]["ticket"], ("tok-123456789",)) is not None


def test_launch_status_follows_the_ticket_for_its_owner_only() -> None:
    clock = _Clock()
    T = L.LaunchTickets(clock=clock, wall=lambda: 1794700000.0)
    got = T.issue("tok-123456789", "s1", None)
    st = T.status(got["launch"], "tok-123456789", "s1")
    assert st["state"] == "pending" and st["exchanged_at"] is None and st["expires_in"] == 60.0
    assert T.status(got["launch"], "tok-123456789", "s2") is None
    assert T.status(got["launch"], "another-token-1", "s1") is None
    assert T.status("nope", "tok-123456789", "s1") is None
    T.exchange(got["ticket"], ("tok-123456789",))
    st = T.status(got["launch"], "tok-123456789", "s1")
    assert st["state"] == "exchanged" and st["exchanged_at"] == 1794700000.0
    clock.t += L.LAUNCH_KEEP_S + 1
    assert T.status(got["launch"], "tok-123456789", "s1") is None


# ── Tickets: the routes ──────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_launch_ticket_round_trip(client: AsyncClient, auth_headers: dict, caplog) -> None:
    h = await _session(client, auth_headers)
    cube = {"dataset_id": "ds-1", "path": "/data/ngc3137.fits", "kind": "cube", "box": [[0, 9], [0, 9], [2, 5]]}
    caplog.set_level(logging.DEBUG)
    r = await client.post("/v1/vr/launch", json={"cube": cube}, headers=h)
    assert r.status_code == 200, r.text
    assert r.headers["cache-control"] == "no-store"
    got = r.json()
    assert got["page"] == "/vr/?launch=immersive" and got["expires_in"] == L.TICKET_TTL_S
    ticket, launch = got["ticket"], got["launch"]

    st = await client.get(f"/v1/vr/launch/{launch}", headers=h)
    assert st.status_code == 200 and st.json()["state"] == "pending" and st.json()["client"] is None

    # The exchange is public: no token header, the ticket is the credential.
    r = await client.post("/v1/vr/launch/exchange", json={"ticket": ticket})
    assert r.status_code == 200, r.text
    assert r.headers["cache-control"] == "no-store"
    out = r.json()
    assert out == {"token": auth_headers["X-Visivo-Token"], "session": h["X-Visivo-Session"],
                   "launch": launch, "cube": cube}
    again = await client.post("/v1/vr/launch/exchange", json={"ticket": ticket})
    assert again.status_code == 404

    st = (await client.get(f"/v1/vr/launch/{launch}", headers=h)).json()
    assert st["state"] == "exchanged" and st["exchanged_at"] > 0

    # The page's hello names the launch: the desktop sees which page it is.
    r = await client.post("/v1/vr/headset/hello", json={"client": "q3-app", "transport": "adb",
                                                        "label": "Quest 3", "launch": launch}, headers=h)
    assert r.status_code == 200, r.text
    st = (await client.get(f"/v1/vr/launch/{launch}", headers=h)).json()
    assert st["client"] == "q3-app"
    listed = (await client.get("/v1/vr/headset", headers=h)).json()["headsets"]
    assert listed[0]["launch"] == launch
    # A reconnect's hello without it keeps it.
    await client.post("/v1/vr/headset/hello", json={"client": "q3-app", "transport": "adb"}, headers=h)
    assert (await client.get(f"/v1/vr/launch/{launch}", headers=h)).json()["client"] == "q3-app"

    # Neither the ticket nor the token reaches the log.
    assert ticket not in caplog.text
    assert auth_headers["X-Visivo-Token"] not in caplog.text


@pytest.mark.asyncio
async def test_launch_without_a_cube_and_without_a_body(client: AsyncClient, auth_headers: dict) -> None:
    h = await _session(client, auth_headers)
    for kwargs in ({}, {"json": {}}):
        r = await client.post("/v1/vr/launch", headers=h, **kwargs)
        assert r.status_code == 200, r.text
        out = (await client.post("/v1/vr/launch/exchange", json={"ticket": r.json()["ticket"]})).json()
        assert out["cube"] is None and out["session"] == h["X-Visivo-Session"]


@pytest.mark.asyncio
async def test_launch_needs_the_token(client: AsyncClient, auth_headers: dict) -> None:
    assert (await client.post("/v1/vr/launch", json={})).status_code == 401
    assert (await client.post("/v1/vr/launch", json={}, headers={"X-Visivo-Token": "wrong-token"})).status_code == 401
    h = await _session(client, auth_headers)
    launch = (await client.post("/v1/vr/launch", json={}, headers=h)).json()["launch"]
    assert (await client.get(f"/v1/vr/launch/{launch}")).status_code == 401


@pytest.mark.asyncio
async def test_launch_status_is_the_owners_only(client: AsyncClient, auth_headers: dict) -> None:
    h1 = await _session(client, auth_headers)
    h2 = await _session(client, auth_headers)
    launch = (await client.post("/v1/vr/launch", json={}, headers=h1)).json()["launch"]
    assert (await client.get(f"/v1/vr/launch/{launch}", headers=h2)).status_code == 404
    assert (await client.get("/v1/vr/launch/unknown-launch", headers=h1)).status_code == 404


@pytest.mark.asyncio
async def test_exchange_refusals(client: AsyncClient) -> None:
    r = await client.post("/v1/vr/launch/exchange", json={"ticket": "A" * 43})
    assert r.status_code == 404 and r.headers["cache-control"] == "no-store"
    # Not JSON (a form a foreign page could post without a preflight): refused.
    r = await client.post("/v1/vr/launch/exchange", content=b"ticket=" + b"A" * 43,
                          headers={"Content-Type": "application/x-www-form-urlencoded"})
    assert r.status_code == 415
    r = await client.post("/v1/vr/launch/exchange", content=b"{}", headers={"Content-Type": "application/json"})
    assert r.status_code == 422
    r = await client.post("/v1/vr/launch/exchange", content=b'{"ticket": "' + b"A" * 2000 + b'"}',
                          headers={"Content-Type": "application/json"})
    assert r.status_code == 413
    # The ticket never goes in the address.
    assert (await client.get("/v1/vr/launch/exchange?ticket=" + "A" * 43)).status_code in (401, 404, 405)


@pytest.mark.asyncio
async def test_exchanges_are_rate_limited(client: AsyncClient, auth_headers: dict, monkeypatch) -> None:
    monkeypatch.setattr(launch_routes, "EXCHANGES", type(launch_routes.EXCHANGES)(per_minute=3, global_per_minute=100))
    for _ in range(3):
        assert (await client.post("/v1/vr/launch/exchange", json={"ticket": "B" * 43})).status_code == 404
    r = await client.post("/v1/vr/launch/exchange", json={"ticket": "B" * 43})
    assert r.status_code == 429 and r.headers["retry-after"] == "60"
    # A forwarded-for header does not buy a fresh key.
    r = await client.post("/v1/vr/launch/exchange", json={"ticket": "B" * 43}, headers={"X-Forwarded-For": "10.0.0.9"})
    assert r.status_code in (404, 429)


@pytest.mark.asyncio
async def test_hello_launch_field_is_checked(client: AsyncClient, auth_headers: dict) -> None:
    h = await _session(client, auth_headers)
    r = await client.post("/v1/vr/headset/hello", json={"client": "q3", "transport": "adb", "launch": "bad launch!"},
                          headers=h)
    assert r.status_code == 422
