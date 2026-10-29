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
             so FREQ / WAVE / ENER axes all work.

Anything the WCS can't support (e.g. BAND on an image with no spectral
axis) is skipped rather than fatal; the ``applied`` dict in the return
value records exactly which subsets took effect, so callers can tell a
no-op cutout from a real one.
"""
from __future__ import annotations

import io
import logging
import warnings
from dataclasses import dataclass, field
from typing import Any

import astropy.units as u
import numpy as np
from astropy.io import fits
from astropy.nddata import Cutout2D
from astropy.wcs import WCS

logger = logging.getLogger("visivo.soda")

# FITS CTYPE prefixes (4-char codes, case-insensitive) that identify the
# longitude / latitude / spectral world axes.
_LNG_PREFIXES = ("RA--", "GLON", "ELON", "SLON")
_LAT_PREFIXES = ("DEC-", "GLAT", "ELAT", "SLAT")
_SPEC_PREFIXES = ("FREQ", "WAVE", "VELO", "VRAD", "VOPT", "ENER", "WAVN", "AWAV", "ZOPT")


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


# ── WCS axis discovery ──────────────────────────────────────────────────────


def _axis_roles(wcs: WCS) -> dict[str, int]:
    """Map 'lng'/'lat'/'spec' to their 0-based FITS axis index, when present."""
    roles: dict[str, int] = {}
    for i, ctype in enumerate(wcs.wcs.ctype):
        head = (ctype or "")[:4].upper()
        if head in _LNG_PREFIXES:
            roles["lng"] = i
        elif head in _LAT_PREFIXES:
            roles["lat"] = i
        elif head in _SPEC_PREFIXES:
            roles["spec"] = i
    return roles


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
    edges = u.Quantity(spec.pixel_to_world_values(np.array([-0.5, n - 0.5])), unit)
    cover = sorted(edges.to(u.m, equivalencies=u.spectral()).value)
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


# ── Header bookkeeping ──────────────────────────────────────────────────────


def _shift_crpix(header: fits.Header, fits_axis: int, start_pixel: int) -> None:
    """After slicing a numpy axis at ``start_pixel``, fix the FITS CRPIX.

    CRPIX is 1-based; trimming ``start_pixel`` leading pixels moves the
    reference pixel by the same amount.
    """
    key = f"CRPIX{fits_axis + 1}"
    if key in header:
        header[key] = float(header[key]) - start_pixel


# ── Public entry point ──────────────────────────────────────────────────────


def make_cutout(
    path: str,
    *,
    pos: str | None = None,
    band: str | None = None,
    time: str | None = None,  # MJD interval, checked against the observation's time
    filename_hint: str = "cutout",
) -> CutoutResult:
    """Open a FITS file and return a spatial/spectral subset as FITS bytes.

    Raises :class:`CutoutError` on invalid parameters or non-overlapping
    regions. When neither POS nor BAND is supplied the full file is
    returned (a valid, if trivial, SODA response).
    """
    parsed_pos = parse_pos(pos)
    parsed_band = parse_band(band)
    parsed_time = parse_time(time)
    applied: dict[str, Any] = {"pos": False, "band": False, "time": False}

    try:
        hdul = fits.open(path, memmap=True)
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
        if parsed_band is not None:
            slices.update(_spectral_slice(data, wcs, roles, parsed_band))
            applied["band"] = True

        if slices:
            selector = tuple(slices.get(ax, slice(None)) for ax in range(data.ndim))
            sub = np.asarray(data[selector])
            for np_axis, sl in slices.items():
                start = sl.start or 0
                if start:
                    _shift_crpix(header, _fits_axis_to_numpy(np_axis, data.ndim), start)
            out_hdu = fits.PrimaryHDU(data=sub, header=header)
            applied["out_shape"] = list(sub.shape)
        else:
            out_hdu = fits.PrimaryHDU(data=np.asarray(data), header=header)
            applied["out_shape"] = list(np.asarray(data).shape)
    finally:
        hdul.close()

    buf = io.BytesIO()
    fits.HDUList([out_hdu]).writeto(buf, overwrite=True)
    logger.info(
        "[cutout] path=%s pos=%s band=%s applied=%s shape=%s",
        path, pos, band, applied, applied.get("out_shape"),
    )
    return CutoutResult(
        fits_bytes=buf.getvalue(),
        filename=f"{filename_hint}_cutout.fits",
        applied=applied,
    )


def _first_image_hdu(hdul: fits.HDUList):
    for hdu in hdul:
        if getattr(hdu, "is_image", False) and hdu.data is not None:
            return hdu
    return hdul[0]
