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


def test_r11_assumed_frame_warns(tmp_path):
    """R-11: a header with no VFRAME/WCS yields an 'assumed frame' warning."""
    path, _ = _write_multicomponent(tmp_path, n=8)
    f = load_velocity_field(path)
    assert any("assumed supergalactic" in w for w in f.warnings)
    assert "warnings" in f.summary()


def test_r11_flipped_cdelt_warns(tmp_path):
    """R-11: a negative CDELT (flipped axis) raises a mirror-image warning."""
    z, y, x = np.mgrid[0:8, 0:8, 0:8].astype(np.float32)
    data = np.stack([-(y - 3.5), (x - 3.5), 0.1 * (z - 3.5)]).astype(np.float32)
    hdu = fits.PrimaryHDU(data)
    hdu.header["CDELT1"] = -1.0   # flipped x axis
    hdu.header["CDELT2"] = 1.0
    hdu.header["CDELT3"] = 1.0
    path = str(tmp_path / "flipped.fits")
    hdu.writeto(path, overwrite=True)
    f = load_velocity_field(path)
    assert any("mirror" in w.lower() and "CDELT1" in w for w in f.warnings)


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


def test_r16_streaming_density_matches_in_memory(tmp_path, monkeypatch):
    """R-16: the out-of-core (z-slab) divergence must be byte-identical to the
    in-memory np.gradient result, and produce the correct ∇·(p)=3 value."""
    import app.velocity_field as vf

    path = _write_radial(tmp_path, n=20, sign=+1.0)
    # In-memory (huge threshold → never streams).
    monkeypatch.setattr(vf, "_VELOCITY_STREAM_BYTES", 1 << 60)
    ref = vf.velocity_density_worker(path, None, 0)
    # Force streaming (threshold 0) with a small slab that exercises the halo.
    monkeypatch.setattr(vf, "_VELOCITY_STREAM_BYTES", 0)
    monkeypatch.setattr(vf, "_VELOCITY_SLAB", 4)
    ooc = vf.velocity_density_worker(path, None, 0)
    assert ooc["nx"] == ref["nx"] and ooc["nz"] == ref["nz"]
    a = np.frombuffer(ref["raw_bytes"], dtype="<f4")
    b = np.frombuffer(ooc["raw_bytes"], dtype="<f4")
    np.testing.assert_array_equal(a, b)          # byte-for-byte identical
    assert np.allclose(b.reshape(20, 20, 20), 3.0, atol=1e-3)
    assert ooc["div_min"] == ref["div_min"] and ooc["div_max"] == ref["div_max"]


def test_density_uncalibrated_with_linear_factor(tmp_path):
    """Density payload is marked uncalibrated and carries the linear-theory
    factor (Planck18 f·H0) only when the physical box is known."""
    from app.velocity_field import velocity_density_worker

    # box_mpc given → box_known → factor present.
    with_box = velocity_density_worker(_write_radial(tmp_path, sign=+1.0), 1000.0, 0)
    assert with_box["calibrated"] is False
    assert with_box["linear_density_factor"] is not None
    assert with_box["linear_density_factor"] > 0
    # No box → per-pixel spacing, factor undefined.
    no_box = velocity_density_worker(_write_radial(tmp_path, sign=-1.0), None, 0)
    assert no_box["linear_density_factor"] is None


def test_r20_velocity_difference(tmp_path):
    """R-20: |v_a − v_b| per cell; identical fields → 0, offset field → known."""
    from app.velocity_field import velocity_difference_worker

    path_a, data = _write_multicomponent(tmp_path, n=10)
    # Same field vs itself → zero difference.
    same = velocity_difference_worker(path_a, path_a)
    d = np.frombuffer(same["raw_bytes"], dtype="<f4")
    assert same["diff_max"] == 0.0
    assert np.all(d == 0.0)

    # A field offset by (+3,+4,0) in every cell → |Δv| = 5 everywhere.
    off = data.copy()
    off[0] += 3.0
    off[1] += 4.0
    path_b = str(tmp_path / "vel_off.fits")
    fits.PrimaryHDU(off.astype(np.float32)).writeto(path_b, overwrite=True)
    diff = velocity_difference_worker(path_a, path_b)
    dd = np.frombuffer(diff["raw_bytes"], dtype="<f4")
    assert np.allclose(dd, 5.0, atol=1e-4)
    assert abs(diff["diff_mean"] - 5.0) < 1e-4


def test_r20_difference_shape_mismatch_raises(tmp_path):
    from app.velocity_field import velocity_difference_worker
    import pytest as _pytest
    a = str(tmp_path / "a8.fits")
    b = str(tmp_path / "b10.fits")
    fits.PrimaryHDU(np.zeros((3, 8, 8, 8), dtype=np.float32)).writeto(a, overwrite=True)
    fits.PrimaryHDU(np.zeros((3, 10, 10, 10), dtype=np.float32)).writeto(b, overwrite=True)
    with _pytest.raises(ValueError, match="same grid"):
        velocity_difference_worker(a, b)


def test_r20_difference_unit_mismatch_warns(tmp_path):
    """A cell-wise |v_a − v_b| across different BUNITs is a mixed-unit diff — the
    worker must surface a loud warning rather than silently subtracting."""
    from app.velocity_field import velocity_difference_worker
    a = np.zeros((3, 6, 6, 6), dtype=np.float32)
    b = np.zeros((3, 6, 6, 6), dtype=np.float32)
    pa = str(tmp_path / "kms.fits")
    pb = str(tmp_path / "ms.fits")
    ha = fits.PrimaryHDU(a); ha.header["BUNIT"] = "km/s"; ha.writeto(pa, overwrite=True)
    hb = fits.PrimaryHDU(b); hb.header["BUNIT"] = "m/s"; hb.writeto(pb, overwrite=True)
    diff = velocity_difference_worker(pa, pb)
    assert any("different units" in w for w in diff["warnings"])


def test_r20_difference_box_mismatch_warns(tmp_path):
    """Comparing two fields opened with different physical box sizes compares
    different physical positions cell-by-cell — warn (don't silently subtract)."""
    from app.velocity_field import velocity_difference_worker
    z = np.zeros((3, 6, 6, 6), dtype=np.float32)
    pa = str(tmp_path / "boxa.fits")
    pb = str(tmp_path / "boxb.fits")
    fits.PrimaryHDU(z).writeto(pa, overwrite=True)
    fits.PrimaryHDU(z).writeto(pb, overwrite=True)
    diff = velocity_difference_worker(pa, pb, box_mpc=100.0, box_mpc_b=200.0)
    assert any("different box sizes" in w for w in diff["warnings"])
    # Same box on both → no box warning.
    ok = velocity_difference_worker(pa, pb, box_mpc=100.0, box_mpc_b=100.0)
    assert not any("different box sizes" in w for w in ok["warnings"])


def test_r20_difference_bin_endpoint_range_headers(tmp_path):
    """The HTTP diff route carries generic Range-Min/Max (a scalar magnitude
    range), not the ∇·v Div headers, and forwards mismatch warnings."""
    import os
    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)
    a = np.zeros((3, 8, 8, 8), dtype=np.float32); a[0] += 3.0; a[1] += 4.0
    b = np.zeros((3, 8, 8, 8), dtype=np.float32)
    pa = str(tmp_path / "va.fits")
    pb = str(tmp_path / "vb.fits")
    ha = fits.PrimaryHDU(a); ha.header["BUNIT"] = "km/s"; ha.writeto(pa, overwrite=True)
    hb = fits.PrimaryHDU(b); hb.header["BUNIT"] = "m/s"; hb.writeto(pb, overwrite=True)
    ida = client.post("/v1/velocity/open", json={"path": pa}, headers=auth).json()["dataset_id"]
    idb = client.post("/v1/velocity/open", json={"path": pb}, headers=auth).json()["dataset_id"]
    r = client.post("/v1/velocity/difference_bin",
                    json={"dataset_id_a": ida, "dataset_id_b": idb}, headers=auth)
    assert r.status_code == 200, r.text
    assert "X-Visivo-Range-Max" in r.headers
    assert "X-Visivo-Div-Max" not in r.headers
    assert float(r.headers["X-Visivo-Range-Max"]) > 0.0
    assert "different units" in r.headers.get("X-Visivo-Warnings", "")


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
