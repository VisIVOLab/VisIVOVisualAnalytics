#!/usr/bin/env python3
"""Minimal EGL render — a single coloured sphere, no volume, no FITS.

Purpose: isolate whether ``vtkEGLRenderWindow`` itself is broken on this
NVIDIA driver / ppc64le build, OR whether only the volume mapper is.
A sphere render that hangs in the same place as the volume spike =
the window is broken, the mapper is innocent. A sphere render that
SUCCEEDS = the window is fine, the volume mapper needs separate fix.

Usage (inside the EGL SIF with the same binds as the full spike):
    python3 spike_egl_minimal.py /tmp/sphere.png
"""
from __future__ import annotations

import os
import sys
import threading
import time


def _watchdog(seconds: int = 20):
    def _kill():
        time.sleep(seconds)
        print(f"\n[minimal] FATAL: hung after {seconds}s — "
              f"vtkEGLRenderWindow itself is broken.")
        os._exit(3)
    threading.Thread(target=_kill, daemon=True).start()


def main() -> int:
    # The shared run_spike_egl.sh wrapper invokes us with TWO positional
    # args (fits_path, png_path) — the same as the volume spike — even
    # though we don't need the FITS one. Take the LAST argv element as
    # the output, and refuse to write to anything ending in ``.fits``
    # (defensive guard: a previous version of this script accidentally
    # treated argv[1]=FITS as the output path and overwrote the user's
    # downloaded cube file with PNG bytes).
    out = sys.argv[-1] if len(sys.argv) > 1 else "/tmp/sphere.png"
    if out.lower().endswith(".fits"):
        print(f"[minimal] REFUSING to write PNG to a .fits path: {out}")
        print(f"[minimal] (full argv={sys.argv})")
        out = "/tmp/sphere.png"
        print(f"[minimal] Falling back to {out}")
    print(f"[minimal] CUDA_VISIBLE_DEVICES={os.environ.get('CUDA_VISIBLE_DEVICES','(unset)')}")
    print(f"[minimal] LD_LIBRARY_PATH={os.environ.get('LD_LIBRARY_PATH','(unset)')}")
    print(f"[minimal] __EGL_VENDOR_LIBRARY_FILENAMES="
          f"{os.environ.get('__EGL_VENDOR_LIBRARY_FILENAMES','(unset)')}")

    _watchdog(20)

    import vtk
    print(f"[minimal] vtk {vtk.VTK_VERSION}")

    # 1. Sphere
    print("[minimal] Creating sphere source...", flush=True)
    sphere = vtk.vtkSphereSource()
    sphere.SetThetaResolution(40)
    sphere.SetPhiResolution(40)

    mapper = vtk.vtkPolyDataMapper()
    mapper.SetInputConnection(sphere.GetOutputPort())

    actor = vtk.vtkActor()
    actor.SetMapper(mapper)
    actor.GetProperty().SetColor(0.9, 0.3, 0.2)

    renderer = vtk.vtkRenderer()
    renderer.AddActor(actor)
    renderer.SetBackground(0.05, 0.05, 0.10)

    # 2. EGL render window
    print("[minimal] Creating vtkEGLRenderWindow...", flush=True)
    from vtkmodules.vtkRenderingOpenGL2 import vtkEGLRenderWindow
    window = vtkEGLRenderWindow()
    window.SetSize(512, 512)
    window.SetOffScreenRendering(1)
    if hasattr(window, "SetDeviceIndex"):
        window.SetDeviceIndex(0)
        print(f"[minimal] EGL device count: {window.GetNumberOfDevices()}", flush=True)

    print("[minimal] AddRenderer...", flush=True)
    window.AddRenderer(renderer)

    print("[minimal] ResetCamera...", flush=True)
    renderer.ResetCamera()

    print("[minimal] Render() — this is where the volume spike hangs...", flush=True)
    t0 = time.perf_counter()
    window.Render()
    t1 = time.perf_counter()
    print(f"[minimal] Render() returned in {t1 - t0:.3f} s — EGL window OK ✓",
          flush=True)

    # Save
    w2if = vtk.vtkWindowToImageFilter()
    w2if.SetInput(window)
    w2if.Update()
    writer = vtk.vtkPNGWriter()
    writer.SetFileName(out)
    writer.SetInputConnection(w2if.GetOutputPort())
    writer.Write()
    print(f"[minimal] Saved {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
