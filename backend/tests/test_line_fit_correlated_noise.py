"""Gaussian line-fit errors under spectrally correlated noise (R-23).

``curve_fit`` assumes independent samples. Real HI cubes frequently violate that
— HGC 44 in validation/ has a lag-1 channel autocorrelation of 0.74 — and the
consequence is silent: every ``*_err`` the fit returns comes back too small, so a
line looks better measured than it is.

The correction is a variance inflation by the measured effective-sample factor
F = 1 + 2·Σρ_k. These tests pin what actually matters about it: that it does
nothing when the noise is white, that it grows with correlation, and — the point
of the whole exercise — that the corrected error bars have roughly the right
coverage, checked by Monte Carlo against the real scatter of recovered
parameters rather than against a formula.
"""

from __future__ import annotations

import numpy as np
import pytest
from scipy import ndimage

from app.compute.spectral import effective_sample_factor, worker_line_fit

N_CHAN = 128
AMP, MU, SIG, NOISE = 1.0, 64.0, 6.0, 0.08
AXIS = np.arange(N_CHAN, dtype=float)
CLEAN = AMP * np.exp(-((AXIS - MU) ** 2) / (2.0 * SIG * SIG))


def correlated_noise(rng: np.random.Generator, smooth: float) -> np.ndarray:
    """White noise smoothed to a chosen correlation length, renormalised so the
    per-channel sigma is the same however correlated it is — otherwise a coverage
    test would confound correlation with amplitude."""
    w = rng.normal(0.0, 1.0, N_CHAN)
    if smooth > 0:
        w = ndimage.gaussian_filter1d(w, smooth, mode="wrap")
        w /= w.std()
    return NOISE * w


def fit(y: np.ndarray) -> dict:
    return worker_line_fit(y.tolist(), AXIS.tolist(), with_baseline=True)


# ── the factor itself ────────────────────────────────────────────────────────

def test_white_noise_gives_no_inflation():
    rng = np.random.default_rng(1)
    for _ in range(20):
        assert effective_sample_factor(rng.normal(0.0, 1.0, 512)) == pytest.approx(1.0, abs=0.35)


def test_factor_grows_with_correlation():
    rng = np.random.default_rng(2)
    previous = 0.0
    for smooth in (0.0, 0.8, 1.5, 2.5):
        vals = [effective_sample_factor(correlated_noise(rng, smooth)) for _ in range(40)]
        current = float(np.median(vals))
        assert current > previous, f"smoothing {smooth} did not raise F"
        previous = current
    assert previous > 3.0, "heavily smoothed noise should give a large factor"


def test_factor_is_at_least_one_and_finite_on_degenerate_input():
    # Fewer than 8 points, all-zero, and constant residuals must not produce a
    # factor below 1 (which would SHRINK the error bars) or a NaN.
    for bad in (np.zeros(4), np.zeros(64), np.full(64, 3.0)):
        f = effective_sample_factor(bad)
        assert np.isfinite(f) and f >= 1.0


# ── what it does to a fit ────────────────────────────────────────────────────

def test_uncorrelated_fit_is_unchanged():
    """The correction must be a no-op on white noise, or it would silently inflate
    every existing result."""
    rng = np.random.default_rng(3)
    r = fit(CLEAN + correlated_noise(rng, 0.0))
    assert r["spectral_correlation_factor"] == pytest.approx(1.0, abs=0.5)


def test_correlated_fit_reports_larger_errors_than_white():
    rng = np.random.default_rng(4)
    white = [fit(CLEAN + correlated_noise(rng, 0.0))["integrated_flux_err"] for _ in range(30)]
    corr = [fit(CLEAN + correlated_noise(rng, 1.5))["integrated_flux_err"] for _ in range(30)]
    assert np.median(corr) > 1.5 * np.median(white)


def test_factor_is_reported_so_a_user_can_audit_it():
    rng = np.random.default_rng(5)
    r = fit(CLEAN + correlated_noise(rng, 1.5))
    assert r["spectral_correlation_factor"] > 2.0


# ── coverage: the only test that really matters ──────────────────────────────

def test_a_large_factor_carries_a_model_caveat():
    """A big inflation can mean correlated channels OR a model that does not fit,
    and this estimator cannot tell them apart — so it must not be presented as a
    settled result."""
    rng = np.random.default_rng(6)
    # White noise must never trip it: the threshold (F >= 2.25, a 1.5x inflation)
    # sits well above the finite-sample scatter of F on an uncorrelated spectrum.
    for _ in range(30):
        assert fit(CLEAN + correlated_noise(rng, 0.0))["spectral_correlation_note"] == ""
    loud = fit(CLEAN + correlated_noise(rng, 2.5))
    assert "model that does not fit" in loud["spectral_correlation_note"]
    # ...and it must fire while the inflation is still modest, not only once the
    # errors have already been doubled.
    mild = fit(CLEAN + correlated_noise(rng, 0.8))
    assert mild["spectral_correlation_factor"] >= 2.25
    assert mild["spectral_correlation_note"]


@pytest.mark.parametrize("smooth", [0.0, 1.5])
def test_reported_errors_match_the_actual_scatter(smooth):
    """Monte Carlo: refit many noise realisations and compare the reported 1σ with
    the true scatter of the recovered parameters.

    This is the check the correction exists to pass. Before it, ρ₁ = 0.89 noise
    gave errors ~2.3× too small; the tolerance below would have failed by a wide
    margin. The band is generous (0.6–1.6) because 150 realisations leave real
    Monte-Carlo scatter and F recovered from residuals is itself slightly low
    (measured: every parameter lands at 1.00–1.16 once corrected) — it is
    calibrated to catch a factor-of-two error, not a 10 % one.
    """
    rng = np.random.default_rng(20260904)
    # EVERY error the fit reports, not just two: one scalar factor scales the whole
    # covariance, and a scalar is not guaranteed to be right for each parameter of
    # a non-linear fit — each has its own channel weighting. So check them all.
    params = {"peak": "peak_err", "center": "center_err", "sigma": "sigma_err",
              "fwhm": "fwhm_err", "integrated_flux": "integrated_flux_err"}
    got: dict[str, list[float]] = {k: [] for k in params}
    errs: dict[str, list[float]] = {k: [] for k in params}
    for _ in range(150):
        r = fit(CLEAN + correlated_noise(rng, smooth))
        if not (0.5 < r["sigma"] < N_CHAN / 4) or abs(r["peak"]) > 5 * AMP:
            continue          # a runaway fit is not a measurement
        for key, ekey in params.items():
            got[key].append(r[key])
            if r[ekey]:
                errs[key].append(r[ekey])
    assert len(got["peak"]) > 120, "the fixture should fit almost every realisation"

    for key in params:
        actual = float(np.std(got[key], ddof=1))
        reported = float(np.median(errs[key]))
        ratio = actual / reported
        assert 0.6 < ratio < 1.6, (
            f"{key} error bars are off by {ratio:.2f}x at smoothing {smooth} "
            f"(actual scatter {actual:.4g}, reported {reported:.4g})")
