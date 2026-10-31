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
                # The reconnect loop ends its own ssh on TERM.
                kill "$PID"
                for _ in $(seq 1 20); do
                    kill -0 "$PID" 2>/dev/null || break
                    sleep 0.25
                done
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
# Before submitting anything: a tunnel still open from an earlier run, or a
# local port already taken, would make the new tunnels fail after a job had
# been submitted, and --stop would then know only the new job.
for pf in "$PIDFILE" "$DASH_PIDFILE"; do
    if [[ -f "$pf" ]] && kill -0 "$(cat "$pf")" 2>/dev/null; then
        echo "[visivo] ERROR: a tunnel from an earlier run is still active (pid $(cat "$pf"))." >&2
        echo "[visivo]        Stop it first: bash $0 --stop" >&2
        exit 1
    fi
done
if [[ -f "$JOBFILE" ]]; then
    echo "[visivo] ERROR: a job from an earlier run is still recorded ($(cut -d' ' -f1 "$JOBFILE"))." >&2
    echo "[visivo]        Stop it first: bash $0 --stop" >&2
    exit 1
fi
if [[ "$WITH_DASHBOARD" == "true" && "$LOCAL_PORT" == "$LOCAL_DASH_PORT" ]]; then
    echo "[visivo] ERROR: --port and --dashboard-port are the same (${LOCAL_PORT})." >&2
    exit 2
fi
PORTS=("$LOCAL_PORT")
[[ "$WITH_DASHBOARD" == "true" ]] && PORTS+=("$LOCAL_DASH_PORT")
for port in "${PORTS[@]}"; do
    if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then
        echo "[visivo] ERROR: localhost:${port} is already in use." >&2
        echo "[visivo]        Stop what holds it, or choose another --port / --dashboard-port." >&2
        exit 1
    fi
done

# ── Submit via SSH and capture the launcher's output ─────────────────────────
echo "[visivo] Submitting Dask multi-node backend job on ${SSH_USER}@${FRONTEND}…"
REMOTE_CMD=(bash "${REMOTE_REPO}/scripts/launch_backend_slurm_dask.sh" "${REMOTE_OPTS[@]}")

# Stream + capture (BSD mktemp on macOS: no trailing suffix).
TMP="$(mktemp /tmp/visivo_slurm_dask_XXXXXX)"
# ssh joins its arguments into one remote shell line: quote each one.
REMOTE_LINE="$(printf '%q ' "${REMOTE_CMD[@]}")"
LAUNCH_RC=0
ssh "${SSH_USER}@${FRONTEND}" "$REMOTE_LINE" 2>&1 | while IFS= read -r line; do
    printf '%s\n' "$line"
    # errexit does not apply in this loop (the pipeline is tested below): a
    # failed write ends it explicitly, which the status then reports.
    printf '%s\n' "$line" >> "$TMP" || exit 1
    if [[ ! -f "$JOBFILE" && "$line" =~ Job\ ID:\ ([0-9]+) ]]; then
        echo "${BASH_REMATCH[1]} ${FRONTEND} ${SSH_USER}" > "$JOBFILE" || exit 1
    fi
done || LAUNCH_RC=$?

# Parse the launcher's output. The dask launcher prints (see README block):
#   [visivo] Job ID: <N>
#   [visivo] Backend host (first node): <hostname>
#   ║  Backend port:    <port>
#   ║  Dashboard:       http://<host>:<port>
#   ║  Token: <token>     (read from token line after the ascii box)
JOB_ID="$(grep -oE 'Job ID: [0-9]+' "$TMP" | head -1 | awk '{print $3}' || true)"
# The loop above recorded the job as soon as the launcher printed its id.
NODE="$(grep -oE 'Backend host \(first node\): [^ ]+' "$TMP" | head -1 | awk '{print $5}' || true)"
REMOTE_PORT="$(grep -oE 'Backend port: +[0-9]+' "$TMP" | head -1 | awk '{print $3}' || true)"
DASH_PORT="$(grep -oE 'Dashboard: +http://[^:]+:[0-9]+' "$TMP" \
             | head -1 | sed -E 's/.*:([0-9]+)$/\1/' || true)"
TOKEN="$(grep -oE 'Token: +[A-Za-z0-9_.-]+' "$TMP" | head -1 | awk '{print $2}' || true)"

if [[ "$LAUNCH_RC" -ne 0 || -z "$JOB_ID" || -z "$NODE" || -z "$REMOTE_PORT" || -z "$TOKEN" ]]; then
    if [[ "$LAUNCH_RC" -ne 0 ]]; then
        echo "[visivo] ERROR: the launcher failed (exit $LAUNCH_RC); see its output above." >&2
    else
        echo "[visivo] ERROR: could not parse the launcher's output." >&2
    fi
    echo "[visivo] Saved to $TMP for inspection." >&2
    if [[ -n "$JOB_ID" ]]; then
        echo "[visivo] Job ${JOB_ID} may still be queued or running: bash $0 --stop cancels it." >&2
    fi
    exit 1
fi
rm -f "$TMP"

# A tunnel that drops (a network change, the laptop sleeping, an idle timeout)
# is opened again by itself: ssh runs in a loop in the background, with
# keep-alives so a dead connection is noticed within a minute. --stop ends the
# loop and its ssh. Non-interactive (BatchMode): key-based access.
open_tunnel() {   # <local:host:remote> <pidfile>
    nohup bash -c '
        child=
        trap "[ -n \"\$child\" ] && kill \$child 2>/dev/null; exit 0" TERM INT
        while true; do
            ssh -N -o BatchMode=yes -o ExitOnForwardFailure=yes \
                -o ServerAliveInterval=15 -o ServerAliveCountMax=4 \
                -L "$1" "$2" &
            child=$!
            wait "$child"
            child=
            sleep 3 & wait $!
        done' visivo-tunnel "$1" "${SSH_USER}@${FRONTEND}" >/dev/null 2>&1 &
    echo "$!" > "$2"
}

# ── Backend tunnel ────────────────────────────────────────────────────────────
echo "[visivo] Opening backend tunnel: localhost:${LOCAL_PORT} → ${NODE}:${REMOTE_PORT} via ${FRONTEND}"
open_tunnel "${LOCAL_PORT}:${NODE}:${REMOTE_PORT}" "$PIDFILE"
# Ready means the backend answers through the tunnel, not only that the local
# port listens.
READY=false
DEADLINE=$((SECONDS + 30))
while (( SECONDS < DEADLINE )); do
    LEFT=$((DEADLINE - SECONDS))
    (( LEFT > 0 )) || break   # --max-time 0 would mean no limit at all
    if curl -sf -o /dev/null --max-time "$(( LEFT < 3 ? LEFT : 3 ))" \
            -H "X-Visivo-Token: ${TOKEN}" "http://localhost:${LOCAL_PORT}/v1/health"; then
        READY=true
        break
    fi
    sleep 0.5
done
if [[ "$READY" != "true" ]]; then
    kill "$(cat "$PIDFILE")" 2>/dev/null || true
    rm -f "$PIDFILE"
    echo "[visivo] ERROR: the backend did not answer through the tunnel within 30 s." >&2
    echo "[visivo]        The job (${JOB_ID}) is still queued or running: bash $0 --stop cancels it." >&2
    exit 1
fi

# ── Dashboard tunnel (optional) ───────────────────────────────────────────────
DASH_LINE=""
if [[ "$WITH_DASHBOARD" == "true" && -n "$DASH_PORT" ]]; then
    echo "[visivo] Opening dashboard tunnel: localhost:${LOCAL_DASH_PORT} → ${NODE}:${DASH_PORT} via ${FRONTEND}"
    open_tunnel "${LOCAL_DASH_PORT}:${NODE}:${DASH_PORT}" "$DASH_PIDFILE"
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
