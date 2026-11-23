"""The Gaussian line fit starts its width from the data and keeps the best of
several starts (issue #416).

Started at span/6 (about 33 km/s), the fit of the CHIMPS C18O 3-2 Clump
spectrum settled on a negative 8 km/s dip at 54 km/s instead of the 1.9 km/s
line at 84 km/s, and which minimum won flipped with one-pixel region shifts.
The reference values are the radio-astronomy check of that spectrum: peak
0.62 +- 0.09 K, centre 83.98 +- 0.14 km/s, FWHM 1.86 +- 0.33 km/s, integral
1.22 +- 0.19 K km/s.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from app.compute.spectral import worker_line_fit

_SPECTRUM = Path(__file__).parent / "data" / "chimps_c18o_clump_spectrum.csv"


def _clump():
    v, ta = np.loadtxt(_SPECTRUM, delimiter=",", unpack=True)
    return v, ta


def test_clump_spectrum_finds_the_emission_line():
    v, ta = _clump()
    assert v.size == 400
    fit = worker_line_fit(ta.tolist(), v.tolist(), with_baseline=True)
    assert fit["peak"] > 0
    assert fit["peak"] == pytest.approx(0.62, abs=0.09)
    assert fit["center"] == pytest.approx(84.0, abs=0.3)
    assert 1.5 <= fit["fwhm"] <= 2.3
    assert fit["integrated_flux"] == pytest.approx(1.22, abs=0.19)


def test_clump_spectrum_fit_window_gives_the_same_line():
    v, ta = _clump()
    fit = worker_line_fit(ta.tolist(), v.tolist(), True, x_min=100.0, x_max=70.0)
    assert fit["n_points"] == int(np.sum((v >= 70.0) & (v <= 100.0)))
    assert fit["center"] == pytest.approx(84.0, abs=0.3)
    assert 1.5 <= fit["fwhm"] <= 2.3


def test_broad_line_still_found():
    x = np.linspace(-100.0, 100.0, 401)
    rng = np.random.default_rng(7)
    y = 0.5 + 2.0 * np.exp(-((x - 10.0) ** 2) / (2 * 25.0 ** 2)) + rng.normal(0, 0.05, x.size)
    fit = worker_line_fit(y.tolist(), x.tolist(), with_baseline=True)
    assert fit["center"] == pytest.approx(10.0, abs=1.0)
    assert fit["sigma"] == pytest.approx(25.0, rel=0.1)


def test_absorption_line_keeps_its_sign():
    x = np.linspace(0.0, 200.0, 401)
    rng = np.random.default_rng(11)
    y = 1.0 - 0.6 * np.exp(-((x - 120.0) ** 2) / (2 * 1.0 ** 2)) + rng.normal(0, 0.05, x.size)
    fit = worker_line_fit(y.tolist(), x.tolist(), with_baseline=True)
    assert fit["peak"] < 0
    assert fit["center"] == pytest.approx(120.0, abs=0.3)


def test_one_sided_and_invalid_windows():
    v, ta = _clump()
    fit = worker_line_fit(ta.tolist(), v.tolist(), True, x_min=60.0)
    assert fit["n_points"] == int(np.sum(v >= 60.0))
    with pytest.raises(ValueError, match="finite"):
        worker_line_fit(ta.tolist(), v.tolist(), True, x_min=float("nan"))


def test_edge_spike_is_flagged_not_trusted():
    v, ta = _clump()
    spiked = ta.copy()
    spiked[0] += 50.0
    fit = worker_line_fit(spiked.tolist(), v.tolist(), True)
    assert fit["fit_warning"]
    clean = worker_line_fit(ta.tolist(), v.tolist(), True)
    assert clean["fit_warning"] == ""


def test_broad_line_beats_a_single_spike():
    x = np.linspace(-100.0, 100.0, 401)
    found = 0
    for seed in range(20):
        rng = np.random.default_rng(seed)
        y = np.exp(-((x - 10.0) ** 2) / (2 * 40.0 ** 2)) + rng.normal(0, 0.25, x.size)
        y[rng.integers(0, x.size)] += 2.0
        fit = worker_line_fit(y.tolist(), x.tolist(), True)
        found += fit["peak"] > 0 and abs(fit["center"] - 10.0) < 10.0
    assert found >= 18


def test_descending_frequency_axis_with_window():
    v, ta = _clump()
    rest_hz = 329.3305453e9
    nu = rest_hz * (1.0 - v / 299792.458)
    lo, hi = rest_hz * (1.0 - 100.0 / 299792.458), rest_hz * (1.0 - 70.0 / 299792.458)
    fit = worker_line_fit(ta.tolist(), nu.tolist(), True, x_min=hi, x_max=lo)
    centre_kms = (1.0 - fit["center"] / rest_hz) * 299792.458
    assert centre_kms == pytest.approx(84.0, abs=0.3)
    assert fit["n_points"] == int(np.sum((v >= 70.0) & (v <= 100.0)))
