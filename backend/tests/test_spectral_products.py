"""Moments and PV on the spectral axis the user chose (issue #496).

The cubes hold Gaussian lines whose centre and width are known in a given
velocity: the moments computed on that axis must give those numbers back.
"""

from __future__ import annotations

import base64

import numpy as np
import pytest
from astropy.io import fits

import app.compute.moment as M
from app.compute import worker_moment, worker_pv, worker_pv_save_fits
from app.spectral_axis import normalize_choice, spectral_axis

C_KMS = 299792.458
F0 = 268.0e9            # rest frequency of the line
NZ, NY, NX = 241, 3, 4
DF = -1.0e6             # channel width: 1.12 km/s, so a 12 km/s line spans ~10 channels
LINES = {(0, 0): (-30.0, 12.0), (1, 2): (25.0, 15.0), (2, 3): (5.0, 20.0)}   # centre, sigma (km/s)


def _cube(tmp_path, *, specsys="BARYCENT", name="cube.fits", line_in=None, **extra):
    """A FREQ cube; the lines are Gaussian in ``line_in``: a (quantity, frame)
    pair whose axis table is read from the file itself."""
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"] = "RA---SIN", "DEC--SIN"
    h["CRVAL1"], h["CRVAL2"], h["CRPIX1"], h["CRPIX2"] = 136.9, -0.7, 2.0, 2.0
    h["CDELT1"], h["CDELT2"], h["CUNIT1"], h["CUNIT2"] = -1e-4, 1e-4, "deg", "deg"
    h["CTYPE3"], h["CRVAL3"], h["CRPIX3"] = "FREQ", F0, 121.0
    h["CDELT3"], h["CUNIT3"] = DF, "Hz"
    h["RESTFRQ"], h["RADESYS"], h["BUNIT"] = F0, "ICRS", "Jy/beam"
    h["BMAJ"], h["BMIN"], h["BPA"] = 1e-3, 1e-3, 0.0
    if specsys:
        h["SPECSYS"] = specsys
    for key, value in extra.items():
        h[key] = value
    path = tmp_path / name
    data = np.zeros((NZ, NY, NX), np.float32)
    fits.PrimaryHDU(data, h).writeto(path)
    if line_in is not None:
        v = np.asarray(spectral_axis(str(path), *line_in)["values"])
        for (y, x), (centre, sigma) in LINES.items():
            data[:, y, x] = np.exp(-0.5 * ((v - centre) / sigma) ** 2)
        fits.PrimaryHDU(data, h).writeto(path, overwrite=True)
    return path


def _map(result):
    return np.frombuffer(base64.b64decode(result["data_base64"]), np.float32).reshape(
        result["height"], result["width"])


def _moment(path, order, choice=None, **kwargs):
    return worker_moment(str(path), order, 0, NZ - 1, False, 0.0, False, False, None, choice,
                         **kwargs)


def _choice(quantity, frame="native", rest=None):
    return normalize_choice(quantity, frame, rest)


@pytest.mark.parametrize("quantity", ["vrad", "vopt"])
def test_m1_m2_m0_in_velocity_are_the_line(tmp_path, quantity):
    path = _cube(tmp_path, line_in=(quantity,))
    m1, m2, m0 = (_moment(path, o, _choice(quantity)) for o in (1, 2, 0))
    assert m1["moment_unit"] == "km/s" and m2["moment_unit"] == "km/s"
    assert m0["moment_unit"] == "Jy/beam km/s"
    assert m1["spectral_quantity"] == quantity and m1["spectral_frame"] == "BARYCENT"
    assert m1["rest_freq_hz"] == pytest.approx(F0)
    img1, img2, img0 = _map(m1), _map(m2), _map(m0)
    for (y, x), (centre, sigma) in LINES.items():
        assert img1[y, x] == pytest.approx(centre, abs=2e-3)
        assert img2[y, x] == pytest.approx(sigma, rel=2e-3)
        # Sum of I * |dv| over the channels = integral of a unit Gaussian.
        assert img0[y, x] == pytest.approx(sigma * np.sqrt(2 * np.pi), rel=2e-3)


def test_the_file_axis_is_still_frequency_without_a_choice(tmp_path):
    path = _cube(tmp_path, line_in=("vrad",))
    m1 = _moment(path, 1)
    assert m1["moment_unit"] == "Hz" and m1["spectral_axis_unit"] == "Hz"
    assert m1["spectral_quantity"] == "frequency" and m1["spectral_frame"] == "BARYCENT"
    assert _moment(path, 0)["moment_unit"] == "Jy/beam Hz"
    # M1 in Hz is the frequency of the radio-velocity line centre.
    assert _map(m1)[0, 0] == pytest.approx(F0 * (1 + 30.0 / C_KMS), rel=1e-6)


def test_frequency_as_the_chosen_axis(tmp_path):
    path = _cube(tmp_path, line_in=("vrad",))
    m1 = _moment(path, 1, _choice("frequency"))
    assert m1["moment_unit"] == "Hz" and m1["spectral_quantity"] == "frequency"


def test_a_frame_shift_is_an_affine_map_of_the_radio_velocity(tmp_path):
    """Radio velocity in another frame is v' = c(1 - k) + k v (every frequency
    is multiplied by one factor k): M1 follows that map and M2 scales by k."""
    path = _cube(tmp_path, line_in=("vrad", "BARYCENT"))
    base = spectral_axis(str(path), "vrad")["values"]
    shifted = spectral_axis(str(path), "vrad", "LSRK")["values"]
    k = (shifted[1] - shifted[0]) / (base[1] - base[0])
    assert abs(k - 1.0) > 1e-5                      # a real frame change
    m1 = _map(_moment(path, 1, _choice("vrad", "LSRK")))
    m2_native = _map(_moment(path, 2, _choice("vrad")))
    m2 = _map(_moment(path, 2, _choice("vrad", "LSRK")))
    m1_native = _map(_moment(path, 1, _choice("vrad")))
    for (y, x), (centre, _sigma) in LINES.items():
        assert m1[y, x] == pytest.approx(C_KMS * (1 - k) + k * m1_native[y, x], abs=2e-3)
        assert m1[y, x] == pytest.approx(C_KMS * (1 - k) + k * centre, abs=5e-3)
        assert m2[y, x] == pytest.approx(k * m2_native[y, x], rel=1e-5)
    assert _moment(path, 1, _choice("vrad", "LSRK"))["spectral_frame"] == "LSRK"


def test_a_line_gaussian_in_the_new_frame_comes_back_there(tmp_path):
    path = _cube(tmp_path, line_in=("vrad", "LSRK"))
    img = _map(_moment(path, 1, _choice("vrad", "LSRK")))
    for (y, x), (centre, _sigma) in LINES.items():
        assert img[y, x] == pytest.approx(centre, abs=2e-3)


def test_a_rest_frequency_of_the_user_replaces_the_headers(tmp_path):
    path = _cube(tmp_path, line_in=("vrad",))
    rest = F0 * (1 + 100.0 / C_KMS)
    result = _moment(path, 1, _choice("vrad", "native", rest))
    assert result["rest_freq_hz"] == pytest.approx(rest)
    assert "rest frequency" in result["spectral_note"]
    expect = spectral_axis(str(path), "vrad", "native", rest)["values"]
    assert expect[120] == pytest.approx(C_KMS * (1 - F0 / rest), abs=1e-6)
    # Radio velocity against another rest frequency is affine: v' = c(1 - k) + k v.
    k = F0 / rest
    assert _map(result)[0, 0] == pytest.approx(expect[120] + k * -30.0, abs=2e-3)


def test_a_velocity_without_a_rest_frequency_is_explained(tmp_path):
    path = _cube(tmp_path)
    with fits.open(path, mode="update") as hdul:
        del hdul[0].header["RESTFRQ"]
    with pytest.raises(ValueError, match="rest frequency"):
        _moment(path, 1, _choice("vrad"))


def test_moments_without_an_axis_ignore_the_choice(tmp_path):
    """Peak, RMS and minimum use no axis: a choice the cube cannot give (no
    rest frequency) must not refuse them, and they say nothing of it."""
    path = _cube(tmp_path, line_in=("vrad",))
    with fits.open(path, mode="update") as hdul:
        del hdul[0].header["RESTFRQ"]
    peak = _moment(path, 8, _choice("vrad", "LSRK"))
    assert peak["moment_unit"] == "Jy/beam" and peak["spectral_quantity"] == "frequency"
    with pytest.raises(ValueError, match="rest frequency"):
        _moment(path, 1, _choice("vrad", "LSRK"))


def test_a_bad_choice_is_refused():
    with pytest.raises(ValueError, match="quantity"):
        normalize_choice("speed", "native", None)
    with pytest.raises(ValueError, match="frame"):
        normalize_choice("vrad", "MARS", None)
    assert normalize_choice("native", "native", 1e9) is None
    assert normalize_choice(None, None, None) is None


def test_streaming_and_dask_paths_agree_with_in_memory(tmp_path, monkeypatch):
    path = _cube(tmp_path, line_in=("vopt", "LSRK"))
    choice = _choice("vopt", "LSRK")
    monkeypatch.setattr(M, "_moment_stream_bytes", lambda: 1 << 62)
    in_memory = {o: _map(_moment(path, o, choice)) for o in (0, 1, 2)}
    monkeypatch.setattr(M, "_moment_stream_bytes", lambda: 0)
    for order in (0, 1, 2):
        streamed = _moment(path, order, choice)
        assert streamed["moment_unit"] == _moment(path, order, choice)["moment_unit"]
        np.testing.assert_allclose(_map(streamed), in_memory[order], rtol=1e-4, atol=1e-4)
        assert streamed["spectral_quantity"] == "vopt" and streamed["spectral_frame"] == "LSRK"
    if M._DASK_OK:
        for order in (0, 1, 2):
            dask = M._moment_map_dask(str(path), order, 0, NZ - 1, 0.0, False, 64, None, False,
                                      False, None, choice)
            np.testing.assert_allclose(_map(dask), in_memory[order], rtol=1e-4, atol=1e-4)
            assert dask["spectral_quantity"] == "vopt" and dask["moment_unit"] in (
                "km/s", "Jy/beam km/s")


def test_a_channel_range_keeps_its_channel_indices(tmp_path):
    """The range is in channels: the axis values of those channels are used."""
    path = _cube(tmp_path, line_in=("vrad",))
    whole = _map(_moment(path, 1, _choice("vrad")))
    part = _map(worker_moment(str(path), 1, 60, 180, False, 0.0, False, False, None,
                              _choice("vrad")))
    for (y, x) in LINES:
        assert part[y, x] == pytest.approx(whole[y, x], abs=0.2)   # line inside 60..180


def test_the_saved_fits_states_the_axis(tmp_path):
    from app.compute import worker_moment_save_fits

    path = _cube(tmp_path, line_in=("vrad", "LSRK"))
    out = tmp_path / "m1.fits"
    worker_moment_save_fits(str(path), str(out), 1, 0, NZ - 1, False, 0.0, False, False, None,
                            _choice("vrad", "LSRK"))
    h = fits.getheader(out)
    assert h["BUNIT"] == "km/s" and h["SPECSYS"] == "LSRK" and h["SPECQNT"] == "vrad"
    assert h["RESTFRQ"] == pytest.approx(F0)
    assert any("radio velocity" in str(c) and "LSRK" in str(c) for c in h["COMMENT"])
    out0 = tmp_path / "m0.fits"
    worker_moment_save_fits(str(path), str(out0), 0, 0, NZ - 1, False, 0.0, False, False, None,
                            _choice("vrad", "LSRK"))
    assert fits.getheader(out0)["BUNIT"] == "Jy/beam km/s"
    # Without a choice the file's own axis and frame are stated.
    out_native = tmp_path / "m1_native.fits"
    worker_moment_save_fits(str(path), str(out_native), 1, 0, NZ - 1, False, 0.0)
    hn = fits.getheader(out_native)
    assert hn["BUNIT"] == "Hz" and hn["SPECSYS"] == "BARYCENT" and hn["SPECQNT"] == "frequency"
    # A moment that has no spectral axis in it says nothing about one.
    out_peak = tmp_path / "m8.fits"
    worker_moment_save_fits(str(path), str(out_peak), 8, 0, NZ - 1, False, 0.0, False, False, None,
                            _choice("vrad"))
    assert "SPECQNT" not in fits.getheader(out_peak)


def _decode_f64(b64):
    return np.frombuffer(base64.b64decode(b64), "<f8")


def test_pv_coordinates_are_in_the_chosen_axis(tmp_path):
    path = _cube(tmp_path, line_in=("vrad", "LSRK"))
    verts = [[0.0, 0.0], [3.0, 2.0]]
    own = worker_pv(str(path), verts, 1)
    assert own["spectral_axis_unit"] == "Hz" and own["spectral_quantity"] == "frequency"
    chosen = worker_pv(str(path), verts, 1, None, _choice("vrad", "LSRK"))
    table = spectral_axis(str(path), "vrad", "LSRK")["values"]
    np.testing.assert_allclose(_decode_f64(chosen["spectral_coordinates_base64"]), table)
    assert chosen["spectral_axis_unit"] == "km/s" and chosen["spectral_axis_type"] == "VRAD"
    assert chosen["spectral_frame"] == "LSRK" and chosen["spectral_quantity"] == "vrad"
    assert chosen["spectral_crval"] == pytest.approx(table[0])
    assert chosen["data_base64"] == own["data_base64"]          # the diagram itself is the same


def _row_peak_velocity(header, data, column):
    """Velocity of the brightest channel of one column, from the FITS axis 2."""
    row = int(np.nanargmax(data[:, column]))
    return header["CRVAL2"] + header["CDELT2"] * row


def test_pv_fits_radio_velocity_is_a_linear_axis(tmp_path):
    path = _cube(tmp_path, line_in=("vrad", "LSRK"))
    out = tmp_path / "pv.fits"
    worker_pv_save_fits(str(path), str(out), [[0.0, 0.0], [1.0, 0.0]], 1, None,
                        _choice("vrad", "LSRK"))
    h = fits.getheader(out)
    table = np.asarray(spectral_axis(str(path), "vrad", "LSRK")["values"])
    assert (h["CTYPE2"], h["CUNIT2"], h["SPECSYS"], h["SPECQNT"]) == ("VRAD", "km/s", "LSRK", "vrad")
    assert h["CDELT2"] == pytest.approx((table[-1] - table[0]) / (NZ - 1))
    assert h["CRVAL2"] == pytest.approx(table[0])
    # Every channel is on the linear axis: it is exact, nothing was resampled.
    np.testing.assert_allclose(h["CRVAL2"] + h["CDELT2"] * np.arange(NZ), table, atol=1e-6)
    assert not any("resampled" in str(x) for x in h["HISTORY"])
    data = fits.getdata(out)
    assert _row_peak_velocity(h, data, 0) == pytest.approx(-30.0, abs=1.2)


def test_pv_fits_optical_velocity_is_resampled_to_a_linear_axis(tmp_path):
    path = _cube(tmp_path, line_in=("vopt",))
    out = tmp_path / "pv_opt.fits"
    worker_pv_save_fits(str(path), str(out), [[0.0, 0.0], [1.0, 0.0]], 1, None, _choice("vopt"))
    h = fits.getheader(out)
    assert h["CTYPE2"] == "VOPT" and h["CUNIT2"] == "km/s" and h["SPECQNT"] == "vopt"
    assert any("resampled" in str(x) for x in h["HISTORY"])
    data = fits.getdata(out)
    # The Gaussian line is centred where it is on the optical axis, within a channel.
    assert _row_peak_velocity(h, data, 0) == pytest.approx(-30.0, abs=abs(h["CDELT2"]))
    # And the profile keeps its shape: its peak is still ~1 and its flux is conserved.
    assert float(np.nanmax(data[:, 0])) == pytest.approx(1.0, abs=2e-2)
    table = np.asarray(spectral_axis(str(path), "vopt")["values"])
    raw = fits.getdata(path)[:, 0, 0]
    flux_in = np.sum(raw * np.abs(np.gradient(table)))
    flux_out = np.sum(data[:, 0]) * abs(h["CDELT2"])
    assert flux_out == pytest.approx(flux_in, rel=5e-3)


def test_pv_fits_without_a_choice_keeps_the_files_axis(tmp_path):
    path = _cube(tmp_path, line_in=("vrad",))
    out = tmp_path / "pv_native.fits"
    worker_pv_save_fits(str(path), str(out), [[0.0, 0.0], [1.0, 0.0]], 1)
    h = fits.getheader(out)
    assert h["CTYPE2"] == "FREQ" and h["CUNIT2"] == "Hz" and "SPECQNT" not in h


async def test_the_routes_take_and_state_the_choice(client, auth_headers, tmp_path):
    path = _cube(tmp_path, line_in=("vrad", "LSRK"))
    opened = (await client.post("/v1/datasets/open", json={"path": str(path)},
                                headers=auth_headers)).json()
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    base = {"dataset_id": opened["dataset_id"], "moment_order": 1, "channel_start": 0,
            "channel_end": NZ - 1}
    own = (await client.post("/v1/products/moment", json=base, headers=headers)).json()
    assert own["valid"] and own["moment_unit"] == "Hz"
    body = {**base, "spectral_quantity": "vrad", "spectral_frame": "LSRK"}
    chosen = (await client.post("/v1/products/moment", json=body, headers=headers)).json()
    assert chosen["valid"], chosen
    assert chosen["moment_unit"] == "km/s" and chosen["spectral_frame"] == "LSRK"
    assert chosen["spectral_quantity"] == "vrad" and chosen["data_base64"] != own["data_base64"]
    # A second ask is the cached one and is still the chosen axis.
    again = (await client.post("/v1/products/moment", json=body, headers=headers)).json()
    assert again["moment_unit"] == "km/s" and again["data_base64"] == chosen["data_base64"]
    bad = (await client.post("/v1/products/moment", json={**base, "spectral_quantity": "speed"},
                             headers=headers)).json()
    assert not bad["valid"] and "quantity" in bad["error"]
    pv = {"dataset_id": opened["dataset_id"], "vertices": [[0.0, 0.0], [3.0, 2.0]],
          "width_pixels": 1}
    pv_own = (await client.post("/v1/cube/pv", json=pv, headers=headers)).json()
    pv_chosen = (await client.post("/v1/cube/pv", json={**pv, "spectral_quantity": "vopt",
                                                        "spectral_frame": "LSRK"},
                                   headers=headers)).json()
    assert pv_own["spectral_axis_unit"] == "Hz" and pv_chosen["spectral_axis_unit"] == "km/s"
    assert pv_chosen["spectral_frame"] == "LSRK" and pv_chosen["spectral_axis_type"] == "VOPT"
    fits_body = {**base, "spectral_quantity": "vrad", "spectral_frame": "LSRK",
                 "output_basename": "m1_lsrk.fits"}
    saved = (await client.post("/v1/products/moment_fits", json=fits_body, headers=headers)).json()
    assert saved["valid"] and saved["moment_unit"] == "km/s"
    assert fits.getheader(saved["output_path"])["SPECSYS"] == "LSRK"
