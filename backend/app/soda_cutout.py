"""Server-side SODA cutout — the byte-level half of "compute next to data".

A SODA client (or SKAVA acting on its behalf) asks for a spatial /
spectral subset of a dataset identified by an ``access_url``. Instead of
shipping the whole cube to the client, the backend — co-located with the
data on the SRC node — opens the FITS in place, slices it with astropy,
and returns only the (much smaller) cutout.

This module is deliberately transport-free: it takes a *local path* and
the parsed SODA parameters and returns FITS bytes. The router
(``/v1/datasets/cutout``) owns access_url → path resolution and the
``VISIVO_DATA_ROOT`` jail (see ``skava_client.access_url_to_local_path``).

Supported SODA parameters:
* ``POS``  — ``CIRCLE <ra> <dec> <radius>`` or
             ``RANGE <ra1> <ra2> <dec1> <dec2>`` (deg). POLYGON is
             reduced to its bounding box.
* ``BAND`` — ``<min> <max>`` in metres (wavelength); converted to the
             dataset's spectral axis unit via the spectral equivalency,
             so FREQ / WAVE / ENER axes all work. On an Obit MFImage
             cube (``CTYPE = 'SPECLNMF'``) it keeps the sub-bands whose
             centre frequency (``FREQnnnn``) falls in the interval.
* ``POL``  — one or more polarization states from the ObsCore vocabulary
             (``I Q U V RR LL RL LR XX YY XY YX POLI POLA``), matched to the
             planes of the STOKES axis through its WCS and the FITS Stokes
             codes. A dataset without a STOKES axis is one total-intensity
             (``I``) plane.

Anything the WCS can't support (e.g. BAND on an image with no spectral
axis) is skipped rather than fatal; the ``applied`` dict in the return
value records exactly which subsets took effect, so callers can tell a
no-op cutout from a real one.
"""
from __future__ import annotations

import io
import logging
import re
import warnings
from dataclasses import dataclass, field
from typing import Any

import astropy.units as u
import numpy as np
from astropy.io import fits
from astropy.nddata import Cutout2D
from astropy.wcs import WCS

from .fits_dataset import MFImageSubbands, mfimage_subbands

logger = logging.getLogger("visivo.soda")

# FITS CTYPE prefixes (4-char codes, case-insensitive) that identify the
# longitude / latitude / spectral world axes.
_LNG_PREFIXES = ("RA--", "GLON", "ELON", "SLON")
_LAT_PREFIXES = ("DEC-", "GLAT", "ELAT", "SLAT")
_SPEC_PREFIXES = ("FREQ", "WAVE", "VELO", "VRAD", "VOPT", "ENER", "WAVN", "AWAV", "ZOPT")
# Obit MFImage's sub-band axis: not a WCS spectral axis (wcslib does not know
# it), its frequencies are per plane in FREQnnnn (fits_dataset.mfimage_subbands).
_SUBBAND_CTYPE = "SPECLNMF"
_C_M_S = 299792458.0

# SODA POL values: the ObsCore polarization vocabulary (pol_states).
POL_STATES = ("I", "Q", "U", "V", "RR", "LL", "RL", "LR", "XX", "YY", "XY", "YX", "POLI", "POLA")
# FITS Stokes codes (FITS 4.0, Table 29). POLI and POLA have no FITS code: a
# file never holds them as STOKES planes, so asking for them selects nothing.
_FITS_STOKES = {1: "I", 2: "Q", 3: "U", 4: "V", -1: "RR", -2: "LL", -3: "RL", -4: "LR",
                -5: "XX", -6: "YY", -7: "XY", -8: "YX"}


class CutoutError(RuntimeError):
    """Raised when a cutout cannot be produced (bad params, unreadable FITS)."""


class CutoutNoOverlap(CutoutError):
    """The request is valid but selects nothing: POS, BAND or TIME misses the data.

    SODA answers this with an empty response (HTTP 204), not an error.
    """


@dataclass
class Circle:
    ra: float
    dec: float
    radius: float


@dataclass
class Range:
    ra1: float
    ra2: float
    dec1: float
    dec2: float


@dataclass
class Band:
    lo: float
    hi: float


@dataclass
class CutoutResult:
    fits_bytes: bytes
    filename: str
    applied: dict[str, Any] = field(default_factory=dict)


# ── Parameter parsing ───────────────────────────────────────────────────────


def parse_pos(value: str | None) -> Circle | Range | None:
    """Parse a SODA POS string. Returns None when value is empty."""
    if not value or not value.strip():
        return None
    parts = value.replace(",", " ").split()
    shape = parts[0].upper()
    try:
        nums = [float(p) for p in parts[1:]]
    except ValueError as exc:
        raise CutoutError(f"POS contains non-numeric values: {value!r}") from exc

    if shape == "CIRCLE":
        if len(nums) != 3:
            raise CutoutError("POS CIRCLE requires '<ra> <dec> <radius>'.")
        return Circle(*nums)
    if shape == "RANGE":
        if len(nums) != 4:
            raise CutoutError("POS RANGE requires '<ra1> <ra2> <dec1> <dec2>'.")
        return Range(*nums)
    if shape == "POLYGON":
        if len(nums) < 6 or len(nums) % 2 != 0:
            raise CutoutError("POS POLYGON requires >= 3 '<ra> <dec>' vertices.")
        ras = nums[0::2]
        decs = nums[1::2]
        # Reduce to bounding box — a conservative superset of the polygon.
        return Range(min(ras), max(ras), min(decs), max(decs))
    raise CutoutError(f"Unsupported POS shape {shape!r}. Use CIRCLE, RANGE or POLYGON.")


def parse_time(value: str | None) -> tuple[float, float] | None:
    """SODA TIME: '<mjd_lo> <mjd_hi>' (either may be -Inf/+Inf)."""
    if not value or not value.strip():
        return None
    parts = value.replace(",", " ").split()
    if len(parts) != 2:
        raise CutoutError("TIME requires '<mjd_lo> <mjd_hi>'.")
    try:
        lo, hi = (float(p) for p in parts)
    except ValueError as exc:
        raise CutoutError(f"TIME contains non-numeric values: {value!r}") from exc
    if lo != lo or hi != hi or lo > hi:
        raise CutoutError("TIME needs lo <= hi.")
    return lo, hi


def _time_coverage(header: fits.Header) -> tuple[float, float] | None:
    """The observation's time span in MJD from its header, when it says so."""
    start = header.get("MJD-OBS")
    if start is None and header.get("DATE-OBS"):
        try:
            from astropy.time import Time

            start = Time(str(header["DATE-OBS"]), format="isot", scale="utc").mjd
        except ValueError:
            start = None
    if start is None:
        return None
    end = header.get("MJD-END")
    if end is None:
        exposure = header.get("EXPTIME") or header.get("EXPOSURE") or header.get("TELAPSE")
        end = float(start) + (float(exposure) / 86400.0 if exposure else 0.0)
    return float(start), float(end)


def parse_band(value: str | None) -> Band | None:
    if not value or not value.strip():
        return None
    parts = value.replace(",", " ").split()
    try:
        nums = [float(p) for p in parts]
    except ValueError as exc:
        raise CutoutError(f"BAND contains non-numeric values: {value!r}") from exc
    if len(nums) != 2 or any(x != x for x in nums):
        raise CutoutError("BAND requires '<min> <max>' (metres).")
    lo, hi = sorted(nums)
    return Band(lo, hi)


def parse_pol(values: str | list[str] | tuple[str, ...] | None) -> tuple[str, ...] | None:
    """SODA POL: the requested states, in request order and without repeats.

    POL is multi-valued (one state per parameter); a value holding several
    states separated by blanks is accepted too, so a list stored as one
    string round-trips. Unknown states are a usage error.
    """
    if values is None:
        return None
    items = [values] if isinstance(values, str) else list(values)
    states: list[str] = []
    for item in items:
        for token in item.replace(",", " ").split():
            state = token.upper()
            if state not in POL_STATES:
                raise CutoutError(f"POL {token!r} is not a polarization state; use {' '.join(POL_STATES)}.")
            if state not in states:
                states.append(state)
    return tuple(states) or None


# ── WCS axis discovery ──────────────────────────────────────────────────────


def _axis_roles(wcs: WCS) -> dict[str, int]:
    """Map 'lng'/'lat'/'spec'/'subband'/'stokes' to their 0-based FITS axis
    index, when present."""
    roles: dict[str, int] = {}
    for i, ctype in enumerate(wcs.wcs.ctype):
        head = (ctype or "")[:4].upper()
        if (ctype or "").strip().upper().startswith(_SUBBAND_CTYPE):
            roles["subband"] = i
        elif head in _LNG_PREFIXES:
            roles["lng"] = i
        elif head in _LAT_PREFIXES:
            roles["lat"] = i
        elif head in _SPEC_PREFIXES:
            roles["spec"] = i
        elif (ctype or "").upper() == "STOKES":
            roles["stokes"] = i
    return roles


def stokes_states(header: fits.Header) -> list[str | None] | None:
    """The polarization state of each plane of the STOKES axis, in pixel order.

    ``None`` when the dataset has no STOKES axis. A plane whose world value is
    not a FITS Stokes code (fractional, or a non-standard code) is ``None``.
    """
    wcs = WCS(header)
    roles = _axis_roles(wcs)
    if "stokes" not in roles:
        return None
    axis = roles["stokes"]
    # An axis beyond NAXIS (WCSAXES > NAXIS) is implicit: one plane, pixel 1.
    n = int(header.get(f"NAXIS{axis + 1}", 1) or 1) if axis < int(header.get("NAXIS", 0) or 0) else 1
    # Evaluate the whole WCS, the other axes at their reference pixel (always
    # inside the projection), and read the STOKES world coordinate.
    pixel = [np.full(n, wcs.wcs.crpix[i] - 1.0) for i in range(wcs.naxis)]
    pixel[axis] = np.arange(n, dtype=float)
    world = wcs.pixel_to_world_values(*pixel)      # an array, not a tuple, for a 1-axis WCS
    codes = np.atleast_1d(world if wcs.naxis == 1 else world[axis])
    states: list[str | None] = []
    for code in codes:
        rounded = int(np.rint(code)) if np.isfinite(code) else None
        exact = rounded is not None and abs(code - rounded) < 1e-6
        states.append(_FITS_STOKES.get(rounded) if exact else None)
    return states


def _fits_axis_to_numpy(fits_axis: int, ndim: int) -> int:
    """FITS axis index (0-based, NAXIS order) → numpy array axis index."""
    return ndim - 1 - fits_axis


# ── Spatial / spectral pixel-range computation ──────────────────────────────


def _spatial_slices(
    data: np.ndarray,
    wcs: WCS,
    roles: dict[str, int],
    pos: Circle | Range,
) -> dict[int, slice]:
    """Compute numpy slices for the two celestial axes covering ``pos``."""
    if "lng" not in roles or "lat" not in roles:
        raise CutoutError("Dataset WCS has no celestial axes; POS cannot apply.")

    cel = wcs.celestial
    lng_np = _fits_axis_to_numpy(roles["lng"], data.ndim)
    lat_np = _fits_axis_to_numpy(roles["lat"], data.ndim)

    # Build a representative 2D celestial plane (index every other axis at 0).
    index: list[Any] = [0] * data.ndim
    index[lng_np] = slice(None)
    index[lat_np] = slice(None)
    plane = data[tuple(index)]
    # ``plane`` axis order follows ascending numpy axis order. Cutout2D wants
    # data as [y, x] with a matching 2D WCS; wcs.celestial is [lat(y), lng(x)].
    # When lat_np > lng_np the plane comes out [lng, lat]; transpose so it is
    # always [lat, lng] to match the celestial WCS pixel convention.
    transposed = lat_np > lng_np
    if transposed:
        plane = plane.T

    if isinstance(pos, Circle):
        from astropy.coordinates import SkyCoord  # local import: optional dep path

        # SODA positions are ICRS whatever the cube's own frame: astropy
        # converts to the cube's frame (Galactic cubes included) on the way.
        center = SkyCoord(pos.ra, pos.dec, unit="deg", frame="icrs")
        # A point outside the projection's valid domain (e.g. the far side
        # of a SIN tangent plane) maps to NaN pixels — treat as no overlap
        # rather than letting astropy raise a bare ValueError.
        px, py = cel.world_to_pixel(center)
        if not (np.isfinite(px) and np.isfinite(py)):
            raise CutoutNoOverlap("POS CIRCLE does not overlap the dataset footprint.")
        size = 2.0 * pos.radius * u.deg
        try:
            cut = Cutout2D(plane, position=center, size=size, wcs=cel, mode="trim")
        except ValueError as exc:  # NoOverlapError / PartialOverlapError / NaN
            raise CutoutNoOverlap(
                "POS CIRCLE does not overlap the dataset footprint.") from exc
    else:  # Range bounding box → pixel corners
        (y0, y1), (x0, x1) = _range_to_pixel_bbox(cel, pos, plane.shape)
        try:
            cut = Cutout2D(
                plane,
                position=((x0 + x1) / 2.0, (y0 + y1) / 2.0),
                size=(abs(y1 - y0) + 1, abs(x1 - x0) + 1),
                wcs=cel,
                mode="trim",
            )
        except ValueError as exc:
            raise CutoutNoOverlap(
                "POS RANGE does not overlap the dataset footprint.") from exc

    (ymin, ymax), (xmin, xmax) = cut.bbox_original  # inclusive bounds
    lat_slice = slice(ymin, ymax + 1)
    lng_slice = slice(xmin, xmax + 1)
    return {lat_np: lat_slice, lng_np: lng_slice}


def _range_to_pixel_bbox(cel: WCS, rng: Range, shape: tuple[int, int]):
    """Convert an ICRS RA/Dec RANGE to inclusive (y0,y1),(x0,x1) pixel bounds.

    The box is tested against the cube's own grid, not by projecting its
    corners: the corners of a large box (a hemisphere, a box wrapping through
    RA 0) can lie outside the projection while the box contains the image,
    and a thin strip can cross the image with both ends outside it.

    The image is covered by cells between at most ~512x512 sample points; a
    cell is kept when the RA and Dec ranges spanned by its corners intersect
    the box. The kept cells, widened by one cell, bound every pixel inside the
    box (a conservative superset, a cell or two larger than exact). ``ra1 > ra2`` is a box that
    wraps through RA 0; an infinite or >= 360 deg RA span is the full circle.
    """
    ny, nx = shape
    step = max(1, int(np.ceil(max(nx, ny) / 512)))
    ys = np.unique(np.append(np.arange(0, ny, step), ny - 1)).astype(float)
    xs = np.unique(np.append(np.arange(0, nx, step), nx - 1)).astype(float)
    # Sample the pixel edges, not centres, so cells cover whole pixels.
    ys = np.unique(np.concatenate(([-0.5], ys[1:-1], [ny - 0.5]))) if ny > 1 else np.array([-0.5, 0.5])
    xs = np.unique(np.concatenate(([-0.5], xs[1:-1], [nx - 0.5]))) if nx > 1 else np.array([-0.5, 0.5])
    gx, gy = np.meshgrid(xs, ys)
    with np.errstate(invalid="ignore"):
        world = cel.pixel_to_world(gx.ravel(), gy.ravel()).icrs
        ra = np.asarray(world.ra.deg, dtype=float).reshape(gx.shape)
        dec = np.asarray(world.dec.deg, dtype=float).reshape(gx.shape)

    def corners(a):
        return np.stack([a[:-1, :-1], a[1:, :-1], a[:-1, 1:], a[1:, 1:]])

    dec_c, ra_c = corners(dec), corners(ra)
    valid = np.isfinite(dec_c).any(axis=0)
    with np.errstate(invalid="ignore"), warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        cell_dec_lo, cell_dec_hi = np.nanmin(dec_c, axis=0), np.nanmax(dec_c, axis=0)
        # RA spanned by each cell, unwrapped around its first finite corner.
        ref = np.where(np.isfinite(ra_c[0]), ra_c[0], np.nanmax(ra_c, axis=0))
        rel = ((ra_c - ref + 180.0) % 360.0) - 180.0
        cell_ra_lo, cell_ra_hi = ref + np.nanmin(rel, axis=0), ref + np.nanmax(rel, axis=0)

    # A celestial pole inside a cell: the corners do not reach its Dec, and
    # around it every RA occurs. Extend that cell to the pole.
    from astropy.coordinates import SkyCoord

    pole_cell = np.zeros_like(valid)
    for pole in (90.0, -90.0):
        with np.errstate(invalid="ignore"):
            px, py = cel.world_to_pixel(SkyCoord(0.0, pole, unit="deg", frame="icrs"))
        px, py = float(px), float(py)
        if np.isfinite(px) and np.isfinite(py) and xs[0] <= px <= xs[-1] and ys[0] <= py <= ys[-1]:
            r = min(max(int(np.searchsorted(ys, py)) - 1, 0), len(ys) - 2)
            c = min(max(int(np.searchsorted(xs, px)) - 1, 0), len(xs) - 2)
            pole_cell[r, c] = True
            valid[r, c] = True
            if pole > 0:
                cell_dec_hi[r, c] = 90.0
            else:
                cell_dec_lo[r, c] = -90.0

    dec_lo, dec_hi = sorted((rng.dec1, rng.dec2))
    keep = valid & (cell_dec_hi >= dec_lo) & (cell_dec_lo <= dec_hi)
    full_ra = not (np.isfinite(rng.ra1) and np.isfinite(rng.ra2)) or abs(rng.ra2 - rng.ra1) >= 360.0
    if not full_ra:
        ra1 = rng.ra1 % 360.0
        ra2 = rng.ra2 % 360.0
        if ra2 < ra1:
            ra2 += 360.0                  # wraps through RA 0
        hit = np.zeros_like(keep)
        for shift in (-360.0, 0.0, 360.0):
            hit |= (cell_ra_hi + shift >= ra1) & (cell_ra_lo + shift <= ra2)
        keep &= hit | pole_cell | (cell_ra_hi - cell_ra_lo >= 180.0)   # around a pole: every RA

    rows, cols = np.nonzero(keep)
    if rows.size == 0:
        raise CutoutNoOverlap("POS RANGE does not overlap the dataset footprint.")
    # One more cell on each side: next to a pole (or any extremum) a cell's
    # Dec can exceed its corners' along an edge, so the neighbours of the kept
    # cells are taken too. Cell (r, c) spans the pixel edges ys[r]..ys[r+1],
    # xs[c]..xs[c+1].
    r0, r1 = max(0, rows.min() - 1), min(len(ys) - 2, rows.max() + 1)
    c0, c1 = max(0, cols.min() - 1), min(len(xs) - 2, cols.max() + 1)
    y0 = int(max(0, np.floor(ys[r0] + 0.5)))
    y1 = int(min(ny - 1, np.ceil(ys[r1 + 1] - 0.5)))
    x0 = int(max(0, np.floor(xs[c0] + 0.5)))
    x1 = int(min(nx - 1, np.ceil(xs[c1 + 1] - 0.5)))
    return (y0, y1), (x0, x1)


def _band_coverage(wcs: WCS, n: int) -> tuple[float, float]:
    """The wavelength interval (metres) covered by the ``n`` spectral pixels,
    edges included."""
    spec = wcs.spectral
    unit = u.Unit(spec.wcs.cunit[0] or "m")
    edges = u.Quantity(spec.pixel_to_world_values(np.array([-0.5, n - 0.5])), unit)
    lo, hi = sorted(edges.to(u.m, equivalencies=u.spectral()).value)
    return float(lo), float(hi)


def _spectral_slice(
    data: np.ndarray,
    wcs: WCS,
    roles: dict[str, int],
    band: Band,
) -> dict[int, slice]:
    """Compute a numpy slice on the spectral axis covering ``band`` (metres)."""
    if "spec" not in roles:
        raise CutoutError("Dataset WCS has no spectral axis; BAND cannot apply.")

    spec = wcs.spectral
    unit = u.Unit(spec.wcs.cunit[0] or "m")
    spec_np = _fits_axis_to_numpy(roles["spec"], data.ndim)
    n = data.shape[spec_np]

    # Clip the band (metres, possibly open-ended: SODA allows -Inf/+Inf) to the
    # cube's own wavelength coverage BEFORE converting: an infinite wavelength
    # would become a zero frequency.
    cover = _band_coverage(wcs, n)
    band_lo, band_hi = max(band.lo, cover[0]), min(band.hi, cover[1])
    if band_lo > band_hi:
        raise CutoutNoOverlap("BAND does not overlap the dataset spectral range.")
    lo = u.Quantity(band_lo, u.m).to(unit, equivalencies=u.spectral()).value
    hi = u.Quantity(band_hi, u.m).to(unit, equivalencies=u.spectral()).value
    lo, hi = sorted((lo, hi))

    p_lo = spec.world_to_pixel(u.Quantity(lo, unit))
    p_hi = spec.world_to_pixel(u.Quantity(hi, unit))
    a, b = sorted((float(p_lo), float(p_hi)))
    i0 = int(max(0, np.floor(a)))
    i1 = int(min(n - 1, np.ceil(b)))
    if i1 < i0:
        raise CutoutNoOverlap("BAND does not overlap the dataset spectral range.")
    return {spec_np: slice(i0, i1 + 1)}


def _subband_axis(header: fits.Header, roles: dict[str, int]) -> tuple[int, MFImageSubbands | None] | None:
    """(0-based FITS axis, its sub-band table) when the spectral axis is a
    sub-band axis, else None.

    A SPECLNMF axis always is one; its table is None when NTERM / NSPEC /
    FREQnnnn do not describe its planes. A WCS spectral axis is one when
    those keywords describe it: a BAND cut that made the axis ``FREQ``
    (``_rewrite_subband_header``) keeps them, and cutting that cutout again
    has to select by sub-band centre and renumber them too.
    """
    for role in ("subband", "spec"):
        if role not in roles:
            continue
        axis = roles[role]
        table = None
        if axis < int(header.get("NAXIS", 0) or 0):
            table = mfimage_subbands(header, int(header.get(f"NAXIS{axis + 1}", 0) or 0))
        return (axis, table) if role == "subband" or table is not None else None
    return None


def _subband_band_m(table: MFImageSubbands) -> tuple[float, float]:
    """Wavelength interval (metres) the sub-bands cover: their edges when the
    header gives them, else their centres (what BAND selects on)."""
    low = table.low_hz if table.low_hz is not None else table.freq_hz
    high = table.high_hz if table.high_hz is not None else table.freq_hz
    return _C_M_S / float(np.max(high)), _C_M_S / float(np.min(low))


def _subband_slice(
    data: np.ndarray,
    axis: int,
    table: MFImageSubbands | None,
    band: Band,
) -> tuple[dict[int, slice], MFImageSubbands, np.ndarray]:
    """A numpy slice over the sub-bands whose centre frequency lies in ``band``,
    with the table and the selected sub-band indices (0-based, into it).

    A sub-band is one measurement at one frequency, so it is in or out by its
    centre: one that the interval only clips at an edge is not returned. The
    NTERM fitted planes before the sub-bands have no frequency and never are.
    """
    if table is None:
        raise CutoutError(
            "The SPECLNMF axis carries no usable per-sub-band frequencies "
            "(NTERM, NSPEC, FREQnnnn); BAND cannot apply.")
    # Metres to Hz without passing through 1/0 or 1/inf: a non-positive upper
    # wavelength selects nothing, an open or non-positive lower one no limit.
    f_lo = _C_M_S / band.hi if band.hi > 0 else np.inf
    f_hi = _C_M_S / band.lo if band.lo > 0 else np.inf
    hit = np.flatnonzero((table.freq_hz >= f_lo) & (table.freq_hz <= f_hi))
    if hit.size == 0:
        raise CutoutNoOverlap("BAND does not contain the centre of any sub-band.")
    if np.any(np.diff(hit) != 1):
        # Only possible when FREQnnnn is not monotonic: one slice cannot
        # hold the selection, and widening it would return planes outside BAND.
        raise CutoutError("The sub-band frequencies are not ordered; BAND cannot apply.")
    spec_np = _fits_axis_to_numpy(axis, data.ndim)
    return {spec_np: slice(table.offset + int(hit[0]), table.offset + int(hit[-1]) + 1)}, table, hit


def _pol_planes(
    data: np.ndarray,
    header: fits.Header,
    roles: dict[str, int],
    pol: tuple[str, ...],
) -> tuple[int, list[int], list[str]] | None:
    """The numpy STOKES axis, the indices of the planes ``pol`` asks for
    (in the file's order) and their states; ``None`` when the whole dataset
    matches and nothing is cut along polarization.

    A dataset with no STOKES axis is total intensity: POL including I keeps
    it whole, any other selection selects nothing. So does a STOKES axis
    that is only implicit (WCSAXES > NAXIS: one plane, no array axis).
    """
    states = stokes_states(header)
    if states is None:
        states = ["I"]
        absent = "the dataset has no STOKES axis (total intensity, I only)"
    else:
        absent = f"the dataset has {' '.join(s for s in states if s) or 'no known state'}"
    picked = [i for i, state in enumerate(states) if state in pol]
    if not picked:
        raise CutoutNoOverlap(f"POL does not select any plane ({absent}).")
    if "stokes" not in roles or roles["stokes"] >= data.ndim:
        return None
    axis = _fits_axis_to_numpy(roles["stokes"], data.ndim)
    return axis, picked, [states[i] or "" for i in picked]


def _even_slice(picked: list[int]) -> slice | None:
    """``picked`` as one slice when its planes are evenly spaced (I and V of
    IQUV: step 3), else ``None``: a linear STOKES axis cannot list them."""
    steps = {b - a for a, b in zip(picked, picked[1:])}
    if len(steps) > 1:
        return None
    return slice(picked[0], picked[-1] + 1, steps.pop() if steps else 1)


# ── Header bookkeeping ──────────────────────────────────────────────────────


def _reslice_axis(header: fits.Header, fits_axis: int, start: int, step: int = 1) -> None:
    """After slicing a numpy axis as ``start::step``, fix the axis' WCS.

    New pixel ``p'`` (1-based) is old pixel ``start + 1 + (p' - 1) * step``,
    so CRPIX becomes ``1 + (CRPIX - 1 - start) / step`` and the pixel scale of
    the axis is multiplied by ``step``: its column of CD when CD is used, its
    column of PC when PC couples it to another axis, otherwise its CDELT.
    """
    n = fits_axis + 1
    key = f"CRPIX{n}"
    crpix = float(header.get(key, 0.0))
    header[key] = crpix - start if step == 1 else 1.0 + (crpix - 1.0 - start) / step
    if step == 1:
        return
    naxis = int(header.get("NAXIS", 0))
    cd = [f"CD{i}_{n}" for i in range(1, naxis + 1) if f"CD{i}_{n}" in header]
    if cd:
        for k in cd:
            header[k] = float(header[k]) * step
        return
    diagonal = f"PC{n}_{n}"
    coupled = [f"PC{i}_{n}" for i in range(1, naxis + 1)
               if i != n and float(header.get(f"PC{i}_{n}", 0.0)) != 0.0]
    if coupled:
        header[diagonal] = float(header.get(diagonal, 1.0)) * step
        for k in coupled:
            header[k] = float(header[k]) * step
        return
    header[f"CDELT{n}"] = float(header.get(f"CDELT{n}", 1.0)) * step


def _rewrite_subband_header(header: fits.Header, fits_axis: int,
                            table: MFImageSubbands, hit: np.ndarray) -> None:
    """Describe the sub-bands ``hit`` kept by a BAND cut, and nothing else.

    The MFImage keywords are renumbered to the planes the cutout holds
    (NTERM = 0, NSPEC = len(hit), FREQ/FREL/FREHnnnn from 0001), so Obit and
    every reader of that convention — this backend's RM synthesis and SODA
    included — find each plane's frequency. The axis itself becomes a plain
    ``FREQ`` axis in Hz whenever the kept centres lie on a linear grid (one
    sub-band, two, or evenly spaced ones), which every FITS reader
    understands and which then agrees exactly with FREQnnnn. Uneven centres
    cannot be a linear axis without misplacing the middle planes, so the axis
    stays ``SPECLNMF`` there, as in the release, with CRVAL/CDELT spanning the
    first to the last centre.
    """
    n = fits_axis + 1
    old_n_spec = int(header.get("NSPEC", 0) or 0)
    for i in range(1, max(old_n_spec, len(table.freq_hz)) + 1):
        for prefix in ("FREQ", "FREL", "FREH"):
            header.remove(f"{prefix}{i:04d}", ignore_missing=True)
    freq = table.freq_hz[hit]
    for j, k in enumerate(hit, start=1):
        header[f"FREQ{j:04d}"] = float(table.freq_hz[k])
        if table.low_hz is not None and table.high_hz is not None:
            header[f"FREL{j:04d}"] = float(table.low_hz[k])
            header[f"FREH{j:04d}"] = float(table.high_hz[k])
    header["NTERM"] = (0, "fitted planes: none in this cutout")
    header["NSPEC"] = (len(hit), "sub-band planes, frequencies in FREQnnnn")

    if len(freq) > 1:
        step = float(freq[-1] - freq[0]) / (len(freq) - 1)
    elif table.low_hz is not None and table.high_hz is not None:
        step = float(table.high_hz[hit[0]] - table.low_hz[hit[0]])   # the sub-band's width
    else:
        step = float(header.get(f"CDELT{n}", 0.0) or 0.0) or 1.0
    linear = freq[0] + step * np.arange(len(freq))
    even = step != 0.0 and float(np.max(np.abs(freq - linear))) <= 1.0   # to 1 Hz
    step = step or 1.0                  # a zero increment is not a valid WCS
    header[f"CTYPE{n}"] = "FREQ" if even else _SUBBAND_CTYPE
    if even:
        header[f"CUNIT{n}"] = "Hz"
    header[f"CRPIX{n}"] = 1.0
    header[f"CRVAL{n}"] = float(freq[0])
    # The increment has to be the whole story: whatever the linear
    # transformation was (CD, or PC with its own scale or couplings), the
    # axis is now uncoupled with this one step.
    keys = list(header.keys())
    uses_cd = any(re.fullmatch(r"CD\d+_\d+", k) for k in keys)
    for k in keys:
        match = re.fullmatch(r"(CD|PC)(\d+)_(\d+)", k)
        if match and n in (int(match.group(2)), int(match.group(3))):
            header.remove(k, ignore_missing=True)
    if uses_cd:
        header[f"CD{n}_{n}"] = step    # a missing diagonal CD element would read as 0
    else:
        header[f"CDELT{n}"] = step     # PCn_n defaults to 1


# ── Public entry point ──────────────────────────────────────────────────────


def make_cutout(
    path: str,
    *,
    pos: str | None = None,
    band: str | None = None,
    time: str | None = None,  # MJD interval, checked against the observation's time
    pol: str | list[str] | tuple[str, ...] | None = None,
    filename_hint: str = "cutout",
) -> CutoutResult:
    """Open a FITS file and return a spatial/spectral/polarization subset as FITS bytes.

    Raises :class:`CutoutError` on invalid parameters and
    :class:`CutoutNoOverlap` when a valid request selects nothing. Without
    any constraint the first image HDU is returned whole (a valid, if
    trivial, SODA response).
    """
    parsed_pos = parse_pos(pos)
    parsed_band = parse_band(band)
    parsed_time = parse_time(time)
    parsed_pol = parse_pol(pol)
    applied: dict[str, Any] = {"pos": False, "band": False, "time": False, "pol": False}

    try:
        from .volume_source import open_fits  # noqa: PLC0415
        hdul = open_fits(path, memmap=True)   # a VBT field too (issue #83)
    except Exception as exc:  # noqa: BLE001 — surface any astropy/io failure
        raise CutoutError(f"Could not open FITS at {path!r}: {exc}") from exc

    try:
        hdu = _first_image_hdu(hdul)
        if hdu.data is None:
            raise CutoutError("Primary image HDU has no pixel data.")
        data = hdu.data
        header = hdu.header.copy()
        wcs = WCS(header)
        roles = _axis_roles(wcs)

        # A cube has no time axis to cut, but its observation has a time: a
        # TIME interval that misses it selects nothing. Without a recorded
        # time the constraint cannot be evaluated and is not applied.
        if parsed_time is not None:
            coverage = _time_coverage(header)
            if coverage is not None:
                if parsed_time[1] < coverage[0] or parsed_time[0] > coverage[1]:
                    raise CutoutNoOverlap("TIME does not overlap the observation.")
                applied["time"] = True

        slices: dict[int, slice] = {}
        if parsed_pos is not None:
            slices.update(_spatial_slices(data, wcs, roles, parsed_pos))
            applied["pos"] = True
        subbands: tuple[int, MFImageSubbands, np.ndarray] | None = None
        if parsed_band is not None:
            subband_axis = _subband_axis(header, roles)
            if subband_axis is not None:
                cut, table, hit = _subband_slice(data, *subband_axis, parsed_band)
                slices.update(cut)
                subbands = (subband_axis[0], table, hit)
                applied["subbands"] = [int(k) + 1 for k in hit]    # FREQnnnn numbers
            else:
                slices.update(_spectral_slice(data, wcs, roles, parsed_band))
            applied["band"] = True
        # Planes on the STOKES axis that one slice cannot hold: one HDU each.
        split: tuple[int, list[int], list[str]] | None = None
        if parsed_pol is not None:
            planes = _pol_planes(data, header, roles, parsed_pol)
            if planes is not None:
                even = _even_slice(planes[1])
                if even is not None:
                    slices[planes[0]] = even
                else:
                    split = planes
            applied["pol"] = True

        for np_axis, sl in slices.items():
            start, step = sl.start or 0, sl.step or 1
            if start or step != 1:
                _reslice_axis(header, _fits_axis_to_numpy(np_axis, data.ndim), start, step)
        if subbands is not None:
            _rewrite_subband_header(header, *subbands)
        if split is None:
            selector = tuple(slices.get(ax, slice(None)) for ax in range(data.ndim))
            sub = np.asarray(data[selector])
            out_hdus = [fits.PrimaryHDU(data=sub, header=header)]
            applied["out_shape"] = list(sub.shape)
        else:
            # SODA returns the requested states only (one result, "some or all
            # of these states"): I, Q and V of IQUV cannot share a linear axis
            # without U, so each state is its own image HDU, the primary first.
            axis, picked, states = split
            out_hdus = []
            for index, state in zip(picked, states):
                selector = tuple(slice(index, index + 1) if ax == axis else slices.get(ax, slice(None))
                                 for ax in range(data.ndim))
                plane_header = header.copy()
                _reslice_axis(plane_header, _fits_axis_to_numpy(axis, data.ndim), index)
                plane_header["EXTNAME"] = f"STOKES_{state}"
                kind = fits.PrimaryHDU if not out_hdus else fits.ImageHDU
                out_hdus.append(kind(data=np.asarray(data[selector]), header=plane_header))
            applied["out_shape"] = list(out_hdus[0].data.shape)
            applied["hdus"] = len(out_hdus)
    finally:
        hdul.close()

    buf = io.BytesIO()
    fits.HDUList(out_hdus).writeto(buf, overwrite=True)
    logger.info(
        "[cutout] path=%s pos=%s band=%s pol=%s applied=%s shape=%s",
        path, pos, band, parsed_pol, applied, applied.get("out_shape"),
    )
    return CutoutResult(
        fits_bytes=buf.getvalue(),
        filename=f"{filename_hint}_cutout.fits",
        applied=applied,
    )


@dataclass
class CutoutAxes:
    """What a cutout of one dataset can constrain, for its service descriptor."""

    celestial: bool
    band_m: tuple[float, float] | None    # wavelength coverage, metres
    time_mjd: tuple[float, float] | None  # observation time, MJD
    pol: list[str]                         # states present, in plane order


def describe(path: str) -> CutoutAxes:
    """The constraints ``make_cutout`` can apply to the dataset at ``path``.

    Reads the header only. POL lists the states the STOKES axis holds (the
    planes with a FITS Stokes code), or ``I`` for a dataset without one,
    which is what ``make_cutout`` accepts as a non-empty selection.
    """
    try:
        from .volume_source import open_fits  # noqa: PLC0415
        hdul = open_fits(path, memmap=True)
    except Exception as exc:  # noqa: BLE001 — surface any astropy/io failure
        raise CutoutError(f"Could not open FITS at {path!r}: {exc}") from exc
    try:
        header = _first_image_header(hdul)
    finally:
        hdul.close()
    if header is None:
        raise CutoutError("The dataset holds no image to cut.")
    wcs = WCS(header)
    roles = _axis_roles(wcs)
    band = None
    subband_axis = _subband_axis(header, roles)
    if subband_axis is not None:
        band = _subband_band_m(subband_axis[1]) if subband_axis[1] is not None else None
    elif "spec" in roles:
        n = int(header.get(f"NAXIS{roles['spec'] + 1}", 1) or 1)
        try:
            band = _band_coverage(wcs, n)
        except (u.UnitsError, ValueError):   # a spectral axis astropy cannot convert
            band = None
        if band is not None and not all(np.isfinite(band)):
            band = None
    states = stokes_states(header)
    pol = ["I"] if states is None else list(dict.fromkeys(s for s in states if s))
    return CutoutAxes(celestial="lng" in roles and "lat" in roles, band_m=band,
                      time_mjd=_time_coverage(header), pol=pol)


def _first_image_header(hdul: fits.HDUList) -> fits.Header | None:
    """A copy of the header of the HDU ``_first_image_hdu`` picks, decided
    from the headers alone: touching ``.data`` would read (or decompress)
    the whole image just to describe it. ``None`` when no HDU holds an image."""
    for hdu in hdul:
        header = hdu.header
        naxis = int(header.get("NAXIS", 0) or 0)
        if (getattr(hdu, "is_image", False) and naxis
                and all(int(header.get(f"NAXIS{i}", 0) or 0) > 0 for i in range(1, naxis + 1))):
            return header.copy()
    return None


def _first_image_hdu(hdul: fits.HDUList):
    for hdu in hdul:
        if getattr(hdu, "is_image", False) and hdu.data is not None:
            return hdu
    return hdul[0]
