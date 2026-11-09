"""Advanced 2-D image products: pixel math, mosaicking, publication figures
and image-quality metrics.

These back four SKAVA visualization use cases (UC 3.2, 3.3, 1.3, 4.2). Each
worker is a pure function — paths in, FITS/PNG out (or a metrics dict) — so
it can be unit-tested without the FastAPI session machinery. The router
(``app/routers/products.py``) owns dataset resolution and workspace paths.
"""
from __future__ import annotations

import logging
import warnings
from typing import Any

import numpy as np
from astropy.io import fits
from astropy.wcs import WCS

logger = logging.getLogger("visivo.products")


# ── Shared loader ───────────────────────────────────────────────────────────


def _first_image_hdu(hdul: fits.HDUList):
    for hdu in hdul:
        if getattr(hdu, "is_image", False) and hdu.data is not None:
            return hdu
    return hdul[0]


def load_image2d(path: str) -> tuple[np.ndarray, fits.Header, WCS]:
    """Return (2-D float32 array, header, WCS) for a FITS image or the first
    plane of a cube. Degenerate axes are squeezed; for >2-D data the first
    spectral/stokes plane is taken."""
    with _vs_open_fits(path, memmap=False) as hdul:
        hdu = _first_image_hdu(hdul)
        if hdu.data is None:
            raise ValueError(f"{path!r} has no image data.")
        data = np.asarray(hdu.data, dtype=np.float32)
        header = hdu.header.copy()
        # Built while the file is open: lookup-table distortions (CPDIS/D2IM)
        # live in extension HDUs that astropy reads through ``fobj``.
        wcs = _plane_wcs(header, hdul)
    squeezed = np.squeeze(data)
    while squeezed.ndim > 2:
        squeezed = squeezed[0]
    if squeezed.ndim != 2:
        raise ValueError(f"{path!r} does not contain a 2-D image plane.")
    return np.asarray(squeezed, dtype=np.float32), header, wcs


def _plane_wcs(header: fits.Header, hdul=None) -> WCS:
    """Celestial WCS of the plane :func:`load_image2d` returns, or an empty one.

    The full-dimensional WCS is tried first. If it cannot be built, the first
    two axes alone are (the plane taken is FITS axes 1 and 2): astropy refuses
    SIP on a header with more than two axes, and falling straight to an empty
    WCS would make a SIP cube look as if it had no sky coordinates, so pixel
    math would combine two such cubes without aligning them. ``hdul`` is the
    open file, needed for lookup-table distortions.
    """
    fobj = hdul if isinstance(hdul, fits.HDUList) else None
    for kwargs in ({}, {"naxis": 2}):
        try:
            return WCS(header, fobj=fobj, **kwargs).celestial
        except Exception:  # noqa: BLE001 — WCS may be absent/invalid
            continue
    return WCS(naxis=2)


def _robust_std(data: np.ndarray) -> float:
    """MAD-based robust standard deviation (ignoring NaN/inf)."""
    finite = data[np.isfinite(data)]
    if finite.size == 0:
        return float("nan")
    med = np.median(finite)
    mad = np.median(np.abs(finite - med))
    return float(1.4826 * mad)


# ── UC 3.2: pixel math / spectral index ─────────────────────────────────────

_UNARY_OPS = {"log10", "abs", "square"}
_BINARY_OPS = {"add", "subtract", "multiply", "divide", "ratio", "spectral_index"}


def worker_map_math(
    output_path: str,
    *,
    path_a: str,
    op: str,
    path_b: str | None = None,
    freq_a: float | None = None,
    freq_b: float | None = None,
) -> dict[str, Any]:
    """Pixel-wise math on one or two 2-D maps.

    Unary ops (``log10``, ``abs``, ``square``) need only ``path_a``. Binary
    ops (``add/subtract/multiply/divide/ratio``) and ``spectral_index`` need
    ``path_b``; ``spectral_index`` additionally needs ``freq_a``/``freq_b``
    (α = log10(A/B) / log10(νa/νb), NaN wherever A or B is not a finite
    positive flux). Map B is combined pixel by pixel only when it lies on A's
    sky grid; otherwise it is first reprojected onto A's grid (see
    :func:`_align_to_a`), and ``reprojected`` in the result says so.
    """
    a, header, wcs_a = load_image2d(path_a)
    reprojected = False

    if op in _UNARY_OPS:
        with np.errstate(divide="ignore", invalid="ignore"):
            if op == "log10":
                out = np.log10(a)
            elif op == "abs":
                out = np.abs(a)
            else:  # square
                out = np.square(a)
    elif op in _BINARY_OPS:
        if not path_b:
            raise ValueError(f"Operation '{op}' requires a second map (path_b).")
        if op == "spectral_index" and (
                not freq_a or not freq_b or freq_a <= 0 or freq_b <= 0
                or freq_a == freq_b):
            raise ValueError(
                "spectral_index requires positive, different freq_a and freq_b.")
        b, header_b, wcs_b = load_image2d(path_b)
        for name, hdr, wcs in (("A", header, wcs_a), ("B", header_b, wcs_b)):
            if _declares_sky(hdr) and not _has_celestial(wcs):
                raise ValueError(
                    f"Map {name} declares sky axes but its WCS cannot be read, so "
                    "the maps cannot be matched on the sky.")
        b, reprojected = _align_to_a(a, wcs_a, b, wcs_b)
        with np.errstate(divide="ignore", invalid="ignore"):
            if op == "add":
                out = a + b
            elif op == "subtract":
                out = a - b
            elif op == "multiply":
                out = a * b
            elif op in ("divide", "ratio"):
                out = a / b
            else:  # spectral_index
                out = spectral_index(a, b, float(freq_a), float(freq_b))
    else:
        raise ValueError(
            f"Unsupported operation '{op}'. Supported: "
            f"{sorted(_UNARY_OPS | _BINARY_OPS)}")

    out = np.asarray(out, dtype=np.float32)
    out_header = header.copy()
    out_header["BUNIT"] = "" if op in ("divide", "ratio", "spectral_index") else out_header.get("BUNIT", "")
    out_header["HISTORY"] = f"VisIVO map_math op={op}"
    if reprojected:
        out_header["HISTORY"] = (
            "VisIVO map_math: map B reprojected onto map A's grid "
            "(reproject_interp, bilinear)")
    hdus = [fits.PrimaryHDU(data=out, header=out_header), *_distortion_hdus(path_a)]
    fits.HDUList(hdus).writeto(output_path, overwrite=True)

    finite = out[np.isfinite(out)]
    return {
        "output_path": output_path,
        "width": int(out.shape[1]),
        "height": int(out.shape[0]),
        "op": op,
        "reprojected": reprojected,
        "range_min": float(np.min(finite)) if finite.size else 0.0,
        "range_max": float(np.max(finite)) if finite.size else 0.0,
    }


def _distortion_hdus(path: str) -> list:
    """Copies of the lookup-table distortion extensions (WCSDVARR, D2IMARR).

    The output keeps A's header, whose CPDIS/D2IM cards point at these
    extensions by name; written without them, the output's WCS could not be
    read back and the result would lose its sky coordinates.
    """
    try:
        with _vs_open_fits(path, memmap=False) as hdul:
            return [hdu.copy() for hdu in hdul[1:]
                    if str(hdu.header.get("EXTNAME", "")).upper() in ("WCSDVARR", "D2IMARR")]
    except Exception:  # noqa: BLE001 — a source without extensions has none to carry
        return []


def spectral_index(flux_a: np.ndarray, flux_b: np.ndarray,
                   freq_a: float, freq_b: float) -> np.ndarray:
    """Two-point spectral index α = log10(Sa/Sb) / log10(νa/νb), S ∝ ν^α.

    α is defined only where BOTH fluxes are finite and positive: the ratio of
    two negative fluxes is positive, so taking its log would turn two noise
    pixels (−2 and −1 at 2 and 1 GHz) into a confident α = 1. Every other
    pixel is NaN.
    """
    a = np.asarray(flux_a, dtype=np.float64)
    b = np.asarray(flux_b, dtype=np.float64)
    valid = np.isfinite(a) & np.isfinite(b) & (a > 0) & (b > 0)
    ratio = np.divide(a, b, out=np.ones(valid.shape), where=valid)
    out = np.full(valid.shape, np.nan)
    np.divide(np.log10(ratio), np.log10(freq_a / freq_b), out=out, where=valid)
    return out


# Two maps whose sky positions differ by less than this (in A's pixels) at
# every sampled pixel are on the same grid. It absorbs the rounding of WCS
# keywords written by different tools (CDELT vs CD, float32 header values)
# while a real misregistration, even a tenth of a pixel, is reprojected.
_SAME_GRID_TOLERANCE_PX = 0.01


_SKY_CTYPE_PREFIXES = ("RA", "DEC", "GLON", "GLAT", "ELON", "ELAT", "SLON", "SLAT")


def _declares_sky(header: fits.Header) -> bool:
    """Whether the header names a celestial axis, readable or not.

    Tells "this map has no sky WCS" apart from "its sky WCS could not be
    parsed" (an astropy-unreadable distortion, say): the second must not be
    combined by shape as if it were a plain array.
    """
    for i in range(1, int(header.get("NAXIS", 0) or 0) + 1):
        ctype = str(header.get(f"CTYPE{i}", "")).upper()
        if ctype.split("-", 1)[0] in _SKY_CTYPE_PREFIXES:
            return True
    return False


def _has_celestial(wcs: WCS) -> bool:
    try:
        return bool(wcs.has_celestial)
    except Exception:  # noqa: BLE001 — a half-built WCS has no sky
        return False


def _same_sky_grid(wcs_a: WCS, wcs_b: WCS, shape: tuple[int, int]) -> bool:
    """Whether every pixel of A falls on the same pixel of B (within tolerance).

    A grid of A's pixels (every pixel up to 33 per axis, otherwise 33 evenly
    spaced ones including both edges) is taken to the sky with A's WCS and
    back to pixels with B's, through SkyCoord, so two equivalent descriptions
    (CD vs CDELT+PC, ICRS vs Galactic) agree. The density matters for SIP: a
    polynomial difference of order < 33 per axis cannot vanish at 33 distinct
    points of every row and column without vanishing identically, whereas a
    3×3 sample misses a 4th-order term placed to cancel there. Lookup-table
    distortions (CPDIS/D2IM) can hide anything between samples, so a map with
    one is always treated as on a different grid. A failure to transform
    counts as "different".
    """
    for wcs in (wcs_a, wcs_b):
        if any(getattr(wcs, name, None) is not None
               for name in ("cpdis1", "cpdis2", "det2im1", "det2im2")):
            return False
    ny, nx = shape
    xs = np.unique(np.round(np.linspace(0.0, nx - 1.0, min(nx, 33))))
    ys = np.unique(np.round(np.linspace(0.0, ny - 1.0, min(ny, 33))))
    px, py = (g.ravel() for g in np.meshgrid(xs, ys))
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            sky = wcs_a.pixel_to_world(px, py)
            bx, by = wcs_b.world_to_pixel(sky)
    except Exception:  # noqa: BLE001 — incompatible frames, bad WCS
        return False
    offset = np.hypot(np.asarray(bx, dtype=float) - px, np.asarray(by, dtype=float) - py)
    return bool(np.all(np.isfinite(offset)) and np.max(offset) <= _SAME_GRID_TOLERANCE_PX)


def _align_to_a(a: np.ndarray, wcs_a: WCS,
                b: np.ndarray, wcs_b: WCS) -> tuple[np.ndarray, bool]:
    """Map B on A's pixel grid, and whether it had to be reprojected.

    Same shape and same sky grid: B is untouched, so maps that were already
    aligned combine exactly as before. Otherwise B is resampled onto A's grid
    with ``reproject_interp`` (bilinear), the convention of the backend's
    reprojection route (``worker_reproject``, order 1): interpolation keeps a
    surface brightness (Jy/beam, MJy/sr, K), which is what these maps hold,
    whereas a flux-conserving resampling would scale each value by the ratio
    of pixel areas. B pixels outside its footprint become NaN. Two maps with
    no sky WCS are combined by shape alone; one with a sky WCS and one without
    cannot be matched on the sky and are refused.
    """
    has_a, has_b = _has_celestial(wcs_a), _has_celestial(wcs_b)
    if not has_a and not has_b:
        if b.shape != a.shape:
            raise ValueError(
                f"Maps A {a.shape} and B {b.shape} differ in shape and neither "
                "has a celestial WCS, so B cannot be reprojected onto A.")
        return b, False
    if has_a != has_b:
        with_wcs, without = ("A", "B") if has_a else ("B", "A")
        raise ValueError(
            f"Map {with_wcs} has a celestial WCS but map {without} does not, so "
            "their pixels cannot be matched on the sky. Give both maps a WCS "
            "(or neither) before combining them.")
    if b.shape == a.shape and _same_sky_grid(wcs_a, wcs_b, a.shape):
        return b, False
    return _reproject_to(b, wcs_b, wcs_a, a.shape), True


def _reproject_to(image: np.ndarray, src_wcs: WCS, target_wcs: WCS,
                  target_shape: tuple[int, int]) -> np.ndarray:
    from reproject import reproject_interp  # noqa: PLC0415

    out, _ = reproject_interp((image, src_wcs), target_wcs,
                              shape_out=(int(target_shape[0]), int(target_shape[1])))
    return np.asarray(out, dtype=np.float32)


# ── UC 3.3: noise-weighted mosaic ───────────────────────────────────────────


def worker_mosaic(
    output_path: str,
    *,
    paths: list[str],
    weight_mode: str = "noise",
) -> dict[str, Any]:
    """Reproject + co-add 2-D images onto a common optimal WCS.

    ``weight_mode='noise'`` weights each input by 1/σ² (σ = robust MAD std),
    suppressing noisy edges; ``'uniform'`` weights all inputs equally. Edge
    effects are handled by reproject's footprint-aware coadd.
    """
    if len(paths) < 2:
        raise ValueError("Mosaicking requires at least two input images.")

    from reproject import reproject_interp  # noqa: PLC0415
    from reproject.mosaicking import find_optimal_celestial_wcs, reproject_and_coadd  # noqa: PLC0415

    inputs: list[tuple[np.ndarray, WCS]] = []
    weights: list[np.ndarray] = []
    for p in paths:
        data, _, wcs = load_image2d(p)
        inputs.append((data, wcs))
        if weight_mode == "noise":
            sigma = _robust_std(data)
            w = 1.0 / (sigma * sigma) if sigma and np.isfinite(sigma) and sigma > 0 else 1.0
        else:
            w = 1.0
        weights.append(np.full(data.shape, w, dtype=np.float32))

    optimal_wcs, shape_out = find_optimal_celestial_wcs(inputs)
    array, footprint = reproject_and_coadd(
        inputs,
        optimal_wcs,
        shape_out=shape_out,
        input_weights=weights,
        reproject_function=reproject_interp,
        combine_function="mean",
    )
    array = np.asarray(array, dtype=np.float32)
    out_header = optimal_wcs.to_header()
    out_header["HISTORY"] = f"VisIVO mosaic n={len(paths)} weights={weight_mode}"
    fits.PrimaryHDU(data=array, header=out_header).writeto(output_path, overwrite=True)

    finite = array[np.isfinite(array)]
    covered = footprint > 0
    return {
        "output_path": output_path,
        "width": int(array.shape[1]),
        "height": int(array.shape[0]),
        "n_inputs": len(paths),
        "weight_mode": weight_mode,
        "coverage_fraction": float(np.count_nonzero(covered) / covered.size),
        "range_min": float(np.min(finite)) if finite.size else 0.0,
        "range_max": float(np.max(finite)) if finite.size else 0.0,
    }


# ── UC 1.3: publication-quality figure ──────────────────────────────────────


def worker_publication_figure(
    output_path: str,
    *,
    path: str,
    cmap: str = "inferno",
    stretch: str = "linear",
    percent: float = 99.5,
    title: str = "",
    add_grid: bool = True,
    add_colorbar: bool = True,
    dpi: int = 200,
) -> dict[str, Any]:
    """Render a 2-D image as a publication-quality figure with a WCS sky grid,
    a labelled colorbar (BUNIT) and a configurable stretch. Output format is
    inferred from the path extension (.png/.pdf/.svg)."""
    import matplotlib  # noqa: PLC0415

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt  # noqa: PLC0415
    from astropy.visualization import simple_norm  # noqa: PLC0415

    data, header, wcs = load_image2d(path)
    has_celestial = bool(wcs.has_celestial)

    norm = simple_norm(data, stretch=stretch, percent=percent)
    fig = plt.figure(figsize=(8.0, 6.5), dpi=dpi)
    if has_celestial:
        ax = fig.add_subplot(1, 1, 1, projection=wcs)
        ax.set_xlabel("Right Ascension")
        ax.set_ylabel("Declination")
        if add_grid:
            ax.coords.grid(True, color="white", ls=":", alpha=0.5)
    else:
        ax = fig.add_subplot(1, 1, 1)
        ax.set_xlabel("X (pixels)")
        ax.set_ylabel("Y (pixels)")

    im = ax.imshow(data, origin="lower", cmap=cmap, norm=norm)
    if title:
        ax.set_title(title)
    if add_colorbar:
        cbar = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
        cbar.set_label(str(header.get("BUNIT", "")).strip() or "value")

    fig.savefig(output_path, bbox_inches="tight", dpi=dpi)
    plt.close(fig)
    return {
        "output_path": output_path,
        "width": int(data.shape[1]),
        "height": int(data.shape[0]),
        "stretch": stretch,
        "has_wcs": has_celestial,
    }


# ── UC 4.2: image-quality / artifact metrics ────────────────────────────────


# Largest value of the striping metric. Perfect stripes (one axis with
# variance, the other with exactly none) have an infinite ratio; JSON has no
# infinity, so they are reported at this ceiling, and any finite ratio above
# it is clipped to it, keeping the scale monotonic. 1e6 is far beyond the
# ``striping`` flag threshold of 5, so the flag is unaffected by the cap.
STRIPING_METRIC_MAX = 1.0e6


def striping_ratio(row_var: float, col_var: float) -> float:
    """Larger over smaller variance of row and column means, capped.

    Both zero (a flat image) is 0: nothing is striped. One zero and the other
    not is the most striped an image can be, so it is
    :data:`STRIPING_METRIC_MAX`, not 0 as a "denominator is zero" guard would
    say — that guard hid every image made of perfectly uniform bands.
    """
    hi, lo = max(row_var, col_var), min(row_var, col_var)
    if not np.isfinite(hi) or hi <= 0:
        return 0.0
    if lo <= 0 or hi >= lo * STRIPING_METRIC_MAX:
        return STRIPING_METRIC_MAX
    return float(hi / lo)


def worker_image_quality(path: str) -> dict[str, Any]:
    """Compute quality/artifact metrics for a 2-D image: blanked/zero/negative
    fractions, robust statistics, dynamic range, a negative-bowl flag and a
    simple striping metric (row-vs-column mean variance ratio)."""
    data, _, _ = load_image2d(path)
    total = int(data.size)
    finite_mask = np.isfinite(data)
    finite = data[finite_mask]
    n_finite = int(finite.size)

    blanked_fraction = float((total - n_finite) / total) if total else 0.0
    if n_finite == 0:
        return {
            "total_pixels": total,
            "blanked_fraction": blanked_fraction,
            "zero_fraction": 0.0,
            "negative_fraction": 0.0,
            "min": 0.0, "max": 0.0, "mean": 0.0, "median": 0.0,
            "robust_std": 0.0, "dynamic_range": 0.0,
            "negative_bowl": False, "striping_metric": 0.0,
            "flags": ["all_blanked"],
        }

    med = float(np.median(finite))
    rstd = _robust_std(data)
    vmin, vmax = float(np.min(finite)), float(np.max(finite))
    dynamic_range = float(vmax / rstd) if rstd and rstd > 0 else 0.0
    negative_fraction = float(np.count_nonzero(finite < 0) / n_finite)
    zero_fraction = float(np.count_nonzero(finite == 0) / n_finite)
    # Negative bowl: a strong negative excursion relative to the noise — a
    # classic imaging/deconvolution artifact around bright sources.
    negative_bowl = bool(rstd > 0 and vmin < -8.0 * rstd)

    # Striping: compare the spread of row means vs column means. Coherent
    # row/column structure (e.g. scan stripes, bandpass residuals) makes one
    # axis far more variable than the other.
    filled = np.where(finite_mask, data, np.nan)
    with warnings.catch_warnings():
        # Fully-blanked rows/cols yield an all-NaN slice; we filter the NaNs
        # immediately below, so the "Mean of empty slice" warning is noise.
        warnings.simplefilter("ignore", RuntimeWarning)
        row_means = np.nanmean(filled, axis=1)
        col_means = np.nanmean(filled, axis=0)
    rm = row_means[np.isfinite(row_means)]
    cm = col_means[np.isfinite(col_means)]
    row_var = float(np.var(rm, dtype=np.float64)) if rm.size else 0.0
    col_var = float(np.var(cm, dtype=np.float64)) if cm.size else 0.0
    striping_metric = striping_ratio(row_var, col_var)

    flags: list[str] = []
    if blanked_fraction > 0.2:
        flags.append("high_blanked_fraction")
    if negative_bowl:
        flags.append("negative_bowl")
    if striping_metric > 5.0:
        flags.append("striping")
    if dynamic_range and dynamic_range < 5.0:
        flags.append("low_dynamic_range")

    return {
        "total_pixels": total,
        "blanked_fraction": blanked_fraction,
        "zero_fraction": zero_fraction,
        "negative_fraction": negative_fraction,
        "min": vmin,
        "max": vmax,
        "mean": float(np.mean(finite)),
        "median": med,
        "robust_std": rstd,
        "dynamic_range": dynamic_range,
        "negative_bowl": negative_bowl,
        "striping_metric": striping_metric,
        "flags": flags,
    }


def _vs_open_fits(path, **kwargs):
    # A FITS file, or a field of a VBT volume read in place (issue #83).
    from app.volume_source import open_fits  # noqa: PLC0415
    return open_fits(path, **kwargs)
