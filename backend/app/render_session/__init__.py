"""Server-side render session package.

Split from the former monolithic render_session.py. The public surface
(`RenderSession`, `server_side_rendering_available`, `SessionViewState`,
`SessionStats`) is unchanged: existing `from app.render_session import ...`
call sites keep working.
"""
from __future__ import annotations

import asyncio
import io
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Any

import numpy as np
from PIL import Image

from ._common import (
    SessionStats,
    SessionViewState,
    _build_color_tf,
    _build_opacity_tf,
    logger,
)

from ._scene import SceneMixin
from ._modes import ModesMixin
from ._pipeline import PipelineMixin


# ── Capability probe (used by the dataset open endpoint to set the flag) ─────
# Kept in the package __init__ (not a submodule) so the documented re-probe
# hook stays at ``app.render_session._EGL_AVAILABLE`` exactly as before the
# package split.

_EGL_AVAILABLE: bool | None = None


def server_side_rendering_available() -> bool:
    """Return True iff this backend can actually run a RenderSession.

    Cached because the probe imports a non-trivial module, which is
    wasted work past the first call. Re-probe by setting
    ``app.render_session._EGL_AVAILABLE = None`` (e.g. in tests).

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


class RenderSession(SceneMixin, ModesMixin, PipelineMixin):
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
        # Volume-mapper sampling (auto-adjust, image and ray sample distances)
        # saved across an LOD cycle; None when not in interactive mode.
        self._lod_prev_sampling = None
        # Current drag LOD level (index into _modes._LOD_LEVELS), adapted to
        # the measured render time and kept across drags.
        self._lod_level = 0
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
        # Out-of-core rendering from the brick pyramid (_ooc.py), for cubes
        # above VISIVO_RENDER_OUT_OF_CORE_BYTES; None otherwise.
        self._ooc = None
        self._ooc_more = False
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
        # Dirty-flag rendering. ``True`` means the scene state changed
        # since the last render; ``False`` means a re-render would
        # produce the SAME pixels as the cached frame. With this in
        # place, a static view sends the previous frame's bytes
        # (encoded as P-frames with no delta = ~zero bandwidth) and
        # SKIPS the 70 ms VTK render + readback entirely on a 22 GB
        # cube. Effect: while the user looks at a still frame the
        # stream runs at the pacing FPS (60) with ~0 ms server CPU.
        # Set ``True`` initially so the first frame renders.
        self._scene_dirty = True
        # Incremented on every change. Push streams compare it with the
        # version they last sent, each its own: a flag cleared by whichever
        # consumer rendered first (the WebRTC path, another connection) would
        # hide the change from the others.
        self._scene_version = 0
        # Cached RGBA buffer + render-ms from the last successful
        # render. Returned by ``render_rgb_frame`` when the scene is
        # not dirty. Owned by RenderSession; do NOT mutate from the
        # WebRTC track.
        self._last_rgba_view = None
        self._last_render_ms = 0.0
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
        # Set when the chunked reader has delivered the whole cube (or failed).
        self._cube_complete = True
        # A threshold the client sent, even 0, is never replaced by the
        # percentile computed once a streaming cube is in.
        self._threshold_from_client = False
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
        # Set whenever a command changes the scene: the push transport waits
        # on it instead of on a client's request for each frame.
        self.changed = asyncio.Event()

    # ── Public API ───────────────────────────────────────────────────────

    def levels(self) -> dict[str, Any]:
        """The starting values the client shows: the threshold the session
        renders with and the cube's data range. While a large cube is still
        streaming in, ``pending`` is true; the threshold and range are None
        until it is revealed and may still change until it is complete."""
        loading = self._cube_reader is not None and not self._cube_complete
        known = not loading or self._volume_revealed
        return {
            "threshold": self.view.threshold if known else None,
            "data_min": self._vmin if known else None,
            "data_max": self._vmax if known else None,
            "pending": loading,
        }

    async def initialize(self) -> None:
        """Run the VTK pipeline setup on the worker thread."""
        loop = asyncio.get_event_loop()
        await loop.run_in_executor(self._executor, self._init_on_thread)

    def mark_dirty(self) -> None:
        """Mark the scene as needing a fresh render. Cheap; called from
        every code path that mutates camera, view state, cube data, or
        UI options. The next ``render_rgb_frame`` will actually run
        VTK; subsequent calls reuse the cached RGBA until ``mark_dirty``
        fires again. Also called on the worker thread (chunk arrivals):
        it only sets flags.
        """
        self._scene_dirty = True
        self._scene_version += 1

    def is_dirty(self) -> bool:
        return self._scene_dirty

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
        if command.get("type") == "keepalive":
            # Only proof that someone is still looking: nothing to apply, and
            # marking the scene would render and push a frame for nothing.
            return
        await loop.run_in_executor(
            self._executor, self._apply_command_on_thread, command,
        )
        # Every command mutates view state — flag the scene dirty so
        # the next frame is actually rendered. Without this the
        # client would see a stale image until the next non-command
        # cause of dirtying (chunk arrival, etc.).
        self.mark_dirty()
        self.changed.set()

    async def render_frame(self) -> bytes:
        """Render the current view state to a JPEG and return the bytes."""
        if self._closed:
            raise RuntimeError(f"Session {self.session_id} is closed.")
        loop = asyncio.get_event_loop()
        async with self._render_lock:
            return await loop.run_in_executor(
                self._executor, self._render_frame_on_thread,
            )

    async def render_frame_if_changed(self, since: int) -> tuple[bytes | None, int]:
        """The push transport's frame: ``(jpeg, version)`` of the current view
        if the scene changed after ``version`` ``since``, else ``(None, since)``.

        Polls the chunk reader first, so a cube still loading keeps arriving
        on screen without any command. The version is read BEFORE rendering:
        a command applied meanwhile moves it on and is rendered next, never
        lost.
        """
        if self._closed:
            raise RuntimeError(f"Session {self.session_id} is closed.")
        loop = asyncio.get_event_loop()
        async with self._render_lock:
            if self._cube_reader is not None:
                await loop.run_in_executor(self._executor, self._poll_cube_chunks)
            version = self._scene_version
            if version == since:
                return None, since
            jpeg = await loop.run_in_executor(
                self._executor, self._render_frame_on_thread,
            )
            return jpeg, version

    async def render_rgb_frame(self):
        """Render the current view state and return ``(rgb_ndarray, render_ms)``.

        Same camera/scene path as :meth:`render_frame` but skips the
        JPEG encode step — the WebRTC transport feeds the raw numpy
        array straight into an ``av.VideoFrame`` for NVENC. Stats are
        updated in-place; the caller doesn't need to.

        DIRTY-FLAG SHORT-CIRCUIT: if the scene hasn't changed since the
        last render, return the cached RGBA buffer immediately —
        skipping VTK render + GPU readback entirely (typically a
        70 ms saving on a 22 GB cube at 1080p). NVENC will encode the
        identical frame as a near-zero-byte P-frame, so the WebRTC
        stream keeps flowing at the pacing FPS without any actual
        compute work. The user sees ~infinite FPS on static views
        and the cube goes from "14 FPS sluggish" to "instant" the
        moment they stop interacting.
        """
        if self._closed:
            raise RuntimeError(f"Session {self.session_id} is closed.")
        # Always check the chunk reader on the executor thread (it
        # touches _image_data so it MUST run inside the render lock).
        # If a chunk arrived, the poll calls Modified() and marks the
        # scene dirty — which then forces a fresh VTK render below.
        loop = asyncio.get_event_loop()
        async with self._render_lock:
            if not self._scene_dirty and self._last_rgba_view is not None:
                # Skip VTK entirely. Re-poll chunks just to drain
                # progress events (and dirty if new data arrived).
                await loop.run_in_executor(
                    self._executor, self._poll_cube_chunks,
                )
                if not self._scene_dirty:
                    self.stats.frames_rendered += 1
                    return self._last_rgba_view, self._last_render_ms
            # Dirty (or first frame) — actually render.
            rgb, render_ms = await loop.run_in_executor(
                self._executor, self._render_rgb_on_thread,
            )
            # Blocks still loading out of core: the next frame shows more.
            self._scene_dirty = self._ooc_more
            self._last_rgba_view = rgb
            self._last_render_ms = render_ms
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


__all__ = [
    "RenderSession",
    "server_side_rendering_available",
    "SessionViewState",
    "SessionStats",
]
