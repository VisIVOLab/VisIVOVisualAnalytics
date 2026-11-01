#!/usr/bin/env python3
"""
image_tile_bench.py - build the averaged pyramid of a 2-D image and time its
tiles against tiles sampled from the file (issue #85).

Measures what the issue's "Done when" asks for, with the backend's own code
(``app.brick_pyramid.build_pyramid`` and ``app.compute.cube.worker_image_tile``,
the function behind ``/v1/image/tile``):

* the build: wall time, the peak of the Python and numpy allocations
  (tracemalloc) and, inside a container, the cgroup's memory limit and peak.
  Finishing under a limit several times smaller than the image is the evidence
  that the build does not hold it;
* tiles: for every level, the time to serve random tiles sampled from the file
  (the path before #85, and still the path while the pyramid builds) and from
  the pyramid, median and 95th percentile (nearest rank).

``--make WIDTHxHEIGHT`` first writes a synthetic image there (unit noise and
Gaussian sources), row block by row block, never holding it.

    PYTHONPATH=backend python3 image_tile_bench.py --image /data/img_16gb.fits \
        --make 65536x65536 --out /work/img_16gb.pyr.h5 --json /work/results.json
"""
from __future__ import annotations

import argparse
import json
import math
import os
import platform
import random
import statistics
import time
import tracemalloc
from pathlib import Path

import numpy as np


def _cgroup_memory() -> dict:
    out = {}
    for name in ("memory.max", "memory.peak"):
        try:
            value = Path("/sys/fs/cgroup", name).read_text().strip()
            out[name] = None if value == "max" else int(value)
        except (OSError, ValueError):
            out[name] = None
    return out


def make_image(path: Path, width: int, height: int, seed: int = 0) -> None:
    """Unit noise with a few hundred Gaussian sources, written in row blocks."""
    from astropy.io import fits

    rng = np.random.default_rng(seed)
    n_src = 400
    sx, sy = rng.uniform(0, width, n_src), rng.uniform(0, height, n_src)
    amp, sig = rng.uniform(3, 30, n_src), rng.uniform(2, 40, n_src)
    header = fits.Header()
    header["SIMPLE"] = True
    header["BITPIX"] = -32
    header["NAXIS"] = 2
    header["NAXIS1"] = width
    header["NAXIS2"] = height
    hdu = fits.StreamingHDU(str(path), header)
    rows = max(1, (256 * 1024 ** 2) // (4 * width))
    xs = np.arange(width, dtype=np.float32)
    for y0 in range(0, height, rows):
        y1 = min(height, y0 + rows)
        block = rng.standard_normal((y1 - y0, width), dtype=np.float32)
        ys = np.arange(y0, y1, dtype=np.float32)[:, None]
        near = np.nonzero((sy + 5 * sig > y0) & (sy - 5 * sig < y1))[0]
        for i in near:
            x0, x1 = int(max(0, sx[i] - 5 * sig[i])), int(min(width, sx[i] + 5 * sig[i] + 1))
            if x1 <= x0:
                continue
            block[:, x0:x1] += amp[i] * np.exp(
                -((xs[x0:x1] - sx[i]) ** 2 + (ys - sy[i]) ** 2) / (2 * sig[i] ** 2))
        hdu.write(block.astype(">f4"))
    hdu.close()


def _pct(values: list[float], q: float) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(q * len(ordered)) - 1)]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--image", required=True)
    ap.add_argument("--make", help="write a synthetic WIDTHxHEIGHT image at --image first")
    ap.add_argument("--out", required=True, help="where to write the pyramid")
    ap.add_argument("--tile", type=int, default=512, help="tile size (the desktop asks for 512)")
    ap.add_argument("--reads", type=int, default=30, help="random tiles per level and path")
    ap.add_argument("--workers", type=int, default=None)
    ap.add_argument("--json", help="also write the results as JSON")
    args = ap.parse_args()

    from app.brick_pyramid import BrickPyramid, build_pyramid, default_workers
    from app.compute.cube import _num_pyramid_levels, worker_image_tile

    image = Path(args.image)
    if args.make:
        w, h = (int(n) for n in args.make.lower().split("x"))
        if not image.exists():
            t = time.perf_counter()
            make_image(image, w, h)
            print(f"wrote {image} ({w}x{h}) in {time.perf_counter() - t:.0f} s")
    size = image.stat().st_size
    workers = args.workers or default_workers()
    Path(args.out).unlink(missing_ok=True)

    tracemalloc.start()
    t0 = time.perf_counter()
    build_pyramid(image, args.out, workers=workers)
    build_s = time.perf_counter() - t0
    _, traced_peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    with BrickPyramid(args.out) as pyr:
        _, height, width = pyr.shape(0)
        pyramid_bytes = Path(args.out).stat().st_size
    print(f"image {size / 1024 ** 3:.2f} GiB ({width}x{height}) -> pyramid "
          f"{pyramid_bytes / 1024 ** 3:.2f} GiB in {build_s:.0f} s ({workers} workers)")

    levels = _num_pyramid_levels(width, height, args.tile)
    rng = random.Random(0)
    tiles = []
    for level in range(levels):
        step = 1 << level
        cols = math.ceil(width / (args.tile * step))
        rows = math.ceil(height / (args.tile * step))
        picks = [(rng.randrange(cols), rng.randrange(rows)) for _ in range(args.reads)]
        row = {"level": level, "tiles": cols * rows}
        for name, pyramid in (("sampled", None), ("averaged", args.out)):
            times = []
            for tx, ty in picks:
                t = time.perf_counter()
                worker_image_tile(str(image), level, tx, ty, args.tile, pyramid)
                times.append(time.perf_counter() - t)
            row[f"{name}_median_ms"] = round(1000 * statistics.median(times), 1)
            row[f"{name}_p95_ms"] = round(1000 * _pct(times, 0.95), 1)
        tiles.append(row)
        print(f"level {level:2d} ({row['tiles']:6d} tiles): sampled {row['sampled_median_ms']:7.1f} ms "
              f"(p95 {row['sampled_p95_ms']:7.1f}), averaged {row['averaged_median_ms']:6.1f} ms "
              f"(p95 {row['averaged_p95_ms']:6.1f})")

    result = {
        "image": str(image), "image_bytes": size, "width": width, "height": height,
        "pyramid_bytes": pyramid_bytes, "build_s": round(build_s, 1), "workers": workers,
        "traced_peak_bytes": traced_peak, "cgroup": _cgroup_memory(),
        "cpus": os.cpu_count(), "platform": platform.platform(), "tile": args.tile,
        "reads_per_level": args.reads, "levels": tiles,
    }
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
