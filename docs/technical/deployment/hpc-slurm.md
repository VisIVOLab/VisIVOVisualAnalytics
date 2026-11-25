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

On CINECA Leonardo use `--site leonardo`, see [CINECA Leonardo](#cineca-leonardo).

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
compute node to be assigned (up to `VISIVO_ASSIGN_TIMEOUT` seconds, 300 by
default, set in the frontend's environment), then polls the backend's `/v1/health` over SSH
from the frontend to the compute node until it answers. It then prints the job
id, the compute node, the port, the `ssh -N -L` command to run on the laptop,
and the entries for the desktop's Backends page.

| Option | Environment default | Default | Meaning |
|---|---|---|---|
| `--site NAME` | `VISIVO_SITE` | detected from the host name, else `generic` | The [site profile](#site-profiles): `leonardo`, `pleiadi` or `generic`. |
| `--partition P` | `VISIVO_PARTITION` | `gpu` (the site's, with a profile) | SLURM partition. |
| `--time HH:MM:SS` | `VISIVO_TIME` | `08:00:00` | Wall-clock limit of the job. |
| `--mem M` | `VISIVO_MEM` | `64G` | Memory per node, passed to `sbatch --mem` as given. |
| `--cpus N` | `VISIVO_CPUS` | `8` | `--cpus-per-task` of the job. |
| `--workers N` | `VISIVO_WORKERS` | the value of `--cpus` | Size of the backend's process pool (`VISIVO_WORKERS` in the container). Lets a job take a whole node (`--cpus 128`) while keeping the number of concurrent worker processes, and their CUDA initialisation, small. |
| `--gres G` | `VISIVO_GRES` | `gpu:1` | GPU request (`sbatch --gres`). Empty or `0` requests no GPU and drops `--nv`. |
| `--no-gpu` | | | Same as `--gres ""`. |
| `--port N` | `VISIVO_PORT` | `8000` | Port the backend listens on, on the compute node. |
| `--bind PATH` | `VISIVO_BIND` | `$HOME` | A host path bound into the container at the same path. |
| `--data-root PATH` | `VISIVO_DATA_ROOT` | unset | The data directory, bound at the same path and set as `VISIVO_DATA_ROOT` in the container, so the backend opens nothing outside it. Recommended for any shared or SRC deployment. |
| `--sif PATH` | `VISIVO_SIF` | `apptainer/backend.sif` next to the script, or the file `--image` pulls | The container image (a `.sif`). |
| `--image REF` | `VISIVO_IMAGE` | unset | An image to pull once on the frontend, for example `docker://ghcr.io/visivolab/visivo-backend:<version>-egl`. It goes to `--sif` when given, else to `<workspace>/images/` (`~/.visivo_images` without a workspace), and is reused while the file exists: delete it to pull a moving tag again. Pulling happens in the launcher, on the frontend, because compute nodes often have no internet. |
| `--cleanenv`, `--no-cleanenv` | `VISIVO_CLEANENV` (`1` or `0`) | on for `leonardo`, off elsewhere | Run the container with `--cleanenv`, so that the host's environment (proxy and CA-bundle variables, for example) does not reach it. What reaches the container is passed by name: the variables the launcher sets for the backend, `SLURM_JOB_ID`, `SLURM_MEM_PER_NODE`, `SLURM_MEM_PER_CPU`, `SLURM_CPUS_ON_NODE`, `SLURM_CPUS_PER_TASK`, `CUDA_VISIBLE_DEVICES` and `NVIDIA_VISIBLE_DEVICES` (as the job sees them), and every `VISIVO_*` variable plus `VTK_EGL_DEVICE_INDEX` exported when the launcher is run (for example `VISIVO_RENDER_MAX_SESSIONS`, `VISIVO_HEAVY_TIMEOUT_S`, `VISIVO_PYRAMID_DIR`). The launcher's own options (`VISIVO_ACCOUNT`, `VISIVO_PARTITION`, `VISIVO_IMAGE`, `VISIVO_SITE` and the like) and the token are not forwarded as they are, and a variable the launcher has already set is not overridden. |
| | `VISIVO_CONTAINER_RUNTIME` | `apptainer`, else `singularity` | The container command, when neither the default nor the fallback is the one to use. |
| `--backend-src PATH` | `VISIVO_BACKEND_SRC_OVERRIDE` | unset | A `backend/app` source tree bound over `/app/app` in the container, to try Python changes without rebuilding the image. The launcher refuses a directory that is not the package (no `__init__.py`), and prints which copy runs (path, git commit, uncommitted changes, newest file) in its output, in the job log and in the ready box: a stale copy on the cluster looks like a new route answering 404. |
| `--token T` | `VISIVO_TOKEN` | generated | The backend token; a random one is generated when none is given. |
| `--workspace DIR` | `VISIVO_WORKSPACE` | unset | A writable directory for everything the backend and the launcher write (see below). Needed where `$HOME` is read-only. |
| `--node-code CODE` | `VISIVO_NODE_CODE` | unset | The node's code in the SKAVA catalogue, reported in `/v1/node/status`. |
| `--account ACCOUNT` | `VISIVO_ACCOUNT` | unset | The SLURM account (project) charged for the job; required on CINECA Leonardo and BSC MareNostrum 5. |
| `--qos QOS` | `VISIVO_QOS` | unset | The SLURM quality of service. |
| `--job-name NAME` | `VISIVO_JOB_NAME` | `visivo_backend` | The SLURM job name. The desktop gives each job a unique name, so that it can cancel a job whose id it never received. |
| `--version TEXT` | `VISIVO_VERSION` | the image's | What the status document reports as the backend version, for example the commit the image was built from. |
| | `VISIVO_ASSIGN_TIMEOUT` | `300` | Seconds to wait for SLURM to assign the node before the launcher stops (the job stays queued; `--stop` cancels it). |
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
| `--port N` | `VISIVO_PORT`, else `18900` | The local port of the tunnel. When `--port` is given, it is also passed to the launcher, so the backend listens on the same port on the node; otherwise the backend listens on the launcher's default and the tunnel maps one to the other. Without `--port` (`VISIVO_PORT` only changes the default), a port that is taken, or that is the desktop client's own local backend port, is replaced by the next free one. |
| `--stop` | | Stop the tunnel, cancel the job and remove the token file. |
| any other option | | Passed unchanged to the launcher (`--partition`, `--gres`, `--sif`, `--data-root`, `--node-code`, `--workspace`, ...). |

The helper:

* **checks before submitting anything**: it refuses to start while a tunnel
  from an earlier run is still active (stop it first with `--stop`), so a job
  is never submitted that its tunnel could not reach. The local port must be
  free and must not be the port of the desktop client's own local backend
  (read from the client's saved Backend URL, `http://127.0.0.1:8000` by
  default): the client starts that backend when it opens, possibly after the
  tunnel is up. Without `--port` the helper takes the next free port; with
  `--port` it stops and says why;
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

### Connecting from the desktop

The desktop can do all of the above itself. In the Data Hub, **Connect to a
cluster…** keeps named profiles: the frontend and your login there, the VisIVO
clone on the frontend (for `scripts/launch_backend_slurm.sh`), the SLURM
resources (partition, account, QOS, GPUs, CPUs, memory, time), the backend image, the data
root, the workspace, and any further launcher options. **Connect** opens a new
VisIVO window on that cluster; the window you were using stays on its own
backend.

The new window, before it opens:

* submits the backend job over `ssh` (OpenSSH, as on macOS, Linux and
  Windows 10 and later) with key-based access to the frontend, since nothing
  can be typed: set it up once with `ssh-copy-id`;
* reads the job, the compute node, the port and the token from the launcher
  (the token is never shown);
* opens the tunnel on a free local port that is not the desktop's own local
  backend port, and opens it again if it drops;
* waits for the backend to answer, then runs on it as with `--backend` below.

The window owns the job. Closing it, or **Disconnect from NAME** in its Data
Hub, closes the tunnel and cancels the job (`scancel`). Cancel on the progress
screen does the same at any stage. Only one window per profile can be open; a
job left by a window that did not close normally is cancelled by the next one
for the same profile (each job is recorded, under its unique name, until a
cancel succeeds). From a terminal, the same is
`VisIVOVisualAnalytics --connect-cluster NAME`; a dataset path after it is a
path on the cluster, opened once the window is connected
(`--connect-cluster NAME /scratch/me/data/cube.fits`).

Recent Datasets remembers where each dataset was opened, and marks those
opened on a cluster with its name. All windows share the list. Clicking an
entry from a window on another backend does not try a path that backend does
not have: for a cluster entry it offers to open a window on that cluster and
reopens the dataset there (`--connect-cluster NAME --reopen-recent PATH`);
otherwise it says where the dataset was opened.

### Opening the desktop client on the cluster backend

Started with no options, the desktop client runs on its own local backend. To
use the cluster backend instead, start it with the command the helper prints:

```bash
# macOS
open -n -a VisIVOVisualAnalytics --args --backend http://localhost:18901 \
    --backend-token-file ~/.visivo_slurm_token --backend-name PLEIADI
# Linux, from the extracted package
./VisIVOVisualAnalytics --backend http://localhost:18901 \
    --backend-token-file ~/.visivo_slurm_token --backend-name PLEIADI
```

| Option | Environment | Meaning |
|---|---|---|
| `--backend URL` | `VISIVO_BACKEND_URL` | The backend this run uses. |
| `--backend-token-file FILE` | `VISIVO_BACKEND_TOKEN_FILE`, or the token itself in `VISIVO_BACKEND_TOKEN` | Its token, read from a file so that it does not appear in the process list. |
| `--backend-name NAME` | `VISIVO_BACKEND_NAME` | The name shown in the status bar and the backend chip (default: `host:port`). |

For that run only:

* the client starts no local backend and sends only this backend's token,
  never the local one from `~/.visivo_token`, even though the tunnel is on
  `localhost`;
* the saved settings, the list of remote backends and `~/.visivo_token` are
  not changed. The Settings dialog shows the backend URL and token read-only.
  The next plain launch is local again;
* the status bar and the backend chip name the backend, and the 3-D view
  offers Remote rendering (View ▸ Rendering, or Auto / Local / Remote in the
  view's header);
* an option given without its value, a malformed URL, or an empty or
  unreadable token file stops the launch with an error, rather than starting
  on the local backend.
* a dataset path after the options is a path on the backend, opened there
  as *Open…* would open it:

  ```bash
  VisIVOVisualAnalytics --backend http://localhost:18901 \
      --backend-token-file ~/.visivo_slurm_token --backend-name PLEIADI \
      /scratch/me/data/cube.fits
  ```

  If the backend does not find it, or it lies outside the folders the backend
  may read, the window says so. On the local backend a path that does not
  exist on this machine is reported too, rather than ignored.

If the backend does not answer, the start-up screen names it and says what to
check: the job, the tunnel, and that the token is this job's (a new job has a
new token).

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

The Dask helper behaves like the single-node one: it refuses to start while
an earlier tunnel or job is recorded or a local port is taken (and when
`--port` and `--dashboard-port` are the same), reopens either tunnel when it
drops, records the job as soon as the launcher prints its id, and is ready
only when the backend answers through the tunnel.

## Site profiles

`--site` (or `VISIVO_SITE`) picks a profile; without it the launcher looks at
the frontend's host name (a CINECA Leonardo host, that is `login*.leonardo.local`, `*.leonardo.cineca.it` or a `lrdn*`/`lrdl*` node name, selects `leonardo`; `pleiadi` in the name selects `pleiadi`; any other host is `generic`; case is ignored) and prints what it chose. A profile only supplies
defaults for what no option or variable gives, so a site with no profile works
with plain options (`generic`).

| Profile | What it sets |
|---|---|
| `generic` | Nothing: the options and the defaults of the table above. |
| `pleiadi` | The same as `generic`: those defaults were written for PLEIADI-GPU. |
| `leonardo` | Partition `boost_usr_prod`; `--account` required; `--cleanenv`; the workspace (`<scratch>/visivo`: logs, token, caches, exports, pulled images) and the data root (`<scratch>`) on the user's scratch area; image caches (`APPTAINER_CACHEDIR`, `SINGULARITY_CACHEDIR` and their `TMPDIR`) there too; the host's glvnd vendor directory bound for EGL; no `LD_LIBRARY_PATH=/usr/lib64`; the readiness check falls back to `srun` when `ssh` to the node is refused. |

The container runtime is found at launch: `apptainer` if it exists, else
`singularity`. With Singularity (SingularityCE, SingularityPRO) the job script
hands every `APPTAINERENV_<NAME>` on as `SINGULARITYENV_<NAME>`, so the
variables of [Other backend settings](#other-backend-settings) work with either
runtime.

## CINECA Leonardo

The backend runs on a GPU node of the booster partition (A100) next to the
simulation data on the shared scratch, and renders on it. On Leonardo the
runtime is `singularity` (SingularityPRO; there is no `apptainer`), the
partition is `boost_usr_prod` (24 h limit), a project account is mandatory
(`--account`), and the quality of service `boost_qos_dbg` is for a short
test. The visualisation nodes of `lrd_all_viz` are used with
`--partition lrd_all_viz`; the CPU partition is `dcgp_usr_prod` with `--no-gpu`.

From a login node, with a clone of the repository in your home (a few
megabytes), start a GPU backend. The image is pulled once, on the login node:

```bash
bash VisIVOVisualAnalytics/scripts/launch_backend_slurm.sh \
    --site leonardo --account <ACCOUNT> \
    --image docker://ghcr.io/visivolab/visivo-backend:<version>-egl \
    --data-root /path/to/the/simulation/data
```

It prints the compute node, the log file (on scratch) and the tunnel command
for your laptop. From the laptop, the same in one step, tunnel included:

```bash
bash scripts/visivo_slurm_tunnel.sh --frontend <login host> --user <user> \
    --site leonardo --account <ACCOUNT> \
    --image docker://ghcr.io/visivolab/visivo-backend:<version>-egl \
    --data-root /path/to/the/simulation/data
```

* **The account and paths** are yours: nothing of them is written into the
  repository. `--data-root` jails the backend to that directory: by default it
  is your scratch area (`$CINECA_SCRATCH`, else `$SCRATCH`, else `$WORK`), so
  pass the directory that holds the data when it lies elsewhere. A snapshot is
  opened in place, nothing is copied.
* **Access.** The login nodes are reached with a short-lived certificate (the
  site's two-factor step). The tunnel helper is non-interactive, so obtain the
  certificate first and keep it valid: the tunnel reopens itself only while it
  is. The tunnel goes through the login node to the compute node's port; it
  does not need `ssh` to the compute node, but the launcher's readiness check
  tries it, and runs the check through `srun` when it is refused.
* **The image.** `--image` needs a login node (compute nodes may have no
  internet). To use an image pulled earlier, give `--sif <file>`.
  The image must be the EGL build for amd64 (`...-egl`): the 3-D view is
  rendered on the node only with it.
* **Why `--cleanenv`.** The host exports `REQUESTS_CA_BUNDLE` and pip's CA
  variables; inside a Debian-based image they point at files that are not
  there and break Python's TLS. The profile therefore cleans the environment
  and passes only what the backend needs (listed under `--cleanenv`).
* **EGL.** `--nv` binds `libEGL`, `libEGL_nvidia` and `libGLdispatch`, but not
  the glvnd vendor file that points at `libEGL_nvidia`. The job binds the
  host's `/usr/share/glvnd/egl_vendor.d` and sets
  `__EGL_VENDOR_LIBRARY_FILENAMES` to its `10_nvidia.json`.
* **Trying a branch.** `--backend-src <checkout>/backend/app` overlays the
  image's backend code; the launcher prints which copy runs. Copy a fresh
  checkout each time: a copy left from an earlier day is the usual reason a
  new route answers 404.
* **Stopping.** `scancel <job id>`, or `bash scripts/visivo_slurm_tunnel.sh
  --stop` from the laptop (tunnel and job).

The multi-node Dask launcher has no site profile yet: it takes plain options
only (and no `--account` or `--qos`).

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
On a site whose compute nodes have no internet, or on x86_64, see [Building the image yourself](#building-the-image-yourself): the same definition builds in two steps, from a VTK source tree prepared on the login node.
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
  (not with the `leonardo` profile, which binds the vendor directory instead)
  and `__EGL_VENDOR_LIBRARY_FILENAMES` pointing at the NVIDIA vendor file;
* `XDG_CACHE_HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and `MPLCONFIGDIR` under
  `/tmp`, because fontconfig takes a file lock in `~/.cache`, and on an
  NFS-mounted home that lock can hang the first render for ever.

## Published container images

Each release publishes the backend as an OCI image on the GitHub Container
Registry, so a site pulls it instead of building it (the recipe is
`docker/Dockerfile`, the workflow `.github/workflows/backend-image.yml`). To
build it yourself (`docker build -f docker/Dockerfile --target cpu .` from the
repository root, `--target egl` for the EGL one, which compiles VTK) or through
`docker compose build`, BuildKit is required: it is the default of Docker 23 and
later, and `DOCKER_BUILDKIT=1` turns it on in an older one. To turn the result into a `.sif`, or to build on the cluster, see [Building the image yourself](#building-the-image-yourself).

| Image | Platforms | What it is |
|---|---|---|
| `ghcr.io/visivolab/visivo-backend:<version>` (and `latest`) | amd64, arm64 | The analysis API with the full scientific stack and SoFiA-2. Server-side 3-D rendering reports itself unavailable. |
| `ghcr.io/visivolab/visivo-backend:<version>-egl` | amd64 | The same, with VTK built from source with EGL, so a node with an NVIDIA GPU renders the cube itself. |

The images run as a non-root user and write nothing inside themselves. Their
entrypoint reads:

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_TOKEN` | a random token, printed at start (never written to `HOME`) | The access token. Pass it in the environment, not on a command line. |
| `PORT` | `8000` | The port (`VISIVO_HOST`, default `0.0.0.0`, the address). |
| the data | `/data` | The directory the data is bound at. When `/data` exists (or `VISIVO_DATA_ROOT` names another directory that does), the backend is confined to it and to the exports directory. `VISIVO_DATA_ROOTS` sets the roots outright. With none of these the image **refuses to start**: an unconfined backend opens any file its user can read, and under Apptainer that includes the whole home directory (`~/.ssh`) for anyone holding the token. `VISIVO_ALLOW_UNRESTRICTED=1` starts it anyway, for a machine where that is acceptable. |
| `VISIVO_EXPORTS_DIR` | scratch under `/tmp` | Where products are written. Bind a directory here to keep them. |
| `VISIVO_WORKERS` | `4` | The size of the backend's process pool for the heavy computations. The server itself is one uvicorn worker (`--workers 1`): this variable does not add HTTP workers. |

Caches and libraries' configuration go to `${TMPDIR:-/tmp}/visivo-<uid>`
(`VISIVO_SCRATCH` moves it), never to `HOME`.

**Docker or Podman**

```bash
docker run --rm -p 8000:8000 \
    -e VISIVO_TOKEN=<token> \
    -v /path/to/data:/data:ro \
    ghcr.io/visivolab/visivo-backend:<version>
```

For rendering, `--gpus all` (the NVIDIA Container Toolkit) with the `-egl` image;
for a read-only root file system, `--read-only --tmpfs /tmp`. In the desktop,
register `http://<host>:8000` and the token under **Settings ▸ Backends ▸ Remote
Backends**. `docker/smoke_test.sh <image>` runs the checks CI runs: it starts the
image as a locked-down site would, calls `/v1/health` and opens a small cube,
and checks that it refuses to start with no data directory and that a modified
copy of `backend/app` bound onto `/app/app` is the code that runs.

**Apptainer or Singularity, on an HPC system**

```bash
# The image cache is large: keep it off the home directory.
export APPTAINER_CACHEDIR=/path/on/scratch/apptainer-cache      # SINGULARITY_CACHEDIR on Singularity
apptainer pull visivo-backend-egl.sif docker://ghcr.io/visivolab/visivo-backend:<version>-egl

# On a GPU node. VISIVO_TOKEN reaches the container through APPTAINERENV_.
export APPTAINERENV_VISIVO_TOKEN=<token>
export APPTAINERENV_PORT=8000
# --cleanenv drops SLURM's GPU assignment: pass it on, or the backend sees every GPU of the node.
[ -z "${CUDA_VISIBLE_DEVICES:-}" ] || export APPTAINERENV_CUDA_VISIBLE_DEVICES="$CUDA_VISIBLE_DEVICES"
apptainer run --nv --cleanenv \
    --bind /path/to/data:/data \
    --bind /usr/share/glvnd/egl_vendor.d:/usr/share/glvnd/egl_vendor.d \
    --env __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json \
    visivo-backend-egl.sif
```

`apptainer` and `singularity` take the same options here (`singularity pull
docker://...` is the same command). The CPU image needs neither `--nv` nor the
two EGL lines. Two points are specific to the EGL image:

* Singularity's `--nv` binds the host's `libEGL_nvidia` but not the glvnd vendor
  file that names it, so libglvnd finds no EGL vendor and the render window
  cannot be created. Binding the host's vendor directory
  (`/usr/share/glvnd/egl_vendor.d`, the whole directory, as the launcher's site
  profiles do) and pointing `__EGL_VENDOR_LIBRARY_FILENAMES` at its
  `10_nvidia.json` fixes that. Where the host has no such directory, leave the
  bind out: the image carries a file of that name that names
  `libEGL_nvidia.so.0`.
* The data is bound at `/data` here, which is also what confines the backend to
  it (see the table above).
* `--cleanenv` keeps the host's environment (a site's module paths, its
  `LD_LIBRARY_PATH`) out of the container, so the libraries are the image's and
  the ones `--nv` adds.

Run it in a job, not on the frontend, and reach it from the desktop through an
SSH tunnel as described [above](#quick-start). The launcher runs these images too: with
the site profile (`--site leonardo`, see [Site profiles](#site-profiles) and
[CINECA Leonardo](#cineca-leonardo)) it pulls `--image
docker://ghcr.io/visivolab/visivo-backend:<version>-egl` (or takes a pulled
file with `--sif`), binds the host's glvnd vendor directory, leaves
`LD_LIBRARY_PATH` alone and sets the data root, as in the example there:

```bash
bash scripts/launch_backend_slurm.sh --site leonardo --account <ACCOUNT> \
    --image docker://ghcr.io/visivolab/visivo-backend:<version>-egl \
    --data-root /path/to/the/data
```

`--backend-src <checkout>/backend/app` overlays the image's backend code: the
image keeps it at `/app/backend/app` and `/app/app` is a link to it, so the
launcher's bind onto `/app/app` replaces it.

## Building the image yourself

To use the backend on an x86_64 site without pulling the published image (a site
with no route to the registry, or a modified backend), build it by one of two
routes. Both produce a `.sif` that the launcher takes with `--sif`.

| | Route A: on a workstation | Route B: on the cluster |
|---|---|---|
| Needs | Docker or Podman on any x86_64 machine | Apptainer or Singularity with `--fakeroot` |
| Recipe | `docker/Dockerfile`, as the published images | `apptainer/backend-conda.def` and `backend-conda-egl.def` |
| Python stack | PyPI | conda-forge |
| Entrypoint | `docker/entrypoint.sh` | the definition's `%runscript` |

Both pass `server_side_rendering_available()` (the EGL variant, with VTK 9.3.1
built from source with EGL). They differ in the stack and the entrypoint, below.
Route B has not been run on Leonardo: its steps are those proven on PLEIADI,
with the changes described here for a site without internet on the compute nodes.

### Route A: Docker or Podman, then `docker/build_sif.sh`

From a checkout, on an x86_64 machine:

```bash
docker/build_sif.sh --target egl --out visivo-backend-egl.sif   # --target cpu for the CPU image
scp visivo-backend-egl.sif <login host>:<scratch>/
bash scripts/launch_backend_slurm.sh --site leonardo --account <ACCOUNT> \
    --sif <scratch>/visivo-backend-egl.sif --data-root /path/to/the/data
```

The script builds `docker/Dockerfile` for `linux/amd64` (Docker, or Podman with
`DOCKER=podman`), saves the image to an archive and converts it with `apptainer
build` or `singularity build` from `docker-archive://`, whichever exists. The
`egl` target compiles VTK, about an hour. On an Apple Silicon Mac `linux/amd64`
is emulated and the `egl` build takes many hours: use an x86_64 machine for it.
`--tag` names the image in the local store, `--keep-archive` keeps the archive,
and what follows `--` goes to the build (`-- --build-arg VTK_TAG=v9.3.1`).

Without `apptainer` or `singularity` on the machine, the script stops at the
archive and prints the conversion for the cluster:

```bash
scp visivo-backend-egl.tar <login host>:<scratch>/
# on a login node
export SINGULARITY_CACHEDIR=<scratch>/singularity-cache SINGULARITY_TMPDIR=<scratch>/singularity-tmp
mkdir -p "$SINGULARITY_CACHEDIR" "$SINGULARITY_TMPDIR"
singularity build visivo-backend-egl.sif docker-archive://<scratch>/visivo-backend-egl.tar
```

The Apptainer and SingularityCE manuals show this conversion run without `sudo`
or `--fakeroot`, but do not state that it needs no privilege; a site's
SingularityPRO may differ. If the command is refused, add `--fakeroot` (check
that it works with Route B's test below). The conversion unpacks the image under
`SINGULARITY_TMPDIR`, so keep it off a small home directory.

### Route B: on the cluster with `--fakeroot`

First check that the site allows it, on a login node:

```bash
singularity build --fakeroot test.sif docker://debian:bookworm-slim && rm test.sif
```

(`apptainer` where it is the runtime; there, pass the build variables below as `APPTAINERENV_<NAME>`, which Apptainer prefers to `SINGULARITYENV_<NAME>`.) Three constraints shape the build on
CINECA Leonardo and similar sites: the login nodes have internet but a CPU-time
limit per process, the compute nodes have no internet, and the VTK compile
(70 to 90 minutes on 16 cores) belongs on a compute node. The EGL definition
therefore builds in two steps, the first of which needs the internet (apt) and
the second only CPU time (VTK), given a VTK source tree cloned beforehand.

On a login node:

```bash
B=<scratch>/visivo-build
export SINGULARITY_CACHEDIR=$B/cache SINGULARITY_TMPDIR=$B/tmp
mkdir -p "$SINGULARITY_CACHEDIR" "$SINGULARITY_TMPDIR"
git clone https://github.com/VisIVOLab/VisIVOVisualAnalytics.git $B/VisIVO
cd $B/VisIVO     # the definition reads backend/ relative to here

# 1. The conda stack (about 5 minutes). CuPy for the GPU kernels: APPTAINERENV_ENABLE_CUPY=1 (SINGULARITYENV_ on Singularity)
singularity build --fakeroot --force ../backend-conda-base.sif apptainer/backend-conda.def

# 2. Compilers and EGL headers, conda's X11 VTK removed (minutes). The base is read from the
#    current directory under the name backend-conda.sif; --notest because the checks need VTK.
cd $B && ln -sf backend-conda-base.sif backend-conda.sif
SINGULARITYENV_VTK_STAGE=deps singularity build --fakeroot --force --notest \
    backend-egl-deps.sif VisIVO/apptainer/backend-conda-egl.def

# 3. The VTK source, once (a few hundred MB)
git clone --depth 1 --branch v9.3.1 https://gitlab.kitware.com/vtk/vtk.git $B/vtk-src
```

Then the VTK build on a compute node, `build-vtk.sbatch`:

```bash
#!/bin/bash
#SBATCH --job-name=visivo-vtk
#SBATCH --partition=<cpu partition>
#SBATCH --account=<ACCOUNT>
#SBATCH --nodes=1 --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --mem=64G
#SBATCH --time=04:00:00
#SBATCH --output=%x-%j.log
set -e
B=<scratch>/visivo-build
export SINGULARITY_CACHEDIR=$B/cache SINGULARITY_TMPDIR=$B/tmp
cd $B
ln -sf backend-egl-deps.sif backend-conda.sif       # the step-2 image is the base now
VTK_SRC=$B/vtk-src SINGULARITYENV_VTK_STAGE=build \
    singularity build --fakeroot --force backend-egl.sif VisIVO/apptainer/backend-conda-egl.def
```

`sbatch build-vtk.sbatch`; `scancel <jobid>` stops it. The compile uses at most
half the memory in GB as ninja jobs (64 GB: 32 jobs). The result is
`$B/backend-egl.sif`; run it with `--sif`:

```bash
bash VisIVO/scripts/launch_backend_slurm.sh --site leonardo --account <ACCOUNT> \
    --sif $B/backend-egl.sif --data-root /path/to/the/data
```

What the definition does for this:

* `VTK_STAGE` is `all` (the default: both steps in one build, exactly as before),
  `deps` (apt, conda's VTK removed) or `build` (VTK only). Without `VTK_SRC`
  the build clones VTK from GitLab, so on a machine with internet and no
  `VTK_SRC` one command builds it all, as on PLEIADI.
* `VTK_SRC` is read by `%setup`, on the build host, so it is passed plain; the
  `APPTAINERENV_` and `SINGULARITYENV_` forms are read too. `%setup` copies the
  tree (without `.git`) into the image while it builds, and the build stops if
  it is not a VTK source tree. It must be the tree of `VTK_TAG`. A copy in
  `%setup` was chosen over a build-time `--bind` (not an option of every
  Apptainer and SingularityPRO release) and over a `%files` entry (no variables
  in its paths).
* `VTK_STAGE` and `VTK_TAG` reach `%post` prefixed (`APPTAINERENV_` or
  `SINGULARITYENV_`), as `ENABLE_CUPY` does.

The definition is correct on x86_64 as it is: the library directory comes from
`gcc -print-multiarch` (`/usr/lib/x86_64-linux-gnu`), libglvnd and the vendor
file are the same, and the `ldd` and `vtkEGLRenderWindow` checks do not depend
on the architecture. On linux-64 conda-forge does publish an EGL VTK, but the
definition replaces it by the source build so that the two architectures end up
alike.

### The differences between the routes

* **Stack.** Route B is the conda-forge stack (Numba, healpy, SoFiA-2 from
  conda-forge on linux-64); Route A and the published images use PyPI wheels and
  SoFiA-2 built from source. Both build VTK from source with EGL.
* **Entrypoint.** Route B uses the definition's `%runscript`: uvicorn on
  `PORT` (default 8000) and `VISIVO_HOST`, with `VISIVO_WORKERS` (default: the
  core count), and the backend itself reads `VISIVO_TOKEN`, `VISIVO_DATA_ROOT`
  and `VISIVO_DATA_ROOTS`. The launcher sets all of them, so a job started with
  it behaves as with the published image. What the `%runscript` does not do,
  and `docker/entrypoint.sh` does: it does not refuse to start without a data
  root, and it does not move caches and configuration to a scratch directory
  (the launcher sets `XDG_*`, `MPLCONFIGDIR` and the exports and cache
  directories). Started by hand, set them, or use Route A.

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
[Large cube loading](../design/large-cube-loading.md)) is at most a quarter of that
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
| `VISIVO_RENDER_MAX_SESSIONS` | `8` | Remote views the backend renders at once; beyond it a new one waits in the queue (or is refused with HTTP 503 when it did not ask to wait or the queue is full). |
| `VISIVO_RENDER_IDLE_TTL_SECONDS` | `300` | A view with no command for this long is closed. |
| `VISIVO_RENDER_QUEUE_MAX` | `16` | Views that may wait for a slot; `0` refuses at the cap, as before. |
| `VISIVO_RENDER_QUEUE_TIMEOUT_SECONDS` | `600` | Longest wait for a slot. |
| `VISIVO_RENDER_QUEUE_STALE_SECONDS` | `15` | A waiting client that stops asking for this long loses its place. |
| `VISIVO_RENDER_DISCONNECT_GRACE_SECONDS` | `30` | While views wait, a view with no client connected for this long gives up its slot. |
| `VISIVO_RENDER_IN_PROCESS` | unset | `1` renders in the backend's own process instead (slower: rendering then holds the process that serves the WebSocket). |
| `VTK_EGL_DEVICE_INDEX` | unset | Binds every view to that EGL device instead of spreading them over the job's GPUs. |

A cube larger than `VISIVO_RENDER_OUT_OF_CORE_BYTES` (6 GiB) is not loaded:
the view draws it from its brick pyramid, a few blocks at the resolution each
part of the view needs, so a cube larger than the GPU's memory can be shown.
How it works and its settings are in
[Brick pyramid](../design/brick-pyramid.md#rendering-from-the-pyramid). If the
pyramid is missing, the backend builds it in the background and the desktop
shows its progress over the preview, then switches to the full-resolution
view. To skip that wait, build it beforehand in a job of its own
(`python -m app.brick_pyramid build`, with the backend's `VISIVO_PYRAMID_DIR`). On PLEIADI-GPU a 23.4 GiB cube, larger than the V100's 16 GB, opened in
2.6 s from its pyramid and turned at 54 frames per second.

How the view streams frames and lowers its quality while you rotate is
described for users in the
[Cube viewer](../../user/reference/cube-viewer.md#where-the-rendering-happens-auto--local--remote)
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

**The job stays pending.** The launcher waits `VISIVO_ASSIGN_TIMEOUT` seconds
(300 by default) for SLURM to assign a node, then stops with `no compute node
assigned`; the job stays queued and starts when the resources free up. The
helper has recorded the job as soon as it was submitted, so `--stop` cancels
it. To wait longer, set `VISIVO_ASSIGN_TIMEOUT` on the frontend; otherwise ask
for fewer resources (a shorter `--time`, fewer GPUs, another `--partition`).
A new run refuses to start while a job from an earlier run is still recorded:
stop it first.

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
