"""The VR page's free clipping plane on a render session (issue #369).

The page cuts a simulation volume with a plane (or a slab of two parallel
planes) and draws the section on it; its refined frames must show the same
cut, so the session clips the volume with the plane and, in a stereo frame,
composites the same section over the volume.

Conventions (as ``set_crop`` and the desktop's cutting plane): the cube's
voxel coordinates, 0-based, voxel *i* centred on *i* along x (NAXIS1), y
(NAXIS2) and z. ``origin`` is a point of the plane, ``normal`` a vector in
the same coordinates (made unit here); the plane keeps the voxels *v* with
``normal · (v - origin) >= 0``, the side its normal points to. A slab
(``thickness`` given, in voxels) keeps ``|normal · (v - origin)| <=
thickness / 2`` instead. The section is the volume's value where a ray
enters the kept part through the plane, coloured by the session's colour
transfer function, drawn with ``section_opacity`` where the opacity transfer
function is above zero (the page's threshold), in composite blending only;
the page's shader does the same (``static/vr/js/clip-plane.js``).
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import numpy as np

#: The section's opacity when the page does not send one (its SECTION_ALPHA).
DEFAULT_SECTION_OPACITY = 0.85


@dataclass(frozen=True)
class ClipPlane:
    """A plane in the cube's voxel coordinates (see the module doc)."""
    origin: tuple[float, float, float]
    normal: tuple[float, float, float]       # unit
    half: float | None                       # a slab's half thickness (voxels), or None
    section_opacity: float

    def signed(self, points: np.ndarray) -> np.ndarray:
        """Signed distance (voxels) of ``points`` (..., 3) from the plane."""
        return (np.asarray(points, dtype=np.float64) - np.asarray(self.origin)) @ np.asarray(self.normal)

    def keeps(self, points: np.ndarray) -> np.ndarray:
        s = self.signed(points)
        return np.abs(s) <= self.half if self.half is not None else s >= 0.0


def _number(v: Any) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def parse_clip_plane(spec: Any) -> ClipPlane | None:
    """``set_clip_plane``'s ``plane``: ``{"origin": [3], "normal": [3],
    "thickness": number | null, "section_opacity": 0..1}`` or null (no
    plane). ValueError for anything malformed: a normal of zero length,
    numbers that are not finite, a thickness not above zero."""
    if spec is None:
        return None
    if not isinstance(spec, dict):
        raise ValueError('plane must be {"origin": [x, y, z], "normal": [x, y, z]} or null')
    origin, normal = spec.get("origin"), spec.get("normal")
    for name, v in (("origin", origin), ("normal", normal)):
        if not isinstance(v, (list, tuple)) or len(v) != 3 or not all(_number(x) for x in v):
            raise ValueError(f"plane.{name} must be three numbers")
    o = np.asarray(origin, dtype=np.float64)
    n = np.asarray(normal, dtype=np.float64)
    if not (np.isfinite(o).all() and np.isfinite(n).all()):
        raise ValueError("plane.origin and plane.normal must be finite")
    length = float(np.linalg.norm(n))
    if not length > 1e-9:
        raise ValueError("plane.normal must not be zero")
    n = n / length
    thickness = spec.get("thickness")
    half = None
    if thickness is not None:
        if not _number(thickness) or not np.isfinite(thickness) or not thickness > 0:
            raise ValueError("plane.thickness must be a number above zero, or null")
        half = float(thickness) / 2.0
    opacity = spec.get("section_opacity", DEFAULT_SECTION_OPACITY)
    if not _number(opacity) or not 0.0 <= float(opacity) <= 1.0:
        raise ValueError("plane.section_opacity must be a number from 0 to 1")
    return ClipPlane(tuple(float(x) for x in o), tuple(float(x) for x in n), half, float(opacity))


def world_planes(clip: ClipPlane | None, scale=(1.0, 1.0, 1.0)) -> list[tuple[tuple, tuple]]:
    """VTK's clipping planes ``[(origin, normal), ...]`` for ``clip`` in a
    world where voxel *v* is at ``scale * v`` (a prop's diagonal user matrix:
    a stereo frame's spectral stretch or a simulation's voxel aspect). VTK
    keeps the side a plane's normal points to; a slab is two planes facing
    each other. Points map by the scale, normals by its inverse."""
    if clip is None:
        return []
    s = np.asarray(scale, dtype=np.float64)
    o = np.asarray(clip.origin) * s
    n = np.asarray(clip.normal) / s
    n = n / np.linalg.norm(n)
    if clip.half is None:
        return [(tuple(o), tuple(n))]
    # The half thickness along the world normal: the voxel normal's unit step
    # is 1 / |normal / scale| world units long there.
    h = clip.half / float(np.linalg.norm(np.asarray(clip.normal) / s))
    return [(tuple(o - h * n), tuple(n)), (tuple(o + h * n), tuple(-n))]


def vtk_planes(clip: ClipPlane | None, scale=(1.0, 1.0, 1.0)) -> list:
    """``world_planes`` as vtkPlane objects."""
    import vtk  # noqa: PLC0415
    out = []
    for origin, normal in world_planes(clip, scale):
        p = vtk.vtkPlane()
        p.SetOrigin(*origin)
        p.SetNormal(*normal)
        out.append(p)
    return out


def set_mapper_planes(mapper, planes: list) -> None:
    """Replace a volume mapper's clipping planes with ``planes`` (vtkPlane)."""
    if mapper is None or not hasattr(mapper, "RemoveAllClippingPlanes"):
        return
    mapper.RemoveAllClippingPlanes()
    for p in planes:
        mapper.AddClippingPlane(p)


# ── The section in a stereo frame ───────────────────────────────────────────


def _trilinear(volume: np.ndarray, idx: np.ndarray) -> np.ndarray:
    """``volume`` (z, y, x) at fractional sample indices ``idx`` (N, 3: x, y,
    z), clamped to the samples (VTK's linear interpolation inside the grid)."""
    nz, ny, nx = volume.shape
    hi = np.array([nx - 1, ny - 1, nz - 1], dtype=np.float64)
    f = np.clip(idx, 0.0, hi)
    i0 = np.floor(f).astype(np.int64)
    i0 = np.minimum(i0, np.maximum(hi.astype(np.int64) - 1, 0))
    w = f - i0
    i1 = np.minimum(i0 + 1, hi.astype(np.int64))
    out = np.zeros(len(f), dtype=np.float64)
    for dz in (0, 1):
        z = i1[:, 2] if dz else i0[:, 2]
        wz = w[:, 2] if dz else 1.0 - w[:, 2]
        for dy in (0, 1):
            y = i1[:, 1] if dy else i0[:, 1]
            wy = w[:, 1] if dy else 1.0 - w[:, 1]
            for dx in (0, 1):
                x = i1[:, 0] if dx else i0[:, 0]
                wx = w[:, 0] if dx else 1.0 - w[:, 0]
                out += wz * wy * wx * volume[z, y, x]
    return out


def tf_colour(points, values: np.ndarray) -> np.ndarray:
    """The colour transfer function ``points`` ([[x, r, g, b], ...]) at
    ``values``, linear between points and clamped past the ends (VTK's)."""
    pts = np.asarray(sorted(points, key=lambda p: p[0]), dtype=np.float64)
    return np.stack([np.interp(values, pts[:, 0], pts[:, c]) for c in (1, 2, 3)], axis=-1)


def tf_threshold(points) -> float:
    """The lowest value the opacity transfer function ``points`` ([[x, a],
    ...]) draws: the first point above zero (the page puts one at the
    threshold, a zero just below it); -inf when the first point is above zero."""
    pts = sorted(points, key=lambda p: p[0])
    for i, (x, a) in enumerate(pts):
        if a > 0:
            return float("-inf") if i == 0 else float(x)
    return float("inf")


def section_layer(clip: ClipPlane, eye: dict, size: tuple[int, int], shape_xyz,
                  sample, colour_points, opacity_points, crop=None):
    """The section of one stereo eye: ``(rgba, window_depth)``, premultiplied
    float RGBA (h, w, 4) and the window depth of the section (h, w; 1 where
    there is none), rows bottom-up as VTK reads them; or None when the eye
    sees no section.

    ``eye`` is :func:`_stereo.stereo_eye`'s (its view, stretch and
    projection: voxel *v* is at ``stretch @ v`` in the world the rigid view
    looks at). ``sample`` is ``(array z, y, x, origin, spacing)``, the volume
    drawn, in voxel coordinates. ``crop``: the clipping box's planes
    ``(x0, x1, y0, y1, z0, z1)`` (``_pipeline.crop_planes``), or None.
    A pixel shows the section where its ray, from the eye's near plane, enters
    the kept part through a face of the plane, inside the cube and the box,
    at a value the opacity transfer function draws."""
    w, h = size
    proj = np.asarray(eye["projection"], dtype=np.float64)
    view = np.asarray(eye["view"], dtype=np.float64)
    stretch = np.asarray(eye["stretch"], dtype=np.float64)
    # Pixel centres in NDC, rows bottom-up.
    xs = (np.arange(w) + 0.5) / w * 2.0 - 1.0
    ys = (np.arange(h) + 0.5) / h * 2.0 - 1.0
    X, Y = np.meshgrid(xs, ys)
    inv = np.linalg.inv(stretch) @ np.linalg.inv(view) @ np.linalg.inv(proj)   # clip -> voxels
    ones = np.ones_like(X)

    def unproject(z):
        q = np.stack([X, Y, z * ones, ones], axis=-1) @ inv.T
        return q[..., :3] / q[..., 3:4]

    near, far = unproject(-1.0), unproject(1.0)
    d = far - near
    n = np.asarray(clip.normal)
    s0 = (near - np.asarray(clip.origin)) @ n
    sd = d @ n
    with np.errstate(divide="ignore", invalid="ignore"):
        if clip.half is None:
            t = np.where((s0 < 0) & (sd > 0), -s0 / sd, np.nan)
        else:
            t = np.where((s0 < -clip.half) & (sd > 0), (-clip.half - s0) / sd,
                         np.where((s0 > clip.half) & (sd < 0), (clip.half - s0) / sd, np.nan))
    hit = np.isfinite(t) & (t >= 0.0) & (t <= 1.0)
    if not hit.any():
        return None
    rows, cols = np.nonzero(hit)
    p = near[rows, cols] + d[rows, cols] * t[rows, cols, None]
    lo = np.full(3, -0.5)
    hi = np.asarray(shape_xyz, dtype=np.float64) - 0.5
    if crop is not None:
        c = np.asarray(crop, dtype=np.float64)
        lo = np.maximum(lo, c[0::2])
        hi = np.minimum(hi, c[1::2])
    eps = 1e-6
    inside = np.all((p >= lo - eps) & (p <= hi + eps), axis=1)
    rows, cols, p = rows[inside], cols[inside], p[inside]
    if not len(p):
        return None
    array, origin, spacing = sample
    values = _trilinear(array, (p - np.asarray(origin, dtype=np.float64)) / float(spacing))
    seen = values >= tf_threshold(opacity_points)
    rows, cols, p, values = rows[seen], cols[seen], p[seen], values[seen]
    if not len(p):
        return None
    a = clip.section_opacity
    rgba = np.zeros((h, w, 4), dtype=np.float32)
    rgba[rows, cols, :3] = tf_colour(colour_points, values) * a
    rgba[rows, cols, 3] = a
    # The window depth of each section point, through the eye's projection.
    q = np.concatenate([p, np.ones((len(p), 1))], axis=1) @ (proj @ view @ stretch).T
    depth = np.ones((h, w), dtype=np.float32)
    depth[rows, cols] = np.clip(0.5 * q[:, 2] / q[:, 3] + 0.5, 0.0, 1.0)
    return rgba, depth


def composite_section(rgba: np.ndarray, depth: np.ndarray | None, layer, premultiplied: bool):
    """The section ``layer`` (:func:`section_layer`) in front of an eye's
    ``rgba`` (uint8, h, w, 4: the volume mapper's premultiplied colour over
    a transparent clear value, or the window's pixels over the background)
    and its window ``depth`` (or None). Returns new arrays."""
    if layer is None:
        return rgba, depth
    s, sdepth = layer
    on = s[..., 3] > 0
    out = np.array(rgba, copy=True)
    base = out[on].astype(np.float32) / 255.0
    sa = s[on][:, 3:4]
    if premultiplied:
        # The mapper's clear value is transparent white: its colour is the
        # opacity's at most (see _stereo.over_background).
        base[:, :3] = np.minimum(base[:, :3], base[:, 3:4])
    base[:, :3] = s[on][:, :3] + (1.0 - sa) * base[:, :3]
    base[:, 3:4] = sa + (1.0 - sa) * base[:, 3:4] if premultiplied else 1.0
    out[on] = np.clip(np.rint(base * 255.0), 0, 255).astype(np.uint8)
    if depth is not None:
        depth = np.array(depth, copy=True)
        depth[on] = np.minimum(depth[on], sdepth[on])
    return out, depth
