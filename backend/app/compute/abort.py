"""Cooperative abort of a computation running in a worker process.

The pool's workers are separate processes: the event loop cannot interrupt
one, and killing it would take the pool down with every other task in it.
A long computation therefore checks, between the pieces of work it streams
(a spectral slab of a moment map, a block of channels of a PV diagram), for
a *stop file* the task registry touches when the client aborts the task
(``TaskRegistry.cancel``). The check costs one ``stat``; a slab costs
seconds, so an abort lands within one slab, the partial sums are dropped and
the worker is free again. A computation that has no such step (a cutout,
one read and one write) runs to its end as before.
"""
from __future__ import annotations

import os


class ComputationAborted(Exception):
    """The client aborted the task; the partial result is discarded."""


def check_abort(stop_file: str | None) -> None:
    """Raise :class:`ComputationAborted` when the task's stop file exists.
    ``None`` (no stop file was given) never raises."""
    if stop_file and os.path.exists(stop_file):
        raise ComputationAborted("Aborted by the client.")
