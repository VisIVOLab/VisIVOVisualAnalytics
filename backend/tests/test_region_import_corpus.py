"""Region import corpus (issue #342): every ds9 and CRTF shape, in every frame
and coordinate format, placed on synthetic TAN, SIN, rotated/skewed-CD,
galactic CAR images and on the sky axes of a cube.

The expected pixel positions, sizes and angles are computed here
independently of ``app.regions``: with astropy's ``WCS.world_to_pixel`` on a
``SkyCoord`` in the region's own frame, and with segments of the region's
own size stepped along its axes on the sky (``directional_offset_by``) and
measured on the grid. ds9's angle is counter-clockwise from the frame's
longitude axis, i.e. a position angle of ``angle - 90``.

``python -m tests.test_region_import_corpus <dir>`` writes the corpus (.reg,
.crtf and the matching FITS images) to ``<dir>`` for trying it in the viewers.
"""
from __future__ import annotations

import math
import os
import sys
import tempfile
import unittest

import numpy as np

from app import regions as R

_TEST_TOKEN = "test-token-visivo-12345"
_N = 64                                  # image side, pixels
_ARCSEC = 1.0 / 3600.0


# ── synthetic headers ────────────────────────────────────────────────────────


def _hdr_tan():
    from astropy.io import fits
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"] = "RA---TAN", "DEC--TAN"
    h["CRPIX1"], h["CRPIX2"] = 32.5, 32.5
    h["CRVAL1"], h["CRVAL2"] = 202.4696, 47.1953
    h["CDELT1"], h["CDELT2"] = -1.0 * _ARCSEC, 1.0 * _ARCSEC
    h["CUNIT1"] = h["CUNIT2"] = "deg"
    h["RADESYS"] = "ICRS"
    return h


def _hdr_sin():
    from astropy.io import fits
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"] = "RA---SIN", "DEC--SIN"
    h["CRPIX1"], h["CRPIX2"] = 30.0, 34.0
    h["CRVAL1"], h["CRVAL2"] = 83.8221, -5.3911
    h["CDELT1"], h["CDELT2"] = -2.0 * _ARCSEC, 2.0 * _ARCSEC
    h["CUNIT1"] = h["CUNIT2"] = "deg"
    h["RADESYS"], h["EQUINOX"] = "FK5", 2000.0
    return h


def _hdr_cd():
    """TAN with a CD matrix: rotated 30°, unequal scales (1" and 1.6") and a skew."""
    from astropy.io import fits
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"] = "RA---TAN", "DEC--TAN"
    h["CRPIX1"], h["CRPIX2"] = 33.0, 31.0
    h["CRVAL1"], h["CRVAL2"] = 266.4049962340225, -28.93617240339179
    c, s = math.cos(math.radians(30.0)), math.sin(math.radians(30.0))
    s1, s2 = 1.0 * _ARCSEC, 1.6 * _ARCSEC
    h["CD1_1"], h["CD1_2"] = -s1 * c, s1 * s + 0.1 * s1
    h["CD2_1"], h["CD2_2"] = s2 * s, s2 * c
    h["RADESYS"] = "FK5"
    return h


def _hdr_car_galactic():
    from astropy.io import fits
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"] = "GLON-CAR", "GLAT-CAR"
    h["CRPIX1"], h["CRPIX2"] = 32.0, 32.0
    h["CRVAL1"], h["CRVAL2"] = 0.0, 0.0
    h["CDELT1"], h["CDELT2"] = -3.0 * _ARCSEC, 3.0 * _ARCSEC
    return h


def _hdr_cube():
    """A spectral cube: sky axes rotated by 20° with a PC matrix, then FREQ."""
    from astropy.io import fits
    h = fits.Header()
    h["NAXIS"] = 3
    h["CTYPE1"], h["CTYPE2"], h["CTYPE3"] = "RA---TAN", "DEC--TAN", "FREQ"
    h["CRPIX1"], h["CRPIX2"], h["CRPIX3"] = 32.0, 32.0, 1.0
    h["CRVAL1"], h["CRVAL2"], h["CRVAL3"] = 150.1, 2.2, 1.42e9
    h["CDELT1"], h["CDELT2"], h["CDELT3"] = -1.5 * _ARCSEC, 1.5 * _ARCSEC, 1.0e5
    c, s = math.cos(math.radians(20.0)), math.sin(math.radians(20.0))
    h["PC1_1"], h["PC1_2"], h["PC2_1"], h["PC2_2"] = c, -s, s, c
    h["CUNIT1"] = h["CUNIT2"] = "deg"
    h["CUNIT3"] = "Hz"
    h["RADESYS"] = "ICRS"
    return h


_HEADERS = {"tan": _hdr_tan, "sin": _hdr_sin, "cd": _hdr_cd, "car": _hdr_car_galactic, "cube": _hdr_cube}
# IRAF: image = LTM · physical + LTV. An image blocked 2× (LTM = 0.5) and cut
# 10.25 and -4.5 blocked pixels from the origin; and a Chandra-style cut.
_LTM = ((0.5, 0.0), (0.0, 0.5))
_LTV = (10.25, -4.5)


def _celestial(header):
    from astropy.wcs import WCS
    return WCS(header).celestial


# ── formatting positions the way the files write them ────────────────────────


def _sexa(value: float, hours: bool, style: str) -> str:
    v = value / 15.0 if hours else value
    sign = "-" if v < 0 else ("+" if not hours else "")
    v = abs(v)
    a = int(v)
    b = int((v - a) * 60.0)
    c = ((v - a) * 60.0 - b) * 60.0
    if c >= 59.9999999995:
        c, b = 0.0, b + 1
    if b == 60:
        b, a = 0, a + 1
    if style == "colon":
        return f"{sign}{a:02d}:{b:02d}:{c:011.8f}"
    if style == "casa":                   # CASA's dd.mm.ss.s for latitudes
        return f"{sign}{a:02d}.{b:02d}.{c:011.8f}"
    unit = "h" if hours else "d"
    return f"{sign}{a:02d}{unit}{b:02d}m{c:011.8f}s"


_EQUATORIAL = {"fk5", "icrs", "fk4"}


def _fmt_pos(lon: float, lat: float, frame: str, style: str) -> tuple[str, str]:
    """A sky position in one of the ds9 formats: degrees (bare or ``d``),
    radians (``r``), sexagesimal with colons or letters."""
    hours = frame in _EQUATORIAL
    if style == "deg":
        return f"{lon:.10f}", f"{lat:+.10f}"
    if style == "dsuffix":
        return f"{lon:.10f}d", f"{lat:+.10f}d"
    if style == "rad":
        return f"{math.radians(lon):.15f}r", f"{math.radians(lat):+.15f}r"
    if style in ("colon", "letters"):
        return _sexa(lon, hours, style), _sexa(lat, False, style)
    raise ValueError(style)


_DS9_STYLES = ("deg", "dsuffix", "rad", "colon", "letters")
# Size suffixes and their value in degrees.
_SIZE_FORMS = {'"': _ARCSEC, "'": 1.0 / 60.0, "d": 1.0, "r": 180.0 / math.pi, "": 1.0}


_AS, _AM = '"', "'"                      # arcsec and arcmin suffixes


def _size(arcsec: float, form: str) -> str:
    return f"{arcsec * _ARCSEC / _SIZE_FORMS[form]:.12g}{form}"


# ── independent expectations ─────────────────────────────────────────────────


class _Sky:
    """Independent sky → pixel reference for one WCS and one region frame."""

    def __init__(self, wcs, frame: str):
        self.wcs = wcs
        self.frame = R.sky_frame(frame)

    def coord(self, lon, lat):
        import astropy.units as u
        from astropy.coordinates import SkyCoord
        return SkyCoord(lon * u.deg, lat * u.deg, frame=self.frame)

    def pix(self, c) -> np.ndarray:
        x, y = self.wcs.world_to_pixel(c)
        return np.array([float(x) + 1.0, float(y) + 1.0])     # 1-based

    def axis(self, c, angle: float, half_deg: float) -> tuple[float, float]:
        """The pixel length and angle of a segment of ``2 * half_deg`` centred
        on ``c`` along the ds9 angle ``angle``."""
        import astropy.units as u
        p1 = c.directional_offset_by((angle - 90.0) * u.deg, half_deg * u.deg)
        p0 = c.directional_offset_by((angle + 90.0) * u.deg, half_deg * u.deg)
        d = self.pix(p1) - self.pix(p0)
        return float(np.hypot(*d)), math.degrees(math.atan2(d[1], d[0]))

    def radius(self, c, r_deg: float) -> float:
        """Radius in pixels of the same area: from the pixel vectors of small
        steps east and north (the parallelogram they span)."""
        import astropy.units as u
        step = 1e-4
        p0 = self.pix(c)
        e = self.pix(c.directional_offset_by(90 * u.deg, step * u.deg)) - p0
        n = self.pix(c.directional_offset_by(0 * u.deg, step * u.deg)) - p0
        return r_deg * math.sqrt(abs(e[0] * n[1] - e[1] * n[0])) / step


def _ds9_sky_corpus(wcs, frame: str, style: str, phys: R.PhysicalTransform | None):
    """A ds9 file with every shape in ``frame``/``style`` placed on ``wcs``,
    and the expected image-frame regions (one per region, in file order)."""
    import astropy.units as u
    sky = _Sky(wcs, frame)
    native = R._PixelFrame(wcs, 1)

    def at(px, py):
        """The sky position of 1-based pixel (px, py), in the region frame."""
        lon, lat = native.to_sky([px], [py])
        c = sky.coord(0, 0).__class__(lon[0] * u.deg, lat[0] * u.deg, frame=native.frame)
        t = c.transform_to(sky.frame)
        return float(t.spherical.lon.deg) % 360.0, float(t.spherical.lat.deg)

    lines = ["# Region file format: DS9 version 4.1",
             'global color=green dashlist=8 3 width=1 font="helvetica 10 normal roman" '
             "select=1 highlite=1 dash=0 fixed=0 edit=1 move=1 delete=1 include=1 source=1",
             frame]
    exp: list[dict] = []
    forms = list(_SIZE_FORMS)

    def P(lon, lat):
        a, b = _fmt_pos(lon, lat, frame, style)
        return f"{a},{b}"

    def read_back(lon, lat):
        # The expectation uses the value as written, read back by astropy.
        from astropy.coordinates import Angle
        a, b = _fmt_pos(lon, lat, frame, style)

        def val(tok, is_lon):
            if tok.endswith("r"):
                return math.degrees(float(tok[:-1]))
            if tok.endswith("d") and "m" not in tok:
                return float(tok[:-1])
            if ":" in tok or "m" in tok:
                unit = u.hourangle if (is_lon and frame in _EQUATORIAL) else u.deg
                return Angle(tok, unit=unit).deg
            return float(tok)
        return val(a, True), val(b, False)

    # circle (radius in each size form in turn)
    for i, form in enumerate(forms):
        lon, lat = at(20 + 4 * i, 12)
        r_as = 3.0 + i
        lines.append(f"circle({P(lon, lat)},{_size(r_as, form)}) # color=red tag={{c{i}}}")
        c = sky.coord(*read_back(lon, lat))
        exp.append({"shape": "circle", "center": sky.pix(c), "radius": sky.radius(c, r_as * _ARCSEC)})
    # ellipse, box at several angles with mixed size units
    for i, ang in enumerate((0.0, 37.5, 123.0)):
        lon, lat = at(18 + 10 * i, 24)
        a_as, b_as = 6.0, 3.5
        lines.append(f"ellipse({P(lon, lat)},{_size(a_as, _AS)},{_size(b_as, _AM)},{ang})")
        c = sky.coord(*read_back(lon, lat))
        la, aa = sky.axis(c, ang, a_as * _ARCSEC)
        lb, _ = sky.axis(c, ang + 90.0, b_as * _ARCSEC)
        exp.append({"shape": "ellipse", "center": sky.pix(c), "semi_axes": [la / 2, lb / 2],
                    "angle_mod": (aa, 180.0)})
        lon, lat = at(18 + 10 * i, 34)
        w_as, h_as = 9.0, 4.0
        lines.append(f"box({P(lon, lat)},{_size(w_as, 'd')},{_size(h_as, 'r')},{ang}) # dash=1")
        c = sky.coord(*read_back(lon, lat))
        lw, aw = sky.axis(c, ang, w_as * _ARCSEC / 2)
        lh, _ = sky.axis(c, ang + 90.0, h_as * _ARCSEC / 2)
        exp.append({"shape": "box", "center": sky.pix(c), "size": [lw, lh], "angle_mod": (aw, 180.0)})
    # polygon, line, point (styles), text, vector
    verts = [at(44, 10), at(52, 12), at(50, 18), at(45, 17)]
    lines.append("polygon(" + ",".join(P(*v) for v in verts) + ")")
    exp.append({"shape": "polygon", "points": [sky.pix(sky.coord(*read_back(*v))) for v in verts]})
    ends = [at(10, 44), at(20, 50)]
    lines.append("line(" + ",".join(P(*v) for v in ends) + ") # line=1 0")
    exp.append({"shape": "line", "points": [sky.pix(sky.coord(*read_back(*v))) for v in ends]})
    for k, style_name in enumerate(("circle", "box", "diamond", "cross", "x", "arrow", "boxcircle")):
        lon, lat = at(8 + 7 * k, 56)
        if k % 2:
            lines.append(f"point({P(lon, lat)}) # point={style_name} 9")
        else:
            lines.append(f"{style_name} point({P(lon, lat)})")
        exp.append({"shape": "point", "center": sky.pix(sky.coord(*read_back(lon, lat))),
                    "point": style_name})
    lon, lat = at(30, 46)
    lines.append(f"# text({P(lon, lat)}) text={{NGC 5194, core}}")
    exp.append({"shape": "text", "center": sky.pix(sky.coord(*read_back(lon, lat))), "text": "NGC 5194, core"})
    lon, lat = at(36, 44)
    lines.append(f"text {P(lon, lat).replace(',', ' ')} {{inline}}")
    exp.append({"shape": "text", "center": sky.pix(sky.coord(*read_back(lon, lat))), "text": "inline"})
    lon, lat = at(40, 40)
    lines.append(f"# vector({P(lon, lat)},{_size(12.0, _AS)},210) vector=1")
    c = sky.coord(*read_back(lon, lat))
    p1 = c.directional_offset_by((210.0 - 90.0) * u.deg, 12.0 * _ARCSEC * u.deg)
    d = sky.pix(p1) - sky.pix(c)
    exp.append({"shape": "vector", "center": sky.pix(c), "length": float(np.hypot(*d)),
                "angle_mod": (math.degrees(math.atan2(d[1], d[0])), 360.0)})
    # annuli: several radii, n=, elliptical and box with several pairs
    lon, lat = at(48, 30)
    lines.append(f"annulus({P(lon, lat)},{_size(2, _AS)},{_size(4, _AS)},{_size(6.5, _AS)})")
    c = sky.coord(*read_back(lon, lat))
    for r1, r2 in ((2, 4), (4, 6.5)):
        exp.append({"shape": "annulus", "center": sky.pix(c),
                    "radii": [sky.radius(c, r1 * _ARCSEC), sky.radius(c, r2 * _ARCSEC)]})
    lines.append(f"annulus {P(lon, lat).replace(',', ' ')} {_size(3, _AS)} {_size(9, _AS)} n=3")
    for r1, r2 in ((3, 5), (5, 7), (7, 9)):
        exp.append({"shape": "annulus", "center": sky.pix(c),
                    "radii": [sky.radius(c, r1 * _ARCSEC), sky.radius(c, r2 * _ARCSEC)]})
    lon, lat = at(52, 44)
    ang = 25.0
    lines.append(f"ellipse({P(lon, lat)},4\",2\",6\",3\",8\",4\",{ang})")
    c = sky.coord(*read_back(lon, lat))
    for (a1, b1), (a2, b2) in (((4, 2), (6, 3)), ((6, 3), (8, 4))):
        la1, aa = sky.axis(c, ang, a1 * _ARCSEC)
        lb1, _ = sky.axis(c, ang + 90, b1 * _ARCSEC)
        la2, _ = sky.axis(c, ang, a2 * _ARCSEC)
        lb2, _ = sky.axis(c, ang + 90, b2 * _ARCSEC)
        exp.append({"shape": "ellipse_annulus", "center": sky.pix(c), "inner": [la1 / 2, lb1 / 2],
                    "outer": [la2 / 2, lb2 / 2], "angle_mod": (aa, 180.0)})
    lines.append(f"box({P(lon, lat)},4\",2\",8\",4\",{ang})")
    lw1, aw = sky.axis(c, ang, 2 * _ARCSEC)
    lh1, _ = sky.axis(c, ang + 90, 1 * _ARCSEC)
    lw2, _ = sky.axis(c, ang, 4 * _ARCSEC)
    lh2, _ = sky.axis(c, ang + 90, 2 * _ARCSEC)
    exp.append({"shape": "box_annulus", "center": sky.pix(c), "inner": [lw1, lh1], "outer": [lw2, lh2],
                "angle_mod": (aw, 180.0)})
    # pixel sizes on a sky region: image pixels as written, physical through LTM
    lon, lat = at(12, 30)
    lines.append(f"circle({P(lon, lat)},5i)")
    exp.append({"shape": "circle", "center": sky.pix(sky.coord(*read_back(lon, lat))), "radius": 5.0})
    lines.append(f"circle({P(lon, lat)},5p)")
    exp.append({"shape": "circle", "center": sky.pix(sky.coord(*read_back(lon, lat))),
                "radius": 5.0 * (phys.scale if phys else 1.0)})
    return "\n".join(lines) + "\n", exp


def _pixel_corpus(phys: R.PhysicalTransform | None, wcs):
    """A ds9 file in ``image`` then ``physical`` coordinates, with expectations."""
    lines = ["# Region file format: DS9 version 4.1", "image",
             "circle(20.5,21.25,4.5)", "box(30,30,6,3,30)", "ellipse(40,12,5,2,-20)",
             "polygon(10,10,14,10,12,15)", "line(5,5,9,8)", "point(33.3,44.4)",
             "# vector(20,40,7,135)", "# text(25,25) text={pix}", "annulus(30,40,1,2,3)",
             "box(45,45,2,1,4,2,15)",
             'circle(50.5,50.5,3")',                       # angular size on a pixel region
             "physical", "circle(10,30,4)", "box(12,20,6,2,30)", "vector(10,10,8,60)",
             "polygon(0,10,4,10,2,15)", "circle(10,30,3i)"]
    E = [
        {"shape": "circle", "center": [20.5, 21.25], "radius": 4.5},
        {"shape": "box", "center": [30, 30], "size": [6, 3], "angle_mod": (30.0, 180.0)},
        {"shape": "ellipse", "center": [40, 12], "semi_axes": [5, 2], "angle_mod": (-20.0, 180.0)},
        {"shape": "polygon", "points": [[10, 10], [14, 10], [12, 15]]},
        {"shape": "line", "points": [[5, 5], [9, 8]]},
        {"shape": "point", "center": [33.3, 44.4]},
        {"shape": "vector", "center": [20, 40], "length": 7, "angle_mod": (135.0, 360.0)},
        {"shape": "text", "center": [25, 25], "text": "pix"},
        {"shape": "annulus", "center": [30, 40], "radii": [1, 2]},
        {"shape": "annulus", "center": [30, 40], "radii": [2, 3]},
        {"shape": "box_annulus", "center": [45, 45], "inner": [2, 1], "outer": [4, 2], "angle_mod": (15.0, 180.0)},
    ]
    # 3" on the grid around (50.5, 50.5): area scale from the WCS pixel vectors.
    native = R._PixelFrame(wcs, 1)
    lon, lat = native.to_sky([50.5], [50.5])
    sky = _Sky(wcs, R.native_frame_name(wcs))
    import astropy.units as u
    c = sky.coord(0, 0).__class__(lon[0] * u.deg, lat[0] * u.deg, frame=native.frame)
    E.append({"shape": "circle", "center": [50.5, 50.5], "radius": sky.radius(c, 3 * _ARCSEC)})
    ltm = np.array(phys.ltm if phys else _identity())
    ltv = np.array(phys.ltv if phys else (0.0, 0.0))
    fwd = ltm            # image = LTM · physical + LTV, written out here on its own

    def img(p):
        return fwd @ np.asarray(p, float) + ltv
    det_scale = math.sqrt(abs(np.linalg.det(ltm)))
    E.append({"shape": "circle", "center": img([10, 30]), "radius": 4 * det_scale})
    a = math.radians(30.0)
    wv, hv = fwd @ (6 * np.array([math.cos(a), math.sin(a)])), fwd @ (2 * np.array([-math.sin(a), math.cos(a)]))
    E.append({"shape": "box", "center": img([12, 20]), "size": [np.hypot(*wv), np.hypot(*hv)],
              "angle_mod": (math.degrees(math.atan2(wv[1], wv[0])), 180.0)})
    a = math.radians(60.0)
    vv = fwd @ (8 * np.array([math.cos(a), math.sin(a)]))
    E.append({"shape": "vector", "center": img([10, 10]), "length": np.hypot(*vv),
              "angle_mod": (math.degrees(math.atan2(vv[1], vv[0])), 360.0)})
    E.append({"shape": "polygon", "points": [img(p) for p in ([0, 10], [4, 10], [2, 15])]})
    E.append({"shape": "circle", "center": img([10, 30]), "radius": 3.0})      # 'i': image pixels
    return "\n".join(lines) + "\n", E


def _identity():
    return ((1.0, 0.0), (0.0, 1.0))


def _crtf_corpus(wcs, frame: str, style: str):
    """A CRTF file with every CRTF shape in ``frame`` (a CRTF coord name)."""
    import astropy.units as u
    canon = R._CRTF_COORD_TO_FRAME[frame.lower()]
    sky = _Sky(wcs, canon)
    native = R._PixelFrame(wcs, 1)

    def at(px, py):
        lon, lat = native.to_sky([px], [py])
        c = sky.coord(0, 0).__class__(lon[0] * u.deg, lat[0] * u.deg, frame=native.frame)
        t = c.transform_to(sky.frame)
        return float(t.spherical.lon.deg) % 360.0, float(t.spherical.lat.deg)

    hours = canon in _EQUATORIAL

    def P(lon, lat):
        if style == "deg":
            return f"[{lon:.10f}deg, {lat:.10f}deg]"
        if style == "rad":
            return f"[{math.radians(lon):.15f}rad, {math.radians(lat):.15f}rad]"
        if style == "colon":       # RA h:m:s, Dec CASA dd.mm.ss
            return f"[{_sexa(lon, hours, 'colon')}, {_sexa(lat, False, 'casa')}]"
        return f"[{_sexa(lon, hours, 'letters')}, {_sexa(lat, False, 'letters')}]"

    def read_back(lon, lat):
        from astropy.coordinates import Angle
        if style == "deg":
            return float(f"{lon:.10f}"), float(f"{lat:.10f}")
        if style == "rad":
            return math.degrees(float(f"{math.radians(lon):.15f}")), math.degrees(float(f"{math.radians(lat):.15f}"))
        lon_s = _sexa(lon, hours, "colon" if style == "colon" else "letters")
        lat_s = _sexa(lat, False, "casa" if style == "colon" else "letters")
        lon_v = Angle(lon_s, unit=u.hourangle if hours else u.deg).deg
        if style == "colon":
            sign = -1 if lat_s.startswith("-") else 1
            d, m, s = lat_s.lstrip("+-").split(".", 2)
            lat_v = sign * (int(d) + int(m) / 60 + float(s) / 3600)
        else:
            lat_v = Angle(lat_s, unit=u.deg).deg
        return lon_v, lat_v

    co = f" coord={frame}"
    lines = ["#CRTFv0 CASA Region Text Format version 0"]
    E = []
    lon, lat = at(20, 20)
    c = sky.coord(*read_back(lon, lat))
    lines.append(f"circle[{P(lon, lat)}, 5arcsec]{co}, color=red")
    E.append({"shape": "circle", "center": sky.pix(c), "radius": sky.radius(c, 5 * _ARCSEC)})
    # CASA: b1 = 6" along the position angle 40° (from north), b2 = 3" across
    # it; the ds9 axis at angle 40 has position angle -50°, i.e. along b2.
    lines.append(f"ellipse[{P(lon, lat)}, [0.1arcmin, 3arcsec], 40deg]{co}")
    la, aa = sky.axis(c, 40, 3 * _ARCSEC)
    lb, _ = sky.axis(c, 130, 6 * _ARCSEC)
    E.append({"shape": "ellipse", "center": sky.pix(c), "semi_axes": [la / 2, lb / 2], "angle_mod": (aa, 180.0)})
    lines.append(f"rotbox[{P(lon, lat)}, [8arcsec, 4000mas], 70deg]{co}")
    lw, aw = sky.axis(c, 70, 4 * _ARCSEC)
    lh, _ = sky.axis(c, 160, 2 * _ARCSEC)
    E.append({"shape": "box", "center": sky.pix(c), "size": [lw, lh], "angle_mod": (aw, 180.0)})
    lines.append(f"centerbox[{P(lon, lat)}, [{6 * _ARCSEC:.12g}deg, {math.radians(3 * _ARCSEC):.15g}rad]]{co}")
    lw, aw = sky.axis(c, 0, 3 * _ARCSEC)
    lh, _ = sky.axis(c, 90, 1.5 * _ARCSEC)
    E.append({"shape": "box", "center": sky.pix(c), "size": [lw, lh], "angle_mod": (aw, 180.0)})
    lines.append(f"annulus[{P(lon, lat)}, [2arcsec, 4arcsec]]{co}")
    E.append({"shape": "annulus", "center": sky.pix(c),
              "radii": [sky.radius(c, 2 * _ARCSEC), sky.radius(c, 4 * _ARCSEC)]})
    lines.append(f"-symbol[{P(lon, lat)}, D]{co}")
    E.append({"shape": "point", "center": sky.pix(c), "point": "diamond", "include": False})
    lines.append(f"ann text[{P(lon, lat)}, 'a label']{co}")
    E.append({"shape": "text", "center": sky.pix(c), "text": "a label"})
    verts = [at(40, 40), at(48, 40), at(44, 50)]
    lines.append("poly[" + ", ".join(P(*v) for v in verts) + f"]{co}")
    E.append({"shape": "polygon", "points": [sky.pix(sky.coord(*read_back(*v))) for v in verts]})
    lines.append("line[" + ", ".join(P(*v) for v in verts[:2]) + f"]{co}")
    E.append({"shape": "line", "points": [sky.pix(sky.coord(*read_back(*v))) for v in verts[:2]]})
    lines.append("vector[" + ", ".join(P(*v) for v in verts[1:]) + f"]{co}")
    E.append({"shape": "line", "points": [sky.pix(sky.coord(*read_back(*v))) for v in verts[1:]]})
    # Pixel positions are 0-based in CRTF, 1-based in the canonical model.
    lines.append("circle[[9pix, 19pix], 3pix]")
    E.append({"shape": "circle", "center": [10, 20], "radius": 3})
    lines.append("box[[9pix, 9pix], [19pix, 14pix]]")
    E.append({"shape": "box", "center": [15, 12.5], "size": [10, 5], "angle_mod": (0.0, 180.0)})
    return "\n".join(lines) + "\n", E


# ── comparison ───────────────────────────────────────────────────────────────


def _close_mod(a: float, b: float, period: float) -> float:
    return abs((a - b + period / 2) % period - period / 2)


def _compare(tc, got: list[dict], exp: list[dict], tag, pos_tol=2e-4, rel_tol=2e-4, ang_tol=2e-3):
    tc.assertEqual([g["shape"] for g in got], [e["shape"] for e in exp], tag)
    for g, e in zip(got, exp):
        t = (tag, e["shape"], g.get("source_line"))
        tc.assertEqual(g["frame"], "image", t)
        if "center" in e:
            np.testing.assert_allclose(g["center"], e["center"], rtol=0, atol=pos_tol, err_msg=str(t))
        if "points" in e:
            np.testing.assert_allclose(g["points"], np.asarray(e["points"], float), rtol=0, atol=pos_tol,
                                       err_msg=str(t))
        for k in ("radius", "radii", "semi_axes", "size", "inner", "outer", "length"):
            if k in e:
                tc.assertEqual(g["unit"], "pix", t)
                np.testing.assert_allclose(g[k], e[k], rtol=rel_tol, atol=1e-6, err_msg=f"{t} {k}")
        if "angle_mod" in e:
            ang, period = e["angle_mod"]
            tc.assertLess(_close_mod(g["angle_deg"], ang, period), ang_tol, f"{t} angle {g['angle_deg']} vs {ang}")
        if "text" in e:
            tc.assertEqual(g["text"], e["text"], t)
        if "point" in e:
            tc.assertEqual(g["props"].get("point"), e["point"], t)
        if "include" in e:
            tc.assertEqual(g["props"].get("include"), e["include"], t)


# ── the tests ────────────────────────────────────────────────────────────────


_DS9_FRAMES = ("fk5", "icrs", "fk4", "galactic", "ecliptic")


class TestDs9Corpus(unittest.TestCase):
    """Every ds9 shape × sky frame × coordinate format × WCS, against astropy."""

    def test_sky_frames_formats_and_wcs(self):
        phys = R.PhysicalTransform(_LTM, _LTV)
        n = 0
        for wname, hdr in _HEADERS.items():
            wcs = _celestial(hdr())
            for frame in _DS9_FRAMES:
                for style in _DS9_STYLES:
                    text, exp = _ds9_sky_corpus(wcs, frame, style, phys)
                    rep = R.parse_report(text, "auto", to_frame="image", wcs=wcs, physical=phys)
                    self.assertEqual(rep["format"], "ds9")
                    self.assertEqual(rep["diagnostics"], [], (wname, frame, style))
                    _compare(self, rep["regions"], exp, (wname, frame, style))
                    n += len(exp)
        self.assertEqual(n, len(_HEADERS) * len(_DS9_FRAMES) * len(_DS9_STYLES) * 33)

    def test_image_and_physical(self):
        for wname, hdr in _HEADERS.items():
            wcs = _celestial(hdr())
            for phys in (R.PhysicalTransform(_LTM, _LTV), None):
                text, exp = _pixel_corpus(phys, wcs)
                rep = R.parse_report(text, "ds9", to_frame="image", wcs=wcs, physical=phys)
                self.assertEqual(rep["diagnostics"], [], wname)
                _compare(self, rep["regions"], exp, (wname, phys is not None))

    def test_rotated_ltm_turns_angles(self):
        a = math.radians(25.0)
        ltm = ((0.5 * math.cos(a), -0.5 * math.sin(a)), (0.5 * math.sin(a), 0.5 * math.cos(a)))
        phys = R.PhysicalTransform(ltm, (3.0, -2.0))
        reg = R.parse_ds9("physical\nbox(20,30,8,4,10)\n", physical=phys)[0]
        np.testing.assert_allclose(reg["center"], np.array(ltm) @ [20, 30] + [3.0, -2.0], atol=1e-12)
        np.testing.assert_allclose(reg["size"], [4.0, 2.0], rtol=1e-12)
        self.assertAlmostEqual(reg["angle_deg"], 10.0 + 25.0, places=9)

    def test_chandra_and_blocked_physical(self):
        # A Chandra cut (LTV = -3799.5) and an 8x blocked image, as the
        # reviewers of ds9's physical frame would check them by hand.
        cut = R.PhysicalTransform(((1, 0), (0, 1)), (-3799.5, -3899.5))
        reg = R.parse_ds9("physical\ncircle(4000,4000,10)\n", physical=cut)[0]
        self.assertEqual((reg["center"], reg["radius"]), ([200.5, 100.5], 10.0))
        blk = R.PhysicalTransform(((0.125, 0), (0, 0.125)), (0.5625, 0.5625))
        reg = R.parse_ds9("physical\ncircle(80.5,80.5,80)\n", physical=blk)[0]
        np.testing.assert_allclose(reg["center"], [10.625, 10.625])
        self.assertAlmostEqual(reg["radius"], 10.0)

    def test_physical_without_keywords_is_image(self):
        reg = R.parse_ds9("physical\ncircle(10,20,3)\n")[0]
        self.assertEqual((reg["frame"], reg["center"], reg["radius"]), ("image", [10.0, 20.0], 3.0))

    def test_frames_switch_within_a_file(self):
        wcs = _celestial(_hdr_tan())
        native = R._PixelFrame(wcs, 1)
        lon, lat = native.to_sky([20.0], [30.0])
        import astropy.units as u
        from astropy.coordinates import SkyCoord
        g = SkyCoord(lon[0] * u.deg, lat[0] * u.deg, frame=native.frame).galactic
        text = (f"icrs;circle({lon[0]},{lat[0]},2\")\nimage\ncircle(5,6,1)\n"
                f"galactic; circle({g.l.deg},{g.b.deg},2\") ; image ; point(7,8)\n")
        rep = R.parse_report(text, "ds9", to_frame="image", wcs=wcs)
        self.assertEqual(rep["diagnostics"], [])
        self.assertEqual([r["source_line"] for r in rep["regions"]], [1, 3, 4, 4])
        np.testing.assert_allclose(rep["regions"][0]["center"], [20, 30], atol=1e-6)
        np.testing.assert_allclose(rep["regions"][2]["center"], [20, 30], atol=1e-6)
        self.assertEqual(rep["regions"][3]["center"], [7.0, 8.0])

    def test_properties(self):
        text = ('global color=green width=1 tag={all} dash=0\n'
                'image\n'
                'circle(10,10,3) # color=red width=3 dash=1 tag={a} tag={b c} text={x, y} font="times 12 bold"\n'
                '-box(5,5,2,2,0) # background\n'
                'line(1,1,4,4) # line=1 1 color=#00ff00\n')
        c, b, ln = R.parse_ds9(text)
        self.assertEqual(c["props"]["color"], "red")
        self.assertEqual(c["props"]["width"], 3)
        self.assertTrue(c["props"]["dash"])
        self.assertEqual(c["props"]["tag"], ["all", "a", "b c"])
        self.assertEqual(c["props"]["text"], "x, y")
        self.assertEqual(c["props"]["font"], "times 12 bold")
        self.assertEqual((b["props"]["include"], b["props"]["source"], b["props"]["color"]),
                         (False, False, "green"))
        self.assertNotIn("include", c["props"])
        self.assertEqual((ln["props"]["arrows"], ln["props"]["color"]), ([1, 1], "#00ff00"))

    def test_round_trip_of_new_shapes(self):
        text = ("image\n# vector(20,40,7,135) vector=1\n# text(25,25) text={a b}\nline(5,5,9,8) # line=0 1\n"
                "annulus(30,40,1,2,3)\nellipse(45,45,2,1,4,2,15)\nbox(45,45,2,1,4,2,15)\n"
                "# segment(1,1,2,2,3,1)\n-circle point(3,4) # color=blue\n"
                "fk5\n# vector(202.4,47.2,12\",30)\n")
        regs = R.parse_ds9(text)
        back = R.parse_ds9(R.export_ds9(regs))
        self.assertEqual(len(back), len(regs))
        for a, b in zip(regs, back):
            self.assertEqual((a["shape"], a["frame"]), (b["shape"], b["frame"]))
            for k in ("center", "points", "radii", "inner", "outer", "length", "angle_deg", "text"):
                if k in a:
                    np.testing.assert_allclose(b[k], a[k]) if k != "text" else self.assertEqual(b[k], a[k])
            for k in ("arrows", "point", "include", "color", "arrow"):
                self.assertEqual(a["props"].get(k), b["props"].get(k), (a["shape"], k))


class TestDiagnostics(unittest.TestCase):
    """Nothing is dropped silently: each statement not read has a line and a reason."""

    TEXT = ("# Region file format: DS9 version 4.1\n"
            "image\n"
            "circle(10,10,3)\n"
            "panda(10,10,0,360,4,1,2,2)\n"
            "# compass(10,10,5) compass=image {N} {E} 1 1\n"
            "circle(10,10)\n"
            "polygon(1,1,2,2)\n"
            "wcs\n"
            "circle(1,2,3)\n"
            "fk5\n"
            "circle(10,abc,3\")\n"
            "circle(10,10,5i,3\")\n"
            "hello world\n"
            "# a plain comment, not reported\n"
            "fk5; circle(202.4,47.2,3\")\n")

    def test_each_statement_reported(self):
        diags = []
        regs = R.parse_ds9(self.TEXT, diags)
        self.assertEqual(len(regs), 2)
        self.assertEqual([d["line"] for d in diags], [4, 5, 6, 7, 9, 11, 12, 13])
        reasons = " | ".join(d["reason"] for d in diags)
        for word in ("panda", "compass", "takes 3 values", "at least 3", "'wcs'", "abc", "not a ds9"):
            self.assertIn(word, reasons)
        self.assertEqual(diags[0]["text"], "panda(10,10,0,360,4,1,2,2)")

    def test_mixed_size_units_reported(self):
        diags = []
        R.parse_ds9('fk5\nbox(10,20,5i,3",0)\n', diags)
        self.assertEqual(len(diags), 1)
        self.assertIn("mixing pixel and angular", diags[0]["reason"])

    def test_review_cases(self):
        diags = []
        regs = R.parse_ds9('fk5\ncircle(10,95,3")\ncircle(10,20,3")\nannulus(1,2,1,3,n=100000000)\n'
                           "image\ncircle(1,2,3) # width=1.e999 textangle=1e999 point=circle 1e999\n"
                           "global include=0 background\n+circle(1,2,3) # source\ncircle(4,5,6)\n"
                           "ellipse(10,20,3,4,1,2,0)\n", diags)
        self.assertEqual([d["line"] for d in diags], [2, 4])
        self.assertIn("latitude", diags[0]["reason"])
        self.assertNotIn("width", regs[1]["props"])
        self.assertNotIn("textangle", regs[1]["props"])
        self.assertEqual(regs[2]["props"].get("include", True), True)
        self.assertNotIn("source", regs[2]["props"])
        self.assertEqual((regs[3]["props"]["include"], regs[3]["props"]["source"]), (False, False))
        self.assertEqual((regs[4]["inner"], regs[4]["outer"]), ([1.0, 2.0], [3.0, 4.0]))
        diags = []
        R.parse_crtf("circle[[1deg, 2deg], 3arcsec], linewidth=1e999, fontsize=1.5e999\n"
                     "circle[[1deg, 95deg], 3arcsec]\n", diags)
        self.assertEqual([d["line"] for d in diags], [2])

    def test_excluded_commented_shapes_round_trip(self):
        regs = R.parse_ds9("image\n-text(1,2) # text={a}\n-vector(3,4,5,6)\n-segment(1,1,2,2,3,1)\n")
        back = R.parse_ds9(R.export_ds9(regs))
        self.assertEqual([(r["shape"], r["props"].get("include")) for r in back],
                         [("text", False), ("vector", False), ("line", False)])

    def test_crtf_ellipse_matches_casa(self):
        # b1 along the position angle from north: at pa = 0 the 5-pixel
        # semi-axis is along +y, the 3-pixel one along +x.
        e = R.parse_crtf("ellipse[[10pix, 20pix], [5pix, 3pix], 0deg]\n")[0]
        self.assertEqual((e["semi_axes"], e["angle_deg"]), ([3.0, 5.0], 0.0))
        self.assertIn("[5pix, 3pix], 0deg", R.export_crtf([e]))

    def test_strict_mode_still_raises(self):
        with self.assertRaises(R.RegionParseError):
            R.parse_ds9("image\npanda(1,2,0,360,4,1,2,2)\n")

    def test_sky_region_without_wcs(self):
        rep = R.parse_report('image\ncircle(3,3,1)\nfk5\ncircle(10,20,3")\npoint(10,20)\n', "ds9",
                             to_frame="image", wcs=None, no_wcs_reason="the dataset has no celestial WCS")
        self.assertEqual([r["source_line"] for r in rep["regions"]], [2])
        self.assertEqual([(d["line"], d["text"]) for d in rep["diagnostics"]],
                         [(4, 'circle(10,20,3")'), (5, "point(10,20)")])
        self.assertIn("no celestial WCS", rep["diagnostics"][0]["reason"])

    def test_crtf_diagnostics(self):
        diags = []
        regs = R.parse_crtf("#CRTF\ncircle[[1pix,2pix],3pix]\nfoo[[1pix,2pix]]\n"
                            "circle[[1deg,2deg],3arcsec] coord=SUPERGAL\nglobal coord=AZEL\n"
                            "circle[[1deg,2deg],3arcsec]\ncircle[[1pix,2pix],3pix]\npoly[[1pix,1pix],[2pix,2pix]]\n",
                            diags)
        self.assertEqual(len(regs), 2)
        self.assertEqual([d["line"] for d in diags], [3, 4, 5, 6, 8])


class TestCrtfCorpus(unittest.TestCase):
    def test_every_shape_frame_format_and_wcs(self):
        n = 0
        for wname, hdr in _HEADERS.items():
            wcs = _celestial(hdr())
            for frame in ("J2000", "ICRS", "B1950", "GALACTIC", "ECLIPTIC"):
                for style in ("deg", "rad", "colon", "letters"):
                    text, exp = _crtf_corpus(wcs, frame, style)
                    rep = R.parse_report(text, "auto", to_frame="image", wcs=wcs)
                    self.assertEqual(rep["format"], "crtf")
                    self.assertEqual(rep["diagnostics"], [], (wname, frame, style))
                    _compare(self, rep["regions"], exp, (wname, frame, style))
                    n += len(exp)
        self.assertEqual(n, len(_HEADERS) * 5 * 4 * 12)

    def test_round_trip_new_shapes(self):
        text = ("#CRTF\nline[[1pix, 2pix], [3pix, 4pix]], color=red\ntext[[5pix, 6pix], 'hi there']\n"
                "-symbol[[7pix, 8pix], s]\nvector[[10deg, 20deg], [10.01deg, 20.01deg]] coord=ICRS\n")
        regs = R.parse_crtf(text)
        back = R.parse_crtf(R.export_crtf(regs))
        self.assertEqual([r["shape"] for r in back], ["line", "text", "point", "line"])
        np.testing.assert_allclose(back[0]["points"], [[2, 3], [4, 5]])
        self.assertEqual(back[1]["text"], "hi there")
        self.assertEqual((back[2]["props"]["point"], back[2]["props"]["include"]), ("box", False))

    def test_vector_exported_to_crtf(self):
        reg = R.parse_ds9("image\n# vector(10,10,5,90)\n")[0]
        line = R.parse_crtf(R.export_crtf([reg]))[0]
        np.testing.assert_allclose(line["points"], [[10, 10], [10, 15]], atol=1e-9)


class TestRegionsPackageCrossCheck(unittest.TestCase):
    """When the astropy ``regions`` package is installed (it is not a backend
    dependency), the corpus agrees with its ds9 reader.

    Compared in each WCS's own sky frame and, for sizes and angles, on grids
    with square pixels: ``regions`` scales every size by one number and does
    not turn angles between sky frames, where ``app.regions`` follows each
    axis on the sky (checked against astropy above). Lines it cannot read
    (``n=``, ``i``/``p`` sizes, vectors, the ``circle point`` form) are left out.
    """

    _KINDS = {"CircleSkyRegion": "circle", "EllipseSkyRegion": "ellipse", "RectangleSkyRegion": "box",
              "PolygonSkyRegion": "polygon", "PointSkyRegion": "point", "LineSkyRegion": "line",
              "TextSkyRegion": "text", "CircleAnnulusSkyRegion": "annulus",
              "EllipseAnnulusSkyRegion": "ellipse_annulus", "RectangleAnnulusSkyRegion": "box_annulus"}

    def test_against_regions_package(self):
        try:
            from regions import Regions
        except ImportError:
            self.skipTest("the regions package is not installed")
        import warnings
        for wname, hdr in _HEADERS.items():
            wcs = _celestial(hdr())
            frame = R.native_frame_name(wcs)
            text, _ = _ds9_sky_corpus(wcs, frame, "deg", None)
            text = "\n".join(ln for ln in text.splitlines()
                             if not any(k in ln for k in ("n=3", "5i)", "5p)", "vector", " point(")))
            ours = R.parse_report(text, "ds9", to_frame="image", wcs=wcs)["regions"]
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                theirs = [(self._KINDS[type(t).__name__], t.to_pixel(wcs)) for t in Regions.parse(text, format="ds9")]
            self.assertEqual([o["shape"] for o in ours], [k for k, _ in theirs], wname)
            for o, (k, p) in zip(ours, theirs):
                if hasattr(p, "center"):
                    np.testing.assert_allclose(o["center"], [p.center.x + 1, p.center.y + 1], atol=1e-6)
                if wname == "cd":
                    continue
                if k == "circle":
                    self.assertAlmostEqual(o["radius"] / p.radius, 1.0, places=6)
                if k in ("ellipse", "box"):
                    a, b = (p.width / 2, p.height / 2) if k == "ellipse" else (p.width, p.height)
                    key = "semi_axes" if k == "ellipse" else "size"
                    np.testing.assert_allclose(o[key], [a, b], rtol=1e-6)
                    self.assertLess(_close_mod(o["angle_deg"], p.angle.to_value("deg"), 180.0), 1e-3)


# ── HTTP end to end ──────────────────────────────────────────────────────────


def _write_fits(header, path, ndim=2):
    from astropy.io import fits
    shape = (4, _N, _N) if ndim == 3 else (_N, _N)
    fits.PrimaryHDU(np.ones(shape, np.float32), header).writeto(path, overwrite=True)


class TestParseRouteEndToEnd(unittest.TestCase):
    """POST /v1/regions/parse on a cube (sky axes rotated), an image with
    LTM/LTV, and an image without a celestial WCS."""

    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls.h = {"X-Visivo-Token": _TEST_TOKEN}
        cls.tmp = tempfile.mkdtemp(prefix="visivo-regions-")
        cube_h = _hdr_cube()
        cls.cube = os.path.join(cls.tmp, "cube.fits")
        _write_fits(cube_h, cls.cube, ndim=3)
        phys_h = _hdr_cd()
        phys_h["LTM1_1"], phys_h["LTM2_2"] = _LTM[0][0], _LTM[1][1]
        phys_h["LTV1"], phys_h["LTV2"] = _LTV
        cls.phys_img = os.path.join(cls.tmp, "phys.fits")
        _write_fits(phys_h, cls.phys_img)
        cls.plain = os.path.join(cls.tmp, "plain.fits")
        _write_fits(__import__("astropy.io.fits", fromlist=["Header"]).Header(), cls.plain)
        cls.ids = {k: cls._open(p) for k, p in (("cube", cls.cube), ("phys", cls.phys_img), ("plain", cls.plain))}

    @classmethod
    def _open(cls, path):
        d = cls.client.post("/v1/datasets/open", json={"path": path}, headers=cls.h).json()
        if d.get("session_id"):
            cls.h["X-Visivo-Session"] = d["session_id"]
        return d["dataset_id"]

    @classmethod
    def tearDownClass(cls):
        import shutil
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _parse(self, text, ds, to_frame="image", fmt="auto"):
        r = self.client.post("/v1/regions/parse", headers=self.h, json={
            "text": text, "format": fmt, "dataset_id": self.ids[ds], "to_frame": to_frame})
        self.assertEqual(r.status_code, 200, r.text)
        return r.json()

    def test_cube_every_shape(self):
        wcs = _celestial(_hdr_cube())
        for frame in ("fk5", "galactic"):
            text, exp = _ds9_sky_corpus(wcs, frame, "colon", None)
            body = self._parse(text, "cube")
            self.assertEqual((body["n_skipped"], body["diagnostics"]), (0, []))
            self.assertEqual(body["n_regions"], len(exp))
            _compare(self, body["regions"], exp, ("route-cube", frame))
        text, exp = _crtf_corpus(wcs, "J2000", "letters")
        body = self._parse(text, "cube")
        _compare(self, body["regions"], exp, ("route-cube", "crtf"))

    def test_physical_uses_the_dataset_ltm_ltv(self):
        wcs = _celestial(_hdr_cd())
        text, exp = _pixel_corpus(R.PhysicalTransform(_LTM, _LTV), wcs)
        body = self._parse(text, "phys")
        self.assertEqual(body["diagnostics"], [])
        _compare(self, body["regions"], exp, "route-physical")
        # Without to_frame the regions keep their frame, but physical is still image.
        body = self._parse("physical\ncircle(10,30,4)\n", "phys", to_frame="")
        np.testing.assert_allclose(body["regions"][0]["center"], [15.25, 10.5])
        self.assertEqual(body["regions"][0]["frame"], "image")

    def test_no_celestial_wcs_reports_sky_regions(self):
        body = self._parse('image\ncircle(3,3,1)\nfk5\ncircle(10,20,3")\nbogus(1)\n', "plain")
        self.assertEqual((body["n_regions"], body["n_skipped"]), (1, 2))
        self.assertEqual([d["line"] for d in body["diagnostics"]], [4, 5])
        self.assertIn("no celestial WCS", body["diagnostics"][0]["reason"])
        self.assertFalse(body["diagnostics_truncated"])
        self.assertTrue(body["valid"])           # one region was read

    def test_diagnostics_are_capped(self):
        body = self._parse("image\n" + "nonsense\n" * 1500, "plain", to_frame="")
        self.assertEqual((body["n_skipped"], len(body["diagnostics"])), (1500, 1000))
        self.assertTrue(body["diagnostics_truncated"])
        self.assertFalse(body["valid"])
        self.assertIn("and 1499 more", body["error"])


def write_corpus(out_dir: str) -> None:
    """Write the corpus files and the images they belong to (for the viewers)."""
    os.makedirs(out_dir, exist_ok=True)
    phys = R.PhysicalTransform(_LTM, _LTV)
    for wname, hdr in _HEADERS.items():
        h = hdr()
        if wname == "cd":
            h["LTM1_1"], h["LTM2_2"] = _LTM[0][0], _LTM[1][1]
            h["LTV1"], h["LTV2"] = _LTV
        _write_fits(h, os.path.join(out_dir, f"{wname}.fits"), ndim=3 if wname == "cube" else 2)
        wcs = _celestial(h)
        for frame in _DS9_FRAMES:
            for style in _DS9_STYLES:
                text, _ = _ds9_sky_corpus(wcs, frame, style, phys)
                with open(os.path.join(out_dir, f"{wname}_{frame}_{style}.reg"), "w") as f:
                    f.write(text)
        text, _ = _pixel_corpus(phys if wname == "cd" else None, wcs)
        with open(os.path.join(out_dir, f"{wname}_image_physical.reg"), "w") as f:
            f.write(text)
        for frame in ("J2000", "GALACTIC"):
            text, _ = _crtf_corpus(wcs, frame, "colon")
            with open(os.path.join(out_dir, f"{wname}_{frame.lower()}.crtf"), "w") as f:
                f.write(text)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        write_corpus(sys.argv[1])
    else:
        unittest.main()
