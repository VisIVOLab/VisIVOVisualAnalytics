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

### Where the time goes, and several processes (issue #89)

With `--breakdown` the script times the build's stages: reading the FITS
file, reading back the level below from the HDF5 file, the 2x2x2 averages,
the per-brick statistics and the writes (tracemalloc is off for that run).
With `--workers N`, N processes read and reduce while the script's own
process writes. The runs below are on PLEIADI-GPU, node r33c1s01 (POWER9,
128 CPUs), image `backend-egl.sif`, the synthetic 23.44 GiB cube of
`benchmark/remote_render/make_synthetic_cube.py` on BeeGFS, 64-voxel bricks
and 256 MiB of blocks.

One process, three versions of the code (`breakdown_23gb_pleiadi_*.json`):

| Version | Build | Averages | Statistics | HDF5 writes | HDF5 reads | FITS reads |
|---|---|---|---|---|---|---|
| before #89 | 215 s | 164.7 s | 19.8 s | 19.0 s | 6.9 s | 3.9 s |
| averages from eight strided views | 89 s | 36.0 s | 19.9 s | 21.2 s | 7.1 s | 3.9 s |
| and statistics with fmin / fmax | 77 s | 35.8 s | 9.4 s | 20.6 s | 6.8 s | 3.9 s |

The output is the same in all three. The FITS file was in the node's page
cache, hence its short reads.

Several processes (`scale_23gb_pleiadi_w*.json`, a job of 32 CPUs on a node
of 128; the JSON files record the node's 128 as `cpus`):

| Processes | 1 | 4 | 8 | 16 | 32 |
|---|---|---|---|---|---|
| Build | 78 s | 45 s | 46 s | 47 s | 46 s |

`wait_workers` is the writer waiting for the workers: 7.2 s with four
processes, 1 to 2 s with eight or more. From there the build is bound by the
writer, writing the 27.4 GiB file from one process at about 600 MB/s; the
17 to 22 s of `other` in the parallel runs is not attributed (the reads of
the FITS file happen in the workers and are not timed there), and it moves
into the writes when the blocks are larger. Only the pyramid's data is the
same in all runs; the files differ in size by a few hundred KB, as HDF5 lays
out the chunks in the order they arrive. larger blocks (1 or 2 GiB in all, `scale_23gb_pleiadi_w8_mb*.json`)
give 43 and 45 s. Writing from several processes (parallel HDF5 with MPI, or
one file per process joined as an HDF5 virtual dataset) only pays if the file
system takes more from several writers, which is to be measured first.
