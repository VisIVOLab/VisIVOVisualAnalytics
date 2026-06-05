#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# launch_backend_slurm.sh
# Submit the VisIVO backend to a SLURM compute node, wait for it to become
# ready, and print the SSH tunnel command the operator runs on the laptop.
#
# Designed for the INAF Power9 SRC testbed where ``pleiadi-gpu.oact.inaf.it``
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
#   --no-gpu                Shorthand for --gres ""; also drops --nv at runtime.
#   --help                  Print this help.
#
# Examples:
#   # Power9 testbed: 1 GPU, jail under /data
#   bash scripts/launch_backend_slurm.sh \
#       --partition gpu --gres gpu:1 --data-root /data \
#       --sif ~/VisIVO-next/backend-gpu.sif
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
LOG_DIR="${VISIVO_LOG_DIR:-$HOME/.visivo_logs}"
POLL_MAX=60   # max poll attempts × 5 s = 5 min
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

TOKEN_FILE="$HOME/.visivo_token"
echo "$TOKEN" > "$TOKEN_FILE"
chmod 600 "$TOKEN_FILE"
echo "[visivo] Token saved to $TOKEN_FILE"

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
#SBATCH --output=${SLURM_LOG}
#SBATCH --error=${SLURM_LOG}
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
# Decoupled from --cpus: the backend's ProcessPoolExecutor is sized by
# --workers, while --cpus is the SLURM allocation (which Numba's
# parallel kernels exploit via OpenMP threads inside ONE worker).
# 128 simultaneously-init-CUDA workers is what triggers
# cudaErrorInitializationError on a single V100.
export APPTAINERENV_VISIVO_WORKERS="${WORKERS}"
export APPTAINERENV_PORT="${PORT}"
export APPTAINERENV_VISIVO_HOST="0.0.0.0"
${DATA_ROOT_ENV}

apptainer run ${NV_FLAG} \\
    --bind "${BIND_PATH}:${BIND_PATH}" \\
    ${DATA_BIND_FLAG} \\
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
for _ in $(seq 1 30); do
    NODE="$(squeue -j "$JOB_ID" -h -o '%N' 2>/dev/null || true)"
    if [[ -n "$NODE" && "$NODE" != "None" ]]; then
        break
    fi
    sleep 2
done

if [[ -z "$NODE" || "$NODE" == "None" ]]; then
    echo "[visivo] ERROR: Could not determine compute node. Check: squeue -j $JOB_ID"
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
          ID:       src-power9-slurm
          URL:      http://localhost:${PORT}
          Token:    $(cat "$TOKEN_FILE")
          SRC code: POWER9

  Job log:
      ssh ${FRONTEND} tail -f ${LOG_DIR}/visivo_backend_${JOB_ID}.log

  Cancel the job:
      ssh ${FRONTEND} scancel ${JOB_ID}

══════════════════════════════════════════════════════════════════════
EOF
