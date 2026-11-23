"""The spectral axis in another quantity and frame (issue #496)."""

from __future__ import annotations

import numpy as np
import pytest
from astropy.io import fits

from app.spectral_axis import spectral_axis

C_KMS = 299792.458
F0 = 268.0e9


def _cube(tmp_path, *, ctype="FREQ", crval=F0, cdelt=-50e6, unit="Hz", specsys="BARYCENT",
          obs=True, rest=True, stokes_before_freq=False, nz=21, name="c.fits"):
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"] = "RA---SIN", "DEC--SIN"
    h["CRVAL1"], h["CRVAL2"], h["CRPIX1"], h["CRPIX2"] = 136.9, -0.7, 5.0, 5.0
    h["CDELT1"], h["CDELT2"], h["CUNIT1"], h["CUNIT2"] = -1e-6, 1e-6, "deg", "deg"
    spec = {"CTYPE": ctype, "CRVAL": crval, "CRPIX": 11.0, "CDELT": cdelt, "CUNIT": unit}
    stokes = {"CTYPE": "STOKES", "CRVAL": 1.0, "CRPIX": 1.0, "CDELT": 1.0, "CUNIT": ""}
    for i, axis in enumerate((stokes, spec) if stokes_before_freq else (spec, stokes), start=3):
        for key, value in axis.items():
            h[f"{key}{i}"] = value
    if specsys:
        h["SPECSYS"] = specsys
    if rest:
        h["RESTFRQ"] = F0
    if obs:
        h["MJD-OBS"] = 57327.4
        h["OBSGEO-X"], h["OBSGEO-Y"], h["OBSGEO-Z"] = 2225142.18, -5440307.37, -2481029.85
    h["RADESYS"] = "ICRS"
    shape = (nz, 1, 9, 9) if stokes_before_freq else (1, nz, 9, 9)
    path = tmp_path / name
    fits.PrimaryHDU(np.zeros(shape, np.float32), h).writeto(path)
    return path


def _freqs(nz=21):
    return F0 + (np.arange(nz) + 1 - 11.0) * -50e6


def test_native_and_frequency_are_the_linear_axis(tmp_path):
    path = _cube(tmp_path)
    for quantity in ("native", "frequency"):
        out = spectral_axis(str(path), quantity)
        np.testing.assert_allclose(out["values"], _freqs(), rtol=1e-12)
        assert out["unit"] == "Hz" and out["ctype"] == "FREQ"
        assert out["frame"] == out["native_frame"] == "BARYCENT"


def test_radio_and_optical_velocities(tmp_path):
    path = _cube(tmp_path)
    f = _freqs()
    vrad = spectral_axis(str(path), "vrad")
    np.testing.assert_allclose(vrad["values"], C_KMS * (1 - f / F0), atol=1e-6)
    assert vrad["values"][10] == pytest.approx(0.0, abs=1e-9)
    assert vrad["unit"] == "km/s" and vrad["ctype"] == "VRAD"
    vopt = spectral_axis(str(path), "vopt")
    np.testing.assert_allclose(vopt["values"], C_KMS * (F0 / f - 1), atol=1e-6)
    shifted = spectral_axis(str(path), "vrad", rest_freq_hz=F0 * (1 + 1e-4))
    assert shifted["values"][10] == pytest.approx(C_KMS * (1 - 1 / (1 + 1e-4)), rel=1e-9)
    assert "instead of the header" in shifted["note"]


def test_another_frame_shifts_by_a_plausible_amount(tmp_path):
    path = _cube(tmp_path)
    native = np.array(spectral_axis(str(path), "frequency")["values"])
    for frame in ("LSRK", "TOPOCENT", "GEOCENTR"):
        out = spectral_axis(str(path), "frequency", frame)
        shift = np.array(out["values"]) / native - 1
        # Earth's orbit, rotation and the solar motion: tens of km/s at most.
        assert 1e-7 < np.max(np.abs(shift)) < 1.5e-4, frame
        assert out["frame"] == frame
    same = spectral_axis(str(path), "frequency", "BARYCENT")
    np.testing.assert_allclose(same["values"], native, rtol=1e-12)


def test_topocentric_and_back_agree(tmp_path):
    bary = _cube(tmp_path)
    topo_values = np.array(spectral_axis(str(bary), "frequency", "TOPOCENT")["values"])
    # A TOPOCENT cube with exactly those frequencies converts back to BARYCENT.
    step = topo_values[1] - topo_values[0]
    topo = _cube(tmp_path, crval=float(topo_values[10]), cdelt=float(step),
                 specsys="TOPOCENT", name="t.fits")
    back = np.array(spectral_axis(str(topo), "frequency", "BARYCENT")["values"])
    np.testing.assert_allclose(back[10], _freqs()[10], rtol=2e-9)


def test_missing_information_is_explained(tmp_path):
    with pytest.raises(ValueError, match="observation date"):
        spectral_axis(str(_cube(tmp_path, obs=False, name="a.fits")), "frequency", "LSRK")
    with pytest.raises(ValueError, match="no SPECSYS"):
        spectral_axis(str(_cube(tmp_path, specsys=None, name="b.fits")), "frequency", "LSRK")
    with pytest.raises(ValueError, match="rest frequency"):
        spectral_axis(str(_cube(tmp_path, rest=False, name="d.fits")), "vrad")
    with pytest.raises(ValueError, match="Unknown spectral frame"):
        spectral_axis(str(_cube(tmp_path, name="e.fits")), "frequency", "HELIO")


def test_a_velocity_cube_as_frequency(tmp_path):
    path = _cube(tmp_path, ctype="VRAD", crval=0.0, cdelt=5000.0, unit="m/s", name="v.fits")
    out = spectral_axis(str(path), "frequency")
    v = (np.arange(21) + 1 - 11.0) * 5.0
    np.testing.assert_allclose(out["values"], F0 * (1 - v / C_KMS), rtol=1e-9)
    assert spectral_axis(str(path), "vrad")["values"][15] == pytest.approx(25.0, rel=1e-9)


def test_stokes_before_freq(tmp_path):
    out = spectral_axis(str(_cube(tmp_path, stokes_before_freq=True)), "vrad")
    assert len(out["values"]) == 21 and out["values"][10] == pytest.approx(0.0, abs=1e-9)


async def test_the_route(client, auth_headers, tmp_path):
    path = _cube(tmp_path)
    opened = (await client.post("/v1/datasets/open", json={"path": str(path)},
                                headers=auth_headers)).json()
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    body = {"dataset_id": opened["dataset_id"], "quantity": "vrad", "frame": "LSRK"}
    resp = await client.post("/v1/spectral/axis", json=body, headers=headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["frame"] == "LSRK" and len(resp.json()["values"]) == 21
    body["quantity"] = "speed"
    assert (await client.post("/v1/spectral/axis", json=body, headers=headers)).status_code == 422
