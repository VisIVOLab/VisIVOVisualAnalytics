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
    assert f.spacing == pytest.approx((100.0, 100.0, 100.0))
    # grid centred on the origin
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
