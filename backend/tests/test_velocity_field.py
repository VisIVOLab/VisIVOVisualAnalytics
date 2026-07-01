"""Tests for the F1 velocity-field loader + vector-field geometry."""
import numpy as np
import pytest
from astropy.io import fits

from app.velocity_field import (
    load_velocity_field,
    load_component_fits,
    to_vtk_image,
    compute_streamlines,
)


def _write_multicomponent(tmp_path, n=16):
    """A synthetic divergence-free-ish rotational field on an n^3 grid, written
    as a (3, n, n, n) FITS (component axis = NAXIS4 = 3, like CF4 files)."""
    z, y, x = np.mgrid[0:n, 0:n, 0:n].astype(np.float32)
    cx = cy = cz = (n - 1) / 2.0
    vx = -(y - cy)
    vy = (x - cx)
    vz = 0.1 * (z - cz)
    data = np.stack([vx, vy, vz]).astype(np.float32)  # (3, n, n, n)
    path = tmp_path / "vel.fits"
    fits.PrimaryHDU(data).writeto(path, overwrite=True)
    return str(path), data


def test_load_multicomponent_shape_and_components(tmp_path):
    path, data = _write_multicomponent(tmp_path, n=12)
    f = load_velocity_field(path)
    assert f.shape == (12, 12, 12)
    np.testing.assert_allclose(f.vx, data[0])
    np.testing.assert_allclose(f.vy, data[1])
    np.testing.assert_allclose(f.vz, data[2])
    assert f.box_known is False
    assert f.spacing == (1.0, 1.0, 1.0)


def test_box_mpc_sets_physical_spacing(tmp_path):
    path, _ = _write_multicomponent(tmp_path, n=10)
    f = load_velocity_field(path, box_mpc=1000.0)
    assert f.box_known is True
    # Point-centred model: points span [-box/2, +box/2] → spacing = box/(n-1).
    assert f.spacing == pytest.approx((1000.0 / 9, 1000.0 / 9, 1000.0 / 9))
    assert f.origin == pytest.approx((-500.0, -500.0, -500.0))


def test_summary_speed_stats(tmp_path):
    path, data = _write_multicomponent(tmp_path, n=12)
    f = load_velocity_field(path)
    s = f.summary()
    assert s["finite_fraction"] == 1.0
    expected_max = float(np.sqrt(data[0] ** 2 + data[1] ** 2 + data[2] ** 2).max())
    assert s["speed_max"] == pytest.approx(expected_max, rel=1e-4)


def test_to_vtk_image_has_vector_array(tmp_path):
    path, _ = _write_multicomponent(tmp_path, n=8)
    img = to_vtk_image(load_velocity_field(path))
    assert img.GetDimensions() == (8, 8, 8)
    vec = img.GetPointData().GetVectors()
    assert vec is not None and vec.GetNumberOfComponents() == 3
    assert vec.GetName() == "velocity"


def test_streamlines_are_produced(tmp_path):
    path, _ = _write_multicomponent(tmp_path, n=16)
    n_cells = compute_streamlines(load_velocity_field(path), n_seeds=100)
    assert n_cells > 0


def test_rejects_non_3component_fits(tmp_path):
    # a plain 3-D scalar cube must be rejected by the multi-component loader
    path = tmp_path / "scalar.fits"
    fits.PrimaryHDU(np.zeros((8, 8, 8), dtype=np.float32)).writeto(path, overwrite=True)
    with pytest.raises(ValueError):
        load_velocity_field(str(path))


def test_velocity_router_open_and_grid_bin(tmp_path):
    """End-to-end: /v1/velocity/open registers the field, /v1/velocity/grid_bin
    returns the raw interleaved vector grid with the right size + headers."""
    import os

    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app

    n = 8
    path, _ = _write_multicomponent(tmp_path, n=n)
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)

    r = client.post("/v1/velocity/open", json={"path": path, "box_mpc": 800.0}, headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"] is True, body
    dsid = body["dataset_id"]
    assert dsid.startswith("vf_")
    assert body["summary"]["shape"] == [n, n, n]
    assert body["summary"]["box_known"] is True

    g = client.post("/v1/velocity/grid_bin", json={"dataset_id": dsid}, headers=auth)
    assert g.status_code == 200, g.text
    assert g.headers["X-Visivo-Components"] == "3"
    assert g.headers["X-Visivo-Width"] == str(n)
    assert len(g.content) == n * n * n * 3 * 4  # float32 × 3 components

    # unknown dataset → 404
    bad = client.post("/v1/velocity/grid_bin", json={"dataset_id": "vf_nope"}, headers=auth)
    assert bad.status_code == 404


def test_downsample_caps_largest_dimension(tmp_path):
    from app.velocity_field import downsample
    path, _ = _write_multicomponent(tmp_path, n=16)
    f = load_velocity_field(path)
    d = downsample(f, max_dim=8)
    assert max(d.shape) <= 8
    # spacing grows by the block factor so the physical extent is preserved
    assert d.spacing == pytest.approx((2.0, 2.0, 2.0))
    # no-op when already small enough
    assert downsample(load_velocity_field(path), max_dim=64).shape == (16, 16, 16)


def test_grid_worker_downsamples(tmp_path):
    from app.velocity_field import velocity_grid_worker
    path, _ = _write_multicomponent(tmp_path, n=20)
    p = velocity_grid_worker(path, None, max_dim=8)
    assert max(p["nx"], p["ny"], p["nz"]) <= 8
    assert p["full_shape"] == [20, 20, 20]
    assert p["downsample_factor"] >= 2
    # full-res when max_dim=0
    p0 = velocity_grid_worker(path, None, max_dim=0)
    assert (p0["nx"], p0["ny"], p0["nz"]) == (20, 20, 20)


def test_subgrid_worker_full_res_slice(tmp_path):
    from app.velocity_field import velocity_subgrid_worker
    path, data = _write_multicomponent(tmp_path, n=20)
    p = velocity_subgrid_worker(path, 4, 12, 2, 10, 0, 6, None)
    assert (p["nx"], p["ny"], p["nz"]) == (8, 8, 6)
    assert (p["x0"], p["y0"], p["z0"]) == (4, 2, 0)
    assert p["downsample_factor"] == 1
    assert len(p["raw_bytes"]) == 8 * 8 * 6 * 3 * 4
    # out-of-range indices are clamped, not crashing
    pc = velocity_subgrid_worker(path, -5, 999, 0, 5, 0, 5, None)
    assert pc["nx"] == 20


def test_velocity_subgrid_router(tmp_path):
    import os
    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)
    path, _ = _write_multicomponent(tmp_path, n=16)
    dsid = client.post("/v1/velocity/open", json={"path": path}, headers=auth).json()["dataset_id"]
    g = client.post("/v1/velocity/subgrid_bin",
                    json={"dataset_id": dsid, "x0": 2, "x1": 10, "y0": 2, "y1": 10,
                          "z0": 2, "z1": 10}, headers=auth)
    assert g.status_code == 200, g.text
    assert g.headers["X-Visivo-Width"] == "8"
    assert g.headers["X-Visivo-Roi-X0"] == "2"
    assert len(g.content) == 8 * 8 * 8 * 3 * 4


def test_component_fits_loader(tmp_path):
    n = 10
    paths = []
    for name in ("vx", "vy", "vz"):
        p = tmp_path / f"{name}.fits"
        fits.PrimaryHDU(np.ones((n, n, n), dtype=np.float32)).writeto(p, overwrite=True)
        paths.append(str(p))
    f = load_component_fits(*paths)
    assert f.shape == (n, n, n)
    assert f.summary()["speed_max"] == pytest.approx(np.sqrt(3.0), rel=1e-5)


def _write_radial(tmp_path, n=16, sign=1.0):
    """A purely radial field v = sign·(p − centre); with unit spacing its
    divergence is 3·sign everywhere (outflow → +, infall → −)."""
    z, y, x = np.mgrid[0:n, 0:n, 0:n].astype(np.float32)
    c = (n - 1) / 2.0
    data = np.stack([sign * (x - c), sign * (y - c), sign * (z - c)]).astype(np.float32)
    p = tmp_path / f"radial_{'out' if sign > 0 else 'in'}.fits"
    fits.PrimaryHDU(data).writeto(p, overwrite=True)
    return str(p)


def test_velocity_density_worker_signs(tmp_path):
    from app.velocity_field import velocity_density_worker

    out = velocity_density_worker(_write_radial(tmp_path, sign=+1.0), None, 0)
    assert out["nx"] == out["ny"] == out["nz"] == 16
    assert len(out["raw_bytes"]) == 16 ** 3 * 4
    div = np.frombuffer(out["raw_bytes"], dtype="<f4").reshape(16, 16, 16)
    assert np.allclose(div, 3.0, atol=1e-3)   # ∇·(p) = 3 exactly at unit spacing (linear ramp)
    assert out["unit"].endswith("/pixel")     # box unknown → per-pixel divergence

    inn = velocity_density_worker(_write_radial(tmp_path, sign=-1.0), None, 0)
    idiv = np.frombuffer(inn["raw_bytes"], dtype="<f4").reshape(16, 16, 16)
    assert np.all(idiv < 0)                   # infall → convergent everywhere (overdensity)


def test_velocity_density_bin_endpoint(tmp_path):
    import os
    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)
    path = _write_radial(tmp_path, n=16, sign=1.0)
    dsid = client.post("/v1/velocity/open", json={"path": path}, headers=auth).json()["dataset_id"]
    r = client.post("/v1/velocity/density_bin", json={"dataset_id": dsid}, headers=auth)
    assert r.status_code == 200, r.text
    assert r.headers["X-Visivo-Width"] == "16"
    assert r.headers["X-Visivo-Scalar-Type"] == "float32"
    assert float(r.headers["X-Visivo-Div-Max"]) > 0.0
    assert len(r.content) == 16 ** 3 * 4
