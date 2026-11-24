"""The PV tool's spectral range, width in arcsec and vertex offsets (issue #520).

A range keeps only the channels of the line: the diagram is the full one cut to
those rows, and every spectral axis written for it (the response's coordinates,
the FITS WCS) describes just them, in the chosen axis too.
"""
from __future__ import annotations

import base64
import zlib

import numpy as np
import pytest
from astropy.io import fits

from app.compute import compute_pv, worker_pv, worker_pv_save_fits
from app.compute.pv_iso import resolve_channel_range
from app.spectral_axis import normalize_choice, spectral_axis

from .test_pv_axes import _coords, _cube_header, _spectral_world, _write
from .test_spectral_products import NZ, _cube

VERTS = [[2, 10], [25, 10]]


@pytest.fixture
def cube(tmp_path):
    """40 channels, velocity decreasing from 500 km/s in steps of 2; each channel
    holds its own number, so a row says which channel it came from."""
    nx, ny, nz = 32, 20, 40
    data = np.broadcast_to(np.arange(nz, dtype=np.float32)[:, None, None], (nz, ny, nx)).copy()
    return _write(tmp_path, data, _cube_header(nx, ny, nz, cdelt3=-2.0, crval3=500.0))


def _f32(b64):
    """A qt-zlib float32 array: 4 bytes of big-endian length, then zlib."""
    return np.frombuffer(zlib.decompress(base64.b64decode(b64)[4:]), np.float32)


def _pv(result):
    return _f32(result["data_base64"]).reshape(result["depth"], result["num_samples"])


def test_channel_range_is_the_full_diagram_cut_to_those_rows(cube):
    full = worker_pv(cube, VERTS, 1)
    part = worker_pv(cube, VERTS, 1, None, None, {"channel_start": 10, "channel_end": 19})
    assert (part["channel_start"], part["channel_end"], part["depth"]) == (10, 19, 10)
    np.testing.assert_array_equal(_pv(part), _pv(full)[10:20])
    np.testing.assert_allclose(_coords(part), _coords(full)[10:20])
    assert part["spectral_crval"] == pytest.approx(480.0)
    assert part["spectral_cdelt"] == pytest.approx(-2.0)
    assert (full["channel_start"], full["channel_end"]) == (0, 39)


def test_value_range_is_in_the_axis_either_order_and_open_ended(cube):
    # Velocities 500 - 2 c: 440..420 are channels 30..40, clipped to 39 (422).
    part = worker_pv(cube, VERTS, 1, None, None, {"value_min": 421.0, "value_max": 440.0})
    assert (part["channel_start"], part["channel_end"]) == (30, 39)
    swapped = worker_pv(cube, VERTS, 1, None, None, {"value_min": 440.0, "value_max": 421.0})
    assert (swapped["channel_start"], swapped["channel_end"]) == (30, 39)
    open_low = worker_pv(cube, VERTS, 1, None, None, {"value_max": 490.0})
    assert (open_low["channel_start"], open_low["channel_end"]) == (5, 39)


def test_both_kinds_of_range_intersect_and_empty_ranges_are_refused():
    v = 500.0 - 2.0 * np.arange(40)
    assert resolve_channel_range(v, 0, 20, 450.0, None) == (0, 20)
    assert resolve_channel_range(v, 10, 30, None, 470.0) == (15, 30)
    assert resolve_channel_range(v, 35, 100, None, None) == (35, 39)        # clamped
    assert resolve_channel_range(v, 20, 10, None, None) == (10, 20)         # reversed
    with pytest.raises(ValueError, match="spectral range"):
        resolve_channel_range(v, None, None, 1000.0, 2000.0)
    with pytest.raises(ValueError, match="outside"):
        resolve_channel_range(v, 50, 60, None, None)


def test_width_in_arcsec_and_vertex_offsets(cube):
    r = worker_pv(cube, [[2, 10], [12, 10], [12, 15]], 3)
    # 1 arcsec pixels: the band is 3" wide; the vertices sit at 0, 10 and 15".
    assert r["width_arcsec"] == pytest.approx(3.0, rel=1e-6)
    assert r["vertex_offsets_arcsec"] == pytest.approx([0.0, 10.0, 15.0], rel=1e-6)
    offsets = _f32(r["positions_arcsec_base64"])
    assert offsets[-1] == pytest.approx(r["vertex_offsets_arcsec"][-1], abs=1e-3)


def test_fits_native_axis_after_a_range(tmp_path):
    """Nonlinear optical-velocity axis: the WCS of the kept rows is the cube's
    own, shifted, on every kept channel."""
    nx, ny, nz = 12, 10, 25
    h = _cube_header(nx, ny, nz, -2.0, 500.0)
    h["CTYPE3"], h["CUNIT3"], h["CRPIX3"], h["CRVAL3"], h["CDELT3"] = "VOPT-F2W", "m/s", 1.0, 3.0e7, -3.0e6
    path = _write(tmp_path, np.ones((nz, ny, nx), np.float32), h)
    out = tmp_path / "pv.fits"
    worker_pv_save_fits(path, str(out), [[1, 5], [10, 5]], 2, None, None,
                        {"channel_start": 6, "channel_end": 15})
    with fits.open(out) as hdul:
        hdr, data = hdul[0].header, hdul[0].data
    assert data.shape[0] == 10
    np.testing.assert_allclose(_spectral_world(hdr, 2, 10), _spectral_world(h, 3, nz)[6:16],
                               rtol=1e-9, atol=1e-6)
    assert (hdr["PVCHAN1"], hdr["PVCHAN2"]) == (6, 15)
    assert hdr["PVWIDARC"] == pytest.approx(2.0, rel=1e-6) and hdr["PVWIDTH"] == 2
    history = "\n".join(str(x) for x in hdr["HISTORY"])
    assert "PV vertex 1: pix 1.000 5.000" in history and "PV vertex 2: pix 10.000 5.000" in history


@pytest.mark.parametrize("quantity", ["vrad", "vopt"])
def test_fits_velocity_choice_after_a_range(tmp_path, quantity):
    """A range in km/s of the chosen axis, on a frequency cube: the output's axis
    starts at the first kept channel and every row sits on it (optical velocity
    is resampled, radio is exact)."""
    path = _cube(tmp_path, line_in=(quantity,))
    choice = normalize_choice(quantity, "native", None)
    table = np.asarray(spectral_axis(str(path), quantity)["values"])
    lo, hi = -60.0, 40.0
    out = tmp_path / "pv.fits"
    worker_pv_save_fits(str(path), str(out), [[0.0, 0.0], [1.0, 0.0]], 1, None, choice,
                        {"value_min": lo, "value_max": hi})
    inside = np.flatnonzero((table >= lo) & (table <= hi))
    h = fits.getheader(out)
    data = fits.getdata(out)
    assert data.shape[0] == inside.size
    assert (h["PVCHAN1"], h["PVCHAN2"]) == (inside[0], inside[-1])
    assert h["CRVAL2"] == pytest.approx(table[inside[0]])
    assert h["CRVAL2"] + h["CDELT2"] * (inside.size - 1) == pytest.approx(table[inside[-1]])
    assert h["CRVAL2"] + h["CDELT2"] * (inside.size - 1) <= hi + 1e-9 or quantity == "vopt"
    # The line sits at -30 km/s in the first column.
    row = int(np.nanargmax(data[:, 0]))
    assert h["CRVAL2"] + h["CDELT2"] * row == pytest.approx(-30.0, abs=abs(h["CDELT2"]) * 1.5)
    assert inside.size < NZ


async def test_routes_take_the_range_and_key_the_cache_on_it(client, auth_headers, cube):
    opened = (await client.post("/v1/datasets/open", json={"path": cube}, headers=auth_headers)).json()
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    body = {"dataset_id": opened["dataset_id"], "vertices": VERTS, "width_pixels": 1}
    full = (await client.post("/v1/cube/pv", json=body, headers=headers)).json()
    part = (await client.post("/v1/cube/pv", json={**body, "channel_start": 5, "channel_end": 14},
                              headers=headers)).json()
    again = (await client.post("/v1/cube/pv", json={**body, "spectral_min": 480.0, "spectral_max": 462.0},
                               headers=headers)).json()
    assert full["valid"] and part["valid"] and again["valid"]
    assert (full["depth"], part["depth"]) == (40, 10)
    assert (part["channel_start"], part["channel_end"]) == (5, 14)
    assert (again["channel_start"], again["channel_end"]) == (10, 19)
    assert part["width_arcsec"] == pytest.approx(1.0, rel=1e-6)
    fits_resp = (await client.post("/v1/products/pv_fits", json={
        **body, "channel_start": 5, "channel_end": 14, "output_basename": "r_pv.fits"},
        headers=headers)).json()
    assert fits_resp["valid"], fits_resp["error"]
    assert fits_resp["height"] == 10
    empty = (await client.post("/v1/cube/pv", json={**body, "spectral_min": 1e6}, headers=headers)).json()
    assert not empty["valid"] and "spectral range" in empty["error"]
