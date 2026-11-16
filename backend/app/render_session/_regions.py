"""The desktop's region list on the cutting plane of a render session (#356).

The desktop draws its regions on the 3-D view's cutting plane (#343). With the
view rendered here (Remote rendering) the session draws them, so they sit in
the frames it sends, behind or in front of the volume by its own depth buffer,
and never lag the camera the way a drawing laid over the stream on the client
would.

The client sends the drawing, not the regions: closed outlines, open paths,
point markers and labels, in the cube's full-resolution 0-based pixels, as its
own overlay draws them (``set_regions``). The session places pixel (x, y) at
world (x, y, z) with z the plane's channel: its world is the cube's voxels, the
volume's voxel i at i, which is where the desktop puts the same pixel at full
resolution. The plane is spanned from -0.5 to n - 0.5 so its texel i is
centred there too.

``parse_regions`` checks a payload without VTK; ``RegionsMixin`` builds the
actors on the render thread.
"""
from __future__ import annotations

import math
from typing import Any

# A payload larger than this is refused: a region file of a few thousand
# shapes is a few hundred thousand points, and the plane is drawn every frame.
MAX_POINTS = 500_000
MAX_LABEL = 200
# Each label is a text actor of its own, laid out every frame.
MAX_LABELS = 2_000
# Lines and markers pulled this many depth units towards the camera so the
# textured plane they lie on never hides them (the desktop's lineOffsetUnits).
LINE_OFFSET_UNITS = 4.0
LINE_WIDTH = 2.0
SELECTED_LINE_WIDTH = 3.5
POINT_SIZE = 11.0


def _point(p: Any) -> tuple[float, float]:
    if (not isinstance(p, (list, tuple)) or len(p) != 2
            or any(isinstance(v, bool) or not isinstance(v, (int, float)) for v in p)):
        raise ValueError(f"a point is [x, y] in numbers, got {p!r}")
    x, y = float(p[0]), float(p[1])
    if not (math.isfinite(x) and math.isfinite(y)):
        raise ValueError("a point must be finite")
    return x, y


def _polylines(value: Any, name: str) -> list[list[tuple[float, float]]]:
    if value is None:
        return []
    if not isinstance(value, list):
        raise ValueError(f"{name} is a list of polylines")
    out = []
    for line in value:
        if not isinstance(line, list):
            raise ValueError(f"{name}: a polyline is a list of points")
        pts = [_point(p) for p in line]
        if len(pts) >= 2:
            out.append(pts)
    return out


def _rgb(value: Any) -> tuple[float, float, float]:
    if (not isinstance(value, list) or len(value) != 3
            or any(isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 255
                   for v in value)):
        raise ValueError(f"rgb is three integers 0..255, got {value!r}")
    return value[0] / 255.0, value[1] / 255.0, value[2] / 255.0


def parse_regions(command: dict[str, Any]) -> list[dict[str, Any]]:
    """The regions of a ``set_regions`` command, checked. Raises ValueError.

    Each region: ``rgb`` [r, g, b] (0..255), ``selected`` (drawn thicker),
    ``loops`` (closed outlines), ``paths`` (open polylines), ``points``
    (markers) and ``labels`` ([{"text", "at": [x, y]}]), all optional but rgb.
    """
    regions = command.get("regions")
    if not isinstance(regions, list):
        raise ValueError("regions is a list")
    out = []
    total = 0
    n_labels = 0
    for r in regions:
        if not isinstance(r, dict):
            raise ValueError("a region is an object")
        loops = _polylines(r.get("loops"), "loops")
        paths = _polylines(r.get("paths"), "paths")
        for key in ("points", "labels"):
            if r.get(key) is not None and not isinstance(r.get(key), list):
                raise ValueError(f"{key} is a list")
        points = [_point(p) for p in (r.get("points") or [])]
        labels = []
        for lab in r.get("labels") or []:
            if not isinstance(lab, dict) or not isinstance(lab.get("text"), str):
                raise ValueError("a label is {text, at}")
            labels.append((lab["text"][:MAX_LABEL], _point(lab.get("at"))))
        n_labels += len(labels)
        if n_labels > MAX_LABELS:
            raise ValueError(f"more than {MAX_LABELS} labels")
        total += sum(len(p) for p in loops) + sum(len(p) for p in paths) + len(points)
        if total > MAX_POINTS:
            raise ValueError(f"more than {MAX_POINTS} points")
        out.append({"rgb": _rgb(r.get("rgb")), "selected": bool(r.get("selected", False)),
                    "loops": loops, "paths": paths, "points": points, "labels": labels})
    return out


class RegionsMixin:
    """The region actors, built and placed on the session's render thread."""

    def _apply_regions_on_thread(self, regions: list[dict[str, Any]]) -> None:
        """Replace the drawing with @p regions (already checked)."""
        import vtk

        if self._renderer is None:
            return
        self._remove_region_actors_on_thread()

        def line_actor(selected: bool):
            pts = vtk.vtkPoints()
            cells = vtk.vtkCellArray()
            colours = vtk.vtkUnsignedCharArray()
            colours.SetNumberOfComponents(3)
            for r in regions:
                if r["selected"] != selected:
                    continue
                rgb = tuple(int(round(c * 255)) for c in r["rgb"])
                for closed, lines in ((True, r["loops"]), (False, r["paths"])):
                    for line in lines:
                        first = pts.GetNumberOfPoints()
                        for x, y in line:
                            pts.InsertNextPoint(x, y, 0.0)
                        cells.InsertNextCell(len(line) + (1 if closed else 0))
                        for i in range(len(line)):
                            cells.InsertCellPoint(first + i)
                        if closed:
                            cells.InsertCellPoint(first)
                        colours.InsertNextTuple3(*rgb)
            if cells.GetNumberOfCells() == 0:
                return None
            poly = vtk.vtkPolyData()
            poly.SetPoints(pts)
            poly.SetLines(cells)
            poly.GetCellData().SetScalars(colours)
            return self._region_actor(poly, SELECTED_LINE_WIDTH if selected else LINE_WIDTH)

        actors = [a for a in (line_actor(False), line_actor(True)) if a is not None]

        pts = vtk.vtkPoints()
        verts = vtk.vtkCellArray()
        colours = vtk.vtkUnsignedCharArray()
        colours.SetNumberOfComponents(3)
        for r in regions:
            rgb = tuple(int(round(c * 255)) for c in r["rgb"])
            for x, y in r["points"]:
                verts.InsertNextCell(1)
                verts.InsertCellPoint(pts.InsertNextPoint(x, y, 0.0))
                colours.InsertNextTuple3(*rgb)
        if verts.GetNumberOfCells():
            poly = vtk.vtkPolyData()
            poly.SetPoints(pts)
            poly.SetVerts(verts)
            poly.GetCellData().SetScalars(colours)
            marker = self._region_actor(poly, 1.0)
            marker.GetProperty().SetPointSize(POINT_SIZE)
            actors.append(marker)

        labels = []
        for r in regions:
            for text, (x, y) in r["labels"]:
                t = vtk.vtkTextActor()
                t.SetInput(text)
                t.GetPositionCoordinate().SetCoordinateSystemToWorld()
                tp = t.GetTextProperty()
                tp.SetJustificationToCentered()
                tp.SetVerticalJustificationToBottom()
                tp.SetFontSize(13)
                tp.ShadowOn()
                tp.SetColor(*r["rgb"])
                tp.SetBold(r["selected"])
                t.PickableOff()
                labels.append((t, x, y))

        for a in actors:
            self._renderer.AddActor(a)
        for t, _, _ in labels:
            self._renderer.AddViewProp(t)
        self._region_actors = actors
        self._region_labels = labels
        self._place_regions_on_thread()

    def _region_actor(self, poly, width: float):
        import vtk

        mapper = vtk.vtkPolyDataMapper()
        mapper.SetInputData(poly)
        mapper.SetScalarModeToUseCellData()
        mapper.SetColorModeToDirectScalars()
        mapper.SetRelativeCoincidentTopologyLineOffsetParameters(0.0, -LINE_OFFSET_UNITS)
        mapper.SetRelativeCoincidentTopologyPointOffsetParameter(-LINE_OFFSET_UNITS)
        actor = vtk.vtkActor()
        actor.SetMapper(mapper)
        prop = actor.GetProperty()
        prop.SetLineWidth(width)
        prop.LightingOff()
        actor.PickableOff()
        return actor

    def _remove_region_actors_on_thread(self) -> None:
        if self._renderer is None:
            return
        for a in self._region_actors:
            self._renderer.RemoveActor(a)
        for t, _, _ in self._region_labels:
            self._renderer.RemoveViewProp(t)
        self._region_actors = []
        self._region_labels = []

    def _place_regions_on_thread(self) -> None:
        """At the plane's channel, shown when the plane is: read off the plane
        actor, which the drag level of detail hides while the view moves."""
        index = getattr(self, "_cutting_plane_slice_index", -1)
        z = float(max(0, index))
        plane = getattr(self, "_cutting_plane_actor", None)
        shown = plane is not None and bool(plane.GetVisibility()) and index >= 0
        for a in getattr(self, "_region_actors", []):
            a.SetPosition(0.0, 0.0, z)
            a.SetVisibility(1 if shown else 0)
        for t, x, y in getattr(self, "_region_labels", []):
            t.GetPositionCoordinate().SetValue(x, y, z)
            t.SetVisibility(1 if shown else 0)
