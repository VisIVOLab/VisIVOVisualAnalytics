"""Regression tests for issue #137: photometry and polarisation read the wrong
plane, the wrong frame, or combine mismatched Stokes maps without an error.

Each case is a small synthetic FITS whose wrong answer is different from the
right one, so a silent regression shows up as a number, not only as an error.

1. A 4-D cube stored (spectral, Stokes, y, x): photometry and the Gaussian fit
   must measure Stokes I of the requested channel.
2. Q and U whose celestial WCS differ by a 90 degree rotation (PC or CD) must be
   refused; the same WCS written once as CDELT+PC and once as CD must not.
3. Q and U with different plane counts or spectral coordinates must not be
   paired plane by plane.
4. An ICRS position on a Galactic (or FK4/FK5) image must be converted to the
   image's frame before it becomes a pixel.
"""
from __future__ import annotations

import numpy as np
import pytest
from astropy import units as u
from astropy.coordinates import SkyCoord
from astropy.io import fits
from astropy.wcs import WCS
from astropy.wcs.utils import pixel_to_skycoord

N = 48                      # image side, pixels
CENTRE = (N - 1) / 2.0      # 0-based centre pixel
SCALE_DEG = 1.0 / 3600.0    # 1 arcsec pixels
SIGMA = 3.0                 # source sigma, pixels


def _gauss(amplitude: float, x0: float = CENTRE, y0: float = CENTRE) -> np.ndarray:
    yy, xx = np.mgrid[0:N, 0:N]
    return (amplitude * np.exp(-0.5 * ((xx - x0) ** 2 + (yy - y0) ** 2) / SIGMA ** 2)
            ).astype(np.float32)


def _celestial_header(h: fits.Header, ctype=("RA---TAN", "DEC--TAN"), crval=(150.0, 2.0)):
    h["CTYPE1"], h["CTYPE2"] = ctype
    h["CRVAL1"], h["CRVAL2"] = crval
    h["CRPIX1"] = h["CRPIX2"] = CENTRE + 1.0
    h["CDELT1"], h["CDELT2"] = -SCALE_DEG, SCALE_DEG
    h["CUNIT1"] = h["CUNIT2"] = "deg"


# ── 1. 4-D plane selection ───────────────────────────────────────────────────

# Stokes axis V, U, Q, I (CRVAL = 4, CDELT = -1): I is the LAST Stokes plane, so
# neither "index 0" nor "the requested channel" lands on it by accident.
N_STOKES, N_FREQ, CHANNEL, TRUE_AMP = 4, 3, 2, 20.0


def _plane_amplitude(freq: int, stokes_index: int) -> float:
    if freq == CHANNEL and stokes_index == 3:
        return TRUE_AMP                     # Stokes I of the requested channel
    return 11.0 + freq + 0.5 * stokes_index  # anything else: a different number


def _write_4d(path: str, spectral_outer: bool) -> None:
    """Spectral outer: numpy (freq, stokes, y, x), CTYPE3 = STOKES, CTYPE4 = FREQ.
    Otherwise the usual numpy (stokes, freq, y, x), CTYPE3 = FREQ, CTYPE4 = STOKES."""
    if spectral_outer:
        data = np.zeros((N_FREQ, N_STOKES, N, N), dtype=np.float32)
        for f in range(N_FREQ):
            for s in range(N_STOKES):
                data[f, s] = _gauss(_plane_amplitude(f, s))
        stokes_axis, freq_axis = 3, 4
    else:
        data = np.zeros((N_STOKES, N_FREQ, N, N), dtype=np.float32)
        for f in range(N_FREQ):
            for s in range(N_STOKES):
                data[s, f] = _gauss(_plane_amplitude(f, s))
        stokes_axis, freq_axis = 4, 3
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    _celestial_header(h)
    h[f"CTYPE{stokes_axis}"] = "STOKES"
    h[f"CRPIX{stokes_axis}"] = 1.0
    h[f"CRVAL{stokes_axis}"] = 4.0
    h[f"CDELT{stokes_axis}"] = -1.0
    h[f"CTYPE{freq_axis}"] = "FREQ"
    h[f"CUNIT{freq_axis}"] = "Hz"
    h[f"CRPIX{freq_axis}"] = 1.0
    h[f"CRVAL{freq_axis}"] = 1.4e9
    h[f"CDELT{freq_axis}"] = 1.0e6
    hdu.writeto(path, overwrite=True)


def _centre_sky(path: str) -> tuple[float, float]:
    w = WCS(fits.getheader(path)).celestial
    c = pixel_to_skycoord(CENTRE, CENTRE, w, origin=0).icrs
    return float(c.ra.deg), float(c.dec.deg)


@pytest.mark.parametrize("spectral_outer", [True, False], ids=["freq-stokes-y-x", "stokes-freq-y-x"])
def test_aperture_photometry_measures_stokes_i_of_the_requested_channel(tmp_path, spectral_outer):
    from app.compute import worker_aperture_photometry
    path = str(tmp_path / "cube4d.fits")
    _write_4d(path, spectral_outer)
    ra, dec = _centre_sky(path)
    r = worker_aperture_photometry(path, CHANNEL, ra, dec, 15.0, 0.0, 0.0)
    # A radius-5-sigma aperture holds the whole Gaussian: 2 pi sigma^2 A.
    amplitude = r["flux"] / (2.0 * np.pi * SIGMA ** 2)
    assert amplitude == pytest.approx(TRUE_AMP, rel=1e-3)
    assert r["resolved_channel"] == CHANNEL
    assert r["stokes"] == "I"


@pytest.mark.parametrize("spectral_outer", [True, False], ids=["freq-stokes-y-x", "stokes-freq-y-x"])
def test_gauss_fit_measures_stokes_i_of_the_requested_channel(tmp_path, spectral_outer):
    from app.compute import worker_gauss_fit_2d
    path = str(tmp_path / "cube4d.fits")
    _write_4d(path, spectral_outer)
    ra, dec = _centre_sky(path)
    r = worker_gauss_fit_2d(path, CHANNEL, ra, dec, 30.0)
    assert r["peak_jy_beam"] == pytest.approx(TRUE_AMP, rel=1e-3)
    assert r["resolved_channel"] == CHANNEL
    assert r["stokes"] == "I"


def test_a_cube_without_stokes_i_is_refused(tmp_path):
    """Q and U only: there is no total-intensity plane to measure."""
    from app.compute import worker_aperture_photometry
    path = str(tmp_path / "qu.fits")
    data = np.stack([_gauss(3.0), _gauss(4.0)])[:, None]     # (stokes=2, freq=1, y, x)
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    _celestial_header(h)
    h["CTYPE3"], h["CRPIX3"], h["CRVAL3"], h["CDELT3"] = "FREQ", 1.0, 1.4e9, 1e6
    h["CTYPE4"], h["CRPIX4"], h["CRVAL4"], h["CDELT4"] = "STOKES", 1.0, 2.0, 1.0
    hdu.writeto(path, overwrite=True)
    ra, dec = _centre_sky(path)
    with pytest.raises(ValueError, match="Stokes I"):
        worker_aperture_photometry(path, None, ra, dec, 10.0, 0.0, 0.0)


def test_three_d_cube_still_takes_the_requested_channel(tmp_path):
    from app.compute import worker_aperture_photometry
    path = str(tmp_path / "cube3d.fits")
    data = np.stack([_gauss(5.0), _gauss(TRUE_AMP), _gauss(7.0)])
    hdu = fits.PrimaryHDU(data)
    _celestial_header(hdu.header)
    hdu.header["CTYPE3"], hdu.header["CRPIX3"] = "VRAD", 1.0
    hdu.header["CRVAL3"], hdu.header["CDELT3"], hdu.header["CUNIT3"] = 0.0, 1000.0, "m/s"
    hdu.writeto(path, overwrite=True)
    ra, dec = _centre_sky(path)
    r = worker_aperture_photometry(path, 1, ra, dec, 15.0, 0.0, 0.0)
    assert r["flux"] / (2.0 * np.pi * SIGMA ** 2) == pytest.approx(TRUE_AMP, rel=1e-3)
    assert r["resolved_channel"] == 1
    assert r["stokes"] is None


# ── 4. ICRS input on images in other frames ──────────────────────────────────

FRAMES = {
    # name: (CTYPE pair, extra keywords)
    "icrs": (("RA---TAN", "DEC--TAN"), {"RADESYS": "ICRS"}),
    "fk5": (("RA---TAN", "DEC--TAN"), {"RADESYS": "FK5", "EQUINOX": 2000.0}),
    "fk4": (("RA---TAN", "DEC--TAN"), {"RADESYS": "FK4", "EQUINOX": 1950.0}),
    "galactic-car": (("GLON-CAR", "GLAT-CAR"), {}),
    "galactic-tan": (("GLON-TAN", "GLAT-TAN"), {}),
    "no-frame-keyword": (("RA---TAN", "DEC--TAN"), {}),
}


def _write_frame_image(path: str, frame: str) -> None:
    ctype, extra = FRAMES[frame]
    hdu = fits.PrimaryHDU(_gauss(10.0))
    h = hdu.header
    _celestial_header(h, ctype=ctype, crval=(30.0, 0.5))
    for k, v in extra.items():
        h[k] = v
    h["BUNIT"] = "Jy/beam"
    h["BMAJ"], h["BMIN"], h["BPA"] = 4.0 * SCALE_DEG, 3.0 * SCALE_DEG, 30.0
    hdu.writeto(path, overwrite=True)


@pytest.mark.parametrize("frame", list(FRAMES))
def test_icrs_position_of_the_centre_lands_on_the_centre_pixel(tmp_path, frame):
    from app.compute import worker_aperture_photometry, worker_gauss_fit_2d
    path = str(tmp_path / f"{frame}.fits")
    _write_frame_image(path, frame)
    ra, dec = _centre_sky(path)          # the centre, as ICRS RA/Dec
    ap = worker_aperture_photometry(path, None, ra, dec, 10.0, 0.0, 0.0)
    assert ap["pix_x"] == pytest.approx(CENTRE, abs=1e-3)
    assert ap["pix_y"] == pytest.approx(CENTRE, abs=1e-3)
    fit = worker_gauss_fit_2d(path, None, ra, dec, 30.0)
    assert fit["center_pix_x"] == pytest.approx(CENTRE, abs=1e-2)
    assert fit["center_pix_y"] == pytest.approx(CENTRE, abs=1e-2)
    assert fit["peak_jy_beam"] == pytest.approx(10.0, rel=1e-3)


def test_galactic_image_refuses_a_position_that_is_really_off_the_image(tmp_path):
    """The conversion must not turn every request into the centre."""
    from app.compute import worker_aperture_photometry
    path = str(tmp_path / "gal.fits")
    _write_frame_image(path, "galactic-car")
    far = SkyCoord(l=31.0 * u.deg, b=0.5 * u.deg, frame="galactic").icrs
    with pytest.raises(ValueError, match="outside the image"):
        worker_aperture_photometry(path, None, float(far.ra.deg), float(far.dec.deg),
                                   5.0, 0.0, 0.0)


# ── 2 and 3. Q/U pairing ─────────────────────────────────────────────────────

NQ = 16


def _write_stokes(path: str, stokes: int, value: float, *, n_freq: int = 1,
                  freq0: float = 1.4e9, rotate: str | None = None, cd: bool = False,
                  rm: float | None = None) -> str:
    """One Stokes plane as (stokes=1, freq, y, x). `rotate` = "pc" or "cd" turns
    the celestial axes by 90 degrees; `cd=True` writes the same, unrotated WCS
    as a CD matrix instead of CDELT."""
    c = 2.99792458e8
    freqs = freq0 + np.arange(n_freq) * 5.0e7
    data = np.full((1, n_freq, NQ, NQ), value, dtype=np.float32)
    if rm is not None:
        angle = 2.0 * rm * (c / freqs) ** 2
        wave = np.cos(angle) if stokes == 2 else np.sin(angle)
        data[0] = wave[:, None, None] * value
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["CTYPE1"], h["CTYPE2"] = "RA---SIN", "DEC--SIN"
    h["CRVAL1"], h["CRVAL2"] = 266.4, -29.0
    h["CRPIX1"] = h["CRPIX2"] = (NQ + 1) / 2.0
    h["CUNIT1"] = h["CUNIT2"] = "deg"
    d = 4.166667e-4
    if rotate == "cd":
        h["CD1_1"], h["CD1_2"], h["CD2_1"], h["CD2_2"] = 0.0, d, d, 0.0
    elif cd:
        h["CD1_1"], h["CD1_2"], h["CD2_1"], h["CD2_2"] = -d, 0.0, 0.0, d
    else:
        h["CDELT1"], h["CDELT2"] = -d, d
        if rotate == "pc":
            h["PC1_1"], h["PC1_2"], h["PC2_1"], h["PC2_2"] = 0.0, -1.0, 1.0, 0.0
    h["CTYPE3"], h["CUNIT3"], h["CRPIX3"] = "FREQ", "Hz", 1.0
    h["CRVAL3"], h["CDELT3"] = freq0, 5.0e7
    h["CTYPE4"], h["CRPIX4"], h["CRVAL4"], h["CDELT4"] = "STOKES", 1.0, float(stokes), 1.0
    if rotate == "cd" or cd:
        # With a CD matrix CDELTi is ignored on EVERY axis, so the spectral and
        # Stokes steps have to be in the matrix too.
        h["CD3_3"], h["CD4_4"] = 5.0e7, 1.0
    hdu.writeto(path, overwrite=True)
    return path


@pytest.mark.parametrize("rotate", ["pc", "cd"])
def test_pi_pa_refuses_q_and_u_rotated_by_90_degrees(tmp_path, rotate):
    from app.compute import worker_polarisation_pi_pa
    q = _write_stokes(str(tmp_path / "q.fits"), 2, 3.0)
    uf = _write_stokes(str(tmp_path / "u.fits"), 3, 4.0, rotate=rotate)
    with pytest.raises(ValueError, match="same sky grid"):
        worker_polarisation_pi_pa(q, 0, None, {"U": uf})


@pytest.mark.parametrize("rotate", ["pc", "cd"])
def test_rm_synthesis_refuses_q_and_u_rotated_by_90_degrees(tmp_path, rotate):
    from app.compute import worker_rm_synthesis
    q = _write_stokes(str(tmp_path / "q.fits"), 2, 1.0, n_freq=4, rm=20.0)
    uf = _write_stokes(str(tmp_path / "u.fits"), 3, 1.0, n_freq=4, rm=20.0, rotate=rotate)
    with pytest.raises(ValueError, match="same sky grid"):
        worker_rm_synthesis(q, -100.0, 100.0, 1.0, {"U": uf}, reduce="peak")


def test_the_same_wcs_written_as_pc_and_as_cd_is_accepted(tmp_path):
    from app.compute import worker_polarisation_pi_pa, worker_rm_synthesis
    q = _write_stokes(str(tmp_path / "q.fits"), 2, 3.0, n_freq=4, rm=20.0)
    uf = _write_stokes(str(tmp_path / "u.fits"), 3, 3.0, n_freq=4, rm=20.0, cd=True)
    r = worker_polarisation_pi_pa(q, 0, None, {"U": uf})
    assert r["width"] == NQ and r["height"] == NQ
    rm = worker_rm_synthesis(q, -100.0, 100.0, 1.0, {"U": uf}, reduce="peak")
    assert rm["phi_peak_min"] == pytest.approx(20.0, abs=2.0)


def test_pi_pa_refuses_a_channel_that_u_does_not_have(tmp_path):
    """3-plane Q, 1-plane U: channel 2 used to pair Q[2] with U[0]."""
    from app.compute import worker_polarisation_pi_pa
    q = _write_stokes(str(tmp_path / "q.fits"), 2, 3.0, n_freq=3)
    uf = _write_stokes(str(tmp_path / "u.fits"), 3, 4.0, n_freq=1)
    with pytest.raises(ValueError, match="Stokes U"):
        worker_polarisation_pi_pa(q, 2, None, {"U": uf})
    # Plane 0 is the same frequency in both files, and stays usable.
    r = worker_polarisation_pi_pa(q, 0, None, {"U": uf})
    assert r["channel"] == 0


def test_pi_pa_refuses_q_and_u_at_different_frequencies(tmp_path):
    from app.compute import worker_polarisation_pi_pa
    q = _write_stokes(str(tmp_path / "q.fits"), 2, 3.0, n_freq=3, freq0=1.4e9)
    uf = _write_stokes(str(tmp_path / "u.fits"), 3, 4.0, n_freq=3, freq0=1.5e9)
    with pytest.raises(ValueError, match="spectral"):
        worker_polarisation_pi_pa(q, 1, None, {"U": uf})


def test_pi_pa_line_free_channels_must_exist_in_both(tmp_path):
    """The noise estimate reads every line-free plane from Q AND U."""
    from app.compute import worker_polarisation_pi_pa
    q = _write_stokes(str(tmp_path / "q.fits"), 2, 3.0, n_freq=3)
    uf = _write_stokes(str(tmp_path / "u.fits"), 3, 4.0, n_freq=1)
    with pytest.raises(ValueError, match="Stokes U"):
        worker_polarisation_pi_pa(q, 0, [1, 2], {"U": uf})


def _write_all_sky(path: str, stokes: int, rotate: bool) -> str:
    """A 361 x 181 AIT map: its corners are off the sky, only the interior is not."""
    hdu = fits.PrimaryHDU(np.ones((181, 361), dtype=np.float32))
    h = hdu.header
    h["CTYPE1"], h["CTYPE2"] = "RA---AIT", "DEC--AIT"
    h["CRVAL1"], h["CRVAL2"], h["CRPIX1"], h["CRPIX2"] = 0.0, 0.0, 181.0, 91.0
    h["CDELT1"], h["CDELT2"] = -1.0, 1.0
    if rotate:
        h["PC1_1"], h["PC1_2"], h["PC2_1"], h["PC2_2"] = 0.0, -1.0, 1.0, 0.0
    hdu.writeto(path, overwrite=True)
    return path


def test_rotated_all_sky_maps_are_refused(tmp_path):
    """With the corners off the sky, the centre alone cannot see a rotation."""
    from app.compute.polarisation import _require_same_sky_grid
    from app.fits_dataset import ScientificFitsDataset
    q = ScientificFitsDataset(_write_all_sky(str(tmp_path / "q.fits"), 2, False))
    same = ScientificFitsDataset(_write_all_sky(str(tmp_path / "u0.fits"), 3, False))
    turned = ScientificFitsDataset(_write_all_sky(str(tmp_path / "u1.fits"), 3, True))
    _require_same_sky_grid(q, same)
    with pytest.raises(ValueError, match="same sky grid"):
        _require_same_sky_grid(q, turned)


def test_files_without_any_coordinates_are_not_a_registered_pair(tmp_path):
    """astropy builds an identity WCS for a bare header; that is not a sky grid."""
    from app.compute.polarisation import _require_same_sky_grid
    from app.fits_dataset import ScientificFitsDataset
    paths = []
    for name in ("q.fits", "u.fits"):
        p = str(tmp_path / name)
        fits.PrimaryHDU(np.ones((8, 8), dtype=np.float32)).writeto(p)
        paths.append(p)
    with pytest.raises(ValueError, match="no pixel scale"):
        _require_same_sky_grid(ScientificFitsDataset(paths[0]), ScientificFitsDataset(paths[1]))


def test_a_single_labelled_stokes_plane_is_reported_by_name(tmp_path):
    """A one-plane Q file is measured as Q, and the response says so."""
    from app.compute import worker_aperture_photometry
    path = _write_stokes(str(tmp_path / "q.fits"), 2, 5.0)
    w = WCS(fits.getheader(path)).celestial
    c = pixel_to_skycoord((NQ - 1) / 2.0, (NQ - 1) / 2.0, w, origin=0).icrs
    r = worker_aperture_photometry(path, None, float(c.ra.deg), float(c.dec.deg), 3.0, 0.0, 0.0)
    assert r["stokes"] == "Q"
    assert r["resolved_channel"] == 0


def test_mfimage_pairs_compare_only_the_planes_used(tmp_path):
    """Sub-band 0 (file plane 2) agrees; an unused later sub-band does not."""
    from app.compute import worker_polarisation_pi_pa
    from tests.test_polarisation import _make_mfimage_fits
    freqs = [1.0e9, 1.1e9, 1.2e9]
    q = _make_mfimage_fits(stokes_value=2, rm_rad_m2=10.0, freqs_hz=freqs)
    uf = _make_mfimage_fits(stokes_value=3, rm_rad_m2=10.0, quadrature=True,
                            freqs_hz=[1.0e9, 1.15e9, 1.2e9])
    try:
        r = worker_polarisation_pi_pa(q, 2, None, {"U": uf})
        assert r["channel"] == 2
        with pytest.raises(ValueError, match="sub-band frequencies for plane 3"):
            worker_polarisation_pi_pa(q, 3, None, {"U": uf})
    finally:
        import os
        os.unlink(q)
        os.unlink(uf)


def test_narrow_channels_one_channel_apart_are_refused(tmp_path):
    """1 kHz channels at 1.4 GHz: a part per million is more than a channel."""
    from app.compute import worker_polarisation_pi_pa
    q = _write_stokes(str(tmp_path / "q.fits"), 2, 3.0, n_freq=3)
    uf = _write_stokes(str(tmp_path / "u.fits"), 3, 4.0, n_freq=3, freq0=1.4e9 + 1.0e3)
    for p in (q, uf):
        with fits.open(p, mode="update") as hdul:
            hdul[0].header["CDELT3"] = 1.0e3
    with pytest.raises(ValueError, match="spectral"):
        worker_polarisation_pi_pa(q, 1, None, {"U": uf})


def test_a_single_line_free_channel_is_not_read_and_not_checked(tmp_path):
    """One line-free channel is not a noise estimate, so U need not have it."""
    from app.compute import worker_polarisation_pi_pa
    q = _write_stokes(str(tmp_path / "q.fits"), 2, 3.0, n_freq=3)
    uf = _write_stokes(str(tmp_path / "u.fits"), 3, 4.0, n_freq=1)
    r = worker_polarisation_pi_pa(q, 0, [2], {"U": uf})
    assert r["sigma_qu2"] == 0.0


def test_a_degenerate_frequency_axis_is_not_replaced_by_time(tmp_path):
    """(time=3, freq=1, y, x): there is one channel, and TIME is not it."""
    from app.compute import worker_aperture_photometry
    path = str(tmp_path / "time_freq.fits")
    hdu = fits.PrimaryHDU(np.stack([_gauss(float(i + 1))[None] for i in range(3)]))
    h = hdu.header
    _celestial_header(h)
    h["CTYPE3"], h["CUNIT3"], h["CRPIX3"], h["CRVAL3"], h["CDELT3"] = "FREQ", "Hz", 1.0, 1.4e9, 1e3
    h["CTYPE4"], h["CUNIT4"], h["CRPIX4"], h["CRVAL4"], h["CDELT4"] = "TIME", "s", 1.0, 0.0, 1.0
    hdu.writeto(path)
    ra, dec = _centre_sky(path)
    with pytest.raises(ValueError, match="TIME"):
        worker_aperture_photometry(path, 2, ra, dec, 10.0, 0.0, 0.0)
