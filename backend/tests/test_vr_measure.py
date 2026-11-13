"""The VR page's measurement records (issue #224): POST /v1/vr/measurements
writes one JSON-lines file per record, scrubbed, bounded, each batch once,
and a record that cannot be stored answers 507 (the page carries on). Also
through the pairing bridge, as the page sends it from the public loader."""
from __future__ import annotations

import asyncio
import json
from pathlib import Path

import pytest
from httpx import AsyncClient

from app import vr_bridge as B
from app.routers import vr_measure as VM
from tests.conftest import _TEST_TOKEN

RECORD = "20261113-100000-0a1b2c3d"


@pytest.fixture(autouse=True)
def _fresh(monkeypatch, tmp_path):
    monkeypatch.setenv("VISIVO_VR_MEASURE_DIR", str(tmp_path / "measure"))
    monkeypatch.setattr(VM, "RATE", VM._Rate())
    VM._last_seq.clear()
    VM._warned.clear()


def _body(seq=0, lines=None, record=RECORD) -> dict:
    return {"record": record, "seq": seq, "lines": lines if lines is not None else [
        {"k": "h", "format": "visivo-vr-measure/1", "record": record, "device": {"user_agent": "Mozilla/5.0 (X11; Linux x86_64)"}},
        {"k": "s", "t": 1000.0, "frames": {"n": 3, "fps": 71.9, "missed_pct": None}, "iv": [13.9, 14.0, 13.8]},
        {"k": "e", "t": 1200.5, "name": "refined-first", "data": {"ms": 150}},
    ]}


def _read(tmp_path: Path, record=RECORD) -> list[dict]:
    text = (tmp_path / "measure" / f"vr-measure-{record}.jsonl").read_text(encoding="utf-8")
    return [json.loads(line) for line in text.splitlines()]


@pytest.mark.asyncio
async def test_needs_the_token(client: AsyncClient) -> None:
    r = await client.post("/v1/vr/measurements", json=_body())
    assert r.status_code == 401


@pytest.mark.asyncio
async def test_writes_the_lines_in_order_with_their_batch_number(client: AsyncClient, auth_headers, tmp_path) -> None:
    r = await client.post("/v1/vr/measurements", json=_body(), headers=auth_headers)
    assert r.status_code == 200, r.text
    got = r.json()
    assert got["accepted"] == 3 and got["file"] == f"vr-measure-{RECORD}.jsonl"
    assert "/" not in got["file"] and str(tmp_path) not in r.text          # the name, never the directory
    lines = _read(tmp_path)
    assert [line["k"] for line in lines] == ["h", "s", "e"]
    assert all(line["q"] == 0 for line in lines)
    assert "qn" not in lines[0] and lines[2]["qn"] == 3       # the batch's last line closes it
    assert lines[1]["iv"] == [13.9, 14.0, 13.8] and lines[1]["frames"]["missed_pct"] is None
    r = await client.post("/v1/vr/measurements", json=_body(1, [{"k": "s", "t": 2000.0}]), headers=auth_headers)
    assert r.json()["accepted"] == 1
    assert [line["q"] for line in _read(tmp_path)] == [0, 0, 0, 1]


@pytest.mark.asyncio
async def test_a_batch_sent_again_is_written_once_also_after_a_restart(client: AsyncClient, auth_headers, tmp_path) -> None:
    for seq in (0, 1):
        assert (await client.post("/v1/vr/measurements", json=_body(seq), headers=auth_headers)).status_code == 200
    again = await client.post("/v1/vr/measurements", json=_body(1), headers=auth_headers)
    assert again.status_code == 200 and again.json()["duplicate"] is True and again.json()["accepted"] == 0
    VM._last_seq.clear()                       # the backend restarted: read back from the file
    again = await client.post("/v1/vr/measurements", json=_body(1), headers=auth_headers)
    assert again.json()["duplicate"] is True
    assert len(_read(tmp_path)) == 6
    assert (await client.post("/v1/vr/measurements", json=_body(2), headers=auth_headers)).json()["accepted"] == 3


def test_a_batch_cut_short_is_cut_off_and_written_whole_when_sent_again(tmp_path) -> None:
    """A crash in the middle of an append leaves part of a batch: it is not
    taken as written, and its lines go when the batch comes again."""
    VM.write_batch(RECORD, 0, [{"k": "h"}, {"k": "s", "t": 1}])
    path = tmp_path / "measure" / f"vr-measure-{RECORD}.jsonl"
    with open(path, "ab") as f:
        f.write(b'{"k":"s","t":2,"q":1}\n{"k":"s","t":3,"q":1,"qn"')    # batch 1: a line and a torn one
    VM._last_seq.clear()                      # as after a restart
    got = VM.write_batch(RECORD, 1, [{"k": "s", "t": 2}, {"k": "s", "t": 3}])
    assert got["accepted"] == 2
    lines = _read(tmp_path)
    assert [line.get("t") for line in lines] == [None, 1, 2, 3]
    assert [line["q"] for line in lines] == [0, 0, 1, 1]
    assert lines[1]["qn"] == 2 and lines[3]["qn"] == 2 and "qn" not in lines[2]


def test_the_batch_markers_are_the_servers_not_the_pages(tmp_path) -> None:
    VM.write_batch(RECORD, 0, [{"k": "s", "q": 99, "qn": 1}, {"k": "s", "qn": 7}])
    lines = _read(tmp_path)
    assert [line["q"] for line in lines] == [0, 0]
    assert "qn" not in lines[0] and lines[1]["qn"] == 2


def test_a_record_that_cannot_be_read_back_is_not_appended_to(tmp_path, monkeypatch) -> None:
    import builtins
    VM.write_batch(RECORD, 0, [{"k": "h"}])
    VM._last_seq.clear()
    real_open = builtins.open
    monkeypatch.setattr(builtins, "open", lambda p, mode="r", *a, **k: (_ for _ in ()).throw(OSError(5, "Input/output error"))
                        if mode == "r+b" else real_open(p, mode, *a, **k))
    with pytest.raises(VM.StoreError, match="read the record back"):
        VM.write_batch(RECORD, 0, [{"k": "h"}])
    monkeypatch.setattr(builtins, "open", real_open)
    assert len(_read(tmp_path)) == 1
    assert VM.write_batch(RECORD, 0, [{"k": "h"}])["duplicate"] is True


def test_a_failure_before_the_append_never_cuts_the_record(tmp_path, monkeypatch) -> None:
    VM.write_batch(RECORD, 0, [{"k": "h"}, {"k": "s"}])
    path = tmp_path / "measure" / f"vr-measure-{RECORD}.jsonl"
    before = path.stat().st_size
    real = Path.stat
    failed = []

    def once(self, *a, **k):
        if self == path and not failed:
            failed.append(1)
            raise OSError(5, "Input/output error")
        return real(self, *a, **k)

    monkeypatch.setattr(Path, "stat", once)
    with pytest.raises(VM.StoreError):
        VM.write_batch(RECORD, 1, [{"k": "s"}])
    monkeypatch.setattr(Path, "stat", real)
    assert path.stat().st_size == before


def test_a_write_that_fails_half_way_leaves_nothing_of_its_batch(tmp_path, monkeypatch) -> None:
    import builtins
    real_open = builtins.open
    VM.write_batch(RECORD, 0, [{"k": "h"}])

    class Half:
        def __init__(self, path):
            self.f = real_open(path, "ab")

        def __enter__(self):
            return self

        def __exit__(self, *a):
            self.f.close()

        def write(self, data):
            self.f.write(data.split(b"\n", 1)[0] + b"\n")
            self.f.flush()
            raise OSError(28, "No space left on device")

        def flush(self):
            pass

    monkeypatch.setattr(builtins, "open", lambda p, mode="r", *a, **k: Half(p) if mode == "ab" else real_open(p, mode, *a, **k))
    with pytest.raises(VM.StoreError):
        VM.write_batch(RECORD, 1, [{"k": "s", "t": 1}, {"k": "s", "t": 2}])
    monkeypatch.setattr(builtins, "open", real_open)
    assert VM.write_batch(RECORD, 1, [{"k": "s", "t": 1}, {"k": "s", "t": 2}])["accepted"] == 2
    assert [line.get("t") for line in _read(tmp_path)] == [None, 1, 2]


@pytest.mark.asyncio
async def test_no_token_no_path_in_what_is_written(client: AsyncClient, auth_headers, tmp_path) -> None:
    lines = [{"k": "e", "t": 1.0, "name": "log", "data": {
        "message": f"open /Users/Jane Doe/data/secret_cube.fits failed with token={_TEST_TOKEN}",
        "url": f"http://localhost:8000/v1/render/0123456789abcdef/ws?token={_TEST_TOKEN}",
        "list": ["C:\\Users\\jane\\cube.fits", 1, None]}}]
    r = await client.post("/v1/vr/measurements", json=_body(0, lines), headers=auth_headers)
    assert r.status_code == 200
    text = (tmp_path / "measure" / f"vr-measure-{RECORD}.jsonl").read_text()
    assert _TEST_TOKEN not in text and "Jane Doe" not in text and "0123456789abcdef" not in text
    assert "jane" not in text and "secret_cube.fits" in text


@pytest.mark.asyncio
async def test_non_finite_numbers_become_null(client: AsyncClient, auth_headers, tmp_path) -> None:
    raw = '{"record": "%s", "seq": 0, "lines": [{"k": "s", "a": NaN, "b": Infinity, "iv": [1.5, -Infinity]}]}' % RECORD
    r = await client.post("/v1/vr/measurements", content=raw, headers={**auth_headers, "Content-Type": "application/json"})
    assert r.status_code == 200, r.text
    line = _read(tmp_path)[0]
    assert line["a"] is None and line["b"] is None and line["iv"] == [1.5, None]


@pytest.mark.asyncio
@pytest.mark.parametrize("body, what", [
    ({"record": "../../etc/x", "seq": 0, "lines": [{"k": "s"}]}, "record"),
    ({"record": RECORD, "seq": -1, "lines": [{"k": "s"}]}, "seq"),
    ({"record": RECORD, "seq": True, "lines": [{"k": "s"}]}, "seq"),
    ({"record": RECORD, "seq": 0, "lines": []}, "lines"),
    ({"record": RECORD, "seq": 0, "lines": [{"k": "s"}] * (VM.MAX_LINES + 1)}, "lines"),
    ({"record": RECORD, "seq": 0, "lines": [{"k": "x"}]}, "Bad line"),
    ({"record": RECORD, "seq": 0, "lines": ["text"]}, "Bad line"),
    ({"record": RECORD, "seq": 0, "lines": [{"k": "s", "bad key\n": 1}]}, "key"),
    ({"record": RECORD, "seq": 0, "lines": [{"k": "s", "a": {"b": {"c": {"d": {"e": {"f": {"g": 1}}}}}}}]}, "deep"),
    ({"record": RECORD, "seq": 0, "lines": [{"k": "s", "iv": list(range(VM.MAX_LIST + 1))}]}, "list"),
    ({"record": RECORD, "seq": 0, "lines": [{"k": "s", "s": "x" * (VM.MAX_STRING + 1)}]}, "string"),
    ({"record": RECORD, "seq": 0, "lines": [{"k": "s", **{f"k{i}": "y" * 1500 for i in range(30)}}]}, "over"),
])
async def test_bounds(client: AsyncClient, auth_headers, tmp_path, body, what) -> None:
    r = await client.post("/v1/vr/measurements", json=body, headers=auth_headers)
    assert r.status_code == 422, r.text
    assert what in r.text
    assert not (tmp_path / "measure").exists() or not any((tmp_path / "measure").iterdir())


@pytest.mark.asyncio
async def test_a_body_over_the_cap_is_413(client: AsyncClient, auth_headers) -> None:
    big = {"record": RECORD, "seq": 0, "lines": [{"k": "s", "s": "x" * 1900}] * 140}
    r = await client.post("/v1/vr/measurements", json=big, headers=auth_headers)
    assert r.status_code == 413


@pytest.mark.asyncio
async def test_rate_limited_per_token(client: AsyncClient, auth_headers, monkeypatch) -> None:
    monkeypatch.setattr(VM, "RATE_REQUESTS", 3)
    for seq in range(3):
        assert (await client.post("/v1/vr/measurements", json=_body(seq), headers=auth_headers)).status_code == 200
    r = await client.post("/v1/vr/measurements", json=_body(3), headers=auth_headers)
    assert r.status_code == 429 and int(r.headers["retry-after"]) >= 1


@pytest.mark.asyncio
async def test_an_unwritable_directory_is_507_and_says_why_without_the_path(client: AsyncClient, auth_headers, tmp_path, monkeypatch) -> None:
    blocker = tmp_path / "not-a-dir"
    blocker.write_text("x")
    monkeypatch.setenv("VISIVO_VR_MEASURE_DIR", str(blocker / "sub"))
    r = await client.post("/v1/vr/measurements", json=_body(), headers=auth_headers)
    assert r.status_code == 507
    assert "cannot write the record" in r.json()["detail"] and str(tmp_path) not in r.text
    # The next request is answered the same way, never a 500.
    assert (await client.post("/v1/vr/measurements", json=_body(1), headers=auth_headers)).status_code == 507


@pytest.mark.asyncio
async def test_a_full_disk_is_507(client: AsyncClient, auth_headers, monkeypatch) -> None:
    import builtins
    real_open = builtins.open

    def full(path, mode="r", *a, **k):
        if "vr-measure-" in str(path) and "a" in mode:
            raise OSError(28, "No space left on device")
        return real_open(path, mode, *a, **k)

    monkeypatch.setattr(builtins, "open", full)
    r = await client.post("/v1/vr/measurements", json=_body(), headers=auth_headers)
    assert r.status_code == 507 and "No space left on device" in r.json()["detail"]


@pytest.mark.asyncio
async def test_a_record_and_the_directory_are_bounded(client: AsyncClient, auth_headers, monkeypatch, tmp_path) -> None:
    monkeypatch.setattr(VM, "MAX_FILE_BYTES", 600)
    assert (await client.post("/v1/vr/measurements", json=_body(0), headers=auth_headers)).status_code == 200
    r = await client.post("/v1/vr/measurements", json=_body(1), headers=auth_headers)
    assert r.status_code == 507 and "MiB" in r.json()["detail"]
    monkeypatch.setattr(VM, "MAX_FILE_BYTES", 64 << 20)
    monkeypatch.setattr(VM, "MAX_DIR_BYTES", 100)
    other = "20261113-100001-0a1b2c3e"
    r = await client.post("/v1/vr/measurements", json=_body(0, record=other), headers=auth_headers)
    assert r.status_code == 507 and "directory" in r.json()["detail"]
    # The record already started may still grow (its own cap is what bounds it).
    assert (await client.post("/v1/vr/measurements", json=_body(1), headers=auth_headers)).status_code == 200


@pytest.mark.asyncio
async def test_the_default_directory_is_under_the_user_data_directory(monkeypatch) -> None:
    monkeypatch.delenv("VISIVO_VR_MEASURE_DIR", raising=False)
    assert VM.measure_dir() == Path.home() / ".visivo" / "vr-measurements"


class _Channel:
    def __init__(self) -> None:
        self.readyState = "open"
        self.bufferedAmount = 0
        self.bufferedAmountLowThreshold = 0
        self.sent: list = []
        self._handlers: dict[str, list] = {}

    def on(self, event, fn=None):
        self._handlers.setdefault(event, []).append(fn)
        return fn

    def remove_listener(self, event, fn):
        self._handlers.get(event, []).remove(fn)

    def emit(self, event, *args):
        for fn in list(self._handlers.get(event, [])):
            fn(*args)

    def send(self, data):
        self.sent.append(data)

    def close(self):
        self.readyState = "closed"
        self.emit("close")


@pytest.mark.asyncio
async def test_through_the_pairing_bridge_without_a_token_from_the_page(tmp_path) -> None:
    """The page in the public loader sends no token: the bridge adds its own."""
    from app.main import app

    ch = _Channel()
    bridge = B.ApiBridge(ch, app, _TEST_TOKEN)
    payload = json.dumps(_body()).encode()
    ch.emit("message", json.dumps({"t": "req", "id": 1, "method": "POST", "path": "/v1/vr/measurements",
                                   "headers": {"content-type": "application/json"}, "body": True}))
    ch.emit("message", B.api_chunk(1, payload, True))
    for _ in range(200):
        await asyncio.sleep(0.01)
        if any(isinstance(f, bytes) and B.parse_api_chunk(f)[1] for f in ch.sent):
            break
    head = json.loads(next(m for m in ch.sent if isinstance(m, str)))
    body = b"".join(B.parse_api_chunk(f)[2] for f in ch.sent if isinstance(f, bytes))
    assert head["status"] == 200, body
    assert json.loads(body)["accepted"] == 3
    assert len(_read(tmp_path)) == 3
    bridge.close()
