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
BACKEND_SRC="${VISIVO_BACKEND_SRC_OVERRIDE:-}"
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

# Optional: bind a host backend/app dir over /app/app inside the
# container, so the operator can iterate on the Python code without
# rebuilding the .sif. Applied to every apptainer invocation in the
# job (scheduler exec, local worker exec, ssh-spawned remote worker
# exec, and the backend run). The ssh-spawned workers receive the
# bind via the same shell-expanded ${SRC_BIND_FLAG}.
SRC_BIND_FLAG=""
if [[ -n "$BACKEND_SRC" ]]; then
    if [[ ! -d "$BACKEND_SRC" ]]; then
        echo "[visivo] ERROR: --backend-src directory not found: $BACKEND_SRC" >&2
        exit 1
    fi
    SRC_BIND_FLAG="--bind ${BACKEND_SRC}:/app/app"
    echo "[visivo] Overlaying /app/app with host source: $BACKEND_SRC"
fi

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

# ─── Launch strategy: NO srun for sub-steps ────────────────────────────
# We tried ``srun --overlap`` for scheduler/workers/backend running in
# parallel inside the same job and SLURM ≥22 silently queued the second
# and third srun, never allocating new step slots — even with --overlap.
# (Symptoms: ``squeue -s -j NNN`` shows only the .0 step + .batch step,
# and the parallel sruns sit forever.)
#
# Pattern that works everywhere (used by dask-jobqueue, dask-mpi):
#   1. scheduler in background on the local node (= the batch script's
#      node, which is the first allocated node). apptainer exec, no srun.
#   2. local worker on the same node, also via apptainer exec in
#      background.
#   3. remote workers on the OTHER allocated nodes via ssh + nohup. ssh
#      between compute nodes is allowed within the job's allocation.
#   4. backend in the FOREGROUND on the local node. This keeps the
#      batch script (and the SLURM job) alive — the moment it exits,
#      SLURM cleans up everything, including the ssh-spawned remote
#      workers.

# ─── 1. Dask scheduler on the local (first) node, background ────────────
# PYTHONPATH=/app is REQUIRED so the scheduler can deserialise task graphs
# submitted by the backend (which reference ``app.compute.worker_*``).
# Without it, the scheduler raises ``ModuleNotFoundError: No module named
# 'app'`` on every client.submit() and tasks fail silently with no
# results — symptomatic of "task graph deserialisation" errors in the
# scheduler log.
apptainer exec \\
    --env PYTHONPATH=/app \\
    --bind "${BIND_PATH}:${BIND_PATH}" \\
    ${DATA_BIND_FLAG} \\
    ${SRC_BIND_FLAG} \\
    "${SIF}" \\
    dask scheduler \\
        --host 0.0.0.0 \\
        --port ${SCHED_PORT} \\
        --dashboard-address :${DASHBOARD_PORT} \\
        > "\$HOME/.visivo_logs/dask_scheduler_\${SLURM_JOB_ID}.log" 2>&1 &
SCHED_PID=\$!
echo "[dask] Scheduler PID on \$SCHED_NODE: \$SCHED_PID"

# ─── 2. Wait for the scheduler to accept connections ────────────────────
echo "[dask] Waiting for scheduler to be ready..."
for attempt in \$(seq 1 60); do
    if (exec 3<>/dev/tcp/localhost/${SCHED_PORT}) 2>/dev/null; then
        exec 3<&- 3>&-
        echo "[dask] Scheduler ready after \$attempt attempt(s)."
        break
    fi
    sleep 1
done

# ─── 3a. Local dask workers (same node as scheduler), background ────────
# Same PYTHONPATH=/app rationale as the scheduler: workers must be able
# to import ``app.compute`` to execute the deserialised task graph.
#
# --memory-limit 0 disables Dask's own memory accounting. Without it,
# ``--memory-limit auto`` reads psutil.virtual_memory() FROM INSIDE the
# Apptainer container, which on this cluster returns ~10 GiB (some
# cgroup limit invisible to SLURM's --mem flag). Divided by 4 workers,
# each worker thinks it has only 2.43 GiB and the nanny kills it the
# moment we mmap a 10 GB cube. We have 250 GB of real RAM per node,
# so leaving memory pressure handling to the OS is fine.
apptainer exec \\
    --env PYTHONPATH=/app \\
    --bind "${BIND_PATH}:${BIND_PATH}" \\
    ${DATA_BIND_FLAG} \\
    ${SRC_BIND_FLAG} \\
    "${SIF}" \\
    dask worker "\$SCHED_URL" \\
        --nworkers ${WORKERS} \\
        --nthreads 1 \\
        --memory-limit 0 \\
        --local-directory /tmp \\
        > "\$HOME/.visivo_logs/dask_worker_\${SCHED_NODE}_\${SLURM_JOB_ID}.log" 2>&1 &
LOCAL_WORKER_PID=\$!
echo "[dask] Local workers on \$SCHED_NODE: PID \$LOCAL_WORKER_PID"

# ─── 3b. Remote dask workers on other nodes, via ssh + nohup ───────────
# ssh between compute nodes inside the same SLURM allocation is allowed
# by default; nohup detaches from our shell so the command keeps running
# when the ssh exits.
REMOTE_WORKER_PIDS=""
for node in "\${NODELIST[@]:1}"; do
    echo "[dask] Spawning workers on \$node via ssh..."
    ssh -n -o StrictHostKeyChecking=no -o BatchMode=yes "\$node" \\
        "nohup apptainer exec \\
            --env PYTHONPATH=/app \\
            --bind '${BIND_PATH}:${BIND_PATH}' \\
            ${DATA_BIND_FLAG} \\
            ${SRC_BIND_FLAG} \\
            '${SIF}' \\
            dask worker '\$SCHED_URL' \\
                --nworkers ${WORKERS} \\
                --nthreads 1 \\
                --memory-limit 0 \\
                --local-directory /tmp \\
                > \$HOME/.visivo_logs/dask_worker_\${node}_\${SLURM_JOB_ID}.log 2>&1 &"
done

# Give all workers a few seconds to register with the scheduler.
sleep 10

# ─── 4. Backend on the local node, FOREGROUND (keeps the SLURM job alive)
echo "[visivo] Starting backend on \$SCHED_NODE:${PORT} (with Dask scheduler at \$SCHED_URL)"
apptainer run ${NV_FLAG} \\
    --bind "${BIND_PATH}:${BIND_PATH}" \\
    ${DATA_BIND_FLAG} \\
    ${SRC_BIND_FLAG} \\
    --env VISIVO_TOKEN="${TOKEN}" \\
    --env VISIVO_WORKERS=4 \\
    --env VISIVO_HOST=0.0.0.0 \\
    --env PORT=${PORT} \\
    --env VISIVO_DASK_SCHEDULER="\$SCHED_URL" \\
    ${DATA_ROOT_ENV} \\
    "${SIF}"

# When the backend exits, tear down local scheduler + local workers.
# Remote workers (ssh+nohup'd onto other nodes) are killed by SLURM
# when the job allocation ends — no explicit cleanup needed for them.
kill \$LOCAL_WORKER_PID \$SCHED_PID 2>/dev/null || true
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
