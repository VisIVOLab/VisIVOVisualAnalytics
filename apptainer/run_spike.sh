#!/usr/bin/env bash
# Phase 0 spike runner — fire the EGL/volume-render probe inside the
# Apptainer container, on a SLURM-allocated GPU node.
#
# Why this wrapper exists:
#   * Bind the data directory (BeeGFS) into the container.
#   * Bind the spike directory so the .py file is visible.
#   * Pass --nv to expose the NVIDIA driver + libEGL_nvidia.so.0.
#   * Pre-flight ldconfig diagnostic — if EGL is missing, surface it
#     before the Python script even starts.
#
# Usage (FROM A GPU COMPUTE NODE, NOT THE FRONTEND):
#   bash apptainer/run_spike.sh \
#       /mnt/beegfs/fvitello/data/WALLABY.fits \
#       /tmp/spike_output.png
#
# Or via salloc / srun --pty:
#   srun --partition=gpu --gres=gpu:v100:1 --cpus-per-task=4 --mem=16G \
#        --pty bash apptainer/run_spike.sh \
#        /mnt/beegfs/fvitello/data/WALLABY.fits /tmp/spike_output.png
set -euo pipefail

FITS="${1:-}"
OUT="${2:-/tmp/spike_output.png}"
SIF="${VISIVO_SIF:-$HOME/VisIVOVisualAnalytics/backend-gpu.sif}"
SPIKE="${VISIVO_SPIKE:-$HOME/VisIVOVisualAnalytics/apptainer/spike_egl_volume.py}"

if [[ -z "$FITS" ]]; then
    echo "Usage: $0 <fits_path> [<output_png>]" >&2
    exit 2
fi

if [[ ! -f "$SIF" ]]; then
    echo "[spike] ERROR: SIF not found: $SIF" >&2
    exit 1
fi
if [[ ! -f "$SPIKE" ]]; then
    echo "[spike] ERROR: spike script not found: $SPIKE" >&2
    exit 1
fi
if [[ ! -f "$FITS" ]]; then
    echo "[spike] ERROR: FITS not found: $FITS" >&2
    exit 1
fi

# Auto-detect a data-root bind: take everything before /fvitello in the
# FITS path. Works for /mnt/beegfs/fvitello/data/X.fits → /mnt/beegfs.
DATA_ROOT="$(dirname "$(dirname "$FITS")")"
echo "[spike] Will bind data root: $DATA_ROOT"
echo "[spike] Will bind spike dir : $(dirname "$SPIKE")"
echo "[spike] SIF                  : $SIF"
echo

# Pre-flight: peek at NVIDIA libs the container will see.
echo "[spike] Pre-flight: NVIDIA + EGL libs visible inside container"
apptainer exec --nv "$SIF" bash -c '
    echo "  ldconfig: libnvidia-glcore  → $(ldconfig -p | grep -c libnvidia-glcore.so 2>/dev/null || echo 0) entries"
    echo "  ldconfig: libEGL_nvidia     → $(ldconfig -p | grep -c libEGL_nvidia.so 2>/dev/null || echo 0) entries"
    echo "  ldconfig: libEGL            → $(ldconfig -p | grep -c "libEGL\\.so" 2>/dev/null || echo 0) entries"
    echo "  ldconfig: libGLX_nvidia     → $(ldconfig -p | grep -c libGLX_nvidia.so 2>/dev/null || echo 0) entries"
'
echo

echo "[spike] Running volume-render spike..."
apptainer exec --nv \
    --bind "$DATA_ROOT:$DATA_ROOT" \
    --bind "$(dirname "$SPIKE"):$(dirname "$SPIKE")" \
    --env PYTHONPATH=/app \
    "$SIF" \
    python3 "$SPIKE" "$FITS" "$OUT"

echo
echo "[spike] Done. Output:"
ls -la "$OUT" 2>/dev/null || echo "  (no PNG written — check the diagnostics above)"
