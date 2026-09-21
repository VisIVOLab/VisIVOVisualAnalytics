"""Smooth and regrid a FITS file — the legacy's *Filter FITS*, done properly.

The legacy dialog (``FilterFITSDialog`` + ``imutils.cpp``) offered two actions on
the file in front of the user: a Gaussian filter given a FWHM in pixels, and an
integer shrink. It did them with libwcs' ``FiltFITS`` / ``ShrinkFITSImage``,
which know nothing about blank pixels or about what the numbers mean. Three
things follow from that, and this module exists to fix them:

* **NaN was contagious.** A single blank pixel — the edge of a Hi-GAL tile, a
  masked artefact — spread over the whole kernel footprint, eating a 4σ-wide
  hole around itself. Here the convolution is normalised by the smoothed
  coverage, which is the standard renormalisation: blanks neither leak into
  their neighbours nor drag them down, and they stay blank in the output.
* **The beam was left behind.** Smoothing a map changes its resolution, so a
  ``BMAJ``/``BMIN`` copied through unchanged makes every later flux and column
  density wrong. The beams add in quadrature, and that is what is written.
* **Averaging is not always right.** Block-averaging preserves surface
  brightness (Jy/beam, MJy/sr); for a per-pixel unit (Jy/pixel) the flux is in
  the pixel, so the block has to be *summed* or the map loses flux by the
  square of the factor. The unit decides, and the choice is recorded.

Everything is written into the workspace as a new file: the input is never
touched, which is also the legacy's behaviour and worth keeping.
"""
from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np
from astropy.io import fits

from ._common import logger

#: FWHM = sigma · (2 √(2 ln 2)). The legacy used 2.355, which is this to four
#: figures; the exact factor costs nothing.
_FWHM_PER_SIGMA = 2.0 * np.sqrt(2.0 * np.log(2.0))

#: Kernel radius, in sigmas. scipy's own default, and wider than the legacy's
#: 3σ box: at 3σ the truncated Gaussian still carries ~1% of its weight.
_TRUNCATE_SIGMAS = 4.0

#: Units whose value belongs to the pixel rather than to the sky area, so a
#: block of them must be summed rather than averaged.
_PER_PIXEL_UNITS = ("/pix", "/pixel", "per pixel")


def fwhm_to_sigma(fwhm: float) -> float:
    return float(fwhm) / _FWHM_PER_SIGMA


def combine_for_unit(bunit: str) -> str:
    """"sum" or "mean" — how a block of pixels in this unit must be combined."""
    unit = (bunit or "").strip().lower().replace(" ", "")
    if any(marker.replace(" ", "") in unit for marker in _PER_PIXEL_UNITS):
        return "sum"
    return "mean"


def smooth_plane(plane: np.ndarray, sigma_pixels: float) -> np.ndarray:
    """Gaussian-smooth one plane, leaving blank pixels blank.

    The renormalisation is ``(G * (data·m)) / (G * m)`` with ``m`` the finite
    mask: each output pixel is the weighted mean of the finite pixels under the
    kernel, so a hole neither spreads nor pulls its neighbours towards zero.
    """
    from scipy.ndimage import gaussian_filter  # noqa: PLC0415

    if sigma_pixels <= 0:
        return np.array(plane, dtype=np.float64, copy=True)

    data = np.asarray(plane, dtype=np.float64)
    finite = np.isfinite(data)
    if not finite.any():
        return np.full(data.shape, np.nan)

    filled = np.where(finite, data, 0.0)
    weight = finite.astype(np.float64)
    kwargs = {"sigma": float(sigma_pixels), "mode": "constant", "cval": 0.0,
              "truncate": _TRUNCATE_SIGMAS}
    numerator = gaussian_filter(filled, **kwargs)
    denominator = gaussian_filter(weight, **kwargs)

    out = np.full(data.shape, np.nan)
    # Where no finite pixel had any weight there is nothing to say; dividing
    # would produce 0/0. Blank in, blank out.
    usable = denominator > 1e-8
    out[usable] = numerator[usable] / denominator[usable]
    out[~finite] = np.nan
    return out


def regrid_plane(plane: np.ndarray, factor: int, combine: str = "mean") -> np.ndarray:
    """Shrink one plane by an integer factor, block by block.

    Pixels left over at the top/right edge are dropped — a partial block would
    be an average over fewer pixels at a different place than the WCS says.
    """
    factor = int(factor)
    if factor <= 1:
        return np.array(plane, dtype=np.float64, copy=True)
    data = np.asarray(plane, dtype=np.float64)
    ny, nx = data.shape
    ny_trim, nx_trim = (ny // factor) * factor, (nx // factor) * factor
    if ny_trim == 0 or nx_trim == 0:
        raise ValueError(
            f"A factor of {factor} leaves nothing of a {nx}×{ny} image.")
    blocks = data[:ny_trim, :nx_trim].reshape(ny_trim // factor, factor,
                                              nx_trim // factor, factor)
    finite = np.isfinite(blocks)
    count = finite.sum(axis=(1, 3))
    total = np.where(finite, blocks, 0.0).sum(axis=(1, 3))
    safe = np.maximum(count, 1)
    if combine == "sum":
        # A block that is entirely blank stays blank; a partly blank one is
        # scaled up to the flux the whole block would carry, which is the only
        # estimate available and better than silently losing it.
        out = total * (factor * factor) / safe
    else:
        out = total / safe
    return np.where(count > 0, out, np.nan)


#: Header keywords that carry one value per axis, as "<KEY><n>".
_PER_AXIS_KEYS = ("CTYPE", "CRPIX", "CRVAL", "CDELT", "CUNIT", "CROTA", "CNAME",
                  "CRDER", "CSYER")

#: Projection parameters, which are "<KEY><axis>_<m>" — PV2_1 is the first
#: parameter of axis 2, not of axis 21. They have to move with their axis or the
#: projection changes: a SIN with PV2_1 left behind is a different SIN.
_PER_AXIS_INDEXED_KEYS = ("PV", "PS")


def drop_axes(header: fits.Header, kept_axes: list[int]) -> None:
    """Keep only ``kept_axes`` (1-based FITS numbers) and renumber them, in place.

    A degenerate axis — the length-1 Stokes axis radio images carry — is
    squeezed out of the data, and its WCS keywords have to go with it. Leaving
    ``CTYPE3``/``WCSAXES=4`` on a two-dimensional image describes a four-axis
    WCS over a plane, which astropy then builds and every consumer has to argue
    with.
    """
    # WCSAXES and NAXIS can disagree — a header may describe fewer WCS axes than
    # the data has — so the range to renumber is whichever is larger, and the
    # kept axes themselves can reach beyond both.
    total = max(int(header.get("WCSAXES", 0) or 0), int(header.get("NAXIS", 0) or 0),
                max(kept_axes) if kept_axes else 0)
    if not kept_axes or sorted(kept_axes) == list(range(1, total + 1)):
        # Nothing was dropped and nothing moves: leave the header alone.
        return

    renumber = {old: new for new, old in enumerate(sorted(kept_axes), start=1)}

    # Read everything first: renumbering in place would overwrite an axis that
    # has not been moved yet (3 → 1 while 1 is still there).
    saved: dict[str, Any] = {}
    for key in _PER_AXIS_KEYS:
        for axis in range(1, total + 1):
            name = f"{key}{axis}"
            if name in header:
                saved[name] = (header[name], header.comments[name])
                del header[name]
    matrices: dict[str, Any] = {}
    for kind in ("CD", "PC"):
        for i in range(1, total + 1):
            for j in range(1, total + 1):
                name = f"{kind}{i}_{j}"
                if name in header:
                    matrices[name] = header[name]
                    del header[name]
    # PVi_m / PSi_m: read them by pattern, since m is unbounded.
    import re as _re  # noqa: PLC0415
    projection: dict[tuple[str, int, str], Any] = {}
    for name in list(header.keys()):
        match = _re.match(r"^(PV|PS)(\d+)_(\d+)$", str(name))
        if match:
            projection[(match.group(1), int(match.group(2)), match.group(3))] = header[name]
            del header[name]

    for key in _PER_AXIS_KEYS:
        for old, new in renumber.items():
            stored = saved.get(f"{key}{old}")
            if stored is not None:
                header[f"{key}{new}"] = (stored[0], stored[1])
    for kind in ("CD", "PC"):
        for old_i, new_i in renumber.items():
            for old_j, new_j in renumber.items():
                value = matrices.get(f"{kind}{old_i}_{old_j}")
                if value is not None:
                    header[f"{kind}{new_i}_{new_j}"] = value
    for (kind, old_axis, parameter), value in projection.items():
        new_axis = renumber.get(old_axis)
        if new_axis is not None:
            header[f"{kind}{new_axis}_{parameter}"] = value

    if "WCSAXES" in header or total != len(kept_axes):
        header["WCSAXES"] = len(kept_axes)


def regrid_header(header: fits.Header, factor: int) -> None:
    """Move the WCS onto the shrunken grid, in place.

    A block-averaged pixel sits at the centre of the block it came from, so the
    reference pixel moves as ``(CRPIX − 0.5)/f + 0.5`` — the half-pixel terms
    are the difference between FITS' 1-based pixel centres and the block edges,
    and dropping them puts the map half a block off, which at a factor of 8 is
    four pixels of pointing error.
    """
    factor = int(factor)
    if factor <= 1:
        return
    for axis in (1, 2):
        crpix = header.get(f"CRPIX{axis}")
        if crpix is not None:
            header[f"CRPIX{axis}"] = (float(crpix) - 0.5) / factor + 0.5
        cdelt = header.get(f"CDELT{axis}")
        if cdelt is not None:
            header[f"CDELT{axis}"] = float(cdelt) * factor
    # A CD matrix carries the scale itself, so it is scaled too — only its
    # celestial 2×2 block, since a spectral third axis is untouched by a spatial
    # regrid. PC is NOT touched: it is the dimensionless rotation/skew that
    # multiplies CDELT, so scaling both would apply the factor twice.
    for i in (1, 2):
        for j in (1, 2):
            key = f"CD{i}_{j}"
            if key in header:
                header[key] = float(header[key]) * factor


def is_per_beam(bunit: str) -> bool:
    """Whether the values are *per beam* — Jy/beam and friends."""
    unit = (bunit or "").strip().lower().replace(" ", "")
    return "/beam" in unit or "perbeam" in unit.replace(" ", "")


def beam_flux_scale(bunit: str, old_major: Any, old_minor: Any,
                    new_major: Any, new_minor: Any) -> float:
    """How much to multiply the pixels by after the beam grew, or 1.0.

    This is the correction the beam bookkeeping alone does not make. Convolving
    with a normalised kernel preserves the *sum* of the pixels; for a map in
    Jy/beam the integrated flux is that sum divided by the beam area, so once the
    beam is wider the same numbers describe **less** flux. A source smoothed with
    a kernel equal to its own beam doubles the beam area, and its measured flux
    would halve — quietly, in a map that otherwise looks right. The values are
    scaled by the ratio of beam areas, which is what CASA's imsmooth does.

    Only for per-beam units: per-pixel values keep their sum, and surface
    brightness (MJy/sr, K) is per solid angle and unchanged by the convolution.
    """
    if not is_per_beam(bunit):
        return 1.0
    try:
        areas = [float(old_major) * float(old_minor), float(new_major) * float(new_minor)]
    except (TypeError, ValueError):
        return 1.0
    if not all(np.isfinite(area) and area > 0 for area in areas):
        return 1.0
    return float(areas[1] / areas[0])


def widen_beam(header: fits.Header, fwhm_degrees: float) -> None:
    """Add the smoothing kernel to the beam, in quadrature, in place.

    Two Gaussians convolved give a Gaussian of quadrature-summed width. A map
    that keeps its old ``BMAJ`` after smoothing reports a resolution it no
    longer has, and every Jy/beam → Jy conversion downstream inherits the error.
    """
    if fwhm_degrees <= 0:
        return
    for key in ("BMAJ", "BMIN"):
        value = header.get(key)
        if value is None:
            continue
        try:
            header[key] = float(np.hypot(float(value), fwhm_degrees))
        except (TypeError, ValueError):
            continue


def pixel_scale_degrees(header: fits.Header) -> float:
    """Mean celestial pixel scale in degrees, or 0 when the header has none.

    For a CD matrix the scale of axis *j* is the norm of its **column**, not the
    diagonal element: a 90°-rotated field has zeros on the diagonal, and reading
    those reports a pixel scale of 0 — which then silently skips the beam
    widening and reports a smoothing kernel of 0 arcseconds.
    """
    def _number(value: Any) -> float | None:
        try:
            number = float(value)
        except (TypeError, ValueError):
            return None
        return number if np.isfinite(number) else None

    # CD, if present, is the whole transformation. Otherwise it is CDELT_i
    # times PC_i_j — and PC's FITS default is the identity, so an omitted
    # PC1_1 is 1, not 0. Reading only what is written gives a scale of 0 for a
    # header that carries PC1_2 alone.
    has_cd = any(header.get(f"CD{i}_{j}") is not None for i in (1, 2) for j in (1, 2))
    has_pc = any(header.get(f"PC{i}_{j}") is not None for i in (1, 2) for j in (1, 2))

    matrix = [[0.0, 0.0], [0.0, 0.0]]
    for i in (1, 2):
        for j in (1, 2):
            if has_cd:
                element = _number(header.get(f"CD{i}_{j}")) or 0.0
            else:
                cdelt = _number(header.get(f"CDELT{i}"))
                if cdelt is None:
                    return 0.0
                default = 1.0 if i == j else 0.0
                pc = _number(header.get(f"PC{i}_{j}")) if has_pc else default
                element = cdelt * (default if pc is None else pc)
            matrix[i - 1][j - 1] = element

    # The scale of axis j is the norm of column j: for a rotated frame the
    # diagonal can be zero while the pixels are perfectly square.
    scales = [float(np.hypot(matrix[0][j], matrix[1][j])) for j in (0, 1)]
    scales = [value for value in scales if np.isfinite(value) and value > 0]
    return float(np.mean(scales)) if scales else 0.0


def worker_filter_fits(input_path: str, output_path: str, smooth_fwhm_pixels: float,
                       regrid_factor: int, combine: str = "auto") -> dict[str, Any]:
    """Smooth and/or shrink ``input_path`` into ``output_path``.

    Either step may be skipped (FWHM ≤ 0, factor ≤ 1), but not both.
    A cube is treated plane by plane: this is a *spatial* operation, and
    smoothing along the spectral axis would be a different instrument.
    """
    smooth_fwhm_pixels = float(smooth_fwhm_pixels or 0.0)
    regrid_factor = int(regrid_factor or 1)
    if smooth_fwhm_pixels <= 0 and regrid_factor <= 1:
        raise ValueError("Nothing to do: give a smoothing FWHM, a regrid factor, or both.")
    if smooth_fwhm_pixels < 0:
        raise ValueError("The smoothing FWHM cannot be negative.")
    if regrid_factor < 1:
        raise ValueError("The regrid factor must be 1 (no regrid) or more.")
    if regrid_factor > 64:
        raise ValueError("A regrid factor above 64 is refused: check the value.")

    with fits.open(input_path, memmap=False) as hdul:
        # An IMAGE extension, not merely the first HDU that carries data: a file
        # whose only payload is a table would otherwise be read as an image of
        # no dimensions and refused with a confusing message.
        image_types = (fits.PrimaryHDU, fits.ImageHDU, fits.CompImageHDU)
        hdu = next((h for h in hdul
                    if isinstance(h, image_types) and getattr(h, "data", None) is not None), None)
        if hdu is None:
            raise ValueError("This FITS file has no image data to filter.")
        header = hdu.header.copy()
        data = np.asarray(hdu.data, dtype=np.float64)

    squeezed = np.squeeze(data)
    if squeezed.ndim not in (2, 3):
        raise ValueError(
            f"Only 2-D images and 3-D cubes can be filtered (this one has "
            f"{squeezed.ndim} significant axes).")

    # numpy's axis order is the reverse of FITS': data.shape[-1] is NAXIS1. The
    # axes that survive the squeeze are the ones whose WCS keywords survive too.
    kept_axes = [data.ndim - index for index, length in enumerate(data.shape) if length > 1]
    drop_axes(header, sorted(kept_axes))

    bunit = str(header.get("BUNIT", "")).strip()
    if combine == "auto":
        combine = combine_for_unit(bunit)
    if combine not in ("mean", "sum"):
        raise ValueError("combine must be 'auto', 'mean' or 'sum'.")

    sigma = fwhm_to_sigma(smooth_fwhm_pixels)
    planes = [squeezed] if squeezed.ndim == 2 else list(squeezed)
    processed = []
    for plane in planes:
        out = smooth_plane(plane, sigma) if sigma > 0 else np.asarray(plane, dtype=np.float64)
        if regrid_factor > 1:
            out = regrid_plane(out, regrid_factor, combine)
        processed.append(out)
    result = processed[0] if squeezed.ndim == 2 else np.stack(processed, axis=0)

    scale = pixel_scale_degrees(header)
    flux_scale = 1.0
    if sigma > 0:
        old_beam = (header.get("BMAJ"), header.get("BMIN"))
        widen_beam(header, smooth_fwhm_pixels * scale)
        # The beam is wider now, so the same numbers stand for less flux unless
        # they are scaled with it.
        flux_scale = beam_flux_scale(bunit, old_beam[0], old_beam[1],
                                     header.get("BMAJ"), header.get("BMIN"))
        if flux_scale != 1.0:
            result = result * flux_scale
            header["HISTORY"] = (
                f"VisIVO: values scaled by {flux_scale:.6f} for the widened beam "
                f"(flux conserved)")
        header["HISTORY"] = (
            f"VisIVO: Gaussian smooth, FWHM {smooth_fwhm_pixels:.3f} px "
            f"(sigma {sigma:.3f} px, truncated at {_TRUNCATE_SIGMAS:.0f} sigma), "
            f"blank-aware")
    if regrid_factor > 1:
        regrid_header(header, regrid_factor)
        header["HISTORY"] = (
            f"VisIVO: regridded by {regrid_factor} ({combine} of each "
            f"{regrid_factor}x{regrid_factor} block)")

    # The header described the input's shape; the writer will fix NAXISn, but a
    # stale NAXIS3 on a squeezed cube would survive it.
    for key in list(header.keys()):
        if key.startswith("NAXIS") and key != "NAXIS":
            del header[key]

    Path(output_path).parent.mkdir(parents=True, exist_ok=True)
    fits.PrimaryHDU(data=result.astype(np.float32), header=header).writeto(
        output_path, overwrite=True)

    finite = result[np.isfinite(result)]
    logger.info("[filter-fits] %s -> %s smooth=%.3fpx factor=%d combine=%s",
                input_path, output_path, smooth_fwhm_pixels, regrid_factor, combine)
    return {
        "output_path": str(output_path),
        "width": int(result.shape[-1]),
        "height": int(result.shape[-2]),
        "depth": int(result.shape[0]) if result.ndim == 3 else 1,
        "range_min": float(finite.min()) if finite.size else 0.0,
        "range_max": float(finite.max()) if finite.size else 0.0,
        "bunit": bunit,
        "combine": combine,
        "smooth_fwhm_pixels": smooth_fwhm_pixels,
        "smooth_fwhm_arcsec": float(smooth_fwhm_pixels * scale * 3600.0) if scale else 0.0,
        "regrid_factor": regrid_factor,
        # 1.0 unless the beam grew under a per-beam unit, in which case the
        # pixels were scaled with it so the integrated flux is unchanged.
        "flux_scale": float(flux_scale),
    }
