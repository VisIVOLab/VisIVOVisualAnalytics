#!/bin/sh
# Entrypoint of the VisIVO backend images (docker/Dockerfile).
#
# One script for every runtime the image meets: Docker/Podman (a non-root user,
# optionally with --read-only), and Apptainer/Singularity (the caller's own uid,
# the image mounted read-only, the host's HOME and /tmp visible). All it does is
# point the backend's writable state at a directory that is writable in all of
# them; the backend reads the settings below itself.
#
#   VISIVO_TOKEN        access token (unset: a random one, which the backend prints)
#   PORT                TCP port (default 8000); VISIVO_HOST the address (0.0.0.0)
#   VISIVO_DATA_ROOT    the directory the data is bound at. When it, or /data,
#                       exists, the backend is confined to it (VISIVO_DATA_ROOTS
#                       is set from it, plus the exports directory), unless
#                       VISIVO_DATA_ROOTS is already set
#   VISIVO_EXPORTS_DIR  where products are written (default: scratch). Bind a
#                       directory here to keep them when the container ends.
#   VISIVO_SCRATCH      the writable scratch directory (default
#                       ${TMPDIR:-/tmp}/visivo-<uid>)
#
# Anything else the backend reads (VISIVO_WORKERS, ...) passes through.
set -eu

# Without a token the backend makes one, prints it, and writes it to
# ~/.visivo_token. Under Apptainer that HOME is the user's real one, where the
# file belongs to the desktop's own backend: make the token here instead.
if [ -z "${VISIVO_TOKEN:-}" ]; then
    VISIVO_TOKEN="$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    export VISIVO_TOKEN
    echo "[visivo] VISIVO_TOKEN is not set: using a random token, printed below"
fi

uid="$(id -u)"
scratch="${VISIVO_SCRATCH:-${TMPDIR:-/tmp}/visivo-${uid}}"
mkdir -p "$scratch"

# A uid with no passwd entry and no HOME (docker run -u 1234) has no home
# directory, and the backend resolves one when it is imported.
if [ -z "${HOME:-}" ] || [ "$HOME" = "/" ]; then
    export HOME="$scratch/home"
    mkdir -p "$HOME"
fi

# Caches and configuration that libraries would put under HOME: on an HPC
# system that is a shared filesystem where a lock can hang the first render,
# and in a read-only container it is not writable at all. Set only when the
# caller has not chosen.
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$scratch/cache}"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$scratch/config}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-$scratch/data}"
export MPLCONFIGDIR="${MPLCONFIGDIR:-$scratch/mpl}"
export NUMBA_CACHE_DIR="${NUMBA_CACHE_DIR:-$scratch/numba}"
export VISIVO_EXPORTS_DIR="${VISIVO_EXPORTS_DIR:-$scratch/exports}"
export VISIVO_HIPS_CACHE="${VISIVO_HIPS_CACHE:-$scratch/hips}"
mkdir -p "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$MPLCONFIGDIR" \
         "$NUMBA_CACHE_DIR" "$VISIVO_EXPORTS_DIR" "$VISIVO_HIPS_CACHE"

# The data directory is the one the operator bound in. Confining the backend to
# it matters under Apptainer, where the user's whole HOME is visible too. The
# exports directory is a root as well, so a product opens where it was made.
if [ -z "${VISIVO_DATA_ROOTS:-}" ]; then
    data="${VISIVO_DATA_ROOT:-}"
    if [ -z "$data" ] && [ -d /data ]; then
        data=/data
    fi
    if [ -n "$data" ] && [ -d "$data" ]; then
        export VISIVO_DATA_ROOTS="$data:$VISIVO_EXPORTS_DIR"
    fi
fi

# A command given to the container runs as it is (docker run IMAGE python -V).
if [ "$#" -gt 0 ]; then
    cd /app/backend
    exec "$@"
fi

PORT="${PORT:-8000}"
HOST="${VISIVO_HOST:-0.0.0.0}"
echo "[visivo] backend on ${HOST}:${PORT}, scratch ${scratch}, data roots ${VISIVO_DATA_ROOTS:-unrestricted}"

# --app-dir because Apptainer starts in the caller's working directory, not in
# the image's.
exec uvicorn app.main:app --host "$HOST" --port "$PORT" --workers 1 \
    --log-level info --app-dir /app/backend
