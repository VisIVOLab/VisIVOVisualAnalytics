"""Region interchange — parse/serialize CASA CRTF and DS9 region formats.

This is interop glue, not a new analysis feature: radio astronomers carry
regions between CASA, CARTA, ds9 and pyregion. VisIVO can already *draw*
regions (box/circle/ellipse/polygon/point/annulus); this module lets it read
and write them in the two standard text formats so a region defined in CARTA
can be loaded here and vice-versa.

Canonical region model (plain dict, JSON-friendly):

    {
      "shape":  "circle" | "ellipse" | "box" | "polygon" | "point" | "annulus"
              | "ellipse_annulus" | "box_annulus" | "line" | "vector" | "text",
      "frame":  "image" | "icrs" | "fk5" | "fk4" | "galactic" | "ecliptic",
      "center": [x, y],          # pixels if frame=="image" else degrees
      "radius": r,               # circle
      "radii":  [inner, outer],  # annulus
      "semi_axes": [a, b],       # ellipse: along angle_deg, and across it
      "size":   [width, height], # box
      "inner":  [a, b] / [w, h], # ellipse_annulus semi-axes / box_annulus size
      "outer":  [a, b] / [w, h],
      "length": l,               # vector, from center along angle_deg
      "angle_deg": a,            # ellipse / box / their annuli / vector
      "points": [[x, y], ...],   # polygon, line (2 or more vertices)
      "text":   "...",           # text
      "unit":   "pix" | "deg" | "arcsec" | "arcmin",  # unit of sizes/radii
      "props":  {"color": ..., "text": ...},          # optional cosmetics
      "source_line": n,          # 1-based line of the file it was read from
    }

World centers are always decimal degrees in the region's own frame (``fk5``
is J2000, ``fk4`` B1950, ``ecliptic`` the mean ecliptic of J2000); ``unit``
governs only the *sizes* (radius, semi-axes, box w/h, annulus radii, vector
length), and all sizes of one region are held in that one unit. A region's
sizes may be pixels in a sky frame or angles in the image frame, as ds9 and
CRTF allow; the conversion to the image frame turns them into pixels. Image
pixels are 1-based, as in ds9 (CRTF's 0-based pixels are shifted on the way in
and out).

``angle_deg`` follows ds9: counter-clockwise from the first axis, i.e. from
pixel +x in the image frame and from the longitude axis of the region's own
sky frame. Changing the sky frame rotates it by the angle between the two
frames' north directions at the region's centre (see ``convert_regions``).

CRTF angles are CASA's (casa6 ``imageanalysis/Annotations``, checked against
casatools 6.7 masks, issue #151): an ellipse ``[[x, y], [b1, b2], pa]`` has
``b1`` at position angle ``pa`` from north through east (CASA lays it at
``pa + 90`` from pixel +x, i.e. from +y towards -x), so it reads as ds9's
``semi_axes [b2, b1]`` at ``angle_deg = pa``. A ``rotbox [[x, y], [w, h], pa]``
turns its ``h`` side by ``pa`` from north through east, which leaves ``w`` at
ds9's angle ``pa``: it is read unchanged. A sky region without ``coord=``
is in the image's own frame (CASA's default), so ``parse_report`` gives it
the dataset's frame when it has a WCS, and J2000 otherwise.

Reading never drops a region silently: every statement that cannot be turned
into a region (an unsupported shape or coordinate system, a malformed number,
a sky region on a dataset without a celestial WCS) is reported as a
diagnostic ``{line, text, reason}`` when the caller passes a list to collect
them, and raises ``RegionParseError`` otherwise.
"""
from __future__ import annotations

import math
import re
from typing import Any

# DS9 coordinate-system lines → canonical frame. ``physical`` is resolved to
# ``image`` at parse time through the dataset's LTM/LTV keywords.
_DS9_FRAMES = {
    "image": "image", "physical": "physical",
    "fk5": "fk5", "j2000": "fk5", "fk4": "fk4", "b1950": "fk4",
    "icrs": "icrs", "galactic": "galactic", "ecliptic": "ecliptic",
}
# DS9 systems that need information a region file does not carry (the image's
# default WCS, detector geometry): refused rather than read in the wrong frame.
_DS9_UNSUPPORTED = {"wcs", "linear", "amplifier", "detector"} | {f"wcs{c}" for c in "abcdefghijklmnopqrstuvwxyz"}
_WORLD_FRAMES = {"icrs", "fk5", "fk4", "galactic", "ecliptic"}
_EQUATORIAL = {"icrs", "fk5", "fk4"}          # longitude is RA (hours when sexagesimal)

_ANGLE_TO_DEG = {"deg": 1.0, "arcmin": 1.0 / 60.0, "arcsec": 1.0 / 3600.0}

#: Shapes whose geometry is a list of vertices; every other shape has a centre.
_POINT_LIST_SHAPES = {"polygon", "line"}
#: Shapes carrying an ``angle_deg``.
_ANGLED_SHAPES = {"ellipse", "box", "ellipse_annulus", "box_annulus", "vector"}
#: Size keys measured the same way in every direction, and pairs measured along
#: the region's angle (first value) and across it (second value).
_ISO_SIZE_KEYS = ("radius", "radii")
_AXIS_SIZE_KEYS = ("semi_axes", "size", "inner", "outer")

#: Most rings one annulus statement may expand to (``n=``, or radii given).
_MAX_ANNULI = 1000
#: Longest statement text echoed in a diagnostic.
_DIAG_TEXT_MAX = 200


class RegionParseError(ValueError):
    """Raised when region text cannot be parsed (or a region cannot be converted)."""


class _Skip(Exception):
    """One statement that is not turned into a region, with the reason."""


def _report(diagnostics: list | None, line: int | None, text: str, reason: str) -> None:
    """Record a statement that was not converted, or raise when the caller
    collects no diagnostics (the strict mode of the export and stats paths)."""
    if diagnostics is None:
        where = f"line {line}: " if line else ""
        raise RegionParseError(f"{where}{reason}" + (f" ({text.strip()})" if text.strip() else ""))
    diagnostics.append({"line": line, "text": text.strip()[:_DIAG_TEXT_MAX], "reason": reason})


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


def _finite(v: float, what: str) -> float:
    if not math.isfinite(v):
        raise RegionParseError(f"{what} is not a finite number")
    return v


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
    if val < 0:
        raise RegionParseError(f"Negative size '{token}'.")
    suffix = m.group(2)
    if not suffix:
        return val, default_unit
    unit, factor = _SIZE_UNITS[suffix]
    return val * factor, unit


def _common_unit(parsed: list[tuple[float, str]], tokens: list[str]) -> tuple[list[float], str]:
    """Values sharing one unit: angular ones in the unit of the first, pixels
    alone (only a WCS relates pixels and angles, so a mix is refused)."""
    units = {u for _, u in parsed}
    if "pix" in units and len(units) > 1:
        raise RegionParseError(
            "A region mixing pixel and angular sizes is not supported: " + ", ".join(tokens))
    unit = parsed[0][1]
    if unit == "pix":
        return [v for v, _ in parsed], unit
    return [v * _ANGLE_TO_DEG[u] / _ANGLE_TO_DEG[unit] for v, u in parsed], unit


def _parse_sizes(tokens: list[str], default_unit: str) -> tuple[list[float], str]:
    """Parse the size tokens of one region into values sharing one unit.

    Angular tokens are converted to the unit of the first one, so
    ``1', 30"`` is ``[1, 0.5]`` arcmin. Pixel and angular sizes cannot share a
    unit without a WCS, so a region mixing them is refused.
    """
    return _common_unit([_parse_size(t, default_unit) for t in tokens], tokens)


def _parse_angle(token: str) -> float:
    """Parse a rotation angle (bare number = degrees; ``deg``/``d``/``rad``/``r``)."""
    m = re.fullmatch(rf"({_FLOAT})\s*(deg|rad|d|r)?", token.strip())
    if not m:
        raise RegionParseError(f"Cannot parse angle token '{token}'.")
    val = float(m.group(1))
    return math.degrees(val) if m.group(2) in ("rad", "r") else val


def _finite_num(token: str) -> int | float | None:
    """A property number (an int when whole), or None when it is not a finite
    number: a JSON answer cannot carry inf or NaN."""
    try:
        v = float(token)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(v):
        return None
    return int(v) if v.is_integer() and abs(v) < 2 ** 53 else v


def _fmt_num(v: float) -> str:
    """Compact number formatting: no trailing zeros, and enough digits (12
    significant) that a sky position keeps sub-milliarcsecond precision."""
    v = float(v)
    if v == int(v) and abs(v) < 1e15:
        return str(int(v))
    return f"{v:.12g}"


# ── physical (IRAF LTM/LTV) coordinates ──────────────────────────────────────


class PhysicalTransform:
    """ds9 ``physical`` coordinates of an image through IRAF's keywords:
    image = LTM · physical + LTV, both 1-based (so a 2× blocked image has
    LTM = 0.5, and a cut out of a larger one LTV = −offset). Missing keywords
    default to the identity."""

    def __init__(self, ltm=((1.0, 0.0), (0.0, 1.0)), ltv=(0.0, 0.0)):
        (a, b), (c, d) = ltm
        det = a * d - b * c
        if not math.isfinite(det) or abs(det) < 1e-300:
            raise RegionParseError("The dataset's LTM matrix is singular.")
        self.ltm = ((float(a), float(b)), (float(c), float(d)))
        self.ltv = (float(ltv[0]), float(ltv[1]))
        self.scale = math.sqrt(abs(det))     # image pixels per physical pixel (area scale)

    @classmethod
    def from_header(cls, header) -> "PhysicalTransform | None":
        """From a FITS header, or None when it has no LTM/LTV keyword (then
        physical and image coordinates are the same)."""
        keys = ("LTM1_1", "LTM1_2", "LTM2_1", "LTM2_2", "LTV1", "LTV2")
        if not any(k in header for k in keys):
            return None

        def val(k, default):
            try:
                return float(header.get(k, default))
            except (TypeError, ValueError):
                return default
        return cls(((val("LTM1_1", 1.0), val("LTM1_2", 0.0)), (val("LTM2_1", 0.0), val("LTM2_2", 1.0))),
                   (val("LTV1", 0.0), val("LTV2", 0.0)))

    def vec(self, dx: float, dy: float) -> tuple[float, float]:
        """A physical offset as an image offset."""
        (a, b), (c, d) = self.ltm
        return a * dx + b * dy, c * dx + d * dy

    def point(self, x: float, y: float) -> list[float]:
        ix, iy = self.vec(x, y)
        return [ix + self.ltv[0], iy + self.ltv[1]]


def _physical_to_image(region: dict[str, Any], phys: PhysicalTransform | None, phys_sizes: bool) -> None:
    """Move a region read in ds9's ``physical`` frame (or with physical-pixel
    sizes) to image pixels in place.

    Positions go through LTM/LTV. Sizes along an axis (ellipse, box, vector)
    are mapped as vectors along that axis, which also gives the new angle;
    radii use the area scale ``sqrt|det LTM|``. Physical-pixel sizes of a
    region in another frame (a ``p`` suffix) can only use the area scale.
    """
    in_physical = region["frame"] == "physical"
    if in_physical:
        region["frame"] = "image"
    if phys is None:
        return
    if in_physical:
        if region["shape"] in _POINT_LIST_SHAPES:
            region["points"] = [phys.point(*p) for p in region["points"]]
        else:
            region["center"] = phys.point(*region["center"])
    if not phys_sizes and not in_physical:
        return
    if in_physical and region["shape"] in _ANGLED_SHAPES:
        a = math.radians(region.get("angle_deg", 0.0))
        ux, uy = phys.vec(math.cos(a), math.sin(a))          # along the angle
        vx, vy = phys.vec(-math.sin(a), math.cos(a))         # across it
        region["angle_deg"] = math.degrees(math.atan2(uy, ux)) % 360.0
        f0, f1 = (math.hypot(ux, uy), math.hypot(vx, vy)) if phys_sizes else (1.0, 1.0)
    else:
        f0 = f1 = phys.scale if phys_sizes else 1.0
    fi = phys.scale if phys_sizes else 1.0
    _scale_sizes(region, f0, f1, fi)


def _scale_sizes(region: dict[str, Any], f0: float, f1: float, fiso: float) -> None:
    """Multiply a region's sizes in place: ``f0`` along its angle, ``f1``
    across it, ``fiso`` for radii (the same in every direction)."""
    for k in _ISO_SIZE_KEYS:
        if k in region:
            v = region[k]
            region[k] = [x * fiso for x in v] if isinstance(v, list) else v * fiso
    for k in _AXIS_SIZE_KEYS:
        if k in region:
            a, b = region[k]
            region[k] = [a * f0, b * f1]
    if "length" in region:
        region["length"] = region["length"] * f0


# ── DS9 ──────────────────────────────────────────────────────────────────────

#: Shapes ds9 writes behind a ``#`` so older readers skip them; a commented
#: line starting with one of these is a region, any other is a comment.
_DS9_COMMENTED_SHAPES = {"text", "vector", "ruler", "compass", "projection", "composite", "segment"}
_DS9_POINT_STYLES = {"circle", "box", "diamond", "cross", "x", "arrow", "boxcircle"}
_DS9_REFUSED_SHAPES = {
    "compass": "a compass is a display indicator, not a region",
    "panda": "pie-annulus (panda) regions are not supported",
    "epanda": "elliptical pie-annulus (epanda) regions are not supported",
    "bpanda": "box pie-annulus (bpanda) regions are not supported",
}
_DS9_QUOTE_CLOSE = {"{": "}", '"': '"', "'": "'"}


def _opens_string(text: str, i: int) -> bool:
    """Whether the quote at ``text[i]`` opens a string. A brace always does; a
    ``"`` or ``'`` right after a digit or letter is a unit (arcsec, arcmin)."""
    if text[i] == "{":
        return True
    j = i - 1
    return j < 0 or not (text[j].isalnum() or text[j] == ".")


def _ds9_scan(line: str) -> tuple[list[str], str]:
    """Split one ds9 line at ``;`` and at the comment ``#``, outside strings and
    parentheses → (statements, comment). ``#`` right after ``=`` is a hex colour."""
    stmts: list[str] = []
    buf: list[str] = []
    close, depth = None, 0
    for i, ch in enumerate(line):
        if close:
            buf.append(ch)
            if ch == close:
                close = None
        elif ch in _DS9_QUOTE_CLOSE and _opens_string(line, i):
            close = _DS9_QUOTE_CLOSE[ch]
            buf.append(ch)
        elif ch == "(":
            depth += 1
            buf.append(ch)
        elif ch == ")":
            depth = max(0, depth - 1)
            buf.append(ch)
        elif ch == ";" and depth == 0:
            stmts.append("".join(buf))
            buf = []
        elif ch == "#" and depth == 0 and not (buf and "".join(buf).rstrip().endswith("=")):
            stmts.append("".join(buf))
            return [s.strip() for s in stmts if s.strip()], line[i + 1:].strip()
        else:
            buf.append(ch)
    stmts.append("".join(buf))
    return [s.strip() for s in stmts if s.strip()], ""


def _ds9_tokens(text: str) -> list[str]:
    """Split at commas and blanks outside strings (``{...}``, quoted text)."""
    out: list[str] = []
    buf: list[str] = []
    close = None
    for i, ch in enumerate(text):
        if close:
            buf.append(ch)
            if ch == close:
                close = None
        elif ch in _DS9_QUOTE_CLOSE and _opens_string(text, i) and not buf:
            close = _DS9_QUOTE_CLOSE[ch]
            buf.append(ch)
        elif ch in ", \t":
            if buf:
                out.append("".join(buf))
                buf = []
        else:
            buf.append(ch)
    if buf:
        out.append("".join(buf))
    return out


def _unquote(token: str) -> str:
    t = token.strip()
    if len(t) >= 2 and t[0] in _DS9_QUOTE_CLOSE and t[-1] == _DS9_QUOTE_CLOSE[t[0]]:
        return t[1:-1]
    return t


def _ds9_prop_tokens(text: str) -> list[str]:
    """Property tokens: ``key=value`` with a string value kept whole, extra
    space-separated values (``point=circle 11``) as their own tokens."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        if text[i] in " \t,|":
            i += 1
            continue
        j = i
        while j < n and text[j] not in " \t=":
            j += 1
        if j < n and text[j] == "=":
            j += 1
            if j < n and text[j] in _DS9_QUOTE_CLOSE:
                close = _DS9_QUOTE_CLOSE[text[j]]
                k = text.find(close, j + 1)
                k = n - 1 if k < 0 else k
                out.append(text[i:k + 1])
                i = k + 1
                continue
            while j < n and text[j] not in " \t":
                j += 1
            out.append(text[i:j])
        else:
            if text[i] in _DS9_QUOTE_CLOSE:
                k = text.find(_DS9_QUOTE_CLOSE[text[i]], i + 1)
                j = n if k < 0 else k + 1
            out.append(text[i:j])
        i = j
    return out


def _ds9_props(text: str) -> dict[str, Any]:
    """The ds9 properties kept in the canonical dict (cosmetics such as
    ``select`` or ``highlite`` are not): colour, line width and dash, font,
    text, tags, include/exclude, source/background and the point, line,
    vector and ruler styles."""
    props: dict[str, Any] = {}
    key, vals = None, []

    def flush():
        if key is None:
            return
        v = " ".join(vals).strip()
        if key in ("color", "font", "dashlist", "ruler"):
            props[key] = v
        elif key == "text":
            props["text"] = v
        elif key == "tag":
            props.setdefault("tag", []).append(v)
        elif key in ("width", "textangle"):
            num = _finite_num(v)
            if num is not None:
                props[key] = num
        elif key in ("dash", "fill", "textrotate"):
            props[key] = v.strip() not in ("0", "")
        elif key in ("include", "source"):
            props[key] = v.strip() not in ("0", "")
        elif key == "background":
            props["source"] = False
        elif key == "point":
            parts = v.split()
            if parts:
                props["point"] = parts[0].lower()
                num = _finite_num(parts[1]) if len(parts) > 1 else None
                if num is not None:
                    props["point_size"] = num
        elif key == "line":
            try:
                props["arrows"] = [int(x) for x in v.split()[:2]]
            except ValueError:
                pass
        elif key == "vector":
            props["arrow"] = v.strip() not in ("0", "")

    for tok in _ds9_prop_tokens(text):
        if tok in ("||", "|"):
            continue
        if "=" in tok:
            flush()
            k, v = tok.split("=", 1)
            key, vals = k.strip().lower(), [_unquote(v)]
        elif tok.lower() in ("background", "source") and (key is None or vals):
            flush()
            key, vals = None, []
            props["source"] = tok.lower() == "source"
        elif key is not None:
            vals.append(_unquote(tok))
    flush()
    return props


def _ds9_marks(props: dict[str, Any]) -> dict[str, Any]:
    """Keep only the exceptions: ``include: false`` and ``source: false``
    (included source regions are the default)."""
    for k in ("include", "source"):
        if props.get(k, False) is not False:
            del props[k]
    return props


def _ds9_coord(token: str, frame: str, is_lon: bool) -> float:
    """One ds9 position value: pixels in the image frame, degrees in a sky frame.

    Sky values may be sexagesimal (RA in hours only for equatorial frames) or
    decimal with a ``d``/``r`` suffix; a pixel-suffixed position in a sky
    frame has no meaning without a WCS and is refused.
    """
    t = token.strip()
    if frame not in _WORLD_FRAMES:
        m = re.fullmatch(rf"({_FLOAT})\s*[pi]?", t)
        if not m:
            raise RegionParseError(f"Cannot parse {frame} coordinate '{token}'.")
        return float(m.group(1))
    sx = _sexagesimal_to_deg(t, is_ra=is_lon and frame in _EQUATORIAL)
    if sx is not None:
        return _finite(sx, f"Coordinate '{token}'")
    m = re.fullmatch(rf"({_FLOAT})\s*(d|deg|r|rad)?", t)
    if not m:
        raise RegionParseError(f"Cannot parse {frame} coordinate '{token}'.")
    val = float(m.group(1))
    return math.degrees(val) if m.group(2) in ("r", "rad") else val


def _check_lat(lat: float, frame: str) -> float:
    if frame in _WORLD_FRAMES and abs(lat) > 90.0:
        raise RegionParseError(f"latitude {lat:g} is beyond ±90°")
    return lat


def _ds9_center(xs: str, ys: str, frame: str) -> list[float]:
    return [_ds9_coord(xs, frame, True), _check_lat(_ds9_coord(ys, frame, False), frame)]


def _ds9_points(tokens: list[str], frame: str, minimum: int, what: str) -> list[list[float]]:
    if len(tokens) % 2 or len(tokens) < 2 * minimum:
        raise RegionParseError(f"{what} needs at least {minimum} x,y pairs, got {len(tokens)} values")
    vals = [_ds9_coord(a, frame, i % 2 == 0) for i, a in enumerate(tokens)]
    return [[vals[i], _check_lat(vals[i + 1], frame)] for i in range(0, len(vals), 2)]


def _ds9_sizes(tokens: list[str], frame: str) -> tuple[list[float], str, bool]:
    """ds9 sizes of one region → (values, unit, physical-pixel sizes).

    A bare number is in the frame's own unit: image pixels in ``image``,
    physical pixels in ``physical``, degrees in a sky frame. ``i`` is an image
    pixel and ``p`` a physical pixel in any frame; a region mixing the two is
    refused, as one mixing pixels and angles is."""
    parsed, kinds = [], set()
    for t in tokens:
        t = t.strip()
        m = re.fullmatch(rf"({_FLOAT})\s*([ip]?)", t)
        if m:
            val = float(m.group(1))
            if val < 0:
                raise RegionParseError(f"Negative size '{t}'.")
            if m.group(2):
                kind = "phys" if m.group(2) == "p" else "img"
            elif frame in ("image", "physical"):
                kind = "phys" if frame == "physical" else "img"
            else:
                parsed.append((val, "deg"))
                continue
            kinds.add(kind)
            parsed.append((val, "pix"))
        else:
            parsed.append(_parse_size(t, "deg"))
    if len(kinds) > 1:
        raise RegionParseError("A region mixing image and physical pixel sizes is not supported: "
                               + ", ".join(tokens))
    values, unit = _common_unit(parsed, tokens)
    return values, unit, kinds == {"phys"}


def _split_n(tokens: list[str]) -> tuple[list[str], int | None]:
    """Take ds9's ``n=K`` (number of evenly spaced annuli) out of the arguments."""
    rest, n = [], None
    for t in tokens:
        if t.lower().startswith("n="):
            try:
                n = int(t[2:])
            except ValueError as exc:
                raise RegionParseError(f"Cannot parse '{t}'.") from exc
            if not 1 <= n <= _MAX_ANNULI:
                raise RegionParseError(f"'{t}': between 1 and {_MAX_ANNULI} annuli")
        else:
            rest.append(t)
    return rest, n


def _ds9_shape(shape: str, args: list[str], frame: str) -> tuple[list[dict[str, Any]], bool]:
    """The canonical regions of one ds9 shape statement, and whether its sizes
    are physical pixels. ``frame`` may still be ``physical`` here."""
    args, n = _split_n(args)
    base = {"frame": frame}
    pix_unit = "pix" if frame in ("image", "physical") else "deg"

    def need(k: int):
        if len(args) != k:
            raise RegionParseError(f"{shape} takes {k} values, got {len(args)}")

    if n is not None and shape not in ("annulus", "ellipse", "box"):
        raise RegionParseError(f"n= applies to annuli only, not to {shape}")

    if shape == "circle":
        need(3)
        (r,), unit, phys = _ds9_sizes(args[2:3], frame)
        return [{**base, "shape": "circle", "center": _ds9_center(args[0], args[1], frame),
                 "radius": r, "unit": unit}], phys

    if shape == "annulus":
        c = _ds9_center(*args[:2], frame) if len(args) >= 2 else None
        if n is not None:
            if len(args) != 4:
                raise RegionParseError("annulus with n= takes x, y, inner, outer")
            (r1, r2), unit, phys = _ds9_sizes(args[2:4], frame)
            radii = [r1 + (r2 - r1) * i / n for i in range(n + 1)]
        else:
            if len(args) < 4:
                raise RegionParseError(f"annulus needs a centre and at least two radii, got {len(args)} values")
            radii, unit, phys = _ds9_sizes(args[2:], frame)
            if len(radii) > _MAX_ANNULI + 1:
                raise RegionParseError(f"annulus: more than {_MAX_ANNULI} annuli")
        radii = sorted(radii)
        return [{**base, "shape": "annulus", "center": list(c), "radii": [radii[i], radii[i + 1]],
                 "unit": unit} for i in range(len(radii) - 1)], phys

    if shape in ("ellipse", "box"):
        if len(args) < 4:
            raise RegionParseError(f"{shape} needs a centre and two sizes, got {len(args)} values")
        c = _ds9_center(args[0], args[1], frame)
        rest = args[2:]
        angle = 0.0
        if n is not None:
            if len(rest) not in (4, 5):
                raise RegionParseError(f"{shape} with n= takes x, y, inner pair, outer pair[, angle]")
            if len(rest) == 5:
                angle = _parse_angle(rest[4])
            vals, unit, phys = _ds9_sizes(rest[:4], frame)
            (a1, b1), (a2, b2) = vals[:2], vals[2:]
            pairs = [[a1 + (a2 - a1) * i / n, b1 + (b2 - b1) * i / n] for i in range(n + 1)]
        else:
            if len(rest) % 2:                    # odd: the last one is the angle
                angle = _parse_angle(rest[-1])
                rest = rest[:-1]
            vals, unit, phys = _ds9_sizes(rest, frame)
            pairs = [vals[i:i + 2] for i in range(0, len(vals), 2)]
        if len(pairs) > _MAX_ANNULI + 1:
            raise RegionParseError(f"{shape}: more than {_MAX_ANNULI} annuli")
        pairs.sort(key=lambda p: (p[0] * p[1], p[0]))        # from the inside out
        key = "semi_axes" if shape == "ellipse" else "size"
        if len(pairs) == 1:
            return [{**base, "shape": shape, "center": c, key: pairs[0], "angle_deg": angle,
                     "unit": unit}], phys
        return [{**base, "shape": f"{shape}_annulus", "center": list(c), "inner": pairs[i],
                 "outer": pairs[i + 1], "angle_deg": angle, "unit": unit}
                for i in range(len(pairs) - 1)], phys

    if shape == "polygon":
        return [{**base, "shape": "polygon", "points": _ds9_points(args, frame, 3, "polygon"),
                 "unit": pix_unit}], False

    if shape == "point":
        need(2)
        return [{**base, "shape": "point", "center": _ds9_center(args[0], args[1], frame),
                 "unit": pix_unit}], False

    if shape in ("line", "ruler"):
        need(4)
        return [{**base, "shape": "line", "points": _ds9_points(args, frame, 2, shape),
                 "unit": pix_unit}], False

    if shape == "segment":
        return [{**base, "shape": "line", "points": _ds9_points(args, frame, 2, "segment"),
                 "unit": pix_unit}], False

    if shape == "projection":
        need(5)
        return [{**base, "shape": "line", "points": _ds9_points(args[:4], frame, 2, shape),
                 "unit": pix_unit}], False

    if shape == "vector":
        need(4)
        (length,), unit, phys = _ds9_sizes(args[2:3], frame)
        return [{**base, "shape": "vector", "center": _ds9_center(args[0], args[1], frame),
                 "length": length, "angle_deg": _parse_angle(args[3]), "unit": unit}], phys

    if shape == "text":
        if len(args) not in (2, 3):
            raise RegionParseError(f"text takes x, y and an optional string, got {len(args)} values")
        reg = {**base, "shape": "text", "center": _ds9_center(args[0], args[1], frame),
               "unit": pix_unit}
        if len(args) == 3:
            reg["text"] = _unquote(args[2])
        return [reg], False

    if shape in _DS9_REFUSED_SHAPES:
        raise _Skip(_DS9_REFUSED_SHAPES[shape])
    raise _Skip(f"'{shape}' is not a ds9 region shape")


_DS9_HEAD_RE = re.compile(
    r"^([+-]?)\s*(?:(circle|box|diamond|cross|x|arrow|boxcircle)\s+(point)\b|([a-zA-Z_]\w*))\s*",
    re.IGNORECASE)


def _ds9_split_statement(stmt: str) -> tuple[str, str, str | None, list[str], str] | None:
    """``[+-]shape(args) tail`` or ``[+-]shape args`` → (sign, shape, point
    style, argument tokens, tail text holding properties)."""
    m = _DS9_HEAD_RE.match(stmt)
    if not m:
        return None
    sign = m.group(1)
    if m.group(3):
        shape, style = "point", m.group(2).lower()
    else:
        shape, style = m.group(4).lower(), None
    rest = stmt[m.end():]
    if rest.startswith("("):
        close, depth, end = None, 0, -1
        for i, ch in enumerate(rest):
            if close:
                if ch == close:
                    close = None
            elif ch in _DS9_QUOTE_CLOSE and _opens_string(rest, i):
                close = _DS9_QUOTE_CLOSE[ch]
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        if end < 0:
            raise RegionParseError("unbalanced parentheses")
        return sign, shape, style, _ds9_tokens(rest[1:end]), rest[end + 1:]
    return sign, shape, style, _ds9_tokens(rest), ""


def parse_ds9(text: str, diagnostics: list | None = None,
              physical: PhysicalTransform | None = None) -> list[dict[str, Any]]:
    """Parse a DS9 region file into canonical region dicts.

    Every ds9 shape is read: circle, ellipse, box, polygon, point (any point
    style, also the ``circle point`` form), line, vector, text, ruler and
    segment (as lines), projection (as its centre line), and annuli with any
    number of radii (circular, elliptical and box; ``n=`` too), which become
    one canonical annulus per ring. Properties of the ``global`` lines and of
    each region (after ``#``) are kept in ``props``; ``-`` marks an excluded
    region. ``physical`` coordinates go through ``physical`` (the dataset's
    LTM/LTV), or equal image pixels without it. A statement that cannot be read
    is reported in ``diagnostics`` (or raises when it is None).
    """
    regions: list[dict[str, Any]] = []
    frame: str | None = "image"
    bad_frame = ""
    global_props: dict[str, Any] = {}
    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        commented = False
        if line.startswith("#"):
            body = line[1:].strip()
            m = re.match(r"[+-]?\s*([a-zA-Z]+)\s*\(", body)
            if not (m and m.group(1).lower() in _DS9_COMMENTED_SHAPES):
                continue                         # a comment
            line, commented = body, True
        stmts, comment = _ds9_scan(line)
        for k, stmt in enumerate(stmts):
            props_text = comment if k == len(stmts) - 1 else ""
            low = stmt.lower()
            if low.startswith("global") and (len(low) == 6 or low[6] in " \t"):
                global_props.update(_ds9_props(stmt[6:] + " " + props_text))
                continue
            if low in _DS9_FRAMES:
                frame, bad_frame = _DS9_FRAMES[low], ""
                continue
            if low in _DS9_UNSUPPORTED:
                frame, bad_frame = None, low
                continue
            try:
                split = _ds9_split_statement(stmt.rstrip("| "))
                if split is None:
                    raise _Skip("not a ds9 region or coordinate system")
                sign, shape, style, args, tail = split
                if shape == "composite":
                    raise _Skip("composite regions are not kept as a group; "
                                "the regions that follow it are read one by one")
                if shape in ("tile",):
                    continue
                if frame is None:
                    raise _Skip(f"coordinate system '{bad_frame}' is not supported; save the "
                                "regions in image, physical, fk5, fk4, icrs, galactic or ecliptic")
                props = {**global_props, **_ds9_props(tail + " " + props_text)}
                if "tag" in global_props and "tag" in props and props["tag"] is not global_props["tag"]:
                    props["tag"] = list(dict.fromkeys(global_props["tag"] + props["tag"]))
                if sign:
                    props["include"] = sign == "+"
                if style:
                    props["point"] = style
                if shape == "ruler":
                    props.setdefault("ruler", "")       # marks a ds9 ruler read as a line
                if shape == "projection":
                    props["projection_width"] = args[4] if len(args) > 4 else ""
                if shape == "vector":
                    props.setdefault("arrow", True)     # ds9 draws the arrow unless vector=0
                try:
                    built, phys_sizes = _ds9_shape(shape, args, frame)
                except (IndexError, ValueError) as exc:
                    raise _Skip(f"malformed {shape}: {exc}") from exc
                for reg in built:
                    _physical_to_image(reg, physical, phys_sizes)
                    if shape == "text" and "text" not in reg:
                        reg["text"] = props.get("text", "")
                    reg["props"] = _ds9_marks(dict(props))
                    reg["source_line"] = line_no
                    _check_finite(reg)
                    regions.append(reg)
            except _Skip as exc:
                _report(diagnostics, line_no, ("# " if commented else "") + stmt, str(exc))
            except (RegionParseError, ValueError) as exc:
                _report(diagnostics, line_no, ("# " if commented else "") + stmt, str(exc))
    return regions


def _check_finite(region: dict[str, Any]) -> None:
    """Refuse a region with a non-finite number (JSON cannot carry it)."""
    def walk(v):
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            if not math.isfinite(v):
                raise RegionParseError("a value is not a finite number")
        elif isinstance(v, list):
            for x in v:
                walk(x)
    for k in ("center", "points", "radius", "radii", "semi_axes", "size", "inner", "outer",
              "length", "angle_deg"):
        if k in region:
            walk(region[k])


# Pixel sizes in a sky frame need the explicit image-pixel suffix: a bare number
# there means degrees. Angular sizes always carry their suffix.
_DS9_UNIT_SUFFIX = {"deg": "d", "arcmin": "'", "arcsec": '"'}


def _ds9_size_str(val: float, unit: str, frame: str) -> str:
    if unit == "pix":
        return _fmt_num(val) + ("" if frame == "image" else "i")
    if unit not in _DS9_UNIT_SUFFIX:
        raise RegionParseError(f"Unknown size unit '{unit}'.")
    return f"{_fmt_num(val)}{_DS9_UNIT_SUFFIX[unit]}"


def _ds9_quote(s: str) -> str:
    return "{" + s + "}" if "}" not in s else '"' + s.replace('"', "'") + '"'


def _ds9_props_str(props: dict[str, Any], shape: str) -> str:
    out = []
    for k in ("color", "width", "font", "dashlist"):
        if k in props:
            v = props[k]
            out.append(f'{k}="{v}"' if k == "font" else f"{k}={v}")
    if props.get("dash"):
        out.append("dash=1")
    if props.get("text"):
        out.append(f"text={_ds9_quote(str(props['text']))}")
    if "textangle" in props:
        out.append(f"textangle={props['textangle']}")
    for t in props.get("tag") or []:
        out.append(f"tag={_ds9_quote(str(t))}")
    if shape == "point" and props.get("point"):
        out.append(f"point={props['point']}" + (f" {props['point_size']}" if "point_size" in props else ""))
    if shape == "line" and props.get("arrows"):
        out.append("line=" + " ".join(str(int(a)) for a in props["arrows"][:2]))
    if shape == "vector":
        out.append(f"vector={1 if props.get('arrow', True) else 0}")
    if props.get("source") is False:
        out.append("background")
    return " ".join(out)


def export_ds9(regions: list[dict[str, Any]]) -> str:
    """Serialize canonical regions to a DS9 region file (grouped by frame).

    Text and vector regions are written behind ``#``, as ds9 itself writes
    them; a line of more than two vertices is a ds9 ``segment``."""
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
            cx, cy = (r["center"] if shape not in _POINT_LIST_SHAPES else (0, 0))
            c = f"{_fmt_num(cx)},{_fmt_num(cy)}"

            def S(v):  # noqa: E306,N802
                return _ds9_size_str(v, unit, frame)
            ang = _fmt_num(_export_angle(r.get("angle_deg", 0.0)))
            prefix = ""
            if shape == "circle":
                line = f"circle({c},{S(r['radius'])})"
            elif shape == "ellipse":
                maj, mn = r["semi_axes"]
                line = f"ellipse({c},{S(maj)},{S(mn)},{ang})"
            elif shape == "box":
                w, h = r["size"]
                line = f"box({c},{S(w)},{S(h)},{ang})"
            elif shape == "annulus":
                line = f"annulus({c}," + ",".join(S(v) for v in r["radii"]) + ")"
            elif shape in ("ellipse_annulus", "box_annulus"):
                name = shape.split("_")[0]
                vals = ",".join(S(v) for v in list(r["inner"]) + list(r["outer"]))
                line = f"{name}({c},{vals},{ang})"
            elif shape == "polygon":
                line = "polygon(" + ",".join(_fmt_num(v) for pt in r["points"] for v in pt) + ")"
            elif shape == "line":
                name = "line" if len(r["points"]) == 2 else "segment"
                prefix = "" if name == "line" else "# "
                line = f"{name}(" + ",".join(_fmt_num(v) for pt in r["points"] for v in pt) + ")"
            elif shape == "point":
                line = f"point({c})"
            elif shape == "vector":
                prefix = "# "
                line = f"vector({c},{S(r['length'])},{ang})"
            elif shape == "text":
                prefix = "# "
                line = f"text({c})"
            else:
                raise RegionParseError(f"Unknown region shape '{shape}'.")
            props = dict(r.get("props") or {})
            if shape == "text":
                props["text"] = r.get("text", props.get("text", ""))
            if props.get("include") is False:
                line = "-" + line
            extras = _ds9_props_str(props, shape)
            if prefix:
                out.append(f"{prefix}{line} {extras}".rstrip())
            else:
                out.append(f"{line} # {extras}" if extras else line)
    return "\n".join(out) + "\n"


# ── CRTF (CASA) ──────────────────────────────────────────────────────────────

_CRTF_COORD_TO_FRAME = {
    "j2000": "fk5", "icrs": "icrs", "galactic": "galactic", "ecliptic": "ecliptic",
    "b1950": "fk4", "b1950_vla": "fk4",
}
_FRAME_TO_CRTF = {"fk5": "J2000", "fk4": "B1950", "icrs": "ICRS",
                  "galactic": "GALACTIC", "ecliptic": "ECLIPTIC"}
# CRTF symbols (matplotlib markers) that have a ds9 point style.
_CRTF_SYMBOL_TO_POINT = {"o": "circle", "s": "box", "D": "diamond", "d": "diamond",
                         "+": "cross", "x": "x", "^": "arrow"}
_POINT_TO_CRTF_SYMBOL = {"circle": "o", "box": "s", "diamond": "D", "cross": "+", "x": "x",
                         "arrow": "^", "boxcircle": "o"}
_CRTF_SHAPES = {"circle", "ellipse", "box", "centerbox", "rotbox", "annulus", "poly", "polygon",
                "symbol", "line", "vector", "text"}


def _crtf_pos(token: str, is_ra: bool) -> tuple[float, str]:
    """Parse a CRTF position token → (value, frame_kind) with kind image|world.

    CRTF pixels are 0-based (CASA's convention); they come back 1-based, as
    the canonical image frame is."""
    t = token.strip()
    if t.endswith("pix"):
        return _finite(float(t[:-3]), f"Coordinate '{t}'") + 1.0, "image"
    if t.endswith("deg"):
        return _finite(float(t[:-3]), f"Coordinate '{t}'"), "world"
    if t.endswith("rad"):
        return _finite(math.degrees(float(t[:-3])), f"Coordinate '{t}'"), "world"
    deg = _sexagesimal_to_deg(t, is_ra)
    if deg is not None:
        return deg, "world"
    return _finite(float(t), f"Coordinate '{t}'"), "world"


def _crtf_modifiers(tail: str) -> tuple[str | None, dict[str, Any]]:
    """Parse trailing ``coord=J2000, color=red, label='x'`` → (frame, props).

    A coordinate system the canonical model has no frame for (``SUPERGAL``,
    ``AZEL``, apparent or "of date" systems) is refused instead of being read
    as J2000.
    """
    frame = None
    props: dict[str, Any] = {}
    for m in re.finditer(r"(\w+)\s*=\s*('[^']*'|\"[^\"]*\"|\[[^\]]*\]|[^,]+?)\s*(?:,|$)", tail):
        key, val = m.group(1).lower(), m.group(2).strip().strip("'\"")
        if key == "coord":
            frame = _CRTF_COORD_TO_FRAME.get(val.lower())
            if frame is None:
                raise RegionParseError(f"CRTF coordinate system '{val}' is not supported "
                                       "(J2000, B1950, ICRS, GALACTIC, ECLIPTIC are)")
        elif key in ("color", "font", "fontstyle", "range", "corr", "labelpos", "labelcolor"):
            props[key] = val
        elif key in ("label", "text"):
            props["text"] = val
        elif key in ("linewidth", "fontsize", "symsize", "symthick"):
            num = _finite_num(val)
            if num is not None:
                props["width" if key == "linewidth" else key] = num
        elif key == "linestyle":
            props["dash"] = val not in ("-", "solid")
    return frame, props


def _crtf_split(line: str) -> tuple[str, str, str, str]:
    """``[-][ann ]shape[geom] tail`` → (sign, shape, geom, tail)."""
    m = re.match(r"([+-]?)\s*(ann\s+)?([a-zA-Z]+)\s*\[", line)
    if not m:
        raise _Skip("not a CRTF region")
    shape = m.group(3).lower()
    start = m.end() - 1
    depth, end, quote = 0, -1, None
    for i in range(start, len(line)):
        ch = line[i]
        if quote:
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
        elif ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end < 0:
        raise _Skip(f"malformed {shape}: unbalanced brackets")
    sign = "-" if m.group(1) == "-" else ""
    if m.group(2):
        sign += "ann"
    return sign, shape, line[start:end + 1], line[end + 1:].lstrip(", ")


def parse_crtf(text: str, diagnostics: list | None = None,
               default_frame: str = "fk5") -> list[dict[str, Any]]:
    """Parse a CASA CRTF region file into canonical region dicts.

    Shapes: circle, ellipse, box (two corners), centerbox, rotbox, annulus,
    poly, symbol (as a point), line, vector (a line with an arrow at its
    end) and text; ``ann`` annotations and ``-`` exclusions are read too.
    A sky region without ``coord=`` takes the file's ``global coord=``, else
    ``default_frame``: CASA uses the image's own frame there ("Default: image
    value"), which the caller passes when it knows the image; J2000 when it
    does not. A statement that cannot be read is reported in ``diagnostics``
    (or raises when it is None).
    """
    if default_frame not in _FRAME_TO_CRTF:
        raise RegionParseError(f"Unknown default CRTF frame '{default_frame}'.")
    regions: list[dict[str, Any]] = []
    global_frame: str | None = None
    global_bad = ""
    global_props: dict[str, Any] = {}
    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if re.match(r"global\b", line, re.IGNORECASE):
            try:
                gframe, gprops = _crtf_modifiers(line[6:].strip())
            except RegionParseError as exc:
                bad = re.search(r"coord\s*=\s*([^,\s]+)", line, re.IGNORECASE)
                global_bad = bad.group(1) if bad else "?"
                _report(diagnostics, line_no, line, str(exc))
                continue
            if gframe:
                global_frame, global_bad = gframe, ""
            global_props.update(gprops)
            continue
        try:
            sign, shape, geom, tail = _crtf_split(line)
            if shape not in _CRTF_SHAPES:
                raise _Skip(f"'{shape}' is not a CRTF region shape")
            coord_frame, props = _crtf_modifiers(tail)
            props = {**global_props, **props}
            if sign.startswith("-"):
                props["include"] = False
            if sign.endswith("ann"):
                props["annotation"] = True
            built = _crtf_shape(shape, geom, coord_frame or global_frame or default_frame)
            if (global_bad and not coord_frame
                    and any(r["frame"] != "image" for r in built)):
                raise _Skip(f"CRTF coordinate system '{global_bad}' is not supported "
                            "(J2000, B1950, ICRS, GALACTIC, ECLIPTIC are)")
            for reg in built:
                if reg["shape"] == "point":
                    sym = reg.pop("_symbol", "")
                    if sym:
                        props = {**props, "symbol": sym}
                        if sym in _CRTF_SYMBOL_TO_POINT:
                            props["point"] = _CRTF_SYMBOL_TO_POINT[sym]
                if shape == "vector":
                    props = {**props, "arrows": [0, 1]}
                reg["props"] = dict(props)
                reg["source_line"] = line_no
                _check_finite(reg)
                regions.append(reg)
        except _Skip as exc:
            _report(diagnostics, line_no, line, str(exc))
        except (RegionParseError, ValueError, IndexError) as exc:
            _report(diagnostics, line_no, line, f"malformed region: {exc}")
    return regions


def _crtf_shape(shape: str, geom: str, sky_frame_name: str) -> list[dict[str, Any]]:
    """The canonical region of one CRTF shape (its geometry ``[...]``)."""
    equatorial = sky_frame_name in _EQUATORIAL
    groups = re.findall(r"\[([^\[\]]+)\]", geom)

    def centre(g):
        (cx, kx), (cy, ky) = _pair(g, equatorial)
        if kx != ky:
            raise RegionParseError(f"position [{g}] mixes pixels and sky coordinates")
        frame = "image" if kx == "image" else sky_frame_name
        return [cx, _check_lat(cy, frame)], frame, ("pix" if frame == "image" else "deg")

    def sizes(g, default_unit):
        toks = [t.strip() for t in g.split(",")]
        if len(toks) != 2:
            raise RegionParseError(f"[{g}] should hold two sizes")
        return _parse_sizes(toks, default_unit)

    if shape == "circle":
        c, frame, dflt = centre(groups[0])
        (r,), unit = _parse_sizes([_after_pairs(geom, 1)], dflt)
        return [{"shape": "circle", "frame": frame, "center": c, "radius": r, "unit": unit}]
    if shape == "ellipse":
        c, frame, dflt = centre(groups[0])
        (b1, b2), unit = sizes(groups[1], dflt)
        ang_tok = _trailing_scalar(geom)
        pa = _parse_angle(ang_tok) if ang_tok else 0.0
        # CASA's b1 lies along the position angle pa, from north through east
        # (AnnEllipse lays it at pa + 90 from pixel +x), so across the ds9
        # axis at angle pa (whose position angle is pa - 90). b1 is along pa
        # even when it is the shorter one (CASA then swaps and adds 90).
        return [{"shape": "ellipse", "frame": frame, "center": c, "semi_axes": [b2, b1],
                 "angle_deg": pa, "unit": unit}]
    if shape in ("box", "centerbox", "rotbox"):
        (ax, ay), frame, dflt = centre(groups[0])
        if shape == "box":  # two corners [blc],[trc] → center+size
            (bx, by), frame_b, _ = centre(groups[1])
            if frame_b != frame:
                raise RegionParseError("the two corners are in different frames")
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
            # CASA turns the box's h side by pa from north through east
            # (AnnPolygon::_doCorners rotates the sky offsets by -pa), which
            # puts w at ds9's angle pa: the number is the same.
            ang = _parse_angle(ang_tok) if ang_tok else 0.0
        return [{"shape": "box", "frame": frame, "center": center, "size": size,
                 "angle_deg": ang, "unit": unit}]
    if shape == "annulus":
        c, frame, dflt = centre(groups[0])
        (r1, r2), unit = sizes(groups[1], dflt)
        return [{"shape": "annulus", "frame": frame, "center": c, "radii": sorted([r1, r2]),
                 "unit": unit}]
    if shape in ("poly", "polygon", "line", "vector"):
        pts, frames = [], set()
        for g in groups:
            p, fr, _ = centre(g)
            pts.append(p)
            frames.add(fr)
        if len(frames) != 1:
            raise RegionParseError("vertices in different frames")
        frame = frames.pop()
        if shape in ("poly", "polygon"):
            if len(pts) < 3:
                raise RegionParseError("a polygon needs at least three vertices")
            name = "polygon"
        else:
            if len(pts) != 2:
                raise RegionParseError(f"a {shape} takes two positions")
            name = "line"
        return [{"shape": name, "frame": frame, "points": pts,
                 "unit": "pix" if frame == "image" else "deg"}]
    if shape == "symbol":
        c, frame, dflt = centre(groups[0])
        sym = _trailing_scalar(geom).strip("'\"")
        return [{"shape": "point", "frame": frame, "center": c, "unit": dflt, "_symbol": sym}]
    if shape == "text":
        c, frame, dflt = centre(groups[0])
        m = re.search(r"\]\s*,\s*(['\"])(.*)\1\s*\]$", geom)
        if not m:
            raise RegionParseError("text needs a quoted string")
        return [{"shape": "text", "frame": frame, "center": c, "text": m.group(2), "unit": dflt}]
    raise _Skip(f"'{shape}' is not a CRTF region shape")


def _pair(group: str, equatorial: bool = True) -> list[tuple[float, str]]:
    """Parse a CRTF 'x, y' position group → [(x, kind), (y, kind)]."""
    toks = [t.strip() for t in group.split(",")]
    if len(toks) != 2:
        raise RegionParseError(f"[{group}] is not an x, y position")
    a, b = toks
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
    return f"{_fmt_num(v - 1.0)}pix" if frame == "image" else f"{_fmt_num(v)}deg"


def _crtf_size_str(v: float, unit: str) -> str:
    if unit not in ("pix", "deg", "arcmin", "arcsec"):
        raise RegionParseError(f"Unknown size unit '{unit}'.")
    return f"{_fmt_num(v)}{unit}"


def _vector_end(r: dict[str, Any], frame: str, unit: str) -> list[float]:
    """The end point of a vector region in its own frame."""
    cx, cy = r["center"]
    a = float(r.get("angle_deg", 0.0))
    length = float(r["length"])
    if frame == "image":
        if unit != "pix":
            raise RegionParseError("A pixel vector with an angular length cannot be written as CRTF.")
        return [cx + length * math.cos(math.radians(a)), cy + length * math.sin(math.radians(a))]
    if unit == "pix":
        raise RegionParseError("A sky vector with a pixel length cannot be written as CRTF.")
    import astropy.units as u  # noqa: PLC0415
    from astropy.coordinates import SkyCoord  # noqa: PLC0415
    c = SkyCoord(cx * u.deg, cy * u.deg, frame=sky_frame(frame))
    e = c.directional_offset_by((a - 90.0) * u.deg, length * _ANGLE_TO_DEG[unit] * u.deg)
    return [float(e.spherical.lon.deg) % 360.0, float(e.spherical.lat.deg)]


def export_crtf(regions: list[dict[str, Any]]) -> str:
    """Serialize canonical regions to CASA CRTF text.

    A vector becomes a CRTF ``vector`` between its two ends. Elliptical and
    box annuli have no CRTF form and are refused. CRTF has no polyline either:
    a line of more than two vertices (a PV path) is written as one ``line``
    per segment, in order, each with the properties of the whole."""
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
        def C(p):  # noqa: E306,N802
            return f"[{P(p[0], True)}, {P(p[1], False)}]"
        props = dict(r.get("props") or {})
        bodies = None
        if shape == "circle":
            body = f"circle[{C(r['center'])}, {Z(r['radius'])}]"
        elif shape == "ellipse":
            a, b = r["semi_axes"]           # CASA: [b1, b2] with b1 along the angle from north
            body = f"ellipse[{C(r['center'])}, [{Z(b)}, {Z(a)}], {_fmt_num(_export_angle(r.get('angle_deg', 0.0)))}deg]"
        elif shape == "box":
            w, h = r["size"]; ang = _export_angle(r.get("angle_deg", 0.0))
            if ang:
                body = f"rotbox[{C(r['center'])}, [{Z(w)}, {Z(h)}], {_fmt_num(ang)}deg]"
            else:
                body = f"centerbox[{C(r['center'])}, [{Z(w)}, {Z(h)}]]"
        elif shape == "annulus":
            r1, r2 = r["radii"]
            body = f"annulus[{C(r['center'])}, [{Z(r1)}, {Z(r2)}]]"
        elif shape == "polygon":
            body = "poly[" + ", ".join(C(p) for p in r["points"]) + "]"
        elif shape == "line":
            pts = r["points"]
            bodies = ["line[" + ", ".join(C(p) for p in pair) + "]"
                      for pair in (zip(pts[:-1], pts[1:]) if len(pts) > 2 else [pts])]
        elif shape == "vector":
            body = f"vector[{C(r['center'])}, {C(_vector_end(r, frame, unit))}]"
        elif shape == "point":
            sym = props.get("symbol") or _POINT_TO_CRTF_SYMBOL.get(props.get("point", ""), ".")
            body = f"symbol[{C(r['center'])}, {sym}]"
        elif shape == "text":
            txt = str(r.get("text", props.get("text", ""))).replace("'", "’")
            body = f"text[{C(r['center'])}, '{txt}']"
            props.pop("text", None)
        else:
            raise RegionParseError(f"CRTF has no '{shape}' region.")
        if bodies is None:
            bodies = [body]
        extra = ""
        if "color" in props:
            extra += f", color={props['color']}"
        if "width" in props:
            extra += f", linewidth={props['width']}"
        if props.get("dash"):
            extra += ", linestyle=--"
        if props.get("text"):
            extra += ", label='" + str(props["text"]).replace("'", "’") + "'"
        for body in bodies:
            if props.get("annotation"):
                body = "ann " + body
            if props.get("include") is False:
                body = "-" + body
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


def parse_regions(text: str, fmt: str = "auto", *, diagnostics: list | None = None,
                  physical: PhysicalTransform | None = None,
                  crtf_default_frame: str = "fk5") -> list[dict[str, Any]]:
    """Parse region text; ``crtf_default_frame`` is the frame of a CRTF sky
    region that states none (the image's own frame in CASA)."""
    fmt = (fmt or "auto").lower()
    if fmt == "auto":
        fmt = detect_format(text)
    if fmt == "crtf":
        return parse_crtf(text, diagnostics, default_frame=crtf_default_frame)
    if fmt == "ds9":
        return parse_ds9(text, diagnostics, physical)
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


def _is_ecliptic_grid(wcs) -> bool:
    """True for a WCS whose sky axes are ELON/ELAT (in either order)."""
    try:
        ctypes = [str(c)[:4].upper() for c in wcs.celestial.wcs.ctype]
    except (AttributeError, TypeError):
        return False
    return sorted(ctypes[:2]) == ["ELAT", "ELON"]


def celestial_frame(wcs):
    """The astropy frame of a celestial WCS's own coordinates: astropy's
    ``wcs_to_celestial_frame``, except for an ecliptic grid (ELON/ELAT axes), for
    which it has no frame and answers ICRS. That made a figure or a region file
    treat the image's own ecliptic coordinates as equatorial ones; here they are
    the mean ecliptic of J2000 that ``sky_frame("ecliptic")`` is."""
    from astropy.wcs.utils import wcs_to_celestial_frame  # noqa: PLC0415
    if _is_ecliptic_grid(wcs):
        return sky_frame("ecliptic")
    return wcs_to_celestial_frame(wcs)


def native_frame_name(wcs) -> str:
    """Canonical name of a celestial WCS's own frame; ``fk5`` when that frame
    has no canonical name (e.g. FK5 of another equinox), so a conversion to
    "the dataset's frame" lands in a frame region files can state."""
    from astropy.coordinates import FK4, FK5, ICRS, BarycentricMeanEcliptic, Galactic  # noqa: PLC0415
    from astropy.time import Time  # noqa: PLC0415

    f = celestial_frame(wcs)
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


def _north_rotation(lon, lat, src, dst):
    """Position angle, in ``dst``, of ``src``'s north at (lon, lat) in degrees
    (scalars or arrays).

    An angle measured from ``src``'s axes becomes, in ``dst``, that angle plus
    this one: both frames are right-handed on the sky, so the rotation is a
    pure offset. Measured numerically from a 1" step north.
    """
    import astropy.units as u  # noqa: PLC0415
    import numpy as np  # noqa: PLC0415
    from astropy.coordinates import SkyCoord  # noqa: PLC0415

    scalar = np.ndim(lon) == 0
    if src.is_equivalent_frame(dst):
        return 0.0 if scalar else np.zeros(np.shape(lon))
    lat = np.minimum(np.asarray(lat, float), 90.0 - 1e-6)
    c = SkyCoord(np.asarray(lon, float) * u.deg, lat * u.deg, frame=src)
    n = c.directional_offset_by(0 * u.deg, 1 * u.arcsec)
    pa = c.transform_to(dst).position_angle(n.transform_to(dst)).deg
    return float(pa) if scalar else np.asarray(pa, float)


class _PixelFrame:
    """Pixel ↔ native-sky mapping of a 2-D celestial WCS, and of angles and
    lengths between the sky and the pixel grid. The ``*_many`` methods take
    arrays, one entry per region."""

    def __init__(self, wcs, origin: int):
        import numpy as np  # noqa: PLC0415
        from astropy.wcs.utils import proj_plane_pixel_scales  # noqa: PLC0415

        self.wcs = wcs
        self.origin = origin
        self.frame = celestial_frame(wcs)
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

    def _offsets_to_pix(self, lon, lat, pa_deg, dist_deg):
        """Pixels of the points ``dist_deg`` from (lon, lat) at position
        angles ``pa_deg`` (arrays of one shape), NaN where off the projection."""
        import astropy.units as u  # noqa: PLC0415
        import numpy as np  # noqa: PLC0415
        from astropy.coordinates import SkyCoord, UnitSphericalRepresentation  # noqa: PLC0415

        c = SkyCoord(np.asarray(lon, float) * u.deg, np.asarray(lat, float) * u.deg, frame=self.frame)
        p = c.directional_offset_by(np.asarray(pa_deg, float) * u.deg, dist_deg * u.deg)
        s = p.represent_as(UnitSphericalRepresentation)
        x, y = self.to_pix(s.lon.deg.ravel(), s.lat.deg.ravel())
        return x.reshape(np.shape(pa_deg)), y.reshape(np.shape(pa_deg))

    def sky_angle_to_pix_many(self, lon, lat, angle):
        """ds9 sky angles at (lon, lat) in the native frame → pixel angles.

        The axis is followed one pixel scale along the sky (position angle
        ``angle - 90``) and the step is read on the grid, so rotation, mirror
        parity, unequal pixel scales and skew all come out of the WCS itself.
        """
        import numpy as np  # noqa: PLC0415
        lon, lat, angle = (np.atleast_1d(np.asarray(v, float)) for v in (lon, lat, angle))
        x0, y0 = self.to_pix(lon, lat)
        x1, y1 = self._offsets_to_pix(lon, lat, angle - 90.0, self.scale_deg)
        return np.degrees(np.arctan2(y1 - y0, x1 - x0))

    def sky_angle_to_pix(self, lon: float, lat: float, angle: float) -> float:
        a = self.sky_angle_to_pix_many([lon], [lat], [angle])
        _finite_or_raise(a)
        return float(a[0])

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

    def jacobian_many(self, lon, lat):
        """Pixels per degree of a small step east (column 0) and north
        (column 1) at each (lon, lat) in the native frame → array (n, 2, 2),
        by central differences of one pixel scale: the local grid with its
        rotation, parity, unequal scales and skew. NaN where off the projection."""
        import numpy as np  # noqa: PLC0415
        lon = np.atleast_1d(np.asarray(lon, float))
        lat = np.clip(np.atleast_1d(np.asarray(lat, float)), -90.0 + 1e-6, 90.0 - 1e-6)
        n = lon.size
        pa = np.broadcast_to(np.array([90.0, 270.0, 0.0, 180.0]), (n, 4))
        x, y = self._offsets_to_pix(np.repeat(lon[:, None], 4, 1), np.repeat(lat[:, None], 4, 1),
                                    pa, self.scale_deg)
        d = 2.0 * self.scale_deg
        jac = np.empty((n, 2, 2))
        jac[:, 0, 0], jac[:, 0, 1] = (x[:, 0] - x[:, 1]) / d, (x[:, 2] - x[:, 3]) / d
        jac[:, 1, 0], jac[:, 1, 1] = (y[:, 0] - y[:, 1]) / d, (y[:, 2] - y[:, 3]) / d
        return jac

    def jacobian(self, lon: float, lat: float):
        jac = self.jacobian_many([lon], [lat])[0]
        _finite_or_raise(jac)
        return jac


def _sky_dir_ppd(jac, angle: float) -> float:
    """Pixels per degree along the ds9 sky angle ``angle`` (PA = angle - 90)."""
    th = math.radians(angle - 90.0)
    e, n = math.sin(th), math.cos(th)
    (a, b), (c, d) = jac
    return math.hypot(a * e + b * n, c * e + d * n)


def _pix_dir_ppd(jac, angle: float) -> float:
    """Pixels per degree along the pixel angle ``angle``."""
    (a, b), (c, d) = jac
    det = a * d - b * c
    ux, uy = math.cos(math.radians(angle)), math.sin(math.radians(angle))
    e, n = (d * ux - b * uy) / det, (-c * ux + a * uy) / det    # degrees per pixel step
    return 1.0 / math.hypot(e, n)


def _iso_ppd(jac) -> float:
    """Pixels per degree for radii: the area scale sqrt|det J|."""
    (a, b), (c, d) = jac
    return math.sqrt(abs(a * d - b * c))


def _finite_or_raise(*arrays) -> None:
    import numpy as np  # noqa: PLC0415
    for a in arrays:
        if not np.all(np.isfinite(a)):
            raise RegionParseError("The region lies outside the dataset's sky projection.")


def _rescale(nr: dict[str, Any], src_unit: str, dst_unit: str, along, iso: float) -> None:
    """Rewrite sizes from ``src_unit`` to ``dst_unit`` in place, where one of
    the two is pixels: ``along(k)`` is pixels per degree along the region's
    axis ``k`` (0 along its angle, 1 across it) and ``iso`` the same for radii."""
    if src_unit == "pix":     # pixels → degrees → dst angle unit
        conv = 1.0 / _ANGLE_TO_DEG[dst_unit]
        _scale_sizes(nr, conv / along(0), conv / along(1), conv / iso)
    else:                     # src angle unit → degrees → pixels
        conv = _ANGLE_TO_DEG[src_unit]
        _scale_sizes(nr, conv * along(0), conv * along(1), conv * iso)
    nr["unit"] = dst_unit


def rescale_angle_units(region: dict[str, Any], dst_unit: str) -> None:
    """Rewrite a sky region's sizes from its own angle unit to ``dst_unit``
    (deg, arcmin or arcsec) in place. Pixel sizes are not handled here: a
    region moved onto the sky by :func:`convert_regions` already has its
    sizes in an angle unit, measured along its own axes."""
    src_unit = region.get("unit", "deg")
    if src_unit == dst_unit or not _has_sizes(region):
        region["unit"] = dst_unit
        return
    if src_unit not in _ANGLE_TO_DEG or dst_unit not in _ANGLE_TO_DEG:
        raise RegionParseError(f"Cannot rescale sizes from '{src_unit}' to '{dst_unit}'.")
    f = _ANGLE_TO_DEG[src_unit] / _ANGLE_TO_DEG[dst_unit]
    _scale_sizes(region, f, f, f)
    region["unit"] = dst_unit


def _has_sizes(region: dict[str, Any]) -> bool:
    return any(k in region for k in _ISO_SIZE_KEYS + _AXIS_SIZE_KEYS + ("length",))


def _batch_sky_to_image(regions: list[dict[str, Any]], pf: _PixelFrame) -> dict[int, tuple]:
    """For the sky regions with a centre, everything their move onto the grid
    needs, computed for all of them at once (a region file can hold
    thousands): index → (pixel x, y, native lon, lat, native angle, pixel
    angle, Jacobian). Entries are NaN for a region off the projection; the
    caller reports it. An empty answer makes the caller go region by region."""
    import numpy as np  # noqa: PLC0415

    groups: dict[str, list[int]] = {}
    for i, r in enumerate(regions):
        if r.get("frame") in _WORLD_FRAMES and r.get("shape") not in _POINT_LIST_SHAPES and "center" in r:
            groups.setdefault(r["frame"], []).append(i)
    out: dict[int, tuple] = {}
    try:
        for name, idx in groups.items():
            cur = sky_frame(name)
            lon = np.array([float(regions[i]["center"][0]) for i in idx])
            lat = np.array([float(regions[i]["center"][1]) for i in idx])
            ang = np.array([float(regions[i].get("angle_deg", 0.0) or 0.0) for i in idx])
            if not cur.is_equivalent_frame(pf.frame):
                ang = ang + _north_rotation(lon, lat, cur, pf.frame)
                lon, lat = _transform(lon, lat, cur, pf.frame)
            x, y = pf.to_pix(lon, lat)
            pix_ang = pf.sky_angle_to_pix_many(lon, lat, ang)
            jac = pf.jacobian_many(lon, lat)
            for k, i in enumerate(idx):
                out[i] = (x[k], y[k], lon[k], lat[k], ang[k], pix_ang[k], jac[k])
    except (RegionParseError, ValueError, KeyError, TypeError, IndexError):
        return {}                  # e.g. a SIP inverse failing for one of them: go one by one
    return out


def convert_regions(
    regions: list[dict[str, Any]],
    *,
    to_frame: str,
    wcs=None,
    origin: int = 1,
    diagnostics: list | None = None,
    no_wcs_reason: str = "",
) -> list[dict[str, Any]]:
    """Convert regions to ``to_frame``: ``image`` (pixels) or a sky frame.

    ``wcs`` is the dataset's 2-D celestial WCS (``WCS.celestial``), needed
    whenever pixels are involved; ``origin`` is the pixel base (1 for region
    files, 0 for numpy-indexed analysis). Sky ↔ sky needs no WCS.

    Positions go through astropy: from the region's frame to the WCS's own
    frame, then to pixels (and back). Sizes are angles on the sky and do not
    change between sky frames. A position angle is measured from the region
    frame's axes, so it is rotated by the angle between the two frames' north
    directions at the region's centre; to or from pixels, the axis is followed
    one pixel scale along the sky and read on the grid, which carries the
    grid's rotation, parity, unequal scales and skew. Between pixels and
    angles a size is measured along its own axis in the frame the region was
    drawn in (the width of a sky box along its sky angle, of a pixel box along
    its pixel angle) through the local grid at the centre; radii use the local
    area scale. In the image frame sizes always end in pixels.

    A region that cannot be converted (its centre or vertices do not project
    onto the WCS, pixels without a WCS) is reported in ``diagnostics`` by its
    ``source_line``, with ``no_wcs_reason`` when the WCS is missing, or raises
    ``RegionParseError`` when no list is given.
    """
    want_image = to_frame == "image"
    dst = None if want_image else sky_frame(to_frame)
    pf = _PixelFrame(wcs, origin) if wcs is not None else None
    batch = _batch_sky_to_image(regions, pf) if (want_image and pf is not None) else {}
    out: list[dict[str, Any]] = []
    for i, r in enumerate(regions):
        try:
            out.append(_convert_one(r, to_frame, want_image, dst, pf, no_wcs_reason, batch.get(i)))
        except RegionParseError as exc:
            _report(diagnostics, r.get("source_line"), "", str(exc))
        except (ValueError, KeyError, TypeError, IndexError) as exc:   # e.g. astropy refusing a value
            _report(diagnostics, r.get("source_line"), "", f"cannot convert this region: {exc}")
    return out


def _convert_one(r, to_frame, want_image, dst, pf, no_wcs_reason, pre=None) -> dict[str, Any]:
    src_name = r.get("frame", "image")
    src_unit = r.get("unit", "pix" if src_name == "image" else "deg")
    if src_name == to_frame and (not want_image or src_unit == "pix" or not _has_sizes(r)):
        nr = dict(r)
        if want_image:
            nr["unit"] = "pix"
        return nr
    if src_name != "image" and src_name not in _WORLD_FRAMES:
        raise RegionParseError(f"Unknown region frame '{src_name}'.")
    pixel_sizes_to_sky = src_unit == "pix" and not want_image and _has_sizes(r)
    if (want_image or src_name == "image" or pixel_sizes_to_sky) and pf is None:
        raise RegionParseError(no_wcs_reason or
                               "Converting between pixels and sky needs the dataset's WCS (dataset_id).")
    nr = dict(r)
    shape = r["shape"]
    # A shape with an angle but none given is at 0, and 0 turns with the frame too.
    has_angle = shape in _ANGLED_SHAPES
    angle = float(r.get("angle_deg", 0.0) or 0.0)

    if pre is not None:                             # a sky centre, computed with the others
        x, y, nlon, nlat, sky_angle, pix_angle, jac = pre
        _finite_or_raise([x, y, pix_angle], jac)
        if src_unit != "pix" and _has_sizes(r):
            _rescale(nr, src_unit, "pix", lambda k: _sky_dir_ppd(jac, sky_angle + 90.0 * k), _iso_ppd(jac))
        nr.update(frame="image", unit="pix", center=[float(x), float(y)])
        if has_angle:
            nr["angle_deg"] = _wrap_angle(float(pix_angle))
        return nr

    pts = r["points"] if shape in _POINT_LIST_SHAPES else [r["center"]]
    lon = [p[0] for p in pts]
    lat = [p[1] for p in pts]
    pix_angle = angle                               # the angle on the grid, for image regions

    # 1. to the native sky frame of the WCS (or straight to dst for sky→sky)
    if src_name == "image":
        if want_image:                              # angular sizes on a pixel region
            nlon, nlat = pf.to_sky(lon[:1], lat[:1])
            _finite_or_raise(nlon, nlat)
            jac = pf.jacobian(float(nlon[0]), float(nlat[0]))
            _rescale(nr, src_unit, "pix", lambda k: _pix_dir_ppd(jac, pix_angle + 90.0 * k),
                     _iso_ppd(jac))
            return nr
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
        if src_unit != "pix" and _has_sizes(r):
            jac = pf.jacobian(float(nlon[0]), float(nlat[0]))
            sky_angle = angle
            _rescale(nr, src_unit, "pix", lambda k: _sky_dir_ppd(jac, sky_angle + 90.0 * k),
                     _iso_ppd(jac))
        if has_angle:
            angle = pf.sky_angle_to_pix(float(nlon[0]), float(nlat[0]), angle)
        new_pts = [[float(a), float(b)] for a, b in zip(x, y)]
        nr["unit"] = "pix"
        nr["frame"] = "image"
    else:
        if pixel_sizes_to_sky:                     # pixel sizes on the sky: express them as angles
            if src_name == "image":
                jac = pf.jacobian(float(nlon[0]), float(nlat[0]))
                _rescale(nr, "pix", "arcsec", lambda k: _pix_dir_ppd(jac, pix_angle + 90.0 * k),
                         _iso_ppd(jac))
            else:
                if not cur.is_equivalent_frame(pf.frame):
                    na = angle + (_north_rotation(float(nlon[0]), float(nlat[0]), cur, pf.frame)
                                  if has_angle else 0.0)
                    plon, plat = _transform(nlon[:1], nlat[:1], cur, pf.frame)
                else:
                    na, plon, plat = angle, nlon[:1], nlat[:1]
                jac = pf.jacobian(float(plon[0]), float(plat[0]))
                _rescale(nr, "pix", "arcsec", lambda k: _sky_dir_ppd(jac, na + 90.0 * k),
                         _iso_ppd(jac))
        if has_angle:
            angle += _north_rotation(float(nlon[0]), float(nlat[0]), cur, dst)
        tlon, tlat = _transform(nlon, nlat, cur, dst)
        _finite_or_raise(tlon, tlat)
        new_pts = [[float(a), float(b)] for a, b in zip(tlon, tlat)]
        nr["frame"] = to_frame
    if shape in _POINT_LIST_SHAPES:
        nr["points"] = new_pts
    else:
        nr["center"] = new_pts[0]
    if has_angle:
        nr["angle_deg"] = _wrap_angle(angle)
    return nr


def _export_angle(a: float) -> float:
    """An angle for a region file: in [0, 360), and 0 for what is only the
    float noise of a pixel-to-sky round trip (359.999972818 → 0, issue #502).
    Within 1e-4° (0.4″, far below what a drawn region can show) of the axis
    the angle is the axis; elsewhere it keeps six decimals."""
    a = float(a) % 360.0
    if a < 1e-4 or a > 360.0 - 1e-4:
        return 0.0
    return round(a, 6)


def _wrap_angle(a: float) -> float:
    """Angle into [0, 360), rounded at 1e-9° so a no-op rotation stays exact."""
    a = round(a % 360.0, 9)
    return 0.0 if a >= 360.0 else a


# ── one call for the parse route ─────────────────────────────────────────────


def parse_report(
    text: str,
    fmt: str = "auto",
    *,
    to_frame: str = "",
    wcs=None,
    origin: int = 1,
    physical: PhysicalTransform | None = None,
    no_wcs_reason: str = "",
) -> dict[str, Any]:
    """Parse region text and optionally reframe it, collecting every statement
    that did not become a region → ``{format, regions, diagnostics}``.

    Diagnostics are sorted by line and carry the file's own text for the
    regions that failed in the conversion. A CRTF sky region without
    ``coord=`` is in the frame of ``wcs`` when one is given, as in CASA."""
    fmt = (fmt or "auto").lower()
    if fmt == "auto":
        fmt = detect_format(text)
    diagnostics: list[dict[str, Any]] = []
    crtf_default = "fk5"
    if wcs is not None and fmt == "crtf":
        try:
            crtf_default = native_frame_name(wcs)
        except ValueError:   # a celestial WCS astropy names no frame for
            pass
    regions = parse_regions(text, fmt, diagnostics=diagnostics, physical=physical,
                            crtf_default_frame=crtf_default)
    if to_frame:
        regions = convert_regions(regions, to_frame=to_frame, wcs=wcs, origin=origin,
                                  diagnostics=diagnostics, no_wcs_reason=no_wcs_reason)
    lines = text.splitlines()
    for d in diagnostics:
        if not d["text"] and d["line"] and 0 < d["line"] <= len(lines):
            d["text"] = lines[d["line"] - 1].strip()[:_DIAG_TEXT_MAX]
    diagnostics.sort(key=lambda d: d["line"] or 0)
    return {"format": fmt, "regions": regions, "diagnostics": diagnostics}
