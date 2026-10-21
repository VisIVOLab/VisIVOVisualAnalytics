"""The kinematic fit of a selection recovers a disc whose parameters are known."""
import json
import math

import numpy as np
import pytest
from astropy.io import fits

from app.compute.kinematic_fit import (fit_flat, geometry_from_m0, kinematic_fit,
                                       source_measures, velocity_axis_kms,
                                       worker_kinematic_fit)
from app.kinematic_model import tilted_ring_model

NX, NY, NZ = 72, 72, 64
VEL = np.linspace(800.0, 1300.0, NZ)          # km/s, ascending
TRUE = dict(x0=35.5, y0=37.0, pa=40.0, incl=60.0, vsys=1050.0, vrot=150.0, rmax=24.0)


def _disc(noise=0.002, seed=1):
    model = tilted_ring_model(NX, NY, VEL, TRUE["x0"], TRUE["y0"], TRUE["pa"], TRUE["incl"],
                              TRUE["vsys"], TRUE["vrot"], TRUE["rmax"], sigma=12.0)
    rng = np.random.default_rng(seed)
    data = (0.05 * model + rng.normal(0.0, noise, model.shape)).astype(np.float32)
    return data, model > 0.05


def test_flat_fit_recovers_the_disc():
    data, mask = _disc()
    dvk = np.full(NZ, abs(VEL[1] - VEL[0]))
    meas = source_measures(data, mask, VEL, dvk, beam_px=None, sigma=0.002)
    geom = geometry_from_m0(meas["m0"])
    assert abs(geom["x0"] - TRUE["x0"]) < 1.0 and abs(geom["y0"] - TRUE["y0"]) < 1.0
    assert abs(geom["incl"] - TRUE["incl"]) < 6.0
    flat = fit_flat(meas["m1"], meas["m0"], geom)
    dpa = (flat["pa"] - TRUE["pa"] + 180.0) % 360.0 - 180.0
    assert abs(dpa) <= 3.0
    assert abs(flat["vsys"] - TRUE["vsys"]) < 5.0
    # V_rot and sin i are degenerate: the projected amplitude is what is measured.
    assert abs(flat["vrot_sini"] - TRUE["vrot"] * math.sin(math.radians(TRUE["incl"]))) < 10.0
    assert flat["vrot"] > 0.0


def test_full_fit_has_small_residual_and_rings():
    data, mask = _disc()
    dvk = np.full(NZ, abs(VEL[1] - VEL[0]))
    fit = kinematic_fit(data, mask, VEL, dvk, beam_px=None, sigma=0.002, min_ring_pix=40)
    assert fit["resid_flat"]["median"] < 2 * fit["dv"]
    assert len(fit["rings"]) >= 3
    pas = [r["pa"] for r in fit["rings"]]
    assert max(pas) - min(pas) < 30.0        # a flat disc has no warp to find


def test_frequency_axis_is_optical():
    rest = 1420405751.79
    v = 1100.0
    freq = rest / (1.0 + v / 299792.458)
    out, unit, mode = velocity_axis_kms(np.array([freq]), "Hz", "FREQ", rest)
    assert (unit, mode) == ("km/s", "velocity")
    assert abs(out[0] - v) < 1e-6


def test_worker_on_a_fits_cube_is_json_safe(tmp_path):
    data, mask = _disc()
    h = fits.Header()
    h["CTYPE1"], h["CRPIX1"], h["CRVAL1"], h["CDELT1"], h["CUNIT1"] = "RA---SIN", 1, 150.0, -0.001, "deg"
    h["CTYPE2"], h["CRPIX2"], h["CRVAL2"], h["CDELT2"], h["CUNIT2"] = "DEC--SIN", 1, -29.0, 0.001, "deg"
    h["CTYPE3"], h["CRPIX3"], h["CRVAL3"] = "VOPT", 1, VEL[0] * 1000.0
    h["CDELT3"], h["CUNIT3"] = (VEL[1] - VEL[0]) * 1000.0, "m/s"
    h["BUNIT"], h["BMAJ"], h["BMIN"], h["BPA"] = "Jy/beam", 0.003, 0.003, 0.0
    path = tmp_path / "disc.fits"
    fits.PrimaryHDU(data, header=h).writeto(path)
    z, y, x = np.nonzero(mask)
    roi = mask[z.min():z.max() + 1, y.min():y.max() + 1, x.min():x.max() + 1].astype("<f4")
    out = worker_kinematic_fit(str(path), roi.tobytes(), int(x.min()), int(y.min()), int(z.min()),
                               roi.shape[2], roi.shape[1], roi.shape[0], sigma=0.002,
                               min_ring_pix=40)
    json.dumps(out, allow_nan=False)          # the route returns it as JSON
    assert out["sigma_source"] == "noise estimate"
    assert out["velocity_unit"] == "km/s"
    assert out["measures"]["flux_is_jy"] is True and out["measures"]["flux"] > 0
    assert out["width"] == NX and out["height"] == NY
    assert len(out["spectrum"]) == NZ


def test_worker_rejects_an_empty_selection(tmp_path):
    data, _ = _disc()
    path = tmp_path / "c.fits"
    h = fits.Header()
    h["CTYPE3"], h["CRPIX3"], h["CRVAL3"], h["CDELT3"], h["CUNIT3"] = "VOPT", 1, 8e5, 7.9e3, "m/s"
    fits.PrimaryHDU(data, header=h).writeto(path)
    with pytest.raises(ValueError):
        worker_kinematic_fit(str(path), np.zeros(8, "<f4").tobytes(), 0, 0, 0, 2, 2, 2)


@pytest.mark.asyncio
async def test_route_rejects_a_mask_of_the_wrong_size(client, opened_cube):
    resp = await client.post(
        "/v1/cube/kinematic_fit_bin",
        params={"dataset_id": opened_cube["dataset_id"], "origin_x": 0, "origin_y": 0,
                "origin_z": 0, "width": 4, "height": 4, "depth": 4},
        content=b"\x00" * 10, headers=opened_cube["headers"])
    assert resp.status_code == 422


def _write(path, data, spectral_axis=3):
    h = fits.Header()
    axes = {1: ("RA---SIN", "deg", 150.0, -0.001), 2: ("DEC--SIN", "deg", -29.0, 0.001),
            3: ("VOPT", "m/s", VEL[0] * 1000.0, (VEL[1] - VEL[0]) * 1000.0)}
    if spectral_axis == 2:
        axes[2], axes[3] = axes[3], axes[2]
    for ax, (ctype, unit, crval, cdelt) in axes.items():
        h[f"CTYPE{ax}"], h[f"CUNIT{ax}"], h[f"CRPIX{ax}"] = ctype, unit, 1.0
        h[f"CRVAL{ax}"], h[f"CDELT{ax}"] = crval, cdelt
    fits.PrimaryHDU(data, header=h).writeto(path)


def test_worker_refuses_a_transposed_cube(tmp_path):
    data, mask = _disc()
    path = tmp_path / "t.fits"
    _write(path, data.transpose(1, 0, 2), spectral_axis=2)
    roi = mask.transpose(1, 0, 2).astype("<f4")
    with pytest.raises(ValueError, match="spectral axis"):
        worker_kinematic_fit(str(path), roi.tobytes(), 0, 0, 0, roi.shape[2], roi.shape[1],
                             roi.shape[0], sigma=0.002)


def test_worker_refuses_a_cube_over_the_cap(tmp_path):
    from app.kinematic_model import ModelTooLargeError
    data, mask = _disc()
    path = tmp_path / "big.fits"
    _write(path, data)
    roi = mask.astype("<f4")
    with pytest.raises(ModelTooLargeError):
        worker_kinematic_fit(str(path), roi.tobytes(), 0, 0, 0, NX, NY, NZ, sigma=0.002,
                             cap=1000)


def test_residual_of_a_given_model_matches_the_fit():
    from app.compute.kinematic_fit import model_residual
    data, mask = _disc()
    dvk = np.full(NZ, abs(VEL[1] - VEL[0]))
    fit = kinematic_fit(data, mask, VEL, dvk, beam_px=None, sigma=0.002, min_ring_pix=40)
    g, f = fit["geometry"], fit["flat"]
    out = model_residual(data, mask, VEL, dvk, 0.002, g["x0"], g["y0"], f["vsys"],
                         fit["line_sigma"], f["pa"], g["incl"], f["vrot"], g["rmax"])
    assert abs(out["stats"]["median"] - fit["resid_flat"]["median"]) < 1e-9
    rings = [(r["r"], r["pa"], r["incl"], r["vrot"]) for r in fit["rings"]]
    out_r = model_residual(data, mask, VEL, dvk, 0.002, g["x0"], g["y0"], f["vsys"],
                           fit["line_sigma"], rings=rings)
    assert abs(out_r["stats"]["median"] - fit["resid_warp"]["median"]) < 1e-9
    # A wrong position angle is visibly worse: the residual measures the model given.
    bad = model_residual(data, mask, VEL, dvk, 0.002, g["x0"], g["y0"], f["vsys"],
                         fit["line_sigma"], f["pa"] + 90.0, g["incl"], f["vrot"], g["rmax"])
    assert bad["stats"]["median"] > 3 * out["stats"]["median"]


@pytest.mark.asyncio
async def test_residual_route_validates_rings(client, opened_cube):
    base = {"dataset_id": opened_cube["dataset_id"], "origin_x": 0, "origin_y": 0,
            "origin_z": 0, "width": 2, "height": 2, "depth": 2, "x0": 1, "y0": 1,
            "vsys": 0, "line_sigma": 5}
    body = np.ones(8, "<f4").tobytes()
    for rings in ("1:2:3", "1:0:95:10", "5:0:30:10,5:0:30:10"):
        resp = await client.post("/v1/cube/kinematic_residual_bin", params={**base, "rings": rings},
                                 content=body, headers=opened_cube["headers"])
        assert resp.status_code == 422, (rings, resp.status_code)
    resp = await client.post("/v1/cube/kinematic_residual_bin", params=base, content=body,
                             headers=opened_cube["headers"])
    assert resp.status_code == 422           # neither rings nor a single disc
