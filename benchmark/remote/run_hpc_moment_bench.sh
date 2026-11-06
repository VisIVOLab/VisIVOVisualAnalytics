#!/usr/bin/env bash
# run_hpc_moment_bench.sh — out-of-core moment maps (M0, M1, M2) on an HPC
# node, under a memory cap, one measurement per process
# (benchmark/moment_ooc_bench.py). Runs inside a SLURM allocation or on any
# Linux host; no Docker, no root.
#
#   benchmark/remote/run_hpc_moment_bench.sh OUT_DIR LABEL=/path/cube.fits [LABEL=...]
#
# e.g. from an sbatch script on the node:
#   SIF=/path/backend.sif BINDS=/mnt/beegfs CAP_GB=16 MASK=8.6e-5 \
#   CHECK_CHANNELS=3000:3127 \
#   benchmark/remote/run_hpc_moment_bench.sh out sdc2_beegfs=/mnt/beegfs/.../sky_ldev_v2.fits
#
# Environment knobs (defaults in brackets):
#   SIF []            Apptainer image to run Python in; empty: run $PY directly
#   PY [python3]      interpreter (inside the image when SIF is set)
#   BINDS []          comma list of host paths bound into the image (the cube's file system)
#   BACKEND [repo]    directory holding the backend's app/ (bound and used as is, so the
#                     code measured is this checkout's, whatever the image ships)
#   CAP_GB [16]       RLIMIT_DATA cap of every measuring process, GB (see moment_ooc_bench.py)
#   ORDERS [0 1 2]    moments to measure
#   WAYS [dask stream]  dask = what the service runs by default for large ranges,
#                     stream = worker_moment's slab streamer (VISIVO_DASK_MODE=off)
#   MASK [auto]       auto, none, or an explicit threshold (preferred: auto depends on the range)
#   CHANNELS []       channel range A:B (default: all)
#   THREADS [0]       VISIVO_MOMENT_THREADS for the dask way (0: backend default)
#   REPEATS [3]       timed repeats after the first run (repeat 0, reported apart)
#   EVICT [1]         drop the cube from the page cache before every run
#   CAP_PROOF [1]     also run the in-memory way once per cube, expected to fail under the cap
#   CHECK_CHANNELS [] run the equality check (in-memory vs streaming vs dask vs a float64
#                     reference) on this channel range of the first cube, before timing
#   CRITERION [moment_criterion_ulp.py]  the check's acceptance criterion
#
# Outputs in OUT_DIR: results.jsonl (one line per run), results.md (table),
# check.json, env.txt (node, file systems, image, cap, knobs).
set -uo pipefail

OUT=${1:?output directory}
shift
[ $# -gt 0 ] || { echo "no LABEL=cube given" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SIF=${SIF:-}
PY=${PY:-python3}
BINDS=${BINDS:-}
BACKEND=${BACKEND:-$ROOT/backend}
CAP_GB=${CAP_GB:-16}
ORDERS=${ORDERS:-0 1 2}
WAYS=${WAYS:-dask stream}
MASK=${MASK:-auto}
CHANNELS=${CHANNELS:-}
THREADS=${THREADS:-0}
REPEATS=${REPEATS:-3}
EVICT=${EVICT:-1}
CAP_PROOF=${CAP_PROOF:-1}
CHECK_CHANNELS=${CHECK_CHANNELS:-}
CRITERION=${CRITERION:-$ROOT/benchmark/moment_criterion_ulp.py}

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
BENCH="$ROOT/benchmark/moment_ooc_bench.py"

# BLAS thread pools reserve their buffers up front, per thread: with one per
# hardware thread (128 on a POWER9 node) OpenBLAS's reservation alone exceeds
# a 16 GB RLIMIT_DATA and it aborts at import. The moments use no BLAS, so the
# pools are kept small; the moment threads are set by THREADS.
BLAS_THREADS=${BLAS_THREADS:-4}

# The commit measured: given by run_hpc_moment_campaign.sh (which exports it
# with git archive), else this checkout's HEAD, marked when the tree differs.
if [ -z "${VISIVO_BENCH_COMMIT:-}" ]; then
    VISIVO_BENCH_COMMIT=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)
    if [ -n "$(git -C "$ROOT" status --porcelain -- backend/app benchmark 2>/dev/null)" ]; then
        VISIVO_BENCH_COMMIT="$VISIVO_BENCH_COMMIT+uncommitted"
    fi
fi

run_py() {
    # The repository, the output and the given binds are visible in the image;
    # the backend's app/ comes from BACKEND, not from the image.
    local envs=(VISIVO_TOKEN=bench OPENBLAS_NUM_THREADS=$BLAS_THREADS
                OMP_NUM_THREADS=$BLAS_THREADS MKL_NUM_THREADS=$BLAS_THREADS
                SLURM_JOB_ID=${SLURM_JOB_ID:-} VISIVO_BENCH_COMMIT=$VISIVO_BENCH_COMMIT
                VISIVO_BENCH_IMAGE=${SIF:-none})
    if [ -n "$SIF" ]; then
        local b="$ROOT,$OUT,$BACKEND"
        [ -n "$BINDS" ] && b="$b,$BINDS"
        local flags=()
        for e in "${envs[@]}"; do flags+=(--env "$e"); done
        apptainer exec --cleanenv "${flags[@]}" -B "$b" "$SIF" "$PY" "$@"
    else
        env "${envs[@]}" "$PY" "$@"
    fi
}

{
    echo "date $(date -Is)"
    echo "host $(hostname)  slurm_job ${SLURM_JOB_ID:-none}  cpus_on_node ${SLURM_CPUS_ON_NODE:-?}"
    echo "kernel $(uname -r)"
    grep MemTotal /proc/meminfo 2>/dev/null
    echo "commit $VISIVO_BENCH_COMMIT"
    echo "sif ${SIF:-none}  backend $BACKEND"
    echo "cap RLIMIT_DATA ${CAP_GB} GB per process  ways '$WAYS'  orders '$ORDERS'  mask $MASK"
    echo "channels ${CHANNELS:-all}  threads $THREADS  blas threads ${BLAS_THREADS}  repeats $REPEATS (+ first run)  evict $EVICT"
    for spec in "$@"; do
        cube=${spec#*=}
        echo "cube ${spec%%=*} $cube $(stat -c %s "$cube" 2>/dev/null) bytes"
        df -hT "$cube" 2>/dev/null | tail -1
    done
} > "$OUT/env.txt"
cat "$OUT/env.txt"

status=0
common=(--mask "$MASK" --threads "$THREADS" --backend "$BACKEND")
[ -n "$CHANNELS" ] && common+=(--channels "$CHANNELS")

if [ -n "$CHECK_CHANNELS" ]; then
    first=${1#*=}
    echo "=== check on $first channels $CHECK_CHANNELS"
    run_py "$BENCH" check --cube "$first" --channels "$CHECK_CHANNELS" \
        --mask "$MASK" --backend "$BACKEND" --criterion "$CRITERION" \
        --out "$OUT/check.json" || { echo "check FAILED (see check.json)" >&2; status=1; }
fi

for spec in "$@"; do
    label=${spec%%=*}
    cube=${spec#*=}
    echo "=== $label  $cube"
    # Never an empty array: "${a[@]}" of one is an unbound variable under
    # set -u in bash < 4.4.
    evict=(--cap-gb "$CAP_GB")
    [ "$EVICT" = 1 ] && evict+=(--evict)
    for rep in $(seq 0 "$REPEATS"); do
        for order in $ORDERS; do
            for way in $WAYS; do
                run_py "$BENCH" measure --cube "$cube" --label "$label" --order "$order" \
                    --path "$way" --repeat "$rep" "${evict[@]}" \
                    "${common[@]}" --out "$OUT/results.jsonl" \
                    | cut -c1-200 || status=1
            done
        done
    done
    if [ "$CAP_PROOF" = 1 ]; then
        # The in-memory way materialises the range: under the cap it must fail.
        run_py "$BENCH" measure --cube "$cube" --label "$label" --order 0 --path inmem \
            --cap-gb "$CAP_GB" "${common[@]}" --allow-fail \
            --out "$OUT/cap_proof.jsonl" | cut -c1-200 || status=1
    fi
done

if [ -s "$OUT/results.jsonl" ]; then
    if run_py "$BENCH" summarise "$OUT/results.jsonl" > "$OUT/results.md"; then
        cat "$OUT/results.md"
    else
        echo "summary FAILED" >&2
        status=1
    fi
else
    echo "no measurement completed" >&2
    status=1
fi
exit $status
