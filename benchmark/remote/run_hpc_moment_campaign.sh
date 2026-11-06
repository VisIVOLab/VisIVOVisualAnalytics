#!/usr/bin/env bash
# run_hpc_moment_campaign.sh — the whole out-of-core moment campaign of issue #3
# from one command, against a given commit and image, on a SLURM cluster.
# Run it on the cluster's login node, from a git checkout of the repository:
#
#   SIF=/shared/backend.sif COMMIT=v1.0.10 \
#   benchmark/remote/run_hpc_moment_campaign.sh /shared/campaign_dir cubes.txt
#
# It exports COMMIT's backend/app and benchmark/ (git archive) into
# CAMPAIGN_DIR/src, so the code measured is exactly that commit whatever the
# working tree or the image holds, writes CAMPAIGN_DIR/PROVENANCE.txt, and
# submits one SLURM job per line of the cube list, chained one after the other
# so that no two measurements share the node's or the file system's bandwidth.
# Each job runs run_hpc_moment_bench.sh and writes its raw results beside
# itself in CAMPAIGN_DIR/<label>_<ways>/.
#
# Cube list: one line per job, '#' starts a comment, whitespace-separated:
#
#   label  fs      path                              mask     repeats  ways         [option ...]
#   sdc2   shared  /mnt/beegfs/.../sky_ldev_v2.fits  8.6e-5   3        dask,stream  check=3630:3757
#   sdc2   local   /mnt/beegfs/.../sky_ldev_v2.fits  8.6e-5   3        dask,stream
#   syn450 shared  /mnt/beegfs/.../cube_450gb.fits   3.0      2        dask         make=450
#
#   fs      shared: the cube is read where it is (a parallel file system);
#           local: the job copies it to node-local LOCAL_DIR first, and deletes
#           the copy when it ends, however it ends
#   mask    explicit threshold (preferred), none, or auto
#   ways    comma list of dask, stream
#   options check=A:B  equality check on that channel range first
#           make=GB    create the cube with make_synthetic_cube.py (frequency
#                      axis) when the path does not exist yet
#           channels=A:B  measure this channel range only
#
# Environment knobs (defaults in brackets):
#   SIF (required)     Apptainer image providing the backend's Python stack
#   COMMIT [HEAD]      commit, tag or branch to measure
#   CAP_GB [16]        RLIMIT_DATA cap per measuring process
#   PARTITION [gpu]    SBATCH_ARGS ["--gres=gpu:1 -c 32 --exclusive"]   TIME [04:00:00]
#   BINDS []           host paths bound into the image (the shared file system)
#   LOCAL_DIR [/tmp]   node-local directory for fs=local copies
#   THREADS [0]        VISIVO_MOMENT_THREADS for the dask way (0: backend default)
#   AFTER []           a job id the first job waits for
#   DRY_RUN [0]        write the job scripts and provenance, submit nothing
#
# Stop a campaign: scancel the job ids printed at the end (listed in
# CAMPAIGN_DIR/jobs.txt); jobs waiting on a cancelled one are cancelled with it.
set -euo pipefail

CAMPAIGN=${1:?campaign directory (on a file system the compute nodes see)}
CUBES=${2:?cube list}
SIF=${SIF:?SIF: the Apptainer image}
COMMIT=${COMMIT:-HEAD}
CAP_GB=${CAP_GB:-16}
PARTITION=${PARTITION:-gpu}
SBATCH_ARGS=${SBATCH_ARGS:---gres=gpu:1 -c 32 --exclusive}
TIME=${TIME:-04:00:00}
BINDS=${BINDS:-}
LOCAL_DIR=${LOCAL_DIR:-/tmp}
THREADS=${THREADS:-0}
AFTER=${AFTER:-}
DRY_RUN=${DRY_RUN:-0}

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$HERE" rev-parse --show-toplevel)
SHA=$(git -C "$REPO" rev-parse --verify "$COMMIT^{commit}")
# The jobs start in LOCAL_DIR: every path they use must be absolute.
absolute() { case "$1" in /*) printf '%s' "$1" ;; *) printf '%s' "$PWD/$1" ;; esac; }
SIF=$(absolute "$SIF")
CUBES=$(absolute "$CUBES")
q() { printf '%q' "$1"; }
[ -f "$SIF" ] || { echo "no image at $SIF" >&2; exit 2; }
[ -f "$CUBES" ] || { echo "no cube list at $CUBES" >&2; exit 2; }
if [ -e "$CAMPAIGN/src" ]; then
    echo "$CAMPAIGN/src exists: use a new campaign directory" >&2
    exit 2
fi

mkdir -p "$CAMPAIGN/src" "$CAMPAIGN/jobs"
CAMPAIGN=$(cd "$CAMPAIGN" && pwd)
git -C "$REPO" archive "$SHA" backend/app benchmark | tar -x -C "$CAMPAIGN/src"
cp "$CUBES" "$CAMPAIGN/cubes.txt"
{
    echo "campaign  $CAMPAIGN"
    echo "created   $(date -Is) on $(hostname) by $(id -un)"
    echo "commit    $SHA ($(git -C "$REPO" describe --tags --always "$SHA" 2>/dev/null))"
    echo "code      git archive of that commit in src/ (backend/app, benchmark)"
    echo "image     $SIF"
    echo "image_sha256 $(sha256sum "$SIF" | cut -d' ' -f1)"
    echo "cap       RLIMIT_DATA $CAP_GB GB per measuring process (see benchmark/remote/README.md)"
    echo "slurm     partition $PARTITION, $SBATCH_ARGS, time $TIME, jobs chained (afterany)"
    echo "local_dir $LOCAL_DIR  binds ${BINDS:-none}  moment threads $THREADS"
} > "$CAMPAIGN/PROVENANCE.txt"

prev=$AFTER
: > "$CAMPAIGN/jobs.txt"
# "|| [ -n ... ]": a last line without a newline is still a job.
while read -r label fs path mask repeats ways opts || [ -n "${label:-}" ]; do
    case "$label" in ''|'#'*) continue ;; esac
    path=$(absolute "$path")
    check="" make="" channels=""
    for opt in $opts; do
        case "$opt" in
            check=*) check=${opt#check=} ;;
            make=*) make=${opt#make=} ;;
            channels=*) channels=${opt#channels=} ;;
            *) echo "unknown option '$opt' for $label" >&2; exit 2 ;;
        esac
    done
    case "$fs" in shared|local) ;; *) echo "fs must be shared or local ($label)" >&2; exit 2 ;; esac
    name="${label}_${fs}_${ways//,/-}"
    job="$CAMPAIGN/jobs/$name.sbatch"
    log="$CAMPAIGN/jobs/${name}_%j.log"
    {
        echo "#!/bin/bash"
        echo "#SBATCH -p $PARTITION $SBATCH_ARGS -t $TIME -J m3_$label"
        echo "# submitted with --output=$(q "$log")"
        echo "set -uo pipefail"
        echo "cd $(q "$LOCAL_DIR")"
        echo "grep MemTotal /proc/meminfo"
        echo "SRC=$(q "$CAMPAIGN/src")"
        echo "SIF=$(q "$SIF")"
        echo "CUBE=$(q "$path")"
        # Whatever the job leaves behind (a partial cube, a local copy) goes
        # when it ends, however it ends: time limit and scancel included.
        echo "CLEAN=()"
        echo "trap 'rm -rf \"\${CLEAN[@]:-}\"' EXIT"   # :- for set -u on bash < 4.4
        echo "trap 'exit 143' TERM INT"
        if [ -n "$make" ]; then
            # Written beside the cube under another name and renamed only when
            # complete: a failed generation never leaves a cube to be reused.
            echo "if [ ! -e \"\$CUBE\" ]; then"
            echo "  PART=\"\$CUBE.partial.\$SLURM_JOB_ID\""
            echo "  CLEAN+=(\"\$PART\")"
            echo "  apptainer exec ${BINDS:+-B $(q "$BINDS")} -B \"\$SRC\" \"\$SIF\" python \\"
            echo "      \"\$SRC/benchmark/make_synthetic_cube.py\" --gb $(q "$make") --spectral freq \\"
            echo "      --out \"\$PART\" || exit 1"
            echo "  mv \"\$PART\" \"\$CUBE\" || exit 1"
            echo "fi"
        fi
        if [ "$fs" = local ]; then
            echo "L=$(q "$LOCAL_DIR")/visivo_moment_bench_\$SLURM_JOB_ID"
            echo "CLEAN+=(\"\$L\")"
            echo "mkdir -p \"\$L\" || exit 1"
            echo "df -hT \"\$L\""
            echo "t0=\$(date +%s); cp \"\$CUBE\" \"\$L/\" || exit 1; echo \"copy \$(( \$(date +%s) - t0 )) s\""
            echo "CUBE=\"\$L/\$(basename \"\$CUBE\")\""
        fi
        local_bind=""
        [ "$fs" = local ] && local_bind=",$LOCAL_DIR"
        echo "export SIF BINDS=$(q "$BINDS$local_bind") BACKEND=\"\$SRC/backend\" PY=python"
        echo "export CAP_GB=$(q "$CAP_GB") MASK=$(q "$mask") REPEATS=$(q "$repeats") WAYS=$(q "${ways//,/ }")"
        echo "export THREADS=$(q "$THREADS") CHECK_CHANNELS=$(q "$check") CHANNELS=$(q "$channels")"
        echo "export VISIVO_BENCH_COMMIT=$(q "$SHA")"
        echo "bash \"\$SRC/benchmark/remote/run_hpc_moment_bench.sh\" $(q "$CAMPAIGN/$name") \"$(q "$label")_$fs=\$CUBE\""
    } > "$job"
    if [ "$DRY_RUN" = 1 ]; then
        echo "written $job (sbatch --output=$(q "$log") $(q "$job"))"
        continue
    fi
    id=$(sbatch --parsable --output="$log" ${prev:+-d afterany:$prev} "$job")
    echo "$id $name" | tee -a "$CAMPAIGN/jobs.txt"
    prev=$id
done < "$CUBES"
echo "provenance: $CAMPAIGN/PROVENANCE.txt   stop with: scancel \$(cut -d' ' -f1 $CAMPAIGN/jobs.txt)"
