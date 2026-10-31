#!/usr/bin/env python3
"""
pyramid_bench.py - build the brick pyramid of a cube and time brick reads
(issue #69).

Measures what the issue's "Done when" asks for:

* the build: wall time, and the peak of the Python and numpy allocations it
  traces (tracemalloc, to which numpy reports its arrays; native allocations
  such as HDF5's own caches are not included). Inside a container it also
  records the cgroup's memory limit and lifetime peak: the peak counts all the
  memory the cgroup was charged, page cache included, so it cannot isolate the
  build's working set; finishing under a limit smaller than the cube is the
  evidence that the build does not hold the cube;
* brick reads: for each level, the time to read random bricks with a one-voxel
  ghost, median and 95th percentile (nearest rank).

Run it with the backend's code on the path, e.g. in a memory-capped container
smaller than the cube (see README.md):

    PYTHONPATH=backend python3 pyramid_bench.py --cube /data/cube_8gb.fits \
        --out /work/cube_8gb.pyr.h5 --json /work/results.json
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


def _cgroup_memory() -> dict:
    """The cgroup v2 memory limit and peak of this process, when readable."""
    out = {}
    for name in ("memory.max", "memory.peak"):
        try:
            value = Path("/sys/fs/cgroup", name).read_text().strip()
            out[name] = None if value == "max" else int(value)
        except (OSError, ValueError):
            out[name] = None
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--cube", required=True)
    ap.add_argument("--out", required=True, help="where to write the pyramid")
    ap.add_argument("--hdu", type=int, default=0)
    ap.add_argument("--brick", type=int, default=64)
    ap.add_argument("--max-block-mb", type=float, default=256)
    ap.add_argument("--reads", type=int, default=50, help="random bricks read per level")
    ap.add_argument("--json", help="also write the results as JSON")
    args = ap.parse_args()

    from app.brick_pyramid import BrickPyramid, build_pyramid  # noqa: PLC0415

    cube_bytes = Path(args.cube).stat().st_size
    tracemalloc.start()
    start = time.perf_counter()
    path = build_pyramid(args.cube, args.out, hdu=args.hdu, brick=args.brick,
                         max_block_bytes=int(args.max_block_mb * 1024 ** 2))
    build_s = time.perf_counter() - start
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    cgroup = _cgroup_memory()

    rng = random.Random(42)
    levels = []
    with BrickPyramid(path, source=args.cube) as p:
        for level in range(p.levels):
            grid = p.grid(level)
            times = []
            for _ in range(args.reads):
                idx = tuple(rng.randrange(n) for n in grid)
                t0 = time.perf_counter()
                p.read_brick(level, *idx, ghost=1)
                times.append((time.perf_counter() - t0) * 1000)
            times.sort()
            levels.append({"level": level, "shape": p.shape(level), "bricks": grid,
                           "read_ms_median": round(statistics.median(times), 2),
                           "read_ms_p95": round(times[math.ceil(0.95 * len(times)) - 1], 2)})

    result = {
        "cube": args.cube, "cube_bytes": cube_bytes, "brick": args.brick,
        "max_block_bytes": int(args.max_block_mb * 1024 ** 2),
        "pyramid": str(path), "pyramid_bytes": Path(path).stat().st_size,
        "build_s": round(build_s, 1), "build_alloc_peak_bytes": peak,
        "cgroup_memory_max": cgroup["memory.max"], "cgroup_memory_peak": cgroup["memory.peak"],
        "reads_per_level": args.reads, "p95": "nearest rank",
        "levels": levels, "host": platform.node(), "cpus": os.cpu_count(),
        "when": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    gib = 1024 ** 3
    print(f"cube {cube_bytes / gib:.2f} GiB -> pyramid {result['pyramid_bytes'] / gib:.2f} GiB "
          f"in {build_s:.0f} s; traced allocation peak {peak / 1024 ** 2:.0f} MiB"
          + (f"; cgroup limit {cgroup['memory.max'] / gib:.2f} GiB" if cgroup["memory.max"] else ""))
    for row in levels:
        print(f"  level{row['level']} {row['shape']}: brick read median {row['read_ms_median']} ms, "
              f"p95 {row['read_ms_p95']} ms")
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
