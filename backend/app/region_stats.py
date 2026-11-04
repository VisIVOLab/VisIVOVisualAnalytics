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
  most ``_IN_MEMORY_VALUES`` of them. Otherwise a random sample drawn in the
  first pass brackets each rank, and one more pass counts the values below the
  bracket and keeps those inside it, which hold the exact value (issue #99);
  should a bracket miss its rank, a histogram is narrowed over further passes
  until the value of each rank is isolated. Those passes read
  a copy of the region's valid values, written to local disk during the first
  pass (``_Spill``): on a shared filesystem (BeeGFS on a cluster) each pass
  over the file reads it again, and the copy is read from the node's own disk
  and cache. Without room for it, or with a temporary directory held in memory
  (a tmpfs ``/tmp``, common on clusters), the passes read the file.

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
import os
import shutil
import tempfile
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
#: Free space left on the spill directory beyond the copy of the region.
_SPILL_MARGIN_BYTES = 1 << 30
#: Values sampled at random from the region during the first pass, to bracket
#: the ranks of the median and of the MAD (32 MiB as float64).
_SAMPLE_VALUES = 1 << 22
#: Half-width of a bracket, in standard deviations of the sample's rank
#: estimate: at 8 the rank falls outside it about once in 10^15.
_BRACKET_SIGMAS = 8.0
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
            read.bound_pixels = width * (y_max - y_min + 1)
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
    read.bound_pixels = 0       # the clipped bounding box, known once read
    return read


def _memory_backed(path: str, mountinfo: str | None = None) -> bool:
    """Whether ``path`` is on a filesystem held in memory (tmpfs, ramfs): on
    many clusters and containers ``/tmp`` is, and what is written there counts
    against the job's memory. Read from ``/proc/self/mountinfo`` (Linux; else
    False), or from ``mountinfo`` when given."""
    if mountinfo is None:
        try:
            with open("/proc/self/mountinfo") as fh:
                mountinfo = fh.read()
        except OSError:
            return False
    mounts = mountinfo.splitlines()
    target = os.path.realpath(path)
    best, fstype = "", ""
    for line in mounts:
        fields = line.split()
        if " - " not in line or len(fields) < 5:
            continue
        point = fields[4].replace("\\040", " ")
        inside = target == point or target.startswith(point.rstrip("/") + "/")
        if inside and len(point) >= len(best):
            best, fstype = point, line.split(" - ", 1)[1].split()[0]
    return fstype in ("tmpfs", "ramfs")


def _spill_dir() -> str | None:
    """Where the copy goes: ``VISIVO_REGION_SPILL_DIR`` when set (an explicit
    choice, trusted as it is); otherwise the system's temporary directory,
    which honours a per-job ``$TMPDIR``, unless it is held in memory. None:
    no copy, the passes read the file."""
    chosen = os.environ.get("VISIVO_REGION_SPILL_DIR")
    if chosen:
        return chosen
    default = tempfile.gettempdir()
    return None if _memory_backed(default) else default


class _Spill:
    """A copy of the region's valid values on local disk, written during the
    first pass and read by the histogram passes in its place. It holds float32
    while every value is one exactly (float32 images, the common case: half
    the space), float64 otherwise; the values read back are those written, so
    every pass sees the same values the file would give."""

    def __init__(self, first: list[np.ndarray], bound_pixels: int) -> None:
        exact = all(np.array_equal(v.astype(np.float32), v) for v in first)
        self.dtype = np.dtype(np.float32 if exact else np.float64)
        directory = _spill_dir()
        if directory is None:
            raise OSError("no disk-backed temporary directory for the copy")
        need = bound_pixels * self.dtype.itemsize + _SPILL_MARGIN_BYTES
        if shutil.disk_usage(directory).free < need:
            raise OSError(f"{directory} has less than {need} bytes free")
        # Deleted on close, whatever ends the computation.
        self._file = tempfile.TemporaryFile(dir=directory, prefix="visivo-region-")
        try:
            for values in first:
                # A copy missing values gives wrong ranks: refused, and the
                # statistics read the source again.
                if not self.write(values):
                    raise OSError("the copy of the region's values could not be written")
        except BaseException:
            self._file.close()
            raise

    def write(self, values: np.ndarray) -> bool:
        """Append ``values`` (finite); False when they do not fit the copy's
        type or the disk (filled meanwhile by other requests or jobs: the room
        was checked once), and the copy is then of no use."""
        out = values.astype(self.dtype)
        if self.dtype == np.float32 and not np.array_equal(out, values):
            return False
        try:
            out.tofile(self._file)
        except OSError:
            return False
        return True

    def read(self) -> Iterator[np.ndarray]:
        self._file.flush()
        self._file.seek(0)
        while True:
            values = np.fromfile(self._file, dtype=self.dtype, count=_BLOCK_PIXELS)
            if not values.size:
                return
            yield values.astype(np.float64)

    def close(self) -> None:
        self._file.close()


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
    if cmin == cmax:
        return [cmin] * len(ranks)          # the candidates are all one value
    if not width > 0:
        # The bounds are a few float steps apart and round to one bin edge:
        # few distinct values remain, so they are counted exactly.
        return _ranks_of_distinct(read, ranks, transform, kept_bins, below)
    cum = np.cumsum(hist)
    groups: dict[int, list[int]] = {}
    for r in ranks:
        groups.setdefault(int(np.searchsorted(cum, r - below, side="right")), []).append(r)
    out: list[float] = []
    for k in sorted(groups):
        out += _select(read, groups[k], transform, kept_bins + [(a, width, k)],
                       below + (int(cum[k - 1]) if k else 0), a + k * width, a + (k + 1) * width)
    return out


def _ranks_of_distinct(read, ranks: list[int], transform,
                       kept_bins: list[tuple[float, float, int]], below: int) -> list[float]:
    """The values at ``ranks`` among the candidates, counting each distinct
    value: for candidates whose range a histogram can no longer split."""
    counts: dict[float, int] = {}
    for values in read():
        v = transform(values[np.isfinite(values)])
        for fa, fw, fk in kept_bins:
            v = v[_bin(v, fa, fw) == fk]
        for value, n in zip(*np.unique(v, return_counts=True)):
            counts[float(value)] = counts.get(float(value), 0) + int(n)
    keys = sorted(counts)
    cum = np.cumsum([counts[k] for k in keys])
    return [keys[int(np.searchsorted(cum, r - below, side="right"))] for r in ranks]


def _interval(sample: np.ndarray, ranks: list[int], n: int) -> tuple[float, float]:
    """An interval of values that holds the given ranks of ``n`` values with
    near certainty, from a sorted random ``sample`` of them: the sample's
    quantiles a few standard deviations either side of each rank's. An
    interval that misses only costs the slower passes."""
    m = sample.size
    lo, hi = math.inf, -math.inf
    for r in ranks:
        p = (r + 0.5) / n
        centre = p * m
        half = _BRACKET_SIGMAS * math.sqrt(m * p * (1.0 - p)) + 2.0
        i, j = math.floor(centre - half), math.ceil(centre + half)
        lo = min(lo, float(sample[i]) if i >= 0 else -math.inf)
        hi = max(hi, float(sample[j]) if j < m else math.inf)
    return lo, hi


def _bracketed(passes: Callable[[], Iterator[np.ndarray]], ranks: list[int], transform,
               lo: float, hi: float) -> list[float] | None:
    """The values of ``ranks`` among ``transform(v)`` of the finite values, in
    one pass: those below ``lo`` are counted, those in ``[lo, hi]`` kept. None
    when a rank is not in the interval, or the interval holds more values than
    memory takes (unless they are all one value)."""
    below = inside = 0
    kept: list[np.ndarray] | None = []
    for values in passes():
        v = transform(values[np.isfinite(values)])
        below += int(np.count_nonzero(v < lo))
        within = v[(v >= lo) & (v <= hi)]
        inside += within.size
        if kept is not None:
            if inside <= _IN_MEMORY_VALUES:
                kept.append(within)
            else:
                kept = None
        if kept is None and lo != hi:
            return None             # more than memory takes: no use reading on
    if any(not below <= r < below + inside for r in ranks):
        return None
    if kept is None:
        return [lo] * len(ranks) if lo == hi else None
    candidates = np.concatenate(kept) if kept else np.empty(0)
    local = [r - below for r in ranks]
    part = np.partition(candidates, local)
    return [float(part[r]) for r in local]


def _rank_values(passes, ranks: list[int], transform, sample: np.ndarray, n: int,
                 lo: float, hi: float) -> list[float]:
    """The values of ``ranks`` among ``transform(v)``, all within ``[lo, hi]``:
    in one pass when the sample brackets them, else by histogram passes."""
    a, b = _interval(np.sort(transform(sample)), ranks, n)
    # An interval open on both sides (no sample) holds every value: straight
    # to the histograms.
    found = _bracketed(passes, ranks, transform, a, b) if a > -math.inf or b < math.inf else None
    return found if found is not None else _ranks(passes, ranks, transform, lo, hi)


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
    kept: list[np.ndarray] = []
    # Past _IN_MEMORY_VALUES the valid values go to a copy on local disk, for
    # the histogram passes; None once there is none (no room, or a value the
    # copy's type cannot hold), and those passes then read the file.
    spill: _Spill | None = None
    spilling = True
    # A random sample of the valid values, to bracket the ranks (_interval);
    # drawn with a fixed seed, so a region gives the same passes every time.
    rng = np.random.default_rng(0)
    sample: list[np.ndarray] = []
    try:
        for values in read():
            total += values.size
            finite = values[np.isfinite(values)]
            blank += values.size - finite.size
            if not finite.size:
                continue
            k = finite.size
            b_mean = float(finite.mean())
            b_m2 = float(((finite - b_mean) ** 2).sum())
            delta = b_mean - mean
            mean += delta * k / (n + k)
            m2 += b_m2 + delta * delta * n * k / (n + k)
            n += k
            total_sum += float(finite.sum())
            vmin, vmax = min(vmin, float(finite.min())), max(vmax, float(finite.max()))
            # Each value drawn with probability _SAMPLE_VALUES / (the region's
            # pixels), the number of draws rounded at random.
            rate = k * _SAMPLE_VALUES / max(read.bound_pixels, 1)
            draws = int(rate) + (rng.random() < rate - int(rate))
            if draws:
                sample.append(finite[rng.integers(0, k, min(draws, k))])
            if n <= _IN_MEMORY_VALUES:
                kept.append(finite)
                continue
            if spilling and spill is None:
                try:
                    spill = _Spill(kept + [finite], read.bound_pixels)
                except OSError:
                    spilling = False
                kept = []
            elif spill is not None and not spill.write(finite):
                spill.close()
                spill, spilling = None, False
        result = {"total_count": total, "valid_count": n, "blank_count": blank}
        if n == 0:
            return result
        if n <= _IN_MEMORY_VALUES:
            values = np.concatenate(kept)
            median = _median(values)
            mad = _median(np.abs(values - median))
        else:
            passes = spill.read if spill is not None else read
            ranks = [n // 2] if n % 2 else [n // 2 - 1, n // 2]
            drawn = np.concatenate(sample) if sample else np.empty(0)
            median = float(np.mean(_rank_values(passes, ranks, lambda v: v, drawn, n,
                                                vmin, vmax)))
            spread = max(vmax - median, median - vmin)
            mad = float(np.mean(_rank_values(passes, ranks, lambda v: np.abs(v - median),
                                             drawn, n, 0.0, spread)))
    finally:
        if spill is not None:
            spill.close()
    result.update({
        "min": vmin, "max": vmax, "sum": total_sum, "mean": mean,
        "stddev": math.sqrt(max(0.0, m2 / n)),
        "median": median, "mad_sigma": _MAD_TO_SIGMA * mad,
    })
    return result
