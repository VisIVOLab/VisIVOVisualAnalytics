#!/usr/bin/env bash
# run_local_bench.sh — the single-host campaign of the paper's responsiveness
# table: the embedded service on the client's machine, a fresh service per cube
# (cold product cache), REPEATS repetitions per operation.
#
#   benchmark/run_local_bench.sh ~/visivo_bench_data out_dir \
#       cube_1gb.fits cube_8gb.fits cube_30gb.fits
#
# Memory is not reported here: summing the resident sets of the worker
# processes counts the memory-mapped cube once per process. The memory of the
# service is measured in the container campaign (remote/run_remote_bench.sh),
# where the cgroup counts every page once.
set -euo pipefail

DATA=${1:?data directory}
OUT=${2:?output directory}
shift 2
REPEATS=${REPEATS:-3}
WORKERS=${WORKERS:-4}
PORT=8011
TOKEN=benchtok
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PY=$ROOT/backend/.venv/bin/python

mkdir -p "$OUT"
pid=""
# Stop the previous service and wait until it has released the port, so the
# readiness probe below cannot be answered by a service that is shutting down.
cleanup() {
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        pid=""
    fi
    for _ in $(seq 1 30); do
        lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1 || return 0
        sleep 1
    done
}
trap cleanup EXIT

for cube in "$@"; do
    tag=${cube%.fits}
    cleanup
    (cd "$ROOT/backend" && VISIVO_TOKEN=$TOKEN VISIVO_WORKERS=$WORKERS \
        exec "$PY" -m uvicorn app.main:app --host 127.0.0.1 --port $PORT --no-access-log) \
        > "$OUT/backend_$tag.log" 2>&1 &
    pid=$!
    for _ in $(seq 1 60); do
        kill -0 "$pid" 2>/dev/null || { echo "service for $cube exited; see $OUT/backend_$tag.log" >&2; exit 1; }
        curl -s -o /dev/null -H "X-Visivo-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/health" && break
        sleep 1
    done
    "$PY" "$ROOT/benchmark/benchmark_visivo.py" \
        --base-url "http://127.0.0.1:$PORT" --token $TOKEN \
        --cube "$DATA/$cube" --repeats "$REPEATS" --roi-only-above 2 \
        --note "embedded service on the client host; workers $WORKERS" \
        --out "$OUT/$tag"
done
