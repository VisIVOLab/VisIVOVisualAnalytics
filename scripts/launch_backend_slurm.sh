#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# launch_backend_slurm.sh
# Submit the VisIVO backend to a SLURM compute node, wait for it to become
# ready, and print the SSH tunnel command the operator runs on the laptop.
#
# Designed for the INAF PLEIADI-GPU system, where ``pleiadi-gpu.oact.inaf.it``
# is the frontend (login node) of a SLURM cluster. The frontend should not
# run long-lived services; this script puts the backend on a real compute
# node instead, optionally with GPU allocation.
#
# Usage:
#   bash scripts/launch_backend_slurm.sh [options]
#
# Options (or env vars):
#   --partition  PARTITION  SLURM partition           (default: $VISIVO_PARTITION or "gpu")
#   --time       HH:MM:SS   Wall-clock limit          (default: $VISIVO_TIME or "08:00:00")
#   --mem        MB         Memory per node           (default: $VISIVO_MEM or "64G")
#   --cpus       N          CPUs per task             (default: $VISIVO_CPUS or 8)
#   --gres       GRES       GPU request, e.g. gpu:1   (default: $VISIVO_GRES or "gpu:1")
#                           Pass empty to disable.
#   --port       PORT       Backend TCP port          (default: $VISIVO_PORT or 8000)
#   --bind       PATH       Generic host path to bind (default: $HOME)
#   --data-root  PATH       Data dir bound at /data + sets VISIVO_DATA_ROOT
#                           (default: $VISIVO_DATA_ROOT or unset)
#                           STRONGLY recommended for production.
#   --sif        PATH       Apptainer .sif image      (default: apptainer/backend.sif)
#   --backend-src PATH      Host path to a backend/app source tree to bind over
#                           /app/app inside the container. Lets you iterate on
#                           Python code without rebuilding the .sif.
#                           (default: $VISIVO_BACKEND_SRC_OVERRIDE or unset)
#   --token      TOKEN      Auth token (auto-generated if absent)
#   --workspace  DIR        A writable directory for everything the backend and
#                           this launcher write: products (exports, also served
#                           as a data root), caches, the job logs and the token
#                           file. Needed where $HOME is read-only.
#                           ($VISIVO_WORKSPACE; default: under $HOME)
#   --node-code  CODE       The node's code in the SKAVA catalogue, reported
#                           in /v1/node/status ($VISIVO_NODE_CODE)
#   --version    TEXT       What the status document reports as the backend
#                           version, e.g. the VisIVO commit the image was
#                           built from ($VISIVO_VERSION; default: the image's)
#   --no-gpu                Shorthand for --gres ""; also drops --nv at runtime.
#   --help                  Print this help.
#
# Examples:
#   # Power9 testbed: 1 GPU, jail under /data
#   bash scripts/launch_backend_slurm.sh \
#       --partition gpu --gres gpu:1 --data-root /data \
#       --sif ~/VisIVOVisualAnalytics/backend-gpu.sif
#
#   # CPU-only smoke test
#   bash scripts/launch_backend_slurm.sh --no-gpu --partition normal
#
# What it does:
#   1. Submits a SLURM batch job that starts the Apptainer container with
#      --nv (when --gres includes gpu) and the path jail set.
#   2. Polls squeue every 2 s until the compute node is assigned.
#   3. Polls /v1/health via SSH to the compute node until the backend is up.
#   4. Prints the SSH tunnel command for the laptop + the auth token.
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────────────
PARTITION="${VISIVO_PARTITION:-gpu}"
TIME="${VISIVO_TIME:-08:00:00}"
MEM="${VISIVO_MEM:-64G}"
CPUS="${VISIVO_CPUS:-8}"
WORKERS="${VISIVO_WORKERS:-}"  # default empty → matches CPUS
GRES="${VISIVO_GRES:-gpu:1}"
PORT="${VISIVO_PORT:-8000}"
BIND_PATH="${VISIVO_BIND:-$HOME}"
DATA_ROOT="${VISIVO_DATA_ROOT:-}"
SIF="${VISIVO_SIF:-$(dirname "$0")/../apptainer/backend.sif}"
BACKEND_SRC="${VISIVO_BACKEND_SRC_OVERRIDE:-}"
TOKEN="${VISIVO_TOKEN:-}"
NODE_CODE="${VISIVO_NODE_CODE:-}"
WORKSPACE="${VISIVO_WORKSPACE:-}"
VERSION="${VISIVO_VERSION:-}"
LOG_DIR="${VISIVO_LOG_DIR:-$HOME/.visivo_logs}"
POLL_MAX=60   # max poll attempts × 5 s = 5 min
# How long to wait for SLURM to assign a node (a queued job can wait longer).
ASSIGN_TIMEOUT="${VISIVO_ASSIGN_TIMEOUT:-300}"
USE_GPU=true

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --partition) PARTITION="$2"; shift 2 ;;
        --time)      TIME="$2";      shift 2 ;;
        --mem)       MEM="$2";       shift 2 ;;
        --cpus)      CPUS="$2";      shift 2 ;;
        --workers)   WORKERS="$2";   shift 2 ;;
        --gres)      GRES="$2";      shift 2 ;;
        --port)      PORT="$2";      shift 2 ;;
        --bind)      BIND_PATH="$2"; shift 2 ;;
        --data-root) DATA_ROOT="$2"; shift 2 ;;
        --sif)       SIF="$2";       shift 2 ;;
        --backend-src) BACKEND_SRC="$2"; shift 2 ;;
        --token)     TOKEN="$2";     shift 2 ;;
        --node-code) NODE_CODE="$2"; shift 2 ;;
        --workspace) WORKSPACE="$2"; shift 2 ;;
        --version)   VERSION="$2";   shift 2 ;;
        --no-gpu)    USE_GPU=false; GRES=""; shift ;;
        --help|-h)
            grep '^#' "$0" | sed 's/^# \{0,2\}//'
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# Drop --nv when no GPU is being requested.
if [[ -z "$GRES" || "$GRES" == "0" ]]; then
    USE_GPU=false
fi

# Backend's ProcessPoolExecutor size. Decoupled from --cpus so an
# operator can allocate a whole node (--cpus 128) but keep the
# concurrent CUDA-init footprint small (--workers 4). When unset,
# default to CPUS for backward compat.
if [[ -z "$WORKERS" ]]; then
    WORKERS="$CPUS"
fi

# ── Generate token if not provided ───────────────────────────────────────────
if [[ -z "$TOKEN" ]]; then
    TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')"
fi

# With a workspace, everything this launcher writes goes there (a read-only
# $HOME is common on HPC frontends); an explicit VISIVO_LOG_DIR still wins.
if [[ -n "$WORKSPACE" ]]; then
    mkdir -p "$WORKSPACE"
    WORKSPACE="$(cd "$WORKSPACE" && pwd -P)"
    LOG_DIR="${VISIVO_LOG_DIR:-$WORKSPACE/logs}"
    TOKEN_FILE="$WORKSPACE/token"
else
    TOKEN_FILE="$HOME/.visivo_token"
fi
if ( umask 077 && printf '%s\n' "$TOKEN" > "$TOKEN_FILE" && chmod 600 "$TOKEN_FILE" ) 2>/dev/null; then
    echo "[visivo] Token saved to $TOKEN_FILE"
else
    echo "[visivo] WARNING: could not write $TOKEN_FILE; the token is printed below"
fi

# ── Verify the Apptainer image exists ────────────────────────────────────────
if [[ ! -f "$SIF" ]]; then
    echo "[visivo] ERROR: Apptainer image not found: $SIF"
    echo "[visivo] Build it first, e.g.:"
    echo "         apptainer build $SIF apptainer/backend-conda.def"
    exit 1
fi

# ── Create log directory ──────────────────────────────────────────────────────
mkdir -p "$LOG_DIR"
SLURM_LOG="$LOG_DIR/visivo_backend_%j.log"

# ── Build the SBATCH header ───────────────────────────────────────────────────
SBATCH_HEADER=$(cat <<EOF
#!/bin/bash
#SBATCH --job-name=visivo_backend
#SBATCH --partition=${PARTITION}
#SBATCH --time=${TIME}
#SBATCH --mem=${MEM}
#SBATCH --cpus-per-task=${CPUS}
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --output="${SLURM_LOG}"
#SBATCH --error="${SLURM_LOG}"
EOF
)

if [[ -n "$GRES" ]]; then
    SBATCH_HEADER+=$'\n'"#SBATCH --gres=${GRES}"
fi

# ── Build the data-bind + env var fragments ──────────────────────────────────
# Same-path bind: the container sees the host path verbatim. Makes the
# access_url in SKAVA describe a real on-disk location (rather than a
# container-translated one), so consumers outside the backend can also
# use it (publisher, direct download, sibling tools).
DATA_BIND_FLAG=""
DATA_ROOT_ENV=""
if [[ -n "$DATA_ROOT" ]]; then
    DATA_BIND_FLAG="--bind ${DATA_ROOT}:${DATA_ROOT}"
    DATA_ROOT_ENV="export APPTAINERENV_VISIVO_DATA_ROOT=${DATA_ROOT}"
fi

NV_FLAG=""
if [[ "$USE_GPU" == "true" ]]; then
    NV_FLAG="--nv"
fi

# What the node says about itself in /v1/node/status: its code in the
# catalogue, whether it has a GPU (the one SLURM allocated), the version.
# Values are quoted for the job script, which bash reads again.
NODE_ENV="export APPTAINERENV_VISIVO_NODE_GPU=$([[ "$USE_GPU" == "true" ]] && echo 1 || echo 0)"

# The workspace inside the container: products (also a data root, so a
# product opens where it was written), caches; bound at the same path.
WORKSPACE_ENV=""
WORKSPACE_BIND_FLAG=""
if [[ -n "$WORKSPACE" ]]; then
    mkdir -p "$WORKSPACE/exports" "$WORKSPACE/cache/hips" "$WORKSPACE/cache/numba" "$WORKSPACE/cache/cupy"
    ROOTS="$WORKSPACE/exports"
    [[ -n "$DATA_ROOT" ]] && ROOTS="${DATA_ROOT}:${ROOTS}"
    WORKSPACE_ENV="export APPTAINERENV_VISIVO_EXPORTS_DIR=$(printf '%q' "$WORKSPACE/exports")"
    WORKSPACE_ENV+=$'\n'"export APPTAINERENV_VISIVO_HIPS_CACHE=$(printf '%q' "$WORKSPACE/cache/hips")"
    WORKSPACE_ENV+=$'\n'"export APPTAINERENV_NUMBA_CACHE_DIR=$(printf '%q' "$WORKSPACE/cache/numba")"
    WORKSPACE_ENV+=$'\n'"export APPTAINERENV_CUPY_CACHE_DIR=$(printf '%q' "$WORKSPACE/cache/cupy")"
    WORKSPACE_ENV+=$'\n'"export APPTAINERENV_VISIVO_DATA_ROOTS=$(printf '%q' "$ROOTS")"
    WORKSPACE_BIND_FLAG="--bind $(printf '%q' "$WORKSPACE:$WORKSPACE")"
fi
if [[ -n "$NODE_CODE" ]]; then
    NODE_ENV+=$'\n'"export APPTAINERENV_VISIVO_NODE_CODE=$(printf '%q' "$NODE_CODE")"
fi
if [[ -n "$VERSION" ]]; then
    NODE_ENV+=$'\n'"export APPTAINERENV_VISIVO_VERSION=$(printf '%q' "$VERSION")"
fi

# Optional: bind a host backend/app dir over /app/app inside the
# container, so an operator can iterate on the Python code without
# rebuilding the .sif. The typical use is testing the CUDA-after-fork
# fix and similar runtime-only patches.
SRC_BIND_FLAG=""
if [[ -n "$BACKEND_SRC" ]]; then
    if [[ ! -d "$BACKEND_SRC" ]]; then
        echo "[visivo] ERROR: --backend-src directory not found: $BACKEND_SRC"
        exit 1
    fi
    SRC_BIND_FLAG="--bind ${BACKEND_SRC}:/app/app"
    echo "[visivo] Overlaying /app/app with host source: $BACKEND_SRC"
fi

# ── EGL / server-side rendering support ─────────────────────────────────────
# When the backend uses ``backend-egl.sif`` and a client opens a
# RenderSession, VTK does ``vtkEGLRenderWindow.Render()`` which on
# this Power9 cluster needs:
#   * the NVIDIA runtime libs (libcuda, libnvidia-tls, libnvidia-ml,
#     ...) bound from ``/usr/lib64`` of the host — Apptainer ``--nv``
#     misses several on ppc64le
#   * ``LD_LIBRARY_PATH=/usr/lib64`` so libglvnd resolves them
#   * ``XDG_CACHE_HOME`` / ``XDG_CONFIG_HOME`` redirected to a
#     local /tmp tree, because Freetype/fontconfig take a POSIX file
#     lock on ``~/.cache/fontconfig`` and on a NFS-mounted $HOME the
#     lockd never replies → the first Render() hangs forever
# We add the binds defensively even when the cluster isn't ppc64le —
# they're harmless on x86 (the host libs simply aren't there).
NV_RUNTIME_BINDS=""
for lib in \
    libEGL_nvidia.so.0 libGLX_nvidia.so.0 libEGL.so.1 \
    libnvidia-glcore.so.* libnvidia-glsi.so.* libnvidia-egl-wayland.so.* \
    libcuda.so.1 libcuda.so.* \
    libnvidia-tls.so.* libnvidia-rtcore.so.* \
    libnvidia-allocator.so.* libnvidia-cfg.so.* libnvidia-ml.so.* \
    libnvidia-encode.so.* libnvidia-opencl.so.* \
    libnvidia-ptxjitcompiler.so.* libnvidia-nvvm.so.* \
    libnvidia-fatbinaryloader.so.* libnvidia-compiler.so.* ; do
    for f in /usr/lib64/${lib} /usr/lib/powerpc64le-linux-gnu/${lib} ; do
        if [[ -e "$f" ]]; then
            NV_RUNTIME_BINDS="${NV_RUNTIME_BINDS} --bind ${f}:${f}"
        fi
    done
done
GLVND_JSON="/usr/share/glvnd/egl_vendor.d/10_nvidia.json"
if [[ -f "$GLVND_JSON" ]]; then
    NV_RUNTIME_BINDS="${NV_RUNTIME_BINDS} --bind ${GLVND_JSON}:${GLVND_JSON}"
fi

# ── Write the SLURM job script ────────────────────────────────────────────────
JOB_SCRIPT="$(mktemp /tmp/visivo_slurm_XXXXXX.sh)"
cat > "$JOB_SCRIPT" <<SLURM_EOF
${SBATCH_HEADER}

echo "[visivo] Job started on \$(hostname) at \$(date)"
echo "[visivo] Port: ${PORT}"
echo "[visivo] GPU:  ${GRES:-disabled}"
echo "[visivo] Data root: ${DATA_ROOT:-(not jailed)}"

# Env vars consumed inside the container.
export APPTAINERENV_VISIVO_TOKEN="${TOKEN}"
export APPTAINERENV_VISIVO_WORKERS="${WORKERS}"
export APPTAINERENV_PORT="${PORT}"
export APPTAINERENV_VISIVO_HOST="0.0.0.0"
# EGL / SSR env (see comment on NV_RUNTIME_BINDS above).
export APPTAINERENV_LD_LIBRARY_PATH=/usr/lib64
export APPTAINERENV_XDG_CACHE_HOME=/tmp/visivo-cache
export APPTAINERENV_XDG_CONFIG_HOME=/tmp/visivo-config
export APPTAINERENV_XDG_DATA_HOME=/tmp/visivo-data
export APPTAINERENV_MPLCONFIGDIR=/tmp/visivo-mpl
export APPTAINERENV___EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
export APPTAINERENV___GLX_VENDOR_LIBRARY_NAME=nvidia
# Pre-create the local-tmp dirs the env vars point at, so fontconfig
# and matplotlib don't fail on first-access mkdir under a read-only
# filesystem mount.
mkdir -p /tmp/visivo-cache /tmp/visivo-config /tmp/visivo-data /tmp/visivo-mpl
${DATA_ROOT_ENV}
${NODE_ENV}
${WORKSPACE_ENV}

apptainer run ${NV_FLAG} \\
    --bind "${BIND_PATH}:${BIND_PATH}" \\
    ${DATA_BIND_FLAG} \\
    ${WORKSPACE_BIND_FLAG} \\
    ${NV_RUNTIME_BINDS} \\
    ${SRC_BIND_FLAG} \\
    "${SIF}"
SLURM_EOF

# ── Submit the job ────────────────────────────────────────────────────────────
echo "[visivo] Submitting SLURM job..."
echo "[visivo]   partition: ${PARTITION}"
echo "[visivo]   gres:      ${GRES:-(none)}"
echo "[visivo]   cpus:      ${CPUS}     (SLURM allocation)"
echo "[visivo]   workers:   ${WORKERS}  (backend ProcessPoolExecutor)"
echo "[visivo]   mem:       ${MEM}"
echo "[visivo]   time:      ${TIME}"
echo "[visivo]   sif:       ${SIF}"

JOB_ID="$(sbatch --parsable "$JOB_SCRIPT")"
rm -f "$JOB_SCRIPT"
echo "[visivo] Job ID: $JOB_ID"

# ── Wait for the compute node to be assigned ──────────────────────────────────
echo "[visivo] Waiting for compute node assignment..."
NODE=""
for _ in $(seq 1 $(( (ASSIGN_TIMEOUT + 1) / 2 ))); do
    NODE="$(squeue -j "$JOB_ID" -h -o '%N' 2>/dev/null || true)"
    if [[ -n "$NODE" && "$NODE" != "None" ]]; then
        break
    fi
    sleep 2
done

if [[ -z "$NODE" || "$NODE" == "None" ]]; then
    echo "[visivo] ERROR: no compute node assigned to job $JOB_ID within ${ASSIGN_TIMEOUT} s." >&2
    echo "[visivo]        The job is still queued and starts when the resources free up; cancel" >&2
    echo "[visivo]        it with: scancel $JOB_ID (or wait longer: VISIVO_ASSIGN_TIMEOUT=<seconds>)." >&2
    exit 1
fi

echo "[visivo] Compute node: $NODE"

# ── Poll the health endpoint via SSH to the compute node ──────────────────────
echo "[visivo] Waiting for backend to become ready (polling every 5 s)..."
READY=false
for attempt in $(seq 1 "$POLL_MAX"); do
    if ssh -o StrictHostKeyChecking=no -o BatchMode=yes \
           "$NODE" "curl -sf -H 'X-Visivo-Token: ${TOKEN}' http://localhost:${PORT}/v1/health" \
           >/dev/null 2>&1; then
        READY=true
        break
    fi
    echo "[visivo]  attempt $attempt/$POLL_MAX – not ready yet..."
    sleep 5
done

if [[ "$READY" != "true" ]]; then
    echo "[visivo] ERROR: Backend did not become ready within $((POLL_MAX * 5)) seconds."
    echo "[visivo] Check the log: $LOG_DIR/visivo_backend_${JOB_ID}.log"
    exit 1
fi

FRONTEND="$(hostname -f)"

cat <<EOF

══════════════════════════════════════════════════════════════════════
  VisIVO backend is READY
══════════════════════════════════════════════════════════════════════
  Job ID:        ${JOB_ID}
  Compute node:  ${NODE}
  Port:          ${PORT}
  Frontend:      ${FRONTEND}
  GPU:           ${GRES:-disabled}
  Data root:     ${DATA_ROOT:-(not jailed)}
══════════════════════════════════════════════════════════════════════

  Run this SSH tunnel command ON YOUR LAPTOP:

      ssh -N -L ${PORT}:${NODE}:${PORT} \${USER}@${FRONTEND}

  Then in the VisIVO desktop client:

      Settings → Remote Backends → Add:
          ID:       src-$(printf '%s' "${NODE_CODE:-node}" | tr '[:upper:]' '[:lower:]')
          URL:      http://localhost:${PORT}
          Token:    ${TOKEN}
          SRC code: ${NODE_CODE:-<the node code in SKAVA>}

  Job log:
      ssh ${FRONTEND} tail -f ${LOG_DIR}/visivo_backend_${JOB_ID}.log

  Cancel the job:
      ssh ${FRONTEND} scancel ${JOB_ID}

══════════════════════════════════════════════════════════════════════
EOF
