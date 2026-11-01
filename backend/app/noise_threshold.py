"""The default threshold of a 3-D view, from the cube's noise (issue #93).

A threshold that lets noise through draws a fog; one chosen as a fixed number
of standard deviations lets through a number of noise voxels that grows with
the cube (3σ passes 0.13 % of a cube's noise voxels: 100 000 in a 77 M-voxel
cube). The default here instead fixes the NUMBER of noise voxels expected
above it, ``NOISE_VOXELS`` (and at most ``MAX_NOISE_FRACTION`` of a small
cube), so the cut rises with the size of the cube.

The noise is read from the negative side of the cube. Emission is positive,
so the values below the median are noise alone: the threshold mirrors their
tail, as many voxels lying above it as the noise puts below the mirror point.
On Gaussian noise this is ``median + Φ⁻¹(1 − NOISE_VOXELS / N) · σ``; on
heavier-tailed noise it rises by itself. Two cases are left to the caller's
own default (``None`` is returned): data that is not noise around a median
(a constant cube, or a skewed positive distribution such as a simulation's
density), and a cube without a lower side to read.

Measured on HCG 44 and WALLABY against their reference masks, and on an
APERTIF cube with heavy-tailed noise: ``validation/threshold_from_noise.py``.
"""
from __future__ import annotations

import numpy as np
from scipy.stats import norm

#: Noise voxels expected above the default threshold, in the whole cube.
NOISE_VOXELS = 100.0

#: ...and at most this fraction of the cube's voxels: in a small cube 100
#: noise voxels would be a visible share of it (10 % of a 10³ cube).
MAX_NOISE_FRACTION = 1e-4

#: The smallest lower tail, in sample voxels, whose quantile is read directly;
#: rarer fractions are extrapolated from it along a Gaussian tail.
_TAIL_SAMPLES = 20

#: The mirrored tail may raise the cut above the Gaussian one by this factor
#: at most: heavy-tailed noise needs it (×1.43 on the APERTIF cube), while
#: negative artefacts (absorption, sidelobes) would raise it without bound.
_MAX_TAIL_EXCESS = 1.6

#: Noise is symmetric about the median. Above this ratio of the upper to the
#: lower spread the data is not noise around a median, and the rule does not
#: apply.
_MAX_ASYMMETRY = 1.5

#: σ from the median absolute deviation of a Gaussian.
_MAD_TO_SIGMA = 1.4826


def noise_threshold(sample: np.ndarray, n_voxels: int,
                    noise_voxels: float = NOISE_VOXELS) -> float | None:
    """The threshold above which about ``noise_voxels`` noise voxels are
    expected in a cube of ``n_voxels``, estimated from ``sample`` (any
    unbiased sample of the cube's voxels: a stride, a set of bricks).

    ``None`` when the data does not look like noise around a median; the
    caller then keeps a default of its own.
    """
    s = np.asarray(sample).ravel()
    s = s[np.isfinite(s)]
    quantized = s.size > 0 and _quantized(s)
    # Exact zeros are blanking (or slabs not read yet), not noise -- unless the
    # data is quantized (integer counts), where 0 is the commonest noise value.
    if s.size and np.count_nonzero(s == 0.0) > 0.01 * s.size and not quantized:
        s = s[s != 0.0]
    if s.size < 1000 or n_voxels <= 0:
        return None
    med = float(np.median(s))
    lower = med - s[s < med]
    upper = s[s > med] - med
    if lower.size < 100 or upper.size < 100:
        return None
    if quantized:
        # On a few levels the median absolute deviation is a whole step (σ of
        # 1 count reads as 1.48): the power of each side, counted over all the
        # values (half of the noise lies on each side), is not.
        sigma = float(np.sqrt(2.0 * np.sum(lower.astype(np.float64) ** 2) / s.size))
        sigma_up = float(np.sqrt(2.0 * np.sum(upper.astype(np.float64) ** 2) / s.size))
    else:
        sigma = _MAD_TO_SIGMA * float(np.median(lower))
        sigma_up = _MAD_TO_SIGMA * float(np.median(upper))
    if not sigma > 0.0 or sigma_up > _MAX_ASYMMETRY * sigma:
        return None

    f = min(MAX_NOISE_FRACTION, max(noise_voxels, 1.0) / float(n_voxels))
    k_gauss = float(norm.isf(f))
    # The mirror of the lower tail, read where the sample can measure it and
    # carried out to ``f`` along a Gaussian tail.
    f0 = min(0.25, max(f, _TAIL_SAMPLES / s.size))
    k0 = (med - float(np.quantile(s, f0))) / sigma
    k = k0 * k_gauss / float(norm.isf(f0))
    k = min(max(k, k_gauss), _MAX_TAIL_EXCESS * k_gauss)
    return med + k * sigma


def _quantized(s: np.ndarray) -> bool:
    """Whether the values take few distinct levels (integer counts, or BSCALE
    data whose noise spans a few steps): then exact zeros are noise."""
    head = s[: 100_000]
    return np.unique(head).size < 0.5 * head.size


def default_threshold(sample: np.ndarray, n_voxels: int) -> float:
    """The noise threshold of ``sample`` (see :func:`noise_threshold`), or its
    99th percentile where the data is not noise around a median."""
    t = noise_threshold(sample, n_voxels)
    if t is not None:
        return t
    s = np.asarray(sample).ravel()
    s = s[np.isfinite(s)]
    return float(np.percentile(s, 99.0)) if s.size else 0.0


def stride_for(n_voxels: int, target: int = 5_000_000) -> int:
    """The stride along each axis that samples about ``target`` voxels
    (between a third and three times as many: the tails need the voxels)."""
    return max(1, round((n_voxels / target) ** (1.0 / 3.0)))
