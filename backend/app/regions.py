"""Region interchange — parse/serialize CASA CRTF and DS9 region formats.

This is interop glue, not a new analysis feature: radio astronomers carry
regions between CASA, CARTA, ds9 and pyregion. VisIVO can already *draw*
regions (box/circle/ellipse/polygon/point/annulus); this module lets it read
and write them in the two standard text formats so a region defined in CARTA
can be loaded here and vice-versa.

Canonical region model (plain dict, JSON-friendly):

    {
      "shape":  "circle" | "ellipse" | "box" | "polygon" | "point" | "annulus",
      "frame":  "image" | "icrs" | "fk5" | "galactic",
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

World centers are always decimal degrees; ``unit`` governs only the angular
*sizes* (radius, semi-axes, box w/h, annulus radii). For ``frame=="image"``
everything is in pixels and ``unit`` is ``"pix"``.
"""
from __future__ import annotations

import re
from typing import Any

# Frames we recognise; the left tokens are what the formats write.
_DS9_FRAMES = {
    "image": "image", "physical": "image",
    "fk5": "fk5", "icrs": "icrs", "j2000": "fk5", "galactic": "galactic",
}
_WORLD_FRAMES = {"icrs", "fk5", "galactic"}

_ANGLE_TO_DEG = {"deg": 1.0, "arcmin": 1.0 / 60.0, "arcsec": 1.0 / 3600.0}


class RegionParseError(ValueError):
    """Raised when region text cannot be parsed."""


# ── helpers ──────────────────────────────────────────────────────────────────


def _sexagesimal_to_deg(token: str, is_ra: bool) -> float | None:
    """Convert an RA/Dec sexagesimal token to degrees, or None if not sexagesimal.

    Accepts ``18h12m24s`` / ``18:12:24`` (RA, hours) and ``-23d11m00s`` /
    ``-23:11:00`` (Dec, degrees). Decimal-degree tokens return None so the
    caller treats them as already-decimal.
    """
    t = token.strip()
    if t.endswith("deg") or t.endswith("pix"):  # plain decimal value, not DMS
        return None
    if "h" in t and "m" in t:  # RA in h m s, e.g. 18h12m24s
        parts = re.split(r"[hms]", t)
        h, m, s = (float(parts[0]), float(parts[1]), float(parts[2] or 0))
        return (h + m / 60.0 + s / 3600.0) * 15.0
    if "d" in t and "m" in t:  # Dec in d m s, e.g. -23d11m00s
        sign = -1.0 if t.lstrip().startswith("-") else 1.0
        parts = re.split(r"[dms]", t.lstrip("+-"))
        d, m, s = (float(parts[0]), float(parts[1]), float(parts[2] or 0))
        return sign * (d + m / 60.0 + s / 3600.0)
    if ":" in t:  # colon sexagesimal — RA is hours, Dec is degrees
        sign = -1.0 if t.lstrip().startswith("-") else 1.0
        a, b, c = (t.lstrip("+-").split(":") + ["0", "0"])[:3]
        val = abs(float(a)) + float(b) / 60.0 + float(c) / 3600.0
        return sign * (val * 15.0 if is_ra else val)
    return None


def _parse_size(token: str) -> tuple[float, str]:
    """Parse an angular/pixel size token → (value, unit) with unit in
    {pix, deg, arcmin, arcsec}. ``30arcsec`` / ``2.5'`` / ``1.3"`` / ``100pix``."""
    t = token.strip()
    m = re.fullmatch(r"([+-]?[\d.eE]+)\s*(pix|deg|arcmin|arcsec|'|\")?", t)
    if not m:
        raise RegionParseError(f"Cannot parse size token '{token}'.")
    val = float(m.group(1))
    suffix = m.group(2) or ""
    unit = {"": "deg", "'": "arcmin", '"': "arcsec"}.get(suffix, suffix)
    return val, unit


def _fmt_num(v: float) -> str:
    """Compact number formatting (no trailing zeros, no sci-notation noise)."""
    if v == int(v):
        return str(int(v))
    return f"{v:.6g}"


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


def _ds9_center(xs: str, ys: str, frame: str) -> list[float]:
    if frame in _WORLD_FRAMES:
        ra = _sexagesimal_to_deg(xs, is_ra=True)
        dec = _sexagesimal_to_deg(ys, is_ra=False)
        ra = ra if ra is not None else float(xs)
        dec = dec if dec is not None else float(ys)
        return [ra, dec]
    return [float(xs), float(ys)]


def parse_ds9(text: str) -> list[dict[str, Any]]:
    """Parse a DS9 region file into canonical region dicts."""
    regions: list[dict[str, Any]] = []
    frame = "image"
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.lower().startswith("global"):
            continue
        low = line.lower().rstrip(";")
        if low in _DS9_FRAMES:
            frame = _DS9_FRAMES[low]
            continue
        m = _DS9_SHAPE_RE.match(line)
        if not m:
            continue
        shape = m.group(1).lower()
        args = [a.strip() for a in m.group(2).split(",") if a.strip()]
        props = _ds9_props(m.group(3))
        unit = "pix" if frame == "image" else "deg"
        try:
            if shape == "circle":
                c = _ds9_center(args[0], args[1], frame)
                r, unit = _parse_size(args[2]) if frame in _WORLD_FRAMES else (float(args[2]), "pix")
                regions.append({"shape": "circle", "frame": frame, "center": c,
                                "radius": r, "unit": unit, "props": props})
            elif shape == "ellipse":
                c = _ds9_center(args[0], args[1], frame)
                if frame in _WORLD_FRAMES:
                    maj, unit = _parse_size(args[2]); mn, _ = _parse_size(args[3])
                else:
                    maj, mn, unit = float(args[2]), float(args[3]), "pix"
                ang = float(args[4]) if len(args) > 4 else 0.0
                regions.append({"shape": "ellipse", "frame": frame, "center": c,
                                "semi_axes": [maj, mn], "angle_deg": ang, "unit": unit, "props": props})
            elif shape == "box":
                c = _ds9_center(args[0], args[1], frame)
                if frame in _WORLD_FRAMES:
                    w, unit = _parse_size(args[2]); h, _ = _parse_size(args[3])
                else:
                    w, h, unit = float(args[2]), float(args[3]), "pix"
                ang = float(args[4]) if len(args) > 4 else 0.0
                regions.append({"shape": "box", "frame": frame, "center": c,
                                "size": [w, h], "angle_deg": ang, "unit": unit, "props": props})
            elif shape == "annulus":
                c = _ds9_center(args[0], args[1], frame)
                if frame in _WORLD_FRAMES:
                    r1, unit = _parse_size(args[2]); r2, _ = _parse_size(args[3])
                else:
                    r1, r2, unit = float(args[2]), float(args[3]), "pix"
                regions.append({"shape": "annulus", "frame": frame, "center": c,
                                "radii": [r1, r2], "unit": unit, "props": props})
            elif shape == "polygon":
                vals = [float(a) if frame == "image" else
                        (_sexagesimal_to_deg(a, is_ra=(i % 2 == 0)) or float(a))
                        for i, a in enumerate(args)]
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


_DS9_UNIT_SUFFIX = {"pix": "", "deg": "d", "arcmin": "'", "arcsec": '"'}


def _ds9_size_str(val: float, unit: str) -> str:
    return f"{_fmt_num(val)}{_DS9_UNIT_SUFFIX.get(unit, '')}"


def export_ds9(regions: list[dict[str, Any]]) -> str:
    """Serialize canonical regions to a DS9 region file (grouped by frame)."""
    out = ["# Region file format: DS9 version 4.1"]
    # Emit per frame so the coordinate-system line is correct for each block.
    by_frame: dict[str, list[dict]] = {}
    for r in regions:
        by_frame.setdefault(r.get("frame", "image"), []).append(r)
    for frame, group in by_frame.items():
        out.append(frame)
        for r in group:
            shape = r["shape"]
            unit = r.get("unit", "pix" if frame == "image" else "deg")
            cx, cy = (r["center"] if shape != "polygon" else (0, 0))
            def S(v):  # noqa: E306,N802
                return _ds9_size_str(v, unit)
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

_CRTF_COORD_TO_FRAME = {"j2000": "fk5", "icrs": "icrs", "galactic": "galactic", "b1950": "fk5"}


def _crtf_pos(token: str, is_ra: bool) -> tuple[float, str]:
    """Parse a CRTF position token → (value, frame_kind) with kind image|world."""
    t = token.strip()
    if t.endswith("pix"):
        return float(t[:-3]), "image"
    if t.endswith("deg"):
        return float(t[:-3]), "world"
    deg = _sexagesimal_to_deg(t, is_ra)
    if deg is not None:
        return deg, "world"
    return float(t), "world"


def _crtf_modifiers(tail: str) -> tuple[str | None, dict[str, Any]]:
    """Parse trailing ``coord=J2000, color=red, label='x'`` → (frame, props)."""
    frame = None
    props: dict[str, Any] = {}
    for m in re.finditer(r"(\w+)\s*=\s*('[^']*'|\"[^\"]*\"|\S+?)(?:,|$)", tail):
        key, val = m.group(1).lower(), m.group(2).strip("'\"")
        if key == "coord":
            frame = _CRTF_COORD_TO_FRAME.get(val.lower())
        elif key == "color":
            props["color"] = val
        elif key in ("label", "text"):
            props["text"] = val
    return frame, props


def parse_crtf(text: str) -> list[dict[str, Any]]:
    """Parse a CASA CRTF region file into canonical region dicts."""
    regions: list[dict[str, Any]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
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
        # Extract bracketed groups: positions like [x, y] and size-pairs [a, b].
        groups = re.findall(r"\[([^\[\]]+)\]", geom)
        try:
            if shape in ("circle",):
                (cx, kx), (cy, _) = _pair(groups[0])
                frame = "image" if kx == "image" else (coord_frame or "fk5")
                r, unit = _parse_size(_after_pairs(geom, 1))
                if frame == "image":
                    unit = "pix"
                regions.append({"shape": "circle", "frame": frame, "center": [cx, cy],
                                "radius": r, "unit": unit, "props": props})
            elif shape == "ellipse":
                (cx, kx), (cy, _) = _pair(groups[0])
                frame = "image" if kx == "image" else (coord_frame or "fk5")
                maj, unit = _parse_size(groups[1].split(",")[0])
                mn, _ = _parse_size(groups[1].split(",")[1])
                ang_tok = _trailing_scalar(geom)
                ang = float(re.sub(r"deg", "", ang_tok)) if ang_tok else 0.0
                if frame == "image":
                    unit = "pix"
                regions.append({"shape": "ellipse", "frame": frame, "center": [cx, cy],
                                "semi_axes": [maj, mn], "angle_deg": ang, "unit": unit, "props": props})
            elif shape in ("box", "centerbox", "rotbox"):
                (ax, kx), (ay, _) = _pair(groups[0])
                frame = "image" if kx == "image" else (coord_frame or "fk5")
                if shape == "box":  # two corners [blc],[trc] → center+size
                    (bx, _), (by, _) = _pair(groups[1])
                    center = [(ax + bx) / 2.0, (ay + by) / 2.0]
                    size = [abs(bx - ax), abs(by - ay)]
                    unit, ang = ("pix" if frame == "image" else "deg"), 0.0
                else:  # centerbox / rotbox → center + [w, h] (+ angle)
                    center = [ax, ay]
                    w, unit = _parse_size(groups[1].split(",")[0])
                    h, _ = _parse_size(groups[1].split(",")[1])
                    size = [w, h]
                    ang_tok = _trailing_scalar(geom)
                    ang = float(re.sub(r"deg", "", ang_tok)) if (shape == "rotbox" and ang_tok) else 0.0
                    if frame == "image":
                        unit = "pix"
                regions.append({"shape": "box", "frame": frame, "center": center,
                                "size": size, "angle_deg": ang, "unit": unit, "props": props})
            elif shape == "annulus":
                (cx, kx), (cy, _) = _pair(groups[0])
                frame = "image" if kx == "image" else (coord_frame or "fk5")
                r1, unit = _parse_size(groups[1].split(",")[0])
                r2, _ = _parse_size(groups[1].split(",")[1])
                if frame == "image":
                    unit = "pix"
                regions.append({"shape": "annulus", "frame": frame, "center": [cx, cy],
                                "radii": [r1, r2], "unit": unit, "props": props})
            elif shape in ("poly", "polygon"):
                pts, frame = [], None
                for i, g in enumerate(groups):
                    (px, kx), (py, _) = _pair(g)
                    frame = "image" if kx == "image" else (coord_frame or "fk5")
                    pts.append([px, py])
                regions.append({"shape": "polygon", "frame": frame or "image", "points": pts,
                                "unit": "pix" if frame == "image" else "deg", "props": props})
            elif shape == "symbol":
                (cx, kx), (cy, _) = _pair(groups[0])
                frame = "image" if kx == "image" else (coord_frame or "fk5")
                regions.append({"shape": "point", "frame": frame, "center": [cx, cy],
                                "unit": "pix" if frame == "image" else "deg", "props": props})
        except (IndexError, ValueError) as exc:
            raise RegionParseError(f"Malformed CRTF {shape}: {line} ({exc})") from exc
    return regions


def _pair(group: str) -> list[tuple[float, str]]:
    """Parse a CRTF 'x, y' position group → [(x, kind), (y, kind)]."""
    a, b = [t.strip() for t in group.split(",")][:2]
    return [_crtf_pos(a, is_ra=True), _crtf_pos(b, is_ra=False)]


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
    return f"{_fmt_num(v)}pix" if unit == "pix" else f"{_fmt_num(v)}{unit}"


def export_crtf(regions: list[dict[str, Any]]) -> str:
    """Serialize canonical regions to CASA CRTF text."""
    out = ["#CRTF"]
    for r in regions:
        shape, frame = r["shape"], r.get("frame", "image")
        unit = r.get("unit", "pix" if frame == "image" else "deg")
        coord = "" if frame == "image" else f" coord={'J2000' if frame == 'fk5' else frame.upper()}"
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
    if re.search(r"\b(fk5|icrs|image|physical|galactic)\b", head):
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


# ── optional pixel ↔ world conversion ────────────────────────────────────────


def convert_regions(
    regions: list[dict[str, Any]],
    *,
    to_frame: str,
    sky_to_pix,
    pix_to_sky,
    pixel_scale_arcsec: float,
    world_frame: str = "fk5",
) -> list[dict[str, Any]]:
    """Convert regions between the ``image`` (pixel) and a world frame.

    ``sky_to_pix(ra_deg, dec_deg) -> (x, y)`` and ``pix_to_sky(x, y) ->
    (ra_deg, dec_deg)`` are supplied by the caller (from the dataset WCS), which
    keeps this function pure and unit-testable. Angular sizes use
    ``pixel_scale_arcsec``. ``to_frame`` is ``"image"`` or a world frame
    (``icrs``/``fk5``/``galactic`` → stored as ``world_frame``).

    Regions already in the requested kind (pixel vs world) are passed through.
    """
    want_image = to_frame == "image"
    out: list[dict[str, Any]] = []
    for r in regions:
        is_image = r.get("frame", "image") == "image"
        if is_image == want_image:
            out.append(dict(r))
            continue
        nr = dict(r)
        if want_image:  # world → pixel
            nr["frame"] = "image"
            nr["unit"] = "pix"
            if r["shape"] == "polygon":
                nr["points"] = [list(sky_to_pix(x, y)) for x, y in r["points"]]
            else:
                nr["center"] = list(sky_to_pix(*r["center"]))
            _scale_sizes(nr, factor=1.0 / pixel_scale_arcsec, src_unit=r.get("unit", "arcsec"))
        else:  # image → world
            nr["frame"] = world_frame
            nr["unit"] = "arcsec"
            if r["shape"] == "polygon":
                nr["points"] = [list(pix_to_sky(x, y)) for x, y in r["points"]]
            else:
                nr["center"] = list(pix_to_sky(*r["center"]))
            _scale_sizes(nr, factor=pixel_scale_arcsec, src_unit="pix")
        out.append(nr)
    return out


def _scale_sizes(region: dict[str, Any], *, factor: float, src_unit: str) -> None:
    """Scale a region's angular sizes in place.

    ``factor`` already encodes the pix↔arcsec conversion; ``src_unit`` lets us
    first normalise a non-arcsec world unit (deg/arcmin) to arcsec.
    """
    pre = _ANGLE_TO_DEG[src_unit] * 3600.0 if src_unit in _ANGLE_TO_DEG else 1.0
    k = pre * factor
    if "radius" in region:
        region["radius"] *= k
    if "radii" in region:
        region["radii"] = [v * k for v in region["radii"]]
    if "semi_axes" in region:
        region["semi_axes"] = [v * k for v in region["semi_axes"]]
    if "size" in region:
        region["size"] = [v * k for v in region["size"]]
