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
#: Rows of positions read, and quantised, in float64 at a time.
_POS_CHUNK = 1_000_000
#: Rows of the colour column read, or scanned for its range, at a time: its
#: float64 copy is whole, so the temporaries are kept smaller.
_STAT_CHUNK = 262_144
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


def quantise64(values: np.ndarray, lo: float, hi: float) -> np.ndarray:
    """:func:`quantise` in float64 (positions and colours, a chunk at a time).
    A range wider than the largest double (-1.7e308 to 1.7e308) is worked on
    values scaled by its largest magnitude, as hi - lo would be infinite."""
    lo, hi = float(lo), float(hi)
    scale = 1.0
    if hi > lo and not math.isfinite(hi - lo):
        scale = max(abs(lo), abs(hi))
        lo, hi = lo / scale, hi / scale
    span = hi - lo if hi > lo else 1.0
    q = np.array(values, dtype=np.float64)            # a copy, worked in place
    with np.errstate(invalid="ignore", over="ignore"):
        if scale != 1.0:
            q /= scale
        q -= lo
        q *= _Q / span
        np.rint(q, out=q)
        np.clip(q, 0, _Q, out=q)
    q[~np.isfinite(q)] = 0
    return q.astype(np.uint16)


def read_offsets(column, rows: np.ndarray,
                 finite: np.ndarray | None = None) -> tuple[np.ndarray, float]:
    """``column[rows]`` as float32 offsets from a reference value of the
    column (read in float64), and that reference. ``finite`` (bool, one per
    row), when given, is cleared where the value read is not finite or beyond
    float32's range (3.4e38, as the remote point session and the page draw
    in float32): judged on the float64 value, not on the offset, which may
    overflow float32 even for values within it (see :func:`sample_points`). The reference is the first row finite here and
    on the axes read before (``finite``), so that it is usually a row the
    sample keeps.

    Kept in float32 so that 10 M rows stay at 40 MB an axis, yet without the
    float32 cast of the values themselves, which merged distinct points far
    from 0: 1e9, 1e9 + 1 and 1e9 + 2 are one float32 value, so equal bounds and
    codes [0, 0, 0]. The difference is taken in float64 a chunk at a time;
    with the reference among (or near) the rows kept, every offset is a few
    spans of the column at most and float32 keeps it to about 6e-8 of a span
    per span, far below one 16-bit step (1.5e-5 of it): :func:`offsets_hold`
    checks that. Non-finite values stay non-finite."""
    out = np.empty(len(rows), dtype=np.float32)
    ref = None
    for start in range(0, len(rows), _POS_CHUNK):
        # A read by an index array is a copy of its own: worked in place.
        v = np.asarray(column[rows[start:start + _POS_CHUNK]], dtype=np.float64)
        # Beyond float32's range is no position: the page and the remote
        # point session (float32 points, a model matrix from the bounds)
        # cannot draw it, so such a row is dropped as a non-finite one is.
        with np.errstate(invalid="ignore"):
            ok = np.abs(v) <= _F32_MAX
        if finite is not None:
            ok &= finite[start:start + _POS_CHUNK]
        if ref is None:
            first = int(np.argmax(ok))
            if ok.size and ok[first]:
                ref = float(v[first])
        if finite is not None:
            finite[start:start + _POS_CHUNK] = ok
        del ok
        with np.errstate(invalid="ignore", over="ignore"):
            v -= ref or 0.0
            out[start:start + _POS_CHUNK] = v
    return out, (ref or 0.0)


#: The largest position kept (float32's largest value).
_F32_MAX = float(np.finfo(np.float32).max)
#: Offsets up to this many spans from their reference keep 16 x 6e-8 of the
#: span, under a tenth of a 16-bit step.
_REF_SPANS = 16.0
#: Below this span float32 offsets are subnormal or near it (resolution lost).
_MIN_SPAN = 1e-30


def offsets_hold(lo_off: float, hi_off: float) -> bool:
    """Whether float32 offsets with this range kept the 16-bit precision: all
    finite (none overflowed float32: a span beyond 3.4e38), the reference no
    farther than ``_REF_SPANS`` spans from them (it may be a row dropped for
    another axis's NaN, outside the rows kept), and a span that is zero or
    above ``_MIN_SPAN``."""
    if not (math.isfinite(lo_off) and math.isfinite(hi_off)):
        return False
    span = hi_off - lo_off
    if 0.0 < span < _MIN_SPAN:
        return False
    return max(abs(lo_off), abs(hi_off)) <= _REF_SPANS * span or (lo_off == hi_off == 0.0)


def read_normalised(column, rows: np.ndarray) -> tuple[np.ndarray, float, float]:
    """``column[rows]`` (every one finite) as float32 fractions of their
    range, 0 at the minimum and 1 at the maximum, and that range in float64.
    Two reads: the fallback of :func:`sample_points` for an axis whose
    offsets would not keep their precision or overflow float32. The values
    are scaled by their largest magnitude first, so nothing overflows even
    for a span beyond the largest double."""
    lo, hi = math.inf, -math.inf
    for start in range(0, len(rows), _POS_CHUNK):
        v = np.asarray(column[rows[start:start + _POS_CHUNK]], dtype=np.float64)
        if v.size:
            lo, hi = min(lo, float(v.min())), max(hi, float(v.max()))
    out = np.zeros(len(rows), dtype=np.float32)
    if not hi > lo:
        return out, lo, hi
    s = max(abs(lo), abs(hi))
    span = hi / s - lo / s
    for start in range(0, len(rows), _POS_CHUNK):
        v = np.asarray(column[rows[start:start + _POS_CHUNK]], dtype=np.float64)
        v /= s
        v -= lo / s
        v /= span
        np.clip(v, 0.0, 1.0, out=v)
        out[start:start + _POS_CHUNK] = v
    return out, lo, hi


def _quantise_chunked(values: np.ndarray, lo: float, hi: float) -> np.ndarray:
    """:func:`quantise64` a chunk at a time (one float64 temporary of a chunk)."""
    out = np.empty(len(values), dtype=np.uint16)
    for start in range(0, len(values), _STAT_CHUNK):
        out[start:start + _STAT_CHUNK] = quantise64(values[start:start + _STAT_CHUNK], lo, hi)
    return out


def _colour_stats(values: np.ndarray) -> tuple[float, float, float]:
    """(min, max) of the finite values ((0, 1) when none) and the smallest
    positive one (inf when none), a chunk at a time (no temporary the size of
    the column)."""
    lo, hi, pos = math.inf, -math.inf, math.inf
    for start in range(0, len(values), _STAT_CHUNK):
        v = values[start:start + _STAT_CHUNK]
        ok = np.isfinite(v)
        if ok.any():
            f = v[ok]
            lo, hi = min(lo, float(f.min())), max(hi, float(f.max()))
            p = f[f > 0]
            if p.size:
                pos = min(pos, float(p.min()))
    if not (math.isfinite(lo) and math.isfinite(hi)):
        lo, hi = 0.0, 1.0
    return lo, hi, pos


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
    # Positions as float32 offsets from a value of their own axis (see
    # read_offsets): 10 M rows stay at 120 MB, and a coordinate far from 0
    # keeps its separations. Bounds and codes are then worked in float64.
    xyz = np.empty((len(rows), 3), dtype=np.float32)
    ref = np.zeros(3)
    placed = np.ones(len(rows), dtype=bool)        # finite in float64 on every axis
    for i, name in enumerate(axes):
        xyz[:, i], ref[i] = read_offsets(table.columns[name], rows, placed)
    if not placed.all():
        rows, xyz = rows[placed], xyz[placed]
    if not len(rows):
        raise ValueError("No row has a finite position.")
    # The bounds are ref + the float32 offsets of the extreme rows: within
    # float32 rounding of the true minimum and maximum (6e-8 of the span when
    # the offsets are trusted below), the scale the codes are measured on.
    bounds = [0.0] * 6
    lo_off, hi_off = np.zeros(3), np.zeros(3)
    for i, name in enumerate(axes):
        with np.errstate(invalid="ignore"):
            lo_off[i], hi_off[i] = float(xyz[:, i].min()), float(xyz[:, i].max())
        if offsets_hold(lo_off[i], hi_off[i]):
            bounds[2 * i:2 * i + 2] = [float(ref[i] + lo_off[i]), float(ref[i] + hi_off[i])]
            continue
        # The offsets cannot be trusted (see offsets_hold): this axis is read
        # again as fractions of its range (rare).
        xyz[:, i], lo, hi = read_normalised(table.columns[name], rows)
        lo_off[i], hi_off[i] = 0.0, 1.0
        bounds[2 * i:2 * i + 2] = [lo, hi]
    order = prefix_order(len(rows))
    xyz = xyz[order]          # the columns are read in file order, then reordered
    positions = np.empty((len(rows), 3), dtype=np.uint16)
    for start in range(0, len(rows), _POS_CHUNK):
        part = xyz[start:start + _POS_CHUNK]
        for i in range(3):
            positions[start:start + _POS_CHUNK, i] = quantise64(part[:, i], lo_off[i], hi_off[i])
    del xyz
    out = PointSample(positions, None, bounds, axes, int(table.num_rows),
                      table.num_rows / max(1, len(rows)), names=list(table.columns))
    if colour_field is not None:
        # Read once, in float64 (a float32 copy merged 1e9 and 1e9 + 1 into
        # one colour), in file order; the 16-bit codes are put in the sample's
        # order, not the values (2 bytes a row moved instead of 8).
        column = table.columns[colour_field]
        values = np.empty(len(rows), dtype=np.float64)
        for start in range(0, len(rows), _STAT_CHUNK):
            values[start:start + _STAT_CHUNK] = column[rows[start:start + _STAT_CHUNK]]
        dlo, dhi, smallest_positive = _colour_stats(values)
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
                cmin = smallest_positive if math.isfinite(smallest_positive) else 1.0
            if not cmax > cmin:
                cmax = cmin * 10.0
            with np.errstate(divide="ignore", invalid="ignore"):
                np.maximum(values, cmin, out=values)
                np.log10(values, out=values)
            codes = _quantise_chunked(values, math.log10(cmin), math.log10(cmax))
        else:
            if not cmax > cmin:
                cmax = cmin + 1.0
            codes = _quantise_chunked(values, cmin, cmax)
        del values
        out.colour = codes[order]
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
