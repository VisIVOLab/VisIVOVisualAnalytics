#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# visivo_slurm_tunnel_dask.sh
# Mac-side / laptop-side helper for the Dask multi-node variant.
#
#   1. SSHes into the SLURM frontend.
#   2. Submits the backend + Dask cluster via launch_backend_slurm_dask.sh.
#   3. Parses the launcher output (job id, backend host, ports, token).
#   4. Opens TWO SSH tunnels: backend and (optional) Dask dashboard.
#   5. Prints the Remote Backend snippet for the VisIVO desktop client.
#
# Differences vs visivo_slurm_tunnel.sh (single-node):
#   - calls launch_backend_slurm_dask.sh on the frontend
#   - second tunnel for the Dask dashboard (http://localhost:<port> in browser)
#   - --stop also tears down both tunnels + scancels the multi-node job
#
# Usage:
#   bash scripts/visivo_slurm_tunnel_dask.sh \
#       --frontend pleiadi-gpu.oact.inaf.it \
#       --user     fvitello \
#       --partition gpu \
#       --gres     gpu:v100:1 \
#       --nodes    4 \
#       --cpus     32 \
#       --workers  32 \
#       --data-root /mnt/beegfs/fvitello/data \
#       --sif      ~/VisIVOVisualAnalytics/backend-gpu.sif \
#       --token    "$(cat ~/.visivo/power9_token)"
#
# Local-side options (do NOT pass through to the launcher):
#   --frontend HOST    SLURM frontend hostname (required)
#   --repo-dir DIR     the clone on the frontend, relative to its home
#                      (default VisIVOVisualAnalytics; $VISIVO_REMOTE_REPO)
#   --user     USER    SSH login (default: $USER)
#   --port     PORT    Local port for the backend tunnel (default: 18900,
#                      not the local backend's 8000)
#   --dashboard-port P Local port for the Dask dashboard tunnel (default: 18787)
#   --no-dashboard     Skip the dashboard tunnel
#   --stop             Tear down tunnels + scancel job + exit
#   --help             Print this help
#
# Every other flag (--nodes, --cpus, --gres, --data-root, --sif, --token, …)
# is passed through verbatim to the remote launch_backend_slurm_dask.sh.
#
# Once it returns, both SSH tunnels run in the background. Stop with:
#   bash scripts/visivo_slurm_tunnel_dask.sh --stop
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

FRONTEND=""
SSH_USER="${USER}"
LOCAL_PORT="${VISIVO_PORT:-18900}"
# The clone on the frontend, relative to the remote home (an older checkout
# may still be called VisIVO-next: pass --repo-dir or set VISIVO_REMOTE_REPO).
REMOTE_REPO="${VISIVO_REMOTE_REPO:-VisIVOVisualAnalytics}"
LOCAL_DASH_PORT="${VISIVO_DASHBOARD_PORT:-18787}"
WITH_DASHBOARD=true
REMOTE_OPTS=()
PIDFILE="${HOME}/.visivo_slurm_tunnel_dask.pid"
DASH_PIDFILE="${HOME}/.visivo_slurm_tunnel_dask_dashboard.pid"
JOBFILE="${HOME}/.visivo_slurm_dask_job"
# The remote backend's token. Not ~/.visivo_token: that is the local
# backend's, which the desktop would then fail to authenticate against.
TOKENFILE="${HOME}/.visivo_slurm_dask_token"
STOP=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --frontend)       FRONTEND="$2"; shift 2 ;;
        --repo-dir)      REMOTE_REPO="$2"; shift 2 ;;
        --user)           SSH_USER="$2"; shift 2 ;;
        --port)           LOCAL_PORT="$2"; REMOTE_OPTS+=("--port" "$2"); shift 2 ;;
        --dashboard-port) LOCAL_DASH_PORT="$2"; shift 2 ;;
        --no-dashboard)   WITH_DASHBOARD=false; shift ;;
        --stop)           STOP=true; shift ;;
        --help|-h)
            grep '^#' "$0" | sed 's/^# \{0,2\}//'
            exit 0
            ;;
        # Reject empty positional args explicitly — same rationale as the
        # single-node wrapper: silent empty args shift subsequent flags
        # into wrong slots.
        "")
            echo "[visivo] ERROR: empty argument detected. Did you forget to set" >&2
            echo "         an env var like \$VISIVO_POWER9_TOKEN? Run" >&2
            echo "             echo \"len: \${#VISIVO_POWER9_TOKEN}\"" >&2
            echo "         and ensure it prints a non-zero length." >&2
            exit 2
            ;;
        *) REMOTE_OPTS+=("$1"); shift ;;
    esac
done

# ── Stop mode ─────────────────────────────────────────────────────────────────
if [[ "$STOP" == "true" ]]; then
    for pf in "$PIDFILE" "$DASH_PIDFILE"; do
        if [[ -f "$pf" ]]; then
            PID="$(cat "$pf")"
            if kill -0 "$PID" 2>/dev/null; then
                kill "$PID"
                echo "[visivo] SSH tunnel stopped (pid $PID, from $pf)."
            fi
            rm -f "$pf"
        fi
    done
    if [[ -f "$JOBFILE" ]]; then
        read -r SLURM_JOB_ID SLURM_FRONTEND SLURM_USER < "$JOBFILE"
        echo "[visivo] Cancelling SLURM job ${SLURM_JOB_ID} on ${SLURM_FRONTEND}…"
        ssh "${SLURM_USER}@${SLURM_FRONTEND}" "scancel ${SLURM_JOB_ID}" || true
        rm -f "$JOBFILE"
    fi
    rm -f "$TOKENFILE"
    exit 0
fi

# ── Validate ──────────────────────────────────────────────────────────────────
if [[ -z "$FRONTEND" ]]; then
    echo "ERROR: --frontend <host> is required" >&2
    exit 2
fi

# ── Submit via SSH and capture the launcher's output ─────────────────────────
echo "[visivo] Submitting Dask multi-node backend job on ${SSH_USER}@${FRONTEND}…"
REMOTE_CMD=(bash "${REMOTE_REPO}/scripts/launch_backend_slurm_dask.sh" "${REMOTE_OPTS[@]}")

# Stream + capture (BSD mktemp on macOS: no trailing suffix).
TMP="$(mktemp /tmp/visivo_slurm_dask_XXXXXX)"
# ssh joins its arguments into one remote shell line: quote each one.
REMOTE_LINE="$(printf '%q ' "${REMOTE_CMD[@]}")"
ssh "${SSH_USER}@${FRONTEND}" "$REMOTE_LINE" 2>&1 | tee "$TMP"

# Parse the launcher's output. The dask launcher prints (see README block):
#   [visivo] Job ID: <N>
#   [visivo] Backend host (first node): <hostname>
#   ║  Backend port:    <port>
#   ║  Dashboard:       http://<host>:<port>
#   ║  Token: <token>     (read from token line after the ascii box)
JOB_ID="$(grep -oE 'Job ID: [0-9]+' "$TMP" | head -1 | awk '{print $3}')"
NODE="$(grep -oE 'Backend host \(first node\): [^ ]+' "$TMP" | head -1 | awk '{print $5}')"
REMOTE_PORT="$(grep -oE 'Backend port: +[0-9]+' "$TMP" | head -1 | awk '{print $3}')"
DASH_PORT="$(grep -oE 'Dashboard: +http://[^:]+:[0-9]+' "$TMP" \
             | head -1 | sed -E 's/.*:([0-9]+)$/\1/')"
TOKEN="$(grep -oE 'Token: +[A-Za-z0-9_.-]+' "$TMP" | head -1 | awk '{print $2}')"

if [[ -z "$JOB_ID" || -z "$NODE" || -z "$REMOTE_PORT" || -z "$TOKEN" ]]; then
    echo "[visivo] ERROR: could not parse the launcher's output." >&2
    echo "[visivo] Saved to $TMP for inspection." >&2
    exit 1
fi
rm -f "$TMP"

# Remember the job id + frontend so --stop can kill it later.
echo "${JOB_ID} ${FRONTEND} ${SSH_USER}" > "$JOBFILE"

# ── Backend tunnel ────────────────────────────────────────────────────────────
echo "[visivo] Opening backend tunnel: localhost:${LOCAL_PORT} → ${NODE}:${REMOTE_PORT} via ${FRONTEND}"
ssh -N -f \
    -L "${LOCAL_PORT}:${NODE}:${REMOTE_PORT}" \
    -o ExitOnForwardFailure=yes \
    -o ServerAliveInterval=30 \
    "${SSH_USER}@${FRONTEND}"
sleep 1
TUNNEL_PID="$(pgrep -f "ssh -N.*${LOCAL_PORT}:${NODE}:${REMOTE_PORT}" | head -1 || true)"
[[ -n "$TUNNEL_PID" ]] && echo "$TUNNEL_PID" > "$PIDFILE"

# ── Dashboard tunnel (optional) ───────────────────────────────────────────────
DASH_LINE=""
if [[ "$WITH_DASHBOARD" == "true" && -n "$DASH_PORT" ]]; then
    echo "[visivo] Opening dashboard tunnel: localhost:${LOCAL_DASH_PORT} → ${NODE}:${DASH_PORT} via ${FRONTEND}"
    ssh -N -f \
        -L "${LOCAL_DASH_PORT}:${NODE}:${DASH_PORT}" \
        -o ExitOnForwardFailure=yes \
        -o ServerAliveInterval=30 \
        "${SSH_USER}@${FRONTEND}"
    sleep 1
    DASH_PID="$(pgrep -f "ssh -N.*${LOCAL_DASH_PORT}:${NODE}:${DASH_PORT}" | head -1 || true)"
    [[ -n "$DASH_PID" ]] && echo "$DASH_PID" > "$DASH_PIDFILE"
    DASH_LINE="  Dashboard:    http://localhost:${LOCAL_DASH_PORT}"
fi

cat <<EOF

══════════════════════════════════════════════════════════════════════
  VisIVO backend (Dask multi-node) tunnel READY
══════════════════════════════════════════════════════════════════════
  SLURM job:    ${JOB_ID} (driver on ${NODE})
  Local URL:    http://localhost:${LOCAL_PORT}
  Token:        ${TOKEN}
  Token file:   ${TOKENFILE}
${DASH_LINE}
══════════════════════════════════════════════════════════════════════

  In VisIVO desktop:
      Settings → Remote Backends → Add
          ID:        src-slurm-dask
          URL:       http://localhost:${LOCAL_PORT}
          Token:     <paste from above>
          SRC code:  <the node's code in SKAVA>

  Tail the SLURM job log:
      ssh ${SSH_USER}@${FRONTEND} \\
          tail -f \\\$HOME/.visivo_logs/visivo_backend_dask_${JOB_ID}.log

  Stop everything (tunnels + scancel):
      bash scripts/visivo_slurm_tunnel_dask.sh --stop

══════════════════════════════════════════════════════════════════════
EOF

# A private file renamed into place: an existing one keeps its mode on overwrite.
TOKENTMP="$(umask 077 && mktemp "${TOKENFILE}.XXXXXX")"
printf '%s\n' "$TOKEN" > "$TOKENTMP" && mv -f "$TOKENTMP" "$TOKENFILE"
