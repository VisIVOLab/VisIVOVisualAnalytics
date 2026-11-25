# syntax=docker/dockerfile:1.7
#
# The VisIVO backend (FastAPI) as an OCI image, for Docker/Podman and, pulled
# with `apptainer pull docker://...`, for HPC and SRC nodes. Build from the
# repository root:
#
#   docker build -f docker/Dockerfile -t visivo-backend .                  # CPU
#   docker build -f docker/Dockerfile --target egl -t visivo-backend:egl . # EGL
#
# Two targets, one recipe:
#
#   cpu (default)  linux/amd64 and linux/arm64. The scientific stack from PyPI
#                  (backend/requirements.txt), SoFiA-2 built from source. It does
#                  every analysis; server-side 3-D rendering reports itself
#                  unavailable.
#   egl            linux/amd64. The same stack with VTK built from source headless
#                  with EGL (vtkEGLRenderWindow), so a GPU node renders the cube
#                  itself. The PyPI VTK wheel is NOT used there: it has no EGL
#                  window, and on a Leonardo node (A100) the 9.7.2 wheel crashed
#                  creating vtkPolyDataMapper under EGL. The flags are those of
#                  apptainer/backend-conda-egl.def, which rendered on PLEIADI-GPU.
#
# The EGL image does not need conda's OpenGL libraries removed, as that
# definition does: there is no conda here, and the only other source of GL
# libraries, the VTK wheel, is uninstalled before the source build is added.
#
# The VTK build takes about an hour and depends on nothing but the base image
# and VTK_TAG, so it is a stage of its own; CI caches it (type=gha).
#
# At run time: docker/entrypoint.sh. Nothing is written inside the image, the
# user is not root, and the backend starts only when it is told which directories
# it may read (a bound /data, or VISIVO_DATA_ROOT[S]).

ARG PYTHON_IMAGE=python:3.12-slim-bookworm

# ---- SoFiA-2 ---------------------------------------------------------------
# conda-forge builds sofia-2 for linux-64 only, and PyPI has no build at all.
# SoFiA-2 lives on GitLab (the GitHub repository is archived); WCSLIB from apt.
FROM ${PYTHON_IMAGE} AS sofia
ARG SOFIA_REF=v2.7.1
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential wcslib-dev git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch "${SOFIA_REF}" https://gitlab.com/SoFiA-Admin/SoFiA-2.git /tmp/SoFiA-2 \
    && make -C /tmp/SoFiA-2 OMP=true CC=/usr/bin/gcc \
    && install -m 755 /tmp/SoFiA-2/sofia /usr/local/bin/sofia

# ---- Python environment ------------------------------------------------------
# A virtualenv, so the final stages take it with one COPY. Requirements first
# (layer cache), the application after.
FROM ${PYTHON_IMAGE} AS pydeps-cpu
COPY backend/requirements.txt /tmp/requirements.txt
RUN python -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir -r /tmp/requirements.txt

# The EGL image: the same environment without the VTK wheel.
FROM pydeps-cpu AS pydeps-egl
RUN /opt/venv/bin/pip uninstall -y vtk \
    && if /opt/venv/bin/python -c "import vtkmodules" 2>/dev/null; then \
         echo "FATAL: the PyPI vtkmodules is still importable"; exit 1; fi

# ---- VTK with EGL ----------------------------------------------------------
FROM ${PYTHON_IMAGE} AS vtk-egl
ARG VTK_TAG=v9.3.1
# Mesa's EGL/GL development packages give the headers and libglvnd's libEGL.so
# to link against; at run time the host's NVIDIA libraries are the vendor.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config ca-certificates \
        libegl-dev libegl1 libgl-dev libopengl-dev libglvnd-dev \
    && rm -rf /var/lib/apt/lists/*
RUN <<'BUILD'
set -eu
PY=/usr/local/bin/python3
PYVER="$($PY -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
SITE="lib/python${PYVER}/site-packages"
EGL_LIBDIR="/usr/lib/$(gcc -print-multiarch)"
EGL_LIB="$EGL_LIBDIR/libEGL.so"
[ -e "$EGL_LIB" ] || ln -sf libEGL.so.1 "$EGL_LIB"
for f in "$EGL_LIB" "$EGL_LIBDIR/libOpenGL.so" /usr/include/EGL/egl.h /usr/include/KHR/khrplatform.h; do
    [ -e "$f" ] || { echo "FATAL: $f missing"; exit 1; }
done

git clone --depth 1 --branch "${VTK_TAG}" https://gitlab.kitware.com/vtk/vtk.git /tmp/vtk-src
mkdir -p /tmp/vtk-build
cd /tmp/vtk-build
# Every -D carries its :TYPE, or CMake stores it UNINITIALIZED and VTK's options
# never see it. Desktop OpenGL through EGL, not GLES: on PLEIADI-GPU a GLES build
# crashed inside the NVIDIA driver on the first GL call. The cell-grid and LIC
# modules stay off: the backend renders volumes and slices only. Installed under
# /opt/venv (the layout of the virtualenv it joins), into DESTDIR=/out.
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE:STRING=Release \
    -DCMAKE_INSTALL_PREFIX:PATH=/opt/venv \
    -DCMAKE_INSTALL_LIBDIR:STRING=lib \
    -DBUILD_SHARED_LIBS:BOOL=ON \
    -DBUILD_TESTING:BOOL=OFF \
    -DBUILD_EXAMPLES:BOOL=OFF \
    -DVTK_USE_X:BOOL=OFF \
    -DVTK_USE_COCOA:BOOL=OFF \
    -DVTK_OPENGL_HAS_EGL:BOOL=ON \
    -DVTK_OPENGL_HAS_OSMESA:BOOL=OFF \
    -DVTK_OPENGL_USE_GLES:BOOL=OFF \
    -DVTK_DEFAULT_RENDER_WINDOW_HEADLESS:BOOL=ON \
    -DOpenGL_GL_PREFERENCE:STRING=GLVND \
    -DOPENGL_opengl_LIBRARY:FILEPATH="$EGL_LIBDIR/libOpenGL.so" \
    -DOPENGL_egl_LIBRARY:FILEPATH="$EGL_LIB" \
    -DOPENGL_EGL_INCLUDE_DIR:PATH=/usr/include \
    -DEGL_INCLUDE_DIR:PATH=/usr/include \
    -DEGL_LIBRARY:FILEPATH="$EGL_LIB" \
    -DVTK_WRAP_PYTHON:BOOL=ON \
    -DVTK_PYTHON_VERSION:STRING=3 \
    -DPython3_EXECUTABLE:FILEPATH="$PY" \
    -DVTK_PYTHON_SITE_PACKAGES_SUFFIX:STRING="$SITE" \
    -DVTK_GROUP_ENABLE_StandAlone:STRING=YES \
    -DVTK_GROUP_ENABLE_Rendering:STRING=YES \
    -DVTK_GROUP_ENABLE_Imaging:STRING=YES \
    -DVTK_GROUP_ENABLE_Views:STRING=YES \
    -DVTK_GROUP_ENABLE_MPI:STRING=DONT_WANT \
    -DVTK_GROUP_ENABLE_Qt:STRING=DONT_WANT \
    -DVTK_GROUP_ENABLE_Web:STRING=DONT_WANT \
    -DVTK_MODULE_ENABLE_VTK_RenderingVolume:STRING=YES \
    -DVTK_MODULE_ENABLE_VTK_RenderingVolumeOpenGL2:STRING=YES \
    -DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2:STRING=YES \
    -DVTK_MODULE_ENABLE_VTK_RenderingLICOpenGL2:STRING=NO \
    -DVTK_MODULE_ENABLE_VTK_RenderingCellGrid:STRING=NO \
    -DVTK_MODULE_ENABLE_VTK_FiltersCellGrid:STRING=NO \
    -DVTK_MODULE_ENABLE_VTK_IOCellGrid:STRING=NO \
    /tmp/vtk-src 2>&1 | tee /tmp/vtk-cmake.log
# A configure that quietly dropped EGL still builds; refuse it here.
grep -qE "^OPENGL_egl_LIBRARY:FILEPATH=.+libEGL" CMakeCache.txt \
    || { echo "FATAL: OPENGL_egl_LIBRARY not in the cache"; exit 1; }
grep -qE "^OPENGL_opengl_LIBRARY:FILEPATH=.+libOpenGL" CMakeCache.txt \
    || { echo "FATAL: OPENGL_opengl_LIBRARY not in the cache"; exit 1; }
grep -qi "vtkEGLRenderWindow" build.ninja \
    || { echo "FATAL: vtkEGLRenderWindow is not built"; exit 1; }

# One heavy C++ unit can take 1 GB: cap the jobs at half the memory in GB.
JOBS="$(nproc)"
SAFE="$(awk '/MemTotal/ {printf "%d", $2/1024/1024/2}' /proc/meminfo)"
[ "$SAFE" -ge 1 ] || SAFE=1
[ "$JOBS" -le "$SAFE" ] || JOBS="$SAFE"
echo "ninja -j$JOBS"
ninja -j "$JOBS"
DESTDIR=/out ninja install
[ -d "/out/opt/venv/$SITE/vtkmodules" ] \
    || { echo "FATAL: vtkmodules not installed under /opt/venv/$SITE"; exit 1; }
# The runtime needs neither headers nor CMake files.
rm -rf /out/opt/venv/include /out/opt/venv/lib/cmake /out/opt/venv/share /tmp/vtk-src /tmp/vtk-build
BUILD

# ---- Shared runtime base -----------------------------------------------------
FROM ${PYTHON_IMAGE} AS runtime
# libgomp: Numba/OpenMP. libgl1, libglib2: the VTK libraries of the CPU wheel.
# libwcs7: SoFiA-2. curl: container healthchecks.
RUN apt-get update \
    && apt-get install -y --no-install-recommends libgomp1 libglib2.0-0 libgl1 libwcs7 curl ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 visivo \
    && useradd --uid 10001 --gid 10001 --create-home --home-dir /home/visivo --shell /usr/sbin/nologin visivo
COPY --from=sofia /usr/local/bin/sofia /usr/local/bin/sofia
# The backend looks for either name.
RUN ln -s sofia /usr/local/bin/sofia2 \
    && if ldd /usr/local/bin/sofia | grep -q "not found"; then ldd /usr/local/bin/sofia; exit 1; fi
COPY --chmod=755 docker/entrypoint.sh /usr/local/bin/visivo-entrypoint
ENV PATH=/opt/venv/bin:$PATH \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8 \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PORT=8000
WORKDIR /app/backend
# The code is /app/backend/app. The launcher scripts' --backend-src binds a host
# checkout onto /app/app (the layout of the older Apptainer images), and a bind
# onto a symlink lands on its target, so that option works here unchanged.
RUN ln -s /app/backend/app /app/app
EXPOSE 8000
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/visivo-entrypoint"]

# ---- EGL image ---------------------------------------------------------------
FROM runtime AS egl
USER root
# libglvnd's dispatch libraries and libEGL: the host's NVIDIA EGL (libEGL_nvidia,
# which Docker's NVIDIA runtime or Apptainer's --nv provide) is the vendor
# behind them. No Mesa vendor is installed, so none can be picked instead.
RUN apt-get update \
    && apt-get install -y --no-install-recommends libegl1 libopengl0 libglx0 libglvnd0 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /usr/share/glvnd/egl_vendor.d \
    && printf '%s\n' '{' '    "file_format_version" : "1.0.0",' \
        '    "ICD" : { "library_path" : "libEGL_nvidia.so.0" }' '}' \
        > /usr/share/glvnd/egl_vendor.d/10_nvidia.json
COPY --from=pydeps-egl /opt/venv /opt/venv
COPY --from=vtk-egl /out/opt/venv /opt/venv
COPY backend/ /app/backend/
# Check what was built, so that a VTK without EGL fails the build here and not
# on a GPU node: the EGL window exists and links libEGL and glvnd's libOpenGL, no
# library or extension module of VTK is missing one of its own, the application's
# probe imports. No GPU is needed (and a builder has none): whether it renders is
# for the node to say.
RUN VISIVO_TOKEN=build HOME=/tmp VISIVO_EXPORTS_DIR=/tmp/x VISIVO_HIPS_CACHE=/tmp/y \
    python - <<'CHECK'
import glob, os, subprocess, sys
import vtk
from vtkmodules import vtkRenderingOpenGL2 as gl
print(f"vtk {vtk.VTK_VERSION} from {vtk.__file__}")
if not vtk.__file__.startswith("/opt/venv/"):
    sys.exit("FATAL: vtk is not the source build")
if not hasattr(gl, "vtkEGLRenderWindow"):
    sys.exit("FATAL: no vtkEGLRenderWindow")
libs = glob.glob("/opt/venv/lib/libvtk*.so*")
if not libs:
    sys.exit("FATAL: no VTK libraries in /opt/venv/lib")
for lib in libs:
    if "not found" in subprocess.check_output(["ldd", lib], text=True):
        sys.exit(f"FATAL: {lib} has a missing dependency")
# The Python extension modules, which link the libraries above: a module whose
# dependency is missing fails at import on the node, not here, unless looked at.
modules = glob.glob(os.path.join(os.path.dirname(gl.__file__), "*.so"))
if not modules:
    sys.exit("FATAL: no extension modules in vtkmodules")
for mod in modules:
    if "not found" in subprocess.check_output(["ldd", mod], text=True):
        sys.exit(f"FATAL: {mod} has a missing dependency")
render = glob.glob("/opt/venv/lib/libvtkRenderingOpenGL2-*.so")[0]
links = subprocess.check_output(["ldd", render], text=True)
if "libEGL" not in links:
    sys.exit(f"FATAL: {render} does not link libEGL")
# glvnd's libOpenGL, which dispatches to the host's NVIDIA libraries, and not
# the legacy libGL.so.1 (Mesa's, which would answer without a GPU).
if "libOpenGL.so" not in links or "libGL.so.1" in links:
    sys.exit(f"FATAL: {render} does not link glvnd's libOpenGL, or links libGL.so.1:\n{links}")
# What the backend's isosurface and velocity-field code uses, against the numpy
# the requirements resolved.
import numpy as np
from vtkmodules.util import numpy_support as ns
ns.vtk_to_numpy(ns.numpy_to_vtk(np.zeros(4, "f4")))
sys.path.insert(0, "/app/backend")
from app.render_session import server_side_rendering_available  # noqa: F401
print("vtkEGLRenderWindow present, linked to libEGL and libOpenGL; the application's probe imports")
CHECK
RUN python -m compileall -q /app/backend/app
# libglvnd picks the NVIDIA EGL vendor, not another. Under Apptainer the host's
# file is bound over this path (docs/technical/deployment/hpc-slurm.md).
ENV __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json \
    NVIDIA_VISIBLE_DEVICES=all \
    NVIDIA_DRIVER_CAPABILITIES=compute,utility,graphics
USER 10001:10001
ARG VERSION=dev
ARG REVISION=unknown
LABEL org.opencontainers.image.title="VisIVO backend (EGL)" \
      org.opencontainers.image.description="VisIVO Visual Analytics backend with server-side EGL rendering (NVIDIA GPU)" \
      org.opencontainers.image.source="https://github.com/VisIVOLab/VisIVOVisualAnalytics" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.version="${VERSION}-egl" \
      org.opencontainers.image.revision="${REVISION}"

# ---- CPU image (the default target) ------------------------------------------
FROM runtime AS cpu
COPY --from=pydeps-cpu /opt/venv /opt/venv
COPY backend/ /app/backend/
USER root
# The application imports with its dependencies, and its bytecode is in the
# image (it cannot be written next to the sources at run time).
RUN VISIVO_TOKEN=build HOME=/tmp VISIVO_EXPORTS_DIR=/tmp/x VISIVO_HIPS_CACHE=/tmp/y \
    python -c "import sys; sys.path.insert(0, '/app/backend'); import app.main, numba, vtk, healpy; print('imports OK, vtk', vtk.vtkVersion.GetVTKVersion())" \
    && python -m compileall -q /app/backend/app
USER 10001:10001
ARG VERSION=dev
ARG REVISION=unknown
LABEL org.opencontainers.image.title="VisIVO backend" \
      org.opencontainers.image.description="VisIVO Visual Analytics backend (CPU): the analysis API, no server-side rendering" \
      org.opencontainers.image.source="https://github.com/VisIVOLab/VisIVOVisualAnalytics" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${REVISION}"
