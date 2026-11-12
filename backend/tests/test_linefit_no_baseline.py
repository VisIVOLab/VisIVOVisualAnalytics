"""worker_line_fit with with_baseline=False fits only the 3 Gaussian parameters.

The model used to keep the two baseline terms as free parameters that it then
ignored, so the Jacobian had two zero columns: with 4 samples curve_fit refused
(5 parameters > 4 points) and with more samples the covariance was singular, so
every uncertainty came back unavailable.
"""
from __future__ import annotations

import numpy as np
import pytest

from app.compute.spectral import worker_line_fit


def _gauss(x, amp, mu, sigma):
    return amp * np.exp(-((x - mu) ** 2) / (2.0 * sigma ** 2))


def test_four_samples_fit_without_baseline():
    x = np.array([-3.0, -1.0, 1.0, 3.0])
    y = _gauss(x, 5.0, 0.3, 1.5) + np.array([0.01, -0.02, 0.015, -0.01])
    out = worker_line_fit(y.tolist(), x.tolist(), with_baseline=False)
    assert out["peak"] == pytest.approx(5.0, rel=0.05)
    assert out["center"] == pytest.approx(0.3, abs=0.05)
    assert out["sigma"] == pytest.approx(1.5, rel=0.05)
    assert out["baseline"] == [0.0, 0.0]
    assert out["continuum"] == 0.0
    assert out["equivalent_width"] is None


def test_noisy_fit_without_baseline_has_finite_errors():
    rng = np.random.default_rng(3)
    x = np.linspace(-20.0, 20.0, 41)
    y = _gauss(x, 4.0, 1.0, 3.0) + rng.normal(0.0, 0.2, x.size)
    out = worker_line_fit(y.tolist(), x.tolist(), with_baseline=False)
    for key in ("peak_err", "center_err", "sigma_err", "fwhm_err",
                "integrated_flux_err"):
        assert out[key] is not None and np.isfinite(out[key]) and out[key] > 0, key
    assert out["peak"] == pytest.approx(4.0, abs=4 * out["peak_err"])
    assert out["baseline"] == [0.0, 0.0]


def test_too_few_samples_still_rejected():
    with pytest.raises(ValueError, match="at least"):
        worker_line_fit([1.0, 2.0, 1.0], [0.0, 1.0, 2.0], with_baseline=False)
    # With a baseline the model has 5 parameters; 5 points would leave zero
    # degrees of freedom (an interpolating fit with every error unavailable).
    with pytest.raises(ValueError, match="at least 6"):
        worker_line_fit([0.1, 1.0, 2.0, 1.0, 0.2], [0.0, 1.0, 2.0, 3.0, 4.0],
                        with_baseline=True)


def test_six_samples_with_baseline_give_errors():
    x = np.array([-5.0, -3.0, -1.0, 1.0, 3.0, 5.0])
    y = 1.0 + _gauss(x, 3.0, 0.0, 1.5) + np.array([0.05, -0.03, 0.02, 0.04, -0.06, 0.01])
    out = worker_line_fit(y.tolist(), x.tolist(), with_baseline=True)
    assert out["peak_err"] is not None and out["peak_err"] > 0


def test_negative_width_solution_keeps_the_flux_error(monkeypatch):
    """The model depends on sigma² only: a fit that lands on -sigma must report
    the same integrated-flux error as the +sigma fit (the amplitude-width
    covariance flips sign with sigma)."""
    import scipy.optimize as so

    rng = np.random.default_rng(5)
    x = np.linspace(-20.0, 20.0, 81)
    y = _gauss(x, 4.0, 1.0, 3.0) + rng.normal(0.0, 0.2, x.size)
    ref = worker_line_fit(y.tolist(), x.tolist(), with_baseline=False)

    real = so.curve_fit

    def negative_width(f, xdata, ydata, p0=None, **kw):
        p0 = list(p0)
        p0[2] = -abs(p0[2])
        return real(f, xdata, ydata, p0=p0, **kw)

    monkeypatch.setattr(so, "curve_fit", negative_width)
    neg = worker_line_fit(y.tolist(), x.tolist(), with_baseline=False)
    assert neg["sigma"] == pytest.approx(ref["sigma"], rel=1e-6)
    assert neg["integrated_flux"] == pytest.approx(ref["integrated_flux"], rel=1e-6)
    assert neg["integrated_flux_err"] == pytest.approx(ref["integrated_flux_err"], rel=1e-4)
