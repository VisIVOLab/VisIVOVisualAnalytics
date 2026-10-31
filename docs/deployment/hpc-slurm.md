# Running the backend on a cluster

On a SLURM cluster the backend runs in an Apptainer container on a compute
node, not on the frontend (the login node), and the desktop reaches it through
an SSH tunnel. Two scripts do this:

* `scripts/launch_backend_slurm.sh`, run **on the frontend**, submits the job,
  waits for the backend to answer and prints the tunnel command and the token;
* `scripts/visivo_slurm_tunnel.sh`, run **on the laptop**, does the same over
  SSH, opens the tunnel itself and keeps it open.

Both print their full help with `--help`. A multi-node variant that spreads the
heavy computations over a Dask cluster is described
[below](#multi-node-backend-with-dask).

## Quick start

From the laptop, with key-based SSH access to the frontend and a clone of the
repository in the frontend's home:

```bash
bash scripts/visivo_slurm_tunnel.sh \
    --frontend <frontend> --user <user> \
    --partition gpu --gres gpu:1 \
    --data-root /path/to/data \
    --sif ~/VisIVOVisualAnalytics/backend-egl.sif \
    --port 18900
```

When it reports the tunnel ready, register the backend in the desktop under
**Settings ▸ Backends ▸ Remote Backends** with the URL it prints
(`http://localhost:18900`), the token (also saved in `~/.visivo_slurm_token`
on the laptop) and, for an SRC node, the node's code in the SKAVA catalogue.
`bash scripts/visivo_slurm_tunnel.sh --stop` closes the tunnel and cancels the
job.

```{tip}
The helper's local port defaults to `18900`, not to the port of the desktop's
local backend (`8000` by default). Keep it that way if you pass `--port`: the
desktop treats a loopback URL on its local backend's port as the local backend
and, when a local token exists (`VISIVO_TOKEN` or `~/.visivo_token`), sends
that instead of the one registered for the entry, which the cluster backend
then refuses.
```

## The launcher: `launch_backend_slurm.sh`

The launcher writes a batch script, submits it with `sbatch`, waits for the
compute node to be assigned, then polls the backend's `/v1/health` over SSH
from the frontend to the compute node until it answers. It then prints the job
id, the compute node, the port, the `ssh -N -L` command to run on the laptop,
and the entries for the desktop's Backends page.

| Option | Environment default | Default | Meaning |
|---|---|---|---|
| `--partition P` | `VISIVO_PARTITION` | `gpu` | SLURM partition. |
| `--time HH:MM:SS` | `VISIVO_TIME` | `08:00:00` | Wall-clock limit of the job. |
| `--mem M` | `VISIVO_MEM` | `64G` | Memory per node, passed to `sbatch --mem` as given. |
| `--cpus N` | `VISIVO_CPUS` | `8` | `--cpus-per-task` of the job. |
| `--workers N` | `VISIVO_WORKERS` | the value of `--cpus` | Size of the backend's process pool (`VISIVO_WORKERS` in the container). Lets a job take a whole node (`--cpus 128`) while keeping the number of concurrent worker processes, and their CUDA initialisation, small. |
| `--gres G` | `VISIVO_GRES` | `gpu:1` | GPU request (`sbatch --gres`). Empty or `0` requests no GPU and drops `--nv`. |
| `--no-gpu` | | | Same as `--gres ""`. |
| `--port N` | `VISIVO_PORT` | `8000` | Port the backend listens on, on the compute node. |
| `--bind PATH` | `VISIVO_BIND` | `$HOME` | A host path bound into the container at the same path. |
| `--data-root PATH` | `VISIVO_DATA_ROOT` | unset | The data directory, bound at the same path and set as `VISIVO_DATA_ROOT` in the container, so the backend opens nothing outside it. Recommended for any shared or SRC deployment. |
| `--sif PATH` | `VISIVO_SIF` | `apptainer/backend.sif` next to the script | The Apptainer image. |
| `--backend-src PATH` | `VISIVO_BACKEND_SRC_OVERRIDE` | unset | A `backend/app` source tree bound over `/app/app` in the container, to try Python changes without rebuilding the image. |
| `--token T` | `VISIVO_TOKEN` | generated | The backend token; a random one is generated when none is given. |
| `--workspace DIR` | `VISIVO_WORKSPACE` | unset | A writable directory for everything the backend and the launcher write (see below). Needed where `$HOME` is read-only. |
| `--node-code CODE` | `VISIVO_NODE_CODE` | unset | The node's code in the SKAVA catalogue, reported in `/v1/node/status`. |
| `--version TEXT` | `VISIVO_VERSION` | the image's | What the status document reports as the backend version, for example the commit the image was built from. |
| `--help` | | | Print the help. |

`VISIVO_LOG_DIR` (default `~/.visivo_logs`, or `<workspace>/logs` with a
workspace) is where the job log `visivo_backend_<jobid>.log` goes.

The token is saved, readable only by you, to `~/.visivo_token` on the
frontend, or to `<workspace>/token` with a workspace. A job that was given a
GPU declares it in the status document (`VISIVO_NODE_GPU=1`, otherwise `0`).

### The workspace

With `--workspace DIR` the launcher creates and binds (at the same path):

| Path | Used as |
|---|---|
| `DIR/exports` | the backend's workspace exports (`VISIVO_EXPORTS_DIR`): products such as cutouts, moment maps and finder outputs. It is also a data root (`VISIVO_DATA_ROOTS` is the data root, if any, plus this directory), so a product opens where it was written. |
| `DIR/cache/hips`, `DIR/cache/numba`, `DIR/cache/cupy` | the HiPS, Numba and CuPy caches |
| `DIR/logs` | the job logs (unless `VISIVO_LOG_DIR` is set) |
| `DIR/token` | the token file |

### Other backend settings

The launcher passes only the settings above into the container. Any other
backend variable, for example `VISIVO_RENDER_MAX_SESSIONS` or
`VISIVO_FULL_RES_THRESHOLD_BYTES`, can be exported with the `APPTAINERENV_`
prefix in the shell the launcher runs in: `sbatch` hands that environment to
the job, and Apptainer passes `APPTAINERENV_<NAME>` into the container as
`<NAME>`.

```bash
export APPTAINERENV_VISIVO_RENDER_MAX_SESSIONS=4
bash scripts/launch_backend_slurm.sh --sif backend-egl.sif ...
```

## The laptop helper: `visivo_slurm_tunnel.sh`

The helper runs the launcher on the frontend over SSH, reads the job id, the
compute node, the port and the token from its output, and opens the tunnel
`localhost:<port>` to `<node>:<port>` through the frontend.

| Option | Default | Meaning |
|---|---|---|
| `--frontend HOST` | required | The SLURM frontend. |
| `--user USER` | `$USER` | The SSH login on the frontend. |
| `--repo-dir DIR` | `VISIVO_REMOTE_REPO`, else `VisIVOVisualAnalytics` | The clone on the frontend, relative to the remote home. |
| `--port N` | `VISIVO_PORT`, else `18900` | The local port of the tunnel. When given, it is also passed to the launcher, so the backend listens on the same port on the node; otherwise the backend listens on the launcher's default and the tunnel maps one to the other. |
| `--stop` | | Stop the tunnel, cancel the job and remove the token file. |
| any other option | | Passed unchanged to the launcher (`--partition`, `--gres`, `--sif`, `--data-root`, `--node-code`, `--workspace`, ...). |

The helper:

* **checks before submitting anything**: it refuses to start while a tunnel
  from an earlier run is still active (stop it first with `--stop`) or while
  the local port is already in use, so a job is never submitted that its
  tunnel could not reach;
* **keeps the tunnel open**: `ssh` runs in a loop in the background, with
  keep-alives (`ServerAliveInterval=15`, `ServerAliveCountMax=4`) so a dead
  connection is noticed within about a minute, and is started again three
  seconds after it ends (a network change, the laptop sleeping). It runs
  non-interactively (`BatchMode`), so it needs key-based SSH access to the
  frontend;
* **reports ready only when the backend answers through the tunnel**: it polls
  `/v1/health` on the local port for up to 30 s. If the backend does not
  answer, it closes the tunnel and says that the job is still queued or
  running (`--stop` cancels it);
* saves the token in **`~/.visivo_slurm_token`** on the laptop, readable only
  by you. It is never written to `~/.visivo_token`: that file belongs to the
  desktop's local backend, and the desktop would then fail to authenticate
  against it.

`--stop` ends the reconnect loop (and the `ssh` it started), runs `scancel` on
the frontend for the job it recorded (`~/.visivo_slurm_job`) and removes
`~/.visivo_slurm_token`.

## Multi-node backend with Dask

`scripts/launch_backend_slurm_dask.sh` allocates several nodes, starts a Dask
scheduler and Dask workers on the first node, Dask workers on each of the
others, and the backend on the first node with `VISIVO_DASK_SCHEDULER`
pointing at the scheduler, so heavy operations (moment maps, channel maps) are
spread over all the workers. `scripts/visivo_slurm_tunnel_dask.sh` is its
laptop helper.

Launcher options (on the frontend):

| Option | Environment default | Default | Meaning |
|---|---|---|---|
| `--nodes N` | `VISIVO_NODES` | `4` | Nodes to allocate. |
| `--cpus N` | `VISIVO_CPUS` | `32` | CPUs per node. |
| `--workers N` | `VISIVO_WORKERS_PER_NODE` | the value of `--cpus` | Dask workers per node (one thread each). |
| `--gres G` | `VISIVO_GRES` | empty (no GPU) | GPUs per node. |
| `--partition`, `--mem`, `--time`, `--port`, `--bind`, `--data-root`, `--sif`, `--backend-src`, `--token`, `--no-gpu` | as for the single-node launcher | | |

It has no `--workspace`, `--node-code` or `--version`. The scheduler listens
on port 18786 and its dashboard on 18787 of the first node; the backend itself
runs with four local workers. The token is written to `~/.visivo_token` on the
frontend, and the scheduler and worker logs go to `~/.visivo_logs`.

Helper options (on the laptop): `--frontend`, `--user`, `--repo-dir` and
`--port` as above, plus `--dashboard-port P` (the local port of a second tunnel
to the Dask dashboard, default `VISIVO_DASHBOARD_PORT` or `18787`) and
`--no-dashboard` (no dashboard tunnel). Every other option goes to the Dask
launcher. The token is saved in `~/.visivo_slurm_dask_token`; `--stop` closes
both tunnels and cancels the job.

```{note}
The Dask helper is simpler than the single-node one: its tunnels are plain
`ssh -N -f` processes that are not reopened when they drop, it does not check
for an earlier tunnel or a busy port, and it does not wait for the backend to
answer through the tunnel.
```

## Apptainer images

The definitions are in `apptainer/`. Build them from the repository root (the
paths in their `%files` and `%setup` sections are relative to it).

| Definition | Base | What it contains |
|---|---|---|
| `backend.def` | `python:3.12-slim` | The slim image. No VTK, Numba or llvmlite (no usable install on POWER9 from PyPI): no isosurfaces, no velocity-field products, no server-side rendering, and the moment kernels run unaccelerated. `ENABLE_CUPY=1` and `ENABLE_SOFIA=1` at build time add CuPy and SoFiA-2 (export them as `APPTAINERENV_ENABLE_CUPY` and `APPTAINERENV_ENABLE_SOFIA`: Apptainer passes only prefixed variables into the build). |
| `backend-conda.def` | `condaforge/miniforge3` | The full scientific stack from conda-forge (Numba, VTK, healpy), for ppc64le, x86_64 and aarch64, with SoFiA-2 (from conda-forge, or built from source where there is no package; the build does not fail without it). CuPy with `APPTAINERENV_ENABLE_CUPY=1` at build time. The recommended base for an SRC node. |
| `backend-x86.def` | `condaforge/mambaforge` | x86_64 with NVIDIA Blackwell GPUs (CUDA 12.8 runtime, CuPy on by default; NVIDIA driver 570 or later on the host). |
| `backend-conda-egl.def` | `backend-conda.sif` (local image) | The conda image with VTK rebuilt from source with EGL, for server-side 3-D rendering on the node's GPU. Built as `backend-egl.sif`. |
| `backend-egl-h265-overlay.def` | `backend-egl.sif` (local image) | **Not up to date**, as its own help says: it targets an earlier EGL image that kept the stack in conda's base environment, so it would install FFmpeg, PyAV and aiortc where the backend does not look. Server-side rendering does not need it (the JPEG stream is the default transport). |

```{important}
The launcher's default image (`apptainer/backend.sif`) cannot render
server-side, and neither can `backend-conda.sif` on POWER9. For the 3-D view to
be rendered on the node, start the job with `--sif backend-egl.sif`. Without
it the backend reports server-side rendering as unavailable and the desktop
renders a decimated proxy of the cube locally.
```

### Building the EGL image

conda-forge publishes VTK for `linux-ppc64le` only as the Qt/X11 build (9.2.6,
9.3.0, 9.3.1); the EGL and OSMesa variants exist for `linux-64` only. The EGL
definition is therefore a layer on the conda image that removes conda-forge's
VTK and its OpenGL libraries and builds VTK (tag `v9.3.1` by default) headless
with EGL, keeping everything else of the base. The build fails if
`vtkEGLRenderWindow` is not built or not linked to `libEGL`.

In the directory that holds the conda image under the name `backend-conda.sif`
(a symlink is enough):

```bash
ln -sf backend-conda-gpu.sif backend-conda.sif
apptainer build --force --fakeroot backend-egl.sif VisIVOVisualAnalytics/apptainer/backend-conda-egl.def
```

`--force` is needed, or Apptainer keeps the base image's help and labels.
`APPTAINERENV_VTK_TAG` selects another VTK tag. The definition estimates 70 to
90 minutes on 16 POWER9 cores; on PLEIADI-GPU, with 32 cores, it took about 20
minutes. Build where Apptainer's temporary directory (`APPTAINER_TMPDIR`) is
node-local, for example in a SLURM job on a compute node.

### What the launcher adds for EGL

Every job (the bindings are harmless where the libraries do not exist) gets:

* the host's NVIDIA libraries (`libEGL_nvidia`, `libcuda`, `libnvidia-*`, from
  `/usr/lib64` or `/usr/lib/powerpc64le-linux-gnu`) and the glvnd vendor file
  `10_nvidia.json`, bound at their host paths, because Apptainer's `--nv`
  misses several of them on some ppc64le installs; `LD_LIBRARY_PATH=/usr/lib64`
  and `__EGL_VENDOR_LIBRARY_FILENAMES` pointing at the NVIDIA vendor file;
* `XDG_CACHE_HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and `MPLCONFIGDIR` under
  `/tmp`, because fontconfig takes a file lock in `~/.cache`, and on an
  NFS-mounted home that lock can hang the first render for ever.

## Node capacity under SLURM

The backend reads the memory it may use where it is true for its process
(`backend/app/node_status.py`):

1. the **cgroup v2** hierarchy: `memory.max` and usage at its own cgroup and
   every ancestor it can see, the tightest limit winning;
2. if no v2 memory files are readable, the **cgroup v1** memory controller
   (`memory.limit_in_bytes`, `memory.usage_in_bytes`), as on older SLURM
   clusters;
3. inside a SLURM job (`SLURM_JOB_ID` set), the **memory SLURM granted** as one
   more limit: `SLURM_MEM_PER_NODE`, or `SLURM_MEM_PER_CPU` times
   `SLURM_CPUS_ON_NODE`. A grant counts even where the cluster does not enforce
   it with a cgroup; `--mem=0` (the whole node) sets no cap;
4. otherwise the host's memory (`/proc/meminfo`).

Available memory is the limit minus the working set (usage without the
reclaimable file cache), never more than the host has left.
`VISIVO_CGROUP_ROOT` names the cgroup directory explicitly (and then cgroup v1
is not consulted).

This matters because the backend sizes its work by it rather than by the
host's RAM: the full-resolution threshold (see
[Large cube loading](../large-cube-loading.md)) is at most a quarter of that
limit, the compute paths choose between holding a range in memory and
streaming it from the available figure, and the node status that a catalogue
ranks nodes on reports the same numbers (see
[The backend as an SRC node](src-node.md)). A job given `--mem 64G` on a
node with 512 GB is a 64 GB backend.

## Server-side rendering on the node

With the EGL image, each remote 3-D view is a render session that runs in a
**child process of its own** (`backend/app/render_session/_process.py`): VTK
and the JPEG encoder then hold only that process's GIL, so the backend keeps
receiving commands and sending frames while the next frame is rendered, and a
crash in VTK or the driver ends that session, not the backend.

Sessions are **spread over the job's GPUs** in turn. EGL lists every GPU of
the node whatever `CUDA_VISIBLE_DEVICES` says, so each session asks the driver
for the CUDA device behind each EGL device (`EGL_NV_device_cuda`): only the
job's GPUs answer, and the session binds VTK to one of them through
`VTK_EGL_DEVICE_INDEX`. If no EGL device has that extension, the backend logs
a warning and VTK picks the device; if the mapping shows none of the node's
GPUs in the job, the session does not start.

| Variable | Default | Meaning |
|---|---|---|
| `VISIVO_RENDER_MAX_SESSIONS` | `8` | Remote views the backend accepts at once; beyond it a new one is refused with HTTP 503. |
| `VISIVO_RENDER_IDLE_TTL_SECONDS` | `300` | A view with no command for this long is closed. |
| `VISIVO_RENDER_IN_PROCESS` | unset | `1` renders in the backend's own process instead (slower: rendering then holds the process that serves the WebSocket). |
| `VTK_EGL_DEVICE_INDEX` | unset | Binds every view to that EGL device instead of spreading them over the job's GPUs. |

How the view streams frames and lowers its quality while you rotate is
described for users in the
[Cube viewer](../user-guide/cube-viewer.md#where-the-rendering-happens-auto-local-remote)
page. On PLEIADI-GPU (POWER9, V100), through an SSH tunnel, a 2540 x 1560 view
of a 2 GB cube ran at about 26 frames per second once the drag level had
settled; four views at once ran at 20.0 frames per second each when spread over
four V100s, against 6.8 on one. The scripts that measure this and the recorded
runs are in
[`benchmark/remote_render`](https://github.com/VisIVOLab/VisIVOVisualAnalytics/tree/main/benchmark/remote_render).

A complete, recorded run on PLEIADI-GPU together with a SKAVA catalogue is
described in the
[SKAVA documentation](https://skava.readthedocs.io/en/latest/operations/e2e-pleiadi-gpu-runbook.html).

## Troubleshooting

**The job stays pending.** The launcher waits about 60 seconds for SLURM to
assign a node, then stops with `Could not determine compute node`, but the job
stays queued. The tunnel helper then cannot read the node and has recorded no
job, so `--stop` does not know it: check it with `squeue -j <jobid>` on the
frontend and `scancel` it yourself, or ask for fewer resources (a shorter
`--time`, fewer GPUs, another `--partition`).

**The backend does not become ready.** The launcher polls for five minutes
(ten for the Dask variant), then names the job log. Look there first: a
missing image, a data root that does not exist, or a Python error at start.

**The local port is in use.** The helper refuses to start
(`localhost:<port> is already in use`). Stop what holds it (often the
desktop's local backend on `8000`) or choose another `--port`.

**A tunnel from an earlier run is still active.** The helper refuses to start
until it is stopped: `bash scripts/visivo_slurm_tunnel.sh --stop`.

**The tunnel dropped.** The helper reopens it by itself within a few seconds
of the connection ending, and keep-alives notice a dead connection within
about a minute. If it does not come back, check that `ssh <user>@<frontend>`
still works without a password, and that the job is still running: when the
job ends (its `--time` is over) there is nothing left to tunnel to.

**The desktop gets 401 from the cluster backend.** The token in the desktop's
entry is not the job's. Each job has its own token unless `--token` is given:
copy it again from `~/.visivo_slurm_token` (single node) or
`~/.visivo_slurm_dask_token` (Dask), never from `~/.visivo_token`, which is
the local backend's. If the entry's URL is a loopback address on the local
backend's port, the desktop sends it the local token, when there is one,
whatever the entry says: use another `--port`.

**The 3-D view is never rendered on the node.** The backend has no EGL. The
dataset-open response reports `server_side_rendering_available: false`, the
Remote mode is disabled, and the desktop renders a decimated proxy of the cube
locally. Start the job with `--sif backend-egl.sif` and a GPU (`--gres`).
`apptainer test backend-egl.sif` runs the image's own check, which fails when
the backend would not offer server-side rendering.
