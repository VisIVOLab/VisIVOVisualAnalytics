"""Stereo frames for a headset's pose (issue #169, VR epic #166).

A WebXR page sends, per frame, the pose it wants a picture for: each eye's
view matrix (world to eye) and projection, the eye buffer size, and where the
cube stands in its world (the model matrix). The session renders both eyes on
its one GPU copy of the cube, one camera after the other into a window of one
eye's size, and sends back one binary message: the two eyes side by side as a
JPEG, a small depth image for reprojection, and a JSON header with the pose id
the frame was rendered for and the server's timings. The desktop's mono view
is untouched: a session renders stereo only between an ``xr_pose`` and an
``xr_stop``.

Coordinates. Matrices travel as WebGL/WebXR do, 16 numbers in column-major
order, world units in metres. The model matrix maps the VR page's model space
to world: the box centred on the origin, its longest side 1, x along NAXIS1,
y along NAXIS2, z along the spectral axis stretched by ``z_stretch``, its
faces at the outer pixel edges (``app/static/vr/js/volume.js`` boxExtent).

Depth. For a volume, the depth of the first sample along each ray whose
opacity is above zero, i.e. the first sample above the transfer function's
threshold (VTK's render-to-image depth). For an isosurface, the surface's
depth. Either is the distance along the eye's -z axis, in metres, quantised
to 16 bits over the frame's own range (``z_near``..``z_far``, 0 = no sample),
reduced by ``scale`` keeping the nearest sample of each block (a reprojection
that errs towards the viewer leaves no holes behind the emission), and
deflated (zlib). The pure functions here have no VTK; the mixin at the end
runs on the session's render thread.

Colour-only frames (issue #187). A GL may give the colour back and not the
depth: a GLES context cannot read a depth buffer with ``glReadPixels``, and a
driver's render-to-image depth texture may come back empty, cleared or
unwritten (Mesa, some EGL stacks on clusters). Each depth image is checked
against its own picture (:func:`volume_depth_problem`,
:func:`surface_depth_problem`); a frame whose depth fails goes without it,
and after :data:`DEPTH_STRIKES` such frames in a row the session stops
reading depth. ``VISIVO_XR_DEPTH=0`` turns depth off on a backend from the
start. Every frame header says which reprojection its frame allows
(``"reprojection": "depth"`` or ``"pose"``) and, without depth, why
(``"depth_unavailable"``); the page then warps the frame by the pose alone.
"""
from __future__ import annotations

import io
import json
import logging
import math
import os
import struct
import time
import zlib
from dataclasses import dataclass, field
from typing import Any

import numpy as np

logger = logging.getLogger("visivo.render")

#: First bytes of every stereo frame message, then a little-endian uint32: the
#: length of the UTF-8 JSON header that follows. The payloads come after it.
MAGIC = b"VXR1"
_PREFIX = struct.Struct("<4sI")

#: Eye buffer size accepted per eye, in pixels (a Quest 3 eye is 2064 x 2208).
MIN_EYE_PX, MAX_EYE_PX = 16, 2560
#: Depth reduction factors accepted (1 = full resolution).
MAX_DEPTH_SCALE = 16
#: Largest spectral stretch accepted (the VR page's slider stays well inside).
MAX_Z_STRETCH = 100.0
#: Ray step of a stereo frame, in voxels. Half a voxel puts the first sample
#: above the threshold within a voxel of the emission's surface (measured on
#: a sphere: median 0.5, 95th percentile 0.9 voxel; the mono view's adaptive
#: step of about a voxel gave 0.7 and 1.4), which reprojection needs.
DEFAULT_SAMPLE_DISTANCE = 0.5
# The bounds keep one eye of a 2 GB cube near 0.2 s of GPU work at most: a
# command buffer that runs for seconds makes Apple's GL driver abort the
# whole render process (seen with the first version of this mode).
MIN_SAMPLE_DISTANCE, MAX_SAMPLE_DISTANCE = 0.25, 4.0
#: Quantisation of the depth image: 0 is "no sample", 1..65535 the range.
_DEPTH_LEVELS = 65534


# ── Request ──────────────────────────────────────────────────────────────────


@dataclass
class EyeRequest:
    eye: str                   # "left" or "right"
    view: np.ndarray           # 4x4, world -> eye, rigid
    projection: np.ndarray     # 4x4, eye -> clip (OpenGL convention)


@dataclass
class XRPose:
    pose_id: int
    t_client: float | None
    model: np.ndarray          # 4x4, model space -> world
    z_stretch: float
    eyes: list[EyeRequest]     # left, then right
    width: int                 # one eye's buffer
    height: int
    depth_scale: int | None    # None: no depth image
    quality: int | None        # JPEG quality; None: the session's
    background: tuple[float, float, float]
    t_received: float | None   # stamped by the backend when the pose arrived
    sample_distance: float = DEFAULT_SAMPLE_DISTANCE   # voxels between ray samples


def _matrix(value: Any, name: str) -> np.ndarray:
    """A 4x4 matrix (row-major, for ``m @ v``) from 16 column-major numbers."""
    try:
        a = np.asarray(value, dtype=float)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be 16 numbers.") from exc
    if a.shape != (16,) or not np.isfinite(a).all():
        raise ValueError(f"{name} must be 16 finite numbers (column-major).")
    return a.reshape(4, 4).T.copy()


def _number(value: Any, name: str) -> float:
    if isinstance(value, bool):
        raise ValueError(f"{name} must be a number.")
    try:
        v = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be a number.") from exc
    if not math.isfinite(v):
        raise ValueError(f"{name} must be finite.")
    return v


def _integer(value: Any, name: str, lo: int, hi: int) -> int:
    v = _number(value, name)
    if v != int(v) or not lo <= v <= hi:
        raise ValueError(f"{name} must be an integer in {lo}..{hi}.")
    return int(v)


def projection_from_fov(fov: dict) -> np.ndarray:
    """OpenGL projection from OpenXR-style half angles in radians (``left`` and
    ``down`` negative) and ``near``/``far`` distances in metres."""
    if not isinstance(fov, dict):
        raise ValueError("fov must be an object with left, right, up, down, near, far.")
    left, right, up, down, near, far = (_number(fov.get(k), f"fov.{k}") for k in
                                        ("left", "right", "up", "down", "near", "far"))
    if not -math.pi / 2 < left < right < math.pi / 2 or not -math.pi / 2 < down < up < math.pi / 2:
        raise ValueError("fov angles must satisfy -pi/2 < left < right < pi/2 and "
                         "-pi/2 < down < up < pi/2.")
    if not 0.0 < near < far:
        raise ValueError("fov needs 0 < near < far.")
    l, r = near * math.tan(left), near * math.tan(right)
    b, t = near * math.tan(down), near * math.tan(up)
    return np.array([
        [2 * near / (r - l), 0.0, (r + l) / (r - l), 0.0],
        [0.0, 2 * near / (t - b), (t + b) / (t - b), 0.0],
        [0.0, 0.0, -(far + near) / (far - near), -2 * far * near / (far - near)],
        [0.0, 0.0, -1.0, 0.0],
    ])


def _check_projection(p: np.ndarray, name: str) -> None:
    """A perspective frustum as WebXR gives it: w = -z, depth from z alone."""
    if not np.allclose(p[3], [0.0, 0.0, -1.0, 0.0], atol=1e-6) or \
            abs(p[2, 0]) > 1e-6 or abs(p[2, 1]) > 1e-6:
        raise ValueError(f"{name} must be a perspective projection (OpenGL convention).")
    if not p[0, 0] > 0 or not p[1, 1] > 0:
        raise ValueError(f"{name} has a degenerate field of view.")
    near, _ = clip_range(p)
    if not near > 0:
        raise ValueError(f"{name} has no positive near plane.")


def clip_range(p: np.ndarray) -> tuple[float, float]:
    """Near and far distances of an OpenGL perspective projection, in the
    units of its eye space; far is infinite for an infinite-far projection.

    General in w = -c z: a projection that scales the eye space first (the
    voxel camera's, :func:`voxel_camera`) has c = s, not 1, and its planes
    are the original ones divided by s."""
    c = -p[3, 2]
    near = p[2, 3] / (p[2, 2] - c)
    far = p[2, 3] / (p[2, 2] + c) if p[2, 2] + c != 0.0 else math.inf
    return float(near), float(far if far > 0 else math.inf)


def _check_rigid(v: np.ndarray, name: str, message: str | None = None) -> None:
    r = v[:3, :3]
    if not np.allclose(v[3], [0.0, 0.0, 0.0, 1.0], atol=1e-6) or \
            not np.allclose(r @ r.T, np.eye(3), atol=1e-3) or np.linalg.det(r) <= 0:
        raise ValueError(message or f"{name} must be a rigid transform (rotation and translation).")


def parse_pose(command: dict) -> XRPose:
    """The ``xr_pose`` command, checked; a ValueError names the first fault."""
    pose_id = _integer(command.get("pose_id"), "pose_id", 0, 2 ** 53)
    t_client = command.get("t_client")
    t_client = None if t_client is None else _number(t_client, "t_client")
    model = _matrix(command.get("model"), "model")
    det = np.linalg.det(model[:3, :3])
    if not det > 1e-12 or not np.allclose(model[3], [0.0, 0.0, 0.0, 1.0], atol=1e-6):
        raise ValueError("model must be a rotation, a translation and a uniform scale.")
    unscaled = model.copy()
    unscaled[:3, :3] /= np.cbrt(det)
    _check_rigid(unscaled, "model", "model must be a rotation, a translation and a uniform scale.")
    z_stretch = _number(command.get("z_stretch", 1.0), "z_stretch")
    if not 0.0 < z_stretch <= MAX_Z_STRETCH:
        raise ValueError(f"z_stretch must be in (0, {MAX_Z_STRETCH:g}].")
    views = command.get("views")
    if not isinstance(views, list) or len(views) != 2 or \
            not all(isinstance(v, dict) for v in views):
        raise ValueError("views must be a list of two objects, one per eye.")
    eyes: dict[str, EyeRequest] = {}
    sizes = set()
    for i, v in enumerate(views):
        eye = v.get("eye")
        if eye not in ("left", "right") or eye in eyes:
            raise ValueError("views need one 'left' and one 'right' eye.")
        view = _matrix(v.get("view"), f"views[{i}].view")
        _check_rigid(view, f"views[{i}].view")
        if v.get("projection") is not None:
            proj = _matrix(v.get("projection"), f"views[{i}].projection")
        elif v.get("fov") is not None:
            proj = projection_from_fov(v.get("fov"))
        else:
            raise ValueError(f"views[{i}] needs a projection or a fov.")
        _check_projection(proj, f"views[{i}].projection")
        sizes.add((_integer(v.get("width"), f"views[{i}].width", MIN_EYE_PX, MAX_EYE_PX),
                   _integer(v.get("height"), f"views[{i}].height", MIN_EYE_PX, MAX_EYE_PX)))
        eyes[eye] = EyeRequest(eye, view, proj)
    if len(sizes) != 1:
        raise ValueError("both eyes must have the same width and height.")
    width, height = sizes.pop()
    depth = command.get("depth", {})
    if depth is False or depth is None:
        depth_scale = None
    elif isinstance(depth, dict):
        depth_scale = _integer(depth.get("scale", 4), "depth.scale", 1, MAX_DEPTH_SCALE)
    else:
        raise ValueError("depth must be an object ({\"scale\": n}) or false.")
    quality = command.get("quality")
    quality = None if quality is None else _integer(quality, "quality", 1, 100)
    bg = command.get("background", [0.0, 0.0, 0.0])
    if not isinstance(bg, (list, tuple)) or len(bg) != 3:
        raise ValueError("background must be three numbers in 0..1.")
    background = tuple(min(1.0, max(0.0, _number(c, "background"))) for c in bg)
    t_received = command.get("t_received")
    t_received = None if t_received is None else _number(t_received, "t_received")
    step = _number(command.get("sample_distance", DEFAULT_SAMPLE_DISTANCE), "sample_distance")
    if not MIN_SAMPLE_DISTANCE <= step <= MAX_SAMPLE_DISTANCE:
        raise ValueError(f"sample_distance must be in {MIN_SAMPLE_DISTANCE:g}.."
                         f"{MAX_SAMPLE_DISTANCE:g} voxels.")
    return XRPose(pose_id, t_client, model, z_stretch, [eyes["left"], eyes["right"]],
                  width, height, depth_scale, quality, background, t_received, step)


# ── Geometry ─────────────────────────────────────────────────────────────────


def voxel_to_model(nx: int, ny: int, nz: int, z_stretch: float = 1.0) -> np.ndarray:
    """4x4 from the session's voxel coordinates (voxel i at x = i, as the VTK
    image data is laid out) to the VR page's model space (see the module doc)."""
    m = float(max(nx, ny, nz * z_stretch))
    s = np.array([1.0 / m, 1.0 / m, z_stretch / m])
    out = np.diag([*s, 1.0])
    out[:3, 3] = (0.5 - np.array([nx, ny, nz]) / 2.0) * s
    return out


def camera_from_view(view: np.ndarray) -> tuple[list[float], list[float], list[float]]:
    """Position, focal point and view up of the camera whose view transform is
    the rigid ``view`` (world -> eye; the eye looks down its -z)."""
    inv = np.linalg.inv(view)
    pos = inv[:3, 3]
    return pos.tolist(), (pos - inv[:3, 2]).tolist(), inv[:3, 1].tolist()


def voxel_camera(view: np.ndarray, projection: np.ndarray, model: np.ndarray,
                 shape_xyz: tuple[int, int, int], z_stretch: float
                 ) -> tuple[np.ndarray, np.ndarray, np.ndarray, float]:
    """The eye in the cube's voxel coordinates: ``(view, projection, stretch, s)``.

    The scene stays where the mono view has it (voxel i at x = i), and the
    camera moves instead: the cube's placement in the headset's world, a
    rotation, a translation and a uniform scale s (metres per voxel), is
    folded into a rigid view in voxel units and a projection that scales by s
    first. VTK then ray-casts the volume with its usual magnitudes (a volume a
    thousandth of a unit across hung Apple's GPU). The spectral stretch, not
    uniform, stays a prop matrix: ``stretch``, the diagonal (1, 1, z_stretch).
    Every voxel reaches exactly the clip coordinates it reaches through
    ``projection @ view @ model @ voxel_to_model``."""
    stretch = np.diag([1.0, 1.0, z_stretch, 1.0])
    placed = model @ voxel_to_model(*shape_xyz, z_stretch) @ np.linalg.inv(stretch)
    ev = view @ placed                       # s * rotation, translation
    s = float(np.cbrt(np.linalg.det(ev[:3, :3])))
    rigid = ev.copy()
    rigid[:3, :] /= s
    return rigid, projection @ np.diag([s, s, s, 1.0]), stretch, s


#: Where the rays start, in voxels in front of the eye, at most: VTK's GPU
#: ray caster loses the volume when the camera is inside its box and the near
#: plane is several voxels away (measured, VTK 9.6.2, camera at the centre of
#: a 64^3 box: everything drawn with the near plane 1 voxel away, 76% at 4,
#: nothing at 12.8, which is a headset's 0.1 m with a 0.5 m box).
RAY_NEAR_VOXELS = 0.5


def with_depth_range(projection: np.ndarray, near: float, far: float) -> np.ndarray:
    """``projection`` with its depth range replaced: the same picture (x and y
    do not depend on it), its near and far planes at ``near`` and ``far``."""
    out = projection.copy()
    out[2] = [0.0, 0.0, -(far + near) / (far - near), -2.0 * far * near / (far - near)]
    return out


def stereo_eye(view: np.ndarray, projection: np.ndarray, model: np.ndarray,
               shape_xyz: tuple[int, int, int], z_stretch: float) -> dict:
    """Everything one eye is drawn with: the voxel camera (:func:`voxel_camera`)
    with a depth range that VTK draws correctly, the camera inside the cube
    included. The near plane comes to :data:`RAY_NEAR_VOXELS` (or the
    headset's own, if nearer); the far plane goes just past the box's farthest
    corner. The picture is the headset's; emission nearer than the headset's
    near plane is drawn, not clipped. ``depth_projection`` (metres) turns the
    frame's window depth into distances (:func:`eye_distance`)."""
    rigid, _, stretch, s = voxel_camera(view, projection, model, shape_xyz, z_stretch)
    nx, ny, nz = shape_xyz
    corners = np.array([[x, y, z, 1.0] for x in (-0.5, nx - 0.5) for y in (-0.5, ny - 0.5)
                        for z in (-0.5, nz - 0.5)])
    ahead = -(rigid @ stretch @ corners.T)[2]          # voxels along the eye's -z
    near_m = min(clip_range(projection)[0], RAY_NEAR_VOXELS * s)
    far_m = max(float(ahead.max()) * 1.01 + 1.0, 2.0 * near_m / s) * s
    depth_projection = with_depth_range(projection, near_m, far_m)
    return {"view": rigid, "stretch": stretch, "scale": s,
            "projection": depth_projection @ np.diag([s, s, s, 1.0]),
            "clipping": (near_m / s, far_m / s), "depth_projection": depth_projection}


def eye_distance(window_depth: np.ndarray, projection: np.ndarray) -> np.ndarray:
    """Distance along the eye's -z (metres) of each window depth in [0, 1];
    +inf where nothing was drawn (depth 1)."""
    d = np.asarray(window_depth, dtype=np.float64)
    ndc = 2.0 * d - 1.0
    with np.errstate(divide="ignore", invalid="ignore"):
        dist = projection[2, 3] / (ndc + projection[2, 2])
    dist[(d >= 1.0) | ~np.isfinite(dist) | (dist <= 0)] = np.inf
    return dist.astype(np.float32)


def nearest_per_block(dist: np.ndarray, scale: int) -> np.ndarray:
    """``dist`` reduced ``scale`` times on both axes, keeping each block's
    nearest sample (+inf where a block has none)."""
    if scale == 1:
        return dist
    h, w = dist.shape
    h2, w2 = -(-h // scale), -(-w // scale)
    if (h2 * scale, w2 * scale) == (h, w):
        padded = dist                     # no copy when the blocks fit
    else:
        padded = np.full((h2 * scale, w2 * scale), np.inf, dtype=np.float32)
        padded[:h, :w] = dist
    # One axis at a time: contiguous rows first, then the columns.
    return padded.reshape(h2, scale, w2 * scale).min(axis=1).reshape(h2, w2, scale).min(axis=2)


def encode_depth(dist: np.ndarray) -> tuple[bytes, float, float]:
    """Deflated little-endian uint16 rows (top row first): 0 where there is no
    sample, else 1 + round((d - z_near) / (z_far - z_near) * 65534)."""
    hit = np.isfinite(dist)
    q = np.zeros(dist.shape, dtype="<u2")
    if not hit.any():
        return zlib.compress(q.tobytes(), 1), 0.0, 0.0
    z0, z1 = float(dist[hit].min()), float(dist[hit].max())
    span = max(z1 - z0, 1e-9)
    q[hit] = 1 + np.rint((dist[hit] - z0) / span * _DEPTH_LEVELS).astype(np.int64)
    return zlib.compress(q.tobytes(), 1), z0, z1


def decode_depth(payload: bytes, width: int, height: int, z_near: float,
                 z_far: float) -> np.ndarray:
    """Inverse of :func:`encode_depth`: metres, NaN where there is no sample."""
    q = np.frombuffer(zlib.decompress(payload), dtype="<u2").reshape(height, width)
    out = z_near + (q.astype(np.float64) - 1.0) / _DEPTH_LEVELS * (z_far - z_near)
    out[q == 0] = np.nan
    return out


def over_background(rgba: np.ndarray, background) -> np.ndarray:
    """The volume mapper's render-to-image colour over ``background``.

    Where the volume's box is drawn, a pixel holds the volume's premultiplied
    colour P and its opacity A; elsewhere it keeps the image's clear value,
    transparent white (measured with VTK 9.6.2). A premultiplied colour never
    exceeds its opacity, so min(pixel, A) is P everywhere, and the window
    would show P + (1 - A) B over a background B. RGBA uint8 in, RGBA out (a
    new array). Pixels are handled whole (one uint32 each), and only the
    partly transparent ones, few, pixel by pixel: on a 2048 x 1024 frame the
    per-channel version took 16-40 ms, more than the JPEG."""
    bg = np.rint(np.asarray(background, dtype=float) * 255.0).astype(np.int32)
    px = np.ascontiguousarray(rgba).view("<u4")[..., 0]
    alpha = px >> 24
    clear = np.uint32(int(bg[0]) | int(bg[1]) << 8 | int(bg[2]) << 16)
    # Transparent: the background (with A = 0); opaque: as drawn.
    out = np.where(alpha == 0, clear, px).astype("<u4")
    # Over black that is all (a drawn pixel is already P); otherwise the
    # partly transparent pixels get their share of the background.
    part = np.nonzero((alpha > 0) & (alpha < 255)) if bg.any() else (np.empty(0),)
    if part[0].size:
        sub = rgba[part].astype(np.int32)                     # (n, 4)
        a = sub[:, 3:4]
        rgb = np.minimum(sub[:, :3], a) + ((255 - a) * bg + 127) // 255
        sub[:, :3] = np.clip(rgb, 0, 255)
        out[part] = sub.astype(np.uint8).view("<u4")[:, 0]
    return out.view(np.uint8).reshape(rgba.shape)


# ── Whether a depth image can be trusted (issue #187) ────────────────────────

#: Structural failures in a row (the GL cannot read depth back at all) before
#: a session stops reading depth: such a frame goes without depth, and after
#: this many the depth is not asked for again (until the blend or render mode
#: changes). A frame whose depth merely looks wrong goes without depth and is
#: never counted: one odd picture must not cost the session its depth.
DEPTH_STRIKES = 2
#: A pixel at least this opaque (of 255) had a sample above zero opacity, so
#: in composite blending its ray has a first hit: its depth must be nearer
#: than the far plane.
_OPAQUE_ALPHA = 16
#: Share of such pixels allowed without a first hit (rounding at the silhouette).
_DRAWN_MISS_SHARE = 0.05
#: How much a window depth may stray outside [0, 1] (float rounding).
_DEPTH_SLACK = 1e-4


class DepthProblem(str):
    """Why a frame has no depth. ``structural``: the readback itself failed
    (counted towards :data:`DEPTH_STRIKES`); otherwise only this frame's
    depth is dropped."""

    structural: bool = False

    def __new__(cls, reason: str, structural: bool = False):
        obj = super().__new__(cls, reason)
        obj.structural = structural
        return obj


def depth_off_on_this_backend() -> bool:
    """``VISIVO_XR_DEPTH=0`` (or off, false, no): stereo frames without depth."""
    return os.environ.get("VISIVO_XR_DEPTH", "").strip().lower() in ("0", "off", "false", "no")


def _depth_range_problem(depth: np.ndarray) -> DepthProblem | None:
    if depth.size == 0:
        return DepthProblem("empty depth image", True)
    if not np.isfinite(depth).all():
        return DepthProblem("depth image holds NaN or infinite values", True)
    lo, hi = float(depth.min()), float(depth.max())
    if lo < -_DEPTH_SLACK or hi > 1.0 + _DEPTH_SLACK:
        return DepthProblem(f"depth image outside 0..1 ({lo:.3g}..{hi:.3g})", True)
    return None


def volume_depth_problem(depth: np.ndarray, alpha: np.ndarray) -> DepthProblem | None:
    """Why the volume mapper's first-hit depth image (composite blending only:
    MIP and MinIP write none) does not belong to its colour image (``alpha``,
    uint8, same shape), or None when it does.

    Structural (the texture was not written): a wrong size, values that are
    not depths, or one value over the whole image that contradicts the
    picture: all 1 (cleared) with opaque pixels, or all one value below 1
    with transparent pixels. An eye inside the emission (every pixel opaque,
    every ray hit at once, depth 0 everywhere) passes. Not structural: more
    than :data:`_DRAWN_MISS_SHARE` of the opaque pixels without a hit; that
    frame goes without depth and nothing is counted."""
    depth = np.asarray(depth)
    alpha = np.asarray(alpha)
    if depth.shape != alpha.shape:
        return DepthProblem(f"depth image {depth.shape} for a picture of {alpha.shape}", True)
    problem = _depth_range_problem(depth)
    if problem:
        return problem
    drawn = alpha >= _OPAQUE_ALPHA
    lo, hi = float(depth.min()), float(depth.max())
    if lo == hi:
        if lo >= 1.0 and drawn.any():
            return DepthProblem("depth image is 1 everywhere under a drawn picture "
                                "(not written)", True)
        if lo < 1.0 and (alpha == 0).any():
            return DepthProblem(f"depth image is {lo:.3g} everywhere, empty pixels included "
                                "(not written)", True)
    n_drawn = int(drawn.sum())
    if n_drawn:
        missed = int((depth[drawn] >= 1.0).sum())
        if missed > _DRAWN_MISS_SHARE * n_drawn:
            return DepthProblem(f"{100.0 * missed / n_drawn:.0f} % of the drawn pixels "
                                "have no depth")
    return None


def surface_depth_problem(depth: np.ndarray, status: int | None = 1) -> DepthProblem | None:
    """Why the window's depth buffer, read back for a surface, cannot be used,
    or None. ``status``: what ``GetZbufferData`` returned (VTK_OK is 1; a GLES
    context refuses to read a depth buffer). A buffer that is one value below
    1 everywhere was not read: a drawn surface is never at one depth across
    the whole view, and an empty view is all 1. All structural."""
    if status is not None and status != 1:
        return DepthProblem(f"the window's depth buffer could not be read (status {status})",
                            True)
    depth = np.asarray(depth)
    problem = _depth_range_problem(depth)
    if problem:
        return problem
    if float(depth.min()) == float(depth.max()) < 1.0:
        return DepthProblem(f"depth buffer is {float(depth.min()):.3g} everywhere "
                            "(not read back)", True)
    return None


# ── The frame ────────────────────────────────────────────────────────────────


@dataclass
class XRFrame:
    """One stereo frame, or the reason there is none. Made in the render
    process; the backend stamps ``ready`` and ``sent`` and serialises it."""
    header: dict = field(default_factory=dict)
    color: bytes = b""
    depth: bytes | None = None
    error: str | None = None
    #: Why, for a client to act on without reading the text: "out_of_core",
    #: "unsupported" (no stereo frames from this session), or None.
    code: str | None = None

    def error_message(self) -> str:
        message = {"type": "xr_error", "pose_id": self.header.get("pose_id"),
                   "error": self.error}
        if self.code:
            message["code"] = self.code
        return json.dumps(message, separators=(",", ":"))

    def to_bytes(self, ready: float | None = None, sent: float | None = None) -> bytes:
        """The wire message: MAGIC, header length, JSON header, then the
        payloads at the offsets the header gives (from the end of the header)."""
        header = json.loads(json.dumps(self.header))   # a copy, never the cached one
        t = header.setdefault("timing", {})
        if ready is not None:
            t["ready"] = ready
        if sent is not None:
            t["sent"] = sent
        header["timing_ms"] = timing_ms(t)
        header["color"]["offset"], header["color"]["length"] = 0, len(self.color)
        if self.depth is not None and header.get("depth"):
            header["depth"]["offset"] = len(self.color)
            header["depth"]["length"] = len(self.depth)
        blob = json.dumps(header, separators=(",", ":")).encode()
        return b"".join((_PREFIX.pack(MAGIC, len(blob)), blob, self.color, self.depth or b""))


def timing_ms(t: dict) -> dict:
    """Durations between the timestamps of a frame (seconds since the epoch,
    one clock: the backend's machine), in milliseconds."""
    steps = (("wait", "received", "render_start"), ("render", "render_start", "render_end"),
             ("depth", "render_end", "depth_end"), ("encode", "depth_end", "encode_end"),
             ("handoff", "encode_end", "ready"), ("queue", "ready", "sent"),
             ("total", "received", "sent"))
    return {name: round(1000.0 * (t[b] - t[a]), 3) for name, a, b in steps
            if t.get(a) is not None and t.get(b) is not None}


def parse_frame(message: bytes) -> tuple[dict, bytes, bytes | None]:
    """A stereo frame message back into ``(header, jpeg, depth payload)``."""
    magic, n = _PREFIX.unpack_from(message)
    if magic != MAGIC:
        raise ValueError("not a stereo frame")
    start = _PREFIX.size + n
    header = json.loads(message[_PREFIX.size:start])
    c = header["color"]
    color = message[start + c["offset"]:start + c["offset"] + c["length"]]
    d = header.get("depth")
    depth = message[start + d["offset"]:start + d["offset"] + d["length"]] if d else None
    return header, color, depth


def is_frame(message: bytes) -> bool:
    return message[:4] == MAGIC


# ── On the render thread ─────────────────────────────────────────────────────


def _vtk_matrix(m: np.ndarray):
    import vtk
    out = vtk.vtkMatrix4x4()
    for i in range(4):
        for j in range(4):
            out.SetElement(i, j, float(m[i, j]))
    return out


class _NoRenderToImage(RuntimeError):
    """The volume mapper gave no image back (a GL without the readback it
    needs): the frame is drawn again from the window."""


def _image_pixels(getter, shape: tuple[int, ...]) -> np.ndarray:
    """The pixels of one of the volume mapper's render-to-image images."""
    import vtk
    from vtk.util.numpy_support import vtk_to_numpy
    image = vtk.vtkImageData()
    getter(image)
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        raise _NoRenderToImage("empty image")
    pixels = vtk_to_numpy(scalars)
    if pixels.size != int(np.prod(shape)):
        raise _NoRenderToImage(f"image of {pixels.size} values for {shape}")
    return pixels.reshape(shape)


class StereoMixin:
    """``xr_pose`` / ``xr_stop`` and the stereo frame of :class:`RenderSession`."""

    def _apply_xr_command_on_thread(self, command: dict[str, Any]) -> None:
        if command.get("type") == "xr_pose":
            try:
                self._xr_pose = parse_pose(command)
                self._xr_error = None
            except ValueError as exc:
                # The backend checks a pose before it is sent here; this is
                # for a caller that drives the session directly.
                self._xr_error = (command.get("pose_id"), str(exc))
        elif command.get("type") == "xr_stop":
            self._xr_pose = None
            self._xr_error = None
            self._set_render_to_image(False)

    def _xr_active(self) -> bool:
        return self._xr_pose is not None or self._xr_error is not None

    def _set_render_to_image(self, on: bool) -> None:
        mapper = self._volume_mapper
        if mapper is not None and hasattr(mapper, "SetRenderToImage") and \
                bool(mapper.GetRenderToImage()) != on:
            mapper.SetRenderToImage(1 if on else 0)
            if on:
                mapper.SetDepthImageScalarTypeToFloat()

    def _render_xr_frame_on_thread(self) -> XRFrame:
        """Both eyes of the latest pose, side by side, with their depth."""
        if self._xr_error is not None:
            pose_id, message = self._xr_error
            self._xr_error = None
            return XRFrame({"pose_id": pose_id}, error=message)
        pose = self._xr_pose
        base = {"v": 1, "pose_id": pose.pose_id, "t_client": pose.t_client}
        if self._ooc is not None:
            self._xr_pose = None
            return XRFrame(base, error="Stereo frames are not available yet for a cube "
                                       "drawn out of core (larger than the in-core limit).",
                           code="out_of_core")
        t_start = time.time()
        self._poll_cube_chunks()
        self._xr_depth_check_mode()
        try:
            eyes, depth_src, per_eye, from_mapper, depth_problem = self._render_eyes_on_thread(pose)
        except _NoRenderToImage as exc:
            # Kept for the session: the window's own pixels from now on,
            # without a volume's depth.
            logger.warning("[render-session %s] stereo: no render-to-image (%s); "
                           "frames without volume depth", self.session_id, exc)
            self._xr_render_to_image = False
            self._set_render_to_image(False)
            eyes, depth_src, per_eye, from_mapper, depth_problem = self._render_eyes_on_thread(pose)
        t_render = time.time()
        depth_unavailable = self._note_depth(pose, depth_src, depth_problem)

        w, h = pose.width, pose.height
        depth_info, depth_payload = None, None
        if depth_unavailable is None:
            # Reduced first, converted after: the window depth grows with the
            # distance, so the nearest of a block is its smallest value.
            dist = np.concatenate(
                [eye_distance(nearest_per_block(d, pose.depth_scale), proj)
                 if d is not None else
                 np.full(nearest_per_block(np.ones((h, w), np.float32),
                                           pose.depth_scale).shape, np.inf, np.float32)
                 for _, d, proj in eyes], axis=1)
            depth_payload, z0, z1 = encode_depth(dist[::-1])   # rows top first
            depth_info = {"format": "u16-zlib", "width": int(dist.shape[1]),
                          "height": int(dist.shape[0]), "scale": pose.depth_scale,
                          "z_near": z0, "z_far": z1, "source": depth_src,
                          "units": "m, along each eye's -z"}
        t_depth = time.time()

        from PIL import Image
        rgba = np.concatenate([c for c, _, _ in eyes], axis=1)
        if from_mapper:
            rgba = over_background(rgba, pose.background)
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
            # How the page may warp this frame to a later pose (issue #187):
            # through its depth, or by the pose alone; without depth, why.
            "reprojection": "depth" if depth_info else "pose",
            "depth_unavailable": depth_unavailable,
            "timing": {"received": pose.t_received, "render_start": t_start,
                       "render_end": t_render, "depth_end": t_depth, "encode_end": t_encode},
            "eye_ms": per_eye,
        })
        self.stats.frames_rendered += 1
        self.stats.last_render_ms = 1000.0 * (t_render - t_start)
        self.stats.last_jpeg_kb = len(jpeg) / 1024.0
        self.stats.bytes_sent += len(jpeg) + len(depth_payload or b"")
        return XRFrame(header, jpeg, depth_payload)

    def _render_eyes_on_thread(self, pose: XRPose):
        """``(eyes, depth source, per-eye timings, colour from the volume
        mapper, depth problem)``: per eye ``(rgba bottom-up, window depth
        bottom-up or None, the projection that depth is in)``; the depth
        source ``"volume-first-hit"``, ``"surface"`` or ``"none"``; the
        problem, when depth was wanted and could not be read or failed its
        check (:func:`volume_depth_problem`), and then no eye has a depth.
        The mono camera, the props' placement and the chrome's visibility
        are put back before returning."""
        import vtk

        ren, win = self._renderer, self._render_window
        w, h = pose.width, pose.height
        if tuple(win.GetSize()) != (w, h):
            win.SetSize(w, h)
        use_volume = (self._volume is not None and bool(self._volume.GetVisibility())
                      and hasattr(self._volume_mapper, "SetRenderToImage")
                      and self._xr_render_to_image)
        surface = self._iso_actor is not None and bool(self._iso_actor.GetVisibility())
        # The ray caster writes a depth at the first sample with some opacity,
        # in composite blending only: MIP and MinIP fill the colour after the
        # ray loop, and their depth image is 1 everywhere.
        blend = int(getattr(getattr(self, "view", None), "blend_mode", 0) or 0)
        mip = use_volume and blend != 0
        want_depth = pose.depth_scale is not None and self._xr_depth_failure is None \
            and not depth_off_on_this_backend() and not mip
        depth_problem = None
        shape = (self._nx, self._ny, self._nz)
        cameras = [stereo_eye(e.view, e.projection, pose.model, shape, pose.z_stretch)
                   for e in pose.eyes]
        stretch = cameras[0]["stretch"]
        placement = None if pose.z_stretch == 1.0 else _vtk_matrix(stretch)
        placed = [(p, p.GetUserMatrix()) for p in (self._volume, self._iso_actor,
                                                   self._iso_lod_actor) if p is not None]
        hidden = [p for p in (self._outline_actor, self._cutting_plane_actor)
                  if p is not None and p.GetVisibility()]
        mono_cam = ren.GetActiveCamera()
        mono_bg = ren.GetBackground()
        # The mono view's sampling (adaptive, or the drag level's), put back
        # after the eyes are drawn with the pose's fixed step.
        mapper = self._volume_mapper
        sampling = ((mapper.GetAutoAdjustSampleDistances(), mapper.GetSampleDistance(),
                     mapper.GetImageSampleDistance())
                    if mapper is not None and hasattr(mapper, "GetSampleDistance") else None)
        axes_drawn = self._axes_renderer.GetDraw() if self._axes_renderer is not None else None
        if self._xr_camera is None:
            self._xr_camera = vtk.vtkCamera()
            self._xr_camera.UseExplicitProjectionTransformMatrixOn()
        cam = self._xr_camera
        out, per_eye = [], []
        try:
            for p, _ in placed:
                p.SetUserMatrix(placement)
            for p in hidden:
                p.SetVisibility(0)
            if self._axes_renderer is not None:
                self._axes_renderer.SetDraw(0)
            ren.SetBackground(*pose.background)
            ren.SetActiveCamera(cam)
            self._set_render_to_image(use_volume)
            if sampling is not None:
                mapper.SetAutoAdjustSampleDistances(0)
                mapper.SetSampleDistance(pose.sample_distance)
                mapper.SetImageSampleDistance(1.0)
            for eye, geometry in zip(pose.eyes, cameras):
                position, focal, up = camera_from_view(geometry["view"])
                cam.SetPosition(*position)
                cam.SetFocalPoint(*focal)
                cam.SetViewUp(*up)
                cam.SetExplicitProjectionTransformMatrix(_vtk_matrix(geometry["projection"]))
                cam.SetClippingRange(*geometry["clipping"])
                t0 = time.perf_counter()
                win.Render()
                t1 = time.perf_counter()
                depth = None
                if use_volume:
                    rgba = _image_pixels(self._volume_mapper.GetColorImage, (h, w, 4))
                    if want_depth and depth_problem is None:
                        # The colour came back; the depth may not (issue #187).
                        try:
                            depth = _image_pixels(self._volume_mapper.GetDepthImage, (h, w))
                            depth_problem = volume_depth_problem(depth, rgba[..., 3])
                        except _NoRenderToImage as exc:
                            depth_problem = DepthProblem(
                                f"the volume mapper gave no depth image ({exc})", True)
                else:
                    rgba = self._window_rgba(win, w, h)
                    if want_depth and surface and depth_problem is None:
                        status, depth = self._window_depth(win, w, h)
                        depth_problem = surface_depth_problem(depth, status)
                t2 = time.perf_counter()
                out.append((np.array(rgba), None if depth is None else
                            np.array(depth, dtype=np.float32), geometry["depth_projection"]))
                per_eye.append({"eye": eye.eye, "render": round(1000 * (t1 - t0), 3),
                                "readback": round(1000 * (t2 - t1), 3)})
        finally:
            if sampling is not None:
                mapper.SetAutoAdjustSampleDistances(sampling[0])
                mapper.SetSampleDistance(sampling[1])
                mapper.SetImageSampleDistance(sampling[2])
            ren.SetActiveCamera(mono_cam)
            ren.SetBackground(*mono_bg)
            for p, previous in placed:
                p.SetUserMatrix(previous)
            for p in hidden:
                p.SetVisibility(1)
            if axes_drawn is not None:
                self._axes_renderer.SetDraw(axes_drawn)
        source = "volume-first-hit" if use_volume else ("surface" if surface else "none")
        if depth_problem is not None:
            # Both eyes or neither: a frame half with depth would be warped
            # two ways.
            out = [(c, None, proj) for c, _, proj in out]
        elif (mip and pose.depth_scale is not None and self._xr_depth_failure is None
              and not depth_off_on_this_backend()):
            depth_problem = DepthProblem(
                "no first-hit depth in " + ("MIP" if blend == 1 else "MinIP"))
        elif (want_depth and not use_volume and not surface and self._volume is not None
              and bool(self._volume.GetVisibility())):
            depth_problem = DepthProblem("the volume mapper cannot render to an image on this "
                                         "GL (colour read from the window, no volume depth)")
        return out, source, per_eye, use_volume, depth_problem

    @staticmethod
    def _window_rgba(win, w: int, h: int) -> np.ndarray:
        """The window's back buffer, RGBA rows bottom-up."""
        import vtk
        from vtk.util.numpy_support import vtk_to_numpy
        grab = vtk.vtkWindowToImageFilter()
        grab.SetInput(win)
        grab.ReadFrontBufferOff()
        grab.SetInputBufferTypeToRGBA()
        grab.Update()
        return vtk_to_numpy(grab.GetOutput().GetPointData().GetScalars()).reshape(h, w, 4)

    @staticmethod
    def _window_depth(win, w: int, h: int) -> tuple[int | None, np.ndarray]:
        """``(GetZbufferData's status, the window's depth buffer, rows bottom-up)``.
        A GLES context refuses the read (status not VTK_OK); what the array
        then holds is not a depth, and the check says so."""
        import vtk
        from vtk.util.numpy_support import vtk_to_numpy
        z = vtk.vtkFloatArray()
        status = win.GetZbufferData(0, 0, w - 1, h - 1, z)
        values = vtk_to_numpy(z)
        if values.size != w * h:
            return status, np.full((h, w), np.nan, np.float32)
        return (None if status is None else int(status)), values.reshape(h, w)

    def _xr_depth_check_mode(self) -> None:
        """A depth readback given up in one blend or render mode is tried again
        in another (a GL may read a surface's depth and not a volume's)."""
        blend = int(getattr(getattr(self, "view", None), "blend_mode", 0) or 0)
        surface = self._iso_actor is not None and bool(self._iso_actor.GetVisibility())
        key = (blend, surface)
        if getattr(self, "_xr_depth_mode", None) != key:
            self._xr_depth_mode = key
            self._xr_depth_failure = None
            self._xr_depth_strikes = 0

    def _note_depth(self, pose: XRPose, depth_src: str, problem: str | None) -> str | None:
        """Why this frame goes without depth (None: it has one), and the
        session's record of depth failures: after :data:`DEPTH_STRIKES` bad
        frames in a row it reads no depth any more."""
        if pose.depth_scale is None:
            return "not requested"
        if self._xr_depth_failure is not None:
            return self._xr_depth_failure
        if depth_off_on_this_backend():
            return "turned off on this backend (VISIVO_XR_DEPTH=0)"
        if problem is not None:
            if not getattr(problem, "structural", False):
                return str(problem)       # this frame only; nothing counted
            self._xr_depth_strikes += 1
            if self._xr_depth_strikes >= DEPTH_STRIKES:
                self._xr_depth_failure = str(problem)
                logger.warning("[render-session %s] stereo: depth readback unusable (%s); "
                               "frames without depth from now on (reprojection by pose)",
                               self.session_id, problem)
            return str(problem)
        self._xr_depth_strikes = 0
        if depth_src == "none":
            return "nothing on show gives a depth (neither a volume nor a surface)"
        return None
