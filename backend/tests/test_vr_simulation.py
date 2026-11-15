"""Simulation volumes in VR (issue #317): the reduced cube's log scale (both
from the whole cube and from its brick pyramid), the detection of a
simulation volume (no spectral axis, linear axes) and what reduced_info says
of it (axes, spacing, a percentile threshold, no noise, beam or rest line),
and that a radio cube is reduced exactly as before."""
from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import cube_reduce
from app.brick_pyramid import build_pyramid
from app.compute import reduced as R
from app.volume_source import vbt_field_ref


# ── A density over seven decades, as a VBT volume and as a FITS file ───────

def _density(shape=(16, 24, 32)) -> np.ndarray:
    """1e-12 in the ambient medium up to 1e-5 in a shell: seven decades."""
    nz, ny, nx = shape
    z, y, x = np.mgrid[0:nz, 0:ny, 0:nx].astype(np.float32)
    r = np.sqrt((x - nx / 2) ** 2 + (y - ny / 2) ** 2 + (2 * (z - nz / 2)) ** 2)
    return (1e-12 * 10 ** (7 * np.exp(-(r - 8) ** 2 / 8))).astype(np.float32)


def _write_vbt(tmp_path: Path, fields, spacing=(0.5, 0.5, 1.0), names=("rho", "vx")) -> Path:
    nz, ny, nx = fields[0].shape
    data = tmp_path / "v.bin"
    with open(data, "wb") as fh:
        for f in fields:
            f.astype("<f4").tofile(fh)
    sx, sy, sz = spacing
    (tmp_path / "v.bin.head").write_text("\n".join(
        ["float", str(len(fields)), f"{nx * ny * nz} {nx} {ny} {nz} {sx} {sy} {sz}", "little",
         *names[:len(fields)]]) + "\n")
    return data


@pytest.fixture
def vbt(tmp_path):
    rho = _density()
    vx = np.sin(np.arange(rho.size, dtype=np.float32) / 50).reshape(rho.shape)
    return _write_vbt(tmp_path, [rho, vx]), rho, vx


# ── Quantisation ────────────────────────────────────────────────────────────

def test_auto_scale_is_log_only_for_a_simulation_with_positive_values_over_three_decades():
    sim = dict(simulation=True)
    assert cube_reduce.resolve_scale("auto", 1e-12, 2e-5, **sim) == "log"
    assert cube_reduce.resolve_scale("auto", 1.0, 999.0, **sim) == "linear"       # under 3 decades
    assert cube_reduce.resolve_scale("auto", -0.01, 1e6, **sim) == "linear"      # signed field
    assert cube_reduce.resolve_scale("auto", 0.0, 1e6, **sim) == "linear"        # zeros: no log
    assert cube_reduce.resolve_scale("auto", 0.0, 0.0, **sim) == "linear"        # an empty cube
    # Not a simulation (a radio cube, masked or moment-derived): auto stays linear.
    assert cube_reduce.resolve_scale("auto", 1e-12, 2e-5) == "linear"
    assert cube_reduce.resolve_scale("auto", 1e-12, 2e-5, simulation=False) == "linear"
    # Asked for: on any cube.
    assert cube_reduce.resolve_scale("log", -1.0, 1.0) == "log"
    assert cube_reduce.resolve_scale("log", 1e-12, 2e-5) == "log"
    assert cube_reduce.resolve_scale("linear", 1e-12, 1e-5, **sim) == "linear"
    with pytest.raises(ValueError):
        cube_reduce.resolve_scale("asinh", 1.0, 2.0)


@pytest.mark.parametrize("encoding, decades_err", [("uint8", 7 / 254 / 2), ("float16", 2e-3)])
def test_log_quantisation_round_trips_every_decade(encoding, decades_err):
    """Codes of log10(v): the relative error is the same at every decade (half
    a code of the log range for uint8), where linear codes put all but the top
    decade in code 1."""
    v = np.logspace(-12, -5, 4096, dtype=np.float64).astype(np.float32).reshape(4, 32, 32)
    lo, hi = math.log10(float(v.min())), math.log10(float(v.max()))
    raw = cube_reduce.quantise(v, lo, hi, encoding, "log")
    back = cube_reduce.dequantise(raw, v.shape, lo, hi, encoding, "log")
    err = np.abs(np.log10(back) - np.log10(v))
    assert float(err.max()) <= decades_err * 1.05
    if encoding == "uint8":
        codes = np.frombuffer(raw, np.uint8)
        assert len(np.unique(codes)) == 255
        linear = np.frombuffer(cube_reduce.quantise(v, float(v.min()), float(v.max()), "uint8"), np.uint8)
        # Linear: the four lower decades (4/7 of the voxels) all in code 1.
        assert (linear[v.ravel() < 1e-8] == 1).all()
        assert np.count_nonzero(linear == 1) > 4 / 7 * v.size


@pytest.mark.parametrize("encoding", ["uint8", "float16"])
def test_values_at_or_below_zero_are_blank_on_a_log_scale(encoding):
    v = np.array([[[1e-3, 0.0, -2.0, np.nan, 1.0, 10.0]]], dtype=np.float32)
    lo, hi, n = cube_reduce.positive_range(v)
    assert (lo, n) == (pytest.approx(1e-3), 2) and hi == pytest.approx(10.0)
    raw = cube_reduce.quantise(v, math.log10(lo), math.log10(hi), encoding, "log")
    back = cube_reduce.dequantise(raw, v.shape, math.log10(lo), math.log10(hi), encoding, "log")
    assert np.isnan(back[0, 0, 1:4]).all()
    np.testing.assert_allclose(back[0, 0, [0, 4, 5]], [1e-3, 1.0, 10.0], rtol=0.03)
    assert cube_reduce.positive_range(np.array([[[-1.0, np.nan]]], np.float32)) == (0.0, 0.0, 1)


# ── Detection and reduced_info ──────────────────────────────────────────────

def test_a_vbt_volume_is_a_simulation_with_its_axes_spacing_and_a_percentile_threshold(vbt):
    data, rho, _ = vbt
    info = R.worker_cube_reduced_info(vbt_field_ref(data, 0), 0, 0, 256)
    assert info["simulation"] is True
    assert info["object"] == "rho"
    assert info["spacing"] == [0.5, 0.5, 1.0]
    ax = info["axes"]
    assert [ax[k]["name"] for k in "xyz"] == ["X", "Y", "Z"]
    assert [ax[k]["spacing"] for k in "xyz"] == [0.5, 0.5, 1.0]
    np.testing.assert_allclose(ax["z"]["world"], np.arange(16) * 1.0)
    np.testing.assert_allclose(ax["x"]["world"], np.arange(32) * 0.5)
    assert ax["z"]["velocity_kms"] is None and ax["z"]["full_velocity_kms"] is None
    # No noise, beam, rest line or sky scale; the 99th percentile instead of the noise rule.
    assert info["noise_sigma"] is None and info["beam"] is None
    assert info["rest_frequency_hz"] is None and info["rest_line"] == ""
    assert info["pixel_scale_arcsec"] is None
    assert info["default_threshold"] == pytest.approx(float(np.percentile(rho, 99.0)), rel=1e-6)
    assert "99th percentile" in info["default_threshold_method"]


def test_a_radio_cube_is_not_a_simulation(opened_cube_path):
    info = R.worker_cube_reduced_info(str(opened_cube_path), 0, 0, 256)
    assert info["simulation"] is False
    assert info["axes"]["x"]["name"] == "RA" and info["noise_sigma"] is not None


@pytest.fixture
def opened_cube_path(synthetic_fits_cube):
    return synthetic_fits_cube


@pytest.mark.parametrize("ctypes, sim", [
    (("X", "Y", "Z"), True), (("", "", ""), True), (("LINEAR", "LINEAR", "LINEAR"), True),
    (("RA---SIN", "DEC--SIN", "FREQ"), False), (("GLON-CAR", "GLAT-CAR", "VELO-LSR"), False),
    (("X", "Y", "FREQ"), False), (("RA", "DEC", "Z"), False)])
def test_simulation_detection_by_the_axes(tmp_path, ctypes, sim):
    h = fits.Header()
    for i, c in enumerate(ctypes, start=1):
        if c:
            h[f"CTYPE{i}"] = c
    path = tmp_path / "c.fits"
    fits.PrimaryHDU(np.ones((4, 5, 6), np.float32), h).writeto(path)
    from app.fits_dataset import ScientificFitsDataset
    assert R.is_simulation(ScientificFitsDataset(str(path))) is sim


# ── The reduced cube ────────────────────────────────────────────────────────

@pytest.mark.parametrize("mode", ["mean", "max"])
def test_a_density_is_reduced_on_a_log_scale_by_default(vbt, mode):
    data, rho, _ = vbt
    res = R.worker_cube_reduced(vbt_field_ref(data, 0), 2, 2, 256, mode, "uint8")
    assert res["scale"] == "log" and res["simulation"] is True and res["nonpositive_voxels"] == 0
    want = cube_reduce.reduce_cube(rho, (2, 2, 2), mode)
    assert res["range_min"] == pytest.approx(float(np.nanmin(want)), rel=1e-6)
    assert res["log_min"] == pytest.approx(math.log10(res["range_min"]), abs=1e-9)
    assert res["log_max"] == pytest.approx(math.log10(res["range_max"]), abs=1e-9)
    shape = (res["depth"], res["height"], res["width"])
    back = cube_reduce.dequantise(res["raw_bytes"], shape, res["log_min"], res["log_max"], "uint8", "log")
    np.testing.assert_allclose(np.log10(back), np.log10(want), atol=(res["log_max"] - res["log_min"]) / 254 / 2 + 1e-6)
    # The threshold is a value (the 99th percentile), not its logarithm.
    assert res["threshold"] > res["range_min"]


def test_a_signed_field_of_a_simulation_stays_linear(vbt):
    data, _, vx = vbt
    res = R.worker_cube_reduced(vbt_field_ref(data, 1), 1, 1, 256, "mean", "uint8")
    assert res["scale"] == "linear" and res["log_min"] is None
    assert res["threshold"] == pytest.approx(float(np.percentile(vx, 99.0)), rel=1e-5)


def test_log_asked_for_a_cube_without_positive_values_is_refused(tmp_path):
    path = tmp_path / "neg.fits"
    fits.PrimaryHDU(-np.ones((4, 5, 6), np.float32)).writeto(path)
    with pytest.raises(ValueError, match="positive"):
        R.worker_cube_reduced(str(path), 1, 1, 256, "mean", "uint8", scale="log")


@pytest.mark.parametrize("encoding", ["uint8", "float16"])
def test_a_radio_cube_is_reduced_exactly_as_before(synthetic_fits_cube, encoding):
    """auto on a cube with negative noise is the linear scale, byte for byte."""
    path = str(synthetic_fits_cube)
    auto = R.worker_cube_reduced(path, 2, 1, 256, "mean", encoding)
    lin = R.worker_cube_reduced(path, 2, 1, 256, "mean", encoding, scale="linear")
    assert auto["scale"] == "linear" and auto["raw_bytes"] == lin["raw_bytes"]
    data = np.squeeze(fits.getdata(path)).astype(np.float32)
    red = cube_reduce.reduce_cube(data, (2, 2, 1), "mean")
    lo, hi = cube_reduce.finite_range(red)
    assert auto["raw_bytes"] == cube_reduce.quantise(red, lo, hi, encoding)
    assert (auto["range_min"], auto["range_max"]) == (lo, hi)


def _masked_radio_cube(tmp_path) -> Path:
    """An all-positive spectral cube over five decades, NaN outside its mask
    (an Isolate or masked-cube output): RA, DEC, FREQ."""
    rho = _density((16, 24, 32)).astype(np.float64) * 1e9      # 1e-3 .. 1e4 Jy/beam
    rho[rho < 2e-3] = np.nan                                    # blanked outside the mask
    h = fits.Header()
    for k, v in dict(CTYPE1="RA---SIN", CRPIX1=16.0, CRVAL1=150.0, CDELT1=-0.002, CUNIT1="deg",
                     CTYPE2="DEC--SIN", CRPIX2=12.0, CRVAL2=2.0, CDELT2=0.002, CUNIT2="deg",
                     CTYPE3="FREQ", CRPIX3=1.0, CRVAL3=1.4e9, CDELT3=-1e5, CUNIT3="Hz",
                     RESTFRQ=1.420405751768e9, BUNIT="Jy/beam").items():
        h[k] = v
    path = tmp_path / "masked_radio.fits"
    fits.PrimaryHDU(rho.astype(np.float32), h).writeto(path)
    return path


@pytest.mark.parametrize("encoding", ["uint8", "float16"])
@pytest.mark.parametrize("mode", ["mean", "max"])
def test_an_all_positive_masked_radio_cube_stays_linear_under_auto(tmp_path, encoding, mode):
    """Positive values over more than three decades, NaN-masked, with a
    spectral axis: not a simulation, so auto is linear, byte for byte; a log
    scale asked for still applies."""
    path = str(_masked_radio_cube(tmp_path))
    data = np.squeeze(fits.getdata(path))
    fin = data[np.isfinite(data)]
    assert fin.min() > 0 and math.log10(fin.max() / fin.min()) > 3 and np.isnan(data).any()
    info = R.worker_cube_reduced_info(path, 2, 1, 256)
    assert info["simulation"] is False
    auto = R.worker_cube_reduced(path, 2, 1, 256, mode, encoding)
    lin = R.worker_cube_reduced(path, 2, 1, 256, mode, encoding, scale="linear")
    assert auto["scale"] == "linear" and auto["log_min"] is None
    assert auto["raw_bytes"] == lin["raw_bytes"]
    assert (auto["range_min"], auto["range_max"], auto["threshold"]) == \
        (lin["range_min"], lin["range_max"], lin["threshold"])
    log = R.worker_cube_reduced(path, 2, 1, 256, mode, encoding, scale="log")
    assert log["scale"] == "log" and log["raw_bytes"] != lin["raw_bytes"]


def test_the_pyramid_path_quantises_the_same_log_scale(tmp_path):
    """A density read from its brick pyramid: the same log codes as from the cube."""
    rho = _density((32, 32, 48))
    path = tmp_path / "rho.fits"
    fits.PrimaryHDU(rho).writeto(path)
    out = tmp_path / "rho.pyr.h5"
    build_pyramid(path, out, brick=8)
    whole = R.worker_cube_reduced(str(path), 4, 4, 16, "mean", "uint8")
    pyr = R.worker_cube_reduced(str(path), 4, 4, 16, "mean", "uint8", pyramid_path=str(out))
    assert pyr["source"] == "pyramid" and pyr["scale"] == whole["scale"] == "log"
    a = np.frombuffer(whole["raw_bytes"], np.uint8).astype(int)
    b = np.frombuffer(pyr["raw_bytes"], np.uint8).astype(int)
    assert np.abs(a - b).max() <= 1
    info = R.worker_cube_reduced_info(str(path), 4, 4, 16, pyramid_path=str(out))
    assert info["simulation"] is True and info["noise_sigma"] is None
    assert "percentile" in info["default_threshold_method"]


# ── The routes ──────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_the_routes_say_simulation_and_send_the_log_scale(client, auth_headers, vbt):
    data, rho, _ = vbt
    opened = await client.post("/v1/datasets/open", json={"path": vbt_field_ref(data, 0)}, headers=auth_headers)
    assert opened.status_code == 200, opened.text
    assert opened.json()["kind"] == "cube"
    hd = {**auth_headers, "X-Visivo-Session": opened.json()["session_id"]}
    req = {"dataset_id": opened.json()["dataset_id"], "factor_xy": 1, "factor_z": 1, "source": "cube"}
    info = (await client.post("/v1/cube/reduced_info", json=req, headers=hd)).json()
    assert info["simulation"] is True and info["spacing"] == [0.5, 0.5, 1.0]
    r = await client.post("/v1/cube/reduced_bin", json=req, headers=hd)
    assert r.status_code == 200, r.text
    assert r.headers["X-Visivo-Scale"] == "log"
    lo, hi = (float(v) for v in r.headers["X-Visivo-Log-Range"].split(","))
    assert float(r.headers["X-Visivo-Range-Min"]) == pytest.approx(10 ** lo, rel=1e-6)
    assert float(r.headers["X-Visivo-Range-Max"]) == pytest.approx(10 ** hi, rel=1e-6)
    assert r.headers["X-Visivo-Nonpositive-Voxels"] == "0"
    back = cube_reduce.dequantise(r.content, rho.shape, lo, hi, "uint8", "log")
    np.testing.assert_allclose(back, rho, rtol=0.035)
    lin = await client.post("/v1/cube/reduced_bin", json={**req, "scale": "linear"}, headers=hd)
    assert lin.headers["X-Visivo-Scale"] == "linear" and lin.headers["X-Visivo-Log-Range"] == ""
    bad = await client.post("/v1/cube/reduced_bin", json={**req, "scale": "asinh"}, headers=hd)
    assert bad.status_code == 422
