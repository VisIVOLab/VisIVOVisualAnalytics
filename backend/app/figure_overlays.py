"""What a publication figure draws over the image: regions, contours, text and
arrow annotations, the beam, a scale bar and a compass (issue #41).

Everything the viewer shows over an image reaches the figure in the one form the
backend already speaks, the canonical region of ``regions.py``: a contour line
is a ``polygon`` or ``line`` with ``props.color/width/dash``, an annotation a
``text`` or ``vector``, a region the shape it is. A region in a sky frame is
brought to image pixels with ``regions.convert_regions``; the drawing below only
knows pixels. Pixels are 1-based, as in region files; the axes of the figure are
the 0-based pixels of the array, so each position loses one on the way.
"""
from __future__ import annotations

import math
from typing import Any

import numpy as np

# More vertices than this are refused: a figure is for people, and a contour of
# noise at five million vertices is neither readable nor printable.
MAX_OVERLAY_POINTS = 3_000_000

DEFAULT_REGION_COLOR = "#00e5ff"


def _color(value: Any, fallback: str) -> str:
    """A matplotlib colour name or hex, or ``fallback``: a region file may carry a
    colour matplotlib does not know and one of them must not fail the figure."""
    from matplotlib.colors import is_color_like  # noqa: PLC0415
    return str(value) if isinstance(value, str) and is_color_like(value) else fallback


def is_light(color: str) -> bool:
    """Whether ``color`` is closer to white than to black (Rec. 601 luma)."""
    from matplotlib.colors import to_rgb  # noqa: PLC0415
    r, g, b = to_rgb(color)
    return 0.299 * r + 0.587 * g + 0.114 * b > 0.5


def _box_color(text_color: str) -> str:
    """Opaque background of a text annotation: dark for a light text, pale for a
    dark one, so the text reads on a light colour map as on a dark one."""
    return "#2b2b2b" if is_light(text_color) else "#f2f2f2"


def _xy(p) -> tuple[float, float]:
    return float(p[0]) - 1.0, float(p[1]) - 1.0


def _style(region: dict[str, Any], color: str) -> dict[str, Any]:
    props = region.get("props") or {}
    try:
        width = float(props.get("width", 1.0))
    except (TypeError, ValueError):
        width = 1.0
    return {"color": _color(props.get("color"), color),
            "lw": width if math.isfinite(width) and width > 0 else 1.0,
            "ls": (0, (4, 3)) if props.get("dash") else "-"}


def count_points(regions: list[dict[str, Any]]) -> int:
    return sum(len(r.get("points") or ()) + (1 if "center" in r else 0) for r in regions)


def _rotated_rect(cx, cy, w, h, angle_deg):
    t = math.radians(angle_deg)
    c, s = math.cos(t), math.sin(t)
    corners = np.array([[-w / 2, -h / 2], [w / 2, -h / 2], [w / 2, h / 2], [-w / 2, h / 2]])
    return corners @ np.array([[c, s], [-s, c]]) + [cx, cy]


def draw_regions(ax, regions: list[dict[str, Any]], *, default_color: str = DEFAULT_REGION_COLOR,
                 font_size: float = 10.0) -> int:
    """Draw canonical image-frame regions (pixels, 1-based) on ``ax``; return how
    many were drawn. Polylines of one style go into one collection, so a contour
    map of tens of thousands of lines is a handful of artists."""
    from matplotlib.collections import LineCollection  # noqa: PLC0415
    from matplotlib.patches import Circle, Ellipse, Polygon  # noqa: PLC0415

    lines: dict[tuple, list[np.ndarray]] = {}
    drawn = 0
    for r in regions:
        shape = r.get("shape")
        st = _style(r, default_color)
        key = (st["color"], st["lw"], str(st["ls"]))
        try:
            if shape in ("polygon", "line"):
                pts = np.array([_xy(p) for p in r["points"]], dtype=float)
                if shape == "polygon" and len(pts) > 2:
                    pts = np.vstack([pts, pts[:1]])
                if len(pts) >= 2 and np.isfinite(pts).all():
                    lines.setdefault(key + (st["ls"],), []).append(pts)
                    drawn += 1
                continue
            cx, cy = _xy(r["center"]) if "center" in r else (0.0, 0.0)
            ang = float(r.get("angle_deg", 0.0))
            kw = {"fill": False, "edgecolor": st["color"], "linewidth": st["lw"],
                  "linestyle": st["ls"]}
            if shape == "circle":
                ax.add_patch(Circle((cx, cy), float(r["radius"]), **kw))
            elif shape == "annulus":
                for rad in r["radii"]:
                    ax.add_patch(Circle((cx, cy), float(rad), **kw))
            elif shape == "ellipse":
                a, b = r["semi_axes"]
                ax.add_patch(Ellipse((cx, cy), 2 * float(a), 2 * float(b), angle=ang, **kw))
            elif shape == "ellipse_annulus":
                for a, b in (r["inner"], r["outer"]):
                    ax.add_patch(Ellipse((cx, cy), 2 * float(a), 2 * float(b), angle=ang, **kw))
            elif shape == "box":
                w, h = r["size"]
                ax.add_patch(Polygon(_rotated_rect(cx, cy, float(w), float(h), ang), closed=True, **kw))
            elif shape == "box_annulus":
                for w, h in (r["inner"], r["outer"]):
                    ax.add_patch(Polygon(_rotated_rect(cx, cy, float(w), float(h), ang),
                                         closed=True, **kw))
            elif shape == "point":
                ax.plot([cx], [cy], marker="x", color=st["color"], ms=6, mew=st["lw"], ls="none")
            elif shape == "vector":
                length = float(r["length"])
                t = math.radians(ang)
                # The head is at the far end, as ds9 draws a vector.
                ax.annotate("", xy=(cx + length * math.cos(t), cy + length * math.sin(t)),
                            xytext=(cx, cy),
                            arrowprops={"arrowstyle": "-|>", "color": st["color"],
                                        "lw": st["lw"], "shrinkA": 0, "shrinkB": 0})
            elif shape == "text":
                ax.text(cx, cy, str(r.get("text", "")), color=st["color"], fontsize=font_size,
                        ha="left", va="bottom",
                        # Opaque: EPS has no transparency and would print a black box.
                        # The opposite tone of the text, so it reads on any colour map.
                        bbox={"boxstyle": "round,pad=0.2", "fc": _box_color(st["color"]),
                              "ec": "none"})
            else:
                continue
            drawn += 1
        except (KeyError, TypeError, ValueError):
            # A malformed region is left out rather than failing the figure; the
            # caller reports how many of the list were drawn.
            continue
    for (color, lw, _ls_text, ls), segs in lines.items():
        ax.add_collection(LineCollection(segs, colors=color, linewidths=lw, linestyles=ls))
    return drawn


# ── Sky geometry from the WCS ────────────────────────────────────────────────
# Lengths and directions on the pixel grid are read with regions._PixelFrame (the
# one place that turns a sky angle into a pixel angle through the WCS, whatever
# the rotation, mirror and skew) and regions._north_rotation (the angle between
# the north of two frames); nothing here measures a WCS a second way.


def pixel_frame(wcs):
    """The ``regions._PixelFrame`` of the celestial part of ``wcs`` for 0-based
    pixels, or None when there is no usable sky grid. ``scale_deg`` of it is the
    mean sky size of a pixel."""
    if not wcs.has_celestial:
        return None
    from . import regions as R  # noqa: PLC0415
    try:
        pf = R._PixelFrame(wcs.celestial, 0)  # noqa: SLF001
    except (ValueError, TypeError):
        return None
    return pf if math.isfinite(pf.scale_deg) and pf.scale_deg > 0 else None


def sky_pixel_angle(pf, x: float, y: float, pa_deg: float, frame=None) -> float | None:
    """Angle from pixel +x, counter-clockwise, of the direction at position angle
    ``pa_deg`` (from north through east) at 0-based pixel (x, y), or None off the
    projection. The position angle is that of the WCS's own frame, or of ``frame``
    (an astropy frame) when one is given: the north of the two differ by an angle
    that depends on where in the sky the pixel is."""
    from . import regions as R  # noqa: PLC0415
    try:
        lon, lat = (float(v[0]) for v in pf.to_sky([x], [y]))
        if frame is not None:
            pa_deg = pa_deg - float(R._north_rotation(lon, lat, pf.frame, frame))  # noqa: SLF001
        # ds9's sky angle is the position angle plus 90.
        return pf.sky_angle_to_pix(lon, lat, pa_deg + 90.0)
    except (R.RegionParseError, ValueError):
        return None


# ── Corner placement ─────────────────────────────────────────────────────────

CORNERS = ("bottom-left", "bottom-right", "top-left", "top-right")


def corner_xy(ax, corner: str, margin_frac: float = 0.07) -> tuple[float, float, int, int]:
    """Data position near a corner of the image area and the direction pointing
    into the image along x and y (+1 or -1)."""
    x0, x1 = sorted(ax.get_xlim())
    y0, y1 = sorted(ax.get_ylim())
    mx, my = (x1 - x0) * margin_frac, (y1 - y0) * margin_frac
    right = corner.endswith("right")
    top = corner.startswith("top")
    return (x1 - mx if right else x0 + mx, y1 - my if top else y0 + my,
            -1 if right else 1, -1 if top else 1)


def halo(color: str):
    """A thin outline in the opposite tone, so a white mark reads over the white of
    a blanked corner and a black one over a dark map."""
    from matplotlib import patheffects  # noqa: PLC0415
    return [patheffects.withStroke(linewidth=3.0, foreground="black" if is_light(color) else "white")]


def draw_beam(ax, wcs, beam, corner: str, color: str, notes: list[str]) -> bool:
    """The beam as an ellipse of its true size and orientation (BMAJ, BMIN, BPA
    of the header, or the beam typed for the dataset), in a corner."""
    from matplotlib.patches import Ellipse  # noqa: PLC0415
    pf = pixel_frame(wcs)
    if pf is None:
        notes.append("Beam: the image has no sky coordinates, so its size cannot be drawn.")
        return False
    scale = pf.scale_deg
    if not beam or not beam.major or not beam.minor:
        notes.append("Beam: the header has no BMAJ/BMIN (and none was typed for the dataset).")
        return False
    major, minor = float(beam.major) / scale, float(beam.minor) / scale
    cx, cy, dx, dy = corner_xy(ax, corner, 0.0)
    x0, x1 = sorted(ax.get_xlim())
    y0, y1 = sorted(ax.get_ylim())
    pad = 0.6 * major + 0.03 * min(x1 - x0, y1 - y0)
    cx = cx + dx * pad
    cy = cy + dy * pad
    angle = sky_pixel_angle(pf, cx, cy, float(beam.pa or 0.0))
    if angle is None:
        angle = float(beam.pa or 0.0) + 90.0
    if major > 0.4 * min(x1 - x0, y1 - y0):
        notes.append("Beam: it covers a large part of the image.")
    ax.add_patch(Ellipse((cx, cy), major, minor, angle=angle, facecolor="white",
                         edgecolor="black", linewidth=1.0, zorder=5))
    return True


_UNIT_DEG = {"arcsec": 1.0 / 3600.0, "arcmin": 1.0 / 60.0, "deg": 1.0}
_UNIT_SYMBOL = {"arcsec": "″", "arcmin": "′", "deg": "°"}


def nice_length(target: float) -> float:
    """1, 2 or 5 times a power of ten at or below ``target``."""
    if not target > 0 or not math.isfinite(target):
        return 0.0
    mag = 10.0 ** math.floor(math.log10(target))
    for m in (5.0, 2.0, 1.0):
        if m * mag <= target:
            return m * mag
    return mag


def auto_unit(length_deg: float) -> str:
    """The unit a bar of ``length_deg`` is written in: arcsec below 60", arcmin below
    60', else degrees, so a survey image does not get a bar of 5000"."""
    if length_deg * 3600.0 < 60.0:
        return "arcsec"
    return "arcmin" if length_deg * 60.0 < 60.0 else "deg"


def draw_scale_bar(ax, wcs, length: float, unit: str, corner: str, color: str,
                   font_size: float, notes: list[str]) -> bool:
    """An angular scale bar along x. ``length`` 0 picks 1, 2 or 5 x 10^n of
    ``unit`` that is about a fifth of the image width. ``unit`` ``auto`` is the
    unit that suits that fifth (``auto_unit``); a length typed with it is in that
    unit, which the label states."""
    pf = pixel_frame(wcs)
    if pf is None:
        notes.append("Scale bar: the image has no sky coordinates.")
        return False
    scale = pf.scale_deg
    if unit != "auto" and unit not in _UNIT_DEG:
        raise ValueError(f"Scale bar unit must be auto or one of {sorted(_UNIT_DEG)}.")
    x0, x1 = sorted(ax.get_xlim())
    if unit == "auto":
        unit = auto_unit(0.2 * (x1 - x0) * scale)
    width_unit = (x1 - x0) * scale / _UNIT_DEG[unit]
    length = float(length)
    if length <= 0:
        length = nice_length(0.2 * width_unit)
    pix = length * _UNIT_DEG[unit] / scale
    if length <= 0 or pix > (x1 - x0):
        notes.append("Scale bar: the length is zero or longer than the image.")
        return False
    cx, cy, dx, _dy = corner_xy(ax, corner)
    xa, xb = (cx, cx + dx * pix)
    fx = halo(color)
    ax.plot([xa, xb], [cy, cy], color=color, lw=2.0, solid_capstyle="butt", zorder=5,
            path_effects=fx)
    for xe in (xa, xb):
        ax.plot([xe, xe], [cy - 0.008 * (x1 - x0), cy + 0.008 * (x1 - x0)], color=color, lw=1.5,
                zorder=5, path_effects=fx)
    label = f"{length:g}{_UNIT_SYMBOL[unit]}"
    ax.text((xa + xb) / 2, cy + 0.015 * (x1 - x0), label, color=color, fontsize=font_size,
            ha="center", va="bottom", zorder=5, path_effects=fx)
    return True


def draw_compass(ax, wcs, frame: str, corner: str, color: str, font_size: float,
                 notes: list[str]) -> bool:
    """North and east arrows of the sky frame the axes are labelled in (``frame``,
    "" for the WCS's own), drawn at the pixel grid's orientation at the centre of
    the image and named after the frame: N, E, or b, l for Galactic. A frame other
    than the WCS's is turned to by the angle between the two norths."""
    from . import regions as R  # noqa: PLC0415
    pf = pixel_frame(wcs)
    if pf is None:
        notes.append("Compass: the image has no sky coordinates.")
        return False
    try:
        native = R.native_frame_name(wcs.celestial)
    except ValueError:
        native = ""
    target = R.sky_frame(frame) if frame and frame != native else None
    x0, x1 = sorted(ax.get_xlim())
    y0, y1 = sorted(ax.get_ylim())
    centre = (0.5 * (x0 + x1), 0.5 * (y0 + y1))
    angles = [sky_pixel_angle(pf, *centre, pa, target) for pa in (0.0, 90.0)]
    if None in angles:
        notes.append("Compass: the centre of the image is off its sky projection.")
        return False
    names = ("b", "l") if (frame or native) == "galactic" else ("N", "E")
    cx, cy, dx, dy = corner_xy(ax, corner, 0.12)
    length = 0.08 * min(x1 - x0, y1 - y0)
    # Anchored so that both arrows stay inside the corner they are put in.
    ox, oy = cx + dx * 1.2 * length, cy + dy * 1.2 * length
    for ang, name in zip(angles, names):
        vec = (math.cos(math.radians(ang)), math.sin(math.radians(ang)))
        tip = (ox + vec[0] * length, oy + vec[1] * length)
        arrow = ax.annotate("", xy=tip, xytext=(ox, oy), zorder=5,
                            arrowprops={"arrowstyle": "-|>", "color": color, "lw": 1.5,
                                        "shrinkA": 0, "shrinkB": 0})
        arrow.arrow_patch.set_path_effects(halo(color))
        ax.text(ox + vec[0] * length * 1.3, oy + vec[1] * length * 1.3, name, color=color,
                fontsize=font_size, ha="center", va="center", zorder=5, path_effects=halo(color))
    return True
