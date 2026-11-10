"""Server-side rendering of point data (issue #83): a VisIVO Binary Table point
table, or a catalogue, drawn on the backend and streamed like a cube's view.
The dataset is a VBT header path, or a JSON catalogue spec
``{"paths": [...], "format": str, "coordinates": {...}}``.

The session has the public surface of :class:`app.render_session.RenderSession`
(``initialize``, ``apply_command``, ``render_frame``, ``render_frame_if_changed``,
``render_rgb_frame``, ``levels``, ``close``...), so the render router, the
per-session process (``_process.py``, ``factory``) and both transports serve it
unchanged.

It draws what the desktop's point viewer draws (``vtkWindowVbt``): points or
emissive Gaussian splats, coloured by one column through a named colour map over
a range, with the same splat size and opacity rules, a bounding box and an
orientation marker. The table is memory-mapped where it lies; the columns drawn
are copied once into the GPU's arrays.

Commands (besides the camera / viewport ones the cube session takes):

``{"type": "set_point_fields", "x": str, "y": str, "z": str}``
``{"type": "set_color", "field": str | None, "lut": str,
   "min": float | None, "max": float | None,
   "points": [[x, r, g, b], ...], "log": bool}``  — field None: solid colour;
   ``points``, the client's own colour function (instead of ``lut``), ``log``
   its scale
``{"type": "set_point_style", "mode": "points" | "gaussian",
   "size": int 1..12, "intensity": int 1..100}``
``{"type": "set_filters", "filters": [{"field", "op", "value"}, ...]}``
``{"type": "set_background", "name": "Dark" | "Dark gray" | "White"}``
``{"type": "set_show_outline" | "set_show_axes" | "set_show_colorbar", "visible": bool}``
``{"type": "set_sky_positions", "frame": "fk5" | "galactic" | "supergalactic",
   "distance_mode": "comoving" | "redshift_space", "cosmology": str}``
                                         — a sky catalogue's positions
"""
from __future__ import annotations

import asyncio
import json
import io
import logging
import math
import os
import time
from dataclasses import dataclass, field
from typing import Any

import numpy as np
import pandas as pd
from PIL import Image

from ._common import SessionStats, _build_color_tf, _parse_tf_points, set_camera
from ._window import (RenderThreadError, new_offscreen_window, offscreen_kind,
                      render_executor)

logger = logging.getLogger("visivo.render")

#: Points drawn at most; a larger table is drawn from an even sample of its rows
#: (the octree of issue #32 will replace the sample).
MAX_POINTS = int(os.environ.get("VISIVO_POINT_RENDER_MAX_POINTS", str(100_000_000)))
#: Points drawn while the view is dragged.
LOD_POINTS = int(os.environ.get("VISIVO_POINT_RENDER_LOD_POINTS", str(1_000_000)))
#: JPEG quality of a frame of a drag (as the cube session's).
_LOD_JPEG_QUALITY = 60

_BACKGROUNDS = {"White": (0.98, 0.98, 0.98), "Dark gray": (0.16, 0.17, 0.19),
                "Dark": (0.06, 0.06, 0.1)}
_SOLID = (0.8, 0.85, 1.0)
_NUMERIC_OPS = {"=", "!=", ">", ">=", "<", "<="}




@dataclass
class PointViewState:
    width: int = 800
    height: int = 600
    interactive: bool = False
    jpeg_quality: int = 85
    x: str = ""
    y: str = ""
    z: str = ""
    color_field: str | None = None
    lut: str = "Inferno"
    color_min: float | None = None
    color_max: float | None = None
    color_points: list | None = None   # the client's colour function, (x, r, g, b)
    color_log: bool = False
    mode: str = "points"
    size: float = 3   # points: pixels; splats: the size step (0.25..12)
    intensity: int = 35
    background: str = "Dark"
    show_outline: bool = True
    show_axes: bool = True
    show_colorbar: bool = True
    # A sky catalogue's positions (app/catalogue_positions.py).
    sky_frame: str = "fk5"
    distance_mode: str = "comoving"
    cosmology: str = "Planck18"
    filters: list[dict] = field(default_factory=list)


def _sampled_lut(ctf, cmin: float, cmax: float, log: bool):
    """``ctf`` as a 1024-entry vtkLookupTable over [cmin, cmax], as the desktop
    colours its points. A mapper colours every point through its table, and a
    colour transfer function of 256 nodes on a log scale took seconds per
    mapper for 3 M points on a POWER9 node; a table takes milliseconds."""
    import vtk
    n = 1024
    lut = vtk.vtkLookupTable()
    lut.SetNumberOfTableValues(n)
    lut.SetRange(cmin, cmax)
    centres = (np.arange(n) + 0.5) / n
    if log:
        lut.SetScaleToLog10()
        xs = cmin * (cmax / cmin) ** centres
    else:
        xs = cmin + (cmax - cmin) * centres
    for i, x in enumerate(xs):
        lut.SetTableValue(i, *ctf.GetColor(float(x)), 1.0)
    return lut


def _filter_mask(columns, filters: list[dict], rows: np.ndarray) -> np.ndarray:
    """The rows (of ``rows``) passing every filter. Numeric operators only: a
    VBT holds numbers. Unknown fields or operators raise ValueError."""
    keep = np.ones(len(rows), dtype=bool)
    for f in filters:
        name, op = str(f.get("field", "")), str(f.get("op", ""))
        if name not in columns:
            raise ValueError(f"Unknown field: {name!r}")
        if op not in _NUMERIC_OPS:
            raise ValueError(f"Unsupported filter operator for point data: {op!r}")
        value = float(f.get("value"))
        col = np.asarray(columns[name][rows])
        keep &= {"=": col == value, "!=": col != value, ">": col > value,
                 ">=": col >= value, "<": col < value, "<=": col <= value}[op]
    return keep


def gaussian_splat(bounds, npts: int, size: int, intensity: int) -> tuple[float, float]:
    """Splat radius and per-splat opacity, as the desktop viewer derives them
    (vtkWindowCatalogue3D::updateGaussianSplats): radius = mean spacing ×
    size / 8, the spacing ∛(volume / N) of the box the points fill (a filter
    that cuts a slab keeps it, as it keeps the density); opacity =
    min(0.9, 20 / D) × intensity, clamped to [0.05, 0.9], D the splats along
    the longest sightline (longest side / spacing: ∛N for a cube, as the
    former VBT viewer had, and unchanged by a filter)."""
    dims = [bounds[1] - bounds[0], bounds[3] - bounds[2], bounds[5] - bounds[4]]
    longest = max(max(dims), 1.0e-6)
    # A flat axis (points in a plane) counts as a thousandth of the longest.
    volume = math.prod(max(d, longest * 1.0e-3) for d in dims)
    spacing = math.cbrt(volume / max(npts, 1))
    radius = spacing * size / 8.0
    depth = max(longest / spacing, 1.0)
    opacity = min(max(min(0.9, 20.0 / depth) * intensity / 100.0, 0.05), 0.9)
    return radius, opacity


class PointRenderSession:
    """One client × one point table × one render thread (see the module doc)."""

    def __init__(self, session_id: str, dataset_path: str, hdu: int | None = None):
        self.session_id = session_id
        self.dataset_path = dataset_path
        self.view = PointViewState()
        self.stats = SessionStats()
        self.changed = asyncio.Event()
        # A thread of its own for EGL; on a Mac the caller's, which must be the
        # main thread (render_session/_window.py).
        self._window_kind = offscreen_kind()
        self._executor = render_executor(self._window_kind, f"vtk-pts-{session_id[:8]}")
        self._render_lock = asyncio.Lock()
        self._closed = False
        self._scene_version = 1
        self._scene_dirty = True
        self._idr = False
        self._last_rgba = None
        self._last_render_ms = 0.0
        # Data.
        self._columns: dict[str, np.memmap] = {}
        self._names: list[str] = []
        self._num_rows = 0
        self._rows = np.empty(0, dtype=np.int64)   # rows drawn (sample ∩ filters)
        self._splat_basis = None   # (bounds, count) of the unfiltered points
        self._table = None  # app/point_table.py's PointTable
        self._camera: dict[str, Any] | None = None   # copied on the render thread
        self._sky = None    # (ra, dec) columns of a sky catalogue
        self._frame = None  # the catalogue's table, for its positions
        self._data_range: tuple[float, float] | None = None
        # VTK, on the worker thread.
        self._render_window = self._renderer = None
        self._polydata = self._lod_polydata = None
        # Mappers per mode and per point set (all rows; the drag's sample):
        # giving one mapper the other set re-uploads every point to the GPU.
        self._mappers: dict[tuple[str, bool], Any] = {}
        self._actor = None
        self._outline = self._axes_renderer = self._axes_actor = None
        self._colorbar = None

    # ── Public API (the RenderSession surface) ───────────────────────────

    def levels(self) -> dict[str, Any]:
        """The colour field's data range, as a cube session reports its own."""
        lo, hi = self._data_range if self._data_range else (None, None)
        return {"threshold": None, "data_min": lo, "data_max": hi, "pending": False,
                "camera": self._camera}

    async def initialize(self) -> None:
        await self._run(self._init_on_thread)
        self._camera = await self._run(self._camera_on_thread)

    def _camera_on_thread(self) -> dict[str, Any] | None:
        """The view's camera, for a render made elsewhere of the same view (the
        ray-traced still, app/routers/particle_render.py)."""
        if self._renderer is None:
            return None
        cam = self._renderer.GetActiveCamera()
        return {"position": list(cam.GetPosition()), "focal_point": list(cam.GetFocalPoint()),
                "view_up": list(cam.GetViewUp()), "view_angle": cam.GetViewAngle(),
                "parallel": bool(cam.GetParallelProjection()),
                "parallel_scale": cam.GetParallelScale()}

    def mark_dirty(self) -> None:
        self._scene_dirty = True
        self._scene_version += 1

    def is_dirty(self) -> bool:
        return self._scene_dirty

    def request_idr(self) -> None:
        self._idr = True

    def consume_idr_flag(self) -> bool:
        idr, self._idr = self._idr, False
        return idr

    async def apply_command(self, command: dict[str, Any]) -> None:
        self.stats.commands_received += 1
        self.stats.last_command_at = time.time()
        if command.get("type") == "keepalive":
            return
        try:
            await self._run(self._apply_command_on_thread, command)
        except ValueError as exc:
            # Refused, the view unchanged: raised, it would close the stream,
            # and a client that resends its state on reconnecting would close
            # it again.
            logger.warning("[point-render %s] %s refused: %s", self.session_id[:8],
                           command.get("type"), exc)
            return
        self._camera = await self._run(self._camera_on_thread)
        self.mark_dirty()
        self.changed.set()

    async def render_frame(self) -> bytes:
        self._check_open()
        async with self._render_lock:
            return await self._run(self._render_frame_on_thread)

    async def render_frame_if_changed(self, since: int) -> tuple[bytes | None, int]:
        self._check_open()
        async with self._render_lock:
            version = self._scene_version
            if version == since:
                return None, since
            jpeg = await self._run(self._render_frame_on_thread)
            return jpeg, version

    async def render_rgb_frame(self):
        self._check_open()
        async with self._render_lock:
            if not self._scene_dirty and self._last_rgba is not None:
                self.stats.frames_rendered += 1
                return self._last_rgba, self._last_render_ms
            rgba, ms = await self._run(self._render_rgb_on_thread)
            self._scene_dirty = False
            self._last_rgba, self._last_render_ms = rgba, ms
        self.stats.frames_rendered += 1
        self.stats.last_render_ms = ms
        return rgba, ms

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            await self._run(self._close_on_thread)
        except RenderThreadError:
            # Nothing but VTK to release, and it may not be touched from here.
            logger.warning("[point-render %s] closed off the main thread: VTK left "
                           "to the process exit", self.session_id[:8])
        finally:
            self._executor.shutdown(wait=False, cancel_futures=True)

    async def _run(self, fn, *args):
        """``fn`` on the render thread: the session's own (EGL), or on a Mac
        the calling one, refused unless it is the main thread."""
        return await asyncio.get_event_loop().run_in_executor(self._executor, fn, *args)

    def _check_open(self) -> None:
        if self._closed:
            raise RuntimeError(f"Session {self.session_id} is closed.")

    # ── Worker thread ────────────────────────────────────────────────────

    def _open_table(self) -> None:
        """The table, read as app/point_table.py reads it for every point
        renderer: a VBT in place, or a catalogue (a sky one with its scene
        positions in ``@x``, ``@y``, ``@z``)."""
        from ..point_table import open_point_table
        self._table = open_point_table(self.dataset_path)
        self._names = self._table.names
        self._columns = self._table.columns
        self._num_rows = self._table.num_rows
        self._frame = self._table.frame
        self._sky = self._table.sky
        self.view.x, self.view.y, self.view.z = self._table.axes

    def _compute_sky_positions(self) -> None:
        self._table.set_sky_positions(self.view.sky_frame, self.view.distance_mode,
                                      self.view.cosmology)

    def _sample_rows(self) -> np.ndarray:
        """Every row, or an even sample of MAX_POINTS of them."""
        if self._num_rows <= MAX_POINTS:
            return np.arange(self._num_rows, dtype=np.int64)
        logger.info("[point-render %s] %d rows, drawing an even sample of %d",
                    self.session_id[:8], self._num_rows, MAX_POINTS)
        return np.linspace(0, self._num_rows - 1, MAX_POINTS).astype(np.int64)

    def _make_window(self):
        import vtk
        if self._window_kind is not None:   # EGL, or a hidden Cocoa window on a Mac
            window = new_offscreen_window(self._window_kind)
        else:   # neither (tests on a desktop): VTK's own offscreen window
            window = vtk.vtkRenderWindow()
            window.SetOffScreenRendering(1)
        window.SetSize(self.view.width, self.view.height)
        return window

    def _init_on_thread(self) -> None:
        import vtk
        t0 = time.time()
        self._open_table()
        self._polydata = vtk.vtkPolyData()
        self._lod_polydata = vtk.vtkPolyData()
        self._gauss_opacity = vtk.vtkPiecewiseFunction()
        for lod, poly in ((False, self._polydata), (True, self._lod_polydata)):
            points = vtk.vtkPolyDataMapper()
            # The Gaussian mapper draws the same arrays and VTK shares their
            # vertex buffer: stored shifted and scaled by the points mapper (it
            # does from about a million points), it drew the splats as a dot.
            points.SetVBOShiftScaleMethod(0)
            points.SetInputData(poly)
            gauss = vtk.vtkPointGaussianMapper()
            gauss.SetEmissive(True)
            # Splats drawn out to 3 σ. VTK before 9.4 (the POWER9 EGL image
            # builds 9.3.1) names it TriangleScale.
            (getattr(gauss, "SetBoundScale", None) or gauss.SetTriangleScale)(3.0)
            gauss.SetOpacityArray("VbtScalar")
            gauss.SetScalarOpacityFunction(self._gauss_opacity)
            gauss.SetInputData(poly)
            self._mappers[("points", lod)] = points
            self._mappers[("gaussian", lod)] = gauss
        self._actor = vtk.vtkActor()
        self._actor.GetProperty().SetRepresentationToPoints()
        self._renderer = vtk.vtkRenderer()
        self._renderer.AddActor(self._actor)
        # The labelled box of the desktop viewer (vtkCubeAxesActor).
        self._outline = vtk.vtkCubeAxesActor()
        self._outline.SetCamera(self._renderer.GetActiveCamera())
        self._outline.SetXTitle("X")
        self._outline.SetYTitle("Y")
        self._outline.SetZTitle("Z")
        self._outline.SetFlyModeToOuterEdges()
        for axis in "XYZ":
            getattr(self._outline, f"Draw{axis}GridlinesOff")()
        self._renderer.AddActor(self._outline)
        # The colour bar the desktop viewer shows, at the same place.
        self._colorbar = vtk.vtkScalarBarActor()
        self._colorbar.SetMaximumWidthInPixels(100)
        self._colorbar.SetPosition(0.88, 0.1)
        self._renderer.AddViewProp(self._colorbar)
        self._axes_renderer = vtk.vtkRenderer()
        self._axes_renderer.SetViewport(0.0, 0.0, 0.14, 0.14)
        self._axes_renderer.SetLayer(1)
        self._axes_renderer.InteractiveOff()
        self._axes_actor = vtk.vtkAxesActor()
        self._axes_renderer.AddActor(self._axes_actor)
        self._render_window = self._make_window()
        self._render_window.SetNumberOfLayers(2)
        self._renderer.SetLayer(0)
        self._render_window.AddRenderer(self._renderer)
        self._render_window.AddRenderer(self._axes_renderer)
        self._sample = self._sample_rows()
        self._rebuild_points_on_thread()
        self._apply_background_on_thread()
        self._renderer.ResetCamera()
        logger.info("[point-render %s] %d of %d rows ready in %.1f s", self.session_id[:8],
                    len(self._rows), self._num_rows, time.time() - t0)

    def _rebuild_points_on_thread(self) -> None:
        """Positions of the drawn rows (sample ∩ filters), then their colours."""
        import vtk
        from vtk.util.numpy_support import numpy_to_vtk
        rows = self._sample
        if self.view.filters:
            rows = rows[_filter_mask(self._columns, self.view.filters, rows)]
        self._rows = rows
        xyz = np.empty((len(rows), 3), dtype=np.float32)
        for i, name in enumerate((self.view.x, self.view.y, self.view.z)):
            xyz[:, i] = self._columns[name][rows] if name in self._columns else 0.0
        # A row without a position (no RA/Dec, a NaN coordinate) is not drawn,
        # as the desktop skips it when it parses the rows.
        placed = np.isfinite(xyz).all(axis=1)
        if not placed.all():
            rows, xyz = rows[placed], xyz[placed]
            self._rows = rows
        stride = max(1, -(-len(rows) // LOD_POINTS))
        for poly, pts in ((self._polydata, xyz), (self._lod_polydata, xyz[::stride])):
            points = vtk.vtkPoints()
            arr = numpy_to_vtk(np.ascontiguousarray(pts), deep=True)
            points.SetData(arr)
            poly.SetPoints(points)
            verts = vtk.vtkCellArray()
            n = len(pts)
            # One vertex cell per point, offsets 0..n and connectivity 0..n-1.
            verts.SetData(numpy_to_vtk(np.arange(n + 1, dtype=np.int64), deep=True),
                          numpy_to_vtk(np.arange(n, dtype=np.int64), deep=True))
            poly.SetVerts(verts)
        self._lod_stride = stride
        # The splats are sized on the points drawn with no filter: a filter
        # removes points, it does not restyle the others.
        if not self.view.filters or self._splat_basis is None:
            self._splat_basis = (self._polydata.GetBounds(), self._polydata.GetNumberOfPoints())
        self._outline.SetBounds(self._polydata.GetBounds())
        self._outline.SetVisibility(1 if self.view.show_outline and len(rows) else 0)
        self._recolour_on_thread(field_changed=True)

    def _recolour_on_thread(self, field_changed: bool = False) -> None:
        from vtk.util.numpy_support import numpy_to_vtk
        name = self.view.color_field
        if name and name in self._columns and len(self._rows):
            values = np.asarray(self._columns[name][self._rows], dtype=np.float32)
            finite = values[np.isfinite(values)]
            lo, hi = (float(finite.min()), float(finite.max())) if finite.size else (0.0, 1.0)
            if not hi > lo:
                hi = lo + 1.0
            self._data_range = (lo, hi)
            if field_changed or self.view.color_min is None or self.view.color_max is None:
                cmin = self.view.color_min if self.view.color_min is not None else lo
                cmax = self.view.color_max if self.view.color_max is not None else hi
            else:
                cmin, cmax = self.view.color_min, self.view.color_max
            if not cmax > cmin:
                cmin, cmax = lo, hi
        else:
            values = np.zeros(len(self._rows), dtype=np.float32)
            self._data_range, cmin, cmax = None, 0.0, 1.0
        for poly, vals in ((self._polydata, values), (self._lod_polydata,
                                                       values[::self._lod_stride])):
            scalars = numpy_to_vtk(np.ascontiguousarray(vals), deep=True)
            scalars.SetName("VbtScalar")
            poly.GetPointData().SetScalars(scalars)
        log = False
        if self.view.color_points:
            import vtk
            ctf = vtk.vtkColorTransferFunction()
            for x, r, g, b in self.view.color_points:
                ctf.AddRGBPoint(x, r, g, b)
            log = self.view.color_log and cmin > 0   # the client's log scale
            if log:
                ctf.SetScale(1)   # VTK_CTF_LOG10: interpolated as the client does
        else:
            ctf = _build_color_tf(self.view.lut, cmin, cmax)
        lut = _sampled_lut(ctf, cmin, cmax, log)
        coloured = self._data_range is not None
        for mapper in self._mappers.values():
            mapper.SetLookupTable(lut)
            mapper.SetScalarRange(cmin, cmax)
            mapper.SetScalarVisibility(1 if coloured else 0)
        self._actor.GetProperty().SetColor(*_SOLID)
        self._colorbar.SetLookupTable(lut)
        self._colorbar.SetTitle(name if coloured else "")
        self._colorbar.SetVisibility(1 if coloured and self.view.show_colorbar else 0)
        self._apply_style_on_thread()

    def _apply_style_on_thread(self) -> None:
        if self.view.mode == "gaussian":
            bounds, count = self._splat_basis
            radius, opacity = gaussian_splat(bounds, count, self.view.size, self.view.intensity)
            self._gauss_opacity.RemoveAllPoints()
            self._gauss_opacity.AddPoint(-1.0e15, opacity)
            self._gauss_opacity.AddPoint(1.0e15, opacity)
            for lod in (False, True):
                self._mappers[("gaussian", lod)].SetScaleFactor(radius)
        else:
            self._actor.GetProperty().SetPointSize(self.view.size)
        self._actor.SetMapper(self._mappers[(self.view.mode, self.view.interactive)])
        self._actor.GetProperty().SetOpacity(1.0)

    def _apply_background_on_thread(self) -> None:
        bg = _BACKGROUNDS.get(self.view.background, _BACKGROUNDS["Dark"])
        self._renderer.SetBackground(*bg)
        dark = bg[0] < 0.5
        title, label, line = (0.85, 0.75, 0.8) if dark else (0.1, 0.2, 0.15)
        for axis in range(3):
            self._outline.GetTitleTextProperty(axis).SetColor(title, title, title)
            self._outline.GetLabelTextProperty(axis).SetColor(label, label, label)
        for prop in (self._outline.GetXAxesLinesProperty(), self._outline.GetYAxesLinesProperty(),
                     self._outline.GetZAxesLinesProperty()):
            prop.SetColor(line, line, line)
        text = 1.0 if bg[0] < 0.5 else 0.1
        for prop in (self._colorbar.GetTitleTextProperty(),
                     self._colorbar.GetLabelTextProperty()):
            prop.SetColor(text, text, text)

    def _apply_command_on_thread(self, command: dict[str, Any]) -> None:
        ctype = command.get("type", "")
        cam = self._renderer.GetActiveCamera() if self._renderer is not None else None
        if ctype == "rotate" and cam is not None:
            cam.Azimuth(float(command.get("d_azimuth", 0.0)))
            cam.Elevation(float(command.get("d_elevation", 0.0)))
            cam.OrthogonalizeViewUp()
        elif ctype == "zoom" and cam is not None:
            cam.Zoom(max(0.05, min(20.0, float(command.get("factor", 1.0)))))
        elif ctype == "pan" and cam is not None:
            # Screen-aligned: the focal point and position move together in
            # the view plane, by a fraction of the view's height.
            dx, dy = float(command.get("dx", 0.0)), float(command.get("dy", 0.0))
            fp, pos, up = map(np.array, (cam.GetFocalPoint(), cam.GetPosition(),
                                         cam.GetViewUp()))
            right = np.cross(fp - pos, up)
            right /= np.linalg.norm(right) or 1.0
            scale = 2.0 * np.linalg.norm(fp - pos) * math.tan(math.radians(cam.GetViewAngle()) / 2)
            shift = (-dx * right + dy * up) * scale
            cam.SetFocalPoint(*(fp + shift))
            cam.SetPosition(*(pos + shift))
        elif ctype == "reset":
            self._renderer.ResetCamera()
        elif ctype == "set_camera" and cam is not None:
            # A camera given whole: the desktop's ray-traced view, turned
            # meanwhile, hands it back.
            set_camera(cam, command)
            self._renderer.ResetCameraClippingRange()
        elif ctype == "set_interactive":
            self.view.interactive = bool(command.get("value", False))
            self._apply_style_on_thread()
        elif ctype == "set_sky_positions":
            if self._sky is None:
                raise ValueError("Not a sky catalogue: its positions are columns.")
            from ..catalogue_positions import DISTANCE_MODES, FRAMES
            frame = str(command.get("frame", self.view.sky_frame))
            mode = str(command.get("distance_mode", self.view.distance_mode))
            cosmology = str(command.get("cosmology", self.view.cosmology))
            if frame not in FRAMES or mode not in DISTANCE_MODES:
                raise ValueError(f"Unknown frame or distance mode: {frame!r}, {mode!r}")
            if (frame, mode, cosmology) != (self.view.sky_frame, self.view.distance_mode,
                                            self.view.cosmology):
                previous = (self.view.sky_frame, self.view.distance_mode, self.view.cosmology)
                self.view.sky_frame, self.view.distance_mode, self.view.cosmology = (
                    frame, mode, cosmology)
                try:
                    self._compute_sky_positions()
                except ValueError:
                    (self.view.sky_frame, self.view.distance_mode,
                     self.view.cosmology) = previous
                    raise
                self._splat_basis = None
                self._rebuild_points_on_thread()
                self._renderer.ResetCamera()
                self.request_idr()
        elif ctype == "set_point_fields":
            fields = {axis: command[axis] for axis in ("x", "y", "z")
                      if command.get(axis) is not None}
            for name in fields.values():
                if name not in self._columns:
                    raise ValueError(f"Unknown field: {name!r}")
            if any(getattr(self.view, axis) != name for axis, name in fields.items()):
                for axis, name in fields.items():
                    setattr(self.view, axis, name)
                self._splat_basis = None   # new positions: sized again
                self._rebuild_points_on_thread()
                self._renderer.ResetCamera()
                self.request_idr()
        elif ctype == "set_color":
            name = command.get("field") or None
            if name is not None and name not in self._columns:
                raise ValueError(f"Unknown field: {name!r}")
            cmin = None if command.get("min") is None else float(command["min"])
            cmax = None if command.get("max") is None else float(command["max"])
            points = (_parse_tf_points(command["points"], 4)
                      if command.get("points") else None)
            changed = name != self.view.color_field
            self.view.color_field = name
            self.view.lut = str(command.get("lut", self.view.lut))
            self.view.color_min, self.view.color_max = cmin, cmax
            self.view.color_points = points
            self.view.color_log = bool(command.get("log", False))
            self._recolour_on_thread(field_changed=changed)
            self.request_idr()
        elif ctype == "set_point_style":
            mode = str(command.get("mode", self.view.mode))
            self.view.mode = mode if mode in ("points", "gaussian") else "points"
            # Points: pixels, physical (a Retina client sends twice its own);
            # splats: the size step, 1..12.
            size = float(command.get("size", self.view.size))
            self.view.size = (min(max(size, 0.25), 12.0) if self.view.mode == "gaussian"
                              else int(round(min(max(size, 1.0), 64.0))))
            self.view.intensity = max(1, min(100, int(command.get("intensity",
                                                                  self.view.intensity))))
            self._apply_style_on_thread()
            self.request_idr()
        elif ctype == "set_filters":
            filters = [dict(f) for f in command.get("filters", [])]
            _filter_mask(self._columns, filters, self._sample[:1])   # validate first
            if filters != self.view.filters:
                self.view.filters = filters
                self._rebuild_points_on_thread()
                self.request_idr()
        elif ctype == "set_background":
            self.view.background = str(command.get("name", "Dark"))
            self._apply_background_on_thread()
        elif ctype == "set_show_outline":
            self.view.show_outline = bool(command.get("visible", True))
            self._outline.SetVisibility(1 if self.view.show_outline else 0)
        elif ctype == "set_show_colorbar":
            self.view.show_colorbar = bool(command.get("visible", True))
            self._colorbar.SetVisibility(
                1 if self.view.show_colorbar and self._data_range is not None else 0)
        elif ctype == "set_show_axes":
            self.view.show_axes = bool(command.get("visible", True))
            self._axes_actor.SetVisibility(1 if self.view.show_axes else 0)
        elif ctype == "resize":
            self.view.width = max(64, min(4096, int(command.get("width", self.view.width))))
            self.view.height = max(64, min(4096, int(command.get("height", self.view.height))))
            self._render_window.SetSize(self.view.width, self.view.height)
        elif ctype == "jpeg_quality":
            self.view.jpeg_quality = max(1, min(100, int(command.get("quality", 85))))
        elif ctype == "keepalive":
            pass
        else:
            logger.warning("[point-render %s] unknown command type %r",
                           self.session_id[:8], ctype)

    def _render_rgb_on_thread(self):
        import vtk
        from vtk.util.numpy_support import vtk_to_numpy
        if self._render_window is None:
            raise RuntimeError(f"Session {self.session_id} not initialised.")
        cam = self._renderer.GetActiveCamera()
        axes_cam = self._axes_renderer.GetActiveCamera()
        pos, fp = np.array(cam.GetPosition()), np.array(cam.GetFocalPoint())
        d = pos - fp
        d /= np.linalg.norm(d) or 1.0
        axes_cam.SetFocalPoint(0.0, 0.0, 0.0)
        axes_cam.SetPosition(*d)
        axes_cam.SetViewUp(cam.GetViewUp())
        self._axes_renderer.ResetCamera()
        self._renderer.ResetCameraClippingRange()
        t0 = time.perf_counter()
        self._render_window.Render()
        render_ms = (time.perf_counter() - t0) * 1000.0
        w2if = vtk.vtkWindowToImageFilter()
        w2if.SetInput(self._render_window)
        w2if.ReadFrontBufferOff()
        w2if.SetInputBufferTypeToRGBA()
        w2if.Update()
        arr = vtk_to_numpy(w2if.GetOutput().GetPointData().GetScalars())
        w, h = self._render_window.GetSize()
        return arr.reshape((h, w, 4)), render_ms

    def _render_frame_on_thread(self) -> bytes:
        rgba, render_ms = self._render_rgb_on_thread()
        self._scene_dirty = False
        self._last_rgba, self._last_render_ms = rgba, render_ms
        h, w = rgba.shape[:2]
        img = Image.frombuffer("RGBX", (w, h), rgba, "raw", "RGBX", 0, -1)
        buf = io.BytesIO()
        quality = (min(self.view.jpeg_quality, _LOD_JPEG_QUALITY)
                   if self.view.interactive else self.view.jpeg_quality)
        img.save(buf, format="JPEG", quality=quality)
        jpeg = buf.getvalue()
        self.stats.frames_rendered += 1
        self.stats.last_render_ms = render_ms
        self.stats.last_jpeg_kb = len(jpeg) / 1024.0
        self.stats.bytes_sent += len(jpeg)
        return jpeg

    def _close_on_thread(self) -> None:
        if self._render_window is not None:
            self._render_window.Finalize()
        self._render_window = self._renderer = None
        self._columns = {}
