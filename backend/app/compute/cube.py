from __future__ import annotations

import base64
import concurrent.futures as _cf  # noqa: F401
import logging  # noqa: F401
import os as _os  # noqa: F401
import shutil  # noqa: F401
import subprocess  # noqa: F401
import warnings  # noqa: F401
import zlib  # noqa: F401
from pathlib import Path  # noqa: F401
from typing import Any  # noqa: F401

import numpy as np

from ..fits_dataset import ScientificFitsDataset  # noqa: F401
from ._common import (  # noqa: F401
    logger,
    _qt_zlib_compress,
    _b64f32,
    _b64i32,
    _b64f32_compressed,
    _b64i32_compressed,
    _finite_range,
    _integrated_spacing,
)


def worker_cube_slice(path: str, z_index: int) -> dict[str, Any]:
    """
    Extract a single Z-axis slice from a FITS cube.

    With memmap=True astropy reads only the bytes for the requested plane
    from disk – O(width * height * 4 bytes) I/O regardless of cube depth.
    """
    import time as _time
    t0 = _time.perf_counter()
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Slice endpoint requires a 3D cube.")
        if z_index < 0 or z_index >= data.shape[0]:
            raise ValueError(f"Slice index {z_index} out of range [0, {data.shape[0] - 1}].")
        # Materialise ONLY this plane – memmap ensures disk read is minimal.
        plane = np.asarray(data[z_index, :, :], dtype=np.float32)
        t_read = _time.perf_counter()
    finally:
        hdul.close()

    range_min, range_max = _finite_range(plane)
    compression, data_b64 = _b64f32_compressed(plane)
    t_encode = _time.perf_counter()
    print(
        "[slice-timing] z={} shape=({},{}) raw_kB={:.0f} wire_kB={:.0f} "
        "stages_s: read={:.3f} encode={:.3f} total={:.3f}".format(
            z_index, int(plane.shape[0]), int(plane.shape[1]),
            plane.nbytes / 1024.0, len(data_b64) / 1024.0,
            t_read - t0, t_encode - t_read, t_encode - t0,
        ),
        flush=True,
    )
    return {
        "width": int(plane.shape[1]),
        "height": int(plane.shape[0]),
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "compression": compression,
        "data_base64": data_b64,
    }


# ── Worker: cube subvolume ────────────────────────────────────────────────────


def worker_cube_subvolume_raw(
    path: str, x0: int, x1: int, y0: int, y1: int, z0: int, z1: int
) -> dict[str, Any]:
    """Binary-streaming variant of :func:`worker_cube_subvolume`.

    Returns the metadata as usual BUT the voxel buffer is a raw
    little-endian float32 ``bytes`` value (``raw_bytes``) — NO base64
    wrapping, NO compression. The wrapping route streams this body
    directly with metadata in response headers.

    Why NO compression: empirically zlib level 1 on HI cube noise
    achieved ~5% compression (1655 MB → 2055 MB after base64) while
    spending ~50 s of single-CPU time. The encoding pipeline became
    the bottleneck (81 s of the 82 s end-to-end). On noise-dominated
    float32 data zlib effectively cannot save bandwidth, only spend
    it. Skipping compression and base64 together turns a ~80 s
    pipeline into ~5-10 s of pure memcpy + transport.

    Why NOT use Dask: the data path is read-once, no compute. The
    monolithic ProcessPoolExecutor task is simpler and avoids the
    pickle-the-1.6-GB-result-across-cluster overhead.
    """
    import time as _time
    t0 = _time.perf_counter()

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Subvolume endpoint requires a 3D cube.")
        depth, height, width = data.shape
        x0, x1 = max(0, x0), min(x1, width - 1)
        y0, y1 = max(0, y0), min(y1, height - 1)
        z0, z1 = max(0, z0), min(z1, depth - 1)
        if x0 > x1 or y0 > y1 or z0 > z1:
            raise ValueError("Invalid subvolume ROI.")
        sub = np.asarray(data[z0 : z1 + 1, y0 : y1 + 1, x0 : x1 + 1], dtype=np.float32)
        t_read = _time.perf_counter()
    finally:
        hdul.close()

    range_min, range_max = _finite_range(sub)
    try:
        spectral = dataset.spectral_axis_metadata(z0, z1)
        spectral_axis_type = str(spectral.get("axis_type", ""))
        spectral_axis_unit = str(spectral.get("axis_unit", ""))
    except Exception:
        spectral_axis_type = ""
        spectral_axis_unit = ""

    # Raw little-endian float32 bytes — what numpy stores natively
    # on x86 / ppc64le. No base64, no compression. The wrapping route
    # streams this as ``application/octet-stream``.
    raw_bytes = np.ascontiguousarray(sub, dtype=np.float32).tobytes()
    t_bytes = _time.perf_counter()
    print(
        "[subvolume-bin-timing] roi=({},{},{},{},{},{}) "
        "shape=({},{},{}) raw={:.1f}MB "
        "stages_s: read={:.2f} tobytes={:.2f} total={:.2f}".format(
            x0, x1, y0, y1, z0, z1,
            int(sub.shape[0]), int(sub.shape[1]), int(sub.shape[2]),
            len(raw_bytes) / 1e6,
            t_read - t0, t_bytes - t_read, t_bytes - t0,
        ),
        flush=True,
    )
    return {
        "width": int(sub.shape[2]),
        "height": int(sub.shape[1]),
        "depth": int(sub.shape[0]),
        "scalar_type": "float32",
        "range_min": float(range_min),
        "range_max": float(range_max),
        "spectral_axis_type": spectral_axis_type,
        "spectral_axis_unit": spectral_axis_unit,
        "bunit": dataset.bunit or "",
        "raw_bytes": raw_bytes,
    }


def worker_cube_subvolume(
    path: str, x0: int, x1: int, y0: int, y1: int, z0: int, z1: int
) -> dict[str, Any]:
    # Stage-by-stage timing so we can prove (or disprove) the
    # "transport is the bottleneck" hypothesis. The result is logged
    # but not returned to the client; it goes into the backend log.
    import time as _time
    t0 = _time.perf_counter()

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Subvolume endpoint requires a 3D cube.")
        depth, height, width = data.shape
        x0, x1 = max(0, x0), min(x1, width - 1)
        y0, y1 = max(0, y0), min(y1, height - 1)
        z0, z1 = max(0, z0), min(z1, depth - 1)
        if x0 > x1 or y0 > y1 or z0 > z1:
            raise ValueError("Invalid subvolume ROI.")
        t_open = _time.perf_counter()
        # np.asarray on a memmap forces a real disk read of the slab.
        # On BeeGFS this is the dominant I/O cost; if it dwarfs the
        # encoding/transport stages below the bottleneck is the
        # filesystem, not the wire.
        sub = np.asarray(data[z0 : z1 + 1, y0 : y1 + 1, x0 : x1 + 1], dtype=np.float32)
        t_read = _time.perf_counter()
    finally:
        hdul.close()

    range_min, range_max = _finite_range(sub)
    t_range = _time.perf_counter()

    # Spectral metadata for each channel in the sub-volume — used by
    # Channel Maps to label each panel with velocity / frequency.
    try:
        spectral = dataset.spectral_axis_metadata(z0, z1)
        spectral_coords = list(spectral.get("coordinates", []))
        spectral_axis_type = str(spectral.get("axis_type", ""))
        spectral_axis_unit = str(spectral.get("axis_unit", ""))
    except Exception:
        spectral_coords = []
        spectral_axis_type = ""
        spectral_axis_unit = ""

    # Empirical finding: zlib (level 1) on float32 HI-cube noise data
    # achieves ~5 % compression (1655 MB → 2055 MB after base64) while
    # spending ~50 s of single-CPU time on a 1.6 GB subcube. Noise is
    # high-entropy; zlib finds no patterns and just adds overhead.
    # We skip compression entirely on this endpoint and pay only the
    # base64 cost (~25-30 s on 1.6 GB). For real speedup the operator
    # should adopt /v1/cube/subvolume_bin which skips base64 too.
    data_b64 = _b64f32(sub)
    compression = ""
    t_encode = _time.perf_counter()

    raw_bytes = int(sub.nbytes)
    final_bytes = int(len(data_b64))
    # Use ``print(flush=True)`` instead of ``logger.info`` because:
    #   1. When Dask dispatches this function, it runs in a dask worker
    #      process whose root logger is NOT configured at INFO. Calls
    #      to ``logger.info(...)`` are silently dropped.
    #   2. ``print`` lands on the worker's stdout, which dask redirects
    #      to the per-node worker log we already write to disk
    #      (``~/.visivo_logs/dask_worker_<host>_<jobid>.log``).
    # So this line is reliably grep-able regardless of where the
    # function actually runs (backend process or distributed worker).
    print(
        "[subvolume-timing] roi=({},{},{},{},{},{}) "
        "shape=({},{},{}) raw={:.1f}MB on_wire={:.1f}MB "
        "stages_s: open={:.2f} read={:.2f} range={:.2f} encode={:.2f} total={:.2f}".format(
            x0, x1, y0, y1, z0, z1,
            int(sub.shape[0]), int(sub.shape[1]), int(sub.shape[2]),
            raw_bytes / 1e6, final_bytes / 1e6,
            t_open - t0, t_read - t_open, t_range - t_read, t_encode - t_range,
            t_encode - t0,
        ),
        flush=True,
    )
    return {
        "width": int(sub.shape[2]),
        "height": int(sub.shape[1]),
        "depth": int(sub.shape[0]),
        "scalar_type": "float32",
        "range_min": float(range_min),
        "range_max": float(range_max),
        "spectral_axis_type": spectral_axis_type,
        "spectral_axis_unit": spectral_axis_unit,
        "bunit": dataset.bunit or "",
        "compression": compression,
        "data_base64": data_b64,
    }


# ── Worker: save a cube sub-region as a new FITS file ────────────────────────


def worker_cube_save_subregion(
    src_path: str,
    output_path: str,
    x0: int, x1: int,
    y0: int, y1: int,
    z0: int, z1: int,
) -> dict[str, Any]:
    """Crop the source cube to ``[z0..z1, y0..y1, x0..x1]`` (all inclusive,
    0-indexed) and write the result as a standalone FITS file at
    ``output_path``.

    The output preserves the source WCS, only shifting the reference pixels
    (``CRPIX1/2/3``) to account for the new origin so coordinates of any
    pixel in the cropped cube match the same physical position in the
    source. ``BUNIT`` and the spectral keywords (``CTYPE3``, ``CUNIT3``,
    ``CRVAL3``, ``CDELT3``) are kept verbatim.

    Returns a dict with the cropped cube's dimensions and value range plus
    the output path, ready to be registered as a new session dataset.
    """
    from astropy.io import fits as _fits  # noqa: PLC0415 (lazy)

    dataset = ScientificFitsDataset(src_path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Sub-region export requires a 3D cube dataset.")
        depth, height, width = data.shape
        x0c, x1c = max(0, int(x0)), min(int(x1), width - 1)
        y0c, y1c = max(0, int(y0)), min(int(y1), height - 1)
        z0c, z1c = max(0, int(z0)), min(int(z1), depth - 1)
        if x0c > x1c or y0c > y1c or z0c > z1c:
            raise ValueError(
                f"Invalid sub-region: x=({x0c}..{x1c}), y=({y0c}..{y1c}), "
                f"z=({z0c}..{z1c})."
            )
        sub = np.asarray(data[z0c:z1c + 1, y0c:y1c + 1, x0c:x1c + 1],
                         dtype=np.float32)
        # Header is cloned from the source PRIMARY so we keep BUNIT, OBJECT,
        # HISTORY and every WCS keyword by default. NAXIS / CRPIX get the
        # crop-aware updates below.
        header = hdul[0].header.copy()
    finally:
        hdul.close()

    new_depth, new_height, new_width = sub.shape
    header["NAXIS"]  = 3
    header["NAXIS1"] = int(new_width)
    header["NAXIS2"] = int(new_height)
    header["NAXIS3"] = int(new_depth)
    # CRPIX is FITS 1-indexed; cropping at numpy index x0 means the old pixel
    # x0 becomes the new pixel 0 → CRPIX1' = CRPIX1 - x0 (and analogously for
    # the other two axes). The crop preserves orientation, scale and rotation.
    for axis_num, offset in ((1, x0c), (2, y0c), (3, z0c)):
        key = f"CRPIX{axis_num}"
        if key in header:
            header[key] = float(header[key]) - float(offset)

    # Provenance trail — astropy splits long HISTORY lines automatically.
    header.add_history(
        f"VisIVO sub-cube extraction from {Path(src_path).name}: "
        f"x={x0c}..{x1c} y={y0c}..{y1c} z={z0c}..{z1c} (0-indexed, inclusive)"
    )

    logger.info(
        "[save_subregion] writing FITS  src=%s  out=%s  shape=%s  "
        "crop=(x=%s..%s, y=%s..%s, z=%s..%s)",
        src_path, output_path, sub.shape,
        x0c, x1c, y0c, y1c, z0c, z1c,
    )
    primary_hdu = _fits.PrimaryHDU(data=sub, header=header)
    _fits.HDUList([primary_hdu]).writeto(output_path, overwrite=True)
    logger.info("[save_subregion] FITS written successfully")

    range_min, range_max = _finite_range(sub)
    return {
        "width": int(new_width),
        "height": int(new_height),
        "depth": int(new_depth),
        "range_min": float(range_min),
        "range_max": float(range_max),
        "output_path": output_path,
    }


def worker_cube_save_channel_2d(
    src_path: str,
    output_path: str,
    channel: int,
) -> dict[str, Any]:
    """Extract a single spectral channel from a 3-D cube and write it as a
    **standalone 2-D FITS image** at ``output_path``.

    Unlike :func:`worker_cube_save_subregion` with a 1-channel z-range — which
    keeps the spectral axis degenerate — this writer drops the spectral axis
    entirely, so the output is `NAXIS=2` and opens directly in the image
    viewer. Designed for the Stokes-analysis workflow where the user needs
    several single-frequency 2-D maps (Q, U, I, V at one channel each).

    The output header inherits ``BUNIT``, ``OBJECT``, the **celestial** WCS
    of the source cube (axes 1 and 2 only) and a HISTORY card recording the
    source file and the channel that was extracted.
    """
    from astropy.io import fits as _fits  # noqa: PLC0415
    from astropy.wcs import WCS as _WCS   # noqa: PLC0415

    dataset = ScientificFitsDataset(src_path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Channel extraction requires a 3-D cube dataset.")
        depth, height, width = data.shape
        ch = int(channel)
        if ch < 0 or ch >= depth:
            raise ValueError(
                f"Channel {ch} is out of range (cube has {depth} channels)."
            )
        plane = np.asarray(data[ch, :, :], dtype=np.float32).copy()
        src_header = hdul[0].header.copy()
    finally:
        hdul.close()

    # Build a clean 2-D header from the source WCS by projecting away the
    # spectral / Stokes axes. This is the same trick used by
    # worker_moment_save_fits — it gives us a valid (RA, Dec) / (l, b) WCS
    # with no degenerate axis.
    try:
        wcs2d = _WCS(src_header, relax=True).celestial
        out_header = wcs2d.to_header()
    except Exception as exc:
        logger.warning(
            "[save_channel_2d] WCS extraction failed for %s (%s) — "
            "writing array with no celestial WCS.", src_path, exc,
        )
        out_header = _fits.Header()

    out_header["NAXIS"]  = 2
    out_header["NAXIS1"] = int(plane.shape[1])
    out_header["NAXIS2"] = int(plane.shape[0])
    for key in ("BUNIT", "OBJECT", "TELESCOP", "INSTRUME"):
        if key in src_header:
            out_header[key] = src_header[key]

    # Record the source channel's spectral coordinate (e.g. central frequency)
    # so the user knows what frequency this 2-D map represents — vital for
    # the Stokes / spectral-index / Faraday-RM workflows in the image viewer.
    if "CRVAL3" in src_header and "CRPIX3" in src_header and "CDELT3" in src_header:
        try:
            spec_value = (
                float(src_header["CRVAL3"])
                + (float(ch + 1) - float(src_header["CRPIX3"]))
                * float(src_header["CDELT3"])
            )
            out_header["SPECVAL"] = (spec_value, "Spectral coord of channel")
            for key in ("CTYPE3", "CUNIT3"):
                if key in src_header:
                    out_header["SPECTYPE" if key == "CTYPE3" else "SPECUNIT"] = (
                        src_header[key],
                        "Spectral axis type/unit of source cube",
                    )
        except Exception:
            pass

    out_header.add_history(
        f"VisIVO 2-D channel export from {Path(src_path).name}: "
        f"channel {ch} (0-indexed)"
    )

    logger.info(
        "[save_channel_2d] writing FITS  src=%s  out=%s  channel=%s  shape=%s",
        src_path, output_path, ch, plane.shape,
    )
    primary_hdu = _fits.PrimaryHDU(data=plane, header=out_header)
    _fits.HDUList([primary_hdu]).writeto(output_path, overwrite=True)
    logger.info("[save_channel_2d] FITS written successfully")

    range_min, range_max = _finite_range(plane)
    return {
        "width":  int(plane.shape[1]),
        "height": int(plane.shape[0]),
        "channel": ch,
        "range_min": float(range_min),
        "range_max": float(range_max),
        "output_path": output_path,
    }


# ── Worker: moment map ────────────────────────────────────────────────────────


# ── Parallelised CPU moment helpers (used when GPU is unavailable) ────────────


def worker_noise_estimate(
    path: str,
    x0: int, x1: int,
    y0: int, y1: int,
    channel_start: int,
    channel_end: int,
) -> dict[str, Any]:
    """
    Estimate per-channel noise in a user-specified emission-free spatial region.

    Returns the per-channel median absolute deviation (MAD) and the derived
    Gaussian sigma (MAD × 1.4826).  The spatial region [x0:x1, y0:y1] should
    be an area of the cube that contains only noise (no astronomical emission).
    """
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Noise estimation requires a 3D cube.")
        depth = data.shape[0]
        height = data.shape[1]
        width = data.shape[2]
        x0c, x1c = max(0, x0), min(x1, width - 1)
        y0c, y1c = max(0, y0), min(y1, height - 1)
        z0 = max(0, min(channel_start, depth - 1))
        z1 = max(0, min(channel_end,   depth - 1))
        if x0c > x1c or y0c > y1c or z0 > z1:
            raise ValueError("Invalid spatial or spectral region for noise estimation.")
        # Materialise only the requested region.
        region = np.asarray(data[z0 : z1 + 1, y0c : y1c + 1, x0c : x1c + 1], dtype=np.float32)
    finally:
        hdul.close()

    mad_values: list[float] = []
    sigma_values: list[float] = []
    total_finite = 0
    total_zero   = 0
    for z in range(region.shape[0]):
        plane = region[z].ravel()
        finite = plane[np.isfinite(plane)]
        total_finite += int(finite.size)
        if finite.size:
            total_zero += int(np.count_nonzero(finite == 0))
        if finite.size < 2:
            mad_values.append(float("nan"))
            sigma_values.append(float("nan"))
            continue
        # Zero-as-blank heuristic: a lot of radio/IR cubes use exactly 0.0 for
        # off-source pixels instead of NaN. If you compute MAD over a region
        # that mostly overlaps that blanked area, the median of the finite
        # pixels is 0 and |finite - 0| also has median 0 — you get a flat
        # zero noise that hides the actual data noise. When more than half
        # the finite pixels in this channel are exactly 0, treat them as
        # blanks and use only the non-zero pixels for the MAD. We keep zeros
        # in when they are a minority because some cubes (post-baseline
        # subtraction, calibrated brightness temperatures) legitimately have
        # noise centred on zero.
        nonzero = finite[finite != 0]
        if nonzero.size >= 2 and nonzero.size < finite.size / 2:
            sample = nonzero
        else:
            sample = finite
        median = float(np.median(sample))
        mad = float(np.median(np.abs(sample - median)))
        mad_values.append(mad)
        sigma_values.append(mad * 1.4826)  # MAD → σ for Gaussian noise

    # Surface what fraction of finite pixels in the whole selection are
    # exactly zero — the dialog uses this to warn the user when the result
    # is suspicious (e.g. "80 % of the region is blanked; pick a tighter
    # box inside the data footprint").
    blank_fraction = float(total_zero / total_finite) if total_finite else 0.0

    return {
        "channel_start": z0,
        "channel_end": z1,
        "num_channels": z1 - z0 + 1,
        "mad": mad_values,
        "sigma": sigma_values,
        "region": {"x0": x0c, "x1": x1c, "y0": y0c, "y1": y1c},
        "blank_fraction": blank_fraction,
    }


# ── Worker: image full / preview ──────────────────────────────────────────────


def _build_preview(image: np.ndarray, max_side: int) -> tuple[np.ndarray, float]:
    h, w = image.shape
    longest = max(w, h)
    limit = max(1, int(max_side))
    if longest <= limit:
        return np.ascontiguousarray(image, dtype=np.float32), 1.0
    scale = float(limit) / float(longest)
    pw = max(1, int(round(w * scale)))
    ph = max(1, int(round(h * scale)))
    xi = np.clip(
        np.floor((np.arange(pw, dtype=np.float64) + 0.5) * (w / pw)).astype(np.int64), 0, w - 1
    )
    yi = np.clip(
        np.floor((np.arange(ph, dtype=np.float64) + 0.5) * (h / ph)).astype(np.int64), 0, h - 1
    )
    preview = np.ascontiguousarray(image[np.ix_(yi, xi)], dtype=np.float32)
    return preview, float(longest) / float(max(preview.shape[0], preview.shape[1]))


def worker_image_full(path: str) -> dict[str, Any]:
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 2:
            raise ValueError("Image endpoint requires a 2D FITS dataset.")
        image = np.asarray(data, dtype=np.float32)
    finally:
        hdul.close()

    range_min, range_max = _finite_range(image)
    compression, data_b64 = _b64f32_compressed(image)
    return {
        "width": int(image.shape[1]),
        "height": int(image.shape[0]),
        "full_width": int(image.shape[1]),
        "full_height": int(image.shape[0]),
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "is_preview": False,
        "preview_scale_factor": 1.0,
        "compression": compression,
        "data_base64": data_b64,
    }


def worker_image_preview(path: str, max_longest_side: int) -> dict[str, Any]:
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 2:
            raise ValueError("Image preview endpoint requires a 2D FITS dataset.")
        full_h, full_w = int(data.shape[0]), int(data.shape[1])
        # Out-of-core: _build_preview reads only the decimated pixels straight
        # from the memmap (via np.ix_), so a multi-GB mosaic is never fully
        # loaded into RAM just to be downsampled for the initial preview.
        preview, scale = _build_preview(data, max_longest_side)
    finally:
        hdul.close()

    is_preview = preview.shape[0] != full_h or preview.shape[1] != full_w
    range_min, range_max = _finite_range(preview)
    compression, data_b64 = _b64f32_compressed(preview)
    return {
        "width": int(preview.shape[1]),
        "height": int(preview.shape[0]),
        "full_width": full_w,
        "full_height": full_h,
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "is_preview": is_preview,
        "preview_scale_factor": scale,
        "compression": compression,
        "data_base64": data_b64,
    }


def _num_pyramid_levels(full_w: int, full_h: int, tile_size: int) -> int:
    """Number of pyramid levels until the whole image fits in a single tile.

    Level 0 is full resolution; each level halves the resolution. Level
    ``num_levels - 1`` is the coarsest, where the image is ≤ tile_size on both
    axes.
    """
    levels = 1
    w, h = int(full_w), int(full_h)
    ts = max(1, int(tile_size))
    while w > ts or h > ts:
        w = (w + 1) // 2
        h = (h + 1) // 2
        levels += 1
    return levels


def worker_image_tile(
    path: str, level: int, tile_x: int, tile_y: int, tile_size: int = 256
) -> dict[str, Any]:
    """Serve one pyramid tile of a 2-D image, **out-of-core**.

    Level 0 is full resolution; level ``L`` decimates by ``2**L``. A tile
    ``(tile_x, tile_y)`` covers ``tile_size`` downsampled pixels per axis. Only
    the strided sub-region is materialised (≤ ``tile_size²`` floats), so a
    multi-GB mosaic never loads into RAM — unlike ``worker_image_full`` /
    ``worker_image_preview`` which read the whole plane.

    Downsampling is nearest-neighbour decimation (stride read); this keeps the
    memory bound independent of level at the cost of some aliasing on coarse
    levels, which is acceptable for pan/zoom navigation.
    """
    level = int(level)
    tile_x = int(tile_x)
    tile_y = int(tile_y)
    tile_size = int(tile_size)
    if level < 0 or tile_x < 0 or tile_y < 0:
        raise ValueError("level, tile_x and tile_y must be non-negative.")
    if tile_size < 1 or tile_size > 4096:
        raise ValueError("tile_size must be in [1, 4096].")

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 2:
            raise ValueError("Image tile endpoint requires a 2D FITS dataset.")
        full_h, full_w = int(data.shape[0]), int(data.shape[1])
        num_levels = _num_pyramid_levels(full_w, full_h, tile_size)
        if level >= num_levels:
            raise ValueError(
                f"level {level} is out of range: this {full_w}x{full_h} image has "
                f"{num_levels} pyramid level(s) (0..{num_levels - 1}) for tile_size "
                f"{tile_size}."
            )
        step = 1 << level  # 2 ** level
        x0 = tile_x * tile_size * step
        y0 = tile_y * tile_size * step
        if x0 >= full_w or y0 >= full_h:
            raise ValueError(
                f"Tile (level={level}, x={tile_x}, y={tile_y}) is out of range "
                f"for a {full_w}x{full_h} image."
            )
        x_stop = min(x0 + tile_size * step, full_w)
        y_stop = min(y0 + tile_size * step, full_h)
        # Out-of-core: the strided slice copies only the decimated tile
        # (≤ tile_size² floats), never the full plane.
        tile = np.asarray(data[y0:y_stop:step, x0:x_stop:step], dtype=np.float32)
    finally:
        hdul.close()

    range_min, range_max = _finite_range(tile)
    compression, data_b64 = _b64f32_compressed(tile)
    return {
        "width": int(tile.shape[1]),
        "height": int(tile.shape[0]),
        "full_width": full_w,
        "full_height": full_h,
        "level": level,
        "num_levels": num_levels,
        "tile_x": tile_x,
        "tile_y": tile_y,
        "tile_size": tile_size,
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "compression": compression,
        "data_base64": data_b64,
    }


def worker_cube_preview(path: str, downsample: int) -> dict[str, Any]:
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        data = dataset.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError("Cube preview requires a 3D cube.")
        stride = max(1, int(downsample))
        preview = np.asarray(data[::stride, ::stride, ::stride], dtype=np.float32)
    finally:
        hdul.close()

    range_min, range_max = _finite_range(preview)
    # zlib level 1 — same speed-vs-ratio rationale as worker_cube_subvolume.
    # For a 10 GB cube + downsample 4 the preview is ~170 MB; compression
    # brings it to ~70-90 MB on the wire and shaves seconds off the tunnel
    # transit on slow links.
    compression, data_b64 = _b64f32_compressed(preview, level=1)
    return {
        "width": int(preview.shape[2]),
        "height": int(preview.shape[1]),
        "depth": int(preview.shape[0]),
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "compression": compression,
        "data_base64": data_b64,
    }


# ── S-02: Spectral line-width maps ────────────────────────────────────────────
