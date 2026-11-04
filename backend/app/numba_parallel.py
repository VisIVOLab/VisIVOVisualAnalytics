"""One numba parallel kernel at a time in a process.

Numba runs ``parallel=True`` kernels on a threading layer; without TBB (or
OpenMP) it is the workqueue layer, which is not thread-safe: two kernels
launched at once from two Python threads (a render task, a GPU view's grid, a
preview, each on its own worker thread) make numba abort the whole process,
not raise. Every call of a parallel kernel holds this lock; each kernel uses
every core anyway, so waiting costs nothing in throughput.
"""
from __future__ import annotations

import threading

LOCK = threading.Lock()
