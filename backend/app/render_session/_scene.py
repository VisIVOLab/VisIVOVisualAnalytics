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

class SceneMixin:
    """Scene-build worker methods (run on the session's VTK thread)."""

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
        from ..cube_reader import CubeReader
        from ..fits_dataset import image_hdu_index

        # ── 1. Peek the FITS for cube shape WITHOUT loading data ─────
        # Phase 4 Path 1: the FITS read happens in a separate worker
        # process via CubeReader so this executor thread isn't blocked
        # for 2-3 minutes on a 22 GB cube. Here we just open the header
        # to discover (nz, ny, nx) so we can pre-allocate the shared
        # memory buffer that backs vtkImageData.
        with _fits.open(self.dataset_path, memmap=True) as hdul:
            hdu_index = image_hdu_index(hdul)
            # From the header: touching .data would decompress a whole
            # tile-compressed cube here.
            shape = tuple(hdul[hdu_index].shape)  # type: ignore[attr-defined]
            if len(shape) < 3:
                raise ValueError(
                    f"Volume rendering requires a 3-D cube, got shape {shape}"
                )
            disk_shape = shape[-3:]   # (nz, ny, nx); leading axes: first plane
            nz, ny, nx = disk_shape
            cube_size_bytes = int(np.prod(disk_shape)) * 4   # float32

        # Threshold for the chunked path: cubes above this size load
        # progressively via CubeReader; smaller ones go through the
        # legacy synchronous path which is simpler and adds no extra
        # process. 256 MB (~256³ voxels) is the same threshold the
        # client uses for local-vs-remote rendering.
        chunked_threshold = 256 * 1024 * 1024
        # Larger still, the cube is not loaded at all: it is drawn from its
        # brick pyramid, block by block (_ooc.py, issue #70).
        from ._ooc import BlockVolumes, OutOfCoreCube, wants_out_of_core
        out_of_core = wants_out_of_core(cube_size_bytes)
        use_chunked = cube_size_bytes > chunked_threshold and not out_of_core
        proxy_origin, proxy_spacing = (0.0, 0.0, 0.0), 1.0

        # ── 2. Allocate the cube buffer + start reader if chunked ─────
        if out_of_core:
            self._cube_reader = None
            self._ooc = OutOfCoreCube(self.dataset_path, hdu_index, self.session_id)
            # The coarse proxy stands in for the cube wherever the whole of it
            # is needed (outline, isosurface, percentiles).
            cube, proxy_origin, proxy_spacing = self._ooc.proxy()
            finite_mask = np.ones(cube.shape, dtype=bool)
            vmin, vmax = self._ooc.vmin, self._ooc.vmax
            logger.info("[render-session %s] out-of-core rendering of a %.2f GB cube "
                        "(proxy %s, spacing %g)", self.session_id,
                        cube_size_bytes / (1024 ** 3), cube.shape, proxy_spacing)
        elif use_chunked:
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
            self._cube_complete = False
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
                data = hdul[image_hdu_index(hdul)].data
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
            # Out of core, from level-0 bricks: the proxy's averages would put
            # p99 well inside the noise.
            sample = self._ooc.sample() if out_of_core else None
            if self.view.threshold is None:
                finite = sample if out_of_core else (cube[finite_mask] if not finite_mask.all() else cube)
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
            finite = sample if out_of_core else (cube[finite_mask] if not finite_mask.all() else cube)
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
        # Out of core this is the coarse proxy, placed in level-0 voxels so
        # that its bounds are the cube's.
        pz, py, px = cube.shape
        img.SetDimensions(px, py, pz)
        img.SetSpacing(proxy_spacing, proxy_spacing, proxy_spacing)
        img.SetOrigin(*proxy_origin)
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
        if out_of_core:
            # The blocks of the view, behind the same interface: the code that
            # sets sampling, blend mode and visibility does not change.
            self._volume_mapper = BlockVolumes(prop)
            self._ooc.blocks = self._volume_mapper
        else:
            self._volume_mapper = vtk.vtkGPUVolumeRayCastMapper()
            self._volume_mapper.SetInputData(img)
        self._volume_mapper.SetAutoAdjustSampleDistances(1)
        self._volume_mapper.SetUseJittering(1)
        self._apply_blend_mode_on_thread(self.view.blend_mode)

        if out_of_core:
            self._volume = self._volume_mapper
        else:
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
        if out_of_core:
            self._volume.attach(self._renderer)
        else:
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
        # The EGL device is the one the window's constructor took from
        # VTK_DEFAULT_EGL_DEVICE_INDEX, which the render process sets to this
        # session's GPU (render_session/_process.py); unset, VTK's default, 0.
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
        self._iso_filter = flying
        self._iso_mapper = mapper
        self._iso_actor  = actor
        self._renderer.AddActor(actor)
        # Through the shared applier rather than deciding here: this runs during
        # construction and used to overwrite whatever visibility the client had
        # already asked for, so a session told to hide the rendering got it back
        # the moment the isosurface finished building.
        self._apply_cube_render_mode_on_thread(self.view.cube_render_mode)

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
        # Out of core the cube is not in memory: the plane is read from the
        # file at full resolution.
        slice2d = self._ooc.plane(z) if self._ooc is not None else self._cube_np[z]  # (ny, nx)
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
