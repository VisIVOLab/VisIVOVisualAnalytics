#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# visivo_slurm_tunnel.sh
# Mac-side / laptop-side helper that:
#   1. SSHes into the SLURM frontend
#   2. Submits the backend job via launch_backend_slurm.sh
#   3. Extracts the compute-node hostname + auth token from the output
#   4. Opens an SSH tunnel from the laptop to the compute node
#   5. Prints the URL + token for the desktop client's Remote Backends form
#
# So the operator goes from "I want the backend on Power9" to
# "VisIVO desktop knows where to connect" in a single command.
#
# Usage:
#   bash scripts/visivo_slurm_tunnel.sh \
#       --frontend pleiadi-gpu.oact.inaf.it \
#       --user     fvitello \
#       --partition gpu --gres gpu:1 \
#       --data-root /data \
#       --sif ~/VisIVOVisualAnalytics/backend-gpu.sif
#
# After it returns, the SSH tunnel is running in the background; kill it
# (and free the local port) with:
#   (--repo-dir DIR: the clone on the frontend, relative to its home;
#    default VisIVOVisualAnalytics, or $VISIVO_REMOTE_REPO)
#
#   bash scripts/visivo_slurm_tunnel.sh --stop
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

FRONTEND=""
SSH_USER="${USER}"
LOCAL_PORT="${VISIVO_PORT:-8000}"
# The clone on the frontend, relative to the remote home (an older checkout
# may still be called VisIVO-next: pass --repo-dir or set VISIVO_REMOTE_REPO).
REMOTE_REPO="${VISIVO_REMOTE_REPO:-VisIVOVisualAnalytics}"
REMOTE_OPTS=()
PIDFILE="${HOME}/.visivo_slurm_tunnel.pid"
JOBFILE="${HOME}/.visivo_slurm_job"
STOP=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --frontend)  FRONTEND="$2"; shift 2 ;;
        --repo-dir) REMOTE_REPO="$2"; shift 2 ;;
        --user)      SSH_USER="$2"; shift 2 ;;
        --port)      LOCAL_PORT="$2"; REMOTE_OPTS+=("--port" "$2"); shift 2 ;;
        --stop)      STOP=true; shift ;;
        --help|-h)
            grep '^#' "$0" | sed 's/^# \{0,2\}//'
            exit 0
            ;;
        # Reject empty positional args explicitly. They survive bash's
        # quoting locally but collapse to nothing inside ``ssh "$@"``,
        # which then shifts subsequent flags' values into wrong slots
        # ("Unknown option: <next-value>"). Catching the empty string
        # here surfaces the real problem (probably an unset env var)
        # with an actionable message instead of a misleading one.
        "")
            echo "[visivo] ERROR: empty argument detected. Did you forget to set" >&2
            echo "         an env var like \$VISIVO_POWER9_TOKEN? Run" >&2
            echo "             echo \"len: \${#VISIVO_POWER9_TOKEN}\"" >&2
            echo "         and ensure it prints a non-zero length." >&2
            exit 2
            ;;
        # Pass everything else through to the remote launch script.
        *) REMOTE_OPTS+=("$1"); shift ;;
    esac
done

# ── Stop mode ─────────────────────────────────────────────────────────────────
if [[ "$STOP" == "true" ]]; then
    if [[ -f "$PIDFILE" ]]; then
        PID="$(cat "$PIDFILE")"
        if kill -0 "$PID" 2>/dev/null; then
            kill "$PID"
            echo "[visivo] SSH tunnel stopped (pid $PID)."
        fi
        rm -f "$PIDFILE"
    else
        echo "[visivo] No tunnel pidfile found at $PIDFILE."
    fi
    if [[ -f "$JOBFILE" ]]; then
        read -r SLURM_JOB_ID SLURM_FRONTEND SLURM_USER < "$JOBFILE"
        echo "[visivo] Cancelling SLURM job ${SLURM_JOB_ID} on ${SLURM_FRONTEND}…"
        ssh "${SLURM_USER}@${SLURM_FRONTEND}" "scancel ${SLURM_JOB_ID}" || true
        rm -f "$JOBFILE"
    fi
    exit 0
fi

# ── Validate ──────────────────────────────────────────────────────────────────
if [[ -z "$FRONTEND" ]]; then
    echo "ERROR: --frontend <host> is required" >&2
    exit 2
fi

# ── Submit via SSH and capture the launcher's output ─────────────────────────
echo "[visivo] Submitting backend job on ${SSH_USER}@${FRONTEND}…"
REMOTE_CMD=(bash "${REMOTE_REPO}/scripts/launch_backend_slurm.sh" "${REMOTE_OPTS[@]}")

# Stream the launcher's stdout to our terminal AND capture it for parsing.
# macOS BSD mktemp wants ``XXXXXX`` at the END of the template; a
# trailing suffix (``.out``) confuses it and surfaces as a misleading
# ``File exists`` error. Drop the suffix entirely — the file is local
# and ephemeral, no extension needed.
TMP="$(mktemp /tmp/visivo_slurm_XXXXXX)"
ssh "${SSH_USER}@${FRONTEND}" "${REMOTE_CMD[@]}" 2>&1 | tee "$TMP"

JOB_ID="$(grep -oE 'Job ID: [0-9]+' "$TMP" | head -1 | awk '{print $3}')"
NODE="$(grep -oE 'Compute node: [^ ]+' "$TMP" | head -1 | awk '{print $3}')"
REMOTE_PORT="$(grep -oE 'Port: +[0-9]+' "$TMP" | head -1 | awk '{print $2}')"
TOKEN="$(grep -oE 'Token: +[A-Za-z0-9_.-]+' "$TMP" | head -1 | awk '{print $2}')"

if [[ -z "$JOB_ID" || -z "$NODE" || -z "$REMOTE_PORT" || -z "$TOKEN" ]]; then
    echo "[visivo] ERROR: could not parse the launcher's output." >&2
    echo "[visivo] Saved to $TMP for inspection." >&2
    exit 1
fi
rm -f "$TMP"

# Remember the job id + frontend so --stop can kill it later.
echo "${JOB_ID} ${FRONTEND} ${SSH_USER}" > "$JOBFILE"

# ── Open the SSH tunnel in the background ─────────────────────────────────────
echo "[visivo] Opening tunnel: localhost:${LOCAL_PORT} → ${NODE}:${REMOTE_PORT} via ${FRONTEND}"
ssh -N -f \
    -L "${LOCAL_PORT}:${NODE}:${REMOTE_PORT}" \
    -o ExitOnForwardFailure=yes \
    -o ServerAliveInterval=30 \
    "${SSH_USER}@${FRONTEND}"

# Capture the SSH pid for --stop.
sleep 1
TUNNEL_PID="$(pgrep -f "ssh -N.*${LOCAL_PORT}:${NODE}:${REMOTE_PORT}" | head -1 || true)"
if [[ -n "$TUNNEL_PID" ]]; then
    echo "$TUNNEL_PID" > "$PIDFILE"
fi

cat <<EOF

══════════════════════════════════════════════════════════════════════
  VisIVO backend tunnel READY
══════════════════════════════════════════════════════════════════════
  SLURM job:    ${JOB_ID} (on ${NODE})
  Local URL:    http://localhost:${LOCAL_PORT}
  Token:        ${TOKEN}
  Token file:   ${HOME}/.visivo_token
══════════════════════════════════════════════════════════════════════

  In VisIVO desktop:
      Settings → Remote Backends → Add
          ID:        src-power9-slurm
          URL:       http://localhost:${LOCAL_PORT}
          Token:     <paste from above>
          SRC code:  POWER9

  Tail the SLURM job log:
      ssh ${SSH_USER}@${FRONTEND} \\
          tail -f \\\$HOME/.visivo_logs/visivo_backend_${JOB_ID}.log

  Stop everything (tunnel + scancel):
      bash scripts/visivo_slurm_tunnel.sh --stop

══════════════════════════════════════════════════════════════════════
EOF

# Save the token for the desktop launcher if it ever reads from this path.
echo "$TOKEN" > "$HOME/.visivo_token"
chmod 600 "$HOME/.visivo_token"
