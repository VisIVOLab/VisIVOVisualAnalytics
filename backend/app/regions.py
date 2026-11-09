"""Region interchange — parse/serialize CASA CRTF and DS9 region formats.

This is interop glue, not a new analysis feature: radio astronomers carry
regions between CASA, CARTA, ds9 and pyregion. VisIVO can already *draw*
regions (box/circle/ellipse/polygon/point/annulus); this module lets it read
and write them in the two standard text formats so a region defined in CARTA
can be loaded here and vice-versa.

Canonical region model (plain dict, JSON-friendly):

    {
      "shape":  "circle" | "ellipse" | "box" | "polygon" | "point" | "annulus",
      "frame":  "image" | "icrs" | "fk5" | "fk4" | "galactic" | "ecliptic",
      "center": [x, y],          # pixels if frame=="image" else degrees
      "radius": r,               # circle
      "radii":  [inner, outer],  # annulus
      "semi_axes": [maj, min],   # ellipse
      "size":   [width, height], # box
      "angle_deg": a,            # ellipse / box position angle
      "points": [[x, y], ...],   # polygon (pixels or degrees per frame)
      "unit":   "pix" | "deg" | "arcsec" | "arcmin",  # unit of sizes/radii
      "props":  {"color": ..., "text": ...},          # optional cosmetics
    }

World centers are always decimal degrees in the region's own frame (``fk5``
is J2000, ``fk4`` B1950, ``ecliptic`` the mean ecliptic of J2000); ``unit``
governs only the *sizes* (radius, semi-axes, box w/h, annulus radii), and all
sizes of one region are held in that one unit. A region's sizes may be pixels
in a sky frame or angles in the image frame, as ds9 and CRTF allow; the
conversion to the image frame turns them into pixels.

``angle_deg`` follows ds9: counter-clockwise from the first axis, i.e. from
pixel +x in the image frame and from the longitude axis of the region's own
sky frame. Changing the sky frame rotates it by the angle between the two
frames' north directions at the region's centre (see ``convert_regions``).
"""
from __future__ import annotations

import math
import re
from typing import Any

# DS9 coordinate-system lines → canonical frame.
_DS9_FRAMES = {
    "image": "image", "physical": "image",
    "fk5": "fk5", "j2000": "fk5", "fk4": "fk4", "b1950": "fk4",
    "icrs": "icrs", "galactic": "galactic", "ecliptic": "ecliptic",
}
# DS9 systems that need information a region file does not carry (the image's
# default WCS, detector geometry): refused rather than read in the wrong frame.
_DS9_UNSUPPORTED = {"wcs", "linear", "amplifier", "detector"} | {f"wcs{c}" for c in "abcdefghijklmnopqrstuvwxyz"}
_WORLD_FRAMES = {"icrs", "fk5", "fk4", "galactic", "ecliptic"}
_EQUATORIAL = {"icrs", "fk5", "fk4"}          # longitude is RA (hours when sexagesimal)

_ANGLE_TO_DEG = {"deg": 1.0, "arcmin": 1.0 / 60.0, "arcsec": 1.0 / 3600.0}


class RegionParseError(ValueError):
    """Raised when region text cannot be parsed (or a region cannot be converted)."""


# ── helpers ──────────────────────────────────────────────────────────────────

_FLOAT = r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?"
# Size suffixes of both formats → (canonical unit, factor into it). ds9: d r p i ' ";
# CRTF: deg rad arcmin arcsec mas pix. Radians and mas are folded into deg / arcsec.
_SIZE_UNITS = {
    "pix": ("pix", 1.0), "p": ("pix", 1.0), "i": ("pix", 1.0),
    "deg": ("deg", 1.0), "d": ("deg", 1.0),
    "rad": ("deg", 180.0 / math.pi), "r": ("deg", 180.0 / math.pi),
    "arcmin": ("arcmin", 1.0), "'": ("arcmin", 1.0),
    "arcsec": ("arcsec", 1.0), '"': ("arcsec", 1.0),
    "mas": ("arcsec", 1e-3),
}
_SIZE_RE = re.compile(rf"({_FLOAT})\s*(pix|deg|rad|arcmin|arcsec|mas|d|r|p|i|'|\")?")


def _sexagesimal_to_deg(token: str, is_ra: bool) -> float | None:
    """Convert a sexagesimal position token to degrees, or None if not sexagesimal.

    Accepts ``18h12m24s`` / ``18:12:24`` (RA, hours when ``is_ra``),
    ``-23d11m00s`` / ``-23:11:00`` and CASA's ``-23.11.00`` (degrees). Decimal
    tokens return None so the caller reads them as already-decimal.
    """
    t = token.strip()
    if re.fullmatch(rf"{_FLOAT}\s*[a-z]*", t):
        return None  # plain decimal value with an optional unit suffix, not DMS
    sign = -1.0 if t.startswith("-") else 1.0
    body = t.lstrip("+-")
    if "h" in body and "m" in body:  # RA in h m s, e.g. 18h12m24s
        parts = re.split(r"[hms]", body)
        h, m, s = (float(parts[0]), float(parts[1]), float(parts[2] or 0))
        return sign * (h + m / 60.0 + s / 3600.0) * 15.0
    if "d" in body and "m" in body:  # Dec in d m s, e.g. -23d11m00s
        parts = re.split(r"[dms]", body)
        d, m, s = (float(parts[0]), float(parts[1]), float(parts[2] or 0))
        return sign * (d + m / 60.0 + s / 3600.0)
    if ":" in body:  # colon sexagesimal: hours for RA, degrees otherwise
        a, b, c = (body.split(":") + ["0", "0"])[:3]
        val = float(a) + float(b) / 60.0 + float(c) / 3600.0
        return sign * (val * 15.0 if is_ra else val)
    if re.fullmatch(r"\d+\.\d+\.\d+(?:\.\d*)?", body):  # CASA dd.mm.ss[.s]
        d, m, s = body.split(".", 2)
        return sign * (float(d) + float(m) / 60.0 + float(s) / 3600.0)
    return None


def _parse_size(token: str, default_unit: str) -> tuple[float, str]:
    """Parse one size token → (value, unit) with unit in {pix, deg, arcmin, arcsec}.

    Each token carries its own unit: ``30arcsec`` / ``2.5'`` / ``1.3"`` /
    ``0.01d`` / ``0.1r`` / ``20i`` / ``100pix``. A bare number takes
    ``default_unit``, which is what ds9 does: pixels in the image frame,
    degrees in a sky frame.
    """
    m = _SIZE_RE.fullmatch(token.strip())
    if not m:
        raise RegionParseError(f"Cannot parse size token '{token}'.")
    val = float(m.group(1))
    suffix = m.group(2)
    if not suffix:
        return val, default_unit
    unit, factor = _SIZE_UNITS[suffix]
    return val * factor, unit


def _parse_sizes(tokens: list[str], default_unit: str) -> tuple[list[float], str]:
    """Parse the size tokens of one region into values sharing one unit.

    Angular tokens are converted to the unit of the first one, so
    ``1', 30"`` is ``[1, 0.5]`` arcmin. Pixel and angular sizes cannot share a
    unit without a WCS, so a region mixing them is refused.
    """
    parsed = [_parse_size(t, default_unit) for t in tokens]
    units = {u for _, u in parsed}
    if "pix" in units and len(units) > 1:
        raise RegionParseError(
            "A region mixing pixel and angular sizes is not supported: " + ", ".join(tokens))
    unit = parsed[0][1]
    if unit == "pix":
        return [v for v, _ in parsed], unit
    return [v * _ANGLE_TO_DEG[u] / _ANGLE_TO_DEG[unit] for v, u in parsed], unit


def _parse_angle(token: str) -> float:
    """Parse a rotation angle (bare number = degrees; ``deg``/``d``/``rad``/``r``)."""
    m = re.fullmatch(rf"({_FLOAT})\s*(deg|rad|d|r)?", token.strip())
    if not m:
        raise RegionParseError(f"Cannot parse angle token '{token}'.")
    val = float(m.group(1))
    return math.degrees(val) if m.group(2) in ("rad", "r") else val


def _fmt_num(v: float) -> str:
    """Compact number formatting: no trailing zeros, and enough digits (12
    significant) that a sky position keeps sub-milliarcsecond precision."""
    v = float(v)
    if v == int(v) and abs(v) < 1e15:
        return str(int(v))
    return f"{v:.12g}"


# ── DS9 ──────────────────────────────────────────────────────────────────────

_DS9_SHAPE_RE = re.compile(r"^\s*[-+]?\s*(\w+)\s*\(([^)]*)\)\s*(?:#(.*))?$")


def _ds9_props(comment: str | None) -> dict[str, Any]:
    props: dict[str, Any] = {}
    if not comment:
        return props
    for m in re.finditer(r"(\w+)\s*=\s*(\{[^}]*\}|\"[^\"]*\"|\S+)", comment):
        key, val = m.group(1), m.group(2).strip("{}\"")
        if key in ("color", "text"):
            props[key] = val
    return props


def _ds9_coord(token: str, frame: str, is_lon: bool) -> float:
    """One ds9 position value: pixels in the image frame, degrees in a sky frame.

    Sky values may be sexagesimal (RA in hours only for equatorial frames) or
    decimal with a ``d``/``r`` suffix; a pixel-suffixed position in a sky
    frame has no meaning without a WCS and is refused.
    """
    t = token.strip()
    if frame not in _WORLD_FRAMES:
        return float(re.sub(r"[pi]$", "", t))
    sx = _sexagesimal_to_deg(t, is_ra=is_lon and frame in _EQUATORIAL)
    if sx is not None:
        return sx
    m = re.fullmatch(rf"({_FLOAT})\s*(d|deg|r|rad)?", t)
    if not m:
        raise RegionParseError(f"Cannot parse {frame} coordinate '{token}'.")
    val = float(m.group(1))
    return math.degrees(val) if m.group(2) in ("r", "rad") else val


def _ds9_center(xs: str, ys: str, frame: str) -> list[float]:
    return [_ds9_coord(xs, frame, True), _ds9_coord(ys, frame, False)]


def _ds9_statements(text: str):
    """Yield ds9 statements: lines, further split at ``;`` outside a comment,
    so the common one-line form ``fk5;circle(...)`` reads like two lines."""
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        hash_at = line.find("#")
        code, comment = (line, "") if hash_at < 0 else (line[:hash_at], line[hash_at:])
        parts = [p.strip() for p in code.split(";")]
        for p in parts[:-1]:
            if p:
                yield p
        last = (parts[-1] + " " + comment).strip() if parts else comment
        if last:
            yield last


def parse_ds9(text: str) -> list[dict[str, Any]]:
    """Parse a DS9 region file into canonical region dicts."""
    regions: list[dict[str, Any]] = []
    frame = "image"
    for line in _ds9_statements(text):
        if line.startswith("#") or line.lower().startswith("global"):
            continue
        low = line.lower()
        if low in _DS9_FRAMES:
            frame = _DS9_FRAMES[low]
            continue
        if low in _DS9_UNSUPPORTED:
            raise RegionParseError(f"DS9 coordinate system '{line}' is not supported; "
                                   "save the regions in image, fk5, fk4, icrs, galactic or ecliptic.")
        m = _DS9_SHAPE_RE.match(line)
        if not m:
            continue
        shape = m.group(1).lower()
        args = [a.strip() for a in m.group(2).split(",") if a.strip()]
        props = _ds9_props(m.group(3))
        default_unit = "pix" if frame == "image" else "deg"
        unit = default_unit
        try:
            if shape == "circle":
                c = _ds9_center(args[0], args[1], frame)
                (r,), unit = _parse_sizes(args[2:3], default_unit)
                regions.append({"shape": "circle", "frame": frame, "center": c,
                                "radius": r, "unit": unit, "props": props})
            elif shape == "ellipse":
                c = _ds9_center(args[0], args[1], frame)
                (maj, mn), unit = _parse_sizes(args[2:4], default_unit)
                ang = _parse_angle(args[4]) if len(args) > 4 else 0.0
                regions.append({"shape": "ellipse", "frame": frame, "center": c,
                                "semi_axes": [maj, mn], "angle_deg": ang, "unit": unit, "props": props})
            elif shape == "box":
                c = _ds9_center(args[0], args[1], frame)
                (w, h), unit = _parse_sizes(args[2:4], default_unit)
                ang = _parse_angle(args[4]) if len(args) > 4 else 0.0
                regions.append({"shape": "box", "frame": frame, "center": c,
                                "size": [w, h], "angle_deg": ang, "unit": unit, "props": props})
            elif shape == "annulus":
                c = _ds9_center(args[0], args[1], frame)
                (r1, r2), unit = _parse_sizes(args[2:4], default_unit)
                regions.append({"shape": "annulus", "frame": frame, "center": c,
                                "radii": [r1, r2], "unit": unit, "props": props})
            elif shape == "polygon":
                vals = [_ds9_coord(a, frame, i % 2 == 0) for i, a in enumerate(args)]
                pts = [[vals[i], vals[i + 1]] for i in range(0, len(vals) - 1, 2)]
                regions.append({"shape": "polygon", "frame": frame, "points": pts,
                                "unit": unit, "props": props})
            elif shape == "point":
                c = _ds9_center(args[0], args[1], frame)
                regions.append({"shape": "point", "frame": frame, "center": c,
                                "unit": unit, "props": props})
        except (IndexError, ValueError) as exc:
            raise RegionParseError(f"Malformed DS9 {shape}: {line} ({exc})") from exc
    return regions


# Pixel sizes in a sky frame need the explicit image-pixel suffix: a bare number
# there means degrees. Angular sizes always carry their suffix.
_DS9_UNIT_SUFFIX = {"deg": "d", "arcmin": "'", "arcsec": '"'}


def _ds9_size_str(val: float, unit: str, frame: str) -> str:
    if unit == "pix":
        return _fmt_num(val) + ("" if frame == "image" else "i")
    if unit not in _DS9_UNIT_SUFFIX:
        raise RegionParseError(f"Unknown size unit '{unit}'.")
    return f"{_fmt_num(val)}{_DS9_UNIT_SUFFIX[unit]}"


def export_ds9(regions: list[dict[str, Any]]) -> str:
    """Serialize canonical regions to a DS9 region file (grouped by frame)."""
    out = ["# Region file format: DS9 version 4.1"]
    # Emit per frame so the coordinate-system line is correct for each block.
    by_frame: dict[str, list[dict]] = {}
    for r in regions:
        frame = r.get("frame", "image")
        if frame != "image" and frame not in _WORLD_FRAMES:
            raise RegionParseError(f"Unknown region frame '{frame}'.")
        by_frame.setdefault(frame, []).append(r)
    for frame, group in by_frame.items():
        out.append(frame)
        for r in group:
            shape = r["shape"]
            unit = r.get("unit", "pix" if frame == "image" else "deg")
            cx, cy = (r["center"] if shape != "polygon" else (0, 0))
            def S(v):  # noqa: E306,N802
                return _ds9_size_str(v, unit, frame)
            if shape == "circle":
                line = f"circle({_fmt_num(cx)},{_fmt_num(cy)},{S(r['radius'])})"
            elif shape == "ellipse":
                maj, mn = r["semi_axes"]
                line = f"ellipse({_fmt_num(cx)},{_fmt_num(cy)},{S(maj)},{S(mn)},{_fmt_num(r.get('angle_deg', 0.0))})"
            elif shape == "box":
                w, h = r["size"]
                line = f"box({_fmt_num(cx)},{_fmt_num(cy)},{S(w)},{S(h)},{_fmt_num(r.get('angle_deg', 0.0))})"
            elif shape == "annulus":
                r1, r2 = r["radii"]
                line = f"annulus({_fmt_num(cx)},{_fmt_num(cy)},{S(r1)},{S(r2)})"
            elif shape == "polygon":
                line = "polygon(" + ",".join(_fmt_num(v) for pt in r["points"] for v in pt) + ")"
            elif shape == "point":
                line = f"point({_fmt_num(cx)},{_fmt_num(cy)})"
            else:
                continue
            props = r.get("props") or {}
            extras = " ".join(f"{k}={v}" for k, v in props.items() if k in ("color", "text"))
            out.append(f"{line} # {extras}" if extras else line)
    return "\n".join(out) + "\n"


# ── CRTF (CASA) ──────────────────────────────────────────────────────────────

_CRTF_COORD_TO_FRAME = {
    "j2000": "fk5", "icrs": "icrs", "galactic": "galactic", "ecliptic": "ecliptic",
    "b1950": "fk4", "b1950_vla": "fk4",
}
_FRAME_TO_CRTF = {"fk5": "J2000", "fk4": "B1950", "icrs": "ICRS",
                  "galactic": "GALACTIC", "ecliptic": "ECLIPTIC"}


def _crtf_pos(token: str, is_ra: bool) -> tuple[float, str]:
    """Parse a CRTF position token → (value, frame_kind) with kind image|world."""
    t = token.strip()
    if t.endswith("pix"):
        return float(t[:-3]), "image"
    if t.endswith("deg"):
        return float(t[:-3]), "world"
    if t.endswith("rad"):
        return math.degrees(float(t[:-3])), "world"
    deg = _sexagesimal_to_deg(t, is_ra)
    if deg is not None:
        return deg, "world"
    return float(t), "world"


def _crtf_modifiers(tail: str) -> tuple[str | None, dict[str, Any]]:
    """Parse trailing ``coord=J2000, color=red, label='x'`` → (frame, props).

    A coordinate system the canonical model has no frame for (``SUPERGAL``,
    ``AZEL``, apparent or "of date" systems) is refused instead of being read
    as J2000.
    """
    frame = None
    props: dict[str, Any] = {}
    for m in re.finditer(r"(\w+)\s*=\s*('[^']*'|\"[^\"]*\"|\S+?)(?:,|$)", tail):
        key, val = m.group(1).lower(), m.group(2).strip("'\"")
        if key == "coord":
            frame = _CRTF_COORD_TO_FRAME.get(val.lower())
            if frame is None:
                raise RegionParseError(f"CRTF coordinate system '{val}' is not supported "
                                       "(J2000, B1950, ICRS, GALACTIC, ECLIPTIC are).")
        elif key == "color":
            props["color"] = val
        elif key in ("label", "text"):
            props["text"] = val
    return frame, props


def parse_crtf(text: str) -> list[dict[str, Any]]:
    """Parse a CASA CRTF region file into canonical region dicts.

    A sky region without ``coord=`` takes the file's ``global coord=``, else
    J2000 (CASA would use the image's own frame, which a region file alone
    does not know).
    """
    regions: list[dict[str, Any]] = []
    global_frame: str | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.lower().startswith("global"):
            gframe, _ = _crtf_modifiers(line[6:].strip())
            global_frame = gframe or global_frame
            continue
        mshape = re.match(r"([a-zA-Z]+)\s*\[", line)
        if not mshape:
            continue
        shape = mshape.group(1).lower()
        # Split the geometry (balanced [...] after the shape word) from the tail.
        depth, start = 0, line.index("[")
        end = start
        for i in range(start, len(line)):
            depth += line[i] == "["
            depth -= line[i] == "]"
            if depth == 0:
                end = i
                break
        geom, tail = line[start:end + 1], line[end + 1:].lstrip(", ")
        coord_frame, props = _crtf_modifiers(tail)
        sky_frame = coord_frame or global_frame or "fk5"
        equatorial = sky_frame in _EQUATORIAL
        # Extract bracketed groups: positions like [x, y] and size-pairs [a, b].
        groups = re.findall(r"\[([^\[\]]+)\]", geom)

        def centre(g):
            (cx, kx), (cy, _) = _pair(g, equatorial)
            frame = "image" if kx == "image" else sky_frame
            return [cx, cy], frame, ("pix" if frame == "image" else "deg")

        def sizes(g, default_unit):
            return _parse_sizes([t.strip() for t in g.split(",")][:2], default_unit)

        try:
            if shape in ("circle",):
                c, frame, dflt = centre(groups[0])
                (r,), unit = _parse_sizes([_after_pairs(geom, 1)], dflt)
                regions.append({"shape": "circle", "frame": frame, "center": c,
                                "radius": r, "unit": unit, "props": props})
            elif shape == "ellipse":
                c, frame, dflt = centre(groups[0])
                (maj, mn), unit = sizes(groups[1], dflt)
                ang_tok = _trailing_scalar(geom)
                ang = _parse_angle(ang_tok) if ang_tok else 0.0
                regions.append({"shape": "ellipse", "frame": frame, "center": c,
                                "semi_axes": [maj, mn], "angle_deg": ang, "unit": unit, "props": props})
            elif shape in ("box", "centerbox", "rotbox"):
                (ax, ay), frame, dflt = centre(groups[0])
                if shape == "box":  # two corners [blc],[trc] → center+size
                    (bx, by), _, _ = centre(groups[1])
                    if frame == "image":
                        center = [(ax + bx) / 2.0, (ay + by) / 2.0]
                        size = [abs(bx - ax), abs(by - ay)]
                    else:  # longitude difference across 0/360, on the sky at the centre latitude
                        dlon = (bx - ax + 180.0) % 360.0 - 180.0
                        clat = (ay + by) / 2.0
                        center = [(ax + dlon / 2.0) % 360.0, clat]
                        size = [abs(dlon) * math.cos(math.radians(clat)), abs(by - ay)]
                    unit, ang = dflt, 0.0
                else:  # centerbox / rotbox → center + [w, h] (+ angle)
                    center = [ax, ay]
                    size, unit = sizes(groups[1], dflt)
                    ang_tok = _trailing_scalar(geom)
                    ang = _parse_angle(ang_tok) if (shape == "rotbox" and ang_tok) else 0.0
                regions.append({"shape": "box", "frame": frame, "center": center,
                                "size": size, "angle_deg": ang, "unit": unit, "props": props})
            elif shape == "annulus":
                c, frame, dflt = centre(groups[0])
                (r1, r2), unit = sizes(groups[1], dflt)
                regions.append({"shape": "annulus", "frame": frame, "center": c,
                                "radii": [r1, r2], "unit": unit, "props": props})
            elif shape in ("poly", "polygon"):
                pts, frame = [], None
                for g in groups:
                    p, frame, _ = centre(g)
                    pts.append(p)
                regions.append({"shape": "polygon", "frame": frame or "image", "points": pts,
                                "unit": "pix" if frame in (None, "image") else "deg", "props": props})
            elif shape == "symbol":
                c, frame, dflt = centre(groups[0])
                regions.append({"shape": "point", "frame": frame, "center": c,
                                "unit": dflt, "props": props})
        except (IndexError, ValueError) as exc:
            raise RegionParseError(f"Malformed CRTF {shape}: {line} ({exc})") from exc
    return regions


def _pair(group: str, equatorial: bool = True) -> list[tuple[float, str]]:
    """Parse a CRTF 'x, y' position group → [(x, kind), (y, kind)]."""
    a, b = [t.strip() for t in group.split(",")][:2]
    return [_crtf_pos(a, is_ra=equatorial), _crtf_pos(b, is_ra=False)]


def _after_pairs(geom: str, n_brackets: int) -> str:
    """Return the scalar token that follows ``n_brackets`` [...] groups."""
    # e.g. circle[[x,y], 20pix] → after 1 bracket group → '20pix'
    rest = re.sub(r"^\[", "", geom)
    for _ in range(n_brackets):
        rest = rest[rest.index("]") + 1:]
    return rest.strip(" ,]")


def _trailing_scalar(geom: str) -> str:
    """Return the trailing scalar (e.g. a rotation angle) inside a CRTF geom.

    Strips the outer brackets, removes every ``[...]`` sub-group, and returns
    the last non-empty comma-separated token — e.g. for
    ``[[cx,cy], [a,b], 30deg]`` this yields ``30deg``.
    """
    inner = geom.strip()
    if inner.startswith("[") and inner.endswith("]"):
        inner = inner[1:-1]
    inner = re.sub(r"\[[^\[\]]*\]", "", inner)
    tokens = [t.strip() for t in inner.split(",") if t.strip()]
    return tokens[-1] if tokens else ""


def _crtf_pos_str(v: float, frame: str, is_ra: bool) -> str:
    return f"{_fmt_num(v)}pix" if frame == "image" else f"{_fmt_num(v)}deg"


def _crtf_size_str(v: float, unit: str) -> str:
    if unit not in ("pix", "deg", "arcmin", "arcsec"):
        raise RegionParseError(f"Unknown size unit '{unit}'.")
    return f"{_fmt_num(v)}{unit}"


def export_crtf(regions: list[dict[str, Any]]) -> str:
    """Serialize canonical regions to CASA CRTF text."""
    out = ["#CRTF"]
    for r in regions:
        shape, frame = r["shape"], r.get("frame", "image")
        if frame != "image" and frame not in _FRAME_TO_CRTF:
            raise RegionParseError(f"Unknown region frame '{frame}'.")
        unit = r.get("unit", "pix" if frame == "image" else "deg")
        coord = "" if frame == "image" else f" coord={_FRAME_TO_CRTF[frame]}"
        def P(v, ra):  # noqa: E306,N802
            return _crtf_pos_str(v, frame, ra)
        def Z(v):  # noqa: E306,N802
            return _crtf_size_str(v, unit)
        if shape == "circle":
            cx, cy = r["center"]
            body = f"circle[[{P(cx, True)}, {P(cy, False)}], {Z(r['radius'])}]"
        elif shape == "ellipse":
            cx, cy = r["center"]; maj, mn = r["semi_axes"]
            body = f"ellipse[[{P(cx, True)}, {P(cy, False)}], [{Z(maj)}, {Z(mn)}], {_fmt_num(r.get('angle_deg', 0.0))}deg]"
        elif shape == "box":
            cx, cy = r["center"]; w, h = r["size"]; ang = r.get("angle_deg", 0.0)
            if ang:
                body = f"rotbox[[{P(cx, True)}, {P(cy, False)}], [{Z(w)}, {Z(h)}], {_fmt_num(ang)}deg]"
            else:
                body = f"centerbox[[{P(cx, True)}, {P(cy, False)}], [{Z(w)}, {Z(h)}]]"
        elif shape == "annulus":
            cx, cy = r["center"]; r1, r2 = r["radii"]
            body = f"annulus[[{P(cx, True)}, {P(cy, False)}], [{Z(r1)}, {Z(r2)}]]"
        elif shape == "polygon":
            pts = ", ".join(f"[{P(x, True)}, {P(y, False)}]" for x, y in r["points"])
            body = f"poly[{pts}]"
        elif shape == "point":
            cx, cy = r["center"]
            body = f"symbol[[{P(cx, True)}, {P(cy, False)}], .]"
        else:
            continue
        props = r.get("props") or {}
        extra = "".join(f", {k if k != 'text' else 'label'}='{v}'" for k, v in props.items())
        out.append(body + coord + extra)
    return "\n".join(out) + "\n"


# ── dispatch ─────────────────────────────────────────────────────────────────


def detect_format(text: str) -> str:
    head = text.lstrip()[:400].lower()
    if "#crtf" in head or re.search(r"\b(centerbox|rotbox|symbol)\b", head) or "[[" in head:
        return "crtf"
    if "ds9" in head or re.search(r"^\s*(circle|box|ellipse|polygon|point|annulus)\s*\(", head, re.M):
        return "ds9"
    if re.search(r"\b(fk5|fk4|icrs|image|physical|galactic|ecliptic)\b", head):
        return "ds9"
    return "ds9"


def parse_regions(text: str, fmt: str = "auto") -> list[dict[str, Any]]:
    fmt = (fmt or "auto").lower()
    if fmt == "auto":
        fmt = detect_format(text)
    if fmt == "crtf":
        return parse_crtf(text)
    if fmt == "ds9":
        return parse_ds9(text)
    raise RegionParseError(f"Unknown region format '{fmt}'.")


def export_regions(regions: list[dict[str, Any]], fmt: str) -> str:
    fmt = (fmt or "").lower()
    if fmt == "crtf":
        return export_crtf(regions)
    if fmt == "ds9":
        return export_ds9(regions)
    raise RegionParseError(f"Unknown region format '{fmt}'.")


# ── frame conversion (pixel ↔ sky, sky ↔ sky) ────────────────────────────────


def sky_frame(name: str):
    """The astropy frame a canonical frame name stands for (equinoxes fixed as
    ds9 and CRTF fix them)."""
    from astropy.coordinates import FK4, FK5, ICRS, BarycentricMeanEcliptic, Galactic  # noqa: PLC0415
    frames = {
        "icrs": ICRS, "galactic": Galactic,
        "fk5": lambda: FK5(equinox="J2000"),
        "fk4": lambda: FK4(equinox="B1950"),
        # Barycentric, not geocentric: a direction at infinite distance must
        # not pick up the ~20" annual aberration a geocentric frame would add.
        "ecliptic": lambda: BarycentricMeanEcliptic(equinox="J2000"),
    }
    if name not in frames:
        raise RegionParseError(f"Unknown region frame '{name}'.")
    return frames[name]()


def native_frame_name(wcs) -> str:
    """Canonical name of a celestial WCS's own frame; ``fk5`` when that frame
    has no canonical name (e.g. FK5 of another equinox), so a conversion to
    "the dataset's frame" lands in a frame region files can state."""
    from astropy.coordinates import FK4, FK5, ICRS, BarycentricMeanEcliptic, Galactic  # noqa: PLC0415
    from astropy.time import Time  # noqa: PLC0415
    from astropy.wcs.utils import wcs_to_celestial_frame  # noqa: PLC0415

    f = wcs_to_celestial_frame(wcs)
    if isinstance(f, ICRS):
        return "icrs"
    if isinstance(f, Galactic):
        return "galactic"
    if isinstance(f, FK5) and f.equinox == Time("J2000"):
        return "fk5"
    if isinstance(f, FK4) and f.equinox == Time("B1950"):
        return "fk4"
    if isinstance(f, BarycentricMeanEcliptic) and f.equinox == Time("J2000"):
        return "ecliptic"
    return "fk5"


def _transform(lon, lat, src, dst):
    """Transform degree arrays between astropy frames → (lon, lat) degree arrays."""
    import astropy.units as u  # noqa: PLC0415
    import numpy as np  # noqa: PLC0415
    from astropy.coordinates import SkyCoord, UnitSphericalRepresentation  # noqa: PLC0415

    c = SkyCoord(np.atleast_1d(lon) * u.deg, np.atleast_1d(lat) * u.deg, frame=src)
    s = c.transform_to(dst).represent_as(UnitSphericalRepresentation)
    return s.lon.wrap_at(360 * u.deg).deg, s.lat.deg


def _north_rotation(lon: float, lat: float, src, dst) -> float:
    """Position angle, in ``dst``, of ``src``'s north at (lon, lat) in degrees.

    An angle measured from ``src``'s axes becomes, in ``dst``, that angle plus
    this one: both frames are right-handed on the sky, so the rotation is a
    pure offset. Measured numerically from a 1" step north.
    """
    import astropy.units as u  # noqa: PLC0415
    from astropy.coordinates import SkyCoord  # noqa: PLC0415

    if src.is_equivalent_frame(dst):
        return 0.0
    lat = min(lat, 90.0 - 1e-6)
    c = SkyCoord(lon * u.deg, lat * u.deg, frame=src)
    n = c.directional_offset_by(0 * u.deg, 1 * u.arcsec)
    ct, nt = c.transform_to(dst), n.transform_to(dst)
    return float(ct.position_angle(nt).deg)


class _PixelFrame:
    """Pixel ↔ native-sky mapping of a 2-D celestial WCS, and of angles
    between the sky and the pixel grid."""

    def __init__(self, wcs, origin: int):
        import numpy as np  # noqa: PLC0415
        from astropy.wcs.utils import proj_plane_pixel_scales, wcs_to_celestial_frame  # noqa: PLC0415

        self.wcs = wcs
        self.origin = origin
        self.frame = wcs_to_celestial_frame(wcs)
        self.lng, self.lat = wcs.wcs.lng, wcs.wcs.lat
        scales = proj_plane_pixel_scales(wcs)
        self.scale_deg = float(np.mean(scales))
        self.scale_arcsec = self.scale_deg * 3600.0

    def to_pix(self, lon, lat):
        import numpy as np  # noqa: PLC0415
        world = [None, None]
        world[self.lng], world[self.lat] = np.atleast_1d(lon), np.atleast_1d(lat)
        from astropy.wcs import NoConvergence  # noqa: PLC0415
        try:
            x, y = self.wcs.all_world2pix(world[0], world[1], self.origin)
        except NoConvergence as exc:   # distortion (SIP) inverse fails far off the image
            raise RegionParseError("The region lies outside the dataset's sky projection.") from exc
        return np.asarray(x, float), np.asarray(y, float)

    def to_sky(self, x, y):
        import numpy as np  # noqa: PLC0415
        w = self.wcs.all_pix2world(np.atleast_1d(x), np.atleast_1d(y), self.origin)
        return np.asarray(w[self.lng], float) % 360.0, np.asarray(w[self.lat], float)

    def sky_angle_to_pix(self, lon: float, lat: float, angle: float) -> float:
        """A ds9 sky angle at (lon, lat) in the native frame → pixel angle.

        The axis is followed one pixel scale along the sky (position angle
        ``angle - 90``) and the step is read on the grid, so rotation, mirror
        parity, unequal pixel scales and skew all come out of the WCS itself.
        """
        import astropy.units as u  # noqa: PLC0415
        from astropy.coordinates import SkyCoord, UnitSphericalRepresentation  # noqa: PLC0415

        c = SkyCoord(lon * u.deg, lat * u.deg, frame=self.frame)
        p = c.directional_offset_by((angle - 90.0) * u.deg, self.scale_deg * u.deg)
        s0, s1 = (q.represent_as(UnitSphericalRepresentation) for q in (c, p))
        xs, ys = self.to_pix([s0.lon.deg, s1.lon.deg], [s0.lat.deg, s1.lat.deg])
        _finite_or_raise(xs, ys)
        return math.degrees(math.atan2(ys[1] - ys[0], xs[1] - xs[0]))

    def pix_angle_to_sky(self, x: float, y: float, angle: float) -> float:
        """A pixel angle at (x, y) → ds9 sky angle in the native frame (the
        inverse of ``sky_angle_to_pix``: a one-pixel step read on the sky)."""
        import astropy.units as u  # noqa: PLC0415
        from astropy.coordinates import SkyCoord  # noqa: PLC0415

        t = math.radians(angle)
        lon, lat = self.to_sky([x, x + math.cos(t)], [y, y + math.sin(t)])
        _finite_or_raise(lon, lat)
        c = SkyCoord(lon * u.deg, lat * u.deg, frame=self.frame)
        return float(c[0].position_angle(c[1]).deg) + 90.0


def _size_keys(region: dict[str, Any]) -> list[str]:
    return [k for k in ("radius", "radii", "semi_axes", "size") if k in region]


def _rescale_sizes(region: dict[str, Any], src_unit: str, dst_unit: str, pf: _PixelFrame | None) -> None:
    """Rewrite a region's sizes from ``src_unit`` to ``dst_unit`` in place
    (pixels ↔ angle through the WCS's mean pixel scale)."""
    if src_unit == dst_unit or not _size_keys(region):
        region["unit"] = dst_unit
        return
    to_deg = (lambda v: v * pf.scale_deg) if src_unit == "pix" else (lambda v: v * _ANGLE_TO_DEG[src_unit])
    from_deg = (lambda v: v / pf.scale_deg) if dst_unit == "pix" else (lambda v: v / _ANGLE_TO_DEG[dst_unit])
    if (src_unit == "pix" or dst_unit == "pix") and pf is None:
        raise RegionParseError("Converting between pixel and angular sizes needs the dataset's WCS (dataset_id).")
    for k in _size_keys(region):
        v = region[k]
        region[k] = [from_deg(to_deg(x)) for x in v] if isinstance(v, list) else from_deg(to_deg(v))
    region["unit"] = dst_unit


def _finite_or_raise(*arrays) -> None:
    import numpy as np  # noqa: PLC0415
    for a in arrays:
        if not np.all(np.isfinite(a)):
            raise RegionParseError("The region lies outside the dataset's sky projection.")


def convert_regions(
    regions: list[dict[str, Any]],
    *,
    to_frame: str,
    wcs=None,
    origin: int = 1,
) -> list[dict[str, Any]]:
    """Convert regions to ``to_frame``: ``image`` (pixels) or a sky frame.

    ``wcs`` is the dataset's 2-D celestial WCS (``WCS.celestial``), needed
    whenever pixels are involved; ``origin`` is the pixel base (1 for region
    files, 0 for numpy-indexed analysis). Sky ↔ sky needs no WCS.

    Positions go through astropy: from the region's frame to the WCS's own
    frame, then to pixels (and back). Sizes are angles on the sky and do not
    change between sky frames; pixels and angles are related by the WCS's
    mean pixel scale. A position angle is measured from the region frame's
    axes, so it is rotated by the angle between the two frames' north
    directions at the region's centre; to or from pixels, the axis is
    followed one pixel scale along the sky and read on the grid, which
    carries the grid's rotation, parity, unequal scales and skew. In the image
    frame sizes always end in pixels. A region whose centre or vertices do
    not project onto the WCS raises ``RegionParseError``.
    """
    want_image = to_frame == "image"
    dst = None if want_image else sky_frame(to_frame)
    pf = _PixelFrame(wcs, origin) if wcs is not None else None
    out: list[dict[str, Any]] = []
    for r in regions:
        src_name = r.get("frame", "image")
        src_unit = r.get("unit", "pix" if src_name == "image" else "deg")
        if src_name == to_frame and (not want_image or src_unit == "pix"):
            out.append(dict(r))
            continue
        if src_name != "image" and src_name not in _WORLD_FRAMES:
            raise RegionParseError(f"Unknown region frame '{src_name}'.")
        if (want_image or src_name == "image") and pf is None:
            raise RegionParseError("Converting between pixels and sky needs the dataset's WCS (dataset_id).")
        nr = dict(r)
        if src_name == "image" and want_image:   # angular sizes on a pixel region
            _rescale_sizes(nr, src_unit, "pix", pf)
            out.append(nr)
            continue
        shape = r["shape"]
        pts = r["points"] if shape == "polygon" else [r["center"]]
        lon = [p[0] for p in pts]
        lat = [p[1] for p in pts]
        # An ellipse or box without an angle is at 0, and 0 turns with the frame too.
        has_angle = shape in ("ellipse", "box")
        angle = float(r.get("angle_deg", 0.0) or 0.0)

        # 1. to the native sky frame of the WCS (or straight to dst for sky→sky)
        if src_name == "image":
            nlon, nlat = pf.to_sky(lon, lat)
            _finite_or_raise(nlon, nlat)
            if has_angle:
                angle = pf.pix_angle_to_sky(float(lon[0]), float(lat[0]), angle)
            cur = pf.frame
        else:
            nlon, nlat = lon, lat
            cur = sky_frame(src_name)

        if want_image:
            if not cur.is_equivalent_frame(pf.frame):
                if has_angle:
                    angle += _north_rotation(float(nlon[0]), float(nlat[0]), cur, pf.frame)
                nlon, nlat = _transform(nlon, nlat, cur, pf.frame)
            x, y = pf.to_pix(nlon, nlat)
            _finite_or_raise(x, y)
            if has_angle:
                angle = pf.sky_angle_to_pix(float(nlon[0]), float(nlat[0]), angle)
            new_pts = [[float(a), float(b)] for a, b in zip(x, y)]
            _rescale_sizes(nr, src_unit, "pix", pf)
            nr["frame"] = "image"
        else:
            if has_angle:
                angle += _north_rotation(float(nlon[0]), float(nlat[0]), cur, dst)
            tlon, tlat = _transform(nlon, nlat, cur, dst)
            _finite_or_raise(tlon, tlat)
            new_pts = [[float(a), float(b)] for a, b in zip(tlon, tlat)]
            if src_unit == "pix":   # pixel sizes on the sky: express them as angles
                _rescale_sizes(nr, "pix", "arcsec", pf)
            nr["frame"] = to_frame
        if shape == "polygon":
            nr["points"] = new_pts
        else:
            nr["center"] = new_pts[0]
        if has_angle:
            nr["angle_deg"] = _wrap_angle(angle)
        out.append(nr)
    return out


def _wrap_angle(a: float) -> float:
    """Angle into [0, 360), rounded at 1e-9° so a no-op rotation stays exact."""
    a = round(a % 360.0, 9)
    return 0.0 if a >= 360.0 else a
