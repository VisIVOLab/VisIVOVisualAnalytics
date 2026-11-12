"""A table of points sampled for a headset (issue #190, VR epic #166).

The VR page draws a point table on the headset's own GPU, so moving it never
waits for the network; the table may hold far more rows than a headset can
draw at 72-120 Hz in stereo, so the page asks for an even sample within a
budget, and the backend's point render session refines the view with every
row when it is still (render_session/_stereo_points.py).

The sample is the point session's own rule (render_session/points.py,
``_sample_rows``): rows evenly spaced over the table, so a snapshot stored
region by region is sampled everywhere, not cut. Positions are quantised to
16 bits over the sample's bounding box (a 50 Mpc box in steps of 0.8 kpc), the
colour column to 16 bits over its range (linear, or log10 for a log scale),
so a row costs 8 bytes on the wire and in the GPU's buffer instead of 16.

The rows are sent in a low-discrepancy order (``prefix_order``: van der
Corput, the bit-reversed index), so a prefix of the answer is spread over the
whole sample: a prefix of length 2^k is exactly every (n / 2^k)-th row, and
any prefix of length m has no gap between consecutive chosen rows wider than
2 n / m (tested). The page draws such a prefix while a frame would otherwise
be late.

Wire format (``encode``): little-endian ``uint16``, the positions interleaved
(x, y, z per row), then one colour code per row when a colour column is
given. Code 0 of the colour is also what a non-finite value gets; the page
draws it with the map's lowest colour, as the desktop does.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

#: The most rows one request may ask for (the desktop browser's budget; a
#: Quest's is far lower, the page chooses). The peak working memory of a
#: request of 10 M rows with a colour column is measured in
#: tests/test_vr_layers.py (about 0.4 GB; it was 0.88 GB with float64
#: temporaries).
MAX_SAMPLE = 10_000_000
#: Rows read at a time when a filter runs over the whole table.
_CHUNK = 4_000_000
#: A positive colour column spanning more decades than this is put on a log
#: scale when the request leaves the scale to the backend (color_log null).
AUTO_LOG_DECADES = 3.0
_Q = 65535


@dataclass
class PointSample:
    positions: np.ndarray            # (n, 3) uint16
    colour: np.ndarray | None        # (n,) uint16, or None
    bounds: list[float]              # xmin, xmax, ymin, ymax, zmin, zmax (of the sample)
    axes: tuple[str, str, str]
    total_rows: int
    step: float                      # rows of the table per row of the sample
    colour_field: str | None = None
    colour_range: tuple[float, float] | None = None   # what codes 0 and 65535 stand for
    colour_log: bool = False
    data_range: tuple[float, float] | None = None     # of the colour column, finite rows
    names: list[str] = field(default_factory=list)

    def encode(self) -> bytes:
        parts = [np.ascontiguousarray(self.positions, dtype="<u2").tobytes()]
        if self.colour is not None:
            parts.append(np.ascontiguousarray(self.colour, dtype="<u2").tobytes())
        return b"".join(parts)


def even_rows(num_rows: int, limit: int) -> np.ndarray:
    """Every row, or ``limit`` rows evenly spaced over the table (the point
    render session's sample, render_session/points.py)."""
    if num_rows <= limit:
        return np.arange(num_rows, dtype=np.int64)
    return np.linspace(0, num_rows - 1, limit).astype(np.int64)


def prefix_order(n: int) -> np.ndarray:
    """A permutation of ``range(n)`` in van der Corput order (bit-reversed
    indices, those >= n skipped), int32, built a chunk at a time: a prefix of
    length 2^k is every (2^bits / 2^k)-th index, and any prefix of length m
    leaves no gap wider than 2 n / m between consecutive chosen indices."""
    if n <= 1:
        return np.arange(n, dtype=np.int32)
    bits = int(n - 1).bit_length()
    out = []
    for start in range(0, 1 << bits, _CHUNK):
        i = np.arange(start, min(1 << bits, start + _CHUNK), dtype=np.int32)
        r = np.zeros_like(i)
        for b in range(bits):
            r |= ((i >> b) & 1) << (bits - 1 - b)
        out.append(r[r < n])
    return np.concatenate(out)


def filtered_rows(table, filters: list[dict]) -> np.ndarray:
    """Every row passing ``filters``, read a chunk at a time (the filter runs
    before the sample, so a filter keeps the budget's rows, not a share of it)."""
    from .point_table import filter_mask   # noqa: PLC0415
    kept = []
    for start in range(0, table.num_rows, _CHUNK):
        rows = np.arange(start, min(table.num_rows, start + _CHUNK), dtype=np.int64)
        kept.append(rows[filter_mask(table.columns, filters, rows)])
    return np.concatenate(kept) if kept else np.zeros(0, np.int64)


def quantise(values: np.ndarray, lo: float, hi: float) -> np.ndarray:
    """``values`` as 16-bit codes over [lo, hi] (non-finite: 0), computed in
    float32 in place: one temporary the size of ``values`` in float32."""
    span = float(hi - lo) if hi > lo else 1.0
    q = np.array(values, dtype=np.float32)            # a copy, worked in place
    with np.errstate(invalid="ignore", over="ignore"):
        q -= np.float32(lo)
        q *= np.float32(_Q / span)
        np.rint(q, out=q)
        np.clip(q, 0, _Q, out=q)
    q[~np.isfinite(q)] = 0
    return q.astype(np.uint16)


def dequantise(codes: np.ndarray, lo: float, hi: float) -> np.ndarray:
    """Inverse of :func:`quantise` (tests, and the page's own rule)."""
    return lo + (np.asarray(codes, dtype=np.float64) / _Q) * (hi - lo)


def sample_points(table, limit: int, axes: tuple[str, str, str] | None = None,
                  colour_field: str | None = None, colour_log: bool | None = False,
                  colour_min: float | None = None, colour_max: float | None = None,
                  filters: list[dict] | None = None) -> PointSample:
    """An even sample of at most ``limit`` placed rows of ``table`` (a
    :class:`app.point_table.PointTable`)."""
    limit = max(1, min(int(limit), MAX_SAMPLE))
    axes = tuple(axes) if axes else tuple(table.axes)
    for name in axes:
        if name not in table.columns:
            raise ValueError(f"Unknown position field: {name!r}")
    if colour_field is not None and colour_field not in table.columns:
        raise ValueError(f"Unknown colour field: {colour_field!r}")
    if filters:
        candidates = filtered_rows(table, filters)
        rows = candidates[even_rows(len(candidates), limit)]
    else:
        rows = even_rows(table.num_rows, limit)
    # float32 positions: 16-bit codes need no more, and 10 M rows stay at 120 MB.
    xyz = np.empty((len(rows), 3), dtype=np.float32)
    for i, name in enumerate(axes):
        xyz[:, i] = np.asarray(table.columns[name][rows], dtype=np.float32)
    placed = np.isfinite(xyz).all(axis=1)
    if not placed.all():
        rows, xyz = rows[placed], xyz[placed]
    if not len(rows):
        raise ValueError("No row has a finite position.")
    order = prefix_order(len(rows))
    xyz = xyz[order]          # the columns are read in file order, then reordered
    lo, hi = xyz.min(axis=0), xyz.max(axis=0)
    bounds = [float(v) for pair in zip(lo, hi) for v in pair]
    positions = np.stack([quantise(xyz[:, i], lo[i], hi[i]) for i in range(3)], axis=1)
    out = PointSample(positions, None, bounds, axes, int(table.num_rows),
                      table.num_rows / max(1, len(rows)), names=list(table.columns))
    if colour_field is not None:
        # Read once, in float32, in the sample's order; no copy of the finite ones.
        values = np.asarray(table.columns[colour_field][rows], dtype=np.float32)[order]
        with np.errstate(invalid="ignore"):
            any_finite = bool(np.isfinite(values).any())
            dlo = float(np.nanmin(values)) if any_finite else 0.0
            dhi = float(np.nanmax(values)) if any_finite else 1.0
        if not (math.isfinite(dlo) and math.isfinite(dhi)):
            fin = values[np.isfinite(values)]
            dlo, dhi = (float(fin.min()), float(fin.max())) if fin.size else (0.0, 1.0)
        out.data_range = (dlo, dhi)
        cmin = dlo if colour_min is None else float(colour_min)
        cmax = dhi if colour_max is None else float(colour_max)
        if colour_log is None:
            # The backend chooses: a positive column over more than
            # AUTO_LOG_DECADES decades (a mass, a density) on a log scale.
            colour_log = bool(dlo > 0 and dhi > 0
                              and math.log10(dhi / dlo) > AUTO_LOG_DECADES)
        log = bool(colour_log)
        if log:
            # As the ray-traced render does (routers/particle_render.py): a
            # log scale starts at the smallest positive value.
            if colour_min is None or cmin <= 0:
                with np.errstate(invalid="ignore"):
                    pos = np.where(values > 0, values, np.inf).min() if values.size else np.inf
                cmin = float(pos) if math.isfinite(float(pos)) else 1.0
            if not cmax > cmin:
                cmax = cmin * 10.0
            with np.errstate(divide="ignore", invalid="ignore"):
                np.maximum(values, np.float32(cmin), out=values)
                np.log10(values, out=values)
            out.colour = quantise(values, math.log10(cmin), math.log10(cmax))
        else:
            if not cmax > cmin:
                cmax = cmin + 1.0
            out.colour = quantise(values, cmin, cmax)
        out.colour_field, out.colour_range, out.colour_log = colour_field, (cmin, cmax), log
    return out


def headers(s: PointSample) -> dict[str, str]:
    """The sample's ``X-Visivo-*`` headers (docs/backend-api/vr-layers.md)."""
    h = {
        "X-Visivo-Count": str(len(s.positions)),
        "X-Visivo-Total-Rows": str(s.total_rows),
        "X-Visivo-Step": repr(float(s.step)),
        "X-Visivo-Bounds": ",".join(repr(v) for v in s.bounds),
        "X-Visivo-Axes": ",".join(s.axes),
        "X-Visivo-Scalar-Type": "uint16",
        "X-Visivo-Byte-Order": "little",
    }
    if s.colour_field is not None:
        h["X-Visivo-Color-Field"] = s.colour_field
        h["X-Visivo-Color-Range"] = ",".join(repr(float(v)) for v in s.colour_range)
        h["X-Visivo-Color-Log"] = "1" if s.colour_log else "0"
        if s.data_range is not None:
            h["X-Visivo-Data-Range"] = ",".join(repr(float(v)) for v in s.data_range)
    return h
