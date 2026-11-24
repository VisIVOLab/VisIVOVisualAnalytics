"""The noise of a continuum image in one action (issue #503): the RMS and the
robust σ over the pixels with no source in them.

Two ways to say which pixels those are:

* **background regions** the user chose: each is measured at full resolution
  (``region_stats.region_statistics``) and the answer is the average of the
  regions' values, with every region's own beside it, so that a region sitting
  on faint emission shows as the odd one out;
* **automatically**: the whole plane, sigma-clipped. A first pass over the file
  in blocks of rows draws a uniform random sample of at most ``_SAMPLE_VALUES``
  pixels (all of them, when the plane is smaller) and counts the rest; the clip
  (median centre, standard deviation, as ``astropy.stats.sigma_clip``) iterates
  on that sample, which is cheap and bounded whatever the plane's size; a second
  pass over the file measures the window the clip converged to exactly. The
  memory is the sample, never the plane (a 10⁴ × 10⁴ image reads in blocks).

RMS is √⟨x²⟩ over the pixels (what CASA ``imstat`` calls rms, zero-centred) and
σ(MAD) is 1.4826 × the median absolute deviation, the two numbers a radio
astronomer quotes for the noise of a map.
"""
from __future__ import annotations

import math

import numpy as np

from .region_stats import (
    _MAD_TO_SIGMA, _SAMPLE_VALUES, Region, _value_blocks, region_statistics,
)

#: Iterations of the clip on the sample (it settles in a handful).
_MAX_ITERATIONS = 20


def _clip(sample: np.ndarray, clip: float) -> tuple[float, float, np.ndarray, int]:
    """Sigma-clip ``sample``: the centre (median of what is kept), σ (its
    standard deviation), the kept mask and the iterations used."""
    keep = np.ones(sample.size, dtype=bool)
    centre = float(np.median(sample))
    sigma = float(sample.std())
    iterations = 0
    for iterations in range(1, _MAX_ITERATIONS + 1):
        window = np.abs(sample - centre) <= clip * sigma
        if window.sum() < 2 or np.array_equal(window, keep):
            break
        keep = window
        kept = sample[keep]
        centre, sigma = float(np.median(kept)), float(kept.std())
        if sigma == 0.0:
            break
    # The window the last (centre, sigma) defines: what the exact pass measures,
    # so the sample's kept set is that window even when the iterations ran out.
    final = np.abs(sample - centre) <= clip * sigma
    if final.sum() >= 2:
        keep = final
    return centre, sigma, keep, iterations


def auto_noise(path: str, hdu: int, channel: int = 0, clip: float = 3.0) -> dict:
    """The sigma-clipped noise of the whole plane ``channel`` of ``path``."""
    if not clip > 0:
        raise ValueError("The clip must be a positive number of sigma.")
    read = _value_blocks(path, hdu, Region({"shape": "image"}), channel)
    rng = np.random.default_rng(0)
    total = n_valid = 0
    sample: list[np.ndarray] = []
    for values in read():
        total += values.size
        finite = values[np.isfinite(values)]
        n_valid += finite.size
        if not finite.size:
            continue
        # Every pixel of a plane that fits in the sample; else each one with
        # the same probability, so the sample stays uniform over the plane.
        if read.bound_pixels <= _SAMPLE_VALUES:
            sample.append(finite)
        else:
            draws = rng.binomial(finite.size, _SAMPLE_VALUES / read.bound_pixels)
            if draws:
                sample.append(finite[rng.choice(finite.size, draws, replace=False)])
    if not n_valid:
        return {"mode": "auto", "total_count": total, "valid_count": 0}
    exact = read.bound_pixels <= _SAMPLE_VALUES
    values = np.concatenate(sample)
    centre, sigma, keep, iterations = _clip(values, clip)
    kept = values[keep]
    if exact:
        n_used, mean = kept.size, float(kept.mean())
        rms_sq = float(np.dot(kept, kept)) / kept.size
        stddev = float(kept.std())
    else:
        # The window the clip settled on, measured over the whole plane.
        lo, hi = centre - clip * sigma, centre + clip * sigma
        n_used = 0
        s = s2 = 0.0
        for block in read():
            v = block[np.isfinite(block)]
            v = v[(v >= lo) & (v <= hi)]
            n_used += v.size
            s += float(v.sum())
            s2 += float(np.dot(v, v))
        if not n_used:
            raise ValueError("No pixel is left inside the clipping window.")
        mean = s / n_used
        rms_sq = s2 / n_used
        stddev = math.sqrt(max(0.0, rms_sq - mean * mean))
    median = float(np.median(kept))
    mad = float(np.median(np.abs(kept - median)))
    return {
        "mode": "auto", "total_count": total, "valid_count": n_valid, "used_count": n_used,
        "clip_sigma": clip, "iterations": iterations,
        "rms": math.sqrt(rms_sq), "stddev": stddev, "mad_sigma": _MAD_TO_SIGMA * mad,
        "mean": mean, "median": median,
        "exact": exact, "sample_count": int(values.size),
    }


def region_noise(path: str, regions: list[dict], hdu: int, channel: int = 0) -> dict:
    """The noise over background ``regions`` (canonical image-frame dicts):
    their values averaged (a plain mean of the regions' RMS, and of their
    σ(MAD)), each region's own listed. Here ``rms`` is √⟨x²⟩; the statistics
    route's ``rms`` is the standard deviation under its older name."""
    per_region = []
    for spec in regions:
        stats = region_statistics(path, Region(spec), hdu, channel)
        entry = {"shape": str(spec.get("shape", "")), "name": str(spec.get("name", "")),
                 "total_count": stats["total_count"], "valid_count": stats["valid_count"]}
        if stats["valid_count"] >= 2:
            entry.update({"rms": stats["rms_unweighted"], "stddev": stats["stddev"],
                          "mad_sigma": stats["mad_sigma"], "mean": stats["mean"],
                          "median": stats["median"]})
        per_region.append(entry)
    usable = [e for e in per_region if "rms" in e]
    if not usable:
        raise ValueError("The background regions hold no valid pixel to measure.")
    rms = np.array([e["rms"] for e in usable])
    mad = np.array([e["mad_sigma"] for e in usable])
    return {
        "mode": "regions", "regions": per_region, "used_regions": len(usable),
        "valid_count": sum(e["valid_count"] for e in usable),
        "rms": float(rms.mean()), "mad_sigma": float(mad.mean()),
        # How much the regions disagree: a region on faint emission stands out.
        "rms_scatter": float(rms.std(ddof=1)) if len(usable) > 1 else None,
        "mad_sigma_scatter": float(mad.std(ddof=1)) if len(usable) > 1 else None,
    }
