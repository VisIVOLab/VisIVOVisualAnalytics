"""Tests for the "Last 5 jobs" history.

The panel was permanently empty: only ``/v1/tasks/moment`` and ``/v1/tasks/pv``
create a TaskRecord, and everything else the backend computes is synchronous.
The history is now taken in the HTTP middleware, so these tests are about two
things — that real work is recorded, and that browsing, polling and interactive
scrubbing are not (a channel slice per mouse move would erase the five jobs the
user actually ran).
"""
from __future__ import annotations

import asyncio

import numpy as np
import pytest
from astropy.io import fits

from app.main import _envelope_failed, _history_operation_name
from app.tasks import TaskRegistry


class _Request:
    """The two things ``_history_operation_name`` reads off a request."""

    def __init__(self, method: str, route_path: str | None = None):
        self.method = method
        self.scope = {}
        if route_path is not None:
            self.scope["route"] = type("Route", (), {"path": route_path})()


# ── What counts as a job ─────────────────────────────────────────────────────

def test_a_computation_is_recorded():
    assert _history_operation_name(_Request("POST"), "/v1/products/moment") == "products/moment"


def test_the_route_template_is_preferred_over_the_concrete_path():
    """Twenty moment maps on twenty datasets are twenty of the same operation."""
    request = _Request("POST", "/v1/photometry/aperture/{dataset_id}")
    assert _history_operation_name(request, "/v1/photometry/aperture/ds_abc123") \
        == "photometry/aperture/{dataset_id}"


def test_reads_are_not_jobs():
    assert _history_operation_name(_Request("GET"), "/v1/products/moment") is None


@pytest.mark.parametrize("path", [
    "/v1/files/list",
    "/v1/files/classify",
    "/v1/sessions/new",
    "/v1/render/create_session",
    "/v1/image/tile",
    "/v1/samp/send",
    "/v1/assistant/config",
])
def test_housekeeping_is_not_a_job(path):
    assert _history_operation_name(_Request("POST"), path) is None


@pytest.mark.parametrize("path", [
    "/v1/cube/slice",          # channel scrubbing
    "/v1/spectral/probe",      # spectrum under the cursor
    "/v1/vlkb/mcutout/phase",  # polled every 2 s
])
def test_interactive_traffic_is_not_a_job(path):
    assert _history_operation_name(_Request("POST"), path) is None


def test_tasks_routes_are_left_to_the_task_registry():
    """They already create a TaskRecord; recording them here would double them."""
    assert _history_operation_name(_Request("POST"), "/v1/tasks/moment") is None


def test_collecting_a_computed_product_is_not_a_second_job():
    assert _history_operation_name(_Request("POST"), "/v1/moment/result/binary") is None


def test_a_chat_stream_is_not_a_job():
    """It runs for as long as the user reads; /v1/assistant/query is the job."""
    assert _history_operation_name(_Request("POST"), "/v1/assistant/stream") is None


def test_computed_binary_products_are_still_jobs():
    """Only `/result/binary` collects an already-recorded product; the rest
    compute on the spot."""
    assert _history_operation_name(_Request("POST"), "/v1/spectral/linewidth/binary") \
        == "spectral/linewidth/binary"
    assert _history_operation_name(_Request("POST"), "/v1/moment/result/binary") is None


# ── Reading the envelope ─────────────────────────────────────────────────────

def test_a_failed_envelope_is_detected():
    """Most routes report failure as HTTP 200 with valid:false."""
    assert _envelope_failed(b'{"valid":false,"error":"Unsupported file type."}')


def test_a_successful_envelope_is_not_a_failure():
    assert not _envelope_failed(b'{"valid":true,"dataset_id":"ds_1"}')


def test_a_nested_failure_does_not_condemn_the_response():
    """One failed row of a batch that itself succeeded — including when the
    body is longer than the peek window, so the JSON cannot be parsed."""
    body = b'{"valid":true,"results":[{"valid":false}],"pad":"' + b"x" * 5000
    assert not _envelope_failed(body)


def test_a_truncated_failed_envelope_is_still_detected():
    assert _envelope_failed(b'{"valid":false,"rows":[' + b"1," * 5000)


def test_a_non_json_body_is_not_a_failure():
    assert not _envelope_failed(b"\x89PNG\r\n\x1a\n")


def test_non_versioned_paths_are_ignored():
    assert _history_operation_name(_Request("POST"), "/static/logo.png") is None


# ── The registry ─────────────────────────────────────────────────────────────

def test_recorded_operations_appear_in_the_snapshot():
    registry = TaskRegistry()
    registry.record_operation("products/moment", duration_seconds=2.5)
    recent = registry.snapshot()["recent_tasks"]
    assert [entry["operation"] for entry in recent] == ["products/moment"]
    assert recent[0]["duration_seconds"] == 2.5
    assert recent[0]["status"] == "done"


def test_operations_do_not_inflate_the_task_registry():
    """They carry no task id and must not look like pollable tasks."""
    registry = TaskRegistry()
    registry.record_operation("products/moment")
    assert registry.stats()["entries"] == 0


def test_tasks_and_operations_share_one_ordered_list():
    registry = TaskRegistry()
    registry.record_operation("spectral/stack")
    record = registry.create("moment")
    registry.update(record.task_id, status="done")
    registry.record_operation("products/isosurface")

    recent = registry.snapshot()["recent_tasks"]
    assert [entry["operation"] for entry in recent] \
        == ["products/isosurface", "moment", "spectral/stack"]


def test_a_completed_task_is_in_the_snapshot():
    """The task API's terminal status is "completed", not "done".

    That is what `/v1/tasks/moment` sets and what the client polls for, so a
    snapshot that recognised only "done" listed no moment at all — neither
    running nor finished. Driving the real application through Compute Moment
    is what showed it: the map appeared, and the activity panel stayed empty.
    """
    registry = TaskRegistry()
    record = registry.create("moment")
    registry.update(record.task_id, status="completed")

    recent = registry.snapshot()["recent_tasks"]
    assert [entry["operation"] for entry in recent] == ["moment"]
    assert recent[0]["status"] == "completed"
    assert registry.snapshot()["running_count"] == 0


def test_the_snapshot_limit_is_honoured():
    registry = TaskRegistry()
    for index in range(10):
        registry.record_operation(f"op/{index}")
    assert len(registry.snapshot(recent_limit=5)["recent_tasks"]) == 5


def test_history_is_bounded():
    """A long session must not accumulate operations without limit."""
    registry = TaskRegistry()
    for index in range(500):
        registry.record_operation(f"op/{index}")
    assert len(registry._history) <= 50
    assert registry.snapshot()["recent_tasks"][0]["operation"] == "op/499"


def test_a_failed_operation_is_recorded_as_failed():
    registry = TaskRegistry()
    registry.record_operation("products/moment", status="failed")
    assert registry.snapshot()["recent_tasks"][0]["status"] == "failed"


def test_a_negative_duration_is_clamped():
    registry = TaskRegistry()
    registry.record_operation("products/moment", duration_seconds=-1.0)
    assert registry.snapshot()["recent_tasks"][0]["duration_seconds"] == 0.0


# ── End to end ───────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_opening_a_dataset_shows_up_in_health(client, auth_headers, tmp_path):
    path = tmp_path / "map.fits"
    fits.PrimaryHDU(np.zeros((16, 16), dtype=np.float32)).writeto(path)

    await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    # Browsing in between must not push the job out of the list.
    await client.post("/v1/files/classify", json={"path": str(path)}, headers=auth_headers)

    recent = (await client.get("/v1/health", headers=auth_headers)).json()["recent_tasks"]
    operations = [entry["operation"] for entry in recent]
    assert "datasets/open" in operations
    assert "files/classify" not in operations


@pytest.mark.asyncio
async def test_a_failed_open_is_not_filed_as_a_finished_job(client, auth_headers, tmp_path):
    """The route answers HTTP 200 with valid:false; the history must not say
    the job succeeded."""
    path = tmp_path / "notes.txt"
    path.write_text("this is not a dataset\n")

    await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)

    recent = (await client.get("/v1/health", headers=auth_headers)).json()["recent_tasks"]
    opens = [entry for entry in recent if entry["operation"] == "datasets/open"]
    assert opens and opens[0]["status"] == "failed"


# ── The whole way round: a task route reaching the health panel ──────────────

@pytest.mark.asyncio
async def test_a_moment_task_reaches_the_activity_panel(client, opened_cube) -> None:
    """Submit a real moment task and find it in `/v1/health`.

    The unit test above pins the registry, but it writes the terminal status
    itself: it would still pass if `/v1/tasks/moment` stopped recording the
    task at all. This one goes through the route the viewer uses and reads the
    list the Data Hub shows, which is the thing that was actually empty.
    """
    created = await client.post(
        "/v1/tasks/moment",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "moment_order": 0,
            "channel_start": 0,
            "channel_end": 8,
            "mask_enabled": False,
            "threshold_value": 0.0,
        },
        headers=opened_cube["headers"],
    )
    assert created.status_code == 200
    task_id = created.json()["task_id"]

    for _ in range(50):
        status = await client.get(f"/v1/tasks/{task_id}", headers=opened_cube["headers"])
        if status.json()["status"] in ("completed", "failed"):
            break
        await asyncio.sleep(0.1)
    assert status.json()["status"] == "completed"

    health = await client.get("/v1/health", headers=opened_cube["headers"])
    assert health.status_code == 200
    body = health.json()
    assert "moment" in [entry["operation"] for entry in body["recent_tasks"]], body
    assert body["running_tasks"] == 0
