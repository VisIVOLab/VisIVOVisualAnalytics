#!/usr/bin/env bash
# run_remote_bench.sh — the responsiveness benchmark against a service that is
# NOT the client's embedded one: a Linux container with a hard memory cap,
# reached over a link shaped with `tc netem`. It answers two questions the
# single-host campaign cannot: what a remote client pays in latency and bytes,
# and whether the service still works when the cube is larger than its memory.
#
# Usage (from the repository root, Docker running, image built as in the
# Dockerfile next to this script):
#
#   benchmark/remote/run_remote_bench.sh ~/visivo_bench_data out_dir \
#       cube_1gb.fits cube_8gb.fits cube_30gb.fits
#
# Environment knobs (defaults in brackets):
#   MEM [8g]        container memory cap; swap is disabled (memory-swap = MEM)
#   CPUS [8]        container CPU quota
#   WORKERS [4]     backend worker processes (the embedded default)
#   DELAY [20ms]    delay added to the service's egress. Only egress is shaped,
#                   so every round trip gains DELAY once: 20 ms is the order of
#                   a national research-network path between two sites
#   RATE [1gbit]    egress bandwidth of the service
#   REPEATS [3]     repetitions per operation
#   SKIP []         operations to skip, passed to benchmark_visivo.py --skip
#
# A fresh container per cube, so each cube starts with a cold service, cold
# product cache and an empty cgroup memory peak.
set -euo pipefail

DATA=${1:?data directory}
OUT=${2:?output directory}
shift 2
CUBES=("$@")
[ ${#CUBES[@]} -gt 0 ] || { echo "no cubes given" >&2; exit 2; }

MEM=${MEM:-8g}
CPUS=${CPUS:-8}
WORKERS=${WORKERS:-4}
DELAY=${DELAY:-20ms}
RATE=${RATE:-1gbit}
REPEATS=${REPEATS:-3}
SKIP=${SKIP:-}
NAME=visivo-bench
PORT=8012
TOKEN=benchtok
IMAGE=visivo-bench-backend
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PY=${PY:-$ROOT/backend/.venv/bin/python}

mkdir -p "$OUT"
status=0
cleanup() { docker rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT

for cube in "${CUBES[@]}"; do
    tag=${cube%.fits}
    echo "=== $cube  (mem $MEM, cpus $CPUS, workers $WORKERS, egress delay $DELAY, rate $RATE)"
    cleanup
    docker run -d --name "$NAME" \
        --memory "$MEM" --memory-swap "$MEM" --cpus "$CPUS" \
        --cap-add NET_ADMIN \
        -e VISIVO_TOKEN=$TOKEN -e VISIVO_DATA_ROOTS=/data -e VISIVO_WORKERS="$WORKERS" \
        -v "$DATA":/data:ro -p $PORT:8000 "$IMAGE" >/dev/null
    for _ in $(seq 1 60); do
        curl -s -o /dev/null -H "X-Visivo-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/health" && break
        sleep 1
    done
    docker exec "$NAME" tc qdisc add dev eth0 root netem delay "$DELAY" rate "$RATE"

    # Round-trip time of an empty authenticated request, as the client sees it.
    rtt=$(for _ in $(seq 1 20); do
              curl -s -o /dev/null -w "%{time_total}\n" -H "X-Visivo-Token: $TOKEN" \
                   "http://127.0.0.1:$PORT/v1/health"
          done | sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}')

    # The harness's status is kept, not acted on, until the container's own
    # record (memory peak, OOM kills, link statistics) has been collected:
    # a failed run is exactly the one whose diagnostics matter.
    mkdir -p "$OUT/$tag"
    set +e
    "$PY" "$ROOT/benchmark/benchmark_visivo.py" \
        --base-url "http://127.0.0.1:$PORT" --token $TOKEN \
        --cube "/data/$cube" --repeats "$REPEATS" --roi-only-above 2 \
        ${SKIP:+--skip "$SKIP"} \
        --mem-cmd "docker exec $NAME cat /sys/fs/cgroup/memory.current" \
        --note "container: mem $MEM (no swap), cpus $CPUS, workers $WORKERS; egress netem delay $DELAY rate $RATE; health RTT median ${rtt}s" \
        --out "$OUT/$tag"
    rc=$?
    [ $rc -eq 0 ] || { echo "benchmark failed for $cube (exit $rc)" >&2; status=$rc; }

    # What the cgroup saw over the whole run, and what crossed the shaped link.
    {
        echo "memory_peak_bytes $(docker exec "$NAME" cat /sys/fs/cgroup/memory.peak)"
        echo "memory_max_bytes $(docker exec "$NAME" cat /sys/fs/cgroup/memory.max)"
        echo "oom_events $(docker exec "$NAME" sh -c 'grep -E "^oom_kill " /sys/fs/cgroup/memory.events | cut -d" " -f2')"
        echo "health_rtt_median_s $rtt"
        echo "--- tc"
        docker exec "$NAME" tc -s qdisc show dev eth0
        echo "--- memory.stat"
        docker exec "$NAME" sh -c 'grep -E "^(anon|file|file_mapped|shmem) " /sys/fs/cgroup/memory.stat'
        echo "--- docker inspect (state)"
        docker inspect -f '{{json .State}}' "$NAME"
    } > "$OUT/$tag/container.txt" 2>&1
    # Best effort: a container that has stopped still leaves its logs.
    docker logs "$NAME" > "$OUT/$tag/container.log" 2>&1
    head -4 "$OUT/$tag/container.txt"
    set -e
done
exit $status
