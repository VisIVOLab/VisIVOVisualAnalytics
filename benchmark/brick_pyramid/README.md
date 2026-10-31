# Brick pyramid: build and read

`pyramid_bench.py` builds the brick pyramid of a FITS cube
(`backend/app/brick_pyramid.py`, issue #69) and reports:

- the build's wall time and the peak of the Python and numpy allocations it
  traces (tracemalloc; native allocations such as HDF5's caches are not
  included);
- inside a container, the cgroup's memory limit and lifetime peak; the peak
  counts everything the cgroup was charged, page cache included, so it cannot
  isolate the build's working set. That the build finishes under a limit
  smaller than the cube is the evidence that it does not hold the cube;
- for each level, the time to read random bricks with a one-voxel ghost
  border, median and 95th percentile (nearest rank, 50 reads per level).

## The check of issue #69: a cube larger than the memory available

Build the pyramid in a container whose memory limit (no swap) is smaller than
the cube, with the backend image of `benchmark/remote/` and the backend's
current code mounted over it:

```bash
python3 benchmark/make_synthetic_cube.py --gb 8 --out /data/cube_8gb.fits
docker run --rm --memory 2g --memory-swap 2g \
    -e PYTHONPATH=/app/backend \
    -v "$PWD/backend/app:/app/backend/app:ro" \
    -v "$PWD/benchmark/brick_pyramid:/bench:ro" \
    -v /data:/work \
    visivo-bench-backend \
    python /bench/pyramid_bench.py --cube /work/cube_8gb.fits \
        --out /work/cube_8gb.pyr.h5 --json /work/results.json
```

`visivo-bench-backend` is the image built from `benchmark/remote/Dockerfile`.

## Results

`results/` holds two runs, on a MacBook Pro (Apple M5 Max, 18 cores, 64 GB;
Docker with 16 GB) in a container limited to 2 GiB without swap, 64-voxel
bricks and 256 MiB blocks:

| Cube | Build | Traced allocation peak | Pyramid file | Brick read, level 0 (median / p95) |
|---|---|---|---|---|
| 1586 x 1586 x 793, 7.43 GiB | 46 s | 610 MiB | 9.32 GiB | 7.7 / 11.3 ms |
| 794 x 794 x 397, 0.93 GiB | 6 s | 596 MiB | 1.38 GiB | 4.2 / 9.6 ms |

The cube of 7.43 GiB was built in a container of 2 GiB. The traced allocation
peak is the same for a cube eight times smaller: it depends on the block size,
not on the cube. Brick reads get faster at each level up: for the larger cube
the median is 6.5 ms at level 1, 2.7 ms at level 2, 1.0 ms at level 3 and
under 0.5 ms above (the JSON files have every level). The data sat on the host's disk behind
Docker's file sharing, so reads on a node's local or parallel filesystem will
differ.
