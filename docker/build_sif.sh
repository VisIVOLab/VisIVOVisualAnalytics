#!/usr/bin/env bash
# Build the backend image on any x86_64 machine with Docker or Podman and turn
# it into a .sif for an Apptainer/Singularity site that you then copy it to:
#
#   docker/build_sif.sh [--target egl|cpu] [--out FILE.sif] [--tag NAME[:TAG]]
#                       [--keep-archive] [-- EXTRA BUILD ARGS]
#
# The recipe is docker/Dockerfile (the one the published images come from), built
# for linux/amd64. The image is saved to an archive and converted with
# `apptainer build` or `singularity build` from docker-archive://, whichever the
# machine has. With neither (a workstation), the archive is left next to the
# output name and the command to run on the cluster is printed.
#
#   --target   egl (default: VTK built with EGL, server-side rendering on an
#              NVIDIA GPU node; the VTK build takes about an hour) or cpu
#   --out      the .sif to write (default: visivo-backend-<target>.sif here)
#   --tag      the image name in the local Docker/Podman store
#              (default: visivo-backend:local-<target>)
#   --keep-archive   keep the archive after a successful conversion
#   --          what follows goes to the build (e.g. --build-arg VTK_TAG=v9.3.1)
#
# DOCKER=podman (or having only podman) selects Podman. On an Apple Silicon Mac
# linux/amd64 is emulated and the egl target takes many hours: build it on an
# x86_64 machine.
set -euo pipefail

usage() { sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; }

TARGET=egl
OUT=""
TAG=""
KEEP=0
EXTRA=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --target)       TARGET="${2:?--target needs egl or cpu}"; shift 2 ;;
        --out)          OUT="${2:?--out needs a file name}"; shift 2 ;;
        --tag)          TAG="${2:?--tag needs a name}"; shift 2 ;;
        --keep-archive) KEEP=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        --)             shift; EXTRA=("$@"); break ;;
        *)              echo "build_sif.sh: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
done
case "$TARGET" in
    egl|cpu) ;;
    *) echo "build_sif.sh: --target is egl or cpu, not $TARGET" >&2; exit 2 ;;
esac
OUT="${OUT:-visivo-backend-$TARGET.sif}"
TAG="${TAG:-visivo-backend:local-$TARGET}"
case "$OUT" in
    *.sif) ARCHIVE="${OUT%.sif}.tar" ;;
    *)     ARCHIVE="$OUT.tar" ;;
esac

# The repository root, wherever the script is called from.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/docker/Dockerfile" ] || { echo "build_sif.sh: no docker/Dockerfile in $ROOT" >&2; exit 1; }

# Docker or Podman.
if [ -n "${DOCKER:-}" ]; then
    ENGINE="$DOCKER"
elif command -v docker >/dev/null 2>&1; then
    ENGINE=docker
elif command -v podman >/dev/null 2>&1; then
    ENGINE=podman
else
    echo "build_sif.sh: neither docker nor podman found" >&2
    exit 1
fi
command -v "$ENGINE" >/dev/null 2>&1 || { echo "build_sif.sh: $ENGINE not found" >&2; exit 1; }

# The version and revision go into the image's labels, as in the published ones.
REVISION="unknown"
if command -v git >/dev/null 2>&1; then
    REVISION="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
fi

echo "[build_sif] $ENGINE build, target $TARGET, linux/amd64, tag $TAG"
if [ "$(basename "$ENGINE")" = podman ]; then
    "$ENGINE" build --platform linux/amd64 -f "$ROOT/docker/Dockerfile" --target "$TARGET" \
        --build-arg "REVISION=$REVISION" ${EXTRA[@]+"${EXTRA[@]}"} -t "$TAG" "$ROOT"
    "$ENGINE" save --format docker-archive -o "$ARCHIVE" "$TAG"
else
    DOCKER_BUILDKIT=1 "$ENGINE" build --platform linux/amd64 -f "$ROOT/docker/Dockerfile" \
        --target "$TARGET" --build-arg "REVISION=$REVISION" ${EXTRA[@]+"${EXTRA[@]}"} \
        -t "$TAG" "$ROOT"
    "$ENGINE" save -o "$ARCHIVE" "$TAG"
fi
echo "[build_sif] image archive: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"

# Apptainer, else Singularity: both take the same command.
RUNTIME=""
for r in apptainer singularity; do
    if command -v "$r" >/dev/null 2>&1; then RUNTIME="$r"; break; fi
done
if [ -z "$RUNTIME" ]; then
    cat <<EOF
[build_sif] no apptainer or singularity here. Copy the archive to the cluster and convert it there:

    scp $ARCHIVE <login host>:<scratch>/
    # on the cluster (singularity or apptainer):
    singularity build $OUT docker-archive://<scratch>/$(basename "$ARCHIVE")

Both runtimes' manuals show this conversion without sudo; if yours refuses, add --fakeroot.
EOF
    exit 0
fi

echo "[build_sif] $RUNTIME build $OUT"
"$RUNTIME" build --force "$OUT" "docker-archive://$ARCHIVE"
if [ "$KEEP" = 0 ]; then
    rm -f "$ARCHIVE"
fi
echo "[build_sif] done: $OUT ($(du -h "$OUT" | cut -f1))"
echo "[build_sif] use it with: scripts/launch_backend_slurm.sh --sif $OUT ..."
