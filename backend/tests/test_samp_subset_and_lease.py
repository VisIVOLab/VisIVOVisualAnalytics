"""SAMP: sending a subset of a catalogue, and the acknowledged outbound queue (issue #130).

* ``/v1/samp/send-catalogue`` with ``row_indices`` stages only those rows (as
  ``/v1/catalogue/query`` numbers them in ``row_index``) as a new VOTable, and
  says so with ``subset: true``; a backend without it would send the whole
  table, which the client must not take for the subset.
* ``GET /v1/samp/pending?lease=true`` keeps the messages queued until
  ``POST /v1/samp/pending/ack``: a client that fails between the fetch and
  the send gets them again once the lease runs out.
"""
from __future__ import annotations

import os
from io import BytesIO

import pytest

os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")

AUTH = {"X-Visivo-Token": "test-token-visivo-12345"}


@pytest.fixture()
def client():
    from fastapi.testclient import TestClient
    from app.main import app
    return TestClient(app, raise_server_exceptions=True)


@pytest.fixture()
def empty_queue():
    from app.samp_bridge import SAMP
    SAMP.drain_pending()
    yield SAMP
    SAMP.drain_pending()


def _csv(tmp_path, rows=10):
    p = tmp_path / "cat.csv"
    lines = ["name,ra,dec,flux"]
    lines += [f"s{i},{10.0 + i},{-5.0 + i * 0.1},{100.0 - i}" for i in range(rows)]
    p.write_text("\n".join(lines) + "\n")
    return p


def _staged_table(params):
    from astropy.table import Table
    from app.samp_bridge import get_samp_file_path
    token = params["url"].rsplit("/", 1)[-1]
    path = get_samp_file_path(token)
    assert path is not None
    return Table.read(path, format="votable")


def _send(client, path, fmt, **extra):
    r = client.post("/v1/samp/send-catalogue",
                    json={"path": str(path), "format": fmt, "deliver": "return", **extra},
                    headers=AUTH)
    assert r.status_code == 200, r.text
    return r.json()


def test_subset_of_a_csv_keeps_the_rows_asked_for(client, tmp_path):
    body = _send(client, _csv(tmp_path), "csv", row_indices=[7, 2, 3],
                 filename="cat-subset.vot", table_id="visivo-sub")
    assert body["valid"], body
    assert body["subset"] is True
    assert body["rows"] == 3
    message = body["message"]
    assert message["mtype"] == "table.load.votable"
    assert message["params"]["table-id"] == "visivo-sub"
    assert message["params"]["name"] == "cat-subset.vot"
    table = _staged_table(message["params"])
    assert list(table["name"]) == ["s7", "s2", "s3"]


def test_whole_table_is_not_a_subset(client, tmp_path):
    body = _send(client, _csv(tmp_path), "csv")
    assert body["valid"], body
    assert body["subset"] is False
    assert body["rows"] == 10


def test_subset_of_a_votable_keeps_units(client, tmp_path):
    from astropy import units as u
    from astropy.table import Table
    src = tmp_path / "cat.vot"
    table = Table({"ra": [1.0, 2.0, 3.0, 4.0], "flux": [5.0, 6.0, 7.0, 8.0]})
    table["flux"].unit = u.mJy
    table.write(src, format="votable")
    body = _send(client, src, "votable", row_indices=[3, 1])
    assert body["valid"], body
    assert body["subset"] is True
    sent = _staged_table(body["message"]["params"])
    assert list(sent["ra"]) == [4.0, 2.0]
    assert sent["flux"].unit == u.mJy


def test_subset_of_a_fits_table_goes_out_as_votable(client, tmp_path):
    from astropy.table import Table
    src = tmp_path / "cat.fits"
    Table({"ra": [1.0, 2.0, 3.0]}).write(src, format="fits")
    body = _send(client, src, "fits", row_indices=[0, 2])
    assert body["valid"], body
    assert body["message"]["mtype"] == "table.load.votable"
    assert body["message"]["params"]["name"].endswith(".vot")
    assert list(_staged_table(body["message"]["params"])["ra"]) == [1.0, 3.0]


@pytest.mark.parametrize("rows", [[], [10], [-1]])
def test_subset_refuses_rows_outside_the_table(client, tmp_path, rows):
    body = _send(client, _csv(tmp_path), "csv", row_indices=rows)
    assert body["valid"] is False
    assert body["error"]


def test_lease_keeps_messages_until_acknowledged(client, empty_queue):
    job = empty_queue.enqueue_for_qt_client("coord.pointAt.sky", {"ra": "1", "dec": "2"})

    first = client.get("/v1/samp/pending", params={"lease": "true"}, headers=AUTH).json()
    assert [m["job_id"] for m in first["messages"]] == [job]
    assert "leased_until" not in first["messages"][0]
    assert first["lease_seconds"] > 0

    # Held for the client that has it: not handed out twice meanwhile.
    again = client.get("/v1/samp/pending", params={"lease": "true"}, headers=AUTH).json()
    assert again["count"] == 0

    # The client never confirmed it (it crashed): once the lease runs out the
    # message comes back (its lease is made to have run out).
    with empty_queue._pending_lock:
        empty_queue._pending_send[0]["leased_until"] = 0.0
    retried = client.get("/v1/samp/pending", params={"lease": "true"}, headers=AUTH).json()
    assert [m["job_id"] for m in retried["messages"]] == [job]

    acked = client.post("/v1/samp/pending/ack", json={"job_ids": [job, "unknown"]},
                        headers=AUTH).json()
    assert acked == {"acked": 1}
    assert client.get("/v1/samp/pending", params={"lease": "true"},
                      headers=AUTH).json()["count"] == 0


def test_plain_pending_still_drains(client, empty_queue):
    job = empty_queue.enqueue_for_qt_client("coord.pointAt.sky", {"ra": "1", "dec": "2"})
    first = client.get("/v1/samp/pending", headers=AUTH).json()
    assert [m["job_id"] for m in first["messages"]] == [job]
    assert client.get("/v1/samp/pending", headers=AUTH).json()["count"] == 0
