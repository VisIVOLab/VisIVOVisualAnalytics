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


def parse_band(value: str | None) -> Band | None:
    if not value or not value.strip():
        return None
    parts = value.replace(",", " ").split()
    try:
        nums = [float(p) for p in parts]
    except ValueError as exc:
        raise CutoutError(f"BAND contains non-numeric values: {value!r}") from exc
    if len(nums) != 2:
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

        center = SkyCoord(pos.ra, pos.dec, unit="deg", frame=_frame_for(cel))
        # A point outside the projection's valid domain (e.g. the far side
        # of a SIN tangent plane) maps to NaN pixels — treat as no overlap
        # rather than letting astropy raise a bare ValueError.
        px, py = cel.world_to_pixel(center)
        if not (np.isfinite(px) and np.isfinite(py)):
            raise CutoutError("POS CIRCLE does not overlap the dataset footprint.")
        size = 2.0 * pos.radius * u.deg
        try:
            cut = Cutout2D(plane, position=center, size=size, wcs=cel, mode="trim")
        except ValueError as exc:  # NoOverlapError / PartialOverlapError / NaN
            raise CutoutError(
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
            raise CutoutError(
                "POS RANGE does not overlap the dataset footprint.") from exc

    (ymin, ymax), (xmin, xmax) = cut.bbox_original  # inclusive bounds
    lat_slice = slice(ymin, ymax + 1)
    lng_slice = slice(xmin, xmax + 1)
    return {lat_np: lat_slice, lng_np: lng_slice}


def _frame_for(cel: WCS) -> str:
    head = (cel.wcs.ctype[0] or "")[:4].upper()
    if head.startswith("GLON"):
        return "galactic"
    return "icrs"


def _range_to_pixel_bbox(cel: WCS, rng: Range, shape: tuple[int, int]):
    """Convert a RA/Dec RANGE to inclusive (y0,y1),(x0,x1) pixel bounds."""
    from astropy.coordinates import SkyCoord

    frame = _frame_for(cel)
    corners = SkyCoord(
        [rng.ra1, rng.ra1, rng.ra2, rng.ra2],
        [rng.dec1, rng.dec2, rng.dec1, rng.dec2],
        unit="deg",
        frame=frame,
    )
    xs, ys = cel.world_to_pixel(corners)
    ny, nx = shape
    x0 = int(max(0, np.floor(np.nanmin(xs))))
    x1 = int(min(nx - 1, np.ceil(np.nanmax(xs))))
    y0 = int(max(0, np.floor(np.nanmin(ys))))
    y1 = int(min(ny - 1, np.ceil(np.nanmax(ys))))
    if x1 < x0 or y1 < y0:
        raise CutoutError("POS RANGE does not overlap the dataset footprint.")
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
    lo = u.Quantity(band.lo, u.m).to(unit, equivalencies=u.spectral()).value
    hi = u.Quantity(band.hi, u.m).to(unit, equivalencies=u.spectral()).value
    lo, hi = sorted((lo, hi))

    spec_np = _fits_axis_to_numpy(roles["spec"], data.ndim)
    n = data.shape[spec_np]
    p_lo = spec.world_to_pixel(u.Quantity(lo, unit))
    p_hi = spec.world_to_pixel(u.Quantity(hi, unit))
    a, b = sorted((float(p_lo), float(p_hi)))
    i0 = int(max(0, np.floor(a)))
    i1 = int(min(n - 1, np.ceil(b)))
    if i1 < i0:
        raise CutoutError("BAND does not overlap the dataset spectral range.")
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
    time: str | None = None,  # accepted for API symmetry; not yet a data axis
    filename_hint: str = "cutout",
) -> CutoutResult:
    """Open a FITS file and return a spatial/spectral subset as FITS bytes.

    Raises :class:`CutoutError` on invalid parameters or non-overlapping
    regions. When neither POS nor BAND is supplied the full file is
    returned (a valid, if trivial, SODA response).
    """
    parsed_pos = parse_pos(pos)
    parsed_band = parse_band(band)
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
