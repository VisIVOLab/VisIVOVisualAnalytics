"""Angular power-spectrum error bars (audit of R-15's `cl_err`).

Found by sweeping every `*_err` the compute layer returns, after the same class
of bug had been fixed twice (R-23 in the line fit, R-24 in the 2-D source fit):
an uncertainty derived by assuming independent samples where the data are
correlated.

Here the samples are the Fourier modes inside a |k| bin, and the Hann window
couples neighbouring ones, so `std(bin)/sqrt(N)` counts more independent modes
than exist. Measured over Gaussian random fields, the uncorrected value came out
1.7-2.5x too small — matching sqrt(<w^4>/<w^2>^2) = 1.94 for a separable 2-D
Hann window, which is the correction now applied. That factor is an asymptotic,
populated-bin result, so the worker also returns `n_modes` per bin and these
tests classify by it rather than assuming every bin is equally well determined.

These tests measure the coverage rather than re-deriving the factor: they compare
the reported error against the true scatter of the binned spectrum across
independent realisations of a field with a known spectrum.
"""

from __future__ import annotations

import numpy as np
import pytest
from astropy.io import fits

from app.compute.cosmology import worker_angular_power_spectrum

SIZE = 256
N_REALISATIONS = 40


def _header() -> fits.Header:
    hdr = fits.Header()
    hdr["CTYPE1"], hdr["CTYPE2"] = "RA---SIN", "DEC--SIN"
    hdr["CRVAL1"], hdr["CRVAL2"] = 10.0, 0.0
    hdr["CRPIX1"] = hdr["CRPIX2"] = SIZE / 2
    hdr["CDELT1"], hdr["CDELT2"] = -0.001, 0.001
    return hdr


def gaussian_random_field(seed: int, slope: float) -> np.ndarray:
    """A field with power ∝ k^(2·slope) and NO zero-mode.

    Two fixture traps, both hit while writing this and both worth naming. Setting
    k[0,0] to a tiny number instead of zeroing the amplitude injects a huge DC
    mode that swamps every bin. And normalising each realisation by its own
    standard deviation makes the low-k modes modulate all bins coherently, which
    shows up as an error ratio that grows with ell and looks like a real finding.
    Neither is a property of the estimator.
    """
    rng = np.random.default_rng(seed)
    ky = np.fft.fftfreq(SIZE)[:, None]
    kx = np.fft.fftfreq(SIZE)[None, :]
    k = np.sqrt(kx ** 2 + ky ** 2)
    amp = np.zeros_like(k)
    nonzero = k > 0
    amp[nonzero] = k[nonzero] ** slope
    field = np.fft.ifft2(np.fft.fft2(rng.normal(0.0, 1.0, (SIZE, SIZE))) * amp).real
    return (field * 1e3).astype(np.float32)      # fixed scale, never per-realisation


def spectra(tmp_path, slope: float, n_bins: int | None = None):
    """Run the worker over many realisations. n_bins=None uses the SHIPPED
    DEFAULT, which is the configuration that actually reaches users and the one
    where sparse bins appear — validating only a coarse binning would test a
    regime nobody runs."""
    cls, errs, modes = [], [], None
    hdr = _header()
    path = str(tmp_path / "field.fits")
    for i in range(N_REALISATIONS):
        fits.PrimaryHDU(gaussian_random_field(400 + i, slope), hdr).writeto(
            path, overwrite=True)
        result = (worker_angular_power_spectrum(path, channel=0) if n_bins is None
                  else worker_angular_power_spectrum(path, channel=0, n_bins=n_bins))
        cls.append(result["cl"])
        errs.append(result["cl_err"])
        modes = result["n_modes"]
    return np.asarray(cls), np.asarray(errs), np.asarray(modes)


# ── coverage ─────────────────────────────────────────────────────────────────

@pytest.mark.parametrize(("slope", "name"), [(0.0, "white"), (-1.0, "red")])
def test_reported_error_matches_the_scatter_across_realisations(tmp_path, slope, name):
    """The reported per-bin error should be the scatter you would actually see if
    you re-observed the same sky. Uncorrected it was ~2x too small in every bin.

    Run at the SHIPPED default binning, and classified by how many modes a bin
    holds rather than by index: "skip the first bin" is not the right rule, the
    mode count is. Sparse bins get a looser band because a handful of modes gives
    a poorly determined error whatever scalar correction is applied — that is a
    property of the estimator, and it is why the worker returns `n_modes`.
    """
    cls, errs, modes = spectra(tmp_path, slope)
    # Bins with fewer than 3 modes report a null error on purpose — a 2-point
    # standard deviation estimates nothing — so there is no ratio to check there.
    estimable = np.array([errs[0][j] is not None for j in range(cls.shape[1])])
    assert np.all(estimable[modes >= 3]) and not np.any(estimable[modes < 3])
    err_arr = np.array([[e if e is not None else np.nan for e in row] for row in errs])
    ratios = np.full(cls.shape[1], np.nan)
    for j in np.flatnonzero(estimable):
        ratios[j] = float(np.std(cls[:, j], ddof=1)) / float(np.median(err_arr[:, j]))
    populated = estimable & (modes > 8)
    assert populated.sum() >= 20, "the default binning should give many usable bins"
    assert np.all((ratios[populated] > 0.5) & (ratios[populated] < 1.6)), (
        f"{name}, populated bins: "
        f"{', '.join(f'{r:.2f}' for r in ratios[populated])}")
    # Sparse bins: still bounded, just not tightly. An order of magnitude here
    # would mean the correction is doing nothing where it is needed most.
    sparse = estimable & (modes <= 8)
    if sparse.any():
        assert np.all((ratios[sparse] > 0.3) & (ratios[sparse] < 2.2)), (
            f"{name}, sparse bins: "
            f"{', '.join(f'{r:.2f}' for r in ratios[sparse])}")


def test_conjugate_duplicates_are_not_counted_twice(tmp_path):
    """rfft2 returns half the plane, but the kx=0 column still holds BOTH members
    of each Hermitian pair. Counting them twice inflates the mode count and shrinks
    the error — worst exactly in the sparse low-k bins."""
    hdr = _header()
    path = str(tmp_path / "modes.fits")
    fits.PrimaryHDU(gaussian_random_field(1234, 0.0), hdr).writeto(path)
    # An inequality against the half-plane size proves nothing: the radial Nyquist
    # cut already drops the total far below it either way (25841 undeduped vs
    # 25714 deduped, both under any such bound). Pin the exact count instead, and
    # the sparse low-k bins where the duplicates actually lived.
    result = worker_angular_power_spectrum(path, channel=0, n_bins=10)
    assert sum(result["n_modes"]) == 25714
    assert result["n_modes"][:2] == [4, 6]      # 5 and 7 before deduplication
    # At the default binning the first bins hold 2 unique modes, not 3, which is
    # what pushes them below the "no estimable error" threshold.
    default = worker_angular_power_spectrum(path, channel=0)
    assert default["n_modes"][0] == 2
    assert default["cl_err"][0] is None


# ── the correction itself ────────────────────────────────────────────────────

def test_correction_matches_the_window_shape(tmp_path):
    """sqrt(<w^4>/<w^2>^2) for the separable 2-D Hann window the worker builds,
    and the worker must REPORT it: the effective mode count a caller needs is
    n_modes / mode_coupling**2, so dropping the field would silently leave every
    raw count looking ~4x better determined than it is."""
    w = np.hanning(SIZE)[:, None] * np.hanning(SIZE)[None, :]
    factor = float(np.sqrt(np.mean(w ** 4) / np.mean(w ** 2) ** 2))
    assert factor == pytest.approx(1.94, abs=0.05)

    hdr = _header()
    path = str(tmp_path / "coupling.fits")
    fits.PrimaryHDU(gaussian_random_field(555, 0.0), hdr).writeto(path)
    result = worker_angular_power_spectrum(path, channel=0, n_bins=10)
    assert result["mode_coupling"] == pytest.approx(factor, rel=1e-6)


def test_an_unwindowed_map_would_need_no_correction():
    """The clip at 1: a flat window gives independent modes and the naive
    standard error is already right, so the factor must not inflate it."""
    w = np.ones((64, 64))
    factor = np.sqrt(max(np.mean(w ** 4) / np.mean(w ** 2) ** 2, 1.0))
    assert factor == pytest.approx(1.0)


def test_errors_are_positive_and_finite(tmp_path):
    hdr = _header()
    path = str(tmp_path / "one.fits")
    fits.PrimaryHDU(gaussian_random_field(999, -1.0), hdr).writeto(path)
    result = worker_angular_power_spectrum(path, channel=0, n_bins=8)
    errs = result["cl_err"]
    assert len(errs) == len(result["cl"]) == len(result["n_modes"])
    for e, n in zip(errs, result["n_modes"]):
        if n < 3:
            assert e is None, "an unestimable bin must report null, not a number"
        else:
            assert np.isfinite(e) and e >= 0.0
