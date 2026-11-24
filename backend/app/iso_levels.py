"""The levels of an isosurface request (issue #526).

A request names its levels one of three ways: ``levels`` (an explicit list),
``n_levels`` between ``level_min`` and ``level_max`` (``spacing`` linear or
log), or the single ``threshold`` it has always taken. The answer is the same
whichever was used: the levels' values, ascending, without duplicates, so a
cache key and a legend read the same list.

Pure functions, no I/O; the extraction is ``app.compute.pv_iso``.
"""
from __future__ import annotations

import math
from typing import Literal, Sequence

import numpy as np

#: At most this many surfaces in one request: more nested translucent shells
#: stop reading as separate surfaces (the headset's route, app/isosurface.py,
#: has the same bound).
MAX_LEVELS = 8

Spacing = Literal["linear", "log"]


def spaced_levels(n_levels: int, level_min: float, level_max: float,
                  spacing: Spacing = "linear") -> list[float]:
    """``n_levels`` values from ``level_min`` to ``level_max`` inclusive, evenly
    spaced in the values (``linear``) or in their logarithm (``log``, both ends
    above zero). One level is the middle of the range (geometric for ``log``)."""
    if not 1 <= n_levels <= MAX_LEVELS:
        raise ValueError(f"n_levels must be between 1 and {MAX_LEVELS}, got {n_levels}.")
    lo, hi = float(level_min), float(level_max)
    if not (math.isfinite(lo) and math.isfinite(hi)):
        raise ValueError("level_min and level_max must be finite numbers.")
    if spacing not in ("linear", "log"):
        raise ValueError(f"spacing must be 'linear' or 'log', got {spacing!r}.")
    if hi < lo:
        lo, hi = hi, lo
    if spacing == "log":
        if lo <= 0.0:
            raise ValueError("Log spacing needs level_min and level_max above zero; "
                             f"got {lo:g}..{hi:g}.")
        if n_levels == 1:
            return [math.sqrt(lo * hi)]
        return [float(v) for v in np.geomspace(lo, hi, n_levels)]
    if n_levels == 1:
        return [0.5 * (lo + hi)]
    return [float(v) for v in np.linspace(lo, hi, n_levels)]


def resolve_levels(threshold: float | None = None, levels: Sequence[float] | None = None,
                   n_levels: int | None = None, level_min: float | None = None,
                   level_max: float | None = None, spacing: Spacing = "linear") -> list[float]:
    """The request's levels: ascending, distinct, finite, at most ``MAX_LEVELS``.

    ``levels`` wins over ``n_levels`` (with its range), which wins over
    ``threshold``; a request with none of them is an error. ``threshold`` alone
    is one level, as before."""
    if levels:
        values = [float(v) for v in levels]
    elif n_levels is not None:
        if level_min is None or level_max is None:
            raise ValueError("n_levels needs level_min and level_max.")
        values = spaced_levels(int(n_levels), level_min, level_max, spacing)
    elif threshold is not None:
        values = [float(threshold)]
    else:
        raise ValueError("Give threshold, levels, or n_levels with level_min and level_max.")
    if any(not math.isfinite(v) for v in values):
        raise ValueError("Every level must be a finite number.")
    out = sorted(set(values))
    if len(out) > MAX_LEVELS:
        raise ValueError(f"At most {MAX_LEVELS} levels in one request, got {len(out)}.")
    return out
