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


# Volume ray casting while the view moves, on the half-size drag window,
# coarsest last: (image sample distance, ray sample-distance factor). One ray
# per N×N pixels, 1/M of the samples along each ray. What a level costs depends
# on the cube and even more on its transfer function (a half-transparent noise
# cube is sampled through its whole depth), so the level adapts to the render
# time measured during the drag. On a V100, a 2 GB cube at 1270×780 took 20 ms
# at the first level with an opaque-ish transfer function and 60-110 ms with a
# half-transparent one. (Shrinking the window further instead of sparser rays
# was tried on PLEIADI-GPU and gave fewer frames per second, not more.)
_LOD_LEVELS = ((2.0, 2.0), (3.0, 3.0), (4.0, 4.0), (6.0, 6.0))
# GPU time (render + readback) a drag frame aims at: with ~15 ms of flip and
# JPEG on top, about 30 FPS. A level is coarsened when a frame
# takes more than _LOD_SLOWER of it, and refined only when the finer level is
# expected to fit it: the measured time scaled by the levels' relative cost
# (rays × samples per ray, image_sd² × factor). Refining on "fast" alone made a
# cube at 42 ms on one level and 12 ms on the next bounce between them.
_LOD_TARGET_MS = 25.0
_LOD_SLOWER = 1.2


def _lod_cost(level: int) -> float:
    """How many times cheaper than full sampling a drag level is."""
    image_sd, factor = _LOD_LEVELS[level]
    return image_sd * image_sd * factor


class ModesMixin:
    """Render-mode, transfer-function and LUT worker methods."""

    def _apply_interactive_mode_on_thread(self, interactive: bool) -> None:
        """Toggle the LOD between full-quality and a fast wireframe.

        Full quality (interactive=False):
            volume mapper OR isosurface mesh (per cube_render_mode) +
            cutting plane (if visible) + outline + axes triad.
            Render window at the client's requested size (e.g. 1586×1244
            on a Retina dock).

        Interactive (interactive=True), three combined tricks:

          1. A coarse version of what is on screen. In volume mode the
             volume itself, with fewer rays and fewer samples per ray
             (see _LOD_LEVELS; the user keeps seeing the data turn, not
             an empty box); in
             isosurface mode a LOW-POLY iso wireframe of the emission.
             The cutting plane and the axes triad are hidden, the
             bounding-box outline forced bright/thick.

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
            self._apply_cube_render_mode_on_thread(self.view.cube_render_mode)
            if self._cutting_plane_actor is not None:
                self._cutting_plane_actor.SetVisibility(0)
            # Bright outline so the cube box reads against the dark
            # background while it's almost the only thing on screen.
            if self._outline_actor is not None:
                self._outline_actor.SetVisibility(1)
                prop = self._outline_actor.GetProperty()
                prop.SetColor(1.0, 1.0, 1.0)
                prop.SetLineWidth(2.0)
            # Hide axes triad to save the extra renderer pass.
            if self._axes_renderer is not None:
                self._axes_renderer.SetDraw(0)
            # Coarse ray casting for the volume while it moves; the
            # previous settings come back on release.
            if self._volume_mapper is not None and self._lod_prev_sampling is None:
                m = self._volume_mapper
                self._lod_prev_sampling = (m.GetAutoAdjustSampleDistances(),
                                           m.GetImageSampleDistance(),
                                           m.GetSampleDistance())
                m.SetAutoAdjustSampleDistances(0)
                self._apply_lod_level_on_thread()
            # Halve render-window dimensions — 4× fewer pixels means
            # 4× faster readback + encode. Cache the previous size so
            # we can restore it on release.
            if self._render_window is not None and self._lod_prev_size is None:
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
            if self._axes_renderer is not None:
                self._axes_renderer.SetDraw(1 if self.view.show_axes else 0)
            if self._volume_mapper is not None and self._lod_prev_sampling is not None:
                auto, image_sd, sample_d = self._lod_prev_sampling
                self._volume_mapper.SetAutoAdjustSampleDistances(auto)
                self._volume_mapper.SetImageSampleDistance(image_sd)
                self._volume_mapper.SetSampleDistance(sample_d)
                self._lod_prev_sampling = None
            if self._render_window is not None and self._lod_prev_size is not None:
                w, h = self._lod_prev_size
                self._render_window.SetSize(w, h)
                self.view.width = w
                self.view.height = h
                self._lod_prev_size = None
            # Through the applier, not a second copy of its rule. This branch
            # used to reinstate the mode's own visibility directly, which meant
            # releasing a drag brought back a rendering the user had switched
            # off — checkbox still clear, rendering back on screen.
            self._apply_cube_render_mode_on_thread(self.view.cube_render_mode)
            if self._cutting_plane_actor is not None:
                self._cutting_plane_actor.SetVisibility(
                    1 if self.view.cutting_plane_visible else 0)

    def _apply_lod_level_on_thread(self) -> None:
        """Set the volume mapper to the current drag level (see _LOD_LEVELS)."""
        if self._volume_mapper is None or self._lod_prev_sampling is None:
            return
        image_sd, factor = _LOD_LEVELS[self._lod_level]
        self._volume_mapper.SetImageSampleDistance(image_sd)
        self._volume_mapper.SetSampleDistance(self._lod_prev_sampling[2] * factor)

    def _adapt_lod_on_thread(self, render_ms: float) -> None:
        """After a drag frame: one level coarser if it rendered too slowly, one
        finer if that one is expected to fit the target. The level is kept for
        the next drag, which then starts where this one settled. Only frames
        that drew the volume count: an isosurface wireframe or a hidden
        rendering costs nothing and says nothing about the volume."""
        if not self.view.interactive or self._lod_prev_sampling is None:
            return
        if self.view.cube_render_mode == "isosurface" or not self.view.show_rendering:
            return
        level = self._lod_level
        if render_ms > _LOD_TARGET_MS * _LOD_SLOWER and level < len(_LOD_LEVELS) - 1:
            level += 1
        elif level > 0 and render_ms * _lod_cost(level) / _lod_cost(level - 1) <= _LOD_TARGET_MS:
            level -= 1
        if level != self._lod_level:
            self._lod_level = level
            self._apply_lod_level_on_thread()

    def _apply_cube_render_mode_on_thread(self, mode: str) -> None:
        """Resolve which of the three rendering props is on screen.

        The single owner of that rule, so the callers below cannot drift from
        each other. Three inputs decide it:

        * ``mode`` — WHICH representation, volume or isosurface.
        * ``show_rendering`` — WHETHER either is drawn at all. Without this the
          mode switch would quietly bring back a rendering the user had hidden.
        * ``interactive`` — during a drag the volume stays (ray-cast coarsely,
          see ``_apply_interactive_mode_on_thread``) while the low-poly LOD
          wireframe stands in for the isosurface, so exactly one of the three
          is ever visible.
          Resolving it here rather than only on the interaction transitions is
          what lets the flag change mid-drag: applied without it, hiding the
          rendering left the wireframe on, and showing it put the full actor up
          alongside the wireframe.
        """
        is_iso = (mode == "isosurface")
        shown = self.view.show_rendering
        lod = self.view.interactive
        if self._volume is not None:
            self._volume.SetVisibility(1 if (shown and not is_iso) else 0)
        if self._iso_actor is not None:
            self._iso_actor.SetVisibility(1 if (shown and is_iso and not lod) else 0)
        if self._iso_lod_actor is not None:
            self._iso_lod_actor.SetVisibility(1 if (shown and is_iso and lod) else 0)

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
