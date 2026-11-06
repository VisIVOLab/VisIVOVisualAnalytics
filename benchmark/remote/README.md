# Remote and memory-capped benchmark

The single-host campaign (`../run_local_bench.sh`) measures the embedded
service on the client's own machine. This one runs the same
`benchmark_visivo.py` against a service that is not the client's:

- **a Linux container with a hard memory cap** (default 8 GB, swap disabled),
  so a 30 GB cube is larger than the memory the service may use. The cgroup's
  `memory.current` is sampled during every operation and `memory.peak`,
  `memory.max` and the OOM-kill count are recorded at the end;
- **a shaped link**: `tc netem` on the container's egress adds a fixed delay
  (default 20 ms per round trip) and caps the bandwidth (default 1 Gbit/s),
  the order of a national research-network path between two sites.

It is an emulation, not two machines: client and container share the host's
CPU and SSD, and the cube is read through Docker's file sharing. What it does
establish is the behaviour of the service under a memory cap smaller than the
cube, and the latency and bytes a client pays over a link of known capacity.
The same script works unchanged against a real remote host by pointing
`--base-url` of `benchmark_visivo.py` at it.

## Run

```bash
docker build -f benchmark/remote/Dockerfile -t visivo-bench-backend .
benchmark/remote/run_remote_bench.sh ~/visivo_bench_data benchmark/results_remote_$(date +%Y%m%d) \
    cube_1gb.fits cube_8gb.fits cube_30gb.fits
```

Knobs: `MEM`, `CPUS`, `WORKERS`, `DELAY`, `RATE`, `REPEATS` (see the script).
Each cube gets a fresh container. Per cube the output directory holds
`results.json` and `results.md` from the benchmark (timings, bytes sent back
per operation, peak memory per operation, slice latency idle and while a masked
M0 runs) and `container.txt` (cgroup peak, cap, OOM kills, link statistics,
health-check round-trip time).

# Out-of-core moments on an HPC node (`run_hpc_moment_bench.sh`)

The container campaign above caps a service with a cgroup. An HPC node
usually has no Docker and no way to create one: on PLEIADI, for example,
SLURM's cgroup v1 does not enforce memory at all. This runner times the
moment maps M0, M1 and M2 directly, one computation per process
(`../moment_ooc_bench.py`), on any Linux host, with or without Apptainer:

```bash
# inside an allocation (sbatch/srun), from the repository:
SIF=/path/backend.sif BINDS=/mnt/beegfs CAP_GB=16 MASK=8.6e-5 REPEATS=3 \
CHECK_CHANNELS=3630:3757 \
benchmark/remote/run_hpc_moment_bench.sh out_dir sdc2=/mnt/beegfs/.../sky_ldev_v2.fits
```

- **Memory cap**: `RLIMIT_DATA` of the measuring process (`CAP_GB`). The
  kernel refuses private writable memory beyond it (heap, numpy arrays,
  copy-on-write mappings). The read-only mapping of the cube and the page
  cache are not counted. So a cube larger than the cap can be mapped, and
  anything that materialises it fails. `RLIMIT_AS` (`ulimit -v`) does not
  work here because it counts the mapping of the file. The cap applies per
  process, and every thread of the computation runs in that one process.
  `CAP_PROOF=1` runs the in-memory way once to show that the cap is enforced
  (it must fail with `MemoryError`).
- **Memory reported**: peaks of `RssAnon` (what the computation
  materialised) and `RssFile` (the mapped cube: clean, reclaimable pages,
  which is why the RSS of a streaming computation over-counts), both read
  from `/proc/self/status`.
- **Ways**: `dask` is what the service runs for a channel range above
  `VISIVO_DASK_MIN_BYTES` (the default, `VISIVO_DASK_MODE=auto`). `stream` is
  the slab streamer of `worker_moment` (`VISIVO_DASK_MODE=off`).
- **Cold runs**: before every run the cube is dropped from the page cache
  with `posix_fadvise(DONTNEED)`, which needs no privileges. A shared node
  has no `drop_caches`. The first run (repeat 0) is reported separately from
  the median ± MAD of the repeats.
- **Correctness**: `CHECK_CHANNELS` computes each moment on that range in
  four ways: in memory, streamed, with dask, and with an independent float64
  reference. It compares the maps with `../moment_criterion_ulp.py`, or with
  a criterion of your own: a file that defines
  `compare(order, ref, test, reference)`.
- **BLAS pools**: with one thread per hardware thread, OpenBLAS reserves more
  than a 16 GB `RLIMIT_DATA` at import and aborts. The runner sets
  `BLAS_THREADS=4`; the moments use no BLAS.

The backend code that is measured is `BACKEND/app` (default: this
checkout), bound over whatever the image ships.

## The whole campaign from one command (`run_hpc_moment_campaign.sh`)

To measure a release again, for example for a paper revision, run this on the
cluster's login node from a git checkout:

```bash
SIF=/mnt/beegfs/fvitello/visivo/backend-egl.sif BINDS=/mnt/beegfs COMMIT=v1.0.10 \
benchmark/remote/run_hpc_moment_campaign.sh /mnt/beegfs/.../moments_v1.0.10 \
    benchmark/remote/cubes_pleiadi_issue3.txt
```

It exports `COMMIT`'s `backend/app` and `benchmark/` with `git archive` into
`CAMPAIGN/src`, so the code measured is that commit whatever the working tree
holds. It then writes `CAMPAIGN/PROVENANCE.txt` (commit, image and its
sha256, cap method, SLURM settings) and submits one job per line of the cube
list. Each job waits for the previous one (`afterany`), so no two
measurements share a node or the file system's bandwidth. A job writes its
raw results beside itself in `CAMPAIGN/<label>_<fs>_<ways>/`. There
`env.txt` records the node, MemTotal, commit and cap, and every line of
`results.jsonl` records the backend function that ran, the cap and whether
the kernel enforced it.

Each line of the cube list gives `label fs path mask repeats ways [options]`.
`fs=local` copies the cube to node-local `LOCAL_DIR` first and deletes the
copy when the job ends, however it ends. Options: `check=A:B` (the
equality check), `make=GB` (create a synthetic cube with a frequency axis if
the path does not exist; it is written under another name and renamed only
when complete), and `channels=A:B`. Site knobs: `PARTITION`, `SBATCH_ARGS`
(default `--gres=gpu:1 -c 32 --exclusive`, the PLEIADI GPU partition's QOS
requires a GPU), `TIME`, `LOCAL_DIR`, `THREADS`, `AFTER` and `DRY_RUN=1` (write
the jobs, submit nothing). To stop a campaign: `scancel $(cut -d' ' -f1
CAMPAIGN/jobs.txt)`. Outputs: `results.jsonl`,
`results.md`, `check.json`, `cap_proof.jsonl` and `env.txt`. The results of
issue #3 are in `../RESULTS.md`.
