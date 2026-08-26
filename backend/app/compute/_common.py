"""Shared infrastructure for the compute worker package.

Imports, logger, the scientific-FITS dataset facade, and the small leaf
helpers (payload encoding + value ranges) used across more than one domain
module.  Domain modules pull what they need from here.
"""

from __future__ import annotations

import base64
import zlib
from typing import Any  # noqa: F401  (re-exported for domain modules)

import numpy as np

from ..fits_dataset import ScientificFitsDataset  # noqa: F401

import logging

logger = logging.getLogger("visivo.compute")


def _qt_zlib_compress(raw: bytes, level: int = 6) -> bytes:
    """Prepend the uncompressed length as a 4-byte big-endian uint (Qt convention).

    ``level`` controls zlib's speed/ratio trade-off:
      * 6 (default) — ~50 MB/s compress, ~60-70% ratio. Fine for small
        payloads (slices, moment maps).
      * 1 — ~150-200 MB/s compress, ~40-50% ratio. Use for multi-GB
        payloads (subvolume, preview) where the per-MB compression time
        dominates the bandwidth saved at higher levels.
    """
    return len(raw).to_bytes(4, byteorder="big", signed=False) + zlib.compress(raw, level)

def _b64f32(array: np.ndarray) -> str:
    return base64.b64encode(np.ascontiguousarray(array, dtype=np.float32).tobytes()).decode("ascii")

def _b64i32(array: np.ndarray) -> str:
    return base64.b64encode(np.ascontiguousarray(array, dtype=np.int32).tobytes()).decode("ascii")

def _b64f32_compressed(array: np.ndarray, level: int = 6) -> tuple[str, str]:
    raw = np.ascontiguousarray(array, dtype=np.float32).tobytes()
    return "qt-zlib", base64.b64encode(_qt_zlib_compress(raw, level)).decode("ascii")

def _b64i32_compressed(array: np.ndarray, level: int = 6) -> tuple[str, str]:
    raw = np.ascontiguousarray(array, dtype=np.int32).tobytes()
    return "qt-zlib", base64.b64encode(_qt_zlib_compress(raw, level)).decode("ascii")

def _finite_range(array: np.ndarray) -> tuple[float, float]:
    finite = np.isfinite(array)
    if not finite.any():
        return 0.0, 0.0
    return float(np.nanmin(array)), float(np.nanmax(array))

def beam_area_pixels(
    bmaj_deg: float | None, bmin_deg: float | None, pix_arcsec: float | None
) -> float | None:
    """Number of pixels covered by one synthesized beam (a Gaussian beam).

    ``beam_area = (pi / (4 ln2)) * BMAJ * BMIN`` (the Gaussian beam solid angle)
    divided by the pixel area, all in the same angular units. BMAJ/BMIN are in
    degrees (as read from the FITS header); ``pix_arcsec`` is the pixel scale.

    Returns ``None`` when the beam or pixel scale is unavailable, so callers can
    honestly report "no beam correction possible" rather than a wrong number.
    This is the single source of truth for the conversion that turns a summed
    Jy/beam quantity into Jy (divide by this).
    """
    if not bmaj_deg or not bmin_deg or not pix_arcsec or pix_arcsec <= 0:
        return None
    pix_deg = pix_arcsec / 3600.0
    area = (np.pi / (4.0 * np.log(2.0))) * (bmaj_deg * bmin_deg) / (pix_deg * pix_deg)
    return float(area) if area > 0 else None


def covariance_propagate(pcov, indices, gradients) -> float | None:
    """1-sigma error of a derived quantity via full-covariance propagation.

    ``sqrt(gᵀ · C · g)`` restricted to the parameters in ``indices`` (so an inf
    covariance entry for an unrelated parameter cannot poison the result). This
    keeps the amplitude–width covariance that a plain sum-of-independent-terms
    ignores — material for Gaussian fits. Returns None when the relevant
    sub-covariance is not finite (e.g. an unconstrained fit).
    """
    idx = list(indices)
    g = np.asarray(gradients, dtype=np.float64)
    try:
        sub = np.asarray(pcov, dtype=np.float64)[np.ix_(idx, idx)]
    except Exception:  # noqa: BLE001
        return None
    if not np.all(np.isfinite(sub)):
        return None
    var = float(g @ sub @ g)
    return float(np.sqrt(var)) if (np.isfinite(var) and var >= 0.0) else None


_C_KMS = 299792.458  # speed of light, km/s


def frequency_width_to_kms(
    axis_type: str | None, axis_unit: str | None, rest_freq_hz: float | None
) -> tuple[float, str]:
    """Factor to convert a spectral *width* in the native frequency axis unit to
    km/s, plus the resulting unit label.

    A line width |Δν| maps to |Δv| = c·|Δν|/ν₀ (convention-independent to first
    order around the line, where ν ≈ ν₀). Returns ``(factor, "km/s")`` when the
    axis is a frequency axis and ``rest_freq_hz`` is a positive rest frequency;
    otherwise ``(1.0, axis_unit)`` so callers leave the width in native units and
    label it honestly rather than silently reporting Hz where km/s is expected.
    """
    at = (axis_type or "").upper()
    au = (axis_unit or "").strip().lower()
    # CTYPE takes precedence: an axis that is already a velocity (VELO/VRAD/VOPT/
    # FELO) or carries a velocity unit is never reinterpreted as frequency.
    if at.startswith(("VELO", "VRAD", "VOPT", "FELO")) or au in ("m/s", "km/s"):
        return 1.0, (axis_unit or "")
    freq_units = {"hz": 1.0, "khz": 1.0e3, "mhz": 1.0e6, "ghz": 1.0e9}
    # Convert only a genuine frequency axis with a frequency-convertible unit —
    # so CTYPE=FREQ but CUNIT=m/s (mislabelled) falls through untouched.
    is_freq = at.startswith("FREQ") or au in freq_units
    if not is_freq or au not in freq_units or not rest_freq_hz or float(rest_freq_hz) <= 0:
        return 1.0, (axis_unit or "")
    return _C_KMS * freq_units[au] / float(rest_freq_hz), "km/s"


def is_per_beam_bunit(bunit: str | None) -> bool:
    """True when the brightness unit is per-beam (Jy/beam), so a summed/integrated
    quantity must be divided by the beam area (in pixels) to become a flux in Jy."""
    return bool(bunit) and "jy/beam" in str(bunit).lower()


def primary_beam_correct(
    image: np.ndarray, pb: np.ndarray, pb_min: float = 0.1
) -> np.ndarray:
    """Divide an image by the primary-beam response to recover true sky flux (R-05).

    Interferometer single-pointing/mosaic maps are attenuated by the primary
    beam away from field centre; the flux-correct image is ``image / pb``. Pixels
    where the PB response is below ``pb_min`` (noisy field edge) are blanked to
    NaN rather than amplified into spurious bright values. ``pb`` must be
    broadcastable to ``image``.
    """
    pb = np.asarray(pb, dtype=np.float64)
    with np.errstate(divide="ignore", invalid="ignore"):
        out = np.asarray(image, dtype=np.float64) / pb
    bad = ~np.isfinite(pb) | (pb < float(pb_min))
    # pb may be lower-rank than image (e.g. a 2-D PB against a 3-D cube); expand
    # the mask to the output shape before indexing.
    out[np.broadcast_to(bad, out.shape)] = np.nan
    return out


def _integrated_spacing(coords: np.ndarray) -> np.ndarray:
    if coords.size == 0:
        return np.zeros(0, dtype=np.float64)
    if coords.size == 1:
        return np.ones(1, dtype=np.float64)
    edges = np.empty(coords.size + 1, dtype=np.float64)
    edges[1:-1] = 0.5 * (coords[:-1] + coords[1:])
    edges[0] = coords[0] - 0.5 * (coords[1] - coords[0])
    edges[-1] = coords[-1] + 0.5 * (coords[-1] - coords[-2])
    return np.abs(np.diff(edges))
