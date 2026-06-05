#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# launch_backend_slurm_dask.sh
# Multi-node variant of launch_backend_slurm.sh. Allocates N nodes via SLURM,
# brings up a Dask scheduler on the first node and Dask workers on every node,
# then runs the VisIVO backend pointing at the scheduler. Heavy operations
# (moment maps, channel maps, …) are fanned out across the worker pool.
#
# Usage:
#   bash scripts/launch_backend_slurm_dask.sh [options]
#
# Options (or env vars):
#   --partition  PARTITION   SLURM partition           (default: $VISIVO_PARTITION or "gpu")
#   --nodes      N           Compute nodes to allocate (default: $VISIVO_NODES or 4)
#   --cpus       N           CPUs per node             (default: $VISIVO_CPUS or 32)
#   --workers    N           Dask workers per node     (default: same as --cpus)
#   --mem        MB          Memory per node           (default: $VISIVO_MEM or 64G)
#   --time       HH:MM:SS    Wall-clock                (default: $VISIVO_TIME or "08:00:00")
#   --gres       GRES        GPU per node, e.g. gpu:1  (default: $VISIVO_GRES or "")
#   --port       PORT        Backend TCP port          (default: $VISIVO_PORT or 8000)
#   --bind       PATH        Generic host bind         (default: $HOME)
#   --data-root  PATH        Shared FS root + jail     (default: $VISIVO_DATA_ROOT or unset)
#   --sif        PATH        Apptainer image           (default: apptainer/backend.sif)
#   --token      TOKEN       Auth token (auto-generated if absent)
#   --no-gpu                 Shorthand for --gres ""
#   --help                   Print this help
#
# Topology:
#   Allocates ``--nodes N`` compute nodes via SLURM.
#   Node 0    → Dask scheduler + Dask workers + backend (uvicorn)
#   Nodes 1..N-1 → Dask workers only
#   Backend reaches Dask via tcp://<node-0-hostname>:8786
#
# Example:
#   bash scripts/launch_backend_slurm_dask.sh \
#       --nodes 4 --cpus 32 --workers 32 \
#       --gres gpu:v100:1 --data-root /mnt/beegfs/fvitello/data \
#       --sif ~/VisIVO-next/backend-gpu.sif
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

PARTITION="${VISIVO_PARTITION:-gpu}"
NODES="${VISIVO_NODES:-4}"
CPUS="${VISIVO_CPUS:-32}"
WORKERS="${VISIVO_WORKERS_PER_NODE:-}"
MEM="${VISIVO_MEM:-64G}"
TIME="${VISIVO_TIME:-08:00:00}"
GRES="${VISIVO_GRES:-}"
PORT="${VISIVO_PORT:-8000}"
BIND_PATH="${VISIVO_BIND:-$HOME}"
DATA_ROOT="${VISIVO_DATA_ROOT:-}"
SIF="${VISIVO_SIF:-$(dirname "$0")/../apptainer/backend.sif}"
TOKEN="${VISIVO_TOKEN:-}"
LOG_DIR="${VISIVO_LOG_DIR:-$HOME/.visivo_logs}"
USE_GPU=true
POLL_MAX=120   # 10 min (multi-node startup can be slower)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --partition) PARTITION="$2"; shift 2 ;;
        --nodes)     NODES="$2";     shift 2 ;;
        --cpus)      CPUS="$2";      shift 2 ;;
        --workers)   WORKERS="$2";   shift 2 ;;
        --mem)       MEM="$2";       shift 2 ;;
        --time)      TIME="$2";      shift 2 ;;
        --gres)      GRES="$2";      shift 2 ;;
        --port)      PORT="$2";      shift 2 ;;
        --bind)      BIND_PATH="$2"; shift 2 ;;
        --data-root) DATA_ROOT="$2"; shift 2 ;;
        --sif)       SIF="$2";       shift 2 ;;
        --token)     TOKEN="$2";     shift 2 ;;
        --no-gpu)    USE_GPU=false; GRES=""; shift ;;
        --help|-h)
            grep '^#' "$0" | sed 's/^# \{0,2\}//'
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$WORKERS" ]]; then WORKERS="$CPUS"; fi
if [[ -z "$GRES" || "$GRES" == "0" ]]; then USE_GPU=false; fi

if [[ -z "$TOKEN" ]]; then
    TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')"
fi
TOKEN_FILE="$HOME/.visivo_token"
echo "$TOKEN" > "$TOKEN_FILE"
chmod 600 "$TOKEN_FILE"

if [[ ! -f "$SIF" ]]; then
    echo "[visivo] ERROR: Apptainer image not found: $SIF" >&2
    exit 1
fi

mkdir -p "$LOG_DIR"
SLURM_LOG="$LOG_DIR/visivo_backend_dask_%j.log"

SBATCH_HEADER=$(cat <<EOF
#!/bin/bash
#SBATCH --job-name=visivo_dask
#SBATCH --partition=${PARTITION}
#SBATCH --time=${TIME}
#SBATCH --mem=${MEM}
#SBATCH --cpus-per-task=${CPUS}
#SBATCH --nodes=${NODES}
#SBATCH --ntasks-per-node=1
#SBATCH --output=${SLURM_LOG}
#SBATCH --error=${SLURM_LOG}
EOF
)
if [[ -n "$GRES" ]]; then
    SBATCH_HEADER+=$'\n'"#SBATCH --gres=${GRES}"
fi

DATA_BIND_FLAG=""
DATA_ROOT_ENV=""
if [[ -n "$DATA_ROOT" ]]; then
    DATA_BIND_FLAG="--bind ${DATA_ROOT}:${DATA_ROOT}"
    DATA_ROOT_ENV="--env VISIVO_DATA_ROOT=${DATA_ROOT}"
fi

NV_FLAG=""
if [[ "$USE_GPU" == "true" ]]; then NV_FLAG="--nv"; fi

# Dask scheduler / workers Dashboard port (informational). Pick a value
# unlikely to collide with system services.
SCHED_PORT=18786
DASHBOARD_PORT=18787

JOB_SCRIPT="$(mktemp /tmp/visivo_slurm_dask_XXXXXX)"
cat > "$JOB_SCRIPT" <<SLURM_EOF
${SBATCH_HEADER}

set -e

echo "[dask] Job started on \$(hostname) at \$(date)"
echo "[dask] Allocated nodes: \$SLURM_NODELIST (\$SLURM_NNODES total)"

# Pick the first node as the scheduler host.
NODELIST=(\$(scontrol show hostname \$SLURM_NODELIST))
SCHED_NODE=\${NODELIST[0]}
SCHED_URL="tcp://\${SCHED_NODE}:${SCHED_PORT}"
echo "[dask] Scheduler host: \$SCHED_NODE"
echo "[dask] Scheduler URL:  \$SCHED_URL"

# ─── 1. Dask scheduler on node 0 (background) ───────────────────────────
srun --nodes=1 --ntasks=1 --nodelist=\$SCHED_NODE \\
    apptainer exec \\
    --bind "${BIND_PATH}:${BIND_PATH}" \\
    ${DATA_BIND_FLAG} \\
    "${SIF}" \\
    dask scheduler \\
        --host 0.0.0.0 \\
        --port ${SCHED_PORT} \\
        --dashboard-address :${DASHBOARD_PORT} &
SCHED_PID=\$!
echo "[dask] Scheduler PID on \$SCHED_NODE: \$SCHED_PID"

# ─── 2. Wait for the scheduler to accept connections ────────────────────
echo "[dask] Waiting for scheduler to be ready..."
for attempt in \$(seq 1 30); do
    if srun --nodes=1 --ntasks=1 --nodelist=\$SCHED_NODE \\
            apptainer exec "${SIF}" \\
            python3 -c "from dask.distributed import Client; Client('\$SCHED_URL', timeout=3); print('ok')" 2>/dev/null \\
            | grep -q ok; then
        echo "[dask] Scheduler ready after \$attempt attempt(s)."
        break
    fi
    sleep 2
done

# ─── 3. Dask workers on every node (each node hosts ${WORKERS} workers) ─
srun --nodes=\$SLURM_NNODES --ntasks-per-node=1 \\
    apptainer exec \\
    --bind "${BIND_PATH}:${BIND_PATH}" \\
    ${DATA_BIND_FLAG} \\
    "${SIF}" \\
    dask worker "\$SCHED_URL" \\
        --nworkers ${WORKERS} \\
        --nthreads 1 \\
        --memory-limit auto \\
        --local-directory /tmp &
WORKER_PID=\$!
echo "[dask] Workers PID (across all nodes): \$WORKER_PID"

# Give workers a few seconds to register.
sleep 10

# ─── 4. Backend on node 0 (foreground; keeps the SLURM job alive) ──────
echo "[visivo] Starting backend on \$SCHED_NODE:${PORT} (with Dask scheduler at \$SCHED_URL)"
srun --nodes=1 --ntasks=1 --nodelist=\$SCHED_NODE \\
    apptainer run ${NV_FLAG} \\
    --bind "${BIND_PATH}:${BIND_PATH}" \\
    ${DATA_BIND_FLAG} \\
    --env VISIVO_TOKEN="${TOKEN}" \\
    --env VISIVO_WORKERS=4 \\
    --env VISIVO_HOST=0.0.0.0 \\
    --env PORT=${PORT} \\
    --env VISIVO_DASK_SCHEDULER="\$SCHED_URL" \\
    ${DATA_ROOT_ENV} \\
    "${SIF}"

# When backend exits, tear down scheduler + workers.
kill \$WORKER_PID \$SCHED_PID 2>/dev/null || true
wait
SLURM_EOF

echo "[visivo] Submitting SLURM Dask job..."
echo "[visivo]   nodes:    ${NODES}"
echo "[visivo]   cpus/node: ${CPUS}"
echo "[visivo]   workers/node: ${WORKERS}"
echo "[visivo]   gres:     ${GRES:-(none)}"
echo "[visivo]   sif:      ${SIF}"

JOB_ID="$(sbatch --parsable "$JOB_SCRIPT")"
rm -f "$JOB_SCRIPT"
echo "[visivo] Job ID: $JOB_ID"

echo "[visivo] Waiting for compute node assignment..."
NODELIST=""
for _ in $(seq 1 30); do
    NODELIST="$(squeue -j "$JOB_ID" -h -o '%N' 2>/dev/null | head -1)"
    if [[ -n "$NODELIST" && "$NODELIST" != "None" ]]; then break; fi
    sleep 2
done
if [[ -z "$NODELIST" || "$NODELIST" == "None" ]]; then
    echo "[visivo] ERROR: could not get a compute node" >&2
    exit 1
fi
# squeue's %N format is SLURM's compact hostlist syntax (e.g.
# ``r33c1s[01-02]``), which is NOT a valid hostname. Expand it with
# scontrol show hostname so we can ssh into the first node directly.
# Fallback to NODELIST verbatim if scontrol fails — then NODELIST is
# already a single hostname (single-node allocation).
FIRST_NODE="$(scontrol show hostname "$NODELIST" 2>/dev/null | head -1)"
if [[ -z "$FIRST_NODE" ]]; then
    FIRST_NODE="${NODELIST%%,*}"
fi
echo "[visivo] Backend host (first node): $FIRST_NODE"

echo "[visivo] Polling /v1/health..."
READY=false
for attempt in $(seq 1 "$POLL_MAX"); do
    if ssh -o StrictHostKeyChecking=no -o BatchMode=yes \
           "$FIRST_NODE" "curl -sf -H 'X-Visivo-Token: ${TOKEN}' http://localhost:${PORT}/v1/health" \
           >/dev/null 2>&1; then
        READY=true; break
    fi
    echo "[visivo]   attempt $attempt/$POLL_MAX – not ready yet..."
    sleep 5
done

if [[ "$READY" != "true" ]]; then
    echo "[visivo] ERROR: backend not ready after $((POLL_MAX * 5)) s" >&2
    echo "[visivo] Log: ssh $(hostname -f) tail -f $LOG_DIR/visivo_backend_dask_${JOB_ID}.log"
    exit 1
fi

FRONTEND="$(hostname -f)"
cat <<EOF

══════════════════════════════════════════════════════════════════════
  VisIVO backend (Dask multi-node) is READY
══════════════════════════════════════════════════════════════════════
  Job ID:          ${JOB_ID}
  Backend host:    ${FIRST_NODE}
  Backend port:    ${PORT}
  Dask scheduler:  tcp://${FIRST_NODE}:${SCHED_PORT}
  Dashboard:       http://${FIRST_NODE}:${DASHBOARD_PORT}
  Total nodes:     ${NODES}
  Workers/node:    ${WORKERS}
  Total workers:   $(( NODES * WORKERS ))
══════════════════════════════════════════════════════════════════════

  SSH tunnel on your laptop (backend):
      ssh -N -L ${PORT}:${FIRST_NODE}:${PORT} \${USER}@${FRONTEND}

  (Optional) SSH tunnel for the Dask dashboard:
      ssh -N -L ${DASHBOARD_PORT}:${FIRST_NODE}:${DASHBOARD_PORT} \${USER}@${FRONTEND}
      → http://localhost:${DASHBOARD_PORT} in your browser

  Token: $(cat "$TOKEN_FILE")
  Log:   ssh ${FRONTEND} tail -f ${LOG_DIR}/visivo_backend_dask_${JOB_ID}.log
  Cancel: ssh ${FRONTEND} scancel ${JOB_ID}
══════════════════════════════════════════════════════════════════════
EOF
