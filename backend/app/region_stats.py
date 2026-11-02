"""Statistics of a region of an image, or of a channel of a cube, computed on
the file at full resolution (issue #95).

The image viewer used to compute them from the pixels it displays. Below full
resolution those are one source pixel per block (sampled) or the mean of a
block (averaged), and neither gives the image's statistics: a sampled sum is
short by the block's area, an averaged σ by its side. Here the region is read
from the file in blocks of rows, never whole, and every statistic is exact:

* count, blanks, minimum, maximum, sum and the mean, with the variance combined
  block by block (Chan et al.), so that a sum of 10⁹ pixels keeps its digits;
* the median and the MAD, from the values themselves when the region has at
  most ``_IN_MEMORY_VALUES`` of them, otherwise by narrowing a histogram over
  further passes until the value of each rank is isolated.

A region is a canonical image-frame region (``app.regions``) in full-resolution
pixel coordinates, pixel ``(x, y)`` centred on integers (FITS pixel minus one),
and a pixel belongs to it when its centre does: inside a circle, ellipse or
(rotated) box, between the radii of an annulus (both edges included), inside a
polygon by the even-odd rule, or nearest a point. These are the viewer's own
rules (``CubeRegionGeom``), so a region drawn at full resolution counts the same
pixels here as there.
"""
from __future__ import annotations

import math
from typing import Callable, Iterator

import numpy as np

#: Rows of the region read at once are limited to about this many pixels (the
#: block and its mask's float64 temporaries, some 100 MiB).
_BLOCK_PIXELS = 4 * 1024 * 1024
#: The file is opened again after this many bytes read: a memory map keeps the
#: pages it has read mapped (and counted in the process's memory) until closed,
#: which over a whole 16 GiB image is 16 GiB.
_REMAP_BYTES = 512 * 1024 * 1024
#: A region with at most this many valid values keeps them for its median and
#: MAD (as float64, 256 MiB); above it they are found by histogram passes.
_IN_MEMORY_VALUES = 32 * 1024 * 1024
#: Bins of each histogram pass.
_BINS = 1 << 16
#: σ from the median absolute deviation of a Gaussian.
_MAD_TO_SIGMA = 1.4826


class Region:
    """A canonical image-frame region (``app.regions``: 0-based pixels, sizes
    and radii in pixels, ``angle_deg`` counter-clockwise) as a test on pixel
    centres, applied block by block."""

    def __init__(self, spec: dict) -> None:
        self.spec = spec
        self.shape = str(spec.get("shape", "")).lower()
        if self.shape == "polygon":
            self.points = [(float(x), float(y)) for x, y in spec.get("points", ())]
            if len(self.points) < 3:
                raise ValueError("A polygon needs at least three points.")
        elif self.shape in ("circle", "annulus", "ellipse", "box", "point"):
            self.cx, self.cy = (float(v) for v in spec["center"])
        else:
            raise ValueError(f"Unsupported region shape '{self.shape}'.")
        self.angle = math.radians(float(spec.get("angle_deg", 0.0) or 0.0))

    def bounds(self) -> tuple[int, int, int, int]:
        """The pixels to scan, ``(x_min, x_max, y_min, y_max)`` inclusive."""
        s = self.spec
        if self.shape == "polygon":
            xs = [p[0] for p in self.points]
            ys = [p[1] for p in self.points]
            lo_x, hi_x, lo_y, hi_y = min(xs), max(xs), min(ys), max(ys)
        elif self.shape == "point" or (self.shape == "circle" and float(s["radius"]) <= 0):
            return round(self.cx), round(self.cx), round(self.cy), round(self.cy)
        else:
            cos, sin = abs(math.cos(self.angle)), abs(math.sin(self.angle))
            if self.shape == "circle":
                rx = ry = float(s["radius"])
            elif self.shape == "annulus":
                rx = ry = float(max(s["radii"]))
            elif self.shape == "ellipse":
                a, b = (float(v) for v in s["semi_axes"])
                rx = math.sqrt((a * cos) ** 2 + (b * sin) ** 2)
                ry = math.sqrt((a * sin) ** 2 + (b * cos) ** 2)
            else:
                hw, hh = (float(v) / 2.0 for v in s["size"])
                rx, ry = hw * cos + hh * sin, hw * sin + hh * cos
            rx, ry = max(rx, 0.0), max(ry, 0.0)
            lo_x, hi_x, lo_y, hi_y = (self.cx - rx, self.cx + rx, self.cy - ry, self.cy + ry)
        return (math.ceil(lo_x - 1e-9), math.floor(hi_x + 1e-9),
                math.ceil(lo_y - 1e-9), math.floor(hi_y + 1e-9))

    def mask(self, x: np.ndarray, y: np.ndarray) -> np.ndarray:
        """Whether pixels ``(x, y)`` (broadcast arrays of integers) are inside."""
        s = self.spec
        if self.shape == "polygon":
            return self._polygon_mask(x, y)
        if self.shape == "point":
            return (x == round(self.cx)) & (y == round(self.cy))
        dx, dy = x - self.cx, y - self.cy
        if self.shape == "circle":
            r = float(s["radius"])
            if r <= 0:
                return (x == round(self.cx)) & (y == round(self.cy))
            return dx * dx + dy * dy <= r * r
        if self.shape == "annulus":
            r1, r2 = (float(v) for v in s["radii"])
            d2 = dx * dx + dy * dy
            # Inner edge included, as the viewer draws it.
            return (d2 <= r2 * r2) & (d2 >= max(r1, 0.0) ** 2)
        cos, sin = math.cos(self.angle), math.sin(self.angle)
        xr = dx * cos + dy * sin
        yr = -dx * sin + dy * cos
        if self.shape == "ellipse":
            a, b = (float(v) for v in s["semi_axes"])
            return (xr / a) ** 2 + (yr / b) ** 2 <= 1.0
        w, h = (float(v) for v in s["size"])
        return (np.abs(xr) <= w / 2.0) & (np.abs(yr) <= h / 2.0)

    def _polygon_mask(self, x: np.ndarray, y: np.ndarray) -> np.ndarray:
        """Even-odd rule by scanline: on each row, the crossings of the edges,
        then for each pixel the number of crossings to its right. The same
        test as the viewer's pointInPolygon, at a cost that grows with the
        edges per row rather than per pixel."""
        # A grid: x along the last axis, y along the first (as the callers'
        # broadcast rows and columns, or mgrid, give them).
        xb, yb = np.broadcast_arrays(np.atleast_2d(x), np.atleast_2d(y))
        xs, rows = xb[0], yb[:, 0]
        p = np.asarray(self.points, dtype=np.float64)
        xi, yi = p[:, 0], p[:, 1]
        xj, yj = np.roll(xi, 1), np.roll(yi, 1)
        out = np.zeros((rows.size, xs.size), dtype=bool)
        for r, row in enumerate(rows.astype(np.float64)):
            crosses = (yi > row) != (yj > row)
            if not crosses.any():
                continue
            c = (xj[crosses] - xi[crosses]) * (row - yi[crosses]) / (
                yj[crosses] - yi[crosses] + 1e-30) + xi[crosses]
            c.sort()
            right = c.size - np.searchsorted(c, xs, side="right")
            out[r] = (right % 2) == 1
        return out


class _WholePlane:
    """A file compressed as a whole (``.fits.gz``) cannot be read in parts:
    its plane is read once, whole, as the route always did for such files."""

    def __init__(self, path: str, hdu: int) -> None:
        from .fits_dataset import ScientificFitsDataset  # noqa: PLC0415

        dataset = ScientificFitsDataset(path)
        hdul, raw = dataset.open_memmap()
        try:
            data = np.asarray(ScientificFitsDataset.squeeze_to_3d(raw), dtype=np.float64)
        finally:
            hdul.close()
        self.data = data[np.newaxis] if data.ndim == 2 else data
        self.shape = self.data.shape

    def block(self, z0, z1, y0, y1, x0, x1, dtype=np.float64):
        return self.data[z0:z1, y0:y1, x0:x1].astype(dtype, copy=False)

    def close(self) -> None:
        pass


def _value_blocks(path: str, hdu: int, region: Region, channel: int) -> Callable[[], Iterator[np.ndarray]]:
    """A function that reads the region again on each call and yields its
    pixels' values (NaN included) in double precision, block of rows by block
    of rows."""
    from .brick_pyramid import _SourceCube  # noqa: PLC0415

    whole: list = []

    def open_source():
        if whole:
            return whole[0]
        try:
            return _SourceCube(path, hdu, 1 << 30)
        except ValueError as exc:
            if "compressed as a whole" not in str(exc):
                raise
            whole.append(_WholePlane(path, hdu))
            return whole[0]

    def read() -> Iterator[np.ndarray]:
        cube = open_source()
        try:
            nz, ny, nx = cube.shape
            if not 0 <= channel < nz:
                raise ValueError(f"channel {channel} outside 0..{nz - 1}")
            x_min, x_max, y_min, y_max = region.bounds()
            x_min, x_max = max(0, x_min), min(nx - 1, x_max)
            y_min, y_max = max(0, y_min), min(ny - 1, y_max)
            if x_max < x_min or y_max < y_min:
                return
            width = x_max - x_min + 1
            rows = max(1, _BLOCK_PIXELS // width)
            xs = np.arange(x_min, x_max + 1)[np.newaxis, :]
            mapped = 0
            for y0 in range(y_min, y_max + 1, rows):
                y1 = min(y_max + 1, y0 + rows)
                if mapped > _REMAP_BYTES and not whole:
                    cube.close()
                    cube = open_source()
                    mapped = 0
                block = cube.block(channel, channel + 1, y0, y1, x_min, x_max + 1,
                                   dtype=np.float64)[0]
                # Whole rows of the file are paged in, whatever the region's width.
                mapped += (y1 - y0) * nx * 4
                inside = region.mask(xs, np.arange(y0, y1)[:, np.newaxis])
                yield block[inside]
        finally:
            cube.close()
    return read


def _bin(v: np.ndarray, a: float, width: float) -> np.ndarray:
    return np.clip(np.floor((v - a) / width), 0, _BINS - 1).astype(np.int64)


def _ranks(read: Callable[[], Iterator[np.ndarray]], ranks: list[int],
           transform: Callable[[np.ndarray], np.ndarray], lo: float, hi: float) -> list[float]:
    """The values of the given 0-based ``ranks`` among ``transform(v)`` of the
    finite values ``read()`` yields, all within ``[lo, hi]``, in the order of
    ``ranks``. Each pass reads the region again, histograms the candidates and
    keeps the bin holding each rank; ranks in the same bin (the two middle ones
    of an even count, nearly always) share their passes. A candidate is a
    value that fell in every bin kept so far, computed the same way each time,
    so no value is counted twice at a bin's edge. When the candidates fit in
    memory, the ranks are read from them."""
    order = sorted(range(len(ranks)), key=lambda i: ranks[i])
    found = _select(read, [ranks[i] for i in order], transform, [], 0, lo, hi)
    out = [0.0] * len(ranks)
    for i, value in zip(order, found):
        out[i] = value
    return out


def _select(read, ranks: list[int], transform, kept_bins: list[tuple[float, float, int]],
            below: int, a: float, b: float) -> list[float]:
    width = (b - a) / _BINS
    hist = np.zeros(_BINS, dtype=np.int64)
    kept: list[np.ndarray] = []
    count = 0
    cmin, cmax = math.inf, -math.inf
    for values in read():
        v = transform(values[np.isfinite(values)])
        for fa, fw, fk in kept_bins:
            v = v[_bin(v, fa, fw) == fk]
        if v.size:
            cmin, cmax = min(cmin, float(v.min())), max(cmax, float(v.max()))
        count += v.size
        if count <= _IN_MEMORY_VALUES:
            kept.append(v)
        if width > 0:
            hist += np.bincount(_bin(v, a, width), minlength=_BINS)
    if count <= _IN_MEMORY_VALUES:
        candidates = np.concatenate(kept) if kept else np.empty(0)
        local = [r - below for r in ranks]
        part = np.partition(candidates, local)
        return [float(part[r]) for r in local]
    if cmin == cmax or not width > 0:
        return [cmin] * len(ranks)          # the candidates are all one value
    cum = np.cumsum(hist)
    groups: dict[int, list[int]] = {}
    for r in ranks:
        groups.setdefault(int(np.searchsorted(cum, r - below, side="right")), []).append(r)
    out: list[float] = []
    for k in sorted(groups):
        out += _select(read, groups[k], transform, kept_bins + [(a, width, k)],
                       below + (int(cum[k - 1]) if k else 0), a + k * width, a + (k + 1) * width)
    return out


def _median(values: np.ndarray) -> float:
    n = values.size
    if n % 2:
        return float(np.partition(values, n // 2)[n // 2])
    part = np.partition(values, (n // 2 - 1, n // 2))
    return 0.5 * (float(part[n // 2 - 1]) + float(part[n // 2]))


def region_statistics(path: str, region: Region, hdu: int, channel: int = 0) -> dict:
    """Statistics of ``region`` in plane ``channel`` (0 for an image) of HDU
    ``hdu`` of ``path``, at full resolution."""
    read = _value_blocks(path, hdu, region, channel)
    total = blank = n = 0
    mean = m2 = 0.0
    total_sum = 0.0
    vmin, vmax = math.inf, -math.inf
    kept: list[np.ndarray] | None = []
    for values in read():
        total += values.size
        finite = values[np.isfinite(values)]
        blank += values.size - finite.size
        if not finite.size:
            continue
        f64 = finite
        k = f64.size
        b_mean = float(f64.mean())
        b_m2 = float(((f64 - b_mean) ** 2).sum())
        delta = b_mean - mean
        mean += delta * k / (n + k)
        m2 += b_m2 + delta * delta * n * k / (n + k)
        n += k
        total_sum += float(f64.sum())
        vmin, vmax = min(vmin, float(finite.min())), max(vmax, float(finite.max()))
        if kept is not None:
            if n <= _IN_MEMORY_VALUES:
                kept.append(finite)
            else:
                kept = None
    result = {"total_count": total, "valid_count": n, "blank_count": blank}
    if n == 0:
        return result
    if kept is not None:
        values = np.concatenate(kept)
        median = _median(values)
        mad = _median(np.abs(values - median))
    else:
        ranks = [n // 2] if n % 2 else [n // 2 - 1, n // 2]
        median = float(np.mean(_ranks(read, ranks, lambda v: v, vmin, vmax)))
        spread = max(vmax - median, median - vmin)
        mad = float(np.mean(_ranks(read, ranks, lambda v: np.abs(v - median), 0.0, spread)))
    result.update({
        "min": vmin, "max": vmax, "sum": total_sum, "mean": mean,
        "stddev": math.sqrt(max(0.0, m2 / n)),
        "median": median, "mad_sigma": _MAD_TO_SIGMA * mad,
    })
    return result
