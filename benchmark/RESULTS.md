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
