"""The profile of an image along a line (issue #503): the value at samples one
pixel apart (the last one at the end of the line), by bilinear interpolation,
against the offset from its start in arcsec on the sky.

The image is read in small windows around runs of samples (never a plane): a
line across a 10⁴ × 10⁴ image touches only the pixels along it. A sampling
``width`` of more than one pixel averages that many parallel lines, one pixel
apart, centred on the line. Samples outside the image, or with a blank among
the pixels they interpolate, are NaN (``None`` in the answer).
"""
from __future__ import annotations

import math

import numpy as np

from .region_stats import _REMAP_BYTES, _cube_opener

#: Samples of a run read together (a window of about this many pixels a side).
_RUN = 256
#: Widest sampling width, in pixels.
MAX_WIDTH = 255
#: Most samples (pixels along the line, times the width) one profile takes:
#: bounds the arrays of a line that runs far beyond the image.
MAX_SAMPLES = 1 << 22


def _samples(points: list[tuple[float, float]], width: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """``(x, y)`` of the samples, each of shape ``(n, width)``, and the path
    length of every sample along the polyline (pixels)."""
    p = np.asarray(points, dtype=np.float64)
    seg = np.diff(p, axis=0)
    seg_len = np.hypot(seg[:, 0], seg[:, 1])
    total = float(seg_len.sum())
    n = max(1, math.ceil(total)) + 1 if total > 0 else 1
    cum = np.concatenate(([0.0], np.cumsum(seg_len)))
    s = np.linspace(0.0, total, n)
    x = np.interp(s, cum, p[:, 0])
    y = np.interp(s, cum, p[:, 1])
    # The perpendicular of the segment each sample is on (the first for a point).
    at = np.clip(np.searchsorted(cum, s, side="right") - 1, 0, len(seg) - 1)
    if total > 0:
        norm = np.where(seg_len[at] > 0, seg_len[at], 1.0)
        ux, uy = seg[at, 0] / norm, seg[at, 1] / norm
    else:
        ux, uy = np.ones(n), np.zeros(n)
    k = (np.arange(width) - (width - 1) / 2.0)[np.newaxis, :]
    return x[:, None] - uy[:, None] * k, y[:, None] + ux[:, None] * k, s


def _bilinear(window: np.ndarray, x0: int, y0: int, x: np.ndarray, y: np.ndarray) -> np.ndarray:
    xf, yf = np.floor(x).astype(int), np.floor(y).astype(int)
    tx, ty = x - xf, y - yf
    h, w = window.shape

    def at(ix, iy):
        return window[np.clip(iy - y0, 0, h - 1), np.clip(ix - x0, 0, w - 1)]

    # A neighbour with weight 0 (the sample sits exactly on a pixel) does not
    # count: a blank next to it must not blank the sample (0 * NaN is NaN).
    out = np.zeros(x.shape)
    for ix, iy, wt in ((xf, yf, (1 - tx) * (1 - ty)), (xf + 1, yf, tx * (1 - ty)),
                       (xf, yf + 1, (1 - tx) * ty), (xf + 1, yf + 1, tx * ty)):
        out += np.where(wt > 0, wt * at(ix, iy), 0.0)
    return out


def _offsets(wcs, x: np.ndarray, y: np.ndarray, along: np.ndarray) -> tuple[np.ndarray, str]:
    """The offset of each sample from the first along the line: arcsec on the
    sky (summed great-circle separations of the successive samples on the
    line's centre), else pixels."""
    if wcs is None:
        return along, "pix"
    lon, lat = (np.radians(a) for a in wcs.pixel_to_world_values(x, y))
    dlon, dlat = np.diff(lon), np.diff(lat)
    h = np.sin(dlat / 2) ** 2 + np.cos(lat[:-1]) * np.cos(lat[1:]) * np.sin(dlon / 2) ** 2
    step = np.degrees(2 * np.arcsin(np.sqrt(np.clip(h, 0.0, 1.0)))) * 3600.0
    along_step = np.diff(along)
    good = np.isfinite(step) & (along_step > 0)
    if not good.any():
        return along, "pix"
    # A step the projection cannot place (off the field) takes the scale of
    # the others, so the offsets stay finite and monotonic.
    scale = float(np.mean(step[good] / along_step[good]))
    step = np.where(np.isfinite(step), step, scale * along_step)
    return np.concatenate(([0.0], np.cumsum(step))), "arcsec"


def line_profile(path: str, hdu: int, channel: int, points: list[tuple[float, float]],
                 width: int = 1, wcs=None) -> dict:
    """The profile of plane ``channel`` along the polyline ``points`` (0-based
    pixels), ``width`` pixels wide."""
    if len(points) < 2:
        raise ValueError("A line needs two points.")
    if not 1 <= width <= MAX_WIDTH:
        raise ValueError(f"The sampling width must be 1..{MAX_WIDTH} pixels.")
    if not all(math.isfinite(c) for p in points for c in p):
        raise ValueError("The line has a point that is not finite.")
    length = float(np.hypot(*np.diff(np.asarray(points, dtype=np.float64), axis=0).T).sum())
    if (math.ceil(length) + 1) * width > MAX_SAMPLES:
        raise ValueError("The line is too long for one profile: shorten it or narrow its width.")
    x, y, along = _samples(points, width)
    open_source = _cube_opener(path, hdu)
    cube = open_source()
    mapped = 0
    try:
        nz, ny, nx = cube.shape
        if not 0 <= channel < nz:
            raise ValueError(f"channel {channel} outside 0..{nz - 1}")
        inside = (x >= 0) & (x <= nx - 1) & (y >= 0) & (y <= ny - 1)
        values = np.full(x.shape, np.nan)
        for a in range(0, x.shape[0], _RUN):
            xs, ys, ok = x[a:a + _RUN], y[a:a + _RUN], inside[a:a + _RUN]
            if not ok.any():
                continue
            x_lo, x_hi = int(np.floor(xs[ok].min())), int(np.ceil(xs[ok].max()))
            y_lo, y_hi = int(np.floor(ys[ok].min())), int(np.ceil(ys[ok].max()))
            x_hi, y_hi = min(x_hi + 1, nx - 1), min(y_hi + 1, ny - 1)
            if mapped > _REMAP_BYTES and not open_source.whole:
                cube.close()
                cube = open_source()
                mapped = 0
            window = cube.block(channel, channel + 1, y_lo, y_hi + 1, x_lo, x_hi + 1,
                                dtype=np.float64)[0]
            mapped += (y_hi - y_lo + 1) * nx * 4
            v = _bilinear(window, x_lo, y_lo, xs, ys)
            values[a:a + _RUN] = np.where(ok, v, np.nan)
    finally:
        cube.close()
    valid = np.isfinite(values)
    counted = valid.sum(axis=1)
    profile = np.where(counted > 0, np.where(valid, values, 0.0).sum(axis=1)
                       / np.maximum(counted, 1), np.nan)
    # The line itself: the mean of the parallel samples (their middle, whatever
    # the parity of the width).
    cx, cy = x.mean(axis=1), y.mean(axis=1)
    offsets, unit = _offsets(wcs, cx, cy, along)
    finite = np.isfinite(profile)
    return {
        "offsets": offsets.tolist(), "offset_unit": unit,
        "values": [float(v) if np.isfinite(v) else None for v in profile],
        "x": cx.tolist(), "y": cy.tolist(),
        "n_samples": int(profile.size), "n_valid": int(finite.sum()),
        "length_pix": float(along[-1]), "width_pix": int(width),
        "min": float(profile[finite].min()) if finite.any() else None,
        "max": float(profile[finite].max()) if finite.any() else None,
        "mean": float(profile[finite].mean()) if finite.any() else None,
    }
