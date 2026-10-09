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

# Explicit NVIDIA library binds — the comprehensive list. Apptainer's
# default ``--nv`` config (nvliblist.conf) on ppc64le misses several
# critical libs that vtkEGLRenderWindow needs at first Render() time:
# libcuda.so (yes, even for pure GL rendering — NVIDIA EGL hits it for
# context bind), libnvidia-tls (TLS support), libnvidia-rtcore (rt
# core helper), libnvidia-allocator (mem mgmt), libnvidia-ml (mgmt).
# Without these, eglMakeCurrent or the first GL call hangs in a futex
# wait inside libEGL_nvidia / libnvidia-glcore.
NV_BINDS=()
for d in /usr/lib64 /usr/lib/powerpc64le-linux-gnu /usr/lib/x86_64-linux-gnu; do
    for lib in \
        libEGL_nvidia.so.0 libGLX_nvidia.so.0 libEGL.so.1 \
        libnvidia-glcore.so.* libnvidia-glsi.so.* libnvidia-egl-wayland.so.* \
        libcuda.so.1 libcuda.so.* \
        libnvidia-tls.so.* \
        libnvidia-rtcore.so.* \
        libnvidia-allocator.so.* \
        libnvidia-cfg.so.* \
        libnvidia-ml.so.* \
        libnvidia-encode.so.* libnvidia-opencl.so.* \
        libnvidia-ptxjitcompiler.so.* \
        libnvidia-nvvm.so.* \
        libnvidia-fatbinaryloader.so.* \
        libnvidia-compiler.so.* ; do
        # Resolve glob, take the first match for each pattern.
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
    --env __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json \
    --env __GLX_VENDOR_LIBRARY_NAME=nvidia \
    "$SIF" bash -c '
    echo "  /usr/lib64/libEGL_nvidia.so.0 file: $(test -e /usr/lib64/libEGL_nvidia.so.0 && echo yes || echo no)"
    echo "  CUDA_VISIBLE_DEVICES envvar: ${CUDA_VISIBLE_DEVICES:-(unset)}"
    echo
    echo "  NVIDIA support libs visible inside container:"
    for lib in libcuda.so.1 libnvidia-tls.so libnvidia-rtcore.so \
               libnvidia-allocator.so.1 libnvidia-cfg.so.1 libnvidia-ml.so.1; do
        loc=$(find /usr/lib64 -name "${lib}*" 2>/dev/null | head -1)
        marker=$([ -n "$loc" ] && echo "✓" || echo "✗")
        echo "    ${marker} ${lib}: ${loc:-MISSING}"
    done
    echo
    echo "  ldconfig: libEGL_nvidia     → $(ldconfig -p | grep -c libEGL_nvidia.so 2>/dev/null || echo 0) entries"
    echo "  ldconfig: libGLX_nvidia     → $(ldconfig -p | grep -c libGLX_nvidia.so 2>/dev/null || echo 0) entries"
    echo "  ldconfig: libcuda           → $(ldconfig -p | grep -c libcuda.so 2>/dev/null || echo 0) entries"
    python3 -c "
import ctypes
try:
    ctypes.CDLL(\"libEGL_nvidia.so.0\")
    print(\"  dlopen libEGL_nvidia.so.0: OK\")
except OSError as e:
    print(f\"  dlopen libEGL_nvidia.so.0: FAILED — {e}\")
"
    echo "  vendor JSON visible         : $([ -f /usr/share/glvnd/egl_vendor.d/10_nvidia.json ] && echo yes || echo no)"
    echo
    echo "  Bare EGL deep probe (replicates what vtkEGLRenderWindow does)..."
    timeout 30 python3 << "EOF" || echo "  EGL_deep_probe: TIMED OUT"
import ctypes, time
egl = ctypes.CDLL("libEGL.so.1")

# EGL constants from EGL/egl.h
EGL_NO_DISPLAY     = 0
EGL_NO_CONTEXT     = 0
EGL_NO_SURFACE     = 0
EGL_VENDOR         = 0x3053
EGL_EXTENSIONS     = 0x3055
EGL_SURFACE_TYPE   = 0x3033
EGL_PBUFFER_BIT    = 0x0001
EGL_RED_SIZE       = 0x3024
EGL_GREEN_SIZE     = 0x3023
EGL_BLUE_SIZE      = 0x3022
EGL_DEPTH_SIZE     = 0x3025
EGL_RENDERABLE_TYPE = 0x3040
EGL_OPENGL_ES2_BIT = 0x0004
EGL_NONE           = 0x3038
EGL_WIDTH          = 0x3057
EGL_HEIGHT         = 0x3056
EGL_CONTEXT_CLIENT_VERSION = 0x3098

egl.eglGetDisplay.argtypes = [ctypes.c_void_p]
egl.eglGetDisplay.restype = ctypes.c_void_p
egl.eglInitialize.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
egl.eglInitialize.restype = ctypes.c_int
egl.eglQueryString.argtypes = [ctypes.c_void_p, ctypes.c_int]
egl.eglQueryString.restype = ctypes.c_char_p
egl.eglChooseConfig.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                                  ctypes.POINTER(ctypes.c_void_p), ctypes.c_int,
                                  ctypes.POINTER(ctypes.c_int)]
egl.eglChooseConfig.restype = ctypes.c_int
egl.eglCreatePbufferSurface.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
egl.eglCreatePbufferSurface.restype = ctypes.c_void_p
egl.eglBindAPI.argtypes = [ctypes.c_int]
egl.eglBindAPI.restype = ctypes.c_int
egl.eglCreateContext.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_int)]
egl.eglCreateContext.restype = ctypes.c_void_p
egl.eglMakeCurrent.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
egl.eglMakeCurrent.restype = ctypes.c_int
egl.eglGetError.argtypes = []
egl.eglGetError.restype = ctypes.c_int

def step(name, fn):
    t0 = time.perf_counter()
    rv = fn()
    dt = (time.perf_counter() - t0) * 1000.0
    err = egl.eglGetError()
    print(f"    {name}: rv={rv} err=0x{err:x} took {dt:.1f}ms")
    return rv

disp = step("1. eglGetDisplay", lambda: egl.eglGetDisplay(EGL_NO_DISPLAY) or 0)
if not disp:
    raise SystemExit(0)

maj, mn = ctypes.c_int(), ctypes.c_int()
step("2. eglInitialize", lambda: egl.eglInitialize(disp, ctypes.byref(maj), ctypes.byref(mn)))
print(f"       version: {maj.value}.{mn.value}")
print(f"       vendor : {egl.eglQueryString(disp, EGL_VENDOR).decode()}")

# Config attribs typical of vtkEGLRenderWindow (GLES2 renderable).
attribs = (ctypes.c_int * 13)(
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_DEPTH_SIZE, 24,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE,
)
config = ctypes.c_void_p()
n_configs = ctypes.c_int()
step("3. eglChooseConfig",
     lambda: egl.eglChooseConfig(disp, attribs, ctypes.byref(config), 1, ctypes.byref(n_configs)))
print(f"       got {n_configs.value} config(s)")
if not n_configs.value:
    print("    → no matching config; this is the FIRST hang point in VTK")
    raise SystemExit(0)

# Pbuffer surface attribs: 256x256.
pb_attribs = (ctypes.c_int * 5)(EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE)
surface = step("4. eglCreatePbufferSurface",
               lambda: egl.eglCreatePbufferSurface(disp, config, pb_attribs))

EGL_OPENGL_API = 0x30A2
EGL_OPENGL_ES_API = 0x30A0
step("5. eglBindAPI(GLES)", lambda: egl.eglBindAPI(EGL_OPENGL_ES_API))

ctx_attribs = (ctypes.c_int * 3)(EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE)
ctx = step("6. eglCreateContext",
           lambda: egl.eglCreateContext(disp, config, EGL_NO_CONTEXT, ctx_attribs))

step("7. eglMakeCurrent", lambda: egl.eglMakeCurrent(disp, surface, surface, ctx))

print("    EGL_deep_probe: ALL 7 STEPS SUCCEEDED")
EOF
'
echo

echo "[spike-egl] Running EGL volume-render spike..."
# CRITICAL: bind targets are /usr/lib64/* (RHEL convention) but the
# container is Debian, which searches /usr/lib/powerpc64le-linux-gnu
# instead. ldconfig inside the container does NOT find the bound
# libs → libglvnd can't dlopen them → EGL Render() hangs forever.
# Explicit LD_LIBRARY_PATH overrides the cache and forces dlopen to
# look in /usr/lib64 first.
# Redirect every XDG cache / config / data path to /tmp (tmpfs, local,
# no NFS, no fcntl locks). Without this, fontconfig and Freetype
# (transitively loaded by vtkRenderingOpenGL2) try to take POSIX file
# locks on cache files under $HOME which sits on an NFS share with
# a misconfigured / unreachable lockd → fcntl(F_SETLKW) hangs FOREVER
# in the kernel's RPC layer. Symptom indistinguishable from an EGL
# bug; root cause has nothing to do with NVIDIA/EGL/VTK.
mkdir -p /tmp/visivo-cache /tmp/visivo-config /tmp/visivo-data \
         /tmp/visivo-mpl   /tmp/visivo-home
apptainer exec --nv \
    "${NV_BINDS[@]}" \
    --bind "$DATA_ROOT:$DATA_ROOT" \
    --bind "$(dirname "$SPIKE"):$(dirname "$SPIKE")" \
    --env PYTHONPATH=/app \
    --env LD_LIBRARY_PATH=/usr/lib64 \
    --env CUDA_VISIBLE_DEVICES="${CUDA_VISIBLE_DEVICES:-0}" \
    --env __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json \
    --env __GLX_VENDOR_LIBRARY_NAME=nvidia \
    --env XDG_CACHE_HOME=/tmp/visivo-cache \
    --env XDG_CONFIG_HOME=/tmp/visivo-config \
    --env XDG_DATA_HOME=/tmp/visivo-data \
    --env XDG_RUNTIME_DIR=/tmp/visivo-runtime \
    --env MPLCONFIGDIR=/tmp/visivo-mpl \
    --env FONTCONFIG_PATH=/etc/fonts \
    "$SIF" \
    python3 "$SPIKE" "$FITS" "$OUT"

echo
echo "[spike-egl] Done. Output:"
ls -la "$OUT" 2>/dev/null || echo "  (no PNG written — check the diagnostics above)"
