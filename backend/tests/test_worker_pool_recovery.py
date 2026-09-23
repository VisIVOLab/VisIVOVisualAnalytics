"""A worker process that dies must not wedge the backend.

A ProcessPoolExecutor whose worker is killed is permanently broken: every later
submit raises, and because the pool is a module-level global that means every
slice / preview / probe fails for the lifetime of the process while /health
keeps answering "ok". These tests pin the recovery.
"""
import asyncio
import os

import pytest
from concurrent.futures.process import BrokenProcessPool

from app import dependencies


def _suicide() -> None:
    """Die the way a segfaulting native library or the OOM killer does."""
    os._exit(1)


def _echo(value: int) -> int:
    return value * 2


@pytest.mark.asyncio
async def test_pool_survives_a_worker_that_dies() -> None:
    before = dependencies.pool_health()["restarts"]
    pool_before = dependencies._POOL

    with pytest.raises(Exception):
        await dependencies._run_in_pool(_suicide)

    # The next request works, on a rebuilt pool.
    assert await dependencies._run_in_pool(_echo, 21) == 42
    assert dependencies._POOL is not pool_before
    assert dependencies.pool_health()["restarts"] == before + 1


@pytest.mark.asyncio
async def test_the_task_that_broke_the_pool_is_not_retried() -> None:
    """Retrying it would kill the replacement pool too."""
    before = dependencies.pool_health()["restarts"]
    with pytest.raises(Exception):
        await dependencies._run_in_pool(_suicide)
    # Exactly one rebuild: the suicide was not resubmitted to the new pool.
    assert dependencies.pool_health()["restarts"] == before + 1


@pytest.mark.asyncio
async def test_normal_work_resumes_after_a_worker_dies() -> None:
    """The real scenario: one task kills a worker, everything after it works."""
    with pytest.raises(Exception):
        await dependencies._run_in_pool(_suicide)
    before = dependencies.pool_health()["restarts"]

    results = await asyncio.gather(*(dependencies._run_in_pool(_echo, n) for n in range(6)))
    assert results == [0, 2, 4, 6, 8, 10]
    # The pool that was handed back is healthy — no further rebuilds.
    assert dependencies.pool_health()["restarts"] == before


def _slow_echo(value: int) -> int:
    import time
    time.sleep(0.4)
    return value * 2


@pytest.mark.asyncio
async def test_requests_in_flight_when_a_worker_dies() -> None:
    """The case that matters: innocent work is already pending when one task
    takes a worker down. Whoever handles the failure first must not leave the
    pool broken, and the killer must not be resubmitted onto the replacement."""
    before = dependencies.pool_health()["restarts"]

    results = await asyncio.gather(
        dependencies._run_in_pool(_slow_echo, 1),
        dependencies._run_in_pool(_slow_echo, 2),
        dependencies._run_in_pool(_suicide),
        dependencies._run_in_pool(_slow_echo, 3),
        return_exceptions=True,
    )
    # The suicide itself never returns a value.
    assert isinstance(results[2], BrokenProcessPool)

    # Whatever the interleaving was, the backend is usable again immediately —
    # no request had to arrive first to "unstick" it.
    assert await dependencies._run_in_pool(_echo, 7) == 14
    restarts = dependencies.pool_health()["restarts"] - before
    # One dead worker. A replacement that got the killer resubmitted onto it
    # would show up here as a second (or third) rebuild.
    assert restarts == 1, f"expected a single rebuild, got {restarts}"


@pytest.mark.asyncio
async def test_work_rejected_by_an_already_broken_pool_is_retried() -> None:
    """The other branch: submit() refuses outright, so the work never ran and
    retrying it on the replacement is safe — and is what spares the caller a
    failure it did nothing to cause."""
    dead = dependencies._new_pool()
    with pytest.raises(BrokenProcessPool):
        dead.submit(_suicide).result()      # break it outside the helper
    dependencies._POOL = dead
    before = dependencies.pool_health()["restarts"]

    # No exception reaches the caller: the submit was rejected, so the task is
    # resubmitted to the pool that replaces it.
    assert await dependencies._run_in_pool(_echo, 4) == 8
    assert dependencies._POOL is not dead
    assert dependencies.pool_health()["restarts"] == before + 1
