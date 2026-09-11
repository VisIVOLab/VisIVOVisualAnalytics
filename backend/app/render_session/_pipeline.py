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

class PipelineMixin:
    """Command-apply, chunk-poll, frame-render and close worker methods."""

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
        ``{"type": "set_show_rendering",  "visible": bool}``    — volume/iso on/off

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
        elif ctype == "set_show_rendering":
            self.view.show_rendering = bool(command.get("visible", True))
            # Through the mode applier, so the flag and cube_render_mode are
            # resolved in one place rather than two that can disagree.
            self._apply_cube_render_mode_on_thread(self.view.cube_render_mode)
            self.request_idr()
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
                # Force a re-render so the user sees the new slab.
                self._scene_dirty = True
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
                # Override the placeholder threshold (0.0) from init.
                # If the user already set a real threshold via the
                # slider during loading, ``threshold != 0.0`` and we
                # respect their pick. Otherwise compute the proper
                # p99 from a stride sample of the loaded data.
                if (self._cube_np is not None
                    and (self.view.threshold is None
                         or self.view.threshold == 0.0)):
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
                # Resolve against the flag and the mode rather than showing the
                # volume outright. A client that hid the rendering before the
                # cube finished streaming — or attached to a session still
                # loading — would otherwise have it appear on its own at the
                # 25 % mark, with the checkbox still clear.
                self._apply_cube_render_mode_on_thread(self.view.cube_render_mode)
                self._volume_revealed = True
                logger.info(
                    "[render-session %s] volume revealed at %d/%d chunks "
                    "vmin=%.4g vmax=%.4g threshold=%.4g",
                    self.session_id, self._chunks_loaded,
                    self._chunks_total, self._vmin, self._vmax,
                    self.view.threshold or 0.0,
                )
                self.request_idr()
                self._scene_dirty = True
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
            self._scene_dirty = True
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
                "vtkWindowToImageFilter returned no scalars, readback failed"
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
                "[render-session %s] close raised, leaking is the lesser evil",
                self.session_id,
            )
