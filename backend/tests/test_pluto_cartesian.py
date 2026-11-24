"""The Cartesian view of PLUTO polar, cylindrical, spherical and stretched
grids (issue #527): analytic fields resampled onto a uniform grid match the
analytic Cartesian function, the outside of the domain is NaN, the cube carries
its provenance and the units of its source, and it opens like any volume."""
from __future__ import annotations

import importlib.util
import math
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import physical_units as pu
from app import pluto_source as ps
from app.compute import pluto_cartesian as pc

_GEN = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"
_spec = importlib.util.spec_from_file_location("generate_demo_data", _GEN)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
write_pluto = _mod.write_pluto

TWO_PI = 2.0 * math.pi


def _centres(edges: np.ndarray) -> np.ndarray:
    return 0.5 * (edges[:-1] + edges[1:])


def _run(tmp_path, geometry, edges, fn, names=("rho",), **kw):
    """A run on ``edges`` (x1, x2, x3) whose variable ``rho`` is ``fn(c1, c2,
    c3)`` at the cell centres, each argument a (nx3, nx2, nx1) array."""
    c1, c2, c3 = np.meshgrid(_centres(edges[0]), _centres(edges[1]), _centres(edges[2]),
                             indexing="ij")
    # meshgrid ij over (x1, x2, x3) is (nx1, nx2, nx3); PLUTO stores (nx3, nx2, nx1).
    c1, c2, c3 = (np.transpose(a, (2, 1, 0)) for a in (c1, c2, c3))
    fields = {name: fn(c1, c2, c3) + i for i, name in enumerate(names)}
    return write_pluto(tmp_path, fields, edges=edges, geometry=geometry, **kw), fields


def _view(first, **kw):
    """(plan, data array in FITS order reversed to (…, y, x), header) of the
    view of the first variable."""
    ref = ps.pluto_dataset_path(first)
    out = pc.worker_pluto_cartesian(ref, kw.pop("fields", None), kw.pop("shape", None),
                                    kw.pop("max_voxels", pc.DEFAULT_MAX_VOXELS),
                                    kw.pop("mirror", False), str(Path(first).parent / "views"))
    with fits.open(out["outputs"][0]["path"]) as hdul:
        return out, np.array(hdul[0].data), hdul[0].header


def _coords(header):
    """Voxel centres per FITS axis, from the header's linear WCS."""
    return [header[f"CRVAL{a}"] + header[f"CDELT{a}"] * np.arange(header[f"NAXIS{a}"])
            for a in range(1, header["NAXIS"] + 1)]


def _edges(lo, hi, n):
    return np.linspace(lo, hi, n + 1)


# ── Spherical ────────────────────────────────────────────────────────────────

@pytest.fixture
def sphere(tmp_path):
    edges = (_edges(0.5, 2.0, 24), _edges(0.0, math.pi, 24), _edges(0.0, TWO_PI, 48))
    first, _ = _run(tmp_path, "SPHERICAL", edges, lambda r, th, ph: r)
    return first


def test_a_spherical_run_is_a_cartesian_cube(sphere):
    out, data, header = _view(sphere, shape=(40, 40, 40))
    assert out["plan"]["mode"] == "cube" and data.shape == (40, 40, 40)
    assert [header[f"CTYPE{a}"] for a in (1, 2, 3)] == ["X", "Y", "Z"]
    x, y, z = _coords(header)
    r = np.sqrt(x[None, None, :] ** 2 + y[None, :, None] ** 2 + z[:, None, None] ** 2)
    outside = (r < 0.5) | (r > 2.0)
    assert np.isnan(data[outside]).all() and not np.isnan(data[~outside]).any()
    # f = r is linear in the native coordinate: exact between the cell centres
    # (the half cell at either end of the domain is held constant).
    r0, r1 = 0.5 + 0.5 * 1.5 / 24, 2.0 - 0.5 * 1.5 / 24
    inner = (r > r0) & (r < r1)
    np.testing.assert_allclose(data[inner], r[inner], atol=1e-5)


def test_a_cartesian_function_of_a_spherical_run_is_recovered_within_interpolation(tmp_path):
    edges = (_edges(0.5, 2.0, 48), _edges(0.0, math.pi, 48), _edges(0.0, TWO_PI, 96))
    first, _ = _run(tmp_path, "SPHERICAL", edges,
                    lambda r, th, ph: r * np.sin(th) * np.cos(ph) + 2.0 * r * np.cos(th))
    _, data, header = _view(first, shape=(32, 32, 32))
    x, y, z = _coords(header)
    X, Y, Z = x[None, None, :], y[None, :, None], z[:, None, None]
    r = np.sqrt(X ** 2 + Y ** 2 + Z ** 2)
    want = np.broadcast_to(X + 2.0 * Z, data.shape)
    mid = (r > 0.7) & (r < 1.8) & (np.abs(Z) < 0.8 * r)
    assert np.nanmax(np.abs(data[mid] - want[mid])) < 0.03 * 2.0 * 1.8


def test_a_wedge_is_nan_where_it_has_no_cells(tmp_path):
    edges = (_edges(1.0, 2.0, 16), _edges(math.pi / 4, 3 * math.pi / 4, 16),
             _edges(0.0, math.pi / 2, 16))
    first, _ = _run(tmp_path, "SPHERICAL", edges, lambda r, th, ph: r)
    _, data, header = _view(first, shape=(32, 32, 32))
    x, y, z = _coords(header)
    X, Y, Z = x[None, None, :], y[None, :, None], z[:, None, None]
    r = np.sqrt(X ** 2 + Y ** 2 + Z ** 2)
    theta = np.arccos(np.where(r > 0, Z / np.where(r > 0, r, 1.0), 0.0))
    phi = np.arctan2(Y, X)
    inside = (r > 1.1) & (r < 1.9) & (theta > 0.8) & (theta < 2.3) & (phi > 0.05) & (phi < 1.5)
    assert not np.isnan(data[inside]).any()
    away = (r < 0.9) | (r > 2.1) | (theta < 0.7) | (theta > 2.45) | (phi < -0.05) | (phi > 1.65)
    assert np.isnan(data[away]).all()


def test_an_axisymmetric_spherical_run_is_the_meridional_plane(tmp_path):
    edges = (_edges(0.5, 2.0, 30), _edges(0.0, math.pi, 30), _edges(0.0, TWO_PI, 1))
    first, _ = _run(tmp_path, "SPHERICAL", edges, lambda r, th, ph: r)
    out, data, header = _view(first, shape=(40, 80))
    assert out["plan"]["mode"] == "meridional" and out["plan"]["axes"] == ["X", "Z"]
    assert header["NAXIS"] == 2 and (header["CTYPE1"], header["CTYPE2"]) == ("X", "Z")
    x, z = _coords(header)
    assert x.min() > 0 and header["PLUTOMIR"] is False
    r = np.hypot(x[None, :], z[:, None])
    inner = (r > 0.55) & (r < 1.95)
    np.testing.assert_allclose(data[inner], r[inner], atol=1e-5)
    assert np.isnan(data[(r < 0.45) | (r > 2.05)]).all()


def test_mirroring_a_meridional_image_reflects_it_to_negative_x(tmp_path):
    edges = (_edges(0.5, 2.0, 30), _edges(0.0, math.pi, 30), _edges(0.0, TWO_PI, 1))
    first, _ = _run(tmp_path, "SPHERICAL", edges, lambda r, th, ph: r)
    out, data, header = _view(first, shape=(80, 80), mirror=True)
    x, _z = _coords(header)
    assert out["plan"]["mirror"] and header["PLUTOMIR"] is True
    assert x[0] == pytest.approx(-x[-1])
    np.testing.assert_allclose(data, data[:, ::-1], equal_nan=True)


def test_an_equatorial_spherical_run_is_the_xy_image(tmp_path):
    edges = (_edges(1.0, 3.0, 24), _edges(math.pi / 2 - 0.05, math.pi / 2 + 0.05, 1),
             _edges(0.0, TWO_PI, 64))
    first, _ = _run(tmp_path, "SPHERICAL", edges, lambda r, th, ph: r)
    out, data, header = _view(first, shape=(60, 60))
    assert out["plan"]["mode"] == "plane" and out["plan"]["axes"] == ["X", "Y"]
    x, y = _coords(header)
    r = np.hypot(x[None, :], y[:, None])
    inner = (r > 1.1) & (r < 2.9)
    np.testing.assert_allclose(data[inner], r[inner], atol=1e-4)
    assert np.isnan(data[(r < 0.9) | (r > 3.1)]).all()


# ── Polar and cylindrical ────────────────────────────────────────────────────

def test_a_2d_polar_run_is_the_xy_image_across_the_phi_seam(tmp_path):
    edges = (_edges(1.0, 3.0, 20), _edges(0.0, TWO_PI, 72), _edges(0.0, 1.0, 1))
    first, _ = _run(tmp_path, "POLAR", edges, lambda R, ph, z: R * np.cos(ph))   # = x
    out, data, header = _view(first, shape=(64, 64))
    assert out["plan"]["mode"] == "plane" and header["NAXIS"] == 2
    x, y = _coords(header)
    R = np.hypot(x[None, :], y[:, None])
    inner = (R > 1.15) & (R < 2.85)
    assert np.nanmax(np.abs(data[inner] - np.broadcast_to(x[None, :], data.shape)[inner])) < 0.05
    assert np.isnan(data[(R < 0.9) | (R > 3.1)]).all()
    # The seam (phi = 0 and 2 pi) is smooth: the ring just above and below the
    # +x axis has no jump (f = x there is 1..3, a step would be 3 or more).
    row = data[np.argmin(np.abs(y))][x > 0]
    assert np.isfinite(row[(x[x > 0] > 1.1) & (x[x > 0] < 2.9)]).all()
    ok = np.isfinite(row)
    assert np.abs(np.diff(row[ok])).max() < 0.2


def test_a_3d_polar_run_is_a_cube(tmp_path):
    edges = (_edges(1.0, 2.0, 12), _edges(0.0, TWO_PI, 48), _edges(-1.0, 1.0, 10))
    first, _ = _run(tmp_path, "POLAR", edges, lambda R, ph, z: R + 10.0 * z)
    out, data, header = _view(first, shape=(24, 24, 20))
    assert out["plan"]["mode"] == "cube" and data.shape == (20, 24, 24)
    x, y, z = _coords(header)
    R = np.hypot(x[None, :], y[:, None])
    inner = (R > 1.1) & (R < 1.9)
    layer = data[10]                              # a z plane
    np.testing.assert_allclose(layer[inner], R[inner] + 10.0 * z[10], atol=1e-5)


def test_a_cylindrical_run_is_the_meridional_plane(tmp_path):
    edges = (_edges(0.0, 2.0, 20), _edges(-1.0, 3.0, 40), _edges(0.0, TWO_PI, 1))
    first, _ = _run(tmp_path, "CYLINDRICAL", edges, lambda R, z, ph: 3.0 * R + z)
    out, data, header = _view(first, shape=(40, 80), mirror=True)
    assert out["plan"]["mode"] == "meridional"
    x, z = _coords(header)
    X, Z = x[None, :], z[:, None]
    want = 3.0 * np.abs(X) + Z
    inner = (np.abs(X) > 0.06) & (np.abs(X) < 1.9) & (Z > -0.95) & (Z < 2.95)
    np.testing.assert_allclose(data[inner], np.broadcast_to(want, data.shape)[inner], atol=1e-5)


# ── Stretched Cartesian ──────────────────────────────────────────────────────

def test_a_stretched_cartesian_grid_is_regridded_uniform(tmp_path):
    x1 = np.concatenate([[0.0], np.cumsum(np.geomspace(0.02, 0.2, 24))])
    edges = (x1, _edges(0.0, 2.0, 10), _edges(0.0, 1.0, 1))
    first, _ = _run(tmp_path, "CARTESIAN", edges, lambda x, y, z: 2.0 * x + y)
    assert ps.parse_grid(Path(first).parent / "grid.out").index_reason()
    out, data, header = _view(first, shape=(50, 20))
    assert out["plan"]["mode"] == "grid" and out["plan"]["axes"] == ["X", "Y"]
    x, y = _coords(header)
    c = _centres(x1)
    inner = (x > c[0]) & (x < c[-1])
    want = 2.0 * x[None, :] + y[:, None]
    ytab = _centres(edges[1])
    ok = (y > ytab[0]) & (y < ytab[-1])
    np.testing.assert_allclose(data[np.ix_(ok, inner)], want[np.ix_(ok, inner)], atol=1e-5)
    assert header["CDELT1"] == pytest.approx((x1[-1] - x1[0]) / 50)


# ── Plans, limits, provenance ────────────────────────────────────────────────

def test_the_default_grid_is_as_fine_as_the_finest_length_and_capped(sphere):
    grid = ps.parse_grid(Path(sphere).parent / "grid.out")
    plan = pc.plan_cartesian(grid, max_voxels=1 << 30)
    h = 1.5 / 24                                  # the radial cell
    assert plan.cell[0] == pytest.approx(h, rel=0.05) and plan.mode == "cube"
    capped = pc.plan_cartesian(grid, max_voxels=50_000)
    assert capped.voxels <= 50_000 and capped.shape[0] == capped.shape[1] == capped.shape[2]
    with pytest.raises(pc.PlutoCartesianError, match="3 voxel counts"):
        pc.plan_cartesian(grid, shape=(10, 10))
    with pytest.raises(pc.PlutoCartesianError, match="voxels"):
        pc.plan_cartesian(grid, shape=(5000, 5000, 5000))


def test_a_1d_run_and_an_unknown_geometry_have_no_view(tmp_path):
    first, _ = _run(tmp_path / "a", "SPHERICAL", (_edges(1, 2, 16), _edges(0, 1, 1),
                                                    _edges(0, 1, 1)), lambda r, a, b: r)
    grid = ps.parse_grid(Path(first).parent / "grid.out")
    with pytest.raises(pc.PlutoCartesianError, match="1-D"):
        pc.plan_cartesian(grid)
    first, _ = _run(tmp_path / "b", "HELICAL", (_edges(1, 2, 8), _edges(0, 1, 8),
                                                  _edges(0, 1, 8)), lambda r, a, b: r)
    with pytest.raises(pc.PlutoCartesianError, match="helical"):
        pc.plan_cartesian(ps.parse_grid(Path(first).parent / "grid.out"))


def test_the_cube_records_how_it_was_made(sphere):
    _, _data, header = _view(sphere, shape=(16, 16, 16))
    ref = ps.pluto_dataset_path(sphere)
    assert header["PLUTOSRC"] == ref and header["PLUTOGEO"] == "SPHERICAL"
    assert header["PLUTOVEW"] == "CARTESIAN" and header["PLUTOINT"] == "trilinear"
    assert header["PLUTOMOD"] == "cube" and header["PLUTOGRD"] == "UNIFORM"
    assert header["PLUTOVAR"] == "rho" and header["BITPIX"] == -32
    assert ps.view_source(Path(sphere).parent / "views") is None


def test_every_variable_can_be_resampled_in_one_pass_and_chunks_do_not_matter(
        tmp_path, monkeypatch):
    edges = (_edges(0.5, 2.0, 12), _edges(0.0, math.pi, 12), _edges(0.0, TWO_PI, 24))
    first, fields = _run(tmp_path, "SPHERICAL", edges, lambda r, th, ph: r * np.cos(th),
                         names=("rho", "prs"))
    big, _, _ = _view(first, fields=["*"], shape=(16, 16, 16))
    assert [o["field"] for o in big["outputs"]] == ["rho", "prs"]
    whole = [fits.getdata(o["path"]).copy() for o in big["outputs"]]
    np.testing.assert_allclose(whole[1][np.isfinite(whole[0])],
                               whole[0][np.isfinite(whole[0])] + 1.0, atol=1e-5)
    monkeypatch.setattr(pc, "CHUNK_VOXELS", 300)
    small, _, _ = _view(first, fields=["*"], shape=(16, 16, 16))
    for a, o in zip(whole, small["outputs"]):
        np.testing.assert_array_equal(a, fits.getdata(o["path"]))
    with pytest.raises(pc.PlutoCartesianError, match="no cell variable"):
        _view(first, fields=["tr9"])


def test_the_view_has_the_physical_units_of_its_source(tmp_path):
    edges = (_edges(0.5, 2.0, 12), _edges(0.0, math.pi, 12), _edges(0.0, TWO_PI, 24))
    first, _ = _run(tmp_path, "SPHERICAL", edges, lambda r, th, ph: r, time=2.0)
    (tmp_path / "definitions.h").write_text(
        "#define UNIT_DENSITY 1.0e-20\n#define UNIT_LENGTH 3.0e18\n#define UNIT_VELOCITY 1.0e5\n")
    ref = ps.pluto_dataset_path(first)
    # As stored the grid is in cell indices: no coordinate has a unit.
    assert "x" not in pu.scales(ref) and pu.scales(ref)["value"]["unit"] == "g/cm^3"
    out, _, _ = _view(first, shape=(8, 8, 8))
    made = pu.scales(out["outputs"][0]["path"])
    assert made["value"] == pu.scales(ref)["value"]
    assert made["x"]["factor"] == pytest.approx(3.0e18) and made["z"]["unit"] == "cm"
    assert made["time"] == pu.scales(ref)["time"]


# ── Routes ───────────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_the_routes_plan_resample_and_open_the_view(client, auth_headers, tmp_path):
    edges = (_edges(0.5, 2.0, 16), _edges(0.0, math.pi, 16), _edges(0.0, TWO_PI, 32))
    first, _ = _run(tmp_path, "SPHERICAL", edges, lambda r, th, ph: r, time=1.5)
    (tmp_path / "definitions.h").write_text("#define UNIT_LENGTH 1.0e10\n")
    plan = await client.post("/v1/simulations/pluto/cartesian/plan",
                             json={"path": str(tmp_path / "dbl.out"), "shape": [20, 20, 20]},
                             headers=auth_headers)
    body = plan.json()
    assert plan.status_code == 200 and body["plan"]["mode"] == "cube"
    assert body["index_reason"] == "spherical geometry"
    assert body["plan"]["shape"] == [20, 20, 20]

    made = await client.post("/v1/simulations/pluto/cartesian",
                             json={"path": str(first), "shape": [20, 20, 20]},
                             headers=auth_headers)
    assert made.status_code == 200, made.text
    (cube,) = made.json()["outputs"]
    opened = await client.post("/v1/datasets/open", json={"path": cube["path"]},
                               headers=auth_headers)
    info = opened.json()
    assert info["valid"] and info["simulation_volume"] is True
    assert info["physical_units"]["columns"]["x"]["unit"] == "cm"
    assert info["physical_units"]["time_code"] == pytest.approx(1.5)
    assert info["physical_units"]["variable"] == "rho"


@pytest.mark.asyncio
async def test_the_routes_refuse_what_has_no_view(client, auth_headers, tmp_path):
    first, _ = _run(tmp_path, "SPHERICAL", (_edges(1, 2, 16), _edges(0, 1, 1), _edges(0, 1, 1)),
                    lambda r, a, b: r)
    bad = await client.post("/v1/simulations/pluto/cartesian", json={"path": str(first)},
                            headers=auth_headers)
    assert bad.status_code == 422 and "1-D" in bad.json()["detail"]
    nope = await client.post("/v1/simulations/pluto/cartesian/plan",
                             json={"path": str(tmp_path / "missing.dbl")}, headers=auth_headers)
    assert nope.status_code == 422


@pytest.mark.asyncio
async def test_a_meridional_image_opens_as_an_image_with_its_units(client, auth_headers, tmp_path):
    edges = (_edges(0.5, 2.0, 24), _edges(0.0, math.pi, 24), _edges(0.0, TWO_PI, 1))
    first, _ = _run(tmp_path, "SPHERICAL", edges, lambda r, th, ph: r)
    (tmp_path / "definitions.h").write_text("#define UNIT_LENGTH 1.0e10\n#define UNIT_DENSITY 1.0\n")
    made = await client.post("/v1/simulations/pluto/cartesian",
                             json={"path": str(first), "shape": [30, 60], "mirror": True},
                             headers=auth_headers)
    assert made.status_code == 200, made.text
    assert made.json()["plan"]["mirror"] is True
    opened = await client.post("/v1/datasets/open",
                               json={"path": made.json()["outputs"][0]["path"]},
                               headers=auth_headers)
    info = opened.json()
    assert info["valid"], info
    assert info["kind"] == "image" and info["width"] == 30 and info["height"] == 60
    # The image viewer does not take physical units (as for a 2-D run as stored),
    # but the table is the source's all the same.
    assert pu.scales(made.json()["outputs"][0]["path"])["x"]["unit"] == "cm"


def test_a_wedge_past_pi_and_a_half_disc_keep_their_cells(tmp_path):
    for lo, hi in ((0.0, 1.5 * math.pi), (math.pi, TWO_PI)):
        edges = (_edges(1.0, 2.0, 8), _edges(lo, hi, 36), _edges(0.0, 1.0, 1))
        first, _ = _run(tmp_path / f"w{lo:.1f}", "POLAR", edges, lambda R, ph, z: R)
        _, data, header = _view(first, shape=(40, 40))
        x, y = _coords(header)
        R = np.hypot(x[None, :], y[:, None])
        phi = np.mod(np.arctan2(y[:, None], x[None, :]), TWO_PI)
        ring = (R > 1.1) & (R < 1.9)
        inside = ring & (phi > lo + 0.05) & (phi < hi - 0.05)
        outside = ring & ((phi < lo - 0.05) | (phi > hi + 0.05))
        assert not np.isnan(data[inside]).any() and inside.sum() > 100
        assert np.isnan(data[outside]).all()


def test_a_variable_named_twice_is_made_once_and_a_failure_leaves_nothing(tmp_path, monkeypatch):
    edges = (_edges(0.5, 2.0, 8), _edges(0.0, math.pi, 8), _edges(0.0, TWO_PI, 16))
    first, _ = _run(tmp_path, "SPHERICAL", edges, lambda r, th, ph: r)
    out, data, _ = _view(first, fields=["rho", "rho"], shape=(8, 8, 8))
    assert len(out["outputs"]) == 1 and np.isfinite(data).any()
    views = Path(first).parent / "views"
    before = sorted(views.iterdir())
    def boom(*a, **k):
        raise RuntimeError("disk full")
    monkeypatch.setattr(pc, "resample", boom)
    with pytest.raises(RuntimeError):
        _view(first, shape=(8, 8, 8))
    assert sorted(views.iterdir()) == before
