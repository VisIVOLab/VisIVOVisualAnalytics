# VisIVO backend performance — benchmark & interpretation

Quantitative performance of the operations the software paper claims are
interactive on multi-GB spectral cubes, measured on **real HI cubes of increasing
size** (24 MB → 2.1 GB). Reproducible via:

```bash
backend/.venv/bin/python validation/benchmark.py    # → out/benchmark.{md,csv}
```

Each `(cube, operation)` runs in its own process; we record wall time and **peak
resident memory** (`ru_maxrss`). Machine: Apple Silicon, macOS.

| Cube | Dims (x×y×z) | Size | Preview | Slice | Moment-0 | Lasso |
|---|---|---:|---|---|---|---|
| WALLABY | 196×280×109 | 24 MB | 0.25 s / 168 MB | 0.19 s / 139 MB | 0.21 s / 368 MB | 0.40 s / 575 MB |
| cubehi-M31 | 512×512×200 | 210 MB | 0.24 s / 199 MB | 0.19 s / 140 MB | 0.38 s / 956 MB | 1.07 s / 2173 MB |
| HGC 44 | 700×700×158 | 310 MB | 0.24 s / 216 MB | 0.24 s / 154 MB | 0.37 s / 1851 MB | 0.78 s / 1012 MB |
| Fornax | 1046×952×398 | 1585 MB | 0.23 s / 275 MB | 0.24 s / 160 MB | 0.91 s / 4419 MB | 1.89 s / 2234 MB |
| 3D-Ape | 661×661×1218 | 2129 MB | 0.23 s / 307 MB | 0.22 s / 152 MB | 1.00 s / 3252 MB | 1.67 s / 2169 MB |

## Key findings

1. **Interactive on multi-GB cubes.** Every operation completes in **≤ 1.9 s**
   across the whole 24 MB → 2.1 GB range — the interactivity claim, quantified.

2. **Constant-memory navigation (the out-of-core win).** A **full-resolution
   channel map** is served in **~0.2 s using ~150 MB regardless of cube size** —
   152 MB for the 2.1 GB cube, the same as for the 24 MB one. The 3-D preview
   likewise stays ~170–310 MB. Slice navigation and the first 3-D view never load
   the cube; they read only the pages they need from the memmap.

3. **Moment maps stream.** Moment-0 over *all* channels of the 2.1 GB cube runs in
   1.0 s. It necessarily reads every voxel once, but the streaming path
   accumulates running sums over spectral slabs instead of materialising the cube:
   it is **bit-identical** to the in-memory path (max |Δ| = 0) yet holds no
   full-cube copy. Evidence: forcing the in-memory path on Fornax / 3D-Ape peaks
   at **11.6 / 17.6 GB**, versus **4.4 / 3.3 GB** streaming — i.e. the in-memory
   path carries ~3 extra full-cube buffers that streaming avoids. (The streaming
   path auto-engages above `VISIVO_MOMENT_STREAM_BYTES`, default 2 GB.)

4. **Kinematic Lasso is window-bounded.** One geodesic object grow takes
   0.4–1.9 s, and its peak memory is set by the seed-centred work window
   (≤ ~2.2 GB), **not** the full cube — the 2.1 GB and 1.5 GB cubes both peak at
   ~2.2 GB, while a compact source in a small cube uses far less.

## Notes

- Peak RSS for full-cube reductions (moment) includes memmap pages the OS keeps
  resident (page cache, reclaimable); the *heap* allocation is bounded by the slab,
  not the cube. Slice/preview touch few pages, so their RSS is the true small
  working set.
- These are single-node, single-run numbers for the software (reference) paper —
  a full scaling / concurrency study is deferred to the methods paper.
