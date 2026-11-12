"""Catalogue sources placed in a cube's pixels (issue #190, VR epic #166).

The VR page draws catalogue sources over a cube: VLKB compact sources and
filaments, a cross-match against an external catalogue, or a catalogue open
in the session (a SoFiA source list, a CSV of RA/Dec). Everything is placed
here, with the cube's own full WCS (astropy), so the projection is the file's
(CAR, TAN, SIN... with its CD/PC matrix), in the file's celestial frame
(equatorial or Galactic): a source catalogued in another frame is converted
first. The page only maps a pixel to its box (pixel i spans [i - 0.5, i + 0.5]).

The desktop's own overlays use the same conventions:

* a VLKB compact source is the ellipse of its FWHMs built in PIXEL space
  around its projected centre, the semi-major axis along +x rotated
  counter-clockwise by the catalogue's PA (the legacy VLKB client's rule,
  src/gui/vtkWindowImage_Catalogue.cpp), sized with the mean pixel scale;
* each band's ellipse sits on that band's own centroid (``glon250`` ...), or
  the band-merged one when the band has none (-9999);
* a filament is its contour, ``"glon1_glon2_..._,glat1_glat2_..._"``, each
  vertex projected.

A source with a spectral position (a frequency or a velocity column) is placed
at its channel too; the others have none, and the page draws them through the
whole spectral axis. Coordinates are 0-based pixels (FITS pixel 1 is 0 here).
"""
from __future__ import annotations

import math
import re
from dataclasses import dataclass, field
from typing import Any

import numpy as np

#: Bands of ``compactsources.sed_view_final`` with a whole ellipse, the
#: band-merged one first (as the desktop's kVlkbBands).
VLKB_BANDS = ("ft", "21", "22", "24", "70", "160", "250", "350", "500", "870", "1100")
#: Vertices of a drawn ellipse.
ELLIPSE_SAMPLES = 48
#: Names of a spectral column, and how each is read.
_SPECTRAL_NAMES = {
    "freq": ("Hz", None), "frequency": ("Hz", None), "nu": ("Hz", None),
    "v_rad": ("km/s", "radio"), "vrad": ("km/s", "radio"),
    "v_opt": ("km/s", "optical"), "vopt": ("km/s", "optical"), "cz": ("km/s", "optical"),
    # SoFiA's v_app is a velocity of the cube axis's own type, in its unit.
    "v_app": (None, "axis"),
    "vlsr": ("km/s", "radio"), "v_lsr": ("km/s", "radio"),
    # A velocity of no stated convention: read as the axis's own, and said.
    "velocity": ("km/s", "unknown"), "vel": ("km/s", "unknown"),
}
#: The frames a catalogue velocity is usually given in, for the frame check.
_BARY = ("BARYCENT", "HELIOCEN")
#: A spectral column is taken to be misread (a unit, a convention) only when
#: none of the sources the sky places on the cube falls in its channels and at
#: least this share falls outside them; a partial overlap (a wide-band survey
#: over a narrow-band cube) is real, and kept.
SPECTRAL_MISREAD_SHARE = 0.95


@dataclass
class Placed:
    sources: list[dict] = field(default_factory=list)
    polylines: list[dict] = field(default_factory=list)
    n_input: int = 0
    n_outside: int = 0
    n_unplaced: int = 0
    warnings: list[str] = field(default_factory=list)
    spectral: dict = field(default_factory=lambda: {"used": False})


@dataclass
class CubeGeometry:
    """The cube as the viewers address it: its non-degenerate axes in file
    order (x, y, z, as the VR page and the desktop draw them), and its WCS."""
    wcs: Any
    cel: Any                       # the celestial WCS (FITS axis order)
    shape: tuple[int, int, int]    # nx, ny, nz (nz = 1 for an image)


def cube_geometry(path: str) -> CubeGeometry:
    """The geometry of a cube (or image) through the backend's own reader
    (app/fits_dataset.py: the sanitised header, the axis map the viewers use)."""
    from .fits_dataset import ScientificFitsDataset   # noqa: PLC0415
    ds = ScientificFitsDataset(path)
    w = ds.wcs
    if w is None or not w.has_celestial:
        raise ValueError("The cube has no celestial WCS: catalogue sources cannot be placed.")
    axes = ds.client_axis_to_fits_axis       # viewer axis (1-based) -> FITS axis (1-based)
    sizes = ds.axis_sizes
    shape = tuple(int(sizes[axes[i] - 1]) if i in axes else 1 for i in (1, 2, 3))
    cel_axes = sorted((w.wcs.lng + 1, w.wcs.lat + 1))
    if (axes.get(1), axes.get(2)) != tuple(cel_axes):
        raise ValueError("The cube's first two axes are not its sky axes.")
    # The celestial WCS keeps the file's axis order, so its first pixel axis
    # is the viewers' x whichever of the two is the longitude.
    return CubeGeometry(w, w.celestial, shape)


def celestial_frame(cel) -> str:
    """'galactic' or 'icrs' (equatorial) for a celestial WCS."""
    ctype = str(cel.wcs.ctype[0]).upper()
    return "galactic" if ctype.startswith("GLON") else "icrs"


def pixel_scale_arcsec(cel) -> float:
    """Mean pixel scale (arcsec): the desktop's 0.5 (|CDELT1| + |CDELT2|) x 3600,
    read through the CD/PC matrix."""
    from astropy.wcs.utils import proj_plane_pixel_scales   # noqa: PLC0415
    s = proj_plane_pixel_scales(cel)
    return float(0.5 * (abs(s[0]) + abs(s[1])) * 3600.0)


def sky_to_pixel(cel, lon, lat, frame: str) -> tuple[np.ndarray, np.ndarray]:
    """0-based pixel x, y of sky positions (degrees) given in ``frame``
    ('icrs', 'fk5' or 'galactic'), through the cube's celestial WCS. NaN for a
    position the projection does not reach."""
    from astropy.coordinates import SkyCoord   # noqa: PLC0415
    lon = np.atleast_1d(np.asarray(lon, dtype=float))
    lat = np.atleast_1d(np.asarray(lat, dtype=float))
    x = np.full(lon.shape, np.nan)
    y = np.full(lon.shape, np.nan)
    ok = np.isfinite(lon) & np.isfinite(lat) & (np.abs(lat) <= 90.0)
    if ok.any():
        sc = SkyCoord(lon[ok], lat[ok], unit="deg", frame=frame)
        with np.errstate(invalid="ignore"):
            px, py = cel.world_to_pixel(sc)
        x[ok], y[ok] = np.asarray(px, dtype=float), np.asarray(py, dtype=float)
    return x, y


def galactic_footprint(cel, nx: int, ny: int) -> tuple[float, float, float, float]:
    """Galactic box (lon_min, lon_max, lat_min, lat_max) around the image, from
    its border sampled every few pixels; a longitude range across 0 comes back
    with lon_min < 0."""
    from astropy.coordinates import SkyCoord   # noqa: PLC0415
    step = max(1, min(nx, ny) // 32)
    xs = np.r_[np.arange(0, nx, step), nx - 1]
    ys = np.r_[np.arange(0, ny, step), ny - 1]
    bx = np.concatenate([xs, xs, np.zeros_like(ys), np.full_like(ys, nx - 1)]).astype(float)
    by = np.concatenate([np.zeros_like(xs), np.full_like(xs, ny - 1), ys, ys]).astype(float)
    # Half a pixel out: the box covers the outer pixels whole.
    bx = np.clip(bx, 0, nx - 1) + np.where(bx <= 0, -0.5, np.where(bx >= nx - 1, 0.5, 0.0))
    by = np.clip(by, 0, ny - 1) + np.where(by <= 0, -0.5, np.where(by >= ny - 1, 0.5, 0.0))
    sky = cel.pixel_to_world(bx, by)
    g = SkyCoord(sky).galactic
    lon = np.asarray(g.l.deg, dtype=float)
    lat = np.asarray(g.b.deg, dtype=float)
    ok = np.isfinite(lon) & np.isfinite(lat)
    if not ok.any():
        raise ValueError("The cube's celestial WCS gives no sky position for its border.")
    lon, lat = lon[ok], lat[ok]
    wrapped = np.where(lon > 180.0, lon - 360.0, lon)
    if np.ptp(wrapped) < np.ptp(lon):
        lon = wrapped
    l0, l1, b0, b1 = float(lon.min()), float(lon.max()), float(lat.min()), float(lat.max())
    # The desktop's margin (vtkWindowImage::imageGalacticFootprint): 10 % of
    # each side plus 0.01 degree.
    ml, mb = 0.1 * (l1 - l0) + 0.01, 0.1 * (b1 - b0) + 0.01
    return l0 - ml, l1 + ml, max(-90.0, b0 - mb), min(90.0, b1 + mb)


def vlkb_where(lon_col: str, lat_col: str, box: tuple[float, float, float, float]) -> str:
    """The ADQL condition of a Galactic box. A box across l = 0 (lon_min < 0,
    galactic_footprint) is two longitude ranges, the catalogue's glon being
    0..360."""
    l0, l1, b0, b1 = box
    if l1 - l0 >= 360.0:
        lon = "1 = 1"
    elif l0 < 0.0:
        lon = (f"({lon_col} BETWEEN {l0 + 360.0:.6f} AND 360 OR "
               f"{lon_col} BETWEEN 0 AND {l1:.6f})")
    elif l1 > 360.0:
        lon = (f"({lon_col} BETWEEN {l0:.6f} AND 360 OR "
               f"{lon_col} BETWEEN 0 AND {l1 - 360.0:.6f})")
    else:
        lon = f"{lon_col} BETWEEN {l0:.6f} AND {l1:.6f}"
    return f"{lon} AND {lat_col} BETWEEN {b0:.6f} AND {b1:.6f}"


def ellipse_pixels(cx: float, cy: float, fwhma_arcsec: float, fwhmb_arcsec: float,
                   pa_deg: float, arcsec_per_px: float, samples: int = ELLIPSE_SAMPLES
                   ) -> list[list[float]]:
    """The desktop's VLKB ellipse: semi-axes FWHM/2 in pixels, the major along
    +x, rotated counter-clockwise by PA, closed."""
    a = 0.5 * fwhma_arcsec / arcsec_per_px
    b = 0.5 * fwhmb_arcsec / arcsec_per_px
    th = math.radians(pa_deg)
    c, s = math.cos(th), math.sin(th)
    out = []
    for k in range(samples + 1):
        t = 2.0 * math.pi * k / samples
        ex, ey = a * math.cos(t), b * math.sin(t)
        out.append([cx + ex * c - ey * s, cy + ey * c + ex * s])
    return out


def parse_contour(text: str) -> list[tuple[float, float]]:
    """A VLKB contour, ``"glon1_glon2_..._,glat1_glat2_..._"``, as (glon, glat)."""
    t = str(text or "").strip().strip('"')
    if "," not in t:
        return []
    lons, lats = t.split(",", 1)
    out = []
    for a, b in zip(lons.split("_"), lats.split("_")):
        try:
            g, l = float(a), float(b)
        except ValueError:
            continue
        if math.isfinite(g) and math.isfinite(l):
            out.append((g, l))
    return out


def _number(row: dict, key: str) -> float | None:
    v = row.get(key)
    if v is None:
        return None
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return f if math.isfinite(f) and f > -9990.0 else None


def _inside(x: float, y: float, nx: int, ny: int, margin: float = 0.5) -> bool:
    return -margin <= x <= nx - 1 + margin and -margin <= y <= ny - 1 + margin


def place_vlkb_sources(rows: list[dict], cel, shape: tuple[int, int, int],
                       bands: tuple[str, ...] = ("ft",), limit: int = 5000) -> Placed:
    """Compact sources of ``sed_view_final`` (rows by column name), one ellipse
    per detected band, as the desktop builds them. Each band's positions are
    projected in one call."""
    nx, ny, _ = shape
    out = Placed(n_input=len(rows))
    arcpix = pixel_scale_arcsec(cel)
    seen = set()
    for band in bands:
        cand = []
        for row in rows:
            fa, fb, pa = (_number(row, f"{k}{band}") for k in ("fwhma", "fwhmb", "pa"))
            if fa is None or fb is None or pa is None or fa <= 0 or fb <= 0:
                continue
            group = str(row.get("designationft") or row.get("idcss") or "").strip()
            lon, lat = _number(row, f"glon{band}"), _number(row, f"glat{band}")
            if lon is None or lat is None:
                lon, lat = _number(row, "glonft"), _number(row, "glatft")
                if lon is None or lat is None:
                    continue
            key = (band, round(lon, 5), round(lat, 5), group)
            if key in seen:
                continue
            seen.add(key)
            cand.append((lon, lat, fa, fb, pa, group))
        if not cand:
            continue
        arr = np.array([c[:5] for c in cand], dtype=float)
        xs, ys = sky_to_pixel(cel, arr[:, 0], arr[:, 1], "galactic")
        for (lon, lat, fa, fb, pa, group), x, y in zip(cand, xs, ys):
            if not (math.isfinite(x) and math.isfinite(y)):
                out.n_unplaced += 1
                continue
            if not _inside(x, y, nx, ny):
                out.n_outside += 1
                continue
            label = group or f"G{lon:07.3f}{'-' if lat < 0 else '+'}{abs(lat):06.3f}"
            out.sources.append({
                "x": float(x), "y": float(y), "z": None, "band": band,
                "label": label if band == "ft" else f"{label} ({band} um)",
                "lon": lon, "lat": lat,
                "ellipse": ellipse_pixels(float(x), float(y), fa, fb, pa, arcpix),
            })
            if len(out.sources) >= limit:
                out.warnings.append(f"only the first {limit} sources are drawn")
                return out
    return out


def place_contours(rows: list[dict], cel, shape: tuple[int, int, int], noun: str = "filament",
                   closed: bool = False, limit: int = 5000) -> Placed:
    """Filaments (or bubbles) by their contour; a contour wholly outside the
    image is left out."""
    nx, ny, _ = shape
    out = Placed(n_input=len(rows))
    seen = set()
    for row in rows:
        ident = str(row.get("idfil") or row.get("idfil_mos") or row.get("idbubble") or "").strip()
        pts = parse_contour(row.get("contour", ""))
        if len(pts) < 2 or (ident and ident in seen):
            continue
        seen.add(ident)
        g = np.array(pts)
        x, y = sky_to_pixel(cel, g[:, 0], g[:, 1], "galactic")
        ok = np.isfinite(x) & np.isfinite(y)
        if ok.sum() < 2:
            out.n_unplaced += 1
            continue
        line = [[float(a), float(b)] for a, b in zip(x[ok], y[ok])]
        if closed and line[0] != line[-1]:
            line.append(line[0])
        if not any(_inside(a, b, nx, ny) for a, b in line):
            out.n_outside += 1
            continue
        out.polylines.append({"label": f"{noun} {ident}".strip(), "points": line})
        if len(out.polylines) >= limit:
            out.warnings.append(f"only the first {limit} {noun}s are drawn")
            break
    return out


_UNIT_SUFFIX = re.compile(r"^\s*(.*?)\s*[\[(]\s*([^\])]+?)\s*[\])]\s*$")


def name_and_unit(name: str) -> tuple[str, str | None]:
    """A column name and the unit its header gives (``vlsr (km/s)``,
    ``freq[Hz]``), or None."""
    m = _UNIT_SUFFIX.match(str(name))
    return (m.group(1), m.group(2)) if m and m.group(1) else (str(name), None)


def spectral_column(names: list[str], requested: str | None) -> str | None:
    """The catalogue's spectral column: the one asked for, else one of the
    usual names (freq, v_rad, v_opt, vlsr...), a unit in the name allowed."""
    if requested:
        return requested if requested in names else None
    base = {name_and_unit(n)[0].lower(): n for n in names}
    for key in _SPECTRAL_NAMES:
        if key in base:
            return base[key]
    return None


_AXIS_KIND = {"FREQ": ("frequency", None), "WAVE": ("wavelength", None),
              "AWAV": ("wavelength", None), "VRAD": ("velocity", "radio"),
              "VOPT": ("velocity", "optical"), "ZOPT": ("velocity", "optical"),
              "VELO": ("velocity", "relativistic")}


def spectral_pixels(wcs, values: np.ndarray, name: str, unit: str | None = None,
                    convention: str | None = None, notes: list | None = None
                    ) -> tuple[np.ndarray, str]:
    """0-based channel of each spectral value (NaN when none), and a note.

    The values are taken in the cube's own spectral reference frame (no
    barycentric/LSR change is applied). A frequency or wavelength is read on
    any spectral axis (through the rest frequency for a velocity axis). A
    velocity of the axis's own convention is read as it is: ``v_app`` (SoFiA
    writes it in the cube axis's own type and, without a unit, in the axis's
    unit), and a column of no stated convention (``velocity``, ``vel``), which
    is said in ``notes``. One of another convention goes through the rest
    frequency, and is refused without one."""
    import astropy.units as u   # noqa: PLC0415
    spec = wcs.spectral
    if spec.naxis == 0:
        raise ValueError("the cube has no spectral axis")
    base, name_unit = name_and_unit(name)
    default_unit, default_conv = _SPECTRAL_NAMES.get(base.lower(), ("km/s", "unknown"))
    ctype = str(spec.wcs.ctype[0]).upper()[:4]
    kind, axis_conv = _AXIS_KIND.get(ctype, (None, None))
    if kind is None:
        raise ValueError(f"the spectral axis {ctype} is not read here")
    axis_unit = u.Unit(str(spec.wcs.cunit[0]) or
                       {"frequency": "Hz", "wavelength": "m"}.get(kind, "m/s"))
    unit = unit or name_unit
    if not unit:
        if default_conv == "axis" and kind == "velocity":
            unit = str(axis_unit)          # SoFiA: the axis's own unit
        else:
            unit = default_unit or "km/s"
    if not convention:
        convention = default_conv
        if convention == "unknown":
            if kind == "velocity":
                if notes is not None:
                    notes.append(f"{name} states no velocity convention: read as the axis's "
                                 f"own ({axis_conv}); give spectral_convention otherwise")
                convention = "axis"
            else:
                convention = "radio"
                if notes is not None:
                    notes.append(f"{name} states no velocity convention: read as radio on "
                                 f"the {ctype} axis; give spectral_convention otherwise")
    q = np.asarray(values, dtype=float) * u.Unit(unit)
    rest = None
    if spec.wcs.restfrq:
        rest = spec.wcs.restfrq * u.Hz
    elif getattr(spec.wcs, "restwav", 0):
        rest = (spec.wcs.restwav * u.m).to(u.Hz, u.spectral())
    doppler = {"radio": u.doppler_radio, "optical": u.doppler_optical,
               "relativistic": u.doppler_relativistic}
    is_velocity = q.unit.physical_type in ("speed", "velocity")
    if is_velocity and convention in (None, "axis"):
        if kind != "velocity":
            raise ValueError(f"{name} is a velocity of the axis's own kind, and the axis "
                             f"({ctype}) is not a velocity")
        convention = axis_conv
    if is_velocity and convention not in doppler:
        raise ValueError(f"unknown velocity convention {convention!r}")
    with np.errstate(invalid="ignore", divide="ignore"):
        if is_velocity and kind == "velocity" and convention == axis_conv:
            world = q.to(axis_unit)
        elif kind == "velocity" or is_velocity:
            if rest is None:
                raise ValueError(f"{name} ({convention or 'spectral'}) on a {ctype} axis "
                                 "needs the cube's rest frequency (RESTFRQ)")
            hz = (q.to(u.Hz, doppler[convention](rest)) if is_velocity
                  else q.to(u.Hz, u.spectral()))
            world = (hz.to(axis_unit, doppler[axis_conv](rest)) if kind == "velocity"
                     else hz.to(axis_unit, u.spectral()))
        else:
            world = q.to(axis_unit, u.spectral())
        z = np.asarray(spec.all_world2pix(np.asarray(world.value, dtype=float)[:, None], 0),
                       dtype=float)[:, 0]
    note = f"{name} ({q.unit}{', ' + convention if is_velocity else ''}) on {ctype}"
    return np.atleast_1d(z), note


def frame_warnings(wcs, column: str, catalogue_specsys: str | None) -> list[str]:
    """What the spectral reference frames say about reading a catalogue's
    velocities in the cube's frame (no frame change is applied)."""
    out = []
    cube = str(getattr(wcs.wcs, "specsys", "") or "").strip().upper()
    cat = str(catalogue_specsys or "").strip().upper()
    if cat and cube and cat != cube:
        out.append(f"the catalogue's spectral frame is {cat}, the cube's {cube}: "
                   "no frame change is applied")
    elif cube and cube not in _BARY:
        out.append(f"the cube's spectral frame is {cube}: the catalogue's values are "
                   "taken in it")
    base = name_and_unit(column)[0].lower()
    if base in ("vlsr", "v_lsr") and cube in _BARY:
        out.append(f"{column} is an LSR velocity and the cube's frame {cube}: "
                   "no LSR correction is applied")
    return out


def place_table(frame, coordinates: dict, cel, wcs, shape: tuple[int, int, int],
                label_field: str | None = None, spectral_field: str | None = None,
                spectral_unit: str | None = None, spectral_convention: str | None = None,
                limit: int = 5000) -> Placed:
    """Sources of a catalogue in the session (a pandas frame), placed by its
    RA/Dec (ICRS) or Galactic columns, and by its spectral column when it has
    one the cube's spectral axis can read."""
    nx, ny, nz = shape
    system = coordinates.get("system")
    if system == "sky":
        lon_col, lat_col, sky_frame = coordinates.get("ra"), coordinates.get("dec"), "icrs"
    elif system == "galactic":
        lon_col, lat_col, sky_frame = coordinates.get("glon"), coordinates.get("glat"), "galactic"
    else:
        raise ValueError("The catalogue has no sky columns (RA/Dec or GLON/GLAT).")
    if lon_col not in frame.columns or lat_col not in frame.columns:
        raise ValueError(f"Sky columns {lon_col!r}, {lat_col!r} not found.")
    import pandas as pd   # noqa: PLC0415
    lon = pd.to_numeric(frame[lon_col], errors="coerce").to_numpy(dtype=float)
    lat = pd.to_numeric(frame[lat_col], errors="coerce").to_numpy(dtype=float)
    out = Placed(n_input=len(frame))
    x, y = sky_to_pixel(cel, lon, lat, sky_frame)
    z = np.full(len(frame), np.nan)
    col = spectral_column([str(c) for c in frame.columns], spectral_field)
    if col is not None and nz > 1:
        try:
            values = pd.to_numeric(frame[col], errors="coerce").to_numpy(dtype=float)
            # A VOTable's, FITS or IPAC table's own unit (app/dependencies.py keeps it).
            attrs = getattr(frame, "attrs", {}) or {}
            unit = spectral_unit or (attrs.get("units") or {}).get(col)
            notes: list[str] = []
            z, note = spectral_pixels(wcs, values, col, unit, spectral_convention, notes)
            out.spectral = {"used": True, "field": col, "note": note}
            out.warnings.extend(notes)
            out.warnings.extend(frame_warnings(wcs, col, attrs.get("specsys")))
            on_sky = np.isfinite(x) & np.isfinite(y) & np.array(
                [_inside(a, b, nx, ny) for a, b in zip(x, y)], dtype=bool)
            declared = bool(unit) or name_and_unit(col)[1] is not None
            z, note, used = _check_reading(z, note, on_sky, nz, wcs, values, col, unit,
                                           spectral_convention, out.warnings, declared)
            out.spectral = {"used": used, "field": col, "note": note}
            if not used:
                z = np.full(len(frame), np.nan)
        except Exception as exc:   # noqa: BLE001 - placed on the sky only, and said
            out.spectral = {"used": False, "field": col, "note": str(exc)}
            out.warnings.append(f"spectral column {col!r} not used: {exc}")
    labels = (frame[label_field].astype(str).tolist()
              if label_field and label_field in frame.columns else None)
    for i in range(len(frame)):
        if not (math.isfinite(x[i]) and math.isfinite(y[i])):
            out.n_unplaced += 1
            continue
        if not _inside(x[i], y[i], nx, ny):
            out.n_outside += 1
            continue
        zi = float(z[i]) if math.isfinite(z[i]) and -0.5 <= z[i] <= nz - 0.5 else None
        if out.spectral.get("used") and zi is None and math.isfinite(z[i]):
            out.n_outside += 1      # on the sky, outside the cube's channels
            continue
        out.sources.append({
            "x": float(x[i]), "y": float(y[i]), "z": zi, "band": "",
            "label": labels[i].strip() if labels else f"#{i}",
            "lon": float(lon[i]), "lat": float(lat[i]), "ellipse": None, "row": int(i),
        })
        if len(out.sources) >= limit:
            out.warnings.append(f"only the first {limit} sources are drawn")
            break
    return out


def _in_channels(z: np.ndarray, nz: int) -> np.ndarray:
    return np.isfinite(z) & (z >= -0.5) & (z <= nz - 0.5)


def _check_reading(z, note, on_sky, nz, wcs, values, col, unit, convention, warnings,
                   unit_declared=False):
    """``(z, note, used)``: the channels as read, unless the column looks
    misread. A partial overlap (some sources in the channels) is kept, the
    others counted outside. With none inside and nearly all outside, only a
    unit that nothing declared is tried again, a thousand times larger or
    smaller (m/s against km/s, Hz against kHz), and applied when it places
    most of them, with a warning. A convention is never changed: one given in
    the request or stated by the column's name may well put a catalogue
    outside a narrow band. Otherwise the sources go on the sky only, with a
    warning."""
    n_sky = int(on_sky.sum())
    inside = on_sky & _in_channels(z, nz)
    off = n_sky - int(inside.sum())
    if not n_sky:
        return z, note, True
    if inside.any() or off < SPECTRAL_MISREAD_SHARE * n_sky:
        if off:
            warnings.append(f"{off} of {n_sky} sources lie outside the cube's channels by "
                            f"{col!r} ({note})")
        return z, note, True
    best = None
    if not unit_declared:
        for f, what in ((1e3, "× 1000"), (1e-3, "/ 1000")):
            try:
                zz, nn = spectral_pixels(wcs, values * f, col, unit, convention)
            except Exception:   # noqa: BLE001 - a reading that does not apply
                continue
            k = int((on_sky & _in_channels(zz, nz)).sum())
            if k > 0.5 * n_sky and (best is None or k > best[0]):
                best = (k, zz, nn, f"{col} {what}")
    if best is not None:
        k, zz, nn, desc = best
        warnings.append(f"none of the {n_sky} sources falls in the cube's channels by {col!r} "
                        f"({note}), which declares no unit; read as {desc} instead ({nn}), "
                        f"{k} do: check the column's unit")
        return zz, nn, True
    warnings.append(f"none of the {n_sky} sources falls in the cube's channels by {col!r} "
                    f"({note}): placed on the sky only")
    return z, f"{note}: out of range", False


def label_column(frame) -> str | None:
    """A name column for the labels (name, designation, id...)."""
    lower = {str(c).lower(): c for c in frame.columns}
    for key in ("name", "designation", "main_id", "source", "id", "idcss", "objname"):
        if key in lower:
            return lower[key]
    return None


def as_response(p: Placed, source: str, frame: str, arcpix: float) -> dict[str, Any]:
    return {
        "valid": True, "error": "", "source": source, "frame": frame,
        "pixel_scale_arcsec": arcpix,
        "n_input": p.n_input, "n_sources": len(p.sources), "n_polylines": len(p.polylines),
        "n_outside": p.n_outside, "n_unplaced": p.n_unplaced,
        "spectral": p.spectral, "warnings": p.warnings,
        "sources": p.sources, "polylines": p.polylines,
    }
