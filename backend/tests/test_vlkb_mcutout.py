"""VLKB multi-cutout: the request body, the job protocol, and the archive.

Ported from the legacy client's MCutoutSummary (VLVA 1.7.4). The service is not
reachable from a test, so what is exercised here is everything either side of
the wire: how a POS string becomes the service's JSON, how a job id is read out
of a 303, how a report is normalised, and — the part that can do damage — how a
results tarball is unpacked.
"""
from __future__ import annotations

import json
import os
import tarfile

import pytest

from app.vlkb_mcutout import (
    McutoutError,
    build_request_body,
    extract_archive,
    job_id_from_location,
    parse_pos,
)

_TEST_TOKEN = "test-token-visivo-12345"


def _client():
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient

    from app.main import app
    return TestClient(app), {"X-Visivo-Token": _TEST_TOKEN}


# ── the request body ──────────────────────────────────────────────────────────

def test_a_cone_becomes_the_services_circle():
    assert parse_pos("CIRCLE 40.000000 0.000000 0.300000") == {
        "circle": {"lon": 40.0, "lat": 0.0, "radius": 0.3}}


def test_a_box_becomes_the_services_range():
    assert parse_pos("RANGE 21.784450 24.515150 -0.455400 0.568600") == {
        "range": {"lon1": 21.78445, "lon2": 24.51515, "lat1": -0.4554, "lat2": 0.5686}}


def test_a_wrapped_box_is_passed_through_as_it_stands():
    # l1 > l2 is how RANGE expresses a box across l = 0; "correcting" it here
    # would ask for the 356° of sky the user did not want.
    parsed = parse_pos("RANGE 359.000000 3.000000 -0.500000 0.500000")
    assert parsed["range"]["lon1"] == 359.0
    assert parsed["range"]["lon2"] == 3.0


@pytest.mark.parametrize("pos, message", [
    ("", "empty POS"),
    ("POLYGON 1 2 3 4 5 6", "unsupported POS shape"),
    ("CIRCLE 40 0", "needs 3 numbers"),
    ("CIRCLE 40 0 0.3 0.4", "needs 3 numbers"),
    ("CIRCLE 40 zero 0.3", "non-numeric"),
    ("CIRCLE 40 0 0", "radius greater than zero"),
    ("RANGE 1 2 3", "needs 4 numbers"),
])
def test_an_unusable_pos_is_refused_with_a_reason(pos, message):
    with pytest.raises(McutoutError) as exc:
        parse_pos(pos)
    assert message in str(exc.value)


def test_the_body_carries_one_entry_per_dataset():
    body = build_request_body([
        {"id": "ivo://a/1", "pos": "CIRCLE 40 0 0.3"},
        {"id": "ivo://a/2", "pos": "RANGE 1 2 -1 1", "possys": "galactic"},
    ])
    assert [entry["id"] for entry in body] == ["ivo://a/1", "ivo://a/2"]
    assert body[0]["possys"] == "GALACTIC"
    assert body[1]["possys"] == "GALACTIC"   # normalised
    assert "circle" in body[0]["pos"] and "range" in body[1]["pos"]


def test_a_dataset_without_an_identifier_is_refused():
    with pytest.raises(McutoutError) as exc:
        build_request_body([{"id": "   ", "pos": "CIRCLE 40 0 0.3"}])
    assert "no identifier" in str(exc.value)


def test_an_empty_batch_is_refused():
    with pytest.raises(McutoutError):
        build_request_body([])


def test_an_absurdly_large_batch_is_refused():
    items = [{"id": f"ivo://a/{i}", "pos": "CIRCLE 40 0 0.3"} for i in range(501)]
    with pytest.raises(McutoutError) as exc:
        build_request_body(items)
    assert "batch limit" in str(exc.value)


# ── the job id ────────────────────────────────────────────────────────────────

def test_the_job_id_is_the_last_segment_of_the_location():
    assert job_id_from_location(
        "https://vlkb.example.org/soda/async/uws/1a2b3c") == "1a2b3c"
    assert job_id_from_location("/soda/async/uws/job-42/") == "job-42"


def test_a_location_with_no_job_is_refused():
    with pytest.raises(McutoutError):
        job_id_from_location("https://vlkb.example.org/")


def test_a_traversal_location_is_reduced_to_its_last_segment():
    # Only the last segment is ever used, so "../.." carries nowhere: the id
    # here is "passwd", an ordinary (if odd) job name.
    assert job_id_from_location(
        "https://vlkb.example.org/uws/../../etc/passwd") == "passwd"


def test_a_job_id_that_is_not_an_identifier_is_refused():
    # It goes straight back into a URL, so a segment that is not a plain
    # identifier is refused rather than quoted away and hoped for.
    for location in ("https://vlkb.example.org/uws/job%20one",
                     "https://vlkb.example.org/uws/job/with spaces",
                     "https://vlkb.example.org/uws/" + "j" * 200):
        with pytest.raises(McutoutError):
            job_id_from_location(location)


def test_a_query_string_in_the_location_is_not_part_of_the_job_id():
    # urlparse keeps it out of the path, so the id is the segment alone.
    assert job_id_from_location("https://vlkb.example.org/uws/job7?x=1") == "job7"


# ── the report ────────────────────────────────────────────────────────────────

def test_the_report_marks_each_dataset_and_keeps_the_failure_text(monkeypatch):
    from app import vlkb_mcutout

    payload = json.dumps([
        {"type": "filename", "content": "cutout_1.fits"},
        {"type": "error", "message": "outside the survey footprint"},
    ]).encode()

    class _Response:
        status = 200

        def read(self, *_args):
            return payload

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

    monkeypatch.setattr(vlkb_mcutout, "_request", lambda *a, **k: _Response())
    results = vlkb_mcutout.job_report("https://vlkb.example.org", "job1")
    assert results[0] == {"index": 0, "ok": True, "filename": "cutout_1.fits", "detail": ""}
    assert results[1]["ok"] is False
    # The legacy reduced this to the word "Error"; it is the only clue to why
    # one dataset of twenty came back empty.
    assert "outside the survey footprint" in results[1]["detail"]


# ── the archive ───────────────────────────────────────────────────────────────

def _tar_with(tmp_path, members):
    """members: [(arcname, content)] — content None means a directory."""
    source = tmp_path / "src"
    source.mkdir(exist_ok=True)
    archive = tmp_path / "mcutout.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for name, content in members:
            path = source / "payload"
            path.write_bytes(content or b"")
            tar.add(path, arcname=name)
    return archive


def test_the_archive_is_unpacked_into_the_jobs_directory(tmp_path):
    archive = _tar_with(tmp_path, [("cutout_1.fits", b"SIMPLE  =  T"),
                                   ("cutout_2.fits", b"SIMPLE  =  T")])
    target = tmp_path / "job"
    target.mkdir()
    files = extract_archive(archive, target)
    assert [os.path.basename(f) for f in files] == ["cutout_1.fits", "cutout_2.fits"]
    assert all(os.path.exists(f) for f in files)


def test_a_member_that_escapes_the_directory_is_refused(tmp_path):
    """A tar member may be named ../../etc/…; extracting it writes there."""
    archive = _tar_with(tmp_path, [("../escaped.fits", b"SIMPLE  =  T")])
    target = tmp_path / "job"
    target.mkdir()
    with pytest.raises(McutoutError) as exc:
        extract_archive(archive, target)
    assert "outside its directory" in str(exc.value)
    assert not (tmp_path / "escaped.fits").exists()


def test_a_symlink_member_is_skipped_not_followed(tmp_path):
    archive = tmp_path / "links.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        link = tarfile.TarInfo("passwd")
        link.type = tarfile.SYMTYPE
        link.linkname = "/etc/passwd"
        tar.addfile(link)
        payload = tmp_path / "ok.fits"
        payload.write_bytes(b"SIMPLE  =  T")
        tar.add(payload, arcname="ok.fits")

    target = tmp_path / "job"
    target.mkdir()
    files = extract_archive(archive, target)
    assert [os.path.basename(f) for f in files] == ["ok.fits"]
    assert not (target / "passwd").exists()


def test_a_corrupt_archive_is_reported_not_raised_raw(tmp_path):
    archive = tmp_path / "broken.tar.gz"
    archive.write_bytes(b"this is not a tarball")
    target = tmp_path / "job"
    target.mkdir()
    with pytest.raises(McutoutError) as exc:
        extract_archive(archive, target)
    assert "could not be read" in str(exc.value)


# ── routes ────────────────────────────────────────────────────────────────────

def test_submit_without_a_configured_service_is_503(monkeypatch):
    client, auth = _client()
    monkeypatch.delenv("VISIVO_VLKB_URL", raising=False)
    r = client.post("/v1/vlkb/mcutout/submit",
                    json={"items": [{"id": "ivo://a/1", "pos": "CIRCLE 40 0 0.3"}]},
                    headers=auth)
    assert r.status_code == 503


def test_submit_refuses_a_loopback_service():
    client, auth = _client()
    r = client.post("/v1/vlkb/mcutout/submit",
                    json={"items": [{"id": "ivo://a/1", "pos": "CIRCLE 40 0 0.3"}],
                          "service_url": "http://127.0.0.1:9999"},
                    headers=auth)
    assert r.status_code == 400


def test_submit_refuses_an_unusable_pos(monkeypatch):
    client, auth = _client()
    from app.routers import vlkb as vlkb_router
    monkeypatch.setattr(vlkb_router, "assert_safe_fetch_url", lambda *a, **k: None)
    r = client.post("/v1/vlkb/mcutout/submit",
                    json={"items": [{"id": "ivo://a/1", "pos": "POLYGON 1 2 3"}],
                          "service_url": "https://vlkb.example.org"},
                    headers=auth)
    assert r.status_code == 422


def test_phase_starts_a_held_job(monkeypatch):
    client, auth = _client()
    from app import vlkb_mcutout
    from app.routers import vlkb as vlkb_router

    monkeypatch.setattr(vlkb_router, "assert_safe_fetch_url", lambda *a, **k: None)
    phases = iter(["HELD", "EXECUTING"])
    monkeypatch.setattr(vlkb_mcutout, "job_phase", lambda *a, **k: next(phases))
    monkeypatch.setattr(vlkb_mcutout, "run_job", lambda *a, **k: vlkb_mcutout.job_phase(*a, **k))

    r = client.post("/v1/vlkb/mcutout/phase",
                    json={"job_id": "job1", "service_url": "https://vlkb.example.org"},
                    headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    # HELD means "accepted but not started" — the client should not have to know
    # that, so the route starts it and reports where it got to.
    assert body["phase"] == "EXECUTING"
    assert body["terminal"] is False


def test_routes_require_auth():
    client, _ = _client()
    assert client.post("/v1/vlkb/mcutout/submit",
                       json={"items": []}).status_code in (401, 403)


# ── regressions from the review pass ──────────────────────────────────────────

def test_a_job_id_from_the_client_cannot_walk_out_of_its_directory(monkeypatch):
    """The id is interpolated into a URL *and* into the results directory."""
    from app.vlkb_mcutout import validate_job_id

    for hostile in ("x/../../escaped", "..", ".", "../../etc", "", "a b"):
        with pytest.raises(McutoutError):
            validate_job_id(hostile)
    assert validate_job_id(" job-7 ") == "job-7"


def test_the_routes_refuse_a_traversal_job_id():
    client, auth = _client()
    r = client.post("/v1/vlkb/mcutout/fetch",
                    json={"job_id": "x/../../escaped", "service_url": "https://vlkb.example.org"},
                    headers=auth)
    # Rejected by the model, before anything touches the filesystem.
    assert r.status_code == 422


def test_a_redirect_is_refused_rather_than_carrying_the_token_elsewhere():
    """urllib copies Authorization into a redirected request, so a service that
    answers 302 with a host of its choosing would be handed the user's token."""
    from app.vlkb_mcutout import _RefuseRedirect

    handler = _RefuseRedirect()
    with pytest.raises(McutoutError) as exc:
        handler.redirect_request(object(), None, 302, "Found", {},
                                 "http://169.254.169.254/latest/meta-data/")
    assert "redirects are not followed" in str(exc.value)


def test_the_token_is_never_sent_over_plain_http():
    from app import vlkb_mcutout

    with pytest.raises(McutoutError) as exc:
        vlkb_mcutout._request("http://vlkb.example.org/x", bearer="secret")
    assert "unencrypted" in str(exc.value)


def test_an_archive_that_unpacks_to_too_much_is_refused(tmp_path, monkeypatch):
    from app import vlkb_mcutout

    # A small download that expands enormously is how a disk gets filled.
    monkeypatch.setattr(vlkb_mcutout, "_MAX_EXTRACTED_BYTES", 1024)
    payload = tmp_path / "big.fits"
    payload.write_bytes(b"\0" * 4096)
    archive = tmp_path / "bomb.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        tar.add(payload, arcname="big.fits")

    target = tmp_path / "job"
    target.mkdir()
    with pytest.raises(McutoutError) as exc:
        vlkb_mcutout.extract_archive(archive, target)
    assert "unpack to more than" in str(exc.value)


def test_an_archive_with_too_many_members_is_refused(tmp_path, monkeypatch):
    from app import vlkb_mcutout

    monkeypatch.setattr(vlkb_mcutout, "_MAX_MEMBERS", 3)
    payload = tmp_path / "one.fits"
    payload.write_bytes(b"SIMPLE  =  T")
    archive = tmp_path / "many.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for i in range(10):
            tar.add(payload, arcname=f"cutout_{i}.fits")

    target = tmp_path / "job"
    target.mkdir()
    with pytest.raises(McutoutError) as exc:
        vlkb_mcutout.extract_archive(archive, target)
    assert "more than 3 entries" in str(exc.value)


def test_a_submitted_job_survives_a_failed_first_phase_read(monkeypatch):
    """The job exists on the archive from the moment the submit returns; losing
    its id would leave it running with nobody able to collect it."""
    client, auth = _client()
    from app import vlkb_mcutout
    from app.routers import vlkb as vlkb_router

    monkeypatch.setattr(vlkb_router, "assert_safe_fetch_url", lambda *a, **k: None)
    monkeypatch.setattr(vlkb_mcutout, "submit_job", lambda *a, **k: "job-42")

    def _phase_fails(*_args, **_kwargs):
        raise OSError("connection reset")

    monkeypatch.setattr(vlkb_mcutout, "job_phase", _phase_fails)
    r = client.post("/v1/vlkb/mcutout/submit",
                    json={"items": [{"id": "ivo://a/1", "pos": "CIRCLE 40 0 0.3"}],
                          "service_url": "https://vlkb.example.org"},
                    headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["job_id"] == "job-42"
    assert body["phase"] == "UNKNOWN"


def test_the_submit_also_refuses_to_send_the_token_over_plain_http():
    """submit_job built its own request and so observed neither the HTTPS rule
    nor the redirect rule; every request now goes through one place."""
    from app.vlkb_mcutout import submit_job

    with pytest.raises(McutoutError) as exc:
        submit_job("http://vlkb.example.org",
                   [{"id": "ivo://a/1", "pos": "CIRCLE 40 0 0.3"}], bearer="secret")
    assert "unencrypted" in str(exc.value)


def test_a_sparse_member_is_refused(tmp_path, monkeypatch):
    """tarfile does not check that a GNU sparse map agrees with the declared
    size, so member.size stops being a bound on what actually gets written —
    and that bound is what the extraction limit relies on.

    tarfile's *writer* cannot produce a sparse member (the attribute is only set
    when reading a GNU sparse header), so the member is stood in for here; what
    is under test is that the guard fires on it at all.
    """
    from app import vlkb_mcutout

    class _SparseMember:
        name = "sparse.fits"
        size = 1
        sparse = [(0, 4096)]
        type = b"0"

        @staticmethod
        def isfile():
            return True

        @staticmethod
        def isdir():
            return False

    class _StubTar:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def __iter__(self):
            return iter([_SparseMember()])

    monkeypatch.setattr(vlkb_mcutout.tarfile, "open", lambda *a, **k: _StubTar())
    target = tmp_path / "job"
    target.mkdir()
    with pytest.raises(McutoutError) as exc:
        vlkb_mcutout.extract_archive(tmp_path / "anything.tar", target)
    assert "sparse member" in str(exc.value)


def test_a_failed_fetch_leaves_no_half_unpacked_directory(tmp_path, monkeypatch):
    from app import vlkb_mcutout

    def _boom(*_args, **_kwargs):
        raise OSError("connection reset")

    monkeypatch.setattr(vlkb_mcutout, "_open", _boom)
    root = tmp_path / "exports"
    with pytest.raises(OSError):
        vlkb_mcutout.fetch_results("https://vlkb.example.org", "job7", str(root))
    # The staging directory goes with the failure: a half-written archive is
    # not a result, and leaving it invites someone to open it.
    assert list(root.glob("mcutout-*")) == []
