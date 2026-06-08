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
        # Cutting plane infrastructure — see _build_cutting_plane().
        self._cutting_plane_source = None  # vtkPlaneSource
        self._cutting_plane_actor = None   # vtkActor
        self._cutting_plane_texture = None # vtkTexture (slice colors)
        self._cutting_plane_slice_img = None  # vtkImageData (current slice)
        self._cutting_plane_slice_colors = None  # vtkImageMapToColors
        self._slice_lut_ctf = None         # vtkColorTransferFunction for slice
        self._cutting_plane_slice_index = -1   # invalidates first update
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

        nz, ny, nx = cube.shape
        self._cube_np = cube
        self._nx, self._ny, self._nz = nx, ny, nz
        self._vmin, self._vmax = vmin, vmax
        self.view.vmin = vmin if self.view.vmin is None else self.view.vmin
        self.view.vmax = vmax if self.view.vmax is None else self.view.vmax
        # Default threshold = 99th percentile. HI cubes have ~99 % of
        # voxels in the Gaussian noise band around zero with maybe 1 %
        # in the emission tail; p99 puts the cutoff right at the edge
        # of noise so the FIRST frame already shows only the emission.
        # Without this default the initial render before the client
        # pushes its slider value looks like a solid noise block —
        # vmin + 3 % span lets the noise voxels render fully opaque.
        if self.view.threshold is None:
            finite = cube[finite_mask] if not finite_mask.all() else cube
            self.view.threshold = float(np.percentile(finite, 99.0))
        # Cutting plane defaults to the middle channel — matches the
        # local viewer's initial state.
        if self.view.cutting_plane_slice_index <= 0:
            self.view.cutting_plane_slice_index = nz // 2

        # Slice-LUT range: the cube's vmin/vmax is dominated by a few
        # outlier voxels (galaxy peaks, edge artefacts), so mapping the
        # slice through the full [vmin, vmax] makes the noise occupy
        # only the bottom ~5% of the LUT — that's the dark-purple end
        # of Inferno, which then looks "all-black" against the desktop's
        # bright-orange slice. The desktop builds ``lutSlice`` over the
        # SLICE's own scalar range (which excludes outliers naturally),
        # so we match that by using cube percentiles p1..p99 — robust
        # to the bright-tail outliers, close to what the desktop sees
        # when it samples a typical channel.
        finite = cube[finite_mask] if not finite_mask.all() else cube
        self._slice_vmin = float(np.percentile(finite, 1.0))
        self._slice_vmax = float(np.percentile(finite, 99.0))

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
        """Push the current threshold into the isosurface extractor."""
        if self._iso_filter is None:
            return
        self._iso_filter.SetValue(0, self.view.threshold or self._vmin)
        self._iso_filter.Modified()

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
        elif ctype == "set_slice_lut":
            self.view.slice_lut_name = str(command.get("name", self.view.slice_lut_name))
            self._rebuild_slice_lut_on_thread()
        elif ctype == "set_threshold":
            self.view.threshold = float(command.get("value", self.view.threshold or self._vmin))
            self._rebuild_volume_tfs_on_thread()
        elif ctype == "set_blend_mode":
            self.view.blend_mode = int(command.get("mode", 0))
            self._apply_blend_mode_on_thread(self.view.blend_mode)
        elif ctype == "set_cube_render_mode":
            mode = str(command.get("mode", "volume")).lower()
            if mode not in ("volume", "isosurface"):
                mode = "volume"
            self.view.cube_render_mode = mode
            self._apply_cube_render_mode_on_thread(mode)
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
        # Drop alpha. WebRTC's NVENC encoder reads RGB directly;
        # JPEG also can't carry transparency.
        rgb = np_local.ascontiguousarray(arr[:, :, :3])
        return rgb, (t1 - t0) * 1000.0

    def _render_frame_on_thread(self) -> bytes:
        """Render one frame and return JPEG bytes. Legacy WS transport path."""
        rgb, render_ms = self._render_rgb_on_thread()

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
        except Exception:
            logger.exception(
                "[render-session %s] close raised — leaking is the lesser evil",
                self.session_id,
            )
