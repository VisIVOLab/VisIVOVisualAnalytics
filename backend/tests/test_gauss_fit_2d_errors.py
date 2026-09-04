"""2-D Gaussian source-fit uncertainties under beam-correlated noise (R-24).

`curve_fit`'s covariance assumes independent pixels. An interferometric image is
the opposite: noise is correlated across the beam, and the measured consequence
is error bars 3–8x too small — worse for the integrated flux, which compounds
through the amp–sigma_x–sigma_y covariance.

Two analytic corrections were measured and rejected before the empirical one was
written (beam-area scaling under-predicts; summing the residual autocorrelation
over the lag plane over-corrects and inflates even WHITE noise by 1.4x). So the
error is measured by injecting the fitted model into source-free parts of the
same image and refitting.

These tests pin the coverage — reported 1-sigma against the true scatter of
recovered parameters — because "the code runs" says nothing about whether an
error bar is the right size.
"""

from __future__ import annotations

import numpy as np
import pytest
from scipy import ndimage
from scipy.optimize import curve_fit

from app.compute.photometry import _gaussian_2d, empirical_fit_errors

WIN = 41                      # fitting window, pixels
FIELD = 220                   # image the noise is drawn from
TRUTH = (1.0, 20.0, 20.0, 4.0, 4.0, 0.0, 0.0)   # amp, x0, y0, sx, sy, theta, bg
NOISE = 0.05
YY, XX = np.mgrid[0:WIN, 0:WIN]
MODEL = _gaussian_2d((XX.ravel(), YY.ravel()), *TRUTH).reshape(WIN, WIN)


def noise_field(rng: np.random.Generator, beam_fwhm: float) -> np.ndarray:
    """A noise image with the requested beam-scale correlation, renormalised so
    the per-pixel sigma is the same however correlated it is."""
    n = rng.normal(0.0, 1.0, (FIELD, FIELD))
    if beam_fwhm > 0:
        n = ndimage.gaussian_filter(n, beam_fwhm / 2.3548, mode="wrap")
        n = n / n.std()
    return NOISE * n


def fit_window(window: np.ndarray):
    try:
        p, _ = curve_fit(_gaussian_2d, (XX.ravel(), YY.ravel()), window.ravel(),
                         p0=list(TRUTH), maxfev=5000)
    except Exception:  # noqa: BLE001
        return None
    if abs(p[0]) > 5.0 or not (1.0 < abs(p[3]) < 15.0):
        return None       # a runaway refit is not a noise realisation
    return p


def true_scatter(rng: np.random.Generator, beam_fwhm: float, n: int = 150):
    """Ground truth: refit many independent realisations of the same source."""
    amps, integrated = [], []
    for _ in range(n):
        window = MODEL + noise_field(rng, beam_fwhm)[:WIN, :WIN]
        p = fit_window(window)
        if p is None:
            continue
        amps.append(p[0])
        integrated.append(2.0 * np.pi * p[0] * abs(p[3]) * abs(p[4]))
    return float(np.std(amps, ddof=1)), float(np.std(integrated, ddof=1))


def measure(rng: np.random.Generator, beam_fwhm: float):
    cov, n = empirical_fit_errors(noise_field(rng, beam_fwhm), MODEL, list(TRUTH),
                                  fit_window, rng, n_trials=60)
    assert cov is not None, "an empty field should always yield clean placements"
    amp_err = float(np.sqrt(cov[0, 0]))
    # Integrated flux propagated through the amp-sx-sy block, as the worker does.
    integrated = 2.0 * np.pi * TRUTH[0] * TRUTH[3] * TRUTH[4]
    idx = np.ix_([0, 3, 4], [0, 3, 4])
    jac = np.array([integrated / TRUTH[0], integrated / TRUTH[3], integrated / TRUTH[4]])
    int_err = float(np.sqrt(jac @ cov[idx] @ jac))
    return amp_err, int_err, n


# ── coverage: the reason this estimator exists ───────────────────────────────

@pytest.mark.parametrize("beam_fwhm", [0.0, 4.0])
def test_empirical_errors_match_the_true_scatter(beam_fwhm):
    rng = np.random.default_rng(20260904)
    true_amp, true_int = true_scatter(rng, beam_fwhm)
    amp_err, int_err, n = measure(rng, beam_fwhm)
    assert n >= 40
    for measured, truth, name in ((amp_err, true_amp, "amplitude"),
                                  (int_err, true_int, "integrated flux")):
        ratio = measured / truth
        assert 0.6 < ratio < 1.6, (
            f"{name} error is {ratio:.2f}x the true scatter at beam {beam_fwhm} px "
            f"(measured {measured:.4g}, true {truth:.4g})")


def test_covariance_errors_are_the_thing_being_fixed():
    """The baseline this replaces: on beam-correlated noise curve_fit's own
    covariance is several times too small. If this ever stops being true the
    empirical estimator has lost its justification."""
    rng = np.random.default_rng(7)
    true_amp, _ = true_scatter(rng, 4.0, n=120)
    reported = []
    for _ in range(60):
        window = MODEL + noise_field(rng, 4.0)[:WIN, :WIN]
        try:
            _, pcov = curve_fit(_gaussian_2d, (XX.ravel(), YY.ravel()), window.ravel(),
                                p0=list(TRUTH), maxfev=5000)
        except Exception:  # noqa: BLE001
            continue
        reported.append(float(np.sqrt(pcov[0, 0])))
    assert true_amp / np.median(reported) > 2.5, (
        "curve_fit's covariance should be badly optimistic on correlated noise")


def test_errors_grow_with_beam_correlation():
    rng = np.random.default_rng(8)
    previous = 0.0
    for beam_fwhm in (0.0, 2.5, 4.0):
        amp_err, _, _ = measure(rng, beam_fwhm)
        assert amp_err > previous, f"beam {beam_fwhm} px did not widen the error"
        previous = amp_err


# ── refusing to answer ───────────────────────────────────────────────────────

def test_a_crowded_field_yields_no_estimate_rather_than_a_wrong_one():
    """Every candidate position covered by emission → None, so the caller reports
    the fallback instead of a scatter measured on other people's sources."""
    rng = np.random.default_rng(9)
    field = noise_field(rng, 0.0)
    # Genuinely crowded: sources on a grid closer together than the window, so no
    # 41x41 box is free of one. A constant pedestal would NOT do — it moves the
    # median with it and leaves the field statistically empty.
    gy, gx = np.mgrid[0:FIELD, 0:FIELD]
    for cy in range(15, FIELD, 25):
        for cx in range(15, FIELD, 25):
            field += 1.0 * np.exp(-(((gx - cx) ** 2 + (gy - cy) ** 2) / (2.0 * 16.0)))
    cov, n = empirical_fit_errors(field, MODEL, list(TRUTH), fit_window, rng,
                                  n_trials=30)
    assert cov is None and n < 20


def test_window_larger_than_the_image_is_refused():
    rng = np.random.default_rng(10)
    tiny = np.zeros((10, 10))
    cov, n = empirical_fit_errors(tiny, MODEL, list(TRUTH), fit_window, rng)
    assert cov is None and n == 0


def test_degenerate_image_is_refused():
    rng = np.random.default_rng(11)
    for bad in (np.zeros((FIELD, FIELD)),                    # rms = 0
                np.full((FIELD, FIELD), np.nan)):            # nothing finite
        cov, _ = empirical_fit_errors(bad, MODEL, list(TRUTH), fit_window, rng)
        assert cov is None


def test_result_is_reproducible_for_the_same_seed():
    """A user refitting the same source must not see the error bar move."""
    field = noise_field(np.random.default_rng(12), 2.5)
    a, _ = empirical_fit_errors(field, MODEL, list(TRUTH), fit_window,
                                np.random.default_rng(1), n_trials=30)
    b, _ = empirical_fit_errors(field, MODEL, list(TRUTH), fit_window,
                                np.random.default_rng(1), n_trials=30)
    np.testing.assert_allclose(a, b)


def test_returns_a_full_covariance_not_just_a_diagonal():
    """The integrated flux is propagated through the off-diagonals; a diagonal
    matrix would silently drop the amp-width correlation that correlated noise
    makes worst."""
    rng = np.random.default_rng(13)
    cov, _ = empirical_fit_errors(noise_field(rng, 4.0), MODEL, list(TRUTH),
                                  fit_window, rng, n_trials=60)
    assert cov.shape == (len(TRUTH), len(TRUTH))
    off = cov[0, 3]
    assert abs(off) > 0.0, "amp and sigma_x should be measurably correlated"


# ── the matched-filter rejection, which replaced a bright-pixel fraction ─────

def contaminated_field(rng, beam_fwhm, src_fwhm, snr):
    """A noise field with ONE unrelated source in it, away from where the model is
    injected. A fraction-of-bright-pixels gate let compact ones through; these are
    the shapes that slipped under it."""
    n = noise_field(rng, beam_fwhm)
    rms = float(1.4826 * np.median(np.abs(n - np.median(n))))
    gy, gx = np.mgrid[0:FIELD, 0:FIELD]
    sg = src_fwhm / 2.3548
    return n + snr * rms * np.exp(-(((gx - 150) ** 2 + (gy - 150) ** 2) / (2.0 * sg * sg)))


@pytest.mark.parametrize(("src_fwhm", "snr"), [(4.0, 20), (6.0, 20)])
def test_a_compact_contaminant_does_not_reach_the_null(src_fwhm, snr):
    """The error measured on a field containing an unrelated source must match the
    error measured on the same field without it: if placements landed on the
    contaminant, the scatter would be inflated.

    These two shapes are the ones a 5 % bright-pixel gate accepted — a FWHM-4 px
    source at SNR 20 puts only ~2 % of a 41x41 window beyond 3 sigma.
    """
    for beam in (0.0, 4.0):
        clean, _ = empirical_fit_errors(
            noise_field(np.random.default_rng(31), beam), MODEL, list(TRUTH),
            fit_window, np.random.default_rng(5), source_sigma_px=4.0,
            centre_xy=(FIELD / 2, FIELD / 2), n_trials=50)
        dirty, _ = empirical_fit_errors(
            contaminated_field(np.random.default_rng(31), beam, src_fwhm, snr),
            MODEL, list(TRUTH), fit_window, np.random.default_rng(5),
            source_sigma_px=4.0, centre_xy=(FIELD / 2, FIELD / 2), n_trials=50)
        assert clean is not None and dirty is not None
        ratio = np.sqrt(dirty[0, 0]) / np.sqrt(clean[0, 0])
        assert 0.7 < ratio < 1.4, (
            f"a FWHM {src_fwhm} SNR {snr} contaminant moved the error by "
            f"{ratio:.2f}x at beam {beam} px — it is reaching the null")


def test_the_documented_miss_errs_conservatively():
    """A FWHM-4 px source at SNR 8, in noise correlated on that same scale, is NOT
    flagged — at that scale it and a noise bump carry comparable flux, and no
    window statistic separates them. What must hold is the DIRECTION: such a
    placement can only widen the error, never shrink it."""
    clean, _ = empirical_fit_errors(
        noise_field(np.random.default_rng(41), 4.0), MODEL, list(TRUTH),
        fit_window, np.random.default_rng(6), source_sigma_px=4.0,
        centre_xy=(FIELD / 2, FIELD / 2), n_trials=50)
    dirty, _ = empirical_fit_errors(
        contaminated_field(np.random.default_rng(41), 4.0, 4.0, 8),
        MODEL, list(TRUTH), fit_window, np.random.default_rng(6),
        source_sigma_px=4.0, centre_xy=(FIELD / 2, FIELD / 2), n_trials=50)
    assert clean is not None and dirty is not None
    assert np.sqrt(dirty[0, 0]) >= 0.9 * np.sqrt(clean[0, 0])


def test_the_search_stays_near_the_source():
    """Placements come from a NEIGHBOURHOOD, not the whole plane — that is what
    bounds memory on a survey image and samples the noise where the source is.

    The field has to be bigger than twice the search reach for this to test
    anything: on a small image the neighbourhood IS the image, and a whole-plane
    implementation would pass. So: a large field, the local neighbourhood blanked,
    distant sky left perfectly clean. A whole-plane search would happily use that
    distant sky; a bounded one must return nothing.
    """
    rng = np.random.default_rng(51)
    big = 900
    field = rng.normal(0.0, NOISE, (big, big))
    reach = max(6 * WIN, 256)          # mirrors the production neighbourhood
    cy = cx = 300
    field[cy - reach - WIN:cy + reach + WIN, cx - reach - WIN:cx + reach + WIN] = np.nan
    cov, n = empirical_fit_errors(field, MODEL, list(TRUTH), fit_window,
                                  np.random.default_rng(7), source_sigma_px=4.0,
                                  centre_xy=(float(cx), float(cy)), n_trials=40)
    assert cov is None and n == 0, (
        "clean sky far from the source must not be used — the search is local")
    # ...and the same call away from the blanked patch still works, so the refusal
    # above is about locality, not about the field being unusable.
    ok, n_ok = empirical_fit_errors(field, MODEL, list(TRUTH), fit_window,
                                    np.random.default_rng(7), source_sigma_px=4.0,
                                    centre_xy=(750.0, 750.0), n_trials=40)
    assert ok is not None and n_ok >= 20


# ── worker-level contract ────────────────────────────────────────────────────

def _write_source_image(path, size, beam_fwhm, seed=77):
    """A FITS image with WCS, a beam, and one source at the centre."""
    from astropy.io import fits
    rng = np.random.default_rng(seed)
    gy, gx = np.mgrid[0:size, 0:size]
    c = size / 2.0
    img = 5.0 * np.exp(-(((gx - c) ** 2 + (gy - c) ** 2) / (2.0 * 16.0)))
    n = rng.normal(0.0, 1.0, (size, size))
    if beam_fwhm:
        n = ndimage.gaussian_filter(n, beam_fwhm / 2.3548, mode="wrap")
        n = n / n.std()
    hdr = fits.Header()
    hdr["CTYPE1"], hdr["CTYPE2"] = "RA---SIN", "DEC--SIN"
    hdr["CRVAL1"], hdr["CRVAL2"] = 150.0, 2.0
    hdr["CRPIX1"] = hdr["CRPIX2"] = c
    hdr["CDELT1"], hdr["CDELT2"] = -0.000555556, 0.000555556
    hdr["BUNIT"] = "Jy/beam"
    hdr["BMAJ"] = hdr["BMIN"] = 0.00222
    hdr["BPA"] = 0.0
    fits.PrimaryHDU((img + 0.15 * n).astype(np.float32), hdr).writeto(path)
    return 150.0, 2.0


def test_worker_reports_empirical_errors_when_there_is_clean_sky(tmp_path):
    from app.compute import worker_gauss_fit_2d
    path = str(tmp_path / "roomy.fits")
    ra, dec = _write_source_image(path, 300, beam_fwhm=4.0)
    r = worker_gauss_fit_2d(path, None, ra, dec, 100.0)
    assert r["error_method"] == "empirical"
    assert r["error_trials"] >= 20
    assert r["peak_err"] is not None and r["peak_err"] > 0
    assert r["integrated_flux_err_jy"] is not None
    # The diagnostic bundle is for the fallback only.
    assert r["diagnostic_covariance_err"] is None


def test_worker_refuses_calibrated_errors_when_there_is_no_clean_sky(tmp_path):
    """The contract that matters: a crowded/small field yields NULL uncertainties,
    not the curve_fit covariance dressed up as one. The raw figures stay available
    under a name that cannot be mistaken for a calibrated error."""
    from app.compute import worker_gauss_fit_2d
    path = str(tmp_path / "tight.fits")
    ra, dec = _write_source_image(path, 64, beam_fwhm=4.0)
    r = worker_gauss_fit_2d(path, None, ra, dec, 100.0)
    assert r["error_method"] == "covariance"
    assert r["peak_err"] is None
    assert r["integrated_flux_err_jy"] is None
    assert r["integrated_flux_err"] is None
    diag = r["diagnostic_covariance_err"]
    assert diag is not None and diag[0] is not None and diag[0] > 0


def test_worker_error_is_reproducible_and_position_dependent(tmp_path):
    """Reproducibility is the property that matters to a user: refitting the same
    source must not move the error bar.

    The second assertion is weaker than it looks and is labelled as such. Two
    sources in one image do give different errors — but they would with a global
    constant seed too, because each search runs in its OWN neighbourhood, so the
    same relative offsets land on different sky. Verified by mutation: replacing
    the position-derived seed with a constant fails NO test here. The derived seed
    is kept because it is free and decorrelates sources whose neighbourhoods
    overlap heavily, but that benefit is not demonstrated by this test and no
    claim is made on it.
    """
    from app.compute import worker_gauss_fit_2d
    path = str(tmp_path / "twice.fits")
    ra, dec = _write_source_image(path, 300, beam_fwhm=4.0)
    from astropy.io import fits
    from astropy.wcs import WCS

    # ONE image with TWO sources, so the only thing that differs between the two
    # fits is the position the seed is derived from. Comparing against a second,
    # modified image would not pin the seed: a global constant would also give
    # different numbers there, because the data changed too.
    two = str(tmp_path / "twosrc.fits")
    with fits.open(path) as hdul:
        data, hdr = hdul[0].data.copy(), hdul[0].header.copy()
    gy, gx = np.mgrid[0:data.shape[0], 0:data.shape[1]]
    data = data + 5.0 * np.exp(-(((gx - 80) ** 2 + (gy - 80) ** 2) / (2.0 * 16.0)))
    fits.PrimaryHDU(data.astype(np.float32), hdr).writeto(two)
    wcs = WCS(hdr).celestial
    centre_sky = wcs.all_pix2world([[data.shape[1] / 2, data.shape[0] / 2]], 0)[0]
    other_sky = wcs.all_pix2world([[80.0, 80.0]], 0)[0]

    a = worker_gauss_fit_2d(two, None, float(centre_sky[0]), float(centre_sky[1]), 100.0)
    b = worker_gauss_fit_2d(two, None, float(centre_sky[0]), float(centre_sky[1]), 100.0)
    assert a["peak_err"] == b["peak_err"], "a repeated fit must not move the error"

    c = worker_gauss_fit_2d(two, None, float(other_sky[0]), float(other_sky[1]), 100.0)
    assert c["error_method"] == "empirical"
    assert c["peak_err"] != a["peak_err"], (
        "two sources must not report an identical error bar")
