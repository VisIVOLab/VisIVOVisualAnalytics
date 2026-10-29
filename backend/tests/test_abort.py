"""Cooperative abort: a cancelled task's worker stops within one slab.

The worker processes cannot be interrupted from the event loop; the moment
and PV workers poll a stop file between their slabs (app/compute/abort.py),
which the task registry touches on cancellation and removes when the work
ends.
"""
from __future__ import annotations

import asyncio
import os
from pathlib import Path

import numpy as np
import pytest

from app.compute.abort import ComputationAborted, check_abort
from app.tasks import TaskRegistry

POS = "CIRCLE 83.82208 -5.39111 0.003"


def _id(path):
    return Path(path).resolve().as_uri()


# ── the check itself and the workers ─────────────────────────────────────────

def test_check_abort_only_raises_for_an_existing_file(tmp_path):
    check_abort(None)
    check_abort(str(tmp_path / "absent"))
    (tmp_path / "stop").touch()
    with pytest.raises(ComputationAborted):
        check_abort(str(tmp_path / "stop"))


def test_streaming_moment_stops_between_slabs(tmp_path, monkeypatch):
    from app.compute import moment

    cube = np.ones((8, 4, 4), dtype=np.float32)
    coords = np.arange(8, dtype=np.float64)
    monkeypatch.setattr(moment, "_moment_chunk_z", lambda plane_bytes, requested: 2)   # four slabs
    seen = []
    stop = tmp_path / "stop"
    armed = {"on": True}

    class Cube:
        shape = cube.shape

        def __getitem__(self, key):
            seen.append(key)
            if armed["on"] and len(seen) == 2:
                stop.touch()     # the client aborts while the second slab is read
            return cube[key]

    with pytest.raises(ComputationAborted):
        moment._moment_map_streaming(Cube(), 0, 7, 0, coords, False, 0.0, str(stop))
    assert len(seen) == 2, "the third slab was never read"
    # Without a stop file the same call completes.
    seen.clear()
    armed["on"] = False
    stop.unlink()
    out = moment._moment_map_streaming(Cube(), 0, 7, 0, coords, False, 0.0, str(stop))
    assert np.allclose(out, 8.0) and len(seen) == 4


def test_moment_workers_refuse_an_aborted_task(synthetic_fits_cube, tmp_path):
    from app.compute import worker_moment, worker_moment_save_fits

    stop = tmp_path / "stop"
    stop.touch()
    with pytest.raises(ComputationAborted):
        worker_moment(str(synthetic_fits_cube), 0, 0, 7, False, 0.0, False, False, str(stop))
    out = tmp_path / "m0.fits"
    with pytest.raises(ComputationAborted):
        worker_moment_save_fits(str(synthetic_fits_cube), str(out), 0, 0, 7, False, 0.0, False, False,
                                str(stop))
    assert not out.exists()
    stop.unlink()
    assert worker_moment(str(synthetic_fits_cube), 0, 0, 7, False, 0.0, False, False, str(stop))["width"] == 64


def test_pv_stops_between_channel_blocks(synthetic_fits_cube, tmp_path, monkeypatch):
    from app.compute import pv_iso

    stop = tmp_path / "stop"
    path = [[10.0, 10.0], [50.0, 50.0]]
    stop.touch()
    with pytest.raises(ComputationAborted):
        pv_iso.compute_pv(str(synthetic_fits_cube), path, 1, str(stop))
    stop.unlink()

    # A stop that lands mid-way: _bilinear_mean reads blocks of channels and
    # checks before each. Eight-channel blocks, the abort during the second.
    monkeypatch.setattr(pv_iso, "_PV_GATHER_BUDGET", 8)
    cube = np.ones((32, 8, 8), dtype=np.float32)
    reads = []
    armed = {"on": True}

    class Cube:
        shape = cube.shape

        def __getitem__(self, key):
            reads.append(key)
            if armed["on"] and len(reads) == 2:
                stop.touch()
            return cube[key]

    with pytest.raises(ComputationAborted):
        pv_iso._bilinear_mean(Cube(), np.array([2.0]), np.array([2.0]), 1, 1, 8, 8, str(stop))
    assert len(reads) == 2, "the third block was never read"
    stop.unlink()
    armed["on"] = False
    reads.clear()
    out = pv_iso._bilinear_mean(Cube(), np.array([2.0]), np.array([2.0]), 1, 1, 8, 8, str(stop))
    assert out.shape == (32, 1) and np.allclose(out, 1.0) and len(reads) == 4
    assert pv_iso.worker_pv(str(synthetic_fits_cube), path, 1, str(stop))["depth"] == 32


# ── the registry's side ──────────────────────────────────────────────────────

def test_registry_without_a_stop_directory_gives_no_stop_file():
    registry = TaskRegistry()
    record = registry.create("moment", owner_token="t")
    assert registry.stop_file(record.task_id) is None
    assert registry.stop_file("no-such-task") is None


def test_cancel_touches_the_stop_file_and_the_work_removes_it(tmp_path):
    loop = asyncio.new_event_loop()
    try:
        registry = TaskRegistry()
        registry.stop_dir_provider = lambda: tmp_path / ".abort"
        record = registry.create("moment", owner_token="t")
        stop = registry.stop_file(record.task_id)
        assert stop is not None and Path(stop).parent == tmp_path / ".abort"
        assert registry.peek(record.task_id).stop_file == stop
        assert not os.path.exists(stop)

        runner, work = loop.create_future(), loop.create_future()
        registry.update(record.task_id, status="running", asyncio_handle=runner, work_handle=work)
        assert registry.cancel(record.task_id)
        assert os.path.exists(stop), "the worker is told to stop"
        runner.cancel()
        loop.run_until_complete(asyncio.sleep(0))
        assert os.path.exists(stop), "the runner ended, the work has not: the file stays"
        work.set_result(None)
        loop.run_until_complete(asyncio.sleep(0))
        assert not os.path.exists(stop)

        # Forgetting a record whose work is going leaves the file to the
        # work's callback; forgetting an ended one removes it at once.
        busy = registry.create("pv", owner_token="t")
        stop_busy = registry.stop_file(busy.task_id)
        work = loop.create_future()
        registry.update(busy.task_id, status="running", work_handle=work)
        registry.cancel(busy.task_id)
        registry.forget(busy.task_id)
        assert os.path.exists(stop_busy)
        work.set_result(None)
        loop.run_until_complete(asyncio.sleep(0))
        assert not os.path.exists(stop_busy)

        idle = registry.create("pv", owner_token="t")
        stop_idle = registry.stop_file(idle.task_id)
        Path(stop_idle).touch()
        registry.forget(idle.task_id)
        assert not os.path.exists(stop_idle)
    finally:
        loop.close()


def test_a_cancel_with_no_live_handle_leaves_nothing_behind(tmp_path):
    registry = TaskRegistry()
    registry.stop_dir_provider = lambda: tmp_path / ".abort"
    record = registry.create("moment", owner_token="t")
    stop = registry.stop_file(record.task_id)
    registry.update(record.task_id, status="running")
    assert registry.cancel(record.task_id)
    assert not os.path.exists(stop)


def test_eviction_removes_the_stop_file(tmp_path):
    registry = TaskRegistry(ttl_seconds=1)
    registry.stop_dir_provider = lambda: tmp_path / ".abort"
    record = registry.create("moment", owner_token="t")
    stop = registry.stop_file(record.task_id)
    Path(stop).touch()
    registry.update(record.task_id, status="done")
    registry.peek(record.task_id).last_touched_monotonic = 0.0
    registry.stats()                                # evicts what the TTL expired
    assert registry.peek(record.task_id) is None
    assert not os.path.exists(stop)


# ── through the routes ───────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_uws_abort_ends_the_worker_within_one_slab(client, auth_headers, synthetic_fits_cube,
                                                         monkeypatch):
    """ABORT touches the job's stop file: a worker that polls it ends soon
    after, the node stops counting the job, and the file is removed."""
    from app.routers import soda
    from app.tasks import TASKS

    polls = {"stop": None, "n": 0}

    async def polling_execute(operation, path, values, out_dir, task_id=None):
        stop_file = TASKS.stop_file(task_id)
        polls["stop"] = stop_file
        for _ in range(500):            # up to 5 s: a real worker's slab
            await asyncio.sleep(0.01)
            polls["n"] += 1
            check_abort(stop_file)
        return {"output_path": str(out_dir / "late.fits"), "applied": ""}

    with monkeypatch.context() as patched:
        patched.setattr(soda, "_execute", polling_execute)
        created = await client.post("/soda/async", headers=auth_headers,
                                    data={"ID": _id(synthetic_fits_cube), "PHASE": "RUN"})
        job = created.headers["location"]
        await asyncio.sleep(0.05)
        assert polls["stop"] and polls["stop"].endswith(job.rsplit("/", 1)[1])
        assert TASKS.snapshot()["running_count"] == 1
        await client.post(job + "/phase", data={"PHASE": "ABORT"}, headers=auth_headers)
        assert (await client.get(job + "/phase", headers=auth_headers)).text == "ABORTED"
        for _ in range(50):
            await asyncio.sleep(0.01)
            if TASKS.snapshot()["running_count"] == 0:
                break
        assert TASKS.snapshot()["running_count"] == 0, "the worker did not stop"
        assert polls["n"] < 100, "the worker ran on after the abort"
        assert not os.path.exists(polls["stop"])


GIVEN: list[tuple[str, str | None]] = []


def _fake_worker_moment(path, order, z0, z1, mask, thr, auto=False, var=False, stop_file=None):
    GIVEN.append(("moment", stop_file))
    return {"width": 1, "height": 1, "scalar_type": "float32", "range_min": 0.0, "range_max": 0.0,
            "spectral_axis_type": "VRAD", "spectral_axis_unit": "m/s", "moment_unit": "", "bunit": "",
            "moment_threads_used": 1, "threshold_used": None, "data_base64": ""}


def _fake_worker_pv(path, vertices, width, stop_file=None):
    GIVEN.append(("pv", stop_file))
    return {"depth": 1, "num_samples": 1}


@pytest.mark.asyncio
async def test_moment_and_pv_tasks_hand_their_workers_a_stop_file(client, opened_cube, monkeypatch):
    """The task routes name their task to the product helpers, which make the
    stop file, give it to the worker and register the pool task on the record."""
    import app.compute as compute
    from app import dependencies
    from app.tasks import TASKS

    async def inline(fn, *args):
        return fn(*args)

    monkeypatch.setattr(dependencies, "_run", inline)
    monkeypatch.setattr(compute, "worker_moment", _fake_worker_moment)
    monkeypatch.setattr(compute, "worker_pv", _fake_worker_pv)
    monkeypatch.setattr(dependencies, "_select_moment_backend", lambda order, nbytes: (False, None))
    GIVEN.clear()
    body = {"dataset_id": opened_cube["dataset_id"], "moment_order": 0, "channel_start": 0,
            "channel_end": 3, "mask_enabled": False, "threshold_value": 0.123}
    task_id = (await client.post("/v1/tasks/moment", json=body, headers=opened_cube["headers"])).json()["task_id"]
    pv_id = (await client.post("/v1/tasks/pv", json={"dataset_id": opened_cube["dataset_id"],
                                                     "vertices": [[1.5, 1.5], [10.5, 10.5]], "width_pixels": 1},
                               headers=opened_cube["headers"])).json()["task_id"]
    for _ in range(100):
        await asyncio.sleep(0.01)
        if len(GIVEN) == 2:
            break
    given = dict(GIVEN)
    assert given["moment"] == TASKS.peek(task_id).stop_file and given["moment"].endswith(task_id)
    assert given["pv"] == TASKS.peek(pv_id).stop_file and given["pv"].endswith(pv_id)
    assert Path(given["moment"]).parent.name == ".abort"
    assert TASKS.peek(task_id).work_handle is not None, "the pool task is on the record"


@pytest.mark.asyncio
async def test_work_cancelled_while_queued_is_never_started(tmp_path, monkeypatch):
    """A task cancelled (or deleted) while its computation waits for a slot
    does not start it: nothing runs, the slot is released, nothing is counted."""
    from app import dependencies
    from app.tasks import TaskRegistry

    registry = TaskRegistry()
    registry.stop_dir_provider = lambda: tmp_path / ".abort"
    monkeypatch.setattr(dependencies, "TASKS", registry)
    sem = asyncio.Semaphore(0)
    monkeypatch.setattr(dependencies, "_HEAVY_SEM", sem)
    ran = []

    async def run(fn, *args):
        ran.append(fn)

    monkeypatch.setattr(dependencies, "_run", run)
    for ending in ("cancel", "forget"):
        record = registry.create("moment", owner_token="t")
        registry.update(record.task_id, status="running")
        caller = asyncio.create_task(dependencies._run_heavy(lambda: None, task_id=record.task_id))
        await asyncio.sleep(0)                        # queued on the semaphore
        registry.cancel(record.task_id) if ending == "cancel" else registry.forget(record.task_id)
        sem.release()
        with pytest.raises(asyncio.CancelledError):
            await caller
        assert ran == [] and sem._value == 1 and registry.snapshot()["running_count"] == 0
        sem._value = 0


@pytest.mark.asyncio
async def test_abort_before_the_worker_starts_is_not_lost(client, auth_headers, synthetic_fits_cube,
                                                          monkeypatch):
    """The stop file exists from the job's start, and an ABORT that lands
    before the computation is dispatched stops it from being dispatched: the
    job is started and aborted within one event-loop step, before its runner
    has run at all."""
    from app.routers import soda
    from app.tasks import TASKS

    started = []

    async def execute(operation, path, values, out_dir, task_id=None):
        record = TASKS.peek(task_id)       # what the real _execute does first
        if record is None or record.status == "cancelled":
            raise asyncio.CancelledError()
        started.append(task_id)
        return {"output_path": str(out_dir / "x.fits"), "applied": ""}

    monkeypatch.setattr(soda, "_execute", execute)
    created = await client.post("/soda/async", headers=auth_headers,
                                data={"ID": _id(synthetic_fits_cube)})          # held: no PHASE=RUN
    job = created.headers["location"]
    job_id = job.rsplit("/", 1)[1]
    assert (await client.get(job + "/phase", headers=auth_headers)).text == "PENDING"
    soda._start(job_id)                      # schedules the runner...
    stop = TASKS.peek(job_id).stop_file
    assert stop and stop.endswith(job_id), "allocated before the runner ran"
    assert TASKS.cancel(job_id)              # ...and the abort lands first
    assert os.path.exists(stop)
    await asyncio.sleep(0.05)
    assert started == [], "the computation was dispatched after the abort"
    assert (await client.get(job + "/phase", headers=auth_headers)).text == "ABORTED"
    assert TASKS.snapshot()["running_count"] == 0
    assert not os.path.exists(stop)


@pytest.mark.asyncio
async def test_real_execute_refuses_a_job_aborted_before_it_ran(client, auth_headers, synthetic_fits_cube,
                                                                monkeypatch):
    """The runner has scheduled the real _execute (one loop step after the
    start) when the abort lands: _execute's own guard sees the cancelled
    record and never dispatches the worker."""
    from app.routers import soda
    from app.tasks import TASKS

    dispatched = []

    async def heavy(fn, *args, **kw):
        dispatched.append(fn)
        return {"output_path": "unused"}

    monkeypatch.setattr(soda, "_run_heavy", heavy)
    created = await client.post("/soda/async", headers=auth_headers,
                                data={"ID": _id(synthetic_fits_cube), "OPERATION": "MOMENT", "MOMENT": "0"})
    job = created.headers["location"]
    job_id = job.rsplit("/", 1)[1]
    soda._start(job_id)
    await asyncio.sleep(0)                   # the runner ran its first step: _execute is scheduled, not started
    assert TASKS.peek(job_id).work_handle is not None and not TASKS.peek(job_id).work_handle.done()
    assert TASKS.cancel(job_id)
    await asyncio.sleep(0.05)
    assert dispatched == [], "the worker was dispatched for an aborted job"
    assert (await client.get(job + "/phase", headers=auth_headers)).text == "ABORTED"
    assert TASKS.snapshot()["running_count"] == 0
    assert not os.path.exists(TASKS.peek(job_id).stop_file)
