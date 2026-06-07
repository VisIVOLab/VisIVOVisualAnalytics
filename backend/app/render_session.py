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

    Cached because the probe imports a non-trivial module and tries to
    open an EGL display, which is wasted work past the first call.
    Re-probe by setting ``_EGL_AVAILABLE = None`` (e.g. in tests).
    """
    global _EGL_AVAILABLE
    if _EGL_AVAILABLE is not None:
        return _EGL_AVAILABLE
    try:
        # The build we point at must include vtkRenderingEGL — see
        # apptainer/backend-conda-egl.def for the cmake flags.
        from vtkmodules.vtkRenderingEGL import vtkEGLRenderWindow  # noqa: F401
        _EGL_AVAILABLE = True
    except ImportError:
        _EGL_AVAILABLE = False
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
        """STUB — fill in when EGL build is confirmed working.

        Real implementation (sketched):

        .. code-block:: python

           import vtk
           from vtkmodules.vtkRenderingEGL import vtkEGLRenderWindow
           from astropy.io import fits

           # 1. Load the FITS slab as a memmap (or full read for small cubes)
           hdul = fits.open(self.dataset_path, memmap=True)
           cube = np.asarray(hdul[0].data, dtype=np.float32)

           # 2. Wrap as vtkImageData
           img = vtk.vtkImageData()
           ... numpy_to_vtk(cube.ravel(), …)

           # 3. Pipeline: SmartVolumeMapper → vtkVolume → vtkRenderer
           self._volume_mapper = vtk.vtkSmartVolumeMapper()
           self._volume_mapper.SetInputData(img)
           self._volume_mapper.SetRequestedRenderModeToGPU()
           self._color_tf = vtk.vtkColorTransferFunction()
           self._opacity_tf = vtk.vtkPiecewiseFunction()
           ... set initial TFs from cube range ...
           prop = vtk.vtkVolumeProperty()
           prop.SetColor(self._color_tf)
           prop.SetScalarOpacity(self._opacity_tf)
           self._volume = vtk.vtkVolume()
           self._volume.SetMapper(self._volume_mapper)
           self._volume.SetProperty(prop)
           self._renderer = vtk.vtkRenderer()
           self._renderer.AddVolume(self._volume)
           self._renderer.SetBackground(*self.view.background_rgb)
           self._render_window = vtkEGLRenderWindow()
           self._render_window.SetOffScreenRendering(1)
           self._render_window.SetSize(self.view.width, self.view.height)
           self._render_window.AddRenderer(self._renderer)
           # Warm-up render so the first user frame is fast.
           self._render_window.Render()
        """
        logger.info("[render-session %s] init STUB — path=%s",
                    self.session_id, self.dataset_path)
        # Mark "initialised but stub" so render_frame returns a placeholder.

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
        """STUB — fill in once VTK pipeline is real.

        Real implementation (sketched):

        .. code-block:: python

           # 1. Update the camera from view state
           cam = self._renderer.GetActiveCamera()
           cam.Azimuth(self.view.azimuth_delta)   # store delta separately
           cam.Elevation(self.view.elevation_delta)
           cam.Zoom(self.view.zoom_delta)

           # 2. Render
           t0 = time.perf_counter()
           self._render_window.Render()
           t1 = time.perf_counter()

           # 3. Capture framebuffer
           w2if = vtk.vtkWindowToImageFilter()
           w2if.SetInput(self._render_window)
           w2if.ReadFrontBufferOff()
           w2if.SetInputBufferTypeToRGB()
           w2if.Update()
           arr = vtk_to_numpy(w2if.GetOutput().GetPointData().GetScalars())
           h, w = self.view.height, self.view.width
           arr = arr.reshape((h, w, 3))[::-1]   # VTK flips Y

           # 4. JPEG encode
           img = Image.fromarray(arr)
           buf = io.BytesIO()
           img.save(buf, format="JPEG", quality=self.view.jpeg_quality)
           jpeg = buf.getvalue()

           self.stats.frames_rendered += 1
           self.stats.last_render_ms = (t1 - t0) * 1000.0
           self.stats.last_jpeg_kb = len(jpeg) / 1024.0
           self.stats.bytes_sent += len(jpeg)
           return jpeg

        For now we return a tiny PLACEHOLDER JPEG so the WebSocket
        plumbing (and the future client) can be tested end-to-end
        independently of the VTK backend.
        """
        # Placeholder: a 2-D image filled with the colormap_name's hash so
        # at least the client sees the frame stream update at the rate
        # we drive it. Replace with the real VTK pipeline above when
        # EGL is online.
        seed = hash(
            (self.view.azimuth, self.view.elevation, self.view.zoom,
             self.view.colormap_name)
        ) & 0xFFFFFFFF
        rng = np.random.RandomState(seed % 2**32)
        h, w = self.view.height, self.view.width
        arr = rng.randint(0, 256, size=(h, w, 3), dtype=np.uint8)
        img = Image.fromarray(arr)
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=self.view.jpeg_quality)
        jpeg = buf.getvalue()
        self.stats.frames_rendered += 1
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
