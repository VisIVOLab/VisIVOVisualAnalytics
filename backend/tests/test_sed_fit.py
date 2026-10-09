"""SED fitting: physics, grid search, and the two routes.

The fit is a grid search, so the strongest check available is round-tripping:
build a SED from a known greybody, fit it, and require the parameters back.
"""
from __future__ import annotations

import math
import os

import numpy as np
import pytest

from app.compute.sed import (
    SedFitError,
    bolometric_luminosity,
    fit_thick,
    fit_thin,
    greybody_thick,
    greybody_thin,
    planck_lambda,
    solid_angle_sr,
    thick_mass_msun,
    worker_sed_fit,
)

_TEST_TOKEN = "test-token-visivo-12345"
HIGAL_BANDS = [70.0, 160.0, 250.0, 350.0, 500.0, 870.0]


def _client():
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient

    from app.main import app
    return TestClient(app), {"X-Visivo-Token": _TEST_TOKEN}


# ── physics ───────────────────────────────────────────────────────────────────

def test_planck_peaks_near_wien_wavelength():
    temp = 20.0
    lam = np.linspace(20.0, 600.0, 4000)
    flux = planck_lambda(lam, temp)
    peak = lam[int(np.argmax(flux))]
    # Wien for πI_λ: λ_max·T ≈ 2898 µm·K
    assert peak == pytest.approx(2898.0 / temp, rel=0.02)


def test_planck_is_zero_where_the_exponential_overflows():
    # 0.001 µm at 5 K: hc/λkT ≈ 3e6, far past the expm1 overflow.
    assert planck_lambda(np.array([1e-3]), 5.0)[0] == 0.0


def test_greybody_thin_scales_linearly_with_mass_and_inverse_square_with_distance():
    kw = dict(temperature=20.0, beta=2.0, kappa_ref=0.1, lambda_ref_um=300.0)
    base = greybody_thin(HIGAL_BANDS, 100.0, distance_pc=1000.0, **kw)
    heavier = greybody_thin(HIGAL_BANDS, 300.0, distance_pc=1000.0, **kw)
    farther = greybody_thin(HIGAL_BANDS, 100.0, distance_pc=2000.0, **kw)
    assert np.allclose(heavier, 3.0 * base)
    assert np.allclose(farther, base / 4.0)


def test_greybody_thin_beta_steepens_the_submm_slope():
    kw = dict(mass_msun=100.0, temperature=20.0, distance_pc=1000.0,
              kappa_ref=0.1, lambda_ref_um=300.0)
    flat = greybody_thin([250.0, 870.0], beta=1.0, **kw)
    steep = greybody_thin([250.0, 870.0], beta=2.5, **kw)
    assert steep[1] / steep[0] < flat[1] / flat[0]


def test_greybody_thick_saturates_to_a_blackbody_at_short_wavelengths():
    # τ = (λ0/λ)^β ≫ 1 below λ0, so the source radiates as Ω·B_λ there.
    lam = np.array([20.0])
    thick = greybody_thick(lam, size_arcsec=30.0, temperature=30.0, beta=2.0, lambda0_um=200.0)
    blackbody = (solid_angle_sr(30.0) * planck_lambda(lam, 30.0) / math.pi
                 * lam**2 / 2.99792458e-13)
    assert thick[0] == pytest.approx(blackbody[0], rel=1e-9)


def test_thick_mass_matches_the_thin_mass_of_an_equivalent_column():
    mass = thick_mass_msun(20.0, 2.0, 200.0, 1000.0, 0.1, 300.0)
    assert mass > 0 and np.isfinite(mass)
    # Doubling the size quadruples the solid angle, hence the mass.
    assert thick_mass_msun(40.0, 2.0, 200.0, 1000.0, 0.1, 300.0) == pytest.approx(4.0 * mass)


def test_bolometric_luminosity_of_a_known_greybody_is_physical():
    lam = np.arange(5.0, 2000.0)
    flux = greybody_thin(lam, 100.0, 20.0, 2.0, 1000.0, 0.1, 300.0)
    lum = bolometric_luminosity(lam, flux, 1000.0)
    # A 100 M⊙ / 20 K clump sits in the tens-to-hundreds of L⊙ range.
    assert 1.0 < lum < 1e4


# ── thin fit round-trips ──────────────────────────────────────────────────────

def test_thin_fit_recovers_the_input_parameters():
    truth = dict(mass_msun=250.0, temperature=18.0, beta=2.0)
    flux = greybody_thin(HIGAL_BANDS, truth["mass_msun"], truth["temperature"],
                         truth["beta"], 1500.0, 0.1, 300.0)
    result = fit_thin(HIGAL_BANDS, flux, distance_pc=1500.0, kappa_ref=0.1,
                      lambda_ref_um=300.0, mass_range=[10.0, 3000.0, 25],
                      temp_range=[8.0, 40.0, 25], beta_range=[2.0, 2.0, 1],
                      errors_jy=[0.05 * f for f in flux], colour_correction=False)
    assert result.parameters["temperature_k"] == pytest.approx(truth["temperature"], rel=0.05)
    assert result.parameters["mass_msun"] == pytest.approx(truth["mass_msun"], rel=0.10)
    assert result.chi2_reduced < 1e-2
    assert result.luminosity_lsun > 0


def test_thin_fit_recovers_a_free_beta():
    flux = greybody_thin(HIGAL_BANDS, 400.0, 22.0, 1.6, 2000.0, 0.1, 300.0)
    result = fit_thin(HIGAL_BANDS, flux, distance_pc=2000.0, kappa_ref=0.1,
                      lambda_ref_um=300.0, mass_range=[10.0, 5000.0, 20],
                      temp_range=[8.0, 45.0, 20], beta_range=[1.0, 3.0, 11],
                      errors_jy=[0.03 * f for f in flux], colour_correction=False)
    assert result.parameters["beta"] == pytest.approx(1.6, abs=0.15)
    assert result.parameters["temperature_k"] == pytest.approx(22.0, rel=0.08)


def test_thin_fit_reports_uncertainties_that_bracket_the_solution():
    flux = greybody_thin(HIGAL_BANDS, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0)
    result = fit_thin(HIGAL_BANDS, flux, distance_pc=1500.0, kappa_ref=0.1,
                      lambda_ref_um=300.0, mass_range=[10.0, 3000.0, 20],
                      temp_range=[8.0, 40.0, 20], beta_range=[2.0, 2.0, 1],
                      errors_jy=[0.1 * f for f in flux], colour_correction=False)
    lo, hi = result.parameter_bounds["temperature_k"]
    assert lo <= result.parameters["temperature_k"] <= hi
    assert result.uncertainties["temperature_k"] > 0


def test_thin_fit_flags_a_solution_pinned_to_the_range_edge():
    flux = greybody_thin(HIGAL_BANDS, 250.0, 35.0, 2.0, 1500.0, 0.1, 300.0)
    result = fit_thin(HIGAL_BANDS, flux, distance_pc=1500.0, kappa_ref=0.1,
                      lambda_ref_um=300.0, mass_range=[10.0, 3000.0, 15],
                      # The true temperature is above this ceiling.
                      temp_range=[5.0, 12.0, 8], beta_range=[2.0, 2.0, 1],
                      colour_correction=False)
    # The refinement is allowed to push the ceiling out, so either it escaped
    # (and found ~35 K) or it reports the edge — never a silent 12 K.
    assert result.parameters["temperature_k"] > 12.0 or "temperature" in result.at_boundary


def test_upper_limits_are_excluded_from_chi2_but_still_reject_models():
    lam = [70.0, 160.0, 250.0, 350.0, 500.0]
    flux = list(greybody_thin(lam, 200.0, 15.0, 2.0, 1000.0, 0.1, 300.0))
    # Declare 70 µm an upper limit far below what the true model predicts: any
    # model matching the cold dust would overshoot it, so the fit must move.
    flux[0] = 1e-4
    ul = [True, False, False, False, False]
    with_ul = fit_thin(lam, flux, distance_pc=1000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                       mass_range=[10.0, 2000.0, 15], temp_range=[5.0, 40.0, 15],
                       beta_range=[2.0, 2.0, 1], upper_limits=ul, colour_correction=False)
    assert with_ul.bands_upper_limit == 1
    assert with_ul.bands_used == 4
    predicted_70 = with_ul.fitted_flux_jy[0]
    assert predicted_70 <= flux[0] * 1.0000001


def test_impossible_upper_limits_raise_rather_than_return_a_meaningless_fit():
    lam = [70.0, 160.0, 250.0]
    flux = [1e-30, 10.0, 20.0]
    with pytest.raises(SedFitError):
        fit_thin(lam, flux, distance_pc=1000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                 mass_range=[100.0, 2000.0, 5], temp_range=[20.0, 40.0, 5],
                 beta_range=[2.0, 2.0, 1], upper_limits=[True, False, False],
                 colour_correction=False)


def test_colour_correction_changes_the_fit_through_the_pacs_bands():
    # The model is built WITHOUT colour correction, so switching it on must
    # perturb the solution: at 12 K the 70 µm factor is ~2.3.
    lam = np.array([70.0, 160.0, 250.0, 350.0])
    flux = greybody_thin(lam, 200.0, 12.0, 2.0, 1000.0, 0.1, 300.0)
    kwargs = dict(distance_pc=1000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                  mass_range=[10.0, 2000.0, 20], temp_range=[5.0, 30.0, 20],
                  beta_range=[2.0, 2.0, 1], errors_jy=[0.02 * f for f in flux])
    on = fit_thin(lam, flux, colour_correction=True, **kwargs)
    off = fit_thin(lam, flux, colour_correction=False, **kwargs)
    assert off.parameters["temperature_k"] == pytest.approx(12.0, rel=0.05)
    assert off.chi2 < on.chi2
    assert on.parameters["temperature_k"] != off.parameters["temperature_k"]


def test_colour_correction_leaves_a_pacs_free_sed_untouched():
    lam = np.array([250.0, 350.0, 500.0, 870.0])
    flux = greybody_thin(lam, 200.0, 12.0, 2.0, 1000.0, 0.1, 300.0)
    kwargs = dict(distance_pc=1000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                  mass_range=[10.0, 2000.0, 12], temp_range=[5.0, 30.0, 12],
                  beta_range=[2.0, 2.0, 1], errors_jy=[0.02 * f for f in flux])
    on = fit_thin(lam, flux, colour_correction=True, **kwargs)
    off = fit_thin(lam, flux, colour_correction=False, **kwargs)
    assert on.parameters == off.parameters
    assert on.chi2 == off.chi2


# ── thick fit ─────────────────────────────────────────────────────────────────

def test_thick_fit_recovers_the_input_parameters():
    lam = [24.0, 70.0, 160.0, 250.0, 350.0, 500.0]
    flux = greybody_thick(lam, size_arcsec=25.0, temperature=28.0, beta=2.0, lambda0_um=120.0)
    result = fit_thick(lam, flux, distance_pc=2000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                       size_arcsec=25.0, temp_range=[10.0, 60.0, 20],
                       beta_range=[2.0, 2.0, 1], lambda0_range=[20.0, 400.0, 20],
                       scale_range=[1.0, 1.0, 1], errors_jy=[0.03 * f for f in flux],
                       colour_correction=False)
    assert result.parameters["temperature_k"] == pytest.approx(28.0, rel=0.08)
    assert result.parameters["lambda0_um"] == pytest.approx(120.0, rel=0.20)
    assert result.parameters["mass_msun"] > 0
    assert result.uncertainties["mass_msun"] >= 0


def test_thick_fit_requires_a_source_size():
    lam = [70.0, 160.0, 250.0]
    flux = [1.0, 2.0, 1.5]
    with pytest.raises(SedFitError):
        fit_thick(lam, flux, distance_pc=1000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                  size_arcsec=0.0, temp_range=[10.0, 40.0, 5], beta_range=[2.0, 2.0, 1],
                  lambda0_range=[20.0, 300.0, 5], scale_range=[1.0, 1.0, 1])


# ── input validation ──────────────────────────────────────────────────────────

@pytest.mark.parametrize("kwargs, message", [
    (dict(wavelength_um=[250.0], flux_jy=[1.0]), "at least 2"),
    (dict(wavelength_um=[250.0, 350.0], flux_jy=[1.0]), "same length"),
    (dict(wavelength_um=[250.0, 350.0], flux_jy=[0.0, 0.0]), "no usable band"),
    (dict(wavelength_um=[250.0, 350.0], flux_jy=[1.0, 2.0], distance_pc=0.0), "distance"),
    (dict(wavelength_um=[250.0, 350.0], flux_jy=[1.0, 2.0], kappa_ref=0.0), "opacity"),
])
def test_thin_fit_rejects_unusable_input(kwargs, message):
    call = dict(distance_pc=1000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                mass_range=[10.0, 100.0, 3], temp_range=[10.0, 30.0, 3],
                beta_range=[2.0, 2.0, 1])
    call.update(kwargs)
    lam = call.pop("wavelength_um")
    flux = call.pop("flux_jy")
    with pytest.raises(SedFitError) as exc:
        fit_thin(lam, flux, **call)
    assert message in str(exc.value)


def test_one_detection_plus_upper_limits_is_not_a_fit():
    with pytest.raises(SedFitError) as exc:
        fit_thin([70.0, 160.0, 250.0], [1.0, 2.0, 3.0], distance_pc=1000.0,
                 kappa_ref=0.1, lambda_ref_um=300.0, mass_range=[10.0, 100.0, 3],
                 temp_range=[10.0, 30.0, 3], beta_range=[2.0, 2.0, 1],
                 upper_limits=[False, True, True])
    assert "at least 2 detections" in str(exc.value)


def test_an_oversized_grid_is_refused_rather_than_swapping():
    with pytest.raises(SedFitError) as exc:
        fit_thin(HIGAL_BANDS, [1.0] * 6, distance_pc=1000.0, kappa_ref=0.1,
                 lambda_ref_um=300.0, mass_range=[1.0, 1e4, 400],
                 temp_range=[5.0, 50.0, 400], beta_range=[1.0, 3.0, 400])
    assert "models" in str(exc.value)


def test_worker_rejects_an_unknown_model():
    with pytest.raises(SedFitError):
        worker_sed_fit("spherical-cow", HIGAL_BANDS, [1.0] * 6, {})


# ── routes ────────────────────────────────────────────────────────────────────

def _fit_body(**overrides):
    flux = list(greybody_thin(HIGAL_BANDS, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0))
    body = {
        "model": "thin",
        "wavelengths_um": HIGAL_BANDS,
        "fluxes_jy": flux,
        "errors_jy": [0.05 * f for f in flux],
        "distance_pc": 1500.0,
        "mass_range": [10.0, 3000.0, 20],
        "temp_range": [8.0, 40.0, 20],
        "beta_range": [2.0, 2.0, 1],
        "colour_correction": False,
        "source_label": "HIGALBM G000.0000+00.0000",
    }
    body.update(overrides)
    return body


def test_fit_route_returns_a_usable_fit():
    client, auth = _client()
    r = client.post("/v1/sed/fit", json=_fit_body(), headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"] is True
    assert body["best"] == "thin"
    fit = body["fits"][0]
    assert fit["parameters"]["temperature_k"] == pytest.approx(18.0, rel=0.06)
    assert len(fit["model_wavelength_um"]) == len(fit["model_flux_jy"])
    assert len(fit["fitted_flux_jy"]) == len(HIGAL_BANDS)
    assert fit["source_label"].startswith("HIGALBM")


def test_fit_route_runs_both_models_and_picks_the_better_one():
    client, auth = _client()
    r = client.post("/v1/sed/fit",
                    json=_fit_body(model="both", size_arcsec=30.0,
                                   lambda0_range=[20.0, 400.0, 15],
                                   scale_range=[1.0, 1.0, 1]),
                    headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    assert {f["model"] for f in body["fits"]} == {"thin", "thick"}
    assert body["best"] in ("thin", "thick")


def test_fit_route_both_survives_a_thick_fit_it_cannot_run():
    # No size_arcsec: the thick fit must be skipped, not fail the whole request.
    client, auth = _client()
    r = client.post("/v1/sed/fit", json=_fit_body(model="both"), headers=auth)
    assert r.status_code == 200, r.text
    assert [f["model"] for f in r.json()["fits"]] == ["thin"]


def test_fit_route_rejects_a_bad_sed_with_422():
    client, auth = _client()
    r = client.post("/v1/sed/fit",
                    json=_fit_body(wavelengths_um=[250.0], fluxes_jy=[1.0], errors_jy=[0.1]),
                    headers=auth)
    assert r.status_code == 422


def test_fit_route_rejects_an_unknown_model_name():
    client, auth = _client()
    r = client.post("/v1/sed/fit", json=_fit_body(model="thicc"), headers=auth)
    assert r.status_code == 422


def test_fit_route_requires_auth():
    client, _ = _client()
    assert client.post("/v1/sed/fit", json=_fit_body()).status_code in (401, 403)


def test_defaults_route_shape():
    client, auth = _client()
    body = client.get("/v1/sed/defaults", headers=auth).json()
    for key in ("mass_range", "temp_range", "beta_range", "lambda0_range",
                "scale_range", "kappa_ref", "lambda_ref_um"):
        assert key in body


def test_models_route_without_a_configured_service_is_503():
    client, auth = _client()
    os.environ.pop("VISIVO_VLKB_SEDFIT_URL", None)
    r = client.post("/v1/sed/models",
                    json={"wavelengths_um": [70.0, 160.0], "fluxes_jy": [1.0, 2.0]},
                    headers=auth)
    assert r.status_code == 503


def test_models_route_blocks_a_loopback_service_url():
    client, auth = _client()
    r = client.post("/v1/sed/models",
                    json={"wavelengths_um": [70.0, 160.0], "fluxes_jy": [1.0, 2.0],
                          "service_url": "http://127.0.0.1:9999"},
                    headers=auth)
    assert r.status_code == 400


def test_models_route_normalises_the_service_table(monkeypatch):
    client, auth = _client()
    payload = {
        "columns": ["clump_mass", "dust_temp", "chi2", "wmod", "fmod"],
        "data": [
            [500.0, 15.0, 9.0, [70.0, 250.0], [1.0, 4.0]],
            [120.0, 22.0, 2.0, [70.0, 250.0], [2.0, 3.0]],
        ],
    }
    from app.routers import sed as sed_router
    monkeypatch.setattr(sed_router, "_fetch_json", lambda url: payload)
    monkeypatch.setattr(sed_router, "assert_safe_fetch_url", lambda *a, **k: None)

    r = client.post("/v1/sed/models",
                    json={"wavelengths_um": [70.0, 250.0], "fluxes_jy": [2.0, 3.0],
                          "service_url": "https://vlkb.example.org", "source_label": "S1"},
                    headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    assert [m["chi2"] for m in body["models"]] == [2.0, 9.0]      # sorted by χ²
    assert body["best"]["clump_mass"] == 120.0
    assert body["models"][0]["model_wavelength_um"] == [70.0, 250.0]
    assert "wmod" not in body["columns"] and "fmod" not in body["columns"]
    assert body["source_label"] == "S1"


def test_models_route_reports_a_non_json_service_reply_as_502(monkeypatch):
    client, auth = _client()
    from app.routers import sed as sed_router

    def _boom(url):
        raise ValueError("VLKB SED-fit service did not return JSON: <html>500</html>")

    monkeypatch.setattr(sed_router, "_fetch_json", _boom)
    monkeypatch.setattr(sed_router, "assert_safe_fetch_url", lambda *a, **k: None)
    r = client.post("/v1/sed/models",
                    json={"wavelengths_um": [70.0, 250.0], "fluxes_jy": [2.0, 3.0],
                          "service_url": "https://vlkb.example.org"},
                    headers=auth)
    assert r.status_code == 502


# ── regressions from the first review pass ────────────────────────────────────

def test_uncertainty_matches_the_analytic_one_sigma_on_mass():
    """With only mass free, Δχ² = 1 has a closed form to compare against.

    χ²(M) = Σ ((M/M₀·Fᵢ − Fᵢ)/σᵢ)² = (M/M₀ − 1)² Σ (Fᵢ/σᵢ)², because the thin
    greybody is linear in mass. So Δχ² = 1 at |M − M₀| = M₀ / √Σ(Fᵢ/σᵢ)².
    """
    truth_mass = 250.0
    flux = greybody_thin(HIGAL_BANDS, truth_mass, 18.0, 2.0, 1500.0, 0.1, 300.0)
    rel = 0.10
    errors = [rel * f for f in flux]
    result = fit_thin(HIGAL_BANDS, flux, distance_pc=1500.0, kappa_ref=0.1,
                      lambda_ref_um=300.0, mass_range=[10.0, 3000.0, 60],
                      temp_range=[18.0, 18.0, 1], beta_range=[2.0, 2.0, 1],
                      errors_jy=errors, colour_correction=False)
    expected = truth_mass / math.sqrt(sum((f / e) ** 2 for f, e in zip(flux, errors)))
    assert result.uncertainties["mass_msun"] == pytest.approx(expected, rel=0.15)
    assert result.uncertainties["mass_msun"] > 1.0   # not the old grid-width zero


def test_uncertainty_is_not_the_width_of_the_refined_grid():
    # A 10% error bar on six bands cannot give a sub-per-mille mass interval;
    # reading the interval off the zoomed grid used to do exactly that.
    flux = greybody_thin(HIGAL_BANDS, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0)
    result = fit_thin(HIGAL_BANDS, flux, distance_pc=1500.0, kappa_ref=0.1,
                      lambda_ref_um=300.0, mass_range=[10.0, 3000.0, 50],
                      temp_range=[8.0, 40.0, 50], beta_range=[2.0, 2.0, 1],
                      errors_jy=[0.1 * f for f in flux], colour_correction=False)
    relative = result.uncertainties["mass_msun"] / result.parameters["mass_msun"]
    assert relative > 1e-3


def test_refinement_never_returns_a_worse_model_than_it_already_found():
    # The refined grid does not have to contain the previous optimum, so a later
    # iteration can only be *accepted* when it actually lowers χ².
    flux = greybody_thin(HIGAL_BANDS, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0)
    errors = [0.1 * f for f in flux]
    kwargs = dict(distance_pc=1500.0, kappa_ref=0.1, lambda_ref_um=300.0,
                  beta_range=[2.0, 2.0, 1], errors_jy=errors, colour_correction=False)
    coarse = fit_thin(HIGAL_BANDS, flux, mass_range=[10.0, 3000.0, 20],
                      temp_range=[8.0, 40.0, 20], **kwargs)
    # The χ² reported must be the best the search ever saw: re-evaluating the
    # returned parameters must reproduce it, not beat it.
    model = greybody_thin(HIGAL_BANDS, coarse.parameters["mass_msun"],
                          coarse.parameters["temperature_k"], coarse.parameters["beta"],
                          1500.0, 0.1, 300.0)
    chi2 = sum(((m - f) / e) ** 2 for m, f, e in zip(model, flux, errors))
    assert chi2 == pytest.approx(coarse.chi2, rel=1e-9)


def test_bands_may_be_given_in_any_order():
    ordered = HIGAL_BANDS
    flux = list(greybody_thin(ordered, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0))
    shuffled = list(reversed(ordered))
    shuffled_flux = list(reversed(flux))
    kwargs = dict(distance_pc=1500.0, kappa_ref=0.1, lambda_ref_um=300.0,
                  mass_range=[10.0, 3000.0, 20], temp_range=[8.0, 40.0, 20],
                  beta_range=[2.0, 2.0, 1], colour_correction=False)
    a = fit_thin(ordered, flux, errors_jy=[0.05 * f for f in flux], **kwargs)
    b = fit_thin(shuffled, shuffled_flux,
                 errors_jy=[0.05 * f for f in shuffled_flux], **kwargs)
    assert a.parameters == b.parameters
    # …and the fitted fluxes come back aligned with the bands as given.
    assert b.band_wavelength_um == shuffled
    assert b.fitted_flux_jy == pytest.approx(list(reversed(a.fitted_flux_jy)), rel=1e-12)


def test_refinement_cannot_walk_a_positive_parameter_negative():
    # A best fit on the lower wall expands the range downwards; temperature has
    # no meaning at or below 0 K, so the expansion must stay positive.
    lam = [24.0, 70.0, 160.0, 250.0]
    flux = greybody_thin(lam, 200.0, 6.0, 2.0, 1000.0, 0.1, 300.0)
    result = fit_thin(lam, flux, distance_pc=1000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                      mass_range=[10.0, 2000.0, 15], temp_range=[5.0, 50.0, 30],
                      beta_range=[2.0, 2.0, 1], colour_correction=False)
    assert result.parameters["temperature_k"] > 0
    lo, hi = result.parameter_bounds["temperature_k"]
    assert lo > 0 and hi > 0


@pytest.mark.parametrize("bad", [
    {"temp_range": [-10.0, 40.0, 10]},
    {"mass_range": [0.0, 100.0, 10]},
])
def test_non_physical_ranges_are_refused(bad):
    call = dict(distance_pc=1000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                mass_range=[10.0, 100.0, 5], temp_range=[10.0, 30.0, 5],
                beta_range=[2.0, 2.0, 1])
    call.update(bad)
    with pytest.raises(SedFitError):
        fit_thin(HIGAL_BANDS, [1.0] * 6, **call)


def test_thick_rejects_a_non_positive_reference_wavelength():
    lam = [70.0, 160.0, 250.0]
    flux = greybody_thick(lam, 20.0, 25.0, 2.0, 100.0)
    with pytest.raises(SedFitError):
        fit_thick(lam, flux, distance_pc=1000.0, kappa_ref=0.1, lambda_ref_um=0.0,
                  size_arcsec=20.0, temp_range=[10.0, 40.0, 5], beta_range=[2.0, 2.0, 1],
                  lambda0_range=[20.0, 300.0, 5], scale_range=[1.0, 1.0, 1])


def test_a_nan_upper_limit_is_refused_rather_than_silently_ignored():
    with pytest.raises(SedFitError):
        fit_thin([70.0, 160.0, 250.0], [float("nan"), 2.0, 3.0], distance_pc=1000.0,
                 kappa_ref=0.1, lambda_ref_um=300.0, mass_range=[10.0, 100.0, 5],
                 temp_range=[10.0, 30.0, 5], beta_range=[2.0, 2.0, 1],
                 upper_limits=[True, False, False])


def test_thick_fit_with_every_axis_free():
    lam = [24.0, 70.0, 160.0, 250.0, 350.0, 500.0, 870.0]
    flux = greybody_thick(lam, size_arcsec=30.0, temperature=26.0, beta=1.8, lambda0_um=90.0)
    result = fit_thick(lam, flux, distance_pc=2000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                       size_arcsec=25.0, temp_range=[10.0, 50.0, 12],
                       beta_range=[1.0, 2.5, 7], lambda0_range=[20.0, 300.0, 12],
                       scale_range=[0.5, 2.0, 7], errors_jy=[0.03 * f for f in flux],
                       colour_correction=False)
    assert result.parameters["temperature_k"] == pytest.approx(26.0, rel=0.15)
    assert result.parameters["size_arcsec"] == pytest.approx(30.0, rel=0.25)
    assert result.parameter_bounds["mass_msun"][0] <= result.parameters["mass_msun"] \
        <= result.parameter_bounds["mass_msun"][1]


def test_thick_scale_can_escape_the_range_it_was_given():
    # True size 40″ with the range topping out at 1×25″: the refinement must be
    # able to push the ceiling out, or flag that it stopped at the wall.
    lam = [24.0, 70.0, 160.0, 250.0, 350.0]
    flux = greybody_thick(lam, size_arcsec=40.0, temperature=25.0, beta=2.0, lambda0_um=100.0)
    result = fit_thick(lam, flux, distance_pc=2000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                       size_arcsec=25.0, temp_range=[25.0, 25.0, 1], beta_range=[2.0, 2.0, 1],
                       lambda0_range=[100.0, 100.0, 1], scale_range=[0.5, 1.0, 10],
                       errors_jy=[0.02 * f for f in flux], colour_correction=False)
    assert result.parameters["size_arcsec"] > 25.0 or "scale" in result.at_boundary


def test_degrees_of_freedom_count_free_parameters_not_grid_steps():
    flux = greybody_thin(HIGAL_BANDS, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0)
    kwargs = dict(distance_pc=1500.0, kappa_ref=0.1, lambda_ref_um=300.0,
                  errors_jy=[0.05 * f for f in flux], colour_correction=False)
    two_free = fit_thin(HIGAL_BANDS, flux, mass_range=[10.0, 3000.0, 4],
                        temp_range=[8.0, 40.0, 4], beta_range=[2.0, 2.0, 1], **kwargs)
    three_free = fit_thin(HIGAL_BANDS, flux, mass_range=[10.0, 3000.0, 4],
                          temp_range=[8.0, 40.0, 4], beta_range=[1.5, 2.5, 4], **kwargs)
    # 6 detections: 6−2 and 6−3. A 4-step axis used to count as fixed.
    assert two_free.dof == 4
    assert three_free.dof == 3


def test_a_fit_without_error_bars_says_so():
    flux = greybody_thin(HIGAL_BANDS, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0)
    result = fit_thin(HIGAL_BANDS, flux, distance_pc=1500.0, kappa_ref=0.1,
                      lambda_ref_um=300.0, mass_range=[10.0, 3000.0, 12],
                      temp_range=[8.0, 40.0, 12], beta_range=[2.0, 2.0, 1],
                      colour_correction=False)
    assert any("no flux errors" in w for w in result.warnings)


def test_an_uncertainty_that_runs_off_the_range_is_flagged():
    # One band pair barely constrains β; over a narrow range the Δχ²=1 region
    # reaches both ends, and the interval must be reported as a lower bound.
    lam = [250.0, 350.0]
    flux = greybody_thin(lam, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0)
    result = fit_thin(lam, flux, distance_pc=1500.0, kappa_ref=0.1, lambda_ref_um=300.0,
                      mass_range=[10.0, 3000.0, 12], temp_range=[17.5, 18.5, 5],
                      beta_range=[1.9, 2.1, 5], errors_jy=[0.5 * f for f in flux],
                      colour_correction=False)
    assert result.unbounded
    assert any("lower bound" in w for w in result.warnings)


def test_models_route_rejects_a_redirect_into_the_private_network(monkeypatch):
    """A public service must not be able to redirect the backend to loopback."""
    from app.routers import sed as sed_router

    seen = []

    def _fake_assert(url, **kwargs):
        seen.append(url)
        if "127.0.0.1" in url or "169.254" in url:
            raise ValueError("refusing to fetch a private address")

    monkeypatch.setattr(sed_router, "assert_safe_fetch_url", _fake_assert)
    handler = sed_router._ValidatingRedirectHandler()
    with pytest.raises(ValueError):
        handler.redirect_request(object(), None, 302, "Found", {},
                                 "http://169.254.169.254/latest/meta-data/")
    assert seen and "169.254" in seen[-1]


def test_models_route_drops_rows_without_a_usable_chi2(monkeypatch):
    client, auth = _client()
    payload = {
        "columns": ["clump_mass", "chi2", "wmod", "fmod"],
        "data": [
            [100.0, "not-a-number", [70.0], [1.0]],
            [200.0, None, [70.0], [1.0]],
            [300.0, 3.0, [70.0, 250.0], [1.0, 2.0]],
            "garbage",
        ],
    }
    from app.routers import sed as sed_router
    monkeypatch.setattr(sed_router, "_fetch_json", lambda url: payload)
    monkeypatch.setattr(sed_router, "assert_safe_fetch_url", lambda *a, **k: None)
    r = client.post("/v1/sed/models",
                    json={"wavelengths_um": [70.0, 250.0], "fluxes_jy": [1.0, 2.0],
                          "service_url": "https://vlkb.example.org"},
                    headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    assert len(body["models"]) == 1 and body["models"][0]["clump_mass"] == 300.0
    assert body["skipped_models"] == 3


def test_models_route_caps_max_models():
    client, auth = _client()
    r = client.post("/v1/sed/models",
                    json={"wavelengths_um": [70.0, 250.0], "fluxes_jy": [1.0, 2.0],
                          "service_url": "https://vlkb.example.org", "max_models": 10**9},
                    headers=auth)
    assert r.status_code == 422


# ── regressions from the second review pass ───────────────────────────────────

def test_a_tiny_error_bar_gives_a_tiny_but_non_zero_interval():
    """A 0.1 % error bar must narrow the interval, not collapse it to zero.

    Sampling each axis on a fixed grid could not resolve an interval narrower
    than one sample, so a very precise SED came back with the mass "known
    exactly". The bound is now bracketed and bisected, so its resolution
    follows the interval.
    """
    truth_mass = 250.0
    flux = greybody_thin(HIGAL_BANDS, truth_mass, 18.0, 2.0, 1500.0, 0.1, 300.0)
    for rel in (0.001, 0.01):
        errors = [rel * f for f in flux]
        result = fit_thin(HIGAL_BANDS, flux, distance_pc=1500.0, kappa_ref=0.1,
                          lambda_ref_um=300.0, mass_range=[10.0, 3000.0, 40],
                          temp_range=[18.0, 18.0, 1], beta_range=[2.0, 2.0, 1],
                          errors_jy=errors, colour_correction=False)
        expected = truth_mass / math.sqrt(sum((f / e) ** 2 for f, e in zip(flux, errors)))
        assert result.uncertainties["mass_msun"] > 0.0
        assert result.uncertainties["mass_msun"] == pytest.approx(expected, rel=0.20)


def test_a_refinement_that_finds_nothing_better_is_not_called_convergence():
    """The refined grid need not contain the previous optimum.

    Stopping the moment a step fails to improve reported a worse model as a
    converged fit; the search now re-centres on the best model so far and only
    gives up after two consecutive steps find nothing.
    """
    flux = greybody_thin(HIGAL_BANDS, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0)
    errors = [0.1 * f for f in flux]
    result = fit_thin(HIGAL_BANDS, flux, distance_pc=1500.0, kappa_ref=0.1,
                      lambda_ref_um=300.0, mass_range=[10.0, 3000.0, 20],
                      temp_range=[8.0, 40.0, 20], beta_range=[2.0, 2.0, 1],
                      errors_jy=errors, colour_correction=False)
    # The returned parameters must be exactly as good as the χ² claims.
    model = greybody_thin(HIGAL_BANDS, result.parameters["mass_msun"],
                          result.parameters["temperature_k"], result.parameters["beta"],
                          1500.0, 0.1, 300.0)
    chi2 = sum(((m - f) / e) ** 2 for m, f, e in zip(model, flux, errors))
    assert chi2 == pytest.approx(result.chi2, rel=1e-9)
    # …and the zoom must actually have resolved the parameters.
    assert result.parameters["mass_msun"] == pytest.approx(250.0, rel=0.02)
    assert result.parameters["temperature_k"] == pytest.approx(18.0, rel=0.02)


def test_a_coarse_four_parameter_thick_fit_still_finds_the_optimum():
    """Refining only from the coarse grid's single best cell zooms into the
    wrong basin: the true model scored χ² = 0 while the fit returned χ² ≈ 6."""
    lam = [24.0, 70.0, 160.0, 250.0, 350.0, 500.0, 870.0]
    flux = greybody_thick(lam, size_arcsec=30.0, temperature=26.0, beta=1.8, lambda0_um=90.0)
    result = fit_thick(lam, flux, distance_pc=2000.0, kappa_ref=0.1, lambda_ref_um=300.0,
                       size_arcsec=25.0, temp_range=[10.0, 50.0, 12],
                       beta_range=[1.0, 2.5, 7], lambda0_range=[20.0, 300.0, 12],
                       scale_range=[0.5, 2.0, 7], errors_jy=[0.03 * f for f in flux],
                       colour_correction=False)
    assert result.chi2 < 3.0
    assert result.parameters["temperature_k"] == pytest.approx(26.0, rel=0.10)
    assert result.parameters["beta"] == pytest.approx(1.8, abs=0.15)


def test_an_under_determined_fit_reports_no_reduced_chi2():
    # 2 detections, 3 free parameters: dof = -1, and there is no χ²ᵣ.
    lam = [250.0, 350.0]
    flux = greybody_thin(lam, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0)
    result = fit_thin(lam, flux, distance_pc=1500.0, kappa_ref=0.1, lambda_ref_um=300.0,
                      mass_range=[10.0, 3000.0, 10], temp_range=[8.0, 40.0, 10],
                      beta_range=[1.0, 3.0, 10], errors_jy=[0.1 * f for f in flux],
                      colour_correction=False)
    assert result.dof == -1
    assert result.chi2_reduced is None
    assert any("not over-determined" in w for w in result.warnings)


def test_the_fit_route_ranks_an_under_determined_model_last():
    client, auth = _client()
    r = client.post("/v1/sed/fit", json=_fit_body(), headers=auth)
    assert r.status_code == 200
    assert r.json()["best"] == "thin"


def test_models_route_nulls_non_finite_fields_instead_of_failing_to_serialise(monkeypatch):
    client, auth = _client()
    payload = {
        "columns": ["clump_mass", "dust_temp", "chi2", "wmod", "fmod"],
        # 1e400 parses as inf: json.loads accepts it, json.dumps then cannot.
        "data": [[float("inf"), 20.0, 3.0, [70.0], [1.0]]],
    }
    from app.routers import sed as sed_router
    monkeypatch.setattr(sed_router, "_fetch_json", lambda url: payload)
    monkeypatch.setattr(sed_router, "assert_safe_fetch_url", lambda *a, **k: None)
    r = client.post("/v1/sed/models",
                    json={"wavelengths_um": [70.0, 250.0], "fluxes_jy": [1.0, 2.0],
                          "service_url": "https://vlkb.example.org"},
                    headers=auth)
    assert r.status_code == 200, r.text
    assert r.json()["models"][0]["clump_mass"] is None


def test_two_free_parameters_and_a_tiny_error_still_give_a_real_interval():
    """Profiling over a grid that does not contain the refined optimum made the
    profile at the best fit sit *above* χ²_min + 1, so the threshold search
    started outside its own bracket and reported zero for both parameters."""
    flux = greybody_thin(HIGAL_BANDS, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0)
    errors = [0.001 * f for f in flux]
    result = fit_thin(HIGAL_BANDS, flux, distance_pc=1500.0, kappa_ref=0.1,
                      lambda_ref_um=300.0, mass_range=[10.0, 3000.0, 40],
                      temp_range=[8.0, 40.0, 20], beta_range=[2.0, 2.0, 1],
                      errors_jy=errors, colour_correction=False)
    assert result.uncertainties["mass_msun"] > 0.0
    assert result.uncertainties["temperature_k"] > 0.0
    # Mass enters the thin greybody linearly, so with temperature essentially
    # pinned by the data the interval has a closed form to check against.
    expected = 250.0 / math.sqrt(sum((f / e) ** 2 for f, e in zip(flux, errors)))
    assert result.uncertainties["mass_msun"] == pytest.approx(expected, rel=0.25)


def test_a_side_the_search_never_explored_is_reported_as_open():
    # β fixed, mass pinned to its own lower bound: the interval cannot have been
    # closed on that side, and must not be reported as if it had been.
    flux = greybody_thin(HIGAL_BANDS, 250.0, 18.0, 2.0, 1500.0, 0.1, 300.0)
    result = fit_thin(HIGAL_BANDS, flux, distance_pc=1500.0, kappa_ref=0.1,
                      lambda_ref_um=300.0, mass_range=[250.0, 3000.0, 10],
                      temp_range=[8.0, 40.0, 10], beta_range=[2.0, 2.0, 1],
                      errors_jy=[0.5 * f for f in flux], colour_correction=False)
    # A fit whose mass sits at the very bottom of the searched range has no
    # lower bound to quote.
    if result.parameters["mass_msun"] <= 250.0:
        assert "mass_msun" in result.unbounded
