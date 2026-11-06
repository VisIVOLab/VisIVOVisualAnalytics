"""SoFiA-2 parameters suggested from the cube (issue #64): measurements on
synthetic cubes of known noise, beam, channel width and correlation; the
rules on hand-made measurements; the widened allow-list; the run summary;
the route."""
import asyncio
import dataclasses
import stat

import math

import numpy as np
import pytest
from astropy.io import fits
from scipy.ndimage import gaussian_filter

from app.compute import sofia_suggest as ss
from app.compute.finders.sofia2 import refused, summarise_log

HI = ss.HI_REST_HZ
SIGMA = 1e-3
BEAM_PX = 4.0
PIX_DEG = 2.0 / 3600.0


def _channel_hz(dv_kms: float, nu: float = HI) -> float:
    return dv_kms * nu / ss.C_KMS


def _cube(path, *, nz=64, ny=96, nx=96, hanning=False, sources=(), continuum=None,
          nan_disc=None, zero_disc=None, ctype3="FREQ", dv_kms=5.5, restfreq=True,
          stokes=None, seed=1):
    """A cube of beam-correlated Gaussian noise of σ=SIGMA, FWHM BEAM_PX pixels,
    channels of ``dv_kms`` at the HI line; ``sources`` are (x, y, z, peak in σ)
    Gaussian blobs (negative peak: absorption)."""
    rng = np.random.default_rng(seed)
    noise = rng.normal(size=(nz, ny, nx))
    noise = gaussian_filter(noise, sigma=(0, BEAM_PX / 2.3548, BEAM_PX / 2.3548))
    if hanning:
        noise = 0.25 * np.roll(noise, 1, 0) + 0.5 * noise + 0.25 * np.roll(noise, -1, 0)
    noise *= SIGMA / noise.std()
    zz, yy, xx = np.mgrid[0:nz, 0:ny, 0:nx]
    for x0, y0, z0, peak in sources:
        noise += peak * SIGMA * np.exp(-0.5 * (((xx - x0) / 4) ** 2 + ((yy - y0) / 4) ** 2 + ((zz - z0) / 5) ** 2))
    if continuum is not None:
        x0, y0, r, level, slope = continuum
        disc = (xx - x0) ** 2 + (yy - y0) ** 2 < r ** 2
        noise += disc * SIGMA * (level + slope * (zz / (nz - 1) - 0.5))
    if nan_disc is not None:
        noise[:, (yy[0] - ny / 2) ** 2 + (xx[0] - nx / 2) ** 2 > nan_disc ** 2] = np.nan
    if zero_disc is not None:
        noise[:, (yy[0] - ny / 2) ** 2 + (xx[0] - nx / 2) ** 2 > zero_disc ** 2] = 0.0
    data = noise.astype(np.float32)
    if stokes is not None:
        data = data[None]
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["CTYPE1"], h["CTYPE2"] = "RA---SIN", "DEC--SIN"
    h["CRVAL1"], h["CRVAL2"], h["CRPIX1"], h["CRPIX2"] = 150.0, -29.0, nx / 2, ny / 2
    h["CDELT1"], h["CDELT2"], h["CUNIT1"], h["CUNIT2"] = -PIX_DEG, PIX_DEG, "deg", "deg"
    if ctype3 == "FREQ":
        h["CTYPE3"], h["CUNIT3"], h["CRPIX3"] = "FREQ", "Hz", 1.0
        h["CRVAL3"], h["CDELT3"] = HI, _channel_hz(dv_kms)
    elif ctype3 == "VOPT":
        h["CTYPE3"], h["CUNIT3"], h["CRPIX3"], h["CRVAL3"], h["CDELT3"] = "VOPT", "m/s", 1.0, 3.0e6, dv_kms * 1e3
    else:
        h["CTYPE3"], h["CUNIT3"], h["CRPIX3"], h["CRVAL3"], h["CDELT3"] = ctype3, "", 1.0, 0.0, 1.0
    if stokes is not None:
        h["CTYPE4"], h["CRVAL4"], h["CRPIX4"], h["CDELT4"] = "STOKES", float(stokes), 1.0, 1.0
    if restfreq:
        h["RESTFRQ"] = HI
    h["BMAJ"] = h["BMIN"] = BEAM_PX * PIX_DEG
    h["BUNIT"] = "Jy/beam"
    hdu.writeto(path, overwrite=True)
    return path


def _params(result):
    return {p["key"]: p["value"] for p in result["parameters"]}


def _all_accepted(result):
    assert refused(result["params"]) is None, refused(result["params"])
    assert result["params"] == _params(result)


# ── measurements on synthetic cubes ───────────────────────────────────────────

def test_emission_cube_measures_noise_beam_channels_and_correlation(tmp_path):
    path = _cube(tmp_path / "em.fits", hanning=True,
                 sources=[(30, 30, 20, 12.0), (60, 70, 40, 10.0)])
    r = ss.suggest(str(path))
    m = r["measurements"]
    assert m["sigma"] == pytest.approx(SIGMA, rel=0.05)
    assert m["b_px"] == pytest.approx(BEAM_PX, rel=1e-3)
    assert m["omega_b_px"] == pytest.approx(1.1331 * BEAM_PX ** 2, rel=1e-3)
    assert m["channel_kms"] == pytest.approx(5.5, rel=1e-3)
    assert m["rho"] == pytest.approx(2.0 / 3.0, abs=0.05)       # Hanning: 0.25/0.375
    assert m["l_z"] == 2 and m["f_nan"] == 0.0 and not m["zero_blanked"]
    # A 96² field of 4-px beams: the spread of the σ estimates is noise, and reads as such.
    assert m["r_chan"] < m["r_chan_noise"] and m["r_sky"] < max(1.2, m["r_sky_noise"])
    assert m["cont_fraction"] < 0.01
    assert r["data_type"]["value"] == "emission" and not r["data_type"]["ambiguous"]
    assert r["line"]["value"] == "hi" and not r["line"]["ambiguous"]
    p = _params(r)
    assert p["scfind.kernelsXY"] == "0, 2, 4"
    # 15/5.5 → 3, 30 → 5, 60 → 11, 120 → 21 dropped (over 64/4 channels).
    assert p["scfind.kernelsZ"] == "0, 3, 5, 11"
    # A small cube (N_res ~ 5e4): reliability off, 5σ (Fabio's small-cube rule).
    assert m["n_res"] < ss.N_RES_RELIABILITY_CAVEAT
    assert p["reliability.enable"] == "false" and p["scfind.threshold"] == "5.0"
    assert p["linker.radiusZ"] == "2" and p["linker.minSizeZ"] == "5"
    assert p["input.invert"] == "false" and p["contsub.enable"] == "false"
    assert p["scaleNoise.enable"] == "false"
    # v1 suggests no dilation (the flux goal was left out).
    assert not any(k.startswith("dilation.") for k in p)
    assert all(e["reason"] for e in r["parameters"])
    _all_accepted(r)


def test_uncorrelated_channels(tmp_path):
    path = _cube(tmp_path / "white.fits", sources=[(40, 40, 30, 15.0)])
    r = ss.suggest(str(path))
    m = r["measurements"]
    assert abs(m["rho"]) < 0.1 and m["l_z"] == 1
    assert "goal" not in r
    _all_accepted(r)


def test_absorption_is_deduced_and_searched_inverted(tmp_path):
    path = _cube(tmp_path / "abs.fits", sources=[(30, 30, 20, -14.0), (60, 60, 45, -10.0)])
    r = ss.suggest(str(path))
    p = _params(r)
    assert r["data_type"]["value"] == "absorption" and r["data_type"]["deduced"]
    assert r["measurements"]["noise_side"] == "positive"
    assert r["measurements"]["sigma"] == pytest.approx(SIGMA, rel=0.05)
    assert p["input.invert"] == "true" and p["reliability.enable"] == "false"
    _all_accepted(r)


def test_data_type_override_is_kept(tmp_path):
    path = _cube(tmp_path / "em2.fits", sources=[(30, 30, 20, 12.0)])
    r = ss.suggest(str(path), data_type="absorption")
    assert r["data_type"] == {**r["data_type"], "value": "absorption", "deduced": False, "ambiguous": False}
    assert _params(r)["input.invert"] == "true"


def test_continuum_not_subtracted_is_deduced(tmp_path):
    path = _cube(tmp_path / "cont.fits", continuum=(48, 48, 25, 3.0, 0.0), sources=[(20, 20, 30, 12.0)])
    r = ss.suggest(str(path))
    p = _params(r)
    assert r["data_type"]["value"] == "continuum"
    assert r["measurements"]["cont_fraction"] > 0.1
    assert p["contsub.enable"] == "true" and p["contsub.order"] == "0"
    assert p["reliability.enable"] == "false"
    _all_accepted(r)


def test_a_sloped_continuum_gets_order_one(tmp_path):
    path = _cube(tmp_path / "slope.fits", continuum=(48, 48, 25, 8.0, 4.0))
    p = _params(ss.suggest(str(path)))
    assert p["contsub.enable"] == "true" and p["contsub.order"] == "1"
    assert p["flag.auto"] == "true"            # 6-10σ off zero: strong residuals


def test_emission_filling_the_band_is_not_continuum(tmp_path):
    # A galaxy spanning ~70 % of the channels (NGC 3137 in its WALLABY cubelet):
    # its pixels' median moves, their 10th percentile does not.
    nz = 64
    path = _cube(tmp_path / "wide.fits", nz=nz)
    with fits.open(path, mode="update") as hdul:
        d = hdul[0].data
        yy, xx = np.mgrid[0:96, 0:96]
        disc = (xx - 48) ** 2 + (yy - 48) ** 2 < 15 ** 2
        d[5:50, disc] += 4 * SIGMA
    r = ss.suggest(str(path))
    assert r["measurements"]["cont_fraction"] < 0.01
    assert r["data_type"]["value"] == "emission"


def test_nan_footprint_counts_only_finite_voxels(tmp_path):
    path = _cube(tmp_path / "nan.fits", nan_disc=30, sources=[(48, 48, 30, 12.0)])
    r = ss.suggest(str(path))
    m = r["measurements"]
    assert m["f_nan"] == pytest.approx(1 - np.pi * 30 ** 2 / 96 ** 2, abs=0.03)
    assert m["sigma"] == pytest.approx(SIGMA, rel=0.06)
    assert m["finite_voxels"] == pytest.approx((1 - m["f_nan"]) * 64 * 96 * 96, rel=1e-6)
    assert any(w["code"] == "blank_footprint" for w in r["warnings"])
    _all_accepted(r)


def test_zero_blanks_are_left_out_and_warned(tmp_path):
    path = _cube(tmp_path / "zero.fits", zero_disc=30, sources=[(48, 48, 30, 12.0)])
    r = ss.suggest(str(path))
    m = r["measurements"]
    assert m["zero_blanked"] and m["f_zero"] > 0.3
    assert m["sigma"] == pytest.approx(SIGMA, rel=0.06)
    assert any(w["code"] == "zero_blanks" for w in r["warnings"])
    assert "exact zeros" in next(e for e in r["parameters"] if e["key"] == "scfind.threshold")["reason"]


def test_optical_velocity_axis_and_missing_rest_frequency(tmp_path):
    path = _cube(tmp_path / "vopt.fits", ctype3="VOPT", dv_kms=8.0, restfreq=False,
                 sources=[(30, 30, 20, 12.0)])
    r = ss.suggest(str(path))
    m = r["measurements"]
    v = 3000.0 + 8.0 * 31.5
    assert m["channel_kms"] == pytest.approx(8.0 / (1 + v / ss.C_KMS), rel=1e-3)
    # No rest frequency: HI assumed, and the client is asked.
    assert r["line"]["value"] == "hi" and r["line"]["ambiguous"]
    assert r["line"]["options"] == ["hi", "galactic"]


def test_frequency_axis_without_rest_frequency_keeps_spectral_defaults(tmp_path):
    path = _cube(tmp_path / "norest.fits", restfreq=False, sources=[(30, 30, 20, 12.0)])
    r = ss.suggest(str(path))
    p = _params(r)
    assert r["measurements"]["channel_kms"] is None
    assert p["scfind.kernelsZ"] == "0, 3, 7, 15" and p["linker.minSizeZ"] == "5"
    assert any(w["code"] == "no_channel_width" for w in r["warnings"])


def test_channel_width_conventions():
    nu = np.array([1.40e9, 1.40e9 + 1e5])
    dv, _, _ = ss.channel_width_kms(nu, "Hz", "FREQ", HI)
    assert dv == pytest.approx(ss.C_KMS * 1e5 / (1.40e9 + 5e4))
    assert ss.channel_width_kms(nu, "Hz", "FREQ", None)[0] is None
    v = np.array([10000.0, 10010.0])
    assert ss.channel_width_kms(v, "km/s", "VRAD", HI)[0] == pytest.approx(10 / (1 - 10005 / ss.C_KMS))
    assert ss.channel_width_kms(v, "km/s", "VOPT", HI)[0] == pytest.approx(10 / (1 + 10005 / ss.C_KMS))
    # Paper III VELO is relativistic; the AIPS VELO-xxx form is optical unless
    # VELREF > 256 (as wcslib reads it: absent/0/1/2 → VOPT, 258 → VRAD).
    fast = np.array([0.1 * ss.C_KMS, 0.1 * ss.C_KMS + 10.0])
    assert ss.channel_width_kms(fast, "km/s", "VELO", HI)[0] == pytest.approx(10 / (1 - 0.1 ** 2), rel=1e-4)
    assert ss.velocity_convention("VELO-F2V") == ("relativistic", "VELO, FITS Paper III")
    assert ss.velocity_convention("VELO-HEL")[0] == "optical"
    for velref in (0, 1, 2):
        assert ss.velocity_convention("VELO-HEL", velref)[0] == "optical"
    assert ss.velocity_convention("VELO-HEL", 258)[0] == "radio"
    assert ss.velocity_convention("FELO-HEL")[0] == "optical"
    # HCG 44's axis: 8.244 km/s at ~1536 km/s, optical.
    hcg = np.array([1531.9, 1540.144])
    assert ss.channel_width_kms(hcg, "km/s", "VELO-HEL", None)[0] == pytest.approx(
        8.244 / (1 + 1536.0 / ss.C_KMS), rel=1e-4)


def test_rest_frequency_is_read_from_freq0_too():
    assert ss._rest_frequency(fits.Header({"FREQ0": 1420405758.58})) == 1420405758.58
    assert ss._rest_frequency(fits.Header({"RESTFRQ": HI, "FREQ0": 1.0e9})) == HI
    assert ss._rest_frequency(fits.Header({})) is None


def test_correlation_ignores_faint_emission_and_continuum():
    # Independent noise, plus +1σ emission over most of the field and band and
    # a continuum offset: a difference cancels both, so ρ stays ~0.
    rng = np.random.default_rng(3)
    a = rng.normal(size=(40, 100, 100)).astype(np.float32)
    b = rng.normal(size=a.shape).astype(np.float32)
    a[:, :, :75] += 1.0
    b[:, :, :75] += 1.0
    a[:, :20, :] += 5.0
    b[:, :20, :] += 5.0
    p = ss._Pass(a=a, b=b, z=np.arange(0, 80, 2), stride=1, f_nan=0.0, f_zero=0.0, zero_blanked=False)
    assert abs(ss._channel_correlation(p, 1.0)) < 0.03
    # Hanning-smoothed: 2/3.
    n = rng.normal(size=(81, 100, 100))
    h = 0.25 * n[:-2] + 0.5 * n[1:-1] + 0.25 * n[2:]
    h /= h.std()
    p = ss._Pass(a=h[0:78:2].astype(np.float32), b=h[1:79:2].astype(np.float32), z=np.arange(0, 78, 2),
                 stride=1, f_nan=0.0, f_zero=0.0, zero_blanked=False)
    assert ss._channel_correlation(p, 1.0) == pytest.approx(2.0 / 3.0, abs=0.02)


def test_reasons_name_dropped_kernels_and_the_linker_condition():
    params, _ = ss.rules(_m(shape=[109, 280, 196], b_px=5.0, omega_b_px=28.5, channel_kms=3.92,
                            rho=0.011, l_z=1, n_res=0.21e6), "emission", "hi")
    by = {p["key"]: p["reason"] for p in params}
    assert "the 120 km/s step (31 channels) is dropped: more than a quarter of the 109 channels" in by["scfind.kernelsZ"]
    assert "narrow (3.92 < 5 km/s)" in by["linker.radiusZ"]
    params, _ = ss.rules(_m(), "emission", "hi")
    by = {p["key"]: p["reason"] for p in params}
    assert "correlated (ρ=0.74)" in by["linker.radiusZ"]
    assert "the 15 km/s step (1 channel) is dropped" in by["scfind.kernelsZ"]


@pytest.mark.parametrize("kwargs, message", [
    ({"stokes": 2}, "Stokes Q"),
    ({"ctype3": "LINEAR"}, "no spectral axis"),
])
def test_cubes_out_of_scope_are_refused(tmp_path, kwargs, message):
    path = _cube(tmp_path / "out.fits", **kwargs)
    with pytest.raises(ss.SuggestRefused, match=message):
        ss.suggest(str(path))


def test_stokes_i_with_a_degenerate_axis_is_accepted(tmp_path):
    path = _cube(tmp_path / "i.fits", stokes=1, sources=[(30, 30, 20, 12.0)])
    assert ss.suggest(str(path))["valid"]


def test_a_two_dimensional_image_is_refused(tmp_path):
    path = tmp_path / "img.fits"
    fits.PrimaryHDU(np.zeros((32, 32), np.float32)).writeto(path)
    with pytest.raises(ss.SuggestRefused):
        ss.suggest(str(path))


def test_bad_choices_are_value_errors(tmp_path):
    path = _cube(tmp_path / "c.fits")
    for kwargs in ({"data_type": "maser"}, {"line": "co"}):
        with pytest.raises(ValueError):
            ss.suggest(str(path), **kwargs)


# ── the rules on hand-made measurements ──────────────────────────────────────

def _m(**overrides):
    base = dict(
        shape=[158, 700, 700], sample_voxels=4_000_000, stride_xy=4, channel_pairs=79, sigma=2.25e-4,
        noise_side="negative", sigma_negative=2.25e-4, sigma_positive=2.25e-4, bunit="Jy/beam",
        beam_major_arcsec=53.0, beam_minor_arcsec=32.7, pixel_arcsec=8.0, b_px=5.2, omega_b_px=30.7,
        channel_kms=8.24, channel_note="", band_kms=1300.0, rest_frequency_hz=HI, spectral_ctype="FREQ",
        rho=0.741, l_z=2, r_chan=1.08, r_chan_noise=1.04, r_sky=1.03, r_sky_noise=1.03, sky_blocks=64, f_nan=0.0, f_zero=0.0, zero_blanked=False,
        n_res=1.26e6, finite_voxels=7.7e7, cont_fraction=0.0, cont_strong_fraction=0.0, cont_slope_sigma=None,
        channel_offsets=0.0,
        tail_high=800, tail_low=1, tail_expected=1.4, cube_bytes=310_000_000, sofia_memory_bytes=840_000_000,
        node_memory_bytes=64 << 30)
    base.update(overrides)
    return ss.Measurements(**base)


def _rules(m, data_type="emission", ladder="hi"):
    params, warnings = ss.rules(m, data_type, ladder)
    as_map = {p["key"]: p["value"] for p in params}
    assert refused(as_map) is None, refused(as_map)
    return as_map, warnings


def test_rules_reproduce_the_spec_table_for_hcg44():
    p, w = _rules(_m())
    assert p["scfind.kernelsXY"] == "0, 3, 6" and p["scfind.kernelsZ"] == "0, 3, 7, 15"
    assert p["reliability.enable"] == "true" and p["scfind.threshold"] == "4.0"
    assert p["linker.radiusXY"] == "2" and p["linker.radiusZ"] == "2"
    assert p["linker.minSizeXY"] == "5" and p["linker.minSizeZ"] == "5"
    assert p["linker.positivity"] == "false" and p["scaleNoise.enable"] == "false"
    assert [x["code"] for x in w] == ["reliability_caveat"]


def test_rules_for_wallaby_small_cube():
    p, _ = _rules(_m(shape=[109, 280, 196], b_px=5.0, omega_b_px=28.5, channel_kms=3.94, rho=0.024,
                     l_z=1, n_res=0.21e6))
    assert p["reliability.enable"] == "false" and p["scfind.threshold"] == "5.0"
    # 120 km/s → 31 channels is over N_ch/4 = 27: dropped by the cap.
    assert p["scfind.kernelsZ"] == "0, 3, 7, 15"
    assert p["linker.radiusZ"] == "2" and p["linker.minSizeZ"] == "5"


def test_rules_reliability_and_threshold_follow_n_res():
    p, w = _rules(_m(n_res=3e6))
    assert p["reliability.enable"] == "true" and p["scfind.threshold"] == "4.0" and not w
    p, _ = _rules(_m(n_res=2e7))
    assert p["scfind.threshold"] == "3.5" and p["scfind.kernelsXY"] == "0, 3, 6"


def test_rules_noise_scaling():
    p, _ = _rules(_m(r_sky=1.3))
    assert p["scaleNoise.enable"] == "true" and p["scaleNoise.mode"] == "local"
    assert p["scaleNoise.windowXY"] == "27" and p["scaleNoise.windowZ"] == "31"
    assert p["reliability.enable"] == "true"          # scaled: the symmetry holds
    p, _ = _rules(_m(r_chan=1.2))
    assert p["scaleNoise.enable"] == "true" and p["scaleNoise.mode"] == "spectral"


def test_rules_galactic_ladder_and_no_beam():
    p, w = _rules(_m(channel_kms=0.5, b_px=None, omega_b_px=None, n_res=None), ladder="galactic")
    # 2/0.5 → 5, 5 → 11, 10 → 21, 20 → 41 (dropped: over 158/4).
    assert p["scfind.kernelsZ"] == "0, 5, 11, 21"
    assert p["linker.minSizeZ"] == "5" and p["scfind.kernelsXY"] == "0, 3, 6"
    assert p["reliability.enable"] == "false"
    assert {"no_beam"} <= {x["code"] for x in w}


def test_rules_propose_a_region_when_the_cube_does_not_fit():
    p, w = _rules(_m(cube_bytes=40 << 30, sofia_memory_bytes=int(2.3 * (40 << 30)), node_memory_bytes=32 << 30))
    region = [int(v) for v in p["input.region"].split(",")]
    assert len(region) == 6 and region[4:] == [0, 157]
    assert 0 < region[0] < region[1] < 700
    assert any(x["code"] == "memory" for x in w)
    # The region fits the node, by the finder's own memory model.
    from app.compute.finders.sofia2 import MEMORY_FACTOR, MEMORY_FLOOR_BYTES

    wx, wy = region[1] - region[0] + 1, region[3] - region[2] + 1
    assert MEMORY_FACTOR * (40 << 30) * wx * wy / 700 ** 2 + MEMORY_FLOOR_BYTES <= 32 << 30
    # And the rules are the sub-cube's: 3e7 resolution elements become ~1e7 → no 3.5σ.
    p, _ = _rules(_m(n_res=3e7, cube_bytes=40 << 30, sofia_memory_bytes=int(2.3 * (40 << 30)),
                     node_memory_bytes=8 << 30))
    assert p["scfind.threshold"] == "4.0" and p["scfind.kernelsXY"] == "0, 3, 6"


def test_no_region_is_proposed_when_none_would_fit():
    p, w = _rules(_m(shape=[1000, 10000, 10000], cube_bytes=4 * 10 ** 11,
                     sofia_memory_bytes=int(2.3 * 4e11), node_memory_bytes=130 << 20))
    assert "input.region" not in p
    assert any(x["code"] == "memory" and "more memory" in x["message"] for x in w)


def test_rules_absorption_and_continuum_keep_reliability_off():
    p, _ = _rules(_m(n_res=5e6), data_type="absorption")
    assert p["input.invert"] == "true" and p["reliability.enable"] == "false"
    p, _ = _rules(_m(n_res=5e6, cont_fraction=0.2, cont_strong_fraction=0.0), data_type="continuum")
    assert p["contsub.enable"] == "true" and p["reliability.enable"] == "false"


def test_measurements_are_json_plain():
    import json

    json.dumps(dataclasses.asdict(_m()))


# ── the allow-list (B2) and the log summary ──────────────────────────────────

def test_the_allowlist_takes_the_sections_the_suggester_needs():
    assert refused({"scaleNoise.enable": True, "scaleNoise.mode": "local", "filter.minSNR": 3.0,
                    "filter.discardNegative": True, "contsub.enable": True, "contsub.order": 1,
                    "input.invert": True, "input.region": "0, 10, 0, 10, 0, 5"}) is None


@pytest.mark.parametrize("params, fragment", [
    ({"input.data": "/etc/passwd"}, "input files are set by the service"),
    ({"input.mask": "/x.fits"}, "input files are set by the service"),
    ({"input.region": "0, 10; rm"}, "six pixel indices"),
    ({"output.filename": "x"}, "output section is set by the service"),
    ({"scfind.kernelXY": 3}, "did you mean 'scfind.kernelsXY'"),
    ({"merge.enable": True}, "'merge.enable' is not a SoFiA-2 2.7 parameter"),
    ({"foo.bar": 1}, "'foo.bar' is not a SoFiA-2 2.7 parameter"),
    ({"background.enable": True}, "does not offer the background section"),
    ({"filter.minSNR": "3\noutput.directory = /tmp"}, "invalid value"),
    ({"scaleNoise.typo": 1}, "not a SoFiA-2 2.7 parameter"),
])
def test_the_allowlist_still_refuses(params, fragment):
    assert fragment in (refused(params) or "")


# SoFiA-2 checks no value: "bogus" runs as the standard deviation, "abc" as 0,
# "True" as off. Each is refused before the run, with what SoFiA would take.
@pytest.mark.parametrize("params, fragment", [
    ({"scfind.statistic": "bogus"}, "must be one of std, mad, gauss"),
    ({"scfind.fluxRange": "all"}, "must be one of negative, positive, full"),
    ({"dilation.iterationsXY": "abc"}, "must be a whole number"),
    ({"linker.radiusXY": "2.5"}, "must be a whole number"),
    ({"scfind.threshold": "4 sigma"}, "must be a number"),
    ({"scfind.threshold": "nan"}, "must be a number"),
    ({"scfind.threshold": "1_0"}, "must be a number"),
    ({"reliability.enable": "yes"}, "must be true or false"),
    ({"scfind.kernelsXY": "0 3 6"}, "numbers separated by commas"),
    ({"scfind.kernelsZ": ""}, "has no value"),
    ({"reliability.parameters": "peak"}, "needs at least two"),
    ({"reliability.parameters": "peak, flux"}, "does not know 'flux'"),
    ({"flag.region": "0, 1, 2, 3, 4"}, "groups of six pixel indices"),
    ({"parameter.prefix": "a b"}, "letters, digits or underscores"),
    # Other scripts' digits: strtod and strtol read them as 0.
    ({"scfind.threshold": "\u0664"}, "must be a number"),
    ({"linker.radiusXY": "\u0662"}, "must be a whole number"),
    ({"scfind.kernelsZ": "0, \u0663"}, "whole numbers separated by commas"),
    ({"scfind.kernelsZ": "0, 3.5"}, "whole numbers separated by commas"),
    ({"reliability.parameters": "peak\tsum"}, "names separated by commas"),
    # Overflows to infinity in strtod.
    ({"scfind.threshold": "1e999"}, "must be a number"),
    ({"scfind.kernelsXY": "0, 1e999"}, "numbers separated by commas"),
])
def test_values_sofia_would_misread_are_refused(params, fragment):
    assert fragment in (refused(params) or "")


@pytest.mark.parametrize("params", [
    {"scfind.statistic": "std"}, {"scfind.threshold": "4"}, {"scfind.threshold": 3.5},
    {"scfind.threshold": "1e1"}, {"linker.radiusXY": 2}, {"reliability.enable": "True"},
    {"scfind.kernelsXY": "0,3, 6"}, {"scfind.kernelsXY": "0, 3.5, 6"},
    {"scfind.kernelsXY": "0,\t3.5, 6"}, {"scfind.kernelsZ": "0,\t3, 7"},
    {"reliability.parameters": "peak sum mean"},
    {"flag.region": "0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11"}, {"flag.auto": "channels"},
    {"input.region": ""},
])
def test_values_sofia_reads_as_meant_are_accepted(params):
    assert refused(params) is None, refused(params)


def test_a_switch_is_written_the_one_way_sofia_reads_as_on():
    from app.compute.finders.sofia2 import par_lines

    assert par_lines("/c.fits", "/o", {"reliability.enable": "True"})[-1] == "reliability.enable = true"


def test_every_accepted_key_has_its_sofia_default():
    from app.compute.finders.sofia2 import DEFAULTS, KNOWN_KEYS, _FILE_INPUTS

    assert set(DEFAULTS) == KNOWN_KEYS - _FILE_INPUTS
    # The template .par of SoFiA-2 2.7.0, for the keys most often added by hand.
    assert DEFAULTS["dilation.enable"] == "false"
    assert DEFAULTS["scfind.kernelsZ"] == "0, 3, 7, 15"
    assert DEFAULTS["input.region"] == ""
    assert all(refused({k: v}) is None for k, v in DEFAULTS.items() if v), \
        [k for k, v in DEFAULTS.items() if v and refused({k: v})]


ANSI_LOG = (
    "\x1b[36m____\x1b[0m\n  Using stride of dx = 8, dy = 8.\n"
    "  Pearson coefficient (adjacent channels): 0.741\n"
    "\x1b[33mWARNING: Significant channel-to-channel noise correlation detected.\x1b[0m\n"
    "  Found 53 positive and 33 negative sources.\n"
    "\x1b[33mWARNING: Only 33 negative detections found.\n         Reliability calculation may be inaccurate.\x1b[0m\n"
    "  18 sources left after reliability analysis and filtering.\n")


def test_the_log_summary():
    s = summarise_log(ANSI_LOG)
    assert (s["n_positive"], s["n_negative"], s["rho"], s["n_after_filter"]) == (53, 33, 0.741, 18)
    assert s["warnings"] == ["Significant channel-to-channel noise correlation detected.",
                             "Only 33 negative detections found."]
    assert s["note"] == "Reliability ran on 33 negative and 53 positive detections."
    empty = summarise_log("")
    assert empty["n_negative"] is None and empty["note"] == ""


# ── routes ───────────────────────────────────────────────────────────────────

async def _open(client, headers, path):
    body = (await client.post("/v1/datasets/open", json={"path": str(path)}, headers=headers)).json()
    assert body["valid"], body["error"]
    return body["dataset_id"], {**headers, "X-Visivo-Session": body["session_id"]}


@pytest.mark.asyncio
async def test_the_suggest_route(client, auth_headers, tmp_path, synthetic_fits_image):
    path = _cube(tmp_path / "route.fits", hanning=True, sources=[(30, 30, 20, 12.0)])
    dataset_id, headers = await _open(client, auth_headers, path)
    # A client of the first draft still sends "goal": ignored, not refused.
    resp = await client.post(f"/v1/sofia2/suggest/{dataset_id}", headers=headers, json={"goal": "flux"})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"] and body["dataset_id"] == dataset_id and "goal" not in body
    assert body["sofia_version"] == "2.7.0"
    assert set(body["data_type"]) >= {"value", "deduced", "ambiguous", "reason", "options", "confidence"}
    assert not any(k.startswith("dilation.") for k in body["params"])
    assert {p["key"] for p in body["parameters"]} == set(body["params"])
    assert body["measurements"]["l_z"] == 2
    assert refused(body["params"]) is None
    # Every key a run accepts, with SoFiA's default: what a client shows for
    # a key the user adds.
    assert body["sofia_defaults"]["dilation.enable"] == "false"
    assert body["sofia_defaults"]["input.region"] == ""
    assert "output.filename" not in body["sofia_defaults"]
    assert (await client.post(f"/v1/sofia2/suggest/{dataset_id}", headers=headers, json={})).status_code == 200
    bad = await client.post(f"/v1/sofia2/suggest/{dataset_id}", headers=headers, json={"line": "co"})
    assert bad.status_code == 422
    image_id, image_headers = await _open(client, auth_headers, synthetic_fits_image)
    wrong = await client.post(f"/v1/sofia2/suggest/{image_id}", headers=image_headers, json={})
    assert wrong.status_code == 422


@pytest.mark.asyncio
async def test_the_suggest_route_refuses_a_stokes_q_cube(client, auth_headers, tmp_path):
    dataset_id, headers = await _open(client, auth_headers, _cube(tmp_path / "q.fits", stokes=2))
    resp = await client.post(f"/v1/sofia2/suggest/{dataset_id}", headers=headers, json={})
    assert resp.status_code == 422 and "Stokes Q" in resp.json()["detail"]


def _fake_sofia(tmp_path, body: str) -> str:
    script = tmp_path / "sofia2"
    script.write_text("#!/bin/sh\n" + body)
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return str(script)


@pytest.mark.asyncio
async def test_the_synchronous_route_refuses_what_the_task_route_refuses(
        client, auth_headers, synthetic_fits_cube, tmp_path, monkeypatch):
    marker = tmp_path / "ran"
    monkeypatch.setenv("VISIVO_SOFIA_BIN", _fake_sofia(tmp_path, f'touch "{marker}"\n'))
    dataset_id, headers = await _open(client, auth_headers, synthetic_fits_cube)
    for params in ({"merge.enable": True}, {"scfind.kernelXY": 3}, {"input.data": "/etc/passwd"},
                   {"output.directory": "/tmp/x"}):
        body = (await client.post(f"/v1/sourcefind/{dataset_id}", headers=headers,
                                  json={"dataset_id": dataset_id, "sofia_params": params})).json()
        assert body["valid"] is False and body["error"] == refused(params)
    assert not marker.exists()


@pytest.mark.asyncio
async def test_the_task_result_carries_the_run_summary(client, auth_headers, synthetic_fits_cube,
                                                       tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_SOFIA_BIN", _fake_sofia(tmp_path, '''
grep -q "scaleNoise.enable = true" "$1" || exit 4
grep -q "input.invert = true" "$1" || exit 5
echo "  Pearson coefficient (adjacent channels): 0.123"
echo "  Found 7 positive and 4 negative sources."
echo "  2 sources left after reliability analysis and filtering."
'''))
    dataset_id, headers = await _open(client, auth_headers, synthetic_fits_cube)
    created = await client.post("/v1/tasks/sourcefind", headers=headers, json={
        "dataset_id": dataset_id, "finder": "sofia2",
        "params": {"scaleNoise.enable": True, "input.invert": True}})
    assert created.status_code == 200, created.text
    for _ in range(400):
        body = (await client.get(f"/v1/tasks/{created.json()['task_id']}", headers=headers)).json()
        if body["status"] in ("completed", "failed", "cancelled"):
            break
        await asyncio.sleep(0.05)
    assert body["status"] == "completed", body
    summary = body["result"]["summary"]
    assert (summary["n_positive"], summary["n_negative"], summary["rho"], summary["n_after_filter"]) == (7, 4, 0.123, 2)


def test_noise_rising_over_the_band_turns_on_spectral_scaling(tmp_path):
    path = _cube(tmp_path / "band.fits", nz=64, ny=128, nx=128, sources=[(30, 30, 20, 12.0)])
    with fits.open(path, mode="update") as hdul:
        hdul[0].data *= np.linspace(1.0, 1.6, 64, dtype=np.float32)[:, None, None]
    r = ss.suggest(str(path))
    p = _params(r)
    assert r["measurements"]["r_chan"] > r["measurements"]["r_chan_noise"]
    assert p["scaleNoise.enable"] == "true" and p["scaleNoise.mode"] == "spectral"


def test_noise_rising_to_the_field_edge_turns_on_local_scaling(tmp_path):
    path = _cube(tmp_path / "edge.fits", nz=64, ny=128, nx=128, sources=[(64, 64, 20, 12.0)])
    with fits.open(path, mode="update") as hdul:
        yy, xx = np.mgrid[0:128, 0:128]
        gain = 1.0 + 1.5 * (((xx - 64) ** 2 + (yy - 64) ** 2) / 64.0 ** 2)
        hdul[0].data *= gain.astype(np.float32)[None]
    r = ss.suggest(str(path))
    p = _params(r)
    assert p["scaleNoise.enable"] == "true" and p["scaleNoise.mode"] == "local"
    assert int(p["scaleNoise.windowXY"]) % 2 == 1 and int(p["scaleNoise.windowZ"]) >= 15
    _all_accepted(r)


def test_a_continuum_under_the_whole_field_is_seen(tmp_path):
    path = _cube(tmp_path / "flat.fits", continuum=(48, 48, 1000, 10.0, 0.0))
    r = ss.suggest(str(path))
    assert r["measurements"]["cont_fraction"] > 0.9
    assert r["data_type"]["value"] == "continuum" and _params(r)["contsub.enable"] == "true"


def test_absorption_against_continuum_is_measured_on_the_cube_as_searched(tmp_path):
    # Continuum not subtracted, with absorption dips: the type becomes continuum
    # BEFORE the sample's sign is chosen, so the noise is read on the negative side.
    path = _cube(tmp_path / "absc.fits", continuum=(48, 48, 30, 6.0, 0.0),
                 sources=[(48, 48, 20, -20.0), (40, 50, 40, -15.0)])
    r = ss.suggest(str(path))
    assert r["data_type"]["value"] == "continuum"
    assert r["measurements"]["noise_side"] == "negative"
    assert _params(r)["input.invert"] == "false"


def test_scaled_integer_cube_with_blanks(tmp_path):
    path = _cube(tmp_path / "f.fits", sources=[(30, 30, 20, 12.0)])
    data = fits.getdata(path)
    header = fits.getheader(path)
    bscale = SIGMA / 100.0
    stored = np.round(data / bscale).astype(np.int16)
    stored[:, :8, :] = -32768
    hdu = fits.PrimaryHDU(stored, header=header)
    hdu.header["BSCALE"], hdu.header["BZERO"], hdu.header["BLANK"] = bscale, 0.0, -32768
    scaled = tmp_path / "scaled.fits"
    hdu.writeto(scaled)
    r = ss.suggest(str(scaled))
    m = r["measurements"]
    assert m["sigma"] == pytest.approx(SIGMA, rel=0.06)
    assert m["f_nan"] == pytest.approx(8 / 96, abs=0.01)
    assert m["cube_bytes"] == 64 * 96 * 96 * 4          # SoFiA holds 16-bit data as float32
    assert m["cube_bytes"] == 64 * 96 * 96 * 4          # SoFiA holds 16-bit data as float32


def test_the_sample_stays_bounded_on_an_elongated_field():
    for nz, ny, nx in ((512, 2, 10_000_000), (64, 96, 96), (6668, 1286, 1286), (2, 3, 4)):
        z, s = ss._plan(nz, ny, nx)
        n = 2 * z.size * len(range(0, ny, s)) * len(range(0, nx, s))
        assert n <= 2.1 * ss.SAMPLE_VOXELS


def test_noise_higher_in_part_of_the_field_is_not_continuum(tmp_path):
    path = _cube(tmp_path / "patchy.fits", nz=96, ny=128, nx=128)
    with fits.open(path, mode="update") as hdul:
        hdul[0].data[:, :, 32:] *= 4.0
    r = ss.suggest(str(path))
    assert r["measurements"]["cont_fraction"] < 0.01
    assert r["data_type"]["value"] != "continuum"


def test_the_continuum_test_has_few_false_alarms_on_noise():
    rng = np.random.default_rng(7)
    a = rng.normal(size=(32, 200, 200)).astype(np.float32)
    p = ss._Pass(a=a, b=rng.normal(size=a.shape).astype(np.float32), z=np.arange(0, 64, 2), stride=1, f_nan=0.0, f_zero=0.0, zero_blanked=False)
    frac, _, _, _ = ss._continuum(p, 1.0)
    assert frac < 1e-3


def test_cubes_sofia_cannot_read_as_they_are_are_refused(tmp_path):
    # SoFiA-2 reads the primary HDU, with the spectrum on axis 3.
    path = _cube(tmp_path / "plain.fits", sources=[(30, 30, 20, 12.0)])
    data, header = fits.getdata(path), fits.getheader(path)
    comp = tmp_path / "comp.fits"
    fits.HDUList([fits.PrimaryHDU(), fits.CompImageHDU(data, header=header)]).writeto(comp)
    with pytest.raises(ss.SuggestRefused, match="extension"):
        ss.suggest(str(comp))
    transposed = tmp_path / "t.fits"
    h = header.copy()
    for k in ("CTYPE", "CUNIT", "CRPIX", "CRVAL", "CDELT"):
        h[f"{k}1"], h[f"{k}3"] = header[f"{k}3"], header[f"{k}1"]
    fits.PrimaryHDU(np.ascontiguousarray(np.swapaxes(data, 0, 2)), header=h).writeto(transposed)
    with pytest.raises(ss.SuggestRefused, match="axis 3"):
        ss.suggest(str(transposed))
    short = _cube(tmp_path / "short.fits", nz=4)
    with pytest.raises(ss.SuggestRefused, match="too few"):
        ss.suggest(str(short))
    gz = tmp_path / "cube.fits.gz"
    fits.PrimaryHDU(data, header=header).writeto(gz)
    with pytest.raises(ss.SuggestRefused, match="gzip"):
        ss.suggest(str(gz))


def test_default_spectral_kernels_fit_a_short_band_of_unknown_channel_width():
    # SoFiA refuses a kernel as long as the cube: an 8-channel band keeps none
    # of 3, 7, 15 (each over a quarter of it), a 32-channel band keeps 3 and 7.
    p, w = _rules(_m(shape=[8, 64, 64], channel_kms=None))
    assert p["scfind.kernelsZ"] == "0"
    assert any(x["code"] == "no_channel_width" for x in w)
    p, _ = _rules(_m(shape=[32, 64, 64], channel_kms=None))
    assert p["scfind.kernelsZ"] == "0, 3, 7"


def test_a_constant_offset_is_not_read_as_correlated_noise(tmp_path):
    # White noise is uncorrelated whatever level it sits on: a channel
    # difference cancels the level, so must the σ it is set against.
    flat = ss.suggest(str(_cube(tmp_path / "flat.fits")))["measurements"]["rho"]
    path = _cube(tmp_path / "offset.fits")
    with fits.open(path, mode="update") as hdul:
        hdul[0].data += 6 * SIGMA
    raised = ss.suggest(str(path))["measurements"]
    assert abs(flat) < 0.15 and raised["rho"] == pytest.approx(flat, abs=0.05)
    assert raised["l_z"] == 1


def _rho_of(cube):
    p = ss._read_pass(cube)
    lo, hi, _ = ss._side_sigma(np.concatenate([p.a.ravel(), p.b.ravel()]), min_lower=500, centre=0.0)
    return ss._noise_rho(p, min(x for x in (lo, hi) if x))


@pytest.mark.parametrize("nz", [8, 16, 192])
def test_rho_reads_correlated_noise_on_short_and_long_bands(nz):
    # Noise with ρ = 0.4 between neighbours, decaying geometrically: the σ at
    # lag 4 keeps 0.4⁴ of it, so 1 − 0.6/(1 − 0.0256) = 0.384 is expected.
    rng = np.random.default_rng(7)
    c = rng.normal(size=(nz, 128, 128)).astype("f4")
    for z in range(1, nz):
        c[z] = 0.4 * c[z - 1] + math.sqrt(1 - 0.16) * c[z]
    assert _rho_of(c) == pytest.approx(0.384, abs=0.03)


@pytest.mark.parametrize("offset", [0.25, 0.5, 1.0, 2.0, 6.0])
def test_a_level_does_not_hide_correlated_noise(offset):
    # A level moves the σ about zero (down, when it leaves fewer values on
    # the noise side); neither difference sees it.
    rng = np.random.default_rng(3)
    h = rng.normal(size=(194, 128, 128)).astype("f4")
    c = 0.25 * h[:-2] + 0.5 * h[1:-1] + 0.25 * h[2:]
    c /= np.std(c)
    assert _rho_of(c + offset) == pytest.approx(2 / 3, abs=0.03)


@pytest.mark.parametrize("steepness", [0, 2, 4])
def test_a_primary_beam_noise_gradient_does_not_bias_rho(steepness):
    # ρ = 0.35 AR(1) under a radial noise gain: both differences are taken at
    # the same pixels, so the gain weighs them alike (expected 0.336).
    rng = np.random.default_rng(18)
    c = rng.normal(size=(192, 96, 96)).astype("f4")
    for z in range(1, 192):
        c[z] = 0.35 * c[z - 1] + math.sqrt(1 - 0.35 ** 2) * c[z]
    y, x = np.indices((96, 96))
    gain = np.exp(steepness * (((x - 47.5) / 48) ** 2 + ((y - 47.5) / 48) ** 2))
    assert _rho_of(c * gain) == pytest.approx(0.336, abs=0.03)


@pytest.mark.parametrize("peak", [5.0, 10.0])
def test_field_filling_emission_with_a_velocity_gradient_is_not_correlated_noise(peak):
    # A Galactic-cloud cutout: a line 3 channels wide over the whole field,
    # its centre moving over 40 of 64 channels, on beam-smoothed noise.
    from scipy.ndimage import gaussian_filter

    rng = np.random.default_rng(25)
    noise = gaussian_filter(rng.normal(size=(64, 96, 96)), sigma=(0, 4 / 2.3548, 4 / 2.3548))
    noise /= noise.std()
    y, x = np.indices((96, 96))
    z = np.arange(64)[:, None, None]
    line = peak * np.exp(-0.5 * ((z - (12.8 + 38.4 * x[None] / 95)) / 3) ** 2)
    assert _rho_of((noise + line).astype("f4")) < 0.15


def test_a_line_does_not_hide_correlated_noise():
    rng = np.random.default_rng(25)
    h = rng.normal(size=(194, 96, 96)).astype("f4")
    c = 0.25 * h[:-2] + 0.5 * h[1:-1] + 0.25 * h[2:]
    c /= c.std()
    x = np.indices((96, 96))[1]
    z = np.arange(192)[:, None, None]
    assert _rho_of(c + 6 * np.exp(-0.5 * ((z - (40 + 110 * x[None] / 95)) / 4) ** 2)) == pytest.approx(
        2 / 3, abs=0.03)


def test_rho_reads_hanning_smoothing():
    rng = np.random.default_rng(3)
    h = rng.normal(size=(194, 128, 128)).astype("f4")
    assert _rho_of(0.25 * h[:-2] + 0.5 * h[1:-1] + 0.25 * h[2:]) == pytest.approx(2 / 3, abs=0.03)


@pytest.mark.parametrize("cover", [0.4, 0.6, 0.8])
def test_broad_emission_is_not_read_as_correlated_noise(cover):
    rng = np.random.default_rng(5)
    c = rng.normal(size=(192, 128, 128)).astype("f4")
    c[: int(192 * cover)] += 4.0
    assert abs(_rho_of(c)) < 0.1


@pytest.mark.parametrize("nz, first", [(16, 6), (8, 3), (32, 19)])
def test_emission_over_much_of_a_short_band_is_not_read_as_correlated_noise(nz, first):
    rng = np.random.default_rng(1)
    c = rng.normal(size=(nz, 128, 128)).astype("f4")
    c[:first] += 4.0
    # The pairs straddling the edge of emission over the whole field are a
    # few among many: the medians over pairs leave them out.
    assert abs(_rho_of(c)) < 0.1


def test_a_level_is_read_through_blanked_planes():
    # Only channels 0, 1, 8, 9 hold data, on a +6σ level: the lag-8
    # differences exist and cancel the level; the σ about zero would not.
    rng = np.random.default_rng(31)
    c = np.full((64, 128, 128), np.nan, dtype="f4")
    for z in (0, 1, 8, 9):
        c[z] = rng.normal(size=(128, 128)) + 6.0
    assert ss._lagged_sigma(ss._read_pass(c)) == pytest.approx(1.0, rel=0.05)
    assert abs(_rho_of(c)) < 0.1


def test_rho_falls_back_to_the_sigma_about_zero_on_a_blanked_band():
    # One finite pair per pixel: no channels far enough apart.
    rng = np.random.default_rng(1)
    c = np.full((64, 128, 128), np.nan, dtype="f4")
    c[:2] = rng.normal(size=(2, 128, 128))
    assert ss._lagged_sigma(ss._read_pass(c)) is None
    assert abs(_rho_of(c)) < 0.1


def test_min_size_z_is_capped_by_a_short_band():
    p, _ = _rules(_m(shape=[10, 700, 700], channel_kms=1.5))
    assert p["linker.minSizeZ"] == "5"          # 20/1.5 → 13, capped at 10/2


@pytest.mark.parametrize("channels", [slice(0, 116), slice(38, 154)])
def test_broad_emission_over_the_whole_field_is_not_continuum(tmp_path, channels):
    # +4σ over 60 % of the band at every pixel: a line, not continuum, since
    # it does not cover both ends of the band.
    path = _cube(tmp_path / "broad.fits", nz=192)
    with fits.open(path, mode="update") as hdul:
        hdul[0].data[channels] += 4 * SIGMA
    r = ss.suggest(str(path))
    assert r["measurements"]["cont_fraction"] < 0.01
    assert _params(r)["contsub.enable"] == "false"
    # Nor varying noise: σ is read from the lower percentiles, not about a centre.
    assert r["measurements"]["sigma"] == pytest.approx(SIGMA, rel=0.06)
    assert _params(r)["scaleNoise.enable"] == "false"
    assert any(w["code"] == "channel_offsets" for w in r["warnings"])


def test_bright_emission_over_part_of_the_band_is_not_absorption(tmp_path):
    path = _cube(tmp_path / "bright.fits", nz=192)
    with fits.open(path, mode="update") as hdul:
        hdul[0].data[:116] += 12 * SIGMA
    r = ss.suggest(str(path))
    assert r["data_type"]["value"] == "emission" and _params(r)["input.invert"] == "false"


@pytest.mark.parametrize("cover", [48, 72])
def test_emission_over_much_of_the_field_is_not_varying_noise(tmp_path, cover):
    path = _cube(tmp_path / "half.fits", nz=192)
    with fits.open(path, mode="update") as hdul:
        hdul[0].data[38:154, :, :cover] += 4 * SIGMA
    r = ss.suggest(str(path))
    assert r["measurements"]["sigma"] == pytest.approx(SIGMA, rel=0.06)
    assert _params(r)["scaleNoise.enable"] == "false"


# ── SoFiA-2 exit 8 and failure messages ──────────────────────────────────────

BOX = ("WARNING: ┌──────────────────────────────────────┐\n"
       "         │ FITS header keyword of CELLSCAL = '1/F' detected.  SoFiA │\n"
       "         │ does not support cubes with variable pixel size.  │\n"
       "         └──────────────────────────────────────┘\n")


def test_failure_detail_surfaces_the_error_lines_past_a_warning_box():
    from app.compute.finders.sofia2 import failure_detail

    out = BOX * 20 + ("\x1b[31mERROR: Unknown parameter settings encountered. Please check\n"
                      "       your input or change 'pipeline.pedantic' to 'false'.\n"
                      "       Terminating with error code 7.\x1b[0m\n")
    detail = failure_detail(out, "")
    assert detail.startswith("Unknown parameter settings encountered. Please check your input")
    assert "error code 7" in detail and len(detail) <= 1200
    assert "\x1b" not in detail
    # SoFiA's rules and box edges are not part of the text.
    ruled = "_" * 70 + "\n  Reading data cube.\n" + "_" * 70 + "\nERROR: Bad thing.\n"
    assert "___" not in failure_detail(ruled, "") and "Reading data cube." in failure_detail(ruled, "")
    assert "┌" not in detail and "└" not in detail
    # No ERROR line: the tail, bounded.
    assert failure_detail("x" * 5000, "").endswith("x") and len(failure_detail("x" * 5000, "")) <= 1200


def test_the_log_summary_reads_boxed_warnings_and_errors():
    s = summarise_log(BOX + "ERROR: No sources left after filtering. Terminating pipeline.\n"
                            "       Terminating with error code 8.\n")
    assert s["warnings"] == ["FITS header keyword of CELLSCAL = '1/F' detected"]
    assert s["errors"] == ["No sources left after filtering. Terminating pipeline."]
    assert s["note"] == "No sources left after filtering."


EXIT8 = '''
d=$(dirname "$1")
printf '%s\\n' "  Pearson coefficient (adjacent channels): 0.741"
printf '%s\\n' "ERROR: No sources left after filtering. Terminating pipeline."
printf '%s\\n' "       Terminating with error code 8."
exit 8
'''


@pytest.mark.asyncio
async def test_exit_8_is_an_empty_result_not_a_failure(client, auth_headers, synthetic_fits_cube,
                                                       tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_SOFIA_BIN", _fake_sofia(tmp_path, EXIT8))
    dataset_id, headers = await _open(client, auth_headers, synthetic_fits_cube)
    created = await client.post("/v1/tasks/sourcefind", headers=headers, json={
        "dataset_id": dataset_id, "finder": "sofia2",
        "params": {"input.invert": "true", "reliability.enable": "false"}})
    task_id = created.json()["task_id"]
    for _ in range(400):
        body = (await client.get(f"/v1/tasks/{task_id}", headers=headers)).json()
        if body["status"] in ("completed", "failed", "cancelled"):
            break
        await asyncio.sleep(0.05)
    assert body["status"] == "completed", body
    result = body["result"]
    assert result["n_sources"] == 0 and result["sources"] == [] and result["exit_code"] == 8
    assert result["summary"]["note"] == "No sources left after filtering." and result["summary"]["rho"] == 0.741
    from pathlib import Path

    assert Path(result["log_path"]).exists()          # the output directory is kept
    # The synchronous route answers the same way.
    sync = (await client.post(f"/v1/sourcefind/{dataset_id}", headers=headers,
                              json={"sofia_params": {"input.invert": True}})).json()
    assert sync["valid"] and sync["n_sources"] == 0 and sync["exit_code"] == 8


@pytest.mark.asyncio
async def test_a_failure_reports_sofias_error_line(client, auth_headers, synthetic_fits_cube,
                                                   tmp_path, monkeypatch):
    body_sh = ("i=0; while [ $i -lt 30 ]; do printf '%s\\n' 'WARNING: ┌───┐' '  │ CELLSCAL box │' '  └───┘';"
               " i=$((i+1)); done\nprintf '%s\\n' 'ERROR: Failed to open input file.'\nexit 3\n")
    monkeypatch.setenv("VISIVO_SOFIA_BIN", _fake_sofia(tmp_path, body_sh))
    dataset_id, headers = await _open(client, auth_headers, synthetic_fits_cube)
    created = await client.post("/v1/tasks/sourcefind", headers=headers,
                                json={"dataset_id": dataset_id, "finder": "sofia2"})
    task_id = created.json()["task_id"]
    for _ in range(400):
        body = (await client.get(f"/v1/tasks/{task_id}", headers=headers)).json()
        if body["status"] in ("completed", "failed", "cancelled"):
            break
        await asyncio.sleep(0.05)
    assert body["status"] == "failed"
    assert "exit 3" in body["error"] and "Failed to open input file." in body["error"]


# ── R2 and C2 (spec §7, rev. 2026-11-05b) ────────────────────────────────────

@pytest.mark.parametrize("b, expected", [
    (5.20, [0, 3, 6]),      # HCG 44
    (5.01, [0, 3, 6]),      # WALLABY
    (2.5, [0, 2, 3]),       # SDC2 ldev, 7"/2.8": k_max = rnd(2.8) = 3, k_mid = rnd(1.5) = 2
    (7.08, [0, 4, 8]),      # k_max = rnd(7.93) = 8
    (1.79, [0, 2]),         # k_max = rnd(2.0) = 2, k_mid = 1 < 2 dropped
    (1.0, [0, 3]),          # k_max = 1 < 2: SoFiA's smallest default
    (4.0, [0, 2, 4]),
    (25.0, [0, 14, 28]),
])
def test_r2_spatial_kernels(b, expected):
    assert ss.spatial_kernels(b) == expected


def test_r2_rounds_halves_up_and_has_no_survey_kernel():
    # 1.12·b = 2.5 → 3 (Python's round would give 2); k_max/2 = 1.5 → 2.
    assert ss.spatial_kernels(2.5 / 1.12) == [0, 2, 3]
    p, _ = _rules(_m(n_res=5e7))
    assert p["scfind.kernelsXY"] == "0, 3, 6"


def test_r2_reason_states_the_resolution_argument():
    params, _ = ss.rules(_m(), "emission", "hi")
    reason = next(x["reason"] for x in params if x["key"] == "scfind.kernelsXY")
    assert "matched filter for point sources" in reason and "1.5 beams" in reason
    assert "6 px" in reason


@pytest.mark.parametrize("band, ladder, on", [
    (1296.0, "hi", False),      # HCG 44
    (428.0, "hi", False),       # WALLABY
    (10171.0, "hi", True),      # Apertif
    (2400.0, "hi", True),       # exactly 20 × 120 km/s
    (399.0, "galactic", False),
    (400.0, "galactic", True),  # 20 × 20 km/s
])
def test_c2_wide_bands_get_order_0_continuum_subtraction(band, ladder, on):
    p, _ = ss.rules(_m(band_kms=band, channel_kms=8.0), "emission", ladder)
    by = {x["key"]: x for x in p}
    assert by["contsub.enable"]["value"] == ("true" if on else "false")
    if on:
        assert by["contsub.order"]["value"] == "0"
        assert "wider than the widest expected line" in by["contsub.enable"]["reason"]
        assert "flag.auto" not in by
        assert by["reliability.enable"]["value"] == "true"       # C2 alone does not switch it off
    else:
        reason = by["contsub.enable"]["reason"]
        assert "already subtracted" not in reason and "sit off zero" in reason


def test_c2_keeps_the_cube_wide_test_for_order_1():
    p, _ = ss.rules(_m(band_kms=10000.0, cont_fraction=0.2, cont_slope_sigma=5.0), "continuum", "hi")
    by = {x["key"]: x["value"] for x in p}
    assert by["contsub.enable"] == "true" and by["contsub.order"] == "1"
