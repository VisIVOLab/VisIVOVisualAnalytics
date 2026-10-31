# VisIVO Visual Analytics benchmark: protocol

Two scripts with no unusual dependencies (`requests`, `numpy`, `astropy`).
They produce reproducible timings of the backend's main operations, the
numbers reported in the paper's benchmark table.

## 1. Preparation

Start the backend on the host to measure (embedded on the desktop machine, or
a remote/HPC node):

    cd backend && .venv/bin/uvicorn app.main:app --port 8000

The token is in `~/.visivo_token` (the script reads it by itself for a local
backend; for a remote backend pass it with `--token`).

### Test cubes

**Option A: synthetic (bit-for-bit reproducible, recommended):**

    python3 make_synthetic_cube.py --gb 1  --out /data/cube_1gb.fits
    python3 make_synthetic_cube.py --gb 8  --out /data/cube_8gb.fits
    python3 make_synthetic_cube.py --gb 30 --out /data/cube_30gb.fits

Each cube holds Gaussian noise plus an injected rotating disc (PA 30°,
inclination 60°, peak S/N about 12) at the centre of the cube: the
benchmark's automatic seed (the brightest pixel of the central channel) falls
on the disc by construction, and the lasso has a realistic, kinematically
extended object to select.

**Option B: real data (for use cases):** a public WALLABY field. Pass
`--seed Z,Y,X` on the chosen galaxy.

## 2. Running

    python3 benchmark_visivo.py \
        --cube /data/cube_1gb.fits \
        --cube /data/cube_8gb.fits \
        --cube /data/cube_30gb.fits \
        --repeats 3 --out results_$(hostname)

Notes:
- The `--cube` paths are paths ON THE BACKEND'S FILESYSTEM.
- Each repetition varies the parameters slightly (the lasso's reach, the
  first channel of M0, the vertices of the PV path) so that the product LRU
  cache does not distort the timings. Results are reported as median ±
  1.4826·MAD.
- On the 30 GB cube the full-resolution load is not measured by default
  (`(ROI)` in the table): above `--roi-only-above 10` GB only the camera ROI
  is measured (the central half-box, 1/8 of the voxels), consistent with how
  the tool is meant to be used. To measure the full resolution anyway:
  `--roi-only-above 100`.
- First ("cold") measurement versus repetitions ("warm", with the operating
  system's page cache): the table uses the medians. To measure a cold start
  as well, restart the backend and drop the page cache (macOS: `sudo purge`)
  between runs, and use `--repeats 1` in a separate run.
- Fill in the hardware specification (CPU, RAM, GPU, disk, filesystem) in
  `environment.note` of `results.json`: it belongs with any published table.

## 3. Output

- `results.md`: a readable table;
- `table2_rows.tex`: LaTeX rows for the paper's benchmark table (same row
  labels);
- `results.json`: raw data plus the environment record (to archive, and to
  deposit with the scripts if the results are published).

## 4. CARTA column (context, not a contest)

Same host, same 8 GB cube (`cube_8gb.fits`), the latest CARTA. Manual
protocol (3 repetitions, report the median):

| row                 | what to measure in CARTA                                         |
|---------------------|------------------------------------------------------------------|
| Preview interactive | File → Open → time to the first interactive render of the channel |
| Full resolution     | time to the full-resolution image of the current channel (progress bar) |
| Lasso grow          | **n/a** (no equivalent operation)                                |
| Masked M0           | Moment Generator: full range, 3σ mask → generation time         |
| PV extraction       | PV Generator: equivalent 3-segment line, width 3 → time          |

With a CARTA backend started with `--enable_scripting`, the same measurements
can be automated with the `carta` Python package; a manual stopwatch
measurement (±0.5 s) is adequate if the caption says so. The two columns
contextualise rather than rank: the tools optimise different interactions
(CARTA renders 2-D tiles server-side, VisIVO Visual Analytics transfers voxels
for local 3-D volume rendering), so they measure homologous, not identical,
operations; the caption should say so.

## 5. Reporting the results

1. `table2_rows.tex` gives the rows of the benchmark table (labels match).
2. The caption states the hardware, the VisIVO Visual Analytics commit, the
   CARTA version, the synthetic datasets with RNG seed 42 (reproducible) and
   the median of 3 runs.
3. `benchmark_visivo.py` and `make_synthetic_cube.py` are in the public
   repository and can be cited in the data and software availability
   statement.
4. The "lasso grow" figure on the real WALLABY cube (with `--seed` on the
   galaxy) is the one to quote for that use case.
