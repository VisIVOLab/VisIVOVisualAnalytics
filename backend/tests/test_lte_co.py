"""LTE analysis of CO isotopologues (issue #416, item 4).

The check numbers come from an independent computation of the same formulas
(the formula sheet behind app/lte_co.py, with CDMS constants); the clump
numbers are use case 13's CHIMPS Clump (13CO and C18O 3-2 Gaussian fits).
Synthetic cubes use a velocity-flat optical depth, for which the integrated
ratio equals the peak ratio and the opacity-corrected column density is exact.
"""
from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import lte_co as L

pytestmark = pytest.mark.filterwarnings("ignore::astropy.wcs.FITSFixedWarning")


# ── the physics against the formula sheet ────────────────────────────────────

def test_tau_from_the_ratio_matches_the_sheet():
    tau, flag = L.tau_from_ratio(4.17, 7.4)
    assert float(tau) == pytest.approx(1.549, abs=1e-3) and int(flag) == L.FLAG_OK
    assert float(tau) / 7.4 == pytest.approx(0.2093, abs=1e-4)
    lo_hi, _ = L.tau_from_ratio([4.84, 3.50], 7.4)
    assert lo_hi == pytest.approx([1.094, 2.154], abs=1e-3)
    assert float(L.ratio_of_tau(tau, 7.4)) == pytest.approx(4.17, rel=1e-10)


def test_tau_flags_outside_one_and_x():
    tau, flag = L.tau_from_ratio(np.array([8.0, 7.4, 1.0, 0.5, np.nan]), 7.4)
    assert list(flag) == [L.FLAG_THIN, L.FLAG_THIN, L.FLAG_NO_SOLUTION, L.FLAG_NO_SOLUTION,
                          L.FLAG_NO_SOLUTION]
    assert tau[0] == 0.0 and tau[1] == 0.0 and np.isnan(tau[2:]).all()


def test_column_density_of_13co_3_2_round_trips():
    ln = L.line("13CO", "3-2")
    assert float(L.j_nu(ln.t0, 20.0)) == pytest.approx(13.1051, abs=1e-4)
    assert float(L.j_nu(ln.t0, 2.73)) == pytest.approx(0.0476, abs=1e-4)
    assert float(L.partition_function(ln.b_hz, 20.0)) == pytest.approx(7.9054, abs=1e-4)
    assert float(L.thin_column_factor(ln, 20.0)) == pytest.approx(5.362e14, rel=1e-3)
    t_peak = (L.j_nu(ln.t0, 20.0) - L.j_nu(ln.t0, 2.73)) * (1 - math.exp(-1.0))
    assert float(t_peak) == pytest.approx(8.254, abs=1e-3)
    w = 1.0645 * float(t_peak) * 2.0
    assert float(L.column_density(w, ln, 20.0, 1.0)) == pytest.approx(1.491e16, rel=1e-3)


def test_column_density_of_c18o_1_0():
    ln = L.line("C18O", "1-0")
    assert float(L.partition_function(ln.b_hz, 15.0)) == pytest.approx(6.0394, abs=1e-4)
    assert float(L.thin_column_factor(ln, 15.0)) == pytest.approx(1.1516e15, rel=1e-3)
    t_peak = float((L.j_nu(ln.t0, 15.0) - L.j_nu(ln.t0, 2.73)) * (1 - math.exp(-0.1)))
    assert t_peak == pytest.approx(1.1063, abs=1e-4)
    assert float(L.column_density(1.0645 * t_peak * 2.0, ln, 15.0, 0.1)) == pytest.approx(
        2.850e15, rel=1e-3)


def test_excitation_temperature_and_mass():
    co10, co32 = L.line("12CO", "1-0"), L.line("12CO", "3-2")
    assert L.tex_from_thick_peak([10.0, 20.0], co10) == pytest.approx([13.416, 23.498], abs=1e-3)
    assert L.tex_from_thick_peak([10.0, 20.0], co32) == pytest.approx([17.007, 27.507], abs=1e-3)
    area = L.pixel_area_cm2(5.0, 7.5 / 3600, 7.5 / 3600)
    assert area == pytest.approx(3.1471e35, rel=1e-4)
    assert L.mass_msun(100 * 1e22, area) == pytest.approx(741.7, rel=1e-3)


def test_abundance_ratio_gradients():
    assert L.abundance_ratio(5.5, "wilson_rood1994") == pytest.approx(7.38, abs=0.01)
    assert L.abundance_ratio(5.5, "milam2005") == pytest.approx(6.82, abs=0.01)
    assert 5.4 < L.galactocentric_radius_kpc(40.157, 0.169, 5.0) < 5.7


# ── the route: the CHIMPS clump from use case 13 ─────────────────────────────

def _clump(**overrides):
    body = {"transition": "3-2", "x_ratio": 7.4, "eta_mb_13co": 0.72, "eta_mb_c18o": 0.72,
            "t_ex_k": 15.0, "measurements": {"w_13co": 5.10, "sigma_13co": 0.21,
                                             "w_c18o": 1.22, "sigma_c18o": 0.19,
                                             "peak_13co": 1.98}}
    body.update(overrides)
    return body


@pytest.mark.asyncio
async def test_the_clump_ratio_gives_its_optical_depth(client, auth_headers):
    body = _clump(measurements={"w_13co": 4.17, "sigma_13co": 0.0, "w_c18o": 1.0,
                                "sigma_c18o": 0.67 / 4.17})
    resp = await client.post("/v1/products/lte_co", json=body, headers=auth_headers)
    out = resp.json()
    assert out["valid"], out["error"]
    reg = out["regions"][0]
    assert reg["ratio"] == pytest.approx(4.17)
    assert reg["sigma_ratio"] == pytest.approx(0.67, abs=1e-6)
    assert reg["tau_13co"] == pytest.approx(1.549, abs=1e-3)
    assert reg["tau_c18o"] == pytest.approx(0.2093, abs=1e-4)
    assert reg["tau_13co_interval"] == pytest.approx([1.094, 2.154], abs=1e-3)


@pytest.mark.asyncio
async def test_the_clump_fits_through_the_route(client, auth_headers):
    resp = await client.post("/v1/products/lte_co", json=_clump(), headers=auth_headers)
    out = resp.json()
    assert out["valid"], out["error"]
    reg = out["regions"][0]
    assert reg["ratio"] == pytest.approx(4.180, abs=1e-3)
    assert reg["sigma_ratio"] == pytest.approx(0.673, abs=1e-3)
    assert reg["tau_13co"] == pytest.approx(1.541, abs=0.01)
    assert reg["tau_c18o"] == pytest.approx(0.208, abs=0.002)
    # N(13CO) at T_ex = 15 K: the thin factor times W_mb times the opacity correction.
    ln = L.line("13CO", "3-2")
    w_mb = 5.10 / 0.72
    expect = float(L.thin_column_factor(ln, 15.0)) * w_mb * reg["tau_13co"] / (
        1 - math.exp(-reg["tau_13co"]))
    assert reg["n_13co_cm2"] == pytest.approx(expect, rel=1e-9)
    assert out["assumptions"]["eta_mb"]["13CO"] == [0.72, "user"]
    assert out["assumptions"]["x_ratio"] == [7.4, "user"]


@pytest.mark.asyncio
async def test_the_clump_tex_from_the_peaks(client, auth_headers):
    body = _clump()
    body.pop("t_ex_k")
    body["measurements"]["peak_c18o"] = 0.617
    out = (await client.post("/v1/products/lte_co", json=body, headers=auth_headers)).json()
    assert out["valid"], out["error"]
    # tau at the peak from the ratio of the peaks (1.98/0.617 = 3.21), not the
    # integrated tau: the latter is a line average and would raise T_ex.
    tau_pk = float(L.tau_from_ratio(1.98 / 0.617, 7.4)[0])
    assert tau_pk == pytest.approx(2.49, abs=0.01)
    ln = L.line("13CO", "3-2")
    expect = float(L.tex_from_peak_and_tau(1.98 / 0.72, tau_pk, ln))
    reg = out["regions"][0]
    assert reg["t_ex_k"] == pytest.approx(expect, rel=1e-9)
    assert 7.0 < reg["t_ex_k"] < 9.3
    assert "peaks" in out["assumptions"]["t_ex"]["source"]


@pytest.mark.asyncio
async def test_tex_at_or_below_the_background_is_refused(client, auth_headers):
    out = (await client.post("/v1/products/lte_co", json=_clump(t_ex_k=2.0),
                             headers=auth_headers)).json()
    assert out["valid"] is False and "T_bg" in out["error"]


@pytest.mark.asyncio
async def test_a_12co_peak_below_the_background_gives_no_columns(client, auth_headers):
    body = _clump(eta_mb_12co=0.72)
    body.pop("t_ex_k")
    body["measurements"]["peak_12co"] = -5.0
    resp = await client.post("/v1/products/lte_co", json=body, headers=auth_headers)
    assert resp.status_code == 200
    reg = resp.json()["regions"][0]
    assert reg["t_ex_k"] is None and reg["n_13co_cm2"] is None
    assert reg["tau_13co"] == pytest.approx(1.541, abs=0.01)


@pytest.mark.asyncio
async def test_eta_mb_is_required_without_a_header_value(client, auth_headers):
    body = _clump()
    body.pop("eta_mb_13co")
    out = (await client.post("/v1/products/lte_co", json=body, headers=auth_headers)).json()
    assert out["valid"] is False and "eta_mb" in out["error"]


@pytest.mark.asyncio
async def test_h2_and_mass_from_a_distance(client, auth_headers):
    body = _clump(distance_kpc=5.0, x_ratio=None)
    body["measurements"].update(glon_deg=40.157, glat_deg=0.169, n_pixels=29, pixel_arcsec=7.6)
    out = (await client.post("/v1/products/lte_co", json=body, headers=auth_headers)).json()
    assert out["valid"], out["error"]
    a = out["assumptions"]
    r_gc = L.galactocentric_radius_kpc(40.157, 0.169, 5.0)
    assert a["r_gc_kpc"][0] == pytest.approx(r_gc)
    assert a["x_ratio"][0] == pytest.approx(L.abundance_ratio(r_gc, "milam2005"))
    reg = out["regions"][0]
    x18 = 8.5e-5 / L.oxygen_ratio(r_gc)
    assert reg["n_h2_cm2"] == pytest.approx(reg["n_c18o_cm2"] / x18, rel=1e-9)
    area = L.pixel_area_cm2(5.0, 7.6 / 3600, 7.6 / 3600)
    assert reg["mass_msun"] == pytest.approx(
        2.8 * L.M_H_G * reg["n_h2_cm2"] * 29 * area / L.M_SUN_G, rel=1e-9)


# ── the route: synthetic cubes of known tau and T_ex ─────────────────────────

NX, NY, NZ = 12, 10, 60
V0, DV = 60.0, 0.5          # km/s
X_TRUE, TEX_TRUE, ETA = 7.4, 20.0, 0.72


def _tau13_map() -> np.ndarray:
    tau = np.full((NY, NX), 1.0)
    tau[:, 4:8] = 0.5
    tau[:, 8:] = 2.0
    return tau


def _write_cube(path: Path, line: L.Line, tau_peak: np.ndarray, *, tex=TEX_TRUE,
                transpose=False, noise=0.0, tempscal="TA*", seed=0) -> Path:
    """T_A* = eta_mb (J(T_ex) − J(T_bg)) (1 − e^(−τ)) on channels 20-29
    (70 to 74.5 km/s), zero elsewhere."""
    dj = float(L.j_nu(line.t0, tex) - L.j_nu(line.t0, L.T_CMB_K))
    cube = np.zeros((NZ, NY, NX))
    cube[20:30] = ETA * dj * (1 - np.exp(-tau_peak))[None]
    if noise:
        cube += np.random.default_rng(seed).normal(0, noise, cube.shape)
    h = fits.Header()
    if transpose:
        cube = np.transpose(cube, (0, 2, 1))
        axes = ("GLAT-CAR", "GLON-CAR")
        crval = (0.17, 40.15)
        cdelt = (2.0e-3, -2.0e-3)
    else:
        axes = ("GLON-CAR", "GLAT-CAR")
        crval = (40.15, 0.17)
        cdelt = (-2.0e-3, 2.0e-3)
    for i, (ax, cv, cd) in enumerate(zip(axes, crval, cdelt), start=1):
        h[f"CTYPE{i}"], h[f"CRVAL{i}"], h[f"CDELT{i}"], h[f"CUNIT{i}"] = ax, cv, cd, "deg"
        h[f"CRPIX{i}"] = (cube.shape[3 - i] + 1) / 2.0
    h["CTYPE3"], h["CRVAL3"], h["CDELT3"], h["CRPIX3"], h["CUNIT3"] = "VRAD", V0, DV, 1.0, "km/s"
    h["SPECSYS"] = "LSRK"
    h["RESTFRQ"] = line.freq_hz
    if tempscal:
        h["TEMPSCAL"] = tempscal
    else:
        h["BUNIT"] = "K"
    fits.PrimaryHDU(cube.astype(np.float32), header=h).writeto(path)
    return path


async def _open(client, auth_headers, path, headers=None):
    resp = await client.post("/v1/datasets/open", json={"path": str(path)},
                             headers=headers or auth_headers)
    body = resp.json()
    assert body["valid"], body["error"]
    return body


async def _pair(client, auth_headers, tmp_path, **kw):
    tau = _tau13_map()
    p13 = _write_cube(tmp_path / "13co.fits", L.line("13CO", "3-2"), tau)
    p18 = _write_cube(tmp_path / "c18o.fits", L.line("C18O", "3-2"), tau / X_TRUE, **kw)
    a = await _open(client, auth_headers, p13)
    h = {**auth_headers, "X-Visivo-Session": a["session_id"]}
    b = await _open(client, auth_headers, p18, headers=h)
    return a, b, h, tau


def _cube_body(a, b, **overrides):
    body = {"transition": "3-2", "dataset_id_13co": a["dataset_id"],
            "dataset_id_c18o": b["dataset_id"], "v_min_kms": 69.0, "v_max_kms": 76.0,
            "rms_13co": 1e-3, "rms_c18o": 1e-3, "eta_mb_13co": ETA, "eta_mb_c18o": ETA,
            "x_ratio": X_TRUE, "t_ex_k": TEX_TRUE}
    body.update(overrides)
    return body


def _read(out, quantity):
    m = next(m for m in out["maps"] if m["quantity"] == quantity)
    with fits.open(m["output_path"]) as hdul:
        return hdul[0].data.astype(np.float64), hdul[0].header


@pytest.mark.asyncio
async def test_cubes_of_known_tau_give_tau_and_columns(client, auth_headers, tmp_path):
    a, b, h, tau = await _pair(client, auth_headers, tmp_path)
    out = (await client.post("/v1/products/lte_co", json=_cube_body(a, b), headers=h)).json()
    assert out["valid"], out["error"]
    got_tau, hdr = _read(out, "tau_13co")
    # Exactly what the method gives for the ratio of the integrals ...
    w = {}
    for species, t in (("13CO", tau), ("C18O", tau / X_TRUE)):
        ln = L.line(species, "3-2")
        dj = float(L.j_nu(ln.t0, TEX_TRUE) - L.j_nu(ln.t0, L.T_CMB_K))
        w[species] = dj * (1 - np.exp(-t)) * (10 * DV)
    method_tau, _ = L.tau_from_ratio(w["13CO"] / w["C18O"], X_TRUE)
    np.testing.assert_allclose(got_tau, method_tau, rtol=1e-6)
    # ... and the true tau within the method's approximation: both lines share
    # J(T_ex) - J(T_bg), whereas their frequencies differ by 0.4 %.
    np.testing.assert_allclose(got_tau, tau, rtol=1e-2)
    got18, _ = _read(out, "tau_c18o")
    np.testing.assert_allclose(got18, method_tau / X_TRUE, rtol=1e-6)
    # The true column: N = factor · ΔJ τ · width (flat τ over 10 channels of 0.5 km/s).
    for species, t in (("13CO", tau), ("C18O", tau / X_TRUE)):
        ln = L.line(species, "3-2")
        dj = float(L.j_nu(ln.t0, TEX_TRUE) - L.j_nu(ln.t0, L.T_CMB_K))
        truth = float(L.thin_column_factor(ln, TEX_TRUE)) * dj * t * (10 * DV)
        got, h_n = _read(out, "n_13co" if species == "13CO" else "n_c18o")
        np.testing.assert_allclose(got, truth, rtol=1e-2)
        assert h_n["BUNIT"] == "cm-2"
    assert hdr["XRATIO"] == pytest.approx(X_TRUE)
    assert hdr["ETAMB13"] == pytest.approx(ETA)
    assert hdr["LTETRANS"] == "3-2"
    history = "\n".join(str(v) for v in hdr["HISTORY"])
    assert "Mangum & Shirley 2015" in history and "CDMS" in history
    flag, _ = _read(out, "flag")
    assert np.all(flag == L.FLAG_OK)
    assert out["pixel_flags"]["ok"] == NX * NY


@pytest.mark.asyncio
async def test_tex_from_the_13co_peak_of_known_cubes(client, auth_headers, tmp_path):
    a, b, h, _ = await _pair(client, auth_headers, tmp_path)
    body = _cube_body(a, b)
    body.pop("t_ex_k")
    out = (await client.post("/v1/products/lte_co", json=body, headers=h)).json()
    assert out["valid"], out["error"]
    tex, hdr = _read(out, "tex")
    np.testing.assert_allclose(tex, TEX_TRUE, rtol=5e-3)
    assert hdr["BUNIT"] == "K"


@pytest.mark.asyncio
async def test_tex_from_thick_12co(client, auth_headers, tmp_path):
    a, b, h, tau = await _pair(client, auth_headers, tmp_path)
    p12 = _write_cube(tmp_path / "12co.fits", L.line("12CO", "3-2"), np.full_like(tau, 60.0),
                      tex=25.0)
    c = await _open(client, auth_headers, p12, headers=h)
    body = _cube_body(a, b, dataset_id_12co=c["dataset_id"], eta_mb_12co=ETA)
    body.pop("t_ex_k")
    out = (await client.post("/v1/products/lte_co", json=body, headers=h)).json()
    assert out["valid"], out["error"]
    tex, _ = _read(out, "tex")
    np.testing.assert_allclose(tex, 25.0, rtol=1e-4)
    assert "12CO" in out["assumptions"]["t_ex"]["source"]


@pytest.mark.asyncio
async def test_c18o_on_a_transposed_grid_is_reprojected(client, auth_headers, tmp_path):
    tau = np.full((NY, NX), 1.2)
    p13 = _write_cube(tmp_path / "13co.fits", L.line("13CO", "3-2"), tau)
    p18 = _write_cube(tmp_path / "c18o.fits", L.line("C18O", "3-2"), tau / X_TRUE,
                      transpose=True)
    a = await _open(client, auth_headers, p13)
    h = {**auth_headers, "X-Visivo-Session": a["session_id"]}
    b = await _open(client, auth_headers, p18, headers=h)
    out = (await client.post("/v1/products/lte_co", json=_cube_body(a, b), headers=h)).json()
    assert out["valid"], out["error"]
    got, _ = _read(out, "tau_13co")
    inner = got[1:-1, 1:-1]
    np.testing.assert_allclose(inner[np.isfinite(inner)], 1.2, rtol=1e-2)
    assert any("reprojected" in w for w in out["warnings"])


@pytest.mark.asyncio
async def test_a_region_on_the_maps(client, auth_headers, tmp_path):
    a, b, h, tau = await _pair(client, auth_headers, tmp_path)
    region = {"shape": "box", "center": [1.5, 4.5], "size": [4, 10], "angle_deg": 0}
    body = _cube_body(a, b, regions=[region], distance_kpc=5.0)
    out = (await client.post("/v1/products/lte_co", json=body, headers=h)).json()
    assert out["valid"], out["error"]
    reg = out["regions"][0]
    assert reg["n_pixels"] == 40
    assert reg["tau_13co"] == pytest.approx(1.0, rel=1e-2)
    assert reg["mass_msun"] > 0
    nh2, hdr = _read(out, "n_h2")
    assert hdr["BUNIT"] == "cm-2" and "XH2" in hdr
    assert reg["n_h2_cm2"] == pytest.approx(float(np.mean(nh2[0:10, 0:4])), rel=1e-6)


@pytest.mark.asyncio
async def test_noise_below_threshold_is_flagged(client, auth_headers, tmp_path):
    a, b, h, _ = await _pair(client, auth_headers, tmp_path)
    body = _cube_body(a, b, rms_c18o=5.0)
    out = (await client.post("/v1/products/lte_co", json=body, headers=h)).json()
    assert out["valid"], out["error"]
    flag, _ = _read(out, "flag")
    assert np.all(flag == L.FLAG_LOW_SNR)
    tau, _ = _read(out, "tau_13co")
    assert np.isnan(tau).all()


@pytest.mark.asyncio
async def test_noise_measured_off_the_line(client, auth_headers, tmp_path):
    tau = _tau13_map()
    p13 = _write_cube(tmp_path / "13co.fits", L.line("13CO", "3-2"), tau, noise=0.01, seed=1)
    p18 = _write_cube(tmp_path / "c18o.fits", L.line("C18O", "3-2"), tau / X_TRUE,
                      noise=0.01, seed=2)
    a = await _open(client, auth_headers, p13)
    h = {**auth_headers, "X-Visivo-Session": a["session_id"]}
    b = await _open(client, auth_headers, p18, headers=h)
    body = _cube_body(a, b)
    body.pop("rms_13co")
    body.pop("rms_c18o")
    region = {"shape": "box", "center": [1.5, 4.5], "size": [4, 10], "angle_deg": 0}
    body["regions"] = [region]
    out = (await client.post("/v1/products/lte_co", json=body, headers=h)).json()
    assert out["valid"], out["error"]
    reg = out["regions"][0]
    expected_sigma = 0.01 * DV * math.sqrt(15) / ETA / math.sqrt(40)
    assert reg["sigma_w_c18o_kkms"] == pytest.approx(expected_sigma, rel=0.2)
    assert reg["tau_13co"] == pytest.approx(1.0, abs=0.1)


@pytest.mark.asyncio
async def test_data_already_on_the_main_beam_scale(client, auth_headers, tmp_path):
    tau = _tau13_map()
    p13 = _write_cube(tmp_path / "13co.fits", L.line("13CO", "3-2"), tau, tempscal="Tmb")
    p18 = _write_cube(tmp_path / "c18o.fits", L.line("C18O", "3-2"), tau / X_TRUE,
                      tempscal="Tmb")
    a = await _open(client, auth_headers, p13)
    h = {**auth_headers, "X-Visivo-Session": a["session_id"]}
    b = await _open(client, auth_headers, p18, headers=h)
    body = _cube_body(a, b)
    body.pop("eta_mb_13co")
    body.pop("eta_mb_c18o")
    out = (await client.post("/v1/products/lte_co", json=body, headers=h)).json()
    assert out["valid"], out["error"]
    assert out["assumptions"]["eta_mb"]["13CO"][0] == 1.0
    assert "T_mb" in out["assumptions"]["eta_mb"]["13CO"][1]


def test_eta_from_a_class_header():
    from app.lte_products import eta_mb_for
    h = fits.Header()
    h["BEAMEFF"], h["FORWEFF"] = 0.78, 0.94
    assert eta_mb_for(h, None, "13CO")[0] == pytest.approx(0.78 / 0.94)
    h2 = fits.Header()
    h2["ETAL"] = 0.85
    with pytest.raises(ValueError, match="ETAL"):
        eta_mb_for(h2, None, "13CO")


def _gauss_cube(path: Path, line: L.Line, tau0: np.ndarray, tex: float) -> Path:
    """A Gaussian optical-depth profile, sigma 1 km/s, centred on channel 30."""
    v = V0 + DV * np.arange(NZ)
    prof = np.exp(-0.5 * ((v - v[30]) / 1.0) ** 2)
    dj = float(L.j_nu(line.t0, tex) - L.j_nu(line.t0, L.T_CMB_K))
    cube = ETA * dj * (1 - np.exp(-tau0[None] * prof[:, None, None]))
    h = fits.Header()
    for i, (ax, cv, cd) in enumerate(zip(("GLON-CAR", "GLAT-CAR"), (40.15, 0.17),
                                         (-2.0e-3, 2.0e-3)), start=1):
        h[f"CTYPE{i}"], h[f"CRVAL{i}"], h[f"CDELT{i}"], h[f"CUNIT{i}"] = ax, cv, cd, "deg"
        h[f"CRPIX{i}"] = (cube.shape[3 - i] + 1) / 2.0
    h["CTYPE3"], h["CRVAL3"], h["CDELT3"], h["CRPIX3"], h["CUNIT3"] = "VRAD", V0, DV, 1.0, "km/s"
    h["TEMPSCAL"] = "TA*"
    fits.PrimaryHDU(cube.astype(np.float32), header=h).writeto(path)
    return path


@pytest.mark.asyncio
async def test_tex_of_gaussian_lines_from_the_peaks(client, auth_headers, tmp_path):
    tau = _tau13_map()
    p13 = _gauss_cube(tmp_path / "13co.fits", L.line("13CO", "3-2"), tau, 15.0)
    p18 = _gauss_cube(tmp_path / "c18o.fits", L.line("C18O", "3-2"), tau / X_TRUE, 15.0)
    a = await _open(client, auth_headers, p13)
    h = {**auth_headers, "X-Visivo-Session": a["session_id"]}
    b = await _open(client, auth_headers, p18, headers=h)
    region = {"shape": "box", "center": [1.5, 4.5], "size": [4, 10], "angle_deg": 0}
    body = _cube_body(a, b, v_min_kms=70.0, v_max_kms=80.0, regions=[region])
    body.pop("t_ex_k")
    out = (await client.post("/v1/products/lte_co", json=body, headers=h)).json()
    assert out["valid"], out["error"]
    tex, _ = _read(out, "tex")
    # Within the equal-J approximation of the two lines (well under 1 %).
    np.testing.assert_allclose(tex, 15.0, rtol=1e-2)
    assert out["regions"][0]["t_ex_k"] == pytest.approx(15.0, rel=1e-2)


@pytest.mark.asyncio
async def test_thin_pixels_never_get_an_infinite_tex(client, auth_headers, tmp_path):
    tau = _tau13_map()
    p13 = _write_cube(tmp_path / "13co.fits", L.line("13CO", "3-2"), tau)
    p18 = _write_cube(tmp_path / "c18o.fits", L.line("C18O", "3-2"), tau / 10.0)
    a = await _open(client, auth_headers, p13)
    h = {**auth_headers, "X-Visivo-Session": a["session_id"]}
    b = await _open(client, auth_headers, p18, headers=h)
    region = {"shape": "box", "center": [5.5, 4.5], "size": [2, 10], "angle_deg": 0}
    body = _cube_body(a, b, regions=[region])
    body.pop("t_ex_k")
    resp = await client.post("/v1/products/lte_co", json=body, headers=h)
    assert resp.status_code == 200
    out = resp.json()
    assert out["valid"], out["error"]
    tex, _ = _read(out, "tex")
    flag, _ = _read(out, "flag")
    assert not np.isinf(tex).any()
    assert np.isnan(tex[flag == L.FLAG_THIN]).all()
    assert out["pixel_flags"]["thin"] > 0 and out["pixel_flags"]["no_tex"] > 0
    reg = out["regions"][0]
    assert reg["flag"] == L.FLAG_THIN and reg["t_ex_k"] is None and reg["n_13co_cm2"] is None


@pytest.mark.asyncio
async def test_a_12co_cube_needs_no_line_free_channels(client, auth_headers, tmp_path):
    a, b, h, tau = await _pair(client, auth_headers, tmp_path)
    p12 = _write_cube(tmp_path / "12co.fits", L.line("12CO", "3-2"), np.full_like(tau, 60.0),
                      tex=25.0)
    c = await _open(client, auth_headers, p12, headers=h)
    body = _cube_body(a, b, dataset_id_12co=c["dataset_id"], eta_mb_12co=ETA,
                      v_min_kms=V0, v_max_kms=V0 + DV * (NZ - 5))
    body.pop("t_ex_k")
    out = (await client.post("/v1/products/lte_co", json=body, headers=h)).json()
    assert out["valid"], out["error"]


@pytest.mark.asyncio
async def test_blank_noise_inside_a_region_is_left_out(client, auth_headers, tmp_path):
    tau = _tau13_map()
    p13 = _write_cube(tmp_path / "13co.fits", L.line("13CO", "3-2"), tau, noise=0.01, seed=1)
    with fits.open(p13, mode="update") as hdul:
        hdul[0].data[:20, 0, 0] = np.nan
        hdul[0].data[30:, 0, 0] = np.nan
    p18 = _write_cube(tmp_path / "c18o.fits", L.line("C18O", "3-2"), tau / X_TRUE,
                      noise=0.01, seed=2)
    a = await _open(client, auth_headers, p13)
    h = {**auth_headers, "X-Visivo-Session": a["session_id"]}
    b = await _open(client, auth_headers, p18, headers=h)
    body = _cube_body(a, b, regions=[{"shape": "box", "center": [1.5, 4.5], "size": [4, 10],
                                      "angle_deg": 0}])
    body.pop("rms_13co")
    body.pop("rms_c18o")
    resp = await client.post("/v1/products/lte_co", json=body, headers=h)
    assert resp.status_code == 200
    reg = resp.json()["regions"][0]
    assert reg["n_pixels"] == 39 and reg["sigma_w_13co_kkms"] is not None


@pytest.mark.asyncio
async def test_a_failed_request_leaves_no_placeholder(client, auth_headers, tmp_path,
                                                      _isolated_exports):
    a, b, h, _ = await _pair(client, auth_headers, tmp_path)
    body = _cube_body(a, b)
    body.pop("eta_mb_13co")
    out = (await client.post("/v1/products/lte_co", json=body, headers=h)).json()
    assert out["valid"] is False
    assert not [p for p in Path(_isolated_exports).iterdir() if "_lte_" in p.name]


@pytest.mark.asyncio
async def test_a_bad_region_is_named(client, auth_headers, tmp_path):
    a, b, h, _ = await _pair(client, auth_headers, tmp_path)
    out = (await client.post("/v1/products/lte_co",
                             json=_cube_body(a, b, regions=[{"shape": "circle"}]),
                             headers=h)).json()
    assert out["valid"] is False and "Region 0" in out["error"]


@pytest.mark.asyncio
async def test_an_unused_12co_needs_no_efficiency(client, auth_headers, tmp_path):
    a, b, h, tau = await _pair(client, auth_headers, tmp_path)
    p12 = _write_cube(tmp_path / "12co.fits", L.line("12CO", "3-2"), np.full_like(tau, 60.0))
    c = await _open(client, auth_headers, p12, headers=h)
    out = (await client.post("/v1/products/lte_co",
                             json=_cube_body(a, b, dataset_id_12co=c["dataset_id"]),
                             headers=h)).json()
    assert out["valid"], out["error"]
    assert "12CO" not in out["assumptions"]["eta_mb"]
    assert any("not used" in w for w in out["warnings"])


@pytest.mark.asyncio
async def test_channels_in_the_notes_count_from_one(client, auth_headers, tmp_path):
    a, b, h, _ = await _pair(client, auth_headers, tmp_path)
    out = (await client.post("/v1/products/lte_co", json=_cube_body(a, b), headers=h)).json()
    assert out["valid"], out["error"]
    # 69 to 76 km/s on an axis starting at 60 km/s in 0.5 km/s steps: the
    # 19th to the 33rd of 60 channels, as the cube viewer numbers them.
    assert out["assumptions"]["inputs"]["13CO"][0].startswith(
        "integrated over channels 19-33 of 60 (69 to 76 km/s")


def _c18o_on_its_own_grid(path: Path, ref13: Path, tau13: np.ndarray, grid: str) -> Path:
    """C18O on a sky grid of its own: ``transposed`` (GLAT on axis 1, GLON on
    axis 2, as some survey cutouts are) or ``icrs`` (RA/Dec, rotated against
    Galactic, and larger). τ at each pixel is τ(13CO)/X of the nearest 13CO
    pixel on the sky, zero off the 13CO field."""
    from astropy import units as u  # noqa: PLC0415
    from astropy.coordinates import SkyCoord  # noqa: PLC0415
    from astropy.wcs import WCS  # noqa: PLC0415

    line = L.line("C18O", "3-2")
    h = fits.Header()
    if grid == "transposed":
        ny, nx = NX, NY
        axes = (("GLAT-CAR", 0.17, 2.0e-3), ("GLON-CAR", 40.15, -2.0e-3))
    else:
        centre = SkyCoord(40.15 * u.deg, 0.17 * u.deg, frame="galactic").icrs
        ny = nx = 24
        axes = (("RA---TAN", centre.ra.deg, -2.0e-3), ("DEC--TAN", centre.dec.deg, 2.0e-3))
        h["RADESYS"] = "ICRS"
    for i, ((ax, cv, cd), n) in enumerate(zip(axes, (nx, ny)), start=1):
        h[f"CTYPE{i}"], h[f"CRVAL{i}"], h[f"CDELT{i}"], h[f"CUNIT{i}"] = ax, cv, cd, "deg"
        h[f"CRPIX{i}"] = (n + 1) / 2.0
    ref = WCS(fits.getheader(ref13)).celestial
    yy, xx = np.mgrid[0:ny, 0:nx]
    px, py = ref.world_to_pixel(WCS(h).pixel_to_world(xx, yy))
    ix, iy = np.rint(px).astype(int), np.rint(py).astype(int)
    inside = (ix >= 0) & (ix < NX) & (iy >= 0) & (iy < NY)
    tau = np.zeros((ny, nx))
    tau[inside] = tau13[iy[inside], ix[inside]] / X_TRUE
    if grid == "transposed":
        # The same pixels with the axes swapped, independently of the WCS.
        np.testing.assert_array_equal(tau, tau13.T / X_TRUE)
    dj = float(L.j_nu(line.t0, TEX_TRUE) - L.j_nu(line.t0, L.T_CMB_K))
    cube = np.zeros((NZ, ny, nx))
    cube[20:30] = ETA * dj * (1 - np.exp(-tau))[None]
    h["CTYPE3"], h["CRVAL3"], h["CDELT3"], h["CRPIX3"], h["CUNIT3"] = "VRAD", V0, DV, 1.0, "km/s"
    h["SPECSYS"] = "LSRK"
    h["RESTFRQ"] = line.freq_hz
    h["TEMPSCAL"] = "TA*"
    fits.PrimaryHDU(cube.astype(np.float32), header=h).writeto(path)
    return path


@pytest.mark.asyncio
@pytest.mark.parametrize("grid", ["same", "transposed", "icrs"])
async def test_region_peaks_and_columns_on_another_grid(client, auth_headers, tmp_path, grid):
    """A region's mean spectra are read on each cube's own grid: a C18O cube
    with its sky axes swapped, or in another frame, gives the region the same
    peaks, T_ex and columns as one on the 13CO grid, and none of them is
    missing."""
    tau = _tau13_map()
    p13 = _write_cube(tmp_path / "13co.fits", L.line("13CO", "3-2"), tau)
    if grid == "same":
        p18 = _write_cube(tmp_path / "c18o.fits", L.line("C18O", "3-2"), tau / X_TRUE)
    else:
        p18 = _c18o_on_its_own_grid(tmp_path / "c18o.fits", p13, tau, grid)
    a = await _open(client, auth_headers, p13)
    h = {**auth_headers, "X-Visivo-Session": a["session_id"]}
    b = await _open(client, auth_headers, p18, headers=h)
    # Inside the τ = 2 band (x 8-11), away from its edges.
    region = {"shape": "box", "center": [9.5, 4.5], "size": [2, 6], "angle_deg": 0}
    body = _cube_body(a, b, regions=[region], distance_kpc=5.0)
    body.pop("t_ex_k")
    out = (await client.post("/v1/products/lte_co", json=body, headers=h)).json()
    assert out["valid"], out["error"]
    reg = out["regions"][0]
    assert reg["n_pixels"] > 0
    ln13, ln18 = L.line("13CO", "3-2"), L.line("C18O", "3-2")
    dj = {s: float(L.j_nu(ln.t0, TEX_TRUE) - L.j_nu(ln.t0, L.T_CMB_K))
          for s, ln in (("13CO", ln13), ("C18O", ln18))}
    # Main-beam peaks: the region is uniform, so its mean spectra are exact.
    assert reg["peak_13co_k"] == pytest.approx(dj["13CO"] * (1 - math.exp(-2.0)), rel=1e-5)
    assert reg["peak_c18o_k"] == pytest.approx(
        dj["C18O"] * (1 - math.exp(-2.0 / X_TRUE)), rel=1e-5)
    assert reg["tau_13co"] == pytest.approx(2.0, rel=1e-2)
    assert reg["t_ex_k"] == pytest.approx(TEX_TRUE, rel=1e-2)
    for key, s, ln, t in (("n_13co_cm2", "13CO", ln13, 2.0),
                          ("n_c18o_cm2", "C18O", ln18, 2.0 / X_TRUE)):
        truth = float(L.thin_column_factor(ln, TEX_TRUE)) * dj[s] * t * (10 * DV)
        assert reg[key] == pytest.approx(truth, rel=2e-2)
    assert reg["n_h2_cm2"] is not None and reg["mass_msun"] > 0
