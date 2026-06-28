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
