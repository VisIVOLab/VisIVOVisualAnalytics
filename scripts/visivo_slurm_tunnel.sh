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
# So the operator goes from "I want the backend on the cluster" to
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
LOCAL_PORT="${VISIVO_PORT:-18900}"
PORT_GIVEN=false
[[ -n "${VISIVO_PORT:-}" ]] && PORT_GIVEN=true
# The clone on the frontend, relative to the remote home (an older checkout
# may still be called VisIVO-next: pass --repo-dir or set VISIVO_REMOTE_REPO).
REMOTE_REPO="${VISIVO_REMOTE_REPO:-VisIVOVisualAnalytics}"
REMOTE_OPTS=()
PIDFILE="${HOME}/.visivo_slurm_tunnel.pid"
JOBFILE="${HOME}/.visivo_slurm_job"
# The remote backend's token. Not ~/.visivo_token: that is the local
# backend's, which the desktop would then fail to authenticate against.
TOKENFILE="${HOME}/.visivo_slurm_token"
STOP=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --frontend)  FRONTEND="$2"; shift 2 ;;
        --repo-dir) REMOTE_REPO="$2"; shift 2 ;;
        --user)      SSH_USER="$2"; shift 2 ;;
        --port)      LOCAL_PORT="$2"; PORT_GIVEN=true; REMOTE_OPTS+=("--port" "$2"); shift 2 ;;
        --node-code) NODE_CODE="$2"; REMOTE_OPTS+=("--node-code" "$2"); shift 2 ;;
        --workspace) WORKSPACE_DIR="$2"; REMOTE_OPTS+=("--workspace" "$2"); shift 2 ;;
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
            # The reconnect loop ends its own ssh on TERM.
            kill "$PID"
            for _ in $(seq 1 20); do
                kill -0 "$PID" 2>/dev/null || break
                sleep 0.25
            done
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
    rm -f "$TOKENFILE"
    exit 0
fi

# ── Validate ──────────────────────────────────────────────────────────────────
if [[ -z "$FRONTEND" ]]; then
    echo "ERROR: --frontend <host> is required" >&2
    exit 2
fi
# Before submitting anything: a tunnel still open (or its reconnect loop) on
# the local port would make the new one fail after a job had been submitted,
# and --stop would then know only the new job.
if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "[visivo] ERROR: a tunnel from an earlier run is still active (pid $(cat "$PIDFILE"))." >&2
    echo "[visivo]        Stop it first: bash $0 --stop" >&2
    exit 1
fi
if [[ -f "$JOBFILE" ]]; then
    echo "[visivo] ERROR: a job from an earlier run is still recorded ($(cut -d' ' -f1 "$JOBFILE"))." >&2
    echo "[visivo]        Stop it first: bash $0 --stop" >&2
    exit 1
fi
# The desktop client starts its own local backend on the port of its saved
# Backend URL (Settings.ini, [Backend] url; http://127.0.0.1:8000 by default),
# possibly after this tunnel is up: the tunnel must never take that port.
client_backend_port() {
    local ini="${HOME}/VisIVO Visual Analytics/Settings.ini" url=""
    if [[ -f "$ini" ]]; then
        url="$(awk '/^\[/{s=($0=="[Backend]")} s && /^url=/{sub(/^url=/,""); print; exit}' "$ini" || true)"
    fi
    url="${url:-http://127.0.0.1:8000}"
    local port="${url##*:}"
    port="${port%%/*}"
    [[ "$port" =~ ^[0-9]+$ ]] || port=80
    printf '%s' "$port"
}
port_in_use() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }

CLIENT_PORT="$(client_backend_port)"
if [[ "$LOCAL_PORT" == "$CLIENT_PORT" ]] || port_in_use "$LOCAL_PORT"; then
    if [[ "$PORT_GIVEN" == "true" ]]; then
        if [[ "$LOCAL_PORT" == "$CLIENT_PORT" ]]; then
            echo "[visivo] ERROR: localhost:${LOCAL_PORT} is the port of the desktop client's own local" >&2
            echo "[visivo]        backend (Settings ▸ Backend URL). Choose another --port." >&2
        else
            echo "[visivo] ERROR: localhost:${LOCAL_PORT} is already in use." >&2
            echo "[visivo]        Stop what holds it, or choose another --port." >&2
        fi
        exit 1
    fi
    WANTED="$LOCAL_PORT"
    for candidate in $(seq $((LOCAL_PORT + 1)) $((LOCAL_PORT + 50))); do
        if [[ "$candidate" != "$CLIENT_PORT" ]] && ! port_in_use "$candidate"; then
            LOCAL_PORT="$candidate"
            break
        fi
    done
    if [[ "$LOCAL_PORT" == "$WANTED" ]]; then
        echo "[visivo] ERROR: no free local port found after ${WANTED}; choose one with --port." >&2
        exit 1
    fi
    echo "[visivo] localhost:${WANTED} is taken (or is the client's local backend port); using ${LOCAL_PORT}."
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
# ssh joins its arguments into one command line for the remote shell: each
# argument is quoted for that shell first, so a value with spaces or shell
# characters reaches the launcher as one argument, and only as data.
REMOTE_LINE="$(printf '%q ' "${REMOTE_CMD[@]}")"
# The launcher can fail after submitting (no node assigned in time, backend not
# ready): its status must not end this script before the job is recorded.
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

JOB_ID="$(grep -oE 'Job ID: [0-9]+' "$TMP" | head -1 | awk '{print $3}' || true)"
# The loop above recorded the job as soon as the launcher printed its id.
NODE="$(grep -oE 'Compute node: [^ ]+' "$TMP" | head -1 | awk '{print $3}' || true)"
REMOTE_PORT="$(grep -oE 'Port: +[0-9]+' "$TMP" | head -1 | awk '{print $2}' || true)"
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

# ── Open the SSH tunnel in the background ─────────────────────────────────────
echo "[visivo] Opening tunnel: localhost:${LOCAL_PORT} → ${NODE}:${REMOTE_PORT} via ${FRONTEND}"
# A tunnel that drops (a network change, the laptop sleeping, an idle
# timeout) is opened again by itself: ssh runs in a loop in the background,
# with keep-alives so a dead connection is noticed within a minute. --stop
# ends the loop and its ssh. Non-interactive (BatchMode): key-based access.
# The loop ends its own ssh when it is told to stop (TERM), so no ssh it
# starts can outlive it.
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
    done' visivo-tunnel "${LOCAL_PORT}:${NODE}:${REMOTE_PORT}" "${SSH_USER}@${FRONTEND}" \
    >/dev/null 2>&1 &
echo "$!" > "$PIDFILE"
# Ready means the backend answers through the tunnel, not only that the
# local port listens.
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

BACKEND_NAME="${NODE_CODE:-${FRONTEND%%.*}}"

cat <<EOF

══════════════════════════════════════════════════════════════════════
  VisIVO backend tunnel READY
══════════════════════════════════════════════════════════════════════
  SLURM job:    ${JOB_ID} (on ${NODE})
  Local URL:    http://localhost:${LOCAL_PORT}
  Token:        ${TOKEN}
  Token file:   ${TOKENFILE}
══════════════════════════════════════════════════════════════════════

  Open VisIVO on this backend (this run only; a plain launch stays local):
      macOS:   open -n -a VisIVOVisualAnalytics --args \\
                   --backend http://localhost:${LOCAL_PORT} --backend-token-file ${TOKENFILE} \\
                   --backend-name ${BACKEND_NAME}
      Linux:   VisIVOVisualAnalytics --backend http://localhost:${LOCAL_PORT} \\
                   --backend-token-file ${TOKENFILE} --backend-name ${BACKEND_NAME}

  For datasets found through SKAVA, also register it:
      Settings → Remote Backends → Add
          ID:        src-$(printf '%s' "${NODE_CODE:-node}" | tr '[:upper:]' '[:lower:]')
          URL:       http://localhost:${LOCAL_PORT}
          Token:     <paste from above>
          SRC code:  ${NODE_CODE:-<the node code in SKAVA>}

  Tail the SLURM job log:
      ssh ${SSH_USER}@${FRONTEND} \\
          tail -f ${WORKSPACE_DIR:-\\\$HOME/.visivo_logs}${WORKSPACE_DIR:+/logs}/visivo_backend_${JOB_ID}.log

  Stop everything (tunnel + scancel):
      bash scripts/visivo_slurm_tunnel.sh --stop

══════════════════════════════════════════════════════════════════════
EOF

# A private file renamed into place: an existing one keeps its mode on overwrite.
TOKENTMP="$(umask 077 && mktemp "${TOKENFILE}.XXXXXX")"
printf '%s\n' "$TOKEN" > "$TOKENTMP" && mv -f "$TOKENTMP" "$TOKENFILE"
