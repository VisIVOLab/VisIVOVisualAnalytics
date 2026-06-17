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
    with fits.open(path, memmap=False) as hdul:
        hdu = _first_image_hdu(hdul)
        if hdu.data is None:
            raise ValueError(f"{path!r} has no image data.")
        data = np.asarray(hdu.data, dtype=np.float32)
        header = hdu.header.copy()
    squeezed = np.squeeze(data)
    while squeezed.ndim > 2:
        squeezed = squeezed[0]
    if squeezed.ndim != 2:
        raise ValueError(f"{path!r} does not contain a 2-D image plane.")
    try:
        wcs = WCS(header).celestial
    except Exception:  # noqa: BLE001 — WCS may be absent/invalid
        wcs = WCS(naxis=2)
    return np.asarray(squeezed, dtype=np.float32), header, wcs


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
    (α = log10(A/B) / log10(νa/νb)). Map B is reprojected onto A's grid when
    their shapes differ.
    """
    a, header, wcs_a = load_image2d(path_a)

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
        b, _, _ = load_image2d(path_b)
        if b.shape != a.shape:
            b = _reproject_to(b, path_b, header)
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
                if not freq_a or not freq_b or freq_a <= 0 or freq_b <= 0:
                    raise ValueError(
                        "spectral_index requires positive freq_a and freq_b.")
                out = np.log10(a / b) / np.log10(freq_a / freq_b)
    else:
        raise ValueError(
            f"Unsupported operation '{op}'. Supported: "
            f"{sorted(_UNARY_OPS | _BINARY_OPS)}")

    out = np.asarray(out, dtype=np.float32)
    out_header = header.copy()
    out_header["BUNIT"] = "" if op in ("divide", "ratio", "spectral_index") else out_header.get("BUNIT", "")
    out_header["HISTORY"] = f"VisIVO map_math op={op}"
    fits.PrimaryHDU(data=out, header=out_header).writeto(output_path, overwrite=True)

    finite = out[np.isfinite(out)]
    return {
        "output_path": output_path,
        "width": int(out.shape[1]),
        "height": int(out.shape[0]),
        "op": op,
        "range_min": float(np.min(finite)) if finite.size else 0.0,
        "range_max": float(np.max(finite)) if finite.size else 0.0,
    }


def _reproject_to(image: np.ndarray, image_path: str, target_header: fits.Header) -> np.ndarray:
    from reproject import reproject_interp  # noqa: PLC0415

    with fits.open(image_path, memmap=False) as hdul:
        src_wcs = WCS(_first_image_hdu(hdul).header).celestial
    target_wcs = WCS(target_header).celestial
    target_shape = (int(target_header["NAXIS2"]), int(target_header["NAXIS1"]))
    out, _ = reproject_interp((image, src_wcs), target_wcs, shape_out=target_shape)
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
    row_var = float(np.var(rm)) if rm.size else 0.0
    col_var = float(np.var(cm)) if cm.size else 0.0
    denom = min(row_var, col_var)
    striping_metric = float(max(row_var, col_var) / denom) if denom > 0 else 0.0

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
