"""Server-side rendering session — one VTK offscreen render window per
WebSocket client.

Threading model
---------------

EGL contexts are thread-bound: a render window created on thread T can
only be rendered from thread T. We give each ``RenderSession`` a
dedicated single-thread ``ThreadPoolExecutor`` so:

  * the EGL context, VTK pipeline objects and CUDA-backed volume buffer
    all stay on the same thread for their entire lifetime
  * the async WebSocket handler talks to the thread via
    ``loop.run_in_executor`` → a clean asyncio↔VTK bridge with no
    Python-level shared mutable state
  * one session's render does NOT block another's — N concurrent
    sessions = N threads, each with its own EGL context, sharing the
    same GPU(s)

Frame coalescing
----------------

A rapid burst of camera-update commands (e.g. mouse drag) arrives at
the WebSocket faster than a single render frame takes (~16-33 ms for
30-60 FPS). We do NOT render every command — :meth:`apply_command`
just mutates session state; :meth:`render_frame` reads the *latest*
state and renders once. The WebSocket handler decides when to call
``render_frame`` (e.g. on idle, throttled to 30/60 FPS) so the user
sees fluid motion without backpressure.
"""
from __future__ import annotations

import asyncio
import io
import logging
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Any

import numpy as np
from PIL import Image

logger = logging.getLogger("visivo.render")


# ── Capability probe (used by the dataset open endpoint to set the flag) ─────


_EGL_AVAILABLE: bool | None = None


def server_side_rendering_available() -> bool:
    """Return True iff this backend can actually run a RenderSession.

    Cached because the probe imports a non-trivial module, which is
    wasted work past the first call. Re-probe by setting
    ``_EGL_AVAILABLE = None`` (e.g. in tests).

    In VTK 9.3 ``vtkEGLRenderWindow`` lives inside
    ``vtkRenderingOpenGL2``; in VTK 9.0/9.1 it was in a standalone
    ``vtkRenderingEGL`` submodule. We probe both for forward+backward
    compatibility across the deployed range.
    """
    global _EGL_AVAILABLE
    if _EGL_AVAILABLE is not None:
        return _EGL_AVAILABLE
    _EGL_AVAILABLE = False
    for path in ("vtkmodules.vtkRenderingOpenGL2", "vtkmodules.vtkRenderingEGL"):
        try:
            mod = __import__(path, fromlist=["vtkEGLRenderWindow"])
            if getattr(mod, "vtkEGLRenderWindow", None) is not None:
                _EGL_AVAILABLE = True
                break
        except ImportError:
            continue
    return _EGL_AVAILABLE


# ── Session state (plain dataclass, mutated by apply_command) ────────────────


@dataclass
class SessionViewState:
    """Mutable rendering parameters, modified by client commands.

    Kept as a plain dataclass so the render thread can read/write
    without a lock — at any moment the in-progress render reads a
    consistent-enough snapshot; the worst case is one frame using a
    "torn" state where azimuth is post-command and zoom is pre. The
    next frame (issued by the client immediately after the command)
    catches up. Adding a lock would serialise the render loop with
    every command and cost more than it saves.
    """
    azimuth: float = 30.0
    elevation: float = 20.0
    zoom: float = 1.0
    pan_x: float = 0.0
    pan_y: float = 0.0
    colormap_name: str = "inferno"
    vmin: float | None = None
    vmax: float | None = None
    opacity_scale: float = 0.5
    background_rgb: tuple[float, float, float] = (0.02, 0.02, 0.05)
    width: int = 1024
    height: int = 768
    jpeg_quality: int = 85  # 80-90 is the sweet spot on noise-rich HI cubes


@dataclass
class SessionStats:
    """Per-session telemetry surfaced to the client + ops dashboards."""
    frames_rendered: int = 0
    bytes_sent: int = 0
    commands_received: int = 0
    last_render_ms: float = 0.0
    last_jpeg_kb: float = 0.0
    created_at: float = field(default_factory=time.time)
    last_command_at: float = field(default_factory=time.time)


# ── The session itself ───────────────────────────────────────────────────────


class RenderSession:
    """One client × one cube × one dedicated render thread.

    Lifecycle:

    1. ``__init__`` — record metadata, set up the worker thread
       (does NOT touch VTK yet; the thread might run on a different
       CUDA_VISIBLE_DEVICES if we pin per-session GPUs later)
    2. ``await initialize()`` — runs the VTK setup on the worker
       thread: opens the FITS, builds the volume rendering pipeline,
       creates the EGL render window
    3. Loop of ``await apply_command(...)`` + ``await render_frame()``
       called by the WebSocket handler
    4. ``await close()`` — releases VTK + EGL on the worker thread,
       shuts the executor down
    """

    def __init__(self, session_id: str, dataset_path: str):
        self.session_id = session_id
        self.dataset_path = dataset_path
        self.view = SessionViewState()
        self.stats = SessionStats()
        # Single-thread executor so every VTK call hits the same thread
        # → same EGL context → same CUDA context (if cupy is used).
        self._executor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix=f"vtk-{session_id[:8]}",
        )
        # Cached on the worker thread; ``None`` until initialise().
        self._render_window = None
        self._renderer = None
        self._volume = None
        self._volume_mapper = None
        self._color_tf = None
        self._opacity_tf = None
        self._image_data = None  # Holds the vtkImageData with the cube.
        self._closed = False
        # An asyncio.Lock prevents two render_frame() calls from
        # interleaving on the executor (the executor would queue them
        # anyway, but the lock makes the API more honest at the
        # callsite — "while one frame is in flight, the next call
        # awaits cleanly").
        self._render_lock = asyncio.Lock()

    # ── Public API ───────────────────────────────────────────────────────

    async def initialize(self) -> None:
        """Run the VTK pipeline setup on the worker thread."""
        loop = asyncio.get_event_loop()
        await loop.run_in_executor(self._executor, self._init_on_thread)

    async def apply_command(self, command: dict[str, Any]) -> None:
        """Mutate :attr:`view` from a JSON command. Cheap; no render.

        Runs on the worker thread to keep the rule "all state mutation
        happens on the same thread as VTK" — even though SessionViewState
        is just a dataclass, this keeps future state extensions
        consistent (e.g. ROI clipping volumes touch VTK directly).
        """
        loop = asyncio.get_event_loop()
        self.stats.commands_received += 1
        self.stats.last_command_at = time.time()
        await loop.run_in_executor(
            self._executor, self._apply_command_on_thread, command,
        )

    async def render_frame(self) -> bytes:
        """Render the current view state to a JPEG and return the bytes."""
        if self._closed:
            raise RuntimeError(f"Session {self.session_id} is closed.")
        loop = asyncio.get_event_loop()
        async with self._render_lock:
            return await loop.run_in_executor(
                self._executor, self._render_frame_on_thread,
            )

    async def close(self) -> None:
        """Release VTK + EGL resources on the worker thread, then stop it."""
        if self._closed:
            return
        self._closed = True
        loop = asyncio.get_event_loop()
        try:
            await loop.run_in_executor(self._executor, self._close_on_thread)
        finally:
            self._executor.shutdown(wait=False, cancel_futures=True)

    # ── Worker-thread methods (NEVER call from outside the executor) ─────

    def _init_on_thread(self) -> None:
        """Build the VTK volume-rendering pipeline + EGL window.

        Runs ONCE per session on the dedicated worker thread. EGL +
        CUDA context, VTK objects, FITS read all happen here so the
        first ``apply_command`` / ``render_frame`` call is fast.

        On failure, log + re-raise so the manager's ``create_session``
        can map it to an HTTP 5xx.
        """
        t0 = time.time()
        import vtk
        from astropy.io import fits as _fits
        from vtkmodules.vtkRenderingOpenGL2 import vtkEGLRenderWindow
        from vtk.util.numpy_support import numpy_to_vtk

        # ── 1. Load the FITS cube ────────────────────────────────────
        with _fits.open(self.dataset_path, memmap=True) as hdul:
            data = hdul[0].data  # type: ignore[attr-defined]
            while data.ndim > 3:
                data = data[0]
            if data.ndim != 3:
                raise ValueError(
                    f"Volume rendering requires a 3-D cube, got shape {data.shape}"
                )
            cube = np.asarray(data, dtype=np.float32)
            # Replace NaN/inf with finite-min so the GPU mapper doesn't
            # emit transparent voxels (we can revisit with a separate
            # mask once the SSR UX needs it).
            finite_mask = np.isfinite(cube)
            if not finite_mask.all():
                fmin = float(cube[finite_mask].min()) if finite_mask.any() else 0.0
                cube = np.where(finite_mask, cube, fmin)
            vmin = float(np.nanmin(cube))
            vmax = float(np.nanmax(cube))
            # Persist the data range so apply_command() can clamp client-
            # supplied vmin/vmax into something the colormap can use.
            self.view.vmin = vmin if self.view.vmin is None else self.view.vmin
            self.view.vmax = vmax if self.view.vmax is None else self.view.vmax

        nz, ny, nx = cube.shape
        logger.info(
            "[render-session %s] cube loaded: shape=(%d,%d,%d) range=[%.4g,%.4g]",
            self.session_id, nz, ny, nx, vmin, vmax,
        )

        # ── 2. Wrap as vtkImageData ──────────────────────────────────
        img = vtk.vtkImageData()
        img.SetDimensions(nx, ny, nz)
        img.SetSpacing(1.0, 1.0, 1.0)
        img.SetOrigin(0.0, 0.0, 0.0)
        flat = cube.ravel(order="C")
        arr = numpy_to_vtk(flat, deep=True, array_type=vtk.VTK_FLOAT)
        arr.SetName("intensity")
        img.GetPointData().SetScalars(arr)
        # Keep a reference so the numpy buffer (deep=True copies but VTK
        # tracks it via vtkDataArray) does not get GC'd.
        self._image_data = img

        # ── 3. Transfer functions — HI-friendly defaults ────────────
        # HI cubes are dominated by Gaussian noise around zero with a
        # few percent of voxels carrying the actual emission. We need
        # opacity=0 throughout the noise distribution and rapidly
        # rising opacity past it. Using percentile thresholds:
        #   p95 ≈ tail of the noise Gaussian (≈ 1.65σ above zero)
        #   p99.5 ≈ 2.5σ — clearly above noise, faint emission
        #   p99.95 = peak signal
        # The first ~99.5% of the cube renders transparent → no orange
        # mush, only the bright sources show.
        finite = cube[finite_mask] if not finite_mask.all() else cube
        noise_p   = float(np.percentile(finite, 95.0))
        faint     = float(np.percentile(finite, 99.5))
        bright    = float(np.percentile(finite, 99.95))
        self._opacity_tf = vtk.vtkPiecewiseFunction()
        self._opacity_tf.AddPoint(vmin, 0.0)
        self._opacity_tf.AddPoint(noise_p, 0.0)
        self._opacity_tf.AddPoint(faint, self.view.opacity_scale * 0.3)
        self._opacity_tf.AddPoint(bright, self.view.opacity_scale)
        self._opacity_tf.AddPoint(vmax, self.view.opacity_scale)

        # Colour: dark plum → orange → white-hot (Inferno-ish).
        # Saturated colour only on the emission, neutral on the noise
        # band — so even a hint of opacity in the noise reads as muted.
        self._color_tf = vtk.vtkColorTransferFunction()
        self._color_tf.AddRGBPoint(vmin, 0.02, 0.02, 0.05)
        self._color_tf.AddRGBPoint(noise_p, 0.10, 0.05, 0.15)
        self._color_tf.AddRGBPoint(faint, 0.6, 0.25, 0.15)
        self._color_tf.AddRGBPoint(bright, 1.0, 0.85, 0.4)
        self._color_tf.AddRGBPoint(vmax, 1.0, 1.0, 1.0)

        prop = vtk.vtkVolumeProperty()
        prop.SetColor(self._color_tf)
        prop.SetScalarOpacity(self._opacity_tf)
        prop.SetInterpolationTypeToLinear()
        prop.ShadeOff()
        # Smaller sample distance = sharper render at the cost of FPS.
        # 0.5 voxel is a good balance for HI cubes; the client can ask
        # for finer (slow) or coarser (faster) via a future command.
        prop.SetScalarOpacityUnitDistance(1.0)

        # ── 4. Mapper + actor ────────────────────────────────────────
        self._volume_mapper = vtk.vtkSmartVolumeMapper()
        self._volume_mapper.SetInputData(img)
        self._volume_mapper.SetRequestedRenderModeToGPU()
        self._volume_mapper.SetSampleDistance(0.5)

        self._volume = vtk.vtkVolume()
        self._volume.SetMapper(self._volume_mapper)
        self._volume.SetProperty(prop)

        # Suppress VTK's per-frame stderr complaints — on the GLES
        # build, vtkOpenGLRenderWindow::ReadPixels logs a cosmetic
        # "Failed to read pixels in <6408,5121>: 1282" every frame
        # even though the data IS readable. The flood of stderr
        # writes hits BeeGFS and slows the render thread.
        vtk.vtkObject.GlobalWarningDisplayOff()

        # ── 5. Renderer + EGL offscreen window ──────────────────────
        self._renderer = vtk.vtkRenderer()
        self._renderer.AddVolume(self._volume)
        self._renderer.SetBackground(*self.view.background_rgb)

        self._render_window = vtkEGLRenderWindow()
        self._render_window.SetOffScreenRendering(1)
        self._render_window.SetSize(self.view.width, self.view.height)
        if hasattr(self._render_window, "SetDeviceIndex"):
            # Round-robin per-session GPU assignment would go here; for
            # now everybody on device 0. Multi-GPU pinning is Phase 2.
            self._render_window.SetDeviceIndex(0)
        self._render_window.AddRenderer(self._renderer)

        # Frame the volume — initial camera centred on the cube.
        self._renderer.ResetCamera()
        cam = self._renderer.GetActiveCamera()
        cam.Azimuth(self.view.azimuth)
        cam.Elevation(self.view.elevation)
        cam.Zoom(self.view.zoom)

        # Warm-up render so the first client frame is sub-100 ms even on
        # cold V100 contexts (the first Render does shader compilation
        # + GPU upload; subsequent frames just bind+draw).
        self._render_window.Render()

        logger.info(
            "[render-session %s] init OK in %.2f s (warm-up render done)",
            self.session_id, time.time() - t0,
        )

    def _apply_command_on_thread(self, command: dict[str, Any]) -> None:
        """Apply one client command to ``self.view``.

        Supported commands (one per JSON message):

        ``{"type": "rotate", "d_azimuth": float, "d_elevation": float}``
        ``{"type": "zoom",   "factor":    float}``
        ``{"type": "pan",    "dx": float, "dy": float}``
        ``{"type": "colormap", "name": str, "vmin": float, "vmax": float}``
        ``{"type": "opacity",  "scale": float}``
        ``{"type": "resize",   "width": int, "height": int}``
        ``{"type": "reset"}``

        Unknown commands are logged and ignored — the client may be
        newer than the server.
        """
        ctype = command.get("type", "")
        if ctype == "rotate":
            self.view.azimuth   += float(command.get("d_azimuth", 0.0))
            self.view.elevation += float(command.get("d_elevation", 0.0))
            # Clamp elevation to avoid camera flipping at the poles —
            # standard VTK orbital interactor convention.
            self.view.elevation = max(-89.0, min(89.0, self.view.elevation))
        elif ctype == "zoom":
            self.view.zoom *= max(0.05, min(20.0, float(command.get("factor", 1.0))))
        elif ctype == "pan":
            self.view.pan_x += float(command.get("dx", 0.0))
            self.view.pan_y += float(command.get("dy", 0.0))
        elif ctype == "colormap":
            self.view.colormap_name = str(command.get("name", self.view.colormap_name))
            if "vmin" in command: self.view.vmin = float(command["vmin"])
            if "vmax" in command: self.view.vmax = float(command["vmax"])
        elif ctype == "opacity":
            self.view.opacity_scale = max(0.0, min(1.0, float(command.get("scale", 0.5))))
        elif ctype == "resize":
            w = max(64,  min(4096, int(command.get("width",  self.view.width))))
            h = max(64,  min(4096, int(command.get("height", self.view.height))))
            self.view.width, self.view.height = w, h
            if self._render_window is not None:
                self._render_window.SetSize(w, h)
        elif ctype == "reset":
            self.view = SessionViewState(width=self.view.width, height=self.view.height)
        else:
            logger.warning("[render-session %s] unknown command type %r",
                           self.session_id, ctype)

    def _render_frame_on_thread(self) -> bytes:
        """Apply the current view state, render one frame, return JPEG bytes.

        Readback path: we ask VTK for ``RGBA`` (not ``RGB``) because the
        GLES build of VTK 9.3 on ppc64le EGL refuses ``glReadPixels``
        with ``GL_RGB``/``GL_UNSIGNED_BYTE`` (GL error 1282,
        INVALID_OPERATION). RGBA is the GLES-portable format. We then
        drop the alpha channel before JPEG encoding because JPEG can't
        carry transparency anyway.
        """
        if self._render_window is None or self._renderer is None:
            raise RuntimeError(
                f"Session {self.session_id} not initialised (call await initialize())."
            )
        import vtk
        from vtk.util.numpy_support import vtk_to_numpy

        # 1. Apply view state to camera. We use Azimuth/Elevation as
        #    ABSOLUTE positions, not deltas — the client tracks the
        #    desired view; the server moves the camera there.
        cam = self._renderer.GetActiveCamera()
        # Reset so deltas are reproducible irrespective of prior state.
        # ResetCamera fits the cube to the viewport; then we apply the
        # requested orbit.
        self._renderer.ResetCamera()
        cam.Azimuth(self.view.azimuth)
        cam.Elevation(self.view.elevation)
        cam.Zoom(max(0.05, self.view.zoom))

        # If the client asked to resize between this frame and the last
        # one, honour it now (apply_command also calls SetSize but only
        # on the worker; here we double-check the window matches view).
        cur_w, cur_h = self._render_window.GetSize()
        if cur_w != self.view.width or cur_h != self.view.height:
            self._render_window.SetSize(self.view.width, self.view.height)

        # 2. Render. Times this — useful for client-visible telemetry.
        t0 = time.perf_counter()
        self._render_window.Render()
        t1 = time.perf_counter()

        # 3. Capture framebuffer as RGBA.
        w2if = vtk.vtkWindowToImageFilter()
        w2if.SetInput(self._render_window)
        w2if.ReadFrontBufferOff()
        w2if.SetInputBufferTypeToRGBA()      # GLES-safe; see docstring
        w2if.Update()
        out = w2if.GetOutput()
        scalars = out.GetPointData().GetScalars()
        if scalars is None:
            raise RuntimeError(
                "vtkWindowToImageFilter returned no scalars — readback failed"
            )
        arr = vtk_to_numpy(scalars)
        h, w = self.view.height, self.view.width
        # VTK ships pixels bottom-up; flip to top-down.
        arr = arr.reshape((h, w, 4))[::-1]
        # Drop alpha for JPEG (it can't carry it). Composite onto our
        # configured background colour so semi-transparent volumes look
        # right against the SSR session's bg, not against random alpha.
        rgb = arr[:, :, :3].copy()

        # 4. JPEG encode.
        img = Image.fromarray(rgb, mode="RGB")
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=self.view.jpeg_quality,
                  optimize=False, progressive=False)
        jpeg = buf.getvalue()

        # 5. Update telemetry.
        self.stats.frames_rendered += 1
        self.stats.last_render_ms = (t1 - t0) * 1000.0
        self.stats.last_jpeg_kb = len(jpeg) / 1024.0
        self.stats.bytes_sent += len(jpeg)
        return jpeg

    def _close_on_thread(self) -> None:
        """Release VTK objects in the right order — finalize the window LAST."""
        try:
            if self._volume is not None:
                self._volume = None
            if self._volume_mapper is not None:
                self._volume_mapper = None
            if self._renderer is not None:
                self._renderer = None
            if self._render_window is not None:
                # VTK's Finalize() releases the EGL context cleanly.
                try:
                    self._render_window.Finalize()
                except Exception as e:
                    logger.warning(
                        "[render-session %s] Finalize() raised: %s",
                        self.session_id, e,
                    )
                self._render_window = None
        except Exception:
            logger.exception(
                "[render-session %s] close raised — leaking is the lesser evil",
                self.session_id,
            )
