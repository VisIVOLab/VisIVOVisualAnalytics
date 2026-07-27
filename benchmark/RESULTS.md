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

CARTA v22.21.1 (backend v6.0.0), same host, same `cube_8gb.fits`, measured in a
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
