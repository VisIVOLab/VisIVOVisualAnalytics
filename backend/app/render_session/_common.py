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


# NB: the EGL capability probe (`server_side_rendering_available` +
# `_EGL_AVAILABLE`) lives in the package `__init__` so the documented
# re-probe hook stays at ``app.render_session._EGL_AVAILABLE``.


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
