# Faster out-of-core moments (issues #116, #117, #119, 2026-11-06)

The #3 campaign below, run again with the same protocol, cubes, node type,
image and 16 GB `RLIMIT_DATA` cap, on code that:

- reads each block with explicit positional reads (whole planes in one read,
  or one read per channel for a band of rows) instead of faulting the memory
  map; the streamer reads two slabs ahead on a thread (#116);
- chooses `VISIVO_MOMENT_THREADS` from the CPUs the process may use and its
  memory budget, here the 16 GB cap: 15 threads for SDC2, 14 for Sky124, 7 for
  the 450 GB cube, against a fixed 4 before (#117);
- computes Export Moment as FITS on the `/v1/products/moment` path (#119; not
  exercised by this benchmark, which calls the computation directly).

**Code measured.** `main` at ca8f335 plus the uncommitted changes of these
issues, copied with `run_hpc_moment_campaign.sh SRC_TREE=...`; sha256 of every
file in `results_pleiadi_20261106/src.sha256` (`moment.py` 3f729d07…51a8).
Image `backend-egl.sif` aa3f592e…dc46, as on 2026-11-05. Node r33c1s02.
Raw records: `results_pleiadi_20261106/`.

### Results (wall seconds, median ± MAD of the repeats; first run in brackets)

Before = the 2026-11-05 table (dask with 4 threads, the streamer on 1).
dask now uses the automatic thread count.

| cube | file system | way | M0 before → now | M1 before → now | M2 before → now | anon peak now |
|---|---|---|---:|---:|---:|---:|
| SDC2, 44 GB | BeeGFS | dask | 55.1 → **21.5** ± 0.3 (22.1) | 75.9 → **31.9** ± 0.4 (32.3) | 126 → **56.9** ± 0.5 (57.1) | 5.1–7.2 |
| | BeeGFS | stream | 197 → **114** ± 7 (113) | 192 → **99.7** ± 2.8 (101) | 324 → **221** ± 2 (220) | 0.9–1.1 |
| | local NVMe | dask | 35.6 → **22.1** ± 0.0 (22.2) | 53.8 → **32.6** ± 0.1 (32.9) | 100 → **58.5** ± 0.3 (58.6) | 4.8–7.0 |
| | local NVMe | stream | 103 → 104 ± 10 (115) | 104 → 104 ± 0 (116) | 201 → 213 ± 1 (210) | 0.9–1.1 |
| Sky124, 124.5 GB | BeeGFS | dask | 169 → **64.4** ± 0.1 (70.2) | 231 → **96.6** ± 0.4 (96.3) | 375 → **172** ± 1 (173) | 4.7–6.8 |
| | BeeGFS | stream | 613 → **291** ± 25 (284) | 614 → **273** ± 3 (280) | 913 → **569** ± 8 (553) | 1.0–1.2 |
| | local NVMe | dask | 106 → **64.8** ± 0.1 (65.8) | 168 → **97.7** ± 0.1 (98.1) | 314 → **176** ± 0 (176) | 4.8–6.7 |
| | local NVMe | stream | 308 → 291 ± 12 (286) | 311 → 302 ± 9 (286) | 633 → 606 ± 8 (620) | 1.0–1.2 |
| synthetic, 450 GB | BeeGFS | dask | 727, 740 → **334** ± 7 (365) | 1075, 1078 → **522** ± 3 (554) | 1924, 1926 → **917** ± 0 (922) | 6.8–9.8 |
| | BeeGFS | stream | 2544 → **1199** | 2520 → **1192** | 4856 → **2318** | 2.5–3.6 |

SDC2 has 3 repeats, Sky124 2; the 450 GB cube 2 repeats for dask and one run
for the streamer, as on 2026-11-05. The cube is evicted from the page cache
before every run. The cap was enforced in every run, and the in-memory way
failed under it on every cube (`cap_proof.jsonl`: "Unable to allocate 41.1
GiB" on SDC2, 419 GiB on the 450 GB cube).

**Read throughput** (bytes the reduction reads per second of wall time; `dd`
reads these files at 2.5 GB/s):

| | dask before | dask now | stream before | stream now |
|---|---:|---:|---:|---:|
| BeeGFS | 0.42–0.80 | 0.86–2.05 | 0.18–0.27 | 0.38–0.46 |
| local NVMe | 0.74–1.24 | 1.27–1.99 | 0.39–0.44 | 0.41–0.43 |

**Threads** (SDC2, dask, one run each, cap 16 GB; job `sweep_1082`):

| `VISIVO_MOMENT_THREADS` | 1 | 4 | 8 | 16 | 32 | 64 |
|---|---:|---:|---:|---:|---:|---:|
| M0 local, s (before) | 111 | 38.6 | 22.6 | 16.9 | 16.4 | MemoryError |
| M0 local, s (now) | 110 | 35.5 | 24.3 | 17.7 | 17.0 | MemoryError |
| M2 local, s (before) | 311 | 111 | 61.9 | 42.7 | MemoryError | MemoryError |
| M2 local, s (now) | 298 | 104 | 60.3 | 43.0 | MemoryError | MemoryError |
| anon peak M2 now, GB | 0.99 | 2.39 | 3.75 | 6.70 | 12.6 (failed) | 12.6 (failed) |

With 4 threads on BeeGFS (the old default, so the explicit reads alone) SDC2
took 45.5 s for M0 and 108 s for M2, against 55.1 and 126 s before. The job
is marked FAILED by SLURM only because the 32- and 64-thread points end in
the expected `MemoryError`; every point ran.

The campaign's default runs on the local disk (15 threads: 22.1 s M0, 58.5 s
M2) were slower than the sweep's 16 threads (17.7 and 43.0 s). One more job
(`t15_1086`, 2026-11-08, same node, two runs each, dask only) ran explicit 15,
explicit 16 and the default one after another:

| threads | M0, s | M2, s | anon peak M2, GB |
|---|---:|---:|---:|
| default (chose 15, memory) | 17.6, 17.4 | 43.6, 43.5 | 6.7 |
| 15 | 17.5, 17.9 | 42.8, 43.0 | 7.1 |
| 16 | 16.9, 17.0 | 42.0, 41.6 | 7.5 |

So the default is within 5 % of 16 threads, and the gap is not the thread
choice. Why the campaign job ran slower is not established (there each dask
run alternated with a streamer run on the same copy); its numbers are kept
as measured, and are the conservative ones.

### Correctness

- `moment_ooc_bench.py check` on SDC2 channels 3630–3757 with
  `moment_criterion_ulp.py`: in memory, streamed and dask agree bit for bit
  on M0, M1 and M2, and every comparison with the float64 reference passes
  (`sdc2_shared_dask-stream/check.json`, `pilot_1075/check.json`).
- Every measured map, whole cube, has the same `map_sum` and the same number
  of finite pixels as the 2026-11-05 run of the same way: 18 of 18 (3 cubes ×
  3 moments × 2 ways). On the 450 GB cube the streamer's M1 and M2 sums differ
  from dask's in the 13th to 15th significant digit, as they already did on
  2026-11-05; that difference is between the two ways, not between the old
  and new code.
- Backend tests: blocks read by explicit reads equal the memory-mapped slices
  for big-endian floats, 32- and 64-bit floats, plain and scaled integers,
  BLANK, unsigned 8- and 16-bit, degenerate and non-degenerate Stokes axes, an
  image extension and VBT fields of both byte orders; dask and streamed maps
  are bit-identical with explicit reads and through the memory map, with
  whole planes, bands of rows and single-channel blocks
  (`backend/tests/test_moment_reads.py`).

### Reading the numbers

- **dask, the service's way for large ranges, is 1.6–2.6 times faster** on
  every cube and file system: M2 over 450 GB on BeeGFS takes 15 min instead
  of 32, M0 5.6 min instead of 12. It now reads SDC2 at 2.05 GB/s on BeeGFS,
  82 % of `dd`. Most of the gain is the thread count: the reads alone (4
  threads) give 1.17–1.21 times on BeeGFS and nothing on the local disk,
  where dask was already CPU-bound.
- **The streamer** gains 1.5–2.3 times on BeeGFS, where it was waiting on
  page faults (CPU/wall 0.5 before, 1.1–1.2 now), and nothing on the local
  disk, where it was and is bound by its one core.
- **The thread default lands near the fastest point that fits.** Under the
  16 GB cap it chose 15 threads for SDC2 (the sweep's fastest point that
  completes M2 is 16), 14 for Sky124 and 7 for the 450 GB cube, whose
  6082² plane makes each partial map 0.3 GB. Peaks stayed at 4.7–7.2 GB on
  SDC2 and Sky124 and 6.8–9.8 GB on the 450 GB cube, within the cap. On an
  18-core laptop with the default four workers it chooses 6, against 4 before
  (local check on an 8 GB cube, warm cache: M0 1.8 s and M2 4.7 s, against
  1.9 and 4.5 s; the streamer 3.9 and 8.1 s against 4.8 and 9.3 s).
- **Throughput is still under `dd`.** At 7 threads on the 450 GB cube
  dask reads 0.86–1.35 GB/s, CPU/wall 5.7–6.1: the reduction, not the
  reads, is now the limit. More threads would need a larger memory budget
  than 16 GB, or a cheaper reduction.
- **The cube moved during the campaign.** SDC2 moved from
  `/mnt/beegfs/fvitello/demo_data/` to
  `/mnt/beegfs/fvitello/visivo/data/demo_data/` (same size) after the
  campaign had read it; `remote/cubes_pleiadi_issue3.txt` now names the
  new path.

M0, M1 and M2 on one PLEIADI-GPU node (INAF-OACT): 2 × 16-core POWER9
(128 hardware threads), 317 or 334 GB of RAM depending on the node (MemTotal
recorded per run), one job per node (`--exclusive`). Cubes were read from
BeeGFS (the shared parallel file system) and from a copy on the node's local
NVMe disk (`/tmp`, xfs).

**Code measured.** VisIVO `main` at 8303e71 (Release 1.0.9) plus this
branch's uncommitted changes. Two staged trees were measured:

- `checkout-3`: the dask M0 blanking fix and the benchmark tooling. It
  produced every row except the "fixed" one.
- `checkout-3b`: the same tree plus the large-plane fix, the task time limit
  and the stop-file polling. It produced the "fixed" row and the final
  equality check. For SDC2 and Sky124 it runs the same code path as
  `checkout-3`: their planes are under the 64 MiB block budget, and the
  benchmark gives no stop file.

The checksums of `moment.py` are in
`results_pleiadi_20261105/PROVENANCE_checkout-3*.txt`. Image `backend-egl.sif`, sha256
aa3f592e…dc46. To measure a release again from one command, see
`remote/README.md` (`run_hpc_moment_campaign.sh` with
`remote/cubes_pleiadi_issue3.txt`).

**Method** (`moment_ooc_bench.py`, `remote/run_hpc_moment_bench.sh`). Each
measurement runs in a fresh process that calls the backend's own functions:

- *dask* is `_moment_map_dask` on the local threaded scheduler. The service
  runs it for every range above 512 MB (the default `VISIVO_DASK_MODE=auto`),
  with 4 threads (the default `VISIVO_MOMENT_THREADS`).
- *stream* is `worker_moment`'s slab streamer (`VISIVO_DASK_MODE=off`), on a
  single thread.

The mask is an explicit 3σ threshold: 8.6e-5 Jy/beam for SDC2, 7.3e-3 Jy/beam
for Sky124 (robust σ 2.45 mJy/beam), 3.0 for the synthetic cube (σ = 1).
Before every run the cube is dropped from the page cache
(`posix_fadvise DONTNEED`); a shared node has no `drop_caches`. The first
run is reported on its own. Then come 3 repeats for SDC2 and 2 for the
larger cubes, given as median ± 1.4826·MAD. Throughput is the bytes the
reduction reads (M2 reads the cube twice) divided by the wall time.

**Memory cap.** SLURM on PLEIADI uses cgroup v1 and does not enforce memory,
and a user cannot create a cgroup. The cap is therefore `RLIMIT_DATA` = 16 GB
on the measuring process: the kernel refuses private writable memory beyond
it, while the read-only mapping of the cube is not counted (`RLIMIT_AS` would
count it and break the memmap). Every run's record shows that the kernel
enforced the cap. To show that the cap bites, the in-memory way was run once
on every cube: it fails at once with `MemoryError` ("Unable to allocate
41.1 GiB" on SDC2). **Anon peak** is the peak of `RssAnon`, the memory the
computation materialised. The **file-backed** resident set (the mapped cube,
clean and reclaimable pages) reaches the whole cube when it fits in RAM, and
about 245 GB on the 450 GB cube. It is the over-count that RSS shows for a
streaming computation.

**Sizes.** Only the synthetic cube is larger than the node's RAM. SDC2 and
Sky124 are real cubes that are larger than the 16 GB cap but smaller than
RAM.

| cube | size | > RAM | > cap |
|---|---:|:---:|:---:|
| SDC2 `sky_ldev_v2` (1286² × 6668, FREQ) | 44.1 GB | no | yes (2.8×) |
| Sky124, WALLABY Eridanus cut-out (2001² × 7776, FREQ) | 124.5 GB | no | yes (7.8×) |
| synthetic, `make_synthetic_cube.py --gb 450 --spectral freq` (6082² × 3041) | 450.0 GB | yes (1.35–1.42×) | yes (28×) |

The full WALLABY Eridanus cube on BeeGFS (651 GiB) could not be used: the
file is truncated (697.9 GB on disk against 961.4 GB declared; 5644 of 7776
planes), and neither astropy nor the backend can map it.

### Results (wall seconds, median ± MAD of the repeats; first run apart)

dask = the service's default way (4 threads); stream = the slab streamer
(1 thread). Anonymous peak in GB (the cap is 16 GB). Raw records:
`results_pleiadi_20261105/`.

| cube | file system | way | M0 | M1 | M2 | anon peak |
|---|---|---|---:|---:|---:|---:|
| SDC2, 44 GB | BeeGFS | dask | 55.1 ± 0.8 (56.0) | 75.9 ± 0.1 (73.7) | 126 ± 2 (119) | 1.9–2.5 |
| | BeeGFS | stream | 197 ± 11 (189) | 192 ± 3 (193) | 324 ± 7 (309) | 0.8–1.0 |
| | local NVMe | dask | 35.6 ± 0.3 (33.6) | 53.8 ± 0.4 (52.5) | 100 ± 0 (100) | 1.9–2.4 |
| | local NVMe | stream | 103 ± 0 (99.0) | 104 ± 0 (102) | 201 ± 2 (198) | 0.8–1.0 |
| Sky124, 124.5 GB | BeeGFS | dask | 169 ± 0 (166) | 231 ± 5 (221) | 375 ± 1 (374) | 2.3–2.9 |
| | BeeGFS | stream | 613 ± 9 (573) | 614 ± 24 (572) | 913 ± 6 (918) | 0.8–1.1 |
| | local NVMe | dask | 106 ± 0 (104) | 168 ± 0 (168) | 314 ± 2 (310) | 2.3–2.9 |
| | local NVMe | stream | 308 ± 3 (301) | 311 ± 1 (305) | 633 ± 1 (623) | 0.8–1.1 |
| synthetic, 450 GB | BeeGFS | dask, before the fix | 776, 756, 751 | **MemoryError** ×3 (14.3 GB) | **MemoryError** ×3 (14.2 GB) | 9.0–10.2 (M0) |
| | BeeGFS | dask, **fixed** | 727, 740 | 1075, 1078 | 1924, 1926 | 5.0–7.7 |
| | BeeGFS | stream | 2544 | 2520 | 4856 | 2.2–3.3 |

Cells: median ± MAD of the repeats, with the first run (cache evicted) in
brackets. SDC2 has 3 repeats, Sky124 2. Every run of the synthetic cube was
a first run with the cache evicted, and its cells list those runs. Read throughput
(`results.md` in each directory) is 0.42–0.80 GB/s for dask and 0.18–0.27
GB/s for stream on BeeGFS, and 0.74–1.24 and 0.39–0.44 GB/s on the local
disk. The first runs are within a few percent of the repeats, so
`posix_fadvise` eviction gave cold reads every time.

**Threads** (SDC2, local disk, dask, one run each, cap 16 GB):

| `VISIVO_MOMENT_THREADS` | 1 | 4 (default) | 8 | 16 | 32 | 64 |
|---|---:|---:|---:|---:|---:|---:|
| M0, s | 111 | 38.6 | 22.6 | 16.9 | 16.4 | MemoryError |
| M2, s | 311 | 111 | 61.9 | 42.7 | MemoryError | MemoryError |
| anon peak M2, GB | 0.9 | 2.4 | 4.0 | 7.2 | 12.7 | 11.6 |

### Correctness

`moment_ooc_bench.py check` on SDC2 channels 3630–3757 (128 channels around
the brightest source of the band, 3σ mask) computed each map four ways: in
memory, streamed, with dask, and with an independent float64 reference. The
criterion is `moment_criterion_ulp.py`. The three backend ways agree **bit for
bit** on M0, M1 and M2 (largest difference 0, same 251,613 finite pixels).
Against the float64 reference the largest differences are 0.5 ULP (M0, M1)
and 0.82 ULP (M2). Every comparison passes, with the final code
(`checkout-3b`; `results_pleiadi_20261105/check_sdc2.json`).
Before the fix in this branch, the dask M0 returned 0 instead of blank on
lines of sight with no voxel above the threshold.

### Reading the numbers

- **Bounded memory, after one fix.** On SDC2 and Sky124 the anonymous peak
  stays at 0.8–1.1 GB for stream and 1.9–2.9 GB for dask, whatever the cube
  size. On the 450 GB cube the dask way **failed** M1 and M2 under the 16 GB
  cap: three runs each, `MemoryError` at 14 GB anon after 6–17 min. M0
  passed at 9–10 GB. The cause was the block: a 6082² plane (148 MB float32)
  was read as one block per channel, and the reduction materialises about
  20 times a block over four threads. Planes larger than
  `VISIVO_MOMENT_CHUNK_BYTES` are now read in bands of rows, with the M1 map
  of the second pass split the same way. The maps are bit-identical (tests
  in `backend/tests/test_compute.py`; the SDC2 check above was re-run with
  the fixed code and passed). Under the same 16 GB cap, the fixed code
  completes M0, M1 and M2 on the 450 GB cube in 12, 18 and 32 min, at
  5.0–7.7 GB anonymous. M0 also halved its memory (10.2 to 5.1 GB) and is
  3 % faster. Without the cap the failing runs would have taken more of the
  node's RAM; the cap is what exposed the problem.
- **Larger than RAM.** The 450 GB cube is 1.35–1.42 times the node's RAM.
  Both ways complete on it: dask (fixed) in 12 / 18 / 32 min for M0 / M1 /
  M2, the streamer in 42 / 42 / 81 min. The file-backed resident set
  levels off at about 245 GB, so the kernel evicts mapped pages as it
  reads. Only the anonymous memory belongs to the computation.
- **Where the time goes.** On BeeGFS the streamer is I/O-bound (CPU/wall
  0.5) at 0.2 GB/s, a tenth of what `dd` reads from the same file
  (2.5 GB/s). The reads are page faults on the memory map, one plane at a
  time. dask's four threads overlap them and reach 0.4–0.8 GB/s. On the
  local disk both ways are CPU-bound: stream on its one core (CPU/wall 1.0),
  dask on its four (3.4–3.9). More threads help up to 16 (M0 2.3× faster,
  M2 2.6× faster than with 4). Memory grows by about 0.4 GB per thread, so at
  32 and 64 threads the dask way no longer fits a 16 GB cap. The default of
  4 is safe but leaves most of a 128-thread node idle.
- **M2 costs twice M0.** It reads the cube twice (the second pass after the
  centroid), by design: it avoids the cancellation of the Σv² − M1²
  identity on frequency axes.
- **The service's limit.** At these throughputs an M1 or M2 over a few
  hundred GB takes longer than `VISIVO_HEAVY_TIMEOUT_S` (900 s). The
  synchronous `/v1/products/moment` answered 504 there, and so did the task
  route `/v1/tasks/moment`. Tasks now have no time limit
  (`VISIVO_TASK_TIMEOUT_S`, default none), a cancelled dask moment stops
  within one block, and Export Moment as FITS gained a task route
  (`/v1/tasks/moment_fits`).
- **Real data on the file system.** The full WALLABY Eridanus cube on BeeGFS
  is truncated (5644 of 7776 planes). The backend cannot open it and says
  only "buffer is too small".

---

# Responsiveness campaigns of 2026-10-26 (paper Tables 4 and 5)

Two campaigns, three calls per operation with fresh parameters (the product
cache is never hit), a fresh service per cube. Cells: first call / median of
the two further calls, seconds. Raw files: `results_local_20261026/`,
`results_remote_20261026/`.

**Local** (`run_local_bench.sh`, embedded service, Apple M5 Max, 64 GB):

| operation | 1 GB | 8 GB | 30 GB |
|---|---:|---:|---:|
| preview | 2.6 / 0.3 | 6.5 / 2.7 | 11.8 / 9.7 |
| full resolution | 1.1 / 0.5 | n/a | n/a |
| camera ROI | 0.4 / 0.4 | 2.7 / 0.7 | 1.6 / 1.4 and 5.7 |
| lasso grow | 1.7 / 1.3 | 6.0 / 4.9 | 16.6 / 15.2 |
| masked M0 | 0.3 / 0.3 | 5.6 / 1.4 | 10.2 / 5.5 |
| PV | 0.1 / 0.1 | 0.5 / 0.5 | 1.5 / 1.5 |
| slice idle (median) | 0.07 | 0.25 | 0.60 |

During a 30 GB masked M0: slice median 0.67 s, max 3.0 s (slices that started
and ended inside the M0).

**Container** (`remote/run_remote_bench.sh`: 8 GB memory cap, no swap, netem
+20 ms per round trip, 1 Gbit/s; measured round trip 47 ms):

| operation | 1 GB | 8 GB | 30 GB | sent at 30 GB |
|---|---:|---:|---:|---:|
| preview | 2.1 / 0.8 | 6.5 / 5.4 | 22.3 / 21.2 | 583 MB |
| camera ROI | 8.4 / 8.4 | 14.5 / 13.9 | 16.1 / 15.6 | 1073 MB |
| lasso grow | 1.8 / 1.8 | 5.6 / 5.5 | 16.8 / 16.8 | 9 MB |
| masked M0 | 0.6 / 0.5 | 4.5 / 3.4 | 23.2 / 23.4 | 32 MB |
| PV | 0.4 / 0.3 | 2.5 / 2.4 | 48.7 / 44.1 | 16 MB |
| slice idle | 0.13 | 0.44 | 1.06 | 29 MB |

No OOM kill. During a 30 GB masked M0 (`cube_30gb_load_probe/`): slice median
1.7 s, max 2.3 s.

**What the container found.** Before the fix, the masked M0 was OOM-killed on
the 8 and 30 GB cubes (`results_*_20261026_before_fix/`): `dask.array.from_array`
copied the memory-mapped cube into RAM. Fixed in `app/compute/moment.py`
(block-by-block reads, blocks bounded in bytes, two-pass M1-M5); locally the
30 GB M0 went from 78 s to 5-10 s as a side effect.

**Traps** (see the scripts' headers): quit Docker before the local campaign (its
VM memory made the 30 GB M0 swap); macOS background indexing (mediaanalysisd,
Spotlight) slowed zlib encoding six-fold during one run, which was discarded
and repeated when the host was idle.

---

# Table 2 — measured responsiveness (fills the paper's [TODO])

Numbers for Table 2, produced with `benchmark_visivo.py` driving the **real REST
API** (end-to-end client-facing latency, including HTTP transport) on synthetic
cubes of increasing size from `make_synthetic_cube.py` (each a rotating-disk
source in noise, so the lasso has a real velocity-coherent object).

**Backend host:** Apple M5 Max, 18 cores (6P + 12E), 64 GB RAM, internal SSD;
macOS; Python 3.11; embedded backend (client and service on the same machine).

## Reproduce

```bash
D=~/visivo_bench_data
for gb in 1 8 30; do
  backend/.venv/bin/python benchmark/make_synthetic_cube.py --gb $gb --out $D/cube_${gb}gb.fits
done
VISIVO_TOKEN=tok backend/.venv/bin/python -m uvicorn app.main:app --port 8000 &   # from backend/
backend/.venv/bin/python benchmark/benchmark_visivo.py \
    --cube $D/cube_1gb.fits --cube $D/cube_8gb.fits --cube $D/cube_30gb.fits \
    --token tok --repeats 1 --roi-only-above 2 --out benchmark/results_cold
```

## Result (cold cache — conservative first-touch, seconds)

| Operation | 1 GB | 8 GB | 30 GB |
|---|---:|---:|---:|
| Preview interactive | 3.4 | 6.4 | 19.9 |
| Full resolution ready | 1.4 | (ROI) | (ROI) |
| Camera ROI ready | 0.6 | 4.4 | 3.2 |
| Lasso grow (typical object) | 4.1 | 6.2 | 16.3 |
| Masked M₀ (out-of-core) | 1.7 | 13.8 | 81.6 |
| PV extraction | 2.4 | 3.6 | 27.2 |

LaTeX rows ready to paste in `results_cold/table2_rows.tex`.

## PV re-run after the extractor rewrite (2026-10-19)

The PV extractor was rewritten (arc-length sampling on the sky, NaN-aware
bilinear, lazy reads of the pixels around the path; see CHANGELOG). Same host,
same protocol as `results_cold_20260923` (repeats = 1, fresh backend per cube,
all operations in order), raw files in `results_cold_20261019/`:

| Operation | 1 GB | 8 GB | 30 GB |
|---|---:|---:|---:|
| PV extraction (new) | 0.10 | 3.25 | 27.8 |
| PV extraction (2026-09-23) | 0.8 | 4.6 | 31.0 |

Only the PV row of the paper's Table 2 was replaced (marked §). The other rows of
this run differ from 2026-09-23 by run-to-run noise (8 GB preview 6.7 vs 3.3 s,
30 GB M₀ 103 vs 89 s), so at 8 and 30 GB the PV gain is within that spread; at
1 GB it is real (the old path read the whole bounding box of the cut).

## Reading the numbers

- **Nothing fails at 30 GB** on a 64 GB laptop: every operation completes, out-of-
  core, on a cube the 2018 tool could not even open. The full-resolution fetch is
  capped at ≈2 GB and replaced by a **camera-ROI** fetch above it (paper §3.2); the
  ROI is a viewport-bounded box (≈1 GB), streamed in ≈3 s even on the 30 GB cube.
- These are **cold-cache, first-touch** timings (empty product cache, one repeat).
  In steady-state interactive use the same operations are faster (page cache warm;
  e.g. the 1 GB preview drops to ~0.3 s) — cold is the conservative figure.
- The two full-cube reductions on 30 GB (masked M₀ 82 s, PV 27 s) read all
  1233 channels from disk; they scale with the data touched, as expected, and are
  the slow end of the table by construction (a full-range masked moment over
  30 GB). A channel-range moment is proportionally faster.
- **Lasso grow** scales with the object and its seed window, not the cube: a
  ~0.5 M-voxel disk is selected in 4–16 s across 1→30 GB.

## CARTA column (external reference, 8 GB cube)

CARTA 6.0.0 (app bundle and backend both report 6.0.0), same host, same `cube_8gb.fits`, measured in a
GUI session (CARTA has no headless/scriptable compute path — its scripting proxies
actions to a connected Electron frontend — which is itself a data point for the
"different architectures" framing):

| Operation | CARTA 8 GB |
|---|---:|
| Preview / open | < 1 s (immediate) |
| Full-resolution 3-D fetch | n/a (thin client, tiled 2-D on demand) |
| Lasso grow | n/a (no 3-D geodesic selection) |
| Masked M₀ | ≈ 13 s |
| PV extraction | < 1 s |

Reading: the two tools **optimise different interactions**, so the table
contextualises rather than ranks (as the paper states):

- **Open/preview** is immediate in CARTA because it is a **2-D tiled thin client** —
  it reads the header and streams only the visible tiles of one channel, never the
  volume. VisIVO's preview (3.4–19.9 s) builds a **downsampled 3-D volume** for GPU
  volume rendering, a fundamentally heavier first view. Different first pixel:
  CARTA shows a 2-D channel; VisIVO shows the 3-D cube.
- **Moment M₀**: comparable (~13 s vs 13.8 s) — both read the cube.
- **PV**: CARTA faster (< 1 s vs 3.6 s), its tiled-2-D path is built for it.
- **Lasso / 3-D selection**: CARTA has **no equivalent** — the differentiator.

Note: to enable CARTA's own performance log, launch with `--log_performance`
(writes `~/.carta/log/performance.log`); the packaged app rejects the flag via its
node launcher, so the GUI-session timings above were read from the widgets.
- The `benchmark_visivo.py` camera-ROI was changed from a fixed 1/8-of-cube box
  (3.7 GB on the 30 GB cube — a single multi-GB `subvolume_bin` blob that the
  desktop never requests; it fetches parallel slabs) to a **viewport-bounded box
  capped at ≈1 GB**, so the measurement is realistic and comparable across sizes.
- Synthetic cubes total ~39 GB in `~/visivo_bench_data/` — delete after use.
