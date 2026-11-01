# Remote 3-D view: frame rate

Two scripts that measure the server-rendered 3-D view without the desktop
GUI, so the numbers can be reproduced.

| Script | Where it runs | What it measures |
|---|---|---|
| `push_stream_fps.py` | where the desktop would run | frames per second a client receives while the view is rotated, through whatever network lies between (an SSH tunnel to a cluster node, for instance) |
| `multi_gpu_sessions.py` | on the backend's node, inside its image | frames per second of several views rotated at once, spread over the node's GPUs or all on one |
| `out_of_core_views.py` | on the backend's node, inside its image | the same views drawn in core and out of core from the brick pyramid: frames, their difference, time to settle, frames per second while dragging, the render process's memory |
| `make_synthetic_cube.py` | anywhere | writes a cube of any size (noise and 40 sources) plane by plane, to test views of cubes larger than a GPU |

Both use a 2540 × 1560 frame (the 3-D pane of a Retina desktop), the
backend's own volume threshold (the cube's 99th percentile) unless
`--threshold` gives another, and rotate by one degree per step. Both wait for
the cube to load before rotating: `--settle` seconds (default 30), then
until no new frame has come for 8 s. The backend does not report the end of
a load, so give a cube that loads slowly a longer `--settle`.

## A backend on a SLURM node

Start the backend on a GPU node with the EGL image and open the tunnel
(see `scripts/visivo_slurm_tunnel.sh --help`):

```bash
bash scripts/visivo_slurm_tunnel.sh --frontend <frontend> --user <user> \
    --partition gpu --gres gpu:4 --cpus 64 --mem 240G \
    --sif <dir>/backend-egl.sif --data-root <dir>/data --port 18900
```

### Through the tunnel

```bash
backend/.venv/bin/python benchmark/remote_render/push_stream_fps.py \
    --path <dir>/data/cube.fits --base-url http://localhost:18900 \
    --token-file ~/.visivo_slurm_token --runs 3 \
    --out benchmark/remote_render/results/push_stream_<site>.json
```

The first rotation starts at the finest drag level, the later ones where the
level settled.

### On the node

No backend or tunnel is needed: the script opens the render sessions
itself, in a job of its own, with the backend's code on the path:

```bash
srun -p gpu --gres=gpu:4 -c 32 --mem=200G --time=00:30:00 \
    apptainer exec --nv --env PYTHONPATH=<checkout>/backend <dir>/backend-egl.sif \
    python3 <checkout>/benchmark/remote_render/multi_gpu_sessions.py \
    --cube <dir>/data/cube.fits --sessions 4 \
    --out <checkout>/benchmark/remote_render/results/multi_gpu_<site>.jsonl
# ... --sessions 4 --one-gpu    all four on the job's first GPU
```

Each session is a render process, as in the backend; the GPU each one
rendered on is read from `nvidia-smi`. On PLEIADI-GPU `--nv` was enough
with the EGL image; where it is not, add the binds and environment
`scripts/launch_backend_slurm.sh` sets for EGL.

### Out of core

```bash
python3 benchmark/remote_render/make_synthetic_cube.py <dir>/synthetic.fits      # 23.4 GiB
srun -p gpu --gres=gpu:1 -c 16 --mem=96G --time=00:30:00 \
    apptainer exec --nv --bind /scratch --env PYTHONPATH=<checkout>/backend,VISIVO_PYRAMID_DIR=<dir>/pyr \
    <dir>/backend-egl.sif python3 <checkout>/benchmark/remote_render/out_of_core_views.py \
    --cube <dir>/synthetic.fits --out <dir>/ooc_synthetic --mode ooc
```

Build the pyramid first (`python -m app.brick_pyramid build`, same
`VISIVO_PYRAMID_DIR`) or the first session builds it, and its time and memory
then count in the run. `--mode both` on a cube that fits the GPU compares the
two paths.

## Results

`results/` holds the runs reported in papers, with the commit they ran at.
