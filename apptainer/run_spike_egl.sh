#!/usr/bin/env bash
# Phase 0 spike runner — EGL build variant.
#
# Differences from ``run_spike.sh``:
#
#   * Uses ``backend-egl.sif`` (built from backend-conda-egl.def)
#   * MANUALLY binds the host's NVIDIA EGL/GLX libs over ``/usr/lib64``
#     in the container. On the INAF Power9 driver bundle (550.54.15),
#     Apptainer's default ``--nv`` does NOT include ``libEGL_nvidia.so.0``
#     in /etc/apptainer/nvliblist.conf, even though the lib is present
#     on the host. The explicit binds work around that without needing
#     admin access to edit nvliblist.conf.
#   * Sets ``__EGL_VENDOR_LIBRARY_FILENAMES`` so libglvnd resolves to
#     the NVIDIA EGL implementation (not Mesa software, which is also
#     present in the container).
#
# Usage (from a GPU compute node — via salloc or srun --pty):
#   bash apptainer/run_spike_egl.sh \
#       /mnt/beegfs/fvitello/data/WALLABY.fits \
#       /tmp/spike_egl_wallaby.png
set -euo pipefail

FITS="${1:-}"
OUT="${2:-/tmp/spike_egl_output.png}"
SIF="${VISIVO_SIF:-$HOME/VisIVO-next/backend-egl.sif}"
SPIKE="${VISIVO_SPIKE:-$HOME/VisIVO-next/apptainer/spike_egl_volume.py}"

if [[ -z "$FITS" ]]; then
    echo "Usage: $0 <fits_path> [<output_png>]" >&2
    exit 2
fi
[[ -f "$SIF" ]]   || { echo "[spike-egl] ERROR: SIF not found: $SIF" >&2; exit 1; }
[[ -f "$SPIKE" ]] || { echo "[spike-egl] ERROR: spike script not found: $SPIKE" >&2; exit 1; }
[[ -f "$FITS" ]]  || { echo "[spike-egl] ERROR: FITS not found: $FITS" >&2; exit 1; }

# Auto-detect a data-root bind (everything before the user's home in
# the FITS path).
DATA_ROOT="$(dirname "$(dirname "$FITS")")"

# Explicit NVIDIA EGL/GLX binds. We probe /usr/lib64 (RHEL/Rocky on
# Power9) and /usr/lib/powerpc64le-linux-gnu (Debian/Ubuntu on
# Power9, less common). Skip silently if the host lib is missing —
# the spike's own backend-probe step will report what's accessible.
NV_BINDS=()
for d in /usr/lib64 /usr/lib/powerpc64le-linux-gnu /usr/lib/x86_64-linux-gnu; do
    for lib in libEGL_nvidia.so.0 libGLX_nvidia.so.0 libEGL.so.1 \
               libnvidia-glcore.so.* libnvidia-glsi.so.* \
               libnvidia-egl-wayland.so.*; do
        # Resolve glob, take the first match.
        for f in $d/$lib; do
            if [[ -e "$f" ]]; then
                NV_BINDS+=(--bind "$f:$f")
            fi
        done
    done
done

# Vendor JSON tells libglvnd which .so implements EGL. NVIDIA ships
# this in /usr/share/glvnd/egl_vendor.d/. Bind it so libglvnd inside
# the container picks the NVIDIA backend over Mesa software.
GLVND_JSON="/usr/share/glvnd/egl_vendor.d/10_nvidia.json"
if [[ -f "$GLVND_JSON" ]]; then
    NV_BINDS+=(--bind "$GLVND_JSON:$GLVND_JSON")
fi

echo "[spike-egl] Bindings:"
printf '             %s\n' "${NV_BINDS[@]}"
echo "[spike-egl] Data root: $DATA_ROOT"
echo

# Pre-flight ldconfig probe — same as run_spike.sh, gives us
# "before" / "after" comparison.
echo "[spike-egl] Pre-flight inside container (with explicit NV binds + LD_LIBRARY_PATH)"
apptainer exec --nv "${NV_BINDS[@]}" \
    --env LD_LIBRARY_PATH=/usr/lib64 \
    "$SIF" bash -c '
    # Probe both filesystem presence and ldconfig visibility.
    echo "  /usr/lib64/libEGL_nvidia.so.0 file: $(test -e /usr/lib64/libEGL_nvidia.so.0 && echo yes || echo no)"
    echo "  ldconfig: libEGL_nvidia     → $(ldconfig -p | grep -c libEGL_nvidia.so 2>/dev/null || echo 0) entries"
    echo "  ldconfig: libGLX_nvidia     → $(ldconfig -p | grep -c libGLX_nvidia.so 2>/dev/null || echo 0) entries"
    # The real test: can python dlopen libEGL_nvidia.so.0? libglvnd
    # does this internally when the EGL window is created; if it
    # fails, EGLs Render() hangs forever (no fallback path).
    python3 -c "
import ctypes, sys
try:
    ctypes.CDLL(\"libEGL_nvidia.so.0\")
    print(\"  dlopen libEGL_nvidia.so.0: OK\")
except OSError as e:
    print(f\"  dlopen libEGL_nvidia.so.0: FAILED — {e}\")
    sys.exit(0)
"
    echo "  vendor JSON visible         : $([ -f /usr/share/glvnd/egl_vendor.d/10_nvidia.json ] && echo yes || echo no)"
'
echo

echo "[spike-egl] Running EGL volume-render spike..."
# CRITICAL: bind targets are /usr/lib64/* (RHEL convention) but the
# container is Debian, which searches /usr/lib/powerpc64le-linux-gnu
# instead. ldconfig inside the container does NOT find the bound
# libs → libglvnd can't dlopen them → EGL Render() hangs forever.
# Explicit LD_LIBRARY_PATH overrides the cache and forces dlopen to
# look in /usr/lib64 first.
apptainer exec --nv \
    "${NV_BINDS[@]}" \
    --bind "$DATA_ROOT:$DATA_ROOT" \
    --bind "$(dirname "$SPIKE"):$(dirname "$SPIKE")" \
    --env PYTHONPATH=/app \
    --env LD_LIBRARY_PATH=/usr/lib64 \
    --env __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json \
    "$SIF" \
    python3 "$SPIKE" "$FITS" "$OUT"

echo
echo "[spike-egl] Done. Output:"
ls -la "$OUT" 2>/dev/null || echo "  (no PNG written — check the diagnostics above)"
