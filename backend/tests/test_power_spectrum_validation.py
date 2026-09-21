"""Does the power spectrum MEASURE anything?

The existing tests cover the shape of the answer (bins increase, power is
positive, white noise is roughly flat) and the size of the error bars. Neither
would notice an estimator that reported a spectrum with the wrong slope, or an
ℓ axis off by 2π — and the slope is the whole reason anybody computes this.

So: build fields whose spectrum is known by construction, and check that the
estimator recovers it. The tolerances are measured, not guessed — six
realisations per slope gave a scatter of 0.08 and a bias below 0.05.
"""
from __future__ import annotations

import numpy as np
import pytest
from astropy.io import fits

from app.compute import worker_angular_power_spectrum


def _gaussian_random_field(size: int, slope: float, seed: int) -> np.ndarray:
    """A field whose power spectrum is P(k) ∝ k**slope, by construction."""
    rng = np.random.default_rng(seed)
    frequencies = np.fft.fftfreq(size)
    wavenumber = np.hypot(frequencies[:, None], frequencies[None, :])
    wavenumber[0, 0] = 1e-9
    amplitude = wavenumber ** (slope / 2.0)
    amplitude[0, 0] = 0.0                     # no DC: the mean is not a mode
    noise = rng.standard_normal((size, size)) + 1j * rng.standard_normal((size, size))
    return np.real(np.fft.ifft2(noise * amplitude)) * size


def _write(tmp_path, data: np.ndarray, pixel_degrees: float | None = None):
    hdu = fits.PrimaryHDU(data.astype(np.float32))
    if pixel_degrees is not None:
        hdu.header["CTYPE1"] = "RA---TAN"
        hdu.header["CTYPE2"] = "DEC--TAN"
        hdu.header["CRVAL1"] = 10.0
        hdu.header["CRVAL2"] = 0.0
        hdu.header["CRPIX1"] = data.shape[1] / 2
        hdu.header["CRPIX2"] = data.shape[0] / 2
        hdu.header["CDELT1"] = -pixel_degrees
        hdu.header["CDELT2"] = pixel_degrees
    path = tmp_path / "field.fits"
    hdu.writeto(path, overwrite=True)
    return str(path)


def _fitted_slope(result: dict) -> float:
    k = np.asarray(result["k_bins"])
    power = np.asarray(result["cl"])
    modes = np.asarray(result["n_modes"])
    # The fit avoids both ends: the largest scales hold a handful of modes and
    # feel the window, and the last bins run into the Nyquist frequency.
    usable = (k > 0.02) & (k < 0.25) & (modes >= 8) & (power > 0)
    return float(np.polyfit(np.log10(k[usable]), np.log10(power[usable]), 1)[0])


@pytest.mark.parametrize("slope", [0.0, -1.0, -2.0, -3.0])
def test_the_slope_of_a_known_spectrum_is_recovered(tmp_path, slope):
    recovered = []
    for seed in range(4):
        path = _write(tmp_path, _gaussian_random_field(256, slope, seed))
        recovered.append(_fitted_slope(worker_angular_power_spectrum(path, 0, 24)))
    mean = float(np.mean(recovered))
    assert abs(mean - slope) < 0.2, f"input {slope}, recovered {mean:.3f} from {recovered}"


def test_the_multipole_axis_follows_the_pixel_scale(tmp_path):
    """ℓ = 2π k / θ_pixel — the conversion the whole axis means nothing without.

    A field is written with one-arcminute pixels, so every bin's ℓ/k must be
    2π / θ_pix in radians, and no bin may exceed the Nyquist multipole π/θ_pix.
    """
    pixel_degrees = 1.0 / 60.0
    path = _write(tmp_path, _gaussian_random_field(128, -2.0, 1), pixel_degrees)
    result = worker_angular_power_spectrum(path, 0, 24)

    theta = np.deg2rad(pixel_degrees)
    k = np.asarray(result["k_bins"])
    ell = np.asarray(result["ell"])
    assert np.allclose(ell / k, 2.0 * np.pi / theta, rtol=1e-9)
    assert ell.max() <= np.pi / theta


def test_without_a_pixel_scale_the_axis_is_not_invented(tmp_path):
    """No WCS means no angle.

    `spatial_pixel_scale_arcsec()` answers 3600 arcsec for a header with no WCS
    — astropy's one-degree-per-pixel default — so the estimator used to hand
    back a multipole axis computed from a pixel size nobody had stated. It read
    as calibrated and was wrong by whatever the real scale was.
    """
    path = _write(tmp_path, _gaussian_random_field(64, -2.0, 3))
    result = worker_angular_power_spectrum(path, 0, 12)
    assert result["ell_is_angular"] is False
    assert result["pixel_scale_arcsec"] is None
    assert np.allclose(result["ell"], result["k_bins"])


def test_with_a_pixel_scale_the_axis_says_so(tmp_path):
    path = _write(tmp_path, _gaussian_random_field(64, -2.0, 3), pixel_degrees=1.0 / 60.0)
    result = worker_angular_power_spectrum(path, 0, 12)
    assert result["ell_is_angular"] is True
    assert result["pixel_scale_arcsec"] == pytest.approx(60.0, rel=1e-6)


def _write_header(tmp_path, data: np.ndarray, cards: dict) -> str:
    hdu = fits.PrimaryHDU(data.astype(np.float32))
    for key, value in cards.items():
        hdu.header[key] = value
    path = tmp_path / "wcs.fits"
    hdu.writeto(path, overwrite=True)
    return str(path)


def test_a_linear_axis_in_metres_is_not_an_angle(tmp_path):
    """A CDELT is not an angular scale.

    The first fix tested for "is there a CDELT1", which a laboratory image in
    metres also has — and its ℓ axis came back as if two metres were two
    degrees.
    """
    path = _write_header(tmp_path, _gaussian_random_field(64, -2.0, 5), {
        "CTYPE1": "LINEAR", "CTYPE2": "LINEAR",
        "CUNIT1": "m", "CUNIT2": "m",
        "CRPIX1": 32, "CRPIX2": 32, "CRVAL1": 0.0, "CRVAL2": 0.0,
        "CDELT1": 2.0, "CDELT2": 2.0,
    })
    result = worker_angular_power_spectrum(path, 0, 12)
    assert result["ell_is_angular"] is False
    assert np.allclose(result["ell"], result["k_bins"])


def test_a_scale_carried_by_a_cd_matrix_still_counts(tmp_path):
    """The scale need not be in CDELT at all.

    A CD matrix with no CDELT is as valid as CDELT with no CD, and rotated
    pixels are written that way as a matter of course. astropy resolves every
    convention; the point of the test is that the decision is left to it rather
    than to a list of keyword names.
    """
    path = _write_header(tmp_path, _gaussian_random_field(64, -2.0, 6), {
        "CTYPE1": "RA---TAN", "CTYPE2": "DEC--TAN",
        "CRPIX1": 32, "CRPIX2": 32, "CRVAL1": 10.0, "CRVAL2": 0.0,
        "CD1_1": -1.0 / 60.0, "CD1_2": 0.0, "CD2_1": 0.0, "CD2_2": 1.0 / 60.0,
    })
    result = worker_angular_power_spectrum(path, 0, 12)
    assert result["ell_is_angular"] is True
    assert result["pixel_scale_arcsec"] == pytest.approx(60.0, rel=1e-6)


def test_a_cube_whose_celestial_axes_are_not_the_plane_is_not_angular(tmp_path):
    """The slice always takes the leading numpy axis — the last FITS axis.

    For a cube written (FREQ, RA, DEC) that leaves an RA/frequency plane, whose
    pixel scale is not an angle in one direction at all. The cube's WCS is
    perfectly celestial, so asking `has_celestial` alone would have put a
    multipole axis on a spectrum.
    """
    cube = np.stack([_gaussian_random_field(32, -2.0, seed) for seed in range(4)])
    path = _write_header(tmp_path, cube, {
        "CTYPE1": "FREQ", "CUNIT1": "Hz", "CRPIX1": 1, "CRVAL1": 1.4e9, "CDELT1": 1.0e5,
        "CTYPE2": "RA---TAN", "CRPIX2": 16, "CRVAL2": 10.0, "CDELT2": -1.0 / 60.0,
        "CTYPE3": "DEC--TAN", "CRPIX3": 16, "CRVAL3": 0.0, "CDELT3": 1.0 / 60.0,
    })
    result = worker_angular_power_spectrum(path, 0, 12)
    assert result["ell_is_angular"] is False
    assert np.allclose(result["ell"], result["k_bins"])


def test_a_known_period_lands_in_the_right_bin(tmp_path):
    """Absolute frequency, not just the slope.

    A sinusoid of period 8 pixels has all its power at k = 1/8 pixel⁻¹. A
    spectrum whose k axis is scaled by a constant — an FFT normalisation, a
    factor of two from the half-plane — still fits the right slope and puts this
    peak in the wrong place.
    """
    size, period = 128, 8.0
    x = np.arange(size)
    data = np.tile(np.sin(2.0 * np.pi * x / period), (size, 1))
    path = _write(tmp_path, data)
    result = worker_angular_power_spectrum(path, 0, 32)

    k = np.asarray(result["k_bins"])
    power = np.asarray(result["cl"])
    assert k[int(np.argmax(power))] == pytest.approx(1.0 / period, rel=0.15)
