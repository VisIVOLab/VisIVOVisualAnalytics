#!/usr/bin/env python3
"""
Phase 0 spike — verify that VTK inside the Apptainer container can do
GPU-accelerated offscreen volume rendering on the Power9 V100s.

What it does:
  1. Probes which VTK render-window backends are available
     (EGL → hardware GPU; OSMesa → CPU software; OpenGL2 → needs DISPLAY).
  2. Loads a FITS cube via astropy memmap.
  3. Wraps it in a ``vtkImageData``.
  4. Builds a minimal volume-rendering pipeline (vtkSmartVolumeMapper +
     vtkVolume + scalar/opacity transfer functions).
  5. Creates an OFFSCREEN render window (EGL preferred), renders, captures
     the framebuffer with ``vtkWindowToImageFilter`` and saves a PNG.
  6. Prints which backend was selected and the render wall-time.

Failure modes:
  - ``ImportError: vtkRenderingEGL`` → VTK in this container has no EGL
    support. Need to install the ``vtk-egl`` conda variant, or build with
    ``-DVTK_OPENGL_HAS_EGL=ON``.
  - ``vtkEGLRenderWindow: cannot initialize EGL display`` → ``--nv`` didn't
    expose libEGL_nvidia.so.0. Check ``ldconfig -p | grep EGL`` inside
    the container.
  - ``cannot find OpenGL volume mapper`` → driver doesn't expose GL_ARB_*
    extensions needed; fall back to a Texture3D-based mapper.

Usage:
  apptainer exec --nv \\
      --bind /mnt/beegfs:/mnt/beegfs \\
      backend-gpu.sif \\
      python3 /home/fvitello/VisIVO-next/apptainer/spike_egl_volume.py \\
      /mnt/beegfs/fvitello/data/WALLABY.fits \\
      /tmp/spike_output.png
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path


def probe_backends() -> dict:
    """Try importing each VTK render-window backend, report what's available.

    In VTK 9.3 ``vtkEGLRenderWindow`` lives inside ``vtkRenderingOpenGL2``
    (conditional on the ``VTK_OPENGL_HAS_EGL`` build flag), NOT in a
    separate ``vtkRenderingEGL`` submodule like in VTK 9.0/9.1. We
    probe both for forward+backward compat across the deployed range.
    """
    backends = {}

    try:
        # Core OpenGL2; needs DISPLAY (X11) — not usable in headless container.
        import vtkmodules.vtkRenderingOpenGL2  # noqa: F401
        backends["OpenGL2"] = "available (but needs X11 DISPLAY)"
    except ImportError as e:
        backends["OpenGL2"] = f"missing: {e}"

    try:
        # OSMesa — CPU software rasteriser, slow but always works headless.
        from vtkmodules.vtkRenderingOpenGL2 import vtkOSOpenGLRenderWindow  # noqa: F401
        backends["OSMesa"] = "available (CPU software)"
    except ImportError as e:
        backends["OSMesa"] = f"missing: {e}"

    # EGL — try VTK 9.3 path first (vtkEGLRenderWindow inside
    # vtkRenderingOpenGL2), then the legacy 9.0/9.1 path.
    backends["EGL"] = "not probed"
    for path in ("vtkmodules.vtkRenderingOpenGL2", "vtkmodules.vtkRenderingEGL"):
        try:
            mod = __import__(path, fromlist=["vtkEGLRenderWindow"])
            cls = getattr(mod, "vtkEGLRenderWindow", None)
            if cls is not None:
                backends["EGL"] = f"available via {path} (hardware GPU)"
                break
        except ImportError as e:
            backends["EGL"] = f"missing: {e}"

    return backends


def make_offscreen_render_window():
    """Build the best offscreen render window we can.

    Preference: EGL (GPU) > OSMesa (CPU) > OpenGL2 (only if DISPLAY is set).
    Returns (window, backend_name). Raises RuntimeError if nothing works.
    """
    # EGL — best case. Try VTK 9.3 path (inside vtkRenderingOpenGL2)
    # first, then the legacy 9.0/9.1 standalone path.
    for path in ("vtkmodules.vtkRenderingOpenGL2", "vtkmodules.vtkRenderingEGL"):
        try:
            mod = __import__(path, fromlist=["vtkEGLRenderWindow"])
            EGLRenderWindow = getattr(mod, "vtkEGLRenderWindow", None)
            if EGLRenderWindow is None:
                continue
            win = EGLRenderWindow()
            win.SetSize(1024, 1024)
            win.SetOffScreenRendering(1)
            # Force EGL device 0 — first GPU. Pinning to a specific
            # device is done via CUDA_VISIBLE_DEVICES on the parent.
            return win, f"EGL (via {path.rsplit('.', 1)[-1]})"
        except ImportError:
            continue
        except Exception as e:
            print(f"[spike] EGL init via {path} failed: {e}", file=sys.stderr)
            continue

    # OSMesa — software fallback.
    try:
        from vtkmodules.vtkRenderingOpenGL2 import vtkOSOpenGLRenderWindow
        win = vtkOSOpenGLRenderWindow()
        win.SetSize(1024, 1024)
        win.SetOffScreenRendering(1)
        return win, "OSMesa"
    except ImportError:
        pass

    # Standard OpenGL2 with DISPLAY (NOT what we want here).
    if os.environ.get("DISPLAY"):
        import vtk
        win = vtk.vtkRenderWindow()
        win.SetSize(1024, 1024)
        win.SetOffScreenRendering(1)
        return win, "OpenGL2+DISPLAY"

    raise RuntimeError(
        "No usable offscreen render-window backend. "
        "Need EGL (preferred), OSMesa, or DISPLAY."
    )


def load_fits_as_image_data(fits_path: str):
    """Read a FITS cube into a vtkImageData via the smallest possible bridge."""
    from astropy.io import fits as _fits
    import numpy as np
    import vtk
    from vtk.util.numpy_support import numpy_to_vtk

    print(f"[spike] Opening {fits_path}...", flush=True)
    with _fits.open(fits_path, memmap=True) as hdul:
        cube = hdul[0].data  # type: ignore[attr-defined]
        # Astropy axis order is (z, y, x); VTK expects (z, y, x) flat with
        # x varying fastest. squeeze trailing 1-dims.
        cube = np.asarray(cube, dtype=np.float32)
        while cube.ndim > 3:
            cube = cube[0]
        if cube.ndim != 3:
            raise ValueError(f"Expected 3D cube, got shape {cube.shape}")
        print(f"[spike] Cube shape: {cube.shape}, dtype {cube.dtype}, "
              f"size {cube.nbytes / 1e9:.2f} GB", flush=True)

        nz, ny, nx = cube.shape
        # Replace NaN with min for the spike — production would build a
        # proper mask. Without this the volume mapper produces black.
        finite_mask = np.isfinite(cube)
        if not finite_mask.all():
            finite_min = float(cube[finite_mask].min()) if finite_mask.any() else 0.0
            cube = np.where(finite_mask, cube, finite_min)
        flat = cube.ravel(order="C")

    img = vtk.vtkImageData()
    img.SetDimensions(nx, ny, nz)
    img.SetSpacing(1.0, 1.0, 1.0)
    img.SetOrigin(0.0, 0.0, 0.0)

    arr = numpy_to_vtk(flat, deep=True, array_type=vtk.VTK_FLOAT)
    arr.SetName("intensity")
    img.GetPointData().SetScalars(arr)

    vmin = float(np.nanmin(cube))
    vmax = float(np.nanmax(cube))
    print(f"[spike] Data range: [{vmin:.6g}, {vmax:.6g}]", flush=True)
    return img, vmin, vmax


def build_volume_pipeline(image_data, vmin: float, vmax: float):
    """Return (volume, renderer, mapper)."""
    import vtk

    # Use SmartVolumeMapper — it picks the best mapper available at runtime
    # (GPURayCast on EGL/CUDA, RayCast or Texture3D fallback otherwise).
    mapper = vtk.vtkSmartVolumeMapper()
    mapper.SetInputData(image_data)
    mapper.SetRequestedRenderModeToGPU()  # explicit GPU when possible

    # Linear opacity ramp on the data range, peaks above the noise floor.
    opacity_tf = vtk.vtkPiecewiseFunction()
    opacity_tf.AddPoint(vmin, 0.0)
    mid = 0.3 * vmin + 0.7 * vmax
    opacity_tf.AddPoint(mid, 0.05)
    opacity_tf.AddPoint(vmax, 0.4)

    # Grey-to-hot colour ramp.
    color_tf = vtk.vtkColorTransferFunction()
    color_tf.AddRGBPoint(vmin, 0.05, 0.05, 0.10)
    color_tf.AddRGBPoint(mid, 0.7, 0.4, 0.2)
    color_tf.AddRGBPoint(vmax, 1.0, 1.0, 0.9)

    volume_property = vtk.vtkVolumeProperty()
    volume_property.SetColor(color_tf)
    volume_property.SetScalarOpacity(opacity_tf)
    volume_property.ShadeOff()
    volume_property.SetInterpolationTypeToLinear()

    volume = vtk.vtkVolume()
    volume.SetMapper(mapper)
    volume.SetProperty(volume_property)

    renderer = vtk.vtkRenderer()
    renderer.AddVolume(volume)
    renderer.SetBackground(0.02, 0.02, 0.04)

    return volume, renderer, mapper


def save_png(window, output_path: str) -> None:
    import vtk

    w2if = vtk.vtkWindowToImageFilter()
    w2if.SetInput(window)
    w2if.ReadFrontBufferOff()
    w2if.SetInputBufferTypeToRGB()
    w2if.Update()

    writer = vtk.vtkPNGWriter()
    writer.SetFileName(output_path)
    writer.SetInputConnection(w2if.GetOutputPort())
    writer.Write()


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        print(f"Usage: {sys.argv[0]} <fits_path> <output_png>", file=sys.stderr)
        return 2

    fits_path = sys.argv[1]
    output_png = sys.argv[2]

    if not Path(fits_path).is_file():
        print(f"[spike] ERROR: file not found: {fits_path}", file=sys.stderr)
        return 2

    print("[spike] VTK backend probe:")
    for name, status in probe_backends().items():
        print(f"  - {name}: {status}")

    import vtk
    print(f"[spike] vtk.VTK_VERSION = {vtk.VTK_VERSION}")

    # Show what's in CUDA_VISIBLE_DEVICES (informs which GPU EGL grabs).
    print(f"[spike] CUDA_VISIBLE_DEVICES = {os.environ.get('CUDA_VISIBLE_DEVICES', '(unset)')}")

    t_load_0 = time.perf_counter()
    image_data, vmin, vmax = load_fits_as_image_data(fits_path)
    t_load_1 = time.perf_counter()
    print(f"[spike] FITS load + ingest into vtkImageData: {t_load_1 - t_load_0:.2f} s")

    t_pipe_0 = time.perf_counter()
    volume, renderer, mapper = build_volume_pipeline(image_data, vmin, vmax)
    window, backend = make_offscreen_render_window()
    window.AddRenderer(renderer)
    print(f"[spike] Render window backend: {backend}")
    t_pipe_1 = time.perf_counter()
    print(f"[spike] Pipeline build: {t_pipe_1 - t_pipe_0:.2f} s")

    # Frame the volume.
    renderer.ResetCamera()
    cam = renderer.GetActiveCamera()
    cam.Azimuth(30)
    cam.Elevation(20)
    cam.Zoom(1.1)

    t_render_0 = time.perf_counter()
    window.Render()
    t_render_1 = time.perf_counter()
    print(f"[spike] First render: {t_render_1 - t_render_0:.3f} s")

    # Report which mapper SmartVolumeMapper actually picked.
    last_mode = mapper.GetLastUsedRenderMode()
    mode_names = {
        0: "DefaultRenderMode",
        1: "RayCastRenderMode",
        2: "GPURenderMode",
        3: "OSPRayRenderMode",
        4: "AnariRenderMode",
    }
    print(f"[spike] SmartVolumeMapper last mode: "
          f"{mode_names.get(last_mode, last_mode)}")

    # Render a second time to measure steady-state FPS.
    t_render_0 = time.perf_counter()
    for _ in range(5):
        cam.Azimuth(2)
        window.Render()
    t_render_1 = time.perf_counter()
    fps = 5.0 / (t_render_1 - t_render_0)
    print(f"[spike] Steady-state: {fps:.1f} FPS over 5 frames")

    save_png(window, output_png)
    print(f"[spike] Saved PNG: {output_png}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
