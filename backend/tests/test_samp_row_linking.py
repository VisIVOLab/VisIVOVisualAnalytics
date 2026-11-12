"""SAMP row linking and staging (issue #130).

* ``/v1/catalogue/query`` returns, for each row, its 0-based position in the
  table as opened (``row_index``): SAMP's table.highlight.row and
  table.select.rowList name rows that way, and a client that loaded a filtered,
  sorted or sampled subset needs it to map them.
* ``/v1/samp/send-catalogue`` puts the client's table-id on the table.load
  message, so later row messages can name the table.
* ``/v1/samp/import-url`` goes through the guarded fetch: a hub-local URL is
  refused (the client stages those with ``upload-file``).
"""
from __future__ import annotations

import os

import pytest

os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")

AUTH = {"X-Visivo-Token": "test-token-visivo-12345"}


@pytest.fixture()
def client():
    from fastapi.testclient import TestClient
    from app.main import app
    return TestClient(app, raise_server_exceptions=True)


def _csv(tmp_path, rows=10):
    p = tmp_path / "cat.csv"
    lines = ["name,ra,dec,flux"]
    # flux decreasing with row: a sort reverses the order.
    lines += [f"s{i},{10.0 + i},{-5.0 + i * 0.1},{100.0 - i}" for i in range(rows)]
    p.write_text("\n".join(lines) + "\n")
    return p


def _open(client, path):
    r = client.post("/v1/catalogue/open", json={"path": str(path), "format": "csv"},
                    headers=AUTH)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"], body
    return body["dataset_id"]


def _query(client, dataset_id, **extra):
    r = client.post("/v1/catalogue/query", json={"dataset_id": dataset_id, **extra},
                    headers=AUTH)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"], body
    return body


def test_query_row_index_plain(client, tmp_path):
    body = _query(client, _open(client, _csv(tmp_path)), limit=4, offset=3)
    assert body["row_index"] == [3, 4, 5, 6]
    assert [r["name"] for r in body["rows"]] == ["s3", "s4", "s5", "s6"]


def test_query_row_index_follows_filter_and_sort(client, tmp_path):
    dataset_id = _open(client, _csv(tmp_path))
    body = _query(client, dataset_id,
                  filters=[{"field": "flux", "op": "<=", "value": "95"}],
                  sort={"field": "flux", "direction": "asc"}, limit=3)
    # flux <= 95 keeps rows 5..9; ascending flux puts row 9 first.
    assert body["row_index"] == [9, 8, 7]
    assert [r["name"] for r in body["rows"]] == ["s9", "s8", "s7"]


def test_query_row_index_even_sample(client, tmp_path):
    dataset_id = _open(client, _csv(tmp_path, rows=10))
    body = _query(client, dataset_id, limit=5, even_sample=True, phase=1)
    assert body["step"] == 2
    assert body["row_index"] == [1, 3, 5, 7, 9]


def test_original_row_positions_refuses_ambiguous_index():
    import pandas as pd
    from app.routers.catalogue import _original_row_positions
    table = pd.DataFrame({"a": [1, 2, 3]}, index=[0, 0, 1])
    assert _original_row_positions(table, table.iloc[:2]) == []


def test_send_catalogue_carries_table_id(client, tmp_path, monkeypatch):
    from app.samp_bridge import SAMP
    sent = []
    monkeypatch.setattr(SAMP, "send_direct", lambda mtype, params, target: False)
    monkeypatch.setattr(SAMP, "enqueue_for_qt_client",
                        lambda mtype, params, target: sent.append((mtype, params)) or "job")
    r = client.post("/v1/samp/send-catalogue",
                    json={"path": str(_csv(tmp_path)), "format": "csv",
                          "table_id": "visivo-abc"},
                    headers=AUTH)
    assert r.status_code == 200, r.text
    assert r.json()["valid"], r.json()
    mtype, params = sent[-1]
    assert mtype == "table.load.votable"
    assert params["table-id"] == "visivo-abc"
    assert params["url"].startswith("__BACKEND_BASE_URL__/v1/samp/files/")


def test_send_catalogue_without_table_id_sends_none(client, tmp_path, monkeypatch):
    from app.samp_bridge import SAMP
    sent = []
    monkeypatch.setattr(SAMP, "send_direct", lambda mtype, params, target: False)
    monkeypatch.setattr(SAMP, "enqueue_for_qt_client",
                        lambda mtype, params, target: sent.append(params) or "job")
    r = client.post("/v1/samp/send-catalogue",
                    json={"path": str(_csv(tmp_path)), "format": "csv"}, headers=AUTH)
    assert r.json()["valid"], r.json()
    assert "table-id" not in sent[-1]


def test_send_catalogue_return_hands_the_message_back(client, tmp_path, monkeypatch):
    """deliver=return: nothing queued or sent; the caller gets the message (to
    send as a SAMP call), the row count and the staged size."""
    from app.samp_bridge import SAMP, get_samp_file_path
    monkeypatch.setattr(SAMP, "send_direct",
                        lambda *a: pytest.fail("must not be sent by the backend"))
    monkeypatch.setattr(SAMP, "enqueue_for_qt_client",
                        lambda *a: pytest.fail("must not be queued"))
    r = client.post("/v1/samp/send-catalogue",
                    json={"path": str(_csv(tmp_path, rows=10)), "format": "csv",
                          "table_id": "visivo-x", "deliver": "return"}, headers=AUTH)
    body = r.json()
    assert body["valid"], body
    assert body["rows"] == 10
    assert body["size_bytes"] > 0
    message = body["message"]
    assert message["mtype"] == "table.load.votable"
    assert message["params"]["table-id"] == "visivo-x"
    token = message["params"]["url"].rsplit("/", 1)[-1]
    staged = get_samp_file_path(token)
    # A converted table is BINARY2: compact, and read back with its rows in order.
    assert b"BINARY2" in staged.read_bytes()
    from astropy.table import Table
    table = Table.read(staged, format="votable")
    assert list(table["name"]) == [f"s{i}" for i in range(10)]


def test_send_fits_return_hands_the_message_back(client, tmp_path, monkeypatch):
    from app.samp_bridge import SAMP
    monkeypatch.setattr(SAMP, "enqueue_for_qt_client",
                        lambda *a: pytest.fail("must not be queued"))
    fits_path = tmp_path / "img.fits"
    fits_path.write_bytes(b"SIMPLE  =                    T" + b" " * 2850)
    r = client.post("/v1/samp/send-fits",
                    json={"path": str(fits_path), "deliver": "return"}, headers=AUTH)
    body = r.json()
    assert body["valid"], body
    assert body["message"]["mtype"] == "image.load.fits"
    assert body["size_bytes"] == fits_path.stat().st_size


def test_import_url_refuses_loopback(client, monkeypatch):
    monkeypatch.delenv("VISIVO_SAMP_ALLOWED_HOSTS", raising=False)
    r = client.post("/v1/samp/import-url",
                    json={"url": "http://127.0.0.1:2525/dynamic/4/t0.vot"}, headers=AUTH)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"] is False
    assert "SSRF" in body["error"] or "non-public" in body["error"]


def test_import_url_refuses_file_scheme(client):
    r = client.post("/v1/samp/import-url", json={"url": "file:///etc/passwd"}, headers=AUTH)
    body = r.json()
    assert body["valid"] is False
    assert "http" in body["error"]


def test_query_random_sample_keeps_original_samp_rows(client, tmp_path):
    dataset_id = _open(client, _csv(tmp_path, rows=103))
    seen = set()
    for phase in range(11):
        body = _query(client, dataset_id, limit=10, random_sample=True, phase=phase)
        assert body["valid"] and body["step"] == 11
        positions = body["row_index"]
        assert seen.isdisjoint(positions)
        assert [r["name"] for r in body["rows"]] == [f"s{i}" for i in positions]
        seen.update(positions)
    assert seen == set(range(103))
