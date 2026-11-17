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




#: Pixels unprojected at once (a tile of rows): bounds the temporary memory
#: (about 60 bytes a pixel in float32) whatever the eye's size.
TILE_PIXELS = 1 << 18


@dataclass(frozen=True)
class Section:
    """One eye's section, sparse: the pixels it covers (``rows``, ``cols``,
    rows bottom-up as VTK reads them), their premultiplied colour (N, 3), the
    opacity, and their window depth (N)."""
    size: tuple[int, int]
    rows: np.ndarray
    cols: np.ndarray
    rgb: np.ndarray
    alpha: float
    depth: np.ndarray
    examined: int          # pixels unprojected (the screen box of the section)

    def dense(self) -> tuple[np.ndarray, np.ndarray]:
        """``(rgba (h, w, 4) premultiplied float32, window depth (h, w), 1 where none)``."""
        w, h = self.size
        rgba = np.zeros((h, w, 4), np.float32)
        rgba[self.rows, self.cols, :3] = self.rgb
        rgba[self.rows, self.cols, 3] = self.alpha
        depth = np.ones((h, w), np.float32)
        depth[self.rows, self.cols] = self.depth
        return rgba, depth


def _box_corners(lo, hi) -> np.ndarray:
    return np.array([[hi[a] if (i >> a) & 1 else lo[a] for a in range(3)] for i in range(8)], dtype=np.float64)


def _plane_box_points(origin, normal, corners) -> np.ndarray:
    """The points where the plane meets the edges of the box ``corners`` (corner
    i has the high end of axis a when bit a of i is set): the section's outline."""
    s = (corners - origin) @ normal
    pts = []
    for i in range(8):
        for bit in (1, 2, 4):
            if i & bit:
                continue
            j = i | bit
            a, b = s[i], s[j]
            if (a < 0 and b < 0) or (a > 0 and b > 0):
                continue
            f = 0.0 if a == b else a / (a - b)
            pts.append(corners[i] + f * (corners[j] - corners[i]))
    return np.array(pts, dtype=np.float64).reshape(-1, 3)


def _screen_box(points: np.ndarray, to_clip: np.ndarray, size) -> tuple[int, int, int, int] | None:
    """The pixel rectangle ``(c0, c1, r0, r1)`` (end exclusive, rows bottom-up)
    covering ``points`` (voxels) through ``to_clip``; the whole eye when one is
    behind the eye (its projection would not bound the rest); None when none."""
    w, h = size
    if not len(points):
        return None
    q = np.concatenate([points, np.ones((len(points), 1))], axis=1) @ to_clip.T
    if np.any(q[:, 3] <= 1e-9):
        return 0, w, 0, h
    ndc = q[:, :2] / q[:, 3:4]
    x0, y0 = ndc.min(axis=0)
    x1, y1 = ndc.max(axis=0)
    if x1 < -1 or y1 < -1 or x0 > 1 or y0 > 1:
        return None
    c0 = int(np.clip(np.floor((x0 + 1) / 2 * w) - 1, 0, w))
    c1 = int(np.clip(np.ceil((x1 + 1) / 2 * w) + 1, 0, w))
    r0 = int(np.clip(np.floor((y0 + 1) / 2 * h) - 1, 0, h))
    r1 = int(np.clip(np.ceil((y1 + 1) / 2 * h) + 1, 0, h))
    return (c0, c1, r0, r1) if c1 > c0 and r1 > r0 else None


def section_layer(clip: ClipPlane, eye: dict, size: tuple[int, int], shape_xyz,
                  sample, colour_points, opacity_points, crop=None) -> Section | None:
    """The section one stereo eye sees (:class:`Section`), or None.

    ``eye`` is :func:`_stereo.stereo_eye`'s (its view, stretch and
    projection: voxel *v* is at ``stretch @ v`` in the world the rigid view
    looks at). ``sample`` is ``(array z, y, x, origin, spacing)``, the volume
    drawn, in voxel coordinates. ``crop``: the clipping box's planes
    ``(x0, x1, y0, y1, z0, z1)`` (``_pipeline.crop_planes``), or None.
    A pixel shows the section where its ray, from the eye's near plane, enters
    the kept part through a face of the plane, inside the cube and the box,
    at a value the opacity transfer function draws. Only the pixels of the
    screen rectangle of the section's outline (the plane's faces within the
    box) are unprojected, a tile of rows at a time, in float32."""
    w, h = size
    proj = np.asarray(eye["projection"], dtype=np.float64)
    view = np.asarray(eye["view"], dtype=np.float64)
    stretch = np.asarray(eye["stretch"], dtype=np.float64)
    to_clip = proj @ view @ stretch                     # voxels -> clip
    lo = np.full(3, -0.5)
    hi = np.asarray(shape_xyz, dtype=np.float64) - 0.5
    if crop is not None:
        c = np.asarray(crop, dtype=np.float64)
        lo = np.maximum(lo, c[0::2])
        hi = np.minimum(hi, c[1::2])
    if np.any(hi <= lo):
        return None
    o = np.asarray(clip.origin, dtype=np.float64)
    n = np.asarray(clip.normal, dtype=np.float64)
    corners = _box_corners(lo, hi)
    offsets = (0.0,) if clip.half is None else (-clip.half, clip.half)
    outline = np.concatenate([_plane_box_points(o + k * n, n, corners) for k in offsets])
    rect = _screen_box(outline, to_clip, size)
    if rect is None:
        return None
    c0, c1, r0, r1 = rect
    inv = np.linalg.inv(to_clip)                        # clip -> voxels
    # A ray's point at NDC depth z is (A x + B y + C z + D) / its w: float32 per pixel.
    A, B, C, D = (inv[:, k].astype(np.float32) for k in range(4))
    of, nf = o.astype(np.float32), n.astype(np.float32)
    xs = ((np.arange(c0, c1) + 0.5) / w * 2.0 - 1.0).astype(np.float32)
    rows_per_tile = max(1, TILE_PIXELS // max(1, c1 - c0))
    eps = 1e-4
    lo32, hi32 = (lo - eps).astype(np.float32), (hi + eps).astype(np.float32)
    found_r, found_c, found_p = [], [], []
    for t0 in range(r0, r1, rows_per_tile):
        t1 = min(r1, t0 + rows_per_tile)
        ys = ((np.arange(t0, t1) + 0.5) / h * 2.0 - 1.0).astype(np.float32)
        X, Y = np.meshgrid(xs, ys)
        base = X[..., None] * A + Y[..., None] * B + D      # (rows, cols, 4)
        near = base - C
        far = base + C
        near = near[..., :3] / near[..., 3:4]
        d = far[..., :3] / far[..., 3:4] - near
        s0 = (near - of) @ nf
        sd = d @ nf
        with np.errstate(divide="ignore", invalid="ignore"):
            if clip.half is None:
                t = np.where((s0 < 0) & (sd > 0), -s0 / sd, np.nan)
            else:
                hh = np.float32(clip.half)
                t = np.where((s0 < -hh) & (sd > 0), (-hh - s0) / sd,
                             np.where((s0 > hh) & (sd < 0), (hh - s0) / sd, np.nan))
        hit = np.isfinite(t) & (t >= 0.0) & (t <= 1.0)
        if not hit.any():
            continue
        rr, cc = np.nonzero(hit)
        p = near[rr, cc] + d[rr, cc] * t[rr, cc, None]
        inside = np.all((p >= lo32) & (p <= hi32), axis=1)
        found_r.append(rr[inside] + t0)
        found_c.append(cc[inside] + c0)
        found_p.append(p[inside].astype(np.float64))
    examined = (c1 - c0) * (r1 - r0)
    if not found_p or not sum(len(p) for p in found_p):
        return None
    rows, cols, p = np.concatenate(found_r), np.concatenate(found_c), np.concatenate(found_p)
    array, origin, spacing = sample
    values = _trilinear(array, (p - np.asarray(origin, dtype=np.float64)) / float(spacing))
    seen = values >= tf_threshold(opacity_points)
    rows, cols, p, values = rows[seen], cols[seen], p[seen], values[seen]
    if not len(p):
        return None
    a = clip.section_opacity
    q = np.concatenate([p, np.ones((len(p), 1))], axis=1) @ to_clip.T
    depth = np.clip(0.5 * q[:, 2] / q[:, 3] + 0.5, 0.0, 1.0).astype(np.float32)
    return Section((w, h), rows, cols, (tf_colour(colour_points, values) * a).astype(np.float32),
                   float(a), depth, examined)


def composite_section(rgba: np.ndarray, depth: np.ndarray | None, layer: Section | None, premultiplied: bool):
    """The section ``layer`` (:func:`section_layer`) in front of an eye's
    ``rgba`` (uint8, h, w, 4: the volume mapper's premultiplied colour over
    a transparent clear value, or the window's pixels over the background)
    and its window ``depth`` (or None). Returns new arrays."""
    if layer is None:
        return rgba, depth
    r, c = layer.rows, layer.cols
    out = np.array(rgba, copy=True)
    base = out[r, c].astype(np.float32) / 255.0
    sa = np.float32(layer.alpha)
    if premultiplied:
        # The mapper's clear value is transparent white: its colour is the
        # opacity's at most (see _stereo.over_background).
        base[:, :3] = np.minimum(base[:, :3], base[:, 3:4])
    base[:, :3] = layer.rgb + (1.0 - sa) * base[:, :3]
    base[:, 3] = sa + (1.0 - sa) * base[:, 3] if premultiplied else 1.0
    out[r, c] = np.clip(np.rint(base * 255.0), 0, 255).astype(np.uint8)
    if depth is not None:
        depth = np.array(depth, copy=True)
        depth[r, c] = np.minimum(depth[r, c], layer.depth)
    return out, depth
