"""Simulation volumes (issue #524): slices along x, y or z at a coordinate, and
the code units of a PLUTO run (UNIT_DENSITY, UNIT_LENGTH, UNIT_VELOCITY) as
physical units for the viewers."""
from __future__ import annotations

import base64
import importlib.util
import math
import zlib
from pathlib import Path

import numpy as np
import pytest

from app import physical_units as pu
from app.compute.cube import axis_plane_index, slice_plane_dims, worker_cube_slice
from app.pluto_source import pluto_ref

_GEN = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"
_spec = importlib.util.spec_from_file_location("generate_demo_data", _GEN)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
write_pluto = _mod.write_pluto

SHAPE = (12, 16, 20)      # (nz, ny, nx)
DEFINITIONS = """\
#define  PHYSICS                 HD
#define  UNIT_DENSITY            (CONST_mp)
#define  UNIT_LENGTH             CONST_pc  /* one parsec */
#define  UNIT_VELOCITY           1.0e5
#define  UNIT_DENSITYX           3.0
"""


def _decode(body: dict) -> np.ndarray:
    raw = base64.b64decode(body["data_base64"])
    if body["compression"] == "qt-zlib":
        raw = zlib.decompress(raw[4:])
    return np.frombuffer(raw, dtype=np.float32).reshape(body["height"], body["width"])


def _run(tmp_path: Path, definitions: str | None = DEFINITIONS):
    k, j, i = np.meshgrid(*(np.arange(n, dtype=np.float64) for n in SHAPE), indexing="ij")
    fields = {"rho": 1.0 + i + 100.0 * j + 10000.0 * k, "vx1": i - j, "prs": 2.0 + k,
              "tr1": np.full(SHAPE, 0.5)}
    edges = (np.linspace(-1.0, 1.0, SHAPE[2] + 1), np.linspace(0.0, 1.6, SHAPE[1] + 1),
             np.linspace(0.0, 1.2, SHAPE[0] + 1))
    first = write_pluto(tmp_path, fields, precision="dbl", edges=edges, time=3.0)
    if definitions is not None:
        (tmp_path / "definitions.h").write_text(definitions)
    return first, fields


# ── Slices ──────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("axis,index", [("z", 4), ("y", 7), ("x", 13)])
def test_a_slice_along_any_axis_is_numpys(tmp_path, axis, index):
    first, fields = _run(tmp_path)
    ref = pluto_ref(first, "rho")
    out = worker_cube_slice(ref, index, 0, None, axis)
    cube = fields["rho"]
    want = {"z": lambda: cube[index], "y": lambda: cube[:, index, :],
            "x": lambda: cube[:, :, index]}[axis]()
    np.testing.assert_array_equal(_decode(out), want.astype(np.float32))
    assert (out["axis"], out["index"]) == (axis, index)
    assert (out["full_width"], out["full_height"]) == (want.shape[1], want.shape[0])
    assert out["planes"] == slice_plane_dims(SHAPE, axis)[0]


def test_a_strided_region_of_an_x_slice_reads_the_planes_own_samples(tmp_path):
    first, fields = _run(tmp_path)
    out = worker_cube_slice(pluto_ref(first, "rho"), 3, 8, (2, 14, 1, 10), "x")
    want = fields["rho"][:, :, 3][1:11:2, 2:15:2].astype(np.float32)
    np.testing.assert_array_equal(_decode(out), want)
    assert (out["x0"], out["y0"], out["stride"]) == (2, 1, 2)


def test_the_coordinate_is_the_nearest_plane(tmp_path):
    first, fields = _run(tmp_path)
    ref = pluto_ref(first, "rho")
    # x cells are centred at -0.95 .. 0.95 (step 0.1): x = 0.04 is cell 10 (0.05).
    out = worker_cube_slice(ref, 0, 0, None, "x", 0.04)
    assert out["index"] == 10
    np.testing.assert_array_equal(_decode(out), fields["rho"][:, :, 10].astype(np.float32))
    # y centres 0.05 .. 1.55 (step 0.1), z 0.05 .. 1.15 (step 0.1).
    assert worker_cube_slice(ref, 0, 0, None, "y", 0.52)["index"] == 5
    assert worker_cube_slice(ref, 0, 0, None, "z", 1.14)["index"] == 11
    with pytest.raises(ValueError, match="outside the axis"):
        worker_cube_slice(ref, 0, 0, None, "x", 3.0)


def test_axis_plane_index_rules():
    assert axis_plane_index(0.0, 1.0, -0.95, 0.1, 20) in (9, 10)
    assert axis_plane_index(-0.95, 1.0, -0.95, 0.1, 20) == 0
    assert axis_plane_index(0.95, 1.0, -0.95, 0.1, 20) == 19
    # Half a plane beyond the last is still the last; a whole one is an error.
    assert axis_plane_index(1.0, 1.0, -0.95, 0.1, 20) == 19
    with pytest.raises(ValueError):
        axis_plane_index(1.2, 1.0, -0.95, 0.1, 20)
    # A decreasing axis.
    assert axis_plane_index(9.0, 1.0, 10.0, -1.0, 5) == 1
    with pytest.raises(ValueError):
        axis_plane_index(1.0, 1.0, 0.0, 0.0, 5)


def test_an_unknown_axis_is_refused(tmp_path):
    first, _ = _run(tmp_path)
    with pytest.raises(ValueError):
        worker_cube_slice(pluto_ref(first, "rho"), 0, 0, None, "w")


# ── definitions.h and the derived units ─────────────────────────────────────

def test_definitions_are_parsed_with_pluto_constants():
    got = pu.parse_definitions(DEFINITIONS)
    assert got["UNIT_DENSITY"] == pytest.approx(1.67262171e-24)
    assert got["UNIT_LENGTH"] == pytest.approx(3.0856775807e18)
    assert got["UNIT_VELOCITY"] == pytest.approx(1.0e5)
    assert "UNIT_DENSITYX" not in got
    assert pu.evaluate_define("(1.5e-24 * 2) // twice") == pytest.approx(3.0e-24)
    assert pu.evaluate_define("1.0e18L") == pytest.approx(1.0e18)
    for bad in ("UNIT_X*2", "__import__('os')", "(1.0", "-3.0", "0"):
        assert pu.evaluate_define(bad) is None


def test_a_run_gives_its_derived_units(tmp_path):
    first, _ = _run(tmp_path)
    rho = pu.scales(pluto_ref(first, "rho"))
    d, l, v = 1.67262171e-24, 3.0856775807e18, 1.0e5
    assert (rho["value"]["unit"], rho["value"]["source"]) == ("g/cm^3", "definitions")
    assert rho["value"]["factor"] == pytest.approx(d)
    assert rho["x"]["factor"] == pytest.approx(l) and rho["x"]["unit"] == "cm"
    assert rho["time"]["factor"] == pytest.approx(l / v) and rho["time"]["unit"] == "s"
    vel = pu.column_scale(pluto_ref(first, "vx1"), "value")
    assert (vel["unit"], vel["factor"]) == ("cm/s", pytest.approx(v))
    prs = pu.column_scale(pluto_ref(first, "prs"), "value")
    assert (prs["unit"], prs["factor"]) == ("dyn/cm^2", pytest.approx(d * v * v))
    assert pu.merged_run_units(pluto_ref(first, "prs")).magnetic == pytest.approx(
        v * math.sqrt(4 * math.pi * d))
    # A tracer is dimensionless: nothing to convert.
    assert pu.column_scale(pluto_ref(first, "tr1"), "value") is None
    # A file that is no PLUTO output has no units of its own.
    assert pu.scales(str(tmp_path / "unrelated.fits")) == {}


def test_no_definitions_means_code_units(tmp_path):
    first, _ = _run(tmp_path, definitions=None)
    assert pu.scales(pluto_ref(first, "rho")) == {}


def test_definitions_are_found_above_the_data_directory(tmp_path):
    first, _ = _run(tmp_path / "output", definitions=None)
    (tmp_path / "definitions.h").write_text(DEFINITIONS)
    assert pu.column_scale(pluto_ref(first, "rho"), "value")["source"] == "definitions"


def test_typed_units_replace_the_file_and_are_shared_by_the_run(tmp_path):
    first, _ = _run(tmp_path)
    rho, prs = pluto_ref(first, "rho"), pluto_ref(first, "prs")
    pu.set_user(rho, run={"unit_density": 2.0e-20, "unit_velocity": 1.0e6})
    got = pu.column_scale(prs, "value")
    assert got["factor"] == pytest.approx(2.0e-20 * 1.0e12) and got["source"] == "user"
    assert pu.merged_run_units(prs).length == pytest.approx(3.0856775807e18)   # the file's, still
    pu.set_user(rho, columns={"value": (7.0, "Msun/pc^3"), "x": (1.0, "pc"),
                              "y": (1.0, "pc"), "z": (1.0, "pc")})
    own = pu.scales(rho)
    assert (own["value"]["factor"], own["value"]["unit"]) == (7.0, "Msun/pc^3")
    assert (own["x"]["factor"], own["x"]["unit"]) == (1.0, "pc")
    assert pu.column_scale(prs, "x")["unit"] == "pc"             # the run's coordinates
    assert pu.column_scale(prs, "value")["source"] == "user"     # derived from the typed run units
    pu.set_user(rho, clear=True)
    back = pu.column_scale(rho, "value")
    assert (back["unit"], back["source"]) == ("g/cm^3", "definitions")
    with pytest.raises(ValueError):
        pu.set_user(rho, run={"unit_density": -1.0})
    with pytest.raises(ValueError):
        pu.set_user(rho, columns={"value": (0.0, "x")})


def test_a_typed_column_replaces_a_volume_that_has_no_run_units(tmp_path):
    """A FITS cube or VBT field has no producer of its own: typed factors are
    its whole table."""
    cube = str(tmp_path / "field.fits")
    assert pu.scales(cube) == {}
    pu.set_user(cube, columns={"value": (3.0, "K"), "x": (2.0, "kpc")})
    got = pu.scales(cube)
    assert got["value"] == {"factor": 3.0, "unit": "K", "source": "user"}
    assert got["x"]["unit"] == "kpc" and "y" not in got


# ── Routes ───────────────────────────────────────────────────────────────────

async def _open(client, auth_headers, path) -> dict:
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    body = resp.json()
    assert body["valid"], body["error"]
    return {**body, "headers": {**auth_headers, "X-Visivo-Session": body["session_id"]}}


@pytest.mark.asyncio
async def test_open_reports_the_units_and_the_slice_route_takes_axis_and_coordinate(
        client, auth_headers, tmp_path):
    first, fields = _run(tmp_path)
    cube = await _open(client, auth_headers, tmp_path / "dbl.out")
    assert cube["simulation_volume"] is True
    info = cube["physical_units"]
    assert info["active"] and info["source"] == "definitions"
    assert info["columns"]["value"]["unit"] == "g/cm^3" and info["columns"]["y"]["unit"] == "cm"
    assert info["time_code"] == pytest.approx(3.0)
    assert info["columns"]["time"]["factor"] == pytest.approx(3.0856775807e18 / 1.0e5)
    assert info["unit_velocity"] == pytest.approx(1.0e5)

    ask = {"dataset_id": cube["dataset_id"], "axis": "x"}
    resp = await client.post("/v1/cube/slice", headers=cube["headers"],
                             json={**ask, "coordinate": 0.04})
    body = resp.json()
    assert body["valid"], body["error"]
    assert (body["axis"], body["index"], body["planes"]) == ("x", 10, 20)
    np.testing.assert_array_equal(_decode(body), fields["rho"][:, :, 10].astype(np.float32))
    resp = await client.post("/v1/cube/slice", headers=cube["headers"], json={**ask, "index": 2})
    np.testing.assert_array_equal(_decode(resp.json()), fields["rho"][:, :, 2].astype(np.float32))
    resp = await client.post("/v1/cube/slice", headers=cube["headers"],
                             json={**ask, "coordinate": 9.0})
    assert resp.json()["valid"] is False and "outside" in resp.json()["error"]
    resp = await client.post("/v1/cube/slice", headers=cube["headers"],
                             json={**ask, "axis": "w", "index": 0})
    assert resp.status_code == 422


@pytest.mark.asyncio
async def test_the_units_route_types_and_clears_and_survives_reopening(
        client, auth_headers, tmp_path):
    _run(tmp_path, definitions=None)
    cube = await _open(client, auth_headers, tmp_path / "dbl.out")
    assert cube["physical_units"]["active"] is False
    resp = await client.post("/v1/datasets/units", headers=cube["headers"],
                             json={"dataset_id": cube["dataset_id"], "unit_density": 1.0e-22,
                                   "unit_length": 3.0e18, "unit_velocity": 2.0e5})
    body = resp.json()
    assert body["valid"], body["error"]
    got = body["physical_units"]
    assert got["source"] == "user" and got["columns"]["value"]["unit"] == "g/cm^3"
    assert got["columns"]["value"]["factor"] == pytest.approx(1.0e-22)
    again = await _open(client, auth_headers, tmp_path / "dbl.out")
    assert again["physical_units"]["columns"]["value"]["factor"] == pytest.approx(1.0e-22)
    resp = await client.post("/v1/datasets/units", headers=again["headers"], json={
        "dataset_id": again["dataset_id"],
        "columns": {"value": {"factor": 7.0, "unit": "Msun/pc^3"},
                    "x": {"factor": 1.0, "unit": "pc"}, "y": {"factor": 1.0, "unit": "pc"},
                    "z": {"factor": 1.0, "unit": "pc"}}})
    columns = resp.json()["physical_units"]["columns"]
    assert columns["value"] == {"factor": 7.0, "unit": "Msun/pc^3", "source": "user"}
    assert columns["y"]["unit"] == "pc" and columns["time"]["source"] == "user"
    resp = await client.post("/v1/datasets/units", headers=again["headers"],
                             json={"dataset_id": again["dataset_id"], "clear": True})
    assert resp.json()["physical_units"]["active"] is False
    resp = await client.post("/v1/datasets/units", headers=again["headers"],
                             json={"dataset_id": again["dataset_id"], "unit_density": -2.0})
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_a_spectral_cube_is_not_a_simulation_volume(client, auth_headers, tmp_path):
    from astropy.io import fits
    hdr = fits.Header()
    for k, v in (("CTYPE1", "RA---SIN"), ("CTYPE2", "DEC--SIN"), ("CTYPE3", "VRAD"),
                 ("CDELT1", -0.001), ("CDELT2", 0.001), ("CDELT3", 1000.0),
                 ("CRPIX1", 1), ("CRPIX2", 1), ("CRPIX3", 1)):
        hdr[k] = v
    path = tmp_path / "hi.fits"
    fits.PrimaryHDU(np.zeros((4, 6, 8), np.float32), header=hdr).writeto(path)
    cube = await _open(client, auth_headers, path)
    assert cube["simulation_volume"] is False and cube["physical_units"] is None
    resp = await client.post("/v1/datasets/units", headers=cube["headers"],
                             json={"dataset_id": cube["dataset_id"], "unit_density": 1.0})
    assert resp.json()["valid"] is False


# ── Values in physical units ────────────────────────────────────────────────

def test_a_value_scale_multiplies_every_data_route(tmp_path):
    from app.compute.cube import (worker_cube_preview, worker_cube_region_level_raw,
                                  worker_cube_subvolume, worker_cube_subvolume_raw)
    first, fields = _run(tmp_path)
    ref = pluto_ref(first, "rho")
    f = 1.67262171e-24
    rho = fields["rho"]

    sl = worker_cube_slice(ref, 5, 0, None, "y", None, f)
    np.testing.assert_allclose(_decode(sl), (rho[:, 5, :] * f).astype(np.float32), rtol=1e-6)
    assert sl["range_max"] == pytest.approx(float(rho[:, 5, :].max() * f), rel=1e-6)

    box = (2, 9, 1, 8, 0, 5)
    sub = worker_cube_subvolume_raw(ref, *box, f)
    got = np.frombuffer(sub["raw_bytes"], np.float32).reshape(6, 8, 8)
    np.testing.assert_allclose(got, rho[0:6, 1:9, 2:10] * f, rtol=1e-6)
    assert sub["range_min"] == pytest.approx(float(got.min()))
    plain = worker_cube_subvolume_raw(ref, *box)
    np.testing.assert_array_equal(np.frombuffer(plain["raw_bytes"], np.float32).reshape(6, 8, 8),
                                  rho[0:6, 1:9, 2:10].astype(np.float32))
    assert worker_cube_subvolume(ref, *box, f)["range_max"] == pytest.approx(sub["range_max"])

    prev = worker_cube_preview(ref, 2, None, f)
    raw = base64.b64decode(prev["data_base64"])
    if prev["compression"] == "qt-zlib":
        raw = zlib.decompress(raw[4:])
    np.testing.assert_allclose(
        np.frombuffer(raw, np.float32).reshape(prev["depth"], prev["height"], prev["width"]),
        rho[::2, ::2, ::2] * f, rtol=1e-6)

    lvl = worker_cube_region_level_raw(ref, 0, 0, 19, 0, 15, 0, 11, 0, None, "auto", f)
    np.testing.assert_allclose(np.frombuffer(lvl["raw_bytes"], np.float32).reshape(12, 16, 20),
                               rho * f, rtol=1e-6)


def test_a_float32_unrepresentable_factor_is_applied_in_double():
    from app.compute._common import scaled_f32
    a = np.array([2.0e10, 1.0], dtype=np.float32)
    out = scaled_f32(a, 1.0e-40)
    assert out.dtype == np.float32 and out[0] == pytest.approx(2.0e-30, rel=1e-5)


def test_a_grid_in_cell_indices_has_no_length_to_convert(tmp_path):
    k, j, i = np.meshgrid(*(np.arange(n, dtype=np.float64) for n in (4, 6, 8)), indexing="ij")
    uneven = tuple(np.cumsum(np.r_[0.0, np.linspace(1.0, 2.0, n)]) for n in (8, 6, 4))
    first = write_pluto(tmp_path, {"rho": 1.0 + i}, edges=uneven)
    (tmp_path / "definitions.h").write_text(DEFINITIONS)
    table = pu.scales(pluto_ref(first, "rho"))
    assert "x" not in table and table["value"]["source"] == "definitions"


# ── Exports ──────────────────────────────────────────────────────────────────

def test_a_plane_export_carries_the_units_and_their_provenance(tmp_path):
    from astropy.io import fits
    from app.compute.cube import worker_cube_save_channel_2d
    first, fields = _run(tmp_path)
    ref = pluto_ref(first, "rho")
    d, l = 1.67262171e-24, 3.0856775807e18
    out = tmp_path / "x_plane.fits"
    worker_cube_save_channel_2d(ref, str(out), 10, "x", True)
    with fits.open(out) as hdul:
        h = hdul[0].header
        np.testing.assert_allclose(hdul[0].data, fields["rho"][:, :, 10] * d, rtol=1e-6)
        assert (h["NAXIS1"], h["NAXIS2"]) == (SHAPE[1], SHAPE[0])
        assert (h["CTYPE1"], h["CTYPE2"]) == ("Y", "Z")
        assert (h["SLICEAX"], h["SLICEIDX"]) == ("x", 10)
        assert h["SLICEPOS"] == pytest.approx(0.05 * l, rel=1e-6)
        assert h["BUNIT"] == "g/cm^3" and h["CUNIT1"] == "cm"
        assert h["UNITFAC"] == pytest.approx(d) and h["LENFAC"] == pytest.approx(l)
        assert h["CODEUNIT"] == "code units" and h["UNITSRC"] == "definitions"
        assert h["CDELT1"] == pytest.approx(0.1 * l, rel=1e-6)
    # In code units: the data and axes untouched, no factor recorded.
    code = tmp_path / "z_plane.fits"
    worker_cube_save_channel_2d(ref, str(code), 4, "z", False)
    with fits.open(code) as hdul:
        np.testing.assert_allclose(hdul[0].data, fields["rho"][4], rtol=1e-6)
        assert "UNITFAC" not in hdul[0].header and hdul[0].header["SLICEAX"] == "z"
    with pytest.raises(ValueError, match="out of range"):
        worker_cube_save_channel_2d(ref, str(tmp_path / "bad.fits"), 99, "x", False)


def test_a_spectral_cube_exports_only_z_channels(tmp_path):
    from astropy.io import fits
    from app.compute.cube import worker_cube_save_channel_2d
    path = tmp_path / "hi.fits"
    fits.PrimaryHDU(np.zeros((4, 6, 8), np.float32)).writeto(path)
    with pytest.raises(ValueError, match="simulation volume"):
        worker_cube_save_channel_2d(str(path), str(tmp_path / "o.fits"), 1, "x", False)


def test_hostile_definitions_do_not_break_an_open():
    assert pu.evaluate_define("True") is None
    assert pu.evaluate_define("+".join(["1.0"] * 20000)) is None
    assert pu.evaluate_define("-" * 5000 + "1.0") is None
    assert pu.parse_definitions("#define UNIT_LENGTH " + "(" * 3000 + "1.0" + ")" * 3000) == {}
