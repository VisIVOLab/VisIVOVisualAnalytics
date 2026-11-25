#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# launch_backend_slurm.sh
# Submit the VisIVO backend to a SLURM compute node, wait for it to become
# ready, and print the SSH tunnel command the operator runs on the laptop.
#
# Written for the INAF PLEIADI-GPU system, where ``pleiadi-gpu.oact.inaf.it``
# is the frontend (login node) of a SLURM cluster, and works on any SLURM site
# with plain options; ``--site leonardo`` adds the defaults of CINECA Leonardo.
# The frontend should not run long-lived services; this script puts the
# backend on a real compute node instead, optionally with GPU allocation.
#
# Usage:
#   bash scripts/launch_backend_slurm.sh [options]
#
# The desktop client does not need a clone of the repository on the frontend:
# it sends this script over the ssh connection (`bash -s -- <options>`, the
# script on standard input, wrapped so that it runs from a temporary copy).
# Run that way there is no apptainer/backend.sif next to it, so --image or
# --sif is required.
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
#   --sif        PATH       Apptainer/Singularity .sif image
#                           (default: apptainer/backend.sif, or the file --image
#                           pulls to)
#   --image      REF        An image to pull once on the frontend (compute nodes
#                           often have no internet), e.g.
#                           docker://ghcr.io/visivolab/visivo-backend:<version>-egl
#                           It is pulled to --sif when given, else to a .sif
#                           under --image-dir (default <workspace>/images), and
#                           reused while the file exists (delete it to pull
#                           again). The pull prints "[visivo] Pull: ..." lines
#                           every few seconds. ($VISIVO_IMAGE)
#   --image-dir  DIR        Where a pulled image goes: a directory shared by a
#                           project, so that it is pulled once for everyone
#                           ($VISIVO_IMAGE_DIR; default <workspace>/images; on
#                           leonardo the project's work area, see below)
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
#   --account    ACCOUNT    SLURM account (project) to charge ($VISIVO_ACCOUNT;
#                           required on CINECA Leonardo and BSC MareNostrum 5)
#   --qos        QOS        SLURM quality of service ($VISIVO_QOS)
#   --job-name   NAME       SLURM job name (default: visivo_backend). A unique
#                           name lets a client cancel the job by name when it
#                           never saw the job id (scancel --name).
#   --assign-timeout SEC    How long to wait for SLURM to give the job a node
#                           ($VISIVO_ASSIGN_TIMEOUT; default 300; 0: for ever).
#                           While it waits, the launcher prints "[visivo] Queue:"
#                           lines with the job's state and SLURM's reason.
#   --ready-timeout SEC     How long to wait for the backend to answer once the
#                           job runs ($VISIVO_READY_TIMEOUT; default 300, 600 on
#                           leonardo: a first start reads the image from a shared
#                           file system).
#   --node-code  CODE       The node's code in the SKAVA catalogue, reported
#                           in /v1/node/status ($VISIVO_NODE_CODE)
#   --version    TEXT       What the status document reports as the backend
#                           version, e.g. the VisIVO commit the image was
#                           built from ($VISIVO_VERSION; default: the image's)
#   --site       NAME       Site profile: leonardo, pleiadi or generic ($VISIVO_SITE;
#                           default: detected from the host name, else generic).
#                           A profile only sets defaults for what an option or
#                           variable does not give; see "Site profiles" below.
#   --cleanenv | --no-cleanenv
#                           Run the container with --cleanenv, so that the host's
#                           environment does not reach it ($VISIVO_CLEANENV=1|0;
#                           default: on for leonardo, off elsewhere). The variables
#                           the backend needs are passed explicitly.
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
#   # CINECA Leonardo, from a login node: A100, image pulled once
#   bash scripts/launch_backend_slurm.sh --site leonardo --account <ACCOUNT> \
#       --image docker://ghcr.io/visivolab/visivo-backend:<version>-egl \
#       --data-root /path/to/simulation/data
#
# Site profiles (defaults only; every option and variable above still wins):
#   generic   what the options say. The default.
#   pleiadi   the same as generic (the defaults above were written for it).
#   leonardo  partition boost_usr_prod; an account is required (--account;
#             --qos boost_qos_dbg for a short test); the container runs with
#             --cleanenv; a pulled image goes to the project's work area
#             (/leonardo_work/<account> when it exists, else $WORK) under
#             visivo/images, so that a project pulls it once; the workspace (logs, token, caches, exports, images)
#             and the data root default to the user's scratch area
#             ($CINECA_SCRATCH, else $SCRATCH, else $WORK), because $HOME has a
#             small quota; the host's glvnd vendor directory is bound for EGL.
#             The container runtime is `singularity` there (no `apptainer`):
#             the launcher uses whichever exists ($VISIVO_CONTAINER_RUNTIME
#             overrides). The data root jails the backend: pass --data-root for
#             data that is elsewhere.
#
# What it does:
#   1. Submits a SLURM batch job that starts the Apptainer container with
#      --nv (when --gres includes gpu) and the path jail set.
#   2. Polls squeue every 2 s until the compute node is assigned.
#   3. Polls /v1/health via SSH to the compute node until the backend is up.
#   4. Prints the SSH tunnel command for the laptop + the auth token.
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail
ORIG_ARGS=("$@")   # for a re-run under a login shell (see the scratch lookup)

# ── Defaults ──────────────────────────────────────────────────────────────────
PARTITION="${VISIVO_PARTITION:-}"   # empty → the site profile's, else "gpu"
TIME="${VISIVO_TIME:-08:00:00}"
MEM="${VISIVO_MEM:-64G}"
CPUS="${VISIVO_CPUS:-8}"
WORKERS="${VISIVO_WORKERS:-}"  # default empty → matches CPUS
GRES="${VISIVO_GRES:-gpu:1}"
PORT="${VISIVO_PORT:-8000}"
BIND_PATH="${VISIVO_BIND:-$HOME}"
DATA_ROOT="${VISIVO_DATA_ROOT:-}"
SIF="${VISIVO_SIF:-}"        # empty → the pulled image, else apptainer/backend.sif
IMAGE="${VISIVO_IMAGE:-}"
IMAGE_DIR="${VISIVO_IMAGE_DIR:-}"
SITE="${VISIVO_SITE:-}"
CLEANENV="${VISIVO_CLEANENV:-}"
RUNTIME="${VISIVO_CONTAINER_RUNTIME:-}"
BACKEND_SRC="${VISIVO_BACKEND_SRC_OVERRIDE:-}"
TOKEN="${VISIVO_TOKEN:-}"
NODE_CODE="${VISIVO_NODE_CODE:-}"
JOB_NAME="${VISIVO_JOB_NAME:-}"
ACCOUNT="${VISIVO_ACCOUNT:-}"
QOS="${VISIVO_QOS:-}"
WORKSPACE="${VISIVO_WORKSPACE:-}"
VERSION="${VISIVO_VERSION:-}"
LOG_DIR="${VISIVO_LOG_DIR:-$HOME/.visivo_logs}"
# How long to wait for SLURM to assign a node (a queued job can wait longer);
# 0 waits for ever (the client's Cancel then ends it).
ASSIGN_TIMEOUT="${VISIVO_ASSIGN_TIMEOUT:-300}"
# How long to wait for the backend to answer once the job runs (empty: the
# site's, else 300 s). Polled every 5 s.
READY_TIMEOUT="${VISIVO_READY_TIMEOUT:-}"
# Run from standard input (the client's way), $0 is then a temporary copy or
# not a file at all, and the repository is not next to it.
STDIN_MODE=false
[[ -n "${VISIVO_LAUNCHER_STDIN:-}" || ! -f "$0" ]] && STDIN_MODE=true
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
        --image)     IMAGE="$2";     shift 2 ;;
        --image-dir) IMAGE_DIR="$2"; shift 2 ;;
        --assign-timeout) ASSIGN_TIMEOUT="$2"; shift 2 ;;
        --ready-timeout)  READY_TIMEOUT="$2";  shift 2 ;;
        --site)      SITE="$2";      shift 2 ;;
        --cleanenv)    CLEANENV=1;   shift ;;
        --no-cleanenv) CLEANENV=0;   shift ;;
        --backend-src) BACKEND_SRC="$2"; shift 2 ;;
        --token)     TOKEN="$2";     shift 2 ;;
        --node-code) NODE_CODE="$2"; shift 2 ;;
        --job-name)  JOB_NAME="$2";  shift 2 ;;
        --account)   ACCOUNT="$2";   shift 2 ;;
        --qos)       QOS="$2";       shift 2 ;;
        --workspace) WORKSPACE="$2"; shift 2 ;;
        --version)   VERSION="$2";   shift 2 ;;
        --no-gpu)    USE_GPU=false; GRES=""; shift ;;
        --help|-h)
            if [[ -f "$0" ]]; then grep '^#' "$0" | sed 's/^# \{0,2\}//'; else echo "See scripts/launch_backend_slurm.sh"; fi
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── Site profile ──────────────────────────────────────────────────────────────
# A profile only supplies defaults for what no option or variable gave, so any
# site stays usable with plain options. Detection is by host name (the launcher
# runs on the frontend); --site or VISIVO_SITE decides when it is wrong.
if [[ -z "$SITE" ]]; then
    # Leonardo: the login hosts (login01.leonardo.local, login01-ext.leonardo.cineca.it)
    # and the node names (lrdn..., lrdl...); a host that merely has the word in
    # its name stays generic.
    SITE_HOST="$(hostname -f 2>/dev/null || hostname)"
    SITE_HOST="$(printf '%s' "$SITE_HOST" | tr '[:upper:]' '[:lower:]')"
    case "$SITE_HOST" in
        login*.leonardo.local|*.leonardo.cineca.it|lrdn*|lrdl*) SITE=leonardo ;;
        *pleiadi*)           SITE=pleiadi ;;
        *)                   SITE=generic ;;
    esac
    SITE_HOW="detected"
else
    SITE_HOW="given"
fi
SITE_PARTITION="gpu"      # partition when none is given
SITE_CLEANENV=0           # --cleanenv when neither option nor variable says
SITE_NEEDS_ACCOUNT=false  # --account is mandatory on the site
SITE_SCRATCH=false        # workspace and data root default to the scratch area
SITE_SHARED_IMAGES=false  # pulled images default to the project's shared work area
SITE_READY_TIMEOUT=300    # seconds for the backend to answer once the job runs
# Host libraries EGL needs. /usr/lib64 is where PLEIADI (ppc64le) keeps them;
# the Debian-based image on an x86 site finds them through --nv and the vendor
# directory, and /usr/lib64 would only shadow its own.
EGL_LD_LIBRARY_PATH="/usr/lib64"
EGL_VENDOR_DIR=false      # bind the host's whole glvnd vendor directory
# ssh from the frontend to the compute node is how readiness is checked; where
# that can be refused, ask SLURM to run the check inside the job instead.
PROBE_SRUN=false
case "$SITE" in
    generic|pleiadi) ;;
    leonardo)
        SITE_PARTITION="boost_usr_prod"
        SITE_CLEANENV=1
        SITE_NEEDS_ACCOUNT=true
        SITE_SCRATCH=true
        SITE_SHARED_IMAGES=true
        SITE_READY_TIMEOUT=600
        EGL_LD_LIBRARY_PATH=""
        EGL_VENDOR_DIR=true
        PROBE_SRUN=true
        ;;
    *)
        echo "[visivo] ERROR: unknown site '$SITE' (leonardo, pleiadi or generic)" >&2
        exit 1
        ;;
esac
echo "[visivo] Site profile: $SITE ($SITE_HOW)"

PARTITION="${PARTITION:-$SITE_PARTITION}"
CLEANENV="${CLEANENV:-$SITE_CLEANENV}"
READY_TIMEOUT="${READY_TIMEOUT:-$SITE_READY_TIMEOUT}"
if [[ "$SITE_NEEDS_ACCOUNT" == "true" && -z "$ACCOUNT" ]]; then
    echo "[visivo] ERROR: the $SITE site needs your project account: --account <ACCOUNT> (or VISIVO_ACCOUNT)." >&2
    exit 1
fi

# The user's scratch area: CINECA exports it as $CINECA_SCRATCH; $SCRATCH and
# $WORK are the common names elsewhere. $HOME is small on these systems.
if [[ "$SITE_SCRATCH" == "true" ]]; then
    SCRATCH_DIR=""
    for cand in "${CINECA_SCRATCH:-}" "${SCRATCH:-}" "${WORK:-}"; do
        if [[ -n "$cand" && -d "$cand" ]]; then
            SCRATCH_DIR="$(cd -P "$cand" && pwd -P)"
            break
        fi
    done
    # Over a non-interactive ssh the shell is not a login shell, and the
    # variables the site's /etc/profile.d sets are missing: read them once.
    # The script must be a file to be run again: the client runs it from a
    # temporary copy for that reason; from a bare pipe there is nothing to re-run.
    if [[ -z "$SCRATCH_DIR" && -z "${VISIVO_LOGIN_SHELL:-}" ]]; then
        if [[ -f "$0" ]]; then
            VISIVO_LOGIN_SHELL=1 exec bash -l "$0" "${ORIG_ARGS[@]}"
        fi
        echo "[visivo] WARNING: the scratch variables are not set and the script is not a file, so it cannot be run again under a login shell." >&2
    fi
    if [[ -n "$SCRATCH_DIR" ]]; then
        [[ -z "$WORKSPACE" ]] && WORKSPACE="$SCRATCH_DIR/visivo"
        [[ -z "$DATA_ROOT" ]] && DATA_ROOT="$SCRATCH_DIR"
        # Pulled layers and their temporary files, where there is room.
        : "${APPTAINER_CACHEDIR:=$SCRATCH_DIR/visivo/cache/containers}"
        : "${SINGULARITY_CACHEDIR:=$SCRATCH_DIR/visivo/cache/containers}"
        : "${APPTAINER_TMPDIR:=$SCRATCH_DIR/visivo/cache/tmp}"
        : "${SINGULARITY_TMPDIR:=$SCRATCH_DIR/visivo/cache/tmp}"
        mkdir -p "$APPTAINER_CACHEDIR" "$SINGULARITY_CACHEDIR" "$APPTAINER_TMPDIR" "$SINGULARITY_TMPDIR"
        export APPTAINER_CACHEDIR SINGULARITY_CACHEDIR APPTAINER_TMPDIR SINGULARITY_TMPDIR
        echo "[visivo] Scratch area: $SCRATCH_DIR (workspace: $WORKSPACE, data root: $DATA_ROOT)"
    else
        echo "[visivo] WARNING: no scratch area found (\$CINECA_SCRATCH, \$SCRATCH, \$WORK); pass --workspace and --data-root." >&2
    fi
fi

# Pulled images in a directory the whole project shares (CINECA keeps a work
# area per project, /leonardo_work/<account>; $WORK is the user's default
# project), so that the project pulls an image once. --image-dir wins. A
# directory made here is group-writable and setgid, so that the project's other
# members can add their images to it; one that exists but is not writable for
# this user is passed over, and the next place (finally the workspace) is used.
if [[ "$SITE_SHARED_IMAGES" == "true" && -z "$IMAGE_DIR" ]]; then
    for cand in "${ACCOUNT:+/leonardo_work/$ACCOUNT}" "${WORK:-}"; do
        [[ -n "$cand" && -d "$cand" ]] || continue
        cand_dir="$cand/visivo/images"
        if [[ -d "$cand_dir" ]]; then
            if [[ -w "$cand_dir" ]]; then
                IMAGE_DIR="$cand_dir"
                break
            fi
            echo "[visivo] The shared image directory $cand_dir is not writable for you; trying the next place." >&2
        elif [[ -w "$cand" ]]; then
            made_parent=false
            [[ -d "$cand/visivo" ]] || made_parent=true
            if mkdir -p "$cand_dir" 2>/dev/null; then
                [[ "$made_parent" == "true" ]] && { chmod 2775 "$cand/visivo" 2>/dev/null || true; }
                chmod 2775 "$cand_dir" 2>/dev/null || true
                IMAGE_DIR="$cand_dir"
                break
            fi
            echo "[visivo] Could not create $cand_dir; trying the next place." >&2
        fi
    done
fi

# The container runtime: Apptainer where it exists, else Singularity (CINECA
# Leonardo has only `singularity`). Without either on this host, keep the old
# name: the compute nodes may provide it through a module.
if [[ -z "$RUNTIME" ]]; then
    if command -v apptainer >/dev/null 2>&1; then
        RUNTIME=apptainer
    elif command -v singularity >/dev/null 2>&1; then
        RUNTIME=singularity
    else
        RUNTIME=apptainer
        echo "[visivo] WARNING: neither apptainer nor singularity is on this host's PATH; the job will call apptainer." >&2
    fi
fi

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

# ── Values written into the job script ───────────────────────────────────────
# The job script is read by bash again on the node: every value goes into it
# quoted (printf %q), the numbers must be numbers, and nothing may hold a
# line break (an #SBATCH line would end there and the rest run as commands).
for name in PARTITION TIME MEM CPUS WORKERS GRES PORT BIND_PATH DATA_ROOT SIF IMAGE BACKEND_SRC \
            TOKEN NODE_CODE JOB_NAME ACCOUNT QOS WORKSPACE VERSION LOG_DIR RUNTIME IMAGE_DIR; do
    if [[ "${!name}" == *$'\n'* || "${!name}" == *$'\r'* ]]; then
        echo "[visivo] ERROR: ${name} holds a line break" >&2
        exit 1
    fi
done
for name in CPUS WORKERS PORT ASSIGN_TIMEOUT READY_TIMEOUT; do
    if ! [[ "${!name}" =~ ^[0-9]+$ ]]; then
        echo "[visivo] ERROR: ${name} must be a whole number, not '${!name}'" >&2
        exit 1
    fi
done
# sbatch splits an #SBATCH line on spaces and reads its quotes: a space in
# these would add options, a quote break the line.
for name in PARTITION TIME MEM GRES; do
    if [[ "${!name}" =~ [[:space:]\"] ]]; then
        echo "[visivo] ERROR: ${name} cannot hold spaces or quotes: '${!name}'" >&2
        exit 1
    fi
done
if [[ "$LOG_DIR" == *\"* ]]; then
    echo "[visivo] ERROR: the log directory cannot hold a double quote: $LOG_DIR" >&2
    exit 1
fi
q() { printf '%q' "$1"; }

if ! [[ "$CLEANENV" =~ ^[01]$ ]]; then
    echo "[visivo] ERROR: VISIVO_CLEANENV must be 1 or 0, not '$CLEANENV'" >&2
    exit 1
fi
if [[ "$RUNTIME" =~ [[:space:]] ]]; then
    echo "[visivo] ERROR: the container runtime must be a command name: '$RUNTIME'" >&2
    exit 1
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

# ── The image ────────────────────────────────────────────────────────────────
# --image is pulled once, here on the frontend (compute nodes often have no
# internet), to --sif when given, else to a .sif named after the reference
# under --image-dir (default <workspace>/images, or ~/.visivo_images). An
# existing file is reused; delete it to pull again, e.g. for a moving tag.
if [[ -n "$IMAGE" ]]; then
    if [[ -z "$SIF" ]]; then
        if [[ -z "$IMAGE_DIR" ]]; then
            IMAGE_DIR="${WORKSPACE:+$WORKSPACE/images}"
            IMAGE_DIR="${IMAGE_DIR:-$HOME/.visivo_images}"
        fi
        SIF="$IMAGE_DIR/$(printf '%s' "${IMAGE#*://}" | tr -c 'A-Za-z0-9._-' '_').sif"
    fi
    if [[ -f "$SIF" ]]; then
        echo "[visivo] Image already pulled: $SIF (delete it to pull $IMAGE again)"
    else
        if ! command -v "$RUNTIME" >/dev/null 2>&1; then
            echo "[visivo] ERROR: --image needs $RUNTIME on this host to pull $IMAGE." >&2
            exit 1
        fi
        mkdir -p "$(dirname "$SIF")"
        # Pulled under a temporary name, so that an interrupted pull leaves no
        # file that the next run would take for the image.
        PULL_TMP="$SIF.pulling.$$"
        PULL_PID=""
        trap '[[ -n "$PULL_PID" ]] && kill "$PULL_PID" 2>/dev/null; rm -f "$PULL_TMP"; exit 130' INT HUP TERM PIPE
        echo "[visivo] Pulling $IMAGE to $SIF (a few GB; this can take minutes)..."
        # The runtime prints its progress bars only on a terminal, which an ssh
        # command has not: a line every few seconds says it is alive and how much
        # has been fetched (the layers land in the runtime's cache first).
        PULL_CACHE="${APPTAINER_CACHEDIR:-${SINGULARITY_CACHEDIR:-$HOME/.apptainer/cache}}"
        pull_kb() { (du -sk "$@" 2>/dev/null || true) | awk '{s += $1} END {print s + 0}'; }
        PULL_BASE_KB="$(pull_kb "$PULL_CACHE")"
        "$RUNTIME" pull "$PULL_TMP" "$IMAGE" &
        PULL_PID=$!
        SECONDS=0
        while kill -0 "$PULL_PID" 2>/dev/null; do
            sleep "${VISIVO_PULL_REPORT_EVERY:-10}" &
            wait $! || true
            kill -0 "$PULL_PID" 2>/dev/null || break
            PULL_KB=$(( $(pull_kb "$PULL_CACHE" "$PULL_TMP") - PULL_BASE_KB ))
            (( PULL_KB < 0 )) && PULL_KB=0
            echo "[visivo] Pull: $SECONDS s, $(( PULL_KB / 1024 )) MB fetched"
        done
        if ! wait "$PULL_PID" || ! mv -f "$PULL_TMP" "$SIF"; then
            rm -f "$PULL_TMP"
            echo "[visivo] ERROR: could not pull $IMAGE." >&2
            exit 1
        fi
        PULL_PID=""
        chmod g+r "$SIF" 2>/dev/null || true   # a project's other members use a shared image
        echo "[visivo] Pull: done in $SECONDS s"
        trap - INT HUP TERM PIPE
    fi
fi
if [[ -z "$SIF" && "$STDIN_MODE" == "true" ]]; then
    # Run from standard input: there is no repository to take an image from.
    echo "[visivo] ERROR: no container image: pass --image docker://... (pulled once) or --sif <file>." >&2
    exit 1
fi
SIF="${SIF:-$(dirname "$0")/../apptainer/backend.sif}"
if [[ ! -f "$SIF" ]]; then
    echo "[visivo] ERROR: container image not found: $SIF"
    echo "[visivo] Pull one (--image docker://...), pass --sif, or build it, e.g.:"
    echo "         $RUNTIME build $SIF apptainer/backend-conda.def"
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
    DATA_BIND_FLAG="--bind $(q "${DATA_ROOT}:${DATA_ROOT}")"
    DATA_ROOT_ENV="export APPTAINERENV_VISIVO_DATA_ROOT=$(q "${DATA_ROOT}")"
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
SRC_INFO="the image's own /app/app"
if [[ -n "$BACKEND_SRC" ]]; then
    if [[ ! -d "$BACKEND_SRC" ]]; then
        echo "[visivo] ERROR: --backend-src directory not found: $BACKEND_SRC"
        exit 1
    fi
    BACKEND_SRC="$(cd "$BACKEND_SRC" && pwd -P)"
    if [[ ! -f "$BACKEND_SRC/__init__.py" ]]; then
        echo "[visivo] ERROR: --backend-src is not the app package (no __init__.py): $BACKEND_SRC" >&2
        echo "[visivo]        Pass the backend/app directory, not backend/." >&2
        exit 1
    fi
    # Which copy runs is the first thing to check when a new route answers
    # 404: a stale checkout on the cluster looks exactly like that.
    SRC_INFO="$BACKEND_SRC"
    if SRC_COMMIT="$(git -C "$BACKEND_SRC" rev-parse --short HEAD 2>/dev/null)"; then
        SRC_INFO+=", commit $SRC_COMMIT"
        [[ -n "$(git -C "$BACKEND_SRC" status --porcelain -- . 2>/dev/null)" ]] && SRC_INFO+=" + uncommitted changes"
    else
        SRC_INFO+=", not a git checkout"
    fi
    # Newest modification time over the whole tree (GNU find; on a find without
    # -printf the line is left out).
    SRC_NEWEST="$(find "$BACKEND_SRC" -name '*.py' -type f -printf '%T@\n' 2>/dev/null | sort -n | tail -1 || true)"
    SRC_NEWEST="${SRC_NEWEST%%.*}"
    if [[ "$SRC_NEWEST" =~ ^[0-9]+$ ]]; then
        SRC_INFO+=", newest file $(date -d "@$SRC_NEWEST" '+%Y-%m-%d %H:%M' 2>/dev/null || date -r "$SRC_NEWEST" '+%Y-%m-%d %H:%M' 2>/dev/null || echo '?')"
    fi
    SRC_BIND_FLAG="--bind $(q "${BACKEND_SRC}:/app/app")"
    echo "[visivo] Overlaying /app/app with host source: $SRC_INFO"
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
            NV_RUNTIME_BINDS="${NV_RUNTIME_BINDS} --bind $(q "${f}:${f}")"
        fi
    done
done
GLVND_DIR="/usr/share/glvnd/egl_vendor.d"
GLVND_JSON="${GLVND_DIR}/10_nvidia.json"
if [[ "$EGL_VENDOR_DIR" != "true" && -f "$GLVND_JSON" ]]; then
    NV_RUNTIME_BINDS="${NV_RUNTIME_BINDS} --bind ${GLVND_JSON}:${GLVND_JSON}"
fi
# Where --nv binds libEGL, libEGL_nvidia and libGLdispatch but not the vendor
# file that points libglvnd at libEGL_nvidia (Leonardo), the whole directory is
# bound. Checked on the compute node, where the job script runs.
EGL_VENDOR_BIND=""
if [[ "$EGL_VENDOR_DIR" == "true" && "$USE_GPU" == "true" ]]; then
    EGL_VENDOR_BIND='EGL_BIND=(); [[ -d '"$GLVND_DIR"' ]] && EGL_BIND=(--bind '"$GLVND_DIR:$GLVND_DIR"')'
fi
LD_LIBRARY_PATH_ENV=""
if [[ -n "$EGL_LD_LIBRARY_PATH" ]]; then
    LD_LIBRARY_PATH_ENV="export APPTAINERENV_LD_LIBRARY_PATH=$(q "$EGL_LD_LIBRARY_PATH")"
fi

# --cleanenv keeps the host's environment (REQUESTS_CA_BUNDLE and pip's CA
# variables break Python inside a Debian-based image) out of the container.
# What the backend reads is passed on by name: the SLURM and CUDA variables of
# the job (read inside the job), and the operator's VISIVO_* settings and
# VTK_EGL_DEVICE_INDEX as they are at submit time (written into the job
# script). The launcher's own VISIVO_* options (account, partition, image...)
# and the token (handled above) are not forwarded; a variable the job script
# has already set explicitly is not overridden.
CLEANENV_FLAG=""
CLEANENV_PASS=""
if [[ "$CLEANENV" == "1" ]]; then
    CLEANENV_FLAG="--cleanenv"
    # shellcheck disable=SC2016  # written verbatim into the job script
    CLEANENV_PASS='for v in SLURM_JOB_ID SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_CPUS_ON_NODE SLURM_CPUS_PER_TASK CUDA_VISIBLE_DEVICES NVIDIA_VISIBLE_DEVICES; do
    [[ -n "${!v+x}" ]] && export "APPTAINERENV_${v}=${!v}"
done'
    LAUNCHER_ONLY=" VISIVO_TOKEN VISIVO_ACCOUNT VISIVO_ASSIGN_TIMEOUT VISIVO_BACKEND_SRC_OVERRIDE VISIVO_BIND VISIVO_CLEANENV VISIVO_CONTAINER_RUNTIME VISIVO_CPUS VISIVO_GRES VISIVO_IMAGE VISIVO_IMAGE_DIR VISIVO_JOB_NAME VISIVO_LAUNCHER_STDIN VISIVO_LOGIN_SHELL VISIVO_PULL_REPORT_EVERY VISIVO_READY_TIMEOUT VISIVO_GONE_TIMEOUT VISIVO_LOG_DIR VISIVO_MEM VISIVO_PARTITION VISIVO_PORT VISIVO_QOS VISIVO_SIF VISIVO_SITE VISIVO_TIME VISIVO_WORKSPACE "
    for v in "${!VISIVO_@}" VTK_EGL_DEVICE_INDEX; do
        [[ -n "${!v+x}" && "$LAUNCHER_ONLY" != *" $v "* ]] || continue
        CLEANENV_PASS+=$'\n'"[[ -n \"\${APPTAINERENV_${v}+x}\" ]] || export APPTAINERENV_${v}=$(q "${!v}")"
    done
fi

# ── Write the SLURM job script ────────────────────────────────────────────────
JOB_SCRIPT="$(mktemp /tmp/visivo_slurm_XXXXXX.sh)"
cat > "$JOB_SCRIPT" <<SLURM_EOF
${SBATCH_HEADER}

echo "[visivo] Job started on \$(hostname) at \$(date)"
echo "[visivo] Port: ${PORT}"
echo "[visivo] GPU: " $(q "${GRES:-disabled}")
echo "[visivo] Data root:" $(q "${DATA_ROOT:-(not jailed)}")
echo "[visivo] Runtime: " $(q "${RUNTIME}") ", image:" $(q "${SIF}")
echo "[visivo] Backend source:" $(q "${SRC_INFO}")

# Env vars consumed inside the container.
export APPTAINERENV_VISIVO_TOKEN=$(q "${TOKEN}")
export APPTAINERENV_VISIVO_WORKERS=${WORKERS}
export APPTAINERENV_PORT=${PORT}
export APPTAINERENV_VISIVO_HOST="0.0.0.0"
# EGL / SSR env (see comment on NV_RUNTIME_BINDS above).
${LD_LIBRARY_PATH_ENV}
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
${CLEANENV_PASS}
${EGL_VENDOR_BIND}

# A Singularity that does not know the APPTAINERENV_ prefix (SingularityCE,
# SingularityPRO) reads SINGULARITYENV_: hand every variable on, so that this
# script, and any APPTAINERENV_ the operator exported, work with either.
if [[ \$(basename $(q "${RUNTIME}")) == singularity ]]; then
    for v in "\${!APPTAINERENV_@}"; do
        n="SINGULARITYENV_\${v#APPTAINERENV_}"
        [[ -n "\${!n+x}" ]] || export "\$n=\${!v}"
    done
fi

$(q "${RUNTIME}") run ${NV_FLAG} ${CLEANENV_FLAG} \\
    --bind $(q "${BIND_PATH}:${BIND_PATH}") \\
    "\${EGL_BIND[@]}" \\
    ${DATA_BIND_FLAG} \\
    ${WORKSPACE_BIND_FLAG} \\
    ${NV_RUNTIME_BINDS} \\
    ${SRC_BIND_FLAG} \\
    $(q "${SIF}")
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
echo "[visivo]   runtime:   ${RUNTIME}$([[ "$CLEANENV" == "1" ]] && echo ' (--cleanenv)')"
echo "[visivo]   backend:   ${SRC_INFO}"

# A name given on the command line takes precedence over the script's #SBATCH line.
JOB_ID="$(sbatch --parsable ${JOB_NAME:+--job-name="$JOB_NAME"} ${ACCOUNT:+--account="$ACCOUNT"} \
    ${QOS:+--qos="$QOS"} "$JOB_SCRIPT")"
rm -f "$JOB_SCRIPT"
echo "[visivo] Job ID: $JOB_ID"

# ── Wait for the compute node to be assigned ──────────────────────────────────
# A job on a production QOS can queue for minutes. The job's state and SLURM's
# reason are printed when they change, and now and then while they do not, so
# that a client can say what the job is waiting for. 0 waits for ever.
echo "[visivo] Waiting for compute node assignment..."
NODE=""
LAST_QUEUE_LINE=""
LAST_QUEUE_AT=0
GONE_SINCE=-1
GONE_LIMIT="${VISIVO_GONE_TIMEOUT:-300}"   # seconds with no word on the job at all
SECONDS=0
while :; do
    QUEUE_LINE="$(squeue -j "$JOB_ID" -h -o '%N|%T|%r' 2>/dev/null || true)"
    NODE="${QUEUE_LINE%%|*}"
    if [[ -n "$NODE" && "$NODE" != "None" ]]; then
        break
    fi
    NODE=""
    if [[ -n "$QUEUE_LINE" ]]; then
        GONE_SINCE=-1
        QUEUE_STATE="${QUEUE_LINE#*|}"
        QUEUE_REASON="${QUEUE_STATE#*|}"
        QUEUE_STATE="${QUEUE_STATE%%|*}"
        if [[ "$QUEUE_LINE" != "$LAST_QUEUE_LINE" ]] || (( SECONDS - LAST_QUEUE_AT >= 30 )); then
            echo "[visivo] Queue: $QUEUE_STATE ($QUEUE_REASON), $SECONDS s since submission"
            LAST_QUEUE_LINE="$QUEUE_LINE"
            LAST_QUEUE_AT=$SECONDS
        fi
    else
        # squeue says nothing: the job has ended (a failure at start-up ends it
        # within the second), or squeue failed (an overloaded slurmctld answers
        # nothing for a while, with the job still pending). Ask the accounting
        # and the controller before believing either; waiting for a node would
        # only hide why a job ended.
        STATE="$(sacct -j "$JOB_ID" -X -n -o State 2>/dev/null | awk 'NF {print $1; exit}' || true)"
        CTL_STATE="$(scontrol show job "$JOB_ID" 2>/dev/null | tr ' ' '\n' | sed -n 's/^JobState=//p' | head -n 1 || true)"
        STATE="${STATE:-$CTL_STATE}"
        case "$STATE" in
            "")
                # Neither knows the job (accounting may be off, the controller
                # down): long enough without an answer is the end of it.
                (( GONE_SINCE >= 0 )) || GONE_SINCE=$SECONDS
                if (( SECONDS - GONE_SINCE >= GONE_LIMIT )); then
                    echo "[visivo] ERROR: job $JOB_ID is no longer in the queue and its state is unknown." >&2
                    scancel "$JOB_ID" 2>/dev/null || true
                    exit 1
                fi
                ;;
            PENDING|RUNNING|CONFIGURING|COMPLETING|REQUEUED|RESIZING|SUSPENDED)
                # Still alive: only the queue listing is missing.
                GONE_SINCE=-1
                ;;
            *)
                echo "[visivo] ERROR: job $JOB_ID ended before the backend started (state: $STATE)." >&2
                echo "[visivo]        Last lines of $LOG_DIR/visivo_backend_${JOB_ID}.log:" >&2
                tail -n 20 "$LOG_DIR/visivo_backend_${JOB_ID}.log" 2>/dev/null | sed 's/^/[visivo]   /' >&2
                exit 1
                ;;
        esac
    fi
    if (( ASSIGN_TIMEOUT > 0 && SECONDS >= ASSIGN_TIMEOUT )); then
        break
    fi
    # Often at first, gently once the wait is long: a whole project shares the
    # scheduler.
    if (( SECONDS < 60 )); then sleep 2; else sleep 10; fi
done

if [[ -z "$NODE" ]]; then
    echo "[visivo] ERROR: no compute node assigned to job $JOB_ID within ${ASSIGN_TIMEOUT} s." >&2
    echo "[visivo]        The job is still queued and starts when the resources free up; cancel" >&2
    echo "[visivo]        it with: scancel $JOB_ID (or wait longer: --assign-timeout <seconds>, 0 for ever)." >&2
    exit 1
fi

echo "[visivo] Compute node: $NODE"

# ── Poll the health endpoint via SSH to the compute node ──────────────────────
# Where ssh to a compute node may be refused (PROBE_SRUN), the same check runs
# as a step of the job itself.
probe_health() {
    local cmd
    cmd="curl -sf -H $(q "X-Visivo-Token: ${TOKEN}") http://localhost:${PORT}/v1/health"
    if ssh -n -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=5 "$NODE" "$cmd" >/dev/null 2>&1; then
        return 0
    fi
    [[ "$PROBE_SRUN" == "true" ]] || return 1
    ${TIMEOUT_CMD:+"$TIMEOUT_CMD" 30} srun --jobid="$JOB_ID" --overlap --gres=none --nodes=1 --ntasks=1 \
        --quiet bash -c "$cmd" </dev/null >/dev/null 2>&1
}
TIMEOUT_CMD="$(command -v timeout || true)"
echo "[visivo] Waiting for backend to become ready (polling every 5 s)..."
READY=false
POLL_MAX=$(( (READY_TIMEOUT + 4) / 5 ))
(( POLL_MAX >= 1 )) || POLL_MAX=1
for attempt in $(seq 1 "$POLL_MAX"); do
    if probe_health; then
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
  Backend code:  ${SRC_INFO}
  Log file:      ${LOG_DIR}/visivo_backend_${JOB_ID}.log
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
