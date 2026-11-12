"""Stereo frames of a point table for a headset's pose (issue #190, VR epic
#166): the point session's counterpart of ``_stereo.py``.

The protocol is the cube session's: ``xr_pose`` (checked by
:func:`app.render_session._stereo.parse_pose`) switches the session to stereo
frames for that pose, ``xr_stop`` back to its mono view, and each frame is one
``VXR1`` message, both eyes side by side as a JPEG with a depth image. Only the
placement differs. A point table has no voxel grid: the VR page draws it in a
box of its own (centred on the origin, its longest side 1, from the bounds of
the rows it drew), so the pose carries that map, ``data_to_model``: 16 numbers,
column-major, from the table's coordinates to the page's model space, a
uniform scale and a translation. Without it the session uses the same rule on
its own rows' bounds.

The session draws every row it holds (render_session/points.py: all of them,
or an even sample of ``VISIVO_POINT_RENDER_MAX_POINTS``), never the drag's
sample, with the colours, style and filters the page sent as ordinary
commands. Depth: points write the depth buffer, so their nearest point per
block is sent, as a surface's; additive splats (``gaussian``) write none, and
the frame is sent without depth (the page reprojects it as a plane).
"""
from __future__ import annotations

import io
import logging
import time
from typing import Any

import numpy as np

from ._stereo import (XRFrame, _matrix, clip_range, encode_depth, eye_distance,
                      nearest_per_block, parse_pose, with_depth_range)

logger = logging.getLogger("visivo.render")


def default_data_to_model(bounds) -> np.ndarray:
    """The VR page's box for these bounds (xmin, xmax, ymin, ymax, zmin, zmax):
    centred on the origin, its longest side 1 (static/vr/js/points.js)."""
    lo = np.array(bounds[0::2], dtype=float)
    hi = np.array(bounds[1::2], dtype=float)
    side = float(max(np.max(hi - lo), 1e-30))
    out = np.diag([1.0 / side] * 3 + [1.0])
    out[:3, 3] = -0.5 * (lo + hi) / side
    return out


def parse_data_to_model(value: Any) -> np.ndarray:
    """``data_to_model`` checked: a uniform positive scale and a translation
    (a rotation too is accepted), as 16 column-major numbers."""
    m = _matrix(value, "data_to_model")
    r = m[:3, :3]
    det = float(np.linalg.det(r))
    if not det > 1e-300 or not np.allclose(m[3], [0.0, 0.0, 0.0, 1.0], atol=1e-9):
        raise ValueError("data_to_model must be a uniform scale and a translation.")
    s = np.cbrt(det)
    if not np.allclose((r / s) @ (r / s).T, np.eye(3), atol=1e-6):
        raise ValueError("data_to_model must be a uniform scale and a translation.")
    return m


def point_eye(view: np.ndarray, projection: np.ndarray, placed: np.ndarray, bounds) -> dict:
    """One eye in the table's own coordinates: the rigid view (data units), a
    projection that scales back to metres, and a depth range spanning the
    rows' bounding box. ``placed`` maps data to world (model @ data_to_model)."""
    ev = view @ placed
    s = float(np.cbrt(np.linalg.det(ev[:3, :3])))
    if not s > 0:
        raise ValueError("degenerate placement")
    rigid = ev.copy()
    rigid[:3, :] /= s
    xs, ys, zs = (bounds[0], bounds[1]), (bounds[2], bounds[3]), (bounds[4], bounds[5])
    corners = np.array([[x, y, z, 1.0] for x in xs for y in ys for z in zs])
    ahead = -(rigid @ corners.T)[2] * s                  # metres along the eye's -z
    near_m = clip_range(projection)[0]
    far_m = max(float(ahead.max()) * 1.01 + 0.01, 2.0 * near_m)
    depth_projection = with_depth_range(projection, near_m, far_m)
    return {"view": rigid, "scale": s,
            "projection": depth_projection @ np.diag([s, s, s, 1.0]),
            "clipping": (near_m / s, far_m / s), "depth_projection": depth_projection}


def _vtk_matrix(m: np.ndarray):
    import vtk
    out = vtk.vtkMatrix4x4()
    for i in range(4):
        for j in range(4):
            out.SetElement(i, j, float(m[i, j]))
    return out


class PointStereoMixin:
    """``xr_pose`` / ``xr_stop`` and the stereo frame of the point session."""

    _xr_pose = None
    _xr_error = None
    _xr_d2m = None
    _xr_camera = None

    def _apply_xr_command_on_thread(self, command: dict[str, Any]) -> None:
        if command.get("type") == "xr_pose":
            try:
                pose = parse_pose(command)
                d2m = (parse_data_to_model(command["data_to_model"])
                       if command.get("data_to_model") is not None else None)
                self._xr_pose, self._xr_d2m, self._xr_error = pose, d2m, None
            except ValueError as exc:
                self._xr_error = (command.get("pose_id"), str(exc))
        elif command.get("type") == "xr_stop":
            self._xr_pose = self._xr_d2m = self._xr_error = None
            if self._render_window is not None:
                self._render_window.SetSize(self.view.width, self.view.height)

    def _xr_active(self) -> bool:
        return self._xr_pose is not None or self._xr_error is not None

    def _render_xr_frame_on_thread(self) -> XRFrame:
        if self._xr_error is not None:
            pose_id, message = self._xr_error
            self._xr_error = None
            return XRFrame({"pose_id": pose_id}, error=message)
        pose = self._xr_pose
        base = {"v": 1, "pose_id": pose.pose_id, "t_client": pose.t_client}
        if self._polydata is None or self._polydata.GetNumberOfPoints() == 0:
            return XRFrame(base, error="Nothing to draw: every row is filtered out.")
        t_start = time.time()
        try:
            eyes, per_eye, depth_src = self._render_point_eyes_on_thread(pose)
        except ValueError as exc:
            # A placement that cannot be drawn (a degenerate scale): said to
            # the page, the stream goes on.
            self._xr_pose = None
            return XRFrame(base, error=f"This pose cannot be drawn: {exc}")
        t_render = time.time()
        w, h = pose.width, pose.height
        depth_info = depth_payload = None
        if pose.depth_scale is not None and depth_src != "none":
            dist = np.concatenate([eye_distance(nearest_per_block(d, pose.depth_scale), proj)
                                   for _, d, proj in eyes], axis=1)
            depth_payload, z0, z1 = encode_depth(dist[::-1])
            depth_info = {"format": "u16-zlib", "width": int(dist.shape[1]),
                          "height": int(dist.shape[0]), "scale": pose.depth_scale,
                          "z_near": z0, "z_far": z1, "source": depth_src,
                          "units": "m, along each eye's -z"}
        t_depth = time.time()
        from PIL import Image
        rgba = np.ascontiguousarray(np.concatenate([c for c, _, _ in eyes], axis=1))
        img = Image.frombuffer("RGBX", (2 * w, h), rgba, "raw", "RGBX", 0, -1)
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=pose.quality or self.view.jpeg_quality,
                 optimize=False, progressive=False)
        jpeg = buf.getvalue()
        t_encode = time.time()
        header = dict(base)
        header.update({
            "eyes": [{"eye": "left", "x": 0, "y": 0, "width": w, "height": h},
                     {"eye": "right", "x": w, "y": 0, "width": w, "height": h}],
            "color": {"format": "jpeg", "width": 2 * w, "height": h},
            "depth": depth_info,
            "timing": {"received": pose.t_received, "render_start": t_start,
                       "render_end": t_render, "depth_end": t_depth, "encode_end": t_encode},
            "eye_ms": per_eye,
            "points": int(self._polydata.GetNumberOfPoints()),
        })
        self.stats.frames_rendered += 1
        self.stats.last_render_ms = 1000.0 * (t_render - t_start)
        self.stats.last_jpeg_kb = len(jpeg) / 1024.0
        self.stats.bytes_sent += len(jpeg) + len(depth_payload or b"")
        return XRFrame(header, jpeg, depth_payload)

    def _render_point_eyes_on_thread(self, pose):
        """``([(rgba bottom-up, window depth or None, its projection)], per-eye
        timings, depth source)``; the mono view is put back afterwards."""
        import vtk
        from vtk.util.numpy_support import vtk_to_numpy
        ren, win = self._renderer, self._render_window
        w, h = pose.width, pose.height
        if tuple(win.GetSize()) != (w, h):
            win.SetSize(w, h)
        bounds = self._polydata.GetBounds()
        d2m = self._xr_d2m if self._xr_d2m is not None else default_data_to_model(bounds)
        placed = pose.model @ d2m
        cameras = [point_eye(e.view, e.projection, placed, bounds) for e in pose.eyes]
        points_mode = self.view.mode != "gaussian"
        want_depth = pose.depth_scale is not None and points_mode
        # Every row (not the drag's sample), without the mono view's chrome.
        mapper = self._actor.GetMapper()
        full = self._mappers[(self.view.mode, False)]
        hidden = [p for p in (self._outline, self._colorbar)
                  if p is not None and p.GetVisibility()]
        axes_drawn = self._axes_renderer.GetDraw() if self._axes_renderer is not None else None
        mono_cam, mono_bg = ren.GetActiveCamera(), ren.GetBackground()
        if self._xr_camera is None:
            self._xr_camera = vtk.vtkCamera()
            self._xr_camera.UseExplicitProjectionTransformMatrixOn()
        cam = self._xr_camera
        out, per_eye = [], []
        try:
            self._actor.SetMapper(full)
            for p in hidden:
                p.SetVisibility(0)
            if self._axes_renderer is not None:
                self._axes_renderer.SetDraw(0)
            ren.SetBackground(*pose.background)
            ren.SetActiveCamera(cam)
            for eye, g in zip(pose.eyes, cameras):
                inv = np.linalg.inv(g["view"])
                pos = inv[:3, 3]
                cam.SetPosition(*pos.tolist())
                cam.SetFocalPoint(*(pos - inv[:3, 2]).tolist())
                cam.SetViewUp(*inv[:3, 1].tolist())
                cam.SetExplicitProjectionTransformMatrix(_vtk_matrix(g["projection"]))
                cam.SetClippingRange(*g["clipping"])
                t0 = time.perf_counter()
                win.Render()
                t1 = time.perf_counter()
                grab = vtk.vtkWindowToImageFilter()
                grab.SetInput(win)
                grab.ReadFrontBufferOff()
                grab.SetInputBufferTypeToRGBA()
                grab.Update()
                rgba = vtk_to_numpy(grab.GetOutput().GetPointData().GetScalars()).reshape(h, w, 4)
                depth = None
                if want_depth:
                    z = vtk.vtkFloatArray()
                    win.GetZbufferData(0, 0, w - 1, h - 1, z)
                    depth = np.array(vtk_to_numpy(z).reshape(h, w), dtype=np.float32)
                t2 = time.perf_counter()
                out.append((np.array(rgba), depth, g["depth_projection"]))
                per_eye.append({"eye": eye.eye, "render": round(1000 * (t1 - t0), 3),
                                "readback": round(1000 * (t2 - t1), 3)})
        finally:
            ren.SetActiveCamera(mono_cam)
            ren.SetBackground(*mono_bg)
            self._actor.SetMapper(mapper)
            for p in hidden:
                p.SetVisibility(1)
            if axes_drawn is not None:
                self._axes_renderer.SetDraw(axes_drawn)
        return out, per_eye, ("surface" if want_depth else "none")
