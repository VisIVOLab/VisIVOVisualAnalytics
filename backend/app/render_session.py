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


# ── LUT registry — match the desktop's ColorMaps ─────────────────────────────
#
# The desktop ``ColorMaps`` registers ~30 named LUTs. The matplotlib
# equivalents below cover the perceptual ones the user is likely to
# pick from the picker (Inferno is the default). For LUT names the
# desktop has but matplotlib does not (Glow, Sar, Temperature, …) we
# fall back to a small set of hand-built control points so the volume
# at least stays recognisable instead of resetting to Inferno silently.
#
# All matplotlib mappings sample 256 evenly-spaced colours and feed
# them as ``AddRGBPoint`` control points to a vtkColorTransferFunction
# — this is what the client does internally via
# ``ColorMaps::SetColorTransferFunction(lut, ctf)``, so the resulting
# colours are pixel-identical between local and remote rendering.

# Names sent by the desktop client (the keys in ColorMaps.cpp) that
# match matplotlib's registry verbatim once lower-cased. Anything
# matching this lookup goes through matplotlib for byte-identical
# colours; anything outside the lookup but matching a built-in
# matplotlib colormap (Rainbow, Hot, Jet, Hsv, …) is also picked up
# because the lookup falls through to a case-insensitive matplotlib
# probe before giving up.
_DESKTOP_TO_MPL = {
    "Default":         "inferno",
    "Inferno":         "inferno",
    "Viridis":         "viridis",
    "Magma":           "magma",
    "Plasma":          "plasma",
    "Cividis":         "cividis",
    "Gray":            "gray",
    "Glow":            "hot",        # closest perceptual match
    "Temperature":     "RdYlBu_r",
    "Sar":             "afmhot",
    "VolRenGreen":     "Greens",
    "VolRenGlow":      "hot",
    "VolRenRGB":       "jet",
    # NB: Pure* and All* are intentionally NOT here — they have
    # hand-built ramps below because their desktop equivalents go
    # BLACK→colour (Pure*) or SOLID colour (All*), not the matplotlib
    # WHITE→colour curves.
}


# Hand-built ramps for desktop LUTs that don't map onto matplotlib's
# colour-named registry. Each ramp is described by a 2-control-point
# definition (r,g,b at vmin, r,g,b at vmax); the builder interpolates
# linearly. ``All*`` ramps repeat the same colour at both ends so the
# whole data range maps to one solid hue — same role as the desktop's
# ``ColorMaps::SetAllRed`` for single-colour overlay masks.
_HAND_BUILT_RAMPS = {
    # name           : ((r0,g0,b0), (r1,g1,b1))
    "PureRed":         ((0.0, 0.0, 0.0), (1.0, 0.0, 0.0)),
    "PureGreen":       ((0.0, 0.0, 0.0), (0.0, 1.0, 0.0)),
    "PureBlue":        ((0.0, 0.0, 0.0), (0.0, 0.0, 1.0)),
    "AllRed":          ((1.0, 0.0, 0.0), (1.0, 0.0, 0.0)),
    "AllGreen":        ((0.0, 1.0, 0.0), (0.0, 1.0, 0.0)),
    "AllBlue":         ((0.0, 0.0, 1.0), (0.0, 0.0, 1.0)),
    "AllCyan":         ((0.0, 1.0, 1.0), (0.0, 1.0, 1.0)),
    "AllMagenta":      ((1.0, 0.0, 1.0), (1.0, 0.0, 1.0)),
    "AllYellow":       ((1.0, 1.0, 0.0), (1.0, 1.0, 0.0)),
    "AllWhite":        ((1.0, 1.0, 1.0), (1.0, 1.0, 1.0)),
    "AllBlack":        ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)),
}


def _build_color_tf(name: str, vmin: float, vmax: float):
    """Build a vtkColorTransferFunction named ``name`` over ``[vmin, vmax]``.

    Lookup order:

    1. Exact-name match against the hand-built ramps (Pure*, All*).
    2. Exact-name match against the desktop ↔ matplotlib table.
    3. Direct matplotlib colormap lookup (lower-cased) — picks up any
       built-in mpl name the user might have configured (Rainbow, Jet,
       Hot, etc.).
    4. Fall back to Inferno + WARN log so the operator sees which name
       went unmatched.

    256 evenly-spaced control points; identical sampling to the
    desktop's ``ColorMaps::SetColorTransferFunction``.
    """
    import vtk
    ctf = vtk.vtkColorTransferFunction()

    # Hand-built ramps short-circuit the matplotlib path entirely.
    hand = _HAND_BUILT_RAMPS.get(name)
    if hand is not None:
        (r0, g0, b0), (r1, g1, b1) = hand
        n = 256
        for i in range(n):
            t = i / (n - 1)
            r = r0 + t * (r1 - r0)
            g = g0 + t * (g1 - g0)
            b = b0 + t * (b1 - b0)
            v = vmin + t * (vmax - vmin)
            ctf.AddRGBPoint(v, r, g, b)
        logger.info("[render-session] LUT %r → hand-built ramp", name)
        return ctf

    try:
        import matplotlib  # noqa: F401
        from matplotlib import colormaps as mpl_colormaps
    except ImportError:
        matplotlib = None  # type: ignore[assignment]

    if matplotlib is None:
        # No matplotlib available — flat fallback so the volume is at
        # least visible. Real fix: install matplotlib in the SIF.
        logger.warning(
            "[render-session] matplotlib unavailable — LUT %r becomes a "
            "grey ramp", name,
        )
        ctf.AddRGBPoint(vmin, 0.0, 0.0, 0.0)
        ctf.AddRGBPoint(vmax, 1.0, 1.0, 1.0)
        return ctf

    mpl_key = _DESKTOP_TO_MPL.get(name)
    if mpl_key is None:
        # Try direct matplotlib lookup, case-insensitively.
        all_mpl_keys = {k.lower(): k for k in mpl_colormaps}
        mpl_key = all_mpl_keys.get(name.lower())
    if mpl_key is None:
        logger.warning(
            "[render-session] unknown LUT %r — falling back to Inferno", name,
        )
        mpl_key = "inferno"
    logger.info("[render-session] LUT %r → matplotlib %r", name, mpl_key)

    cmap = mpl_colormaps.get_cmap(mpl_key)
    n = 256
    for i in range(n):
        t = i / (n - 1)
        r, g, b, _ = cmap(t)
        v = vmin + t * (vmax - vmin)
        ctf.AddRGBPoint(v, r, g, b)
    return ctf


def _build_opacity_tf(vmin: float, vmax: float, threshold: float):
    """Build a vtkPiecewiseFunction matching ``vtkWindowCube::applyThresholdToPipeline``.

    The desktop curve is a 6-point ramp anchored at the threshold:
    ::

        vmin                   → 0.0
        threshold - 1 % span   → 0.0   ← sharp knee
        threshold              → 0.25
        threshold + 5 % span   → 0.6
        threshold + 20 % span  → 0.9
        vmax                   → 1.0

    Reproducing it byte-for-byte means the SSR and local volumes show
    the same opacity vs intensity. ``threshold`` is clamped to
    ``[vmin, vmax]``; out-of-range values fall back to a gentle linear
    ramp, matching the local fallback path.
    """
    import vtk
    span = vmax - vmin
    tf = vtk.vtkPiecewiseFunction()
    if span <= 0.0:
        tf.AddPoint(vmin, 0.0)
        tf.AddPoint(vmax, 1.0)
        return tf
    threshold = max(vmin, min(vmax, threshold))
    tf.AddPoint(vmin,                            0.00)
    knee = max(vmin, threshold - 0.01 * span)
    tf.AddPoint(knee,                            0.00)
    tf.AddPoint(threshold,                       0.25)
    tf.AddPoint(min(threshold + 0.05 * span, vmax), 0.60)
    tf.AddPoint(min(threshold + 0.20 * span, vmax), 0.90)
    tf.AddPoint(vmax,                            1.00)
    return tf


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

    Volume rendering parity with the desktop pipeline
    -------------------------------------------------
    The desktop ``vtkWindowCube`` builds its opacity TF with five
    explicit control points driven by the user's threshold slider:
    ::

        invisibleSentinel → 0.0
        cubeRange[0]      → 0.0
        threshold         → 0.12
        midpoint          → 0.45
        cubeRange[1]      → 0.85

    We mirror that here so the same ``threshold`` value yields the
    same volume look on the remote and local renderers.
    """
    azimuth: float = 30.0
    elevation: float = 20.0
    zoom: float = 1.0
    pan_x: float = 0.0
    pan_y: float = 0.0

    # ── Render mode ───────────────────────────────────────────────
    # ``volume``     → ray-cast volume render (default)
    # ``isosurface`` → vtkFlyingEdges3D mesh at ``threshold`` isovalue
    cube_render_mode: str = "volume"

    # ── Volume LUT / TF state ─────────────────────────────────────
    volume_lut_name: str = "Inferno"           # picker in 3D View Settings
    slice_lut_name:  str = "Inferno"           # picker in 2D View Settings
    # ``threshold`` is the user's lower-bound slider value; the
    # opacity TF is rebuilt around it. ``None`` until either the
    # backend computes a sensible default after FITS load OR the
    # client pushes the slider's current value.
    threshold: float | None = None
    # 0 = Composite, 1 = MIP, 2 = MinIP  — VTK ray-cast blend modes.
    blend_mode: int = 0
    vmin: float | None = None
    vmax: float | None = None
    # Backward-compat: still honoured by ``opacity`` command for older
    # clients that don't speak threshold yet. New clients send the
    # local threshold directly and ignore this knob.
    opacity_scale: float = 0.5

    # ── Scene chrome (matches the local cube view) ────────────────
    show_outline: bool = True       # cube bounding-box wireframe
    show_axes:    bool = True       # XYZ orientation marker
    # Matches ``vtkWindowCube::setupCubeRenderer`` line 1
    # ``ren->SetBackground(0.21, 0.23, 0.25)`` — the same warm-cool
    # gray the desktop renderer uses. Pure black makes the chrome
    # (outline, plane edges) look starker than the local widget.
    background_rgb: tuple[float, float, float] = (0.21, 0.23, 0.25)

    # ── Cutting plane (textured with the slice colors) ────────────
    cutting_plane_visible: bool = True
    cutting_plane_opacity: float = 0.5
    cutting_plane_slice_index: int = 0     # in [0, nz)

    # ── Interactive LOD ─────────────────────────────────────────────
    # When True (client sends ``set_interactive`` on mouse press), the
    # volume + cutting-plane + isosurface actors are hidden so only the
    # cheap bounding-box wireframe renders. Goes back to False on mouse
    # release. Lets us sustain 60 FPS during rotations without giving
    # up the full-quality still frame at rest.
    interactive: bool = False

    # ── Viewport / encoding ────────────────────────────────────────
    # 1920×1080 default keeps the first frames at near-Retina quality
    # even on desktop docks of ~1500 px wide — the client resize-on-Live
    # command catches up within a frame or two, but downscale-from-FHD
    # always looks better than upscale-from-XGA. NVENC encodes 1080p
    # in ~5 ms on a V100, well inside the 30-60 FPS budget.
    width: int = 1920
    height: int = 1080
    # HI cube renderings have lots of fine-grained noise + chrome
    # (slice texture, axes triad, bounding-box edges) that JPEG q=85
    # smears into mosquito artefacts — a side-by-side comparison
    # against the local QVTKOpenGLNativeWidget reads as "blurry". q=95
    # gets us almost-lossless at ~2× the byte cost; on a tunneled WS
    # this is still under 250 KB/frame at 1080p, plenty of headroom.
    jpeg_quality: int = 95


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
        self._volume_property = None      # vtkVolumeProperty — TF holder
        self._color_tf = None
        self._opacity_tf = None
        self._image_data = None  # Holds the vtkImageData with the cube.
        self._cube_np = None     # Original numpy cube (for slice texture extraction)
        # Cube extent + range — derived once at init, then read-only.
        self._nx = self._ny = self._nz = 0
        self._vmin = self._vmax = 0.0
        # Scene chrome actors — kept as attributes so commands can
        # toggle visibility without rebuilding the scene.
        self._outline_actor = None        # vtkActor (bounding box wireframe)
        self._axes_actor = None           # vtkAxesActor (XYZ orientation)
        self._axes_renderer = None        # corner viewport renderer for axes
        # Isosurface rendering — built lazily; mutually exclusive with
        # the volume actor (one visibility-toggled by cube_render_mode).
        self._iso_filter = None           # vtkFlyingEdges3D
        self._iso_mapper = None           # vtkPolyDataMapper
        self._iso_actor  = None           # vtkActor
        # Low-res "galaxy wireframe" pipeline for interactive LOD.
        # Cube is shrunk 4x before FlyingEdges3D so the mesh has
        # roughly 1/64 of the triangles of the full-res iso — renders
        # in <1 ms even for a complex emission. Shown only during
        # set_interactive=True.
        self._iso_lod_shrink = None       # vtkImageShrink3D
        self._iso_lod_filter = None       # vtkFlyingEdges3D on shrunk
        self._iso_lod_mapper = None
        self._iso_lod_actor  = None
        # Render-window size remembered across an LOD cycle so we can
        # restore it on release. None when not in interactive mode.
        self._lod_prev_size = None
        # Cutting plane infrastructure — see _build_cutting_plane().
        self._cutting_plane_source = None  # vtkPlaneSource
        self._cutting_plane_actor = None   # vtkActor
        self._cutting_plane_texture = None # vtkTexture (slice colors)
        self._cutting_plane_slice_img = None  # vtkImageData (current slice)
        self._cutting_plane_slice_colors = None  # vtkImageMapToColors
        self._slice_lut_ctf = None         # vtkColorTransferFunction for slice
        self._cutting_plane_slice_index = -1   # invalidates first update
        self._closed = False
        # Phase 4 Path 1 — background FITS reader. ``None`` on the
        # legacy synchronous path (small cubes < 256 MB); a CubeReader
        # instance for large cubes that fill in progressively.
        self._cube_reader = None
        # vtkFloatArray kept alive while ``_image_data`` is alive.
        # Necessary when ``numpy_to_vtk(deep=False)`` shares the shared
        # memory buffer — without this reference Python would reclaim
        # the array and VTK would dereference freed memory.
        self._vtk_intensity_array = None
        self._cube_flat_view = None
        # Counters surfaced in ``frame steps:`` log for the chunked
        # path so we can see at a glance how much of the cube has
        # landed.
        self._chunks_loaded = 0
        self._chunks_total  = 0
        # Throttle ``vtkImageData.Modified()`` calls. Each Modified()
        # forces the volume mapper to re-upload the FULL texture on
        # the next render — on a 10 GB cube that's a multi-second
        # GPU stall. If we called it after every chunk arrival the
        # render thread would freeze every few seconds. Once every
        # ~3 s is plenty for visible progress without choking the
        # executor.
        self._last_modified_wall = 0.0
        self._pending_modified = False
        # Volume hidden while the cube is still mostly empty (chunked
        # path only). Without this guard the user sees an opaque
        # solid-coloured rectangle for the first ~30 s — that's the
        # volume mapper colouring a sea of all-zero voxels with a
        # placeholder LUT, which looks like "the data is broken".
        # Revealed by ``_poll_cube_chunks`` once at least 25 % of the
        # z-slabs have landed AND the transfer functions have been
        # rebuilt with real vmin/vmax. Outline + axes stay visible
        # the whole time so the user always sees the cube's spatial
        # extent.
        self._volume_revealed = True   # default for legacy path
        # Scene-change IDR request. Set by apply_command for state
        # mutations that drastically change the rendered image (LUT,
        # blend mode, cutting plane position); consumed by the WebRTC
        # track's ``recv()`` to mark the next encoded frame as an
        # I-frame, sparing the client decoder from referencing across
        # an incompatible previous frame.
        self._idr_request_pending = False
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

    def request_idr(self) -> None:
        """Mark the next encoded video frame as an I-frame (IDR).

        Idempotent: multiple calls before the next ``recv()`` collapse
        to one IDR. Called from ``_apply_command_on_thread`` whenever a
        state mutation makes the scene incompatible with motion
        prediction from the previous frame.
        """
        self._idr_request_pending = True

    def consume_idr_flag(self) -> bool:
        """Atomic test-and-clear used by the WebRTC track."""
        if self._idr_request_pending:
            self._idr_request_pending = False
            return True
        return False

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

    async def render_rgb_frame(self):
        """Render the current view state and return ``(rgb_ndarray, render_ms)``.

        Same camera/scene path as :meth:`render_frame` but skips the
        JPEG encode step — the WebRTC transport feeds the raw numpy
        array straight into an ``av.VideoFrame`` for NVENC. Stats are
        updated in-place; the caller doesn't need to.
        """
        if self._closed:
            raise RuntimeError(f"Session {self.session_id} is closed.")
        loop = asyncio.get_event_loop()
        async with self._render_lock:
            rgb, render_ms = await loop.run_in_executor(
                self._executor, self._render_rgb_on_thread,
            )
        self.stats.frames_rendered += 1
        self.stats.last_render_ms = render_ms
        return rgb, render_ms

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

        Scene layout (matches the desktop ``vtkWindowCube`` 3D view):
        ::

            vtkRenderer
            ├── vtkVolume          (cube, mapped through LUT + threshold TF)
            ├── vtkActor outline   (cube bounding-box wireframe)
            ├── vtkAxesActor       (XYZ orientation marker, bottom-left)
            └── vtkActor plane     (cutting plane, textured with slice colors)
        """
        t0 = time.time()
        import vtk
        from astropy.io import fits as _fits
        from vtkmodules.vtkRenderingOpenGL2 import vtkEGLRenderWindow
        from vtk.util.numpy_support import numpy_to_vtk
        from .cube_reader import CubeReader

        # ── 1. Peek the FITS for cube shape WITHOUT loading data ─────
        # Phase 4 Path 1: the FITS read happens in a separate worker
        # process via CubeReader so this executor thread isn't blocked
        # for 2-3 minutes on a 22 GB cube. Here we just open the header
        # to discover (nz, ny, nx) so we can pre-allocate the shared
        # memory buffer that backs vtkImageData.
        with _fits.open(self.dataset_path, memmap=True) as hdul:
            data = hdul[0].data  # type: ignore[attr-defined]
            while data.ndim > 3:
                data = data[0]
            if data.ndim != 3:
                raise ValueError(
                    f"Volume rendering requires a 3-D cube, got shape {data.shape}"
                )
            disk_shape = data.shape   # (nz, ny, nx)
            nz, ny, nx = disk_shape
            cube_size_bytes = int(np.prod(disk_shape)) * 4   # float32

        # Threshold for the chunked path: cubes above this size load
        # progressively via CubeReader; smaller ones go through the
        # legacy synchronous path which is simpler and adds no extra
        # process. 256 MB (~256³ voxels) is the same threshold the
        # client uses for local-vs-remote rendering.
        chunked_threshold = 256 * 1024 * 1024
        use_chunked = cube_size_bytes > chunked_threshold

        # ── 2. Allocate the cube buffer + start reader if chunked ─────
        if use_chunked:
            # Pick a chunk schedule. For a 109-deep cube that's
            # max(1, 109//32) = 3 → ~36 chunks; for a 6668-deep cube
            # that's 208 → ~32 chunks. The render side calls
            # Modified() after every chunk, which costs O(cube_size)
            # in GPU re-upload; keeping the count low (~32) limits
            # the upload churn to once every ~3 % of the cube.
            self._cube_reader = CubeReader(
                self.dataset_path, shape=disk_shape,
            )
            cube = self._cube_reader.cube   # numpy view into shared mem
            self._chunks_total = len(self._cube_reader.chunks)
            # Hide volume until enough chunks are in (revealed by
            # ``_poll_cube_chunks``). Outline + axes stay visible so
            # the user has spatial context.
            self._volume_revealed = False
            self._cube_reader.start()
            logger.info(
                "[render-session %s] using chunked reader for %.2f GB cube "
                "(%d chunks scheduled)",
                self.session_id, cube_size_bytes / (1024**3),
                len(self._cube_reader.chunks),
            )
            # We don't know the real vmin/vmax yet — the reader posts
            # running estimates on the queue. Start with placeholders;
            # the per-frame chunk poll updates these and rebuilds the
            # transfer functions as more data arrives.
            vmin, vmax = 0.0, 1.0
            finite_mask = np.ones(disk_shape, dtype=bool)  # placeholder
            # Reader chunks have already had NaN replaced by 0; no
            # second pass needed here.
        else:
            # Legacy synchronous path — fast for small cubes (under
            # 256 MB), keeps the previous behaviour byte-identical so
            # WALLABY-class datasets don't regress.
            self._cube_reader = None
            with _fits.open(self.dataset_path, memmap=True) as hdul:
                data = hdul[0].data
                while data.ndim > 3:
                    data = data[0]
                cube = np.asarray(data, dtype=np.float32)
                finite_mask = np.isfinite(cube)
                if not finite_mask.all():
                    fmin = float(cube[finite_mask].min()) if finite_mask.any() else 0.0
                    cube = np.where(finite_mask, cube, fmin)
                vmin = float(np.nanmin(cube))
                vmax = float(np.nanmax(cube))

        self._cube_np = cube
        self._nx, self._ny, self._nz = nx, ny, nz
        self._vmin, self._vmax = vmin, vmax
        # On the chunked path, the percentile-based threshold and
        # slice LUT range are deferred to ``_poll_cube_chunks`` once
        # data has actually landed. Sorting 2.5 billion zero-filled
        # voxels (a 10 GB cube on the chunked path) takes minutes and
        # would defeat the whole point of the background reader.
        if use_chunked:
            # Use placeholders matching the legacy code's branches —
            # ``_poll_cube_chunks`` overrides on the ``complete`` event
            # with real p1/p99 from the actual data.
            if self.view.threshold is None:
                self.view.threshold = 0.0
            self._slice_vmin = 0.0
            self._slice_vmax = 1.0
        self.view.vmin = vmin if self.view.vmin is None else self.view.vmin
        self.view.vmax = vmax if self.view.vmax is None else self.view.vmax
        # Default threshold = 99th percentile. HI cubes have ~99 % of
        # voxels in the Gaussian noise band around zero with maybe 1 %
        # in the emission tail; p99 puts the cutoff right at the edge
        # of noise so the FIRST frame already shows only the emission.
        # Without this default the initial render before the client
        # pushes its slider value looks like a solid noise block —
        # vmin + 3 % span lets the noise voxels render fully opaque.
        # Percentile-based threshold + slice LUT range are ONLY
        # computed on the legacy synchronous path. The chunked path
        # already set placeholders above and defers the real values
        # to ``_poll_cube_chunks``'s ``complete`` handler — running
        # ``np.percentile`` on a 10 GB zero-filled buffer takes
        # minutes and blocks the whole executor thread, defeating
        # the point of the background reader.
        if not use_chunked:
            if self.view.threshold is None:
                finite = cube[finite_mask] if not finite_mask.all() else cube
                self.view.threshold = float(np.percentile(finite, 99.0))
            # Slice-LUT range: the cube's vmin/vmax is dominated by a
            # few outlier voxels (galaxy peaks, edge artefacts), so
            # mapping the slice through the full [vmin, vmax] makes
            # the noise occupy only the bottom ~5% of the LUT —
            # that's the dark-purple end of Inferno, which then looks
            # "all-black" against the desktop's bright-orange slice.
            # The desktop builds ``lutSlice`` over the SLICE's own
            # scalar range (which excludes outliers naturally), so we
            # match that by using cube percentiles p1..p99 — robust
            # to the bright-tail outliers, close to what the desktop
            # sees when it samples a typical channel.
            finite = cube[finite_mask] if not finite_mask.all() else cube
            self._slice_vmin = float(np.percentile(finite, 1.0))
            self._slice_vmax = float(np.percentile(finite, 99.0))
        # Cutting plane defaults to the middle channel — matches the
        # local viewer's initial state. Cheap on either path.
        if self.view.cutting_plane_slice_index <= 0:
            self.view.cutting_plane_slice_index = nz // 2

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
        # Chunked path uses ``deep=False`` so the vtkFloatArray points
        # AT the shared-memory buffer the reader process is writing
        # into — zero-copy. Legacy path keeps the original ``deep=True``
        # so the cube can be released after this scope (CubeReader owns
        # the cube buffer in the chunked path; lifetime is bound to the
        # CubeReader).
        if use_chunked:
            arr = numpy_to_vtk(flat, deep=False, array_type=vtk.VTK_FLOAT)
        else:
            arr = numpy_to_vtk(flat, deep=True, array_type=vtk.VTK_FLOAT)
        arr.SetName("intensity")
        # Keep ``arr`` alive while ``self._image_data`` is alive. With
        # ``deep=False`` the underlying buffer is owned by ``cube`` /
        # ``CubeReader.shm``; ``arr`` only holds a weak reference and
        # would be reclaimed when this scope ends.
        self._vtk_intensity_array = arr
        # Same for the flat view — keep a reference so numpy doesn't
        # garbage-collect it out from under VTK.
        self._cube_flat_view = flat
        img.GetPointData().SetScalars(arr)
        self._image_data = img

        # ── 3. Transfer functions — driven by view state ─────────────
        self._color_tf   = _build_color_tf(self.view.volume_lut_name, vmin, vmax)
        self._opacity_tf = _build_opacity_tf(vmin, vmax, self.view.threshold)

        prop = vtk.vtkVolumeProperty()
        prop.SetColor(self._color_tf)
        prop.SetScalarOpacity(self._opacity_tf)
        prop.SetInterpolationTypeToLinear()
        prop.ShadeOff()
        prop.SetScalarOpacityUnitDistance(1.0)
        self._volume_property = prop

        # ── 4. Mapper + actor ────────────────────────────────────────
        # Match the desktop's ``setupCubeRenderer`` mapper exactly:
        # vtkGPUVolumeRayCastMapper (NOT vtkSmartVolumeMapper — the
        # smart variant picks an internal renderer that's close but
        # not byte-identical), plus AutoAdjustSampleDistances and
        # UseJittering. The jittering is what gives the local volume
        # its "denser, more luminous" feel: it dithers the sample
        # positions per pixel so opacity accumulates more uniformly
        # along the ray, no banding, no per-voxel undersampling.
        self._volume_mapper = vtk.vtkGPUVolumeRayCastMapper()
        self._volume_mapper.SetInputData(img)
        self._volume_mapper.SetAutoAdjustSampleDistances(1)
        self._volume_mapper.SetUseJittering(1)
        self._apply_blend_mode_on_thread(self.view.blend_mode)

        self._volume = vtk.vtkVolume()
        self._volume.SetMapper(self._volume_mapper)
        self._volume.SetProperty(prop)
        # Chunked path starts with the volume hidden — the outline +
        # axes are visible immediately so the user has spatial context,
        # but the volume only appears after enough chunks have landed
        # (revealed in ``_poll_cube_chunks``). Without this guard the
        # opaque solid-coloured block of all-zero voxels with a
        # placeholder LUT looks like a broken render.
        if not self._volume_revealed:
            self._volume.SetVisibility(0)

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

        # Bounding box wireframe + XYZ orientation marker — built
        # separately so set_show_outline / set_show_axes can toggle
        # visibility without touching the volume.
        self._build_outline_actor_on_thread()
        self._build_axes_actor_on_thread()
        # Isosurface (vtkFlyingEdges3D mesh at the threshold iso value)
        # — built hidden; ``set_cube_render_mode`` toggles between the
        # volume actor and this one.
        self._build_isosurface_on_thread()
        # Low-res "galaxy wireframe" iso for interactive LOD. Built
        # from a 4x-downsampled volume so it renders in <1 ms while
        # still showing the rough shape of the emission as the user
        # rotates. Shown only during set_interactive=True.
        self._build_iso_lod_on_thread()
        # Cutting plane is the heaviest piece: it needs a slice LUT,
        # an image-to-colours pipeline, a texture, and an actor.
        self._build_cutting_plane_on_thread()

        self._render_window = vtkEGLRenderWindow()
        self._render_window.SetOffScreenRendering(1)
        self._render_window.SetSize(self.view.width, self.view.height)
        if hasattr(self._render_window, "SetDeviceIndex"):
            # Round-robin per-session GPU assignment would go here; for
            # now everybody on device 0. Multi-GPU pinning is Phase 2.
            self._render_window.SetDeviceIndex(0)
        # Two layered renderers: 0=main scene, 1=corner-axes triad.
        self._render_window.SetNumberOfLayers(2)
        self._renderer.SetLayer(0)
        self._render_window.AddRenderer(self._renderer)
        if self._axes_renderer is not None:
            self._render_window.AddRenderer(self._axes_renderer)

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

    # ── Scene-building helpers (worker thread only) ──────────────────────

    def _build_outline_actor_on_thread(self) -> None:
        """Build the cube bounding-box wireframe actor (always white)."""
        import vtk
        outline = vtk.vtkOutlineFilter()
        outline.SetInputData(self._image_data)
        mapper = vtk.vtkPolyDataMapper()
        mapper.SetInputConnection(outline.GetOutputPort())
        actor = vtk.vtkActor()
        actor.SetMapper(mapper)
        actor.GetProperty().SetColor(0.85, 0.85, 0.85)
        actor.GetProperty().SetLineWidth(1.5)
        actor.SetVisibility(1 if self.view.show_outline else 0)
        self._outline_actor = actor
        self._renderer.AddActor(actor)

    def _build_axes_actor_on_thread(self) -> None:
        """Build the XYZ orientation triad in a corner viewport.

        The desktop uses a ``vtkOrientationMarkerWidget`` which needs a
        live interactor — we don't have one offscreen, so we replicate
        its effect with a second ``vtkRenderer`` confined to the
        bottom-left corner of the render window via ``SetViewport``.
        That corner renderer holds ONLY a ``vtkAxesActor`` and its
        camera is synced to the main camera's orientation each frame
        in ``_render_frame_on_thread`` — so the triad rotates with the
        scene but stays anchored to the corner of the viewport
        regardless of zoom or pan.
        """
        import vtk
        axes = vtk.vtkAxesActor()
        axes.SetTotalLength(1.0, 1.0, 1.0)
        axes.SetShaftTypeToCylinder()
        axes.SetCylinderRadius(0.02)
        # Keep the X/Y/Z labels small and crisp — without an interactor
        # the default text scaling produces enormous letters.
        for cap in (axes.GetXAxisCaptionActor2D(),
                    axes.GetYAxisCaptionActor2D(),
                    axes.GetZAxisCaptionActor2D()):
            cap.GetTextActor().SetTextScaleModeToNone()
            cap.GetTextActor().GetTextProperty().SetFontSize(12)
        axes.SetVisibility(1 if self.view.show_axes else 0)
        self._axes_actor = axes

        # Corner viewport renderer. Layered ABOVE the main scene so the
        # axes are drawn last and not clipped by the volume's depth
        # buffer. ``InteractiveOff()`` keeps the offscreen pipeline from
        # trying to grab a non-existent interactor.
        axes_ren = vtk.vtkRenderer()
        # Bottom-left corner, ~18 % wide × ~22 % tall — matches the
        # desktop's vtkOrientationMarkerWidget default size.
        axes_ren.SetViewport(0.0, 0.0, 0.18, 0.22)
        axes_ren.SetBackground(0.0, 0.0, 0.0)
        axes_ren.SetLayer(1)              # above the main scene
        axes_ren.InteractiveOff()
        axes_ren.AddActor(axes)
        self._axes_renderer = axes_ren

    def _build_isosurface_on_thread(self) -> None:
        """Build an isosurface mesh extractor + actor, initially hidden.

        Matches the desktop ``vtkFlyingEdges3D`` → ``vtkPolyDataMapper``
        → ``vtkActor`` chain in ``setupCubeRenderer``. ``SetValue(0,
        threshold)`` is wired at build time and refreshed by
        ``_rebuild_isosurface_value_on_thread`` when ``set_threshold``
        arrives. Mapper colour is fed from the same colour TF as the
        volume so the LUT picker affects both modes.
        """
        import vtk
        flying = vtk.vtkFlyingEdges3D()
        flying.SetInputData(self._image_data)
        flying.SetNumberOfContours(1)
        flying.SetValue(0, self.view.threshold or self._vmin)
        flying.ComputeNormalsOn()
        flying.ComputeGradientsOff()
        flying.ComputeScalarsOn()
        mapper = vtk.vtkPolyDataMapper()
        mapper.SetInputConnection(flying.GetOutputPort())
        mapper.SetLookupTable(self._color_tf)
        mapper.SetScalarRange(self._vmin, self._vmax)
        mapper.ScalarVisibilityOn()
        actor = vtk.vtkActor()
        actor.SetMapper(mapper)
        actor.SetVisibility(1 if self.view.cube_render_mode == "isosurface" else 0)
        self._iso_filter = flying
        self._iso_mapper = mapper
        self._iso_actor  = actor
        self._renderer.AddActor(actor)
        # And start the volume in the right visibility too.
        if self._volume is not None:
            self._volume.SetVisibility(
                    1 if self.view.cube_render_mode == "volume" else 0)

    def _rebuild_isosurface_value_on_thread(self) -> None:
        """Push the current threshold into BOTH the full and LOD iso filters."""
        if self._iso_filter is not None:
            self._iso_filter.SetValue(0, self.view.threshold or self._vmin)
            self._iso_filter.Modified()
        if self._iso_lod_filter is not None:
            self._iso_lod_filter.SetValue(0, self.view.threshold or self._vmin)
            self._iso_lod_filter.Modified()

    def _build_iso_lod_on_thread(self) -> None:
        """Build a low-poly isosurface for the interactive LOD path.

        Pipeline:
        ::

            vtkImageData (full-res cube)
                ↓ vtkImageShrink3D (4× per axis = 1/64 voxels)
            vtkImageData (downsampled)
                ↓ vtkFlyingEdges3D
            vtkPolyData (sparse iso mesh)
                ↓ vtkPolyDataMapper (lighting off, wireframe representation)
            vtkActor (hidden by default)

        Why a downsampled volume instead of decimating the full iso:
        ``vtkDecimatePro`` runs on the polygons AFTER iso extraction;
        on a several-hundred-thousand-triangle mesh it's slow at
        threshold-change time (we'd block apply_command for ~200 ms
        each ``set_threshold``). Shrinking the source volume first
        means FlyingEdges3D produces a sparse mesh directly — sub-ms
        on a 27×70×49 grid.

        The mesh shows the rough silhouette of the emission, which is
        exactly what the user asked for ("wireframe della galassia"):
        rotation feels responsive AND keeps spatial reasoning intact.
        On release the full-quality volume comes back, IDR'd to a
        clean keyframe.
        """
        if self._image_data is None:
            return
        import vtk
        shrink = vtk.vtkImageShrink3D()
        shrink.SetInputData(self._image_data)
        shrink.SetShrinkFactors(4, 4, 4)
        shrink.AveragingOn()  # average voxels (vs nearest) — preserves
                              # the iso shape when the threshold sits
                              # near the noise tail of the cube
        flying = vtk.vtkFlyingEdges3D()
        flying.SetInputConnection(shrink.GetOutputPort())
        flying.SetNumberOfContours(1)
        flying.SetValue(0, self.view.threshold or self._vmin)
        flying.ComputeNormalsOff()    # not needed for wireframe
        flying.ComputeGradientsOff()
        flying.ComputeScalarsOff()
        mapper = vtk.vtkPolyDataMapper()
        mapper.SetInputConnection(flying.GetOutputPort())
        mapper.ScalarVisibilityOff()
        actor = vtk.vtkActor()
        actor.SetMapper(mapper)
        prop = actor.GetProperty()
        # Wireframe-only, lighting off — the wireframe should look the
        # same colour regardless of camera angle so the user can
        # always see it against the dark background.
        prop.SetRepresentationToWireframe()
        prop.SetColor(1.0, 0.6, 0.2)   # Inferno-ish orange, contrasts
                                        # well with both light and dark
                                        # backgrounds
        prop.LightingOff()
        prop.SetLineWidth(1.0)
        actor.SetVisibility(0)
        self._iso_lod_shrink = shrink
        self._iso_lod_filter = flying
        self._iso_lod_mapper = mapper
        self._iso_lod_actor  = actor
        self._renderer.AddActor(actor)

    def _apply_interactive_mode_on_thread(self, interactive: bool) -> None:
        """Toggle the LOD between full-quality and a fast wireframe.

        Full quality (interactive=False):
            volume mapper OR isosurface mesh (per cube_render_mode) +
            cutting plane (if visible) + outline + axes triad.
            Render window at the client's requested size (e.g. 1586×1244
            on a Retina dock).

        Interactive (interactive=True), three combined tricks:

          1. Show only the bounding-box outline (forced bright/thick)
             plus a LOW-POLY iso wireframe of the emission ("galaxy
             wireframe"). Everything else — volume, full iso, cutting
             plane, axes triad — is hidden. VTK ``Render()`` drops to
             ~2 ms instead of ~10 ms.

          2. Halve the render window dimensions. Every downstream
             cost is proportional to pixels: readback at 1586×1244
             takes ~10 ms; halved (793×622) it takes ~3 ms. NVENC
             encode on the smaller frame is similarly cheaper, and
             the H.264 stream over the SSH tunnel is smaller. The
             Qt client paints with ``QImage::scaled(...,
             SmoothTransformation)`` so the upscale is automatic.
             The downside is a brief blurry frame at release before
             the full-res IDR lands; in practice the eye doesn't
             notice during the rotation itself.

          3. Hide the axes triad. The corner viewport renderer is
             cheap individually but its extra Render() pass + its
             scalar-bar text take a few ms that we can spend on
             frames-per-second instead.

        Combined the LOD frame budget at WALLABY scale is roughly
        ~6 ms total = 60+ FPS sustained, vs ~22 ms at full quality.
        Release triggers an IDR so the client sees the full-quality
        still frame at the right orientation immediately.
        """
        if interactive:
            if self._volume is not None:
                self._volume.SetVisibility(0)
            if self._iso_actor is not None:
                self._iso_actor.SetVisibility(0)
            if self._cutting_plane_actor is not None:
                self._cutting_plane_actor.SetVisibility(0)
            # Bright outline so the cube box reads against the dark
            # background while it's almost the only thing on screen.
            if self._outline_actor is not None:
                self._outline_actor.SetVisibility(1)
                prop = self._outline_actor.GetProperty()
                prop.SetColor(1.0, 1.0, 1.0)
                prop.SetLineWidth(2.0)
            # Galaxy wireframe (low-poly iso) — gives spatial context
            # without the cost of the full iso mesh.
            if self._iso_lod_actor is not None:
                self._iso_lod_actor.SetVisibility(1)
            # Hide axes triad to save the extra renderer pass.
            if self._axes_renderer is not None:
                self._axes_renderer.SetDraw(0)
            # Halve render-window dimensions — 4× fewer pixels means
            # 4× faster readback + encode. Cache the previous size so
            # we can restore it on release.
            if self._render_window is not None:
                cur_w, cur_h = self._render_window.GetSize()
                self._lod_prev_size = (cur_w, cur_h)
                lod_w = max(64, cur_w // 2)
                lod_h = max(64, cur_h // 2)
                self._render_window.SetSize(lod_w, lod_h)
                self.view.width = lod_w
                self.view.height = lod_h
        else:
            # Restore outline cosmetics, hide LOD iso, restore axes,
            # restore render-window size, then apply normal visibility
            # per cube_render_mode + cutting_plane_visible.
            if self._outline_actor is not None:
                self._outline_actor.SetVisibility(1 if self.view.show_outline else 0)
                prop = self._outline_actor.GetProperty()
                prop.SetColor(0.85, 0.85, 0.85)
                prop.SetLineWidth(1.5)
            if self._iso_lod_actor is not None:
                self._iso_lod_actor.SetVisibility(0)
            if self._axes_renderer is not None:
                self._axes_renderer.SetDraw(1 if self.view.show_axes else 0)
            if self._render_window is not None and self._lod_prev_size is not None:
                w, h = self._lod_prev_size
                self._render_window.SetSize(w, h)
                self.view.width = w
                self.view.height = h
                self._lod_prev_size = None
            is_iso = (self.view.cube_render_mode == "isosurface")
            if self._volume is not None:
                self._volume.SetVisibility(0 if is_iso else 1)
            if self._iso_actor is not None:
                self._iso_actor.SetVisibility(1 if is_iso else 0)
            if self._cutting_plane_actor is not None:
                self._cutting_plane_actor.SetVisibility(
                    1 if self.view.cutting_plane_visible else 0)

    def _apply_cube_render_mode_on_thread(self, mode: str) -> None:
        """Toggle between volume and isosurface actors atomically."""
        is_iso = (mode == "isosurface")
        if self._volume is not None:
            self._volume.SetVisibility(0 if is_iso else 1)
        if self._iso_actor is not None:
            self._iso_actor.SetVisibility(1 if is_iso else 0)

    def _build_cutting_plane_on_thread(self) -> None:
        """Build the textured cutting plane (slice colors over a vtkPlaneSource).

        Pipeline mirrors the desktop's:
        ::

            vtkImageData (current slice, 2D)
                ↓
            vtkImageMapToColors (slice LUT applied)
                ↓
            vtkTexture
                ↓
            vtkActor (vtkPlaneSource geometry positioned at z = slice_index)

        The slice LUT is built lazily because it depends on
        ``view.slice_lut_name`` which the client may change. The image
        data is filled by ``_update_cutting_plane_slice``.
        """
        import vtk

        # Slice LUT — built over the SLICE-typical range (p1..p99 of
        # the cube), not the full volume range. See _init_on_thread
        # for the why.
        self._slice_lut_ctf = _build_color_tf(
            self.view.slice_lut_name, self._slice_vmin, self._slice_vmax,
        )

        # vtkImageMapToColors expects a vtkScalarsToColors (vtkLookupTable
        # or vtkColorTransferFunction). Use the CTF directly.
        slice_img = vtk.vtkImageData()
        slice_img.SetDimensions(self._nx, self._ny, 1)
        slice_img.SetSpacing(1.0, 1.0, 1.0)
        slice_img.SetOrigin(0.0, 0.0, 0.0)
        # Place a zero-filled scalar array so the first call to Update()
        # in the texture pipeline doesn't crash before the slice index
        # is set. The first frame's render path calls
        # _update_cutting_plane_slice(slice_index) which replaces this.
        from vtk.util.numpy_support import numpy_to_vtk
        zeros = np.zeros((self._ny * self._nx,), dtype=np.float32)
        arr = numpy_to_vtk(zeros, deep=True, array_type=vtk.VTK_FLOAT)
        arr.SetName("slice")
        slice_img.GetPointData().SetScalars(arr)
        self._cutting_plane_slice_img = slice_img

        slice_colors = vtk.vtkImageMapToColors()
        slice_colors.SetInputData(slice_img)
        slice_colors.SetLookupTable(self._slice_lut_ctf)
        slice_colors.SetOutputFormatToRGBA()
        self._cutting_plane_slice_colors = slice_colors

        texture = vtk.vtkTexture()
        texture.SetInputConnection(slice_colors.GetOutputPort())
        texture.InterpolateOn()
        texture.RepeatOff()
        self._cutting_plane_texture = texture

        plane = vtk.vtkPlaneSource()
        # XY plane at z = slice_index, spanning the full cube footprint.
        # Position is refreshed by _update_cutting_plane_slice.
        plane.SetOrigin(0.0, 0.0, 0.0)
        plane.SetPoint1(float(self._nx), 0.0, 0.0)
        plane.SetPoint2(0.0, float(self._ny), 0.0)
        self._cutting_plane_source = plane

        mapper = vtk.vtkPolyDataMapper()
        mapper.SetInputConnection(plane.GetOutputPort())
        actor = vtk.vtkActor()
        actor.SetMapper(mapper)
        actor.SetTexture(texture)
        # Match the desktop ``setupCubeRenderer`` plane-actor settings
        # byte-for-byte: white base colour (so the texture's RGB values
        # pass through unmodulated), 50 % blend, red edges so the plane
        # is visible against the volume even when the slice is mostly
        # noise.
        prop = actor.GetProperty()
        prop.SetColor(1.0, 1.0, 1.0)
        prop.SetOpacity(self.view.cutting_plane_opacity)
        prop.EdgeVisibilityOn()
        prop.SetEdgeColor(1.0, 0.0, 0.0)
        prop.SetLineWidth(3.0)
        prop.LightingOff()
        prop.BackfaceCullingOff()
        prop.FrontfaceCullingOff()
        actor.SetVisibility(1 if self.view.cutting_plane_visible else 0)
        self._cutting_plane_actor = actor
        self._renderer.AddActor(actor)

        # Fill the slice with the initial channel — also moves the plane
        # to that z. Cheap (one numpy copy on a 2D slice).
        self._update_cutting_plane_slice_on_thread(self.view.cutting_plane_slice_index)

    def _update_cutting_plane_slice_on_thread(self, slice_index: int) -> None:
        """Refill the slice image with cube[z] and reposition the plane.

        ``slice_index`` is clamped to ``[0, nz-1]`` so a stale client
        index (e.g. after a resize that changed the channel range)
        never indexes out of bounds.
        """
        if self._cube_np is None or self._cutting_plane_slice_img is None:
            return
        z = max(0, min(self._nz - 1, int(slice_index)))
        if z == self._cutting_plane_slice_index:
            return   # idempotent
        self._cutting_plane_slice_index = z

        import vtk
        from vtk.util.numpy_support import numpy_to_vtk
        slice2d = self._cube_np[z]  # (ny, nx)
        flat = slice2d.ravel(order="C").astype(np.float32, copy=False)
        arr = numpy_to_vtk(flat, deep=True, array_type=vtk.VTK_FLOAT)
        arr.SetName("slice")
        self._cutting_plane_slice_img.GetPointData().SetScalars(arr)
        self._cutting_plane_slice_img.Modified()
        if self._cutting_plane_slice_colors is not None:
            self._cutting_plane_slice_colors.Modified()

        # Re-position the plane at z=slice_index in world space.
        if self._cutting_plane_source is not None:
            zf = float(z)
            self._cutting_plane_source.SetOrigin(0.0, 0.0, zf)
            self._cutting_plane_source.SetPoint1(float(self._nx), 0.0, zf)
            self._cutting_plane_source.SetPoint2(0.0, float(self._ny), zf)
            self._cutting_plane_source.Modified()

    def _apply_blend_mode_on_thread(self, blend_mode: int) -> None:
        """Set the volume mapper's blend mode (0=Composite, 1=MIP, 2=MinIP)."""
        if self._volume_mapper is None:
            return
        if blend_mode == 1:
            self._volume_mapper.SetBlendModeToMaximumIntensity()
        elif blend_mode == 2:
            self._volume_mapper.SetBlendModeToMinimumIntensity()
        else:
            self._volume_mapper.SetBlendModeToComposite()

    def _rebuild_volume_tfs_on_thread(self) -> None:
        """Rebuild color + opacity TFs from current view state.

        Also pushes the new colour TF into the isosurface mapper so
        the isosurface colour follows the LUT picker even when the
        user is viewing the volume mode and never explicitly toggles
        modes — first switch will look right.
        """
        if self._volume_property is None:
            return
        self._color_tf = _build_color_tf(
            self.view.volume_lut_name, self._vmin, self._vmax,
        )
        self._opacity_tf = _build_opacity_tf(
            self._vmin, self._vmax, self.view.threshold or self._vmin,
        )
        self._volume_property.SetColor(self._color_tf)
        self._volume_property.SetScalarOpacity(self._opacity_tf)
        if self._iso_mapper is not None:
            self._iso_mapper.SetLookupTable(self._color_tf)
        self._rebuild_isosurface_value_on_thread()

    def _rebuild_slice_lut_on_thread(self) -> None:
        """Rebuild the cutting plane's slice LUT from current view state."""
        if self._cutting_plane_slice_colors is None:
            return
        self._slice_lut_ctf = _build_color_tf(
            self.view.slice_lut_name, self._slice_vmin, self._slice_vmax,
        )
        self._cutting_plane_slice_colors.SetLookupTable(self._slice_lut_ctf)
        self._cutting_plane_slice_colors.Modified()

    def _apply_command_on_thread(self, command: dict[str, Any]) -> None:
        """Apply one client command to ``self.view``.

        Camera / interaction commands:

        ``{"type": "rotate", "d_azimuth": float, "d_elevation": float}``
        ``{"type": "zoom",   "factor":    float}``
        ``{"type": "pan",    "dx": float, "dy": float}``
        ``{"type": "reset"}``

        Volume rendering commands — these mutate VTK state on the
        worker thread so the next ``render_frame`` reflects the new
        look immediately:

        ``{"type": "set_lut",            "name": str}``         — volume LUT
        ``{"type": "set_slice_lut",      "name": str}``         — cutting-plane LUT
        ``{"type": "set_threshold",      "value": float}``      — opacity ramp anchor
        ``{"type": "set_blend_mode",     "mode":  int}``        — 0/1/2
        ``{"type": "set_cutting_plane",  "visible": bool,
                                          "opacity": float,
                                          "slice_index": int}`` — partial; any subset
        ``{"type": "set_show_outline",   "visible": bool}``
        ``{"type": "set_show_axes",      "visible": bool}``

        Viewport / encoding:

        ``{"type": "resize",       "width": int, "height": int}``
        ``{"type": "jpeg_quality", "quality": int}``           — 1..100

        Legacy (kept for backward compat with the spike client):

        ``{"type": "colormap", "name": str, ...}`` → alias of ``set_lut``
        ``{"type": "opacity",  "scale": float}``  → ignored (use set_threshold)

        Unknown commands are logged and ignored — the client may be
        newer than the server.
        """
        ctype = command.get("type", "")

        # ── Camera / interaction ─────────────────────────────────────
        # All camera mutations go directly to vtkCamera so the next
        # render frame just snapshots. We deliberately do NOT keep
        # view.azimuth/elevation/zoom in sync — those fields are dead
        # state now and only used by `reset` below.
        if ctype == "rotate":
            if self._renderer is not None:
                cam = self._renderer.GetActiveCamera()
                cam.Azimuth(float(command.get("d_azimuth", 0.0)))
                cam.Elevation(float(command.get("d_elevation", 0.0)))
                # OrthogonalizeViewUp prevents view-up drift that
                # accumulates after repeated Elevation calls — same
                # behaviour as vtkInteractorStyleTrackballCamera.
                cam.OrthogonalizeViewUp()
        elif ctype == "zoom":
            if self._renderer is not None:
                factor = max(0.05, min(20.0, float(command.get("factor", 1.0))))
                self._renderer.GetActiveCamera().Zoom(factor)
        elif ctype == "pan":
            # Pan via Dolly is not quite the right semantic but matches
            # what the spike has; pan in screen-aligned space requires
            # the focal-plane projection — TODO when needed.
            self.view.pan_x += float(command.get("dx", 0.0))
            self.view.pan_y += float(command.get("dy", 0.0))
        elif ctype == "reset":
            keep_w, keep_h = self.view.width, self.view.height
            self.view = SessionViewState(width=keep_w, height=keep_h,
                                          vmin=self._vmin, vmax=self._vmax,
                                          threshold=self._vmin + 0.03 * (self._vmax - self._vmin),
                                          cutting_plane_slice_index=self._nz // 2)
            self._rebuild_volume_tfs_on_thread()
            self._rebuild_slice_lut_on_thread()
            self._apply_blend_mode_on_thread(self.view.blend_mode)
            if self._outline_actor is not None:
                self._outline_actor.SetVisibility(1 if self.view.show_outline else 0)
            if self._axes_actor is not None:
                self._axes_actor.SetVisibility(1 if self.view.show_axes else 0)
            if self._cutting_plane_actor is not None:
                self._cutting_plane_actor.SetVisibility(
                    1 if self.view.cutting_plane_visible else 0)
                self._cutting_plane_actor.GetProperty().SetOpacity(
                    self.view.cutting_plane_opacity)
            self._update_cutting_plane_slice_on_thread(
                self.view.cutting_plane_slice_index)
            # Camera back to the cube-fitting default.
            if self._renderer is not None:
                self._renderer.ResetCamera()

        # ── Volume LUT / opacity / blend ─────────────────────────────
        elif ctype in ("set_lut", "colormap"):
            self.view.volume_lut_name = str(command.get("name", self.view.volume_lut_name))
            self._rebuild_volume_tfs_on_thread()
            self.request_idr()
        elif ctype == "set_slice_lut":
            self.view.slice_lut_name = str(command.get("name", self.view.slice_lut_name))
            self._rebuild_slice_lut_on_thread()
            self.request_idr()
        elif ctype == "set_threshold":
            self.view.threshold = float(command.get("value", self.view.threshold or self._vmin))
            self._rebuild_volume_tfs_on_thread()
            self.request_idr()
        elif ctype == "set_blend_mode":
            self.view.blend_mode = int(command.get("mode", 0))
            self._apply_blend_mode_on_thread(self.view.blend_mode)
            self.request_idr()
        elif ctype == "set_cube_render_mode":
            mode = str(command.get("mode", "volume")).lower()
            if mode not in ("volume", "isosurface"):
                mode = "volume"
            self.view.cube_render_mode = mode
            self._apply_cube_render_mode_on_thread(mode)
            self.request_idr()
        elif ctype == "set_interactive":
            new_state = bool(command.get("value", False))
            logger.info(
                "[render-session %s] set_interactive value=%s (current=%s)",
                self.session_id, new_state, self.view.interactive,
            )
            if new_state != self.view.interactive:
                self.view.interactive = new_state
                self._apply_interactive_mode_on_thread(new_state)
                # IDR on BOTH transitions: entering wireframe mode the
                # decoder needs a clean ref, and leaving it the full-quality
                # frame is visually a totally different image.
                self.request_idr()
        elif ctype == "opacity":
            # Legacy spike command — no-op for new clients; honoured
            # only if they really push it.
            self.view.opacity_scale = max(0.0, min(1.0, float(command.get("scale", 0.5))))

        # ── Cutting plane ────────────────────────────────────────────
        elif ctype == "set_cutting_plane":
            if "visible" in command:
                self.view.cutting_plane_visible = bool(command["visible"])
                if self._cutting_plane_actor is not None:
                    self._cutting_plane_actor.SetVisibility(
                        1 if self.view.cutting_plane_visible else 0)
            if "opacity" in command:
                self.view.cutting_plane_opacity = max(0.0, min(1.0,
                        float(command["opacity"])))
                if self._cutting_plane_actor is not None:
                    self._cutting_plane_actor.GetProperty().SetOpacity(
                            self.view.cutting_plane_opacity)
            if "slice_index" in command:
                idx = int(command["slice_index"])
                self.view.cutting_plane_slice_index = idx
                # Diagnostic: log the requested vs realised slice index
                # along with whether the plane infrastructure is ready.
                # The bootstrap can race the cube load — if cube_np /
                # slice_img is still None the update silently no-ops and
                # the user sees the build-time default (nz//2, center)
                # frozen on screen. The log makes that race visible.
                ready = (self._cube_np is not None
                         and self._cutting_plane_slice_img is not None)
                logger.info(
                    "[render-session %s] set_cutting_plane slice_index=%d "
                    "(infra ready=%s, current=%d)",
                    self.session_id, idx, ready,
                    self._cutting_plane_slice_index,
                )
                self._update_cutting_plane_slice_on_thread(idx)
            self.request_idr()

        # ── Chrome toggles ───────────────────────────────────────────
        elif ctype == "set_show_outline":
            self.view.show_outline = bool(command.get("visible", True))
            if self._outline_actor is not None:
                self._outline_actor.SetVisibility(1 if self.view.show_outline else 0)
        elif ctype == "set_show_axes":
            self.view.show_axes = bool(command.get("visible", True))
            if self._axes_actor is not None:
                self._axes_actor.SetVisibility(1 if self.view.show_axes else 0)

        # ── Viewport / encoding ──────────────────────────────────────
        elif ctype == "resize":
            w = max(64,  min(4096, int(command.get("width",  self.view.width))))
            h = max(64,  min(4096, int(command.get("height", self.view.height))))
            if self.view.interactive and self._lod_prev_size is not None:
                # We're in half-res LOD mode. Store the new full-res
                # target so the LOD-exit restore picks it up; don't
                # actually resize the render window now (the user is
                # still dragging, doesn't need the extra resolution).
                self._lod_prev_size = (w, h)
            else:
                self.view.width, self.view.height = w, h
                if self._render_window is not None:
                    self._render_window.SetSize(w, h)
        elif ctype == "jpeg_quality":
            self.view.jpeg_quality = max(1, min(100, int(command.get("quality", 85))))

        elif ctype == "keepalive":
            pass
        else:
            logger.warning("[render-session %s] unknown command type %r",
                           self.session_id, ctype)

    def _poll_cube_chunks(self) -> None:
        """Drain reader-process progress events; update VTK accordingly.

        Called at the top of every render pass when the chunked load
        path is in use (``self._cube_reader is not None``). Cheap
        when nothing new has landed.

        Effects per ``chunk`` event:
          * vmin/vmax tracking (running min/max across chunks)
          * ``vtkImageData.Modified()`` so the next ``Render()``
            re-uploads the volume texture (VTK observes the change
            via its modification-time machinery)
          * Counters for the per-second timing log

        On ``complete``:
          * Rebuild transfer functions with the final vmin/vmax
            (initial chunks may have been rendered with placeholder
            colour mapping)
          * Force an IDR so the H.264 stream cuts cleanly to the
            full-quality cube
        """
        if self._cube_reader is None:
            return
        events = self._cube_reader.poll_progress()
        if not events:
            return
        any_chunk = False
        complete = False
        for ev in events:
            kind = ev.get("event")
            if kind == "chunk":
                any_chunk = True
                self._chunks_loaded += 1
                v_lo = ev.get("vmin", float("inf"))
                v_hi = ev.get("vmax", float("-inf"))
                # Track the running global min/max so transfer
                # functions can be rebuilt with progressively-better
                # estimates. We DON'T rebuild every chunk (expensive
                # vtkPiecewiseFunction reconstruction); the per-second
                # log line shows when we cross significant boundaries
                # and ``complete`` handles the final accurate pass.
                if v_lo < self._vmin or self._chunks_loaded == 1:
                    self._vmin = float(v_lo)
                if v_hi > self._vmax or self._chunks_loaded == 1:
                    self._vmax = float(v_hi)
            elif kind == "complete":
                complete = True
                self._vmin = float(ev.get("vmin", self._vmin))
                self._vmax = float(ev.get("vmax", self._vmax))
                logger.info(
                    "[render-session %s] cube load complete vmin=%.4g vmax=%.4g "
                    "(loaded %d/%d chunks)",
                    self.session_id, self._vmin, self._vmax,
                    self._chunks_loaded, self._chunks_total,
                )
            elif kind == "start":
                logger.info(
                    "[render-session %s] cube reader started, shape=%s",
                    self.session_id, ev.get("shape"))
            elif kind == "error":
                logger.warning(
                    "[render-session %s] cube reader error: %s",
                    self.session_id, ev.get("message"))
        if any_chunk and self._image_data is not None:
            # Throttle ``Modified()`` to once every ~3 s. Each call
            # forces a 10 GB GPU texture re-upload on the next render,
            # which stalls the executor for several seconds — long
            # enough to time out the client's /v1/health probe (5 s).
            # The user sees progress less granularly but the backend
            # stays responsive to commands and the WebRTC stream
            # doesn't freeze.
            self._pending_modified = True
            now = time.time()
            if now - self._last_modified_wall >= 3.0:
                self._image_data.Modified()
                if self._vtk_intensity_array is not None:
                    self._vtk_intensity_array.Modified()
                self._last_modified_wall = now
                self._pending_modified = False
            # Reveal threshold: 25 % of chunks loaded AND running
            # vmin/vmax are well-separated (real data range exists,
            # not just zeros). The 25 % threshold keeps the initial
            # blank-cube illusion short on huge cubes while avoiding
            # a "single tile of cube floating in space" early render.
            if (not self._volume_revealed
                and self._chunks_total > 0
                and self._chunks_loaded * 4 >= self._chunks_total
                and self._vmax > self._vmin + 1e-9):
                # Stride-sample the loaded portion of the cube to get
                # a fast, reasonably-accurate p99 — this matches the
                # legacy path's threshold semantic. HI noise cubes
                # have p99 ≈ vmax/30 (most voxels are noise around
                # zero), so heuristics like ``vmin + 0.7*span`` would
                # set the threshold above all the data and make the
                # cube render as transparent / invisible.
                if self.view.threshold is None and self._cube_np is not None:
                    # Sample stride: target ~5M voxels in the sample
                    # regardless of cube size (sub-100 ms percentile).
                    stride = max(1, int(
                        (self._cube_np.size / 5_000_000) ** (1.0 / 3.0)))
                    sample = self._cube_np[::stride, ::stride, ::stride]
                    if sample.size:
                        # Mask out the still-zero (unloaded) regions
                        # to avoid skewing p99 toward 0. Loaded chunks
                        # have real data including negatives; only the
                        # unloaded z-slabs are EXACTLY zero.
                        nonzero = sample[sample != 0.0]
                        if nonzero.size > 1000:
                            self.view.threshold = float(
                                np.percentile(nonzero, 99.0))
                        else:
                            self.view.threshold = self._vmin + 0.05 * (
                                self._vmax - self._vmin)
                    else:
                        self.view.threshold = self._vmin
                self._rebuild_volume_tfs_on_thread()
                # Force a Modified() at reveal regardless of throttle
                # — the user expects to SEE the volume the moment we
                # toggle visibility.
                self._image_data.Modified()
                if self._vtk_intensity_array is not None:
                    self._vtk_intensity_array.Modified()
                self._last_modified_wall = time.time()
                self._pending_modified = False
                if self._volume is not None:
                    self._volume.SetVisibility(1)
                self._volume_revealed = True
                logger.info(
                    "[render-session %s] volume revealed at %d/%d chunks "
                    "vmin=%.4g vmax=%.4g threshold=%.4g",
                    self.session_id, self._chunks_loaded,
                    self._chunks_total, self._vmin, self._vmax,
                    self.view.threshold or 0.0,
                )
                self.request_idr()
        if complete:
            # Rebuild transfer functions + threshold with the final
            # accurate min/max — placeholder LUT during loading
            # rendered with bad scalar range, this is the cutover.
            # Sampling with a stride keeps np.percentile fast on
            # multi-GB cubes (a 10 GB cube with stride 8 → ~20 MB
            # sample, p99 in <100 ms instead of minutes).
            if self._cube_np is not None:
                stride = max(1, int((self._cube_np.size / 5_000_000) ** (1.0 / 3.0)))
                sample = self._cube_np[::stride, ::stride, ::stride]
                if self.view.threshold is None or self.view.threshold == 0.0:
                    self.view.threshold = float(np.percentile(sample, 99.0))
                self._slice_vmin = float(np.percentile(sample, 1.0))
                self._slice_vmax = float(np.percentile(sample, 99.0))
                logger.info(
                    "[render-session %s] cube complete percentiles "
                    "(stride=%d sample=%d): threshold=%.4g slice_vmin=%.4g "
                    "slice_vmax=%.4g",
                    self.session_id, stride, sample.size,
                    self.view.threshold, self._slice_vmin, self._slice_vmax,
                )
            # Final Modified() so the last chunk is on the GPU before
            # the next render — overrides the throttle below.
            if self._image_data is not None:
                self._image_data.Modified()
                if self._vtk_intensity_array is not None:
                    self._vtk_intensity_array.Modified()
                self._last_modified_wall = time.time()
                self._pending_modified = False
            self._rebuild_volume_tfs_on_thread()
            self._rebuild_slice_lut_on_thread()
            self.request_idr()

    def _render_rgb_on_thread(self) -> tuple:
        """Render one frame and return ``(rgb_ndarray, render_ms)``.

        Shared sub-step of both JPEG and WebRTC transports. The JPEG
        path (``_render_frame_on_thread``) calls this, then encodes
        the numpy array via PIL. The WebRTC path (``webrtc_session``)
        calls this directly and pushes the array into an
        ``av.VideoFrame`` for the NVENC encoder — no PIL round-trip,
        no JPEG layer.

        Readback uses ``RGBA`` (not ``RGB``) because the GLES build of
        VTK 9.3 on ppc64le EGL refuses ``glReadPixels`` with
        ``GL_RGB``/``GL_UNSIGNED_BYTE`` (GL error 1282 INVALID_OPERATION).
        We drop the alpha channel after readback. Returned array is
        contiguous ``uint8`` shape ``(height, width, 3)`` and is safe
        to pass to ``av.VideoFrame.from_ndarray`` directly.
        """
        if self._render_window is None or self._renderer is None:
            raise RuntimeError(
                f"Session {self.session_id} not initialised (call await initialize())."
            )
        import numpy as np_local
        import vtk
        from vtk.util.numpy_support import vtk_to_numpy

        # Phase 4 Path 1: drain any new cube chunks from the background
        # reader BEFORE rendering this frame. Each completed chunk has
        # already been written into the shared-memory buffer the
        # vtkImageData points to; here we just call Modified() so VTK
        # re-uploads the relevant slabs to VRAM on the next draw.
        self._poll_cube_chunks()

        cam = self._renderer.GetActiveCamera()

        # Sync the corner-axes camera so the orientation triad rotates
        # with the main scene. We copy direction-of-projection + view-up
        # from the main camera, then reset the axes renderer's own camera
        # so the (unit-sized) triad fills its tiny viewport.
        if self._axes_renderer is not None:
            axes_cam = self._axes_renderer.GetActiveCamera()
            mp = cam.GetPosition()
            mf = cam.GetFocalPoint()
            mu = cam.GetViewUp()
            # Mirror only ORIENTATION — use a unit-distance camera so
            # the axes are sized by ResetCamera() afterwards, not by
            # the main camera's huge cube-fitting distance.
            dir_p = (mp[0] - mf[0], mp[1] - mf[1], mp[2] - mf[2])
            n = (dir_p[0]**2 + dir_p[1]**2 + dir_p[2]**2) ** 0.5 or 1.0
            ux, uy, uz = dir_p[0]/n, dir_p[1]/n, dir_p[2]/n
            axes_cam.SetFocalPoint(0.0, 0.0, 0.0)
            axes_cam.SetPosition(ux, uy, uz)
            axes_cam.SetViewUp(mu)
            self._axes_renderer.ResetCamera()
            self._axes_renderer.GetActiveCamera().Zoom(1.4)

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
        t2 = time.perf_counter()
        out = w2if.GetOutput()
        scalars = out.GetPointData().GetScalars()
        if scalars is None:
            raise RuntimeError(
                "vtkWindowToImageFilter returned no scalars — readback failed"
            )
        arr = vtk_to_numpy(scalars)
        t3 = time.perf_counter()
        h, w = self.view.height, self.view.width
        # NO Y FLIP, NO ALPHA DROP. Two pieces of folklore that used
        # to live here, both expensive:
        #
        #   1. ``arr.reshape((h,w,4))[::-1]`` — flipping with a
        #      negative stride on axis 0 forces numpy out of its
        #      memcpy-per-row path. The QImage::mirrored() call in
        #      the Qt client does the same job in SIMD. Saved ~15 ms.
        #
        #   2. ``ascontiguousarray(... [:, :, :3])`` — dropping the
        #      alpha channel forces numpy to read 4 bytes and write 3
        #      per pixel: no memcpy, generic strided iterator. PyAV's
        #      ``VideoFrame.from_ndarray(rgba, format="rgba")``
        #      accepts a 4-channel array directly and NVENC ignores
        #      the alpha during RGB→YUV conversion. Saves ~15-20 ms.
        #
        # Net: the WebRTC frame-prep step drops from ~26 ms to
        # essentially zero (just a no-copy view).
        # JPEG path still flips + drops alpha explicitly because PIL's
        # JPEG encoder needs RGB.
        rgba = arr.reshape((h, w, 4))
        t4 = time.perf_counter()

        # Per-step timing breakdown — once a second to keep log noise
        # tolerable. Reveals whether the per-frame ``render_total``
        # we see at the track layer lives in the GPU readback
        # (``w2if.Update``), the numpy bridge (``vtk_to_numpy``), or
        # the reshape view (now near-zero).
        self._render_step_counter = getattr(self, "_render_step_counter", 0) + 1
        if self._render_step_counter % 60 == 0:
            logger.info(
                "[render-session %s] frame steps: vtk_render=%.1fms "
                "readback(w2if)=%.1fms vtk_to_numpy=%.1fms "
                "rgba_view=%.1fms total=%.1fms",
                self.session_id,
                (t1 - t0) * 1000.0,
                (t2 - t1) * 1000.0,
                (t3 - t2) * 1000.0,
                (t4 - t3) * 1000.0,
                (t4 - t0) * 1000.0,
            )
        return rgba, (t1 - t0) * 1000.0

    def _render_frame_on_thread(self) -> bytes:
        """Render one frame and return JPEG bytes. Legacy WS transport path.

        ``_render_rgb_on_thread`` now returns an RGBA bottom-up buffer
        (the WebRTC path consumes it as-is). For JPEG we still need RGB
        top-down, so flip + drop alpha here. Costs ~15-20 ms on the
        legacy path, but that path is fallback-only and has never been
        targeted for 60 FPS — JPEG-over-WS struggles to clear 20 FPS
        anyway because of the encoder cost.
        """
        import numpy as _np
        rgba, render_ms = self._render_rgb_on_thread()

        # JPEG path needs the vertical flip + alpha drop server-side
        # — PIL doesn't do cheap pixel mirroring on the encoder side,
        # and the legacy ws-jpeg client paints the QImage directly
        # without a Qt transform.
        rgb = _np.ascontiguousarray(rgba[::-1, :, :3])
        # JPEG encode.
        img = Image.fromarray(rgb, mode="RGB")
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=self.view.jpeg_quality,
                  optimize=False, progressive=False)
        jpeg = buf.getvalue()

        # Telemetry.
        self.stats.frames_rendered += 1
        self.stats.last_render_ms = render_ms
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
            # Order matters: VTK objects that may hold references into
            # the shared-memory buffer (vtkImageData, vtkFloatArray)
            # must be released BEFORE the CubeReader tears down its
            # SharedMemory segment — otherwise we'd be reading from
            # freed pages on the way out.
            self._vtk_intensity_array = None
            self._cube_flat_view = None
            self._image_data = None
            self._cube_np = None
            if self._cube_reader is not None:
                try:
                    self._cube_reader.close()
                except Exception as e:
                    logger.warning(
                        "[render-session %s] CubeReader close raised: %s",
                        self.session_id, e,
                    )
                self._cube_reader = None
        except Exception:
            logger.exception(
                "[render-session %s] close raised — leaking is the lesser evil",
                self.session_id,
            )
