"""Box and sphere selections in a periodic simulation volume (issue #448).

A cosmological box is periodic: a halo near one face continues across the
opposite one, so a sphere or a box drawn around it must reach particles on
the other side. With the period ``L`` of a coordinate known (a Gadget
snapshot's ``BoxSize``, or one stated with the selection), distances use the
minimum image (each separation folded into [-L/2, L/2]) and a box's interval
wraps around the boundary. Without a period, plain differences, as before.

Every function works on one chunk of one coordinate column, so callers keep
their bounded-memory loops (the out-of-core matcher, the point sessions, the
DataFrame path).
"""
from __future__ import annotations

import math
from collections.abc import Mapping

import numpy as np


def selection_period(explicit, field: str, periods: Mapping[str, float] | None) -> float | None:
    """The period a selection uses on ``field``: ``explicit`` when the
    selection states one (a positive finite length; 0 turns wrapping off),
    else the dataset's own for that column (``periods``), else None."""
    if explicit is not None:
        value = float(explicit)
        if not math.isfinite(value) or value < 0:
            raise ValueError("A selection's period must be a finite length, or 0 for none.")
        return value or None
    if periods:
        value = periods.get(field)
        if value is not None and math.isfinite(value) and value > 0:
            return float(value)
    return None


def box_inside(values, lo: float, hi: float, period: float | None) -> np.ndarray:
    """``lo <= v <= hi`` for one coordinate; on a periodic axis the interval
    is measured from ``lo`` round the box, so [L - 1, L + 1] holds 0.5 and
    L - 0.5 alike, and an interval at least L long holds every finite value."""
    values = np.asarray(values)
    if period is None:
        return (values >= lo) & (values <= hi)
    width = float(hi) - float(lo)
    shifted = np.mod(np.asarray(values, dtype=np.float64) - float(lo), period)
    if width >= period:
        return np.isfinite(shifted)
    return shifted <= width


def separation(values, origin: float, period: float | None) -> np.ndarray:
    """``v - origin`` in float64, folded to the nearest image on a periodic
    axis (|separation| <= L/2)."""
    delta = np.asarray(values, dtype=np.float64) - float(origin)
    if period is not None:
        delta -= period * np.round(delta / period)
    return delta
